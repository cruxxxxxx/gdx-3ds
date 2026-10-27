/* port/3ds/gfx/n64/gdx_nirace.cpp -- nirace (docs/research/native/nirace-progress.md): the walk
 * between the NRR markers of a race frame.
 *
 * [native] ni_race (0 off, 1 on, 2 verify): SPAN PROGRAMS. A race frame's root list is the frame
 * prologue (segment table, the RDP setup list), Race_Draw's setup words and setup lists, the camera
 * and a handful of markers (0x61 category, 0x63 NRR, 0x5E HUD, 0x6C direct scope) that hand the
 * real work to the NRR. What the walker still dispatched between those markers -- ~190 commands per
 * steady frame, ~45 of them at the root, the rest in a few static setup lists -- is a SPAN: the root
 * words from one root marker to the next. Each span is compiled once (cold) into a program; the
 * marker that precedes it runs the program instead of handing its words to Walk / Exec:
 *   ST   consecutive root words that are pure register writes (other modes, combine, tiles, colours,
 *        geometry mode, texture, scissor, syncs, the no-op movewords), each word's handler compiled
 *        as masked stores (the NRR HUD's complement trick, NrrRegCompile: the handler runs on the
 *        live registers and on their complement), the stores of a group merged; each word of the
 *        group is checked against the words the group was compiled from; the tile words (and
 *        G_TEXTURE's first tile) keep their own group for the texture arm; the colour words that
 *        flush a non-empty batch need an empty one;
 *   SEG  a segment moveword: the bridge's segment set with the live word (what SpMoveword does);
 *   EX   any other word (matrices, viewport, colour / z image, loads, the rare draw): Exec with
 *        the live word -- the stock handler, without the walk;
 *   DL   a G_DL: the resolve and the G_DL hook exactly as Exec runs them, then the resolved list
 *        must be the one compiled (base, bound, stride, class, every word -- the SL op of n60hudx),
 *        then the list's own words as nested ST / SEG / EX ops.
 * Every op is either the stock handler on the live word or a store set checked against the word it
 * was compiled from, so a program is exact by construction; an op that does not match (or whose
 * state precondition fails) ends the program there and the walker takes the rest of the span from
 * that word (inside a list: the list pushed at that command), so a program is always an exact
 * prefix. A word that changes between frames (a check that failed) is compiled as EX next time.
 * Deadsetup is not consulted for a compiled G_DL: the program executes the whole list, whose end
 * state is the deadsetup window's by that lever's own contract.
 *
 * [debug] nirace_trace / nirace_sum / nirace_trace_skip / nirace_trace_at1-3 (the walker's
 * commands of N racing frames, one [nitrace] line per command or one [nisum] summary line per
 * frame) were diagnostic scaffolding for the arm/residue investigation closed in nirace-progress.md
 * round 4-7; removed (W6, Fable review -- the hypothesis they existed to test is answered). */
#include "gdx_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#include "gdx_log.h"
#include "gdx3ds_hotcold.h"

/* ni_race_mesh: refuse per draw while the n60stereo frame replay is actually live THIS frame
 * (nimenu W1/H1, review finding H1). Was a per-task-begin latch of
 * `gdx3ds_stereo_active() && gdx3ds_stereo_replay_ok_this_frame()` behind two weak STUB
 * DEFINITIONS (L1); both are gone now, replaced by one plain `extern` of the real cross-frame
 * global (`gdx3ds_stereo_frame.cpp:42`, 1 while this frame's replay is live, reset to 0 at every
 * StartFrame leg, 0 outright in 2D/mono since ReplayEnter is never attempted there). No weak
 * magic needed: this TU also builds as a host test (gdx_interp_host_tests, tools/ci-3ds.sh), but
 * that link already includes gdx_nrr_hud.cpp, whose own `#ifndef __3DS__` block already defines
 * this exact symbol (=0) for the host; the real 3DS build links gdx3ds_stereo_frame.cpp instead.
 * One real definition either way -- an ordinary `extern` here is correct and ODR-safe. */
extern "C" int gdx3ds_stereo_replay_frame;

/* decomp s32 (records.c): the venue. On 3DS, decomp's own strong definition always links (weak
 * here is just the same tolerant idiom gdx3ds_track_sink.c / gdx3ds_track_resident.cpp use for
 * their own reads of this symbol). Unlike gdx3ds_stereo_replay_frame above, nothing else in the
 * narrow gdx_interp_host_tests link (tools/ci-3ds.sh) defines it, and a weak *data* symbol with no
 * definition anywhere in the link is a hard undefined-symbol error for this host's linker (unlike
 * ELF/3DS, which resolves it to a null address) -- so the host leg needs this TU's own stub. */
#ifdef __3DS__
extern "C" int gCourseIndex __attribute__((weak));
#else
extern "C" int gCourseIndex = 0; /* host test stub: decomp/src/overlays/records/records.c, the
                                     real definition, isn't linked into gdx_interp_host_tests */
#endif

