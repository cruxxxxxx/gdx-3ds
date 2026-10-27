/* port/3ds/gfx/n64/gdx_dl.cpp -- the display-list walker: 32-deep call stack, per-list
 * reader fixed at list entry, flat opcode switch, legacy-F3D dialect remap, G_LOAD_UCODE
 * variant switch. Plan §3.2/§3.3. Control-flow semantics mirror LUS's flat dispatch
 * (interpreter.cpp gfx_dl_handler_common / gfx_branch_z_handler_f3dex2 /
 * gfx_load_ucode_flat_handler) and the operand classification mirrors the bridge's
 * ProcessList cases, so a list walks here exactly as its converted copy walked there. */
#include "gdx_interp.h"

#include <math.h>
#include <string.h>

#include "gdx_log.h"
#include "gdx3ds_hotcold.h" // hotcold: GDX_COLD / GDX_UNLIKELY (per-function attributes only)

/* [native] dspcensus clock. Defined in gdx_rt_glue.cpp (3DS) / the host harness. `child`
 * mirrors gdx3ds_prof_child_ticks so a per-opcode timer can subtract the [prof] sections
 * (vtx/tri/imp/drw/mtx) that ran INSIDE the handler -- without itself becoming a child, which
 * would empty the enclosing RUN bucket the census is explaining. */
extern "C" long long gdx_ninterp_now(void);
extern "C" unsigned long long gdx_ninterp_child_ticks(void);

namespace gdxn {

static inline float Clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* [dspx] one handler group's SELF time: wall minus the [prof] child sections entered inside it
 * (read through gdx_ninterp_child_ticks, never published into it -- this runs inside the RUN
 * bucket it is explaining), minus any nested DxScope the caller Exclude()s. Two clock reads per
 * call and nothing at all when `on` is false. */
class DxScope {
  public:
    DxScope(Counters& s, bool on, DxGroup g) : mS(s), mG(g), mOn(on) {
        if (GDX_UNLIKELY(mOn)) {
            Start();
        }
    }
    ~DxScope() {
        Stop();
    }
    /* Closes the scope once; returns the self ticks it recorded (0 when off). */
    uint64_t Stop() {
        if (GDX_LIKELY(!mOn)) {
            return 0;
        }
        return StopOn();
    }
    void Exclude(uint64_t ticks) {
        mExcluded += ticks;
    }

  private:
    /* hotcold: the [dspx] clock reads, out of every Exec case and cold ([debug] dspx only). */
    GDX_COLD void Start() {
        mT0 = static_cast<uint64_t>(gdx_ninterp_now());
        mC0 = gdx_ninterp_child_ticks();
    }
    GDX_COLD uint64_t StopOn() {
        mOn = false;
        const uint64_t gross = static_cast<uint64_t>(gdx_ninterp_now()) - mT0;
        const uint64_t child = gdx_ninterp_child_ticks() - mC0;
        uint64_t self = (child < gross) ? (gross - child) : 0;
        self = (mExcluded < self) ? (self - mExcluded) : 0;
        mS.dxTicks[mG] += self;
        mS.dxCalls[mG]++;
        return self;
    }

