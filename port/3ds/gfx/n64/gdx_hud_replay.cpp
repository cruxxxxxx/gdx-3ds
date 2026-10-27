/* port/3ds/gfx/n64/gdx_hud_replay.cpp -- [native] hud_batch: the owned interpreter's side of
 * the HUD capture (C5 Stage 2, docs/research/native/c5s2-progress.md; o3ds60-design.md
 * section 5).
 *
 * WHAT RUNS HERE. gdx3ds_hud_capture.c compacted the HUD's display list: each run of
 * recordable commands (RDP/RSP state, texture loads, texture and fill rectangles) now sits in a
 * per-GfxPool record behind one G_NOOP p=0x5E marker. HudReplayRun executes the run at that
 * marker through Exec -- the walker's own handlers, in the same order, at the same position in
 * the stream -- so every state change, texture import, rectangle and batch flush is the one
 * the stock walk would have made. What it does NOT pay is the walker's fetch/dispatch for the
 * moved commands (the RDP syncs among them were dropped at capture: no-ops on this port).
 *
 * THE MEMOS. Measured with [native] hudcensus=1 (the [hudop] receipt), a replayed HUD frame
 * spends its time in G_LOADBLOCK (~27 loads: source resolution through the bridge seam, the
 * TMEM mirror, the span store) and in the rectangles' children (state resolution, the
 * texture-key content hash, emission) -- not in the walker's dispatch. So inside a run:
 *   - HudResolveTextureSource memoises (SETTIMG words, load extent) -> (pointer, class), keyed
 *     on the bridge's classification epoch (gdx_gfx_timg_class_epoch, the freshness term
 *     [native] timgmemo uses), for o2r (`immutable`) and GDX1 host-tagged operands. Segmented
 *     non-asset operands are never stored (the segment table moves per task).
 *   - HudHashLookup/Store memoise the source-span hash of immutable non-CI sources (first and
 *     last 8 bytes kept as a sentinel).
 *   - HudTmemHashLookup/Store memoise the TMEM-span hash of the RGBA16/I/IA classes when the
 *     live span was written by a LOADBLOCK from a source the bridge classifies STABLE, NATIVE
 *     or DMA, keyed on the generations sampled before that load's mirror
 *     (gdx_gfx_tmem_source_stamp/_unchanged, the seam [native] tmemskip uses). The race HUD's
 *     textures live in the DMA-loaded segment-4 carve.
 *   - HudRectState lets a draw (rectangle or triangle) that follows a draw of the same kind
 *     with only colour writes between reuse the tri memo instead of re-resolving state.
 * All are cleared with the texture cache (gdx_rdram_mode_reset). With the region's lever at 2
 * (verify) the stock answer is computed every time and compared instead: `vfy ok/bad` on the
 * [hud]/[shd]/[fxb] receipts is the in-process proof that no memo changes an answer.
 *
 * Nothing here touches the backend directly; the batching itself is the interpreter's own
 * [triloop] packed batch plus the [trectbatch] atlas, so draws/batches are unchanged by
 * construction (the [c5cnt] hud d= column is the receipt). */
#include "gdx_interp.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_hud_record.h"
#include "gdx_dlsrc_bridge.h"
#include "gdx3ds_hitch.h" // HITCH: per-frame event counters
#include "gdx3ds_hotcold.h"

/* The [native] dspcensus clock (gdx_rt_glue.cpp / the host harness): the census below splits a
 * replayed run's time by opcode, SELF time with the [prof] child sections removed. */
extern "C" long long gdx_ninterp_now(void);
extern "C" unsigned long long gdx_ninterp_child_ticks(void);

namespace gdxn {

namespace {

constexpr unsigned kSrcSlots = 128;  /* power of two; the race HUD resolves ~30 distinct operands */
constexpr unsigned kHashSlots = 128; /* power of two */
constexpr uint32_t kSentinel = 8;

bool IsRectOp(uint8_t op) {
    return op == OP_TEXRECT || op == OP_TEXRECTFLIP || op == OP_FILLRECT;
}

bool IsTriOp(uint8_t op) {
    return op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD;
}

/* Colour registers: read at emission, never by ResolveDrawState. */
bool IsColourOp(uint8_t op) {
    return op == OP_SETPRIMCOLOR || op == OP_SETENVCOLOR || op == OP_SETFILLCOLOR;
}

bool IsLoadOp(uint8_t op) {
    return op == OP_LOADBLOCK || op == OP_LOADTILE || op == OP_LOADTLUT;
}

uint32_t SrcSlot(const Cmd& c, size_t required) {
    const uint32_t req = static_cast<uint32_t>(required);
    return ((c.w1 * 2654435761u) ^ (c.w1hi * 97u) ^ (req * 40503u)) >> 25 & (kSrcSlots - 1u);
}

uint32_t HashSlot(const uint8_t* addr, uint32_t n) {
    return ((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr)) * 2654435761u) ^ (n * 40503u)) >> 25 &
           (kHashSlots - 1u);
}

