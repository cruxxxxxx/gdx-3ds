/* port/3ds/gfx/gdx3ds_gputl.c -- n60gpu: [debug] gputl (contract in the header). */
#include "gdx3ds_gputl.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "../vendor/citro3d/source/internal.h"

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern void gdx3ds_boot_trail(const char* line) __attribute__((weak)); /* n60tail: include/gdx3ds_boot_trail.h */
extern int gdx3ds_zclr_phase(void) __attribute__((weak)); /* gfx_citro3d.cpp [native] zclear_elide */
extern int gdx3ds_stereo_tail_phase(void) __attribute__((weak)); /* n60tail: stereo_fold (1) / stereo_xl_early (2) */
extern int gdx3ds_skylate_phase(void) __attribute__((weak));     /* n60tail: [native] sky_late (4) */
/* onesubmit [debug] gxcost (port/3ds/gdx3ds_gxcost.c): counts the GX interrupts these callbacks see. */
extern void gdx3ds_gxcost_on_event(int evt) __attribute__((weak));

enum {
    TL_RING = 256,     /* completion stamps; ~25 per 3D frame, drained every frame */
    TL_MAXENT = 32,    /* citro3d's queue capacity */
    TL_SNAPS = 12,     /* 2 per kick (OVERLAP_MAX_KICKS 4) + the frame end */
    TL_CHUNKS = 3,
    TL_TINY_WORDS = 96,
    TL_HEAD_WORDS = 24, /* stereo-race-fps: a long list's head scanned for a framebuffer bind */
    TL_KEEP = 256,     /* per-frame values kept for the window's percentiles */
    TL_NOTES = 8,      /* n60tail: folded lists per frame (2 kicks + the frame end) */
};

enum { K_LIST = 0, K_FILL = 1, K_XFER = 2, K_COPY = 3, K_OTHER = 4 };
enum { EV_PSC0 = 0, EV_PPF = 4, EV_P3D = 5 };

typedef struct {
    u64 t;
    u16 done;
    u8 evt;
} Stamp;

static int sOn = -1;
static unsigned sWindow = 256;
static gxCmdQueue_s* sQueue = NULL;

static Stamp sRing[TL_RING];
static volatile u32 sHead = 0; /* written by the GSP event thread only */
static u32 sTail = 0;

/* the frame recorded at its end, accounted at the next begin */
typedef struct {
    u8 kind, eye, chunk, tiny;
} Ent;
static Ent sEnt[TL_MAXENT];
static int sN = 0;
static u64 sSnapTick[TL_SNAPS];
static int sSnapN[TL_SNAPS];
static u8 sSnapChunk[TL_SNAPS];
static int sSnaps = 0;
static int sKicks = 0;   /* kicks so far in the frame being built */
static int sFrameKicks = 0; /* ...of the recorded frame */
static u64 sEndTick = 0;
static int sPending = 0;
/* n60tail: lists too long to scan whose END switches the colour buffer (a right prologue folded into
 * the closing left list, stereo_fold): the eye after them, noted by the stereo frame code. */
static int sNoteEntry[TL_NOTES];
static u8 sNoteEye[TL_NOTES];
static int sNotes = 0;
static int sEndPhase = 0; /* the recorded frame's lever phase: zc | tp << 1 */

typedef struct {
    unsigned frames, miss, k2frames, overlap, tinyN, fills, xfers, copies, entries;
    double eye[4];               /* busy ms by eye of the lists (2 = off-screen, 3 = both: onesubmit) */
    double chunk[2][TL_CHUNKS];  /* busy ms by eye and chunk, frames with 2 kicks only */
    double fill, xfer, copy, tiny, idle, tail, span;
    float keepLR[TL_KEEP], keepTail[TL_KEEP];
    unsigned kept;
    int zc; /* [native] zclear_elide phase of the window's last frame */
    int tp; /* n60tail: stereo_fold 1 | stereo_xl_early 2 | sky_late 4 in force in the window */
} Win;
static Win sWin;
static unsigned long sFrame = 0;

static void TlLog(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    svcOutputDebugString(buf, len);
    if (&gdx3ds_filelog_write != NULL) {
        gdx3ds_filelog_write(buf, len);
    }
}

static double Ms(u64 ticks) {
    return (double)ticks / (double)CPU_TICKS_PER_MSEC;
}

