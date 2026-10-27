/* port/3ds/gdx3ds_hudbake.c -- see gdx3ds_hudbake.h. The DASH's game art as recipes over the
 * user's own fzerox.o2r, run once at boot.
 *
 * Exactness: the chrome widgets reproduce, pixel for pixel, the template layers
 * tools/gen_nibottom_template.py drew with Pillow (and tools/nibottom_template_import.py
 * quantised). Every Pillow primitive used there is mirrored here with the same arithmetic:
 *   - Image.resize(NEAREST): ImagingScaleAffine, source coordinate accumulated in double from
 *     (in/out)*0.5 in steps of in/out, truncated;
 *   - Image.alpha_composite: ImagingAlphaComposite's 7-bit-precision integer blend;
 *   - ImageDraw.rectangle: (int)-truncated corners, both ends inclusive, fill then 1px outline,
 *     clipped to the 320x240 canvas;
 *   - Python's round() (half to even) for glyph positions, int() (truncation) for sizes.
 * The double arithmetic must round after every operation (no fused multiply-add): the build
 * compiles this TU with -ffp-contract=off. */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include "gdx3ds_hudbake.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- glyph tables: metadata here, pixels baked ------------------------------------------- */

#define HB_FONT_GLYPHS 38 /* Font1 and Font2: A-Z, 0-9, ".", "-" */
#define HB_FONT1_SLOT (16 * 16)
#define HB_FONT2_SLOT (8 * 8)
#define HB_PORTRAIT_PX (32 * 32)

/* Every baked pixel lives in ONE block (the "arena"), allocated on the first bake or cache load
 * through the allocator hook (gdx3ds_hudbake_set_allocator: the console's linear heap), laid out
 * part after part in kHbParts order. That order is also the SD cache's blob, so a cache load
 * decodes straight into the arena and a cache write encodes straight out of it. Until then the
 * glyph tables point at sHbBlankGlyph and the backdrop/widgets are NULL (draw nothing). */
enum {
    HB_PART_TIMER,
    HB_PART_LAP,
    HB_PART_POS_DIGIT,
    HB_PART_POS_SUFFIX,
    HB_PART_FONT1,
    HB_PART_FONT2,
    HB_PART_PORTRAIT,
    HB_PART_KO_STAR,
    HB_PART_LIVES,
    HB_PART_SPEED_DIGIT,
    HB_PART_KMH,
    HB_PART_BACKGROUND,
    HB_PART_W_HEADER,
    HB_PART_W_STANDINGS,
    HB_PART_W_LAP,
    HB_PART_W_TIMER,
    HB_PART_W_GAP,
    HB_PART_W_ENERGY_BAR,
    HB_PART_COUNT
};

static const size_t kHbPartWords[HB_PART_COUNT] = {
    14 * 8 * 16,         6 * 8 * 12, 10 * 28 * 32,   4 * 20 * 20,     38 * HB_FONT1_SLOT, 38 * HB_FONT2_SLOT,
    30 * HB_PORTRAIT_PX, 8 * 8,               8 * 8,          10 * 12 * 16,    20 * 16,
    DASH_ASSET_BACKGROUND_W * DASH_ASSET_BACKGROUND_H,
    320 * 22,            64 * 148,            60 * 28,        60 * 69,         62 * 24,           320 * 24,
};

static unsigned short* sArena = NULL;
static unsigned short* sPart[HB_PART_COUNT];

/* What an unbaked glyph reads: zeros (alpha 0), as large as the largest glyph (a portrait). */
static const unsigned short sHbBlankGlyph[HB_PORTRAIT_PX];

#define sPixTimer (sPart[HB_PART_TIMER])
#define sPixLap (sPart[HB_PART_LAP])
#define sPixPosDigit (sPart[HB_PART_POS_DIGIT])
#define sPixPosSuffix (sPart[HB_PART_POS_SUFFIX])
#define sPixFont1 (sPart[HB_PART_FONT1])
#define sPixFont2 (sPart[HB_PART_FONT2])
#define sPixPortrait (sPart[HB_PART_PORTRAIT])
#define sPixKoStar (sPart[HB_PART_KO_STAR])
#define sPixLives (sPart[HB_PART_LIVES])
#define sPixSpeedDigit (sPart[HB_PART_SPEED_DIGIT])
#define sPixKmh (sPart[HB_PART_KMH])

#define HB_STRIP(key, w, h, pool, i) {key, w, h, w, sHbBlankGlyph}

/* machine_custom_gfx/aTimerSymbolsTex, 8x224, sliced into 14 8x16 cells. */
DashGlyph kDashGlyphTimer[KDASHGLYPHTIMER_COUNT] = {
    HB_STRIP("0", 8, 16, sPixTimer, 0),  HB_STRIP("1", 8, 16, sPixTimer, 1),  HB_STRIP("2", 8, 16, sPixTimer, 2),
    HB_STRIP("3", 8, 16, sPixTimer, 3),  HB_STRIP("4", 8, 16, sPixTimer, 4),  HB_STRIP("5", 8, 16, sPixTimer, 5),
    HB_STRIP("6", 8, 16, sPixTimer, 6),  HB_STRIP("7", 8, 16, sPixTimer, 7),  HB_STRIP("8", 8, 16, sPixTimer, 8),
    HB_STRIP("9", 8, 16, sPixTimer, 9),  HB_STRIP("'", 8, 16, sPixTimer, 10), HB_STRIP("\"", 8, 16, sPixTimer, 11),
    HB_STRIP("+", 8, 16, sPixTimer, 12), HB_STRIP("-", 8, 16, sPixTimer, 13),
};

/* hud_gfx/aLapCounterSymbolsTex, 8x72, sliced into 6 8x12 cells. */
DashGlyph kDashGlyphLap[KDASHGLYPHLAP_COUNT] = {
    HB_STRIP("/", 8, 12, sPixLap, 0), HB_STRIP("1", 8, 12, sPixLap, 1), HB_STRIP("2", 8, 12, sPixLap, 2),
    HB_STRIP("3", 8, 12, sPixLap, 3), HB_STRIP("4", 8, 12, sPixLap, 4), HB_STRIP("5", 8, 12, sPixLap, 5),
};

/* hud_gfx/aPosition<N>Tex, 28x32 each. */
DashGlyph kDashGlyphPosDigit[KDASHGLYPHPOSDIGIT_COUNT] = {
    HB_STRIP("0", 28, 32, sPixPosDigit, 0), HB_STRIP("1", 28, 32, sPixPosDigit, 1),
    HB_STRIP("2", 28, 32, sPixPosDigit, 2), HB_STRIP("3", 28, 32, sPixPosDigit, 3),
    HB_STRIP("4", 28, 32, sPixPosDigit, 4), HB_STRIP("5", 28, 32, sPixPosDigit, 5),
    HB_STRIP("6", 28, 32, sPixPosDigit, 6), HB_STRIP("7", 28, 32, sPixPosDigit, 7),
    HB_STRIP("8", 28, 32, sPixPosDigit, 8), HB_STRIP("9", 28, 32, sPixPosDigit, 9),
};

/* hud_gfx/aPositionOrdinalSuffix<X>Tex, 20x20 each. */
DashGlyph kDashGlyphPosSuffix[KDASHGLYPHPOSSUFFIX_COUNT] = {
    HB_STRIP("ST", 20, 20, sPixPosSuffix, 0), HB_STRIP("ND", 20, 20, sPixPosSuffix, 1),
    HB_STRIP("RD", 20, 20, sPixPosSuffix, 2), HB_STRIP("TH", 20, 20, sPixPosSuffix, 3),
};

/* common_assets_compressed/aFont1<X>Tex. adv = font.c's kFont1CharWidths for the character. */
#define HB_F1(key, w, adv, i) {key, w, 16, adv, sHbBlankGlyph}
DashGlyph kDashGlyphFont1[KDASHGLYPHFONT1_COUNT] = {
    HB_F1("A", 16, 13, 0),  HB_F1("B", 16, 15, 1),  HB_F1("C", 16, 14, 2),  HB_F1("D", 16, 15, 3),
    HB_F1("E", 16, 14, 4),  HB_F1("F", 16, 13, 5),  HB_F1("G", 16, 15, 6),  HB_F1("H", 16, 15, 7),
    HB_F1("I", 8, 7, 8),    HB_F1("J", 16, 14, 9),  HB_F1("K", 16, 15, 10), HB_F1("L", 16, 14, 11),
    HB_F1("M", 16, 17, 12), HB_F1("N", 16, 14, 13), HB_F1("O", 16, 14, 14), HB_F1("P", 16, 15, 15),
    HB_F1("Q", 16, 15, 16), HB_F1("R", 16, 14, 17), HB_F1("S", 16, 14, 18), HB_F1("T", 16, 14, 19),
    HB_F1("U", 16, 14, 20), HB_F1("V", 16, 12, 21), HB_F1("W", 16, 16, 22), HB_F1("X", 16, 16, 23),
    HB_F1("Y", 16, 14, 24), HB_F1("Z", 16, 14, 25), HB_F1("0", 16, 13, 26), HB_F1("1", 8, 7, 27),
    HB_F1("2", 16, 13, 28), HB_F1("3", 16, 13, 29), HB_F1("4", 16, 13, 30), HB_F1("5", 16, 12, 31),
    HB_F1("6", 16, 13, 32), HB_F1("7", 16, 12, 33), HB_F1("8", 16, 13, 34), HB_F1("9", 16, 13, 35),
    HB_F1(".", 8, 8, 36),   HB_F1("-", 16, 16, 37),
};

