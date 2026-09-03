// Standalone unit-test harness for port/n64_audio_hle.c, the software RSP audio DSP interpreter.
// Console exe, no game or asset dependencies -- every vector is synthetic and built in this file.
//
// The interpreter is driven ENTIRELY through its real public entry point,
//     void gdx_audio_hle_run(const void* dataPtr, unsigned int dataSizeBytes);
// exactly as port/n64_sched.c does for a real M_AUDTASK, so n64_audio_hle.c compiles into this
// target completely unmodified (see port/CMakeLists.txt's gdx_dsp_tests). Extracting the DSP
// kernels instead is not mechanical -- RunAdpcm/RunResample/RunFilter are static and ENVMIXER's
// math lives inline in the dispatch switch -- and #include-ing the .c behind a testing guard would
// change what is under test. Every path (VADPCM decode, loop restore, nibble unpack, resample, FIR
// filter, envelope mixer) is reachable through ordinary Acmd command lists, using LOADBUFF/
// SAVEBUFF to move known byte patterns in and out of the interpreter's private DMEM.
//
// Its only non-libc dependencies are the two pointer resolvers normally defined in
// n64_gfx_bridge.cpp, which turn a truncated 32-bit Acmd "address" back into a host pointer, plus
// CVarGetInteger. The stand-ins below are a flat table of fake-address regions (RegisterRegion) and
// a CVar stub returning each requested default, so the tests exercise stock DSP behavior.
//
// The MkXxx encoders mirror the exact w0/w1 bit layouts n64_audio_hle.c's `switch (op)` reads,
// verified field by field against that file.
//
// Each Test*() prints every failing sub-check with expected/actual; main() aggregates and exits
// nonzero if any test failed.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Authoritative RSP resample coefficients, transcribed from the same canonical source
// n64_audio_hle.c uses (mupen64plus-rsp-hle RESAMPLE_LUT / Project64 AziAudio
// Mupen64plusHLE/audio.c, extracted from the aspMain ucode data section). This test-local copy
// exists so the resample tests check the interpreter against an INDEPENDENT copy of the ground
// truth rather than against its own array. Layout: kResampleLut[phase][tap], phase =
// (fracQ16 >> 10) & 63, taps weight {x[-1], x[0], x[+1], x[+2]}.
#define S16T(x) ((int16_t)(x))
static const int16_t kResampleLut[64][4] = {
    { S16T(0x0c39), S16T(0x66ad), S16T(0x0d46), S16T(0xffdf) },
    { S16T(0x0b39), S16T(0x6696), S16T(0x0e5f), S16T(0xffd8) },
    { S16T(0x0a44), S16T(0x6669), S16T(0x0f83), S16T(0xffd0) },
    { S16T(0x095a), S16T(0x6626), S16T(0x10b4), S16T(0xffc8) },
    { S16T(0x087d), S16T(0x65cd), S16T(0x11f0), S16T(0xffbf) },
    { S16T(0x07ab), S16T(0x655e), S16T(0x1338), S16T(0xffb6) },
    { S16T(0x06e4), S16T(0x64d9), S16T(0x148c), S16T(0xffac) },
    { S16T(0x0628), S16T(0x643f), S16T(0x15eb), S16T(0xffa1) },
    { S16T(0x0577), S16T(0x638f), S16T(0x1756), S16T(0xff96) },
    { S16T(0x04d1), S16T(0x62cb), S16T(0x18cb), S16T(0xff8a) },
    { S16T(0x0435), S16T(0x61f3), S16T(0x1a4c), S16T(0xff7e) },
    { S16T(0x03a4), S16T(0x6106), S16T(0x1bd7), S16T(0xff71) },
    { S16T(0x031c), S16T(0x6007), S16T(0x1d6c), S16T(0xff64) },
    { S16T(0x029f), S16T(0x5ef5), S16T(0x1f0b), S16T(0xff56) },
    { S16T(0x022a), S16T(0x5dd0), S16T(0x20b3), S16T(0xff48) },
    { S16T(0x01be), S16T(0x5c9a), S16T(0x2264), S16T(0xff3a) },
    { S16T(0x015b), S16T(0x5b53), S16T(0x241e), S16T(0xff2c) },
    { S16T(0x0101), S16T(0x59fc), S16T(0x25e0), S16T(0xff1e) },
    { S16T(0x00ae), S16T(0x5896), S16T(0x27a9), S16T(0xff10) },
    { S16T(0x0063), S16T(0x5720), S16T(0x297a), S16T(0xff02) },
    { S16T(0x001f), S16T(0x559d), S16T(0x2b50), S16T(0xfef4) },
    { S16T(0xffe2), S16T(0x540d), S16T(0x2d2c), S16T(0xfee8) },
    { S16T(0xffac), S16T(0x5270), S16T(0x2f0d), S16T(0xfedb) },
    { S16T(0xff7c), S16T(0x50c7), S16T(0x30f3), S16T(0xfed0) },
    { S16T(0xff53), S16T(0x4f14), S16T(0x32dc), S16T(0xfec6) },
    { S16T(0xff2e), S16T(0x4d57), S16T(0x34c8), S16T(0xfebd) },
    { S16T(0xff0f), S16T(0x4b91), S16T(0x36b6), S16T(0xfeb6) },
    { S16T(0xfef5), S16T(0x49c2), S16T(0x38a5), S16T(0xfeb0) },
    { S16T(0xfedf), S16T(0x47ed), S16T(0x3a95), S16T(0xfeac) },
    { S16T(0xfece), S16T(0x4611), S16T(0x3c85), S16T(0xfeab) },
    { S16T(0xfec0), S16T(0x4430), S16T(0x3e74), S16T(0xfeac) },
    { S16T(0xfeb6), S16T(0x424a), S16T(0x4060), S16T(0xfeaf) },
    { S16T(0xfeaf), S16T(0x4060), S16T(0x424a), S16T(0xfeb6) },
    { S16T(0xfeac), S16T(0x3e74), S16T(0x4430), S16T(0xfec0) },
    { S16T(0xfeab), S16T(0x3c85), S16T(0x4611), S16T(0xfece) },
    { S16T(0xfeac), S16T(0x3a95), S16T(0x47ed), S16T(0xfedf) },
    { S16T(0xfeb0), S16T(0x38a5), S16T(0x49c2), S16T(0xfef5) },
    { S16T(0xfeb6), S16T(0x36b6), S16T(0x4b91), S16T(0xff0f) },
    { S16T(0xfebd), S16T(0x34c8), S16T(0x4d57), S16T(0xff2e) },
    { S16T(0xfec6), S16T(0x32dc), S16T(0x4f14), S16T(0xff53) },
    { S16T(0xfed0), S16T(0x30f3), S16T(0x50c7), S16T(0xff7c) },
    { S16T(0xfedb), S16T(0x2f0d), S16T(0x5270), S16T(0xffac) },
    { S16T(0xfee8), S16T(0x2d2c), S16T(0x540d), S16T(0xffe2) },
    { S16T(0xfef4), S16T(0x2b50), S16T(0x559d), S16T(0x001f) },
    { S16T(0xff02), S16T(0x297a), S16T(0x5720), S16T(0x0063) },
    { S16T(0xff10), S16T(0x27a9), S16T(0x5896), S16T(0x00ae) },
    { S16T(0xff1e), S16T(0x25e0), S16T(0x59fc), S16T(0x0101) },
    { S16T(0xff2c), S16T(0x241e), S16T(0x5b53), S16T(0x015b) },
    { S16T(0xff3a), S16T(0x2264), S16T(0x5c9a), S16T(0x01be) },
    { S16T(0xff48), S16T(0x20b3), S16T(0x5dd0), S16T(0x022a) },
    { S16T(0xff56), S16T(0x1f0b), S16T(0x5ef5), S16T(0x029f) },
    { S16T(0xff64), S16T(0x1d6c), S16T(0x6007), S16T(0x031c) },
    { S16T(0xff71), S16T(0x1bd7), S16T(0x6106), S16T(0x03a4) },
    { S16T(0xff7e), S16T(0x1a4c), S16T(0x61f3), S16T(0x0435) },
    { S16T(0xff8a), S16T(0x18cb), S16T(0x62cb), S16T(0x04d1) },
    { S16T(0xff96), S16T(0x1756), S16T(0x638f), S16T(0x0577) },
    { S16T(0xffa1), S16T(0x15eb), S16T(0x643f), S16T(0x0628) },
    { S16T(0xffac), S16T(0x148c), S16T(0x64d9), S16T(0x06e4) },
    { S16T(0xffb6), S16T(0x1338), S16T(0x655e), S16T(0x07ab) },
    { S16T(0xffbf), S16T(0x11f0), S16T(0x65cd), S16T(0x087d) },
    { S16T(0xffc8), S16T(0x10b4), S16T(0x6626), S16T(0x095a) },
    { S16T(0xffd0), S16T(0x0f83), S16T(0x6669), S16T(0x0a44) },
    { S16T(0xffd8), S16T(0x0e5f), S16T(0x6696), S16T(0x0b39) },
    { S16T(0xffdf), S16T(0x0d46), S16T(0x66ad), S16T(0x0c39) },
};

static int16_t RefClampS16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Stand-in for the two resolvers n64_audio_hle.c expects from n64_gfx_bridge.cpp: a flat table of
// {fake 32-bit base, host pointer, size} regions registered on demand by the tests below. Mirrors
// the real resolver's "range containing this address" logic without the pointer-registry
// machinery, which is irrelevant to a single-process unit test.
/* Stand-in for the game-side race flag n64_audio_hle.c's capped diagnostic probes gate on. Here
   it only means the probes may log, which is harmless bounded noise in a test run. */
int gGdxRaceActive = 1;

#define GDX_TEST_MAX_REGIONS 256
typedef struct {
    uint32_t base;
    void* host;
    uint32_t size;
} GdxTestRegion;

static GdxTestRegion sTestRegions[GDX_TEST_MAX_REGIONS];
static int sTestRegionCount = 0;
static uint32_t sTestNextAddr = 0x1000u; /* never 0 -- n64_audio_hle.c treats raw==0 as "unset" */

static uint32_t RegisterRegion(void* host, uint32_t size) {
    uint32_t base;
    if (sTestRegionCount >= GDX_TEST_MAX_REGIONS) {
        fprintf(stderr, "FATAL: RegisterRegion: out of test region slots\n");
        exit(2);
    }
    base = sTestNextAddr;
    sTestRegions[sTestRegionCount].base = base;
    sTestRegions[sTestRegionCount].host = host;
    sTestRegions[sTestRegionCount].size = size;
    sTestRegionCount++;
    sTestNextAddr += size + 0x1000u; /* generous padding, never overlap */
    return base;
}

void* gdx_resolve_registered_host_address(unsigned int addr) {
    int i;
    for (i = 0; i < sTestRegionCount; i++) {
        uint32_t off = addr - sTestRegions[i].base;
        if (off < sTestRegions[i].size) {
            return (uint8_t*)sTestRegions[i].host + off;
        }
    }
    return NULL;
}

void* gdx_resolve_module_host_address(unsigned int addr) {
    (void)addr;
    return NULL; /* not used by this harness: every address we hand out is a registered region */
}

int CVarGetInteger(const char* name, int defaultValue) {
    (void)name;
    return defaultValue;
}

// No header declares this: n64_audio_hle.c defines it with external linkage and there is no
// prototype file, so the harness declares it itself.
extern void gdx_audio_hle_run(const void* dataPtr, unsigned int dataSizeBytes);

// Layout-identical to n64_audio_hle.c's private GdxAcmd (two uint32_t), which is all
// gdx_audio_hle_run cares about -- it never sees this struct's name.
typedef struct {
    uint32_t w0;
    uint32_t w1;
} Cmd;

static void RunCmds(const Cmd* cmds, int count) {
    gdx_audio_hle_run(cmds, (unsigned int)(count * (int)sizeof(Cmd)));
}

// Mirrored from n64_audio_hle.c's GDX_A_* enum (EXPANSION_KIT numbering); only the ones this
// harness exercises.
#define OP_ADPCM     1u
#define OP_CLEARBUFF 2u
#define OP_ADDMIXER  4u
#define OP_RESAMPLE  5u
#define OP_FILTER    7u
#define OP_SETBUFF   8u
#define OP_LOADADPCM 11u
#define OP_MIXER     12u
#define OP_SETLOOP   15u
#define OP_ENVSETUP1 18u
#define OP_ENVMIXER  19u
#define OP_LOADBUFF  20u
#define OP_SAVEBUFF  21u
#define OP_ENVSETUP2 22u

