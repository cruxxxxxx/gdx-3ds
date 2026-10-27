/* port/3ds/gdx3ds_oracle.c — ORACLE true frame capture. Contract in the header. */
#include "gdx3ds_oracle.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>
#include <citro3d.h>

#include "gdx3ds_config.h"
#include "gdx3ds_cov.h"
#include "gdx3ds_filelog.h"
#include "gdx_input_rec.h" /* C5TICK: gdx_input_rec_peek_shot */

/* hw bug n60-3d8, item 3: the real branch@hash now comes from this generated, build-time
 * header (port/3ds/cmake/gdx3ds_gen_build_id.cmake) instead of a configure-time -D; the
 * __has_include guard + #ifndef below is a safety net for a build that never generated or
 * wired up the include path for it. */
#if defined(__has_include)
#if __has_include("gdx3ds_build_id.h")
#include "gdx3ds_build_id.h"
#endif
#endif
#ifndef GDX3DS_BUILD_ID
#define GDX3DS_BUILD_ID "unknown-build"
#endif

#define ORACLE_W 400u
#define ORACLE_H 240u
#define ORACLE_TAG_CAP 32
#define ORACLE_MARK_RING 16u

/* gfx_citro3d.cpp: 0 ok, 1 stale (transfer never landed), -1 bad state, -2 right eye inactive. */
extern int gdx3ds_gfx_capture_target(int eye, uint16_t* out, uint32_t width, uint32_t height);

/* Latched once by gdx3ds_oracle_init(); read-only afterwards (render thread reads). */
static int sEnabled = 0;
static int sEvery = 0;
static int sStart = 0; /* [debug] fh_start: auto captures begin at this frame (boot-window crash on the RetroArch lane) */
static int sEyeMask = 1; /* bit0 = L, bit1 = R */
static int sScale = 0;
static int sEveryBmp = 0;
static int sLegacyShot = 0;
/* A10 LOAD-STALL: `[debug] shot_fastwrite` (default 1). A SHOT BMP is written row by row --
 * 240-400 fwrite()s of ~1 KB -- and newlib's default stdio buffer is ~1 KB, so every row was
 * its own FS transaction to the SD card. On hardware that cost ~2 SECONDS per image on the
 * MAIN thread, and the canonical replay fires five SHOTs inside the 60 s measurement window:
 * every fps beat that carried one collapsed into the teens, in every lever configuration.
 * With a 64 KB stdio buffer the same bytes go out in ~5 writes. 0 restores the old behaviour
 * so the cost can be A/B'd on device without a rebuild. */
static int sFastWrite = 1;
static char sPlatform[24] = "unknown";

/* Cross-thread arm: requester writes sPendingTag then stores sPendingArmed=1 (release); the hook
 * exchanges it back to 0 (acq_rel) and copies the tag. */
static volatile int sPendingArmed = 0;
static char sPendingTag[ORACLE_TAG_CAP];

static volatile int sQuit = 0;
static unsigned long sQuitAtTick = 0; /* [debug] quit_at_tick: self-terminating scripted runs */

/* Frame bookkeeping. sFrame counts after-endframe calls (== host frameNum: exactly one per host
 * iteration in every rt mode). The ring carries main's (frame, tick, mode) for that frame.
 *
 * RTPARITY (audit-2026-09-23 finding 12): with the render thread on, MAIN writes this ring and
 * CORE 2 reads it. `.frame` is the publication key, so it needs a release store against the
 * `.tick`/`.mode` writes and an acquire load against the reads — on ARM11 MPCore a plain store
 * to `.frame` can be observed before its payload, which stamps a capture with the PREVIOUS
 * iteration's tick and mode. The oracle is the instrument: a mis-stamped receipt would make a
 * correct frame look divergent (or hide a divergent one) in exactly the mode this track gates.
 * `sPendingArmed` was already __atomic_*; the ring was missed. */
static unsigned long sFrame = 0;
static unsigned long sCaptures = 0;
static struct {
    unsigned long frame;
    unsigned long tick;
    int mode;
} sMarks[ORACLE_MARK_RING];

