/* port/3ds/gdx3ds_bui_kit.c — the bottom-screen UI kit. Contract in gdx3ds_bui_kit.h.
 *
 * Every function mirrors its engine.js / kit.js twin from the bottom-menus design, including the
 * rounding: the mockups compute in 8-bit RGB (floats for tints and dims) and quantise to RGB565 at
 * each pixel store by truncation, and so does this file (doubles where the JS uses floats, integer
 * math where it uses Math.floor). The host test diffs whole screens against the design's own
 * renders, so a "harmless" rounding change here shows up as a pixel count there. */
#include "gdx3ds_bui_kit.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ---- the surface ------------------------------------------------------------------------- */

static uint16_t* sFb = NULL;
static int sDirtyX0 = BUI_W;
static int sDirtyX1 = -1;

void bui_kit_begin(uint16_t* fb) {
    sFb = fb;
}

uint16_t* bui_kit_target(void) {
    return sFb;
}

void bui_kit_dirty_clear(void) {
    sDirtyX0 = BUI_W;
    sDirtyX1 = -1;
}

int bui_kit_dirty(int* x0, int* x1) {
    if (sDirtyX1 < sDirtyX0) {
        return 0;
    }
    *x0 = sDirtyX0;
    *x1 = sDirtyX1;
    return 1;
}

/* o3ds60 bui_kit_row_track: per column, the rows written (screen y, min and max). */
static int16_t* sRowY0 = NULL;
static int16_t* sRowY1 = NULL;

void bui_kit_row_track(int16_t* y0, int16_t* y1) {
    sRowY0 = y0;
    sRowY1 = y1;
}

static void MarkDirtyRows(int x0, int x1, int y0, int y1) {
    int x;
    if (x0 < sDirtyX0) {
        sDirtyX0 = x0;
    }
    if (x1 > sDirtyX1) {
        sDirtyX1 = x1;
    }
    if (sRowY0 == NULL || sRowY1 == NULL) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 >= BUI_W) {
        x1 = BUI_W - 1;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 >= BUI_H) {
        y1 = BUI_H - 1;
    }
    for (x = x0; x <= x1; x++) {
        if (y0 < sRowY0[x]) {
            sRowY0[x] = (int16_t) y0;
        }
        if (y1 > sRowY1[x]) {
            sRowY1[x] = (int16_t) y1;
        }
    }
}

static inline size_t PixelIndex(int x, int y) {
    return (size_t) x * BUI_H + (size_t) (BUI_H - 1 - y);
}

static inline uint16_t Pack565(int r, int g, int b) {
    return (uint16_t) (((unsigned) (r >> 3) << 11) | ((unsigned) (g >> 2) << 5) | (unsigned) (b >> 3));
}

static inline uint16_t Rgb565(BuiRgb c) {
    return Pack565((int) ((c >> 16) & 255u), (int) ((c >> 8) & 255u), (int) (c & 255u));
}

static inline int ClampByte(int v) {
    if (v < 0) {
        return 0;
    }
    return v > 255 ? 255 : v;
}

static inline void Store565(int x, int y, uint16_t v) {
    if (sFb == NULL || x < 0 || y < 0 || x >= BUI_W || y >= BUI_H) {
        return;
    }
    sFb[PixelIndex(x, y)] = v;
    MarkDirtyRows(x, x, y, y);
}

void bui_set(int x, int y, int r, int g, int b) {
    Store565(x, y, Pack565(ClampByte(r), ClampByte(g), ClampByte(b)));
}

/* The quantised value back, widened exactly as the mockup's framebuffer stores it. */
void bui_get(int x, int y, int* r, int* g, int* b) {
    uint16_t v = 0;
    unsigned R, G, B;
    if (sFb != NULL && x >= 0 && y >= 0 && x < BUI_W && y < BUI_H) {
        v = sFb[PixelIndex(x, y)];
    }
    R = (v >> 11) & 31u;
    G = (v >> 5) & 63u;
    B = v & 31u;
    *r = (int) ((R << 3) | (R >> 2));
    *g = (int) ((G << 2) | (G >> 4));
    *b = (int) ((B << 3) | (B >> 2));
}

/* A rectangle clipped to the screen: one contiguous run per column (column-major). */
typedef uint32_t __attribute__((may_alias)) BuiPair565; /* two framebuffer pixels in one store */

/* n pixels of v from p on: two per 32-bit store once p is word-aligned. */
static void FillColumn565(uint16_t* p, int n, uint16_t v) {
    const uint32_t pair = (uint32_t) v | ((uint32_t) v << 16);
    BuiPair565* q;
    if (n > 0 && ((uintptr_t) p & 2u) != 0) {
        *p++ = v;
        n--;
    }
    q = (BuiPair565*) p;
    for (; n >= 2; n -= 2) {
        *q++ = pair;
    }
    if (n > 0) {
        *(uint16_t*) q = v;
    }
}

static void Fill565(int x, int y, int w, int h, uint16_t v) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > BUI_W ? BUI_W : x + w;
    int y1 = y + h > BUI_H ? BUI_H : y + h;
    int cx;
    if (sFb == NULL || x1 <= x0 || y1 <= y0) {
        return;
    }
    for (cx = x0; cx < x1; cx++) {
        FillColumn565(sFb + PixelIndex(cx, y1 - 1), y1 - y0, v);
    }
    MarkDirtyRows(x0, x1 - 1, y0, y1 - 1);
}

void bui_fill(int x, int y, int w, int h, BuiRgb c) {
    Fill565(x, y, w, h, Rgb565(c));
}

void bui_stroke(int x, int y, int w, int h, BuiRgb c, int t) {
    if (t <= 0) {
        t = 1;
    }
    bui_fill(x, y, w, t, c);
    bui_fill(x, y + h - t, w, t, c);
    bui_fill(x, y, t, h, c);
    bui_fill(x + w - t, y, t, h, c);
}

void bui_dashed(int x, int y, int w, int h, BuiRgb c) {
    const uint16_t v = Rgb565(c);
    int i;
    for (i = x; i < x + w; i++) {
        if ((((i - x) >> 2) % 2) == 0) {
            Store565(i, y, v);
            Store565(i, y + h - 1, v);
        }
    }
    for (i = y; i < y + h; i++) {
        if ((((i - y) >> 2) % 2) == 0) {
            Store565(x, i, v);
            Store565(x + w - 1, i, v);
        }
    }
}

/* One channel of bui_shade: the widened stored value v, blended toward t by k, truncated -- the
 * mockup's double expression, evaluated once per possible value instead of once per pixel. */
static int ShadeChannel(int v, double t, double k) {
    return ClampByte((int) (v * k + t * (1 - k)));
}

/* The whole 565 -> 565 map of one (k, tint), cached: the dialogs all shade alike, so after the
 * first one a shaded pixel is one table load (128 KB, allocated on first use; without it the
 * per-channel tables below do the same arithmetic). */
static uint16_t* sShadeMap = NULL;
static double sShadeMapK = -1;
static BuiRgb sShadeMapTint = 0;
static int sShadeMapFailed = 0;

static void ShadeTables(double k, BuiRgb tint, uint16_t* lutRB, uint16_t* lutG) {
    const double tr = (double) ((tint >> 16) & 255u);
    const double tg = (double) ((tint >> 8) & 255u);
    const double tb = (double) (tint & 255u);
    uint16_t lutR[32], lutB[32];
    unsigned i;
    for (i = 0; i < 32u; i++) {
        const int wide = (int) ((i << 3) | (i >> 2));
        lutR[i] = (uint16_t) ((unsigned) (ShadeChannel(wide, tr, k) >> 3) << 11);
        lutB[i] = (uint16_t) (ShadeChannel(wide, tb, k) >> 3);
    }
    for (i = 0; i < 64u; i++) {
        lutG[i] = (uint16_t) ((unsigned) (ShadeChannel((int) ((i << 2) | (i >> 4)), tg, k) >> 2) << 5);
    }
    for (i = 0; i < 1024u; i++) {
        lutRB[i] = (uint16_t) (lutR[i >> 5] | lutB[i & 31u]); /* index = R5 << 5 | B5 */
    }
}