namespace gdxn {

/* ---- [native] ni_race: span programs -------------------------------------------------------- */

namespace {

typedef uint32_t __attribute__((may_alias)) NiWord32;
#define NI_NO_UNROLL _Pragma("GCC unroll 1")

constexpr uint32_t kNiSlotBits = 8;
constexpr uint32_t kNiSlots = 1u << kNiSlotBits; /* span programs, open addressed by the span's key */
constexpr uint32_t kNiProbe = 8;
constexpr uint32_t kNiPoolWords = 24576;  /* every program's ops (96 KB) */
constexpr uint32_t kNiMaxOpWords = 4096;  /* one program while it is compiled */
constexpr uint32_t kNiMaxRoot = 128;      /* root words a program covers (the variable-word mask) */
constexpr uint32_t kNiMaxNested = 96;     /* commands of a list a DL op compiles */
constexpr uint32_t kNiGroupWords = 15;
constexpr uint32_t kNiGroupStores = 24;
constexpr uint32_t kNiWordStores = 8;
constexpr uint32_t kNiScratch = 512;
constexpr uint32_t kNiMaxCompiles = 12;   /* per slot, then one compile per kNiThrottle tasks */
constexpr uint32_t kNiThrottle = 64;
constexpr uint32_t kNiPlan = 160;         /* words a geometry plan covers (root span / one list) */
constexpr uint32_t kNiSegs = 256;         /* interned geometry segments */
constexpr uint32_t kNiSegMaxCmds = 96;
constexpr uint32_t kNiOccBits = 6;         /* native-vs: marker words a multi-view task keys its spans by */
constexpr uint32_t kNiOcc = 1u << kNiOccBits;
enum : uint8_t { kPlanNone = 0, kPlanSkip = 1, kPlanMesh = 2, kPlanIn = 3 };

/* op header: kind | flags | words << 8 | index << 20 (root: from the span's first word; nested: in the list) */
enum : uint32_t {
    kNiEnd = 0,  /* | end << 20: the span's words the program covers; 2 words of op counts (receipt) */
    kNiSt = 1,   /* nStores; root: words x (w0, w1, w1hi); stores x (dst, mask, val) */
    kNiSeg = 2,  /* w0 */
    kNiEx = 3,   /* w0 (its opcode is checked); words = 1, or 3 for a TEXRECT */
    kNiDl = 4,   /* w0, base, limit, ref, nCmd, the list's raw words; nested ops; kNiDlEnd */
    kNiDlEnd = 5,
    kNiCall = 6, /* w0 (its opcode is checked): the word's own handler body, called directly (NrrRaceHotCall) */
    kNiDlh = 7,  /* w0: a G_DL the G_DL hook draws (a machine body, a texture set it skips): resolve + hook */
    kNiMesh = 8, /* | run << 8: seg, cmds | nVtx << 16 -- a geometry segment (ni_race_mesh): the run of
                    triangles at the op's word, with every G_VTX of its window, drawn by the mesh path */
    kNiSkip = 9, /* w0, w1: a G_VTX a geometry segment carries (its effect is the segment's) */
    kNiGeoChk = 10, /* | n << 8: n x (w0, w1, w1hi) -- a root window's every word, checked before any of
                       them applies (no program stops inside a window: its G_VTX words have not run) */
    kNiArm = 0x10,    /* a store that changes something arms the texture flag (SETTILE / SETTILESIZE) */
    kNiArmFt = 0x20,  /* G_TEXTURE: only the first store (the first tile) arms it */
    kNiClosed = 0x40, /* a word of the group flushes a non-empty batch: the batch must be empty */
};

enum NiWhy { kWhyCheck = 0, kWhyState, kWhyDlRes, kWhyDlHook, kWhyDlMiss, kWhyBound, kWhyCount };
const char* const kWhyName[kWhyCount] = { "check", "state", "dlres", "dlhook", "dlmiss", "bound" };

inline uint32_t NiHash(uint32_t marker, uint32_t w0) {
    uint32_t h = marker * 0x9E3779B1u;
    h ^= w0 * 0x85EBCA6Bu;
    return h ^ (h >> 15);
}

/* Words whose handler writes only the register regions (NrrRegRegions) and whose effect is a
 * function of the word: compiled as masked stores. */
inline bool NiPure(uint8_t op) {
    switch (op) {
        case OP_SETTIMG:
        case OP_SETTILE:
        case OP_SETTILESIZE:
        case OP_SETCOMBINE:
        case OP_SETOTHERMODE_L:
        case OP_SETOTHERMODE_H:
        case OP_RDPSETOTHERMODE:
#ifndef EXPANSION_KIT
        case OP_SETSCISSOR: /* the EK build's ScissorPillarSafeX reads a runtime lever */
#endif
        case OP_SETPRIMCOLOR:
        case OP_SETENVCOLOR:
        case OP_SETFOGCOLOR:
        case OP_SETBLENDCOLOR:
        case OP_SETFILLCOLOR:
        case OP_SETPRIMDEPTH:
        case OP_GEOMETRYMODE:
        case OP_TEXTURE:
            return true;
        default:
            return false;
    }
}

/* The pure words whose handler flushes a non-empty batch on a change. */
inline bool NiClosed(uint8_t op) {
    return op == OP_SETPRIMCOLOR || op == OP_SETENVCOLOR || op == OP_SETFOGCOLOR || op == OP_SETBLENDCOLOR ||
           op == OP_SETPRIMDEPTH;
}

/* Words Exec runs as nothing (syncs, CULLDL, a moveword the handler ignores, a stray RDPHALF_2). */
inline bool NiInert(const Cmd& c) {
    const uint8_t op = Opcode(c.w0);
    if (op == OP_RDPLOADSYNC || op == OP_RDPPIPESYNC || op == OP_RDPTILESYNC || op == OP_RDPFULLSYNC ||
        op == OP_CULLDL || op == OP_RDPHALF_2) {
        return true;
    }
    if (op == OP_NOOP && ((c.w0 >> 16) & 0xFFu) == 0) {
        /* a plain G_NOOP inside a setup list (a root one ends the span before this is asked): the
         * marker hook does nothing with p=0, and no program runs while the NRR HUD holds citro3d */
        return true;
    }
    if (op == OP_MOVEWORD) {
        const uint8_t index = static_cast<uint8_t>(C0(c.w0, 16, 8));
        return index != MW_NUMLIGHT && index != MW_FOG && index != MW_SEGMENT;
    }
    return false;
}

inline bool NiSegment(const Cmd& c) {
    return Opcode(c.w0) == OP_MOVEWORD && C0(c.w0, 16, 8) == MW_SEGMENT;
}

inline Rgba NiRgba(uint32_t w1) {
    return Rgba{ static_cast<uint8_t>(w1 >> 24), static_cast<uint8_t>(w1 >> 16), static_cast<uint8_t>(w1 >> 8),
                 static_cast<uint8_t>(w1) };
}

/* Words NrrRaceHotCall runs (their handler body, without Exec). */
inline bool NiCallable(const Cmd& c) {
    switch (Opcode(c.w0)) {
        case OP_MTX:
        case OP_MOVEMEM:
        case OP_SETCIMG:
        case OP_SETZIMG:
        case OP_RDPHALF_1:
        case OP_LOAD_UCODE:
        case OP_TEXTURE:
        case OP_SETTIMG:
        case OP_SETPRIMCOLOR:
        case OP_SETENVCOLOR:
        case OP_SETFOGCOLOR:
        case OP_SETFILLCOLOR:
        case OP_LOADBLOCK:
        case OP_FILLRECT:
            return true;
        /* OP_DMA_IO / OP_LOADTLUT: deliberately NOT typed (size: nointerp combined-binary race
         * budget, 2026-10-15). Both are rare in a steady frame (DMA_IO: the TA ghost's stock-machine
         * lights, ~1/f only in time attack; LOADTLUT: NrrSkyWriteNightSprites' stock sprites, practice
         * only) -- EX (Exec, already resident for every other opcode) costs nothing extra to reach for
         * them. Falls through to kNiEx at compile (NrrRacePlanGeometry / the ST/EX classifier below),
         * same stock handler, same exact result; only the receipt's call/ex split moves. */
        case OP_MOVEWORD:
            return C0(c.w0, 16, 8) == MW_FOG || C0(c.w0, 16, 8) == MW_NUMLIGHT;
        default:
            return false;
    }
}

/* Never compiled: flow (the program would have to follow it). */
inline bool NiFlow(uint8_t op) {
    return op == OP_DL || op == OP_ENDDL || op == OP_BRANCH_Z || op == OP_NOOP || op == OP_SPNOOP;
}

void NiRegCopy(uint8_t* const base[3], const uint32_t len[3], uint8_t* buf, bool toRegs, bool invert) {
    uint32_t at = 0;
    for (int r = 0; r < 3; r++) {
        for (uint32_t b = 0; b < len[r]; b++) {
            if (toRegs) {
                base[r][b] = invert ? static_cast<uint8_t>(~buf[at + b]) : buf[at + b];
            } else {
                buf[at + b] = base[r][b];
            }
        }
        at += len[r];
    }
}

} // namespace

namespace {

/* the open group's stores, merged: a later store over an earlier one to the same word -- masks or'ed,
 * the later value wins where it writes (the same bits as the stores in order) */
inline bool NiMerge(uint32_t* gSt, uint32_t* gNs, const uint32_t* st, uint32_t ns) {
    for (uint32_t k = 0; k < ns; k++) {
        uint32_t e = 0;
        for (; e < *gNs && gSt[3 * e] != st[3 * k]; e++) {
        }
        if (e < *gNs) {
            gSt[3 * e + 1] |= st[3 * k + 1];
            gSt[3 * e + 2] = (gSt[3 * e + 2] & ~st[3 * k + 1]) | st[3 * k + 2];
            continue;
        }
        if (*gNs == kNiGroupStores) {
            return false;
        }
        gSt[3 * e] = st[3 * k];
        gSt[3 * e + 1] = st[3 * k + 1];
        gSt[3 * e + 2] = st[3 * k + 2];
        (*gNs)++;
    }
    return true;
}

} // namespace

struct Interp::NrrRaceSlot {
    uint32_t used;
    uint32_t noMesh;         /* the mesh path refused one of its segments: compiled without geometry */
    uint32_t marker, w0, w1; /* the key: the marker before the span (and the span's ordinal), the span's
                                first word's w0 (w1: the first sight's, diagnostic) */
    uint32_t lastUse;        /* task */
    uint32_t off;            /* the program in the pool + 1 (0: none) */
    uint32_t compiles;
    uint32_t lastCompile;    /* task */
    uint32_t recompile;      /* a word check failed since the compile */
    uint32_t varMask[kNiMaxRoot / 32]; /* root words (from the span start) compiled as EX */
};

struct Interp::NrrRace {
    int level;
    int log;           /* [debug] nirace_log: one line per compile (cold) */
    uint32_t task;
    uint32_t ordinal;  /* the task's span count: part of a span's key (several spans of a frame can
                          follow the same marker word with the same first word: the sky's loads) */
    bool multiView;    /* native-vs: a 2-4P race task, its spans keyed by NrrRaceViewKey */
    struct Occ {
        uint32_t marker, task, n; /* native-vs: a marker word and this task's spans after it so far */
    } occ[kNiOcc];
    uint32_t env[8];
    bool envValid;
    uint32_t poolUsed;
    uint32_t tmpN;
    uint32_t cSt, cSeg, cCall, cDl, cDlh, cStWords, cNested; /* the program being compiled: op counts */
    /* the open register group while compiling */
    uint32_t gN, gNs, gFlags, gRel;
    bool gRoot;
    uint32_t gW[3 * kNiGroupWords];
    uint32_t gSt[3 * kNiGroupStores];
    struct Win {
        uint32_t tasks, spans, runs, done, compiles, clears, evicts, rootWords, nestedWords;
        uint32_t st, seg, ex, call, dl, dlh, stWords, stx, markers, mesh, meshFail, plans, planNo;
        uint32_t bail[kWhyCount];
        uint32_t vOk, vBad;
    } w;
    uint32_t exOp[256]; /* the window's EX ops by opcode (the stock leftovers' inventory) */
    /* ni_race_mesh: the geometry plan of the words being compiled (index = word - planBase) */
    int ps;                   /* the plan set in use: 0 the root span, 1 the list a DL op compiles */
    size_t planBaseS[2], planEndS[2];
    uint8_t planS[2][kNiPlan];
    uint16_t planRunS[2][kNiPlan];
    uint16_t planVtxS[2][kNiPlan];
    uint32_t* planSegS[2][kNiPlan];
    uint16_t planChkS[2][kNiPlan]; /* at a window's first word: the words of the window (root check) */
    struct Seg {
        uint32_t hash, cmds;
        uint32_t* words; /* 2 x cmds (w0, w1), owned -- freed on a course change (NrrRaceTaskBegin) */
    } segs[kNiSegs];
    uint32_t nSegs;
    int lastCourse; /* gCourseIndex as of the last NrrRaceTaskBegin that saw it (W6, Fable review L2):
                        0 at NrrRace's calloc, same as a real course index, but harmless on the very
                        first task -- nSegs is already 0 then, so the "reset" is a no-op either way */
    uint32_t segBuild[2 * kNiSegMaxCmds];
    bool noMesh; /* the slot being compiled had a segment the mesh path refused: plan no geometry */
    bool menu;   /* menus-s2 [native] ni_menu_root: this task is a menu frame's root (NrrRaceArmMenu) */
    uint32_t menuCloses; /* the window's open NRR HUD hand-offs closed at a menu span's start */
    /* W1/H1: no longer a per-task latch -- `gdx3ds_stereo_replay_frame` is read live at each
     * kNiMesh/NrrRaceSegRun dispatch (gdx_nirace.cpp:776/:1153) instead of being cached here once
     * at NrrRaceTaskBegin. The old latch could be a whole frame stale under the tick-ahead pipe
     * (frame N decided one way, frame N+1's walk still saw N's verdict): see nimenu-progress.md
     * W1 and docs/research/native-review-2026-10-15.md H1. */

    GDX_COLD uint8_t PlanAt(size_t i) const {
        return (i >= planBaseS[ps] && i < planEndS[ps]) ? planS[ps][i - planBaseS[ps]] : 0;
    }
    /* A word the plan gives an op: kNiSkip / kNiMesh into the program. Returns the words consumed (0: none). */
    GDX_COLD uint32_t PlanEmit(const ListRef& L, size_t i, uint32_t rel, const Cmd& c, bool root) {
        const uint8_t k = PlanAt(i);
        const uint16_t chk = (k != 0) ? planChkS[ps][i - planBaseS[ps]] : 0;
        if (root && chk != 0) {
            Emit(kNiGeoChk | (static_cast<uint32_t>(chk) << 8) | (rel << 20));
            for (size_t q = 0; q < chk; q++) {
                const Cmd x = ReadCmd(L, i + q);
                Emit(x.w0);
                Emit(x.w1);
                Emit(x.w1hi);
            }
        }
        if (k == kPlanSkip) {
            Emit(kNiSkip | (rel << 20));
            Emit(c.w0);
            Emit(c.w1);
            return 1;
        }
        if (k == kPlanMesh) {
            const size_t p = i - planBaseS[ps];
            const uint32_t run = planRunS[ps][p];
            const uint32_t nv = planVtxS[ps][p] & 0xFFu;
            const uint32_t cmds = planVtxS[ps][p] >> 8;
            Emit(kNiMesh | (run << 8) | (rel << 20));
            Emit(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(planSegS[ps][p])));
            Emit(cmds | (nv << 16));
            (void)root;
            return run;
        }
        return 0;
    }
    uint32_t vBadLogged;
    uint32_t logBails;
    NrrRaceSlot slots[kNiSlots];
    uint32_t pool[kNiPoolWords];
    uint32_t tmp[kNiMaxOpWords];
    uint8_t fxS[kNiScratch], fxR[kNiScratch], fxP[kNiScratch];

    GDX_COLD void Emit(uint32_t v) {
        if (tmpN < kNiMaxOpWords) {
            tmp[tmpN] = v;
        }
        tmpN++;
    }
    /* Close the open register group into the program. */
    GDX_COLD void GroupClose() {
        if (gN == 0 && gNs == 0) {
            return;
        }
        cSt++;
        cStWords += gN;
        Emit(kNiSt | gFlags | (gN << 8) | (gRel << 20));
        Emit(gNs);
        if (gRoot) {
            for (uint32_t k = 0; k < 3 * gN; k++) {
                Emit(gW[k]);
            }
        }
        for (uint32_t k = 0; k < 3 * gNs; k++) {
            Emit(gSt[k]);
        }
        gN = 0;
        gNs = 0;
        gFlags = 0;
    }
    /* One word into the open group (a new group when it cannot merge, or `own`: an arm group). */
    GDX_COLD void GroupAdd(bool root, uint32_t rel, const Cmd& c, const uint32_t* st, uint32_t ns, uint32_t flags,
                           bool own) {
        const bool fits = gN < kNiGroupWords && gNs + ns <= kNiGroupStores && gRoot == root && gFlags == 0;
        if (own || !fits || (gN != 0 && gRel + gN != rel)) {
            GroupClose();
        }
        if (gN == 0 && gNs == 0) {
            gRoot = root;
            gRel = rel;
            gFlags = 0;
        }
        gW[3 * gN] = c.w0;
        gW[3 * gN + 1] = c.w1;
        gW[3 * gN + 2] = c.w1hi; /* a host-tagged operand (wide lists) */
        gN++;
        gFlags |= flags;
        if (own) {
            for (uint32_t k = 0; k < 3 * ns; k++) {
                gSt[k] = st[k];
            }
            gNs = ns;
            GroupClose();
            return;
        }
        (void)NiMerge(gSt, &gNs, st, ns);
    }
};

