/* port/3ds/gfx/gdx3ds_stereo_frame.cpp -- n60stereo: the stereo FRAME REPLAY (contract in
 * gdx3ds_stereo.h, "frame replay"; design, gates and costs in docs/research/native/n60stereo-progress.md).
 *
 * The right eye is the left eye's command lists re-submitted: every draw of the body is emitted once
 * with an eye-independent stereo row (`stk`), and the shader multiplies that row by `steye`, which
 * only the prologues here write (raw words, never through citro3d, so no citro3d re-emission can
 * put a stale eye inside the body). Everything runs on the thread that owns the C3D frame.
 *
 * GX queue rules this file relies on (citro3d 1.7.1 + libctru gxqueue, see gdx3ds_overlap.c):
 *   - the frame's queue is cleared at C3D_FrameBegin and holds every entry of the frame until then
 *     (a kick submits entries, it does not remove them): entries [bodyStart, n) are the body;
 *   - gdx3ds_overlap_before_frame_end drains and STOPS a kicked queue, so what we add before
 *     C3D_FrameEnd waits for its gxCmdQueueRun, after its linear-heap flush; our own lists are
 *     added with GX_CMDLIST_FLUSH anyway;
 *   - a GX_MemoryFill entry: data[1..3] = buffer 0 start / value / end, data[4..6] buffer 1. */
#include "gdx3ds_stereo.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "../vendor/citro3d/source/internal.h"
}
#include "../vendor/citro3d/gdx/c3d_gdx.h" /* C3Dgdx_FlushState: the body's state, emitted at its start */
#include "gdx3ds_gputl.h"                    /* n60tail: a folded list's eye for the [gputl] classification */
#include "gdx3ds_replay.h"                   /* n60tail: C6 [replay] keeps its own transfer targets */
#include "gdx3ds_skylate.h"                  /* n60tail: [native] sky_late's view of the body start */
#include "gdx3ds_cpucut.h"                   /* o3ds-cpucuts: [stereo] head_kick_1p = 2 is a block A/B */

extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" int gdx3ds_config_get_bool(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
/* stereo-race-fps [stereo] head_kick (gdx3ds_overlap.c; weak: the DL harness links no overlap). */
extern "C" int gdx3ds_overlap_head_run(void) __attribute__((weak));
/* niview Part 2: the race's live player count (decomp global). >1 means a multi-view (2P/4P split
 * screen) frame -- the frame replay's clip-square scissor (NrrClipScissorWords, gdx3ds_nrr.cpp) is
 * eye-independent (baked once as a fixed-function register word, not a shader output that can read
 * the dynamic steye), so a multi-view frame's outer-edge scissor is wrong per eye under replay
 * (docs/research/native/niview-progress.md §8 item 2). Part 1 (gdx3ds_stereo_nrr_ok /
 * ReplayOkThisFrame, same file) closed the "consumed via the lever but this frame's replay didn't
 * happen" black-section gap for ANY refusal, so refusing here is now safe: every NRR category falls
 * back cleanly to its own correct per-eye stock path instead of leaving a section undrawn. */
extern int gNumPlayers __attribute__((weak));
/* mp-fullview (game/gdx3ds_fullview_game.c): >= 0 while a VS race shows only player k, full screen. */
extern "C" int gdx_fv_live_k(void) __attribute__((weak));
/* o3ds-cpucuts [stereo] head_kick_1p: a lockstep / online MP session is live (net/gdx3ds_mp.c). */
extern "C" int gdx3ds_mp_active(void) __attribute__((weak));

int gdx3ds_stereo_replay_frame = 0;
int gdx3ds_nrr_handoff_open = 0;
float gdx3ds_stereo_rows[2][4] = {};

