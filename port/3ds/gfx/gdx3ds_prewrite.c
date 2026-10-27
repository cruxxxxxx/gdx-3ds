/* port/3ds/gfx/gdx3ds_prewrite.c -- o3ds3d: [prewrite] / [gpumem] (contract in the header). */
#include "gdx3ds_prewrite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "gdx3ds_pace.h"
#include "../vendor/citro3d/gdx/c3d_gdx.h"

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_config_loaded(void) __attribute__((weak));
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern int gdx3ds_frameskip_render_gap(void) __attribute__((weak)); /* gdx3ds_frameskip.h */
extern int gdx3ds_stereo_active(void) __attribute__((weak));        /* gdx3ds_stereo_frame.cpp */
extern uint32_t gdx3ds_c3d_vbo_used_floats(void) __attribute__((weak)); /* gfx_citro3d.cpp */

enum {
    PW_WINDOW = 256,
    PW_LINEAR_EVERY = 8,      /* linearSpaceFree() every 8th race frame */
    PW_CONTIG_AT = 600,       /* the contiguity probe, frames into the race */
    PW_CHECKPOINT = 3600,     /* a [gpumem] checkpoint every 3600 race frames */
    PW_EXPOSED_VBW_US = 100,  /* a vblank wait after the queue wait shorter than this absorbs nothing */
};

volatile int gdx3ds_pw_armed = 0;

static int sLatched = 0;
static int sOn = 0;    /* [debug] prewrite */
static int sMemOn = 0; /* [debug] gpumem */

/* the frame being built */
static uint64_t sTRet = 0;     /* C3D_FrameBegin's return */
static uint64_t sGsp = 0;      /* its GX queue wait */
static uint64_t sVbw = 0;      /* its vblank wait */
static int sVbwAfter = 0;      /* the vblank wait came after the queue wait (the pace gate) */
static uintptr_t sCmd0 = 0;    /* the command-buffer write pointer at StartFrame's end */
static unsigned sGx0 = 0;      /* GX queue entries then */
static uint32_t sVbo0 = 0;     /* VBO floats then */
static uint32_t sSfWords = 0;  /* words StartFrame itself wrote */
static uint64_t sLastClean = 0;
static uint64_t sFirst = 0;
static uint64_t sFirstLo = 0;
static int sFirstKind = GDX3DS_PW_NONE;
static int sFirstRaw = 0;      /* found by a poll, not a direct stamp */
static uint64_t sDraw = 0;     /* the first write that is not a GX entry (command words, VBO, texture) */
static int sDrawKind = GDX3DS_PW_NONE;

/* the [prewrite] window */
typedef struct PwWin {
    unsigned n, b2b, stereo, exposed, sfFrames, raw;
    unsigned kinds[GDX3DS_PW_KINDS];
    uint64_t sfWords, widthUs, vbwUs, b2bGspUs;
    uint32_t gsp[PW_WINDOW], pre[PW_WINDOW], hide[PW_WINDOW]; /* us, every frame of the window */
    uint32_t draw[PW_WINDOW], hided[PW_WINDOW];
    unsigned hideN;
    unsigned drawKinds[GDX3DS_PW_KINDS];
} PwWin;
static PwWin sWin;
static unsigned long sFrames = 0;

/* the [gpumem] race */
static volatile int sRaceReq = 0; /* bit0 in race scope, bit1 lockstep (main loop) */
static int sRaceOn = 0;
static unsigned sRaceNo = 0;
typedef struct PwRace {
    unsigned long frames, stereo, cmdN, vboN;
    uint32_t linMin, linStart, vramMin, contig;
    uint64_t cmdSum, vboSum;
    uint32_t cmdMax, vboMax, cmdSize, vboPool;
    int lockstep, contigDone;
} PwRace;
static PwRace sRace;

static void PwLog(const char* line) {
    const size_t len = strlen(line);
    svcOutputDebugString(line, len);
    if (&gdx3ds_filelog_write != NULL) {
        gdx3ds_filelog_write(line, len);
    }
}