static unsigned sShadeMapFill = 0; /* entries computed so far (65536 = ready) */
static uint16_t sShadeLutRB[1024];
static uint16_t sShadeLutG[64];

/* Builds up to `budget` more entries of the (k, tint) map; 1 = ready. */
static int ShadeMapStep(double k, BuiRgb tint, unsigned budget) {
    unsigned v, end;
    if (sShadeMap != NULL && sShadeMapK == k && sShadeMapTint == tint && sShadeMapFill >= 65536u) {
        return 1;
    }
    if (sShadeMap == NULL && !sShadeMapFailed) {
        sShadeMap = (uint16_t*) malloc(65536u * sizeof(uint16_t));
        sShadeMapFailed = sShadeMap == NULL;
    }
    if (sShadeMap == NULL) {
        return 0;
    }
    if (sShadeMapK != k || sShadeMapTint != tint) {
        ShadeTables(k, tint, sShadeLutRB, sShadeLutG);
        sShadeMapK = k;
        sShadeMapTint = tint;
        sShadeMapFill = 0;
    }
    end = sShadeMapFill + budget > 65536u ? 65536u : sShadeMapFill + budget;
    for (v = sShadeMapFill; v < end; v++) {
        sShadeMap[v] = (uint16_t) (sShadeLutRB[((v >> 6) & 0x3E0u) | (v & 31u)] | sShadeLutG[(v >> 5) & 63u]);
    }
    sShadeMapFill = end;
    return sShadeMapFill >= 65536u;
}

void bui_shade(int x, int y, int w, int h, double k, BuiRgb tint) {
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = x + w > BUI_W ? BUI_W : x + w;
    const int y1 = y + h > BUI_H ? BUI_H : y + h;
    static uint16_t lutRB[1024];
    uint16_t lutG[64];
    const uint16_t* map;
    int cx;
    if (sFb == NULL || x1 <= x0 || y1 <= y0) {
        return;
    }
    if (ShadeMapStep(k, tint, 0)) {
        map = sShadeMap;
    } else {
        ShadeTables(k, tint, lutRB, lutG);
        map = NULL;
    }
    for (cx = x0; cx < x1; cx++) {
        uint16_t* p = sFb + PixelIndex(cx, y1 - 1);
        int n = y1 - y0;
        if (map != NULL) {
            while (n-- > 0) {
                *p = map[*p];
                p++;
            }
            continue;
        }
        while (n-- > 0) {
            const unsigned v = *p;
            *p++ = (uint16_t) (lutRB[((v >> 6) & 0x3E0u) | (v & 31u)] | lutG[(v >> 5) & 63u]);
        }
    }
    MarkDirtyRows(x0, x1 - 1, y0, y1 - 1);
}

void bui_line(int x0, int y0, int x1, int y1, BuiRgb c, int t) {
    const uint16_t v = Rgb565(c);
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    if (t <= 0) {
        t = 1;
    }
    for (;;) {
        if (t == 1) {
            Store565(x0, y0, v);
        } else {
            Fill565(x0, y0, t, t, v);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            const int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

/* ---- the checker -------------------------------------------------------------------------- */

/* engine.js dim(): ~35% darker, 20% toward its own luma (DashDimBackground's recipe). */
static BuiRgb DimCheckerColour(BuiRgb c) {
    int r = (int) ((c >> 16) & 255u);
    int g = (int) ((c >> 8) & 255u);
    int b = (int) (c & 255u);
    const int luma = (r * 30 + g * 59 + b * 11) / 100;
    r = r * 65 / 100;
    g = g * 65 / 100;
    b = b * 65 / 100;
    r = (r * 80 + luma * 20) / 100;
    g = (g * 80 + luma * 20) / 100;
    b = (b * 80 + luma * 20) / 100;
    return ((BuiRgb) r << 16) | ((BuiRgb) g << 8) | (BuiRgb) b;
}

/* Two precomputed columns (phase 0 and 1); a column of the checker is a copy of one of them. */
static uint16_t sCheckerCol[2][BUI_H];
static int sCheckerReady = 0;

static void CheckerInit(void) {
    const uint16_t a = Rgb565(DimCheckerColour(BUI_TK_CHECK_A));
    const uint16_t b = Rgb565(DimCheckerColour(BUI_TK_CHECK_B));
    int y;
    for (y = 0; y < BUI_H; y++) {
        const int odd = (y >> 3) & 1;
        sCheckerCol[0][BUI_H - 1 - y] = odd ? b : a;
        sCheckerCol[1][BUI_H - 1 - y] = odd ? a : b;
    }
    sCheckerReady = 1;
}

void bui_checker(int x, int y, int w, int h) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > BUI_W ? BUI_W : x + w;
    int y1 = y + h > BUI_H ? BUI_H : y + h;
    int cx;
    if (sFb == NULL || x1 <= x0 || y1 <= y0) {
        return;
    }
    if (!sCheckerReady) {
        CheckerInit();
    }
    for (cx = x0; cx < x1; cx++) {
        const uint16_t* src = sCheckerCol[(cx >> 3) & 1] + (BUI_H - y1);
        memcpy(sFb + PixelIndex(cx, y1 - 1), src, (size_t) (y1 - y0) * sizeof(uint16_t));
    }
    MarkDirtyRows(x0, x1 - 1, y0, y1 - 1);
}

/* ---- glyphs ------------------------------------------------------------------------------- */

/* engine.js c5551(): each 5-bit channel widened with Math.round(v * 255 / 31). */
static const uint8_t kWiden5[32] = {0,   8,   16,  25,  33,  41,  49,  58,  66,  74,  82,
                                    90,  99,  107, 115, 123, 132, 140, 148, 156, 165, 173,
                                    181, 189, 197, 206, 214, 222, 230, 239, 247, 255};

static inline int Widen5(unsigned v) {
    return kWiden5[v & 31u]; /* (v * 255 * 2 + 31) / 62, tabled (no divide on the ARM11) */
}

static const DashGlyph* FindGlyph(int set, const char* key) {
    int count = 0;
    const DashGlyph* t = bui_glyph_table(set, &count);
    int i;
    if (t == NULL) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(t[i].key, key) == 0) {
            return &t[i];
        }
    }
    return NULL;
}

/* Font1 / Font2 by character: a 128-entry index per set, built on first use. */
static int16_t sCharIndex[2][128];
static int sCharIndexReady[2] = {0, 0};

static const DashGlyph* FindCharGlyph(int set, unsigned ch) {
    const int slot = set == BUI_SET_FONT1 ? 0 : 1;
    int count = 0;
    const DashGlyph* t = bui_glyph_table(set, &count);
    if (t == NULL || ch >= 128u) {
        return NULL;
    }
    if (!sCharIndexReady[slot]) {
        int i;
        for (i = 0; i < 128; i++) {
            sCharIndex[slot][i] = -1;
        }
        for (i = 0; i < count; i++) {
            const unsigned char k = (unsigned char) t[i].key[0];
            if (k < 128u && t[i].key[1] == '\0') {
                sCharIndex[slot][k] = (int16_t) i;
            }
        }
        sCharIndexReady[slot] = 1;
    }
    return sCharIndex[slot][ch] < 0 ? NULL : &t[sCharIndex[slot][ch]];
}

typedef struct GlyphPaint {
    int solid;
    int hasTint;
    int tr, tg, tb;
    double dim;
} GlyphPaint;

static void GlyphPaintFrom(GlyphPaint* p, BuiRgb tint, int solid, double dim) {
    p->hasTint = tint != 0;
    p->solid = solid && p->hasTint;
    p->tr = (int) ((tint >> 16) & 255u);
    p->tg = (int) ((tint >> 8) & 255u);
    p->tb = (int) (tint & 255u);
    p->dim = dim;
}

/* One texel through the mockup's colour pipeline: solid -> tint; else the texel, times the tint
 * (/255), times dim; truncated to RGB565 at the store. */
