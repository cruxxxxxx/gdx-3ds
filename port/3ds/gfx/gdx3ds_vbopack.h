/* gdx3ds_vbopack.h — TRILOOP: direct PICA-layout vertex emission contract.
 *
 * The interpreter's tri path used to copy every vertex TWICE: GfxSpTri1 appended
 * a variable-stride vertex to mBufVbo, then DrawTriangles re-walked the whole
 * batch repacking it into the fixed 12-float PICA layout in the linearAlloc VBO
 * pool (per-field branches, fminf clamps, constant re-reads). Precedent:
 * Wyatt-James/sm64-3ds-port's tri-loop optimization — fold the repack into the
 * first write. Under this contract the interpreter asks the backend for the
 * batch's pack parameters ONCE per batch (gdx3ds_vbopack_begin) and writes the
 * final PICA-layout floats straight into the linear pool; Flush() then submits
 * with no repack (gdx3ds_vbopack_draw).
 *
 * Soundness rests on the flush invariant this renderer already enforces
 * everywhere (s7 memo, prim/env constants, fog line): NO state that feeds pack
 * parameters (shader, bound textures + their sampler cms/cmt, fog/blend colour,
 * prim/env) can change between the first append of a batch and its draw without
 * a Flush() splitting the batch first — so parameters latched at batch begin
 * equal what DrawTriangles would have derived at draw time.
 *
 * The variable-stride vertex is LOSSY-reduced by the legacy repack (only
 * vtxColorInput survives per-vertex; other combiner inputs become vertex-0
 * stage constants). The packed path therefore delivers those vertex-0 values
 * out of band in Gdx3dsVboPackAux, written by the interpreter from the SAME
 * state the legacy vertex-0 append would have baked.
 *
 * Everything is __3DS__-only: desktop backends keep the variable-stride path
 * untouched (the interpreter hunk is #ifdef-gated, this header is only on the
 * 3DS include path). Killswitch: [debug] triloop=0 or env GDX_TRILOOP_OFF makes
 * gdx3ds_vbopack_begin always refuse, restoring the legacy two-stage path
 * (which also remains the automatic fallback whenever begin refuses: no shader,
 * frame inactive, pool nearly exhausted).
 */
#ifndef GDX3DS_VBOPACK_H
#define GDX3DS_VBOPACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed PICA vertex layout (shader.v.pica): pos4 + uv0 2 + rgba4 + uv1 2.
 * Must equal gfx_citro3d.cpp's kOutStrideFloats. */
#define GDX3DS_VBOPACK_STRIDE 12

/* Per-batch pack parameters, filled by the backend at batch begin. */
typedef struct Gdx3dsVboPackParams {
    /* Pow2-padding UV rescale for each texture unit (1.0 when unused/unbound). */
    float uScale[2];
    float vScale[2];
    /* Non-shader-clamp content-edge limits in PADDED UV space (the sky-wedge
     * fix's (extent - 0.5) / paddedSize), pre-gated on the sky_clamp_fix flag
     * and the texture's (cms & G_TX_CLAMP) && content < padded condition.
     * 1e30 = no clamp. */
    float wrapClampU[2];
    float wrapClampV[2];
    /* Whether shader-clamp axes (interpreter tm bits) should apply their
     * per-draw clamp limit ((tileExtent - 0.5) / texExtent * scale). Mirrors the
     * sky_clamp_fix gate on the legacy repack's shader-clamp branch. */
    uint8_t shaderClampGate;
    uint8_t used[2]; /* prg.cc.usedTextures */
    /* out[9] policy, precedence order (mirrors the legacy repack):
     * fogFactorToAlpha: out[9] = baked fog factor (prg.fogBlendStage draws);
     * else wantVtxAlpha: out[9] = vtxAlphaInput's per-vertex alpha;
     * else out[9] = 1.0. */
    uint8_t fogFactorToAlpha;
    uint8_t wantVtxAlpha;
    /* 1-based combiner input whose rgb rides the vertex colour attribute
     * (prg.vtxColorInput); 0 = none (out[6..8] = 1.0). */
    int8_t vtxColorInput;
    int8_t vtxAlphaInput;
    /* [trectbatch] atlas VIEW support: per-unit UV offset added AFTER the clamp
     * (uv_final = min(uv * scale, clamp) + off) and a batch flag telling the
     * interpreter to take the offset expression at all (0 = legacy expression,
     * byte-identical). Refreshed mid-batch by gdx3ds_vbopack_refresh on a
     * same-page view merge. */
    float uOff[2];
    float vOff[2];
    uint8_t hasOff;
} Gdx3dsVboPackParams;

/* Vertex-0 batch record: everything the legacy variable-stride buffer delivered
 * that the fixed PICA layout cannot carry. Written by the interpreter at the
 * batch's first vertex, consumed at draw time. */
typedef struct Gdx3dsVboPackAux {
    /* Vertex-0 value of every combiner input (RefreshStageConstants source):
     * rgb from the k==0 mux switch, alpha from the k==1 switch including the
     * standard-fog shade-alpha == 1.0 substitution. */
    float inputRgba[7][4];
    uint8_t numInputs;
    uint8_t hasFog; /* batch appended fog data (use_fog) */
    /* Draw-constant fog rgb (blend_color for shroud mode, fog_color otherwise)
     * — the legacy vertex-0 fog slot. */
    float fogRgb[3];
    /* Vertex-0 fog factor. For the depth-LUT fallback path (exact=0 shroud
     * draws) this is the constant fog_color.a factor: the legacy vertex scan's
     * secant through a constant series is exactly a=0, b=fogFactor0. */
    float fogFactor0;
} Gdx3dsVboPackAux;

/* Batch begin: returns the destination write cursor into the linear VBO pool
 * (room for a full MAX_TRI_BUFFER batch guaranteed), or NULL to refuse — the
 * interpreter must then use the legacy variable-stride path for the whole
 * batch. */
float* gdx3ds_vbopack_begin(Gdx3dsVboPackParams* params);
/* [trectbatch] Recompute the texture-dependent fields (scale/offset/wrap clamp/
 * hasOff) of an OPEN packed batch's params from the currently bound texture ids:
 * called by the interpreter right after a same-page atlas view merge. */
void gdx3ds_vbopack_refresh(Gdx3dsVboPackParams* params);
/* [trectbatch] atlas contract: arm the next UploadTexture as atlas-eligible (the
 * interpreter arms only around a clamp-addressed, UV-inside rect import), page of
 * a texture id (-1 = standalone), receipts {placed, full, pages, resets}. */
void gdx3ds_atlas_arm(int on);
int gdx3ds_tex_page(uint32_t texId);
void gdx3ds_atlas_stats(unsigned long out[5]); /* + linear free KiB */

/* Submit a packed batch of numTris triangles written at the cursor returned by
 * the matching gdx3ds_vbopack_begin. */
void gdx3ds_vbopack_draw(size_t numTris, const Gdx3dsVboPackAux* aux);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_VBOPACK_H */