namespace {

constexpr int kMaxSwaps = 4;     /* mid-body main-target rebinds per frame the replay can swap */
constexpr u32 kReserve = 4;      /* queue slots kept free: prologue, epilogue, two display transfers */
constexpr unsigned kWindow = 256;

/* C3DiF_* everything a body's first draw must re-emit (gdx3ds_replay.cpp kDirtyAll less the
 * framebuffer, which the prologues own). */
constexpr u32 kDirtyBody = C3DiF_AttrInfo | C3DiF_BufInfo | C3DiF_Effect | C3DiF_Viewport | C3DiF_Scissor |
                           C3DiF_Program | C3DiF_TexEnvBuf | C3DiF_TexStatus | C3DiF_FogLut | C3DiF_TexAll |
                           C3DiF_TexEnvAll;

/* A mid-body main-target rebind: queue entry `entry` is the left eye's framebuffer list; the right
 * eye's replay submits `buf`/`words` (the right target's framebuffer words) in its place. */
struct Swap {
    int entry;
    u32* buf;
    u32 words;
};

int sLever = -1;               /* [native] stereo_replay, latched at ReplayInit */
int sStkLoc = -1;
int sEyeLoc = -1;
C3D_RenderTarget* sMain = nullptr;
bool sEyeZeroPending = true;   /* the GPU may not hold steye = 0 (boot, HOME / sleep return) */
aptHookCookie sAptCookie;

bool sInBody = false;
/* niview: "this frame's replay outcome", set ONLY by ReplayEnter() (which runs on the thread that
 * owns the C3D frame -- see the file banner -- never called from here a second time). STICKY: it
 * is NOT cleared at ReplayFrameBegin. First attempt (round 1) cleared it there, "conservative until
 * this frame's real ReplayEnter() runs" -- wrong under the tick-ahead pipe, where the game fiber
 * that reads it (gdx3ds_stereo_nrr_ok, a game-side category admission check) runs AHEAD of the
 * render thread that writes it: the clear-at-begin window is not "rare," it is EVERY frame, so every
 * category refused NRR every frame and the stock interpreter's per-eye double-submit overflowed the
 * GPU command buffer (svcBreak in GPUCMD_Add, gated and caught). So instead this holds the latest
 * decided outcome across frame boundaries -- up to a little pipe-stale, same as this codebase's other
 * cross-thread signals, but never wrongly forced false before its own frame's Enter has even run.
 * Starts true (optimistic: no info yet should never block a category). ReplayOkThisFrame() is the
 * read-only accessor; it must NEVER call ReplayEnter(). */
bool sReplayOkThisFrame = true;
int sChunkStart = 0;           /* queue index of the first entry not yet replayed for the right eye */
int sInterleave = 1;           /* [native] stereo_interleave: replay each kicked chunk at its kick */
int sNoReflush = 1;            /* [native] stereo_noreflush: replayed lists skip GSP's cache flush */
/* n60tail (docs/research/native/n60tail-progress.md). Each: 0 off (default), 1 on, 2 a same-run A/B
 * (on in the even blocks of the frame counter bit named below, off in the odd ones; [gputl] tp=). */
int sFold = 0;                 /* [native] stereo_fold: right prologue in the closing left list, epilogue deferred */
int sXlEarly = 0;              /* [native] stereo_xl_early: the left display transfer right after the left eye */
constexpr unsigned kFoldAbBit = 13;  /* stereo_fold = 2: 8192-frame blocks */
constexpr unsigned kXlAbBit = 11;    /* stereo_xl_early = 2: 2048-frame blocks (zclear_elide = 2 uses bit 10) */
unsigned sFrameNo = 0;         /* frame begins (the A/B blocks) */
bool sFoldNow = false;         /* this frame's stereo_fold phase */
bool sXlNow = false;           /* this frame's stereo_xl_early phase */
bool sKickFolded = false;      /* the current kick's closing list carries the right prologue */
/* stereo-race-fps (STEREO.md "Race GPU path"). Each: 0 off (default), 1 on, 2 a same-run A/B (on in
 * the even blocks of the frame counter bit below, off in the odd ones; [gputl] tp= bits 8 / 16). */
int sHeadKick = 0;             /* [stereo] head_kick: the frame's head (both eyes' clears) runs from the body start */
/* o3ds-cpucuts [stereo] head_kick_1p (DEFAULT 1 = head_kick everywhere; 0 = not in a single-player
 * race; 2 = A/B, gdx3ds_cpucut.h). agent-D-cpu-cuts.md §3.1(b): the head run costs enter_us
 * +330-780 us per D=1 3D frame to start ~1 ms of clears early; in a 1P race the mach kick fires ~4 ms
 * of CPU after the body start and the ~5.4 ms of CPU after it covers chunk c0 + fills, so the drain
 * stays small without it. In MP only ~2 ms of CPU follow the mach kick: kept there. */
int sHeadKick1p = 1;
int sResumeFold = 0;           /* [stereo] resume_fold: the left eye's resume words lead the next left list */
constexpr unsigned kHeadAbBit = 9;    /* head_kick = 2: 512-frame blocks */
constexpr unsigned kResumeAbBit = 12; /* resume_fold = 2: 4096-frame blocks (sky_late = 2 shares it) */
bool sHeadNow = false;         /* this frame's head_kick phase */
bool sResumeNow = false;       /* this frame's resume_fold phase */
/* resume_fold: the command list that begins with the resume words (its buffer address, as its GX
 * entry will carry it) and their size; the right eye replays that list from past them. 0 = none. */
u32 sPrefixVa = 0;
u32 sPrefixBytes = 0;
/* onesubmit (docs/research/native/onesubmit-progress.md, STEREO.md "One submission"): BOTH eyes of a
 * body chunk in ONE GX queue entry. The entry's list is P (left eye only: the resume words, the
 * command-buffer channel 1 return address) + B (the chunk's body: citro3d's full state, the draws, a
 * render-cache flush, GPUREG_CMDBUF_JUMP1) and, behind it in the same memory, R (the right eye's
 * prologue: right framebuffer, steye = +1; channel 0 = B, channel 1 = E; JUMP0) and E (the split's
 * FINALIZE pair). The GPU runs P, B, jumps to R, runs B again for the right eye, jumps to E and raises
 * one P3D interrupt: one GSP submit, one completion IRQ, no CPU between the eyes, the body words not
 * copied. A chunk that is not exactly one open list (a mid-body rebind, an NRR hand-off at the kick,
 * sky_late) falls back to the re-submitted replay, which skips P like resume_fold's words. Levers,
 * latched with the others: onesubmit and onesubmit_narrow default 1 (ship6: hardware card-6 A/B, arm 3),
 * kicks and ab default 0; with onesubmit_ab = 1 the arm cycles per [cpucut] block. */
int sOneSub = 0;          /* [stereo] onesubmit */
int sOneSubKicks = 0;     /* [stereo] onesubmit_kicks: 1 no mid-frame body kick, 2 no kick nor head run */
int sOneSubNarrow = 0;    /* [stereo] onesubmit_narrow: kicks + frame end flush only the noted ranges */
int sOneSubAb = 0;        /* [stereo] onesubmit_ab: an arm per [cpucut] block (base, onesubmit, + kicks, + narrow) */
int sOsArm = 0;           /* this frame's arm: 0 off, 1 onesubmit, 2 + kicks, 3 + narrow, 4 + both */
int sOsArmLogged = -1;
bool sOsNow = false;      /* this frame builds one-submit chunks (decided at ReplayEnter) */
bool sKickOneSub = false; /* the current kick closed its chunk as one entry */
u32* sOsHead = nullptr;   /* the open list this chunk's P starts (nullptr: not a one-submit chunk) */
u32 sOsBodyWord = 0;      /* B's first word in that list */
u32* sOsPatch = nullptr;  /* P's incremental write: [0] SIZE1, [2] ADDR0, [3] ADDR1 */
constexpr u32 kOsTailWords = 64u; /* room R + E need in the command buffer (they take <= 40 words) */
uint64_t sFrameTicks = 0;      /* this frame's right-eye CPU (enter + chunks + end) */
unsigned sFrameChunks = 0;
bool sOtherBound = false;      /* a texture target was bound in the body since the main target */
int sBodyStart = 0;
int sNumSwaps = 0;
Swap sSwaps[kMaxSwaps];
bool sMultiViewLogged = false; /* niview Part 2: "[stereo] replay refused: multi-view", logged once */

struct Window {
    unsigned frames;      /* frames begun */
    unsigned stereo;      /* frames with the right-eye pass active */
    unsigned replayed;    /* frames whose right eye was replayed */
    unsigned refused;     /* ReplayEnter refusals (queue / hold) */
    unsigned multiview;   /* niview Part 2: ReplayEnter refusals (gNumPlayers > 1) */
    unsigned overflow;    /* frame ends that could not fit the replay in the queue */
    unsigned lists;       /* body command lists re-submitted */
    unsigned fills;       /* body clears retargeted */
    unsigned skipped;     /* body captures / copies / uploads not repeated */
    unsigned swaps;       /* mid-body main rebinds swapped */
    unsigned chunks;      /* right-eye chunks replayed at a kick (interleave) */
    unsigned chunkSkip;   /* kicks not used (inside an NRR hand-off / no queue room) */
    unsigned unflushed;   /* replayed lists submitted without GSP's cache flush (noreflush) */
    unsigned gxEntries;   /* GX queue entries of the replayed frames at their end (before the transfers) */
    unsigned folds;       /* n60tail: right prologues folded into a closing left list */
    unsigned eyeDefer;    /* n60tail: epilogues (steye 0) deferred to the next frame's first list */
    unsigned xlEarly;     /* n60tail: left display transfers queued before the right eye's last chunk */
    unsigned headRuns;    /* stereo-race-fps: frames whose head ran from the body start (head_kick) */
    unsigned headClean;   /* ...frames in a head_kick phase whose head was clean (nothing drawn) */
    unsigned headDrew;    /* ...frames refused it: the head held a draw (a heap flush would be due) */
    unsigned head1pSkips; /* o3ds-cpucuts: ...frames head_kick_1p kept from running it (1P race) */
    unsigned resumeFolds; /* stereo-race-fps: resumes whose words lead the next left list (resume_fold) */
    unsigned prefixSkips; /* ...replayed lists that started past those words */
    unsigned osFrames;    /* onesubmit: frames built in a one-submit phase */
    unsigned osChunks;    /* ...chunks sent as one entry for both eyes */
    unsigned osFallback;  /* ...chunks that fell back to the re-submitted replay */
    unsigned osTailWords; /* ...words of R + E written */
    uint64_t enterTicks;  /* CPU in ReplayEnter (the state re-emission included) */
    uint64_t syncTicks;   /* ...of which the body's full citro3d state re-emission */
    uint64_t endTicks;    /* CPU in ReplayFrameEnd */
    uint64_t chunkTicks;  /* CPU in the kick-time chunk replays */
} sWin;

gxCmdQueue_s* Queue() {
    return &C3Di_GetContext()->gxQueue;
}

void Log(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    if (&gdx3ds_filelog_write != nullptr) {
        gdx3ds_filelog_write(msg, std::strlen(msg));
    }
}

void AptHook(APT_HookType hook, void* param) {
    (void)param;
    if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        sEyeZeroPending = true; /* another process drove the GPU: its uniform file is gone */
    }
}

/* steye = (e, 0, 0, 0), raw: C3D_FVec's register image is (w, z, y, x). */
void EmitEye(float e) {
    C3D_FVec v;
    v.x = e;
    v.y = 0.0f;
    v.z = 0.0f;
    v.w = 0.0f;
    GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | static_cast<u32>(sEyeLoc));
    GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, reinterpret_cast<u32*>(&v), 4);
}

/* The words of a switch to `target`'s framebuffer, in the open command list: the render cache
 * flushed first when anything was drawn since the last flush (C3Di_UpdateContext's rule). */
void EmitFrameBuf(C3D_RenderTarget* target) {
    C3D_Context* ctx = C3Di_GetContext();
    if (ctx->flags & C3DiF_DrawUsed) {
        ctx->flags &= ~static_cast<u32>(C3DiF_DrawUsed);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
    }
    C3Di_FrameBufBind(&target->frameBuf);
}

/* Close the open command list into the queue (flushed), if it holds anything. */
void SubmitOpenList() {
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
}

/* The words C3Di_SplitFrame closes a list with after a draw (render cache flush + invalidate,
 * early-depth clear), written now: a folded prologue then follows them in the same list, so the GPU
 * runs exactly the words the unfolded split + prologue list would have (less one FINALIZE pair). */
void EmitSplitFlush() {
    C3D_Context* ctx = C3Di_GetContext();
    if (ctx->flags & C3DiF_DrawUsed) {
        ctx->flags &= ~static_cast<u32>(C3DiF_DrawUsed);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
        GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
    }
}