static inline void PaintTexel(int x, int y, int r, int g, int b, const GlyphPaint* p) {
    if (p->solid) {
        r = p->tr;
        g = p->tg;
        b = p->tb;
        if (p->dim > 0) {
            bui_set(x, y, (int) (r * p->dim), (int) (g * p->dim), (int) (b * p->dim));
            return;
        }
        bui_set(x, y, r, g, b);
        return;
    }
    if (p->hasTint) {
        double fr = (double) (r * p->tr) / 255.0;
        double fg = (double) (g * p->tg) / 255.0;
        double fb = (double) (b * p->tb) / 255.0;
        if (p->dim > 0) {
            fr *= p->dim;
            fg *= p->dim;
            fb *= p->dim;
        }
        bui_set(x, y, (int) fr, (int) fg, (int) fb);
        return;
    }
    if (p->dim > 0) {
        bui_set(x, y, (int) (r * p->dim), (int) (g * p->dim), (int) (b * p->dim));
        return;
    }
    bui_set(x, y, r, g, b);
}

/* A texel's 5-bit channels -> the RGB565 field they land in after Widen5 and the store's
 * truncation: red / blue keep their 5 bits, green widens to 6. */
static inline uint16_t Texel565(unsigned short v) {
    /* = R5 | Widen5(G5) >> 2 | B5: the 6-bit green is G5 with its top bit repeated (checked for
     * all 32 values against the table in the host tests) */
    return (uint16_t) ((v & 0xFFC0u) | ((v >> 5) & 0x20u) | ((v >> 1) & 0x1Fu));
}

/* A texel's colour as PaintTexel would store it, per 5-bit channel value: the same expressions
 * (doubles where it uses doubles), evaluated once per (tint, solid, dim) instead of per texel. */
typedef struct TexelLut {
    int ready;
    int solid, hasTint, tr, tg, tb;
    double dim;
    uint16_t r[32], g[32], b[32];
    int rgReady;
    uint16_t rg[1024]; /* r[v >> 5] | g[v & 31] for the texel's top ten bits: one lookup, not two */
} TexelLut;

static TexelLut sTexelLut;

static int TexelChannel(int w, int t, const GlyphPaint* p) {
    if (p->hasTint) {
        double f = (double) (w * t) / 255.0;
        if (p->dim > 0) {
            f *= p->dim;
        }
        return ClampByte((int) f);
    }
    if (p->dim > 0) {
        return ClampByte((int) (w * p->dim));
    }
    return w;
}

static TexelLut* TexelTable(const GlyphPaint* p) {
    TexelLut* l = &sTexelLut;
    unsigned v;
    if (l->ready && l->solid == p->solid && l->hasTint == p->hasTint && l->tr == p->tr && l->tg == p->tg &&
        l->tb == p->tb && l->dim == p->dim) {
        return l;
    }
    for (v = 0; v < 32u; v++) {
        const int w = Widen5(v);
        l->r[v] = (uint16_t) ((unsigned) (TexelChannel(w, p->tr, p) >> 3) << 11);
        l->g[v] = (uint16_t) ((unsigned) (TexelChannel(w, p->tg, p) >> 2) << 5);
        l->b[v] = (uint16_t) (TexelChannel(w, p->tb, p) >> 3);
    }
    l->solid = p->solid;
    l->hasTint = p->hasTint;
    l->tr = p->tr;
    l->tg = p->tg;
    l->tb = p->tb;
    l->dim = p->dim;
    l->ready = 1;
    l->rgReady = 0;
    return l;
}

/* The combined red|green table, built only for blits big enough to repay its 1024 entries. */
static const uint16_t* TexelTableRg(TexelLut* l) {
    unsigned i;
    if (!l->rgReady) {
        for (i = 0; i < 1024u; i++) {
            l->rg[i] = (uint16_t) (l->r[i >> 5] | l->g[i & 31u]);
        }
        l->rgReady = 1;
    }
    return l->rg;
}

enum { BUI_RG_TABLE_MIN_TEXELS = 512 };

/* solid: the tint (dimmed), the same for every texel */
static uint16_t Solid565(const GlyphPaint* p) {
    if (p->dim > 0) {
        return Pack565(ClampByte((int) (p->tr * p->dim)), ClampByte((int) (p->tg * p->dim)),
                       ClampByte((int) (p->tb * p->dim)));
    }
    return Pack565(p->tr, p->tg, p->tb);
}

/* src index of each destination step at scale num/den: floor(i * den / num), by stepping. */
static void ScaleSteps(int16_t* out, int n, int num, int den) {
    int i, q = 0, acc = 0;
    for (i = 0; i < n; i++) {
        out[i] = (int16_t) q;
        acc += den;
        while (acc >= num) {
            acc -= num;
            q++;
        }
    }
}

/* 1:1 columns: the source walked straight down a column (stride = the glyph's width), no row table. */
static void ColumnTexel(const unsigned short* s, int stride, uint16_t* dst, int n) {
    int i;
    for (i = 0; i < n; i++, s += stride, dst--) {
        const unsigned v = *s;
        if (v & 1u) {
            *dst = Texel565((unsigned short) v);
        }
    }
}

static void ColumnSolid(const unsigned short* s, int stride, uint16_t* dst, int n, uint16_t solid565) {
    int i;
    for (i = 0; i < n; i++, s += stride, dst--) {
        if (*s & 1u) {
            *dst = solid565;
        }
    }
}

static void ColumnLut(const unsigned short* s, int stride, uint16_t* dst, int n, const TexelLut* lut) {
    int i;
    for (i = 0; i < n; i++, s += stride, dst--) {
        const unsigned v = *s;
        if (v & 1u) {
            *dst = (uint16_t) (lut->r[(v >> 11) & 31u] | lut->g[(v >> 6) & 31u] | lut->b[(v >> 1) & 31u]);
        }
    }
}

static void ColumnLutRg(const unsigned short* s, int stride, uint16_t* dst, int n, const uint16_t* rg,
                        const uint16_t* b) {
    int i;
    for (i = 0; i < n; i++, s += stride, dst--) {
        const unsigned v = *s;
        if (v & 1u) {
            *dst = (uint16_t) (rg[v >> 6] | b[(v >> 1) & 31u]);
        }
    }
}

static void BlitGlyphOneToOne(const DashGlyph* g, int x, int y, int x0, int x1, int y0, int y1,
                              const GlyphPaint* p, TexelLut* lut) {
    const int n = y1 - y0;
    const int stride = g->w;
    const uint16_t solid565 = p->solid ? Solid565(p) : 0;
    const uint16_t* rg = (lut != NULL && n * (x1 - x0) >= BUI_RG_TABLE_MIN_TEXELS) ? TexelTableRg(lut) : NULL;
    int xx;
    for (xx = x0; xx < x1; xx++) {
        const unsigned short* s = g->pixels + (size_t) y0 * stride + xx;
        uint16_t* dst = sFb + PixelIndex(x + xx, 0) - y - y0; /* dst[-i] = row y0 + i */
        if (p->solid) {
            ColumnSolid(s, stride, dst, n, solid565);
        } else if (rg != NULL) {
            ColumnLutRg(s, stride, dst, n, rg, lut->b);
        } else if (lut != NULL) {
            ColumnLut(s, stride, dst, n, lut);
        } else {
            ColumnTexel(s, stride, dst, n);
        }
    }
}

static void BlitGlyphScaled(const DashGlyph* g, int x, int y, int x0, int x1, int y0, int y1, int num, int den,
                            const GlyphPaint* p, const TexelLut* lut) {
    static int16_t sx[1024], sy[1024];
    static int32_t rowOff[1024];
    const uint16_t solid565 = p->solid ? Solid565(p) : 0;
    const int n = y1 - y0;
    int xx, i;
    ScaleSteps(sx, x1, num, den);
    ScaleSteps(sy, y1, num, den);
    for (i = 0; i < n; i++) {
        rowOff[i] = (int32_t) sy[y0 + i] * g->w;
    }
    for (xx = x0; xx < x1; xx++) {
        const unsigned short* src = g->pixels + sx[xx];
        uint16_t* dst = sFb + PixelIndex(x + xx, 0) - y - y0; /* dst[-i] = row y0 + i */
        if (p->solid) {
            for (i = 0; i < n; i++) {
                if (src[rowOff[i]] & 1u) {
                    dst[-i] = solid565;
                }
            }
        } else if (lut != NULL) {
            for (i = 0; i < n; i++) {
                const unsigned v = src[rowOff[i]];
                if (v & 1u) {
                    dst[-i] = (uint16_t) (lut->r[(v >> 11) & 31u] | lut->g[(v >> 6) & 31u] | lut->b[(v >> 1) & 31u]);
                }
            }
        } else {
            for (i = 0; i < n; i++) {
                const unsigned v = src[rowOff[i]];
                if (v & 1u) {
                    dst[-i] = Texel565((unsigned short) v);
                }
            }
        }
    }
}

