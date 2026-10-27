/* port/3ds/gdx3ds_hitch.c — HITCH: the per-frame hitch receipt. Contract in
 * include/gdx3ds_hitch.h. Main thread only (the host loop); zero heap. */
#include "gdx3ds_hitch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_dynlod.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_frameskip.h"
#include "gdx3ds_mainprof.h"

#define HITCH_RING 32              /* work samples per class for the running median */
#define HITCH_WARM 16              /* samples before a class can report a hitch */
#define HITCH_PENDING 8            /* buffered lines */
#define HITCH_LINE_CAP 512
#define HITCH_EMIT_GAP 8u          /* iterations between two emitted lines */
#define HITCH_BUDGET 16u           /* lines per HITCH_BUDGET_WINDOW iterations */
#define HITCH_BUDGET_WINDOW 256u
#define HITCH_SUM_EVERY 1024u
/* One top-LCD refresh, the unit of the "half a vblank over the median" floor. */
#define HITCH_VBLANK_TICKS ((uint64_t)((double)SYSCLOCK_ARM11 / 59.8337))

typedef struct HitchClass {
    uint32_t us[HITCH_RING];
    int n;
    int pos;
    uint32_t medianUs; /* recomputed every 8 samples */
    int sinceMedian;
} HitchClass;

static int sChecked = 0;
static int sArmed = 0;
static uint32_t sTexBurstMin = 0; /* N60TEX [debug] texburst: 0 = off */
static uint64_t sIterT0 = 0;
static HitchClass sClass[2]; /* [0] skipped iterations, [1] rendered */
static int sPrevLodTier = -1;
static GdxFilelogStats sLogPrev;

static char sPending[HITCH_PENDING][HITCH_LINE_CAP];
static int sPendingLen[HITCH_PENDING];
static int sPendingHead = 0;
static int sPendingCount = 0;
static unsigned long sNextEmitFrame = 0;
static unsigned long sBudgetWindowStart = 0;
static unsigned sBudgetUsed = 0;
static unsigned long sSuppressed = 0;
static int sLastWasHitch = 0;

/* Summary window. */
static unsigned long sSumFrames = 0;
static unsigned long sSumHitches[2];
static uint32_t sSumWorstUs = 0;
static GdxFilelogStats sSumLogPrev;
/* Texture-miss bursts per iteration (timing-independent: the same on the lane and the console):
 * iterations with 1-3, 4-7, 8-15, 16+ texture cache misses, and the worst. */
static unsigned long sSumTexBins[4];
static uint32_t sSumTexMax = 0;
/* What the hitch iterations had in them (one hitch can count in several). */
enum { CAUSE_FLUSH = 0, CAUSE_TEX, CAUSE_BAKE, CAUSE_LOAD, CAUSE_LOD, CAUSE_AUD, CAUSE_NONE, CAUSE_COUNT };
static unsigned long sSumCause[CAUSE_COUNT];

static void HitchLog(const char* buf, int len) {
    svcOutputDebugString(buf, (size_t)len);
    gdx3ds_filelog_write(buf, (size_t)len);
}

static uint32_t TicksToUs(uint64_t ticks) {
    return (uint32_t)(ticks / (uint64_t)CPU_TICKS_PER_USEC);
}

static void EnsureInit(void) {
    if (sChecked || gdx3ds_config_loaded() == 0) {
        return;
    }
    sChecked = 1;
    sArmed = gdx3ds_config_get_int("debug", "hitch", 0) != 0;
    {
        const int texBurst = gdx3ds_config_get_int("debug", "texburst", 0);
        sTexBurstMin = texBurst > 0 ? (uint32_t)texBurst : 0u;
    }
    if (sArmed) {
        char line[200];
        const int n = snprintf(line, sizeof(line),
                               "[hitch] receipt armed (debug.hitch=1): work > max(2x median, median+%u us) "
                               "per class; %d-line buffer, 1 line per %u iterations, %u per %u; cats need "
                               "gputrace/mainprof",
                               (unsigned)(TicksToUs(HITCH_VBLANK_TICKS) / 2u), HITCH_PENDING, HITCH_EMIT_GAP,
                               HITCH_BUDGET, HITCH_BUDGET_WINDOW);
        if (n > 0) {
            HitchLog(line, n);
        }
        gdx3ds_filelog_stats(&sLogPrev);
        sSumLogPrev = sLogPrev;
    }
}