/* GSP event thread: right after gxCmdQueueInterrupt counted the entry and submitted the next. */
static void OnGpuEvent(void* data) {
    const u32 h = sHead;
    Stamp* s = &sRing[h & (TL_RING - 1)];
    s->t = svcGetSystemTick();
    s->done = sQueue != NULL ? sQueue->lastEntry : 0;
    s->evt = (u8)(uintptr_t)data;
    __sync_synchronize();
    sHead = h + 1u;
    if (&gdx3ds_gxcost_on_event != NULL) {
        gdx3ds_gxcost_on_event((int)(uintptr_t)data); /* EV_* are the GSPGPU_EVENT_* numbers */
    }
}

int gdx3ds_gputl_on(void) {
    if (sOn >= 0) {
        return sOn;
    }
    sOn = 0;
    if (&gdx3ds_config_get_int != NULL) {
        sOn = gdx3ds_config_get_int("debug", "gputl", 0) != 0 ? 1 : 0;
        const int w = gdx3ds_config_get_int("debug", "gputl_window", 256);
        sWindow = w > 0 ? (unsigned)w : 256u;
    }
    if (sOn) {
        sQueue = &C3Di_GetContext()->gxQueue;
        gspSetEventCallback(GSPGPU_EVENT_PSC0, OnGpuEvent, (void*)(uintptr_t)EV_PSC0, false);
        gspSetEventCallback(GSPGPU_EVENT_PPF, OnGpuEvent, (void*)(uintptr_t)EV_PPF, false);
        gspSetEventCallback(GSPGPU_EVENT_P3D, OnGpuEvent, (void*)(uintptr_t)EV_P3D, false);
        TlLog("[gputl] init window=%u (GSP PSC0/PPF/P3D callbacks: per-entry completion stamps; busy = done - "
              "max(prev done, submit))",
              sWindow);
        /* n60tail: the boot trail brackets the registration (a no-op once the trail has closed). */
        if (&gdx3ds_boot_trail != NULL) {
            gdx3ds_boot_trail("[boot] gputl: GSP PSC0/PPF/P3D completion callbacks registered (first frame begin)");
        }
    }
    return sOn;
}