static uint16_t sPixels[ORACLE_W * ORACLE_H]; /* 192,000 B RGBA5551, the hashed buffer */

static void OracleLog(const char* line) {
    size_t n = strlen(line);
    svcOutputDebugString(line, n);
    gdx3ds_filelog_write(line, n);
}

/* CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), table-free bitwise: 192 kB per capture
 * is ~1.5 M bit steps — irrelevant next to the readback stall, and nothing to keep in sync
 * with a host tool (python zlib.crc32 yields the same value). */
static uint32_t Crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < len; i++) {
        uint32_t byte = data[i];
        int bit;
        crc ^= byte;
        for (bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void Put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* 24-bit bottom-up BMP from RGBA5551 (5->8 expansion (v<<3)|(v>>2)). `outW` resamples the width
 * by nearest (fh_scale); the height stays 240 — the hash never goes through this path. */
static long WriteBmp(const char* path, const uint16_t* px, unsigned outW) {
    const unsigned outH = ORACLE_H;
    const unsigned rowBytes = (outW * 3u + 3u) & ~3u;
    const uint32_t imageBytes = rowBytes * outH;
    const uint32_t fileBytes = 54u + imageBytes;
    unsigned char header[54];
    unsigned char row[ORACLE_W * 3u + 3u];
    /* A10: one 64 KB stdio buffer instead of newlib's ~1 KB default, so the 240 row writes
     * below coalesce into ~5 FS transactions. Static (not stack: the render thread's stack is
     * small) and file-lifetime, as setvbuf requires the buffer to outlive the stream. */
    static unsigned char sWriteBuf[64u * 1024u];
    FILE* f = fopen(path, "wb");
    unsigned y;
    if (f == NULL) {
        return -1;
    }
    if (sFastWrite) {
        setvbuf(f, (char*)sWriteBuf, _IOFBF, sizeof(sWriteBuf));
    }
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    Put32(header + 2, fileBytes);
    header[10] = 54;
    header[14] = 40;
    Put32(header + 18, outW);
    Put32(header + 22, outH);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), f);
    for (y = 0; y < outH; y++) {
        const unsigned sy = outH - 1u - y; /* BMP rows are bottom-up */
        unsigned x;
        memset(row, 0, rowBytes);
        for (x = 0; x < outW; x++) {
            const unsigned sx = (outW == ORACLE_W) ? x : (unsigned)(((uint64_t)x * ORACLE_W) / outW);
            const uint16_t p = px[(size_t)sy * ORACLE_W + sx];
            const unsigned r = (p >> 11) & 31u;
            const unsigned g = (p >> 6) & 31u;
            const unsigned b = (p >> 1) & 31u;
            row[x * 3u + 0] = (unsigned char)((b << 3) | (b >> 2));
            row[x * 3u + 1] = (unsigned char)((g << 3) | (g >> 2));
            row[x * 3u + 2] = (unsigned char)((r << 3) | (r >> 2));
        }
        fwrite(row, 1, rowBytes, f);
    }
    fclose(f);
    return (long)fileBytes;
}