void gdx3ds_hitch_frame_begin(void) {
    EnsureInit();
    sIterT0 = svcGetSystemTick();
}

static uint32_t TakeEvent(int ev) {
    return __atomic_exchange_n(&gdx3ds_hitch_ev[ev], 0u, __ATOMIC_RELAXED);
}

static void ClearEvents(uint32_t* out) {
    for (int i = 0; i < GDX_HITCH_EV_COUNT; i++) {
        out[i] = TakeEvent(i);
    }
}

static uint32_t MedianOf(const HitchClass* c) {
    uint32_t sorted[HITCH_RING];
    const int n = c->n;
    memcpy(sorted, c->us, sizeof(uint32_t) * (size_t)n);
    for (int i = 1; i < n; i++) {
        const uint32_t v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }
    return n > 0 ? sorted[n / 2] : 0;
}

static void PushSample(HitchClass* c, uint32_t us) {
    c->us[c->pos] = us;
    c->pos = (c->pos + 1) % HITCH_RING;
    if (c->n < HITCH_RING) {
        c->n++;
    }
    if (++c->sinceMedian >= 8 || c->medianUs == 0) {
        c->medianUs = MedianOf(c);
        c->sinceMedian = 0;
    }
}

static uint32_t Threshold(const HitchClass* c) {
    const uint32_t twice = c->medianUs * 2u;
    const uint32_t floor = c->medianUs + TicksToUs(HITCH_VBLANK_TICKS) / 2u;
    return twice > floor ? twice : floor;
}

/* ---- the line ---- */

typedef struct LineBuf {
    char* p;
    int len;
    int cap;
} LineBuf;

static void Put(LineBuf* b, const char* fmt, ...) {
    va_list ap;
    int n;
    if (b->len >= b->cap - 1) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, (size_t)(b->cap - b->len), fmt, ap);
    va_end(ap);
    if (n > 0) {
        b->len += n < b->cap - b->len ? n : b->cap - b->len - 1;
    }
}

static unsigned MsX10(uint64_t ticks) {
    return (unsigned)((ticks * 10u) / (uint64_t)CPU_TICKS_PER_MSEC);
}

static void PutMs(LineBuf* b, const char* name, uint64_t ticks) {
    const unsigned x = MsX10(ticks);
    Put(b, " %s=%u.%u", name, x / 10u, x % 10u);
}