/* common_assets_compressed/aFont2<X>Tex. adv = font.c's kFont2CharWidths. */
#define HB_F2(key, w, adv, i) {key, w, 8, adv, sHbBlankGlyph}
DashGlyph kDashGlyphFont2[KDASHGLYPHFONT2_COUNT] = {
    HB_F2("A", 8, 8, 0),  HB_F2("B", 8, 8, 1),  HB_F2("C", 8, 8, 2),  HB_F2("D", 8, 8, 3),  HB_F2("E", 8, 8, 4),
    HB_F2("F", 8, 8, 5),  HB_F2("G", 8, 8, 6),  HB_F2("H", 8, 8, 7),  HB_F2("I", 4, 4, 8),  HB_F2("J", 8, 8, 9),
    HB_F2("K", 8, 8, 10), HB_F2("L", 8, 8, 11), HB_F2("M", 8, 8, 12), HB_F2("N", 8, 8, 13), HB_F2("O", 8, 8, 14),
    HB_F2("P", 8, 8, 15), HB_F2("Q", 8, 8, 16), HB_F2("R", 8, 8, 17), HB_F2("S", 8, 8, 18), HB_F2("T", 8, 8, 19),
    HB_F2("U", 8, 8, 20), HB_F2("V", 8, 8, 21), HB_F2("W", 8, 8, 22), HB_F2("X", 8, 8, 23), HB_F2("Y", 8, 8, 24),
    HB_F2("Z", 8, 8, 25), HB_F2("0", 8, 8, 26), HB_F2("1", 8, 8, 27), HB_F2("2", 8, 8, 28), HB_F2("3", 8, 8, 29),
    HB_F2("4", 8, 8, 30), HB_F2("5", 8, 8, 31), HB_F2("6", 8, 8, 32), HB_F2("7", 8, 8, 33), HB_F2("8", 8, 8, 34),
    HB_F2("9", 8, 8, 35), HB_F2(".", 8, 8, 36), HB_F2("-", 8, 8, 37),
};

/* hud_gfx/aPortrait<Name>Tex, keyed by the Racer::character index (hud.c's
 * gCharacterPortraitTextures[] order). */
#define HB_PT(key, i) {key, 32, 32, 32, sHbBlankGlyph}
DashGlyph kDashGlyphPortrait[KDASHGLYPHPORTRAIT_COUNT] = {
    HB_PT("0", 0),   HB_PT("1", 1),   HB_PT("2", 2),   HB_PT("3", 3),   HB_PT("4", 4),   HB_PT("5", 5),
    HB_PT("6", 6),   HB_PT("7", 7),   HB_PT("8", 8),   HB_PT("9", 9),   HB_PT("10", 10), HB_PT("11", 11),
    HB_PT("12", 12), HB_PT("13", 13), HB_PT("14", 14), HB_PT("15", 15), HB_PT("16", 16), HB_PT("17", 17),
    HB_PT("18", 18), HB_PT("19", 19), HB_PT("20", 20), HB_PT("21", 21), HB_PT("22", 22), HB_PT("23", 23),
    HB_PT("24", 24), HB_PT("25", 25), HB_PT("26", 26), HB_PT("27", 27), HB_PT("28", 28), HB_PT("29", 29),
};

DashGlyph kDashGlyphKoStar[KDASHGLYPHKOSTAR_COUNT] = {{"star", 8, 8, 8, sHbBlankGlyph}};
DashGlyph kDashGlyphLives[KDASHGLYPHLIVES_COUNT] = {{"machine", 8, 8, 8, sHbBlankGlyph}};

/* machine_custom_gfx/aSpeedDigitsTex, 12x160, sliced into 10 12x16 cells. */
DashGlyph kDashGlyphSpeedDigit[KDASHGLYPHSPEEDDIGIT_COUNT] = {
    HB_STRIP("0", 12, 16, sPixSpeedDigit, 0), HB_STRIP("1", 12, 16, sPixSpeedDigit, 1),
    HB_STRIP("2", 12, 16, sPixSpeedDigit, 2), HB_STRIP("3", 12, 16, sPixSpeedDigit, 3),
    HB_STRIP("4", 12, 16, sPixSpeedDigit, 4), HB_STRIP("5", 12, 16, sPixSpeedDigit, 5),
    HB_STRIP("6", 12, 16, sPixSpeedDigit, 6), HB_STRIP("7", 12, 16, sPixSpeedDigit, 7),
    HB_STRIP("8", 12, 16, sPixSpeedDigit, 8), HB_STRIP("9", 12, 16, sPixSpeedDigit, 9),
};

DashGlyph kDashGlyphKmh[KDASHGLYPHKMH_COUNT] = {{"kmh", 20, 16, 20, sHbBlankGlyph}};

/* Texture-name suffixes, index-aligned with the tables above. */
static const char* const kFontTexSuffix[HB_FONT_GLYPHS] = {
    "A",    "B",    "C",    "D",    "E",    "F",    "G",    "H",    "I",    "J",      "K",    "L",    "M",
    "N",    "O",    "P",    "Q",    "R",    "S",    "T",    "U",    "V",    "W",    "X",      "Y",    "Z",    "Num0",
    "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9", "Period", "Dash",
};

static const char* const kPortraitTexName[30] = {
    "CaptainFalcon", "DrStewart",    "Pico",         "SamuraiGoroh", "JodySummer",    "MightyGazelle",
    "MrEad",         "Baba",         "Octoman",      "GomarAndShioh", "KateAlen",     "RogerBuster",
    "JamesMcCloud",  "Leon",         "AntonioGuster", "BlackShadow", "MichaelChain",  "JackLevin",
    "SuperArrow",    "MrsArrow",     "JohnTanaka",   "Beastman",     "Zoda",          "DrClash",
    "SilverNeelsen", "BioRex",       "Draq",         "Billy",        "TheSkull",      "BloodFalcon",
};

static const char* const kSuffixTexName[4] = {"ST", "ND", "RD", "TH"};

/* ---- chrome widgets: buffers + table ----------------------------------------------------- */

unsigned short* gDashAssetBackground = NULL;

/* x/y: where each piece sits on the DASH (the user's layout: header at the bottom, energy bar at
 * the top; card-4b moved lap/timer/gap down). The recipes draw in the template's own canvas
 * coordinates (HbWidgetWindow below), which is where those differ. */
DashAssetWidget kDashAssetWidgets[DASH_ASSET_WIDGET_COUNT] = {
    {"header", NULL, 0, 218, 320, 22, 1},
    {"standings", NULL, 0, 67, 64, 148, 1},
    {"lap", NULL, 258, 52, 60, 28, 1},
    {"timer", NULL, 258, 85, 60, 69, 1},
    {"gap", NULL, 258, 155, 62, 24, 1},
    {"energy_bar", NULL, 0, 0, 320, 24, 1},
};

/* ---- allocation ----------------------------------------------------------------------------
 *
 * Every allocation this module makes -- the arena and the bake's transient images, read buffer
 * and LZ tables -- goes through these, so the console can keep the whole bake off the regular
 * heap (its free list is the Old 3DS's scarcest memory; the linear heap has megabytes spare). */
static void* HbDefaultAlloc(size_t n) {
    return malloc(n);
}

static void HbDefaultFree(void* p) {
    free(p);
}

static GdxHudBakeAllocFn sHbAlloc = HbDefaultAlloc;
static GdxHudBakeFreeFn sHbFree = HbDefaultFree;

void gdx3ds_hudbake_set_allocator(GdxHudBakeAllocFn alloc, GdxHudBakeFreeFn release) {
    if (sArena != NULL) {
        return; /* the arena already belongs to the old allocator */
    }
    sHbAlloc = (alloc != NULL && release != NULL) ? alloc : HbDefaultAlloc;
    sHbFree = (alloc != NULL && release != NULL) ? release : HbDefaultFree;
}

static void* HbMalloc(size_t n) {
    return sHbAlloc(n != 0 ? n : 1);
}

static void* HbCalloc(size_t count, size_t size) {
    void* p;
    if (size != 0 && count > (size_t) -1 / size) {
        return NULL;
    }
    p = HbMalloc(count * size);
    if (p != NULL) {
        memset(p, 0, count * size);
    }
    return p;
}

static void HbFree(void* p) {
    if (p != NULL) {
        sHbFree(p);
    }
}

