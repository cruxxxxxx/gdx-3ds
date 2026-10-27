/* gdx3ds_track_replay.cpp — [native] track: the G_NOOP marker replay (gdx3ds_track_marker_seam).
 *
 * The course tessellator's recorded ops (gdx3ds_track_sink.c) are replayed here, at the
 * G_NOOP marker the shim wrote where the stock commands used to be. Vertex loads and ST
 * patches go through the interpreter's own SpVertex / SpModifyVertex (its loaded vertex
 * cache IS the RSP cache mirror; transform, aspect adjust, clip flags and fog factor are the
 * interpreter's code). Triangles: the first tri after any state change (and every tri that
 * needs the clipper) goes through the interpreter's triangle path so the draw state is
 * resolved by the interpreter, then is ADOPTED into a Gdx_NativeBatch (it was appended at the
 * pool cursor: pop it from the interpreter batch, commit it to ours — no copy). The remaining
 * tris take the fast path: the trifast emission's expressions verbatim, from the state
 * snapshot GdxTrackPackState exposes. Both paths live in gdx3ds_native_tri.cpp, shared with
 * the effects replay (native/fx). The batch flushes as ONE draw per material run (and per
 * template-embedded SETCOMBINE segment), in emission order — Z_CMP is off for the track.
 *
 * Interpreter access goes through GdxInterpSeam (gdx3ds_interp_seam.h): the same body runs
 * under the vendored LUS interpreter (its G_NOOP handler -> gdx3ds_seam_lus.cpp) and under
 * the owned walker (OP_NOOP hook -> n64/gdx_seam_owned.cpp).
 *
 * Verify ([native] track=2): the stock GBI runs too; the START marker snapshots the vertex
 * cache and arms the backend's stock-draw hash, the END marker flushes the stock batch,
 * restores the cache, replays the run into a native batch, hashes and DISCARDS it, and
 * compares. Bit-identical VBO bytes per run is the parity proof this branch claims. */
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gdx3ds_interp_seam.h"
#include "gdx3ds_native_tri.h"
#include "gdx3ds_nativebatch.h"
#include "gdx3ds_track_record.h"
#include "gdx3ds_track_retained.h"
#include "gdx3ds_track_templates.h"

extern "C" void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));

using GdxNativeTri::RunCtx;

namespace {

GdxSeamVertex sSnapshot[GDXSEAM_VERTEX_CACHE_COUNT];
bool sSnapValid = false;
uint32_t sVerifyLogged = 0;
uint32_t sErrLogged = 0;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    if (gdx_dbg_logf == nullptr) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

void ReplayTemplate(RunCtx& c, const GdxTrkTemplate& t) {
    int ev = 0;
    for (int i = 0; i < t.ntris; i++) {
        while (ev < t.nevents && t.events[ev].beforeTri == i) {
            GdxNativeTri::FlushNative(c); /* the combiner changes: draw what precedes it with the old state */
            c.I->SetCombineRaw(t.events[ev].w0, t.events[ev].w1);
            c.stats_combines++;
            ev++;
        }
        GdxNativeTri::Tri(c, t.idx[i * 3 + 0], t.idx[i * 3 + 1], t.idx[i * 3 + 2]);
    }
    while (ev < t.nevents) { /* trailing restore pair: state change, no draw follows in the template */
        GdxNativeTri::FlushNative(c);
        c.I->SetCombineRaw(t.events[ev].w0, t.events[ev].w1);
        c.stats_combines++;
        ev++;
    }
}

} // namespace