static void PutCategories(LineBuf* b) {
    uint64_t t[GDX3DS_MPROF_COUNT];
    if (gdx3ds_mprof_snapshot(t, GDX3DS_MPROF_COUNT) == 0) {
        Put(b, " | cats=off");
        return;
    }
    Put(b, " |");
    PutMs(b, "lg", t[GDX3DS_MPROF_LOGIC]);
    PutMs(b, "dl", t[GDX3DS_MPROF_DLB]);
    PutMs(b, "au", t[GDX3DS_MPROF_AUD]);
    PutMs(b, "ld", t[GDX3DS_MPROF_LSEG] + t[GDX3DS_MPROF_SLA] + t[GDX3DS_MPROF_SEG] + t[GDX3DS_MPROF_DMA]);
    PutMs(b, "vi", t[GDX3DS_MPROF_VI] + t[GDX3DS_MPROF_DISP]);
    PutMs(b, "ta", t[GDX3DS_MPROF_STASK] + t[GDX3DS_MPROF_BRM] + t[GDX3DS_MPROF_SUB]);
    PutMs(b, "tl", t[GDX3DS_MPROF_TAIL] + t[GDX3DS_MPROF_INP] + t[GDX3DS_MPROF_APT]);
    PutMs(b, "pc", t[GDX3DS_MPROF_PACE]);
    Put(b, " |");
    PutMs(b, "fb", t[GDX3DS_MPROF_BEG] + t[GDX3DS_MPROF_R_FBEG]);
    PutMs(b, "mi", t[GDX3DS_MPROF_R_MISC]);
    PutMs(b, "sk", t[GDX3DS_MPROF_R_SKY]);
    PutMs(b, "tr", t[GDX3DS_MPROF_R_TRK]);
    PutMs(b, "dr", t[GDX3DS_MPROF_R_DRESS]);
    PutMs(b, "ma", t[GDX3DS_MPROF_R_MACH]);
    PutMs(b, "me", t[GDX3DS_MPROF_R_MESH]);
    PutMs(b, "sh", t[GDX3DS_MPROF_R_SHD]);
    PutMs(b, "fx", t[GDX3DS_MPROF_R_FX]);
    PutMs(b, "hu", t[GDX3DS_MPROF_R_HUD]);
    PutMs(b, "mr", t[GDX3DS_MPROF_R_MIRR]);
    PutMs(b, "sw", t[GDX3DS_MPROF_R_SWAP]);
    PutMs(b, "fe", t[GDX3DS_MPROF_END]);
}

static void PutEvents(LineBuf* b, const uint32_t* ev, int lodDelta) {
    Put(b, " | tex=%lu/%lu/%lu up=%lu/%luK seg=%lu/%luK arc=%lu/%luK bake=%lu rr=%lu mtb=%lu hudm=%lu tsk=%lu lod=%d",
        (unsigned long)ev[GDX_HITCH_EV_TEX_IMPORT], (unsigned long)ev[GDX_HITCH_EV_TEX_MISS],
        (unsigned long)ev[GDX_HITCH_EV_TEX_EVICT], (unsigned long)ev[GDX_HITCH_EV_TEX_UP],
        (unsigned long)(ev[GDX_HITCH_EV_TEX_UP_BYTES] / 1024u), (unsigned long)ev[GDX_HITCH_EV_SEG_BLOB],
        (unsigned long)(ev[GDX_HITCH_EV_SEG_BLOB_BYTES] / 1024u), (unsigned long)ev[GDX_HITCH_EV_ARC_LOAD],
        (unsigned long)(ev[GDX_HITCH_EV_ARC_LOAD_BYTES] / 1024u), (unsigned long)ev[GDX_HITCH_EV_MESH_BAKE],
        (unsigned long)ev[GDX_HITCH_EV_RR_BUILD], (unsigned long)ev[GDX_HITCH_EV_MATTAB_BANK],
        (unsigned long)ev[GDX_HITCH_EV_HUD_MISS], (unsigned long)ev[GDX_HITCH_EV_TEXSKIP_REC], lodDelta);
    PutMs(b, "dec", ev[GDX_HITCH_EV_TEX_DECODE_TICKS]);
    PutMs(b, "gsp", ev[GDX_HITCH_EV_GSP_WAIT_TICKS]);
}

/* o3ds-cpucuts: which caller forced this iteration's synchronous flush (the latest one when there
 * were several): a name (gdx3ds_filelog_flush_src, or the sink's own fatal / durable / ringfull /
 * batch paths) or, for an unnamed gdx3ds_filelog_flush / _flush_sync, its return address. */
static void PutSyncSource(LineBuf* b, const GdxFilelogStats* now) {
    if (now->syncFlushes == sLogPrev.syncFlushes || now->lastSyncSrc == NULL) {
        return;
    }
    if (now->lastSyncRa != 0) {
        Put(b, " src=%s@%08lx", now->lastSyncSrc, (unsigned long)now->lastSyncRa);
    } else {
        Put(b, " src=%s", now->lastSyncSrc);
    }
}