static void BlitGlyphFast(const DashGlyph* g, int x, int y, int tw, int th, int num, int den, const GlyphPaint* p) {
    TexelLut* lut = (!p->solid && (p->hasTint || p->dim > 0)) ? TexelTable(p) : NULL;
    const int x0 = x < 0 ? -x : 0;
    const int y0 = y < 0 ? -y : 0;
    const int x1 = x + tw > BUI_W ? BUI_W - x : tw;
    const int y1 = y + th > BUI_H ? BUI_H - y : th;
    if (sFb == NULL || x1 <= x0 || y1 <= y0 || x1 > 1024 || y1 > 1024) {
        return;
    }
    if (num == den) {
        BlitGlyphOneToOne(g, x, y, x0, x1, y0, y1, p, lut);
    } else {
        BlitGlyphScaled(g, x, y, x0, x1, y0, y1, num, den, p, lut);
    }
    MarkDirtyRows(x + x0, x + x1 - 1, y + y0, y + y1 - 1);
}

static int BlitGlyph(const DashGlyph* g, int x, int y, int num, int den, const GlyphPaint* p) {
    BlitGlyphFast(g, x, y, g->w * num / den, g->h * num / den, num, den, p);
    return g->adv * num / den;
}

static int SpriteScale(const BuiSprite* o, int* num, int* den) {
    *num = (o != NULL && o->num > 0) ? o->num : 1;
    *den = (o != NULL && o->den > 0) ? o->den : 1;
    return 1;
}

int bui_sprite(int set, const char* key, int x, int y, const BuiSprite* o) {
    const DashGlyph* g = FindGlyph(set, key);
    GlyphPaint p;
    int num, den;
    if (g == NULL) {
        return 0;
    }
    SpriteScale(o, &num, &den);
    GlyphPaintFrom(&p, o != NULL ? o->tint : 0, o != NULL ? o->solid : 0, o != NULL ? o->dim : 0);
    return BlitGlyph(g, x, y, num, den, &p);
}

int bui_sprite_index(int set, int index, int x, int y, const BuiSprite* o) {
    int count = 0;
    const DashGlyph* t = bui_glyph_table(set, &count);
    GlyphPaint p;
    int num, den;
    if (t == NULL || index < 0 || index >= count) {
        return 0;
    }
    SpriteScale(o, &num, &den);
    GlyphPaintFrom(&p, o != NULL ? o->tint : 0, o != NULL ? o->solid : 0, o != NULL ? o->dim : 0);
    return BlitGlyph(&t[index], x, y, num, den, &p);
}

void bui_logo(int x, int y) {
    bui_sprite_index(BUI_SET_LOGO, 0, x, y, NULL);
}

void bui_icon(int icon, int x, int y, BuiRgb c, int scale) {
    const uint16_t v = Rgb565(c);
    int r;
    if (icon <= BUI_ICON_NONE || icon >= BUI_ICON_COUNT) {
        return;
    }
    if (scale <= 0) {
        scale = 1;
    }
    for (r = 0; r < 16; r++) {
        const uint16_t bits = kBuiIconRows[icon][r];
        int k;
        for (k = 0; k < 16; k++) {
            if (bits & (0x8000u >> k)) {
                Fill565(x + k * scale, y + r * scale, scale, scale, v);
            }
        }
    }
}

/* ---- the extra glyphs --------------------------------------------------------------------- */

static int ExtraIndex(unsigned cp) {
    int i;
    for (i = 0; i < BUI_EXTRA_COUNT; i++) {
        if (kBuiExtraCodepoint[i] == cp) {
            return i;
        }
    }
    return -1;
}

static void ExtraSpan(int idx, int* left, int* right) {
    int r;
    *left = 8;
    *right = 0;
    for (r = 0; r < BUI_EXTRA_ROWS; r++) {
        int k;
        for (k = 0; k < 8; k++) {
            if (kBuiExtraRows[idx][r] & (0x80u >> k)) {
                if (k < *left) {
                    *left = k;
                }
                if (k + 1 > *right) {
                    *right = k + 1;
                }
            }
        }
    }
}

/* Font2's own gradient rows (engine.js F2_TOP / F2_BOT = c5551(40375) / c5551(45929)). */
static void Font2Row(int row, int* r, int* g, int* b) {
    unsigned v = 0;
    if (row < 2) {
        v = 40375u;
    } else if (row >= BUI_EXTRA_ROWS - 2) {
        v = 45929u;
    } else {
        *r = *g = *b = 255;
        return;
    }
    *r = Widen5((v >> 11) & 31u);
    *g = Widen5((v >> 6) & 31u);
    *b = Widen5((v >> 1) & 31u);
}

/* Font1's gradient for the extra glyphs: white top, lilac middle, violet bottom. */
static void Font1Row(int row, int* r, int* g, int* b) {
    if (row < 3) {
        *r = 255;
        *g = 255;
        *b = 255;
    } else if (row < 5) {
        *r = 230;
        *g = 190;
        *b = 255;
    } else {
        *r = 190;
        *g = 110;
        *b = 230;
    }
}

/* Font1 is italic: Math.round((7 - r) * 0.35) per row. */
static const int kFont1Shear[BUI_EXTRA_ROWS] = {2, 2, 2, 1, 1, 1, 0};

static void BlitExtra(int idx, int x, int y, int scale, int isF1, const GlyphPaint* p) {
    int left, right, r;
    ExtraSpan(idx, &left, &right);
    x -= left * scale;
    for (r = 0; r < BUI_EXTRA_ROWS; r++) {
        int cr, cg, cb, k;
        if (isF1) {
            Font1Row(r, &cr, &cg, &cb);
        } else {
            Font2Row(r, &cr, &cg, &cb);
        }
        for (k = 0; k < 8; k++) {
            const int sh = isF1 ? kFont1Shear[r] : 0;
            const int px = x + k * scale + sh * scale;
            const int py = y + r * scale + (isF1 ? 1 : 0);
            int yy;
            if (!(kBuiExtraRows[idx][r] & (0x80u >> k))) {
                continue;
            }
            for (yy = 0; yy < scale; yy++) {
                int xx;
                for (xx = 0; xx < scale; xx++) {
                    PaintTexel(px + xx, py + yy, cr, cg, cb, p);
                }
            }
        }
    }
}

/* ---- text --------------------------------------------------------------------------------- */

typedef struct FontDef {
    int set, num, den, space, ex;
} FontDef;

static const FontDef kFonts[BUI_FONT_COUNT] = {
    {0, 1, 1, 0, 0},                     /* (unused: fonts start at 1) */
    {BUI_SET_FONT1, 1, 1, 7, 2},         /* F1 */
    {BUI_SET_FONT1, 1, 2, 4, 1},         /* F1h */
    {BUI_SET_FONT2, 1, 1, 4, 1},         /* F2 */
    {BUI_SET_FONT2, 2, 1, 8, 2},         /* F2x2 */
    {BUI_SET_TIMER, 1, 1, 6, 2},         /* T */
    {BUI_SET_POSDIGIT, 1, 1, 14, 4},     /* P */
    {BUI_SET_POSDIGIT, 1, 2, 7, 2},      /* Ph */
};

static const FontDef* Font(int font) {
    if (font <= 0 || font >= BUI_FONT_COUNT) {
        font = BUI_F2;
    }
    return &kFonts[font];
}

/* Decodes one UTF-8 code point; malformed bytes come back as themselves. */
static unsigned NextCodepoint(const char** ps) {
    const unsigned char* s = (const unsigned char*) *ps;
    unsigned cp = s[0];
    int extra = 0;
    int i;
    if (cp >= 0xF0u) {
        cp &= 0x07u;
        extra = 3;
    } else if (cp >= 0xE0u) {
        cp &= 0x0Fu;
        extra = 2;
    } else if (cp >= 0xC0u) {
        cp &= 0x1Fu;
        extra = 1;
    }
    for (i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0u) != 0x80u) {
            *ps = (const char*) (s + 1);
            return s[0];
        }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    *ps = (const char*) (s + 1 + extra);
    return cp;
}