/* The right eye's prologue (right target, steye = +1) at the END of the open left list, which the
 * caller closes next: the list then leaves the right target bound, and its replay re-runs those
 * words on the right target (a flush of the right target's render cache, the same bind: no-ops for
 * the image). `entry` = the queue index that list will get ([gputl] classifies it by this note). */
void EmitFoldedPrologue(int entry) {
    EmitSplitFlush();
    EmitFrameBuf(Gdx3dsStereo::RightTarget()); /* DrawUsed is clear: the bind words only */
    EmitEye(1.0f);
    gdx3ds_gputl_note_eye(entry, 1);
    sWin.folds++;
}

/* Close the open command list WITHOUT queueing it (the right eye's swap-in). */
void DetachOpenList(u32** buf, u32* words) {
    GPUCMD_Split(buf, words);
}

bool QueueRoom(u32 need) {
    const gxCmdQueue_s* q = Queue();
    return static_cast<u32>(q->numEntries) + need <= static_cast<u32>(q->maxEntries);
}

const Swap* SwapFor(int entry) {
    for (int i = 0; i < sNumSwaps; i++) {
        if (sSwaps[i].entry == entry) {
            return &sSwaps[i];
        }
    }
    return nullptr;
}

/* A clear of the main target, retargeted to the right one (same size and formats). */
void RetargetFill(gxCmdEntry_s& e) {
    const C3D_FrameBuf& m = sMain->frameBuf;
    const C3D_FrameBuf& r = Gdx3dsStereo::RightTarget()->frameBuf;
    for (int b = 0; b < 2; b++) {
        u32& start = e.data[1 + 3 * b];
        u32& end = e.data[3 + 3 * b];
        u32 to = 0;
        if (start == reinterpret_cast<u32>(m.colorBuf)) {
            to = reinterpret_cast<u32>(r.colorBuf);
        } else if (start == reinterpret_cast<u32>(m.depthBuf)) {
            to = reinterpret_cast<u32>(r.depthBuf);
        }
        if (to != 0) {
            end = to + (end - start);
            start = to;
        }
    }
}

bool FillTouchesMain(const gxCmdEntry_s& e) {
    const C3D_FrameBuf& m = sMain->frameBuf;
    for (int b = 0; b < 2; b++) {
        const u32 start = e.data[1 + 3 * b];
        if (start == reinterpret_cast<u32>(m.colorBuf) || start == reinterpret_cast<u32>(m.depthBuf)) {
            return true;
        }
    }
    return false;
}

/* A body command list again, for the right eye. Its words are unchanged since its own entry
 * (nothing writes a closed list) and that entry ran first in the same queue, so under
 * [native] stereo_noreflush the copy drops GSP's data-cache flush of the list (word 7 of a
 * ProcessCommandList command): the GPU reads the same bytes, the GSP skips a flush by address of
 * the whole list before it starts the GPU on it. */
void ReplayList(const gxCmdEntry_s& src) {
    const bool skipPrefix = sPrefixBytes != 0u && src.data[1] == sPrefixVa && src.data[2] > sPrefixBytes;
    const bool unflush = sNoReflush > 0 && src.data[7] != 0u;
    if (!skipPrefix && !unflush) {
        gxCmdQueueAdd(Queue(), &src);
        return;
    }
    gxCmdEntry_s e = src;
    if (skipPrefix) {
        /* stereo-race-fps resume_fold: the list's first words are the left eye's resume (main
         * framebuffer, steye = -1); the right eye starts right after them, 16-byte aligned. */
        e.data[1] = src.data[1] + sPrefixBytes;
        e.data[2] = src.data[2] - sPrefixBytes;
        sWin.prefixSkips++;
    }
    if (unflush) {
        e.data[7] = 0u;
        sWin.unflushed++;
    }
    gxCmdQueueAdd(Queue(), &e);
}

/* Re-add one body entry for the right eye. */
void ReplayEntry(int index, const gxCmdEntry_s& src) {
    const u32 id = src.data[0] & 0xFFu;
    if (id == 0x01u) {
        const Swap* sw = SwapFor(index);
        if (sw != nullptr) {
            GX_ProcessCommandList(sw->buf, sw->words * 4u, GX_CMDLIST_FLUSH);
            sWin.swaps++;
        } else {
            ReplayList(src);
            sWin.lists++;
        }
    } else if (id == 0x02u) {
        /* A clear: the main target's goes to the right target; a texture pass's target is cleared
         * again, because the replay draws that pass again. */
        gxCmdEntry_s e = src;
        if (FillTouchesMain(src)) {
            RetargetFill(e);
            sWin.fills++;
        }
        gxCmdQueueAdd(Queue(), &e);
    } else {
        sWin.skipped++; /* captures, copies, uploads: mono, done once (the per-draw loop's rule) */
    }
}


void MarkBodySelfContained() {
    C3D_Context* ctx = C3Di_GetContext();
    ctx->flags = (ctx->flags | kDirtyBody) & ~static_cast<u32>(C3DiF_FrameBuf);
    C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
    /* stk is re-sent by the body even if citro3d never sent it before (EverDirty would skip it). */
    const C3D_FVec& cur = C3D_FVUnif[GPU_VERTEX_SHADER][sStkLoc];
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sStkLoc, cur.x, cur.y, cur.z, cur.w);
}

/* Re-add the queue entries [sChunkStart, n) for the right eye, between its prologue (right target,
 * steye = +1) and nothing: the caller adds what follows. false = no queue room (nothing added).
 * `folded`: the last of those entries already ends with the prologue (EmitFoldedPrologue), so no
 * prologue list; the caller checked the room before folding. */
bool ReplayPendingChunk(u32 tail, bool folded) {
    const int start = sChunkStart;
    const int n = static_cast<int>(Queue()->numEntries) - start;
    if (n <= 0) {
        return true;
    }
    if (!QueueRoom(static_cast<u32>(n) + tail)) {
        return false;
    }
    gxCmdEntry_s chunk[32];
    std::memcpy(chunk, Queue()->entries + start, sizeof(gxCmdEntry_s) * static_cast<size_t>(n));
    if (!folded) {
        EmitFrameBuf(Gdx3dsStereo::RightTarget());
        EmitEye(1.0f);
        SubmitOpenList();
    }
    for (int i = 0; i < n; i++) {
        ReplayEntry(start + i, chunk[i]);
    }
    return true;
}

/* stereo-race-fps resume_fold: steye = (e, 0, 0, 0) like EmitEye, ending the open list's words on a
 * 16-byte boundary (GSP command lists start and end on one): when the words so far plus EmitEye's 8
 * leave 2 over, the uniform config write is repeated first. Writing GPUREG_VSH_FLOATUNIFORM_CONFIG
 * twice with the same value and no data between is the same as writing it once (it only selects the
 * register the data words go to). false = the open list is not word-pair aligned (never seen): the
 * caller falls back to a list of its own. */
bool EmitEyeAligned(float e) {
    if ((gpuCmdBufOffset & 1u) != 0u) {
        EmitEye(e);
        return false;
    }
    if (((gpuCmdBufOffset + 8u) & 3u) != 0u) {
        GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | static_cast<u32>(sEyeLoc));
    }
    EmitEye(e);
    return (gpuCmdBufOffset & 3u) == 0u;
}

/* stereo-race-fps resume_fold: the resume words stay at the head of the open (empty) list instead of
 * closing into a GX entry of their own; the list they lead is replayed for the right eye from past
 * them (ReplayList). The left eye runs the same words in the same order, less one FINALIZE pair.
 * false = not in this frame's phase, or the open list is not empty / not 16-byte aligned: nothing
 * written, the caller takes the unfolded path. */
bool FoldResumeWords() {
    if (!sResumeNow || gpuCmdBufOffset != 0u || gpuCmdBuf == nullptr) {
        return false;
    }
    const u32 head = reinterpret_cast<u32>(gpuCmdBuf);
    if ((head & 15u) != 0u) {
        return false;
    }
    EmitFrameBuf(sMain);
    if (!EmitEyeAligned(-1.0f)) {
        SubmitOpenList(); /* the words are written: close them in a list of their own, as unfolded */
        return true;
    }
    sPrefixVa = head;
    sPrefixBytes = gpuCmdBufOffset * 4u;
    sWin.resumeFolds++;
    return true;
}

