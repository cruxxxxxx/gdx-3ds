/* Standalone unit-test harness for port/gdx_audio_capture.c. Console exe, no game deps, no
 * libultraship, no decomp headers: the module compiles unmodified alongside this file (see
 * port/CMakeLists.txt's gdx_pcm_capture_tests) and is driven entirely through its public API, plus
 * the test-only reset seam gdx_pcm_capture_reset_for_test() so one process can run several
 * independent capture sessions.
 *
 * The module reads GDX_PCM_CAPTURE / GDX_PCM_CAPTURE_FRAMES from the environment, so each case sets
 * those, resets the module, re-inits, feeds known s16 stereo frames, then reads the produced
 * <prefix>.pcm and <prefix>.pcm.sha256 back off disk to verify: the file bytes are exactly the fed
 * samples, the frame cap is honored, the streamed SHA-256 in the sidecar equals an INDEPENDENT
 * SHA-256 (a second implementation, transcribed below) of the file's bytes, and the
 * active/finished/no-op semantics hold.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public API under test, mirrored locally to avoid an include path. */
void gdx_pcm_capture_init(void);
void gdx_pcm_capture_arm(void);
void gdx_pcm_capture_feed(const int16_t* frames, unsigned int frameCount, unsigned int sampleRate);
int  gdx_pcm_capture_active(void);
int  gdx_pcm_capture_finished(void);
void gdx_pcm_capture_shutdown(void);
/* Test-only reset seam (declared in gdx_audio_capture.c, not the public header). */
void gdx_pcm_capture_reset_for_test(void);

/* Sub-check bookkeeping, same shape as dsp_tests.c. */
static int gSubChecks = 0;
static int gSubFails = 0;

static void checkTrue(const char* what, int cond) {
    gSubChecks++;
    if (!cond) {
        gSubFails++;
        printf("    [x] %s\n", what);
    }
}

static void checkEqLong(const char* what, long got, long expected) {
    gSubChecks++;
    if (got != expected) {
        gSubFails++;
        printf("    [x] %s: got %ld, expected %ld\n", what, got, expected);
    }
}

/* Independent SHA-256, a second implementation used to cross-check the module's own digest. */
typedef struct { uint32_t s[8]; uint64_t n; uint8_t b[64]; } Sha;
static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
static uint32_t ror(uint32_t v, int b) { return (v >> b) | (v << (32 - b)); }
static void shaTx(uint32_t s[8], const uint8_t* p) {
    uint32_t w[64], a, b, c, d, e, f, g, h; int i;
    for (i = 0; i < 16; i++) w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    a=s[0];b=s[1];c=s[2];d=s[3];e=s[4];f=s[5];g=s[6];h=s[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25), ch=(e&f)^((~e)&g), t1=h+S1+ch+K[i]+w[i];
        uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22), mj=(a&b)^(a&c)^(b&c), t2=S0+mj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
}
static void shaInit(Sha* x) {
    x->s[0]=0x6a09e667u;x->s[1]=0xbb67ae85u;x->s[2]=0x3c6ef372u;x->s[3]=0xa54ff53au;
    x->s[4]=0x510e527fu;x->s[5]=0x9b05688cu;x->s[6]=0x1f83d9abu;x->s[7]=0x5be0cd19u;x->n=0;
}
static void shaUpd(Sha* x, const uint8_t* d, size_t len) {
    size_t idx = (size_t)(x->n & 63u), i = 0; x->n += len;
    if (idx) { size_t part = 64 - idx; if (len < part) { memcpy(x->b+idx, d, len); return; }
        memcpy(x->b+idx, d, part); shaTx(x->s, x->b); i = part; }
    for (; i + 63 < len; i += 64) shaTx(x->s, d + i);
    memcpy(x->b, d + i, len - i);
}
static void shaHex(Sha* x, char out[65]) {
    uint64_t bits = x->n << 3; uint8_t lb[8], pad = 0x80, dg[32]; int i;
    static const char hx[16] = {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};
    shaUpd(x, &pad, 1); pad = 0;
    while ((x->n & 63u) != 56u) shaUpd(x, &pad, 1);
    for (i = 0; i < 8; i++) lb[i] = (uint8_t)((bits >> ((7-i)*8)) & 0xFF);
    shaUpd(x, lb, 8);
    for (i = 0; i < 32; i++) dg[i] = (uint8_t)((x->s[i>>2] >> ((3-(i&3))*8)) & 0xFF);
    for (i = 0; i < 32; i++) { out[i*2] = hx[dg[i]>>4]; out[i*2+1] = hx[dg[i]&0xF]; }
    out[64] = '\0';
}

/* File and env helpers. Plain CRT is fine here: this test never runs on a fiber. */
static void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof(buf), "%s=%s", name, value ? value : "");
    _putenv(buf);
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