/* The first and last kSentinel bytes of a span (zero-padded when shorter). */
void Sentinel(const uint8_t* addr, uint32_t n, uint8_t head[kSentinel], uint8_t tail[kSentinel]) {
    const uint32_t take = n < kSentinel ? n : kSentinel;
    memset(head, 0, kSentinel);
    memset(tail, 0, kSentinel);
    memcpy(head, addr, take);
    memcpy(tail, addr + (n - take), take);
}

} // namespace

struct Interp::HudMemo {
    struct Src {
        uint32_t w0, w1, w1hi, required, epoch;
        const uint8_t* addr;
        uint8_t native, immutable, filled;
    };
    struct Hash {
        const uint8_t* addr;
        uint32_t n, hash;
        uint8_t head[kSentinel], tail[kSentinel];
        uint8_t filled;
    };
    struct Tmem {
        const uint8_t* addr;
        uint32_t size, line, full, span, hash;
        uint64_t stamp[2]; /* the source generations sampled before the mirror that was hashed */
        uint16_t base;
        uint8_t native, cls, filled;
    };
    Src src[kSrcSlots];
    Hash hash[kHashSlots];
    Tmem tmem[kHashSlots];
    GdxHudReplayStats* rp;
    /* verify: the memo's answer for the import whose stock hash is being computed */
    uint32_t pendingHash;
    const uint8_t* pendingAddr;
    uint32_t pendingN;
    bool pending;
    /* census (gputrace): per-opcode self ticks / calls and the [prof] child ticks, per window */
    uint64_t opTicks[256];
    uint32_t opCalls[256];
    uint64_t childTicks;
    uint32_t runs;
    /* the last load resolved in this run: its source and the generations sampled BEFORE the
     * mirror read it (gdx_gfx_tmem_source_stamp's contract) */
    const uint8_t* loadAddr;
    uint64_t loadStamp[2];
};

void Interp::HudMemoClear() {
    if (mHudMemo != nullptr) {
        memset(mHudMemo->src, 0, sizeof(mHudMemo->src));
        memset(mHudMemo->hash, 0, sizeof(mHudMemo->hash));
        memset(mHudMemo->tmem, 0, sizeof(mHudMemo->tmem));
    }
}

/* ---- source-resolution memo (ResolveTextureSource while a run replays) --------------------- */

bool Interp::HudResolveTextureSource(size_t requiredBytes) {
    auto& tl = mRdp.texture_to_load;
    HudMemo& m = *mHud;
    gdx_gfx_tmem_source_stamp(m.loadStamp); /* before the load's mirror reads the source */
    m.loadAddr = nullptr;
    const uint32_t epoch = mSrc.TimgClassEpoch();
    HudMemo::Src& e = m.src[SrcSlot(tl.cmd, requiredBytes)];
    const bool hit = e.filled != 0 && e.epoch == epoch && e.w0 == tl.cmd.w0 && e.w1 == tl.cmd.w1 &&
                     e.w1hi == tl.cmd.w1hi && e.required == static_cast<uint32_t>(requiredBytes);
    if (hit && mHudLevel == 1) {
        tl.addr = e.addr;
        tl.native_rgba16 = e.native;
        tl.immutable = e.immutable;
        m.rp->srcHit++;
        m.loadAddr = tl.addr;
        return true;
    }
    if (!ResolveTextureSourceStock(requiredBytes)) {
        return false; /* failures are never memoised */
    }
    m.loadAddr = tl.addr;
    if (hit) {
        const bool same = e.addr == tl.addr && e.native == tl.native_rgba16 && e.immutable == tl.immutable;
        if (same) {
            m.rp->verifyOk++;
        } else {
            m.rp->verifyBad++;
        }
    }
    if (tl.immutable == 0 && tl.cmd.w1hi != kHostTag) {
        m.rp->srcInel++; /* a segmented, non-asset operand: the segment table moves per task */
        return true;
    }
    if (!hit) {
        m.rp->srcMiss++;
        gdx3ds_hitch_add(GDX_HITCH_EV_HUD_MISS, 1u); /* HITCH */
    }
    e.w0 = tl.cmd.w0;
    e.w1 = tl.cmd.w1;
    e.w1hi = tl.cmd.w1hi;
    e.required = static_cast<uint32_t>(requiredBytes);
    e.epoch = epoch;
    e.addr = tl.addr;
    e.native = tl.native_rgba16;
    e.immutable = tl.immutable;
    e.filled = 1;
    return true;
}