/* onesubmit: a command-buffer address as GPUREG_CMDBUF_ADDRn holds it (physical, in 8-byte units). */
u32 CmdBufAddr8(const u32* p) {
    return osConvertVirtToPhys(p) >> 3;
}

bool Aligned16(const u32* p) {
    return (reinterpret_cast<u32>(p) & 15u) == 0u;
}

/* onesubmit: P's channel words -- GPUREG_CMDBUF_SIZE1 / ADDR0 / ADDR1 in one incremental write (4
 * words: it keeps a 16-byte aligned offset aligned), patched by OsClose once R's place is known; then
 * B starts. P lives at `head`, the open list's first word. The re-submitted replay of a chunk that
 * falls back starts past P (ReplayList), as it starts past resume_fold's words. */
void OsBeginChunk(u32* head) {
    const u32 zero[3] = {0u, 0u, 0u};
    sOsPatch = gpuCmdBuf + gpuCmdBufOffset;
    GPUCMD_AddIncrementalWrites(GPUREG_CMDBUF_SIZE1, zero, 3);
    sOsHead = head;
    sOsBodyWord = gpuCmdBufOffset;
    sPrefixVa = reinterpret_cast<u32>(head);
    sPrefixBytes = gpuCmdBufOffset * 4u;
}

/* onesubmit: whether the open list is exactly this chunk ([P][B], nothing queued since the chunk
 * started) and the queue and the command buffer have the room its close needs. */
bool OsChunkEligible(u32 tail) {
    if (!sOsNow || sOsHead == nullptr || gpuCmdBuf != sOsHead) {
        return false;
    }
    if (static_cast<int>(Queue()->numEntries) != sChunkStart || gpuCmdBufOffset <= sOsBodyWord ||
        (gpuCmdBufOffset & 1u) != 0u) {
        return false;
    }
    if (gpuCmdBufOffset + kOsTailWords > gpuCmdBufSize) {
        return false;
    }
    return QueueRoom(1u + tail);
}

/* onesubmit: B's tail -- the render-cache flush C3Di_SplitFrame would write, then JUMP1, ending on a
 * 16-byte boundary (a repeated FRAMEBUFFER_FLUSH pads; flushing twice is flushing once). Both passes
 * run it: each flushes its own eye's target and jumps where channel 1 points (P set it to R, R sets
 * it to E). */
void OsEmitBodyTail() {
    C3Di_GetContext()->flags &= ~static_cast<u32>(C3DiF_DrawUsed);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
    if ((gpuCmdBufOffset & 3u) == 0u) {
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    }
    GPUCMD_AddWrite(GPUREG_CMDBUF_JUMP1, 1);
}

/* onesubmit: R -- the right eye's prologue (the folded prologue's words), then channel 0 = B,
 * channel 1 = E (16 bytes: the split's FINALIZE pair right behind R), JUMP0. false = R could not end
 * 16-byte aligned (never seen: GPUCMD offsets are always even); the caller then does not submit it. */
bool OsEmitRight(const u32* body, u32 bodyWords) {
    EmitFrameBuf(Gdx3dsStereo::RightTarget()); /* DrawUsed is clear: the bind words only */
    if (!EmitEyeAligned(1.0f)) {
        return false;
    }
    u32 ch[4];
    ch[0] = bodyWords / 2u;                          /* SIZE0: B, in 8-byte units */
    ch[1] = 2u;                                      /* SIZE1: E, 16 bytes */
    ch[2] = CmdBufAddr8(body);                       /* ADDR0 */
    ch[3] = CmdBufAddr8(gpuCmdBuf + gpuCmdBufOffset + 8u); /* ADDR1: E, after these 6 + 2 words */
    GPUCMD_AddIncrementalWrites(GPUREG_CMDBUF_SIZE0, ch, 4);
    GPUCMD_AddWrite(GPUREG_CMDBUF_JUMP0, 1);
    return (gpuCmdBufOffset & 3u) == 0u;
}

/* onesubmit: close the open list [P][B] as ONE GX entry that runs B for both eyes. The entry hands
 * the GSP [P][B] (B's JUMP1 is the last word it is given, its cache flush covers both); R and E are
 * flushed here (<= 160 bytes, one svcFlushProcessDataCache). The GPU ends with the right target and
 * steye = +1 bound. false = not closed (unreachable while GPUCMD offsets stay even): B's JUMP1 is
 * turned back into a cache flush, and the caller falls back with the words written so far (B's
 * flushes and a right-eye bind the replay's own prologue repeats). */
bool OsClose() {
    OsEmitBodyTail();
    u32* body = sOsHead + sOsBodyWord;
    const u32 bodyEnd = gpuCmdBufOffset;
    u32* right = gpuCmdBuf + bodyEnd;
    if (!OsEmitRight(body, bodyEnd - sOsBodyWord)) {
        gpuCmdBuf[bodyEnd - 1u] = GPUCMD_HEADER(0, 0xF, GPUREG_FRAMEBUFFER_FLUSH);
        sOsHead = nullptr;
        return false;
    }
    const u32 rightWords = gpuCmdBufOffset - bodyEnd;
    sOsPatch[0] = rightWords / 2u; /* SIZE1: R */
    sOsPatch[2] = CmdBufAddr8(body);
    sOsPatch[3] = CmdBufAddr8(right);
    u32* buf = nullptr;
    u32 words = 0;
    GPUCMD_Split(&buf, &words); /* + E: FINALIZE x2 at the 16-byte aligned offset */
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(right), (words - bodyEnd) * 4u);
    gdx3ds_gputl_note_eye(static_cast<int>(Queue()->numEntries), 3);
    GX_ProcessCommandList(buf, bodyEnd * 4u, GX_CMDLIST_FLUSH);
    sWin.osChunks++;
    sWin.osTailWords += words - bodyEnd;
    sOsHead = nullptr;
    return true;
}

/* onesubmit: the next chunk's P at the head of the open (empty, aligned) list: the left eye's resume
 * words (main framebuffer, steye = -1), then the channel words. false = the list is not empty /
 * aligned: nothing written, the caller resumes the classic way. */
bool OsResume() {
    if (gpuCmdBuf == nullptr || gpuCmdBufOffset != 0u || !Aligned16(gpuCmdBuf)) {
        return false;
    }
    u32* head = gpuCmdBuf;
    EmitFrameBuf(sMain);
    if (!EmitEyeAligned(-1.0f)) {
        SubmitOpenList(); /* the words are written: a list of their own, this chunk re-submitted */
        return true;
    }
    OsBeginChunk(head);
    return true;
}

/* The left eye again (main target, steye = -1) and a fresh self-contained start for the next chunk. */
void ResumeLeftEye() {
    sPrefixVa = 0u;
    sPrefixBytes = 0u;
    sOsHead = nullptr;
    if (sOsNow && OsResume()) {
        /* onesubmit: P leads the next list */
    } else if (!FoldResumeWords()) {
        EmitFrameBuf(sMain);
        EmitEye(-1.0f);
        SubmitOpenList();
    }
    sChunkStart = Queue()->numEntries;
    MarkBodySelfContained();
    const uint64_t ts = svcGetSystemTick();
    C3Dgdx_FlushState();
    sWin.syncTicks += svcGetSystemTick() - ts;
}

/* Whether a list closed now (queue index numEntries) plus the replay of [sChunkStart, that list]
 * fits with `tail` slots left, as ReplayPendingChunk(tail) will test it after the close: the room
 * test of a fold, made BEFORE its words are written so that a folded list is always replayed. */
bool FoldRoom(u32 tail) {
    const u32 closed = static_cast<u32>(Queue()->numEntries) + 1u;
    const u32 n = closed - static_cast<u32>(sChunkStart);
    return closed + n + tail <= static_cast<u32>(Queue()->maxEntries);
}

/* n60tail [native] stereo_xl_early: the left eye's display transfer, queued right after the left
 * eye's last list (whose split flushed the left render cache) instead of by C3D_FrameEnd after the
 * right eye; C3D_FrameEnd then transfers only the right target (main->used cleared) and still
 * requests the top swap (the right target is used, so needSwapTop and isTopStereo are set as
 * before). Same PPF operation, same source, same destination (gfxGetFramebuffer: no swap happens
 * before C3D_FrameEnd's own queue finishes). */