/* Read a whole file into a malloc'd buffer. Returns bytes read, -1 if the file is absent. */
static long readFile(const char* path, uint8_t** out) {
    FILE* f = fopen(path, "rb");
    long n;
    *out = NULL;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return -1; }
    *out = (uint8_t*)malloc((size_t)n ? (size_t)n : 1);
    if (n > 0 && fread(*out, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(*out); *out = NULL; return -1; }
    fclose(f);
    return n;
}

static void removeArtifacts(const char* prefix) {
    char p[512];
    snprintf(p, sizeof(p), "%s.pcm", prefix); remove(p);
    snprintf(p, sizeof(p), "%s.pcm.sha256", prefix); remove(p);
}

/* Build interleaved s16 stereo frames with a deterministic pattern. */
static void fillFrames(int16_t* buf, unsigned frames, int seed) {
    unsigned i;
    for (i = 0; i < frames; i++) {
        buf[i*2+0] = (int16_t)(seed + (int)i * 7);
        buf[i*2+1] = (int16_t)(seed - (int)i * 3);
    }
}

/* Verify the sidecar's leading hex token equals the independent SHA-256 of the .pcm file. */
static int sidecarMatchesFile(const char* prefix) {
    char pcm[512], sha[512], expected[65], got[128];
    uint8_t* bytes; long n; FILE* f; Sha s;
    snprintf(pcm, sizeof(pcm), "%s.pcm", prefix);
    snprintf(sha, sizeof(sha), "%s.pcm.sha256", prefix);
    n = readFile(pcm, &bytes);
    if (n < 0) return 0;
    shaInit(&s);
    if (n > 0) shaUpd(&s, bytes, (size_t)n);
    shaHex(&s, expected);
    free(bytes);
    f = fopen(sha, "rb");
    if (!f) return 0;
    got[0] = '\0';
    if (fscanf(f, "%127s", got) != 1) { fclose(f); return 0; }
    fclose(f);
    return strcmp(got, expected) == 0;
}

static void CaseFrameLimitStop(void) {
    const char* prefix = "gdx_pcm_test_limit";
    const unsigned limit = 100;
    int16_t chunk[128 * 2];
    long n; uint8_t* bytes;

    removeArtifacts(prefix);
    gdx_pcm_capture_reset_for_test();
    setEnv("GDX_PCM_CAPTURE", prefix);
    setEnv("GDX_PCM_CAPTURE_FRAMES", "100");
    gdx_pcm_capture_init();

    checkTrue("armed after init when configured", gdx_pcm_capture_active() != 0);
    checkTrue("not finished before any feed", gdx_pcm_capture_finished() == 0);

    /* Feed 80 then 80 frames = 160 total, but the cap is 100 -> only 100 frames written. */
    fillFrames(chunk, 80, 1000);
    gdx_pcm_capture_feed(chunk, 80, 32000);
    checkTrue("not finished at 80/100", gdx_pcm_capture_finished() == 0);
    fillFrames(chunk, 80, 2000);
    gdx_pcm_capture_feed(chunk, 80, 32000);
    checkTrue("finished once cap reached", gdx_pcm_capture_finished() != 0);
    checkTrue("inactive after finalize", gdx_pcm_capture_active() == 0);

    /* Further feeds after finalize are no-ops. */
    fillFrames(chunk, 80, 3000);
    gdx_pcm_capture_feed(chunk, 80, 32000);

    n = readFile("gdx_pcm_test_limit.pcm", &bytes);
    checkEqLong(".pcm byte length == cap*4", n, (long)(limit * 4u));
    if (n >= (long)(limit * 4u)) {
        /* First 80 frames come from the first chunk (seed 1000). */
        int16_t expect[80 * 2];
        fillFrames(expect, 80, 1000);
        checkTrue("first 80 frames match chunk 1", memcmp(bytes, expect, sizeof(expect)) == 0);
        /* Frames 80..99 (20 frames) come from the head of the second chunk (seed 2000). */
        {
            int16_t expect2[20 * 2];
            fillFrames(expect2, 20, 2000);
            checkTrue("frames 80..99 match chunk 2 head",
                      memcmp(bytes + 80 * 2 * sizeof(int16_t), expect2, sizeof(expect2)) == 0);
        }
    }
    free(bytes);
    checkTrue("sidecar SHA-256 matches file bytes", sidecarMatchesFile(prefix));
    removeArtifacts(prefix);
}