GDX_COLD void Interp::NrrRaceSet(int level, int log) {
    if (level <= 0 || level > 2) {
        return;
    }
    if (mNi == nullptr) {
        mNi = new (std::nothrow) NrrRace();
        if (mNi == nullptr) {
            return;
        }
        memset(static_cast<void*>(mNi), 0, sizeof(NrrRace));
    }
    mNi->level = level;
    mNi->log = log;
}

/* menus-s2 [native] ni_menu_root: a menu frame's arm (cold, once per task). The programs are the race's;
 * a menu frame plans no geometry segments (its 3D belongs to ni_course / ni_mach) and a span that starts
 * with the NRR HUD's hand-off still open closes it first (NrrRaceMissRecover). */
GDX_COLD void Interp::NrrRaceArmMenu(bool on) {
    mNiOn = on && mNi != nullptr;
    if (mNi != nullptr) {
        mNi->menu = mNiOn;
    }
}

/* The root's walk starts (the frame configuration of the task is set): the task's counters, and the
 * environment the compiled scissors rest on. */
GDX_COLD void Interp::NrrRaceTaskBegin() {
    NrrRace& N = *mNi;
    N.task++;
    N.ordinal = 0;
    N.w.tasks++;
    /* W1/H1: no more latch here -- `gdx3ds_stereo_replay_frame` is read live at each mesh dispatch
     * instead (gdx_nirace.cpp:776/:1153), so a frame's own verdict is never a frame late. */
    uint32_t env[8];
    NrrRegEnv(env);
    if (!N.envValid || memcmp(env, N.env, sizeof(env)) != 0) {
        /* the compiled scissors rest on the frame configuration: start over */
        memcpy(N.env, env, sizeof(env));
        N.envValid = true;
        memset(static_cast<void*>(N.slots), 0, sizeof(N.slots));
        N.poolUsed = 0;
        N.w.clears++;
    }
    /* W6, Fable review L2: kNiSegs (256 interned geometry segments) is never freed within a course,
     * so a long session across several courses keeps the first course's entries forever and the
     * later ones never intern past the cap -- a performance-only gap (the mesh path falls back
     * gracefully, no overflow bug). Freeing on an actual course change (gCourseIndex, the venue)
     * instead of a content-mismatch rebake (gdx3ds_mesh.cpp's DressEntry strategy doesn't apply
     * here: these entries are content-addressed by a hash of the segment's own bytes, not keyed to
     * a fixed per-decoration slot an asset reload could land back on). */
    if (&gCourseIndex != nullptr && gCourseIndex != N.lastCourse) {
        for (uint32_t k = 0; k < N.nSegs; k++) {
            free(N.segs[k].words);
        }
        N.nSegs = 0;
        N.lastCourse = gCourseIndex;
        /* the programs go with the segments their geometry ops point at -- a program that outlived
         * them ran a freed segment (race-gp-cup, the first frame of the 6th course: a guest data abort
         * in NrrRaceHotRun's geometry op; heap-layout dependent) */
        memset(static_cast<void*>(N.slots), 0, sizeof(N.slots));
        N.poolUsed = 0;
        N.w.clears++;
    }
}

GDX_COLD int Interp::NrrRaceReceipt(char* buf, int cap) {
    if (mNi == nullptr) {
        return snprintf(buf, static_cast<size_t>(cap), " prog=off");
    }
    NrrRace& N = *mNi;
    const double inv = N.w.tasks != 0 ? 1.0 / N.w.tasks : 0.0;
    uint32_t used = 0;
    for (uint32_t k = 0; k < kNiSlots; k++) {
        used += N.slots[k].off != 0 ? 1u : 0u;
    }
    int n = snprintf(buf, static_cast<size_t>(cap),
                     " stock/f=%.1f markers/f=%.1f spans=%.1f runs=%.1f done=%.1f covered root=%.1f nested=%.1f ops st=%.1f(%.1fw) "
                     "seg=%.1f call=%.1f dl=%.1f dlh=%.1f | bail",
                     (N.w.ex + N.w.stx) * inv, N.w.markers * inv, N.w.spans * inv, N.w.runs * inv, N.w.done * inv,
                     N.w.rootWords * inv, N.w.nestedWords * inv, N.w.st * inv, N.w.stWords * inv, N.w.seg * inv,
                     N.w.call * inv, N.w.dl * inv, N.w.dlh * inv);
    for (int k = 0; k < kWhyCount && n < cap; k++) {
        n += snprintf(buf + n, static_cast<size_t>(cap - n), " %s=%u", kWhyName[k], N.w.bail[k]);
    }
    if (n < cap) {
        n += snprintf(buf + n, static_cast<size_t>(cap - n), " | compiles=%u clears=%u evicts=%u slots=%u pool=%u",
                      N.w.compiles, N.w.clears, N.w.evicts, used, N.poolUsed);
    }
    if (N.menuCloses != 0 && n < cap) {
        n += snprintf(buf + n, static_cast<size_t>(cap - n), " menucloses=%.1f", N.menuCloses * inv);
        N.menuCloses = 0;
    }
    if (N.level == 2 && n < cap) {
        n += snprintf(buf + n, static_cast<size_t>(cap - n), " | vfy ok=%u bad=%u", N.w.vOk, N.w.vBad);
    }
    if (n < cap) {
        n += snprintf(buf + n, static_cast<size_t>(cap - n), " | mesh=%.1f fail=%u plans=%u planno=%u | stx=%.1f ex/f",
                      N.w.mesh * inv, N.w.meshFail, N.w.plans, N.w.planNo, N.w.stx * inv);
    }
    for (int k = 0; k < 8 && n < cap; k++) {
        uint32_t best = 0;
        int bi = -1;
        for (int o = 0; o < 256; o++) {
            if (N.exOp[o] > best) {
                best = N.exOp[o];
                bi = o;
            }
        }
        if (bi < 0) {
            break;
        }
        n += snprintf(buf + n, static_cast<size_t>(cap - n), " %02X=%.1f", bi, best * inv);
        N.exOp[bi] = 0;
    }
    memset(N.exOp, 0, sizeof(N.exOp));
    memset(&N.w, 0, sizeof(N.w));
    return n;
}

/* ---- hot: the marker dispatch and the program runner --------------------------------------- */

/* The root's walk from a span start: each span's program, then the root marker after it dispatched
 * exactly as Exec's G_NOOP case dispatches it (the NRR HUD's close, the marker hook), then the next span
 * -- until the root's ENDDL. A span the programs do not take no longer ends the drive for the rest of
 * the frame: `NrrRaceMissRecover` (cold) stock-walks that one span and hands back a root sitting on the
 * next marker/ENDDL, same as a native span would -- the loop below cannot tell the difference and keeps
 * going. Only a frame that ends INSIDE that recovery walk (an ENDDL popped past the root) returns here
 * without one. f is the root frame; `marker` the word of the marker that precedes the span (0 at the
 * root's start). */
__attribute__((noinline)) void Interp::NrrRaceHotDrive(Frame& f, uint32_t marker) {
    NrrRace& N = *mNi;
    for (;;) {
        if (!NrrRaceHotSpan(f, marker) && !NrrRaceMissRecover()) {
            return; /* the frame ended inside the recovery walk (ENDDL popped past the root) */
        }
        if (mSp != 1 || f.idx >= f.list.limit) {
            return;
        }
        const Cmd c = ReadCmd(f.list, f.idx);
        const uint8_t op = Opcode(c.w0);
        if (op != OP_NOOP && op != OP_SPNOOP) {
            return; /* the root's ENDDL (or a word a program stopped before): the walker */
        }
        f.idx++;
        N.w.markers++;
        NrrNoopDispatch(c.w0, c.w1); /* == Exec's G_NOOP (shared, nimenu W4): HudHotClose + the hook */
        marker = c.w0;
    }
}

/* One span from f.idx. true: the span is done and f.idx is at the word after it (a root marker or the
 * root's end); false: the walker takes the root from f.idx (a list may be pushed above it). */
__attribute__((noinline)) bool Interp::NrrRaceHotSpan(Frame& f, uint32_t marker) {
    if (f.idx >= f.list.limit) {
        return false;
    }
    const Cmd c = ReadCmd(f.list, f.idx);
    const uint8_t op = Opcode(c.w0);
    if (op == OP_NOOP || op == OP_SPNOOP || op == OP_ENDDL) {
        return true; /* an empty span: the next marker */
    }
    if (mNrrOpen) {
        return false; /* the NRR HUD holds citro3d: the walker's words close it */
    }
    NrrRace& N = *mNi;
    N.w.spans++;
    if (GDX_UNLIKELY(N.multiView)) {
        marker = NrrRaceViewKey(marker); /* native-vs: a 2-4P frame's key (cold) */
    } else {
        marker ^= (++N.ordinal) << 24; /* the marker's opcode byte (0x00) carries the span's ordinal */
    }
    const uint32_t h = NiHash(marker, c.w0);
    NI_NO_UNROLL
    for (uint32_t p = 0; p < kNiProbe; p++) {
        NrrRaceSlot* s = &N.slots[(h + p) & (kNiSlots - 1u)];
        if (s->used == 0) {
            break;
        }
        if (s->marker == marker && s->w0 == c.w0) {
            if (s->off != 0 && s->recompile == 0) {
                s->lastUse = N.task;
                N.w.runs++;
                return NrrRaceHotRun(N.pool + s->off - 1u, f, s);
            }
            break;
        }
    }
    return NrrRaceMiss(f, marker, c, h);
}

/* A word's handler body, called directly: Exec's case for the opcode, verbatim (the typed ops of the
 * program -- matrices, viewport / lights, the fog / light-count movewords, colour / z image, ucode,
 * texture image, colours, loads, fill). t: the word's frame (t.idx past the word). */