bool QueueLeftTransferEarly() {
    C3D_RenderTarget* right = Gdx3dsStereo::RightTarget();
    if (!sXlNow || sMain == nullptr || right == nullptr || !sMain->used || !right->used || !sMain->linked ||
        sMain->screen != GFX_TOP || right->screen != GFX_TOP || gdx3ds_replay_on) {
        return false;
    }
    C3D_FrameBufTransfer(&sMain->frameBuf, sMain->screen, sMain->side, sMain->transferFlags);
    sMain->used = false;
    sWin.xlEarly++;
    return true;
}

/* stereo-race-fps head_kick: the frame's GX queue so far holds no command list and the open list no
 * draw since its last split (C3DiF_DrawUsed, set by every draw path: citro3d's, the diet's and the NRR
 * direct emitter's), i.e. the GPU would read nothing CPU-written but command words (the clears read
 * nothing). Then starting it needs no heap flush. Counted per frame in a head_kick phase. */
/* Whether the PICA words w[0..n) write a register that draws (GPUREG_DRAWARRAYS 0x22E,
 * GPUREG_DRAWELEMENTS 0x22F, the immediate-mode vertex port GPUREG_FIXEDATTRIB_INDEX 0x232,
 * GPUREG_START_DRAW_FUNC0 0x245). A list longer than the cap counts as drawing (not scanned). */
bool WordsMayDraw(const u32* w, u32 n) {
    constexpr u32 kCap = 1024u;
    if (n > kCap) {
        return true;
    }
    u32 i = 0;
    while (i + 1u < n) {
        const u32 hdr = w[i + 1u];
        const u32 reg = hdr & 0xFFFFu;
        const u32 extra = (hdr >> 20) & 0xFFu;
        const bool seq = (hdr >> 31) != 0u;
        for (u32 k = 0; k <= extra; k++) {
            const u32 r = seq ? reg + k : reg;
            if (r == 0x22Eu || r == 0x22Fu || r == 0x232u || r == 0x245u) {
                return true;
            }
        }
        i += 2u + extra;
        i = (i + 1u) & ~1u;
    }
    return false;
}

bool HeadIsClean() {
    const gxCmdQueue_s* q = Queue();
    bool clean = (C3Di_GetContext()->flags & C3DiF_DrawUsed) == 0u && gpuCmdBuf != nullptr &&
                 !WordsMayDraw(gpuCmdBuf, gpuCmdBufOffset);
    for (int k = 0; clean && k < static_cast<int>(q->numEntries); k++) {
        if ((q->entries[k].data[0] & 0xFFu) == 0x01u) {
            clean = false;
        }
    }
    if (clean) {
        sWin.headClean++;
    } else {
        sWin.headDrew++;
    }
    return clean;
}

/* A single-player frame: no MP session and one player on the race screen. */
bool SinglePlayerFrame() {
    const bool mp = gdx3ds_mp_active != nullptr && gdx3ds_mp_active() != 0;
    const bool onePlayer = &gNumPlayers == nullptr || gNumPlayers <= 1;
    return !mp && onePlayer;
}

/* head_kick_1p: whether this frame may run its head early (head_kick's own phase decided apart). */
bool HeadKickAllowed() {
    if (gdx3ds_cpucut_on(sHeadKick1p, GDX3DS_CPUCUT_BIT_HEAD1P) || !SinglePlayerFrame()) {
        return true;
    }
    sWin.head1pSkips++;
    return false;
}

bool PhaseOn(int lever, unsigned abBit) {
    if (lever == 1) {
        return true;
    }
    return lever == 2 && ((sFrameNo >> abBit) & 1u) == 0u;
}

/* onesubmit: the arm the levers ask for now (onesubmit_ab: the [cpucut] block's, so the overlap
 * module and this file agree within a frame without sharing a latch). The ab order over 8 blocks,
 * 0 1 2 3 2 3 0 1, runs every arm once with block bit 1 clear and once with it set: the card's
 * other A/B levers on bit 1 (steady_d1_drift, head_kick_1p) do not line up with one arm. */
int OneSubArm() {
    if (sOneSubAb != 0) {
        static const int kOrder[8] = { 0, 1, 2, 3, 2, 3, 0, 1 };
        return kOrder[gdx3ds_cpucut_block() & 7ul];
    }
    if (sOneSub == 0) {
        return 0;
    }
    return 1 + (sOneSubKicks != 0 ? 1 : 0) + (sOneSubNarrow != 0 ? 2 : 0);
}

const char* OneSubArmName(int arm) {
    static const char* const kNames[5] = { "baseline", "onesubmit", "onesubmit+kicks", "onesubmit+narrow",
                                           "onesubmit+kicks+narrow" };
    return arm >= 0 && arm < 5 ? kNames[arm] : "?";
}

/* onesubmit: this frame may build one-submit chunks (sky_late reorders the body's lists, the C6
 * [replay] owns the transfer targets: both keep the re-submitted replay). */
bool OneSubFrameWanted() {
    if (sOsArm <= 0 || gdx3ds_replay_on) {
        return false;
    }
    return gdx3ds_skylate_phase() == 0;
}

void LatchRows() {
    Gdx3dsStereo::ClassRow(GDX3DS_STEREO_SCENE, gdx3ds_stereo_rows[0]);
    Gdx3dsStereo::ClassRow(GDX3DS_STEREO_UI_ZERO_PARALLAX, gdx3ds_stereo_rows[1]);
}

double TicksToMs(uint64_t t) {
    return static_cast<double>(t) * 1000.0 / static_cast<double>(SYSCLOCK_ARM11);
}

/* stereo-race-fps: the init receipt of the race GPU-path levers (always, with stereo enabled). */
void LogRaceInit() {
    char msg[384];
    std::snprintf(msg, sizeof(msg),
                  "[stereo] race init head_kick=%d head_kick_1p=%d resume_fold=%d (head_kick: both eyes' clears + "
                  "the pre-body list start on the GPU at the body start; resume_fold: the left eye's resume words "
                  "lead the next left list, the right eye replays it from past them; 2 = A/B, [gputl] tp= +8 / +16)",
                  sHeadKick, sHeadKick1p, sResumeFold);
    Log(msg);
}

/* stereo-race-fps: `[stereo] race` per replay window, only while a race lever is set (the replay line
 * itself is unchanged). hk/f = head runs per replayed frame, hkclean / hkdrew = frames in a head_kick
 * phase whose head was clean / held a draw (refused), rfold/f = resumes folded, rskip/f = replayed
 * lists started past the resume words (rskip = rfold when every folded list was replayed). */
void EmitRaceWindow(double per) {
    if (sHeadKick == 0 && sResumeFold == 0) {
        return;
    }
    char msg[288];
    std::snprintf(msg, sizeof(msg),
                  "[stereo] race frames=%u replayed=%u hk/f=%.2f hkclean=%u hkdrew=%u hk1p_skip=%u rfold/f=%.2f "
                  "rskip/f=%.2f head=%d resume=%d",
                  sWin.frames, sWin.replayed, sWin.headRuns / per, sWin.headClean, sWin.headDrew, sWin.head1pSkips,
                  sWin.resumeFolds / per, sWin.prefixSkips / per, sHeadNow ? 1 : 0, sResumeNow ? 1 : 0);
    Log(msg);
}

/* onesubmit: `[stereo] onesub` per replay window while a onesubmit lever is set. osf = frames built
 * in a one-submit phase, os/f = chunks sent as one entry per replayed frame, fb = chunks that fell
 * back to the re-submitted replay, tailw/f = R + E words per replayed frame. */
void EmitOneSubWindow(double per) {
    if (sOneSub == 0 && sOneSubAb == 0) {
        return;
    }
    char msg[224];
    std::snprintf(msg, sizeof(msg), "[stereo] onesub frames=%u replayed=%u osf=%u os/f=%.2f fb=%u tailw/f=%.1f arm=%d",
                  sWin.frames, sWin.replayed, sWin.osFrames, sWin.osChunks / per, sWin.osFallback,
                  sWin.osTailWords / per, sOsArm);
    Log(msg);
}