/* The font's own glyph for a character, with the capital as the fallback (engine.js glyphKey). */
static const DashGlyph* FontGlyph(const FontDef* f, unsigned cp) {
    const DashGlyph* g;
    char key[2];
    if (cp >= 128u) {
        return NULL;
    }
    if (f->set == BUI_SET_FONT1 || f->set == BUI_SET_FONT2) {
        g = FindCharGlyph(f->set, cp);
        if (g == NULL && cp >= 'a' && cp <= 'z') {
            g = FindCharGlyph(f->set, cp - 'a' + 'A');
        }
        return g;
    }
    key[0] = (char) (cp >= 'a' && cp <= 'z' ? cp - 'a' + 'A' : cp);
    key[1] = '\0';
    return FindGlyph(f->set, key);
}

static int ExtraAdvance(int idx, int font) {
    const FontDef* f = Font(font);
    const int sc = font == BUI_F1 ? 2 : f->ex;
    int left, right, w;
    ExtraSpan(idx, &left, &right);
    w = (right - left + 1) * sc + (font == BUI_F1 ? 1 : 0);
    return w > 2 * sc ? w : 2 * sc;
}

int bui_measure(int font, const char* s) {
    const FontDef* f = Font(font);
    int w = 0;
    if (s == NULL) {
        return 0;
    }
    while (*s != '\0') {
        const unsigned cp = NextCodepoint(&s);
        const DashGlyph* g;
        int idx;
        if (cp == ' ') {
            w += f->space;
            continue;
        }
        g = FontGlyph(f, cp);
        if (g != NULL) {
            w += g->adv * f->num / f->den;
            continue;
        }
        idx = ExtraIndex(cp);
        w += idx >= 0 ? ExtraAdvance(idx, font) : f->space;
    }
    return w;
}

static int sUnknownGlyphs = 0;

int bui_kit_unknown_glyphs(void) {
    return sUnknownGlyphs;
}

void bui_kit_reset_unknown_glyphs(void) {
    sUnknownGlyphs = 0;
}

int bui_text(int font, const char* s, double x, int y, const BuiText* o) {
    const FontDef* f = Font(font);
    GlyphPaint p;
    int cx, x0;
    if (s == NULL) {
        return 0;
    }
    if (o != NULL && o->align == BUI_ALIGN_CENTER) {
        x = floor(x - bui_measure(font, s) / 2.0 + 0.5);
    } else if (o != NULL && o->align == BUI_ALIGN_RIGHT) {
        x = x - bui_measure(font, s);
    }
    x0 = cx = (int) floor(x);
    GlyphPaintFrom(&p, o != NULL ? o->tint : 0, o != NULL ? o->solid : 0, o != NULL ? o->dim : 0);
    while (*s != '\0') {
        const unsigned cp = NextCodepoint(&s);
        const DashGlyph* g;
        int idx;
        if (cp == ' ') {
            cx += f->space;
            continue;
        }
        g = FontGlyph(f, cp);
        if (g != NULL) {
            cx += BlitGlyph(g, cx, y, f->num, f->den, &p);
            continue;
        }
        idx = ExtraIndex(cp);
        if (idx >= 0) {
            BlitExtra(idx, cx, y, font == BUI_F1 ? 2 : f->ex, font == BUI_F1, &p);
            cx += ExtraAdvance(idx, font);
        } else {
            sUnknownGlyphs++;
            cx += f->space;
        }
    }
    return cx - x0;
}

/* ---- components --------------------------------------------------------------------------- */

static int Round(double v) {
    return (int) floor(v + 0.5);
}

static BuiText SolidText(BuiRgb tint) {
    BuiText t;
    memset(&t, 0, sizeof(t));
    t.tint = tint;
    t.solid = 1;
    return t;
}

static BuiHintSink sHintSink = NULL;
static void* sHintCtx = NULL;
static int sGlyphsOff = 0;

void bui_kit_set_hint_sink(BuiHintSink sink, void* ctx) {
    sHintSink = sink;
    sHintCtx = ctx;
}

void bui_kit_note_hint(const char* key, int x, int y) {
    if (sHintSink != NULL && !sGlyphsOff) {
        sHintSink(key, x, y, sHintCtx);
    }
}

void bui_kit_set_glyphs_off(int off) {
    sGlyphsOff = off ? 1 : 0;
}

int bui_kit_glyphs_off(void) {
    return sGlyphsOff;
}

int bui_btn_glyph(int x, int y, const char* letter, BuiRgb color, BuiRgb fg) {
    static const int kPts[12][3] = {
        {3, 0, 6}, {1, 1, 10}, {0, 2, 12}, {0, 3, 12}, {0, 4, 12}, {0, 5, 12},
        {0, 6, 12}, {0, 7, 12}, {0, 8, 12}, {0, 9, 12}, {1, 10, 10}, {3, 11, 6},
    };
    const BuiRgb c = color != 0 ? color : 0xE8DCE2u;
    const BuiText t = SolidText(fg != 0 ? fg : BUI_TK_DARK);
    const int len = (int) strlen(letter);
    const int wide = len > 1 || strcmp(letter, "L") == 0 || strcmp(letter, "R") == 0;
    int i;
    if (sGlyphsOff) {
        return 0;
    }
    bui_kit_note_hint(letter, x, y);
    if (wide) {
        const int w = len > 1 ? 20 : 14;
        bui_fill(x, y + 1, w, 10, c);
        bui_fill(x + 1, y, w - 2, 12, c);
        bui_text(BUI_F2, letter, x + Round((w - bui_measure(BUI_F2, letter)) / 2.0), y + 2, &t);
        return w;
    }
    for (i = 0; i < 12; i++) {
        bui_fill(x + kPts[i][0], y + kPts[i][1], kPts[i][2], 1, c);
    }
    bui_text(BUI_F2, letter, x + Round((12 - bui_measure(BUI_F2, letter)) / 2.0), y + 2, &t);
    return 12;
}

typedef struct BadgeStyle {
    BuiRgb fill, text, line;
} BadgeStyle;

static const BadgeStyle kBadge[BUI_BADGE_KIND_COUNT] = {
    {BUI_TK_CYAN, BUI_TK_DARK, 0},          /* info */
    {0x062A2Eu, BUI_TK_CYAN, BUI_TK_CYAN},  /* cyanline */
    {BUI_TK_GREEN, BUI_TK_DARK, 0},         /* ok */
    {BUI_TK_ORANGE, BUI_TK_DARK, 0},        /* warn */
    {BUI_TK_RED, BUI_TK_WHITE, 0},          /* bad */
    {0x3A2A34u, 0xC0B0B8u, 0},              /* muted */
    {BUI_TK_MAGENTA, BUI_TK_WHITE, 0},      /* ek */
    {0x2A1E26u, BUI_TK_WHITE, 0x7A6A72u},   /* model */
    {BUI_TK_PINK, BUI_TK_DARK, 0},          /* pink */
    {BUI_TK_FOCUS, BUI_TK_DARK, 0},         /* yellow */
};

int bui_badge_width(const char* t) {
    return bui_measure(BUI_F2, t) + 6;
}

int bui_badge(int x, int y, const char* t, int kind) {
    const BadgeStyle* s = &kBadge[(kind >= 0 && kind < BUI_BADGE_KIND_COUNT) ? kind : BUI_BADGE_MUTED];
    const int w = bui_badge_width(t);
    const BuiText tx = SolidText(s->text);
    bui_fill(x, y, w, 12, s->fill);
    if (s->line != 0) {
        bui_stroke(x, y, w, 12, s->line, 1);
    }
    bui_text(BUI_F2, t, x + 3, y + 2, &tx);
    return w;
}

void bui_bars(int x, int y, int n) {
    const BuiRgb col = n >= 3 ? BUI_TK_GREEN : n == 2 ? BUI_TK_FOCUS : n == 1 ? BUI_TK_ORANGE : BUI_TK_RED;
    int i;
    for (i = 0; i < 3; i++) {
        const int h = 4 + i * 3;
        bui_fill(x + i * 5, y + 10 - h, 3, h, i < n ? col : BUI_TK_BARS_OFF);
    }
}

