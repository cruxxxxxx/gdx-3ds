// Software (HLE) implementation of the N64 audio RSP microcode ("aspMain").
//
// The decomp's CPU-side driver (decomp/src/audio/disk/lib/synthesis.c) only BUILDS the RSP
// command list (the "Acmd" ABI in decomp/include/PR/abi.h); on hardware the aspMain microcode
// (decomp/src/rsp/aspmain.s, an un-decompiled .incbin blob) is what executes it. Nothing on this
// host ever ran that list, so the AI buffers handed to the SDL player were never filled with real
// PCM. This file is that missing DSP stage.
//
// Original implementation, written from abi.h's public field layouts and the publicly documented
// N64 "ABI2" audio semantics. No code was copied from any GPL source; where a reference
// implementation's BEHAVIOR was followed, the citation sits on the function that follows it.
//
// POINTER TRUNCATION -- why a w1 word must never be dereferenced directly. Acmd's
// `Awords { u32 w0; u32 w1; }` holds 32 bits. On hardware the values synthesis.c packs into w1
// for LOADBUFF/SAVEBUFF/ADPCM/LOADADPCM/SETLOOP/RESAMPLE/FILTER are already 32-bit physical
// addresses; here they are real host pointers, and abi.h's macros cast them through (u32), so the
// top 32 bits are gone before this interpreter ever reads the command back. Every pointer that
// reaches an audio command lives either in the 16MB gdx_rdram arena or in the exe's static image,
// so the low 32 bits are reconstructed through n64_gfx_bridge.cpp's existing resolvers -- the
// same mechanism it already uses for GBI display-list words.
//
// Deliberate no-ops and approximations, both on rare paths: A_UNK19 (SDK-unknown, reached only by
// the bookOffset==3 note path) leaves DMEM unchanged; A_INTERL is a best-guess decimation
// (out[k] = in[2k]) for the "two-part" note split.
//
// OPEN QUESTION: whether ROM-sourced 16-bit audio data (ADPCM codebooks, loop predictor history)
// is byte-swapped anywhere in the asset/DMA pipeline before decomp C reads it as host s16. Decomp
// reads these as ordinary native shorts everywhere, and this file follows that same assumption.
// If BGM comes out as structured static rather than recognizable (if mistuned) music, ROM audio
// byte order is the first thing to check -- that would be a pipeline-wide issue, not specific to
// this interpreter.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "port_log.h"
#include "gdx_dev_gates.h"

// Defined in port/n64_gfx_bridge.cpp. Deliberately `unsigned int`, not uintptr_t: the decomp's
// own stdint shim typedefs uintptr_t as a 32-bit type inside gdiffuser_game TUs, so only
// `unsigned int` is unambiguously 32 bits on both sides of this TU boundary.
extern void* gdx_resolve_registered_host_address(unsigned int addr);
extern void* gdx_resolve_module_host_address(unsigned int addr);

/* Defined in libultraship (C ABI). Declared here so this C TU need not pull the C++ bridge
 * header. Backs the live reverb kill switch below. */
extern int CVarGetInteger(const char* name, int defaultValue);

/* Diagnostic gate (n64_sched.c): INI [debug] diag_audio=1 / verbose=1 on the 3DS, or the
 * Dev-Tools verbose gate. Every audio-bisect capture family in this file ([rs-cap],
 * [pcm-cap], [spike], [audio-hle]) sits behind it: ungated they emitted ~1000 filelog +
 * svc + console lines exactly at race start (the [rs-cap] chain trigger and the
 * [pcm-cap] budget are race-armed), each line paying a per-line SD fflush on the audio
 * thread while it holds sAudioCtxMutex — the bottom-screen "hex dump" AND a large slice
 * of the race-start halt. Default OFF. */
extern int gdx_diag_audio_enabled(void);

static void* GdxAudioResolveAddr(uint32_t raw, const char* what) {
    void* p;
    static int sMissLogs = 0;

    if (raw == 0) {
        return NULL;
    }

    p = gdx_resolve_registered_host_address((unsigned int)raw);
    if (p != NULL) {
        return p;
    }
    p = gdx_resolve_module_host_address((unsigned int)raw);
    if (p != NULL) {
        return p;
    }

    if (gdx_diag_audio_enabled() && sMissLogs < 16) {
        sMissLogs++;
        gdx_port_logf("[audio-hle] UNRESOLVED %s addr=%08X (op skipped)\n", what, (unsigned)raw);
    }
    return NULL;
}

// Mirrors decomp/include/PR/abi.h. This build is EXPANSION_KIT=1, which is why A_HILOGAIN is 14
// here and not 24 (see abi.h's #ifdef EXPANSION_KIT).
enum {
    GDX_A_SPNOOP       = 0,
    GDX_A_ADPCM        = 1,
    GDX_A_CLEARBUFF    = 2,
    GDX_A_UNK3         = 3,
    GDX_A_ADDMIXER     = 4,
    GDX_A_RESAMPLE     = 5,
    GDX_A_RESAMPLE_ZOH = 6,
    GDX_A_FILTER       = 7,
    GDX_A_SETBUFF      = 8,
    GDX_A_DMEMMOVE     = 10,
    GDX_A_LOADADPCM    = 11,
    GDX_A_MIXER        = 12,
    GDX_A_INTERLEAVE   = 13,
    GDX_A_HILOGAIN     = 14,
    GDX_A_SETLOOP      = 15,
    GDX_A_INTERL       = 17,
    GDX_A_ENVSETUP1    = 18,
    GDX_A_ENVMIXER     = 19,
    GDX_A_LOADBUFF     = 20,
    GDX_A_SAVEBUFF     = 21,
    GDX_A_ENVSETUP2    = 22,
    GDX_A_S8DEC        = 23,
    GDX_A_UNK19        = 25,
    GDX_A_DUPLICATE    = 26
};

// Real RSP DMEM is 4KB, and decomp/src/audio/disk/lib/audio.h's DMEM_* offsets all assume that
// size. Every offset is masked before use so a malformed command can never write outside sDmem.
#define GDX_DMEM_SIZE 0x1000u
#define GDX_DMEM_MASK (GDX_DMEM_SIZE - 1u)
static uint8_t sDmem[GDX_DMEM_SIZE];
/* [spike] diagnostic: per 16-byte block, the opcode that last wrote it. 0xFF = never written. */
static uint8_t sDmemLastOp[GDX_DMEM_SIZE >> 4];
static uint8_t sDmemCurOp = 0xFF;

/* Stage-bypass flags, read once from gdx-audio-debug.txt next to the exe (env vars proved
   unreliable in the target shell). Space/newline-separated keywords, any subset:
     nofilter  -- skip the per-voice A_FILTER
     flatvol   -- envmixer holds one volume for the whole tick (no 8-block ramp staircase)
     noreverb  -- skip the reverb wet->dry return
     nointerl  -- skip the nParts==2 decimation path
   Enable ONE at a time to attribute an artifact to a stage. */
static int GdxAudioDbg(void) {
    static int sFlags = -1;
    if (sFlags == -1) {
        FILE* f = fopen("gdx-audio-debug.txt", "r");
        sFlags = 0;
        if (f != NULL) {
            char buf[256];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            fclose(f);
            if (strstr(buf, "nofilter") != NULL) sFlags |= 1;
            if (strstr(buf, "flatvol") != NULL) sFlags |= 2;
            if (strstr(buf, "noreverb") != NULL) sFlags |= 4;
            if (strstr(buf, "nointerl") != NULL) sFlags |= 8;
        }
        gdx_port_logf("[audio] stage-bypass flags = 0x%X (gdx-audio-debug.txt)\n", (unsigned)sFlags);
    }
    return sFlags;
}

// aLoadADPCM's DMA target is a ucode-internal location never exposed through the C ABI (the
// command carries no DMEM parameter), so the book can live in a private side buffer.
#define GDX_ADPCM_BOOK_MAX_BYTES 4096u
static uint8_t sAdpcmBook[GDX_ADPCM_BOOK_MAX_BYTES];
static uint32_t sAdpcmBookLen = 0;

typedef struct {
    uint32_t w0;
    uint32_t w1;
} GdxAcmd;

static int16_t ClampS16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int16_t DmemGetS16(uint32_t byteOffset) {
    int16_t v;
    memcpy(&v, &sDmem[byteOffset & GDX_DMEM_MASK & ~1u], sizeof(v));
    return v;
}

static void DmemSetS16(uint32_t byteOffset, int16_t v) {
    memcpy(&sDmem[byteOffset & GDX_DMEM_MASK & ~1u], &v, sizeof(v));
    sDmemLastOp[(byteOffset & GDX_DMEM_MASK) >> 4] = sDmemCurOp;
}

static uint8_t DmemGetU8(uint32_t byteOffset) {
    return sDmem[byteOffset & GDX_DMEM_MASK];
}

static void DmemSetU8(uint32_t byteOffset, uint8_t v) {
    sDmem[byteOffset & GDX_DMEM_MASK] = v;
    sDmemLastOp[(byteOffset & GDX_DMEM_MASK) >> 4] = sDmemCurOp;
}

// Pending buffer descriptor set by A_SETBUFF and consumed by decode/resample commands.
typedef struct GdxBufDesc {
    uint32_t dmemIn;
    uint32_t dmemOut;
    uint32_t count;
} GdxBufDesc;

// Unlock-jingle stage capture. synthesis.c registers the exact Acmd range and persistent state
// addresses of each target note, so this file can attribute commands to voices without touching
// the command list or guessing from sample contents. Four timeline-aligned WAVs (resample,
// pre-envelope, envelope, mix) are written after two seconds.
//
// Decode and resampler-input buffers are only summarized in the log, never emitted as WAVs: they
// run in source-sample space and carry per-call overlap, so concatenating them would create
// seams that look exactly like the defect under investigation. Dormant until a target note is
// registered; no file I/O in normal play.
#define GDX_UNLOCK_STAGE_FRAMES 64000u
#define GDX_UNLOCK_STAGE_MAX_CHUNK 256u
#define GDX_UNLOCK_STAGE_MAX_TARGETS 32u
#define GDX_UNLOCK_STAGE_MAX_RANGES 128u

#define GDX_UNLOCK_STAGE_RESAMPLE_PATH "gdiffuser-unlock-hle-resample.wav"
#define GDX_UNLOCK_STAGE_PRE_ENVELOPE_PATH "gdiffuser-unlock-hle-pre-envelope.wav"
#define GDX_UNLOCK_STAGE_ENVELOPE_PATH "gdiffuser-unlock-hle-envelope.wav"
#define GDX_UNLOCK_STAGE_MIX_PATH "gdiffuser-unlock-hle-mix.wav"

typedef struct GdxUnlockStageStats {
    uint64_t samples;
    uint64_t squareSum;
    uint64_t hash;
    int64_t sampleSum;
    int16_t minSample;
    int16_t maxSample;
    int16_t previousSample;
    uint32_t maxDelta;
    uint32_t zeroSamples;
    uint32_t clippedSamples;
    int hasPreviousSample;
} GdxUnlockStageStats;

typedef struct GdxUnlockStageTarget {
    uint32_t adpcmToken;
    uint32_t resampleToken;
    uint32_t decodeCalls;
    uint32_t decodeBoundaryMaxDelta;
    int noteIndex;
    int hasDecodeLast;
    int16_t decodeLast;
} GdxUnlockStageTarget;

typedef struct GdxUnlockStageRange {
    uintptr_t start;
    uintptr_t end;
    int targetIndex;
} GdxUnlockStageRange;

typedef struct GdxUnlockStageCapture {
    unsigned int generation;
    unsigned int sampleRate;
    uint32_t frames;
    uint32_t chunks;
    uint32_t truncatedChunks;
    uint32_t resampleClipSamples;
    uint32_t preEnvelopeClipSamples;
    uint32_t envelopeClipSamples[2];
    int active;
    int complete;
    int sawHle;
    int chunkStarted;
    int currentTarget;
    uint32_t targetCount;
    uint32_t rangeCount;
    uintptr_t commandList;
    GdxUnlockStageTarget targets[GDX_UNLOCK_STAGE_MAX_TARGETS];
    GdxUnlockStageRange ranges[GDX_UNLOCK_STAGE_MAX_RANGES];
    int32_t chunkResample[GDX_UNLOCK_STAGE_MAX_CHUNK];
    int32_t chunkPreEnvelope[GDX_UNLOCK_STAGE_MAX_CHUNK];
    int32_t chunkEnvelope[2][GDX_UNLOCK_STAGE_MAX_CHUNK];
    int16_t resample[GDX_UNLOCK_STAGE_FRAMES];
    int16_t preEnvelope[GDX_UNLOCK_STAGE_FRAMES];
    int16_t envelope[GDX_UNLOCK_STAGE_FRAMES * 2u];
    int16_t mix[GDX_UNLOCK_STAGE_FRAMES * 2u];
    GdxUnlockStageStats decodeStats;
    GdxUnlockStageStats resampleInputStats;
    GdxUnlockStageStats resampleStats;
    GdxUnlockStageStats preEnvelopeStats;
    GdxUnlockStageStats envelopeStats[2];
    GdxUnlockStageStats mixStats[2];
} GdxUnlockStageCapture;

static GdxUnlockStageCapture sGdxUnlockStage;

static void GdxUnlockStageStatsReset(GdxUnlockStageStats* stats) {
    memset(stats, 0, sizeof(*stats));
    stats->hash = UINT64_C(14695981039346656037);
}

static void GdxUnlockStageStatsBreak(GdxUnlockStageStats* stats) {
    stats->hasPreviousSample = 0;
}

static void GdxUnlockStageStatsAdd(GdxUnlockStageStats* stats, int16_t sample) {
    int32_t value = sample;
    uint32_t delta;

    if (stats->samples == 0) {
        stats->minSample = sample;
        stats->maxSample = sample;
    } else {
        if (sample < stats->minSample) {
            stats->minSample = sample;
        }
        if (sample > stats->maxSample) {
            stats->maxSample = sample;
        }
    }
    stats->samples++;
    stats->sampleSum += value;
    stats->squareSum += (uint64_t)(value * value);
    stats->hash ^= (uint8_t)(uint16_t)sample;
    stats->hash *= UINT64_C(1099511628211);
    stats->hash ^= (uint8_t)((uint16_t)sample >> 8);
    stats->hash *= UINT64_C(1099511628211);
    if (sample == 0) {
        stats->zeroSamples++;
    }
    if ((sample == INT16_MIN) || (sample == INT16_MAX)) {
        stats->clippedSamples++;
    }
    if (stats->hasPreviousSample) {
        int32_t signedDelta = value - stats->previousSample;
        delta = signedDelta < 0 ? (uint32_t)-signedDelta : (uint32_t)signedDelta;
        if (delta > stats->maxDelta) {
            stats->maxDelta = delta;
        }
    }
    stats->previousSample = sample;
    stats->hasPreviousSample = 1;
}