static size_t HbArenaWords(void) {
    size_t n = 0;
    int i;
    for (i = 0; i < HB_PART_COUNT; i++) {
        n += kHbPartWords[i];
    }
    return n;
}

static void HbBindTable(DashGlyph* table, int count, unsigned short* pool, int slot) {
    int i;
    for (i = 0; i < count; i++) {
        table[i].pixels = pool + (size_t) i * (size_t) (slot > 0 ? slot : table[i].w * table[i].h);
    }
}

/* Points every table at its part of the arena (the layout HB_STRIP/HB_F1/... describe). */
static void HbBindArena(void) {
    unsigned short* p = sArena;
    int i;
    for (i = 0; i < HB_PART_COUNT; i++) {
        sPart[i] = p;
        p += kHbPartWords[i];
    }
    HbBindTable(kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, sPixTimer, 0);
    HbBindTable(kDashGlyphLap, KDASHGLYPHLAP_COUNT, sPixLap, 0);
    HbBindTable(kDashGlyphPosDigit, KDASHGLYPHPOSDIGIT_COUNT, sPixPosDigit, 0);
    HbBindTable(kDashGlyphPosSuffix, KDASHGLYPHPOSSUFFIX_COUNT, sPixPosSuffix, 0);
    HbBindTable(kDashGlyphFont1, KDASHGLYPHFONT1_COUNT, sPixFont1, HB_FONT1_SLOT);
    HbBindTable(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, sPixFont2, HB_FONT2_SLOT);
    HbBindTable(kDashGlyphPortrait, KDASHGLYPHPORTRAIT_COUNT, sPixPortrait, HB_PORTRAIT_PX);
    HbBindTable(kDashGlyphKoStar, KDASHGLYPHKOSTAR_COUNT, sPixKoStar, 0);
    HbBindTable(kDashGlyphLives, KDASHGLYPHLIVES_COUNT, sPixLives, 0);
    HbBindTable(kDashGlyphSpeedDigit, KDASHGLYPHSPEEDDIGIT_COUNT, sPixSpeedDigit, 0);
    HbBindTable(kDashGlyphKmh, KDASHGLYPHKMH_COUNT, sPixKmh, 0);
    gDashAssetBackground = sPart[HB_PART_BACKGROUND];
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        kDashAssetWidgets[i].pixels = sPart[HB_PART_W_HEADER + i];
    }
}

/* 1 once the arena exists (allocated zeroed on the first call). */
static int HbEnsureArena(void) {
    if (sArena != NULL) {
        return 1;
    }
    sArena = (unsigned short*) HbCalloc(HbArenaWords(), sizeof(*sArena));
    if (sArena == NULL) {
        return 0;
    }
    HbBindArena();
    return 1;
}

size_t gdx3ds_hudbake_arena_bytes(void) {
    return HbArenaWords() * sizeof(unsigned short);
}

int gdx3ds_hudbake_ready(void) {
    return sArena != NULL;
}

/* ---- bake context ------------------------------------------------------------------------ */

#define HB_OTEX_MAGIC 0x4F544558u
#define HB_TEX_RGBA16 2u
#define HB_TEX_I4 5u
#define HB_READ_CAP (0x50 + 160 * 65 * 2 + 64) /* the largest record read: aTitleLogoTex */
#define HB_CANVAS_W 320
#define HB_CANVAS_H 240

typedef struct HbCtx {
    GdxHudBakeReadFn read;
    void* readUser;
    GdxHudBakeLogFn log;
    void* logUser;
    unsigned char* buf;
    GdxHudBakeStats stats;
} HbCtx;

static int sBakeDone = 0;

/* What the last bake read, for the SD cache's key (gdx3ds_hudbake_cache_write). */
#define HB_KEY_MAX 160
#define HB_KEY_LEN GDX3DS_HUDBAKE_KEY_LEN
static char sBakeKeys[HB_KEY_MAX][HB_KEY_LEN];
static int sBakeKeyCount = 0;
static int sBakeKeysOverflow = 0;
static int sBakeMissing = -1; /* -1: no bake yet (or the buffers came from a cache) */

static void HbRecordKey(const char* key) {
    if (sBakeKeyCount >= HB_KEY_MAX || strlen(key) >= HB_KEY_LEN) {
        sBakeKeysOverflow = 1;
        return;
    }
    strcpy(sBakeKeys[sBakeKeyCount], key);
    sBakeKeyCount++;
}