void EmitWindow() {
    if (sWin.frames < kWindow) {
        return;
    }
    if (sWin.stereo > 0) {
        char msg[416];
        const double per = sWin.replayed > 0 ? static_cast<double>(sWin.replayed) : 1.0;
        std::snprintf(msg, sizeof(msg),
                      "[stereo] replay frames=%u stereo=%u replayed=%u refused=%u multiview=%u full=%u "
                      "lists/f=%.2f "
                      "fills/f=%.2f skip/f=%.2f swaps=%u chunks/f=%.2f chunkskip=%u noflush/f=%.2f gx/f=%.1f enter_us=%.1f "
                      "(sync %.1f) chunk_us=%.1f end_us=%.1f fold/f=%.2f eye0defer/f=%.2f xl/f=%.2f",
                      sWin.frames, sWin.stereo, sWin.replayed, sWin.refused, sWin.multiview, sWin.overflow,
                      sWin.lists / per, sWin.fills / per, sWin.skipped / per, sWin.swaps, sWin.chunks / per,
                      sWin.chunkSkip, sWin.unflushed / per, sWin.gxEntries / per, TicksToMs(sWin.enterTicks) * 1000.0 / per,
                      TicksToMs(sWin.syncTicks) * 1000.0 / per, TicksToMs(sWin.chunkTicks) * 1000.0 / per,
                      TicksToMs(sWin.endTicks) * 1000.0 / per, sWin.folds / per, sWin.eyeDefer / per,
                      sWin.xlEarly / per);
        Log(msg);
        EmitRaceWindow(per);
        EmitOneSubWindow(per);
    }
    std::memset(&sWin, 0, sizeof(sWin));
}

} // namespace

namespace Gdx3dsStereo {

/* preset-fix: the replay levers, latched once stereo is ENABLED -- at boot when the ini boots in 3D,
 * else at the first frame after a late Init (the Home 3D button, [stereo] mp_ab race 2 / mp_3d in a
 * 2D-booted session). card-2 hw (o3ds-card2b): an Old 3DS booted with [stereo] enabled = 0 never ran
 * this, so every 3D frame after the button took the per-draw loop (the whole frame drawn twice on
 * the CPU: race wall 44-47 ms/tick, menus misc 3x). `when` names the path in the receipt. */
void LatchReplayLevers(const char* when) {
    sLever = 1;
    if (&gdx3ds_config_get_int != nullptr) {
        sLever = gdx3ds_config_get_int("native", "stereo_replay", 1) != 0 ? 1 : 0;
        sInterleave = gdx3ds_config_get_int("native", "stereo_interleave", 1) != 0 ? 1 : 0;
        sNoReflush = gdx3ds_config_get_int("native", "stereo_noreflush", 1) != 0 ? 1 : 0;
        const int fold = gdx3ds_config_get_int("native", "stereo_fold", 0);
        const int xl = gdx3ds_config_get_int("native", "stereo_xl_early", 0);
        sFold = (fold == 1 || fold == 2) ? fold : 0;
        sXlEarly = (xl == 1 || xl == 2) ? xl : 0;
        const int head = gdx3ds_config_get_int("stereo", "head_kick", 0);
        const int resume = gdx3ds_config_get_int("stereo", "resume_fold", 0);
        sHeadKick = (head == 1 || head == 2) ? head : 0;
        sHeadKick1p = gdx3ds_cpucut_clamp(gdx3ds_config_get_int("stereo", "head_kick_1p", 1), 1);
        gdx3ds_cpucut_lever("head1p", &sHeadKick1p, GDX3DS_CPUCUT_BIT_HEAD1P);
        sResumeFold = (resume == 1 || resume == 2) ? resume : 0;
        sOneSub = gdx3ds_config_get_int("stereo", "onesubmit", 1) != 0 ? 1 : 0;
        const int kicks = gdx3ds_config_get_int("stereo", "onesubmit_kicks", 0);
        sOneSubKicks = (kicks == 1 || kicks == 2) ? kicks : 0;
        sOneSubNarrow = gdx3ds_config_get_int("stereo", "onesubmit_narrow", 1) != 0 ? 1 : 0;
        sOneSubAb = gdx3ds_config_get_int("stereo", "onesubmit_ab", 0) != 0 ? 1 : 0;
    }
    if (sStkLoc < 0 || sEyeLoc < 0 || sMain == nullptr || RightTarget() == nullptr) {
        sLever = 0;
    }
    char msg[352];
    std::snprintf(msg, sizeof(msg),
                  "[stereo] replay init stereo_replay=%d stereo_interleave=%d stereo_noreflush=%d stereo_fold=%d "
                  "stereo_xl_early=%d stk=%d steye=%d at=%s (%s)",
                  sLever, sInterleave, sNoReflush, sFold, sXlEarly, sStkLoc, sEyeLoc, when,
                  sLever ? "right eye = the left's command lists re-submitted" : "per-draw loop");
    Log(msg);
    LogRaceInit();
    if (sOneSub != 0 || sOneSubAb != 0) {
        std::snprintf(msg, sizeof(msg),
                      "[stereo] onesubmit init onesubmit=%d kicks=%d narrow=%d ab=%d (both eyes of a body chunk in one "
                      "GX entry: P + B + JUMP1 -> R + JUMP0 -> B -> E; ab: arm per [cpucut] block, order 0 1 2 3 2 3 0 1: "
                      "0 baseline 1 onesubmit 2 + kicks 3 + narrow; [onesub] at each arm change)",
                      sOneSub, sOneSubKicks, sOneSubNarrow, sOneSubAb);
        Log(msg);
    }
}

void ReplayInit(int stkLoc, int eyeLoc, C3D_RenderTarget* mainTarget) {
    sStkLoc = stkLoc;
    sEyeLoc = eyeLoc;
    sMain = mainTarget;
    aptHook(&sAptCookie, AptHook, nullptr);
    if (stkLoc >= 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, stkLoc, 0.0f, 0.0f, 0.0f, 0.0f); /* citro3d owns stk from here */
    }
    if (!Enabled()) {
        /* -1 = deferred: ReplayFrameBegin latches the levers on the first frame stereo is enabled. */
        sLever = -1;
        Log("[stereo] replay init deferred (stereo off at boot): the replay levers latch when 3D is first "
            "enabled (Home 3D button / mp_3d / mp_ab)");
        return;
    }
    LatchReplayLevers("boot");
}

void ReplayFrameBegin() {
    sFrameTicks = 0;
    sFrameChunks = 0;
    sInBody = false;
    sOtherBound = false;
    sNumSwaps = 0;
    sKickFolded = false;
    sFrameNo++;
    if (sLever < 0 && Enabled()) {
        LatchReplayLevers("late-enable"); /* preset-fix: a 2D-booted session's first 3D frame */
    }
    sFoldNow = PhaseOn(sFold, kFoldAbBit);
    sXlNow = PhaseOn(sXlEarly, kXlAbBit);
    sHeadNow = PhaseOn(sHeadKick, kHeadAbBit);
    sResumeNow = PhaseOn(sResumeFold, kResumeAbBit);
    sPrefixVa = 0u;
    sPrefixBytes = 0u;
    sOsNow = false;
    sKickOneSub = false;
    sOsHead = nullptr;
    sOsArm = sLever > 0 ? OneSubArm() : 0;
    if ((sOneSub != 0 || sOneSubAb != 0) && sOsArm != sOsArmLogged && Active()) {
        sOsArmLogged = sOsArm;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "[onesub] blk=%lu arm=%d (%s) frame=%u", gdx3ds_cpucut_block(), sOsArm,
                      OneSubArmName(sOsArm), sFrameNo);
        Log(msg);
    }
    gdx3ds_stereo_replay_frame = 0;
    /* steye = 0 here also when the previous frame deferred its epilogue (stereo_fold): this frame's
     * first list, before any draw of it. */
    if (sEyeZeroPending && sEyeLoc >= 0) {
        EmitEye(0.0f);
        sEyeZeroPending = false;
    }
    if (sLever > 0) {
        sWin.frames++;
        if (Active()) {
            sWin.stereo++;
        }
        EmitWindow();
    }
}