__attribute__((noinline)) void Interp::NrrRaceHotCall(const Cmd& c, Frame& t) {
    switch (Opcode(c.w0)) {
        case OP_MTX:
            SpMatrix(static_cast<uint8_t>(C0(c.w0, 0, 8) ^ MTX_PUSH), c, t.list);
            break;
        case OP_MOVEMEM:
            SpMovemem(static_cast<uint8_t>(C0(c.w0, 0, 8)), static_cast<uint8_t>(C0(c.w0, 8, 8) * 8), c, 0, t.list);
            break;
        case OP_MOVEWORD:
            SpMoveword(static_cast<uint8_t>(C0(c.w0, 16, 8)), static_cast<uint16_t>(C0(c.w0, 0, 16)), c);
            break;
        case OP_SETCIMG:
            DpSetColorImage(C0(c.w0, 21, 3), C0(c.w0, 19, 2), C0(c.w0, 0, 11), c);
            break;
        case OP_SETZIMG:
            DpSetZImage(c);
            break;
        case OP_RDPHALF_1:
            mBranchZValid = false; /* compiled only where the next word is not a G_BRANCH_Z */
            break;
        case OP_LOAD_UCODE: {
            int variant = 0, isLine = 0;
            if (DlSrc::UcodeVariant(c, &variant, &isLine) == 0) {
                mStats.droppedOps++;
                break;
            }
            mVariant = variant;
            mLineSection = isLine != 0;
            break;
        }
        case OP_TEXTURE:
            SpTexture(static_cast<uint16_t>(C1(c.w1, 16, 16)), static_cast<uint16_t>(C1(c.w1, 0, 16)),
                      static_cast<uint8_t>(C0(c.w0, 11, 3)), static_cast<uint8_t>(C0(c.w0, 8, 3)),
                      static_cast<uint8_t>(C0(c.w0, 1, 7)));
            break;
        case OP_SETTIMG:
            DpSetTextureImage(C0(c.w0, 21, 3), C0(c.w0, 19, 2), C0(c.w0, 0, 12) + 1, c);
            break;
        case OP_SETPRIMCOLOR:
            DpSetPrimColor(static_cast<uint8_t>(C0(c.w0, 8, 8)), static_cast<uint8_t>(C0(c.w0, 0, 8)), NiRgba(c.w1));
            break;
        case OP_SETENVCOLOR:
            DpSetEnvColor(NiRgba(c.w1));
            break;
        case OP_SETFOGCOLOR:
            DpSetFogColor(NiRgba(c.w1));
            break;
        case OP_SETFILLCOLOR:
            DpSetFillColor(c.w1);
            break;
        case OP_LOADBLOCK:
            if (mDirectScope != 0 && !mDirectInLoad) {
                DirectScopeLoadBlock(c, t, mDirectScope, mDirectLevels[mDirectScope]);
            } else if (!(mLoadMemo != 0 && SetListLoadBlock(c, t))) {
                DpLoadBlock(static_cast<uint8_t>(C1(c.w1, 24, 3)), C0(c.w0, 12, 12), C0(c.w0, 0, 12), C1(c.w1, 12, 12),
                            C1(c.w1, 0, 12));
            }
            break;
        case OP_FILLRECT:
            if (mRdp.color_image_address == mRdp.z_buf_address && C1(c.w1, 12, 12) == 0 && C1(c.w1, 0, 12) == 0 &&
                static_cast<int32_t>(C0(c.w0, 12, 12)) >= static_cast<int32_t>(mCfg.nativeW - 1) * 4 &&
                static_cast<int32_t>(C0(c.w0, 0, 12)) >= static_cast<int32_t>(mCfg.nativeH - 1) * 4) {
                mStats.rects++; /* == DpFillRectangle's full-screen depth clear: nothing else (the frame's own) */
                break;
            }
            DpFillRectangle(static_cast<int32_t>(C1(c.w1, 12, 12)), static_cast<int32_t>(C1(c.w1, 0, 12)),
                            static_cast<int32_t>(C0(c.w0, 12, 12)), static_cast<int32_t>(C0(c.w0, 0, 12)));
            break;
        default:
            break;
    }
}

/* Masked stores; returns the bits they changed. */
static inline __attribute__((always_inline)) uint32_t NiStores(const uint32_t* st, uint32_t n) {
    uint32_t changed = 0;
    NI_NO_UNROLL
    for (uint32_t k = 0; k < n; k++, st += 3) {
        NiWord32* d = reinterpret_cast<NiWord32*>(st[0]);
        const uint32_t o = *d;
        const uint32_t v = (o & ~st[1]) | st[2];
        changed |= o ^ v;
        *d = v;
    }
    return changed;
}

/* An ST op's stores; true when they arm the textures (== ArmTexturesChanged's condition: a tile word
 * that changed something, G_TEXTURE's first tile that changed). */
static inline __attribute__((always_inline)) bool NiApply(uint32_t hdr, const uint32_t* st, uint32_t ns) {
    if ((hdr & kNiArmFt) != 0) {
        const uint32_t ft = NiStores(st, 1);
        (void)NiStores(st + 3, ns - 1);
        return ft != 0;
    }
    return NiStores(st, ns) != 0 && (hdr & kNiArm) != 0;
}

/* ni_race_mesh's ops (kNiMesh, kNiSkip, kNiGeoChk); returns the next op, or nullptr when the program
 * stopped (a root window's words changed: nothing of the window ran). */
__attribute__((noinline)) const uint32_t* Interp::NrrRaceGeoOp(const uint32_t* op, Frame& f, const ListRef& L,
                                                                NrrRaceSlot* slot, size_t start) {
    const uint32_t hdr = op[0];
    const uint32_t kind = hdr & 15u;
    if (kind == kNiSkip) {
        return op + 3;
    }
    if (kind == kNiMesh) {
        const uint32_t* seg = reinterpret_cast<const uint32_t*>(op[1]);
        mNi->w.mesh++;
        if (gdx3ds_stereo_replay_frame != 0 || mNiSeg(mNiSegUser, seg, op[2] & 0xFFFFu, L.base) == 0) {
            NrrRaceSegFallback(seg, op[2] & 0xFFFFu);
        }
        return op + 3;
    }
    const size_t r = start + (hdr >> 20); /* kNiGeoChk (root) */
    const uint32_t n = (hdr >> 8) & 0xFFFu;
    if (r + n > f.list.limit) {
        NrrRaceBail(slot, start, r, kWhyBound, r, 0);
        return nullptr;
    }
    const uint32_t* w = op + 1;
    NI_NO_UNROLL
    for (uint32_t k = 0; k < n; k++, w += 3) {
        const Cmd x = ReadCmd(f.list, r + k);
        if (x.w0 != w[0] || x.w1 != w[1] || x.w1hi != w[2]) {
            NrrRaceBail(slot, start, r, kWhyCheck, r + k, w[0]);
            return nullptr;
        }
    }
    return w;
}

/* The program: root ops (checked against the root list's words) and, after a DL op, the list's own
 * ops (the list compared whole) up to its kNiDlEnd. One loop for both: `L` is the list being run. */
__attribute__((noinline)) bool Interp::NrrRaceHotRun(const uint32_t* op, Frame& f, NrrRaceSlot* slot) {
    NrrRace& N = *mNi;
    const size_t start = f.idx;
    const ListRef* L = &f.list;
    ListRef sub;
    size_t dlAt = 0;     /* the root G_DL of the list being run */
    bool nested = false;
    for (;;) {
        const uint32_t hdr = op[0];
        const uint32_t kind = hdr & 15u;
        const size_t r = (nested ? 0 : start) + (hdr >> 20);
        if (kind == kNiSt) {
            const uint32_t nw = (hdr >> 8) & 15u;
            const uint32_t ns = op[1];
            const uint32_t* st = op + 2;
            if (!nested) {
                if (r + nw > f.list.limit) {
                    NrrRaceBail(slot, start, r, kWhyBound, r, 0);
                    return false;
                }
                NI_NO_UNROLL
                for (uint32_t k = 0; k < nw; k++, st += 3) {
                    const Cmd x = ReadCmd(f.list, r + k);
                    if (x.w0 != st[0] || x.w1 != st[1] || x.w1hi != st[2]) {
                        NrrRaceBail(slot, start, r, kWhyCheck, r + k, st[0]); /* nothing of the group ran yet */
                        return false;
                    }
                }
            }
            if (GDX_UNLIKELY(mTexOv != nullptr || ((hdr & kNiClosed) != 0 && mBufNumTris != 0) || N.level == 2)) {
                NrrRaceStOther(op, *L, r, !nested); /* stock handlers (a precondition fails) / verify */
            } else if (NiApply(hdr, st, ns)) {
                mRdp.textures_changed[0] = true; /* == ArmTexturesChanged */
                mRdp.textures_changed[1] = true;
            }
            op = st + 3 * ns;
            continue;
        }
        if (kind == kNiEnd) {
            f.idx = r; /* the span is done; the runner dispatches the next marker */
            /* nimenu W4 (review §4 hot trims): the 7 field adds + done/rootWords below used to be
             * inline here unconditionally, every span end, whether or not [debug] nirace_rcpt's
             * window will ever print them. Moved to a GDX_COLD helper behind mNiRcpt -- off this
             * measured hot function's size either way, and now also skipped outright at runtime
             * when the receipt window is disabled (nirace_rcpt<=0). */
            if (mNiRcpt) {
                NrrRaceEndReceipt(start, r, op[1], op[2]);
            }
            return true;
        }
        if (kind == kNiDlEnd) {
            nested = false;
            L = &f.list;
            op += 1;
            continue;
        }
        if (kind >= kNiMesh) {
            op = NrrRaceGeoOp(op, f, *L, slot, start);
            if (op == nullptr) {
                return false;
            }
            continue;
        }
        if (!nested && r >= f.list.limit) {
            NrrRaceBail(slot, start, r, kWhyBound, r, 0);
            return false;
        }
        const Cmd x = ReadCmd(*L, r);
        if (!nested && ((kind == kNiEx || kind == kNiCall) ? Opcode(x.w0) != Opcode(op[1]) : x.w0 != op[1])) {
            NrrRaceBail(slot, start, r, kWhyCheck, r, op[1]);
            return false;
        }
        if (kind == kNiSeg) {
            mSrc.SetSegment(C0(x.w0, 0, 16) / 4u, x); /* == SpMoveword's MW_SEGMENT */
            op += 2;
        } else if (kind == kNiCall) {
            Frame t;
            t.list = *L;
            t.idx = r + 1;
            if (!nested) {
                if (Opcode(x.w0) == OP_RDPHALF_1 && r + 1 < f.list.limit &&
                    Opcode(ReadCmd(f.list, r + 1).w0) == OP_BRANCH_Z) {
                    NrrRaceBail(slot, start, r, kWhyCheck, r, op[1]); /* the half latches a branch: the walker */
                    return false;
                }
                f.idx = r + 1;
            }
            NrrRaceHotCall(x, nested ? t : f);
            op += 2;
        } else if (kind == kNiEx) {
            if (nested) {
                Frame t;
                t.list = sub;
                t.idx = r + 1;
                (void)Exec(x, t, 0);
            } else {
                f.idx = r + 1;
                (void)Exec(x, f, 0); /* the stock handler (a TEXRECT consumes its halves from f) */
            }
            /* [debug] nicensus (nimenu W2, review finding M1): this Exec is a native-span op, not a
             * Walk() loop iteration, so the walker's own NcNote site never saw it -- nicensus undercounted
             * it. NrrRaceNoteEx (cold, see its definition) does the null check and the call off-budget;
             * this hot site (NrrRaceHotRun, in the measured race set) pays one unconditional `bl` (+12 B,
             * measured). The stale `race gdxn::Interp::SpDmaIo` entry dropped from nrr_hot.list in this
             * same commit (review W4's own "check whether SpDmaIo is stale", verified true by objdump:
             * its only two callers are both inside the general Interp::Exec, never any NrrRaceHot*
             * function) frees far more than that, so the combined race hot total still lands under the
             * 15,272 B ceiling. */
            NrrRaceNoteEx(x);
            N.w.ex++;
            N.exOp[Opcode(x.w0)]++;
            op += 2;
        } else {
            /* kNiDl / kNiDlh (root only) == Exec's G_DL: the resolve (the dlmemo or the bridge), the G_DL
             * hook; kNiDlh: the hook draws the list (a machine body, a texture set it skips) */
            f.idx = r + 1;
            dlAt = r;
            if (!((mDlMemoLevel != 0) ? ResolveListMemo(x, sub, false) : mSrc.ResolveList(x, sub))) {
                mStats.noopLists++;
                NrrRaceBail(slot, start, r + 1, kWhyDlRes, r + 1, 0);
                return false;
            }
            const bool taken = mDlHook != nullptr && (mDlHookSub = sub, mDlHook(mDlHookUser, sub.base, this) != 0);
            if (kind == kNiDlh && taken) {
                op += 2;
                continue;
            }
            if (taken || kind == kNiDlh) {
                if (!taken) {
                    PushList(sub); /* == Exec: the hook declined, the list is walked */
                }
                NrrRaceBail(slot, start, r + 1, kWhyDlHook, r + 1, 0);
                return false;
            }
            const uint32_t ref = static_cast<uint32_t>(sub.stride) | (sub.isBig ? 0x100u : 0u) | (sub.isF3d ? 0x200u : 0u);
            const uint32_t nCmd = op[5];
            /* op[2] (the compile-time base address) is deliberately NOT part of this check: the memcmp
             * below already proves the live list's words are byte-for-byte what the program was built
             * from, which is the whole of what matters -- every nested op downstream reads through the
             * freshly-resolved `sub`/`L` (today's address), never through `op[2]`. A heap copy that moved
             * (dlmemo/ResolveList rotating its cache under memory pressure -- more of it with more NRR
             * categories sharing the same resolve path) used to bail here on address alone even when the
             * content was identical, abandoning a span (and, before NrrRaceMissRecover, the rest of the
             * frame) for no semantic reason. Limit/ref still gate (a shorter/reclassified list IS a
             * different list even at the same address). */
            if (static_cast<uint32_t>(sub.limit) != op[3] || ref != op[4] ||
                memcmp(sub.base, op + 6, nCmd * sub.stride) != 0) {
                PushList(sub); /* == Exec: the list is walked */
                NrrRaceBail(slot, start, dlAt + 1, kWhyDlMiss, dlAt + 1, 0);
                return false;
            }
            nested = true;
            L = &sub;
            op += 6 + nCmd * (sub.stride / 4u);
        }
    }
}


