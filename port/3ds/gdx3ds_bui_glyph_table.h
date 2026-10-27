/* port/3ds/gdx3ds_bui_glyph_table.h — bui_glyph_table() (gdx3ds_bui_kit.h) over the DASH's baked
 * atlases (gdx3ds_hudbake.h, baked at boot from fzerox.o2r). Include it ONCE, in the TU that
 * already includes gdx3ds_hudbake.h and gdx3ds_bottomhud_idle_assets.h (gdx3ds_bottomhud.c on 3DS;
 * the host test on the host), so the logo's static pixel array exists exactly once per binary and
 * the kit reads the very same pixels. */
#ifndef GDX3DS_BUI_GLYPH_TABLE_H
#define GDX3DS_BUI_GLYPH_TABLE_H

#include <stddef.h>

#include "gdx3ds_bui_kit.h"

static const DashGlyph kBuiLogoGlyph[1] = {
    {"logo", DASH_IDLE_LOGO_W, DASH_IDLE_LOGO_H, DASH_IDLE_LOGO_W, kDashIdleLogoPixels},
};

static const DashGlyph* BuiGlyphTableCounted(const DashGlyph* t, int n, int* count) {
    *count = n;
    return t;
}

const DashGlyph* bui_glyph_table(int set, int* count) {
    int unused = 0;
    if (count == NULL) {
        count = &unused;
    }
    switch (set) {
        case BUI_SET_FONT1: return BuiGlyphTableCounted(kDashGlyphFont1, KDASHGLYPHFONT1_COUNT, count);
        case BUI_SET_FONT2: return BuiGlyphTableCounted(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, count);
        case BUI_SET_TIMER: return BuiGlyphTableCounted(kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, count);
        case BUI_SET_POSDIGIT: return BuiGlyphTableCounted(kDashGlyphPosDigit, KDASHGLYPHPOSDIGIT_COUNT, count);
        case BUI_SET_POSSUFFIX: return BuiGlyphTableCounted(kDashGlyphPosSuffix, KDASHGLYPHPOSSUFFIX_COUNT, count);
        case BUI_SET_PORTRAIT: return BuiGlyphTableCounted(kDashGlyphPortrait, KDASHGLYPHPORTRAIT_COUNT, count);
        case BUI_SET_KOSTAR: return BuiGlyphTableCounted(kDashGlyphKoStar, KDASHGLYPHKOSTAR_COUNT, count);
        case BUI_SET_LIVES: return BuiGlyphTableCounted(kDashGlyphLives, KDASHGLYPHLIVES_COUNT, count);
        case BUI_SET_LAP: return BuiGlyphTableCounted(kDashGlyphLap, KDASHGLYPHLAP_COUNT, count);
        case BUI_SET_LOGO: return BuiGlyphTableCounted(kBuiLogoGlyph, 1, count);
        default:
            *count = 0;
            return NULL;
    }
}

#endif /* GDX3DS_BUI_GLYPH_TABLE_H */