static void CaseUnboundedShutdown(void) {
    const char* prefix = "gdx_pcm_test_unbounded";
    int16_t chunk[64 * 2];
    long n; uint8_t* bytes;

    removeArtifacts(prefix);
    gdx_pcm_capture_reset_for_test();
    setEnv("GDX_PCM_CAPTURE", prefix);
    setEnv("GDX_PCM_CAPTURE_FRAMES", NULL); /* unbounded */
    gdx_pcm_capture_init();

    fillFrames(chunk, 64, 500);
    gdx_pcm_capture_feed(chunk, 64, 32000);
    checkTrue("still active while unbounded", gdx_pcm_capture_active() != 0);
    checkTrue("not finished while unbounded", gdx_pcm_capture_finished() == 0);

    gdx_pcm_capture_shutdown();
    checkTrue("finished after shutdown", gdx_pcm_capture_finished() != 0);

    n = readFile("gdx_pcm_test_unbounded.pcm", &bytes);
    checkEqLong(".pcm holds all fed frames", n, (long)(64 * 4));
    if (n == 64 * 4) checkTrue("bytes match fed frames", memcmp(bytes, chunk, (size_t)n) == 0);
    free(bytes);
    checkTrue("sidecar SHA-256 matches file bytes", sidecarMatchesFile(prefix));
    removeArtifacts(prefix);
}

static void CaseUnconfiguredNoOp(void) {
    uint8_t* bytes;
    int16_t chunk[8 * 2];

    removeArtifacts("gdx_pcm_test_should_not_exist");
    gdx_pcm_capture_reset_for_test();
    setEnv("GDX_PCM_CAPTURE", NULL); /* unset -> module is a no-op */
    setEnv("GDX_PCM_CAPTURE_FRAMES", NULL);
    gdx_pcm_capture_init();

    checkTrue("inactive when unconfigured", gdx_pcm_capture_active() == 0);
    checkTrue("never finished when unconfigured", gdx_pcm_capture_finished() == 0);
    fillFrames(chunk, 8, 1);
    gdx_pcm_capture_feed(chunk, 8, 32000); /* no-op: must not create a file */
    checkEqLong("no .pcm created when unconfigured",
                readFile("gdx_pcm_test_should_not_exist.pcm", &bytes), -1);
    free(bytes);
}

static void CaseArmIdempotentAndDeterministic(void) {
    /* Two identical sessions must yield identical SHA (the property the golden gate relies on). */
    const char* prefix = "gdx_pcm_test_det";
    char first[65];
    int16_t chunk[50 * 2];
    FILE* f;

    /* Session 1. */
    removeArtifacts(prefix);
    gdx_pcm_capture_reset_for_test();
    setEnv("GDX_PCM_CAPTURE", prefix);
    setEnv("GDX_PCM_CAPTURE_FRAMES", "50");
    gdx_pcm_capture_init();
    gdx_pcm_capture_arm(); /* idempotent: must not reset or double-open */
    checkTrue("arm is idempotent (still active)", gdx_pcm_capture_active() != 0);
    fillFrames(chunk, 50, 77);
    gdx_pcm_capture_feed(chunk, 50, 32000);
    checkTrue("finished at cap", gdx_pcm_capture_finished() != 0);
    checkTrue("session1 sidecar matches file", sidecarMatchesFile(prefix));
    f = fopen("gdx_pcm_test_det.pcm.sha256", "rb");
    first[0] = '\0';
    if (f) { if (fscanf(f, "%64s", first) != 1) first[0] = '\0'; fclose(f); }

    /* Session 2 -- identical input. */
    removeArtifacts(prefix);
    gdx_pcm_capture_reset_for_test();
    setEnv("GDX_PCM_CAPTURE", prefix);
    setEnv("GDX_PCM_CAPTURE_FRAMES", "50");
    gdx_pcm_capture_init();
    fillFrames(chunk, 50, 77);
    gdx_pcm_capture_feed(chunk, 50, 32000);
    {
        char second[65];
        FILE* g = fopen("gdx_pcm_test_det.pcm.sha256", "rb");
        second[0] = '\0';
        if (g) { if (fscanf(g, "%64s", second) != 1) second[0] = '\0'; fclose(g); }
        checkTrue("two identical sessions produce identical SHA-256",
                  first[0] != '\0' && strcmp(first, second) == 0);
    }
    removeArtifacts(prefix);
}