#if defined(__GNUC__) || defined(__clang__)
static void HbLog(HbCtx* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
static void HbLog(HbCtx* c, const char* fmt, ...) {
    char line[192];
    va_list ap;
    if (c->log == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    c->log(line, c->logUser);
}

static uint32_t HbLe32(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* Reads one OTEX record and checks it is the expected format and size. Returns the payload
 * (valid until the next load) or NULL, after logging why. */
static const unsigned char* HbLoadTex(HbCtx* c, const char* key, uint32_t type, int w, int h) {
    const size_t need = (type == HB_TEX_I4) ? (size_t) (w * h + 1) / 2 : (size_t) w * h * 2;
    size_t got;
    c->stats.textures++;
    HbRecordKey(key);
    got = (c->read != NULL && c->buf != NULL) ? c->read(key, c->buf, HB_READ_CAP, c->readUser) : 0;
    c->stats.bytesRead += (unsigned long) got;
    if (got < 0x50) {
        c->stats.missing++;
        HbLog(c, "[hudbake] missing %s (%u bytes)", key, (unsigned) got);
        return NULL;
    }
    if (HbLe32(c->buf + 0x04) != HB_OTEX_MAGIC || HbLe32(c->buf + 0x40) != type ||
        HbLe32(c->buf + 0x44) != (uint32_t) w || HbLe32(c->buf + 0x48) != (uint32_t) h ||
        HbLe32(c->buf + 0x4C) < need || got < 0x50 + need) {
        c->stats.missing++;
        HbLog(c, "[hudbake] %s: unexpected format (type %u, %u wide)", key, HbLe32(c->buf + 0x40),
              HbLe32(c->buf + 0x44));
        return NULL;
    }
    return c->buf + 0x50;
}

/* Big-endian RGBA16 texels -> host-order RGBA5551 words. */
static void HbCopyWords(unsigned short* dst, const unsigned char* src, int n) {
    int i;
    for (i = 0; i < n; i++) {
        dst[i] = (unsigned short) ((src[2 * i] << 8) | src[2 * i + 1]);
    }
}

/* ---- glyph recipes ----------------------------------------------------------------------- */

/* One texture into one contiguous pool (strips: the cells are consecutive rows). Black texels
 * (r = g = b = 0) become transparent when `blackClear` (the speedometer's opaque cells). */
static void HbBakeTexture(HbCtx* c, const char* key, int w, int h, unsigned short* dst, int blackClear) {
    const unsigned char* px = HbLoadTex(c, key, HB_TEX_RGBA16, w, h);
    int i;
    if (px == NULL) {
        memset(dst, 0, (size_t) w * h * sizeof(*dst));
        return;
    }
    HbCopyWords(dst, px, w * h);
    if (!blackClear) {
        return;
    }
    for (i = 0; i < w * h; i++) {
        if ((dst[i] & 0xFFFEu) == 0u) {
            dst[i] = 0u;
        }
    }
}

/* The glyph textures, in bake order: index -> archive key, cell size, destination, and whether
 * black means clear. One function for the bake AND for gdx3ds_hudbake_list_keys, so the key
 * list a reader prefetches is, by construction, the list the bake reads. */
#define HB_GLYPH_TEX_COUNT (2 + 10 + 4 + HB_FONT_GLYPHS + HB_FONT_GLYPHS + 30 + 4)

typedef struct HbGlyphTex {
    int w, h;
    unsigned short* dst; /* NULL until the arena exists */
    int blackClear;
} HbGlyphTex;

static int HbGlyphTexAt(int i, char* key, size_t cap, HbGlyphTex* t) {
    t->blackClear = 0;
    if (i < 0 || i >= HB_GLYPH_TEX_COUNT) {
        return 0;
    }
    if (i == 0) {
        snprintf(key, cap, "machine_custom_gfx/aTimerSymbolsTex");
        t->w = 8, t->h = 224, t->dst = sPixTimer;
        return 1;
    }
    if (i == 1) {
        snprintf(key, cap, "hud_gfx/aLapCounterSymbolsTex");
        t->w = 8, t->h = 72, t->dst = sPixLap;
        return 1;
    }
    i -= 2;
    if (i < 10) {
        snprintf(key, cap, "hud_gfx/aPosition%dTex", i);
        t->w = 28, t->h = 32, t->dst = sPixPosDigit != NULL ? sPixPosDigit + i * 28 * 32 : NULL;
        return 1;
    }
    i -= 10;
    if (i < 4) {
        snprintf(key, cap, "hud_gfx/aPositionOrdinalSuffix%sTex", kSuffixTexName[i]);
        t->w = 20, t->h = 20, t->dst = sPixPosSuffix != NULL ? sPixPosSuffix + i * 20 * 20 : NULL;
        return 1;
    }
    i -= 4;
    if (i < HB_FONT_GLYPHS) {
        snprintf(key, cap, "common_assets_compressed/aFont1%sTex", kFontTexSuffix[i]);
        t->w = kDashGlyphFont1[i].w, t->h = kDashGlyphFont1[i].h;
        t->dst = sPixFont1 != NULL ? sPixFont1 + i * HB_FONT1_SLOT : NULL;
        return 1;
    }
    i -= HB_FONT_GLYPHS;
    if (i < HB_FONT_GLYPHS) {
        snprintf(key, cap, "common_assets_compressed/aFont2%sTex", kFontTexSuffix[i]);
        t->w = kDashGlyphFont2[i].w, t->h = kDashGlyphFont2[i].h;
        t->dst = sPixFont2 != NULL ? sPixFont2 + i * HB_FONT2_SLOT : NULL;
        return 1;
    }
    i -= HB_FONT_GLYPHS;
    if (i < 30) {
        snprintf(key, cap, "hud_gfx/aPortrait%sTex", kPortraitTexName[i]);
        t->w = 32, t->h = 32, t->dst = sPixPortrait != NULL ? sPixPortrait + i * HB_PORTRAIT_PX : NULL;
        return 1;
    }
    i -= 30;
    switch (i) {
        case 0:
            snprintf(key, cap, "hud_gfx/aHudRacersKOdTex");
            t->w = 8, t->h = 8, t->dst = sPixKoStar;
            break;
        case 1:
            snprintf(key, cap, "hud_gfx/aHudLivesLeftIconTex");
            t->w = 8, t->h = 8, t->dst = sPixLives;
            break;
        case 2:
            snprintf(key, cap, "machine_custom_gfx/aSpeedDigitsTex");
            t->w = 12, t->h = 160, t->dst = sPixSpeedDigit, t->blackClear = 1;
            break;
        default:
            snprintf(key, cap, "machine_custom_gfx/aKmhTex");
            t->w = 20, t->h = 16, t->dst = sPixKmh, t->blackClear = 1;
            break;
    }
    return 1;
}

static void HbBakeGlyphs(HbCtx* c) {
    int i;
    for (i = 0; i < HB_GLYPH_TEX_COUNT; i++) {
        char key[HB_KEY_LEN];
        HbGlyphTex t;
        HbGlyphTexAt(i, key, sizeof(key), &t);
        HbBakeTexture(c, key, t.w, t.h, t.dst, t.blackClear);
    }
}

/* ---- RGBA8 images (the template's Pillow pipeline) --------------------------------------- */

typedef struct HbImg {
    int w, h;
    unsigned char* px; /* RGBA8, row-major; NULL = empty (draws nothing) */
} HbImg;

static HbImg HbImgNew(int w, int h) {
    HbImg im;
    im.w = w;
    im.h = h;
    im.px = (w > 0 && h > 0) ? (unsigned char*) HbCalloc((size_t) w * h, 4) : NULL;
    return im;
}

static void HbImgFree(HbImg* im) {
    HbFree(im->px);
    im->px = NULL;
}

static unsigned char HbExpand5(unsigned v) {
    return (unsigned char) ((v << 3) | (v >> 2));
}

/* RGBA5551 words -> RGBA8 the way tools/o2r_texture_extract.py decoded the gallery PNGs. */
static HbImg HbImgFromWords(const unsigned short* words, int w, int h) {
    HbImg im = HbImgNew(w, h);
    int i;
    if (im.px == NULL) {
        return im;
    }
    for (i = 0; i < w * h; i++) {
        const unsigned v = words[i];
        im.px[4 * i + 0] = HbExpand5((v >> 11) & 31u);
        im.px[4 * i + 1] = HbExpand5((v >> 6) & 31u);
        im.px[4 * i + 2] = HbExpand5((v >> 1) & 31u);
        im.px[4 * i + 3] = (v & 1u) ? 255 : 0;
    }
    return im;
}

static HbImg HbImgFromGlyph(const DashGlyph* g) {
    return HbImgFromWords(g->pixels, g->w, g->h);
}

/* A texture straight from the archive (an empty image if it is missing). */
static HbImg HbImgFromTex(HbCtx* c, const char* key, int w, int h) {
    unsigned short* words;
    const unsigned char* px = HbLoadTex(c, key, HB_TEX_RGBA16, w, h);
    HbImg im;
    if (px == NULL) {
        im.w = w;
        im.h = h;
        im.px = NULL;
        return im;
    }
    words = (unsigned short*) HbMalloc((size_t) w * h * sizeof(*words));
    if (words == NULL) {
        return HbImgNew(0, 0);
    }
    HbCopyWords(words, px, w * h);
    im = HbImgFromWords(words, w, h);
    HbFree(words);
    return im;
}

/* Python int() of a non-negative double, with Pillow's max(1, ...) for sizes. */
static int HbSize(double v) {
    const int n = (int) v;
    return n < 1 ? 1 : n;
}

/* Python round() (half to even), for the template's int(round(x)) placements. */
static int HbRound(double v) {
    const double f = floor(v);
    const double d = v - f;
    if (d > 0.5) {
        return (int) f + 1;
    } else if (d < 0.5) {
        return (int) f;
    } else if (fmod(f, 2.0) == 0.0) {
        return (int) f;
    } else {
        return (int) f + 1;
    }
}

/* Image.resize((dw, dh), NEAREST): Pillow's ImagingScaleAffine. */
static void HbScaleTable(int* out, int nIn, int nOut) {
    const double a = (double) nIn / (double) nOut;
    double o = a * 0.5;
    int i;
    for (i = 0; i < nOut; i++) {
        const int v = o < 0.0 ? -1 : (int) o;
        out[i] = (v >= 0 && v < nIn) ? v : -1;
        o += a;
    }
}

static HbImg HbImgResize(const HbImg* src, int dw, int dh) {
    HbImg out = HbImgNew(dw, dh);
    int* xs;
    int* ys;
    int x;
    int y;
    if (out.px == NULL || src->px == NULL) {
        HbImgFree(&out);
        out.w = dw;
        out.h = dh;
        return out;
    }
    xs = (int*) HbMalloc(sizeof(int) * (size_t) (dw + dh));
    if (xs == NULL) {
        HbImgFree(&out);
        return out;
    }
    ys = xs + dw;
    HbScaleTable(xs, src->w, dw);
    HbScaleTable(ys, src->h, dh);
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            if (xs[x] >= 0 && ys[y] >= 0) {
                memcpy(out.px + 4 * (y * dw + x), src->px + 4 * (ys[y] * src->w + xs[x]), 4);
            }
        }
    }
    HbFree(xs);
    return out;
}

/* putalpha(eval(A, lambda a: int(a * k))). */
static void HbImgScaleAlpha(HbImg* im, double k) {
    int i;
    if (im->px == NULL) {
        return;
    }
    for (i = 0; i < im->w * im->h; i++) {
        im->px[4 * i + 3] = (unsigned char) (int) (im->px[4 * i + 3] * k);
    }
}

/* ImagingAlphaComposite, one pixel: src over dst, in place into dst. */
#define HB_DIV255(a) ((((a) >> 8) + (a)) >> 8)
static void HbBlend(unsigned char* dst, const unsigned char* src) {
    uint32_t blend;
    uint32_t outa255;
    uint32_t coef1;
    uint32_t coef2;
    int i;
    if (src[3] == 0) {
        return;
    }
    blend = (uint32_t) dst[3] * (255u - src[3]);
    outa255 = (uint32_t) src[3] * 255u + blend;
    coef1 = (uint32_t) src[3] * 255u * 255u * 128u / outa255;
    coef2 = 255u * 128u - coef1;
    for (i = 0; i < 3; i++) {
        const uint32_t t = (uint32_t) src[i] * coef1 + (uint32_t) dst[i] * coef2;
        dst[i] = (unsigned char) (HB_DIV255(t + (0x80u << 7)) >> 7);
    }
    dst[3] = (unsigned char) HB_DIV255(outa255 + 0x80u);
}

/* ---- the canvas: a window (ox, oy, w, h) onto the template's 320x240 layer ---------------- */

typedef struct HbCanvas {
    int ox, oy;
    HbImg im;
} HbCanvas;

static unsigned char* HbCanvasAt(HbCanvas* cv, int x, int y) {
    if (cv->im.px == NULL || x < 0 || y < 0 || x >= HB_CANVAS_W || y >= HB_CANVAS_H) {
        return NULL;
    }
    x -= cv->ox;
    y -= cv->oy;
    if (x < 0 || y < 0 || x >= cv->im.w || y >= cv->im.h) {
        return NULL;
    }
    return cv->im.px + 4 * (y * cv->im.w + x);
}

static void HbCanvasComposite(HbCanvas* cv, const HbImg* src, int x0, int y0) {
    int x;
    int y;
    if (src->px == NULL) {
        return;
    }
    for (y = 0; y < src->h; y++) {
        for (x = 0; x < src->w; x++) {
            unsigned char* d = HbCanvasAt(cv, x0 + x, y0 + y);
            if (d != NULL) {
                HbBlend(d, src->px + 4 * (y * src->w + x));
            }
        }
    }
}

typedef struct HbRgb {
    unsigned char r, g, b;
} HbRgb;

static void HbCanvasSet(HbCanvas* cv, int x, int y, HbRgb c) {
    unsigned char* d = HbCanvasAt(cv, x, y);
    if (d != NULL) {
        d[0] = c.r;
        d[1] = c.g;
        d[2] = c.b;
        d[3] = 255;
    }
}

/* ImageDraw.rectangle([x0, y0, x1, y1], fill=...): corners truncated, both ends inclusive. */
static void HbFillRect(HbCanvas* cv, double fx0, double fy0, double fx1, double fy1, HbRgb c) {
    const int x0 = (int) fx0;
    const int y0 = (int) fy0;
    const int x1 = (int) fx1;
    const int y1 = (int) fy1;
    int x;
    int y;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            HbCanvasSet(cv, x, y, c);
        }
    }
}