/* ---- immutable content-hash memo (ImportTexture while a run replays) ----------------------- */

bool Interp::HudHashLookup(const uint8_t* addr, uint32_t n, uint32_t* out) {
    HudMemo& m = *mHud;
    const HudMemo::Hash& e = m.hash[HashSlot(addr, n)];
    m.pending = false;
    if (e.filled == 0 || e.addr != addr || e.n != n) {
        return false;
    }
    uint8_t head[kSentinel], tail[kSentinel];
    Sentinel(addr, n, head, tail);
    if (memcmp(head, e.head, kSentinel) != 0 || memcmp(tail, e.tail, kSentinel) != 0) {
        return false;
    }
    if (mHudLevel == 1) {
        *out = e.hash;
        m.rp->hashHit++;
        return true;
    }
    m.pending = true; /* verify: let the stock hash run, compare it in HudHashStore */
    m.pendingHash = e.hash;
    m.pendingAddr = addr;
    m.pendingN = n;
    return false;
}

void Interp::HudHashStore(const uint8_t* addr, uint32_t n, uint32_t h) {
    HudMemo& m = *mHud;
    if (m.pending && m.pendingAddr == addr && m.pendingN == n) {
        if (m.pendingHash == h) {
            m.rp->verifyOk++;
        } else {
            m.rp->verifyBad++;
        }
        m.pending = false;
        return;
    }
    HudMemo::Hash& e = m.hash[HashSlot(addr, n)];
    e.addr = addr;
    e.n = n;
    e.hash = h;
    Sentinel(addr, n, e.head, e.tail);
    e.filled = 1;
    m.rp->hashMiss++;
}

/* ---- TMEM content-hash memo (ImportTexture's TMEM-hashed classes while a run replays) ---------
 * For a non-immutable RGBA16 / I / IA import the key hashes `span` TMEM bytes from `base`. When
 * the live span at `base` was written by a LOADBLOCK from a source the bridge classifies
 * STABLE (a registered host range: not the GfxPool, not the venue window -- bytes that never
 * move, gdx_dlsrc_bridge.h), those TMEM bytes are a pure function of (source, size, line
 * strides, byte-swap) and so is their hash. Only such entries are stored, and only when the
 * mirror really covered the span (readable source >= span). */
bool Interp::HudTmemHashLookup(uint16_t base, uint32_t span, uint32_t* out) {
    HudMemo& m = *mHud;
    m.pending = false;
    if (base >= 512u || mRdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u) ||
        mRdp.tmem_span_load_kind[base] != 1u) {
        return false;
    }
    const LoadedTexture& lt = mRdp.loaded_texture[base];
    if (lt.addr == nullptr || span > lt.size_bytes) {
        return false;
    }
    const HudMemo::Tmem& e = m.tmem[HashSlot(lt.addr, span ^ (static_cast<uint32_t>(base) << 16))];
    if (e.filled == 0 || e.addr != lt.addr || e.size != lt.size_bytes || e.line != lt.line_size_bytes ||
        e.full != lt.full_image_line_size_bytes || e.native != lt.native_rgba16 || e.span != span ||
        e.base != base) {
        return false;
    }
    if (gdx_gfx_tmem_source_unchanged(reinterpret_cast<uintptr_t>(lt.addr), lt.size_bytes, e.cls, e.stamp) == 0) {
        return false; /* the source moved since the hashed mirror (or its class did) */
    }
    if (mHudLevel == 1) {
        *out = e.hash;
        m.rp->hashHit++;
        return true;
    }
    m.pending = true;
    m.pendingHash = e.hash;
    m.pendingAddr = lt.addr;
    m.pendingN = span;
    return false;
}