bool ReplayEnter() {
    if (sInBody) {
        return true;
    }
    if (sLever <= 0 || !Active()) {
        sReplayOkThisFrame = false;
        return false;
    }
    /* mp-fullview: a 2-4P VS race showing only player k full screen is one full-screen view. */
    const bool fullview = (gdx_fv_live_k != nullptr) && (gdx_fv_live_k() >= 0);
    if (&gNumPlayers != nullptr && gNumPlayers > 1 && !fullview) { /* niview Part 2: §8 item 2, the outer-edge scissor */
        sWin.multiview++;
        if (!sMultiViewLogged) {
            sMultiViewLogged = true;
            Log("[stereo] replay refused: multi-view");
        }
        sReplayOkThisFrame = false;
        return false;
    }
    if (HoldFrame() || !QueueRoom(2u * kReserve + 8u)) {
        sWin.refused++;
        sReplayOkThisFrame = false;
        return false;
    }
    gdx3ds_skylate_before_enter(); /* n60tail: may the sky's run start at the body start? */
    const uint64_t t0 = svcGetSystemTick();
    const bool headClean = sHeadNow && HeadKickAllowed() && HeadIsClean(); /* before EmitFrameBuf clears DrawUsed */
    /* The pre-body list ends with the left eye's own words: steye = -1, the main framebuffer. */
    EmitEye(-1.0f);
    EmitFrameBuf(sMain);
    SubmitOpenList();
    if (headClean && &gdx3ds_overlap_head_run != nullptr && gdx3ds_overlap_head_run() != 0) {
        sWin.headRuns++; /* stereo-race-fps head_kick: both eyes' clears run while the body is built */
    }
    sBodyStart = Queue()->numEntries;
    sChunkStart = sBodyStart;
    gdx3ds_skylate_after_enter(sBodyStart);
    sOsNow = OneSubFrameWanted();
    if (sOsNow) {
        sWin.osFrames++;
        if (gpuCmdBuf != nullptr && gpuCmdBufOffset == 0u && Aligned16(gpuCmdBuf)) {
            OsBeginChunk(gpuCmdBuf); /* onesubmit: chunk 0's P (the pre-body list left the left eye bound) */
        }
    }
    MarkBodySelfContained();
    const uint64_t ts = svcGetSystemTick();
    C3Dgdx_FlushState(); /* every state group and uniform now, at the body's first word */
    sWin.syncTicks += svcGetSystemTick() - ts;
    LatchRows();
    sInBody = true;
    sReplayOkThisFrame = true;
    gdx3ds_stereo_replay_frame = 1;
    const uint64_t spent = svcGetSystemTick() - t0;
    sWin.enterTicks += spent;
    sFrameTicks += spent;
    return true;
}

bool ReplayActive() {
    return sInBody;
}

/* niview: read-only, cross-thread-safe (a single bool, written only by ReplayEnter() on the thread
 * that owns the C3D frame, read from anywhere -- never calls ReplayEnter() itself: see its banner
 * comment on why that matters). Sticky across frame boundaries (see sReplayOkThisFrame): a reader
 * racing ahead of the render thread in the tick-ahead pipe sees the latest decided outcome, which
 * may be the previous render frame's, not necessarily this exact one. A first attempt cleared this
 * to false at ReplayFrameBegin ("conservative until this frame's Enter runs") and that was wrong:
 * under the pipe the game-side reader runs ahead EVERY frame, so every category refused NRR every
 * frame and the stock interpreter's per-eye double-submit overflowed the GPU command buffer
 * (svcBreak in GPUCMD_Add, gated and caught; see docs/research/native/niview-progress.md). */
bool ReplayOkThisFrame() {
    return sReplayOkThisFrame;
}

void ReplayNoteBind(int fbId) {
    if (!sInBody) {
        return;
    }
    if (fbId != 0) {
        sOtherBound = true;
        return;
    }
    if (!sOtherBound) {
        C3Di_GetContext()->flags &= ~static_cast<u32>(C3DiF_FrameBuf); /* the eye's target is bound */
        return;
    }
    sOtherBound = false;
    if (sNumSwaps >= kMaxSwaps) {
        sWin.overflow++;
        return; /* the right eye's rest of the frame lands on the left target: counted, never seen so far */
    }
    SubmitOpenList();
    Swap& sw = sSwaps[sNumSwaps];
    sw.entry = Queue()->numEntries;
    EmitFrameBuf(sMain);
    C3Di_GetContext()->flags &= ~static_cast<u32>(C3DiF_FrameBuf);
    SubmitOpenList();
    EmitFrameBuf(RightTarget());
    DetachOpenList(&sw.buf, &sw.words);
    sNumSwaps++;
}

/* n60tail [native] stereo_fold: called by the [overlap] kick right before its split. When the kick
 * chunk will replay (ReplayKickChunk's tests, one entry ahead), the right prologue goes at the end
 * of the list the split closes instead of into a list of its own (-1 GX entry per kick). */
void ReplayKickPreSplit() {
    sKickFolded = false;
    sKickOneSub = false;
    if (sInBody && sInterleave > 0 && gdx3ds_nrr_handoff_open == 0 && OsChunkEligible(2u * kReserve)) {
        const uint64_t t0 = svcGetSystemTick();
        sKickOneSub = OsClose(); /* onesubmit: the kick's split finds the open list empty */
        const uint64_t spent = svcGetSystemTick() - t0;
        sWin.chunkTicks += spent;
        sFrameTicks += spent;
        if (sKickOneSub) {
            return;
        }
    }
    if (!sInBody || sInterleave <= 0 || !sFoldNow || gdx3ds_nrr_handoff_open != 0 || gpuCmdBufOffset == 0) {
        return;
    }
    const u32 closed = static_cast<u32>(Queue()->numEntries) + 1u;
    if (closed + 2u * kReserve + 8u > static_cast<u32>(Queue()->maxEntries) || !FoldRoom(2u * kReserve)) {
        return; /* the unfolded path decides (and counts the skip) */
    }
    const uint64_t t0 = svcGetSystemTick();
    EmitFoldedPrologue(static_cast<int>(closed) - 1);
    sKickFolded = true;
    const uint64_t spent = svcGetSystemTick() - t0;
    sWin.chunkTicks += spent;
    sFrameTicks += spent;
}

/* [overlap] kick: the chunk just submitted for the left eye goes to the right eye at once, so the GPU
 * draws both eyes of it while the CPU builds the next categories; the frame end then replays only
 * the last chunk. Never inside an NRR hand-off (its direct words believe GPU state that the next
 * chunk's fresh start would re-set from citro3d's context). */
void ReplayKickChunk() {
    if (!sInBody || sInterleave <= 0) {
        return;
    }
    const uint64_t t0 = svcGetSystemTick();
    if (sKickOneSub) {
        /* onesubmit: both eyes of the chunk are one entry already; only the next chunk's start */
        sKickOneSub = false;
        ResumeLeftEye();
        sWin.chunks++;
    } else if (sKickFolded) {
        /* The closing list already switched the GPU to the right eye: replay it (the room was
         * checked before folding) and always come back to the left eye. */
        sKickFolded = false;
        SubmitOpenList();
        if (ReplayPendingChunk(2u * kReserve, true)) {
            if (sOsNow) {
                sWin.osFallback++;
            }
            sWin.chunks++;
        } else {
            sWin.chunkSkip++;
        }
        ResumeLeftEye();
    } else if (gdx3ds_nrr_handoff_open != 0 || !QueueRoom(2u * kReserve + 8u)) {
        sWin.chunkSkip++;
        return;
    } else {
        SubmitOpenList();
        if (ReplayPendingChunk(2u * kReserve, false)) {
            if (sOsNow) {
                sWin.osFallback++;
            }
            ResumeLeftEye();
            sWin.chunks++;
        } else {
            sWin.chunkSkip++;
        }
    }
    const uint64_t spent = svcGetSystemTick() - t0;
    sWin.chunkTicks += spent;
    sFrameTicks += spent;
    sFrameChunks++;
}

int ReplayStkLoc() {
    return sStkLoc;
}

/* n60tail [native] sky_late: the next list starts self-contained (every citro3d group and uniform
 * re-emitted before its first draw, lazily), the framebuffer left as it is. */
void MarkStateSelfContained() {
    C3D_Context* ctx = C3Di_GetContext();
    ctx->flags |= kDirtyBody;
    C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
    if (sStkLoc >= 0) {
        const C3D_FVec& cur = C3D_FVUnif[GPU_VERTEX_SHADER][sStkLoc];
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sStkLoc, cur.x, cur.y, cur.z, cur.w);
    }
}