// Each encoder mirrors the exact bitfield extraction gdx_audio_hle_run performs for that opcode.
static Cmd MkClearBuff(uint32_t dmem, uint32_t size) {
    Cmd c; c.w0 = (OP_CLEARBUFF << 24) | (dmem & 0xFFFFFFu); c.w1 = size; return c;
}
static Cmd MkSetBuff(uint32_t dmemIn, uint32_t dmemOut, uint32_t countBytes) {
    Cmd c;
    c.w0 = (OP_SETBUFF << 24) | (dmemIn & 0xFFFFu);
    c.w1 = ((dmemOut & 0xFFFFu) << 16) | (countBytes & 0xFFFFu);
    return c;
}
static Cmd MkLoadBuff(uint32_t dmemDest, uint32_t sizeBytes /* must be mult of 16 */, uint32_t rawAddr) {
    Cmd c;
    c.w0 = (OP_LOADBUFF << 24) | (((sizeBytes >> 4) & 0xFFu) << 16) | (dmemDest & 0xFFFFu);
    c.w1 = rawAddr;
    return c;
}
static Cmd MkSaveBuff(uint32_t dmemSrc, uint32_t sizeBytes /* must be mult of 16 */, uint32_t rawAddr) {
    Cmd c;
    c.w0 = (OP_SAVEBUFF << 24) | (((sizeBytes >> 4) & 0xFFu) << 16) | (dmemSrc & 0xFFFFu);
    c.w1 = rawAddr;
    return c;
}
static Cmd MkLoadAdpcm(uint32_t byteCount, uint32_t rawAddr) {
    Cmd c; c.w0 = (OP_LOADADPCM << 24) | (byteCount & 0xFFFFFFu); c.w1 = rawAddr; return c;
}
static Cmd MkSetLoop(uint32_t rawAddr) {
    Cmd c; c.w0 = (OP_SETLOOP << 24); c.w1 = rawAddr; return c;
}
static Cmd MkAdpcm(uint32_t flags, uint32_t rawStateAddr) {
    Cmd c; c.w0 = (OP_ADPCM << 24) | ((flags & 0xFFu) << 16); c.w1 = rawStateAddr; return c;
}
static Cmd MkResample(uint32_t flags, uint32_t pitch, uint32_t rawStateAddr) {
    Cmd c;
    c.w0 = (OP_RESAMPLE << 24) | ((flags & 0xFFu) << 16) | (pitch & 0xFFFFu);
    c.w1 = rawStateAddr;
    return c;
}
static Cmd MkFilterPrime(uint32_t sizeBytes, uint32_t rawCoefAddr) {
    Cmd c; c.w0 = (OP_FILTER << 24) | (2u << 16) | (sizeBytes & 0xFFFFu); c.w1 = rawCoefAddr; return c;
}
static Cmd MkFilterApply(uint32_t f, uint32_t dmemBuf, uint32_t rawStateAddr) {
    Cmd c; c.w0 = (OP_FILTER << 24) | ((f & 0xFFu) << 16) | (dmemBuf & 0xFFFFu); c.w1 = rawStateAddr; return c;
}
static Cmd MkEnvSetup1(uint32_t a, int32_t b, int32_t c_, int32_t d) {
    Cmd c;
    c.w0 = (OP_ENVSETUP1 << 24) | ((a & 0xFFu) << 16) | ((uint32_t)b & 0xFFFFu);
    c.w1 = (((uint32_t)c_ & 0xFFFFu) << 16) | ((uint32_t)d & 0xFFFFu);
    return c;
}
static Cmd MkEnvSetup2(uint32_t curVolLeft, uint32_t curVolRight) {
    Cmd c; c.w0 = (OP_ENVSETUP2 << 24); c.w1 = ((curVolLeft & 0xFFFFu) << 16) | (curVolRight & 0xFFFFu); return c;
}
/* count8 = groups of 8 samples (real ucode's count>>4 packing); gain is a SIGNED Q15 value
   (mirrors decomp/src/audio/disk/lib/synthesis.c's aMix call sites, e.g. reverb->volume,
   reverb->decayRatio+0x8000, reverb->leakRtl/leakLtr -- all plain signed 16-bit fields). */
static Cmd MkMixer(uint32_t count8, int32_t gain, uint32_t dmemIn, uint32_t dmemOut) {
    Cmd c;
    c.w0 = (OP_MIXER << 24) | ((count8 & 0xFFu) << 16) | ((uint32_t)gain & 0xFFFFu);
    c.w1 = ((dmemIn & 0xFFFFu) << 16) | (dmemOut & 0xFFFFu);
    return c;
}
static Cmd MkEnvMixer(uint32_t dmemSrc, uint32_t sampleCount, uint32_t swapLR,
                      uint32_t dryLeft, uint32_t dryRight, uint32_t wetLeft, uint32_t wetRight) {
    Cmd c;
    c.w0 = (OP_ENVMIXER << 24) | (((dmemSrc >> 4) & 0xFFu) << 16) | ((sampleCount & 0xFFu) << 8) | ((swapLR & 1u) << 4);
    c.w1 = (((dryLeft >> 4) & 0xFFu) << 24) | (((dryRight >> 4) & 0xFFu) << 16) |
           (((wetLeft >> 4) & 0xFFu) << 8) | ((wetRight >> 4) & 0xFFu);
    return c;
}
/* count8 = groups of 8 samples (abi.h's aAddMixer packs `count>>4`, the same convention as
   A_MIXER's count8 above). a4 is deliberately not exposed: the interpreter does not read it --
   gainless unity add, matching mupen's alist_add. */
static Cmd MkAddMixer(uint32_t count8, uint32_t dmemIn, uint32_t dmemOut) {
    Cmd c;
    c.w0 = (OP_ADDMIXER << 24) | ((count8 & 0xFFu) << 16);
    c.w1 = ((dmemIn & 0xFFFFu) << 16) | (dmemOut & 0xFFFFu);
    return c;
}

static int gSubChecks = 0;
static int gSubFails = 0;

static void CheckEq32(const char* desc, int64_t expected, int64_t actual) {
    gSubChecks++;
    if (expected != actual) {
        gSubFails++;
        printf("       MISMATCH: %s -- expected=%lld actual=%lld\n", desc, (long long)expected, (long long)actual);
    }
}

/* Compares two s16 arrays with a tolerance (0 for bit-exact). Reports every offending index. */
static void CheckS16Array(const char* label, const int16_t* expected, const int16_t* actual, int count, int tolerance) {
    int i;
    for (i = 0; i < count; i++) {
        int diff = (int)expected[i] - (int)actual[i];
        if (diff < 0) diff = -diff;
        gSubChecks++;
        if (diff > tolerance) {
            gSubFails++;
            printf("       MISMATCH: %s[%d] -- expected=%d actual=%d (tolerance=%d)\n",
                   label, i, expected[i], actual[i], tolerance);
        }
    }
}

/* Loads `sizeBytes` (must be mult of 16) worth of `hostSrc` into DMEM at dmemDest via LOADBUFF. */
static void DmemLoad(uint32_t dmemDest, const void* hostSrc, uint32_t sizeBytes) {
    Cmd cmds[2];
    uint32_t addr = RegisterRegion((void*)hostSrc, sizeBytes);
    cmds[0] = MkClearBuff(dmemDest, sizeBytes);
    cmds[1] = MkLoadBuff(dmemDest, sizeBytes, addr);
    RunCmds(cmds, 2);
}

/* Reads `sizeBytes` (must be mult of 16) worth of DMEM at dmemSrc into hostDst via SAVEBUFF. */
static void DmemSave(uint32_t dmemSrc, void* hostDst, uint32_t sizeBytes) {
    Cmd cmd;
    uint32_t addr = RegisterRegion(hostDst, sizeBytes);
    cmd = MkSaveBuff(dmemSrc, sizeBytes, addr);
    RunCmds(&cmd, 1);
}

// =================================================================================================
// TEST 1 -- VADPCM frame-boundary continuity: decoding 2 frames in ONE call must be byte-identical
// to decoding them in TWO calls with the persistent state buffer carried across. A wrong state
// snapshot corrupts every later chunk's prediction, heard as crackle at chunk boundaries.
// =================================================================================================
static void PackAdpcmFrame(uint8_t out9[9], uint8_t header, const int8_t nibbles[16]) {
    int i;
    out9[0] = header;
    for (i = 0; i < 8; i++) {
        uint8_t hi = (uint8_t)(nibbles[i * 2] & 0xF);
        uint8_t lo = (uint8_t)(nibbles[i * 2 + 1] & 0xF);
        out9[1 + i] = (uint8_t)((hi << 4) | lo);
    }
}