static int16_t GdxUnlockStageClamp(int32_t value, uint32_t* clippedSamples) {
    if (value > INT16_MAX) {
        (*clippedSamples)++;
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        (*clippedSamples)++;
        return INT16_MIN;
    }
    return (int16_t)value;
}

static void GdxUnlockStagePutU16(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void GdxUnlockStagePutU32(uint8_t* dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static int GdxUnlockStageWriteWav(const char* path, const int16_t* pcm, uint32_t frames,
                                  uint16_t channels, uint32_t sampleRate) {
    uint8_t header[44];
    uint8_t sampleBytes[4096];
    uint32_t dataBytes = frames * channels * (uint32_t)sizeof(int16_t);
    uint32_t sampleCount = frames * channels;
    uint32_t sampleIndex = 0;
    FILE* file;
    int wroteAll;

    header[0] = 'R';
    header[1] = 'I';
    header[2] = 'F';
    header[3] = 'F';
    GdxUnlockStagePutU32(&header[4], 36u + dataBytes);
    header[8] = 'W';
    header[9] = 'A';
    header[10] = 'V';
    header[11] = 'E';
    header[12] = 'f';
    header[13] = 'm';
    header[14] = 't';
    header[15] = ' ';
    GdxUnlockStagePutU32(&header[16], 16u);
    GdxUnlockStagePutU16(&header[20], 1u);
    GdxUnlockStagePutU16(&header[22], channels);
    GdxUnlockStagePutU32(&header[24], sampleRate);
    GdxUnlockStagePutU32(&header[28], sampleRate * channels * (uint32_t)sizeof(int16_t));
    GdxUnlockStagePutU16(&header[32], (uint16_t)(channels * sizeof(int16_t)));
    GdxUnlockStagePutU16(&header[34], (uint16_t)(sizeof(int16_t) * 8u));
    header[36] = 'd';
    header[37] = 'a';
    header[38] = 't';
    header[39] = 'a';
    GdxUnlockStagePutU32(&header[40], dataBytes);

    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    wroteAll = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    while (wroteAll && (sampleIndex < sampleCount)) {
        uint32_t samplesThisWrite = sampleCount - sampleIndex;
        uint32_t i;
        if (samplesThisWrite > sizeof(sampleBytes) / 2u) {
            samplesThisWrite = sizeof(sampleBytes) / 2u;
        }
        for (i = 0; i < samplesThisWrite; i++) {
            uint16_t value = (uint16_t)pcm[sampleIndex + i];
            sampleBytes[i * 2u + 0u] = (uint8_t)value;
            sampleBytes[i * 2u + 1u] = (uint8_t)(value >> 8);
        }
        wroteAll = fwrite(sampleBytes, 1, samplesThisWrite * 2u, file) == samplesThisWrite * 2u;
        sampleIndex += samplesThisWrite;
    }
    if (fclose(file) != 0) {
        wroteAll = 0;
    }
    return wroteAll;
}

static void GdxUnlockStageLogStats(const char* stage, const char* channel,
                                   const GdxUnlockStageStats* stats) {
    double mean;
    double rms;

    if (stats->samples == 0) {
        gdx_port_logf("[unlock-stage] stats stage=%s channel=%s samples=0\n", stage, channel);
        return;
    }
    mean = (double)stats->sampleSum / (double)stats->samples;
    rms = sqrt((double)stats->squareSum / (double)stats->samples);
    gdx_port_logf("[unlock-stage] stats stage=%s channel=%s samples=%llu min=%d max=%d "
                  "mean=%.3f rms=%.3f zeros=%u clipped=%u maxDelta=%u hash=%016llX\n",
                  stage, channel, (unsigned long long)stats->samples,
                  stats->minSample, stats->maxSample, mean, rms,
                  (unsigned)stats->zeroSamples, (unsigned)stats->clippedSamples,
                  (unsigned)stats->maxDelta, (unsigned long long)stats->hash);
}

static void GdxUnlockStageChunkReset(void) {
    memset(sGdxUnlockStage.chunkResample, 0, sizeof(sGdxUnlockStage.chunkResample));
    memset(sGdxUnlockStage.chunkPreEnvelope, 0, sizeof(sGdxUnlockStage.chunkPreEnvelope));
    memset(sGdxUnlockStage.chunkEnvelope, 0, sizeof(sGdxUnlockStage.chunkEnvelope));
    sGdxUnlockStage.chunkStarted = 1;
    sGdxUnlockStage.currentTarget = -1;
}

static void GdxUnlockStageReset(unsigned int generation, unsigned int sampleRate) {
    memset(&sGdxUnlockStage, 0, sizeof(sGdxUnlockStage));
    sGdxUnlockStage.generation = generation;
    sGdxUnlockStage.sampleRate = sampleRate;
    sGdxUnlockStage.active = 1;
    sGdxUnlockStage.currentTarget = -1;
    GdxUnlockStageStatsReset(&sGdxUnlockStage.decodeStats);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.resampleInputStats);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.resampleStats);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.preEnvelopeStats);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.envelopeStats[0]);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.envelopeStats[1]);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.mixStats[0]);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.mixStats[1]);
    gdx_port_logf("[unlock-stage] armed generation=%u frames=%u sampleRate=%u executor=HLE-required\n",
                  generation, GDX_UNLOCK_STAGE_FRAMES, sampleRate);
}

void gdx_unlock_audio_stage_begin_command_list(unsigned int generation, const void* commandList,
                                               unsigned int sampleRate) {
    if ((generation == 0u) || (commandList == NULL) || (sampleRate == 0u)) {
        return;
    }
    if (generation != sGdxUnlockStage.generation) {
        GdxUnlockStageReset(generation, sampleRate);
    }
    if (sGdxUnlockStage.complete) {
        return;
    }
    sGdxUnlockStage.commandList = (uintptr_t)commandList;
    sGdxUnlockStage.rangeCount = 0;
}

static int GdxUnlockStageFindOrAddTarget(int noteIndex, uint32_t adpcmToken,
                                         uint32_t resampleToken) {
    uint32_t i;
    GdxUnlockStageTarget* target;

    for (i = 0; i < sGdxUnlockStage.targetCount; i++) {
        target = &sGdxUnlockStage.targets[i];
        if ((target->resampleToken == resampleToken) || (target->noteIndex == noteIndex)) {
            target->adpcmToken = adpcmToken;
            target->resampleToken = resampleToken;
            target->noteIndex = noteIndex;
            return (int)i;
        }
    }
    if (sGdxUnlockStage.targetCount >= GDX_UNLOCK_STAGE_MAX_TARGETS) {
        return -1;
    }

    target = &sGdxUnlockStage.targets[sGdxUnlockStage.targetCount++];
    memset(target, 0, sizeof(*target));
    target->adpcmToken = adpcmToken;
    target->resampleToken = resampleToken;
    target->noteIndex = noteIndex;
    gdx_port_logf("[unlock-stage] target generation=%u note=%d adpcm=%08X resample=%08X count=%u\n",
                  sGdxUnlockStage.generation, noteIndex, (unsigned)adpcmToken, (unsigned)resampleToken,
                  (unsigned)sGdxUnlockStage.targetCount);
    return (int)(sGdxUnlockStage.targetCount - 1u);
}

void gdx_unlock_audio_stage_register_command_range(
    unsigned int generation, int noteIndex, const void* commandStart, const void* commandEnd,
    const void* adpcmState, const void* resampleState, unsigned int sampleRate) {
    uint32_t adpcmToken;
    uint32_t resampleToken;
    int targetIndex;
    GdxUnlockStageRange* range;

    if ((generation == 0u) || (commandStart == NULL) || (commandEnd == NULL) ||
        (adpcmState == NULL) || (resampleState == NULL) || (sampleRate == 0u) ||
        ((uintptr_t)commandEnd <= (uintptr_t)commandStart)) {
        return;
    }
    if (generation != sGdxUnlockStage.generation) {
        GdxUnlockStageReset(generation, sampleRate);
    }
    if (sGdxUnlockStage.complete ||
        (sGdxUnlockStage.rangeCount >= GDX_UNLOCK_STAGE_MAX_RANGES)) {
        return;
    }
    adpcmToken = (uint32_t)(uintptr_t)adpcmState;
    resampleToken = (uint32_t)(uintptr_t)resampleState;
    targetIndex = GdxUnlockStageFindOrAddTarget(noteIndex, adpcmToken, resampleToken);
    if (targetIndex < 0) {
        return;
    }
    range = &sGdxUnlockStage.ranges[sGdxUnlockStage.rangeCount++];
    range->start = (uintptr_t)commandStart;
    range->end = (uintptr_t)commandEnd;
    range->targetIndex = targetIndex;
}

static int GdxUnlockStageFindCommandTarget(const GdxAcmd* command) {
    uintptr_t address;
    uint32_t i;

    if (!sGdxUnlockStage.active || sGdxUnlockStage.complete) {
        return -1;
    }
    address = (uintptr_t)command;
    for (i = 0; i < sGdxUnlockStage.rangeCount; i++) {
        const GdxUnlockStageRange* range = &sGdxUnlockStage.ranges[i];
        if ((address >= range->start) && (address < range->end)) {
            return range->targetIndex;
        }
    }
    return -1;
}

static void GdxUnlockStageCaptureDecode(int targetIndex, const GdxBufDesc* buf, uint32_t flags) {
    GdxUnlockStageTarget* target;
    uint32_t sampleCount;
    uint32_t i;
    int16_t first;
    int16_t last;

    if ((targetIndex < 0) || (buf == NULL)) {
        return;
    }
    target = &sGdxUnlockStage.targets[targetIndex];
    if ((flags & 1u) != 0u) {
        target->hasDecodeLast = 0;
    }
    sampleCount = buf->count / 2u;
    if (sampleCount == 0u) {
        return;
    }
    first = DmemGetS16(buf->dmemOut + 32u);
    last = DmemGetS16(buf->dmemOut + 32u + (sampleCount - 1u) * 2u);
    if (target->hasDecodeLast) {
        int32_t signedDelta = (int32_t)first - target->decodeLast;
        uint32_t delta = signedDelta < 0 ? (uint32_t)-signedDelta : (uint32_t)signedDelta;
        if (delta > target->decodeBoundaryMaxDelta) {
            target->decodeBoundaryMaxDelta = delta;
        }
    }
    target->decodeLast = last;
    target->hasDecodeLast = 1;
    target->decodeCalls++;

    GdxUnlockStageStatsBreak(&sGdxUnlockStage.decodeStats);
    for (i = 0; i < sampleCount; i++) {
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.decodeStats,
                               DmemGetS16(buf->dmemOut + 32u + i * 2u));
    }
}

static void GdxUnlockStageCaptureResample(int targetIndex, const GdxBufDesc* buf,
                                          uint32_t pitch, uint32_t numOut) {
    uint32_t needIn;
    uint32_t limit;
    uint32_t i;

    if ((targetIndex < 0) || (buf == NULL) || !sGdxUnlockStage.chunkStarted) {
        return;
    }
    needIn = ((numOut * pitch) >> 15) + 8u;
    GdxUnlockStageStatsBreak(&sGdxUnlockStage.resampleInputStats);
    for (i = 0; i < needIn; i++) {
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.resampleInputStats,
                               DmemGetS16(buf->dmemIn + i * 2u));
    }

    limit = numOut;
    if (limit > GDX_UNLOCK_STAGE_MAX_CHUNK) {
        limit = GDX_UNLOCK_STAGE_MAX_CHUNK;
        sGdxUnlockStage.truncatedChunks++;
    }
    for (i = 0; i < limit; i++) {
        sGdxUnlockStage.chunkResample[i] += DmemGetS16(buf->dmemOut + i * 2u);
    }
}

static void GdxUnlockStageCapturePreEnvelope(uint32_t dmemSrc, uint32_t sampleCount) {
    uint32_t limit = sampleCount;
    uint32_t i;

    if (!sGdxUnlockStage.chunkStarted || (sGdxUnlockStage.currentTarget < 0)) {
        return;
    }
    if (limit > GDX_UNLOCK_STAGE_MAX_CHUNK) {
        limit = GDX_UNLOCK_STAGE_MAX_CHUNK;
        sGdxUnlockStage.truncatedChunks++;
    }
    for (i = 0; i < limit; i++) {
        sGdxUnlockStage.chunkPreEnvelope[i] += DmemGetS16(dmemSrc + i * 2u);
    }
}

static void GdxUnlockStageCaptureEnvelopeSample(uint32_t sampleIndex, int32_t left, int32_t right) {
    if (!sGdxUnlockStage.chunkStarted || (sGdxUnlockStage.currentTarget < 0) ||
        (sampleIndex >= GDX_UNLOCK_STAGE_MAX_CHUNK)) {
        return;
    }
    sGdxUnlockStage.chunkEnvelope[0][sampleIndex] += left;
    sGdxUnlockStage.chunkEnvelope[1][sampleIndex] += right;
}

