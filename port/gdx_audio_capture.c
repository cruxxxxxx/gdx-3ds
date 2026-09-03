/* Streaming PCM capture; see gdx_audio_capture.h for the contract.
 *
 * FILE I/O CONVENTION (same as port_log.h's gdx_port_write_log / gdx_crash_report_write): no CRT
 * FILE*. fopen's per-call buffering corrupts the Debug CRT heap when called from the GFX/audio
 * fiber, whose stack may be mid-switch from the scheduler's point of view. So raw
 * CreateFileA/WriteFile on Windows, open/write on POSIX. The .pcm handle opens once at arm and
 * ticks append into the fixed-size write-behind buffer below, which flushes only when full or at
 * finalize, instead of one syscall per tick. The tiny .sha256 sidecar is opened, written and
 * closed once at finalize.
 *
 * SHA-256 is a vendored public-domain reference (the C++ twin lives in gdx_extract_launch.cpp;
 * ported to C89 here because this TU is plain C). It is an integrity digest, not security code.
 */
#include "gdx_audio_capture.h"
#include "port_log.h"

#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* Vendored SHA-256 (public-domain reference). */
typedef struct {
    uint32_t state[8];
    uint64_t count; /* total bytes hashed */
    uint8_t buffer[64];
} GdxSha256;

static const uint32_t kGdxSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static uint32_t gdx_ror32(uint32_t v, int b) {
    return (v >> b) | (v << (32 - b));
}