static void PutLogAndAudio(LineBuf* b, const GdxFilelogStats* now, const uint32_t* ev) {
    const unsigned syncMsX10 = MsX10(now->syncTicks - sLogPrev.syncTicks);
    Put(b, " | log=%lu sync=%lu/%luK/%u.%u async=%d/%uK aud=%lu/%luK/%d", now->lines - sLogPrev.lines,
        now->syncFlushes - sLogPrev.syncFlushes, (unsigned long)((now->syncBytes - sLogPrev.syncBytes) / 1024u),
        syncMsX10 / 10u, syncMsX10 % 10u, now->writerBusy, now->pending / 1024u,
        (unsigned long)ev[GDX_HITCH_EV_AUD_SD_READ], (unsigned long)(ev[GDX_HITCH_EV_AUD_SD_BYTES] / 1024u),
        gdx3ds_hitch_aud_inflight);
    PutSyncSource(b, now);
}

/* Format the hitch line into the pending ring (dropped + counted when the ring is full). */
static void BufferHitch(unsigned long frame, int rendered, uint64_t workTicks, uint32_t medianUs,
                        const uint32_t* ev, const GdxFilelogStats* logNow, int lodDelta) {
    LineBuf b;
    int d = 0;
    int late = 0;
    const unsigned workX10 = MsX10(workTicks);
    const unsigned medX10 = medianUs / 100u;
    int slot;
    if (sPendingCount >= HITCH_PENDING) {
        sSuppressed++;
        return;
    }
    slot = (sPendingHead + sPendingCount) % HITCH_PENDING;
    b.p = sPending[slot];
    b.len = 0;
    b.cap = HITCH_LINE_CAP;
    gdx3ds_frameskip_hitch_info(&d, &late);
    Put(&b, "[hitch] f=%lu r=%d D=%d late=%d ms=%u.%u med=%u.%u x=%u.%u", frame, rendered, d, late,
        workX10 / 10u, workX10 % 10u, medX10 / 10u, medX10 % 10u,
        medX10 != 0 ? workX10 / medX10 : 0u, medX10 != 0 ? (workX10 * 10u / medX10) % 10u : 0u);
    PutCategories(&b);
    PutEvents(&b, ev, lodDelta);
    PutLogAndAudio(&b, logNow, ev);
    sPendingLen[slot] = b.len;
    sPendingCount++;
}

static int BudgetAllows(unsigned long frame) {
    if (frame - sBudgetWindowStart >= HITCH_BUDGET_WINDOW) {
        sBudgetWindowStart = frame;
        sBudgetUsed = 0;
    }
    return sBudgetUsed < HITCH_BUDGET && frame >= sNextEmitFrame;
}

/* One buffered line out, on a quiet iteration, within the rate budget. */
static void EmitPending(unsigned long frame) {
    char* line;
    int len;
    if (sPendingCount == 0 || !BudgetAllows(frame)) {
        return;
    }
    line = sPending[sPendingHead];
    len = sPendingLen[sPendingHead];
    if (sSuppressed != 0 && len < HITCH_LINE_CAP - 24) {
        len += snprintf(line + len, (size_t)(HITCH_LINE_CAP - len), " supp=%lu", sSuppressed);
        sSuppressed = 0;
    }
    HitchLog(line, len);
    sPendingHead = (sPendingHead + 1) % HITCH_PENDING;
    sPendingCount--;
    sBudgetUsed++;
    sNextEmitFrame = frame + HITCH_EMIT_GAP;
}