static void GdxUnlockStageFinish(void) {
    uint32_t i;
    int writeResample;
    int writePreEnvelope;
    int writeEnvelope;
    int writeMix;

    sGdxUnlockStage.complete = 1;
    writeResample = GdxUnlockStageWriteWav(GDX_UNLOCK_STAGE_RESAMPLE_PATH,
                                           sGdxUnlockStage.resample, sGdxUnlockStage.frames, 1u,
                                           sGdxUnlockStage.sampleRate);
    writePreEnvelope = GdxUnlockStageWriteWav(GDX_UNLOCK_STAGE_PRE_ENVELOPE_PATH,
                                              sGdxUnlockStage.preEnvelope, sGdxUnlockStage.frames, 1u,
                                              sGdxUnlockStage.sampleRate);
    writeEnvelope = GdxUnlockStageWriteWav(GDX_UNLOCK_STAGE_ENVELOPE_PATH,
                                           sGdxUnlockStage.envelope, sGdxUnlockStage.frames, 2u,
                                           sGdxUnlockStage.sampleRate);
    writeMix = GdxUnlockStageWriteWav(GDX_UNLOCK_STAGE_MIX_PATH,
                                      sGdxUnlockStage.mix, sGdxUnlockStage.frames, 2u,
                                      sGdxUnlockStage.sampleRate);

    GdxUnlockStageLogStats("decode", "M", &sGdxUnlockStage.decodeStats);
    GdxUnlockStageLogStats("resample-input", "M", &sGdxUnlockStage.resampleInputStats);
    GdxUnlockStageLogStats("resample", "M", &sGdxUnlockStage.resampleStats);
    GdxUnlockStageLogStats("pre-envelope", "M", &sGdxUnlockStage.preEnvelopeStats);
    GdxUnlockStageLogStats("envelope", "L", &sGdxUnlockStage.envelopeStats[0]);
    GdxUnlockStageLogStats("envelope", "R", &sGdxUnlockStage.envelopeStats[1]);
    GdxUnlockStageLogStats("mix", "L", &sGdxUnlockStage.mixStats[0]);
    GdxUnlockStageLogStats("mix", "R", &sGdxUnlockStage.mixStats[1]);
    for (i = 0; i < sGdxUnlockStage.targetCount; i++) {
        const GdxUnlockStageTarget* target = &sGdxUnlockStage.targets[i];
        gdx_port_logf("[unlock-stage] decode-boundary note=%d token=%08X calls=%u maxDelta=%u\n",
                      target->noteIndex, (unsigned)target->adpcmToken,
                      (unsigned)target->decodeCalls, (unsigned)target->decodeBoundaryMaxDelta);
    }
    gdx_port_logf("[unlock-stage] complete generation=%u executor=%s chunks=%u frames=%u "
                  "truncated=%u sumClip=resample:%u,preEnvelope:%u,envelopeL:%u,envelopeR:%u "
                  "artifacts=resample:%s,preEnvelope:%s,envelope:%s,mix:%s\n",
                  sGdxUnlockStage.generation, sGdxUnlockStage.sawHle ? "HLE" : "none",
                  (unsigned)sGdxUnlockStage.chunks, (unsigned)sGdxUnlockStage.frames,
                  (unsigned)sGdxUnlockStage.truncatedChunks,
                  (unsigned)sGdxUnlockStage.resampleClipSamples,
                  (unsigned)sGdxUnlockStage.preEnvelopeClipSamples,
                  (unsigned)sGdxUnlockStage.envelopeClipSamples[0],
                  (unsigned)sGdxUnlockStage.envelopeClipSamples[1],
                  writeResample ? "ok" : "write-failed",
                  writePreEnvelope ? "ok" : "write-failed",
                  writeEnvelope ? "ok" : "write-failed",
                  writeMix ? "ok" : "write-failed");
}

static void GdxUnlockStageAppendChunk(uint32_t dmemLeft, uint32_t dmemRight, uint32_t sampleCount) {
    uint32_t available;
    uint32_t limit;
    uint32_t i;

    if (!sGdxUnlockStage.active || sGdxUnlockStage.complete || !sGdxUnlockStage.sawHle) {
        return;
    }
    if (!sGdxUnlockStage.chunkStarted) {
        GdxUnlockStageChunkReset();
    }
    available = GDX_UNLOCK_STAGE_FRAMES - sGdxUnlockStage.frames;
    limit = sampleCount < available ? sampleCount : available;
    if (limit > GDX_UNLOCK_STAGE_MAX_CHUNK) {
        limit = GDX_UNLOCK_STAGE_MAX_CHUNK;
        sGdxUnlockStage.truncatedChunks++;
    }

    for (i = 0; i < limit; i++) {
        uint32_t frame = sGdxUnlockStage.frames + i;
        int16_t resample = GdxUnlockStageClamp(sGdxUnlockStage.chunkResample[i],
                                               &sGdxUnlockStage.resampleClipSamples);
        int16_t preEnvelope = GdxUnlockStageClamp(sGdxUnlockStage.chunkPreEnvelope[i],
                                                  &sGdxUnlockStage.preEnvelopeClipSamples);
        int16_t envelopeLeft = GdxUnlockStageClamp(sGdxUnlockStage.chunkEnvelope[0][i],
                                                   &sGdxUnlockStage.envelopeClipSamples[0]);
        int16_t envelopeRight = GdxUnlockStageClamp(sGdxUnlockStage.chunkEnvelope[1][i],
                                                    &sGdxUnlockStage.envelopeClipSamples[1]);
        int16_t mixLeft = DmemGetS16(dmemLeft + i * 2u);
        int16_t mixRight = DmemGetS16(dmemRight + i * 2u);

        sGdxUnlockStage.resample[frame] = resample;
        sGdxUnlockStage.preEnvelope[frame] = preEnvelope;
        sGdxUnlockStage.envelope[frame * 2u + 0u] = envelopeLeft;
        sGdxUnlockStage.envelope[frame * 2u + 1u] = envelopeRight;
        sGdxUnlockStage.mix[frame * 2u + 0u] = mixLeft;
        sGdxUnlockStage.mix[frame * 2u + 1u] = mixRight;
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.resampleStats, resample);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.preEnvelopeStats, preEnvelope);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.envelopeStats[0], envelopeLeft);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.envelopeStats[1], envelopeRight);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.mixStats[0], mixLeft);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.mixStats[1], mixRight);
    }
    sGdxUnlockStage.frames += limit;
    sGdxUnlockStage.chunks++;
    sGdxUnlockStage.chunkStarted = 0;
    sGdxUnlockStage.currentTarget = -1;
    if (sGdxUnlockStage.frames == GDX_UNLOCK_STAGE_FRAMES) {
        GdxUnlockStageFinish();
    }
}

// A codebook's per-predictor block is `8 * order` shorts (audio.h's AdpcmBook: "size (8 * order *
// numPredictors)"). aspMain's ADPCM decode is fixed-function order-2, so order is hardcoded to 2
// here regardless of what a book header's own `order` says.
static int16_t BookCoef(uint32_t predictorIndex, uint32_t tap /* 0 or 1 */, uint32_t col /* 0..7 */) {
    uint32_t idx = (predictorIndex * 16u) + (tap * 8u) + col;
    uint32_t byteOff = idx * 2u;
    int16_t v;
    if (byteOff + 1u >= sAdpcmBookLen) {
        return 0;
    }
    memcpy(&v, &sAdpcmBook[byteOff], sizeof(v));
    return v;
}

// A_SETLOOP's target persists for the life of the process, exactly like the ucode's own internal
// loop register: it is only ever REPLACED by a later A_SETLOOP, never cleared. synthesis.c's own
// comment claims it re-emits aSetLoop before every list that needs one -- it does not
// (synthesis.c:886-890 emits it only under `synthState->restart`), so resetting this per call left
// every later loop wrap of a sustained note with no history to read from, clicking at each wrap.
// RunAdpcm's NULL-safe fallback to the note's own state[] covers the never-set case.
static const int16_t* sPendingLoopState = NULL;
/* [pcm-cap] plumbing: identity of the most recent A_LOADBUFF source, so the
   A_ADPCM capture can attribute its decode to a sample. */
static uintptr_t sPcmCapLastSrc = 0;
static uint32_t sPcmCapLastRaw = 0;
static int sDecCapCount = 0; /* [dec-cap] per-frame VADPCM capture budget */

/* [rs-cap] diagnostic: every A_RESAMPLE call is recorded speculatively (pre/post state, full
   input and output windows) and dumped on two triggers -- an A_HILOGAIN (marks a gained note) and
   an A_CONTINUE whose pitch changed against the previous call on the same state address (a
   pitch-swept note) -- so the resampler can be replayed offline sample for sample. */
#define GDX_RSCAP_MAXSAMPS 192
static struct {
    int valid;
    uint32_t callNo;
    uint32_t flags;
    uint32_t pitch;
    uint32_t prevPitch; /* previous call's pitch for the same state token (or ==pitch if first) */
    uint32_t token;     /* raw w1 state address -- stable per-note identity */
    uint32_t dmemIn, dmemOut, count;
    int interl;         /* an A_INTERL ran since the previous resample (nParts==2 marker) */
    uint32_t adpcmRaw;  /* upstream stage identity, see the run-3 comment below */
    uint32_t adpcmFlags, adpcmIn, adpcmOut, adpcmCnt;
    int setloop;
    int16_t preSt[5], postSt[5]; /* state[0..3] history + state[4] fracQ16 */
    int16_t in[GDX_RSCAP_MAXSAMPS];
    int16_t out[GDX_RSCAP_MAXSAMPS];
    uint32_t inN, outN;
} sRsCapLast;
static uint32_t sRsCapCallNo = 0;
static int sRsCapT1 = 0;
static int sRsCapT2 = 0;
static uint32_t sRsCapLockTok = 0; /* chain mode: state token locked by first swept race call */
static int sRsCapChain = 0;
static int sRsCapInterlPending = 0;
/* Upstream identity for each chain dump: the last A_LOADBUFF source and A_ADPCM parameters that
   fed this resample. A jumping chunk address means a source-windowing bug; a clean 9-byte-frame
   advance with broken PCM means a decode-state (loop restore) bug. */
static uint32_t sRsCapAdpcmRaw = 0;   /* LOADBUFF raw src token at the last A_ADPCM */
static uint32_t sRsCapAdpcmFlags = 0;
static uint32_t sRsCapAdpcmIn = 0, sRsCapAdpcmOut = 0, sRsCapAdpcmCnt = 0;
static int sRsCapSetloopSince = 0;    /* an A_SETLOOP ran since the previous resample */
static struct { uint32_t token; uint32_t pitch; } sRsCapPrev[8];
static int sRsCapPrevN = 0;

/* [spike] stage-bisection detector: scanning at successive pipeline stages names the first
   stage that carries a stray sample. */
static int GdxSpikeScan(uint32_t dmem, uint32_t nSamples) {
    /* A stray sample has NEIGHBOURS THAT AGREE with each other while the centre sits far from
       their mean. The naive "deviates from both neighbours" test was rejected: it fires on
       legitimate near-Nyquist percussion. */
    uint32_t i;
    for (i = 1; i + 1 < nSamples; i++) {
        int32_t prev = DmemGetS16(dmem + (i - 1u) * 2u);
        int32_t cur = DmemGetS16(dmem + i * 2u);
        int32_t next = DmemGetS16(dmem + (i + 1u) * 2u);
        int32_t agree = prev - next;
        int32_t dev = cur - (prev + next) / 2;
        if (agree < 0) agree = -agree;
        if (dev < 0) dev = -dev;
        if (agree < 800 && dev > 3000) return (int)i;
    }
    return -1;
}
static int sSpikeLogsResample = 0;
static int sSpikeLogsFilter = 0;
static int sSpikeLogsInterleave = 0;

static void GdxRsCapDumpWindow(const char* pfx, const int16_t* buf, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        char line[160];
        int len = snprintf(line, sizeof(line), "[rs-cap] %s%03u", pfx, (unsigned)i);
        uint32_t j;
        for (j = 0; j < 16u && i < n && len > 0 && (size_t)len < sizeof(line) - 6u; j++, i++) {
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %04X", (unsigned)(uint16_t)buf[i]);
        }
        gdx_port_logf("%s\n", line);
    }
}

static void GdxRsCapDump(const char* tag) {
    gdx_port_logf("[rs-cap] %s call=%u tok=%08X fl=%u pitch=%04X prev=%04X in=%04X out=%04X cnt=%04X "
                  "interl=%d pre=%04X %04X %04X %04X %04X post=%04X %04X %04X %04X %04X inN=%u outN=%u\n",
                  tag, (unsigned)sRsCapLast.callNo, (unsigned)sRsCapLast.token,
                  (unsigned)sRsCapLast.flags, (unsigned)sRsCapLast.pitch, (unsigned)sRsCapLast.prevPitch,
                  (unsigned)sRsCapLast.dmemIn, (unsigned)sRsCapLast.dmemOut, (unsigned)sRsCapLast.count,
                  sRsCapLast.interl,
                  (unsigned)(uint16_t)sRsCapLast.preSt[0], (unsigned)(uint16_t)sRsCapLast.preSt[1],
                  (unsigned)(uint16_t)sRsCapLast.preSt[2], (unsigned)(uint16_t)sRsCapLast.preSt[3],
                  (unsigned)(uint16_t)sRsCapLast.preSt[4],
                  (unsigned)(uint16_t)sRsCapLast.postSt[0], (unsigned)(uint16_t)sRsCapLast.postSt[1],
                  (unsigned)(uint16_t)sRsCapLast.postSt[2], (unsigned)(uint16_t)sRsCapLast.postSt[3],
                  (unsigned)(uint16_t)sRsCapLast.postSt[4],
                  (unsigned)sRsCapLast.inN, (unsigned)sRsCapLast.outN);
    gdx_port_logf("[rs-cap] %s-up adpcmRaw=%08X adFl=%u adIn=%04X adOut=%04X adCnt=%04X setloop=%d\n",
                  tag, (unsigned)sRsCapLast.adpcmRaw, (unsigned)sRsCapLast.adpcmFlags,
                  (unsigned)sRsCapLast.adpcmIn, (unsigned)sRsCapLast.adpcmOut,
                  (unsigned)sRsCapLast.adpcmCnt, sRsCapLast.setloop);
    GdxRsCapDumpWindow("i", sRsCapLast.in, sRsCapLast.inN);
    GdxRsCapDumpWindow("o", sRsCapLast.out, sRsCapLast.outN);
}