static void CaptureEye(int eye, const char* tag, int writeBmp, unsigned long frame, unsigned long tick, int mode,
                       int markOk) {
    const char eyeCh = (eye == 1) ? 'R' : 'L';
    char line[192];
    const int rc = gdx3ds_gfx_capture_target(eye, sPixels, ORACLE_W, ORACLE_H);
    if (rc == -2) {
        snprintf(line, sizeof(line), "[fh] frame=%lu tick=%lu eye=R inactive tag=%s", frame, tick, tag);
        OracleLog(line);
        return;
    }
    if (rc < 0) {
        snprintf(line, sizeof(line), "[fh] frame=%lu tick=%lu eye=%c capture failed rc=%d tag=%s", frame, tick,
                 eyeCh, rc, tag);
        OracleLog(line);
        return;
    }
    {
        const uint32_t crc = Crc32((const uint8_t*)sPixels, sizeof(sPixels));
        snprintf(line, sizeof(line), "[fh] frame=%lu tick=%lu eye=%c w=%u h=%u hash=%08lx tag=%s mode=%d%s%s", frame,
                 tick, eyeCh, ORACLE_W, ORACLE_H, (unsigned long)crc, tag, mode, rc == 1 ? " stale=1" : "",
                 markOk ? "" : " mark=miss");
        OracleLog(line);
    }
    sCaptures++;
    if (writeBmp) {
        char path[96];
        long bytes;
        /* A10: the write is MAIN-THREAD blocking work inside the measured window, so its cost
         * has to be on the record next to the bytes -- an fps beat that carries a SHOT is not
         * a frame rate until this number is small. */
        const u64 t0 = svcGetSystemTick();
        int writeMs;
        /* mstereo: the RIGHT EYE is `<tag>_RE.bmp`. `<tag>_R.bmp` is the bottom screen's (the
         * touch menu / DASH write it from the main thread a tick later, gdx3ds_menu.c
         * MenuServiceShot), so an `_R` eye file was silently overwritten by a 320-wide bottom
         * capture on every --3d run. The left eye keeps `_L` (the top-screen name). */
        if (eye == 1) {
            snprintf(path, sizeof(path), "autotest/%s_RE.bmp", tag);
        } else {
            snprintf(path, sizeof(path), "autotest/%s_%c.bmp", tag, eyeCh);
        }
        bytes = WriteBmp(path, sPixels, sScale != 0 ? (unsigned)sScale : ORACLE_W);
        writeMs = (int)((svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC);
        if (bytes < 0) {
            snprintf(line, sizeof(line), "[autotest] SHOT capture write failed: %s ms=%d", path, writeMs);
        } else {
            snprintf(line, sizeof(line), "[autotest] SHOT captured -> %s bytes=%ld frame=%lu ms=%d fast=%d",
                     path, bytes, frame, writeMs, sFastWrite);
        }
        OracleLog(line);
    }
}

/* The (tick, mode) main published for `frame`; 1 when the ring still holds it. */
static int OracleLookupMark(unsigned long frame, unsigned long* tick, int* mode) {
    const unsigned slot = (unsigned)(frame % ORACLE_MARK_RING);
    /* acquire: pairs with the release store of .frame in gdx3ds_oracle_frame_mark */
    if (__atomic_load_n(&sMarks[slot].frame, __ATOMIC_ACQUIRE) != frame) {
        return 0;
    }
    *tick = sMarks[slot].tick;
    *mode = sMarks[slot].mode;
    return 1;
}

/* COVERAGE: the frame boundary, taken from the SAME (frame, tick, mode) tuple a [fh]
 * receipt is stamped with -- which is the whole point, since a [cov] frame number has to
 * be usable to place a SHOT that fhdiff will then match by frame. Runs every frame,
 * rendered or skipped; capture only on the sampled ones. */
static void OracleCoverFrame(unsigned long frame) {
    unsigned long tick = 0;
    int mode = -1;
    (void)OracleLookupMark(frame, &tick, &mode);
    gdx3ds_cov_frame(frame, tick, mode);
}

void gdx3ds_oracle_skip_frame(void) {
    sFrame++; /* keeps sFrame == host frameNum across a skipped (never presented) iteration */
    if (gdx3ds_cov_on) {
        OracleCoverFrame(sFrame);
    }
}

/* N60PREWARM ([native] prewarm bit 4, gdx3ds_prewarm.h): the race's opaque appear frame wants to be
 * drawn (its list holds the race's first-use work, under a black fade). Game side, weak: absent in
 * builds without the game library's prewarm unit. */
int gdx3ds_prewarm_wants_render(void) __attribute__((weak));
static int PrewarmWants(void) {
    return &gdx3ds_prewarm_wants_render != NULL && gdx3ds_prewarm_wants_render() != 0;
}

int gdx3ds_oracle_wants_next_frame(void) {
    const unsigned long next = sFrame + 1;
    if (__atomic_load_n(&sPendingArmed, __ATOMIC_ACQUIRE) != 0) {
        return 1;
    }
    if (PrewarmWants()) {
        return 1; /* the iteration about to begin walks the opaque frame's list */
    }
    return sEvery > 0 && next >= (unsigned long)sStart && (next % (unsigned long)sEvery) == 0;
}

/* C5TICK: the veto one iteration further ahead -- will the iteration AFTER the next one want a
 * capture? (The display list built in iteration N is walked in N+1; the hollow-tick lever
 * decides at N's build whether N+1 may skip.) A pending request is answered conservatively. */
int gdx3ds_oracle_wants_frame_after_next(void) {
    const unsigned long afterNext = sFrame + 2;
    if (__atomic_load_n(&sPendingArmed, __ATOMIC_ACQUIRE) != 0) {
        return 1;
    }
    if (gdx_input_rec_peek_shot()) {
        return 1;
    }
    if (PrewarmWants()) {
        return 1; /* the list being built is the opaque frame's: keep it whole (no hollow build) */
    }
    return sEvery > 0 && afterNext >= (unsigned long)sStart && (afterNext % (unsigned long)sEvery) == 0;
}

void gdx3ds_oracle_after_endframe(void) {
    unsigned long frame;
    unsigned long tick = 0;
    int mode = -1;
    int markOk;
    int shot;
    char tag[ORACLE_TAG_CAP];
    sFrame++; /* one increment per host frame: the ring lookup below needs it in every mode */
    if (sPendingArmed == 0 && sEvery == 0 && !gdx3ds_cov_on) {
        return; /* idle path: two loads, no sync, no config, no allocation */
    }
    shot = __atomic_exchange_n(&sPendingArmed, 0, __ATOMIC_ACQ_REL);
    frame = sFrame;
    markOk = OracleLookupMark(frame, &tick, &mode);
    gdx3ds_cov_frame(frame, tick, mode);
    if (!shot && (sEvery == 0 || sFrame < (unsigned long)sStart || (sFrame % (unsigned long)sEvery) != 0)) {
        return;
    }
    if (shot) {
        memcpy(tag, sPendingTag, sizeof(tag));
        tag[sizeof(tag) - 1] = '\0';
    } else if (sEveryBmp) {
        /* [debug] fh_every_bmp: one BMP per captured frame, so the auto tag has to carry the
         * frame number -- a single "auto" name overwrites itself and leaves exactly one file,
         * which is useless for anything temporal (wobble2-progress.md section 2). */
        snprintf(tag, sizeof(tag), "auto%06lu", frame);
    } else {
        strcpy(tag, "auto");
    }
    if (sCaptures == 0) {
        OracleLog("[fh] armed: captured frames are readback-stalled and are NOT perf samples");
    }
    /* Frame-N guarantee: C3D_FrameEnd queued frame N's P3D command list and its display transfer
     * on citro3d's gx command queue, which executes in submission order. The capture below
     * queues its OWN display transfer behind them and polls a sentinel until it lands, so the
     * read proves frame N's GPU work completed. (C3D_FrameSync is citro3d's vblank/framerate
     * pacing wait, not a completion wait — deliberately not called here: it would cost a vblank
     * per captured frame and prove nothing.) */
    if (sEyeMask & 1) {
        CaptureEye(0, tag, shot || sEveryBmp, frame, tick, mode, markOk);
    }
    if (sEyeMask & 2) {
        CaptureEye(1, tag, shot || sEveryBmp, frame, tick, mode, markOk);
    }
}

void gdx3ds_oracle_frame_mark(unsigned long frame, unsigned long tick, int gameMode) {
    const unsigned slot = (unsigned)(frame % ORACLE_MARK_RING);
    if (sQuitAtTick != 0 && tick >= sQuitAtTick && !sQuit) {
        gdx3ds_oracle_request_quit("[debug] quit_at_tick reached");
    }
    sMarks[slot].tick = tick;
    sMarks[slot].mode = gameMode;
    /* release: the payload above must be visible to core 2 before the key it reads */
    __atomic_store_n(&sMarks[slot].frame, frame, __ATOMIC_RELEASE);
}

static void SanitizeTag(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    if (src == NULL || src[0] == '\0') {
        src = "req";
    }
    while (src[i] != '\0' && i + 1 < cap) {
        const char c = src[i];
        const int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                       c == '-' || c == '.';
        dst[i] = ok ? c : '_';
        i++;
    }
    dst[i] = '\0';
}

void gdx_oracle_capture_request(const char* tag) {
    if (!sEnabled) {
        OracleLog("[fh] capture request ignored ([debug] fh=0)");
        return;
    }
    SanitizeTag(sPendingTag, sizeof(sPendingTag), tag);
    __atomic_store_n(&sPendingArmed, 1, __ATOMIC_RELEASE);
}

int gdx3ds_oracle_enabled(void) {
    return sEnabled;
}

int gdx3ds_oracle_owns_shot(void) {
    return sEnabled && !sLegacyShot;
}

const char* gdx3ds_oracle_build_id(void) {
    return GDX3DS_BUILD_ID;
}

int gdx3ds_oracle_quit_requested(void) {
    return sQuit;
}

void gdx3ds_oracle_request_quit(const char* why) {
    char line[128];
    snprintf(line, sizeof(line), "[oracle] quit requested: %s", why != NULL ? why : "?");
    OracleLog(line);
    sQuit = 1;
}

/* Read by the legacy SHOT dumps in port/n64_gfx_bridge.cpp (weak-linked: desktop builds do
 * not compile this TU). Latched by gdx3ds_oracle_init before any capture can run. */
int gdx3ds_shot_fastwrite(void) { return sFastWrite; }

void gdx3ds_oracle_init(void) {
    const char* eye;
    const char* platform;
    char line[192];
    sEnabled = gdx3ds_config_get_bool("debug", "fh", 1) ? 1 : 0;
    sEvery = gdx3ds_config_get_int("debug", "fh_every", 0);
    sStart = gdx3ds_config_get_int("debug", "fh_start", 0);
    if (sStart < 0) {
        sStart = 0;
    }
    if (sEvery < 0 || !sEnabled) {
        sEvery = 0;
    }
    eye = gdx3ds_config_get_string("debug", "fh_eye", "L");
    if (strcmp(eye, "R") == 0 || strcmp(eye, "r") == 0) {
        sEyeMask = 2;
    } else if (strcmp(eye, "both") == 0 || strcmp(eye, "LR") == 0) {
        sEyeMask = 3;
    } else {
        sEyeMask = 1;
    }
    sScale = gdx3ds_config_get_int("debug", "fh_scale", 0);
    if (sScale != 0 && (sScale < 160 || sScale > (int)ORACLE_W)) {
        sScale = 0;
    }
    sEveryBmp = gdx3ds_config_get_bool("debug", "fh_every_bmp", 0) ? 1 : 0;
    sLegacyShot = gdx3ds_config_get_bool("debug", "shot_legacy", 0) ? 1 : 0;
    sFastWrite = gdx3ds_config_get_bool("debug", "shot_fastwrite", 1) ? 1 : 0;
    sQuitAtTick = (unsigned long)gdx3ds_config_get_int("debug", "quit_at_tick", 0);
    platform = gdx3ds_config_get_string("debug", "platform", "unknown");
    snprintf(sPlatform, sizeof(sPlatform), "%s", platform);
    if (sEnabled) {
        mkdir("autotest", 0777); /* EEXIST is fine; CWD-relative = sdmc root */
    }
    snprintf(line, sizeof(line), "[fh] init enabled=%d every=%d start=%d eye=%s scale=%d every_bmp=%d legacy_shot=%d quit_at_tick=%lu build=%s platform=%s",
             sEnabled, sEvery, sStart, sEyeMask == 3 ? "both" : (sEyeMask == 2 ? "R" : "L"), sScale, sEveryBmp, sLegacyShot,
             sQuitAtTick, GDX3DS_BUILD_ID, sPlatform);
    OracleLog(line);
}