/* ImageDraw.rectangle([x0, y0, x1, y1], outline=...), width 1. */
static void HbStrokeRect(HbCanvas* cv, int x0, int y0, int x1, int y1, HbRgb c) {
    int x;
    int y;
    for (x = x0; x <= x1; x++) {
        HbCanvasSet(cv, x, y0, c);
        HbCanvasSet(cv, x, y1, c);
    }
    for (y = y0; y <= y1; y++) {
        HbCanvasSet(cv, x0, y, c);
        HbCanvasSet(cv, x1, y, c);
    }
}

/* nn_resize(im, w * scale, h * scale) composited at (x, y). */
static void HbCanvasPasteScaled(HbCanvas* cv, const HbImg* im, double scale, int x, int y) {
    HbImg s = HbImgResize(im, HbSize(im->w * scale), HbSize(im->h * scale));
    HbCanvasComposite(cv, &s, x, y);
    HbImgFree(&s);
}

static const DashGlyph* HbFindGlyph(const DashGlyph* table, int count, char ch) {
    int i;
    for (i = 0; i < count; i++) {
        if (table[i].key[0] == ch && table[i].key[1] == '\0') {
            return &table[i];
        }
    }
    return NULL;
}

/* draw_font_string: real Font1/Font2 glyphs at the game's proportional advances. */
static void HbDrawText(HbCanvas* cv, const DashGlyph* table, int space, double x, double y, double scale,
                       const char* text) {
    double cx = x;
    for (; *text != '\0'; text++) {
        const DashGlyph* g = (*text == ' ') ? NULL : HbFindGlyph(table, HB_FONT_GLYPHS, *text);
        const double adv = (double) ((*text == ' ') ? space : (g != NULL ? g->adv : 0)) * scale;
        if (g != NULL) {
            HbImg im = HbImgFromGlyph(g);
            HbCanvasPasteScaled(cv, &im, scale, HbRound(cx), HbRound(y));
            HbImgFree(&im);
        }
        cx += adv;
    }
}

/* font_string_width: Python's sum() of the scaled advances, in order. */
static double HbTextWidth(const DashGlyph* table, int space, double scale, const char* text) {
    double w = 0.0;
    for (; *text != '\0'; text++) {
        const DashGlyph* g = (*text == ' ') ? NULL : HbFindGlyph(table, HB_FONT_GLYPHS, *text);
        w += (double) ((*text == ' ') ? space : (g != NULL ? g->adv : 0)) * scale;
    }
    return w;
}

/* glyph_strip: fixed-cell digit atlases, advancing by the cell width * scale. */
static void HbDrawStrip(HbCanvas* cv, const DashGlyph* table, int count, double x, double y, double scale,
                        const char* text) {
    double cx = x;
    for (; *text != '\0'; text++) {
        const DashGlyph* g = HbFindGlyph(table, count, *text);
        if (g != NULL) {
            HbImg im = HbImgFromGlyph(g);
            HbCanvasPasteScaled(cv, &im, scale, HbRound(cx), HbRound(y));
            HbImgFree(&im);
            cx += g->w * scale;
        }
    }
}

/* tools/nibottom_template_import.py's quantisation: round(v * (levels - 1) / 255) (never a tie
 * for 8-bit inputs), alpha >= 128 -> opaque. */
static unsigned HbQ5(unsigned v) {
    return (v * 31u * 2u + 255u) / 510u;
}

static unsigned HbQ6(unsigned v) {
    return (v * 63u * 2u + 255u) / 510u;
}

static unsigned short HbPack5551(const unsigned char* p) {
    return (unsigned short) ((HbQ5(p[0]) << 11) | (HbQ5(p[1]) << 6) | (HbQ5(p[2]) << 1) | (p[3] >= 128 ? 1u : 0u));
}

static unsigned short HbPack565(const unsigned char* p) {
    return (unsigned short) ((HbQ5(p[0]) << 11) | (HbQ6(p[1]) << 5) | HbQ5(p[2]));
}

static int HbCanvasOpen(HbCanvas* cv, int ox, int oy, int w, int h) {
    cv->ox = ox;
    cv->oy = oy;
    cv->im = HbImgNew(w, h);
    return cv->im.px != NULL;
}

static void HbCanvasStore(HbCanvas* cv, unsigned short* dst) {
    int i;
    for (i = 0; i < cv->im.w * cv->im.h; i++) {
        dst[i] = HbPack5551(cv->im.px + 4 * i);
    }
    HbImgFree(&cv->im);
}

/* ---- the template's widget recipes (tools/gen_nibottom_template.py build()) -------------- */

static const HbRgb kFrame = {90, 42, 66};

static void HbRecipeHeader(HbCanvas* cv) {
    static const HbRgb kFill = {33, 12, 24};
    double w2;
    HbFillRect(cv, 0, 0, 320, 22, kFill);
    HbStrokeRect(cv, 0, 0, 320, 22, kFrame);
    HbDrawText(cv, kDashGlyphFont1, DASH_FONT1_SPACE, 6, 3, 0.56, "MUTE CITY I");
    w2 = HbTextWidth(kDashGlyphFont2, DASH_FONT2_SPACE, 0.9, "30 RACERS");
    HbDrawText(cv, kDashGlyphFont2, DASH_FONT2_SPACE, 314 - w2, 7, 0.9, "30 RACERS");
}

/* paint_portrait: a real portrait, the highlight border (ours) or a plain frame, and the rank
 * ribbon chip under it. */
static void HbPaintPortrait(HbCtx* c, HbCanvas* cv, int x, int y, int size, int character, int rank,
                            int highlighted) {
    static const char* const kRibbon[5] = {"First", "Second", "Third", "Fourth", "Fifth"};
    char key[64];
    const double rs = size / 32.0;
    HbImg im = HbImgFromGlyph(&kDashGlyphPortrait[character]);
    HbImg s = HbImgResize(&im, size, size);
    HbCanvasComposite(cv, &s, x, y);
    HbImgFree(&s);
    HbImgFree(&im);
    if (highlighted) {
        im = HbImgFromTex(c, "hud_gfx/aCharacterPortraitHighlightBorderTex", 32, 32);
        s = HbImgResize(&im, size, size);
        HbCanvasComposite(cv, &s, x, y);
        HbImgFree(&s);
        HbImgFree(&im);
    } else {
        HbStrokeRect(cv, x, y, x + size, y + size, kFrame);
    }
    snprintf(key, sizeof(key), "hud_gfx/aPortraitPosition%sTex", kRibbon[rank - 1]);
    im = HbImgFromTex(c, key, 16, 6);
    {
        const int rw = (int) (im.w * rs);
        const int rh = (int) (im.h * rs * 1.6);
        s = HbImgResize(&im, rw, rh);
        HbCanvasComposite(cv, &s, x + (size - rw) / 2, y + size - rh - 2);
        HbImgFree(&s);
    }
    HbImgFree(&im);
}

static void HbRecipeStandings(HbCtx* c, HbCanvas* cv) {
    /* Ranks 1..5: Pico, Samurai Goroh, Jody Summer, Captain Falcon (ours, rank 4), Mighty Gazelle
     * (character indices into kDashGlyphPortrait). */
    static const int kCharacter[5] = {2, 3, 4, 0, 5};
    int i;
    for (i = 0; i < 5; i++) {
        HbPaintPortrait(c, cv, 4, 70 + i * 29, 24, kCharacter[i], i + 1, i == 3);
    }
}

static void HbRecipeLap(HbCtx* c, HbCanvas* cv) {
    HbImg im = HbImgFromTex(c, "machine_custom_gfx/aLapTex", 16, 12);
    HbCanvasComposite(cv, &im, 260, 27);
    HbImgFree(&im);
    HbDrawStrip(cv, kDashGlyphLap, KDASHGLYPHLAP_COUNT, 260, 38, 1.15, "2/3");
}