void gdx3ds_track_marker_seam(GdxInterpSeam& I, uint32_t w0, uintptr_t w1) {
    GdxTrkRecord* rec = reinterpret_cast<GdxTrkRecord*>(w1);
    const uint32_t p = (w0 >> 16) & 0xFFu;
    const uint32_t run = w0 & 0xFFFFu;
    if (rec == nullptr || rec->magic != GDXTRK_MAGIC) {
        if (sErrLogged++ < 4) {
            Log("[trk] marker with bad record %p (w0=%08x)", static_cast<void*>(rec), (unsigned)w0);
        }
        return;
    }
    const bool verify = rec->mode == 2;
    GdxSeamVertex* cache = I.R().vertices;
    if (p == GDXTRK_MARKER_P_START) {
        if (verify) {
            I.Flush(); /* whatever preceded the run is drawn now; the stock hash covers the run only */
            Gdx_NativeBatchStockHash(nullptr, 1);
            Gdx_NativeBatchStockHashArm(1);
            memcpy(sSnapshot, cache, sizeof(sSnapshot));
            sSnapValid = true;
        }
        return;
    }
    if (p == GDXTRK_MARKER_P_RETAINED && run < rec->nRuns) {
        gdx3ds_track_retained_run(I, rec, run); /* [native] track_retained (C5 Stage 3) */
        return;
    }
    if (p != GDXTRK_MARKER_P_END || run >= rec->nRuns) {
        return;
    }
    rec->rp.markers++;
    rec->rp.runs++;

    uint32_t stockTris = 0;
    uint32_t stockHash = 0;
    if (verify) {
        I.Flush();
        stockHash = Gdx_NativeBatchStockHash(&stockTris, 1);
        Gdx_NativeBatchStockHashArm(0);
        if (sSnapValid) {
            memcpy(cache, sSnapshot, sizeof(sSnapshot));
            sSnapValid = false; /* one START per END; a run without its START cannot verify */
        } else {
            stockTris = 0xFFFFFFFFu; /* forces a mismatch receipt below */
        }
    } else if (I.PendingTris() != 0) {
        I.Flush(); /* the native batch must start at the pool cursor with nothing pending */
    }

    RunCtx c;
    GdxNativeTri::Init(c, &I, verify);

    const GdxTrkRun& r = rec->runs[run];
    for (uint32_t k = r.opStart; k < r.opEnd && k < rec->nOps; k++) {
        const GdxTrkOp& op = rec->ops[k];
        switch (op.kind) {
            case GDXTRK_OP_VTX:
                rec->rp.vtxOps++;
                rec->rp.verts += op.n;
                I.SpVertex(op.n, op.slot, reinterpret_cast<const void*>(static_cast<uintptr_t>(op.arg)));
                break;
            case GDXTRK_OP_MOD:
                I.SpModifyVertex(op.slot, GDXSEAM_G_MWO_POINT_ST, op.arg);
                break;
            case GDXTRK_OP_DL:
                if (op.tmpl < GDXTRK_TEMPLATE_COUNT) {
                    ReplayTemplate(c, gGdxTrkTemplate[op.tmpl]);
                }
                break;
            default:
                break;
        }
    }
    GdxNativeTri::FlushNative(c);

    rec->rp.tris += c.stats_tris;
    rec->rp.fast += c.stats_fast;
    rec->rp.exact += c.stats_exact;
    rec->rp.fallback += c.stats_fallback;
    rec->rp.combines += c.stats_combines;
    rec->rp.draws += c.stats_draws;
    if (verify) {
        const uint32_t nativeTris = c.stats_fast + c.stats_exact;
        const bool ok = (c.stats_fallback == 0) && (nativeTris == stockTris) && (c.verifyHash == stockHash);
        if (ok) {
            rec->rp.verifyOk++;
        } else {
            rec->rp.verifyBad++;
            if (sVerifyLogged++ < 12) {
                Log("[trk] verify MISMATCH frame=%u run=%u stock=(%u tris,%08x) native=(%u tris,%08x) fast=%u exact=%u fb=%u",
                    rec->frame, run, stockTris, stockHash, nativeTris, c.verifyHash, c.stats_fast, c.stats_exact,
                    c.stats_fallback);
            }
        }
        /* The replay left the cache exactly as the stock run did (same op sequence). */
    }
}