static void Latch(void) {
    if (sLatched || &gdx3ds_config_get_int == NULL) {
        return;
    }
    if (&gdx3ds_config_loaded != NULL && !gdx3ds_config_loaded()) {
        return;
    }
    sLatched = 1;
    sOn = gdx3ds_config_get_int("debug", "prewrite", 0) != 0;
    sMemOn = gdx3ds_config_get_int("debug", "gpumem", 0) != 0;
    if (sOn || sMemOn) {
        char line[200];
        snprintf(line, sizeof(line),
                 "[prewrite] init prewrite=%d gpumem=%d (race frames: C3D_FrameBegin's queue wait vs the CPU work "
                 "before the frame's first GPU-memory write; per race: linear / cmdbuf / VBO headroom)",
                 sOn, sMemOn);
        PwLog(line);
    }
}

static uint32_t TicksToUs(uint64_t ticks) {
    return (uint32_t)((ticks * 1000u) / (uint64_t)CPU_TICKS_PER_MSEC);
}

static uintptr_t CmdPtr(void) {
    return (uintptr_t)gpuCmdBuf + (uintptr_t)gpuCmdBufOffset * 4u;
}

static uint32_t VboFloats(void) {
    return &gdx3ds_c3d_vbo_used_floats != NULL ? gdx3ds_c3d_vbo_used_floats() : 0u;
}

void gdx3ds_pw_begin(uint64_t tEnter) {
    GdxPaceFrameInfo info;
    Latch();
    if (!sOn) {
        return;
    }
    sTRet = svcGetSystemTick();
    if (gdx3ds_pace_armed()) {
        gdx3ds_pace_frame_info(&info);
        sGsp = info.queueTicks;
        sVbw = info.vblankTicks;
        sVbwAfter = info.phase != 0;
    } else {
        sGsp = sTRet - tEnter; /* stock: SYNCDRAW's vblank wait (if any) came first */
        sVbw = 0;
        sVbwAfter = 0;
    }
    sFirst = 0;
    sFirstLo = 0;
    sFirstKind = GDX3DS_PW_NONE;
    sFirstRaw = 0;
    sDraw = 0;
    sDrawKind = GDX3DS_PW_NONE;
    gdx3ds_pw_armed = 1;
}

void gdx3ds_pw_startframe_done(void) {
    if (gdx3ds_pw_armed != 1) {
        return;
    }
    sCmd0 = CmdPtr();
    sGx0 = C3Dgdx_QueueEntries();
    sVbo0 = VboFloats();
    sSfWords = (uint32_t)gpuCmdBufOffset;
    sLastClean = svcGetSystemTick();
    gdx3ds_pw_armed = 2;
}

/* The first write of any kind, then (a GX entry could be held back cheaply: it is a queue entry, not
 * GPU-read memory) the first one that is not a GX entry; the hooks disarm after that one. */
static void Stamp(int kind, uint64_t hi, uint64_t lo, int raw) {
    if (sFirst == 0) {
        sFirst = hi;
        sFirstLo = lo;
        sFirstKind = kind;
        sFirstRaw = raw;
    }
    if (kind == GDX3DS_PW_GX) {
        sGx0 = C3Dgdx_QueueEntries(); /* the next poll looks past this entry */
        return;
    }
    sDraw = hi;
    sDrawKind = kind;
    gdx3ds_pw_armed = 0;
}

/* The kind of the write a poll sees, or -1 when nothing moved. */
static int PolledKind(void) {
    if (CmdPtr() != sCmd0) {
        return GDX3DS_PW_CMD;
    } else if (C3Dgdx_QueueEntries() != sGx0) {
        return GDX3DS_PW_GX;
    } else if (VboFloats() != sVbo0) {
        return GDX3DS_PW_VBO;
    } else {
        return -1;
    }
}

void gdx3ds_pw_poll_slow(void) {
    const uint64_t now = svcGetSystemTick();
    int kind;
    if (gdx3ds_pw_armed != 2) {
        return;
    }
    kind = PolledKind();
    if (kind < 0) {
        sLastClean = now;
        return;
    }
    Stamp(kind, now, sLastClean, 1);
}