static void EmitSummary(unsigned long frame) {
    GdxFilelogStats now;
    char line[544];
    int n;
    gdx3ds_filelog_stats(&now);
    {
        const unsigned sMs = MsX10(now.syncTicks - sSumLogPrev.syncTicks);
        const unsigned aMs = MsX10(now.asyncTicks - sSumLogPrev.asyncTicks);
        const unsigned aMax = MsX10(now.asyncMaxTicks);
        n = snprintf(line, sizeof(line),
                     "[hitch] sum f=%lu n=%lu hitches r=%lu s=%lu worst=%lu.%lu med r=%lu.%lu s=%lu.%lu | "
                     "log lines=%lu KiB=%lu sync=%lu/%u.%ums async=%d writes=%lu/%u.%ums max=%u.%u waits=%lu "
                     "ringfull=%lu pending=%u | cause flush=%lu tex=%lu bake=%lu load=%lu lod=%lu aud=%lu none=%lu | "
                     "texmiss/iter 1-3=%lu 4-7=%lu 8-15=%lu 16+=%lu max=%lu | quiet=%lu dropped=%lu thinned=%lu "
                     "ayield=%lu fdefer=%lu lastsync=%s",
                     frame, sSumFrames, sSumHitches[1], sSumHitches[0], (unsigned long)(sSumWorstUs / 1000u),
                     (unsigned long)((sSumWorstUs / 100u) % 10u), (unsigned long)(sClass[1].medianUs / 1000u),
                     (unsigned long)((sClass[1].medianUs / 100u) % 10u), (unsigned long)(sClass[0].medianUs / 1000u),
                     (unsigned long)((sClass[0].medianUs / 100u) % 10u), now.lines - sSumLogPrev.lines,
                     (unsigned long)((now.bytes - sSumLogPrev.bytes) / 1024u),
                     now.syncFlushes - sSumLogPrev.syncFlushes, sMs / 10u, sMs % 10u, now.async,
                     now.asyncWrites - sSumLogPrev.asyncWrites, aMs / 10u, aMs % 10u, aMax / 10u, aMax % 10u,
                     now.syncWaits - sSumLogPrev.syncWaits, now.ringFullFlushes - sSumLogPrev.ringFullFlushes,
                     now.pending, sSumCause[CAUSE_FLUSH], sSumCause[CAUSE_TEX], sSumCause[CAUSE_BAKE],
                     sSumCause[CAUSE_LOAD], sSumCause[CAUSE_LOD], sSumCause[CAUSE_AUD], sSumCause[CAUSE_NONE], sSumTexBins[0],
                     sSumTexBins[1], sSumTexBins[2], sSumTexBins[3], (unsigned long)sSumTexMax,
                     now.asyncDeferred - sSumLogPrev.asyncDeferred, now.ringFullDrops - sSumLogPrev.ringFullDrops,
                     now.receiptsThinned - sSumLogPrev.receiptsThinned, now.audioYields - sSumLogPrev.audioYields,
                     now.flushDeferred - sSumLogPrev.flushDeferred,
                     now.lastSyncSrc != NULL ? now.lastSyncSrc : "-");
    }
    if (n > 0) {
        HitchLog(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    }
    sSumLogPrev = now;
    sSumFrames = 0;
    sSumHitches[0] = 0;
    sSumHitches[1] = 0;
    sSumWorstUs = 0;
    memset(sSumCause, 0, sizeof(sSumCause));
    memset(sSumTexBins, 0, sizeof(sSumTexBins));
    sSumTexMax = 0;
}

static void NoteTexBurst(uint32_t misses) {
    if (misses == 0) {
        return;
    } else if (misses < 4u) {
        sSumTexBins[0]++;
    } else if (misses < 8u) {
        sSumTexBins[1]++;
    } else if (misses < 16u) {
        sSumTexBins[2]++;
    } else {
        sSumTexBins[3]++;
    }
    if (misses > sSumTexMax) {
        sSumTexMax = misses;
    }
}

/* Attribute a hitch: a synchronous SD flush on this iteration, a texture burst (4+ misses), a
 * mesh bake / retained build, a blob or archive load, a dynlod tier change, an audio-stream SD read
 * -- or none of these. */
static void NoteCauses(const uint32_t* ev, unsigned long syncFlushes, int lodDelta) {
    int any = 0;
    if (syncFlushes != 0) {
        sSumCause[CAUSE_FLUSH]++;
        any = 1;
    }
    if (ev[GDX_HITCH_EV_TEX_MISS] >= 4u) {
        sSumCause[CAUSE_TEX]++;
        any = 1;
    }
    if (ev[GDX_HITCH_EV_MESH_BAKE] != 0 || ev[GDX_HITCH_EV_RR_BUILD] != 0) {
        sSumCause[CAUSE_BAKE]++;
        any = 1;
    }
    if (ev[GDX_HITCH_EV_SEG_BLOB] != 0 || ev[GDX_HITCH_EV_ARC_LOAD] != 0) {
        sSumCause[CAUSE_LOAD]++;
        any = 1;
    }
    if (lodDelta != 0) {
        sSumCause[CAUSE_LOD]++;
        any = 1;
    }
    /* hwprof2: an audio-stream SD read on this iteration (the producer holds the audio ctx lock
     * across it; main's Audio_Update waits in `au` while the async log writer has the SD). */
    if (ev[GDX_HITCH_EV_AUD_SD_READ] != 0) {
        sSumCause[CAUSE_AUD]++;
        any = 1;
    }
    if (!any) {
        sSumCause[CAUSE_NONE]++;
    }
}

static int LodDelta(void) {
    const int tier = gdx3ds_dynlod_tier();
    const int delta = sPrevLodTier >= 0 ? tier - sPrevLodTier : 0;
    sPrevLodTier = tier;
    return delta;
}

/* ---- N60TEX [debug] texburst: the per-iteration texture burst receipt ------------------------- */

#define TEXBURST_CAP 4096u     /* [texburst] lines per session */
#define TEXBURST_SUM_EVERY 1024u

static unsigned long sTexBurstLines = 0;
static unsigned long sTexBurstWinFrames = 0;
static unsigned long sTexBurstWinBursts = 0;
static uint64_t sTexBurstWinWorstDec = 0;
static uint64_t sTexBurstWinWorstUp = 0;
static uint64_t sTexBurstWinWorstWork = 0;
static uint32_t sTexBurstWinMisses = 0;
static uint32_t sTexBurstWinKiB = 0;

static void TexBurstLine(unsigned long frame, int rendered, uint64_t work, const uint32_t* ev) {
    char line[HITCH_LINE_CAP];
    LineBuf b = { line, 0, (int)sizeof(line) };
    const uint64_t dec = ev[GDX_HITCH_EV_TEX_DECODE_TICKS];
    const uint64_t up = ev[GDX_HITCH_EV_TEX_UP_TICKS];
    Put(&b, "[texburst] f=%lu r=%d miss=%lu imp=%lu up=%lu/%luK", frame, rendered,
        (unsigned long)ev[GDX_HITCH_EV_TEX_MISS], (unsigned long)ev[GDX_HITCH_EV_TEX_IMPORT],
        (unsigned long)ev[GDX_HITCH_EV_TEX_UP], (unsigned long)(ev[GDX_HITCH_EV_TEX_UP_BYTES] / 1024u));
    PutMs(&b, "dec", dec);
    PutMs(&b, "upl", up);
    PutMs(&b, "decode_only", dec > up ? dec - up : 0u);
    PutMs(&b, "work", work);
    HitchLog(line, b.len);
}

static void TexBurstSummary(unsigned long frame) {
    char line[HITCH_LINE_CAP];
    LineBuf b = { line, 0, (int)sizeof(line) };
    Put(&b, "[texburst] sum f=%lu n=%lu bursts=%lu misses=%lu KiB=%lu worst", frame, sTexBurstWinFrames,
        sTexBurstWinBursts, (unsigned long)sTexBurstWinMisses, (unsigned long)sTexBurstWinKiB);
    PutMs(&b, "dec", sTexBurstWinWorstDec);
    PutMs(&b, "upl", sTexBurstWinWorstUp);
    PutMs(&b, "work", sTexBurstWinWorstWork);
    HitchLog(line, b.len);
    sTexBurstWinFrames = 0;
    sTexBurstWinBursts = 0;
    sTexBurstWinWorstDec = 0;
    sTexBurstWinWorstUp = 0;
    sTexBurstWinWorstWork = 0;
    sTexBurstWinMisses = 0;
    sTexBurstWinKiB = 0;
}

static void TexBurstNoteWorst(uint64_t work, const uint32_t* ev) {
    const uint64_t dec = ev[GDX_HITCH_EV_TEX_DECODE_TICKS];
    const uint64_t up = ev[GDX_HITCH_EV_TEX_UP_TICKS];
    sTexBurstWinBursts++;
    sTexBurstWinMisses += ev[GDX_HITCH_EV_TEX_MISS];
    sTexBurstWinKiB += ev[GDX_HITCH_EV_TEX_UP_BYTES] / 1024u;
    if (dec > sTexBurstWinWorstDec) {
        sTexBurstWinWorstDec = dec;
    }
    if (up > sTexBurstWinWorstUp) {
        sTexBurstWinWorstUp = up;
    }
    if (work > sTexBurstWinWorstWork) {
        sTexBurstWinWorstWork = work;
    }
}

static void TexBurstFrame(unsigned long frame, int rendered, uint64_t work, const uint32_t* ev) {
    if (sTexBurstMin == 0u) {
        return;
    }
    sTexBurstWinFrames++;
    if (ev[GDX_HITCH_EV_TEX_MISS] >= sTexBurstMin) {
        TexBurstNoteWorst(work, ev);
        if (sTexBurstLines < TEXBURST_CAP) {
            sTexBurstLines++;
            TexBurstLine(frame, rendered, work, ev);
        }
    }
    if (sTexBurstWinFrames >= TEXBURST_SUM_EVERY) {
        TexBurstSummary(frame);
    }
}

void gdx3ds_hitch_frame_end(unsigned long frame, int rendered) {
    uint32_t ev[GDX_HITCH_EV_COUNT];
    GdxFilelogStats logNow;
    uint64_t wall;
    uint64_t work;
    uint32_t workUs;
    HitchClass* cls;
    int hitch = 0;
    int lodDelta;
    if (!sArmed && sTexBurstMin == 0u) {
        return;
    }
    wall = svcGetSystemTick() - sIterT0;
    ClearEvents(ev);
    work = wall > ev[GDX_HITCH_EV_PACE_TICKS] ? wall - ev[GDX_HITCH_EV_PACE_TICKS] : 0;
    TexBurstFrame(frame, rendered, work, ev);
    if (!sArmed) {
        return;
    }
    workUs = TicksToUs(work);
    cls = &sClass[rendered ? 1 : 0];
    lodDelta = LodDelta();
    gdx3ds_filelog_stats(&logNow);
    NoteTexBurst(ev[GDX_HITCH_EV_TEX_MISS]);
    if (cls->n >= HITCH_WARM && workUs > Threshold(cls)) {
        hitch = 1;
        sSumHitches[rendered ? 1 : 0]++;
        NoteCauses(ev, logNow.syncFlushes - sLogPrev.syncFlushes, lodDelta);
        BufferHitch(frame, rendered ? 1 : 0, work, cls->medianUs, ev, &logNow, lodDelta);
    }
    PushSample(cls, workUs);
    sLogPrev = logNow;
    if (workUs > sSumWorstUs) {
        sSumWorstUs = workUs;
    }
    sLastWasHitch = hitch;
    sSumFrames++;
}

void gdx3ds_hitch_after_frame(unsigned long frame) {
    if (!sArmed) {
        return;
    }
    if (!sLastWasHitch) {
        EmitPending(frame);
    }
    if (sSumFrames >= HITCH_SUM_EVERY) {
        EmitSummary(frame);
    }
}