/* ---- ni_race_mesh: geometry segments (cold planning) ----------------------------------------- *
 *
 * A WINDOW is a stretch of a list's words between two words that can change what a G_VTX computes
 * (a matrix, the geometry mode, G_TEXTURE, a moveword / movemem, a G_DL, ...). Inside it, the words
 * besides G_VTX and the triangles touch only the RDP state the triangles draw with (other modes, the
 * colours, the combiner, tiles, loads, syncs). Every run of consecutive triangles of a window is a
 * SEGMENT: the window's G_VTX words up to the run, then the run -- drawn at the run's position by the
 * mesh path, which transforms the vertices under the same RSP state the G_VTX words would have
 * (nothing that state depends on moved since) and draws with the RDP state of the run's position.
 * The G_VTX words themselves do nothing at their own position (kNiSkip). A plan is all or nothing
 * over the words it covers: a triangle no segment can carry (an index no G_VTX of its window loaded,
 * MODIFYVTX, a line) leaves every word to its handler, so the walker's vertex cache is right for it. */

static bool NiTri(uint8_t op) {
    return op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD;
}

/* Words that leave a window open: they change only the RDP state a triangle draws with. */
static bool NiRdpOnly(const Cmd& c) {
    const uint8_t op = Opcode(c.w0);
    if (op == OP_TEXRECT || op == OP_TEXRECTFLIP || op == OP_FILLRECT || op == OP_SETCIMG || op == OP_SETZIMG) {
        return false;
    }
    if (op == OP_NOOP) {
        return ((c.w0 >> 16) & 0xFFu) == 0;
    }
    return op >= OP_SETOTHERMODE_L || op == OP_RDPLOADSYNC || op == OP_RDPPIPESYNC || op == OP_RDPTILESYNC;
}

static uint32_t NiSegHash(const uint32_t* w, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t k = 0; k < n; k++) {
        h = (h ^ w[k]) * 16777619u;
    }
    return h;
}

GDX_COLD void Interp::NrrRacePlanGeo(const ListRef& L, size_t a, size_t b, size_t base) {
    NrrRace& N = *mNi;
    const int ps = N.ps;
    uint8_t* plan = N.planS[ps];
    N.planBaseS[ps] = base;
    N.planEndS[ps] = b;
    if (b <= base || b - base > kNiPlan) {
        N.planEndS[ps] = base;
        return;
    }
    memset(plan + (a - base), 0, b - a);
    if (mNiSeg == nullptr || N.noMesh) {
        return;
    }
    /* pass 1: every triangle covered by its window's G_VTX words? */
    struct Run {
        size_t at, len, win;
    };
    Run runs[24];
    uint32_t nRuns = 0;
    size_t vtx[48];
    uint32_t vtxWin[48];
    uint32_t nVtx = 0;
    uint32_t win = 0;
    uint64_t loaded = 0;
    bool any = false;
    for (size_t i = a; i < b; i++) {
        const Cmd c = ReadCmd(L, i);
        const uint8_t op = Opcode(c.w0);
        if (L.stride == 16 && c.w1hi != 0 && (op == OP_VTX || NiTri(op))) {
            N.w.planNo++;
            return; /* a host-tagged operand: the narrow segment cannot carry it */
        }
        if (op == OP_VTX) {
            const uint32_t n = C0(c.w0, 12, 8);
            const uint32_t dest = C0(c.w0, 1, 7) - n;
            if (n == 0 || dest + n > static_cast<uint32_t>(kMaxVertices) || nVtx == 48) {
                N.w.planNo++;
                return;
            }
            loaded |= ((n == 64) ? ~0ull : ((1ull << n) - 1ull)) << dest;
            vtxWin[nVtx] = win;
            vtx[nVtx++] = i;
        } else if (NiTri(op)) {
            size_t j = i;
            while (j < b && NiTri(Opcode(ReadCmd(L, j).w0))) {
                const Cmd t = ReadCmd(L, j);
                const uint32_t idx[6] = { C0(t.w0, 16, 8) / 2, C0(t.w0, 8, 8) / 2, C0(t.w0, 0, 8) / 2,
                                          C1(t.w1, 16, 8) / 2, C1(t.w1, 8, 8) / 2, C1(t.w1, 0, 8) / 2 };
                const uint32_t nIdx = Opcode(t.w0) == OP_TRI1 ? 3u : 6u;
                for (uint32_t k = 0; k < nIdx; k++) {
                    if (idx[k] >= 64u || ((loaded >> idx[k]) & 1u) == 0) {
                        N.w.planNo++;
                        return; /* a vertex its window did not load: every word stays stock */
                    }
                }
                j++;
            }
            if (nRuns == 24) {
                N.w.planNo++;
                return;
            }
            runs[nRuns++] = Run{ i, j - i, win };
            any = true;
            i = j - 1;
        } else if (op == OP_MODIFYVTX || op == OP_LINE3D || op == OP_BRANCH_Z) {
            N.w.planNo++;
            return;
        } else if (!NiRdpOnly(c)) {
            win++; /* a word that moves what a G_VTX computes: a new window */
            loaded = 0;
        }
    }
    if (!any) {
        return;
    }
    /* pass 2: the segments (interned), the absorbed G_VTX words, each window's extent */
    memset(N.planChkS[ps] + (a - base), 0, sizeof(uint16_t) * (b - a));
    for (uint32_t r = 0; r < nRuns; r++) {
        size_t first = runs[r].at;
        for (uint32_t v = 0; v < nVtx; v++) {
            if (vtxWin[v] == runs[r].win && vtx[v] < first) {
                first = vtx[v];
            }
        }
        size_t last = runs[r].at + runs[r].len;
        for (uint32_t q = r + 1; q < nRuns && runs[q].win == runs[r].win; q++) {
            last = runs[q].at + runs[q].len;
        }
        if (r == 0 || runs[r - 1].win != runs[r].win) {
            N.planChkS[ps][first - base] = static_cast<uint16_t>(last - first);
        }
    }
    for (uint32_t r = 0; r < nRuns; r++) {
        const Run& run = runs[r];
        uint32_t nW = 0, nv = 0;
        for (uint32_t v = 0; v < nVtx; v++) {
            if (vtxWin[v] == run.win && vtx[v] < run.at) {
                const Cmd c = ReadCmd(L, vtx[v]);
                N.segBuild[2 * nW] = c.w0;
                N.segBuild[2 * nW + 1] = c.w1;
                nW++;
                nv++;
            }
        }
        if (nW + run.len + 1 > kNiSegMaxCmds) {
            memset(plan + (a - base), 0, b - a);
            N.w.planNo++;
            return;
        }
        for (size_t k = 0; k < run.len; k++) {
            const Cmd c = ReadCmd(L, run.at + k);
            N.segBuild[2 * nW] = c.w0;
            N.segBuild[2 * nW + 1] = c.w1;
            nW++;
        }
        N.segBuild[2 * nW] = static_cast<uint32_t>(OP_ENDDL) << 24;
        N.segBuild[2 * nW + 1] = 0;
        nW++;
        const uint32_t h = NiSegHash(N.segBuild, 2 * nW);
        uint32_t* seg = nullptr;
        for (uint32_t k = 0; k < N.nSegs && seg == nullptr; k++) {
            if (N.segs[k].hash == h && N.segs[k].cmds == nW && memcmp(N.segs[k].words, N.segBuild, 8u * nW) == 0) {
                seg = N.segs[k].words;
            }
        }
        if (seg == nullptr && N.nSegs < kNiSegs) {
            seg = static_cast<uint32_t*>(malloc(8u * nW));
            if (seg != nullptr) {
                memcpy(seg, N.segBuild, 8u * nW);
                N.segs[N.nSegs++] = { h, nW, seg };
            }
        }
        if (seg == nullptr) {
            memset(plan + (a - base), 0, b - a);
            N.w.planNo++;
            return;
        }
        const size_t p = run.at - base;
        plan[p] = kPlanMesh;
        N.planRunS[ps][p] = static_cast<uint16_t>(run.len);
        N.planVtxS[ps][p] = static_cast<uint16_t>(nv | (nW << 8));
        N.planSegS[ps][p] = seg;
        for (size_t k = 1; k < run.len; k++) {
            plan[p + k] = kPlanIn;
        }
        for (uint32_t v = 0; v < nVtx; v++) {
            if (vtxWin[v] == run.win && vtx[v] < run.at) {
                plan[vtx[v] - base] = kPlanSkip;
            }
        }
    }
    N.w.plans++;
}