// A_ADPCM: order-2 VADPCM decode, 16 samples per frame (1 header byte + 8 data bytes of 4-bit
// nibbles, or 4 data bytes of 2-bit nibbles for the "small ADPCM" flags|4 variant).
//
// `state` is the persistent history (NoteSynthesisBuffers.adpcmdecState[16], or a
// SynthesisReverb-embedded AdpcmLoop): state[0..15] is the FULL last output frame in temporal
// order, newest at [15]. That is not a free layout choice -- it is observable through the DMEM
// output preamble contract below.
//
// `loopState` is the most recent A_SETLOOP target. A_LOOP reads history from it instead of
// `state`, matching the ucode: SETLOOP overrides where history is READ from, but the running
// state is still always WRITTEN back to `state`.
static void RunAdpcm(const GdxBufDesc* buf, uint32_t flags, int16_t* state, const int16_t* loopState) {
    int smallAdpcm = (flags & 4) != 0;
    int isInit = (flags & 1) != 0;   /* A_INIT */
    int isLoop = (flags & 2) != 0;   /* A_LOOP */
    /* RAW (unclamped) running history, read by the predictor recursion WITHIN a 16-sample frame.
       int32_t because hardware keeps this in the wide RSP accumulator until output; an int16_t
       local would add a wraparound the real ucode does not have. */
    int32_t hist1, hist2;
    /* CLAMPED shadow, updated in lockstep from the value written to DMEM. Only this crosses a
       16-sample FRAME boundary (mupen64plus-rsp-hle audio.c#L109-127 adpcm_compute_residuals: the
       intra-frame recursion sums the RAW predict_frame output, while the history fed into the next
       frame is the clamped output). Clamping hist1 every sample instead discards a loud
       transient's mid-frame overshoot from every later prediction in that frame -- a duller attack
       than the real ucode, which loses the overshoot only at the output tap. */
    int16_t clHist1, clHist2;
    int16_t lastFrame[16]; /* persistent state: the previous call's final 16 output samples */
    uint32_t numOutSamples = buf->count / 2u;
    /* Round UP to whole 16-sample frames like the real ucode, which always decodes complete
       frames; downstream consumes only `count`. Truncating left a chunk's last 1-15 samples
       unwritten -- hundreds of micro-dropouts a second, heard as crackle over otherwise-correct
       music. */
    uint32_t numFrames = (numOutSamples + 15u) / 16u;
    uint32_t inCursor = buf->dmemIn;
    uint32_t outCursor = buf->dmemOut;
    uint32_t f;

    if (state == NULL) {
        return;
    }

    /* OUTPUT LAYOUT CONTRACT. The real op keeps its persistent state as the FULL last 16-sample
       frame (which is why adpcmdecState and AdpcmLoop.predictorState are both s16[16]), WRITES
       THAT FRAME TO THE OUTPUT BUFFER FIRST, and decodes the new frames after it -- the layout is
       [16 previous-tail samples][count/2 freshly decoded samples].
       synthesis.c's window arithmetic is built on that: skipInitialSamples=16 makes frameIndex
       start one frame PAST samplePosInt, and skipBytes indexes into the preamble to serve the
       current frame's remaining samples from the previous tick's decode. Without the preamble
       every continuing tick reads a frame ahead with a wobbling offset and a stale tail -- a full
       waveform discontinuity at every tick boundary, with bit-exact kernel math. The A_LOOP path
       seeds from the ROM-captured loop predictorState, same shape, same reason. */
    {
        uint32_t i;
        if (isInit) {
            for (i = 0; i < 16u; i++) lastFrame[i] = 0;
        } else if (isLoop && loopState != NULL) {
            for (i = 0; i < 16u; i++) lastFrame[i] = loopState[i];
        } else {
            for (i = 0; i < 16u; i++) lastFrame[i] = state[i];
        }
        for (i = 0; i < 16u; i++) {
            DmemSetS16(outCursor + i * 2u, lastFrame[i]);
        }
        outCursor += 32u; /* fresh decode starts after the preamble */
    }
    hist1 = lastFrame[15]; /* newest */
    hist2 = lastFrame[14];
    /* clHist mirrors hist at this point (both sources are already clamped s16 values). */
    clHist1 = (int16_t)hist1;
    clHist2 = (int16_t)hist2;

    {
    uint32_t decoded = 0;

    for (f = 0; f < numFrames; f++) {
        uint8_t header = DmemGetU8(inCursor);
        /* VADPCM frame header: SCALE in the high nibble, PREDICTOR in the low nibble. Swapping
           them decodes every frame with a garbage predictor row and a wrong scale (loud static).
           Verified against this ROM's title BGM: header 0x30 with a 2-predictor book can only be
           scale=3/pred=0, and that decode matches the SDK reference. */
        uint32_t shift = (header >> 4) & 0xF;
        uint32_t predIdx = header & 0xF;
        uint32_t dataBytes = smallAdpcm ? 4u : 8u;
        uint32_t frameBytes = 1u + dataBytes;
        uint32_t nibblesPerByte = smallAdpcm ? 4u : 2u; /* 2-bit vs 4-bit nibbles */
        uint32_t s;

        /* Every NEW frame's prediction starts from the CLAMPED shadow left by the previous frame
           (a no-op on f==0, where the seed above already put the same values in both). Within the
           frame body hist1/hist2 float as RAW values; only clHist1/clHist2 is clamped per sample. */
        hist1 = (int32_t)clHist1;
        hist2 = (int32_t)clHist2;

        /* [dec-cap] history entering this frame, so an offline block-form decode starts alike. */
        int16_t capPreNewer = clHist1;
        int16_t capPreOlder = clHist2;

        /* Hardware-accurate BLOCK-convolution decode: each 8-sample sub-block is computed from
           its ENTRY history using the full book columns plus the intra-block residual
           convolution, truncating ONCE per output. The sequential form below truncates (>>11)
           every sample and feeds the truncated value back, accumulating signal-correlated
           quantization noise (~-55dB on every frame -- the "film grain") that the real RSP does
           not. Standard 4-bit codec only; small-ADPCM stays on the sequential path. */
        {
            /* Read through the dev-gate layer so it is a compile-time 0 without GDX_DEV_TOOLS,
               making the sequential path unreachable in Release. The one-shot log proves which
               path the current setting actually selected. */
            const int blockAdpcm = !gdx_dev_gate(GDX_GATE_SEQ_ADPCM);
            {
                static int sAdpcmLogged = 0;
                if (!sAdpcmLogged) {
                    sAdpcmLogged = 1;
                    gdx_port_logf("[audio] VADPCM decode = %s\n",
                                  blockAdpcm ? "block-convolution (hardware-correct)" : "sequential (GDX_SEQ_ADPCM=1)");
                }
            }
            if (blockAdpcm && !smallAdpcm) {
                int32_t eH2 = (int32_t)clHist2; /* sub-block entry history: older */
                int32_t eH1 = (int32_t)clHist1; /* newer */
                uint32_t sub;
                for (sub = 0; sub < 2u; sub++) {
                    int32_t e[8];
                    uint32_t i, k;
                    for (i = 0; i < 8u; i++) {
                        uint32_t si = sub * 8u + i;
                        uint8_t bv = DmemGetU8(inCursor + 1u + (si / 2u));
                        int32_t nib = (si % 2u == 0u) ? ((bv >> 4) & 0xF) : (bv & 0xF);
                        if (nib & 0x8) nib -= 16;
                        e[i] = nib << shift;
                    }
                    int16_t sblk[8];
                    for (i = 0; i < 8u; i++) {
                        int64_t acc = (int64_t)BookCoef(predIdx, 0, i) * (int64_t)eH2 +
                                      (int64_t)BookCoef(predIdx, 1, i) * (int64_t)eH1;
                        for (k = 0; k < i; k++) {
                            acc += (int64_t)BookCoef(predIdx, 1, i - 1u - k) * (int64_t)e[k];
                        }
                        acc += (int64_t)e[i] << 11;
                        sblk[i] = ClampS16((int32_t)(acc >> 11));
                        DmemSetS16(outCursor + (sub * 8u + i) * 2u, sblk[i]);
                        decoded++;
                    }
                    eH2 = sblk[6];
                    eH1 = sblk[7];
                }
                clHist2 = (int16_t)eH2;
                clHist1 = (int16_t)eH1;
                goto frame_done;
            }
        }

        for (s = 0; s < 16u; s++) {
            int32_t nibble;
            int32_t residual;
            int32_t predicted;
            int32_t sampleOut;
            uint8_t byteVal = DmemGetU8(inCursor + 1u + (s / nibblesPerByte));

            if (smallAdpcm) {
                uint32_t shiftInByte = (3u - (s % 4u)) * 2u;
                nibble = (int32_t)((byteVal >> shiftInByte) & 0x3u);
                if (nibble & 0x2) nibble -= 4; /* sign-extend 2-bit */
            } else {
                uint32_t shiftInByte = (s % 2u == 0u) ? 4u : 0u;
                nibble = (int32_t)((byteVal >> shiftInByte) & 0xFu);
                if (nibble & 0x8) nibble -= 16; /* sign-extend 4-bit */
            }

            residual = nibble << shift;
            /* Sequential per-sample VADPCM needs only COLUMN 0 of the codebook: with a true
               two-sample history, row 0 weights the OLDER sample and row 1 the NEWER (SDK vadpcm
               decodeframe, at j==0 the intra-block residual sum is empty, so the per-sample form
               is exact). Walking the matrix columns while ALSO sliding history each sample
               double-counts the feedback -- those columns already encode the recursion. */
            /* int64 accumulator: two s16*s16 products can each reach 2^30 and their sum 2^31, one
               bit past INT32_MAX, when both coefficients and both history samples sit at -32768.
               In int32 that is overflow UB, and in practice it flips the sign -- a
               polarity-inverted spike instead of a clean saturate. Hardware accumulates this in a
               wide vector accumulator; after the >>11 the result is small again, so narrowing to
               int32_t there is safe. */
            {
                int64_t predAcc = (int64_t)BookCoef(predIdx, 0, 0) * (int64_t)hist2 +
                                   (int64_t)BookCoef(predIdx, 1, 0) * (int64_t)hist1;
                predicted = (int32_t)(predAcc >> 11);
            }
            sampleOut = predicted + residual;

            {
                /* Deferred clamping: DMEM gets the clamped sample, but hist1/hist2 -- this
                   frame's remaining predictions -- carry the RAW value. Only clHist1/clHist2,
                   used at the next frame boundary and for the state write-back, is clamped. */
                int16_t clampedOut = ClampS16(sampleOut);
                DmemSetS16(outCursor + s * 2u, clampedOut);

                hist2 = hist1;
                hist1 = sampleOut; /* RAW carry, intra-frame only */

                clHist2 = clHist1;
                clHist1 = clampedOut; /* CLAMPED shadow, carries across frames */
            }

            decoded++;
        }

        /* [dec-cap] per-frame capture: everything an offline decoder needs to reproduce this
           exact frame and diff it against the port's output. Race-gated, capped, 4-bit only. */
        {
            extern int gGdxRaceActive;
            if (gGdxRaceActive && !smallAdpcm && sDecCapCount < 12) {
                char dbuf[64], bbuf[160], obuf[160];
                int dp = 0, bp = 0, op = 0;
                uint32_t z;
                sDecCapCount++;
                for (z = 0; z < 8u; z++) {
                    dp += snprintf(dbuf + dp, sizeof(dbuf) - (size_t)dp, "%02X",
                                   DmemGetU8(inCursor + 1u + z));
                }
                for (z = 0; z < 8u; z++) {
                    bp += snprintf(bbuf + bp, sizeof(bbuf) - (size_t)bp, "%04X %04X ",
                                   (uint16_t)BookCoef(predIdx, 0, z), (uint16_t)BookCoef(predIdx, 1, z));
                }
                for (z = 0; z < 16u; z++) {
                    op += snprintf(obuf + op, sizeof(obuf) - (size_t)op, "%04X ",
                                   (uint16_t)DmemGetS16(outCursor + z * 2u));
                }
                gdx_port_logf("[dec-cap] raw=%08X pred=%u shift=%u preOlder=%04X preNewer=%04X\n",
                              sPcmCapLastRaw, (unsigned)predIdx, (unsigned)shift,
                              (uint16_t)capPreOlder, (uint16_t)capPreNewer);
                gdx_port_logf("[dec-cap]  data=%s\n", dbuf);
                gdx_port_logf("[dec-cap]  book=%s\n", bbuf);
                gdx_port_logf("[dec-cap]  out=%s\n", obuf);
            }
        }

    frame_done: /* the block-convolution path rejoins here */
        inCursor += frameBytes;
        outCursor += 32u;
    }
    (void)decoded;

    /* Persist the 16 samples ending at the chunk's TRUE boundary (count), not the scratch tail
       of the final rounded-up frame. The DMEM stream is [16-sample preamble][numOutSamples
       fresh], so index numOutSamples+i is exactly the last 16 true samples; at numOutSamples == 0
       it re-reads the unchanged preamble and state passes through untouched. */
    {
        uint32_t i;
        for (i = 0; i < 16u; i++) {
            state[i] = DmemGetS16(buf->dmemOut + (numOutSamples + i) * 2u);
        }
    }
    }
}

// A_S8DEC: signed 8-bit PCM -> 16-bit. The codec is non-predictive, but the OUTPUT LAYOUT is
// A_ADPCM's (see RunAdpcm's contract): synthesis.c uses skipInitialSamples=16 for CODEC_S8 too,
// so the 16-sample state preamble precedes the fresh samples and the last 16 outputs persist.
static void RunS8Dec(const GdxBufDesc* buf, uint32_t flags, int16_t* state) {
    int isInit = (flags & 1) != 0;
    uint32_t numSamples = buf->count / 2u;
    uint32_t i;
    if (state == NULL) {
        return;
    }
    for (i = 0; i < 16u; i++) {
        DmemSetS16(buf->dmemOut + i * 2u, isInit ? 0 : state[i]);
    }
    for (i = 0; i < numSamples; i++) {
        int8_t raw = (int8_t)DmemGetU8(buf->dmemIn + i);
        DmemSetS16(buf->dmemOut + 32u + i * 2u, (int16_t)((int32_t)raw * 256));
    }
    for (i = 0; i < 16u; i++) {
        state[i] = DmemGetS16(buf->dmemOut + (numSamples + i) * 2u);
    }
}