    Counters& mS;
    DxGroup mG;
    bool mOn;
    uint64_t mT0 = 0, mC0 = 0, mExcluded = 0;
};

bool Interp::PushList(const ListRef& list) {
    if (mSp >= kCallStack) {
        mStats.stackOverflow++;
        return false;
    }
    Frame& f = mStack[mSp++];
    f.list = list;
    f.idx = 0;
    mStats.lists++;
    mStats.listsByClass[static_cast<int>(ClassOf(list))]++;
    if (list.isF3d) {
        mStats.listsF3d++;
    }
    return true;
}

void Interp::Walk(const ListRef& root) {
    mSp = 0;
    if (!PushList(root)) {
        return;
    }
    if (GDX_UNLIKELY(mNiOn)) {
        NrrRaceTaskBegin();           /* nirace [native] ni_race (gdx_nirace.cpp): the task's environment */
        NrrRaceHotDrive(mStack[0], 0); /* the root's spans and markers (gdx_nirace.cpp) */
    }
    if (GDX_UNLIKELY(mNc != nullptr)) {
        WalkNicensus(); /* [debug] nicensus (gdx_nicensus.cpp): measurement only */
        return;
    }
    if (mDspCensus) {
        WalkCensus();
        return;
    }
    if (mMsOn) {
        WalkMachSplit(); /* [debug] machsplit (gdx_machsplit.cpp) */
        return;
    }
    while (mSp > 0) {
        Frame& f = mStack[mSp - 1];
        if (f.idx >= f.list.limit) {
            /* No terminator inside the readable bound: the bridge appends an ENDDL here. */
            mStats.runaway++;
            mSp--;
            continue;
        }
        Cmd c = ReadCmd(f.list, f.idx);
        f.idx++;
        mStats.op[Opcode(c.w0)]++;
        mStats.cmds++;
        size_t f3dXfer = 0;
        if (f.list.isF3d && !RemapF3d(c, f.list, &f3dXfer)) {
            mStats.droppedOps++;
            continue;
        }
        if (GDX_UNLIKELY(mVerifyActive)) { /* [nverify] diagnostic; free with interp_verify=0 */
            mBackend->VerifyNoteCmd(c.w0, c.w1);
        }
        const int r = Exec(c, f, f3dXfer);
        if (r == 1) {
            mSp--;
        }
    }
}

/* n60hudx [native] nrr_hud_kept: a G_DL recorded inside a HUD run (gdx3ds_hud_capture.c) is resolved
 * exactly as Exec's G_DL resolves it -- the dlmemo or the bridge, then the G_DL hook -- without the
 * deadsetup look-ahead (whose window is the walker's parent list, not a run). *hookTaken: the hook
 * drew the list (Exec returns without entering it). false: the list does not resolve (Exec counts
 * it as a no-op list and goes on). */
GDX_COLD bool Interp::ResolveDlAsExec(const Cmd& c, ListRef& sub, bool* hookTaken) {
    *hookTaken = false;
    const bool resolved = (mDlMemoLevel != 0) ? ResolveListMemo(c, sub, false) : mSrc.ResolveList(c, sub);
    if (!resolved) {
        mStats.noopLists++;
        return false;
    }
    if (mDlHook != nullptr) {
        mDlHookSub = sub;
        *hookTaken = mDlHook(mDlHookUser, sub.base, this) != 0;
    }
    return true;
}

/* The list walked to its end on top of the walker's stack, as Walk would after Exec pushed it (the
 * same loop: reads, the F3D remap, Exec), returning when the stack is back where it was. */
GDX_COLD void Interp::WalkNestedList(const ListRef& sub) {
    const int depth = mSp;
    if (!PushList(sub)) {
        return;
    }
    while (mSp > depth) {
        Frame& f = mStack[mSp - 1];
        if (f.idx >= f.list.limit) {
            mStats.runaway++;
            mSp--;
            continue;
        }
        Cmd c = ReadCmd(f.list, f.idx);
        f.idx++;
        mStats.op[Opcode(c.w0)]++;
        mStats.cmds++;
        size_t f3dXfer = 0;
        if (f.list.isF3d && !RemapF3d(c, f.list, &f3dXfer)) {
            mStats.droppedOps++;
            continue;
        }
        if (GDX_UNLIKELY(mVerifyActive)) {
            mBackend->VerifyNoteCmd(c.w0, c.w1);
        }
        if (GDX_UNLIKELY(mNc != nullptr)) {
            NcNote(c, f.list.isF3d, NCS_NESTED); /* [debug] nicensus */
        }
        const int r = Exec(c, f, f3dXfer);
        if (r == 1) {
            mSp--;
        }
    }
}

/* A recorded G_DL on the replay's stock path: Exec's G_DL, the list walked in place. */
GDX_COLD void Interp::HudNestedDl(const Cmd& c) {
    ListRef sub;
    bool taken = false;
    if (ResolveDlAsExec(c, sub, &taken) && !taken) {
        WalkNestedList(sub);
    }
}

/* [native] dspcensus: the same loop with three tick reads per command. Kept as a SEPARATE
 * body so the shipping loop above carries no census branch at all -- the whole point of the
 * measurement is the per-command cost, and a predicted-not-taken branch per command is
 * exactly the size of the thing being measured. */
void Interp::WalkCensus() {
    const uint64_t walkT0 = static_cast<uint64_t>(gdx_ninterp_now());
    while (mSp > 0) {
        Frame& f = mStack[mSp - 1];
        if (f.idx >= f.list.limit) {
            mStats.runaway++;
            mSp--;
            continue;
        }
        const uint64_t tFetch = static_cast<uint64_t>(gdx_ninterp_now());
        Cmd c = ReadCmd(f.list, f.idx);
        f.idx++;
        const uint8_t rawOp = Opcode(c.w0);
        mStats.op[rawOp]++;
        mStats.cmds++;
        size_t f3dXfer = 0;
        const bool dropped = f.list.isF3d && !RemapF3d(c, f.list, &f3dXfer);
        const uint64_t tExec = static_cast<uint64_t>(gdx_ninterp_now());
        mStats.phTicks[DSP_PH_FETCH] += tExec - tFetch;
        mStats.phCalls[DSP_PH_FETCH]++;
        if (dropped) {
            mStats.droppedOps++;
            continue;
        }
        if (mVerifyActive) {
            mBackend->VerifyNoteCmd(c.w0, c.w1);
        }
        const uint64_t childT0 = gdx_ninterp_child_ticks();
        const int r = Exec(c, f, f3dXfer);
        const uint64_t tEnd = static_cast<uint64_t>(gdx_ninterp_now());
        /* Self time: the handler minus the [prof] sections it entered. The subtraction can
         * only underflow if a section straddled the bracket, which cannot happen on one
         * thread with balanced ProfScopes; clamp anyway so one bad frame cannot poison a
         * uint64 accumulator for the rest of the window. */
        const uint64_t gross = tEnd - tExec;
        const uint64_t child = gdx_ninterp_child_ticks() - childT0;
        mStats.opTicks[rawOp] += (child < gross) ? (gross - child) : 0; /* rawOp: same index as mStats.op[] */
        if (r == 1) {
            mSp--;
        }
    }
    mStats.walkTicks += static_cast<uint64_t>(gdx_ninterp_now()) - walkT0;
}

/* Legacy F3D -> F3DEX2, one command. Mirrors n64_gfx_bridge.cpp ProcessList's isF3DSource
 * arms (cases 0x01/0x03/0x04/0x06/0xB6..0xBF). Everything else passes through unchanged,
 * exactly as the bridge's default arm emits it. */
bool Interp::RemapF3d(Cmd& c, const ListRef& list, size_t* f3dXferSize) {
    (void)list;
    const uint8_t op = Opcode(c.w0);
    switch (op) {
        case F3D_MTX: {
            const uint8_t legacy = static_cast<uint8_t>(C0(c.w0, 16, 8));
            uint8_t params = 0;
            if (legacy & 0x01u) params |= MTX_PROJECTION;
            if (legacy & 0x02u) params |= MTX_LOAD;
            if (legacy & 0x04u) params |= MTX_PUSH;
            c.w0 = (static_cast<uint32_t>(OP_MTX) << 24) | static_cast<uint32_t>(params ^ 0x01u);
            return true;
        }
        case F3D_MOVEMEM: {
            const uint8_t legacyIndex = static_cast<uint8_t>(C0(c.w0, 16, 8));
            const size_t xfer = static_cast<size_t>(c.w0 & 0xFFFFu);
            uint8_t index = 0, offset = 0;
            if (legacyIndex == 0x80u) {
                index = MV_VIEWPORT;
            } else if (legacyIndex == 0x84u) {
                index = MV_LIGHT;
                offset = MVO_LOOKATX;
            } else if (legacyIndex == 0x82u) {
                index = MV_LIGHT;
                offset = MVO_LOOKATY;
            } else if (legacyIndex >= 0x86u && legacyIndex <= 0x94u && ((legacyIndex - 0x86u) & 1u) == 0) {
                index = MV_LIGHT;
                offset = static_cast<uint8_t>(MVO_L0 + ((legacyIndex - 0x86u) / 2u) * 24u);
            } else {
                return false;
            }
            c.w0 = (static_cast<uint32_t>(OP_MOVEMEM) << 24) | (static_cast<uint32_t>(offset / 8u) << 8) | index;
            *f3dXferSize = xfer;
            return true;
        }
        case F3D_VTX: {
            const uint16_t len = static_cast<uint16_t>(c.w0 & 0xFFFFu);
            const uint8_t n = (len >= 16u) ? static_cast<uint8_t>(len / 16u) : 1u;
            const uint8_t v0 = static_cast<uint8_t>(C0(c.w0, 16, 8) & 0x0Fu);
            c.w0 = (static_cast<uint32_t>(OP_VTX) << 24) | (static_cast<uint32_t>(n) << 12) |
                   static_cast<uint32_t>((v0 + n) * 2u);
            return true;
        }
        case F3D_DL:
            c.w0 = (c.w0 & 0x00FFFFFFu) | (static_cast<uint32_t>(OP_DL) << 24);
            return true;
        case F3D_CLEARGEOMETRYMODE:
            c.w0 = (static_cast<uint32_t>(OP_GEOMETRYMODE) << 24) | (~c.w1 & 0x00FFFFFFu);
            c.w1 = 0;
            return true;
        case F3D_SETGEOMETRYMODE:
            c.w0 = (static_cast<uint32_t>(OP_GEOMETRYMODE) << 24) | 0x00FFFFFFu;
            return true;
        case F3D_ENDDL:
            c.w0 = static_cast<uint32_t>(OP_ENDDL) << 24;
            c.w1 = 0;
            return true;
        case F3D_SETOTHERMODE_L:
        case F3D_SETOTHERMODE_H: {
            const uint32_t shift = C0(c.w0, 8, 8);
            const uint32_t length = C0(c.w0, 0, 8);
            if (length == 0u || shift + length > 32u) {
                return false;
            }
            const uint8_t ex2 = (op == F3D_SETOTHERMODE_L) ? OP_SETOTHERMODE_L : OP_SETOTHERMODE_H;
            c.w0 = (static_cast<uint32_t>(ex2) << 24) | ((32u - shift - length) << 8) | (length - 1u);
            return true;
        }
        case F3D_TEXTURE:
            c.w0 = (c.w0 & 0x00FFFFFFu) | (static_cast<uint32_t>(OP_TEXTURE) << 24);
            return true;
        case F3D_MOVEWORD:
            c.w0 = (c.w0 & 0x00FFFFFFu) | (static_cast<uint32_t>(OP_MOVEWORD) << 24);
            return true;
        case F3D_POPMTX:
            c.w0 = static_cast<uint32_t>(OP_POPMTX) << 24;
            c.w1 = 64u;
            return true;
        case F3D_CULLDL:
            return false;
        case F3D_TRI1: {
            const uint8_t v0 = static_cast<uint8_t>(C1(c.w1, 16, 8)) / 10u;
            const uint8_t v1 = static_cast<uint8_t>(C1(c.w1, 8, 8)) / 10u;
            const uint8_t v2 = static_cast<uint8_t>(C1(c.w1, 0, 8)) / 10u;
            c.w0 = (static_cast<uint32_t>(OP_TRI1) << 24) | (static_cast<uint32_t>(v0 * 2u) << 16) |
                   (static_cast<uint32_t>(v1 * 2u) << 8) | static_cast<uint32_t>(v2 * 2u);
            c.w1 = 0;
            return true;
        }
        default:
            return true;
    }
}

static inline uint32_t ColorComb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xFu) | ((b & 0xFu) << 4) | ((c & 0x1Fu) << 8) | ((d & 7u) << 13);
}
static inline uint32_t AlphaComb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7u) | ((b & 7u) << 3) | ((c & 7u) << 6) | ((d & 7u) << 9);
}