/* A geometry segment at compile: the mesh path draws it, else its words run (and the span is compiled
 * again without geometry). */
GDX_COLD void Interp::NrrRaceSegRun(const uint32_t* seg, uint32_t cmds, const void* hint, NrrRaceSlot* slot) {
    NrrRace& N = *mNi;
    N.w.mesh++;
    /* A stereo-active refusal (nirace-progress.md 4R6) is this frame's condition, not a verdict on
     * the segment: skip it this once without marking the plan noMesh / forcing a recompile, so the
     * SAME plan draws it via the mesh path again as soon as a frame engages (2D, or stereo without
     * a live replay). W1/H1: read live, not the old per-task latch -- see gdx_nirace.cpp:56. */
    if (gdx3ds_stereo_replay_frame != 0) {
        NrrRaceSegFallback(seg, cmds);
        return;
    }
    if (mNiSeg(mNiSegUser, seg, cmds, hint) == 0) {
        NrrRaceSegFallback(seg, cmds);
        slot->noMesh = 1;
        slot->recompile = 1;
    }
}

/* A segment the mesh path did not take: its words through their handlers, from the segment's own
 * packets (the G_VTX words first: the triangles need them in the vertex cache). Counted. */
/* [debug] nicensus (nimenu W2, review finding M1): the one hot Exec site's NcNote, pushed cold. The
 * check-then-call pattern used everywhere else in this interpreter (`if (mNc != nullptr) NcNote(...)`)
 * costs a compare, a branch and a call at the call site; this trampoline is called unconditionally
 * instead, so NrrRaceHotRun (measured in the race hot budget) pays only the one `bl` -- the null check
 * moves in here, where it is off-budget (cold, GDX_COLD). Correct either way: mNc is null whenever
 * [debug] nicensus is off, so this function returns immediately in the common case. */
GDX_COLD void Interp::NrrRaceNoteEx(const Cmd& c) {
    if (mNc != nullptr) {
        NcNote(c, false, NCS_NIEX);
    }
}

/* nimenu W4 (review §4 hot trims): kNiEnd's span-end receipt tally, moved out of NrrRaceHotRun.
 * c0/c1 are the program's op counts (NrrRaceCompile); start/r bound the span's root words. Only
 * called when mNiRcpt is set (the [debug] nirace_rcpt window will actually print this). */
GDX_COLD void Interp::NrrRaceEndReceipt(size_t start, size_t r, uint32_t c0, uint32_t c1) {
    NrrRace& N = *mNi;
    N.w.st += c0 & 0xFFu;
    N.w.seg += (c0 >> 8) & 0xFFu;
    N.w.call += (c0 >> 16) & 0xFFu;
    N.w.dl += c0 >> 24;
    N.w.stWords += c1 & 0xFFFu;
    N.w.dlh += (c1 >> 12) & 0xFFu;
    N.w.nestedWords += c1 >> 20;
    N.w.done++;
    N.w.rootWords += static_cast<uint32_t>(r - start);
}

GDX_COLD bool Interp::NrrRaceSegFallback(const uint32_t* seg, uint32_t cmds) {
    NrrRace& N = *mNi;
    ListRef l;
    l.base = reinterpret_cast<const uint8_t*>(seg);
    l.limit = cmds;
    l.stride = 8;
    for (uint32_t k = 0; k + 1 < cmds; k++) {
        const Cmd c = ReadCmd(l, k);
        Frame t;
        t.list = l;
        t.idx = k + 1u;
        (void)Exec(c, t, 0);
        if (GDX_UNLIKELY(mNc != nullptr)) { /* [debug] nicensus (nimenu W2, review M1) */
            NcNote(c, false, NCS_NISEG);
        }
        N.w.ex++;
    }
    N.w.meshFail++;
    return true;
}

/* ---- cold: misses, compiles, bails, verify -------------------------------------------------- */

/* native-vs: the task's spans keyed per view (the glue: [native] ni_view's NRR_NI_VIEW_RACE, a 2-4P
 * race frame). */
GDX_COLD void Interp::NrrRaceSetMultiView(bool on) {
    if (mNi != nullptr) {
        mNi->multiView = on;
    }
}

/* native-vs: a 2-4P frame's span key. The frame's span ordinal drifts with every view's variable span
 * count (the track's material runs come and go with each camera), so a later view's spans took a new
 * key -- a new compile -- nearly every frame (4P attract demo: 60-76 ordinals per (marker, first word),
 * ~2,700 compiles, ~1,950 evictions and 11 pool clears per 256 frames). Here the ordinal is the span's
 * occurrence after the same marker word in this task: an NRR marker carries its (view, category), so a
 * view's spans keep their keys whatever the other views drew. The occurrence also goes into bits 4-11:
 * NiHash's multiplies only carry a bit upward, so the opcode byte alone never reaches the slot index and
 * a view's ~19 track-run spans (same marker, same first word) all probed one 8-slot window and evicted
 * each other. The keys stay distinct (the opcode byte still decides n). A full table falls back to the
 * frame's ordinal (a key only picks the cache slot: the program still checks its words). */
GDX_COLD uint32_t Interp::NrrRaceViewKey(uint32_t marker) {
    NrrRace& N = *mNi;
    const uint32_t h = (marker * 2654435761u) >> (32u - kNiOccBits);
    N.ordinal++;
    for (uint32_t p = 0; p < kNiOcc; p++) {
        NrrRace::Occ& o = N.occ[(h + p) & (kNiOcc - 1u)];
        if (o.task != N.task) {
            o.task = N.task;
            o.marker = marker;
            o.n = 1;
            return marker ^ (1u << 24) ^ (1u << 4);
        }
        if (o.marker == marker) {
            o.n++;
            return marker ^ ((o.n & 0xFFu) << 24) ^ ((o.n & 0xFFu) << 4);
        }
    }
    return marker ^ (N.ordinal << 24);
}

/* A span's native program did not take (NrrRaceHotSpan false): mSp/the top frame's idx are wherever
 * that left them -- unchanged (the throttle, `mNrrOpen`) or past a bailed op with a list pushed above
 * the root (`NrrRaceBail`'s dlhook/dlmiss/dlres paths). Stock-walks from there, word for word via the
 * SAME dispatch `Walk()`'s own loop uses (op stats, F3D remap, `[nverify]`, `Exec`), until back at the
 * root (mSp == 1) sitting on the next marker/ENDDL -- then returns control WITHOUT consuming that word,
 * so `NrrRaceHotDrive`'s own marker dispatch (and the next span's native attempt) picks up exactly as
 * it would after a native span. Only this one span's words are walked, never "the rest of the frame"
 * (the previous behaviour: `NrrRaceHotDrive` returning unconditionally sent every remaining span to
 * `Walk()`'s own while-loop for good). Returns false only when the frame itself ended inside the walk
 * (an ENDDL popped mSp to 0) -- nothing left for the native loop to resume. Exact by construction: this
 * is the identical stock path a bail already fell back to; it is now bounded instead of open-ended. */
GDX_COLD bool Interp::NrrRaceMissRecover() {
    if (mNi->menu && mNrrOpen && mSp == 1 && NrrRaceMenuReopen()) {
        return true; /* the span ran natively after all */
    }
    while (mSp > 0) {
        Frame& t = mStack[mSp - 1];
        if (t.idx >= t.list.limit) {
            mStats.runaway++;
            mSp--;
            continue;
        }
        if (mSp == 1) {
            const Cmd peek = ReadCmd(t.list, t.idx);
            const uint8_t peekOp = Opcode(peek.w0);
            if (peekOp == OP_NOOP || peekOp == OP_SPNOOP || peekOp == OP_ENDDL) {
                return true; /* back at the root, sitting on a marker or the root's own end */
            }
        }
        Cmd c = ReadCmd(t.list, t.idx);
        t.idx++;
        mStats.op[Opcode(c.w0)]++;
        mStats.cmds++;
        size_t f3dXfer = 0;
        if (t.list.isF3d && !RemapF3d(c, t.list, &f3dXfer)) {
            mStats.droppedOps++;
            continue;
        }
        /* no mVerifyActive/VerifyNoteCmd check here (unlike Walk()'s loop): [native] ni_race already
         * refuses to arm under [native] interp_verify (gdx_rt_glue.cpp), so NrrRaceHotDrive -- and this,
         * its only caller -- never runs while that oracle is live. Checking it here would pull
         * VerifyNoteCmd's body into race's hot-size reachability for a branch that can't be taken. */
        if (GDX_UNLIKELY(mNc != nullptr)) { /* [debug] nicensus (nimenu W2, review M1): a stock-walk
                                              * command, same as Walk()'s own loop counts -- stays in
                                              * whatever ctx this recovery inherited (NC_WALK), matching
                                              * the comment above: this IS the stock walk, word for word. */
            NcNote(c, false, NCS_NIRECOV);
        }
        const int r = Exec(c, t, f3dXfer);
        if (r == 1) {
            mSp--;
        }
    }
    return false; /* the frame ended (mSp hit 0) inside the recovery walk */
}

/* menus-s2 [native] ni_menu_root: a menu span refused only because the NRR HUD still holds citro3d (a
 * MENU run left its batch open; NrrRaceHotSpan's mNrrOpen test, the first refusal it can make on a span
 * that has words). The hand-off is closed here -- the open batch drawn, the backend's viewport / scissor
 * where the stock rectangles leave them -- and the span offered to its program again. The stock walk
 * would close it at the first word that reaches the backend instead; what is drawn and in which order
 * is the same (the batch is complete: only MENU runs add to it, and the next one reopens). The marker
 * before the span is the root word before it (0 at the root's start), as NrrRaceHotDrive passed it.
 * true: the span is done (as NrrRaceHotSpan's true); false: the stock walk takes it from where the
 * program left the root. */
GDX_COLD bool Interp::NrrRaceMenuReopen() {
    Frame& f = mStack[0];
    if (f.idx >= f.list.limit) {
        return false;
    }
    uint32_t marker = 0;
    if (f.idx > 0) {
        const Cmd m = ReadCmd(f.list, f.idx - 1u);
        const uint8_t mo = Opcode(m.w0);
        marker = (mo == OP_NOOP || mo == OP_SPNOOP) ? m.w0 : 0u;
    }
    NrrHudHotClose();
    mNi->menuCloses++;
    return NrrRaceHotSpan(f, marker) && mSp == 1;
}

/* An ST op the hot loop does not apply: its words' stock handlers, in order, from the list they were
 * compiled from -- when the stores' precondition does not hold (an open batch under a flushing colour
 * word, a texskip window) -- or, at [native] ni_race=2, the verify (the stock result stays). */