void Interp::HudTmemHashStore(uint16_t base, uint32_t span, uint32_t h) {
    HudMemo& m = *mHud;
    if (base >= 512u || mRdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u) ||
        mRdp.tmem_span_load_kind[base] != 1u) {
        return;
    }
    const LoadedTexture& lt = mRdp.loaded_texture[base];
    if (m.pending && m.pendingAddr == lt.addr && m.pendingN == span) {
        if (m.pendingHash == h) {
            m.rp->verifyOk++;
        } else {
            m.rp->verifyBad++;
        }
        m.pending = false;
        return;
    }
    if (lt.addr == nullptr || span > lt.size_bytes) {
        return;
    }
    const uintptr_t src = reinterpret_cast<uintptr_t>(lt.addr);
    const unsigned cls = gdx_gfx_tmem_source_class(src, lt.size_bytes);
    if (lt.addr != m.loadAddr || cls == GDX_TMEMSRC_UNKNOWN || DlSrc::ReadableBytes(src) < span) {
        m.rp->hashMiss++; /* not memoisable: counted, recomputed next time */
        return;
    }
    HudMemo::Tmem& e = m.tmem[HashSlot(lt.addr, span ^ (static_cast<uint32_t>(base) << 16))];
    e.addr = lt.addr;
    e.size = lt.size_bytes;
    e.line = lt.line_size_bytes;
    e.full = lt.full_image_line_size_bytes;
    e.span = span;
    e.hash = h;
    e.base = base;
    e.native = lt.native_rgba16;
    e.cls = static_cast<uint8_t>(cls);
    e.stamp[0] = m.loadStamp[0];
    e.stamp[1] = m.loadStamp[1];
    e.filled = 1;
    m.rp->hashMiss++;
}

/* ---- draw fast path (SpTri while mHudRectFast: rectangles and triangles) -------------------------------
 * mHudRectFast is armed by the replay loop only when every command since the previous draw of
 * the same kind in the SAME run was such a draw or a colour-register write (prim/env/fill). None of
 * those writes an input of ResolveDrawState: the rectangle handlers restore what they touch
 * (tile index, combine mode, viewport, geometry mode) and the colour registers are read at
 * emission, not at resolution (a prim/env change that splits the batch flushes it, which
 * invalidates the memo -- checked here). So when the batch the last rectangle resolved is still
 * open, its tri memo IS what ResolveDrawState would return, and the state block (combiner
 * lookup, texture/sampler checks, shader lookup, blend) is skipped. hud_batch=2 runs
 * ResolveDrawState anyway and compares. */
bool Interp::HudRectState(bool isRect, DrawState& out) {
    /* textures_changed is consumed per unit only where the combiner samples it (a unit the
     * combiner never reads stays armed forever), exactly as ResolveDrawState reads it. */
    if (!mTriMemoValid || !mPackBatch || mBufNumTris == 0 || mTriMemo.comb == nullptr ||
        (mTriMemo.comb->usedTextures[0] && mRdp.textures_changed[0]) ||
        (mTriMemo.comb->usedTextures[1] && mRdp.textures_changed[1]) ||
        memcmp(&mRdp.viewport, &mRs.viewport, sizeof(Rect)) != 0 || memcmp(&mRdp.scissor, &mRs.scissor, sizeof(Rect)) != 0) {
        return false;
    }
    GdxHudReplayStats& rp = *mHudMemo->rp;
    if (GDX_UNLIKELY(mHudLevel != 1)) {
        /* hotcold: verify resolves fresh and compares, out of line */
        return [this, &rp, isRect, &out]() GDX_COLD -> bool {
        const DrawState memo = mTriMemo;
        DrawState fresh;
        if (!ResolveDrawState(isRect, fresh)) {
            return false;
        }
        const bool same = fresh.comb == memo.comb && fresh.prg == memo.prg && fresh.tm == memo.tm &&
                          memcmp(fresh.tex_width, memo.tex_width, sizeof(memo.tex_width)) == 0 &&
                          memcmp(fresh.tex_height, memo.tex_height, sizeof(memo.tex_height)) == 0 &&
                          memcmp(fresh.tex_width2, memo.tex_width2, sizeof(memo.tex_width2)) == 0 &&
                          memcmp(fresh.tex_height2, memo.tex_height2, sizeof(memo.tex_height2)) == 0 &&
                          memcmp(fresh.effective_tile, memo.effective_tile, sizeof(memo.effective_tile)) == 0 &&
                          fresh.usedTextures[0] == memo.usedTextures[0] &&
                          fresh.usedTextures[1] == memo.usedTextures[1] && fresh.numInputs == memo.numInputs &&
                          fresh.use_alpha == memo.use_alpha && fresh.use_fog == memo.use_fog &&
                          fresh.use_blend_color == memo.use_blend_color && mBufNumTris != 0;
        if (same) {
            rp.verifyOk++;
        } else {
            rp.verifyBad++;
        }
        out = fresh;
        return true;
        }();
    }
    out = mTriMemo;
    rp.fastTris++;
    return true;
}