static inline Rgba RgbaOf(uint32_t w1) {
    return Rgba{ static_cast<uint8_t>(w1 >> 24), static_cast<uint8_t>(w1 >> 16), static_cast<uint8_t>(w1 >> 8),
                 static_cast<uint8_t>(w1) };
}

/* == gfx_set_combine_handler_rdp: the LUS color_comb/alpha_comb packing. Shared by the
 * walker and the replay seam (template-embedded SETCOMBINE, machine material runs). */
void Interp::SetCombineWords(uint32_t w0, uint32_t w1) {
    DpSetCombineMode(ColorComb(C0(w0, 20, 4), C1(w1, 28, 4), C0(w0, 15, 5), C1(w1, 15, 3)),
                     AlphaComb(C0(w0, 12, 3), C1(w1, 12, 3), C0(w0, 9, 3), C1(w1, 9, 3)),
                     ColorComb(C0(w0, 5, 4), C1(w1, 24, 4), C0(w0, 0, 5), C1(w1, 6, 3)),
                     AlphaComb(C1(w1, 21, 3), C1(w1, 3, 3), C1(w1, 18, 3), C1(w1, 0, 3)));
}

/* hotcold: one [native] dspcensus>=2 G_DL phase lap (clock read + accumulate), cold. */
static GDX_COLD uint64_t DlPhaseLap(Counters& s, int phase, uint64_t t0) {
    const uint64_t t1 = static_cast<uint64_t>(gdx_ninterp_now());
    s.phTicks[phase] += t1 - t0;
    s.phCalls[phase]++;
    return t1;
}