void ReplayFrameEnd() {
    if (!sInBody) {
        return;
    }
    const uint64_t t0 = svcGetSystemTick();
    sInBody = false;
    gdx3ds_stereo_replay_frame = 0;
    if (OsChunkEligible(kReserve) && OsClose()) {
        /* onesubmit: the last chunk's both eyes are one entry; steye = 0 goes into the next frame's
         * first list (the stereo_fold rule: nothing between runs the vertex shader). */
        sEyeZeroPending = true;
        sWin.eyeDefer++;
        sWin.gxEntries += static_cast<unsigned>(Queue()->numEntries);
        C3Di_GetContext()->flags |= C3DiF_FrameBuf;
        sWin.replayed++;
        const uint64_t spent = svcGetSystemTick() - t0;
        sWin.endTicks += spent;
        sFrameTicks += spent;
        return;
    }
    if (sOsNow) {
        sWin.osFallback++;
    }
    /* n60tail stereo_fold: the right prologue closes the body's last list (room checked first). */
    const bool folded = sFoldNow && gpuCmdBufOffset != 0 && FoldRoom(kReserve);
    if (folded) {
        EmitFoldedPrologue(static_cast<int>(Queue()->numEntries));
    }
    SubmitOpenList(); /* the body's last list */
    QueueLeftTransferEarly(); /* n60tail stereo_xl_early: the left eye is complete in VRAM now */
    if (!ReplayPendingChunk(kReserve, folded)) {
        sWin.overflow++;
        if (folded) { /* not reached (room checked before folding): leave no right-eye state behind */
            C3Di_GetContext()->flags |= C3DiF_FrameBuf;
            sEyeZeroPending = true;
        }
        return;
    }
    if (sFoldNow) {
        /* n60tail stereo_fold: steye = 0 goes into the next frame's first list (ReplayFrameBegin):
         * nothing between here and there runs the vertex shader (display transfers, the oracle's
         * readback copies), and a frame that never comes back here emits it the same way. */
        sEyeZeroPending = true;
        sWin.eyeDefer++;
    } else {
        EmitEye(0.0f);
        SubmitOpenList();
    }
    sWin.gxEntries += static_cast<unsigned>(Queue()->numEntries);
    /* The GPU now has the right target bound: citro3d re-binds its own at the next draw. */
    C3Di_GetContext()->flags |= C3DiF_FrameBuf;
    sWin.replayed++;
    const uint64_t spent = svcGetSystemTick() - t0;
    sWin.endTicks += spent;
    sFrameTicks += spent;
}

uint64_t FrameCpuTicks(unsigned* chunks) {
    *chunks = sFrameChunks;
    return sFrameTicks;
}

} // namespace Gdx3dsStereo

extern "C" void gdx3ds_stereo_kick_chunk(void) {
    Gdx3dsStereo::ReplayKickChunk();
}

extern "C" void gdx3ds_stereo_kick_pre_split(void) {
    Gdx3dsStereo::ReplayKickPreSplit();
}

/* n60tail: the stereo_fold / stereo_xl_early phase of the frame being built (bit 0 fold, bit 1
 * xl_early), for the [gputl] receipt's tp= (each window holds one phase). */
extern "C" int gdx3ds_stereo_tail_phase(void) {
    /* stereo-race-fps: 8 = head_kick, 16 = resume_fold (4 is sky_late's, gdx3ds_skylate_phase). */
    /* onesubmit: 32 = one-submit chunks, 64 = onesubmit_kicks, 128 = onesubmit_narrow (this frame's arm) */
    const int arm = sOsNow ? sOsArm : 0;
    return (sFoldNow ? 1 : 0) | (sXlNow ? 2 : 0) | (sHeadNow ? 8 : 0) | (sResumeNow ? 16 : 0) | (sOsNow ? 32 : 0) |
           ((arm == 2 || arm == 4) ? 64 : 0) | ((arm == 3 || arm == 4) ? 128 : 0);
}

/* The pace module's 3D rule ([debug] pace_swapgate_3d) asks this at each fresh begin: the right-eye
 * pass state latched at the latest StartFrame. (Round 3 declared it weak and nothing defined it, so
 * pace_swapgate_3d always fell back to pace_swapgate.) */
extern "C" int gdx3ds_stereo_active(void) {
    return Gdx3dsStereo::Active() ? 1 : 0;
}

/* [native] ni_race_mesh (nirace-progress.md 4R7): same reason as gdx3ds_stereo_active above --
 * gdx_nirace.cpp also builds as a host test TU (gdx_interp_host_tests links neither this file
 * nor gdx3ds_stereo.h's <3ds.h>/<citro3d.h> dependency), so it reads this weakly. */
extern "C" int gdx3ds_stereo_replay_ok_this_frame(void) {
    return Gdx3dsStereo::ReplayOkThisFrame() ? 1 : 0;
}

extern "C" unsigned long long gdx3ds_stereo_frame_cpu_ticks(unsigned* chunks) {
    return Gdx3dsStereo::FrameCpuTicks(chunks);
}

/* niview: "may an NRR category commit to drawing this section natively" -- the latest decided
 * replay outcome, not just the static levers. Active() already folds in [stereo] enabled and the
 * slider. IMPORTANT: this must NOT call ReplayEnter() itself -- ReplayEnter() is stateful and emits
 * GPU words (EmitEye/EmitFrameBuf/SubmitOpenList/C3Dgdx_FlushState) and the file banner is explicit
 * that "everything runs on the thread that owns the C3D frame"; gdx3ds_stereo_nrr_ok() is called from
 * category stubs on the GAME side (gdx3ds_nrr_frame_build), a different thread under the tick-ahead
 * pipe. Calling ReplayEnter() from there was tried and crashed (GPUCMD_Add, svcBreak) -- a real,
 * gated finding, not theoretical. So this reads ReplayOkThisFrame(), sticky across frame boundaries
 * (a strict "clear it every ReplayFrameBegin, true only once this exact frame's Enter ran" was also
 * tried and ALSO gated-and-caught: the game thread runs ahead of the render thread every frame under
 * the pipe, so that version forced every category to refuse NRR every single frame, every category,
 * and the stock interpreter's per-eye double-submit overflowed the GPU command buffer the same way).
 * Closes the latent bug where a category CONSUMES a section (skips Background_Draw/the stock path)
 * on the strength of the static lever alone, then the render side discovers -- too late -- that this
 * particular frame's replay didn't happen, leaving that section undrawn for one eye (the sky
 * black-screen class named in the mission brief) -- at the cost of the flag being a frame or so
 * stale under the pipe, same staleness as this codebase's other cross-thread signals. */
extern "C" int gdx3ds_stereo_nrr_ok(void) {
    if (!Gdx3dsStereo::Active()) {
        return 1;
    }
    return Gdx3dsStereo::ReplayOkThisFrame() ? 1 : 0;
}

/* onesubmit: the arm in force for a 3D (replay) frame, -1 when no right eye runs ([gxcost] arm=). */
extern "C" int gdx3ds_stereo_onesub_arm(void) {
    if (sLever <= 0 || !Gdx3dsStereo::Active() || (sOneSub == 0 && sOneSubAb == 0)) {
        return -1;
    }
    return OneSubArm();
}

/* onesubmit: the [overlap] kick policy of a 3D frame -- 0 the kicks as configured, 1 no mid-frame
 * body kick (the head run stays), 2 no kick nor head run ([stereo] onesubmit_kicks; the ab arm 2 uses
 * its value, 1 when unset). Read at the overlap frame begin, before this frame's StartFrame latched
 * the slider: the latest frame's right-eye state, as [debug] kick_3d reads it. */
extern "C" int gdx3ds_stereo_onesub_kicks(void) {
    const int arm = gdx3ds_stereo_onesub_arm();
    if (arm != 2 && arm != 4) {
        return 0;
    }
    return sOneSubKicks != 0 ? sOneSubKicks : 1;
}

/* onesubmit: 1 = a 3D frame's kicks and frame end flush only the noted dirty ranges
 * ([stereo] onesubmit_narrow, ab arm 3). */
extern "C" int gdx3ds_stereo_onesub_narrow(void) {
    const int arm = gdx3ds_stereo_onesub_arm();
    return (arm == 3 || arm == 4) ? 1 : 0;
}