/* ---- the marker ---------------------------------------------------------------------------- */

void Interp::HudReplayRun(const void* record, uint32_t runIndex) {
    GdxHudRecord* rec = static_cast<GdxHudRecord*>(const_cast<void*>(record));
    if (rec == nullptr || rec->magic != GDXHUD_MAGIC) {
        return;
    }
    if (runIndex >= rec->nRuns) {
        rec->rp[GDXHUD_KIND_HUD].badRecord++;
        return;
    }
    const uint32_t group = rec->runs[runIndex].group;
    if (GDX_LIKELY(group <= 1)) {
        HudReplayRunOne(rec, runIndex);
        return;
    }
    /* n60results [native] hud_group: the marker of a group replays its runs in order, as the walker
     * replayed one marker per run (nothing ran between them) */
    if (runIndex + group > rec->nRuns) {
        rec->rp[GDXHUD_KIND_HUD].badRecord++;
        return;
    }
    for (uint32_t k = 0; k < group; k++) {
        HudReplayRunOne(rec, runIndex + k);
    }
}

void Interp::HudReplayRunOne(void* record, uint32_t runIndex) {
    GdxHudRecord* rec = static_cast<GdxHudRecord*>(record);
    if (rec->runs[runIndex].end > rec->nWords || rec->runs[runIndex].start > rec->runs[runIndex].end ||
        rec->runs[runIndex].kind >= GDXHUD_KIND_COUNT) {
        rec->rp[GDXHUD_KIND_HUD].badRecord++;
        return;
    }
    if (mHudMemo == nullptr) {
        mHudMemo = new (std::nothrow) HudMemo;
        if (mHudMemo == nullptr) {
            rec->rp[rec->runs[runIndex].kind].badRecord++;
            return;
        }
        memset(static_cast<void*>(mHudMemo), 0, sizeof(HudMemo));
    }
    const GdxHudRun& run = rec->runs[runIndex];
    if (GDX_UNLIKELY(mNrrCensus != 0) && run.kind == GDXHUD_KIND_HUD) {
        /* [native] nrr_hud_census: the inventory line (gdx_nrr_hud.cpp), before the run moves state */
        NrrCensusRun(&rec->words[run.start], static_cast<uint32_t>(run.end - run.start), runIndex, rec->frame);
    }
    if (run.kind == GDXHUD_KIND_MENU) {
        HudReplayMenuRun(rec, run); /* nimenu [native] ni_menu */
        return;
    }
    if (GDX_UNLIKELY(mNrrOpen) && run.kind != GDXHUD_KIND_HUD) {
        NrrHudHotClose(); /* [native] nrr_hud: a shadow / flame run draws through the backend */
    }
    /* [debug] nicensus: the NRR HUD decoder's attempt is its own context (measurement only) */
    const int ncPrev =
        (GDX_UNLIKELY(mNc != nullptr) && mNrrHud != 0 && run.kind == GDXHUD_KIND_HUD) ? NcSwitch(NC_NRRHUD) : -1;
    if (GDX_UNLIKELY(mNrrRunClock != 0) && mNrrHud != 0 && run.kind == GDXHUD_KIND_HUD) {
        /* [debug] nrr_hud_runclock (n60hudx, diagnostic) */
        const uint64_t t0 = static_cast<uint64_t>(gdx_ninterp_now());
        if (NrrHudHotRun(&rec->words[run.start], static_cast<uint32_t>(run.end - run.start), run.mode)) {
            NrrRunClockNote(mNrrRunInTask++, &rec->words[run.start], static_cast<uint32_t>(run.end - run.start),
                            static_cast<uint64_t>(gdx_ninterp_now()) - t0);
            rec->rp[GDXHUD_KIND_HUD].markers++;
            if (ncPrev >= 0) {
                NcNoteWords(static_cast<uint32_t>(run.end - run.start), 0);
                NcSwitch(ncPrev);
            }
            return;
        }
    } else if (mNrrHud != 0 && run.kind == GDXHUD_KIND_HUD &&
        NrrHudHotRun(&rec->words[run.start], static_cast<uint32_t>(run.end - run.start), run.mode)) {
        rec->rp[GDXHUD_KIND_HUD].markers++; /* [native] nrr_hud: the NRR HUD decoded (=2: verified) the run */
        if (ncPrev >= 0) {
            NcNoteWords(static_cast<uint32_t>(run.end - run.start), 0);
            NcSwitch(ncPrev);
        }
        return;
    }
    if (ncPrev >= 0) {
        NcSwitch(ncPrev);
    }
    if (GDX_UNLIKELY(mNrrKept != 0) && run.kind == GDXHUD_KIND_HUD) {
        /* n60hudx [native] nrr_hud_kept: a run holding a recorded G_DL takes the plain replay (the
         * direct / retain paths model no G_DL) */
        const GdxHudWord* rw = &rec->words[run.start];
        for (uint32_t k = 0; k < static_cast<uint32_t>(run.end - run.start); k++) {
            if (Opcode(rw[k].w0) == OP_DL) {
                rec->rp[GDXHUD_KIND_HUD].markers++;
                HudReplayRunPlain(rw, static_cast<uint32_t>(run.end - run.start));
                return;
            }
        }
    }
    Frame f;
    f.list.base = reinterpret_cast<const uint8_t*>(&rec->words[run.start]);
    f.list.limit = static_cast<size_t>(run.end - run.start);
    f.list.stride = static_cast<uint8_t>(sizeof(GdxHudWord));
    f.list.isBig = false;
    f.list.isF3d = false;
    f.idx = 0;
    GdxHudReplayStats& rp = rec->rp[run.kind];
    rp.markers++;
    mHudMemo->rp = &rp;
    mHudMemo->pending = false;
    mHudLevel = static_cast<int>(run.mode);
    mHud = mHudMemo;
    const bool census = mHudCensus && mBackend->ProfActive();
    mHudMemo->runs++;
    /* [native] hud_direct (gdx_hud_direct.cpp): HUD runs only -- the shadow and boost-flame
     * runs draw triangles through matrices and stay on this replay as they are. */
    const bool direct = mHudDirect != 0 && run.kind == GDXHUD_KIND_HUD && HudDirectEnsure();
    /* [native] fxshd_direct (C5 Stage 3b): the shadow and boost-flame runs keep their stock
     * handlers except G_LOADBLOCK, which takes the direct load (resolved once, TMEM as debt). */
    const int fxshdLevel = mDirectLevels[0];
    const bool fxshdDirect = fxshdLevel != 0 && (run.kind == GDXHUD_KIND_SHADOW || run.kind == GDXHUD_KIND_FX);
    const bool verifyDirect = direct && HudDirectBegin();
    const bool runClock = direct && mHudClock != 0;
    const bool opClock = direct && mHudClock >= 2;
    const uint64_t runT0 = runClock ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
    uint32_t drains0[FLUSH_COUNT];
    if (GDX_UNLIKELY(runClock)) {
        memcpy(drains0, mStats.flushDrain, sizeof(drains0));
    }
    if (direct) {
        HsxMark(-1); /* [native] hudsky_emit, hudclock=2: the hit phase clock starts */
    }
    const bool elem = mHudElem != 0 && run.kind == GDXHUD_KIND_HUD;
    if (elem) {
        HudElemBegin(); /* [native] hudelem: the per-run census (gdx_hud_retain.cpp) */
    }
    /* [native] hud_retain (gdx_hud_retain.cpp): a direct run whose recorded effects apply
     * exactly is replayed from the record; otherwise it executes below (and is recorded). */
    const bool retained = direct && !verifyDirect && mHudRetain != 0 &&
                          HudRetainBegin(&rec->words[run.start], static_cast<uint32_t>(run.end - run.start)) != 0;
    if (retained) {
        f.idx = f.list.limit;
        if (GDX_UNLIKELY(mNc != nullptr)) {
            NcNoteWords(static_cast<uint32_t>(f.list.limit), 1); /* [debug] nicensus */
        }
    }
    mHsxLean = mHudSkyEmit == 1 && run.kind == GDXHUD_KIND_HUD; /* [native] hudsky_emit: lean drains */
    bool rectChain = false; /* only rectangles / colour writes since the last rectangle of this run */
    uint32_t lastRectKey = 0;
    while (f.idx < f.list.limit) {
        const Cmd c = ReadCmd(f.list, f.idx);
        const uint32_t wordIndex = static_cast<uint32_t>(f.idx);
        f.idx++;
        const uint8_t op = Opcode(c.w0);
        if (GDX_UNLIKELY(mNc != nullptr)) {
            /* [debug] nicensus; ni_strict files the direct emitter's rectangles / loads apart */
            const bool directOp = (direct && (op == OP_TEXRECT || op == OP_TEXRECTFLIP || op == OP_LOADBLOCK)) ||
                                  (fxshdDirect && op == OP_LOADBLOCK);
            NcNote(c, false, directOp ? NCS_HUDDIRECT : NCS_HUDRUN);
        }
        if (mHudRec) {
            HudRecOpPre(wordIndex, op);
        }
        /* The same draw kind: a FILLRECT (FILL-cycle combine), a TEXRECT per tile and a
         * triangle (the RSP's geometry mode and viewport) each resolve their own state. */
        const bool drawOp = IsRectOp(op) || IsTriOp(op);
        const uint32_t rectKey = IsTriOp(op) ? 0x05u : ((static_cast<uint32_t>(op) << 8) | C1(c.w1, 24, 3));
        mHudRectFast = rectChain && drawOp && rectKey == lastRectKey;
        if (drawOp) {
            rectChain = true;
            lastRectKey = rectKey;
        } else if (!IsColourOp(op)) {
            rectChain = false;
        }
        rp.words++;
        if (IsRectOp(op)) {
            rp.rects++;
        } else if (IsLoadOp(op)) {
            rp.loads++;
        }
        const uint64_t t0 = census ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
        const uint64_t k0 = census ? gdx_ninterp_child_ticks() : 0;
        const bool clockedOp = opClock && (op == OP_TEXRECT || op == OP_TEXRECTFLIP || op == OP_LOADBLOCK);
        const uint64_t opT0 = clockedOp ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
        if (direct && (op == OP_TEXRECT || op == OP_TEXRECTFLIP)) {
            if (GDX_UNLIKELY(verifyDirect)) {
                HudVerifyTexRect(c, f);
            } else {
                HudDirectTexRect(c, f);
            }
            if (GDX_UNLIKELY(opClock)) {
                HudDirectTicks(1, static_cast<uint64_t>(gdx_ninterp_now()) - opT0);
            }
        } else if (fxshdDirect && op == OP_LOADBLOCK) {
            DirectScopeLoadBlock(c, f, 0, fxshdLevel);
        } else if (direct && op == OP_LOADBLOCK) {
            HudDirectLoadBlock(c, f);
            if (GDX_UNLIKELY(opClock)) {
                HudDirectTicks(2, static_cast<uint64_t>(gdx_ninterp_now()) - opT0);
            }
        } else {
            (void)Exec(c, f, 0);
        }
        if (mHudRec) {
            HudRecOpPost(wordIndex, op);
        }
        if (GDX_UNLIKELY(mNrrCensus >= 2) && run.kind == GDXHUD_KIND_HUD) {
            NrrDebugStock(&rec->words[run.start], wordIndex); /* [native] nrr_hud_census=2 */
        }
        if (GDX_UNLIKELY(census)) {
            const uint64_t gross = static_cast<uint64_t>(gdx_ninterp_now()) - t0;
            const uint64_t child = gdx_ninterp_child_ticks() - k0;
            mHudMemo->opTicks[op] += child < gross ? gross - child : 0;
            mHudMemo->opCalls[op]++;
            mHudMemo->childTicks += child;
        }
    }
    if (mHudRec) {
        HudRetainEnd();
    }
    mHsxLean = false;
    mHud = nullptr;
    mHudRectFast = false;
    if (elem) {
        HudElemEnd(&rec->words[run.start], static_cast<uint32_t>(run.end - run.start));
    }
    if (run.kind == GDXHUD_KIND_HUD) {
        HudRetainRunDone();
    }
    if (GDX_UNLIKELY(runClock)) {
        HsxPhaseEnd(retained);
        HsxNoteRun(retained, static_cast<uint64_t>(gdx_ninterp_now()) - runT0); /* [hsx] per-path run clock */
        HudDirectTicks(0, static_cast<uint64_t>(gdx_ninterp_now()) - runT0);
        for (int k = 0; k < FLUSH_COUNT; k++) {
            HudDirectTicks(4 + k, mStats.flushDrain[k] - drains0[k]);
        }
    }
}