// A_RESAMPLE 4-tap polyphase FIR table. The real op is NOT linear interpolation: it convolves 4
// neighbouring source samples against a fixed 64-phase table of 4-tap Q15 coefficient sets, one
// per fractional sub-position between two source samples.
//
// These are the ROM-baked constants, transcribed from the canonical RSP audio-HLE resample LUT
// (mupen64plus-rsp-hle src/alist.c `RESAMPLE_LUT`, byte-identical in Project64/AziAudio's
// Mupen64plusHLE/audio.c). Those projects extracted the values from the microcode's own data
// section, so this is the table hardware convolves, not an approximation -- it replaced an earlier
// runtime-generated windowed-sinc table that was audibly close but not bit-exact.
//
// Layout: for phase p = fracQ16 >> 10, sResampleTable[p][0..3] weight the source samples
// {x[-1], x[0], x[+1], x[+2]} -- tap 0 is the persisted one-sample history, taps 2/3 the
// two-sample lookahead. Identical to mupen's `src[0..3] * lut[0..3]`.
//
// Phase 0 is {0x0c39,0x66ad,0x0d46,0xffdf}, NOT an identity {0,0x8000,0,0}: the real resampler
// band-limits even at unity pitch (0x8000), so integer-ratio playback is not a bit-exact
// passthrough on hardware. That mild softness is authentic, not a defect to "simplify" away.
#define GDX_RESAMPLE_PHASE_BITS 6
#define GDX_RESAMPLE_PHASES (1u << GDX_RESAMPLE_PHASE_BITS) /* 64 */
#define GDX_S16(x) ((int16_t)(x)) /* sign-truncate a 16-bit hex constant, MSVC /W3-clean */
static const int16_t sResampleTable[GDX_RESAMPLE_PHASES][4] = {
    { GDX_S16(0x0c39), GDX_S16(0x66ad), GDX_S16(0x0d46), GDX_S16(0xffdf) },
    { GDX_S16(0x0b39), GDX_S16(0x6696), GDX_S16(0x0e5f), GDX_S16(0xffd8) },
    { GDX_S16(0x0a44), GDX_S16(0x6669), GDX_S16(0x0f83), GDX_S16(0xffd0) },
    { GDX_S16(0x095a), GDX_S16(0x6626), GDX_S16(0x10b4), GDX_S16(0xffc8) },
    { GDX_S16(0x087d), GDX_S16(0x65cd), GDX_S16(0x11f0), GDX_S16(0xffbf) },
    { GDX_S16(0x07ab), GDX_S16(0x655e), GDX_S16(0x1338), GDX_S16(0xffb6) },
    { GDX_S16(0x06e4), GDX_S16(0x64d9), GDX_S16(0x148c), GDX_S16(0xffac) },
    { GDX_S16(0x0628), GDX_S16(0x643f), GDX_S16(0x15eb), GDX_S16(0xffa1) },
    { GDX_S16(0x0577), GDX_S16(0x638f), GDX_S16(0x1756), GDX_S16(0xff96) },
    { GDX_S16(0x04d1), GDX_S16(0x62cb), GDX_S16(0x18cb), GDX_S16(0xff8a) },
    { GDX_S16(0x0435), GDX_S16(0x61f3), GDX_S16(0x1a4c), GDX_S16(0xff7e) },
    { GDX_S16(0x03a4), GDX_S16(0x6106), GDX_S16(0x1bd7), GDX_S16(0xff71) },
    { GDX_S16(0x031c), GDX_S16(0x6007), GDX_S16(0x1d6c), GDX_S16(0xff64) },
    { GDX_S16(0x029f), GDX_S16(0x5ef5), GDX_S16(0x1f0b), GDX_S16(0xff56) },
    { GDX_S16(0x022a), GDX_S16(0x5dd0), GDX_S16(0x20b3), GDX_S16(0xff48) },
    { GDX_S16(0x01be), GDX_S16(0x5c9a), GDX_S16(0x2264), GDX_S16(0xff3a) },
    { GDX_S16(0x015b), GDX_S16(0x5b53), GDX_S16(0x241e), GDX_S16(0xff2c) },
    { GDX_S16(0x0101), GDX_S16(0x59fc), GDX_S16(0x25e0), GDX_S16(0xff1e) },
    { GDX_S16(0x00ae), GDX_S16(0x5896), GDX_S16(0x27a9), GDX_S16(0xff10) },
    { GDX_S16(0x0063), GDX_S16(0x5720), GDX_S16(0x297a), GDX_S16(0xff02) },
    { GDX_S16(0x001f), GDX_S16(0x559d), GDX_S16(0x2b50), GDX_S16(0xfef4) },
    { GDX_S16(0xffe2), GDX_S16(0x540d), GDX_S16(0x2d2c), GDX_S16(0xfee8) },
    { GDX_S16(0xffac), GDX_S16(0x5270), GDX_S16(0x2f0d), GDX_S16(0xfedb) },
    { GDX_S16(0xff7c), GDX_S16(0x50c7), GDX_S16(0x30f3), GDX_S16(0xfed0) },
    { GDX_S16(0xff53), GDX_S16(0x4f14), GDX_S16(0x32dc), GDX_S16(0xfec6) },
    { GDX_S16(0xff2e), GDX_S16(0x4d57), GDX_S16(0x34c8), GDX_S16(0xfebd) },
    { GDX_S16(0xff0f), GDX_S16(0x4b91), GDX_S16(0x36b6), GDX_S16(0xfeb6) },
    { GDX_S16(0xfef5), GDX_S16(0x49c2), GDX_S16(0x38a5), GDX_S16(0xfeb0) },
    { GDX_S16(0xfedf), GDX_S16(0x47ed), GDX_S16(0x3a95), GDX_S16(0xfeac) },
    { GDX_S16(0xfece), GDX_S16(0x4611), GDX_S16(0x3c85), GDX_S16(0xfeab) },
    { GDX_S16(0xfec0), GDX_S16(0x4430), GDX_S16(0x3e74), GDX_S16(0xfeac) },
    { GDX_S16(0xfeb6), GDX_S16(0x424a), GDX_S16(0x4060), GDX_S16(0xfeaf) },
    { GDX_S16(0xfeaf), GDX_S16(0x4060), GDX_S16(0x424a), GDX_S16(0xfeb6) },
    { GDX_S16(0xfeac), GDX_S16(0x3e74), GDX_S16(0x4430), GDX_S16(0xfec0) },
    { GDX_S16(0xfeab), GDX_S16(0x3c85), GDX_S16(0x4611), GDX_S16(0xfece) },
    { GDX_S16(0xfeac), GDX_S16(0x3a95), GDX_S16(0x47ed), GDX_S16(0xfedf) },
    { GDX_S16(0xfeb0), GDX_S16(0x38a5), GDX_S16(0x49c2), GDX_S16(0xfef5) },
    { GDX_S16(0xfeb6), GDX_S16(0x36b6), GDX_S16(0x4b91), GDX_S16(0xff0f) },
    { GDX_S16(0xfebd), GDX_S16(0x34c8), GDX_S16(0x4d57), GDX_S16(0xff2e) },
    { GDX_S16(0xfec6), GDX_S16(0x32dc), GDX_S16(0x4f14), GDX_S16(0xff53) },
    { GDX_S16(0xfed0), GDX_S16(0x30f3), GDX_S16(0x50c7), GDX_S16(0xff7c) },
    { GDX_S16(0xfedb), GDX_S16(0x2f0d), GDX_S16(0x5270), GDX_S16(0xffac) },
    { GDX_S16(0xfee8), GDX_S16(0x2d2c), GDX_S16(0x540d), GDX_S16(0xffe2) },
    { GDX_S16(0xfef4), GDX_S16(0x2b50), GDX_S16(0x559d), GDX_S16(0x001f) },
    { GDX_S16(0xff02), GDX_S16(0x297a), GDX_S16(0x5720), GDX_S16(0x0063) },
    { GDX_S16(0xff10), GDX_S16(0x27a9), GDX_S16(0x5896), GDX_S16(0x00ae) },
    { GDX_S16(0xff1e), GDX_S16(0x25e0), GDX_S16(0x59fc), GDX_S16(0x0101) },
    { GDX_S16(0xff2c), GDX_S16(0x241e), GDX_S16(0x5b53), GDX_S16(0x015b) },
    { GDX_S16(0xff3a), GDX_S16(0x2264), GDX_S16(0x5c9a), GDX_S16(0x01be) },
    { GDX_S16(0xff48), GDX_S16(0x20b3), GDX_S16(0x5dd0), GDX_S16(0x022a) },
    { GDX_S16(0xff56), GDX_S16(0x1f0b), GDX_S16(0x5ef5), GDX_S16(0x029f) },
    { GDX_S16(0xff64), GDX_S16(0x1d6c), GDX_S16(0x6007), GDX_S16(0x031c) },
    { GDX_S16(0xff71), GDX_S16(0x1bd7), GDX_S16(0x6106), GDX_S16(0x03a4) },
    { GDX_S16(0xff7e), GDX_S16(0x1a4c), GDX_S16(0x61f3), GDX_S16(0x0435) },
    { GDX_S16(0xff8a), GDX_S16(0x18cb), GDX_S16(0x62cb), GDX_S16(0x04d1) },
    { GDX_S16(0xff96), GDX_S16(0x1756), GDX_S16(0x638f), GDX_S16(0x0577) },
    { GDX_S16(0xffa1), GDX_S16(0x15eb), GDX_S16(0x643f), GDX_S16(0x0628) },
    { GDX_S16(0xffac), GDX_S16(0x148c), GDX_S16(0x64d9), GDX_S16(0x06e4) },
    { GDX_S16(0xffb6), GDX_S16(0x1338), GDX_S16(0x655e), GDX_S16(0x07ab) },
    { GDX_S16(0xffbf), GDX_S16(0x11f0), GDX_S16(0x65cd), GDX_S16(0x087d) },
    { GDX_S16(0xffc8), GDX_S16(0x10b4), GDX_S16(0x6626), GDX_S16(0x095a) },
    { GDX_S16(0xffd0), GDX_S16(0x0f83), GDX_S16(0x6669), GDX_S16(0x0a44) },
    { GDX_S16(0xffd8), GDX_S16(0x0e5f), GDX_S16(0x6696), GDX_S16(0x0b39) },
    { GDX_S16(0xffdf), GDX_S16(0x0d46), GDX_S16(0x66ad), GDX_S16(0x0c39) },
};

// A_RESAMPLE: 4-tap polyphase FIR resampler (table above). pitch is Q15, UNITY_PITCH=0x8000 ==
// 1.0x per PR/abi.h.
//
// State layout (16 shorts, this file's own convention -- decomp C never reads this buffer):
// state[0..3] = the 4 source samples immediately BEFORE this call's dmemIn[0], oldest at [0],
// tap -1 at [3]; state[4] = fractional position (Q16, low 16 bits only); state[15] = pitch,
// stashed by the caller (see the A_RESAMPLE case in the dispatch loop). The 4-sample window
// mirrors mupen64plus-rsp-hle's alist_resample_load/save, whose read cursor starts 4 whole samples
// before dmemi; taps 0/+1/+2 still read straight out of the current call's buffer, which
// synthesis.c's trailing SAMPLES_PER_FRAME padding makes safe.
//
// A_INIT ZERO-primes that window and the accumulator, as alist_resample_reset does; it does NOT
// seed from the incoming buffer. A freshly triggered note therefore ramps in from silence instead
// of smearing its first real sample backward in time.
static int16_t GdxResampleSampleAt(const GdxBufDesc* buf, const int16_t hist[4], uint32_t virtIdx) {
    /* virtIdx 0..3 index the persisted pre-roll; >= 4 index this call's own DMEM buffer, with 4
       aliasing dmemIn[0] -- the reference's `ipos = (dmemi>>1) - 4` starting offset. */
    if (virtIdx < 4u) {
        return hist[virtIdx];
    }
    return DmemGetS16(buf->dmemIn + (virtIdx - 4u) * 2u);
}

static void RunResample(const GdxBufDesc* buf, uint32_t flags, int16_t* state) {
    int isInit = (flags & 1) != 0; /* A_INIT */
    uint32_t pitchQ15;
    uint32_t fracQ16;
    int16_t hist[4];
    /* Byte count rounded UP to a whole 16-byte (8-sample) granule, mirroring mupen's RESAMPLE
       handler (`(count + 0xf) & ~0xf`) and RunAdpcm's frame rounding: always finish the whole
       processing granule. This can write a few trailing samples past the caller's nominal count,
       always real in-window FIR output, never garbage. */
    uint32_t roundedByteCount = (buf->count + 0xFu) & ~0xFu;
    uint32_t numOutSamples = roundedByteCount / 2u;
    uint32_t virtIdx = 4u; /* logical cursor; 4..(4+n) walk this call's own buffer, see above */
    uint32_t n;

    if (state == NULL) {
        return;
    }

    /* The caller stashes pitch into state[15]; see the A_RESAMPLE case in the dispatch loop. */
    pitchQ15 = (uint32_t)(uint16_t)state[15];

    if (isInit) {
        hist[0] = hist[1] = hist[2] = hist[3] = 0; /* zero-prime, see file comment above */
        fracQ16 = 0;
    } else {
        hist[0] = state[0];
        hist[1] = state[1];
        hist[2] = state[2];
        hist[3] = state[3];
        fracQ16 = ((uint32_t)(uint16_t)state[4]);
    }

    for (n = 0; n < numOutSamples; n++) {
        int16_t xm1 = GdxResampleSampleAt(buf, hist, virtIdx - 1u);
        int16_t x0 = GdxResampleSampleAt(buf, hist, virtIdx);
        int16_t x1 = GdxResampleSampleAt(buf, hist, virtIdx + 1u);
        int16_t x2 = GdxResampleSampleAt(buf, hist, virtIdx + 2u);
        uint32_t phase = (fracQ16 >> (16u - GDX_RESAMPLE_PHASE_BITS)) & (GDX_RESAMPLE_PHASES - 1u);
        const int16_t* c = sResampleTable[phase];
        int32_t out = ((int32_t)xm1 * c[0] + (int32_t)x0 * c[1] +
                       (int32_t)x1 * c[2] + (int32_t)x2 * c[3]) >> 15;
        DmemSetS16(buf->dmemOut + n * 2u, ClampS16(out));

        fracQ16 += (pitchQ15 << 1); /* Q15 -> Q16 */
        while (fracQ16 >= 0x10000u) {
            fracQ16 -= 0x10000u;
            virtIdx++;
        }
    }

    /* Persist the 4-sample window ending at this call's final cursor: the next call's pre-roll
       history, mirroring alist_resample_save. */
    state[0] = GdxResampleSampleAt(buf, hist, virtIdx - 4u);
    state[1] = GdxResampleSampleAt(buf, hist, virtIdx - 3u);
    state[2] = GdxResampleSampleAt(buf, hist, virtIdx - 2u);
    state[3] = GdxResampleSampleAt(buf, hist, virtIdx - 1u);
    state[4] = (int16_t)(uint16_t)fracQ16;
}