static void HbRecipeTimer(HbCtx* c, HbCanvas* cv) {
    HbImg im = HbImgFromTex(c, "hud_gfx/aHudTimeTex", 24, 16);
    HbCanvasComposite(cv, &im, 260, 60);
    HbImgFree(&im);
    im = HbImgFromTex(c, "common_assets_compressed/aBestTex", 16, 12);
    HbCanvasComposite(cv, &im, 260, 96);
    HbImgFree(&im);
    HbDrawStrip(cv, kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, 260, 76, 0.78, "0'42\"18");
    HbDrawStrip(cv, kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, 260, 110, 0.62, "0'38\"94");
}

static void HbRecipeGap(HbCanvas* cv) {
    HbDrawText(cv, kDashGlyphFont2, DASH_FONT2_SPACE, 260, 130, 0.9, "GAP");
    HbDrawStrip(cv, kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, 260, 140, 0.62, "-0'01\"22");
}

static void HbRecipeEnergyBar(HbCtx* c, HbCanvas* cv) {
    /* sEnergyBarFillColors (hud.c) rows, the template's frozen 63% example fill. */
    static const HbRgb kRows[5] = {{0x9b, 0x13, 0x13}, {0xdc, 0x14, 0x14}, {0xff, 0x1a, 0x1a},
                                   {0xdc, 0x1e, 0x1e}, {0x9b, 0x13, 0x13}};
    static const HbRgb kBlack = {0, 0, 0};
    const int ex = 54;
    const int ey = 217;
    const int ew = 260;
    const int eh = 18;
    const int innerW = HbRound((ew - 2) * 0.63);
    const double rh = (eh - 2) / 5.0;
    HbImg frame;
    HbImg s;
    int i;
    HbDrawText(cv, kDashGlyphFont2, DASH_FONT2_SPACE, 6, 218, 0.9, "ENERGY");
    HbFillRect(cv, ex, ey, ex + ew, ey + eh, kBlack);
    frame = HbImgFromTex(c, "hud_gfx/aHudEnergyTex", 72, 16);
    s = HbImgResize(&frame, ew, eh);
    HbImgScaleAlpha(&s, 0.35);
    HbCanvasComposite(cv, &s, ex, ey);
    HbImgFree(&s);
    HbImgFree(&frame);
    HbStrokeRect(cv, ex, ey, ex + ew, ey + eh, kFrame);
    for (i = 0; i < 5; i++) {
        HbFillRect(cv, ex + 1, ey + 1 + i * rh, ex + 1 + innerW, ey + 1 + (i + 1) * rh, kRows[i]);
    }
}

typedef enum {
    HB_W_HEADER,
    HB_W_STANDINGS,
    HB_W_LAP,
    HB_W_TIMER,
    HB_W_GAP,
    HB_W_ENERGY_BAR,
} HbWidget;

/* The template-canvas rect each widget was exported from (layout.json before the user moved
 * header/energy_bar; kDashAssetWidgets has where they sit now). */
static void HbWidgetWindow(int widget, int* ox, int* oy) {
    static const int kWin[DASH_ASSET_WIDGET_COUNT][2] = {
        {0, 0}, {0, 67}, {258, 26}, {258, 59}, {258, 129}, {0, 216},
    };
    *ox = kWin[widget][0];
    *oy = kWin[widget][1];
}

static void HbBakeWidget(HbCtx* c, int widget) {
    const DashAssetWidget* w = &kDashAssetWidgets[widget];
    unsigned short* dst = (unsigned short*) w->pixels;
    HbCanvas cv;
    int ox;
    int oy;
    HbWidgetWindow(widget, &ox, &oy);
    if (!HbCanvasOpen(&cv, ox, oy, w->w, w->h)) {
        memset(dst, 0, (size_t) w->w * w->h * sizeof(*dst));
        HbLog(c, "[hudbake] %s: out of memory", w->id);
        return;
    }
    switch (widget) {
        case HB_W_HEADER:
            HbRecipeHeader(&cv);
            break;
        case HB_W_STANDINGS:
            HbRecipeStandings(c, &cv);
            break;
        case HB_W_LAP:
            HbRecipeLap(c, &cv);
            break;
        case HB_W_TIMER:
            HbRecipeTimer(c, &cv);
            break;
        case HB_W_GAP:
            HbRecipeGap(&cv);
            break;
        default:
            HbRecipeEnergyBar(c, &cv);
            break;
    }
    HbCanvasStore(&cv, dst);
}

/* draw_background: a #17080f base, the 16x16 checker decal recoloured to two pinks and tiled at
 * 16% alpha, the title logo scaled to 55% at 16% alpha in the bottom-right corner; RGB565. The
 * tiled layer has only two colours, so each is blended once; only the logo's rect blends per
 * pixel. */
static void HbBakeBackground(HbCtx* c) {
    static const unsigned char kBase[4] = {23, 8, 15, 255};
    static const unsigned char kLight[4] = {230, 90, 150, 40};
    static const unsigned char kDark[4] = {58, 12, 36, 40};
    unsigned char light[4];
    unsigned char dark[4];
    unsigned char lightTile[16 * 16];
    const unsigned char* checker = HbLoadTex(c, "machine_custom_gfx/aDecalCheckeredTex", HB_TEX_I4, 16, 16);
    HbImg logo;
    HbImg s;
    int lx;
    int ly;
    int x;
    int y;
    memcpy(light, kBase, 4);
    memcpy(dark, kBase, 4);
    HbBlend(light, kLight);
    HbBlend(dark, kDark);
    for (y = 0; y < 16 * 16; y++) {
        const unsigned nib = checker == NULL ? 0u : (y & 1) ? (checker[y / 2] & 0xFu) : (checker[y / 2] >> 4);
        lightTile[y] = checker != NULL && nib >= 8u; /* (nib << 4 | nib) > 127 */
    }
    logo = HbImgFromTex(c, "common_assets_compressed/aTitleLogoTex", 160, 65);
    s = HbImgResize(&logo, (int) (logo.w * 0.55), (int) (logo.h * 0.55));
    HbImgScaleAlpha(&s, 0.16);
    HbImgFree(&logo);
    lx = DASH_ASSET_BACKGROUND_W - s.w - 4;
    ly = DASH_ASSET_BACKGROUND_H - s.h - 4;
    for (y = 0; y < DASH_ASSET_BACKGROUND_H; y++) {
        for (x = 0; x < DASH_ASSET_BACKGROUND_W; x++) {
            unsigned char p[4];
            if (checker == NULL) {
                memcpy(p, kBase, 4);
            } else {
                memcpy(p, lightTile[(y & 15) * 16 + (x & 15)] ? light : dark, 4);
            }
            if (s.px != NULL && x >= lx && y >= ly && x < lx + s.w && y < ly + s.h) {
                HbBlend(p, s.px + 4 * ((y - ly) * s.w + (x - lx)));
            }
            gDashAssetBackground[y * DASH_ASSET_BACKGROUND_W + x] = HbPack565(p);
        }
    }
    HbImgFree(&s);
}

int gdx3ds_hudbake_run(GdxHudBakeReadFn read, void* readUser, GdxHudBakeLogFn log, void* logUser,
                       GdxHudBakeStats* stats) {
    HbCtx c;
    int i;
    memset(&c, 0, sizeof(c));
    c.read = read;
    c.readUser = readUser;
    c.log = log;
    c.logUser = logUser;
    sBakeKeyCount = 0;
    sBakeKeysOverflow = 0;
    if (!HbEnsureArena()) {
        /* No buffers to bake into: every element stays undrawn (NULL), nothing is marked done. */
        HbLog(&c, "[hudbake] out of memory for the %u-byte arena: the DASH art stays blank",
              (unsigned) gdx3ds_hudbake_arena_bytes());
        sBakeMissing = -1;
        c.stats.missing = -1;
        if (stats != NULL) {
            *stats = c.stats;
        }
        return -1;
    }
    c.buf = (unsigned char*) HbMalloc(HB_READ_CAP);
    sBakeDone = 1;
    if (c.buf == NULL || read == NULL) {
        /* Nothing can be read: every recipe still runs, so every element comes out blank. */
        HbLog(&c, "[hudbake] %s: the DASH art stays blank", read == NULL ? "no archive reader" : "out of memory");
    }
    HbBakeGlyphs(&c);
    HbBakeBackground(&c);
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        HbBakeWidget(&c, i);
    }
    HbFree(c.buf);
    sBakeMissing = (read != NULL && c.buf != NULL) ? c.stats.missing : -1;
    if (stats != NULL) {
        *stats = c.stats;
    }
    return c.stats.missing;
}

/* The non-glyph records, in the order the background and widget recipes read them (the host
 * tests compare this list with what a bake actually reads). */