/* nimenu [native] ni_menu: a MENU-kind run (a non-race mode's draw, gdx3ds_menu_capture.c) runs through
 * the NRR program runner (gdx_nrr_menu.inc) when the NRR HUD is on; otherwise (or when the NRR refuses
 * it) through the walker's handlers word by word, its recorded G_DLs walked in place: the stock
 * execution of the same words in the same order. */
void Interp::HudReplayMenuRun(void* record, const GdxHudRun& run) {
    GdxHudRecord* rec = static_cast<GdxHudRecord*>(record);
    const GdxHudWord* w = &rec->words[run.start];
    const uint32_t n = static_cast<uint32_t>(run.end - run.start);
    const int ncPrev = GDX_UNLIKELY(mNc != nullptr) ? NcSwitch(mNiMenu != 0 && mNrrHud != 0 ? NC_NRRHUD : NC_HUD) : -1;
    rec->rp[GDXHUD_KIND_MENU].markers++;
    if (GDX_UNLIKELY(mNrrCensus != 0)) {
        NrrCensusRun(w, n, static_cast<uint32_t>(&run - rec->runs), rec->frame); /* [native] nrr_hud_census */
    }
    if (mNiMenu != 0 && mNrrHud != 0 && NrrMenuHotRun(w, n)) {
        if (ncPrev >= 0) {
            NcNoteWords(n, 0); /* [debug] nicensus */
            NcSwitch(ncPrev);
        }
        return;
    }
    if (ncPrev >= 0) {
        NcSwitch(NC_HUD); /* the stock replay is the HUD replay context */
    }
    HudReplayMenuStock(w, n);
    if (ncPrev >= 0) {
        NcSwitch(ncPrev);
    }
}