static int CmpF(const void* a, const void* b) {
    const float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static float Pct(float* v, unsigned n, float p) {
    if (n == 0u) {
        return 0.0f;
    }
    qsort(v, n, sizeof(float), CmpF);
    unsigned i = (unsigned)(p * (float)(n - 1u) + 0.5f);
    return v[i < n ? i : n - 1u];
}

/* n60tail: once per phase (zc / tp) seen, the classified entry sequence of a 2-kick frame, e.g.
 * "F F l l l l l X | l l X": F fill, l/r list of the left / right eye (o off-screen), X transfer,
 * '|' where a snapshot (kick or frame end) made the following entries available. */
static void LogSeq(void) {
    /* onesubmit: tp bits 32 / 64 / 128 (gdx3ds_stereo_tail_phase) outgrew a 64-bit seen mask */
    enum { kSeenMax = 32 };
    static int seenPhases[kSeenMax];
    static int seenN = 0;
    const int noBodyKicks = ((sEndPhase >> 1) & 64) != 0; /* onesubmit_kicks: the head run is the one kick */
    if (sFrameKicks != 2 && !(noBodyKicks && sFrameKicks >= 1)) {
        return;
    }
    for (int i = 0; i < seenN; i++) {
        if (seenPhases[i] == sEndPhase) {
            return;
        }
    }
    if (seenN >= kSeenMax) {
        return;
    }
    seenPhases[seenN++] = sEndPhase;
    char seq[160];
    int n = 0;
    int snap = 0;
    for (int k = 0; k < sN && n < (int)sizeof(seq) - 8; k++) {
        while (snap < sSnaps && sSnapN[snap] <= k) {
            snap++;
            if (k > 0) {
                seq[n++] = '|';
            }
        }
        const Ent* e = &sEnt[k];
        static const char kList[4] = { 'l', 'r', 'o', 'b' };
        char c = '?';
        if (e->kind == K_LIST) {
            c = kList[e->eye < 4 ? e->eye : 2];
            if (e->tiny) {
                c = (char)(c - 'a' + 'A'); /* tiny lists in capitals */
            }
        } else if (e->kind == K_FILL) {
            c = 'F';
        } else if (e->kind == K_XFER) {
            c = e->eye == 0 ? 'X' : (e->eye == 1 ? 'Y' : 'Z'); /* X left, Y right */
        } else if (e->kind == K_COPY) {
            c = 'C';
        }
        seq[n++] = c;
    }
    seq[n] = '\0';
    TlLog("[gputl] seq zc=%d tp=%d n=%d %s (F fill, l/r body list L/R eye, b both eyes (onesubmit), L/R/O tiny list, X/Y left/right transfer, | a submit point)",
          sEndPhase & 1, sEndPhase >> 1, sN, seq);
}

static void EmitWindow(void) {
    const unsigned f = sWin.frames > 0u ? sWin.frames : 1u;
    const unsigned k2 = sWin.k2frames > 0u ? sWin.k2frames : 1u;
    const float p50lr = Pct(sWin.keepLR, sWin.kept, 0.5f);
    const float p90lr = Pct(sWin.keepLR, sWin.kept, 0.9f);
    const float maxlr = Pct(sWin.keepLR, sWin.kept, 1.0f);
    const float p50t = Pct(sWin.keepTail, sWin.kept, 0.5f);
    const float p90t = Pct(sWin.keepTail, sWin.kept, 0.9f);
    const float maxt = Pct(sWin.keepTail, sWin.kept, 1.0f);
    TlLog("[gputl] frame=%lu n=%u zc=%d tp=%d miss=%u | busy ms/f L=%.2f R=%.2f B=%.2f off=%.2f fill=%.2f (%.2f/f) xfer=%.2f (%.2f/f) "
          "copy=%.2f | L+R p50=%.2f p90=%.2f max=%.2f | tail p50=%.2f p90=%.2f max=%.2f | idle=%.2f span=%.2f "
          "tiny=%.0fus x%.1f ovl=%u ent=%.1f",
          sFrame, sWin.frames, sWin.zc, sWin.tp, sWin.miss, sWin.eye[0] / f, sWin.eye[1] / f, sWin.eye[3] / f, sWin.eye[2] / f, sWin.fill / f,
          (double)sWin.fills / f, sWin.xfer / f, (double)sWin.xfers / f, sWin.copy / f, p50lr, p90lr, maxlr, p50t,
          p90t, maxt, sWin.idle / f, sWin.span / f,
          sWin.tinyN > 0u ? 1000.0 * sWin.tiny / sWin.tinyN : 0.0, (double)sWin.tinyN / f, sWin.overlap,
          (double)sWin.entries / f);
    if (sWin.k2frames > 0u) {
        TlLog("[gputl] frame=%lu chunks (frames with 2 kicks: %u) L c0=%.2f c1=%.2f c2=%.2f | R c0=%.2f c1=%.2f "
              "c2=%.2f (c0 sky..dress, c1 mach..fx, c2 hud)",
              sFrame, sWin.k2frames, sWin.chunk[0][0] / k2, sWin.chunk[0][1] / k2, sWin.chunk[0][2] / k2,
              sWin.chunk[1][0] / k2, sWin.chunk[1][1] / k2, sWin.chunk[1][2] / k2);
    }
    const int zc = sWin.zc;
    const int tp = sWin.tp;
    memset(&sWin, 0, sizeof(sWin));
    sWin.zc = zc;
    sWin.tp = tp;
}

/* The snapshot that made entry k available: the first one whose entry count exceeds k. */
static int SnapOf(int k) {
    int snap = 0;
    while (snap < sSnaps - 1 && sSnapN[snap] <= k) {
        snap++;
    }
    return snap;
}

/* Assign the drained stamps to the recorded entries and account the frame. */
static void AccountFrame(void) {
    u64 done[TL_MAXENT];
    int have[TL_MAXENT];
    memset(have, 0, sizeof(have));
    /* completion order per engine = submission order of that engine's entries */
    int nextOf[3] = {0, 0, 0}; /* list, fill, ppf */
    const u32 head = sHead;
    __sync_synchronize();
    for (; sTail != head; sTail++) {
        const Stamp* s = &sRing[sTail & (TL_RING - 1)];
        const int eng = s->evt == EV_P3D ? 0 : (s->evt == EV_PSC0 ? 1 : 2);
        int k = nextOf[eng];
        while (k < sN) {
            const u8 kind = sEnt[k].kind;
            const int match = eng == 0 ? kind == K_LIST : (eng == 1 ? kind == K_FILL : (kind == K_XFER || kind == K_COPY));
            if (match) {
                break;
            }
            k++;
        }
        if (k < sN) {
            done[k] = s->t;
            have[k] = 1;
            nextOf[eng] = k + 1;
        }
    }
    for (int k = 0; k < sN; k++) {
        if (!have[k] && sEnt[k].kind != K_OTHER) {
            sWin.miss++;
            return;
        }
    }
    double lr = 0.0;
    u64 prev = 0;
    u64 first = 0;
    u64 last = 0;
    for (int k = 0; k < sN; k++) {
        if (sEnt[k].kind == K_OTHER) {
            continue;
        }
        const u64 avail = sSnapTick[SnapOf(k)];
        const int backlogged = prev > avail; /* the GPU went straight from entry k-1 to k */
        u64 start = backlogged ? prev : avail;
        if (prev != 0 && avail > prev) {
            sWin.idle += Ms(avail - prev);
        }
        if (first == 0) {
            first = start;
        }
        double busy = 0.0;
        if (done[k] >= start) {
            busy = Ms(done[k] - start);
        } else {
            sWin.overlap++; /* completed before the previous entry: engines overlapped */
        }
        if (done[k] > last) {
            last = done[k];
        }
        prev = done[k] > prev ? done[k] : prev;
        const Ent* e = &sEnt[k];
        switch (e->kind) {
            case K_LIST:
                sWin.eye[e->eye] += busy;
                if (e->eye != 2) { /* L, R, or both (onesubmit) */
                    lr += busy;
                }
                if (e->tiny && backlogged) { /* a tiny list's busy = the per-entry GSP / dispatch cost */
                    sWin.tiny += busy;
                    sWin.tinyN++;
                }
                break;
            case K_FILL:
                sWin.fill += busy;
                sWin.fills++;
                break;
            case K_XFER:
                sWin.xfer += busy;
                sWin.xfers++;
                break;
            default:
                sWin.copy += busy;
                sWin.copies++;
                break;
        }
    }
    if (sFrameKicks == 2) { /* two kicks + the frame end: c0 sky..dress, c1 mach..fx, c2 hud */
        u64 p = 0;
        for (int k = 0; k < sN; k++) {
            if (sEnt[k].kind != K_LIST || sEnt[k].eye > 1) {
                if (sEnt[k].kind != K_OTHER && have[k]) {
                    p = done[k] > p ? done[k] : p;
                }
                continue;
            }
            const int snap = SnapOf(k);
            const u64 avail = sSnapTick[snap];
            const u64 start = p > avail ? p : avail;
            if (done[k] >= start && sSnapChunk[snap] < TL_CHUNKS) {
                sWin.chunk[sEnt[k].eye][sSnapChunk[snap]] += Ms(done[k] - start);
            }
            p = done[k] > p ? done[k] : p;
        }
        sWin.k2frames++;
    }
    const double tail = last > sEndTick ? Ms(last - sEndTick) : 0.0;
    sWin.tail += tail;
    sWin.span += last > first ? Ms(last - first) : 0.0;
    sWin.entries += (unsigned)sN;
    if (sWin.kept < TL_KEEP) {
        sWin.keepLR[sWin.kept] = (float)lr;
        sWin.keepTail[sWin.kept] = (float)tail;
        sWin.kept++;
    }
    sWin.frames++;
}

void gdx3ds_gputl_frame_begin(void) {
    if (!gdx3ds_gputl_on()) {
        return;
    }
    if (sPending) {
        const int zc = sEndPhase & 1;
        const int tp = sEndPhase >> 1;
        if (sWin.frames + sWin.miss > 0u && (zc != sWin.zc || tp != sWin.tp)) {
            EmitWindow(); /* an A/B phase change closes the window: each line is one phase */
        }
        sWin.zc = zc;
        sWin.tp = tp;
        AccountFrame();
        sPending = 0;
        if (sWin.frames + sWin.miss >= sWindow) {
            EmitWindow();
        }
    } else {
        sTail = sHead; /* stamps of work we did not record (captures, boot) */
    }
    sSnaps = 0;
    sKicks = 0;
    sNotes = 0;
    sFrame++;
}

void gdx3ds_gputl_note_eye(int entry, int eye) {
    if (sOn <= 0 || sNotes >= TL_NOTES) {
        return;
    }
    sNoteEntry[sNotes] = entry;
    sNoteEye[sNotes] = (u8)eye;
    sNotes++;
}

/* The eye a noted (folded) list leaves bound, else `eye`. A list noted 3 (onesubmit: both eyes in
 * one list) leaves the right eye bound; frame_end classifies the list itself as 3. */
static int NotedEye(int k, int eye) {
    for (int i = 0; i < sNotes; i++) {
        if (sNoteEntry[i] == k) {
            return sNoteEye[i];
        }
    }
    return eye;
}

static void Snap(void) {
    if (sOn <= 0 || sQueue == NULL || sSnaps >= TL_SNAPS) {
        return;
    }
    sSnapTick[sSnaps] = svcGetSystemTick();
    sSnapN[sSnaps] = sQueue->numEntries;
    sSnapChunk[sSnaps] = (u8)sKicks;
    sSnaps++;
}

void gdx3ds_gputl_note_kick(int phase) {
    if (sOn <= 0) {
        return;
    }
    Snap();
    if (phase != 0) {
        sKicks++;
    }
}

/* The eye a small list leaves bound (a COLORBUFFER_LOC write), else `eye`. */
static int ScanEye(const u32* w, u32 nwords, u32 mainPa, u32 rightPa, int eye) {
    u32 i = 0;
    while (i + 1u < nwords) {
        const u32 hdr = w[i + 1u];
        const u32 reg = hdr & 0xFFFFu;
        const u32 extra = (hdr >> 20) & 0xFFu;
        const int seq = (int)(hdr >> 31);
        for (u32 k = 0; k <= extra; k++) {
            const u32 r = seq ? reg + k : reg;
            if (r == 0x11Du) {
                const u32 idx = k == 0u ? i : i + 1u + k;
                if (idx < nwords) {
                    const u32 pa = w[idx] << 3;
                    eye = pa == mainPa ? 0 : (pa == rightPa && rightPa != 0u ? 1 : 2);
                }
            }
        }
        i += 2u + extra;
        i = (i + 1u) & ~1u;
    }
    return eye;
}

void gdx3ds_gputl_frame_end(const void* mainColor, const void* rightColor) {
    if (sOn <= 0 || sQueue == NULL) {
        return;
    }
    Snap(); /* C3D_FrameEnd ran the queue: its entries are available from (about) now */
    sEndPhase = (&gdx3ds_zclr_phase != NULL ? (gdx3ds_zclr_phase() & 1) : 0) |
                ((&gdx3ds_stereo_tail_phase != NULL ? gdx3ds_stereo_tail_phase() : 0) << 1) |
                ((&gdx3ds_skylate_phase != NULL ? gdx3ds_skylate_phase() : 0) << 1);
    sEndTick = sSnapTick[sSnaps > 0 ? sSnaps - 1 : 0];
    sFrameKicks = sKicks;
    const u32 mainPa = mainColor != NULL ? osConvertVirtToPhys(mainColor) : 0u;
    const u32 rightPa = rightColor != NULL ? osConvertVirtToPhys(rightColor) : 0u;
    const int n = sQueue->numEntries < TL_MAXENT ? sQueue->numEntries : TL_MAXENT;
    int eye = 0;
    for (int k = 0; k < n; k++) {
        const gxCmdEntry_s* en = &sQueue->entries[k];
        const u32 id = en->data[0] & 0xFFu;
        Ent* e = &sEnt[k];
        e->tiny = 0;
        e->eye = (u8)eye;
        if (id == 0x01u) {
            const u32 words = en->data[2] / 4u;
            e->kind = K_LIST;
            if (words <= TL_TINY_WORDS) {
                e->tiny = 1;
                eye = ScanEye((const u32*)en->data[1], words, mainPa, rightPa, eye);
                e->eye = (u8)eye;
            } else {
                /* a body list: the eye bound when it starts -- stereo-race-fps resume_fold: a left list
                 * may begin with the left eye's resume words (its framebuffer bind in the first words) */
                eye = ScanEye((const u32*)en->data[1], TL_HEAD_WORDS, mainPa, rightPa, eye);
                e->eye = (u8)eye;
                eye = NotedEye(k, eye);
                if (eye == 3) {
                    e->eye = 3;
                    eye = 1;
                }
            }
        } else if (id == 0x02u) {
            e->kind = K_FILL;
        } else if (id == 0x03u) {
            e->kind = K_XFER;
            const u32 pa = osConvertVirtToPhys((const void*)en->data[1]);
            e->eye = (u8)(pa == mainPa ? 0 : (pa == rightPa && rightPa != 0u ? 1 : 2));
        } else if (id == 0x04u) {
            e->kind = K_COPY;
        } else {
            e->kind = K_OTHER;
        }
    }
    sN = n;
    /* chunk boundaries: the snapshots taken at the kicks and here; a snapshot that made no new entry
     * available (nothing queued since the previous one) is dropped */
    int w = 0;
    for (int s = 0; s < sSnaps; s++) {
        if (w > 0 && sSnapN[s] == sSnapN[w - 1]) {
            continue;
        }
        sSnapTick[w] = sSnapTick[s];
        sSnapN[w] = sSnapN[s];
        sSnapChunk[w] = sSnapChunk[s];
        w++;
    }
    sSnaps = w;
    LogSeq();
    sPending = 1;
}