GDX_COLD void Interp::NrrRaceStOther(const uint32_t* op, const ListRef& list, size_t idx, bool root) {
    NrrRace& N = *mNi;
    const uint32_t hdr = op[0];
    const uint32_t n = (hdr >> 8) & 15u;
    if (mTexOv == nullptr && !((hdr & kNiClosed) != 0 && mBufNumTris != 0)) {
        NrrRaceVerifySt(op, list, idx, root);
        return;
    }
    for (uint32_t k = 0; k < n; k++) {
        const Cmd c = ReadCmd(list, idx + k);
        Frame t;
        t.list = list;
        t.idx = idx + k + 1u;
        (void)Exec(c, t, 0);
        if (GDX_UNLIKELY(mNc != nullptr)) { /* [debug] nicensus (nimenu W2, review M1) */
            NcNote(c, false, NCS_NISTX);
        }
    }
    N.w.stx += n;
}

/* A program stops before its end: the root frame resumes at `resume` (a list may be pushed above it);
 * a word that no longer matches its compiled check (`var`, from the span start) becomes EX at the
 * span's next compile. */
GDX_COLD void Interp::NrrRaceBail(NrrRaceSlot* slot, size_t start, size_t resume, int why, size_t var, uint32_t expW0) {
    NrrRace& N = *mNi;
    N.w.bail[why]++;
    N.w.rootWords += static_cast<uint32_t>(resume - start);
    if (why == kWhyCheck) {
        /* the same opcode with another operand: a variable word (EX); another opcode: the span changed shape */
        const size_t rel = var - start;
        if (rel < kNiMaxRoot && var < mStack[0].list.limit && Opcode(ReadCmd(mStack[0].list, var).w0) == Opcode(expW0)) {
            slot->varMask[rel >> 5] |= 1u << (rel & 31u);
        }
        slot->recompile = 1;
    }
    mStack[0].idx = resume; /* the root frame */
    if (N.log != 0 && N.logBails < 400) {
        N.logBails++;
        const Cmd x = var < mStack[0].list.limit ? ReadCmd(mStack[0].list, var) : Cmd{};
        GDXN_LOG("[nirace] bail task=%u key=%08X %08X why=%s rel=%u var=%u word=%08X %08X", static_cast<unsigned>(N.task),
                 slot->marker, slot->w0, kWhyName[why], static_cast<unsigned>(resume - start),
                 static_cast<unsigned>(var - start), x.w0, x.w1);
    }
}

GDX_COLD bool Interp::NrrRaceMiss(Frame& f, uint32_t marker, const Cmd& c, uint32_t h) {
    NrrRace& N = *mNi;
    NrrRaceSlot* slot = nullptr;
    NrrRaceSlot* lru = nullptr;
    for (uint32_t p = 0; p < kNiProbe && slot == nullptr; p++) {
        NrrRaceSlot* s = &N.slots[(h + p) & (kNiSlots - 1u)];
        if (s->used == 0 || (s->marker == marker && s->w0 == c.w0)) {
            slot = s;
        } else if (lru == nullptr || s->lastUse < lru->lastUse) {
            lru = s;
        }
    }
    if (slot == nullptr) {
        slot = lru; /* full: the least recently run of the probed slots is taken over */
        memset(static_cast<void*>(slot), 0, sizeof(*slot));
        N.w.evicts++;
    }
    slot->lastUse = N.task;
    if (slot->used == 0) {
        slot->used = 1;
        slot->marker = marker;
        slot->w0 = c.w0;
        slot->w1 = c.w1;
    }
    if (slot->compiles >= kNiMaxCompiles && N.task - slot->lastCompile < kNiThrottle) {
        return false; /* a span that keeps changing: the walker takes it */
    }
    return NrrRaceCompile(f, slot);
}

/* A register word compiled: its handler runs for real on the live registers (S -> R), then on their
 * complement (-> P), and R is put back (NrrRegCompile's method). Returns 1 with the masked stores
 * (dst, mask, val) when the word is a pure masked write, 0 when it is not (it ran once: its effect
 * is in the registers). */
GDX_COLD int Interp::NrrRaceCompileWord(const Cmd& c, const ListRef& list, uint32_t idx, uint32_t* st, uint32_t* ns) {
    NrrRace& N = *mNi;
    Frame t;
    t.list = list;
    t.idx = idx + 1u;
    *ns = 0;
    uint8_t* base[3];
    uint32_t len[3];
    if (NrrRegRegions(base, len) == 0) {
        (void)Exec(c, t, 0);
        return 0;
    }
    uint32_t flushSaved[FLUSH_COUNT];
    NiRegCopy(base, len, N.fxS, false, false);
    (void)Exec(c, t, 0); /* the real run */
    NiRegCopy(base, len, N.fxR, false, false);
    memcpy(flushSaved, mStats.flush, sizeof(flushSaved));
    NiRegCopy(base, len, N.fxS, true, true); /* the complement */
    t.idx = idx + 1u;
    (void)Exec(c, t, 0);
    NiRegCopy(base, len, N.fxP, false, false);
    NiRegCopy(base, len, N.fxR, true, false); /* the real result back */
    memcpy(mStats.flush, flushSaved, sizeof(flushSaved));
    uint32_t at = 0, n = 0;
    const uintptr_t ftWord = reinterpret_cast<uintptr_t>(&mRdp.first_tile_index) & ~static_cast<uintptr_t>(3);
    bool ok = true;
    for (int r = 0; r < 3 && ok; r++) {
        for (uint32_t o = 0; o < len[r] && ok; o += 4) {
            uint32_t s, rv, pv;
            memcpy(&s, N.fxS + at + o, 4);
            memcpy(&rv, N.fxR + at + o, 4);
            memcpy(&pv, N.fxP + at + o, 4);
            const uint32_t keep = NrrRegKeepMask(base[r] + o);
            const uint32_t mask = ~(rv ^ pv) & ~keep;
            if (((rv ^ s) & ~mask & ~keep) != 0 || ((pv ^ ~s) & ~mask & ~keep) != 0) {
                ok = false; /* an unwritten bit moved: not a pure masked write */
            } else if (mask != 0) {
                if (n == kNiWordStores) {
                    ok = false;
                } else {
                    st[3 * n] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base[r] + o));
                    st[3 * n + 1] = mask;
                    st[3 * n + 2] = rv & mask;
                    n++;
                }
            }
        }
        at += len[r];
    }
    if (!ok) {
        return 0;
    }
    if (Opcode(c.w0) == OP_TEXTURE) {
        /* the first-tile store first (kNiArmFt): G_TEXTURE arms the textures on a first-tile change only */
        for (uint32_t k = 1; k < n; k++) {
            if (st[3 * k] == static_cast<uint32_t>(ftWord)) {
                for (uint32_t q = 0; q < 3; q++) {
                    const uint32_t v = st[q];
                    st[q] = st[3 * k + q];
                    st[3 * k + q] = v;
                }
            }
        }
        if (n == 0 || st[0] != static_cast<uint32_t>(ftWord)) {
            return 0; /* no first-tile store to key the arm on: keep it exec-only */
        }
    }
    *ns = n;
    return 1;
}


/* A setup list a DL op can carry: not F3D, a known stride, a terminator within kNiMaxNested commands,
 * no flow inside (a nested G_DL, a branch, a marker). */
GDX_COLD bool Interp::NrrRaceCompileList(const ListRef& sub) {
    if (sub.isF3d || sub.base == nullptr || (sub.stride != 8 && sub.stride != 16)) {
        return false;
    }
    for (size_t j = 0; j < sub.limit && j < kNiMaxNested; j++) {
        const Cmd y = ReadCmd(sub, j);
        const uint8_t o = Opcode(y.w0);
        if (o == OP_ENDDL) {
            return true;
        }
        if (o == OP_NOOP && ((y.w0 >> 16) & 0xFFu) == 0) {
            continue; /* a plain G_NOOP (the setup lists carry them): EX, the hook does nothing with p=0 */
        }
        if (NiFlow(o) || o == OP_RDPHALF_1 || o == OP_LOAD_UCODE) {
            return false;
        }
        if (o == OP_TEXRECT || o == OP_TEXRECTFLIP) {
            j += 2;
        }
    }
    return false;
}

/* Compile the span at f.idx: its words execute exactly as the walker would execute them (each op the
 * way the program will run it), and the program records what ran. Returns with f.idx at the first
 * word the program does not cover (the walker continues there; a list may be pushed). */