void bui_spinner(int cx, int cy, int phase) {
    /* Math.round(cos / sin(i * PI / 4) * 6) */
    static const int kDx[8] = {6, 4, 0, -4, -6, -4, 0, 4};
    static const int kDy[8] = {0, 4, 6, 4, 0, -4, -6, -4};
    int i;
    for (i = 0; i < 8; i++) {
        const int k = ((i - phase) % 8 + 8) % 8;
        const BuiRgb c = k == 0 ? BUI_TK_WHITE : k == 1 ? BUI_TK_PINK : k == 2 ? 0x9A3A6Au : 0x4A2A3Au;
        bui_fill(cx + kDx[i] - 1, cy + kDy[i] - 1, 3, 3, c);
    }
}

void bui_header(const char* title, const BuiHeader* o) {
    const int font = (o != NULL && o->font != 0) ? o->font : BUI_F1;
    int tx = 8;
    bui_fill(0, 0, 320, 24, BUI_TK_HEADER);
    bui_fill(0, 23, 320, 1, BUI_TK_PINK);
    if (o == NULL || !o->noBack) {
        const BuiText w = SolidText(BUI_TK_WHITE);
        bui_fill(0, 0, 44, 23, BUI_TK_PANEL);
        bui_fill(44, 0, 1, 23, BUI_TK_PINK);
        bui_text(BUI_F2, "◀", sGlyphsOff ? 16 : 7, 8, &w);
        bui_btn_glyph(22, 6, "B", 0, 0);
        tx = 52;
    }
    if (o != NULL && o->icon != 0) {
        bui_icon(o->icon, tx, 4, o->iconColor != 0 ? o->iconColor : BUI_TK_CYAN, 1);
        tx += 20;
    }
    bui_text(font, title, tx, font == BUI_F2 ? 8 : 4, NULL);
    if (o != NULL && o->right != NULL) {
        int rx = 314;
        int i;
        for (i = o->rightCount - 1; i >= 0; i--) {
            const BuiHeaderItem* b = &o->right[i];
            if (b->type == BUI_HDR_BARS) {
                rx -= 16;
                bui_bars(rx, 7, b->value);
                rx -= 4;
            } else if (b->type == BUI_HDR_SPIN) {
                rx -= 18;
                bui_spinner(rx + 8, 12, b->value);
                rx -= 4;
            } else {
                rx -= bui_badge_width(b->text);
                bui_badge(rx, 6, b->text, b->kind);
                rx -= 4;
            }
        }
    }
}

void bui_footer(const BuiHint* hints, int n, const char* right) {
    BuiText lbl;
    int x = 6;
    int i;
    memset(&lbl, 0, sizeof(lbl));
    lbl.tint = BUI_TK_TEXT2;
    bui_fill(0, 224, 320, 16, BUI_TK_HEADER);
    bui_fill(0, 224, 320, 1, BUI_TK_LINE);
    for (i = 0; i < n && !sGlyphsOff; i++) {
        const char* k = hints[i].keys;
        while (*k != '\0') {
            char one[4];
            int len = 0;
            while (k[len] != '\0' && k[len] != '/' && len < 3) {
                one[len] = k[len];
                len++;
            }
            one[len] = '\0';
            x += bui_btn_glyph(x, 226, one, 0, 0) + 2;
            k += len;
            if (*k == '/') {
                k++;
            }
        }
        x += 2;
        x += bui_text(BUI_F2, hints[i].label, x, 228, &lbl) + 12;
    }
    if (right != NULL) {
        BuiText r;
        memset(&r, 0, sizeof(r));
        r.tint = BUI_TK_MUTE;
        r.align = BUI_ALIGN_RIGHT;
        bui_text(BUI_F2, right, 314, 228, &r);
    }
}

/* Hold-to-confirm: the part of the button held so far, filled from the left in a lighter tone of
 * the button's own colour (the label stays readable over both). */
static void ButtonMeter(int x, int y, int w, int h, int state, double frac) {
    const BuiRgb c = state == BUI_BTN_DANGER ? 0xC81828u : (state == BUI_BTN_PRIMARY || state == BUI_BTN_PRESSED)
                                                               ? 0xFFD8EBu
                                                               : 0x6A3A58u;
    const int fw = (int) ((w - 2) * (frac > 1 ? 1 : frac) + 0.5);
    if (fw > 0) {
        bui_fill(x + 1, y + 1, fw, h - 2, c);
    }
}

void bui_button(int x, int y, int w, int h, const char* label, const BuiButton* o) {
    static const BuiButton kNone = {0};
    const BuiButton* b = o != NULL ? o : &kNone;
    const int st = b->state;
    BuiRgb fill = BUI_TK_PANEL;
    BuiRgb line = BUI_TK_PINK;
    BuiRgb tint = 0;
    int solid = 0;
    double dimk = 0;
    const int font = b->font != 0 ? b->font : (h >= 32 ? BUI_F1 : BUI_F2);
    const int fh = (font == BUI_F1 || font == BUI_F2X2) ? 16 : 8;
    const char* glyph = sGlyphsOff ? NULL : b->glyph;
    const int gw = glyph != NULL ? ((int) strlen(glyph) > 1 ? 20 : 12) + 4 : 0;
    int iw = b->icon != 0 ? 20 : 0;
    int lw, lx, ly;
    BuiText t;
    if (st == BUI_BTN_PRIMARY || st == BUI_BTN_PRESSED) {
        fill = BUI_TK_PINK;
        line = BUI_TK_PINK;
        tint = BUI_TK_DARK;
        solid = 1;
    } else if (st == BUI_BTN_GO) {
        fill = BUI_TK_OK_FILL;
        line = BUI_TK_GREEN;
    } else if (st == BUI_BTN_DANGER) {
        fill = BUI_TK_BAD_FILL;
        line = BUI_TK_RED;
    } else if (st == BUI_BTN_DISABLED) {
        fill = BUI_TK_OFF_FILL;
        line = BUI_TK_OFF_LINE;
        dimk = 0.42;
    } else if (st == BUI_BTN_GHOST) {
        fill = BUI_TK_DEEP;
        line = BUI_TK_LINE;
    }
    bui_fill(x, y, w, h, fill);
    if (b->meter > 0) {
        ButtonMeter(x, y, w, h, st, b->meter);
    }
    bui_stroke(x, y, w, h, line, 1);
    if (b->focus) {
        bui_stroke(x - 2, y - 2, w + 4, h + 4, BUI_TK_FOCUS, 2);
    }
    if (glyph != NULL) {
        iw = gw;
    }
    lw = bui_measure(font, label) + iw;
    lx = b->alignLeft ? x + 8 : x + Round((w - lw) / 2.0);
    ly = b->sub != NULL ? y + 5 : y + Round((h - fh) / 2.0);
    if (b->icon != 0) {
        const BuiRgb ic = solid ? tint : (st == BUI_BTN_DISABLED ? BUI_TK_OFF_LINE : (b->iconColor != 0 ? b->iconColor : BUI_TK_CYAN));
        bui_icon(b->icon, lx, ly + (fh - 16) / 2, ic, 1);
        lx += 20;
    }
    if (glyph != NULL) {
        bui_btn_glyph(lx, ly + (fh - 12) / 2, glyph, solid ? BUI_TK_DARK : 0, solid ? BUI_TK_PINK : 0);
        lx += gw;
    }
    memset(&t, 0, sizeof(t));
    t.tint = solid ? tint : b->tint;
    t.solid = solid;
    t.dim = dimk;
    bui_text(font, label, lx, ly, &t);
    if (b->sub != NULL) {
        BuiText s;
        memset(&s, 0, sizeof(s));
        s.tint = solid ? BUI_TK_DARK : (b->subTint != 0 ? b->subTint : BUI_TK_TEXT2);
        s.solid = 1;
        s.dim = dimk;
        s.align = BUI_ALIGN_CENTER;
        bui_text(BUI_F2, b->sub, x + w / 2.0, y + h - 12, &s);
    }
}