static const char* const kHbChromeKeys[] = {
    "machine_custom_gfx/aDecalCheckeredTex",    "common_assets_compressed/aTitleLogoTex",
    "hud_gfx/aPortraitPositionFirstTex",        "hud_gfx/aPortraitPositionSecondTex",
    "hud_gfx/aPortraitPositionThirdTex",        "hud_gfx/aCharacterPortraitHighlightBorderTex",
    "hud_gfx/aPortraitPositionFourthTex",       "hud_gfx/aPortraitPositionFifthTex",
    "machine_custom_gfx/aLapTex",               "hud_gfx/aHudTimeTex",
    "common_assets_compressed/aBestTex",        "hud_gfx/aHudEnergyTex",
};
#define HB_CHROME_KEYS ((int) (sizeof(kHbChromeKeys) / sizeof(kHbChromeKeys[0])))

int gdx3ds_hudbake_list_keys(char (*keys)[GDX3DS_HUDBAKE_KEY_LEN], int cap) {
    int n = 0;
    int i;
    for (i = 0; i < HB_GLYPH_TEX_COUNT; i++, n++) {
        HbGlyphTex t;
        if (keys != NULL && n < cap) {
            HbGlyphTexAt(i, keys[n], GDX3DS_HUDBAKE_KEY_LEN, &t);
        }
    }
    for (i = 0; i < HB_CHROME_KEYS; i++, n++) {
        if (keys != NULL && n < cap) {
            snprintf(keys[n], GDX3DS_HUDBAKE_KEY_LEN, "%s", kHbChromeKeys[i]);
        }
    }
    return n;
}

const char* gdx3ds_hudbake_read_key(int i) {
    return (i >= 0 && i < sBakeKeyCount) ? sBakeKeys[i] : NULL;
}

int gdx3ds_hudbake_done(void) {
    return sBakeDone;
}

static uint32_t HbFnv(uint32_t h, const unsigned short* px, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ (px[i] & 0xFFu)) * 16777619u;
        h = (h ^ (px[i] >> 8)) * 16777619u;
    }
    return h;
}

unsigned int gdx3ds_hudbake_digest(void) {
    const DashGlyph* const tables[] = {kDashGlyphTimer, kDashGlyphLap,   kDashGlyphPosDigit,   kDashGlyphPosSuffix,
                                       kDashGlyphFont1, kDashGlyphFont2, kDashGlyphPortrait,   kDashGlyphKoStar,
                                       kDashGlyphLives, kDashGlyphSpeedDigit, kDashGlyphKmh};
    const int counts[] = {KDASHGLYPHTIMER_COUNT,      KDASHGLYPHLAP_COUNT,    KDASHGLYPHPOSDIGIT_COUNT,
                          KDASHGLYPHPOSSUFFIX_COUNT,  KDASHGLYPHFONT1_COUNT,  KDASHGLYPHFONT2_COUNT,
                          KDASHGLYPHPORTRAIT_COUNT,   KDASHGLYPHKOSTAR_COUNT, KDASHGLYPHLIVES_COUNT,
                          KDASHGLYPHSPEEDDIGIT_COUNT, KDASHGLYPHKMH_COUNT};
    uint32_t h = 2166136261u;
    int i;
    if (sArena == NULL) {
        return 0;
    }
    for (i = 0; i < (int) (sizeof(counts) / sizeof(counts[0])); i++) {
        int g;
        for (g = 0; g < counts[i]; g++) {
            h = HbFnv(h, tables[i][g].pixels, (size_t) tables[i][g].w * tables[i][g].h);
        }
    }
    h = HbFnv(h, gDashAssetBackground, DASH_ASSET_BACKGROUND_W * DASH_ASSET_BACKGROUND_H);
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        h = HbFnv(h, kDashAssetWidgets[i].pixels, (size_t) kDashAssetWidgets[i].w * kDashAssetWidgets[i].h);
    }
    return h;
}

/* ---- SD cache: the baked buffers, keyed by the archive entries they were baked from -------- */

#define HB_CACHE_MAGIC0 0x48584447u /* "GDXH" */
#define HB_CACHE_MAGIC1 0x31425544u /* "UDB1" */
#define HB_CACHE_VERSION 3u /* 3: content-hashed recipe id (the PC pack shares the format) */

typedef struct HbCacheHeader {
    uint32_t magic0, magic1;
    uint32_t version;
    uint32_t recipeTag;  /* GDX3DS_HUDBAKE_RECIPE_ID: these recipes' sources (a changed recipe never reads an old cache) */
    uint32_t keyCount;
    uint32_t keysBytes;  /* NUL-terminated archive keys, padded to 4 */
    uint32_t sourceHash; /* the keys' CRC-32 + size fingerprints in the mounted archives */
    uint32_t blobWords;  /* the pixels, decoded */
    uint32_t blobSum;    /* ... and their checksum */
    uint32_t packedWords; /* the pixels as stored (HbLzEncode); the file pads them to 4 bytes */
} HbCacheHeader;

static uint32_t HbStrHash(uint32_t h, const char* str) {
    for (; *str != '\0'; str++) {
        h = (h ^ (unsigned char) *str) * 16777619u;
    }
    return h;
}

/* The recipes' identity: a hash of gdx3ds_hudbake.c + .h (line endings normalised) that the
 * build computes (port/3ds/hudbake_recipe_id.cmake), identical for the console build and the PC
 * tool (tools/hudpack) so a pack baked on the PC is accepted by exactly the builds whose recipes
 * would bake the same pixels. A build without it (the ad-hoc host tests) gets a fixed tag that
 * no CMake-built console or tool ever carries. */
#ifndef GDX3DS_HUDBAKE_RECIPE_ID
#define GDX3DS_HUDBAKE_RECIPE_ID 0x00000000u
#endif

static uint32_t HbRecipeTag(void) {
    return (uint32_t) (GDX3DS_HUDBAKE_RECIPE_ID);
}

unsigned int gdx3ds_hudbake_recipe_id(void) {
    return HbRecipeTag();
}

/* Two 16-bit words per step: cheap enough to check every cached load. */
static uint32_t HbBlobSum(const unsigned short* px, size_t words) {
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i + 1 < words; i += 2) {
        h = (h ^ ((uint32_t) px[i] | ((uint32_t) px[i + 1] << 16))) * 16777619u;
    }
    if (i < words) {
        h = (h ^ px[i]) * 16777619u;
    }
    return h;
}

/* A minimal LZ77 over 16-bit words, so a cached load reads ~1/4 of the bytes (the SD card, not
 * the decode, is what costs on an Old 3DS). The stream is a sequence of tokens:
 *   0x0001..0x7FFF  n literal words follow;
 *   0x8000 | n      (n = 1..0x7FFF) then one word d = 1..0xFFFF: copy n words from d words back
 *                   (d < n overlaps, so d = 1 is a run). */
#define HB_LZ_MAX_LEN 0x7FFFu
#define HB_LZ_MAX_DIST 0xFFFFu
#define HB_LZ_MIN_MATCH 3u
#define HB_LZ_HASH_BITS 13

static size_t HbLzBound(size_t n) {
    return n + n / HB_LZ_MAX_LEN + 2u;
}

static uint32_t HbLzHash(const unsigned short* p) {
    const uint32_t key = (uint32_t) p[0] | ((uint32_t) p[1] << 16);
    return (key * 2654435761u) >> (32 - HB_LZ_HASH_BITS);
}

static size_t HbLzMatchLen(const unsigned short* in, size_t n, size_t pos, size_t from) {
    size_t len = 0;
    while (pos + len < n && len < HB_LZ_MAX_LEN && in[from + len] == in[pos + len]) {
        len++;
    }
    return len;
}

static size_t HbLzFlushLiterals(const unsigned short* in, size_t litStart, size_t litEnd, unsigned short* out,
                                size_t o) {
    while (litStart < litEnd) {
        size_t chunk = litEnd - litStart;
        if (chunk > HB_LZ_MAX_LEN) {
            chunk = HB_LZ_MAX_LEN;
        }
        out[o++] = (unsigned short) chunk;
        memcpy(out + o, in + litStart, chunk * sizeof(*out));
        o += chunk;
        litStart += chunk;
    }
    return o;
}

/* Returns the words written to out (at most HbLzBound(n)), 0 if the hash table is unavailable. */
static size_t HbLzEncode(const unsigned short* in, size_t n, unsigned short* out) {
    uint32_t* table = (uint32_t*) HbMalloc(sizeof(uint32_t) << HB_LZ_HASH_BITS);
    size_t pos = 0;
    size_t lit = 0;
    size_t o = 0;
    if (table == NULL) {
        return 0;
    }
    memset(table, 0xFF, sizeof(uint32_t) << HB_LZ_HASH_BITS);
    while (pos + 1 < n) {
        const uint32_t h = HbLzHash(in + pos);
        const uint32_t cand = table[h];
        size_t best = 0;
        size_t dist = 0;
        table[h] = (uint32_t) pos;
        if (pos >= 1 && in[pos - 1] == in[pos]) {
            best = HbLzMatchLen(in, n, pos, pos - 1);
            dist = 1;
        }
        if (cand != 0xFFFFFFFFu && pos - cand <= HB_LZ_MAX_DIST && pos - cand > 1) {
            const size_t len = HbLzMatchLen(in, n, pos, cand);
            if (len > best) {
                best = len;
                dist = pos - cand;
            }
        }
        if (best < HB_LZ_MIN_MATCH) {
            pos++;
            continue;
        }
        o = HbLzFlushLiterals(in, lit, pos, out, o);
        out[o++] = (unsigned short) (0x8000u | best);
        out[o++] = (unsigned short) dist;
        pos += best;
        lit = pos;
    }
    HbFree(table);
    return HbLzFlushLiterals(in, lit, n, out, o);
}