GDX_COLD void Interp::HudReplayMenuStock(const GdxHudWord* w, uint32_t n) {
    if (GDX_UNLIKELY(mNrrOpen)) {
        NrrHudHotClose(); /* the walker's handlers draw through the backend */
    }
    mNiMenuWin.stockRuns++;
    mNiMenuWin.stockWords += n;
    HudReplayRunPlain(w, n);
}

/* n60hudx [native] nrr_hud_kept: a HUD run holding a recorded G_DL, through the walker's handlers
 * word by word (no memos, no direct path, no retention); the G_DL walks its list in place. */
GDX_COLD void Interp::HudReplayRunPlain(const GdxHudWord* w, uint32_t n) {
    Frame f;
    f.list.base = reinterpret_cast<const uint8_t*>(w);
    f.list.limit = n;
    f.list.stride = static_cast<uint8_t>(sizeof(GdxHudWord));
    f.list.isBig = false;
    f.list.isF3d = false;
    f.idx = 0;
    while (f.idx < f.list.limit) {
        const Cmd c = ReadCmd(f.list, f.idx);
        f.idx++;
        if (GDX_UNLIKELY(mNc != nullptr)) {
            NcNote(c, false, NCS_HUDPLAIN); /* [debug] nicensus */
        }
        if (Opcode(c.w0) == OP_DL) {
            HudNestedDl(c);
        } else {
            (void)Exec(c, f, 0);
        }
    }
}