/* Portraits are keyed by character id ("0".."29"), as the mockups look them up. */
static void DrawPortrait(int character, int x, int y, const BuiSprite* sp) {
    char key[4];
    int n = 0;
    if (character < 0 || character > 99) {
        return;
    }
    if (character >= 10) {
        key[n++] = (char) ('0' + character / 10);
    }
    key[n++] = (char) ('0' + character % 10);
    key[n] = '\0';
    bui_sprite(BUI_SET_PORTRAIT, key, x, y, sp);
}

void bui_row(int x, int y, int w, int h, const BuiRow* o) {
    const double dimk = o->disabled ? 0.45 : 0;
    int tx = x + 8;
    int rx;
    BuiText t;
    bui_fill(x, y, w, h, o->fill != 0 ? o->fill : BUI_TK_DEEP);
    bui_stroke(x, y, w, h, o->line != 0 ? o->line : BUI_TK_LINE, 1);
    if (o->focus) {
        bui_stroke(x - 1, y - 1, w + 2, h + 2, BUI_TK_FOCUS, 2);
    }
    if (o->portrait != 0) {
        BuiSprite sp;
        memset(&sp, 0, sizeof(sp));
        sp.dim = dimk;
        DrawPortrait(o->portrait - 1, x + 3, y + Round((h - 32) / 2.0), &sp);
        tx = x + 40;
    }
    if (o->icon != 0) {
        bui_fill(x + 3, y + Round((h - 32) / 2.0), 32, 32, o->iconFill != 0 ? o->iconFill : BUI_TK_PANEL);
        bui_icon(o->icon, x + 11, y + Round((h - 16) / 2.0), o->iconColor != 0 ? o->iconColor : BUI_TK_CYAN, 1);
        tx = x + 40;
    }
    memset(&t, 0, sizeof(t));
    t.dim = dimk;
    t.tint = o->titleTint;
    bui_text(o->titleFont != 0 ? o->titleFont : BUI_F1, o->title, tx, y + (o->meta != NULL ? 4 : Round((h - 16) / 2.0)), &t);
    if (o->meta != NULL) {
        BuiText m;
        memset(&m, 0, sizeof(m));
        m.tint = o->metaTint != 0 ? o->metaTint : BUI_TK_TEXT2;
        m.solid = 1;
        m.dim = dimk;
        bui_text(BUI_F2, o->meta, tx, y + h - 13, &m);
    }
    rx = x + w - 5 - (o->chev ? 14 : 0);
    if (o->badges != NULL) {
        int i;
        for (i = o->badgeCount - 1; i >= 0; i--) {
            rx -= bui_badge_width(o->badges[i].text);
            bui_badge(rx, y + 4, o->badges[i].text, o->badges[i].kind);
            rx -= 3;
        }
    }
    if (o->bars != 0) {
        bui_bars(x + w - 19, y + h - 15, o->bars - 1);
    }
    if (o->chev) {
        const BuiText c = SolidText(BUI_TK_PINK);
        bui_text(BUI_F2, "▶", x + w - 12, y + Round((h - 8) / 2.0), &c);
    }
}

typedef struct SlotBadge {
    const char* text;
    int kind;
} SlotBadge;

void bui_slot(int x, int y, int w, int h, const BuiSlot* p) {
    static const SlotBadge kStates[] = {
        {NULL, 0},
        {"READY✓", BUI_BADGE_OK},
        {"WAIT", BUI_BADGE_MUTED},
        {"VERSION", BUI_BADGE_BAD},
        {"AWAY", BUI_BADGE_WARN},
        {"CPU", BUI_BADGE_MUTED},
        {"NO DISK", BUI_BADGE_BAD},
        {NULL, BUI_BADGE_INFO}, /* getting: "<pct>%" */
        {"LOADING", BUI_BADGE_INFO},
        {"LOADED✓", BUI_BADGE_OK},
        {"HOST", BUI_BADGE_PINK},
    };
    const int nx = x + 40;
    BuiSprite sp;
    BuiText t;
    if (p->empty) {
        const BuiText m = {BUI_TK_MUTE, 1, 0, BUI_ALIGN_CENTER};
        bui_fill(x, y, w, h, BUI_TK_SLOT_EMPTY);
        bui_dashed(x, y, w, h, BUI_TK_DASH_LINE);
        bui_text(BUI_F2, p->label != NULL ? p->label : "OPEN", x + w / 2.0, y + Round(h / 2.0) - 4, &m);
        return;
    }
    bui_fill(x, y, w, h, p->me ? BUI_TK_ME_FILL : BUI_TK_DEEP);
    bui_stroke(x, y, w, h, p->me ? BUI_TK_FOCUS : BUI_TK_LINE, 1);
    memset(&sp, 0, sizeof(sp));
    sp.dim = p->away ? 0.45 : 0;
    DrawPortrait(p->pilot, x + 3, y + Round((h - 32) / 2.0), &sp);
    memset(&t, 0, sizeof(t));
    t.dim = p->away ? 0.5 : 0;
    bui_text(BUI_F1, p->name, nx, y + 3, &t);
    if (p->host) {
        bui_icon(BUI_ICON_CROWN, nx + bui_measure(BUI_F1, p->name) + 4, y + 4, BUI_TK_FOCUS, 1);
    }
    if (p->sub != NULL) {
        const BuiText s = SolidText(p->subTint != 0 ? p->subTint : BUI_TK_TEXT2);
        bui_text(BUI_F2, p->sub, nx, y + h - 12, &s);
    }
    if (p->state > BUI_SLOT_NONE && p->state <= BUI_SLOT_HOST) {
        char pct[8];
        const char* label = kStates[p->state].text;
        if (p->state == BUI_SLOT_GETTING) {
            int v = p->pct < 0 ? 0 : (p->pct > 100 ? 100 : p->pct);
            int n = 0;
            if (v >= 100) {
                pct[n++] = '1';
                pct[n++] = '0';
                pct[n++] = '0';
            } else {
                if (v >= 10) {
                    pct[n++] = (char) ('0' + v / 10);
                }
                pct[n++] = (char) ('0' + v % 10);
            }
            pct[n++] = '%';
            pct[n] = '\0';
            label = pct;
        }
        bui_badge(x + w - 5 - bui_badge_width(label), y + h - 16, label, kStates[p->state].kind);
    }
    if (p->you) {
        bui_badge(x + w - 5 - bui_badge_width("YOU"), y + 4, "YOU", BUI_BADGE_YELLOW);
    } else if (p->bars != 0) {
        bui_bars(x + w - 20, y + 4, p->bars - 1);
    }
}

static BuiRgb ToneColour(int tone) {
    switch (tone) {
        case BUI_TONE_BAD: return BUI_TK_RED;
        case BUI_TONE_WARN: return BUI_TK_ORANGE;
        case BUI_TONE_OK: return BUI_TK_GREEN;
        case BUI_TONE_PINK: return BUI_TK_PINK;
        default: return BUI_TK_CYAN;
    }
}

void bui_toast(int y, const char* t, int tone, int icon) {
    const BuiRgb col = tone == BUI_TONE_PINK ? BUI_TK_CYAN : ToneColour(tone); /* kit.js: no pink toast */
    BuiText w;
    memset(&w, 0, sizeof(w));
    w.tint = BUI_TK_WHITE;
    bui_fill(6, y, 308, 24, BUI_TK_HEADER);
    bui_stroke(6, y, 308, 24, col, 1);
    bui_fill(6, y, 4, 24, col);
    if (icon != 0) {
        bui_icon(icon, 14, y + 4, col, 1);
    }
    bui_text(BUI_F2, t, icon != 0 ? 34 : 16, y + 8, &w);
}

BuiRect bui_modal_layout(const BuiModal* o, BuiRect* buttonRects) {
    const int bh = o->buttonCount > 0 ? 40 : 0;
    const int h = 30 + o->bodyCount * 12 + 10 + bh + (bh ? 8 : 0) + o->extraH + (o->code != NULL ? 8 : 0);
    const int w = o->w > 0 ? o->w : 288;
    BuiRect r;
    r.w = w;
    r.h = h;
    r.x = (320 - w) >> 1;
    r.y = (240 - h) >> 1;
    if (r.y < 4) {
        r.y = 4;
    }
    if (buttonRects != NULL && bh) {
        const int n = o->buttonCount;
        const int gap = 6;
        const int bw = (w - 16 - gap * (n - 1)) / n;
        int i;
        for (i = 0; i < n; i++) {
            buttonRects[i].x = r.x + 8 + i * (bw + gap);
            buttonRects[i].y = r.y + h - 48;
            buttonRects[i].w = bw;
            buttonRects[i].h = 40;
        }
    }
    return r;
}