static int TestAdpcmFrameBoundaryContinuity(void) {
    /* One predictor, order 2: book[0] weights hist2, book[8] weights hist1. Both deliberately
       NONZERO, or a broken state carry would not perturb the waveform and this would pass
       trivially. */
    int16_t book[16];
    int8_t nibblesF1[16], nibblesF2[16];
    uint8_t frame1[9], frame2[9];
    uint8_t twoFrames[32]; /* 18 real bytes + 14 bytes zero pad (LOADBUFF needs mult-of-16 size) */
    uint8_t frame1Padded[16], frame2Padded[16];
    int16_t combinedState[16], splitState[16];
    int16_t combinedOut[32], splitOut0[16], splitOut1[16];
    Cmd cmds[8];
    int i;

    memset(book, 0, sizeof(book));
    book[0] = 600;   /* tap0 col0 -> weight on hist2 */
    book[8] = -300;  /* tap1 col0 -> weight on hist1 */

    for (i = 0; i < 16; i++) {
        nibblesF1[i] = (int8_t)(((i * 5 + 3) % 16) - 8);
        nibblesF2[i] = (int8_t)(((i * 7 + 1) % 16) - 8);
    }
    PackAdpcmFrame(frame1, 0x00 /* shift=0 pred=0 */, nibblesF1);
    PackAdpcmFrame(frame2, 0x00, nibblesF2);

    memset(twoFrames, 0, sizeof(twoFrames));
    memcpy(twoFrames + 0, frame1, 9);
    memcpy(twoFrames + 9, frame2, 9);

    memset(frame1Padded, 0, sizeof(frame1Padded));
    memcpy(frame1Padded, frame1, 9);
    memset(frame2Padded, 0, sizeof(frame2Padded));
    memcpy(frame2Padded, frame2, 9);

    /* --- Combined: one call decodes both frames (32 samples) in one shot. --- */
    memset(combinedState, 0, sizeof(combinedState));
    {
        uint32_t bookAddr = RegisterRegion(book, sizeof(book));
        uint32_t stateAddr = RegisterRegion(combinedState, sizeof(combinedState));
        DmemLoad(0x0000, twoFrames, 32);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetBuff(0x0000, 0x0100, 64 /* 32 samples */);
        cmds[2] = MkAdpcm(1 /* A_INIT */, stateAddr);
        RunCmds(cmds, 3);
        /* +32: fresh decode output starts after the 16-sample last-frame preamble
           (real ucode output layout -- see RunAdpcm's contract comment). */
        DmemSave(0x0100 + 32, combinedOut, 64);
    }

    /* --- Split: two separate gdx_audio_hle_run calls, same persistent state buffer. --- */
    memset(splitState, 0, sizeof(splitState));
    {
        uint32_t bookAddr = RegisterRegion(book, sizeof(book));
        uint32_t stateAddr = RegisterRegion(splitState, sizeof(splitState));

        /* Call 1: frame1 only, A_INIT. */
        DmemLoad(0x0200, frame1Padded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetBuff(0x0200, 0x0300, 32 /* 16 samples */);
        cmds[2] = MkAdpcm(1 /* A_INIT */, stateAddr);
        RunCmds(cmds, 3);
        DmemSave(0x0300 + 32, splitOut0, 32);

        /* Call 2: frame2 only, continuing (no A_INIT), same state buffer. */
        DmemLoad(0x0240, frame2Padded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetBuff(0x0240, 0x0340, 32 /* 16 samples */);
        cmds[2] = MkAdpcm(0 /* continue */, stateAddr);
        RunCmds(cmds, 3);
        DmemSave(0x0340 + 32, splitOut1, 32);
    }

    CheckS16Array("frame0 (combined vs split call1)", combinedOut + 0, splitOut0, 16, 0);
    CheckS16Array("frame1 (combined vs split call2)", combinedOut + 16, splitOut1, 16, 0);

    return 1;
}

// =================================================================================================
// TEST 2 -- A_SETLOOP restore: the first post-loop ADPCM frame must read its predictor history from
// the TAIL of loopState -- not loopState[0]/[1], and not the note's own adpcmdecState. Reference
// convention (mupen64plus-rsp-hle adpcm_compute_residuals, last_samples = last_frame+14):
// loopState[15] is the newest sample = hist1 (tap1/book[8]), loopState[14] the older = hist2
// (tap0/book[0]). The full 16-short loopState also becomes the output preamble.
// =================================================================================================
static int TestAdpcmLoopRestore(void) {
    int16_t book[16];
    int16_t loopState[16];
    int16_t decoyNoteState[16]; /* the unrelated per-note state -- must NOT be read for A_LOOP */
    uint8_t frame[9];
    int8_t nibbles[16];
    uint8_t framePadded[16];
    int16_t out[16];
    Cmd cmds[4];
    int i;

    /* coef0 (tap0 = hist2 = OLDER = loopState[14]) = 2048 == Q11 unity (2048>>11 == *1);
       coef1 (tap1 = hist1 = newest = loopState[15]) = 0, so the very first decoded sample
       equals loopState[14] exactly, isolating it from loopState[15] and from decoyNoteState. */
    memset(book, 0, sizeof(book));
    book[0] = 2048;
    book[8] = 0;

    memset(loopState, 0, sizeof(loopState));
    loopState[14] = 777;  /* hist2 (older) -- must be USED by sample 0 (tap0 unity) */
    loopState[15] = 333;  /* hist1 (newest) -- must be IGNORED by sample 0 (tap1==0) */

    memset(decoyNoteState, 0, sizeof(decoyNoteState));
    decoyNoteState[0] = 12345 & 0x7FFF; /* decoy: if code wrongly fell back to state[]/[1] instead
                                            of loopState[14]/[15], sample 0 would be wildly off */
    decoyNoteState[1] = -6789;

    for (i = 0; i < 16; i++) nibbles[i] = (int8_t)((i % 16) - 8);
    nibbles[0] = 0; /* residual0 = 0<<shift = 0, so decoded sample0 == predicted exactly */
    PackAdpcmFrame(frame, 0x00 /* shift=0 pred=0 */, nibbles);
    memset(framePadded, 0, sizeof(framePadded));
    memcpy(framePadded, frame, 9);

    {
        uint32_t bookAddr = RegisterRegion(book, sizeof(book));
        uint32_t loopAddr = RegisterRegion(loopState, sizeof(loopState));
        uint32_t noteStateAddr = RegisterRegion(decoyNoteState, sizeof(decoyNoteState));

        DmemLoad(0x0400, framePadded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetLoop(loopAddr);
        cmds[2] = MkSetBuff(0x0400, 0x0440, 32 /* 16 samples */);
        cmds[3] = MkAdpcm(2 /* A_LOOP */, noteStateAddr);
        RunCmds(cmds, 4);
        DmemSave(0x0440 + 32, out, 32);
    }

    CheckEq32("post-loop sample0 == loopState[14] (older/hist2, tap0)", 777, out[0]);

    return 1;
}

// =================================================================================================
// TEST 3 -- scale/predictor nibble unpacking from a known header byte. A zero codebook isolates the
// nibble unpack and scale shift from any predictor contribution (predicted == 0 always). Header
// 0x30 -> shift=3 (high nibble), predIdx=0 (low nibble); dataByte[0]=0x5A -> nibble0 = 5,
// nibble1 = 0xA sign-extended to -6. residual = nibble << shift.
// =================================================================================================
static int TestAdpcmNibbleUnpack(void) {
    int16_t zeroBook[16];
    uint8_t frame[9];
    uint8_t framePadded[16];
    int16_t out[16];
    Cmd cmds[3];

    memset(zeroBook, 0, sizeof(zeroBook));

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x30;       /* header: shift=3, predIdx=0 */
    frame[1] = 0x5A;       /* nibble0 = 0x5 = 5, nibble1 = 0xA -> sign-extended = -6 */

    memset(framePadded, 0, sizeof(framePadded));
    memcpy(framePadded, frame, 9);

    {
        uint32_t bookAddr = RegisterRegion(zeroBook, sizeof(zeroBook));
        uint32_t stateAddr;
        int16_t state[16];
        memset(state, 0, sizeof(state));
        stateAddr = RegisterRegion(state, sizeof(state));

        DmemLoad(0x0500, framePadded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(zeroBook), bookAddr);
        cmds[1] = MkSetBuff(0x0500, 0x0540, 32 /* 16 samples */);
        cmds[2] = MkAdpcm(1 /* A_INIT */, stateAddr);
        RunCmds(cmds, 3);
        DmemSave(0x0540 + 32, out, 32);
    }

    /* sample0: predicted=0 (zero book), residual = 5<<3 = 40 -> out0 = 40. */
    CheckEq32("sample0 = nibble(5)<<shift(3)", 40, out[0]);
    /* sample1: predicted=0 still (both coefs zero regardless of hist), residual = -6<<3 = -48. */
    CheckEq32("sample1 = nibble(-6)<<shift(3)", -48, out[1]);

    return 1;
}

// =================================================================================================
// TEST 4 -- Resample at unity pitch (0x8000). With the exact ROM table this is NOT a bit-exact
// passthrough: phase 0 is {0x0c39,0x66ad,0x0d46,0xffdf}, a mild band-limiting low-pass, so every
// output is the phase-0 4-tap FIR of the input -- authentic hardware behavior. Asserting that exact
// FIR also pins the four row-0 constants the interpreter uses. At unity the accumulator stays on
// phase 0 and the source cursor advances one whole sample per output, and A_INIT ZERO-PRIMES the
// 4-sample pre-roll rather than duplicating in[0] backward in time (mupen's alist_resample_reset,
// alist.c#L611-619, memsets the window and pitch_accu instead of seeding from the buffer).
// =================================================================================================
static int TestResampleUnityPitch(void) {
    int16_t in[24];
    int16_t out[16];
    int16_t expected[16];
    int16_t state[16];
    const int16_t* L = kResampleLut[0]; /* phase 0 */
    int i;

    for (i = 0; i < 24; i++) in[i] = (int16_t)(1000 + i * 137 - (i % 3) * 59);

    for (i = 0; i < 16; i++) {
        int16_t xm1 = (i == 0) ? 0 : in[i - 1]; /* zero-primed pre-roll, task A5 */
        int32_t acc = (int32_t)xm1 * L[0] + (int32_t)in[i] * L[1] +
                      (int32_t)in[i + 1] * L[2] + (int32_t)in[i + 2] * L[3];
        expected[i] = RefClampS16(acc >> 15);
    }

    /* SETBUFF must precede RESAMPLE in the SAME call -- pendingBuf resets every gdx_audio_hle_run
       invocation, so the descriptor it sets is only visible to ops issued in that same call. */
    memset(state, 0, sizeof(state));
    {
        uint32_t stateAddr = RegisterRegion(state, sizeof(state));
        Cmd cmds[2];
        DmemLoad(0x0600, in, 48 /* 24 samples */);
        cmds[0] = MkSetBuff(0x0600, 0x0680, 32 /* 16 output samples */);
        cmds[1] = MkResample(1 /* A_INIT */, 0x8000u, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0680, out, 32);
    }

    CheckS16Array("unity-pitch phase-0 FIR (exact ROM table)", expected, out, 16, 0);

    return 1;
}

// =================================================================================================
// TEST 4b -- Known ROM table entries, row 0. A single impulse of value V at source index 3 at unity
// pitch lands on exactly one tap per output (phase is always 0, the cursor steps one sample per
// output), so out[1] = (V*L[3])>>15, out[2] = (V*L[2])>>15, out[3] = (V*L[1])>>15 and
// out[4] = (V*L[0])>>15 isolate the four phase-0 coefficients individually. The wrong table --
// e.g. the old windowed-sinc, whose phase-0 row was {0,0x8000,0,0} -- fails every one of these.
// =================================================================================================
static int TestResampleTableRow0(void) {
    int16_t in[24];
    int16_t out[16];
    int16_t state[16];
    const int16_t* L = kResampleLut[0];
    const int32_t V = 30000;

    memset(in, 0, sizeof(in));
    in[3] = (int16_t)V;

    memset(state, 0, sizeof(state));
    {
        uint32_t stateAddr = RegisterRegion(state, sizeof(state));
        Cmd cmds[2];
        DmemLoad(0x0620, in, 48);
        cmds[0] = MkSetBuff(0x0620, 0x06A0, 32 /* 16 output samples */);
        cmds[1] = MkResample(1 /* A_INIT */, 0x8000u, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x06A0, out, 32);
    }

    CheckEq32("row0 tap3 (0xffdf) via out[1]", RefClampS16((V * L[3]) >> 15), out[1]);
    CheckEq32("row0 tap2 (0x0d46) via out[2]", RefClampS16((V * L[2]) >> 15), out[2]);
    CheckEq32("row0 tap1 (0x66ad) via out[3]", RefClampS16((V * L[1]) >> 15), out[3]);
    CheckEq32("row0 tap0 (0x0c39) via out[4]", RefClampS16((V * L[0]) >> 15), out[4]);

    return 1;
}

// =================================================================================================
// TEST 4c -- pitch-change continuity. A looping engine sound is resampled with a DIFFERENT pitch
// every audio tick (the sequencer rewrites freqScale each frame), always with A_CONTINUE -- A_INIT
// fires only on note (re)start (synthesis.c AudioSynth_ProcessNote). A defect carrying the
// fractional accumulator or history across a pitch change clicks at every tick boundary.
//
// Drives the interpreter chunk by chunk on a pitch schedule (state carried, dmemIn shifted forward
// by the whole source samples consumed so far, as the CPU note pacing does) and compares BIT-EXACT
// against an independent continuous single-pass reference that never resets state. Also asserts
// the physical no-click criterion: the jump AT each chunk boundary is no larger than the largest
// jump WITHIN the chunks, i.e. the signal's own slope.
// =================================================================================================
static int TestResamplePitchChangeContinuity(void) {
    enum { NCHUNK = 5, NOUT = 24, TOTAL = NCHUNK * NOUT, NSRC = 200 };
    static const uint32_t pitches[NCHUNK] = { 0x8000u, 0x5000u, 0xC000u, 0x6800u, 0xA000u };
    int16_t in[NSRC];
    int16_t refOut[TOTAL];
    int16_t chunkOut[TOTAL];
    uint32_t chunkStartSrc[NCHUNK];
    int i, c;

    /* Smooth continuous source (low-frequency sine) so the natural per-sample slope is small and
       any state-carry click would stand out as a large boundary jump. */
    for (i = 0; i < NSRC; i++) {
        in[i] = (int16_t)floor(9000.0 * sin(2.0 * 3.14159265358979323846 * (double)i / 43.0) + 0.5);
    }

    /* --- Continuous reference: one uninterrupted pass, pitch changes only at chunk boundaries,
       state (frac/lastSample/source-cursor) NEVER reset. This is the ground truth a correct
       chunked run must reproduce exactly. --- */
    {
        uint32_t frac = 0;
        uint32_t src = 0;
        int16_t last = 0; /* task A5: A_INIT (chunk 0) zero-primes the pre-roll history */
        int idx = 0;
        for (c = 0; c < NCHUNK; c++) {
            uint32_t step = pitches[c] << 1;
            int n;
            chunkStartSrc[c] = src;
            for (n = 0; n < NOUT; n++) {
                int16_t xm1 = last;
                int16_t x0 = in[src];
                int16_t x1 = in[src + 1];
                int16_t x2 = in[src + 2];
                const int16_t* L = kResampleLut[(frac >> 10) & 63];
                int32_t acc = (int32_t)xm1 * L[0] + (int32_t)x0 * L[1] +
                              (int32_t)x1 * L[2] + (int32_t)x2 * L[3];
                refOut[idx++] = RefClampS16(acc >> 15);
                frac += step;
                while (frac >= 0x10000u) {
                    frac -= 0x10000u;
                    last = in[src];
                    src++;
                }
            }
        }
    }

    /* --- Chunked through the real interpreter: one gdx_audio_hle_run per chunk, persistent state
       buffer carried across calls, dmemIn base shifted forward by chunkStartSrc[c] whole samples
       (the CPU note-pacing responsibility -- the interpreter itself always restarts its local
       source index at 0). Only the pitch changes between chunks; flags=A_CONTINUE after chunk 0. --- */
    {
        int16_t state[16];
        uint32_t stateAddr;
        uint32_t srcAddr;
        memset(state, 0, sizeof(state));
        stateAddr = RegisterRegion(state, sizeof(state));
        srcAddr = RegisterRegion(in, sizeof(in));

        for (c = 0; c < NCHUNK; c++) {
            Cmd cmds[3];
            int16_t scratch[NOUT];
            uint32_t dmemBase = 0x0000u + chunkStartSrc[c] * 2u;
            uint32_t dmemOut = 0x0800u;
            /* Reload the whole source into DMEM at offset 0 each chunk (LOADBUFF needs a mult-of-16
               byte size); the base offset selects this chunk's starting source sample. */
            cmds[0] = MkLoadBuff(0x0000, sizeof(in), srcAddr);
            cmds[1] = MkSetBuff(dmemBase, dmemOut, NOUT * 2u);
            cmds[2] = MkResample(c == 0 ? 1u : 0u, pitches[c], stateAddr);
            RunCmds(cmds, 3);
            DmemSave(dmemOut, scratch, (NOUT * 2u + 15u) & ~15u);
            memcpy(chunkOut + c * NOUT, scratch, NOUT * sizeof(int16_t));
        }
    }

    CheckS16Array("pitch-change chunked == continuous reference", refOut, chunkOut, TOTAL, 0);

    /* Physical no-click check: the boundary jump must not exceed the largest interior jump. */
    {
        int maxInterior = 0;
        int maxBoundary = 0;
        for (i = 1; i < TOTAL; i++) {
            int d = (int)chunkOut[i] - (int)chunkOut[i - 1];
            if (d < 0) d = -d;
            if (i % NOUT == 0) { if (d > maxBoundary) maxBoundary = d; }
            else               { if (d > maxInterior) maxInterior = d; }
        }
        gSubChecks++;
        if (maxBoundary > maxInterior) {
            gSubFails++;
            printf("       MISMATCH: boundary jump (%d) exceeds max interior slope (%d) -- CLICK\n",
                   maxBoundary, maxInterior);
        }
    }

    return 1;
}

// =================================================================================================
// TEST 4d -- ADPCM loop-wrap lookahead + resample continuity. A short looping sample wraps back to
// its loop point many times a second. At each wrap synthesis.c emits, WITHIN one audio tick: decode
// the pre-loop-end frames (A_CONTINUE), then aSetLoop and decode the loop-START frames (A_LOOP)
// CONTIGUOUSLY after them, then ONE FinalResample over the whole buffer. The resampler needs 2
// samples of lookahead (taps x[+1], x[+2]), so the question is whether the taps straddling the wrap
// read the real post-wrap PCM rather than stale or zero data -- if they read zero, every wrap clicks.
//
// The codebook is an INTEGRATOR (predicted == hist1, book col0 = {0, Q11-unity}), so
// decoded[n] = decoded[n-1] + residual[n] and loop-wrap continuity depends entirely on A_LOOP
// restoring the right history: a wrong restored hist1 offsets the whole post-wrap segment by a DC
// step, i.e. a click. The residuals are a full-period sine, so the integrated waveform is smooth AND
// perfectly periodic (loopStart is the natural continuation of loopEnd) -- a genuinely seamless
// loop. It then:
//   (a) decodes pre-wrap (A_INIT) plus the post-wrap loop-start frame (A_LOOP) into CONTIGUOUS DMEM
//       and asserts all 48 samples are bit-exact against an independent integrator+restore
//       reference, proving the post-wrap samples are present and contiguous; and
//   (b) resamples a window STRADDLING the junction at non-unity pitch, asserting it is bit-exact
//       against a continuous single-pass reference over the same concatenated PCM (proving the
//       lookahead reads real post-wrap samples) plus a physical no-click check.
// =================================================================================================
static int TestAdpcmLoopWrapResampleContinuity(void) {
    enum { LBODY = 32, LWRAP = 16, NCAT = LBODY + LWRAP };
    int r[LBODY];
    int16_t cat[NCAT];       /* independent reference: concatenated decoded PCM across the wrap */
    int16_t book[16];
    int16_t loopState[16];
    int16_t noteState[16];
    int8_t nb0[16], nb1[16];
    uint8_t frame0[9], frame1[9];
    uint8_t srcA[32], srcB[16];
    int16_t hleDecoded[NCAT];
    Cmd cmds[4];
    int i, h1, h2, endH1, endH2;

    /* Integrator codebook: BookCoef(0,0,0)=book[0] weights hist2 (=0), BookCoef(0,1,0)=book[8]
       weights hist1 (=2048 == Q11 unity, 2048>>11 == *1). So predicted == hist1. */
    memset(book, 0, sizeof(book));
    book[0] = 0;
    book[8] = 2048;

    /* Full-period sine residuals (period 16, two periods over the 32-sample body): each period sums
       to exactly 0, so the integrator returns to its start -> seamless periodic loop. Amplitude 3
       stays well inside the 4-bit nibble range [-8,7]. */
    for (i = 0; i < LBODY; i++) {
        double s = 3.0 * sin(2.0 * 3.14159265358979323846 * (double)i / 16.0);
        r[i] = (int)floor(s + (s >= 0 ? 0.5 : -0.5)); /* round to nearest, ties away from 0 */
    }

    /* --- Independent reference decode: pass 1 (A_INIT, hist 0/0) then the loop-start frame from the
       restored ending history (exactly what A_LOOP does: hist1<-loopState[15] newest,
       hist2<-loopState[14] older -- reference tail convention). */
    h1 = 0; h2 = 0;
    for (i = 0; i < LBODY; i++) {
        int pred = h1;                 /* integrator: predicted == hist1 */
        int s = pred + r[i];           /* shift=0 -> residual == nibble */
        h2 = h1;
        h1 = RefClampS16(s);
        cat[i] = (int16_t)h1;
    }
    endH1 = h1;  /* == cat[LBODY-1] */
    endH2 = h2;  /* == cat[LBODY-2] */
    for (i = 0; i < LWRAP; i++) {
        int pred = h1;
        int s = pred + r[i];           /* loop-start frame replays the body's first 16 residuals */
        h2 = h1;
        h1 = RefClampS16(s);
        cat[LBODY + i] = (int16_t)h1;
    }

    /* loopState carries the real-hardware convention: predictor history at the loop-restart point in
       the TAIL slots -- [15] = newest (hist1), [14] = older (hist2). */
    memset(loopState, 0, sizeof(loopState));
    loopState[15] = (int16_t)endH1;
    loopState[14] = (int16_t)endH2;

    /* Encode the body as two 4-bit ADPCM frames; the loop-start frame is a byte-identical replay of
       frame0 (loopStart == 0). shift=0, predIdx=0 -> header 0x00. */
    for (i = 0; i < 16; i++) nb0[i] = (int8_t)r[i];
    for (i = 0; i < 16; i++) nb1[i] = (int8_t)r[16 + i];
    PackAdpcmFrame(frame0, 0x00, nb0);
    PackAdpcmFrame(frame1, 0x00, nb1);

    memset(srcA, 0, sizeof(srcA));
    memcpy(srcA + 0, frame0, 9);
    memcpy(srcA + 9, frame1, 9);
    memset(srcB, 0, sizeof(srcB));
    memcpy(srcB + 0, frame0, 9); /* loop-start frame */

    memset(noteState, 0, sizeof(noteState));

    {
        uint32_t bookAddr = RegisterRegion(book, sizeof(book));
        uint32_t loopAddr = RegisterRegion(loopState, sizeof(loopState));
        uint32_t stateAddr = RegisterRegion(noteState, sizeof(noteState));

        /* Compressed inputs live at DMEM 0x0100 / 0x0140. Under the last-frame preamble
           contract each ADPCM call writes [32-byte preamble][fresh samples], so the two
           chunks decode into SEPARATE regions (chunk B's preamble would otherwise
           overwrite chunk A's tail with loopState's zero padding). The concatenated PCM
           is assembled on the host, then reloaded contiguously for the resample stage. */
        DmemLoad(0x0100, srcA, 32);
        DmemLoad(0x0140, srcB, 16);

        /* Chunk A: pre-wrap body, A_INIT. Fresh samples land at 0x0000+32. */
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetBuff(0x0100, 0x0000, LBODY * 2u);
        cmds[2] = MkAdpcm(1 /* A_INIT */, stateAddr);
        RunCmds(cmds, 3);
        DmemSave(0x0000 + 32, hleDecoded, LBODY * 2u);

        /* Chunk B: loop-start frame, A_LOOP -- separate region, fresh at 0x0800+32. */
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetLoop(loopAddr);
        cmds[2] = MkSetBuff(0x0140, 0x0800, LWRAP * 2u);
        cmds[3] = MkAdpcm(2 /* A_LOOP */, stateAddr);
        RunCmds(cmds, 4);
        DmemSave(0x0800 + 32, hleDecoded + LBODY, LWRAP * 2u);

        /* Reload the host-assembled concatenated PCM contiguously at 0x0000 for the
           resample stage below (which indexes it as one continuous source stream). */
        DmemLoad(0x0000, hleDecoded, NCAT * 2u);
    }

    /* (a) Post-wrap samples are present and contiguous, and A_LOOP restored the right history. */
    CheckS16Array("loop-wrap decode contiguity (A_INIT body + A_LOOP wrap)", cat, hleDecoded, NCAT, 0);

    /* Physical seamlessness sanity: the wrap junction step is no larger than the body's own slope. */
    {
        int maxBodySlope = 0;
        int junctionStep = (int)cat[LBODY] - (int)cat[LBODY - 1];
        if (junctionStep < 0) junctionStep = -junctionStep;
        for (i = 1; i < LBODY; i++) {
            int d = (int)cat[i] - (int)cat[i - 1];
            if (d < 0) d = -d;
            if (d > maxBodySlope) maxBodySlope = d;
        }
        gSubChecks++;
        if (junctionStep > maxBodySlope) {
            gSubFails++;
            printf("       MISMATCH: decoded wrap junction step (%d) exceeds max body slope (%d)\n",
                   junctionStep, maxBodySlope);
        }
    }

    /* (b) Resample a window that STRADDLES the wrap junction (source sample 32). Start at source 24,
       pitch 0x9000 (1.125x): 16 outputs consume ~18 source samples -> reach ~42, lookahead to ~44,
       all inside the 48 decoded samples, and the window crosses the junction at 32. */
    {
        enum { RSTART = 24, ROUT = 16 };
        const uint32_t pitch = 0x9000u;
        int16_t resState[16];
        int16_t hleRes[ROUT];
        int16_t refRes[ROUT];
        uint32_t curAt[ROUT]; /* source cursor at the start of each output (to locate the junction) */
        uint32_t frac = 0, cur = 0;
        int16_t last = 0; /* task A5: A_INIT zero-primes the pre-roll history, not cat[RSTART] */
        int n;

        /* Continuous single-pass reference over the concatenated PCM (mirrors RunResample exactly). */
        for (n = 0; n < ROUT; n++) {
            int16_t xm1 = last;
            int16_t x0 = cat[RSTART + cur];
            int16_t x1 = cat[RSTART + cur + 1];
            int16_t x2 = cat[RSTART + cur + 2];
            const int16_t* L = kResampleLut[(frac >> 10) & 63];
            int32_t acc = (int32_t)xm1 * L[0] + (int32_t)x0 * L[1] +
                          (int32_t)x1 * L[2] + (int32_t)x2 * L[3];
            curAt[n] = cur;
            refRes[n] = RefClampS16(acc >> 15);
            frac += (pitch << 1);
            while (frac >= 0x10000u) {
                frac -= 0x10000u;
                last = cat[RSTART + cur];
                cur++;
            }
        }

        memset(resState, 0, sizeof(resState));
        {
            uint32_t stateAddr = RegisterRegion(resState, sizeof(resState));
            Cmd rc[2];
            /* Decoded PCM still resident in DMEM at 0x0000; resample from source sample RSTART. */
            rc[0] = MkSetBuff(0x0000 + RSTART * 2u, 0x0200, ROUT * 2u);
            rc[1] = MkResample(1 /* A_INIT */, pitch, stateAddr);
            RunCmds(rc, 2);
            DmemSave(0x0200, hleRes, (ROUT * 2u + 15u) & ~15u);
        }

        CheckS16Array("loop-wrap resample straddling junction == continuous reference",
                      refRes, hleRes, ROUT, 0);

        /* Physical no-click on the resampled output, independent of the reference model and of any
           absolute magnitude: the largest adjacent jump among the outputs whose 4-tap window
           STRADDLES the wrap junction (source sample LBODY) must not exceed the largest jump among
           the outputs that lie entirely on ONE side of it. A stale/zero lookahead at the wrap would
           make a junction-straddling output an outlier spike; a seamless loop keeps the two equal.
           Straddle = the tap window [cur-1 .. cur+2] (in source-relative coords, +RSTART) includes
           LBODY, i.e. RSTART+cur is within [LBODY-2, LBODY+1]. */
        {
            int maxJunctionJump = 0;
            int maxBaselineJump = 0;
            for (n = 1; n < ROUT; n++) {
                int d = (int)hleRes[n] - (int)hleRes[n - 1];
                int srcA = (int)(RSTART + curAt[n - 1]);
                int srcB = (int)(RSTART + curAt[n]);
                int straddles = ((srcA >= LBODY - 2 && srcA <= LBODY + 1) ||
                                 (srcB >= LBODY - 2 && srcB <= LBODY + 1));
                if (d < 0) d = -d;
                if (straddles) { if (d > maxJunctionJump) maxJunctionJump = d; }
                else           { if (d > maxBaselineJump) maxBaselineJump = d; }
            }
            gSubChecks++;
            if (maxJunctionJump > maxBaselineJump + 1) {
                gSubFails++;
                printf("       MISMATCH: junction jump (%d) exceeds baseline slope (%d) -- wrap CLICK\n",
                       maxJunctionJump, maxBaselineJump);
            }
        }
    }

    return 1;
}

// =================================================================================================
// TEST 5 -- half and near-double pitch stepping. fracQ16 accumulates `pitch<<1` per output and
// wraps at 0x10000, advancing the source cursor by (N*(pitch<<1))>>16 whole samples with remainder
// (N*(pitch<<1))&0xFFFF -- a closed-form invariant of the accumulator, independent of the FIR taps.
// Checks state[4] (fracQ16) and state[3] (the most recent persisted history sample, i.e. tap -1 for
// the next call, which must equal input[delta-1]).
//
// `nOut` is the NOMINAL requested count; RunResample rounds the processed count up to a whole
// 8-sample (16-byte) granule (mupen alist.c#L621-639's `(count+0xf)&~0xf`), so the invariant is
// computed against that rounded count.
// =================================================================================================
static int RunResamplePitchCase(const char* label, uint32_t pitch, uint32_t nOut,
                                 const int16_t* in, uint32_t inCount) {
    int16_t state[16];
    int16_t out[64];
    char desc[128];
    uint32_t actualCount = ((nOut * 2u + 0xFu) & ~0xFu) / 2u; /* mirrors RunResample's rounding */
    uint32_t step = pitch << 1;
    uint64_t totalFrac = (uint64_t)actualCount * (uint64_t)step;
    uint32_t expectedDelta = (uint32_t)(totalFrac >> 16);
    uint32_t expectedFrac = (uint32_t)(totalFrac & 0xFFFFu);
    uint32_t expectedLastSampleIdx = expectedDelta - 1u;

    memset(state, 0, sizeof(state));
    {
        uint32_t stateAddr = RegisterRegion(state, sizeof(state));
        Cmd cmds[2];
        DmemLoad(0x0700, (const void*)in, (inCount * 2 + 15u) & ~15u);
        cmds[0] = MkSetBuff(0x0700, 0x0780, nOut * 2u);
        cmds[1] = MkResample(1 /* A_INIT */, pitch, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0780, out, (actualCount * 2u + 15u) & ~15u);
    }
    snprintf(desc, sizeof(desc), "%s: final fracQ16 (state[4], rounded count=%u)", label, actualCount);
    CheckEq32(desc, (int32_t)(uint16_t)expectedFrac, (int32_t)(uint16_t)state[4]);
    snprintf(desc, sizeof(desc), "%s: lastSample (state[3]) == input[%u]", label, expectedLastSampleIdx);
    CheckEq32(desc, in[expectedLastSampleIdx], state[3]);
    return 1;
}

static int TestResamplePitchStepping(void) {
    /* Sized generously (40 samples) so even the near-double-pitch case's rounded-up 8-output scan
       (which can advance the source cursor by up to ~16 samples) stays inside the loaded DMEM
       region instead of reading stale/unrelated bytes from the shared static DMEM scratch. */
    int16_t in[40];
    int i;
    for (i = 0; i < 40; i++) in[i] = (int16_t)(i * 111);

    /* Half pitch (0x4000), requested N=4 -> rounds up to actualCount=8:
       delta = (8*0x8000)>>16 = 4, frac = 0. */
    RunResamplePitchCase("half-pitch(0x4000) N=4(rounds to 8)", 0x4000u, 4, in, 40);

    /* Pitch is packed into a 16-bit Acmd field, so the conceptual 2.0x value 0x10000 does not fit
       and silently truncates to 0 -- the ABI's real ceiling, not an interpreter defect. The
       maximum representable pitch is 0xFFFF (~1.99997x). Requested N=4 rounds up to 8:
       delta = (8*(0xFFFF<<1))>>16 = (8*0x1FFFE)>>16 = 0xFFFF0>>16 = 15, frac = 0xFFFF0 & 0xFFFF
       = 0xFFF0. */
    RunResamplePitchCase("near-double-pitch(0xFFFF) N=4(rounds to 8)", 0xFFFFu, 4, in, 40);

    return 1;
}

// =================================================================================================
// TEST 6 -- resampler state continuity: one continuous call producing N samples must be
// bit-identical to two chunked calls, with the state buffer carried across and the second call's
// dmemIn shifted forward by the whole-sample delta the first consumed. Both chunk sizes here are
// already whole 8-sample granules, so count rounding is a no-op -- this isolates STATE continuity;
// rounding has its own case below.
// =================================================================================================
static int TestResampleContinuity(void) {
    int16_t in[48];
    int16_t combinedOut[16];
    int16_t chunkOut[16];
    const uint32_t pitch = 0x9000u; /* arbitrary non-unity pitch */
    int i;

    for (i = 0; i < 48; i++) in[i] = (int16_t)(2000 + i * 91 - (i % 5) * 37);

    /* --- Combined: one call, N=16. --- */
    {
        int16_t state[16];
        uint32_t stateAddr;
        Cmd cmds[2];
        memset(state, 0, sizeof(state));
        stateAddr = RegisterRegion(state, sizeof(state));
        DmemLoad(0x0800, in, 96 /* 48 samples */);
        cmds[0] = MkSetBuff(0x0800, 0x0880, 32 /* 16 output samples */);
        cmds[1] = MkResample(1 /* A_INIT */, pitch, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0880, combinedOut, 32);
    }

    /* --- Chunked: call1 (8 out, A_INIT) + call2 (8 out, continue, dmemIn shifted by delta1). ---
       delta1 = (8*(pitch<<1))>>16 = (8*0x12000)>>16 = 0x90000>>16 = 9. */
    {
        int16_t state[16];
        uint32_t stateAddr;
        Cmd cmds[2];
        const uint32_t delta1 = (uint32_t)(((uint64_t)8 * ((uint64_t)pitch << 1)) >> 16);

        memset(state, 0, sizeof(state));
        stateAddr = RegisterRegion(state, sizeof(state));

        DmemLoad(0x0900, in, 96);
        cmds[0] = MkSetBuff(0x0900, 0x0980, 16 /* 8 output samples */);
        cmds[1] = MkResample(1 /* A_INIT */, pitch, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0980, chunkOut, 16);

        /* Same DMEM input buffer already resident at 0x0900 (untouched since the LOADBUFF above);
           only the SETBUFF dmemIn base shifts forward by delta1 samples for call2. */
        cmds[0] = MkSetBuff(0x0900 + delta1 * 2u, 0x09A0, 16 /* 8 output samples */);
        cmds[1] = MkResample(0 /* continue */, pitch, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x09A0, chunkOut + 8, 16);
    }

    CheckS16Array("resample continuity (combined vs chunked)", combinedOut, chunkOut, 16, 0);

    return 1;
}

// =================================================================================================
// TEST 7 -- A_FILTER cross-block-windowed impulse response within ONE call. RunFilter applies the 8
// primed Q15 coefficients DIRECTLY as a fixed 8-tap FIR, with no per-call LUT averaging (see
// RunFilter's header for why averaging halves the identity row). The FIR slides across a 16-sample
// window = [previous block's 8-sample tail ++ current block]. One call covers TWO blocks, checked
// against an INDEPENDENT reimplementation of that shape rather than a second call into the
// interpreter; the impulse at the very start of block 0 straddles into block 1 via the tail carry.
// =================================================================================================
static int TestFilterImpulseResponse(void) {
    int16_t coef[8] = { 4000, -2000, 2000, 1000, -1000, 500, -500, 200 };
    int16_t in[16];
    int16_t out[16];
    int16_t expected[16];
    const int32_t V = 10000;
    int t, k, blk;

    memset(in, 0, sizeof(in));
    in[0] = (int16_t)V; /* impulse at sample 0 of block 0; block 1 (samples 8..15) is all zero */

    /* Independent reference: the primed coef IS the FIR (no averaging); both 8-sample blocks are
       filtered through it with the previous block's INPUT tail carried forward. */
    {
        int16_t tail[8];
        memset(tail, 0, sizeof(tail));
        for (blk = 0; blk < 2; blk++) {
            int16_t cur[8];
            int16_t win[16];
            for (t = 0; t < 8; t++) cur[t] = in[blk * 8 + t];
            for (t = 0; t < 8; t++) { win[t] = tail[t]; win[8 + t] = cur[t]; }
            for (k = 0; k < 8; k++) {
                int32_t acc = 0;
                for (t = 0; t < 8; t++) acc += (int32_t)coef[t] * (int32_t)win[k + t];
                expected[blk * 8 + k] = RefClampS16((acc + 0x4000) >> 15);
            }
            for (t = 0; t < 8; t++) tail[t] = cur[t];
        }
    }

    {
        int16_t state[16]; /* state[0..7] = INPUT tail carried across blocks, per RunFilter's layout */
        uint32_t coefAddr, stateAddr;
        Cmd cmds[2];
        memset(state, 0, sizeof(state));
        coefAddr = RegisterRegion(coef, sizeof(coef));
        stateAddr = RegisterRegion(state, sizeof(state));

        DmemLoad(0x0A00, in, 32 /* 16 samples = 2 blocks */);
        cmds[0] = MkFilterPrime(32 /* byte size of the buffer the apply call will filter */, coefAddr);
        cmds[1] = MkFilterApply(1 /* A_INIT */, 0x0A00, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0A00, out, 32);
    }

    CheckS16Array("filter impulse response (direct 8-tap FIR, cross-block window)", expected, out, 16, 0);

    return 1;
}

// =================================================================================================
// TEST 8 -- A_FILTER cross-CALL continuity: the INPUT tail persists across SEPARATE
// gdx_audio_hle_run calls, each with its own prime+apply pair -- exactly how
// AudioSynth_FilterReverb drives this opcode, a fresh pair every audio tick. Coefficients are
// applied directly, so re-priming the same table yields the same FIR and the ONLY difference
// between the two calls is the carried tail: zero for call 1 (A_INIT), call 1's real input block
// for call 2.
// =================================================================================================
static int TestFilterContinuity(void) {
    int16_t coef[8] = { 4000, -2000, 2000, 1000, -1000, 500, -500, 200 };
    const int32_t V = 10000, W = 7000;
    int16_t blockA[8], blockB[8];
    int16_t expectedA[8], expectedB[8];
    int16_t hleOutA[8], hleOutB[8];
    int t, k;

    memset(blockA, 0, sizeof(blockA));
    blockA[0] = (int16_t)V;
    memset(blockB, 0, sizeof(blockB));
    blockB[1] = (int16_t)W;

    /* Independent reference model: both calls use the SAME primed coef directly as the FIR. call1's
       tail is zero (A_INIT); call2's tail is call1's real input block (blockA) -- the cross-call
       carry -- so the ONLY difference between the two windows is the tail, not the coefficients. */
    {
        int16_t win[16];
        for (t = 0; t < 8; t++) { win[t] = 0; win[8 + t] = blockA[t]; } /* A_INIT zeroes the tail */
        for (k = 0; k < 8; k++) {
            int32_t acc = 0;
            for (t = 0; t < 8; t++) acc += (int32_t)coef[t] * (int32_t)win[k + t];
            expectedA[k] = RefClampS16((acc + 0x4000) >> 15);
        }
        for (t = 0; t < 8; t++) { win[t] = blockA[t]; win[8 + t] = blockB[t]; }
        for (k = 0; k < 8; k++) {
            int32_t acc = 0;
            for (t = 0; t < 8; t++) acc += (int32_t)coef[t] * (int32_t)win[k + t];
            expectedB[k] = RefClampS16((acc + 0x4000) >> 15);
        }
    }

    {
        int16_t state[16];
        uint32_t coefAddr, stateAddr;
        Cmd cmds[2];
        memset(state, 0, sizeof(state));
        coefAddr = RegisterRegion(coef, sizeof(coef));
        stateAddr = RegisterRegion(state, sizeof(state));

        DmemLoad(0x0B00, blockA, 16);
        cmds[0] = MkFilterPrime(16, coefAddr);
        cmds[1] = MkFilterApply(1 /* A_INIT */, 0x0B00, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0B00, hleOutA, 16);

        /* Same coefficient table re-primed (mirrors a real repeated prime+apply pair); DIFFERENT
           call, same persistent state buffer -- the tail crosses from call1 into call2's window. */
        DmemLoad(0x0B20, blockB, 16);
        cmds[0] = MkFilterPrime(16, coefAddr);
        cmds[1] = MkFilterApply(0 /* continue */, 0x0B20, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0B20, hleOutB, 16);
    }

    CheckS16Array("filter cross-call continuity, call 1 (tail zeroed by A_INIT)", expectedA, hleOutA, 8, 0);
    CheckS16Array("filter cross-call continuity, call 2 (tail carried from call 1)", expectedB, hleOutB, 8, 0);

    return 1;
}

// =================================================================================================
// TEST 8b -- protocol-level A_FILTER: the full two-step prime+apply driven through
// gdx_audio_hle_run with REAL host-order coefficient rows lifted from
// decomp/src/audio/disk/lib/filter_data.c's gLowPassFilterData (the exact table
// AudioHeap_LoadFilter copies into reverb->filterLeft/Right, host-order, no swap), exactly as
// AudioSynth_FilterReverb emits the pair. Three externally observable properties, none referencing
// the interpreter's internal math:
//   (1) IDENTITY passthrough. Row 0 = {0,0,0,32767,0,0,0,0} is the "cutoff 0 == no filtering" row
//       AudioHeap_LoadFilter loads; a loud DC buffer must come out at UNITY once the window fills.
//       Two-LUT averaging halves that row to -6 dB, so the op could only ever attenuate the wet
//       reverb, never faithfully pass it.
//   (2) LOW-PASS and NON-ZERO. A real cutoff row (row 5) passes DC near its coefficient-sum gain
//       but crushes a Nyquist (alternating +/-A) buffer far below that.
//   (3) STATE CARRY across two apply calls. A second apply (A_CONTINUE) on a SILENT block still
//       produces large output bled from the previous call's loud tail; a broken carry zeroes it.
// =================================================================================================
static int TestFilterLowpassProtocol(void) {
    /* Host-order rows, transcribed from gLowPassFilterData (filter_data.c): row 0 (identity) and
       row 5 (a mid low-pass). Same byte order the game hands this op -- no swap anywhere. */
    int16_t coefId[8] = { 0, 0, 0, 32767, 0, 0, 0, 0 };
    int16_t coefLp[8] = { -265, 3421, 7292, 8944, 7292, 3421, -265, -1863 };
    int t;
    int32_t dcGain = 0;
    for (t = 0; t < 8; t++) dcGain += coefLp[t]; /* == 27977 (Q15 ~= 0.854) */

    /* (1) IDENTITY unity passthrough: 2 blocks of loud DC. Once the window fills, block 1 == input. */
    {
        const int16_t V = 12000;
        int16_t in[16], out[16], state[16];
        uint32_t coefAddr, stateAddr;
        Cmd cmds[2];
        int i;
        for (i = 0; i < 16; i++) in[i] = V;
        memset(state, 0, sizeof(state));
        coefAddr = RegisterRegion(coefId, sizeof(coefId));
        stateAddr = RegisterRegion(state, sizeof(state));
        DmemLoad(0x0C00, in, 32);
        cmds[0] = MkFilterPrime(32, coefAddr);
        cmds[1] = MkFilterApply(1 /* A_INIT */, 0x0C00, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0C00, out, 32);
        /* The old two-LUT averaging produced V/2 (6000) here -- this is the decisive guard. */
        CheckEq32("identity row passes loud DC at unity (steady sample)", V, out[15]);
    }

    /* (2) LOW-PASS DC gain (non-zero, at the coef-sum gain) + Nyquist rejection, real row-5. */
    {
        const int16_t V = 10000;
        int16_t dcIn[32], dcOut[32], nyIn[32], nyOut[32], stateDc[16], stateNy[16];
        uint32_t coefAddr, stateAddr;
        Cmd cmds[2];
        int i;
        int32_t dcSteady = RefClampS16((dcGain * (int32_t)V + 0x4000) >> 15); /* ~8538, non-zero */

        for (i = 0; i < 32; i++) dcIn[i] = V;
        memset(stateDc, 0, sizeof(stateDc));
        coefAddr = RegisterRegion(coefLp, sizeof(coefLp));
        stateAddr = RegisterRegion(stateDc, sizeof(stateDc));
        DmemLoad(0x0C80, dcIn, 64);
        cmds[0] = MkFilterPrime(64, coefAddr);
        cmds[1] = MkFilterApply(1 /* A_INIT */, 0x0C80, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0C80, dcOut, 64);
        CheckEq32("low-pass row-5 DC gain (steady sample, non-zero)", dcSteady, dcOut[31]);

        for (i = 0; i < 32; i++) nyIn[i] = (i & 1) ? (int16_t)-V : (int16_t)V; /* Nyquist alternating */
        memset(stateNy, 0, sizeof(stateNy));
        coefAddr = RegisterRegion(coefLp, sizeof(coefLp));
        stateAddr = RegisterRegion(stateNy, sizeof(stateNy));
        DmemLoad(0x0D00, nyIn, 64);
        cmds[0] = MkFilterPrime(64, coefAddr);
        cmds[1] = MkFilterApply(1 /* A_INIT */, 0x0D00, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0D00, nyOut, 64);
        {
            int ny = nyOut[31]; if (ny < 0) ny = -ny;
            gSubChecks++;
            if (!(ny * 8 < dcSteady)) { /* a genuine low-pass crushes Nyquist to << DC gain */
                gSubFails++;
                printf("       MISMATCH: row-5 not low-passing -- |Nyquist steady|=%d not << DC gain=%d\n",
                       ny, (int)dcSteady);
            }
        }
    }

    /* (3) STATE CARRY across two SEPARATE apply calls: loud tail bleeds into a silent block. */
    {
        const int16_t V = 10000;
        int16_t blockA[8], blockB[8], outB[8], state[16];
        uint32_t coefAddr, stateAddr;
        Cmd cmds[2];
        int i;
        for (i = 0; i < 8; i++) { blockA[i] = V; blockB[i] = 0; }
        memset(state, 0, sizeof(state));
        coefAddr = RegisterRegion(coefLp, sizeof(coefLp));
        stateAddr = RegisterRegion(state, sizeof(state));

        DmemLoad(0x0D80, blockA, 16);
        cmds[0] = MkFilterPrime(16, coefAddr);
        cmds[1] = MkFilterApply(1 /* A_INIT */, 0x0D80, stateAddr);
        RunCmds(cmds, 2);

        DmemLoad(0x0DA0, blockB, 16);
        cmds[0] = MkFilterPrime(16, coefAddr);
        cmds[1] = MkFilterApply(0 /* continue */, 0x0DA0, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0DA0, outB, 16);
        /* With the tail carried, outB[0] = sum(coef)*V>>15 (~8538, the whole window is the loud
           tail); a broken carry (tail=0) leaves the silent block silent (outB all 0). */
        {
            int v = outB[0]; if (v < 0) v = -v;
            gSubChecks++;
            if (!(v > 1000)) {
                gSubFails++;
                printf("       MISMATCH: filter state did NOT carry across apply calls -- silent-block outB[0]=%d\n",
                       outB[0]);
            }
        }
    }

    return 1;
}

// =================================================================================================
// TEST 9 -- ENVMIXER with a constant envelope, nead cascade semantics. With ramps at 0,
// dl = (s*curVolLeft)>>16 and dr = (s*curVolRight)>>16 (true Q16 gain), and wet CASCADES the
// dry-scaled sample PER CHANNEL: wetL = (dl*envReverbVol2)>>16, wetR = (dr*envReverbVol2)>>16,
// with envReverbVol2 = a<<8 from ENVSETUP1's 8-bit reverb-volume field. Per mupen's envmix_nead
// (alist.c#L512-562) and the ENVSETUP1 comment deriving why a<<8 shares the same Q16 >>16 scale as
// the dry volumes -- not a single mono wet value taken off the raw input.
// =================================================================================================
static int TestEnvMixerConstantGain(void) {
    int16_t src[8];
    int16_t dryL[8], dryR[8], wetL[8], wetR[8];
    int16_t expDryL[8], expDryR[8], expWetL[8], expWetR[8];
    const uint32_t curVolLeft = 0x8000u;  /* Q16 0.5x */
    const uint32_t curVolRight = 0x4000u; /* Q16 0.25x */
    const uint32_t reverbVol2 = 128u;     /* ENVSETUP1's "a" field (0..254); becomes a<<8 in the
                                              interpreter, i.e. 0x8000 == 0.5x in the same Q16
                                              space as curVolLeft/curVolRight */
    const int32_t envReverbVol2Q16 = (int32_t)reverbVol2 << 8;
    int i;

    for (i = 0; i < 8; i++) {
        src[i] = (int16_t)(i * 1000); /* divisible by 2 and 4 -> exact expected values below */
        expDryL[i] = (int16_t)((src[i] * (int32_t)curVolLeft) >> 16);
        expDryR[i] = (int16_t)((src[i] * (int32_t)curVolRight) >> 16);
        expWetL[i] = (int16_t)(((int32_t)expDryL[i] * envReverbVol2Q16) >> 16); /* cascades dl */
        expWetR[i] = (int16_t)(((int32_t)expDryR[i] * envReverbVol2Q16) >> 16); /* cascades dr */
    }

    {
        Cmd cmds[5];
        /* Clear the dmem regions ENVMIXER accumulates into (it does out += in, not out = in). */
        cmds[0] = MkClearBuff(0x0D40, 16);
        RunCmds(cmds, 1);
        cmds[0] = MkClearBuff(0x0D80, 16);
        RunCmds(cmds, 1);
        cmds[0] = MkClearBuff(0x0DC0, 16);
        RunCmds(cmds, 1);
        cmds[0] = MkClearBuff(0x0E00, 16);
        RunCmds(cmds, 1);

        DmemLoad(0x0D00, src, 16);

        /* a=reverbVol2 (Q8 wet base), b=envRampReverb=0, c=envRampLeft=0, d=envRampRight=0. */
        cmds[0] = MkEnvSetup1(reverbVol2, 0, 0, 0);
        cmds[1] = MkEnvSetup2(curVolLeft, curVolRight);
        cmds[2] = MkEnvMixer(0x0D00 /* src */, 8 /* sampleCount */, 0 /* swapLR */,
                              0x0D40 /* dryLeft */, 0x0D80 /* dryRight */,
                              0x0DC0 /* wetLeft */, 0x0E00 /* wetRight */);
        RunCmds(cmds, 3);

        DmemSave(0x0D40, dryL, 16);
        DmemSave(0x0D80, dryR, 16);
        DmemSave(0x0DC0, wetL, 16);
        DmemSave(0x0E00, wetR, 16);
    }

    CheckS16Array("envmixer dry-left (Q16 0.5x)", expDryL, dryL, 8, 0);
    CheckS16Array("envmixer dry-right (Q16 0.25x)", expDryR, dryR, 8, 0);
    CheckS16Array("envmixer wet-left (cascades dry-left)", expWetL, wetL, 8, 0);
    CheckS16Array("envmixer wet-right (cascades dry-right)", expWetR, wetR, 8, 0);

    return 1;
}

// =================================================================================================
// TEST 10 -- A_MIXER saturation. Real aMix SATURATES on hardware: the RSP stores through a
// saturating vector op and DMEM only ever holds s16. Mixing two near-full-scale buffers at
// near-unity gain overflows that range, and a missing clamp would silently WRAP a huge positive sum
// into a small or negative value -- the "harsh static on loud instruments" symptom. Pins the
// console behavior (+32767), not the wrapped value, against the real A_MIXER path.
// =================================================================================================
static int TestMixerSaturation(void) {
    int16_t inBuf[8], outBuf[8], result[8], expected[8];
    const int32_t V = 30000;      /* near-full-scale on BOTH operands */
    const int32_t gain = 0x7FFF;  /* near-unity signed Q15 */
    int i;

    for (i = 0; i < 8; i++) { inBuf[i] = (int16_t)V; outBuf[i] = (int16_t)V; }
    for (i = 0; i < 8; i++) {
        /* Raw (unclamped) math: 30000 + (30000*32767>>15) = 30000 + 29999 = 59999, which does not
           fit in s16 (max 32767) -- console hardware saturates this; a wraparound bug would store
           (int16_t)59999 == -5537 instead. */
        int32_t raw = (int32_t)outBuf[i] + (((int32_t)inBuf[i] * gain) >> 15);
        expected[i] = RefClampS16(raw);
    }

    DmemLoad(0x0E40, inBuf, 16);
    DmemLoad(0x0E80, outBuf, 16);
    {
        Cmd cmd = MkMixer(1 /* count8 -> 8 samples */, gain, 0x0E40, 0x0E80);
        RunCmds(&cmd, 1);
    }
    DmemSave(0x0E80, result, 16);

    CheckS16Array("A_MIXER: near-full-scale accumulate SATURATES (console behavior), does not wrap",
                  expected, result, 8, 0);
    for (i = 0; i < 8; i++) {
        gSubChecks++;
        if (result[i] != 32767) {
            gSubFails++;
            printf("       MISMATCH: A_MIXER[%d] expected saturated +32767, got %d\n", i, result[i]);
        }
    }

    return 1;
}

// =================================================================================================
// TEST 11 -- ADPCM order-2 predictor INDEX selection. Every other ADPCM test uses predIdx==0. A
// codebook with 2+ predictors picks coefficients via the frame header's low nibble (BookCoef's
// `predictorIndex*16` offset), and an indexing bug there silently decodes with the WRONG
// predictor's coefficients for any bank whose encoder used a nonzero index -- heard as
// instrument-specific static or mistuning. A_CONTINUE seeds hist1/hist2 straight from the state
// buffer, bypassing A_INIT's reset, so one zero-residual first sample isolates the index selection.
// =================================================================================================
static int TestAdpcmPredictorIndexSelection(void) {
    int16_t book[32]; /* 2 predictors x 16 shorts each (8*order(2) per predictor) */
    int16_t state[16];
    uint8_t frame[9];
    int8_t nibbles[16];
    uint8_t framePadded[16];
    int16_t out[16];
    Cmd cmds[3];

    memset(book, 0, sizeof(book));
    /* Predictor 0: predicted = hist2*100 >> 11 -- deliberately a DIFFERENT, easily-distinguished
       formula from predictor 1's, so wrongly falling back to predictor 0 is unmistakable. */
    book[0] = 100; /* predictor0 tap0 (hist2) col0 */
    book[8] = 0;   /* predictor0 tap1 (hist1) col0 */
    /* Predictor 1 (the one the frame header actually selects): predicted == hist1 exactly
       (tap0/hist2 weight 0, tap1/hist1 weight 2048 == Q11 unity). */
    book[16 + 0] = 0;
    book[16 + 8] = 2048;

    memset(state, 0, sizeof(state));
    state[15] = 500;  /* hist1 (newest, tail slot -- full-last-frame state layout) */
    state[14] = -1000; /* hist2 (older) -- must be IGNORED by predictor 1's formula */

    memset(nibbles, 0, sizeof(nibbles)); /* residual0 = 0 -> decoded sample0 == predicted exactly */
    PackAdpcmFrame(frame, 0x01 /* shift=0, predIdx=1 */, nibbles);
    memset(framePadded, 0, sizeof(framePadded));
    memcpy(framePadded, frame, 9);

    {
        uint32_t bookAddr = RegisterRegion(book, sizeof(book));
        uint32_t stateAddr = RegisterRegion(state, sizeof(state));
        DmemLoad(0x0F00, framePadded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetBuff(0x0F00, 0x0F40, 32 /* 16 samples */);
        cmds[2] = MkAdpcm(0 /* A_CONTINUE: hist1/hist2 read from state tail [15]/[14] */, stateAddr);
        RunCmds(cmds, 3);
        DmemSave(0x0F40 + 32, out, 32);
    }

    /* Predictor 1 correctly selected -> predicted == hist1 == 500 exactly. Predictor 0's formula
       would give (100*-1000)>>11 == -49, an unmistakably different (and wrong-sign) value. */
    CheckEq32("predIdx=1 selects predictor-1 coefficients (predicted == hist1, not predictor-0's)",
              500, out[0]);

    return 1;
}

// =================================================================================================
// TEST 12 -- ADPCM order-2 predictor at extreme coefficients and max scale: no 32-bit-overflow
// polarity flip. Hardware computes the predictor multiply-accumulate in a wide (48-bit) RSP vector
// accumulator, never a 32-bit int. With BOTH codebook coefficients AND both history samples at
// -32768 -- the loudest possible signal on both feedback taps at once -- a naive int32 accumulation
// reaches 2^31, one bit past INT32_MAX: overflow UB that under 2's-complement wraparound FLIPS THE
// SIGN, turning a correctly-saturating +32767 into -32768, a full-scale polarity-inverted spike
// instead of a clean clip. Combined with the maximum scale nibble (15) and a large residual, so
// both edges come from the same frame.
// =================================================================================================
static int TestAdpcmExtremeCoefficientAndMaxScaleSaturates(void) {
    int16_t book[16];
    int16_t state[16];
    uint8_t frame[9];
    int8_t nibbles[16];
    uint8_t framePadded[16];
    int16_t out[16];
    Cmd cmds[3];

    memset(book, 0, sizeof(book));
    book[0] = -32768; /* tap0 (hist2) weight -- worst-case magnitude */
    book[8] = -32768; /* tap1 (hist1) weight -- worst-case magnitude */

    memset(state, 0, sizeof(state));
    state[15] = -32768; /* hist1 (newest, tail slot) -- worst-case magnitude */
    state[14] = -32768; /* hist2 (older) -- worst-case magnitude */

    memset(nibbles, 0, sizeof(nibbles));
    nibbles[0] = -8; /* max-magnitude 4-bit nibble */
    PackAdpcmFrame(frame, 0xF0 /* shift=15 (max), predIdx=0 */, nibbles);
    memset(framePadded, 0, sizeof(framePadded));
    memcpy(framePadded, frame, 9);

    {
        uint32_t bookAddr = RegisterRegion(book, sizeof(book));
        uint32_t stateAddr = RegisterRegion(state, sizeof(state));
        DmemLoad(0x0F80, framePadded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetBuff(0x0F80, 0x0FC0, 32 /* 16 samples */);
        cmds[2] = MkAdpcm(0 /* A_CONTINUE: hist1/hist2 read from state tail [15]/[14] */, stateAddr);
        RunCmds(cmds, 3);
        /* Decode region is 0x0FC0..0x0FFF (32B preamble + 32B fresh) -- exactly fills the
           top of the 4KB DMEM; the fresh samples sit at 0x0FE0. */
        DmemSave(0x0FC0 + 32, out, 32);
    }

    /* predicted = ((-32768*-32768) + (-32768*-32768)) >> 11 = 2147483648 >> 11 = 1048576 (needs the
       int64 accumulator -- 2147483648 does not fit in int32_t). + residual (-8<<15 = -262144) =
       786432 -> clamps to +32767. The pre-fix int32 overflow instead wraps the accumulator to
       INT32_MIN before the shift, producing -32768: a full-scale POLARITY FLIP, not a clean clip. */
    CheckEq32("order-2 predictor + max scale(15): saturates to +32767, no int32-overflow polarity flip",
              32767, out[0]);

    return 1;
}

// =================================================================================================
// TEST 13 -- reverb feedback decay: impulse in, assert DECAY not growth (the Mute City tunnel
// "boom"). synthesis.c's per-tick reverb decay is exactly one in-place aMix,
// `aMix(DMEM_2CH_SIZE>>4, reverb->decayRatio + 0x8000, DMEM_WET_LEFT_CH, DMEM_WET_LEFT_CH)`, and
// the comment there -- "(+0x8000) here is -100%" -- confirms the gain field is SIGNED Q15, so the
// op computes dst = dst + dst*gain/32768 = dst*(decayRatio/32768). For any decayRatio in the SDK's
// valid range [0, 0x7FFF] that ratio is always < 1.0: a closed-form proof the op is mix-gain
// stable, not just a spot check. This drives the REAL A_MIXER path over many ticks and asserts the
// magnitude never exceeds the previous tick's (ruling the op out as an unbounded-resonance source)
// and has meaningfully decayed by the end (ruling out a silent no-op). It pins the mixer op ONLY:
// the reverb's actual field values are ROM/decomp-owned data this file cannot read.
// =================================================================================================
static int TestReverbDecayNoGrowth(void) {
    enum { NITER = 8 };
    int16_t buf[8];
    int32_t prevAbs;
    int i, iter;
    const int32_t decayRatio = 0x6000; /* ~0.75 -- a plausible mid-range reverb decay ratio, well
                                           under the 0x8000 (unity) ceiling */
    const int32_t gain = (int16_t)(uint16_t)(decayRatio + 0x8000);

    for (i = 0; i < 8; i++) buf[i] = 20000; /* loud impulse-like reverb tail, all 8 lanes */
    DmemLoad(0x0FE0, buf, 16);
    prevAbs = 20000;

    for (iter = 0; iter < NITER; iter++) {
        int16_t cur[8];
        int k;
        Cmd cmd = MkMixer(1 /* count8 -> 8 samples */, gain, 0x0FE0 /* dmemIn */, 0x0FE0 /* dmemOut:
                                                                         SAME address, matches the
                                                                         real in-place reverb decay */);
        RunCmds(&cmd, 1);
        DmemSave(0x0FE0, cur, 16);
        for (k = 0; k < 8; k++) {
            int32_t a = cur[k] < 0 ? -(int32_t)cur[k] : (int32_t)cur[k];
            gSubChecks++;
            if (a > prevAbs) {
                gSubFails++;
                printf("       MISMATCH: reverb decay GREW at iter %d ch %d: |%d| > |%d| (previous tick)\n",
                       iter, k, (int)a, (int)prevAbs);
            }
        }
        prevAbs = cur[0] < 0 ? -(int32_t)cur[0] : (int32_t)cur[0];
    }

    /* After NITER=8 decays of ~0.75x each (~0.75^8 ~= 0.10x), the tail must be well below its
       starting loudness -- proves genuine bounded decay, not a no-op. */
    gSubChecks++;
    if (prevAbs >= 20000) {
        gSubFails++;
        printf("       MISMATCH: reverb tail did not decay after %d iterations (final |%d|)\n",
               NITER, (int)prevAbs);
    }

    return 1;
}

// =================================================================================================
// TEST 14 -- A_SETLOOP persistence across SEPARATE gdx_audio_hle_run calls. Call 1 issues SETLOOP
// and ADPCM(A_LOOP) together; call 2 is a different invocation carrying ONLY ADPCM(A_LOOP), with no
// SETLOOP of its own. With a function-local pending loop pointer, call 2 silently falls back to its
// own (here deliberately garbage-filled) per-note state instead of the persisted loop history. The
// same book/loopState construction as TestAdpcmLoopRestore isolates loopState[14] into sample0.
// =================================================================================================
static int TestSetLoopPersistsAcrossCalls(void) {
    int16_t book[16];
    int16_t loopState[16];
    int16_t noteState1[16], noteState2[16];
    uint8_t frame[9], framePadded[16];
    int8_t nibbles[16];
    int16_t out1[16], out2[16];
    Cmd cmds[4];
    int i;

    memset(book, 0, sizeof(book));
    book[0] = 2048; /* tap0 (hist2 = older = loopState[14]) Q11-unity -> predicted == loopState[14] */
    book[8] = 0;

    memset(loopState, 0, sizeof(loopState));
    loopState[14] = 777; /* hist2 (older) -- must be used by sample0 of BOTH calls (tap0 unity) */
    loopState[15] = 333; /* hist1 (newest) -- must be ignored (tap1==0) */

    for (i = 0; i < 16; i++) nibbles[i] = (int8_t)((i % 16) - 8);
    nibbles[0] = 0; /* residual0 = 0 -> decoded sample0 == predicted exactly */
    PackAdpcmFrame(frame, 0x00 /* shift=0 pred=0 */, nibbles);
    memset(framePadded, 0, sizeof(framePadded));
    memcpy(framePadded, frame, 9);

    memset(noteState1, 0, sizeof(noteState1));
    memset(noteState2, 0x11, sizeof(noteState2)); /* garbage decoy -- must not be read for A_LOOP */

    {
        uint32_t bookAddr = RegisterRegion(book, sizeof(book));
        uint32_t loopAddr = RegisterRegion(loopState, sizeof(loopState));
        uint32_t noteAddr1 = RegisterRegion(noteState1, sizeof(noteState1));
        uint32_t noteAddr2 = RegisterRegion(noteState2, sizeof(noteState2));

        /* Call 1: SETLOOP + ADPCM(A_LOOP) together in ONE gdx_audio_hle_run invocation. */
        DmemLoad(0x0000, framePadded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetLoop(loopAddr);
        cmds[2] = MkSetBuff(0x0000, 0x0040, 32 /* 16 samples */);
        cmds[3] = MkAdpcm(2 /* A_LOOP */, noteAddr1);
        RunCmds(cmds, 4);
        DmemSave(0x0040 + 32, out1, 32);

        /* Call 2: a SEPARATE gdx_audio_hle_run invocation, ADPCM(A_LOOP) ONLY -- no SETLOOP here. */
        DmemLoad(0x0000, framePadded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetBuff(0x0000, 0x0040, 32 /* 16 samples */);
        cmds[2] = MkAdpcm(2 /* A_LOOP */, noteAddr2);
        RunCmds(cmds, 3);
        DmemSave(0x0040 + 32, out2, 32);
    }

    CheckEq32("call 1 sample0 == loopState[14] (older/hist2, tap0)", 777, out1[0]);
    CheckEq32("call 2 (no SETLOOP of its own) sample0 STILL == loopState[14] (persisted)", 777, out2[0]);

    return 1;
}

// =================================================================================================
// TEST 15 -- ENVMIXER per-block ramp stepping, nead wet cascade, and the full-volume regression.
// Part (a): two 8-sample blocks with nonzero L/R/reverb ramps, expectations computed
// PROGRAMMATICALLY per block (all gains powers of two, so every intermediate >>16 is exact and no
// rounding ambiguity remains), proving the ramps step once per 8-sample block and that wetL/wetR
// are functions of dryL/dryR per channel rather than a shared mono value off the raw input.
// Part (b): near-unity volume (0xFFF0, this port's "full volume" convention) with reverb off must
// NOT drive the dry output into rail-to-rail clipping, and wet must be exactly 0 -- the
// >>16-vs->>12 regression (see n64_audio_hle.c's ENVMIXER comment).
// =================================================================================================
static int TestEnvMixerNeadRampAndRegression(void) {
    /* --- Part (a): per-block ramp stepping + cascaded wet. --- */
    {
        int16_t src[16];
        int16_t dryL[16], dryR[16], wetL[16], wetR[16];
        int16_t expDryL[16], expDryR[16], expWetL[16], expWetR[16];
        const uint32_t curVolLeft0 = 0x4000u, rampLeft = 0x2000;   /* block1: 0x6000 */
        const uint32_t curVolRight0 = 0x2000u, rampRight = 0x1000; /* block1: 0x3000 */
        const uint32_t a0 = 64u, rampReverb = 0x2000;              /* envReverbVol2: 0x4000 -> 0x6000 */
        int32_t curVolL = (int32_t)curVolLeft0, curVolR = (int32_t)curVolRight0;
        int32_t curReverb = (int32_t)(a0 << 8);
        int blk, n, i;

        for (i = 0; i < 16; i++) src[i] = (int16_t)((i + 1) * 512); /* power-of-two-friendly values */

        for (blk = 0; blk < 2; blk++) {
            for (n = 0; n < 8; n++) {
                int idx = blk * 8 + n;
                int32_t dl = (src[idx] * curVolL) >> 16;
                int32_t dr = (src[idx] * curVolR) >> 16;
                expDryL[idx] = (int16_t)dl;
                expDryR[idx] = (int16_t)dr;
                expWetL[idx] = (int16_t)((dl * curReverb) >> 16); /* cascades dl, not raw src */
                expWetR[idx] = (int16_t)((dr * curReverb) >> 16); /* cascades dr, not raw src */
            }
            curVolL += (int32_t)rampLeft;
            curVolR += (int32_t)rampRight;
            curReverb += (int32_t)rampReverb;
        }

        {
            Cmd cmds[3];
            cmds[0] = MkClearBuff(0x0300, 32);
            RunCmds(cmds, 1);
            cmds[0] = MkClearBuff(0x0340, 32);
            RunCmds(cmds, 1);
            cmds[0] = MkClearBuff(0x0380, 32);
            RunCmds(cmds, 1);
            cmds[0] = MkClearBuff(0x03C0, 32);
            RunCmds(cmds, 1);

            DmemLoad(0x0100, src, 32);

            cmds[0] = MkEnvSetup1(a0, (int32_t)rampReverb, (int32_t)rampLeft, (int32_t)rampRight);
            cmds[1] = MkEnvSetup2(curVolLeft0, curVolRight0);
            cmds[2] = MkEnvMixer(0x0100 /* src */, 16 /* sampleCount, 2 blocks */, 0 /* swapLR */,
                                  0x0300 /* dryLeft */, 0x0340 /* dryRight */,
                                  0x0380 /* wetLeft */, 0x03C0 /* wetRight */);
            RunCmds(cmds, 3);

            DmemSave(0x0300, dryL, 32);
            DmemSave(0x0340, dryR, 32);
            DmemSave(0x0380, wetL, 32);
            DmemSave(0x03C0, wetR, 32);
        }

        CheckS16Array("envmixer ramp: dry-left stepped per block", expDryL, dryL, 16, 0);
        CheckS16Array("envmixer ramp: dry-right stepped per block", expDryR, dryR, 16, 0);
        CheckS16Array("envmixer ramp: wet-left cascades dry-left, stepped per block", expWetL, wetL, 16, 0);
        CheckS16Array("envmixer ramp: wet-right cascades dry-right, stepped per block", expWetR, wetR, 16, 0);
    }

    /* --- Part (b): full-volume (near-unity, 0xFFF0) regression -- no clipping, wet == 0. --- */
    {
        int16_t src[8];
        int16_t dryL[8], dryR[8], wetL[8], wetR[8];
        int16_t expDry[8];
        const uint32_t curVol = 0xFFF0u; /* this port's own "full volume" convention, both channels */
        int i;

        for (i = 0; i < 8; i++) {
            src[i] = (int16_t)(20000 + i * 100); /* loud but not already at the s16 ceiling */
            expDry[i] = (int16_t)(((int32_t)src[i] * (int32_t)curVol) >> 16);
        }

        {
            Cmd cmds[3];
            cmds[0] = MkClearBuff(0x0400, 16);
            RunCmds(cmds, 1);
            cmds[0] = MkClearBuff(0x0420, 16);
            RunCmds(cmds, 1);
            cmds[0] = MkClearBuff(0x0440, 16);
            RunCmds(cmds, 1);
            cmds[0] = MkClearBuff(0x0460, 16);
            RunCmds(cmds, 1);

            DmemLoad(0x0480, src, 16);

            /* a=0 -> envReverbVol2=0 -> wet must be exactly 0 regardless of dry scale. */
            cmds[0] = MkEnvSetup1(0 /* a */, 0, 0, 0);
            cmds[1] = MkEnvSetup2(curVol, curVol);
            cmds[2] = MkEnvMixer(0x0480, 8, 0, 0x0400, 0x0420, 0x0440, 0x0460);
            RunCmds(cmds, 3);

            DmemSave(0x0400, dryL, 16);
            DmemSave(0x0420, dryR, 16);
            DmemSave(0x0440, wetL, 16);
            DmemSave(0x0460, wetR, 16);
        }

        CheckS16Array("envmixer full-volume regression: dry-left == src*0xFFF0>>16 (not >>12-overdriven)",
                      expDry, dryL, 8, 0);
        CheckS16Array("envmixer full-volume regression: dry-right == src*0xFFF0>>16 (not >>12-overdriven)",
                      expDry, dryR, 8, 0);
        for (i = 0; i < 8; i++) {
            gSubChecks++;
            if (wetL[i] != 0 || wetR[i] != 0) {
                gSubFails++;
                printf("       MISMATCH: envmixer wet[%d] expected 0 (envWet=0), got L=%d R=%d\n",
                       i, wetL[i], wetR[i]);
            }
        }
    }

    return 1;
}

// =================================================================================================
// TEST 16 -- A_RESAMPLE count rounding. RunResample rounds its requested byte count up to a whole
// 8-sample (16-byte) granule (mupen alist.c#L621-639's `(count+0xf)&~0xf`), so a caller requesting
// a NON-16-byte-aligned count still gets the FULL rounded output written, and the persisted state
// reflects that TRUE consumption rather than the nominal request. Splits a stream at a nominal 5
// samples (rounding to 8 internally) then continues for the remaining aligned 8, asserting
// bit-identical output against a single combined 16-sample run -- so the state carry, and the
// dmemIn delta a caller must apply, follow the ACTUAL rounded count.
// =================================================================================================
static int TestResampleCountRoundingContinuity(void) {
    int16_t in[48];
    int16_t combinedOut[16];
    int16_t chunkOut[16];
    const uint32_t pitch = 0x8800u; /* arbitrary non-unity pitch, > 1.0x */
    int i;

    for (i = 0; i < 48; i++) in[i] = (int16_t)(3000 + i * 73 - (i % 7) * 41);

    /* --- Combined: one call, N=16 (the ground-truth full-rounded-total reference). --- */
    {
        int16_t state[16];
        uint32_t stateAddr;
        Cmd cmds[2];
        memset(state, 0, sizeof(state));
        stateAddr = RegisterRegion(state, sizeof(state));
        DmemLoad(0x0500, in, 96 /* 48 samples */);
        cmds[0] = MkSetBuff(0x0500, 0x0580, 32 /* 16 output samples */);
        cmds[1] = MkResample(1 /* A_INIT */, pitch, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0580, combinedOut, 32);
    }

    /* --- Chunked: call1 requests a NOMINAL 5 samples (10 bytes, non-16-byte-aligned) -- rounds up
       to actualCount1=8 internally. call2 continues for the remaining 8 samples, with dmemIn
       shifted by the WHOLE-SAMPLE delta the ROUNDED call1 actually consumed (not the nominal 5). */
    {
        int16_t state[16];
        uint32_t stateAddr;
        Cmd cmds[2];
        const uint32_t actualCount1 = ((5u * 2u + 0xFu) & ~0xFu) / 2u; /* == 8, mirrors RunResample */
        const uint32_t delta1 = (uint32_t)(((uint64_t)actualCount1 * ((uint64_t)pitch << 1)) >> 16);

        memset(state, 0, sizeof(state));
        stateAddr = RegisterRegion(state, sizeof(state));

        DmemLoad(0x0600, in, 96);
        cmds[0] = MkSetBuff(0x0600, 0x0680, 10 /* nominal 5 output samples, non-16-byte-aligned */);
        cmds[1] = MkResample(1 /* A_INIT */, pitch, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x0680, chunkOut, 16 /* actualCount1=8 real samples were written */);

        cmds[0] = MkSetBuff(0x0600 + delta1 * 2u, 0x06A0, 16 /* 8 output samples */);
        cmds[1] = MkResample(0 /* continue */, pitch, stateAddr);
        RunCmds(cmds, 2);
        DmemSave(0x06A0, chunkOut + 8, 16);
    }

    CheckS16Array("resample count-rounding continuity (nominal-5 chunk vs combined N=16)",
                  combinedOut, chunkOut, 16, 0);

    return 1;
}

// =================================================================================================
// TEST 17 -- A_ADDMIXER: gainless clamped unity add, matching mupen's alist_add (alist.c#L595-609)
// -- `dst = clamp_s16(dst + src)`, no multiply. (a) a moderate case confirms the exact arithmetic
// sum, which a leftover gain read from w0's low 16 bits would scale; (b) a near-full-scale case
// confirms saturation rather than wraparound.
// =================================================================================================
static int TestAddMixerGainlessUnityAdd(void) {
    int16_t inBuf[8], outBuf[8], result[8], expected[8];
    int i;

    /* (a) Moderate values: exact arithmetic sum, no gain scaling. */
    for (i = 0; i < 8; i++) { inBuf[i] = (int16_t)(1000 + i * 100); outBuf[i] = (int16_t)(500 + i * 50); }
    for (i = 0; i < 8; i++) expected[i] = RefClampS16((int32_t)inBuf[i] + (int32_t)outBuf[i]);

    DmemLoad(0x0700, inBuf, 16);
    DmemLoad(0x0740, outBuf, 16);
    {
        Cmd cmd = MkAddMixer(1 /* count8 -> 8 samples */, 0x0700, 0x0740);
        RunCmds(&cmd, 1);
    }
    DmemSave(0x0740, result, 16);
    CheckS16Array("A_ADDMIXER: gainless unity add (moderate values, no gain scaling)", expected, result, 8, 0);

    /* (b) Near-full-scale: SATURATES, does not wrap. */
    {
        const int32_t V = 30000;
        int16_t inBuf2[8], outBuf2[8], result2[8], expected2[8];
        for (i = 0; i < 8; i++) { inBuf2[i] = (int16_t)V; outBuf2[i] = (int16_t)V; }
        for (i = 0; i < 8; i++) expected2[i] = RefClampS16((int32_t)inBuf2[i] + (int32_t)outBuf2[i]);

        DmemLoad(0x0780, inBuf2, 16);
        DmemLoad(0x07C0, outBuf2, 16);
        {
            Cmd cmd = MkAddMixer(1, 0x0780, 0x07C0);
            RunCmds(&cmd, 1);
        }
        DmemSave(0x07C0, result2, 16);
        CheckS16Array("A_ADDMIXER: near-full-scale saturates, does not wrap", expected2, result2, 8, 0);
        for (i = 0; i < 8; i++) {
            gSubChecks++;
            if (result2[i] != 32767) {
                gSubFails++;
                printf("       MISMATCH: A_ADDMIXER[%d] expected saturated +32767, got %d\n", i, result2[i]);
            }
        }
    }

    return 1;
}

// =================================================================================================
// TEST 18 -- ADPCM deferred clamping, loud-transient parity. An INTEGRATOR codebook (predicted ==
// hist1 exactly, book[8]=Q11-unity, book[0]=0) with 5 consecutive strongly-positive residuals
// pushes the running RAW sum past +32767 MID-FRAME, then a negative residual pulls it back down --
// to a value an always-clamp integrator can never reach, since it permanently re-bases off the
// clamped ceiling the instant it first clips. Both an OLD-style (always clamp) and the NEW
// (deferred) reference model are computed independently here; the interpreter must match NEW and
// genuinely DIVERGE from OLD, so the crafted frame is proven to exercise the behavior rather than
// pass vacuously.
// =================================================================================================
static int TestAdpcmDeferredClampingLoudTransient(void) {
    int16_t book[16];
    uint8_t frame[9], framePadded[16];
    int8_t nibbles[16];
    int16_t out[16];
    int16_t oldModel[16], newModel[16];
    int32_t hOld, hNew; /* running integrator state: CLAMPED for hOld, RAW for hNew */
    int i;

    memset(book, 0, sizeof(book));
    book[0] = 0;    /* hist2 weight -- unused (isolates a pure hist1 integrator) */
    book[8] = 2048; /* hist1 weight, Q11-unity -> predicted == hist1 exactly */

    memset(nibbles, 0, sizeof(nibbles));
    for (i = 0; i < 5; i++) nibbles[i] = 7; /* +7<<10 = +7168 each, 5x pushes the sum past 32767 */
    nibbles[5] = -8;                         /* -8<<10 = -8192, pulls the (would-be) overshoot back */
    /* nibbles[6..15] stay 0 -> residual 0, so every later sample's output equals the propagated
       history exactly -- the clean way to observe the divergence downstream. */

    PackAdpcmFrame(frame, 0xA0 /* shift=10 (0xA), predIdx=0 */, nibbles);
    memset(framePadded, 0, sizeof(framePadded));
    memcpy(framePadded, frame, 9);

    /* --- Independent reference models (pure integrator, shift=10, matching the frame above). --- */
    hOld = 0;
    hNew = 0;
    for (i = 0; i < 16; i++) {
        int32_t residual = (int32_t)nibbles[i] << 10;
        int32_t rawOld = hOld + residual; /* OLD: predictor always reads the CLAMPED previous value */
        int32_t rawNew = hNew + residual; /* NEW: predictor reads the RAW previous value */
        oldModel[i] = RefClampS16(rawOld);
        newModel[i] = RefClampS16(rawNew);
        hOld = (int32_t)oldModel[i]; /* clamped feedback, every sample */
        hNew = rawNew;               /* raw feedback, every sample */
    }

    {
        uint32_t bookAddr = RegisterRegion(book, sizeof(book));
        uint32_t stateAddr;
        int16_t state[16];
        memset(state, 0, sizeof(state));
        stateAddr = RegisterRegion(state, sizeof(state));

        DmemLoad(0x0900, framePadded, 16);
        {
            Cmd cmds[3];
            cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
            cmds[1] = MkSetBuff(0x0900, 0x0940, 32 /* 16 samples */);
            cmds[2] = MkAdpcm(1 /* A_INIT */, stateAddr);
            RunCmds(cmds, 3);
        }
        DmemSave(0x0940 + 32, out, 32);
    }

    CheckS16Array("ADPCM deferred clamping matches the NEW (raw intra-frame) reference model",
                  newModel, out, 16, 0);

    /* Sanity: the crafted frame must actually EXERCISE the divergence, or this test would pass
       vacuously regardless of which clamping behavior is implemented. */
    {
        int differs = 0;
        for (i = 0; i < 16; i++) {
            if (oldModel[i] != newModel[i]) { differs = 1; break; }
        }
        gSubChecks++;
        if (!differs) {
            gSubFails++;
            printf("       MISMATCH: crafted frame did not diverge old-vs-new reference models -- "
                   "test is not exercising deferred clamping\n");
        }
    }

    return 1;
}

// =================================================================================================
// TEST 19 -- ADPCM last-frame preamble contract. The real aspMain ADPCM op writes its 16-sample
// persistent state to the output buffer BEFORE the freshly decoded frames (output = [16 prev-tail
// samples][count/2 fresh]) and persists the last 16 output samples ending at the TRUE count
// boundary. synthesis.c's skipInitialSamples=16 / skipBytes window arithmetic depends on that
// layout; without it every continuing tick reads about a frame ahead of its true position, with a
// waveform cliff at every tick boundary. Asserts (a) A_INIT emits a zero preamble, (b) A_CONTINUE
// emits the previous call's final 16 samples, (c) the persisted state equals the last 16 fresh
// output samples.
// =================================================================================================
static int TestAdpcmLastFramePreambleContract(void) {
    int16_t book[16];
    int8_t nibblesF1[16], nibblesF2[16];
    uint8_t frame1[9], frame2[9], frame1Padded[16], frame2Padded[16];
    int16_t state[16];
    int16_t pre1[16], out1[16], pre2[16], out2[16];
    Cmd cmds[3];
    int i;

    memset(book, 0, sizeof(book));
    book[0] = 600;
    book[8] = -300;

    for (i = 0; i < 16; i++) {
        nibblesF1[i] = (int8_t)(((i * 3 + 5) % 16) - 8);
        nibblesF2[i] = (int8_t)(((i * 11 + 2) % 16) - 8);
    }
    PackAdpcmFrame(frame1, 0x20 /* shift=2 pred=0 */, nibblesF1);
    PackAdpcmFrame(frame2, 0x20, nibblesF2);
    memset(frame1Padded, 0, sizeof(frame1Padded));
    memcpy(frame1Padded, frame1, 9);
    memset(frame2Padded, 0, sizeof(frame2Padded));
    memcpy(frame2Padded, frame2, 9);

    memset(state, 0, sizeof(state));
    {
        uint32_t bookAddr = RegisterRegion(book, sizeof(book));
        uint32_t stateAddr = RegisterRegion(state, sizeof(state));

        /* Call 1: A_INIT. */
        DmemLoad(0x0A00, frame1Padded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetBuff(0x0A00, 0x0A40, 32 /* 16 samples */);
        cmds[2] = MkAdpcm(1 /* A_INIT */, stateAddr);
        RunCmds(cmds, 3);
        DmemSave(0x0A40, pre1, 32);      /* preamble slot */
        DmemSave(0x0A40 + 32, out1, 32); /* fresh samples */

        /* Call 2: A_CONTINUE, same state. */
        DmemLoad(0x0A00, frame2Padded, 16);
        cmds[0] = MkLoadAdpcm(sizeof(book), bookAddr);
        cmds[1] = MkSetBuff(0x0A00, 0x0AC0, 32 /* 16 samples */);
        cmds[2] = MkAdpcm(0 /* A_CONTINUE */, stateAddr);
        RunCmds(cmds, 3);
        DmemSave(0x0AC0, pre2, 32);
        DmemSave(0x0AC0 + 32, out2, 32);
    }

    /* (a) A_INIT preamble is all zeros. */
    {
        int16_t zeros[16];
        memset(zeros, 0, sizeof(zeros));
        CheckS16Array("A_INIT preamble is zero", zeros, pre1, 16, 0);
    }
    /* (b) A_CONTINUE preamble replays call 1's final 16 output samples. */
    CheckS16Array("A_CONTINUE preamble == previous call's last frame", out1, pre2, 16, 0);
    /* (c) persisted state == call 2's last 16 fresh samples (16-sample call: all of out2). */
    CheckS16Array("persisted state == last 16 output samples", out2, state, 16, 0);

    return 1;
}

typedef int (*TestFn)(void);
typedef struct {
    const char* name;
    TestFn fn;
} TestCase;

int main(void) {
    /* The game-runtime bisection gate on A_FILTER must not apply to the tests. */
#ifdef _MSC_VER
    _putenv("GDX_HLE_FILTER=1");
#else
    setenv("GDX_HLE_FILTER", "1", 1);
#endif
    static const TestCase tests[] = {
        { "ADPCM: frame-boundary continuity (1 call vs 2 chunked calls)", TestAdpcmFrameBoundaryContinuity },
        { "ADPCM: A_SETLOOP restore uses loopState[14]/[15]",             TestAdpcmLoopRestore },
        { "ADPCM: scale/predictor nibble unpacking from header byte",     TestAdpcmNibbleUnpack },
        { "RESAMPLE: unity pitch (0x8000) == exact phase-0 ROM FIR",       TestResampleUnityPitch },
        { "RESAMPLE: known ROM table entries (row 0)",                     TestResampleTableRow0 },
        { "RESAMPLE: pitch-change continuity (per-chunk pitch, no click)", TestResamplePitchChangeContinuity },
        { "ADPCM+RESAMPLE: loop-wrap lookahead continuity (no click)",     TestAdpcmLoopWrapResampleContinuity },
        { "RESAMPLE: half/double pitch stepping (fracQ16/lastSample)",    TestResamplePitchStepping },
        { "RESAMPLE: state continuity (1 call vs 2 chunked calls)",       TestResampleContinuity },
        { "FILTER: direct 8-tap FIR impulse response, cross-block window", TestFilterImpulseResponse },
        { "FILTER: cross-call continuity (tail carried, no LUT averaging)", TestFilterContinuity },
        { "FILTER: protocol-level low-pass (identity unity, row-5 LP, state carry)", TestFilterLowpassProtocol },
        { "ENVMIXER: constant envelope, nead cascade (wet off dry, per channel)", TestEnvMixerConstantGain },
        { "MIXER: near-full-scale accumulate saturates, no wraparound",   TestMixerSaturation },
        { "ADPCM: order-2 predictor INDEX selection (predIdx != 0)",      TestAdpcmPredictorIndexSelection },
        { "ADPCM: extreme coef + max scale(15), no int32-overflow flip",  TestAdpcmExtremeCoefficientAndMaxScaleSaturates },
        { "REVERB: A_MIXER decay op never grows (Mute City tunnel boom)", TestReverbDecayNoGrowth },
        { "SETLOOP: persists across separate gdx_audio_hle_run calls (A1)", TestSetLoopPersistsAcrossCalls },
        { "ENVMIXER: per-block ramp stepping + wet cascade + full-vol regression (A2)", TestEnvMixerNeadRampAndRegression },
        { "RESAMPLE: count-rounding continuity, non-16-aligned split (A5)", TestResampleCountRoundingContinuity },
        { "ADDMIXER: gainless unity add, matches mupen alist_add (A6)",   TestAddMixerGainlessUnityAdd },
        { "ADPCM: deferred clamping, loud-transient parity (A4)",         TestAdpcmDeferredClampingLoudTransient },
        { "ADPCM: last-frame preamble output layout + state persistence", TestAdpcmLastFramePreambleContract },
    };
    const int numTests = (int)(sizeof(tests) / sizeof(tests[0]));
    int i;
    int failedTests = 0;

    printf("=== G-Diffuser DSP unit-test harness (port/n64_audio_hle.c) ===\n\n");

    for (i = 0; i < numTests; i++) {
        int checksBefore = gSubChecks;
        int failsBefore = gSubFails;
        int result;

        printf("-- %s\n", tests[i].name);
        result = tests[i].fn();
        (void)result;

        if (gSubFails > failsBefore) {
            failedTests++;
            printf("[FAIL] %s (%d/%d sub-checks failed)\n\n",
                   tests[i].name, gSubFails - failsBefore, gSubChecks - checksBefore);
        } else {
            printf("[PASS] %s (%d sub-checks)\n\n", tests[i].name, gSubChecks - checksBefore);
        }
    }

    printf("=== Summary: %d/%d tests passed (%d/%d sub-checks passed) ===\n",
           numTests - failedTests, numTests, gSubChecks - gSubFails, gSubChecks);

    return failedTests ? 1 : 0;
}