static void gdx_sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = gdx_ror32(w[i - 15], 7) ^ gdx_ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = gdx_ror32(w[i - 2], 17) ^ gdx_ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    for (i = 0; i < 64; ++i) {
        uint32_t S1 = gdx_ror32(e, 6) ^ gdx_ror32(e, 11) ^ gdx_ror32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + kGdxSha256K[i] + w[i];
        uint32_t S0 = gdx_ror32(a, 2) ^ gdx_ror32(a, 13) ^ gdx_ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void gdx_sha256_init(GdxSha256* ctx) {
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->count = 0;
}

static void gdx_sha256_update(GdxSha256* ctx, const uint8_t* data, size_t len) {
    size_t idx = (size_t)(ctx->count & 63u);
    size_t i = 0;
    ctx->count += len;
    if (idx > 0) {
        size_t part = 64 - idx;
        if (len < part) {
            memcpy(&ctx->buffer[idx], data, len);
            return;
        }
        memcpy(&ctx->buffer[idx], data, part);
        gdx_sha256_transform(ctx->state, ctx->buffer);
        i = part;
    }
    for (; i + 63 < len; i += 64) {
        gdx_sha256_transform(ctx->state, &data[i]);
    }
    memcpy(ctx->buffer, &data[i], len - i);
}

static void gdx_sha256_final(GdxSha256* ctx, uint8_t out[32]) {
    uint64_t bits = ctx->count << 3;
    uint8_t lenBytes[8];
    uint8_t pad = 0x80;
    int i;
    gdx_sha256_update(ctx, &pad, 1);
    pad = 0x00;
    while ((ctx->count & 63u) != 56u) {
        gdx_sha256_update(ctx, &pad, 1);
    }
    for (i = 0; i < 8; ++i) {
        lenBytes[i] = (uint8_t)((bits >> ((7 - i) * 8)) & 0xFF);
    }
    gdx_sha256_update(ctx, lenBytes, 8);
    for (i = 0; i < 32; ++i) {
        out[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
    }
}

#define GDX_PCM_PATH_MAX 1024

static int          sConfigured  = 0;   /* GDX_PCM_CAPTURE present */
static volatile int sArmed       = 0;   /* capture window open (cross-thread read) */
static volatile int sFinished    = 0;   /* finalized (cross-thread read by host loop) */
static int          sInitDone    = 0;   /* gdx_pcm_capture_init() ran once */

static char     sPcmPath[GDX_PCM_PATH_MAX];
static char     sShaPath[GDX_PCM_PATH_MAX];
static char     sPcmName[GDX_PCM_PATH_MAX]; /* basename, for the sha256sum-style sidecar */
static uint64_t sFrameLimit = 0;            /* 0 = unbounded */
static uint64_t sFrameCount = 0;            /* frames written so far */
static uint64_t sByteCount  = 0;            /* bytes written so far */
static unsigned sSampleRate = 0;            /* last observed, recorded in the sidecar */
static GdxSha256 sSha;

#ifdef _WIN32
static HANDLE sFile = INVALID_HANDLE_VALUE;
#else
static int sFile = -1;
#endif

/* Raw byte writers, no CRT FILE* -- see the header comment. */
static int gdx_pcm_open_output(void) {
#ifdef _WIN32
    sFile = CreateFileA(sPcmPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return sFile != INVALID_HANDLE_VALUE;
#else
    sFile = open(sPcmPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return sFile >= 0;
#endif
}

static void gdx_pcm_write_output(const void* data, size_t bytes) {
#ifdef _WIN32
    if (sFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(sFile, data, (DWORD)bytes, &written, NULL);
    }
#else
    if (sFile >= 0) {
        ssize_t rc = write(sFile, data, bytes);
        (void)rc;
    }
#endif
}

/* Write-behind buffer: one syscall per audio tick is needlessly expensive on the audio thread,
 * so ticks append here and flush only when full or at finalize. Produces byte-identical .pcm and
 * .sha256 output either way. */
#define GDX_PCM_WRITE_BUF_CAP (256u * 1024u)
static uint8_t sWriteBuf[GDX_PCM_WRITE_BUF_CAP];
static size_t  sWriteBufLen = 0;

static void gdx_pcm_flush_output(void) {
    if (sWriteBufLen > 0) {
        gdx_pcm_write_output(sWriteBuf, sWriteBufLen);
        sWriteBufLen = 0;
    }
}

static void gdx_pcm_buffer_append(const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*)data;
    while (bytes > 0) {
        size_t space = GDX_PCM_WRITE_BUF_CAP - sWriteBufLen;
        size_t chunk = (bytes < space) ? bytes : space;
        memcpy(&sWriteBuf[sWriteBufLen], p, chunk);
        sWriteBufLen += chunk;
        p += chunk;
        bytes -= chunk;
        if (sWriteBufLen == GDX_PCM_WRITE_BUF_CAP) {
            gdx_pcm_flush_output();
        }
    }
}

static void gdx_pcm_close_output(void) {
#ifdef _WIN32
    if (sFile != INVALID_HANDLE_VALUE) {
        CloseHandle(sFile);
        sFile = INVALID_HANDLE_VALUE;
    }
#else
    if (sFile >= 0) {
        close(sFile);
        sFile = -1;
    }
#endif
}

/* Split "<dir>/<prefix>.pcm" into just the trailing "<prefix>.pcm" for the sidecar. */
static void gdx_pcm_basename(const char* full, char* out, size_t cap) {
    const char* base = full;
    const char* p;
    for (p = full; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    if (strlen(base) < cap) {
        strcpy(out, base);
    } else if (cap > 0) {
        out[0] = '\0';
    }
}

void gdx_pcm_capture_init(void) {
    const char* prefix;
    const char* frames;

    if (sInitDone) {
        return;
    }
    sInitDone = 1;

    prefix = getenv("GDX_PCM_CAPTURE");
    if (prefix == NULL || prefix[0] == '\0') {
        sConfigured = 0;
        return; /* every call becomes a no-op */
    }
    if (strlen(prefix) + 12 >= GDX_PCM_PATH_MAX) {
        sConfigured = 0; /* pathological path length; degrade to no-op rather than truncate */
        return;
    }

    strcpy(sPcmPath, prefix);
    strcat(sPcmPath, ".pcm");
    strcpy(sShaPath, prefix);
    strcat(sShaPath, ".pcm.sha256");
    gdx_pcm_basename(sPcmPath, sPcmName, sizeof(sPcmName));

    frames = getenv("GDX_PCM_CAPTURE_FRAMES");
    if (frames != NULL && frames[0] != '\0') {
        sFrameLimit = (uint64_t)strtoull(frames, NULL, 10);
    } else {
        sFrameLimit = 0;
    }

    sConfigured = 1;
    /* Arm now, so gdx_pcm_capture_active() is already true when the audio thread reaches the RNG
       determinism pin at thread.c:194 on its very first tick. */
    gdx_pcm_capture_arm();
}

void gdx_pcm_capture_arm(void) {
    if (!sConfigured || sArmed || sFinished) {
        return;
    }
    gdx_sha256_init(&sSha);
    sFrameCount = 0;
    sByteCount = 0;
    sWriteBufLen = 0;
    if (!gdx_pcm_open_output()) {
        /* Could not create the output file: leave capture disarmed (no-op) rather than crash. */
        sConfigured = 0;
        return;
    }
    sArmed = 1;
}

static void gdx_pcm_capture_finalize(void) {
    uint8_t digest[32];
    static const char kHex[16] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };
    char line[GDX_PCM_PATH_MAX + 128];
    size_t n = 0;
    int i;

    if (!sArmed || sFinished) {
        return;
    }
    gdx_pcm_flush_output(); /* drain any bytes still sitting in the write-behind buffer */
    gdx_pcm_close_output();
    gdx_sha256_final(&sSha, digest);

    /* sha256sum-compatible line: "<64 hex lowercase>  <name>\n". */
    for (i = 0; i < 32; ++i) {
        line[n++] = kHex[digest[i] >> 4];
        line[n++] = kHex[digest[i] & 0xF];
    }
    line[n++] = ' ';
    line[n++] = ' ';
    {
        size_t nameLen = strlen(sPcmName);
        if (n + nameLen + 1 < sizeof(line)) {
            memcpy(&line[n], sPcmName, nameLen);
            n += nameLen;
        }
    }
    line[n++] = '\n';

    {
#ifdef _WIN32
        HANDLE sh = CreateFileA(sShaPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (sh != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(sh, line, (DWORD)n, &written, NULL);
            CloseHandle(sh);
        }
#else
        int sh = open(sShaPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sh >= 0) {
            ssize_t rc = write(sh, line, n);
            (void)rc;
            close(sh);
        }
#endif
    }

    sArmed = 0;
    sFinished = 1;
}

void gdx_pcm_capture_feed(const int16_t* frames, unsigned int frameCount, unsigned int sampleRate) {
    size_t bytesToWrite;
    uint64_t framesToWrite;

    if (!sConfigured || sFinished || frames == NULL || frameCount == 0) {
        return;
    }
    if (!sArmed) {
        /* Fail-closed. Self-arming here would run off the main thread and silently mask a
           violation of the init/arm threading contract; log once instead. */
        static unsigned sSkippedUnarmedFeeds = 0;
        sSkippedUnarmedFeeds++;
        if (sSkippedUnarmedFeeds == 1) {
            gdx_port_logf("[gdxcap] gdx_pcm_capture_feed() called while unarmed; dropping call(s) "
                          "(init/arm must run on the main thread before the audio thread starts)\n");
        }
        return;
    }

    sSampleRate = sampleRate;

    framesToWrite = frameCount;
    if (sFrameLimit > 0 && sFrameCount + framesToWrite > sFrameLimit) {
        framesToWrite = sFrameLimit - sFrameCount; /* clamp to the capture window */
    }
    if (framesToWrite > 0) {
        bytesToWrite = (size_t)framesToWrite * 2u /* channels */ * sizeof(int16_t);
        gdx_pcm_buffer_append(frames, bytesToWrite);
        gdx_sha256_update(&sSha, (const uint8_t*)frames, bytesToWrite);
        sFrameCount += framesToWrite;
        sByteCount += bytesToWrite;
    }

    if (sFrameLimit > 0 && sFrameCount >= sFrameLimit) {
        gdx_pcm_capture_finalize();
    }
}

int gdx_pcm_capture_active(void) {
    return sArmed;
}

int gdx_pcm_capture_finished(void) {
    return sFinished;
}

void gdx_pcm_capture_shutdown(void) {
    if (sConfigured && sArmed && !sFinished) {
        gdx_pcm_capture_finalize();
    }
}

/* Test-only: drop module state back to first-boot so one test process can drive several capture
   sessions, each re-reading the environment. Deliberately absent from gdx_audio_capture.h -- the
   unit test declares it extern. Closes any open handle so no descriptor leaks between sessions. */
void gdx_pcm_capture_reset_for_test(void) {
    gdx_pcm_flush_output();
    gdx_pcm_close_output();
    sConfigured = 0;
    sArmed = 0;
    sFinished = 0;
    sInitDone = 0;
    sFrameLimit = 0;
    sFrameCount = 0;
    sByteCount = 0;
    sWriteBufLen = 0;
    sSampleRate = 0;
    sPcmPath[0] = '\0';
    sShaPath[0] = '\0';
    sPcmName[0] = '\0';
}