#define BUI_MODAL_SHADE_K 0.35
#define BUI_MODAL_SHADE_TINT 0x100008u

int bui_modal_prepare(int budget) {
    return ShadeMapStep(BUI_MODAL_SHADE_K, BUI_MODAL_SHADE_TINT, budget > 0 ? (unsigned) budget : 0u);
}

BuiRect bui_modal(const BuiModal* o, BuiRect* buttonRects) {
    BuiRect btn[3];
    const BuiRect r = bui_modal_layout(o, btn);
    const BuiRgb col = (o->tone == BUI_TONE_BAD || o->tone == BUI_TONE_WARN || o->tone == BUI_TONE_OK)
                           ? ToneColour(o->tone)
                           : BUI_TK_PINK;
    const int bh = o->buttonCount > 0 ? 40 : 0;
    int tx = r.x + 10;
    int i;
    if (!o->noShade) {
        bui_shade(0, 0, 320, 240, BUI_MODAL_SHADE_K, BUI_MODAL_SHADE_TINT);
    }
    bui_fill(r.x, r.y, r.w, r.h, BUI_TK_PANEL);
    bui_stroke(r.x, r.y, r.w, r.h, col, 2);
    bui_fill(r.x, r.y, r.w, 26, BUI_TK_HEADER);
    bui_fill(r.x, r.y + 26, r.w, 1, col);
    if (o->icon != 0) {
        bui_icon(o->icon, r.x + 8, r.y + 5, col, 1);
        tx = r.x + 30;
    }
    bui_text(BUI_F1, o->title, tx, r.y + 5, NULL);
    if (o->code != NULL) {
        bui_badge(r.x + r.w - 8 - bui_badge_width(o->code), r.y + r.h - (bh ? 48 : 8) - 15, o->code, BUI_BADGE_MUTED);
    }
    for (i = 0; i < o->bodyCount; i++) {
        BuiText t;
        memset(&t, 0, sizeof(t));
        t.tint = (i == 0 && o->leadTint != 0) ? o->leadTint : BUI_TK_WHITE;
        bui_text(BUI_F2, o->body[i], r.x + 12, r.y + 34 + i * 12, &t);
    }
    if (o->progress > 0 && o->extraH > 0) {
        bui_progress(r.x + 12, r.y + 34 + o->bodyCount * 12 + 6, r.w - 24, 10, o->progress, BUI_TK_CYAN);
    }
    for (i = 0; i < o->buttonCount && i < 3; i++) {
        BuiButton b;
        memset(&b, 0, sizeof(b));
        b.state = o->buttons[i].state;
        b.glyph = o->buttons[i].glyph;
        b.focus = o->buttons[i].focus;
        b.font = o->buttons[i].font != 0 ? o->buttons[i].font : (o->buttonCount > 2 ? BUI_F2 : 0);
        b.sub = o->buttons[i].sub;
        b.meter = o->buttons[i].meter;
        bui_button(btn[i].x, btn[i].y, btn[i].w, btn[i].h, o->buttons[i].text, &b);
        if (buttonRects != NULL) {
            buttonRects[i] = btn[i];
        }
    }
    return r;
}

BuiRect bui_seg_rect(int x, int y, int w, int h, int n, int i) {
    const int sw = w / n;
    BuiRect r;
    r.x = x + i * sw;
    r.y = y;
    r.w = i == n - 1 ? w - sw * (n - 1) : sw;
    r.h = h;
    return r;
}

void bui_seg(int x, int y, int w, int h, const char* const* opts, int n, int sel, const BuiSeg* o) {
    const int font = (o != NULL && o->font != 0) ? o->font : BUI_F1;
    const int fh = font == BUI_F2 ? 8 : 16;
    int i;
    for (i = 0; i < n; i++) {
        const BuiRect r = bui_seg_rect(x, y, w, h, n, i);
        const int off = o != NULL && (o->offMask & (1 << i)) != 0;
        const int on = i == sel;
        const int lw = bui_measure(font, opts[i]) + (off ? 12 : 0);
        const int lx = r.x + Round((r.w - lw) / 2.0);
        BuiText t;
        bui_fill(r.x, r.y, r.w, r.h, on ? BUI_TK_PINK : off ? BUI_TK_OFF_FILL : BUI_TK_DEEP);
        bui_stroke(r.x, r.y, r.w, r.h, on ? BUI_TK_PINK : off ? BUI_TK_OFF_LINE : BUI_TK_LINE, 1);
        memset(&t, 0, sizeof(t));
        if (on) {
            t.tint = BUI_TK_DARK;
            t.solid = 1;
        } else if (off) {
            t.dim = 0.4;
        }
        bui_text(font, opts[i], lx, y + Round((h - fh) / 2.0), &t);
        if (off) {
            bui_icon(BUI_ICON_LOCK, lx + lw - 10, y + Round((h - 12) / 2.0) - 1, BUI_TK_OFF_LINE, 1);
        }
    }
    if (o != NULL && o->focus) {
        bui_stroke(x - 2, y - 2, w + 4, h + 4, BUI_TK_FOCUS, 2);
    }
}

void bui_stepper(int x, int y, int w, int h, const char* label, const char* value, const BuiStepper* o) {
    const int font = (o != NULL && o->font != 0) ? o->font : BUI_F1;
    const int ax = x + w - 150;
    const BuiText lbl = SolidText(BUI_TK_TEXT2);
    const BuiText arrow = SolidText(BUI_TK_WHITE);
    BuiText v;
    bui_fill(x, y, w, h, BUI_TK_DEEP);
    bui_stroke(x, y, w, h, BUI_TK_LINE, 1);
    if (o != NULL && o->focus) {
        bui_stroke(x - 2, y - 2, w + 4, h + 4, BUI_TK_FOCUS, 2);
    }
    bui_text(BUI_F2, label, x + 8, y + Round((h - 8) / 2.0), &lbl);
    bui_fill(ax, y + 2, 36, h - 4, BUI_TK_PANEL);
    bui_stroke(ax, y + 2, 36, h - 4, BUI_TK_PINK, 1);
    bui_text(BUI_F2, "◀", ax + 14, y + Round((h - 8) / 2.0), &arrow);
    bui_fill(x + w - 38, y + 2, 36, h - 4, BUI_TK_PANEL);
    bui_stroke(x + w - 38, y + 2, 36, h - 4, BUI_TK_PINK, 1);
    bui_text(BUI_F2, "▶", x + w - 24, y + Round((h - 8) / 2.0), &arrow);
    memset(&v, 0, sizeof(v));
    v.align = BUI_ALIGN_CENTER;
    v.tint = o != NULL ? o->tint : 0;
    bui_text(font, value, x + w - 76, y + Round((h - (font == BUI_F2 ? 8 : 16)) / 2.0), &v);
}

void bui_progress(int x, int y, int w, int h, double frac, BuiRgb colour) {
    const BuiRgb col = colour != 0 ? colour : BUI_TK_GREEN;
    const int cr = (int) ((col >> 16) & 255u);
    const int cg = (int) ((col >> 8) & 255u);
    const int cb = (int) (col & 255u);
    const int fw = Round((w - 2) * frac);
    int r;
    bui_fill(x, y, w, h, BUI_TK_BLACK);
    bui_stroke(x, y, w, h, BUI_TK_LINE, 1);
    for (r = 0; r < h - 2; r++) {
        const double k = 1 - fabs(r - (h - 3) / 2.0) / ((h - 2) / 1.4);
        const uint16_t v = Pack565(ClampByte((int) (cr * k)), ClampByte((int) (cg * k)), ClampByte((int) (cb * k)));
        Fill565(x + 1, y + 1 + r, fw, 1, v);
    }
}
