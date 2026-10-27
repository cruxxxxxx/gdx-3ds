/* gdx3ds_gad_gbi.c -- [native] gadget_batch (C5 Stage 3b): the real GBI emitters behind
 * gdx3ds_gad_shim_begin.h.
 *
 * Compiled with the game's own gbi.h and defines (F3DEX_GBI_2, PORT), each helper simply
 * invokes the ORIGINAL macro, so whatever course_gadgets.c would have encoded without the shim
 * is encoded here byte for byte -- including the 32-bit PORT's tagged host pointer in w1. No
 * macro body is copied. */
#include "global.h"

#include <stdint.h>

void gdxgad_gbi_dl(void* g, const void* dl) {
    gSPDisplayList((Gfx*)g, (const Gfx*)dl);
}

void gdxgad_gbi_vertex(void* g, const void* v, int n, int v0) {
    gSPVertex((Gfx*)g, (const Vtx*)v, n, v0);
}

void gdxgad_gbi_tri1(void* g, int a, int b, int c, int flag) {
    gSP1Triangle((Gfx*)g, a, b, c, flag);
}

void gdxgad_gbi_tri2(void* g, int a, int b, int c, int flag0, int d, int e, int f, int flag1) {
    gSP2Triangles((Gfx*)g, a, b, c, flag0, d, e, f, flag1);
}

void gdxgad_gbi_modify(void* g, int vtx, int where, uint32_t val) {
    gSPModifyVertex((Gfx*)g, vtx, where, val);
}

void gdxgad_gbi_texture(void* g, int s, int t, int level, int tile, int on) {
    gSPTexture((Gfx*)g, s, t, level, tile, on);
}

void gdxgad_gbi_settimg(void* g, int fmt, int siz, int width, const void* img) {
    gDPSetTextureImage((Gfx*)g, fmt, siz, width, img);
}

/* A G_NOOP whose w0 carries the marker tag and whose w1 is the record pointer (NULL for the
 * direct-load scope markers), written like any host pointer so the owned walker's noop hook
 * receives it unchanged (the fx/track marker shape). */
void gdxgad_gbi_marker(void* g, uint32_t w0, const void* record) {
    Gfx* _g = (Gfx*)g;
    _g->words.w0 = w0;
    _g->words.w1 = _GFXW1_PTR(record);
}

/* o3dsperf: a [c5cat] scene-category marker (G_NOOP p=0x61, port/3ds/gdx3ds_mainprof.h) written at
 * `g`; returns the next command slot. gdx_dl_skip (port/3ds/gdx3ds_c5tick.c) uses it for the
 * multiplayer fullview's local view, whose Race_Draw branch (gNumPlayers 2..4) has no decomp
 * markers. */
void* gdx_c5cat_gbi_marker(void* g, int cat) {
    Gfx* _g = (Gfx*)g;
    _g->words.w0 = ((uint32_t)G_NOOP << 24) | (0x61u << 16) | ((uint32_t)cat & 0xFFFFu);
    _g->words.w1 = 0;
    return (void*)(_g + 1);
}