// A_FILTER is a TWO-STEP protocol, like A_LOADADPCM+A_ADPCM or A_SETLOOP+A_ADPCM
// (AudioSynth_FilterReverb / LoadFilterSize+LoadFilterBuffer always emit the pair back to back):
//   1) f==2 "prime": w1 = coefficient table (8 Q15 coefficients), countOrBuf = the byte size the
//      next call will filter.
//   2) f==A_INIT/A_CONTINUE "apply": countOrBuf = DMEM buffer, w1 = state pointer. Filter that
//      buffer in place for the primed size, carrying history through state.
//
// Reimplemented from alist_filter's documented BEHAVIOR (mupen64plus-rsp-hle alist.c#L794-907);
// its literal per-lane index arithmetic is that project's own expression of the RSP's vector-lane
// shuffles and is deliberately not reproduced here (mupen is GPLv2).
//
// The 8 primed coefficients ARE the FIR, applied directly. There is NO per-call averaging of two
// LUTs: averaging a carried LUT with the fresh coefficients halves gLowPassFilterData's identity
// row {0,0,0,32767,0,0,0,0} to -6 dB on the very first apply and makes the filter's gain depend on
// how many consecutive A_CONTINUE calls have run, breaking the "cutoff 0 == unity passthrough"
// case AudioHeap_LoadFilter relies on. The nead FILTER's "second table" is the delay-line state,
// not a second coefficient set.
//
// Per 8-sample BLOCK, the 8 outputs come from a 16-sample WINDOW = [previous block's 8-sample
// tail] ++ [this block's 8 samples], the tap window sliding one sample at a time
// (out[k] = sum_t coef[t]*win[k+t]). The previous call's final 8 INPUT samples become the next
// call's tail, so the FIR stays continuous across command lists; A_INIT zeroes it.
//
// The coefficient table is host-order and MUST NOT be byte-swapped here. It originates as the
// compile-time s16 array gLowPassFilterData[] (disk/lib/filter_data.c), is copied element by
// element into reverb->filterLeft/Right by AudioHeap_LoadLowPassFilter, and arrives here as a host
// pointer -- the endianness swap lives in the load pipeline's gdx_rd_s16, never in this
// interpreter.
//
// state[0..7] = the previous call's final 8-sample INPUT tail, oldest at [0]; the remaining 24
// shorts are unused. The real filterLeftState/filterRightState buffers are 32 shorts and never
// read by decomp C, so, like adpcmdecState, the layout is this file's choice.
static void RunFilter(uint32_t dmemBuf, uint32_t sizeBytes, uint32_t flags, int16_t* state,
                      const int16_t* coef) {
    uint32_t numSamples = sizeBytes / 2u;
    uint32_t numBlocks = numSamples / 8u; /* real ucode always operates on whole 8-sample blocks */
    int isInit = (flags & 1) != 0; /* A_INIT */
    int16_t tail[8];
    uint32_t blk;
    int t;

    /* This low-pass is the only thing bleeding high-frequency energy out of the engine-echo
       reverb feedback loop; with it off, that loop slowly diverges into progressive race static.
       The gate is normalized to "disable the filter" (0 = stock = filter on) so it can compile
       out of Release without silencing the FIR. */
    if (gdx_dev_gate(GDX_GATE_NO_HLE_FILTER)) {
        return;
    }

    if (state == NULL || coef == NULL || numSamples == 0u) {
        /* No coefficients/state resolved yet (e.g. the priming call was skipped due to an
           unresolved address) -- leave DMEM untouched rather than filter with garbage. */
        return;
    }

    if (isInit) {
        for (t = 0; t < 8; t++) tail[t] = 0;
    } else {
        for (t = 0; t < 8; t++) tail[t] = state[t];
    }

    for (blk = 0; blk < numBlocks; blk++) {
        uint32_t base = dmemBuf + blk * 16u;
        int16_t cur[8];
        int16_t win[16]; /* win[0..7] = previous tail, win[8..15] = this block's input */
        int16_t outBlk[8];
        int k;

        /* Read the block's INPUT before any write-back: output overwrites DMEM in place, but the
           tail carried forward must be this block's input samples, not its output. */
        for (t = 0; t < 8; t++) {
            cur[t] = DmemGetS16(base + (uint32_t)t * 2u);
        }
        for (t = 0; t < 8; t++) { win[t] = tail[t]; win[8 + t] = cur[t]; }

        for (k = 0; k < 8; k++) {
            int32_t acc = 0;
            for (t = 0; t < 8; t++) {
                acc += (int32_t)coef[t] * (int32_t)win[k + t];
            }
            /* Half-LSB bias before the Q15 reduction: round to nearest, not truncate. */
            outBlk[k] = ClampS16((acc + 0x4000) >> 15);
        }

        for (k = 0; k < 8; k++) {
            DmemSetS16(base + (uint32_t)k * 2u, outBlk[k]);
        }

        for (t = 0; t < 8; t++) tail[t] = cur[t]; /* this block's input becomes the next block's tail */
    }

    for (t = 0; t < 8; t++) state[t] = tail[t];
}

// Entry point, called from port/n64_sched.c's osSpTaskStartGo for an M_AUDTASK. dataPtr and
// dataSizeBytes come straight off the OSTask (PR/sptask.h: `u64* data_ptr;`), so they are
// full-width host values -- the truncation hazard above applies only inside the command payload.
/* M1-RACE-FREEZE watchdog progress markers (read by port/3ds/main_3ds.cpp's watchdog thread
   from another OS thread; volatile progress counters, staleness is harmless). If the audio
   thread wedges INSIDE one command, op/idx stop advancing while runs stays flat — naming the
   exact ABI op. Two volatile stores per command; negligible against the DMEM work. */
volatile uint32_t gdx_watch_hle_runs = 0;
volatile uint32_t gdx_watch_hle_op = 0xFF;
volatile uint32_t gdx_watch_hle_idx = 0;

/* [audio-hle] receipts: name the first zero link inside the DSP stage. A ~27-command list
   is the empty mix (clear/reverb/interleave/save, no voices); ADPCM/ENVMIXER counts are the
   per-tick voice work. First nonzero SAVEBUFF output is logged one-shot with its run index;
   a periodic summary keeps the trail alive either way. */
static uint32_t sGdxHleAdpcmOps;
static uint32_t sGdxHleEnvmixOps;
static uint32_t sGdxHleNzSaves;       /* SAVEBUFFs that wrote at least one nonzero byte */
static int sGdxHleFirstNzLogged;