void gdx3ds_pw_note_slow(int kind) {
    const uint64_t now = svcGetSystemTick();
    int polled;
    if (gdx3ds_pw_armed != 2) {
        return; /* inside StartFrame: its own setup (counted by sf=) */
    }
    polled = PolledKind();
    if (polled >= 0) {
        Stamp(polled, now, sLastClean, 1); /* an earlier raw write since the last clean poll */
        return;
    }
    Stamp(kind, now, now, 0);
}

static void MsField(char* buf, size_t cap, uint64_t us) {
    snprintf(buf, cap, "%lu.%lu", (unsigned long)(us / 1000u), (unsigned long)((us % 1000u) / 100u));
}

static int CmpU32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a;
    const uint32_t y = *(const uint32_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* "avg/p90/max" of the first n samples (sorted in place). */
static void Stat3(char* buf, size_t cap, uint32_t* v, unsigned n) {
    char a[16], p[16], m[16];
    uint64_t sum = 0;
    unsigned i;
    if (n == 0) {
        snprintf(buf, cap, "-");
        return;
    }
    for (i = 0; i < n; i++) {
        sum += v[i];
    }
    qsort(v, n, sizeof(v[0]), CmpU32);
    MsField(a, sizeof(a), sum / n);
    MsField(p, sizeof(p), v[(n * 9u) / 10u < n ? (n * 9u) / 10u : n - 1u]);
    MsField(m, sizeof(m), v[n - 1u]);
    snprintf(buf, cap, "%s/%s/%s", a, p, m);
}

static void EmitWindow(void) {
    char line[720];
    char gsp[40], pre[40], hide[40], vbw[16], gap[16], draw[40], hided[40];
    const PwWin* w = &sWin;
    if (w->n == 0) {
        return;
    }
    Stat3(gsp, sizeof(gsp), sWin.gsp, w->n);
    Stat3(pre, sizeof(pre), sWin.pre, w->n);
    Stat3(hide, sizeof(hide), sWin.hide, w->hideN);
    Stat3(draw, sizeof(draw), sWin.draw, w->n);
    Stat3(hided, sizeof(hided), sWin.hided, w->hideN);
    MsField(vbw, sizeof(vbw), w->vbwUs / w->n);
    MsField(gap, sizeof(gap), w->b2b != 0 ? w->b2bGspUs / w->b2b : 0);
    snprintf(line, sizeof(line),
             "[prewrite] frame=%lu n=%u b2b=%u 3d=%u exposed=%u | gsp=%s pre=%s hide=%s ms (avg/p90/max) | "
             "draw=%s hided=%s ms | vbw=%s b2b_gsp=%s ms | first cmd=%u vbo=%u tex=%u gx=%u flush=%u none=%u raw=%u "
             "width=%lu us | drawfirst cmd=%u vbo=%u tex=%u flush=%u none=%u | sf=%u/%luw",
             sFrames, w->n, w->b2b, w->stereo, w->exposed, gsp, pre, hide, draw, hided, vbw, gap,
             w->kinds[GDX3DS_PW_CMD], w->kinds[GDX3DS_PW_VBO], w->kinds[GDX3DS_PW_TEX], w->kinds[GDX3DS_PW_GX],
             w->kinds[GDX3DS_PW_FLUSH], w->kinds[GDX3DS_PW_NONE], w->raw, (unsigned long)(w->widthUs / w->n),
             w->drawKinds[GDX3DS_PW_CMD], w->drawKinds[GDX3DS_PW_VBO], w->drawKinds[GDX3DS_PW_TEX],
             w->drawKinds[GDX3DS_PW_FLUSH], w->drawKinds[GDX3DS_PW_NONE], w->sfFrames,
             (unsigned long)(w->sfFrames != 0 ? w->sfWords / w->sfFrames : 0));
    PwLog(line);
    memset(&sWin, 0, sizeof(sWin));
}

static void NoteFrame(void) {
    const uint64_t first = sFirst != 0 ? sFirst : svcGetSystemTick();
    const uint64_t lo = sFirstLo != 0 ? sFirstLo : first;
    const uint32_t gspUs = TicksToUs(sGsp);
    const uint32_t vbwUs = TicksToUs(sVbw);
    const uint32_t preUs = first > sTRet ? TicksToUs(first - sTRet) : 0u;
    const uint64_t draw = sDraw != 0 ? sDraw : svcGetSystemTick();
    const uint32_t drawUs = draw > sTRet ? TicksToUs(draw - sTRet) : 0u;
    const int b2b = &gdx3ds_frameskip_render_gap == NULL || gdx3ds_frameskip_render_gap() == 0;
    const int exposed = !(sVbwAfter && vbwUs >= PW_EXPOSED_VBW_US);
    PwWin* w = &sWin;
    const unsigned i = w->n;
    w->n++;
    w->stereo += (&gdx3ds_stereo_active != NULL && gdx3ds_stereo_active()) ? 1u : 0u;
    if (sSfWords != 0) {
        w->sfFrames++;
        w->sfWords += sSfWords;
    }
    w->gsp[i] = gspUs;
    w->pre[i] = preUs;
    w->vbwUs += vbwUs;
    w->widthUs += first > lo ? TicksToUs(first - lo) : 0u;
    w->draw[i] = drawUs;
    w->kinds[sFirstKind]++;
    w->drawKinds[sDrawKind]++;
    w->raw += sFirstRaw ? 1u : 0u;
    if (b2b) {
        w->b2b++;
        w->b2bGspUs += gspUs;
    }
    if (exposed) {
        w->exposed++;
        w->hide[w->hideN] = gspUs < preUs ? gspUs : preUs;
        w->hided[w->hideN] = gspUs < drawUs ? gspUs : drawUs;
        w->hideN++;
    }
    if (w->n >= PW_WINDOW) {
        EmitWindow();
    }
}

/* ---- [gpumem] ---- */

static void RaceReset(int lockstep) {
    memset(&sRace, 0, sizeof(sRace));
    sRace.lockstep = lockstep;
    sRace.linMin = (uint32_t)linearSpaceFree();
    sRace.linStart = sRace.linMin;
    sRace.vramMin = (uint32_t)vramSpaceFree();
    sRace.cmdSize = C3Dgdx_CmdBufWords();
}

static void ContigProbe(void) {
    static const uint32_t kSizes[] = { 2048u * 1024u, 1536u * 1024u, 1024u * 1024u, 512u * 1024u };
    unsigned i;
    sRace.contigDone = 1;
    for (i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
        void* p = linearAlloc(kSizes[i]);
        if (p != NULL) {
            linearFree(p);
            sRace.contig = kSizes[i];
            return;
        }
    }
}

static unsigned PctX10(uint64_t num, uint64_t den) {
    return den != 0 ? (unsigned)((num * 1000u) / den) : 0u;
}

static void EmitRace(const char* what) {
    char line[480];
    const PwRace* r = &sRace;
    const uint64_t cmdAvg = r->cmdN != 0 ? r->cmdSum / r->cmdN : 0;
    const uint64_t vboAvg = r->vboN != 0 ? r->vboSum / r->vboN : 0;
    const unsigned cmdPct = PctX10(r->cmdMax, r->cmdSize);
    const unsigned vboPct = PctX10(r->vboMax, r->vboPool);
    snprintf(line, sizeof(line),
             "[gpumem] race=%u %s lockstep=%d frames=%lu 3d=%lu | linear free min=%luK start=%luK contig=%s%luK | "
             "vram free min=%luK | cmdbuf max=%lu avg=%lu of=%lu words (%u.%u%%) | vbo max=%luK avg=%luK of=%luK "
             "(%u.%u%%) | halves cmd=%s vbo=%s",
             sRaceNo, what, r->lockstep, r->frames, r->stereo, (unsigned long)(r->linMin / 1024u),
             (unsigned long)(r->linStart / 1024u), r->contigDone ? "" : "?", (unsigned long)(r->contig / 1024u),
             (unsigned long)(r->vramMin / 1024u), (unsigned long)r->cmdMax, (unsigned long)cmdAvg,
             (unsigned long)r->cmdSize, cmdPct / 10u, cmdPct % 10u, (unsigned long)(r->vboMax / 1024u),
             (unsigned long)(vboAvg / 1024u), (unsigned long)(r->vboPool / 1024u), vboPct / 10u, vboPct % 10u,
             (r->cmdSize != 0 && r->cmdMax * 2u <= r->cmdSize) ? "fits" : "no",
             (r->vboPool != 0 && r->vboMax * 2u <= r->vboPool) ? "fits" : "no");
    PwLog(line);
}

/* The race edge, seen from the frame-building thread. */
static void RaceEdge(void) {
    const int req = sRaceReq;
    const int want = req & 1;
    if (want == sRaceOn) {
        return;
    }
    sRaceOn = want;
    if (want) {
        sRaceNo++;
        memset(&sWin, 0, sizeof(sWin));
        if (sMemOn) {
            RaceReset((req >> 1) & 1);
        }
        return;
    }
    if (sOn && sWin.n >= 32u) {
        EmitWindow();
    }
    memset(&sWin, 0, sizeof(sWin));
    if (sMemOn && sRace.frames != 0) {
        EmitRace("end");
    }
}

static void RaceFrame(uint32_t vboBytes, uint32_t vboPoolBytes) {
    PwRace* r = &sRace;
    r->frames++;
    r->stereo += (&gdx3ds_stereo_active != NULL && gdx3ds_stereo_active()) ? 1u : 0u;
    r->vboPool = vboPoolBytes;
    r->vboSum += vboBytes;
    r->vboN++;
    if (vboBytes > r->vboMax) {
        r->vboMax = vboBytes;
    }
    if ((r->frames % PW_LINEAR_EVERY) == 0u) {
        const uint32_t lin = (uint32_t)linearSpaceFree();
        const uint32_t vram = (uint32_t)vramSpaceFree();
        if (lin < r->linMin) {
            r->linMin = lin;
        }
        if (vram < r->vramMin) {
            r->vramMin = vram;
        }
    }
    if (!r->contigDone && r->frames >= PW_CONTIG_AT) {
        ContigProbe();
    }
    if ((r->frames % PW_CHECKPOINT) == 0u) {
        EmitRace("checkpoint");
    }
}

void gdx3ds_pw_frame_end(uint32_t vboBytes, uint32_t vboPoolBytes) {
    const int armed = gdx3ds_pw_armed;
    gdx3ds_pw_armed = 0;
    Latch();
    if (!sOn && !sMemOn) {
        return;
    }
    RaceEdge();
    if (!sRaceOn) {
        return;
    }
    if (sOn && sTRet != 0) {
        if (armed != 0 && sFirst == 0) {
            sFirstKind = GDX3DS_PW_NONE; /* nothing written after StartFrame before the frame end */
        }
        sFrames++;
        NoteFrame();
    }
    sTRet = 0;
    if (sMemOn) {
        RaceFrame(vboBytes, vboPoolBytes);
    }
}

void gdx3ds_pw_after_frame_end(void) {
    uint32_t words;
    if (!sMemOn || !sRaceOn) {
        return;
    }
    words = (uint32_t)(C3D_GetCmdBufUsage() * (float)sRace.cmdSize + 0.5f);
    sRace.cmdSum += words;
    sRace.cmdN++;
    if (words > sRace.cmdMax) {
        sRace.cmdMax = words;
    }
}

int gdx3ds_pw_enabled(void) {
    Latch();
    return sOn || sMemOn;
}

void gdx3ds_pw_set_race(int inRace, int lockstep) {
    sRaceReq = (inRace ? 1 : 0) | (lockstep ? 2 : 0);
}