static void CaseWriteBehindBufferBoundary(void) {
    /* GDX_PCM_WRITE_BUF_CAP is module-private, so this 256 KiB value is duplicated deliberately.
     * If the module's cap ever changes, update this constant too. */
    const size_t writeBufCap = 256u * 1024u;
    const unsigned bytesPerFrame = 4u; /* s16 stereo */
    const unsigned framesExact = (unsigned)(writeBufCap / bytesPerFrame); /* lands exactly on the cap */
    const unsigned framesOverflow = framesExact + 100u; /* exceeds the cap within one feed() call */

    /* Sub-case A: a single feed() whose byte count lands exactly on the write-behind buffer
     * boundary (sWriteBufLen == GDX_PCM_WRITE_BUF_CAP triggers an inline flush inside
     * gdx_pcm_buffer_append's loop). */
    {
        const char* prefix = "gdx_pcm_test_wbuf_exact";
        int16_t* chunk = (int16_t*)malloc((size_t)framesExact * 2 * sizeof(int16_t));
        long n; uint8_t* bytes;

        removeArtifacts(prefix);
        gdx_pcm_capture_reset_for_test();
        setEnv("GDX_PCM_CAPTURE", prefix);
        setEnv("GDX_PCM_CAPTURE_FRAMES", NULL); /* unbounded: shutdown() finalizes */
        gdx_pcm_capture_init();

        fillFrames(chunk, framesExact, 42);
        gdx_pcm_capture_feed(chunk, framesExact, 32000);
        gdx_pcm_capture_shutdown();

        n = readFile("gdx_pcm_test_wbuf_exact.pcm", &bytes);
        checkEqLong("exact-fill: .pcm byte length == framesExact*4", n,
                    (long)((size_t)framesExact * bytesPerFrame));
        if (n == (long)((size_t)framesExact * bytesPerFrame)) {
            checkTrue("exact-fill: bytes match fed frames", memcmp(bytes, chunk, (size_t)n) == 0);
        }
        free(bytes);
        checkTrue("exact-fill: sidecar SHA-256 matches file bytes", sidecarMatchesFile(prefix));
        removeArtifacts(prefix);
        free(chunk);
    }

    /* Sub-case B: a single feed() whose byte count exceeds the write-behind buffer capacity,
     * forcing gdx_pcm_buffer_append's internal loop to flush mid-call and continue buffering
     * the remainder. */
    {
        const char* prefix = "gdx_pcm_test_wbuf_overflow";
        int16_t* chunk = (int16_t*)malloc((size_t)framesOverflow * 2 * sizeof(int16_t));
        long n; uint8_t* bytes;

        removeArtifacts(prefix);
        gdx_pcm_capture_reset_for_test();
        setEnv("GDX_PCM_CAPTURE", prefix);
        setEnv("GDX_PCM_CAPTURE_FRAMES", NULL); /* unbounded: shutdown() finalizes */
        gdx_pcm_capture_init();

        fillFrames(chunk, framesOverflow, 4242);
        gdx_pcm_capture_feed(chunk, framesOverflow, 32000);
        gdx_pcm_capture_shutdown();

        n = readFile("gdx_pcm_test_wbuf_overflow.pcm", &bytes);
        checkEqLong("overflow: .pcm byte length == framesOverflow*4", n,
                    (long)((size_t)framesOverflow * bytesPerFrame));
        if (n == (long)((size_t)framesOverflow * bytesPerFrame)) {
            checkTrue("overflow: bytes match fed frames", memcmp(bytes, chunk, (size_t)n) == 0);
        }
        free(bytes);
        checkTrue("overflow: sidecar SHA-256 matches file bytes", sidecarMatchesFile(prefix));
        removeArtifacts(prefix);
        free(chunk);
    }
}

int main(void) {
    struct { const char* name; void (*fn)(void); } cases[] = {
        { "frame-limit stop + byte/window correctness", CaseFrameLimitStop },
        { "unbounded capture finalized by shutdown",     CaseUnboundedShutdown },
        { "unconfigured module is a total no-op",        CaseUnconfiguredNoOp },
        { "arm idempotency + run-to-run determinism",    CaseArmIdempotentAndDeterministic },
        { "write-behind buffer boundary (exact fill + single-feed overflow)",
          CaseWriteBehindBufferBoundary },
    };
    int numCases = (int)(sizeof(cases) / sizeof(cases[0]));
    int i, failedCases = 0;

    printf("=== G-Diffuser PCM capture unit-test harness (port/gdx_audio_capture.c) ===\n\n");
    for (i = 0; i < numCases; i++) {
        int failsBefore = gSubFails;
        int checksBefore = gSubChecks;
        printf("-- %s\n", cases[i].name);
        cases[i].fn();
        if (gSubFails > failsBefore) {
            failedCases++;
            printf("[FAIL] %s (%d/%d sub-checks failed)\n\n",
                   cases[i].name, gSubFails - failsBefore, gSubChecks - checksBefore);
        } else {
            printf("[PASS] %s (%d sub-checks)\n\n", cases[i].name, gSubChecks - checksBefore);
        }
    }
    printf("=== Summary: %d/%d cases passed (%d/%d sub-checks passed) ===\n",
           numCases - failedCases, numCases, gSubChecks - gSubFails, gSubChecks);
    return failedCases ? 1 : 0;
}