void gdx_audio_hle_run(const void* dataPtr, unsigned int dataSizeBytes) {
    const GdxAcmd* cmds = (const GdxAcmd*)dataPtr;
    uint32_t count = dataSizeBytes / (uint32_t)sizeof(GdxAcmd);
    uint32_t i;

    /* Pending A_SETBUFF descriptor, consumed by the following A_ADPCM/A_S8DEC/A_RESAMPLE. */
    GdxBufDesc pendingBuf = { 0, 0, 0 };

    /* Pending A_FILTER prime (f==2), consumed by the following apply. Safe to reset every call:
       AudioSynth_FilterReverb always emits the prime+apply pair back to back. */
    int16_t pendingFilterCoef[8] = { 0 };
    uint32_t pendingFilterSizeBytes = 0;
    int pendingFilterHaveCoef = 0;

    /* A_ENVSETUP1/2 state, consumed by the following A_ENVMIXER (the RSP's internal envelope
       registers). */
    int32_t envRampReverb = 0, envRampLeft = 0, envRampRight = 0;
    int32_t envCurVolLeft = 0, envCurVolRight = 0, envReverbVol2 = 0;

    static int sUnhandledLogs = 0;

    if (cmds == NULL || count == 0) {
        return;
    }

    if (sGdxUnlockStage.active &&
        (sGdxUnlockStage.commandList != (uintptr_t)dataPtr)) {
        sGdxUnlockStage.rangeCount = 0;
    }

    if (sGdxUnlockStage.active && !sGdxUnlockStage.complete && !sGdxUnlockStage.sawHle) {
        sGdxUnlockStage.sawHle = 1;
        gdx_port_logf("[unlock-stage] begin generation=%u executor=HLE targets=%u\n",
                      sGdxUnlockStage.generation, (unsigned)sGdxUnlockStage.targetCount);
    }

    gdx_watch_hle_runs++;

    for (i = 0; i < count; i++) {
        uint32_t w0 = cmds[i].w0;
        uint32_t w1 = cmds[i].w1;
        uint32_t op = (w0 >> 24) & 0xFFu;
        sDmemCurOp = (uint8_t)op; /* [spike] last-writer attribution */
        gdx_watch_hle_op = op;    /* watchdog: last op entered */
        gdx_watch_hle_idx = i;

        switch (op) {
            case GDX_A_SPNOOP:
                break;

            case GDX_A_CLEARBUFF: {
                uint32_t dmem = w0 & 0xFFFFFFu; /* aClearBuffer packs dmem into the low 24 bits */
                uint32_t size = w1;
                uint32_t k;
                /* Defensive clamp: size is a raw 32-bit word off the command list. DMEM is 4 KB
                   and all writes wrap (DmemSetU8 masks), so anything past GDX_DMEM_SIZE is
                   meaningless re-zeroing — but a corrupt list (size ~0xFFFFFFFF) would spin the
                   audio thread for minutes inside this loop while it holds sAudioCtxMutex,
                   freezing the game thread at its next AudioThread_QueueCmd. */
                if (size > GDX_DMEM_SIZE) {
                    size = GDX_DMEM_SIZE;
                }
                if (sGdxUnlockStage.active && !sGdxUnlockStage.complete &&
                    (dmem == 0x940u) && (size >= 0x340u)) {
                    GdxUnlockStageChunkReset();
                }
                for (k = 0; k < size; k++) {
                    DmemSetU8(dmem + k, 0);
                }
                break;
            }

            case GDX_A_SETBUFF: {
                uint32_t flags = (w0 >> 16) & 0xFFu; /* unused by any consumer here */
                uint32_t dmemIn = w0 & 0xFFFFu;
                uint32_t dmemOut = (w1 >> 16) & 0xFFFFu;
                uint32_t cnt = w1 & 0xFFFFu;
                (void)flags;
                pendingBuf.dmemIn = dmemIn;
                pendingBuf.dmemOut = dmemOut;
                pendingBuf.count = cnt;
                break;
            }

            case GDX_A_DMEMMOVE: {
                uint32_t dmemIn = w0 & 0xFFFFFFu;
                uint32_t dmemOut = (w1 >> 16) & 0xFFFFu;
                uint32_t cnt = w1 & 0xFFFFu;
                uint32_t k;
                /* Byte-by-byte to safely handle overlap in either direction without relying on
                   memmove's overlap semantics against a masked/wrapping index space. */
                if (dmemOut <= dmemIn) {
                    for (k = 0; k < cnt; k++) DmemSetU8(dmemOut + k, DmemGetU8(dmemIn + k));
                } else {
                    for (k = cnt; k > 0; k--) DmemSetU8(dmemOut + k - 1u, DmemGetU8(dmemIn + k - 1u));
                }
                break;
            }

            case GDX_A_LOADBUFF: {
                uint32_t size = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t dmemDest = w0 & 0xFFFFu;
                void* src = GdxAudioResolveAddr(w1, "LOADBUFF");
                if (src != NULL) {
                    uint32_t k;
                    const uint8_t* s = (const uint8_t*)src;
                    for (k = 0; k < size; k++) DmemSetU8(dmemDest + k, s[k]);
                    /* [pcm-cap] plumbing: remember the last compressed-sample
                       source so the ADPCM capture below can name its input. */
                    sPcmCapLastSrc = (uintptr_t)src;
                    sPcmCapLastRaw = w1;
                    /* [spike] a spike in the LOADED region means the corruption arrived from
                       RDRAM, i.e. it was written on a previous tick's save side. The 0x20 floor
                       is the smallest window a strict-spike scan can mean anything on (reverb
                       ring wraps split a tick into pieces as small as 0x10); the dmemDest gate
                       skips compressed ADPCM chunk loads, which land below 0x580. */
                    if (size >= 0x20u && dmemDest >= 0xC80u && gdx_diag_audio_enabled()) {
                        static int sSpikeLogsLoadbuff = 0;
                        if (sSpikeLogsLoadbuff < 16) {
                            int si = GdxSpikeScan(dmemDest, size / 2u);
                            if (si >= 0) {
                                sSpikeLogsLoadbuff++;
                                gdx_port_logf("[spike] loadbuff at=%d dmem=%04X size=%04X raw=%08X\n",
                                              si, dmemDest, size, w1);
                            }
                        }
                    }
                }
                break;
            }

            case GDX_A_SAVEBUFF: {
                uint32_t size = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t dmemSrc = w0 & 0xFFFFu;
                void* dst = GdxAudioResolveAddr(w1, "SAVEBUFF");
                if (dst != NULL) {
                    uint32_t k;
                    uint8_t* d = (uint8_t*)dst;
                    uint8_t nzAcc = 0;
                    for (k = 0; k < size; k++) {
                        d[k] = DmemGetU8(dmemSrc + k);
                        nzAcc |= d[k];
                    }
                    /* [audio-hle] receipt: first nonzero PCM leaving the DSP stage. */
                    if (nzAcc != 0) {
                        sGdxHleNzSaves++;
                        if (!sGdxHleFirstNzLogged && gdx_diag_audio_enabled()) {
                            sGdxHleFirstNzLogged = 1;
                            gdx_port_logf("[audio-hle] FIRST NONZERO output: run=%u cmd=%u dmem=%04X "
                                          "size=%u dst=%08X\n",
                                          (unsigned)gdx_watch_hle_runs, (unsigned)i, dmemSrc,
                                          (unsigned)size, (unsigned)w1);
                        }
                    }
                }
                break;
            }

            case GDX_A_LOADADPCM: {
                uint32_t byteCount = w0 & 0xFFFFFFu;
                void* src = GdxAudioResolveAddr(w1, "LOADADPCM");
                if (byteCount > GDX_ADPCM_BOOK_MAX_BYTES) {
                    byteCount = GDX_ADPCM_BOOK_MAX_BYTES;
                }
                if (src != NULL) {
                    memcpy(sAdpcmBook, src, byteCount);
                    sAdpcmBookLen = byteCount;
                }
                break;
            }

            case GDX_A_SETLOOP: {
                sPendingLoopState = (const int16_t*)GdxAudioResolveAddr(w1, "SETLOOP");
                sRsCapSetloopSince = 1; /* [rs-cap] loop-wrap marker for the next resample */
                break;
            }

            case GDX_A_ADPCM: {
                uint32_t flags = (w0 >> 16) & 0xFFu;
                int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "ADPCM-state");
                sGdxHleAdpcmOps++; /* [audio-hle] per-voice decode work */
                int stageTarget = GdxUnlockStageFindCommandTarget(&cmds[i]);
                /* [pcm-cap] carry-in snapshot, taken before the decode overwrites it. Under the
                   last-frame state layout the newest two samples are at [15] and [14]. */
                int16_t preSt0 = (state != NULL) ? state[15] : 0;
                int16_t preSt1 = (state != NULL) ? state[14] : 0;
                /* [rs-cap] upstream identity for the next resample's chain dump */
                sRsCapAdpcmRaw = sPcmCapLastRaw;
                sRsCapAdpcmFlags = flags;
                sRsCapAdpcmIn = pendingBuf.dmemIn;
                sRsCapAdpcmOut = pendingBuf.dmemOut;
                sRsCapAdpcmCnt = pendingBuf.count;
                RunAdpcm(&pendingBuf, flags, state, sPendingLoopState);
                GdxUnlockStageCaptureDecode(stageTarget, &pendingBuf, flags);
                /* [pcm-cap] decode ground truth, one block per UNIQUE sample source: input
                   identity, book, first compressed bytes, first 8 decoded samples. An offline
                   decode of the same ROM bytes with the same book either matches (kernels
                   innocent, look at per-note setup/gain) or does not. Diag-gated: the
                   race-armed budget dumped ~120 lines at race start. */
                if (gdx_diag_audio_enabled()) {
                    static uintptr_t sPcmCapSeen[24];
                    static int sPcmCapCount = 0;
                    int pc, pcDup = 0;
                    for (pc = 0; pc < sPcmCapCount; pc++) {
                        if (sPcmCapSeen[pc] == sPcmCapLastSrc) { pcDup = 1; break; }
                    }
                    {
                        extern int gGdxRaceActive;
                        if (!gGdxRaceActive) {
                            /* Race-gated: boot/menu samples would burn the budget first. */
                            break;
                        }
                    }
                    if (!pcDup && sPcmCapCount < 24 && sPcmCapLastSrc != 0) {
                        sPcmCapSeen[sPcmCapCount++] = sPcmCapLastSrc;
                        gdx_port_logf("[pcm-cap] raw=%08X src=%p flags=%02X in=%04X out=%04X n=%u "
                                      "st=%04X %04X\n",
                                      sPcmCapLastRaw, (void*)sPcmCapLastSrc, (unsigned)flags,
                                      pendingBuf.dmemIn, pendingBuf.dmemOut, pendingBuf.count,
                                      (uint16_t)preSt0, (uint16_t)preSt1);
                        /* Both predictors' full 16 coefficients: an offline order-2 decode needs
                           all of them. */
                        {
                            /* sAdpcmBook holds raw BYTES of host-order s16 coefficients (see
                               BookCoef's memcpy) -- read pairs, not single bytes. */
                            const int16_t* bk = (const int16_t*)(const void*)sAdpcmBook;
                            int bl;
                            for (bl = 0; bl < 2; bl++) {
                                gdx_port_logf("[pcm-cap]  bk%d=%04X %04X %04X %04X %04X %04X %04X %04X "
                                              "%04X %04X %04X %04X %04X %04X %04X %04X\n", bl,
                                              (uint16_t)bk[bl * 16 + 0], (uint16_t)bk[bl * 16 + 1],
                                              (uint16_t)bk[bl * 16 + 2], (uint16_t)bk[bl * 16 + 3],
                                              (uint16_t)bk[bl * 16 + 4], (uint16_t)bk[bl * 16 + 5],
                                              (uint16_t)bk[bl * 16 + 6], (uint16_t)bk[bl * 16 + 7],
                                              (uint16_t)bk[bl * 16 + 8], (uint16_t)bk[bl * 16 + 9],
                                              (uint16_t)bk[bl * 16 + 10], (uint16_t)bk[bl * 16 + 11],
                                              (uint16_t)bk[bl * 16 + 12], (uint16_t)bk[bl * 16 + 13],
                                              (uint16_t)bk[bl * 16 + 14], (uint16_t)bk[bl * 16 + 15]);
                            }
                        }
                        gdx_port_logf("[pcm-cap]  book=%04X %04X %04X %04X "
                                      "cmp8=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                      (uint16_t)sAdpcmBook[0], (uint16_t)sAdpcmBook[1],
                                      (uint16_t)sAdpcmBook[2], (uint16_t)sAdpcmBook[3],
                                      DmemGetU8(pendingBuf.dmemIn + 0u), DmemGetU8(pendingBuf.dmemIn + 1u),
                                      DmemGetU8(pendingBuf.dmemIn + 2u), DmemGetU8(pendingBuf.dmemIn + 3u),
                                      DmemGetU8(pendingBuf.dmemIn + 4u), DmemGetU8(pendingBuf.dmemIn + 5u),
                                      DmemGetU8(pendingBuf.dmemIn + 6u), DmemGetU8(pendingBuf.dmemIn + 7u));
                        /* +32: fresh output starts after the 16-sample preamble (RunAdpcm). */
                        gdx_port_logf("[pcm-cap]  pcm8=%04X %04X %04X %04X %04X %04X %04X %04X\n",
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 32u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 34u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 36u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 38u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 40u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 42u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 44u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 46u));
                    }
                }
                break;
            }

            case GDX_A_S8DEC: {
                uint32_t flags = (w0 >> 16) & 0xFFu;
                int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "S8DEC-state");
                RunS8Dec(&pendingBuf, flags, state);
                break;
            }

            case GDX_A_RESAMPLE: {
                uint32_t flags = (w0 >> 16) & 0xFFu;
                uint32_t pitch = w0 & 0xFFFFu;
                int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "RESAMPLE-state");
                int stageTarget = GdxUnlockStageFindCommandTarget(&cmds[i]);
                sGdxUnlockStage.currentTarget = stageTarget;
                if (state != NULL) {
                    uint32_t numOut = ((pendingBuf.count + 0xFu) & ~0xFu) / 2u;
                    int swept = 0;
                    const int diag = gdx_diag_audio_enabled();
                    if (diag) {
                        /* [rs-cap] speculative record, dumped only if a trigger fires. The input
                           window is snapshotted BEFORE the run: on the two-part path the output
                           overlaps its input (DMEM_TEMP vs DMEM_TEMP+0x20) and would clobber it.
                           Diag-gated as a whole: the chain trigger is race-armed and dumped ~850
                           lines (with per-line filelog fflush) right at race start. */
                        uint32_t needIn = ((numOut * pitch) >> 15) + 8u;
                        uint32_t prevPitch = 0xFFFFFFFFu;
                        uint32_t k;
                        sRsCapCallNo++;
                        sRsCapLast.valid = 1;
                        sRsCapLast.callNo = sRsCapCallNo;
                        sRsCapLast.flags = flags;
                        sRsCapLast.pitch = pitch;
                        sRsCapLast.token = w1;
                        sRsCapLast.dmemIn = pendingBuf.dmemIn;
                        sRsCapLast.dmemOut = pendingBuf.dmemOut;
                        sRsCapLast.count = pendingBuf.count;
                        sRsCapLast.interl = sRsCapInterlPending;
                        sRsCapInterlPending = 0;
                        sRsCapLast.adpcmRaw = sRsCapAdpcmRaw;
                        sRsCapLast.adpcmFlags = sRsCapAdpcmFlags;
                        sRsCapLast.adpcmIn = sRsCapAdpcmIn;
                        sRsCapLast.adpcmOut = sRsCapAdpcmOut;
                        sRsCapLast.adpcmCnt = sRsCapAdpcmCnt;
                        sRsCapLast.setloop = sRsCapSetloopSince;
                        sRsCapSetloopSince = 0;
                        sRsCapLast.inN = needIn > GDX_RSCAP_MAXSAMPS ? GDX_RSCAP_MAXSAMPS : needIn;
                        sRsCapLast.outN = numOut > GDX_RSCAP_MAXSAMPS ? GDX_RSCAP_MAXSAMPS : numOut;
                        for (k = 0; k < sRsCapLast.inN; k++) {
                            sRsCapLast.in[k] = DmemGetS16(pendingBuf.dmemIn + k * 2u);
                        }
                        for (k = 0; k < 5u; k++) {
                            sRsCapLast.preSt[k] = state[k];
                        }
                        for (k = 0; k < (uint32_t)sRsCapPrevN; k++) {
                            if (sRsCapPrev[k].token == w1) {
                                prevPitch = sRsCapPrev[k].pitch;
                                sRsCapPrev[k].pitch = pitch;
                                break;
                            }
                        }
                        if (k == (uint32_t)sRsCapPrevN) {
                            if (sRsCapPrevN < 8) {
                                sRsCapPrev[sRsCapPrevN].token = w1;
                                sRsCapPrev[sRsCapPrevN].pitch = pitch;
                                sRsCapPrevN++;
                            } else {
                                sRsCapPrev[sRsCapCallNo & 7u].token = w1;
                                sRsCapPrev[sRsCapCallNo & 7u].pitch = pitch;
                            }
                        }
                        sRsCapLast.prevPitch = (prevPitch == 0xFFFFFFFFu) ? pitch : prevPitch;
                        swept = ((flags & 1u) == 0u) && (prevPitch != 0xFFFFFFFFu) && (prevPitch != pitch);
                        /* Chain mode: the first swept call during a race locks its state token and
                           every later call on that token is dumped, giving directly adjacent
                           pre/post-state pairs across tick boundaries. Dumping only swept calls left
                           no adjacent pairs to continuity-check. */
                        {
                            extern int gGdxRaceActive;
                            if (sRsCapLockTok == 0u && swept && gGdxRaceActive) {
                                sRsCapLockTok = w1;
                            }
                        }
                    }

                    /* Stash pitch in the otherwise-unused last slot of the state buffer for
                       RunResample. Decomp C never reads that slot, only this interpreter. */
                    state[15] = (int16_t)(uint16_t)pitch;
                    RunResample(&pendingBuf, flags, state);
                    GdxUnlockStageCaptureResample(stageTarget, &pendingBuf, pitch, numOut);

                    if (diag) {
                        uint32_t k;
                        for (k = 0; k < sRsCapLast.outN; k++) {
                            sRsCapLast.out[k] = DmemGetS16(pendingBuf.dmemOut + k * 2u);
                        }
                        for (k = 0; k < 5u; k++) {
                            sRsCapLast.postSt[k] = state[k];
                        }
                        if (sRsCapLockTok != 0u && w1 == sRsCapLockTok && sRsCapChain < 24) {
                            sRsCapChain++;
                            GdxRsCapDump("C");
                        } else if (swept && sRsCapT2 < 4) {
                            sRsCapT2++;
                            GdxRsCapDump("T2");
                        }
                        if (sSpikeLogsResample < 16) {
                            int si = GdxSpikeScan(pendingBuf.dmemOut, numOut);
                            if (si >= 0) {
                                sSpikeLogsResample++;
                                gdx_port_logf("[spike] post-resample call=%u tok=%08X fl=%u pitch=%04X at=%d "
                                              "in=%04X out=%04X cnt=%04X interl=%d adCnt=%04X\n",
                                              (unsigned)sRsCapCallNo, (unsigned)w1, (unsigned)flags,
                                              (unsigned)pitch, si, pendingBuf.dmemIn, pendingBuf.dmemOut,
                                              pendingBuf.count, sRsCapLast.interl, (unsigned)sRsCapAdpcmCnt);
                                GdxRsCapDump("SPK");
                            }
                        }
                    }
                }
                break;
            }

            case GDX_A_MIXER: {
                uint32_t count8 = (w0 >> 16) & 0xFFu; /* count>>4, i.e. groups of 8 samples */
                int32_t gain = (int16_t)(w0 & 0xFFFFu); /* Q15 signed gain */
                uint32_t dmemIn = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemOut = w1 & 0xFFFFu;
                uint32_t numSamples = count8 * 8u;
                uint32_t k;
                /* Reverb kill switch. Skips ONLY the reverb wet->dry return (dmemOut ==
                   LEFT_CH 0x940); the decay mix 0xC80->0xC80 and all note mixes are untouched.
                   Three independent sources turn it off: the gEnhancements.Audio.Reverb CVar
                   (default 1, read live each call -- a benign int race with the menu write, so a
                   toggle applies without a restart), the GDX_NO_REVERB dev gate, and the
                   GdxAudioDbg()&4 debug bit. This is the HLE reverb only: under the default LLE
                   engine the reverb is the ucode's own and this path is never taken. */
                {
                    int reverbOff;
                    reverbOff = gdx_dev_gate(GDX_GATE_NO_REVERB) ||
                                !CVarGetInteger("gEnhancements.Audio.Reverb", 1);
                    if ((reverbOff || (GdxAudioDbg() & 4)) && dmemOut == 0x940u) {
                        break;
                    }
                }
                for (k = 0; k < numSamples; k++) {
                    int32_t in = DmemGetS16(dmemIn + k * 2u);
                    int32_t out = DmemGetS16(dmemOut + k * 2u);
                    out = out + ((in * gain) >> 15);
                    DmemSetS16(dmemOut + k * 2u, ClampS16(out));
                }
                /* [spike] scan the dry bus right after the reverb wet->dry return (the only
                   A_MIXER that writes a dry bus). A spike here means reverb transported a
                   pre-existing one; clean here but spiked at interleave means note mixing
                   injected it. Scanning the input too separates "arrived bad" from "made by this
                   op's clamp". */
                if (dmemOut == 0x940u && gdx_diag_audio_enabled()) {
                    static int sSpikeLogsRvbRet = 0;
                    if (sSpikeLogsRvbRet < 12) {
                        int so = GdxSpikeScan(dmemOut, numSamples);
                        int siN = GdxSpikeScan(dmemIn, numSamples);
                        if (so >= 0 || siN >= 0) {
                            sSpikeLogsRvbRet++;
                            gdx_port_logf("[spike] reverb-return dryOut=%d wetIn=%d "
                                          "(out=%04X in=%04X n=%u)\n",
                                          so, siN, dmemOut, dmemIn, numSamples);
                        }
                    }
                }
                break;
            }

            case GDX_A_ADDMIXER: {
                /* Gainless clamped unity add -- `dst = clamp_s16(dst + src)`, no multiply --
                   matching mupen64plus-rsp-hle's alist_add (alist.c#L595-609). This op has no call
                   site in disk/lib/synthesis.c, so there is no evidence that abi.h's aAddMixer
                   `a4` field (packed into w0's low 16 bits as a generic undocumented ucode field)
                   is a gain; treating it as a Q15 gain was this port's own invention. The count>>4
                   packing in bits 16..23 is abi.h's and is unchanged. */
                uint32_t count8 = (w0 >> 16) & 0xFFu;
                uint32_t dmemIn = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemOut = w1 & 0xFFFFu;
                uint32_t numSamples = count8 * 8u;
                uint32_t k;
                for (k = 0; k < numSamples; k++) {
                    int32_t in = DmemGetS16(dmemIn + k * 2u);
                    int32_t out = DmemGetS16(dmemOut + k * 2u);
                    DmemSetS16(dmemOut + k * 2u, ClampS16(out + in));
                }
                break;
            }

            case GDX_A_INTERLEAVE: {
                uint32_t dmemOut = w0 & 0xFFFFu;
                uint32_t dmemL = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemR = w1 & 0xFFFFu;
                uint32_t byteCount = ((w0 >> 16) & 0xFFu) << 4; /* per-channel bytes, c>>4 packed */
                uint32_t numSamples = byteCount / 2u;
                GdxUnlockStageAppendChunk(dmemL, dmemR, numSamples);
                /* [spike] the interleave inputs are the fully mixed dry L/R buses: a spike here
                   but not post-resample means a mixing stage injected it. */
                if (sSpikeLogsInterleave < 16 && gdx_diag_audio_enabled()) {
                    int sl = GdxSpikeScan(dmemL, numSamples);
                    int sr = GdxSpikeScan(dmemR, numSamples);
                    if (sl >= 0 || sr >= 0) {
                        uint8_t wl = (sl >= 0) ? sDmemLastOp[((dmemL + (uint32_t)sl * 2u) & GDX_DMEM_MASK) >> 4]
                                               : 0xFF;
                        uint8_t wr = (sr >= 0) ? sDmemLastOp[((dmemR + (uint32_t)sr * 2u) & GDX_DMEM_MASK) >> 4]
                                               : 0xFF;
                        sSpikeLogsInterleave++;
                        gdx_port_logf("[spike] pre-interleave L=%d R=%d dmemL=%04X dmemR=%04X n=%u "
                                      "lastOpL=%u lastOpR=%u\n",
                                      sl, sr, dmemL, dmemR, numSamples, wl, wr);
                    }
                }
                uint32_t k;
                for (k = 0; k < numSamples; k++) {
                    DmemSetS16(dmemOut + k * 4u + 0u, DmemGetS16(dmemL + k * 2u));
                    DmemSetS16(dmemOut + k * 4u + 2u, DmemGetS16(dmemR + k * 2u));
                }
                break;
            }

            case GDX_A_INTERL: {
                /* Rare nParts==2 note-split path. Best-guess decimation: every other sample. */
                uint32_t numSamples = w0 & 0xFFFFu;
                uint32_t dmemIn = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemOut = w1 & 0xFFFFu;
                uint32_t k;
                sRsCapInterlPending = 1; /* [rs-cap] nParts==2 marker for the next resample */
                if (GdxAudioDbg() & 8) { /* nointerl bypass: straight copy, no decimation */
                    for (k = 0; k < numSamples; k++) {
                        DmemSetS16(dmemOut + k * 2u, DmemGetS16(dmemIn + k * 2u));
                    }
                    break;
                }
                for (k = 0; k < numSamples; k++) {
                    DmemSetS16(dmemOut + k * 2u, DmemGetS16(dmemIn + k * 4u));
                }
                break;
            }

            case GDX_A_ENVSETUP1: {
                int32_t a = (int32_t)((w0 >> 16) & 0xFFu);
                int32_t b = (int32_t)(int16_t)(w0 & 0xFFFFu);
                int32_t c = (int32_t)(int16_t)((w1 >> 16) & 0xFFFFu);
                int32_t d = (int32_t)(int16_t)(w1 & 0xFFFFu);
                /* env_values[2] = a << 8, in the SAME Q16 space as the dry L/R volumes, not a
                   separate Q8 one. mupen's nead ENVSETUP1 computes `(w1 >> 8) & 0xff00`, which
                   with this file's w0 layout is exactly `a << 8`, and envmix_nead then feeds it
                   through the same `(x * env) >> 16` formula as env_values[0]/[1] (already full
                   Q16 -- targetVol<<4). env_steps[2] is w1's low 16 bits, i.e. `b` here, added
                   directly each block with no further scaling; that only lines up because this
                   port derives rampReverb as (delta(reverb & 0x7F) << 9) / blocks, the same
                   reverb<<9 == a<<8 units. */
                envReverbVol2 = a << 8;
                envRampReverb = b;
                envRampLeft = c;
                envRampRight = d;
                break;
            }

            case GDX_A_ENVSETUP2: {
                envCurVolLeft = (int32_t)(uint16_t)((w1 >> 16) & 0xFFFFu);
                envCurVolRight = (int32_t)(uint16_t)(w1 & 0xFFFFu);
                break;
            }

            case GDX_A_ENVMIXER: {
                sGdxHleEnvmixOps++; /* [audio-hle] per-voice envelope-mix work */
                /* EK macro: w0 = bits(opcode) | (dmemi>>4)<<16(8) | count<<8(8) | swapLR<<4(1) |
                   x0<<3 | x1<<2 | x2<<1 | x3<<0. `count` is the RAW sample count for this call
                   (e.g. aiBufLen for the chunk -- NOT pre-shifted), unlike dmemi which IS
                   pre-shifted by 4. w1 = m (the dmemDests word packed via AUDIO_MK_CMD -- NOT a
                   pointer, despite the union field being named `addr` in PR/abi.h). */
                uint32_t dmemSrc = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t sampleCount = (w0 >> 8) & 0xFFu;
                uint32_t swapLR = (w0 >> 4) & 1u;
                uint32_t dmemDests = w1;
                uint32_t dryLeftDmem = ((dmemDests >> 24) & 0xFFu) << 4;
                uint32_t dryRightDmem = ((dmemDests >> 16) & 0xFFu) << 4;
                uint32_t wetLeftDmem = ((dmemDests >> 8) & 0xFFu) << 4;
                uint32_t wetRightDmem = (dmemDests & 0xFFu) << 4;
                /* Ramps advance once per block of 8 samples (see AudioSynth_ProcessEnvelope's
                   `aiBufLen >> 3` ramp-step math -- `sampleCount` here is always a multiple of 8). */
                uint32_t numBlocks = sampleCount >> 3;
                int32_t curVolL = envCurVolLeft, curVolR = envCurVolRight, curReverb = envReverbVol2;
                uint32_t sIdx = 0;
                uint32_t blk;

                sGdxUnlockStage.currentTarget = GdxUnlockStageFindCommandTarget(&cmds[i]);

                /* flatvol bypass: one constant volume for the whole tick, to test whether the
                   8-block volume staircase is the grain. */
                if (GdxAudioDbg() & 2) {
                    envRampLeft = envRampRight = envRampReverb = 0;
                }

                if (swapLR) {
                    uint32_t tmp = dryLeftDmem; dryLeftDmem = dryRightDmem; dryRightDmem = tmp;
                }

                GdxUnlockStageCapturePreEnvelope(dmemSrc, sampleCount);

                for (blk = 0; blk < numBlocks; blk++) {
                    uint32_t n;
                    for (n = 0; n < 8u && sIdx < sampleCount; n++, sIdx++) {
                        int32_t s = DmemGetS16(dmemSrc + sIdx * 2u);
                        /* Volumes are Q16, NOT Q12: AudioSynth_ProcessEnvelope does
                           `targetVol <<= 4` before packing ENVSETUP1/2 (playback.c's Q12 value
                           times 16, full volume 0xFFF0). A >>12 here overdrives every
                           normal-volume voice 16x into rail-to-rail clipping. */
                        int32_t dl = (s * curVolL) >> 16;
                        int32_t dr = (s * curVolR) >> 16;
                        /* nead semantics (mupen64plus-rsp-hle alist.c#L512-562 envmix_nead): wet
                           CASCADES the DRY-SCALED sample per channel -- wetL off dl, wetR off dr
                           -- not the raw input shared across both. curReverb is already in the
                           a<<8 (Q16) space set up by ENVSETUP1, so it uses the same >>16 shift as
                           dl/dr. */
                        int32_t wetL = (dl * curReverb) >> 16;
                        int32_t wetR = (dr * curReverb) >> 16;

                        GdxUnlockStageCaptureEnvelopeSample(sIdx, dl, dr);

                        DmemSetS16(dryLeftDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(dryLeftDmem + sIdx * 2u) + dl));
                        DmemSetS16(dryRightDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(dryRightDmem + sIdx * 2u) + dr));
                        DmemSetS16(wetLeftDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(wetLeftDmem + sIdx * 2u) + wetL));
                        DmemSetS16(wetRightDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(wetRightDmem + sIdx * 2u) + wetR));
                    }
                    curVolL += envRampLeft;
                    curVolR += envRampRight;
                    curReverb += envRampReverb;
                }
                /* [spike] scan this op's own input and all four output buses, so one run says
                   whether the corruption arrives with the source, is injected here, or was
                   already sitting on a bus. */
                if (gdx_diag_audio_enabled()) {
                    static int sSpikeLogsEnvmix = 0;
                    if (sSpikeLogsEnvmix < 12) {
                        int si = GdxSpikeScan(dmemSrc, sampleCount);
                        int sdl = GdxSpikeScan(dryLeftDmem, sampleCount);
                        int sdr = GdxSpikeScan(dryRightDmem, sampleCount);
                        int swl = GdxSpikeScan(wetLeftDmem, sampleCount);
                        int swr = GdxSpikeScan(wetRightDmem, sampleCount);
                        if (si >= 0 || sdl >= 0 || sdr >= 0 || swl >= 0 || swr >= 0) {
                            sSpikeLogsEnvmix++;
                            /* When the source carries the spike, the last-writer tracker names
                               the op that wrote it: 5=A_RESAMPLE, 7=A_FILTER, 14=A_HILOGAIN. */
                            uint8_t srcWriter = (si >= 0)
                                ? sDmemLastOp[((dmemSrc + (uint32_t)si * 2u) & GDX_DMEM_MASK) >> 4]
                                : 0xFF;
                            gdx_port_logf("[spike] envmixer src=%d dryL=%d dryR=%d wetL=%d wetR=%d "
                                          "(dmemSrc=%04X n=%u) srcWriter=%u\n",
                                          si, sdl, sdr, swl, swr, dmemSrc, sampleCount, srcWriter);
                        }
                    }
                }
                sGdxUnlockStage.currentTarget = -1;
                break;
            }

            case GDX_A_HILOGAIN: {
                /* IN-PLACE: the ucode scales the buffer at w1>>16, and w1's low 16 bits are
                   unused padding. synthesis.c:1069 passes 0 for that out field; honouring it as a
                   destination writes every gain-carrying note over DMEM 0 (clobbering scratch
                   consumed later in the tick) while the real note path flows on unscaled. */
                int32_t gain = (int32_t)((w0 >> 16) & 0xFFu); /* Q4, 0x10 == 1.0x */
                uint32_t size = w0 & 0xFFFFu;
                uint32_t dmem = (w1 >> 16) & 0xFFFFu;
                uint32_t numSamples = size / 2u;
                uint32_t k;
                /* [rs-cap] T1: a HILOGAIN means this is a gained note (the booster/low-health
                   set) -- dump its FinalResample record, snapshotted just above in the list. */
                if (sRsCapT1 < 8 && sRsCapLast.valid && gdx_diag_audio_enabled()) {
                    sRsCapT1++;
                    gdx_port_logf("[rs-cap] T1-hilogain gain=%02X size=%04X dmem=%04X\n",
                                  (unsigned)gain, (unsigned)size, (unsigned)dmem);
                    GdxRsCapDump("T1");
                }
                for (k = 0; k < numSamples; k++) {
                    int32_t s = DmemGetS16(dmem + k * 2u);
                    DmemSetS16(dmem + k * 2u, ClampS16((s * gain) >> 4));
                }
                break;
            }

            case GDX_A_FILTER: {
                /* Two-step protocol, see RunFilter's header. f (w0 bits 16..23) selects prime
                   (f==2) or apply. countOrBuf (w0 bits 0..15) is a coefficient-table BYTE SIZE on
                   the prime call but a DMEM ADDRESS on the apply call -- never a host pointer
                   either way, unlike w1. */
                uint32_t f = (w0 >> 16) & 0xFFu;
                uint32_t countOrBuf = w0 & 0xFFFFu;
                if (f == 2u) {
                    void* coefSrc = GdxAudioResolveAddr(w1, "FILTER-coef");
                    if (coefSrc != NULL) {
                        memcpy(pendingFilterCoef, coefSrc, sizeof(pendingFilterCoef));
                        pendingFilterHaveCoef = 1;
                    }
                    pendingFilterSizeBytes = countOrBuf;
                } else if (GdxAudioDbg() & 1) {
                    /* nofilter bypass: leave the note buffer unfiltered. */
                } else {
                    int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "FILTER-state");
                    RunFilter(countOrBuf, pendingFilterSizeBytes, f, state,
                              pendingFilterHaveCoef ? pendingFilterCoef : NULL);
                    /* [spike] the filter runs in place on the note buffer. */
                    if (sSpikeLogsFilter < 16 && gdx_diag_audio_enabled()) {
                        int si = GdxSpikeScan(countOrBuf, pendingFilterSizeBytes / 2u);
                        if (si >= 0) {
                            sSpikeLogsFilter++;
                            gdx_port_logf("[spike] post-filter at=%d dmem=%04X n=%u fl=%u\n",
                                          si, countOrBuf, pendingFilterSizeBytes / 2u, f);
                        }
                    }
                }
                break;
            }

            case GDX_A_UNK19:
                /* SDK-unknown, reached only by the rare bookOffset==3 note path. Safe no-op:
                   src==dst at the one call site, so leaving DMEM untouched drops nothing. */
                break;

            case GDX_A_UNK3:
            case GDX_A_RESAMPLE_ZOH:
            case GDX_A_DUPLICATE:
                /* Not used by decomp/src/audio/disk/lib/synthesis.c; no-op. */
                break;

            default:
                if (sUnhandledLogs < 16 && gdx_diag_audio_enabled()) {
                    sUnhandledLogs++;
                    gdx_port_logf("[audio-hle] unhandled opcode=%u w0=%08X w1=%08X (skipped)\n",
                                  (unsigned)op, (unsigned)w0, (unsigned)w1);
                }
                break;
        }
    }

    /* [audio-hle] periodic receipt (~4 s at 128 runs): last list length, cumulative voice
       ops, cumulative nonzero SAVEBUFFs. cmds~27 with adpcm=0 forever = the empty mix (the
       zero link is upstream in the sequencer); adpcm>0 with nzSaves=0 = decode feeding
       zeros (data-side); nzSaves>0 = the DSP stage is producing real PCM. */
    if ((gdx_watch_hle_runs & 0x1FF) == 1u && gdx_diag_audio_enabled()) {
        gdx_port_logf("[audio-hle] run=%u cmds=%u adpcm=%u envmix=%u nzSaves=%u\n",
                      (unsigned)gdx_watch_hle_runs, (unsigned)count, (unsigned)sGdxHleAdpcmOps,
                      (unsigned)sGdxHleEnvmixOps, (unsigned)sGdxHleNzSaves);
    }
    sGdxUnlockStage.rangeCount = 0;
    sGdxUnlockStage.commandList = 0;
}