/* [hudop] receipt: the replayed runs' self ms per task by opcode (top 8), plus the [prof]
 * sections (tri/imp/drw/vtx/mtx) the handlers entered. Returns 0 when nothing was replayed. */
GDX_COLD int Interp::HudCensusFormat(char* buf, size_t cap, uint32_t tasks, double msPerTick) {
    if (mHudMemo == nullptr || mHudMemo->runs == 0 || tasks == 0 || cap == 0) {
        return 0;
    }
    HudMemo& m = *mHudMemo;
    const double inv = msPerTick / static_cast<double>(tasks);
    int n = snprintf(buf, cap, "[hudop] tasks=%u runs=%.1f child=%.3f |", static_cast<unsigned>(tasks),
                     static_cast<double>(m.runs) / tasks, static_cast<double>(m.childTicks) * inv);
    bool used[256] = {};
    for (int k = 0; k < 8 && n > 0 && static_cast<size_t>(n) < cap; k++) {
        int best = -1;
        for (int op = 0; op < 256; op++) {
            if (!used[op] && m.opCalls[op] != 0 && (best < 0 || m.opTicks[op] > m.opTicks[best])) {
                best = op;
            }
        }
        if (best < 0) {
            break;
        }
        used[best] = true;
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " %02X=%.3f/%.1f", best,
                      static_cast<double>(m.opTicks[best]) * inv, static_cast<double>(m.opCalls[best]) / tasks);
    }
    memset(m.opTicks, 0, sizeof(m.opTicks));
    memset(m.opCalls, 0, sizeof(m.opCalls));
    m.childTicks = 0;
    m.runs = 0;
    return n;
}

} // namespace gdxn