GDX_COLD bool Interp::NrrRaceCompile(Frame& f, NrrRaceSlot* slot) {
    NrrRace& N = *mNi;
    N.w.compiles++;
    slot->compiles++;
    slot->lastCompile = N.task;
    slot->recompile = 0;
    slot->off = 0;
    N.tmpN = 0;
    N.cSt = N.cSeg = N.cCall = N.cDl = N.cDlh = N.cStWords = N.cNested = 0;
    N.gN = 0;
    N.gNs = 0;
    N.gFlags = 0;
    const size_t start = f.idx;
    size_t r = start;
    size_t resume = 0; /* != 0: this frame resumes there (the program ends at r, before a G_DL it ran) */
    const char* why = "marker";
    /* ni_race_mesh: the span's geometry plan (its extent: up to the next root marker) */
    N.ps = 0;
    N.noMesh = slot->noMesh != 0 || N.menu; /* ni_menu_root: a menu frame plans no geometry */
    {
        size_t e = start;
        while (e < f.list.limit && e - start < kNiMaxRoot) {
            const uint8_t o = Opcode(ReadCmd(f.list, e).w0);
            if (o == OP_NOOP || o == OP_SPNOOP || o == OP_ENDDL || o == OP_BRANCH_Z || o == OP_RDPHALF_1) {
                break;
            }
            e += (o == OP_TEXRECT || o == OP_TEXRECTFLIP) ? 3u : 1u;
        }
        NrrRacePlanGeo(f.list, start, e < f.list.limit ? e : f.list.limit, start);
        for (size_t i = start; i < N.planEndS[0]; i++) {
            const size_t rel = i - start;
            if (N.PlanAt(i) != 0 && rel < kNiMaxRoot && ((slot->varMask[rel >> 5] >> (rel & 31u)) & 1u) != 0) {
                N.planEndS[0] = start; /* a geometry word that changes between frames: no plan */
            }
        }
    }
    while (r < f.list.limit && r - start < kNiMaxRoot && N.tmpN + 64 < kNiMaxOpWords) {
        const Cmd c = ReadCmd(f.list, r);
        const uint8_t op = Opcode(c.w0);
        const uint32_t rel = static_cast<uint32_t>(r - start);
        const bool var = (slot->varMask[rel >> 5] >> (rel & 31u)) & 1u;
        if (op == OP_NOOP || op == OP_ENDDL || op == OP_SPNOOP || op == OP_BRANCH_Z ||
            (op == OP_RDPHALF_1 && r + 1 < f.list.limit && Opcode(ReadCmd(f.list, r + 1).w0) == OP_BRANCH_Z)) {
            break; /* the span's end (a marker, the root's end) or flow the walker follows */
        }
        if (N.PlanAt(r) != 0) {
            N.GroupClose();
            const bool mesh = N.PlanAt(r) == kPlanMesh;
            const size_t p = r - N.planBaseS[0];
            r += N.PlanEmit(f.list, r, rel, c, true);
            if (mesh) {
                NrrRaceSegRun(N.planSegS[0][p], N.planVtxS[0][p] >> 8, f.list.base, slot);
            }
            continue;
        }
        if (op == OP_DL) {
            N.GroupClose();
            if (C0(c.w0, 16, 1) != 0 || var) {
                why = var ? "dl-var" : "dl-branch";
                break; /* a branch list replaces the root: the walker takes it */
            }
            f.idx = r + 1;
            ListRef sub;
            if (!((mDlMemoLevel != 0) ? ResolveListMemo(c, sub, false) : mSrc.ResolveList(c, sub))) {
                mStats.noopLists++;
                resume = r + 1;
                why = "dl-res";
                break;
            }
            if (mDlHook != nullptr && (mDlHookSub = sub, mDlHook(mDlHookUser, sub.base, this) != 0)) {
                N.cDlh++;
                N.Emit(kNiDlh | (rel << 20)); /* drawn by the hook (a machine list): the program goes on */
                N.Emit(c.w0);
                r++;
                continue;
            }
            if (!NrrRaceCompileList(sub)) {
                why = "dl-list";
                f.idx = r + 1;
                PushList(sub); /* == Exec: the walker walks it, then the root from r + 1 */
                resume = r + 1;
                break;
            }
            uint32_t nCmd = 0;
            while (Opcode(ReadCmd(sub, nCmd).w0) != OP_ENDDL) {
                nCmd++;
            }
            nCmd++;
            N.cDl++;
            N.cNested += nCmd;
            N.Emit(kNiDl | (rel << 20));
            N.Emit(c.w0);
            N.Emit(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sub.base)));
            N.Emit(static_cast<uint32_t>(sub.limit));
            N.Emit(static_cast<uint32_t>(sub.stride) | (sub.isBig ? 0x100u : 0u) | (sub.isF3d ? 0x200u : 0u));
            N.Emit(nCmd);
            const uint32_t* raw = reinterpret_cast<const uint32_t*>(sub.base);
            for (uint32_t k = 0; k < nCmd * (sub.stride / 4u); k++) {
                N.Emit(raw[k]);
            }
            N.ps = 1;
            NrrRacePlanGeo(sub, 0, nCmd - 1, 0);
            for (uint32_t j = 0; j + 1 < nCmd; j++) {
                const Cmd y = ReadCmd(sub, j);
                const uint8_t yo = Opcode(y.w0);
                if (N.PlanAt(j) != 0) {
                    N.GroupClose();
                    const bool mesh = N.PlanAt(j) == kPlanMesh;
                    const uint32_t* seg = N.planSegS[1][j];
                    const uint32_t cmds = N.planVtxS[1][j] >> 8;
                    j += N.PlanEmit(sub, j, j, y, false) - 1u;
                    if (mesh) {
                        NrrRaceSegRun(seg, cmds, sub.base, slot);
                    }
                    continue;
                }
                if (NiInert(y)) {
                    N.GroupAdd(false, j, y, nullptr, 0, 0, false);
                    continue;
                }
                if (NiSegment(y)) {
                    N.GroupClose();
                    mSrc.SetSegment(C0(y.w0, 0, 16) / 4u, y);
                    N.cSeg++;
                    N.Emit(kNiSeg | (j << 20));
                    N.Emit(y.w0);
                    continue;
                }
                uint32_t st[3 * kNiWordStores];
                uint32_t ns = 0;
                if (NiPure(yo) && mTexOv == nullptr && !(NiClosed(yo) && mBufNumTris != 0)) {
                    if (NrrRaceCompileWord(y, sub, j, st, &ns) == 1) {
                        const uint32_t fl = (NiClosed(yo) ? kNiClosed : 0u) |
                                            ((yo == OP_SETTILE || yo == OP_SETTILESIZE) ? kNiArm : 0u) |
                                            (yo == OP_TEXTURE ? kNiArmFt : 0u);
                        N.GroupAdd(false, j, y, st, ns, fl, fl != 0);
                        continue;
                    }
                    N.GroupClose(); /* not pure: it ran once; EX from now on */
                } else {
                    N.GroupClose();
                    Frame t;
                    t.list = sub;
                    t.idx = j + 1u;
                    (void)Exec(y, t, 0);
                }
                N.cCall += NiCallable(y) ? 1u : 0u;
                N.Emit((NiCallable(y) ? kNiCall : kNiEx) | (j << 20));
                N.Emit(y.w0);
                if (yo == OP_TEXRECT || yo == OP_TEXRECTFLIP) {
                    j += 2;
                }
            }
            N.GroupClose();
            N.Emit(kNiDlEnd);
            N.ps = 0;
            r++;
            continue;
        }
        if (!var && NiInert(c)) {
            N.GroupAdd(true, rel, c, nullptr, 0, 0, false);
            r++;
            continue;
        }
        if (!var && NiSegment(c)) {
            N.GroupClose();
            mSrc.SetSegment(C0(c.w0, 0, 16) / 4u, c);
            N.cSeg++;
            N.Emit(kNiSeg | (rel << 20));
            N.Emit(c.w0);
            r++;
            continue;
        }
        uint32_t st[3 * kNiWordStores];
        uint32_t ns = 0;
        if (!var && NiPure(op) && mTexOv == nullptr && !(NiClosed(op) && mBufNumTris != 0)) {
            if (NrrRaceCompileWord(c, f.list, static_cast<uint32_t>(r), st, &ns) == 1) {
                const uint32_t fl = (NiClosed(op) ? kNiClosed : 0u) |
                                    ((op == OP_SETTILE || op == OP_SETTILESIZE) ? kNiArm : 0u) |
                                    (op == OP_TEXTURE ? kNiArmFt : 0u);
                N.GroupAdd(true, rel, c, st, ns, fl, fl != 0);
                r++;
                continue;
            }
            N.GroupClose(); /* not pure: it ran once; EX from now on */
            N.cCall += NiCallable(c) ? 1u : 0u;
                N.Emit((NiCallable(c) ? kNiCall : kNiEx) | (1u << 8) | (rel << 20));
            N.Emit(c.w0);
            r++;
            continue;
        }
        N.GroupClose();
        f.idx = r + 1;
        (void)Exec(c, f, 0);
        const bool call = NiCallable(c) && !(op == OP_RDPHALF_1 && r + 1 < f.list.limit &&
                                             Opcode(ReadCmd(f.list, r + 1).w0) == OP_BRANCH_Z);
        N.cCall += call ? 1u : 0u;
        N.Emit((call ? kNiCall : kNiEx) | (static_cast<uint32_t>(f.idx - r) << 8) | (rel << 20));
        N.Emit(c.w0);
        r = f.idx;
    }
    N.GroupClose();
    N.Emit(kNiEnd | (static_cast<uint32_t>(r - start) << 20));
    N.Emit((N.cSt & 0xFFu) | ((N.cSeg & 0xFFu) << 8) | ((N.cCall & 0xFFu) << 16) | ((N.cDl & 0xFFu) << 24));
    N.Emit((N.cStWords & 0xFFFu) | ((N.cDlh & 0xFFu) << 12) | ((N.cNested & 0xFFFu) << 20));
    f.idx = resume != 0 ? resume : r;
    if (N.log != 0) {
        const Cmd le = r < f.list.limit ? ReadCmd(f.list, r) : Cmd{};
        GDXN_LOG("[nirace] compile task=%u key=%08X %08X %08X span=%u..%u end=%s at=%08X %08X words=%u n=%u",
                 static_cast<unsigned>(N.task), slot->marker, slot->w0, slot->w1, static_cast<unsigned>(start),
                 static_cast<unsigned>(r), why, le.w0, le.w1, static_cast<unsigned>(N.tmpN),
                 static_cast<unsigned>(slot->compiles));
    }
    const Cmd e = r < f.list.limit ? ReadCmd(f.list, r) : Cmd{};
    const bool ended = resume == 0 && r < f.list.limit &&
                       (Opcode(e.w0) == OP_NOOP || Opcode(e.w0) == OP_SPNOOP || Opcode(e.w0) == OP_ENDDL);
    if (N.tmpN > kNiMaxOpWords || r == start) {
        return ended; /* nothing to keep (an overflow: the span stays the walker's) */
    }
    if (N.poolUsed + N.tmpN + 1u > kNiPoolWords) {
        /* the pool is full: every program goes (each is rebuilt at its next sight) */
        for (uint32_t k = 0; k < kNiSlots; k++) {
            N.slots[k].off = 0;
        }
        N.poolUsed = 0;
        N.w.clears++;
    }
    memcpy(N.pool + N.poolUsed, N.tmp, N.tmpN * sizeof(uint32_t));
    slot->off = N.poolUsed + 1u;
    N.poolUsed += N.tmpN;
    return ended;
}

/* [native] ni_race=2: an ST op's stores vs its words' stock handlers, from the same registers, byte for
 * byte over the register regions (the texture flag included). The stock result stays. */
GDX_COLD void Interp::NrrRaceVerifySt(const uint32_t* op, const ListRef& list, size_t idx, bool root) {
    NrrRace& N = *mNi;
    const uint32_t hdr = op[0];
    const uint32_t nw = (hdr >> 8) & 15u;
    const uint32_t ns = op[1];
    const uint32_t* st = op + 2 + (root ? 3u * nw : 0u);
    uint8_t* base[3];
    uint32_t len[3];
    if (NrrRegRegions(base, len) == 0) {
        return;
    }
    NiRegCopy(base, len, N.fxS, false, false);
    if (NiApply(hdr, st, ns)) {
        mRdp.textures_changed[0] = true;
        mRdp.textures_changed[1] = true;
    }
    NiRegCopy(base, len, N.fxP, false, false); /* B: the program */
    NiRegCopy(base, len, N.fxS, true, false);
    for (uint32_t k = 0; k < nw; k++) {
        const Cmd c = ReadCmd(list, idx + k);
        Frame t;
        t.list = list;
        t.idx = idx + k + 1u;
        (void)Exec(c, t, 0); /* A: the stock handlers */
    }
    NiRegCopy(base, len, N.fxR, false, false);
    const uint32_t total = len[0] + len[1] + len[2];
    if (memcmp(N.fxR, N.fxP, total) == 0) {
        N.w.vOk++;
        return;
    }
    N.w.vBad++;
    if (N.vBadLogged < 16) {
        N.vBadLogged++;
        uint32_t at = 0;
        while (at < total && N.fxR[at] == N.fxP[at]) {
            at++;
        }
        const Cmd c = ReadCmd(list, idx);
        GDXN_LOG("[nirace] vfy MISMATCH root=%d idx=%u words=%u stores=%u hdr=%08X first=%08X %08X byte=%u stock=%02X prog=%02X",
                 root ? 1 : 0, static_cast<unsigned>(idx), nw, ns, hdr, c.w0, c.w1, at, N.fxR[at], N.fxP[at]);
    }
}

} // namespace gdxn