/* nimenu W4 (review §4 hot trims): shared with NrrRaceHotDrive's marker loop (gdx_nirace.cpp) --
 * see the declaration (gdx_interp.h) for why the two independent `if`s below stay independent
 * (HudHotClose runs whenever mNrrOpen regardless of whether a hook is registered). */
void Interp::NrrNoopDispatch(uint32_t w0, uint32_t w1) {
    if (GDX_UNLIKELY(mNrrOpen) && ((w0 >> 16) & 0xFFu) != 0x5Eu) {
        NrrHudHotClose(); /* [native] nrr_hud: a marker hook may draw (track, fx, sky, kick) */
    }
    if (mNoopHook != nullptr) {
        mNoopHook(mNoopHookUser, w0, w1, this);
    }
}

int Interp::Exec(const Cmd& c, Frame& f, size_t f3dXferSize) {
    const uint8_t op = Opcode(c.w0);
    switch (op) {
        /* ---------------- flow ---------------- */
        case OP_NOOP:
        case OP_SPNOOP:
            /* == gfx_noop_handler_f3dex2 (the F3DEX2 table binds both opcodes to it): the
             * port's marker replay gets the two words as-is; p=7/8 (OPEN/CLOSE_DISPS) is
             * bookkeeping the hook ignores, as the LUS handler's exec-stack does on this port.
             * NrrNoopDispatch does the HudHotClose-then-hook pair (shared, nimenu W4); when a hook
             * is registered its call is timed under the same DxScope as before -- HudHotClose's own
             * (cheap) work now falls inside that same timed window too, which it did not before
             * sharing, a minor and deliberate widening of what [dspx]'s TRK/FX buckets cover. */
            if (mNoopHook != nullptr) {
                const uint32_t marker = (c.w0 >> 16) & 0xFFu; /* receipt split only */
                DxScope dx(mStats, mDxOn, (marker == 0x5Au || marker == 0x5Bu) ? DX_TRK : DX_FX);
                NrrNoopDispatch(c.w0, c.w1);
            } else if (GDX_UNLIKELY(mNrrOpen) && ((c.w0 >> 16) & 0xFFu) != 0x5Eu) {
                NrrHudHotClose();
            }
            if (GDX_UNLIKELY(mNiOn) && mSp == 1 && &f == &mStack[0]) {
                NrrRaceHotDrive(f, c.w0); /* nirace [native] ni_race: the spans and markers after a root marker */
            }
            return 0;
        case OP_CULLDL: /* no-op today, as in the LUS path */
        case OP_RDPLOADSYNC:
        case OP_RDPPIPESYNC: /* pipesync is a no-op on 3DS (lus-3ds-pipesync-noop) */
        case OP_RDPTILESYNC:
        case OP_RDPFULLSYNC:
            return 0;

        case OP_ENDDL:
            return 1;

        case OP_DL: {
            DxScope dx(mStats, mDxOn, DX_DL);
            if (mDeadSetLevel != 0) { /* [native] deadsetup (gdx_dlmemo.cpp) */
                const int ds = DeadSetOnDl(c, f);
                if (ds >= 0) {
                    return ds;
                }
            }
            ListRef sub;
            /* [native] dspcensus>=2: split the G_DL handler into resolve / hook / push. The
             * reads are inside the opcode's own bracket, so they inflate 0xDE's self time by
             * ~6 clock reads; compare shares, not the absolute, at level 2. */
            const bool ph = mDspCensus >= 2;
            uint64_t t0 = ph ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
            /* [native] dlmemo (gdx_dlmemo.cpp): the cross-task resolve memo */
            const bool resolved = (mDlMemoLevel != 0) ? ResolveListMemo(c, sub, false) : mSrc.ResolveList(c, sub);
            if (GDX_UNLIKELY(ph)) {
                t0 = DlPhaseLap(mStats, DSP_PH_DLRESOLVE, t0);
            }
            if (!resolved) {
                mStats.noopLists++;
                return 0;
            }
            if (mDlHook != nullptr) {
                DxScope hx(mStats, mDxOn, DX_MCH);
                mDlHookSub = sub; /* [native] machines_speck reads the list's shape (DlHookList) */
                const int taken = mDlHook(mDlHookUser, sub.base, this);
                dx.Exclude(hx.Stop()); /* the hook is DX_MCH, not G_DL dispatch */
                if (GDX_UNLIKELY(ph)) {
                    t0 = DlPhaseLap(mStats, DSP_PH_DLHOOK, t0);
                }
                if (taken != 0) {
                    return 0;
                }
            }
            if (C0(c.w0, 16, 1) == 0) {
                PushList(sub);
            } else {
                f.list = sub;
                f.idx = 0;
                mStats.lists++;
                mStats.listsByClass[static_cast<int>(ClassOf(sub))]++;
                if (sub.isF3d) mStats.listsF3d++;
            }
            if (GDX_UNLIKELY(ph)) {
                DlPhaseLap(mStats, DSP_PH_DLPUSH, t0);
            }
            return 2;
        }

        case OP_RDPHALF_1: {
            /* Latched for a following G_BRANCH_Z (the bridge resolves the half as a list only
             * when the next command is BRANCH_Z); TEXRECT consumes its own halves inline. */
            mBranchZValid = false;
            if (f.idx < f.list.limit && Opcode(ReadCmd(f.list, f.idx).w0) == OP_BRANCH_Z) {
                mBranchZValid = mSrc.ResolveList(c, mBranchZTarget);
            }
            return 0;
        }

        case OP_BRANCH_Z: {
            const bool valid = mBranchZValid;
            mBranchZValid = false;
            const uint8_t vbidx = static_cast<uint8_t>((c.w0 & 0xFFFu) >> 1);
            const uint32_t zval = c.w1;
            if (!valid || vbidx >= kMaxVertices) {
                return 0;
            }
            const LoadedVertex& v = mRsp.loaded_vertices[vbidx];
            const float screenZ = (fabsf(v.w) > 0.000001f)
                                      ? ((v.z / v.w) * mRsp.viewport_z_scale + mRsp.viewport_z_trans)
                                      : mRsp.viewport_z_trans;
            const uint32_t screenZFixed = static_cast<uint32_t>(Clampf(screenZ, 0.0f, 65535.0f) * 65536.0f);
            if (screenZFixed <= zval || (mRsp.extra_geometry_mode & 0x00000002u) != 0) {
                mStats.branchZTaken++;
                f.list = mBranchZTarget;
                f.idx = 0;
                return 2;
            }
            return 0;
        }

        case OP_LOAD_UCODE: {
            int variant = 0, isLine = 0;
            if (DlSrc::UcodeVariant(c, &variant, &isLine) == 0) {
                mStats.droppedOps++;
                return 0;
            }
            mVariant = variant;
            mLineSection = isLine != 0;
            return 0;
        }

        /* ---------------- RSP ---------------- */
        case OP_MTX:
            SpMatrix(static_cast<uint8_t>(C0(c.w0, 0, 8) ^ MTX_PUSH), c, f.list);
            return 0;
        case OP_POPMTX:
            SpPopMatrix(c.w1 / 64u);
            return 0;
        case OP_VTX:
#ifdef EXPANSION_KIT
            /* [native] ek_editmesh: the editor's VTX/PIPESYNC/PRIM/LINE3D line, from its memo */
            if (GDX_UNLIKELY(mEkPvOn != 0) && EkPvVtx(c, f)) {
                return 0;
            }
#endif
            SpVertex(C0(c.w0, 12, 8), C0(c.w0, 1, 7) - C0(c.w0, 12, 8), c, f.list);
            return 0;
        case OP_MODIFYVTX:
            SpModifyVertex(static_cast<uint16_t>(C0(c.w0, 1, 15)), static_cast<uint8_t>(C0(c.w0, 16, 8)), c.w1);
            return 0;
        case OP_TRI1:
            SpTri(static_cast<uint8_t>(C0(c.w0, 16, 8) / 2), static_cast<uint8_t>(C0(c.w0, 8, 8) / 2),
                  static_cast<uint8_t>(C0(c.w0, 0, 8) / 2), false);
            return 0;
        case OP_TRI2:
        case OP_QUAD:
            SpTri(static_cast<uint8_t>(C0(c.w0, 16, 8) / 2), static_cast<uint8_t>(C0(c.w0, 8, 8) / 2),
                  static_cast<uint8_t>(C0(c.w0, 0, 8) / 2), false);
            SpTri(static_cast<uint8_t>(C1(c.w1, 16, 8) / 2), static_cast<uint8_t>(C1(c.w1, 8, 8) / 2),
                  static_cast<uint8_t>(C1(c.w1, 0, 8) / 2), false);
            return 0;
        case OP_LINE3D:
            if (!mLineSection) {
                mStats.droppedOps++;
                return 0;
            }
            SpLine3d(static_cast<uint8_t>(C0(c.w0, 16, 8) / 2), static_cast<uint8_t>(C0(c.w0, 8, 8) / 2),
                     static_cast<uint8_t>(C0(c.w0, 0, 8)));
            return 0;
        case OP_GEOMETRYMODE:
            SpGeometryMode(~C0(c.w0, 0, 24), c.w1);
            return 0;
        case OP_EXTRAGEOMETRYMODE:
            mRsp.extra_geometry_mode &= ~(~C0(c.w0, 0, 24));
            mRsp.extra_geometry_mode |= c.w1;
            return 0;
        case OP_MOVEMEM: {
            DxScope dx(mStats, mDxOn, DX_MV);
            SpMovemem(static_cast<uint8_t>(C0(c.w0, 0, 8)), static_cast<uint8_t>(C0(c.w0, 8, 8) * 8), c,
                      f3dXferSize, f.list);
            return 0;
        }
        case OP_MOVEWORD: {
            DxScope dx(mStats, mDxOn, DX_MV);
            SpMoveword(static_cast<uint8_t>(C0(c.w0, 16, 8)), static_cast<uint16_t>(C0(c.w0, 0, 16)), c);
            return 0;
        }
        case OP_TEXTURE:
            SpTexture(static_cast<uint16_t>(C1(c.w1, 16, 16)), static_cast<uint16_t>(C1(c.w1, 0, 16)),
                      static_cast<uint8_t>(C0(c.w0, 11, 3)), static_cast<uint8_t>(C0(c.w0, 8, 3)),
                      static_cast<uint8_t>(C0(c.w0, 1, 7)));
            return 0;
        case OP_SETOTHERMODE_L:
            SpSetOtherMode(31 - C0(c.w0, 8, 8) - C0(c.w0, 0, 8), C0(c.w0, 0, 8) + 1, c.w1);
            return 0;
        case OP_SETOTHERMODE_H:
            SpSetOtherMode(63 - C0(c.w0, 8, 8) - C0(c.w0, 0, 8), C0(c.w0, 0, 8) + 1,
                           static_cast<uint64_t>(c.w1) << 32);
            return 0;
        case OP_DMA_IO: {
            DxScope dx(mStats, mDxOn, DX_MV);
            SpDmaIo(c);
            return 0;
        }

        /* ---------------- RDP ---------------- */
        case OP_RDPSETOTHERMODE:
            DpSetOtherMode(C0(c.w0, 0, 24), c.w1);
            return 0;
        case OP_SETSCISSOR:
            DpSetScissor(C1(c.w1, 24, 2), C0(c.w0, 12, 12), C0(c.w0, 0, 12), C1(c.w1, 12, 12), C1(c.w1, 0, 12));
            return 0;
        case OP_SETTIMG:
            DpSetTextureImage(C0(c.w0, 21, 3), C0(c.w0, 19, 2), C0(c.w0, 0, 12) + 1, c);
            return 0;
        case OP_SETTILE:
            DpSetTile(static_cast<uint8_t>(C0(c.w0, 21, 3)), C0(c.w0, 19, 2), C0(c.w0, 9, 9), C0(c.w0, 0, 9),
                      static_cast<uint8_t>(C1(c.w1, 24, 3)), C1(c.w1, 20, 4), C1(c.w1, 18, 2), C1(c.w1, 14, 4),
                      C1(c.w1, 10, 4), C1(c.w1, 8, 2), C1(c.w1, 4, 4), C1(c.w1, 0, 4));
            return 0;
        case OP_SETTILESIZE:
            DpSetTileSize(static_cast<uint8_t>(C1(c.w1, 24, 3)), static_cast<uint16_t>(C0(c.w0, 12, 12)),
                          static_cast<uint16_t>(C0(c.w0, 0, 12)), static_cast<uint16_t>(C1(c.w1, 12, 12)),
                          static_cast<uint16_t>(C1(c.w1, 0, 12)));
            return 0;
        case OP_LOADTLUT: {
            DxScope dx(mStats, mDxOn, DX_LOAD);
            DpLoadTlut(static_cast<uint8_t>(C1(c.w1, 24, 3)), C1(c.w1, 14, 10));
            return 0;
        }
        case OP_LOADBLOCK: {
            DxScope dx(mStats, mDxOn, DX_LOAD);
            if (mDirectScope != 0 && !mDirectInLoad) {
                /* C5 Stage 3b direct-load scope (gdx_hud_direct.cpp DirectScopeLoadBlock) */
                DirectScopeLoadBlock(c, f, mDirectScope, mDirectLevels[mDirectScope]);
                return 0;
            }
            if (mLoadMemo != 0 && SetListLoadBlock(c, f)) {
                return 0; /* [native] machines_loadmemo: a machine texture-set list's load */
            }
            DpLoadBlock(static_cast<uint8_t>(C1(c.w1, 24, 3)), C0(c.w0, 12, 12), C0(c.w0, 0, 12), C1(c.w1, 12, 12),
                        C1(c.w1, 0, 12));
            return 0;
        }
        case OP_LOADTILE: {
            DxScope dx(mStats, mDxOn, DX_LOAD);
            DpLoadTile(static_cast<uint8_t>(C1(c.w1, 24, 3)), C0(c.w0, 12, 12), C0(c.w0, 0, 12), C1(c.w1, 12, 12),
                       C1(c.w1, 0, 12));
            return 0;
        }
        case OP_SETCOMBINE:
            SetCombineWords(c.w0, c.w1);
            return 0;
        case OP_SETENVCOLOR:
            DpSetEnvColor(RgbaOf(c.w1));
            return 0;
        case OP_SETPRIMCOLOR:
            DpSetPrimColor(static_cast<uint8_t>(C0(c.w0, 8, 8)), static_cast<uint8_t>(C0(c.w0, 0, 8)), RgbaOf(c.w1));
            return 0;
        case OP_SETFOGCOLOR:
            DpSetFogColor(RgbaOf(c.w1));
            return 0;
        case OP_SETBLENDCOLOR:
            DpSetBlendColor(RgbaOf(c.w1));
            return 0;
        case OP_SETFILLCOLOR:
            DpSetFillColor(c.w1);
            return 0;
        case OP_SETPRIMDEPTH:
            DpSetPrimDepth(static_cast<uint16_t>((c.w1 >> 16) & 0x7FFFu));
            return 0;
        case OP_SETKEYR:
            mRdp.key_center.r = static_cast<uint8_t>(C1(c.w1, 8, 8));
            mRdp.key_scale.r = static_cast<uint8_t>(C1(c.w1, 0, 8));
            return 0;
        case OP_SETKEYGB:
            mRdp.key_center.g = static_cast<uint8_t>(C1(c.w1, 24, 8));
            mRdp.key_scale.g = static_cast<uint8_t>(C1(c.w1, 16, 8));
            mRdp.key_center.b = static_cast<uint8_t>(C1(c.w1, 8, 8));
            mRdp.key_scale.b = static_cast<uint8_t>(C1(c.w1, 0, 8));
            return 0;
        case OP_SETCONVERT:
            mRdp.convert_k[0] = static_cast<int16_t>(C0(c.w0, 13, 9));
            mRdp.convert_k[1] = static_cast<int16_t>(C0(c.w0, 4, 9));
            mRdp.convert_k[2] = static_cast<int16_t>(((c.w0 & 0xFu) << 5) | C1(c.w1, 27, 5));
            mRdp.convert_k[3] = static_cast<int16_t>(C1(c.w1, 18, 9));
            mRdp.convert_k[4] = static_cast<int16_t>(C1(c.w1, 9, 9));
            mRdp.convert_k[5] = static_cast<int16_t>(C1(c.w1, 0, 9));
            return 0;

        case OP_TEXRECT:
        case OP_TEXRECTFLIP: {
            DxScope dx(mStats, mDxOn, DX_RECT);
            /* The two trailing halves (RDPHALF_1: s,t; RDPHALF_2: dsdx,dtdy) belong to this
             * command; consume them here so RDPHALF_1 never latches a branch target. */
            if (f.idx + 2 > f.list.limit) {
                mStats.droppedOps++;
                f.idx = f.list.limit;
                return 0;
            }
            const Cmd h1 = ReadCmd(f.list, f.idx);
            const Cmd h2 = ReadCmd(f.list, f.idx + 1);
            f.idx += 2;
            mStats.op[Opcode(h1.w0)]++;
            mStats.op[Opcode(h2.w0)]++;
            mStats.cmds += 2;
            const int32_t lrx = static_cast<int32_t>(C0(c.w0, 12, 12));
            const int32_t lry = static_cast<int32_t>(C0(c.w0, 0, 12));
            const uint8_t tile = static_cast<uint8_t>(C1(c.w1, 24, 3));
            const int32_t ulx = static_cast<int32_t>(C1(c.w1, 12, 12));
            const int32_t uly = static_cast<int32_t>(C1(c.w1, 0, 12));
            const int16_t uls = static_cast<int16_t>(C1(h1.w1, 16, 16));
            const int16_t ult = static_cast<int16_t>(C1(h1.w1, 0, 16));
            const int16_t dsdx = static_cast<int16_t>(C1(h2.w1, 16, 16));
            const int16_t dtdy = static_cast<int16_t>(C1(h2.w1, 0, 16));
            DpTextureRectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, op == OP_TEXRECTFLIP);
            return 0;
        }
        case OP_FILLRECT: {
            DxScope dx(mStats, mDxOn, DX_RECT);
            DpFillRectangle(static_cast<int32_t>(C1(c.w1, 12, 12)), static_cast<int32_t>(C1(c.w1, 0, 12)),
                            static_cast<int32_t>(C0(c.w0, 12, 12)), static_cast<int32_t>(C0(c.w0, 0, 12)));
            return 0;
        }
        case OP_SETZIMG:
            DpSetZImage(c);
            return 0;
        case OP_SETCIMG:
            DpSetColorImage(C0(c.w0, 21, 3), C0(c.w0, 19, 2), C0(c.w0, 0, 11), c);
            return 0;
        case OP_RDPHALF_2:
            /* Only meaningful as a TEXRECT trailer (consumed above); stray = ignore. */
            return 0;

        default:
            mStats.unknownOps++;
            if (UnknownLoggedOnce(op)) {
                GDXN_LOG("[ninterp] unknown opcode %02X w0=%08X w1=%08X (class=%d f3d=%d)", op, c.w0, c.w1,
                         static_cast<int>(ClassOf(f.list)), f.list.isF3d ? 1 : 0);
            }
            return 0;
    }
}

} // namespace gdxn