/* 1 when `in` decodes to exactly outWords words, every copy in bounds; else 0. */
static int HbLzDecode(const unsigned short* in, size_t inWords, unsigned short* out, size_t outWords) {
    size_t i = 0;
    size_t o = 0;
    while (i < inWords) {
        const unsigned token = in[i++];
        const size_t len = token & HB_LZ_MAX_LEN;
        if (len == 0 || o + len > outWords) {
            return 0;
        }
        if ((token & 0x8000u) == 0) {
            if (i + len > inWords) {
                return 0;
            }
            memcpy(out + o, in + i, len * sizeof(*out));
            i += len;
        } else {
            size_t dist;
            size_t k;
            if (i >= inWords) {
                return 0;
            }
            dist = in[i++];
            if (dist == 0 || dist > o) {
                return 0;
            }
            if (dist >= len) {
                memcpy(out + o, out + o - dist, len * sizeof(*out));
            } else {
                for (k = 0; k < len; k++) {
                    out[o + k] = out[o + k - dist];
                }
            }
        }
        o += len;
    }
    return o == outWords;
}

/* FNV over each key and its fingerprint; 0 when any key is in no mounted archive. */
static int HbSourceHash(const char* keys, uint32_t keyCount, GdxHudBakeFingerprintFn fp, void* user,
                        uint32_t* out, const char** badKey) {
    uint32_t h = 2166136261u;
    uint32_t k;
    for (k = 0; k < keyCount; k++) {
        unsigned int print = 0;
        if (fp(keys, &print, user) <= 0) {
            *badKey = keys;
            return 0;
        }
        h = HbStrHash(h, keys);
        h = (h ^ print) * 16777619u;
        keys += strlen(keys) + 1;
    }
    *out = h;
    return 1;
}

static size_t HbKeysBytes(void) {
    size_t n = 0;
    int i;
    for (i = 0; i < sBakeKeyCount; i++) {
        n += strlen(sBakeKeys[i]) + 1;
    }
    return (n + 3u) & ~(size_t) 3u;
}

size_t gdx3ds_hudbake_cache_write(void* dst, size_t cap, GdxHudBakeFingerprintFn fp, void* user) {
    HbCacheHeader hdr;
    const size_t keysBytes = HbKeysBytes();
    const size_t blobWords = HbArenaWords();
    const size_t bound = sizeof(hdr) + keysBytes + ((HbLzBound(blobWords) + 1u) & ~(size_t) 1u) * 2u;
    unsigned char* out = (unsigned char*) dst;
    unsigned char* p;
    const char* badKey = NULL;
    size_t packed;
    int i;
    if (sBakeMissing != 0 || sBakeKeysOverflow || sBakeKeyCount == 0 || fp == NULL || sArena == NULL) {
        return 0; /* only a complete bake from the archive is worth keeping */
    }
    if (dst == NULL) {
        return bound;
    }
    if (cap < bound) {
        return 0;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic0 = HB_CACHE_MAGIC0;
    hdr.magic1 = HB_CACHE_MAGIC1;
    hdr.version = HB_CACHE_VERSION;
    hdr.recipeTag = HbRecipeTag();
    hdr.keyCount = (uint32_t) sBakeKeyCount;
    hdr.keysBytes = (uint32_t) keysBytes;
    hdr.blobWords = (uint32_t) blobWords;
    p = out + sizeof(hdr);
    memset(p, 0, keysBytes);
    for (i = 0; i < sBakeKeyCount; i++) {
        const size_t len = strlen(sBakeKeys[i]) + 1;
        memcpy(p, sBakeKeys[i], len);
        p += len;
    }
    if (!HbSourceHash((const char*) (out + sizeof(hdr)), hdr.keyCount, fp, user, &hdr.sourceHash, &badKey)) {
        return 0;
    }
    /* The arena is the blob: encoded in place, no gathered copy. */
    hdr.blobSum = HbBlobSum(sArena, blobWords);
    packed = HbLzEncode(sArena, blobWords, (unsigned short*) (out + sizeof(hdr) + keysBytes));
    if (packed == 0) {
        return 0;
    }
    hdr.packedWords = (uint32_t) packed;
    if ((packed & 1u) != 0) {
        ((unsigned short*) (out + sizeof(hdr) + keysBytes))[packed] = 0;
        packed++;
    }
    memcpy(out, &hdr, sizeof(hdr));
    return sizeof(hdr) + keysBytes + packed * 2u;
}

static int HbCacheReject(GdxHudBakeLogFn log, void* logUser, const char* why) {
    if (log != NULL) {
        char line[160];
        snprintf(line, sizeof(line), "[hudbake] cache not used: %s", why);
        log(line, logUser);
    }
    return 0;
}

static int HbKeysTerminated(const unsigned char* p, const unsigned char* end, uint32_t keyCount) {
    uint32_t k;
    for (k = 0; k < keyCount; k++) {
        const void* nul = memchr(p, '\0', (size_t) (end - p));
        if (nul == NULL) {
            return 0;
        }
        p = (const unsigned char*) nul + 1;
    }
    return 1;
}

int gdx3ds_hudbake_cache_read(const void* src, size_t n, GdxHudBakeFingerprintFn fp, void* user,
                              GdxHudBakeLogFn log, void* logUser) {
    const unsigned char* in = (const unsigned char*) src;
    HbCacheHeader hdr;
    const char* keys;
    const char* badKey = NULL;
    uint32_t sourceHash = 0;
    int ok;
    if (src == NULL || n < sizeof(hdr) || fp == NULL) {
        return HbCacheReject(log, logUser, "none");
    }
    memcpy(&hdr, in, sizeof(hdr));
    if (hdr.magic0 != HB_CACHE_MAGIC0 || hdr.magic1 != HB_CACHE_MAGIC1) {
        return HbCacheReject(log, logUser, "not a cache file");
    }
    if (hdr.version != HB_CACHE_VERSION) {
        /* ship6: a cache from an older (or newer) build's format is expected after an update,
         * not a corrupt file: say which, so a version bump does not read like damage */
        char why[64];
        snprintf(why, sizeof(why), "%s cache format (v%lu, want v%u)", hdr.version < HB_CACHE_VERSION ? "old" : "other",
                 (unsigned long) hdr.version, HB_CACHE_VERSION);
        return HbCacheReject(log, logUser, why);
    }
    if (hdr.recipeTag != HbRecipeTag()) {
        return HbCacheReject(log, logUser, "baked by another build");
    }
    if (hdr.blobWords != HbArenaWords() || (hdr.keysBytes & 3u) != 0 || hdr.keyCount == 0 ||
        hdr.keyCount > HB_KEY_MAX || hdr.keysBytes > n || hdr.packedWords > n ||
        n != sizeof(hdr) + (size_t) hdr.keysBytes + (((size_t) hdr.packedWords + 1u) & ~(size_t) 1u) * 2u) {
        return HbCacheReject(log, logUser, "wrong size");
    }
    keys = (const char*) (in + sizeof(hdr));
    if (!HbKeysTerminated((const unsigned char*) keys, in + sizeof(hdr) + hdr.keysBytes, hdr.keyCount)) {
        return HbCacheReject(log, logUser, "corrupt key list");
    }
    if (!HbSourceHash(keys, hdr.keyCount, fp, user, &sourceHash, &badKey)) {
        char why[128];
        snprintf(why, sizeof(why), "%.80s is in no mounted archive", badKey);
        return HbCacheReject(log, logUser, why);
    }
    if (sourceHash != hdr.sourceHash) {
        return HbCacheReject(log, logUser, "the archive changed since it was baked");
    }
    if (!HbEnsureArena()) {
        return HbCacheReject(log, logUser, "out of memory");
    }
    /* Decoded straight into the arena (no staging copy). A stream that fails to decode or to
     * checksum has overwritten part of it, so the arena is cleared: the caller bakes next. */
    ok = HbLzDecode((const unsigned short*) (in + sizeof(hdr) + hdr.keysBytes), hdr.packedWords, sArena,
                    hdr.blobWords);
    if (!ok || HbBlobSum(sArena, hdr.blobWords) != hdr.blobSum) {
        memset(sArena, 0, (size_t) hdr.blobWords * sizeof(*sArena));
        sBakeDone = 0;
        return HbCacheReject(log, logUser, ok ? "checksum mismatch" : "damaged pixel stream");
    }
    sBakeDone = 1;
    sBakeMissing = -1;
    return 1;
}
