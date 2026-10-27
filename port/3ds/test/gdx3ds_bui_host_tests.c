/* port/3ds/test/gdx3ds_bui_host_tests.c — host tests for the bottom-screen UI (bottom-menus design).
 *
 * The kit (gdx3ds_bui_kit.c) is a call-for-call port of the design's mockup engine, so the strongest
 * test is the pixels: this file draws the design's component specimens (and, from M1 on, every
 * product screen through the real screen code with the gallery's canned snapshots), then
 *   - with the user's fzerox.o2r (GDX_O2R, else the Azahar SD copy; the game art is baked from it
 *     at startup exactly as the console does at boot, gdx3ds_hudbake.h): compares an FNV-1a hash
 *     of each RGB565 screen against the golden value recorded when the screen last matched the
 *     mockup pixel for pixel (GOLDEN below). Without the archive the screens still render (blank
 *     game glyphs) and the hash checks are skipped;
 *   - with GDX_BUI_MOCKUP_DIR=<dir of the design's *.raw renders>: diffs every pixel against the
 *     mockup itself (outside the per-screen masks for parts that deliberately differ) and prints
 *     the count, which must be 0;
 *   - with GDX_BUI_DUMP_DIR=<dir>: writes each screen as <id>.bmp for the comparison page.
 * Plus the pure logic: dirty spans, text metrics, UTF-8, the checker, modal geometry, hit tests,
 * focus navigation, the dev-console combo, the key mask and string lint (gdx3ds_bui_layout.h). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx3ds_hudbake.h"
#include "gdx3ds_bottomhud_idle_assets.h"
#include "gdx3ds_bui_glyph_table.h"
#include "gdx3ds_bui_kit.h"
#include "gdx3ds_bui_layout.h"

#ifdef GDX_BUI_HOST_SCREENS
#include "gdx3ds_bui_gallery.h"
#endif
#include "test/gdx3ds_o2r_host_reader.h"

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        gChecks++;                                        \
        if (!(cond)) {                                    \
            gFailures++;                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static uint16_t gFb[BUI_W * BUI_H];
static int gHaveArt = 0; /* the game art baked from a real fzerox.o2r: pixel goldens apply */

/* ---- screen I/O -------------------------------------------------------------------------- */

static uint32_t HashScreen(void) {
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < (size_t) BUI_W * BUI_H; i++) {
        h = (h ^ (gFb[i] & 0xFFu)) * 16777619u;
        h = (h ^ (gFb[i] >> 8)) * 16777619u;
    }
    return h;
}

static void WriteBmp(const char* dir, const char* id) {
    char path[1024];
    FILE* f;
    unsigned char hdr[54];
    const uint32_t rowBytes = BUI_W * 3;
    const uint32_t size = 54 + rowBytes * BUI_H;
    int y;
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, id);
    f = fopen(path, "wb");
    if (f == NULL) {
        printf("note: cannot write %s\n", path);
        return;
    }
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (unsigned char) (BUI_W & 255);
    hdr[19] = (unsigned char) (BUI_W >> 8);
    hdr[22] = (unsigned char) (BUI_H & 255);
    hdr[23] = (unsigned char) (BUI_H >> 8);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = BUI_H - 1; y >= 0; y--) {
        int x;
        for (x = 0; x < BUI_W; x++) {
            int r, g, b;
            unsigned char px[3];
            bui_get(x, y, &r, &g, &b);
            px[0] = (unsigned char) b;
            px[1] = (unsigned char) g;
            px[2] = (unsigned char) r;
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
}

typedef struct MaskRect {
    int x, y, w, h;
} MaskRect;

static int Masked(const MaskRect* masks, int n, int x, int y) {
    int i;
    for (i = 0; i < n; i++) {
        if (x >= masks[i].x && x < masks[i].x + masks[i].w && y >= masks[i].y && y < masks[i].y + masks[i].h) {
            return 1;
        }
    }
    return 0;
}

/* -1 = no mockup to compare with; else the differing pixel count outside the masks. */
static long DiffMockup(const char* dir, const char* id, const MaskRect* masks, int nMasks) {
    char path[1024];
    static unsigned char raw[BUI_W * BUI_H * 4];
    FILE* f;
    long diff = 0;
    int y;
    snprintf(path, sizeof(path), "%s/%s.raw", dir, id);
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    for (y = 0; y < BUI_H; y++) {
        int x;
        for (x = 0; x < BUI_W; x++) {
            const unsigned char* m = raw + ((size_t) y * BUI_W + x) * 4;
            int r, g, b;
            if (Masked(masks, nMasks, x, y)) {
                continue;
            }
            bui_get(x, y, &r, &g, &b);
            if (r != m[0] || g != m[1] || b != m[2]) {
                if (diff < 3) {
                    printf("  %s: (%d,%d) got %d,%d,%d mockup %d,%d,%d\n", id, x, y, r, g, b, m[0], m[1], m[2]);
                }
                diff++;
            }
        }
    }
    return diff;
}

/* ---- the component specimens (screens.js specType / specButtons / ...) -------------------- */

static BuiText Tinted(BuiRgb tint, int solid) {
    BuiText t;
    memset(&t, 0, sizeof(t));
    t.tint = tint;
    t.solid = solid;
    return t;
}

static void SpecType(void) {
    const BuiText orange = Tinted(BUI_TK_ORANGE, 0);
    const BuiText mute = Tinted(BUI_TK_MUTE, 1);
    BuiSprite half;
    BuiText t;
    int i;
    bui_checker(0, 0, BUI_W, BUI_H);
    bui_text(BUI_F1, "FONT1 TITLE 16PX", 6, 6, NULL);
    bui_text(BUI_F1H, "FONT1 HALF 8PX (DASH COURSE NAME)", 6, 28, NULL);
    bui_text(BUI_F2X2, "FONT2 X2 16", 6, 42, NULL);
    bui_text(BUI_F2, "FONT2 8PX BODY AND META, ALL CAPS", 6, 64, NULL);
    bui_text(BUI_F2, "NEW: ' , & / : ! ? + ( ) # % ✓ × ◀ ▶ · …", 6, 78, &orange);
    bui_text(BUI_FT, "1'07\"45", 6, 94, NULL);
    bui_text(BUI_F2, "TIMER 8X16 (CYAN, BAKED)", 70, 98, &mute);
    bui_text(BUI_FP, "3", 6, 116, NULL);
    bui_sprite(BUI_SET_POSSUFFIX, "RD", 34, 118, NULL);
    bui_text(BUI_FPH, "12", 70, 124, NULL);
    memset(&half, 0, sizeof(half));
    half.num = 1;
    half.den = 2;
    bui_sprite(BUI_SET_POSSUFFIX, "TH", 98, 126, &half);
    bui_text(BUI_F2, "POSITION DIGITS 28X32 AND 1/2", 120, 130, &mute);
    for (i = 0; i < 8; i++) {
        char key[4];
        snprintf(key, sizeof(key), "%d", i * 3);
        bui_sprite(BUI_SET_PORTRAIT, key, 6 + i * 38, 156, NULL);
    }
    bui_text(BUI_F2, "30 PILOT PORTRAITS 32X32, ALREADY BAKED", 6, 194, &mute);
    bui_text(BUI_F2, "TINTED: ", 6, 212, &mute);
    t = Tinted(BUI_TK_GREEN, 1);
    bui_text(BUI_F2, "READY", 70, 212, &t);
    t = Tinted(BUI_TK_ORANGE, 1);
    bui_text(BUI_F2, "WAIT", 120, 212, &t);
    t = Tinted(BUI_TK_RED, 1);
    bui_text(BUI_F2, "ERROR", 162, 212, &t);
    t = Tinted(BUI_TK_CYAN, 1);
    bui_text(BUI_F2, "INFO", 212, 212, &t);
    t = Tinted(BUI_TK_FOCUS, 1);
    bui_text(BUI_F2, "FOCUS", 252, 212, &t);
}

static void SpecButtons(void) {
    static const int kStates[5] = {BUI_BTN_NORMAL, BUI_BTN_PRIMARY, BUI_BTN_GHOST, BUI_BTN_DANGER, BUI_BTN_DISABLED};
    static const char* const kLabels[5] = {"NORMAL", "PRIMARY", "GHOST", "DANGER", "DISABLED"};
    BuiButton b;
    int i;
    bui_fill(0, 0, BUI_W, BUI_H, BUI_TK_BLACK);
    bui_checker(0, 0, BUI_W, BUI_H);
    for (i = 0; i < 5; i++) {
        memset(&b, 0, sizeof(b));
        b.state = kStates[i];
        b.glyph = kStates[i] == BUI_BTN_PRIMARY ? "A" : kStates[i] == BUI_BTN_GHOST ? "B" : NULL;
        bui_button(8, 8 + i * 46, 148, 40, kLabels[i], &b);
    }
    memset(&b, 0, sizeof(b));
    b.focus = 1;
    b.glyph = "A";
    bui_button(166, 8, 146, 40, "FOCUSED", &b);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_PRESSED;
    bui_button(166, 54, 146, 40, "PRESSED", &b);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_DISABLED;
    b.sub = "WAITING FOR 1 PLAYER";
    bui_button(166, 100, 146, 40, "WITH REASON", &b);
    /* (166,146) "ICON" uses the camera icon, a courses-only icon this build leaves out: masked */
    memset(&b, 0, sizeof(b));
    b.font = BUI_F2;
    bui_button(166, 192, 70, 40, "◀", &b);
    b.state = BUI_BTN_GHOST;
    bui_button(242, 192, 70, 40, "SMALL", &b);
}

static void SpecRows(void) {
    static const BuiRowBadge kLocal[1] = {{"LOCAL", BUI_BADGE_INFO}};
    static const BuiRowBadge kBad[1] = {{"BAD", BUI_BADGE_BAD}};
    BuiRow r;
    BuiSlot s;
    bui_checker(0, 0, BUI_W, BUI_H);
    memset(&r, 0, sizeof(r));
    r.portrait = BUI_PORTRAIT(3);
    r.title = "KENJI'S ROOM";
    r.meta = "2/4 · MUTE CITY";
    r.badges = kLocal;
    r.badgeCount = 1;
    r.bars = BUI_BARS(3);
    r.focus = 1;
    bui_row(4, 4, 312, 40, &r);
    /* (4,48) is a course row with a track thumbnail (courses flow, not built): masked */
    memset(&r, 0, sizeof(r));
    r.icon = BUI_ICON_WARN;
    r.iconColor = BUI_TK_RED;
    r.iconFill = BUI_TK_BAD_FILL;
    r.title = "TRACK1";
    r.meta = "FILE IS DAMAGED";
    r.metaTint = BUI_TK_RED;
    r.badges = kBad;
    r.badgeCount = 1;
    bui_row(4, 92, 312, 40, &r);
    memset(&s, 0, sizeof(s));
    s.name = "MIKE";
    s.pilot = 0;
    s.sub = "FALCON · O3DS";
    s.state = BUI_SLOT_READY;
    s.host = s.me = s.you = 1;
    bui_slot(4, 138, 188, 38, &s);
    memset(&s, 0, sizeof(s));
    s.name = "ANA";
    s.pilot = 4;
    s.sub = "GETTING COURSE";
    s.state = BUI_SLOT_GETTING;
    s.pct = 60;
    s.subTint = BUI_TK_CYAN;
    bui_slot(4, 180, 188, 38, &s);
    memset(&s, 0, sizeof(s));
    s.empty = 1;
    s.label = "OPEN";
    bui_slot(196, 138, 120, 38, &s);
    memset(&s, 0, sizeof(s));
    s.name = "LEO";
    s.pilot = 13;
    s.sub = "LEON";
    s.state = BUI_SLOT_AWAY;
    s.away = 1;
    bui_slot(196, 180, 120, 38, &s);
}

static void SpecBadges(void) {
    static const struct {
        const char* t;
        int k;
    } kBadges[16] = {
        {"LOCAL", BUI_BADGE_INFO}, {"WIFI", BUI_BADGE_CYANLINE}, {"ONLINE", BUI_BADGE_CYANLINE},
        {"READY✓", BUI_BADGE_OK}, {"SERVER OK", BUI_BADGE_OK}, {"AWAY", BUI_BADGE_WARN},
        {"RACING", BUI_BADGE_WARN}, {"FULL", BUI_BADGE_MUTED}, {"VERSION", BUI_BADGE_BAD},
        {"NO DISK", BUI_BADGE_BAD}, {"CUSTOM", BUI_BADGE_EK}, {"JP EK DISK", BUI_BADGE_EK},
        {"O3DS", BUI_BADGE_MODEL}, {"N3DS", BUI_BADGE_MODEL}, {"CUP 1", BUI_BADGE_PINK},
        {"YOU", BUI_BADGE_YELLOW},
    };
    const BuiText mute = Tinted(BUI_TK_MUTE, 1);
    int i;
    bui_checker(0, 0, BUI_W, BUI_H);
    for (i = 0; i < 16; i++) {
        bui_badge(8 + (i % 4) * 78, 10 + (i / 4) * 20, kBadges[i].t, kBadges[i].k);
    }
    for (i = 0; i < 4; i++) {
        bui_bars(10 + i * 24, 98, 3 - i);
    }
    bui_text(BUI_F2, "SIGNAL 3/2/1/0", 110, 100, &mute);
    bui_spinner(240, 104, 2);
    bui_progress(8, 120, 304, 10, 0.6, BUI_TK_GREEN);
    bui_toast(140, "ANA JOINED THE ROOM", BUI_TONE_OK, BUI_ICON_PEOPLE);
    bui_toast(168, "KENJI'S ROOM IS FULL (4/4)", BUI_TONE_WARN, BUI_ICON_PEOPLE);
    /* (196) the SD-card toast uses the courses-only sd icon: masked */
}

static void SpecControls(void) {
    static const char* const kMachines[5] = {"OFF", "4", "8", "16", "30"};
    static const char* const kTabs[2] = {"DISK 12", "SD 5"};
    static const char* const kGlyphs[8] = {"A", "B", "X", "Y", "L", "R", "ST", "SE"};
    static const BuiHint kHints[3] = {{"A", "JOIN"}, {"Y", "HOST"}, {"B", "BACK"}};
    const BuiText mute = Tinted(BUI_TK_MUTE, 1);
    BuiSeg seg;
    BuiStepper st;
    int i, x = 8;
    bui_checker(0, 0, BUI_W, BUI_H);
    memset(&seg, 0, sizeof(seg));
    seg.offMask = 1 << 4;
    bui_seg(4, 6, 312, 32, kMachines, 5, 3, &seg);
    memset(&seg, 0, sizeof(seg));
    seg.font = BUI_F2;
    bui_seg(4, 46, 204, 32, kTabs, 2, 0, &seg);
    memset(&st, 0, sizeof(st));
    st.focus = 1;
    bui_stepper(4, 86, 312, 36, "LAPS", "3", &st);
    memset(&st, 0, sizeof(st));
    st.font = BUI_F2;
    bui_stepper(4, 128, 312, 36, "CPU LEVEL", "STANDARD", &st);
    for (i = 0; i < 8; i++) {
        x += bui_btn_glyph(x, 176, kGlyphs[i], 0, 0) + 8;
    }
    bui_text(BUI_F2, "BUTTON GLYPHS (FOOTER HINTS)", 8, 196, &mute);
    bui_footer(kHints, 3, "1/2");
}

typedef struct Screen {
    const char* id;
    void (*draw)(void);
    uint32_t golden; /* 0 = not recorded yet (prints the hash) */
    MaskRect masks[4];
    int maskCount;
} Screen;

static const Screen kSpecScreens[] = {
    {"spec-type", SpecType, 0xF599666Au, {{0}}, 0},
    {"spec-buttons", SpecButtons, 0x9B8E1051u, {{164, 144, 150, 44}}, 1},
    {"spec-rows", SpecRows, 0xCE1CCF8Bu, {{2, 46, 316, 44}}, 1},
    {"spec-badges", SpecBadges, 0x1C84EABFu, {{6, 196, 308, 24}}, 1},
    {"spec-controls", SpecControls, 0x6D1BCF28u, {{0}}, 0},
};

static void RunScreen(const char* id, void (*draw)(void), uint32_t golden, const MaskRect* masks, int maskCount) {
    const char* mock = getenv("GDX_BUI_MOCKUP_DIR");
    const char* dump = getenv("GDX_BUI_DUMP_DIR");
    uint32_t h;
    memset(gFb, 0, sizeof(gFb));
    bui_kit_begin(gFb);
    bui_kit_dirty_clear();
    draw();
    h = HashScreen();
    if (!gHaveArt) {
        /* no game art: nothing to compare the pixels against */
    } else if (golden == 0) {
        printf("note: %s hash 0x%08Xu (no golden yet)\n", id, h);
    } else {
        CHECK(h == golden, "%s: hash 0x%08X, golden 0x%08X (re-diff against the mockup)", id, h, golden);
    }
    if (mock != NULL && gHaveArt) {
        const long d = DiffMockup(mock, id, masks, maskCount);
        if (d >= 0) {
            printf("mockup %-22s %ld px differ\n", id, d);
            CHECK(d == 0, "%s: %ld pixels differ from the mockup", id, d);
        }
    }
    if (dump != NULL) {
        WriteBmp(dump, id);
    }
}

static void TestSpecScreens(void) {
    size_t i;
    for (i = 0; i < sizeof(kSpecScreens) / sizeof(kSpecScreens[0]); i++) {
        const Screen* s = &kSpecScreens[i];
        RunScreen(s->id, s->draw, s->golden, s->masks, s->maskCount);
    }
}

/* ---- kit unit checks ---------------------------------------------------------------------- */

static void TestDirtySpan(void) {
    int x0 = -1, x1 = -1;
    bui_kit_begin(gFb);
    bui_kit_dirty_clear();
    CHECK(!bui_kit_dirty(&x0, &x1), "clean after clear");
    bui_fill(10, 5, 4, 4, BUI_TK_PINK);
    bui_fill(-5, 0, 7, 1, BUI_TK_PINK);
    CHECK(bui_kit_dirty(&x0, &x1) && x0 == 0 && x1 == 13, "dirty span %d..%d", x0, x1);
    bui_kit_dirty_clear();
    bui_fill(400, 0, 4, 4, BUI_TK_PINK);
    bui_fill(10, 300, 4, 4, BUI_TK_PINK);
    CHECK(!bui_kit_dirty(&x0, &x1), "off-screen writes leave it clean");
    bui_set(319, 239, 255, 255, 255);
    CHECK(bui_kit_dirty(&x0, &x1) && x0 == 319 && x1 == 319, "corner pixel");
    CHECK(gFb[319 * 240 + 0] == 0xFFFF, "column-major layout: (319,239) is index 319*240");
}

static void TestColumnLayout(void) {
    bui_kit_begin(gFb);
    memset(gFb, 0, sizeof(gFb));
    bui_set(1, 0, 255, 0, 0);
    CHECK(gFb[1 * 240 + 239] == 0xF800, "(1,0) -> index 1*240+239");
    bui_fill(0, 0, 1, 240, BUI_TK_WHITE);
    CHECK(gFb[0] == 0xFFFF && gFb[239] == 0xFFFF && gFb[240] == 0, "a column fill stays in its column");
}

static void TestQuantise(void) {
    int r, g, b;
    bui_kit_begin(gFb);
    bui_set(0, 0, 0xFF, 0x6D, 0xB5);
    bui_get(0, 0, &r, &g, &b);
    CHECK(r == 255 && g == 109 && b == 181, "pink quantises to %d,%d,%d", r, g, b);
    bui_set(0, 0, 300, -4, 7);
    bui_get(0, 0, &r, &g, &b);
    CHECK(r == 255 && g == 0 && b == 0, "clamped %d,%d,%d", r, g, b);
}

static void TestMeasure(void) {
    CHECK(bui_measure(BUI_F2, "") == 0, "empty");
    CHECK(bui_measure(BUI_F2, " ") == 4, "F2 space is 4");
    CHECK(bui_measure(BUI_F1, " ") == 7, "F1 space is 7");
    CHECK(bui_measure(BUI_F2, "a") == bui_measure(BUI_F2, "A"), "lower case falls back to capitals");
    CHECK(bui_measure(BUI_F2X2, "AB") == 2 * bui_measure(BUI_F2, "AB"), "F2x2 doubles");
    CHECK(bui_measure(BUI_F2, "\xE2\x9C\x93") > 0, "check mark is an extra glyph");
    CHECK(bui_measure(BUI_F2, "\xE2\x82\xAC") == 4, "an unknown code point advances a space");
    CHECK(bui_measure(BUI_F2, "\xC3") == 4, "a truncated UTF-8 byte advances a space");
    CHECK(bui_badge_width("YOU") == bui_measure(BUI_F2, "YOU") + 6, "badge width");
}

static void TestModalLayout(void) {
    static const char* const kBody[2] = {"ONE", "TWO"};
    static const BuiModalButton kButtons[2] = {{"STAY", "B", BUI_BTN_GHOST, 0, 0, NULL, 0}, {"CLOSE", "A", BUI_BTN_DANGER, 0, 0, NULL, 0}};
    BuiModal m;
    BuiRect rects[3];
    BuiRect r;
    memset(&m, 0, sizeof(m));
    m.title = "CLOSE THE ROOM?";
    m.body = kBody;
    m.bodyCount = 2;
    m.buttons = kButtons;
    m.buttonCount = 2;
    r = bui_modal_layout(&m, rects);
    CHECK(r.w == 288 && r.h == 30 + 24 + 10 + 40 + 8, "modal size %dx%d", r.w, r.h);
    CHECK(r.x == 16 && r.y == (240 - r.h) / 2, "modal origin %d,%d", r.x, r.y);
    CHECK(rects[0].x == 24 && rects[1].x == rects[0].x + rects[0].w + 6, "button rects");
    CHECK(rects[1].x + rects[1].w <= r.x + r.w - 8, "buttons inside the frame");
}

static void TestSegRect(void) {
    const BuiRect a = bui_seg_rect(4, 6, 312, 32, 5, 0);
    const BuiRect e = bui_seg_rect(4, 6, 312, 32, 5, 4);
    CHECK(a.x == 4 && a.w == 62, "first segment");
    CHECK(e.x == 4 + 4 * 62 && e.x + e.w == 316, "last segment takes the remainder");
}

static void TestCheckerMatchesFormula(void) {
    int x, y, bad = 0;
    int ar, ag, ab, br, bg, bb;
    bui_kit_begin(gFb);
    bui_checker(0, 0, BUI_W, BUI_H);
    bui_get(0, 0, &ar, &ag, &ab);
    bui_get(8, 0, &br, &bg, &bb);
    for (y = 0; y < BUI_H; y++) {
        for (x = 0; x < BUI_W; x++) {
            int r, g, b;
            const int odd = ((x >> 3) + (y >> 3)) & 1;
            bui_get(x, y, &r, &g, &b);
            if (r != (odd ? br : ar) || g != (odd ? bg : ag) || b != (odd ? bb : ab)) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "checker pattern: %d bad pixels", bad);
    CHECK(!(ar == br && ag == bg && ab == bb), "two distinct checker colours");
}

#ifdef GDX_BUI_HOST_SCREENS
static const BuiView* sGalleryView = NULL;
static BuiWidgets sGalleryWidgets;

static BuiGalleryDraw sGalleryDraw = NULL;

static void DrawGalleryView(void) {
    if (sGalleryDraw != NULL) {
        bui_widgets_clear(&sGalleryWidgets);
        sGalleryDraw();
        return;
    }
    bui_screen_paint(sGalleryView, &sGalleryWidgets);
}

static void CheckWidgets(const char* id, const BuiWidgets* ws) {
    int i;
    for (i = 0; i < ws->count; i++) {
        const BuiWidget* b = &ws->w[i];
        CHECK(b->x >= 0 && b->y >= 0 && b->x + b->w <= BUI_W && b->y + b->h <= BUI_H, "%s: widget %d off screen", id, b->id);
        CHECK(b->w >= 16 && b->h >= 12, "%s: widget %d is %dx%d, too small to touch", id, b->id, b->w, b->h);
    }
}

/* GDX_BUI_DUMP_DIR: manifest.tsv, one line per gallery screen and one per widget, for the
 * comparison page and the scripted-input touch coordinates. */
static void WriteJsonString(FILE* f, const char* s) {
    fputc('"', f);
    for (; s != NULL && *s != '\0'; s++) {
        if (*s == '"' || *s == '\\') {
            fputc('\\', f);
        }
        fputc(*s, f);
    }
    fputc('"', f);
}

static void ManifestEntry(FILE* f, const BuiGalleryEntry* e, const BuiWidgets* ws, int first) {
    int m;
    fprintf(f, "%s{\"id\":", first ? "" : ",\n");
    WriteJsonString(f, e->id);
    fprintf(f, ",\"mockup\":");
    if (e->mockup != NULL) {
        WriteJsonString(f, e->mockup);
    } else {
        fprintf(f, "null");
    }
    fprintf(f, ",\"note\":");
    WriteJsonString(f, e->note);
    fprintf(f, ",\"masks\":[");
    for (m = 0; m < e->maskCount; m++) {
        fprintf(f, "%s[%d,%d,%d,%d]", m ? "," : "", e->masks[m].x, e->masks[m].y, e->masks[m].w, e->masks[m].h);
    }
    fprintf(f, "],\"widgets\":[");
    for (m = 0; m < ws->count; m++) {
        const BuiWidget* w = &ws->w[m];
        fprintf(f, "%s[%d,%d,%d,%d,%d,%d]", m ? "," : "", w->id, w->x, w->y, w->w, w->h, w->flags);
    }
    fprintf(f, "]}");
}

static void TestProductScreens(void) {
    int i;
    FILE* manifest = NULL;
    if (getenv("GDX_BUI_DUMP_DIR") != NULL) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/manifest.json", getenv("GDX_BUI_DUMP_DIR"));
        manifest = fopen(path, "w");
        if (manifest != NULL) {
            fprintf(manifest, "[\n");
        }
    }
    for (i = 0; i < bui_gallery_count(); i++) {
        const BuiGalleryEntry* e = bui_gallery_entry(i);
        BuiView v;
        MaskRect masks[BUI_GALLERY_MAX_MASKS];
        int m;
        bui_gallery_view(i, &v);
        for (m = 0; m < e->maskCount; m++) {
            masks[m].x = e->masks[m].x;
            masks[m].y = e->masks[m].y;
            masks[m].w = e->masks[m].w;
            masks[m].h = e->masks[m].h;
        }
        sGalleryView = &v;
        sGalleryDraw = bui_gallery_draw(i);
        bui_kit_reset_unknown_glyphs();
        if (e->mockup != NULL && strcmp(e->mockup, e->id) != 0) {
            printf("note: %s diffs against %s\n", e->id, e->mockup);
        }
        RunScreen(e->id, DrawGalleryView, e->golden, masks, e->mockup != NULL ? e->maskCount : 0);
        CHECK(bui_kit_unknown_glyphs() == 0, "%s: %d characters have no glyph", e->id, bui_kit_unknown_glyphs());
        CheckWidgets(e->id, &sGalleryWidgets);
        if (manifest != NULL) {
            ManifestEntry(manifest, e, &sGalleryWidgets, i == 0);
        }
    }
    if (manifest != NULL) {
        fprintf(manifest, "\n]\n");
        fclose(manifest);
    }
}

/* The shell's overlay-only repaints give the very bytes of a full paint. */
static uint32_t PaintedHash(const BuiView* v, int mode, BuiWidgets* ws) {
    BuiView base = *v;
    memset(gFb, 0, sizeof(gFb));
    bui_kit_begin(gFb);
    if (mode == 0) {
        bui_screen_paint(v, ws);
    } else if (mode == 1) {
        base.dialog = BUI_DLG_NONE;
        bui_screen_paint(&base, ws);
        bui_screen_paint_dialog(v, ws, 1);
    } else {
        base.dialogFocus = (int8_t) (v->dialogFocus == 0 ? 1 : 0);
        bui_screen_paint(&base, ws);
        bui_screen_paint_dialog(v, ws, 0);
    }
    return HashScreen();
}

static void TestOverlayRepaint(void) {
    int i;
    int steps = 0;
    while (!bui_modal_prepare(4096) && steps < 64) {
        steps++;
    }
    CHECK(steps == 15, "the shade table fills in 16 steps of 4096 (took %d)", steps + 1);
    for (i = 0; i < bui_gallery_count(); i++) {
        BuiView v;
        BuiWidgets a, b;
        uint32_t full;
        bui_gallery_view(i, &v);
        if (v.dialog == BUI_DLG_NONE || v.toast[0] != '\0') {
            continue;
        }
        v.dialogFocus = 0;
        full = PaintedHash(&v, 0, &a);
        if (gHaveArt && bui_gallery_entry(i)->golden != 0) {
            BuiView g;
            bui_gallery_view(i, &g);
            CHECK(PaintedHash(&g, 0, &b) == bui_gallery_entry(i)->golden, "%s: the cached shade table changes pixels",
                  bui_gallery_entry(i)->id);
        }
        CHECK(PaintedHash(&v, 1, &b) == full, "%s: base + shaded dialog != full paint", bui_gallery_entry(i)->id);
        CHECK(b.count == a.count, "%s: overlay widgets differ", bui_gallery_entry(i)->id);
        CHECK(PaintedHash(&v, 2, &b) == full, "%s: dialog focus repaint != full paint", bui_gallery_entry(i)->id);
    }
}

/* ---- card-4 version gating: the UPDATE / SERVER / newer-older dialogs fit their panel ---------- */
static void TestVersionDialogs(void) {
    static const int kIds[4] = { BUI_DLG_UPDATE, BUI_DLG_SERVER_OLD, BUI_DLG_VERSION_NEWER, BUI_DLG_VERSION_OLDER };
    int i;
    for (i = 0; i < 4; i++) {
        BuiView v;
        BuiWidgets ws;
        char lines[5][48];
        int n;
        int k;
        int sawName = 0;
        bui_view_init(&v);
        v.screen = BUI_SCR_ONLINE;
        v.dialog = (int8_t) kIds[i];
        snprintf(v.dialogName, sizeof(v.dialogName), "%s", i < 2 ? "65535" : "WWWWWWWWWWWW");
        snprintf(v.dialogMine, sizeof(v.dialogMine), "65534");
        snprintf(v.dialogExtra, sizeof(v.dialogExtra), "EXAMPLE.ORG/UPDATE-THE-GAME-HERE-PLEASE");
        snprintf(v.version, sizeof(v.version), "card-4@2035c52a");
        n = bui_dialog_text(&v, lines);
        CHECK(n >= 4, "dialog %d: %d lines", kIds[i], n);
        for (k = 0; k < n; k++) {
            CHECK(bui_measure(BUI_F2, lines[k]) <= 264, "dialog %d line %d too wide: %s", kIds[i], k, lines[k]);
            sawName |= strstr(lines[k], v.dialogName) != NULL;
        }
        CHECK(sawName, "dialog %d never names its subject", kIds[i]);
        if (kIds[i] == BUI_DLG_UPDATE) {
            CHECK(strcmp(lines[0], "THIS SERVER NEEDS VERSION 65535.") == 0, "update line 0: %s", lines[0]);
            CHECK(strcmp(lines[1], "THIS 3DS HAS VERSION 65534.") == 0, "update line 1: %s", lines[1]);
            CHECK(lines[4][0] != '\0' && strncmp(lines[4], v.dialogExtra, strlen(lines[4])) == 0,
                  "update line 4 (the URL, cut to fit): %s", lines[4]);
            CHECK(strcmp(bui_dialog_title(kIds[i]), "UPDATE REQUIRED") == 0, "update title");
        }
        if (kIds[i] == BUI_DLG_SERVER_OLD) {
            CHECK(strcmp(bui_dialog_title(kIds[i]), "SERVER NEEDS AN UPDATE") == 0, "server-old title");
        }
        CHECK(bui_dialog_buttons(kIds[i]) == 1, "dialog %d: one OK button", kIds[i]);
        memset(gFb, 0, sizeof(gFb));
        bui_kit_begin(gFb);
        bui_screen_paint(&v, &ws);
        CHECK(ws.count >= 1, "dialog %d: its OK button is a widget", kIds[i]);
    }
}

/* ---- hint audit (mp-menus): every key glyph a screen draws names a key that works there ---- */

#define HINTS_MAX 32

typedef struct HintLog {
    int count;
    char key[HINTS_MAX][4];
    int16_t x[HINTS_MAX], y[HINTS_MAX];
} HintLog;

static void LogHint(const char* key, int x, int y, void* ctx) {
    HintLog* log = (HintLog*) ctx;
    if (log->count < HINTS_MAX) {
        snprintf(log->key[log->count], sizeof(log->key[0]), "%s", key);
        log->x[log->count] = (int16_t) x;
        log->y[log->count] = (int16_t) y;
        log->count++;
    }
}

static const BuiWidget* WidgetById(const BuiWidgets* ws, int id) {
    int i;
    for (i = 0; i < ws->count; i++) {
        if (ws->w[i].id == id) {
            return &ws->w[i];
        }
    }
    return NULL;
}

/* A glyph sitting on a widget: A is that widget's own key once it is focused (the D-pad gets it
 * there); any other key must be the one the key map runs for that widget. A disabled widget may
 * keep its glyph (key and tap both do nothing yet). The header's ◀ B cell runs the shell's Back(),
 * which is what B runs on every screen that maps it (CANCEL, DEL, LEAVE are Back() there). */
static void CheckHintOnWidget(const char* id, const BuiView* v, const BuiWidget* w, int key, const char* glyph) {
    if (key == BUI_KEY_A) {
        CHECK(w->flags & BUI_WF_FOCUS, "%s: hint A on widget %d, which A can never reach", id, w->id);
        return;
    }
    if (key == BUI_KEY_B && w->id == BUI_W_BACK) {
        CHECK(bui_screen_key(v, key) != BUI_W_NONE, "%s: the header shows B but B does nothing", id);
        return;
    }
    CHECK(bui_screen_key(v, key) == w->id, "%s: hint %s on widget %d, but %s runs %d", id, glyph, w->id, glyph,
          bui_screen_key(v, key));
}

/* A glyph in a footer or a line of text: its key must run a widget on the screen. */
static void CheckHintFree(const char* id, const BuiView* v, const BuiWidgets* ws, int key, const char* glyph) {
    const int target = bui_screen_key(v, key);
    const BuiWidget* w = WidgetById(ws, target);
    CHECK(w != NULL && !(w->flags & BUI_WF_DISABLED), "%s: hint %s runs widget %d, which is %s", id, glyph, target,
          w == NULL ? "not on the screen" : "disabled");
}

static void CheckHints(const char* id, const BuiView* v, const BuiWidgets* ws, const HintLog* log) {
    int i;
    for (i = 0; i < log->count; i++) {
        const int key = bui_key_from_glyph(log->key[i]);
        const BuiWidget* hit = WidgetById(ws, bui_widgets_hit(ws, log->x[i] + 4, log->y[i] + 4));
        CHECK(key != BUI_KEY_NONE, "%s: hint glyph \"%s\" names no key", id, log->key[i]);
        if (key == BUI_KEY_NONE) {
            continue;
        }
        if (hit != NULL) {
            CheckHintOnWidget(id, v, hit, key, log->key[i]);
        } else {
            CheckHintFree(id, v, ws, key, log->key[i]);
        }
    }
}

/* Paints entry i's screen (and then its dialog on its own) with the hint log on. */
static void AuditEntry(int i, int buttonsOff, int* hintsSeen) {
    const char* id = bui_gallery_entry(i)->id;
    BuiView v, base;
    BuiWidgets ws;
    HintLog log;
    bui_gallery_view(i, &v);
    v.buttonsOff = (int8_t) buttonsOff;
    base = v;
    base.dialog = BUI_DLG_NONE;
    memset(&log, 0, sizeof(log));
    bui_kit_begin(gFb);
    bui_kit_set_hint_sink(LogHint, &log);
    bui_screen_paint(&base, &ws);
    if (!buttonsOff) {
        CheckHints(id, &base, &ws, &log);
    }
    *hintsSeen += log.count;
    if (v.dialog != BUI_DLG_NONE) {
        memset(&log, 0, sizeof(log));
        bui_screen_paint_dialog(&v, &ws, 1);
        if (!buttonsOff) {
            CheckHints(id, &v, &ws, &log);
        }
        *hintsSeen += log.count;
    }
    bui_kit_set_hint_sink(NULL, NULL);
}

static void TestHintAudit(void) {
    int i, key;
    int on = 0, off = 0;
    for (i = 0; i < bui_gallery_count(); i++) {
        BuiView v;
        if (bui_gallery_draw(i) != NULL) {
            continue;
        }
        AuditEntry(i, 0, &on);
        AuditEntry(i, 1, &off);
        bui_gallery_view(i, &v);
        v.buttonsOff = 1;
        for (key = BUI_KEY_A; key <= BUI_KEY_START; key++) {
            CHECK(bui_screen_key(&v, key) == BUI_W_NONE, "%s: buttons off, yet key %d runs something", bui_gallery_entry(i)->id,
                  key);
        }
    }
    CHECK(on > 50, "the audit saw %d hints (the sink is wired)", on);
    CHECK(off == 0, "%d hints drawn while the game has the buttons", off);
}

/* card-4b (hw: "some of the buttons and d-pad nav on the multiplayer menus doesnt work.. seems a
 * bit random"): every screen's D-pad graph. From the screen's first focus, the four directions
 * (the lobby also rests on disabled widgets, as gdx3ds_bui.c's Navigate allows) must reach every
 * focusable enabled widget; a focusable widget must lead somewhere when it is not the only one;
 * and no screen may fill the widget table (a widget past BUI_MAX_WIDGETS is silently dropped:
 * dead to touch and to the D-pad). */
static void AuditFocus(const char* id, const BuiWidgets* ws, int allowDisabled) {
    int seen[BUI_MAX_WIDGETS];
    int queue[BUI_MAX_WIDGETS];
    int head = 0, tail = 0, focusable = 0, reached = 0, i, d;
    static const int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    const int first = bui_widgets_first_focus(ws);
    CHECK(ws->count < BUI_MAX_WIDGETS, "%s: %d widgets fill the table (the last ones are dropped)", id, ws->count);
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < ws->count; i++) {
        if ((ws->w[i].flags & BUI_WF_FOCUS) && !(ws->w[i].flags & BUI_WF_DISABLED)) {
            focusable++;
        }
    }
    if (first < 0) {
        return;
    }
    for (i = 0; i < ws->count; i++) {
        if (ws->w[i].id == first) {
            seen[i] = 1;
            queue[tail++] = i;
            break;
        }
    }
    while (head < tail) {
        const int at = queue[head++];
        int moves = 0;
        for (d = 0; d < 4; d++) {
            const int to = bui_widgets_nav(ws, ws->w[at].id, kDirs[d][0], kDirs[d][1], allowDisabled);
            if (to == ws->w[at].id) {
                continue;
            }
            moves++;
            for (i = 0; i < ws->count; i++) {
                if (ws->w[i].id == to && !seen[i]) {
                    seen[i] = 1;
                    queue[tail++] = i;
                }
            }
        }
        CHECK(moves > 0 || focusable <= 1, "%s: widget %d is a dead end for the D-pad", id, ws->w[at].id);
    }
    for (i = 0; i < ws->count; i++) {
        if ((ws->w[i].flags & BUI_WF_FOCUS) && !(ws->w[i].flags & BUI_WF_DISABLED)) {
            if (seen[i]) {
                reached++;
            } else {
                CHECK(0, "%s: widget %d (at %d,%d) is unreachable with the D-pad", id, ws->w[i].id, ws->w[i].x,
                      ws->w[i].y);
            }
        }
    }
    (void) reached;
}

static void TestFocusGraphs(void) {
    int i;
    for (i = 0; i < bui_gallery_count(); i++) {
        BuiView v;
        BuiWidgets ws;
        if (bui_gallery_draw(i) != NULL) {
            continue;
        }
        bui_gallery_view(i, &v);
        v.dialog = BUI_DLG_NONE;
        bui_kit_begin(gFb);
        bui_screen_paint(&v, &ws);
        AuditFocus(bui_gallery_entry(i)->id, &ws, v.screen == BUI_SCR_LOBBY);
        if (getenv("GDX_BUI_FOCUS_VERBOSE") != NULL) {
            printf("focus %-20s widgets=%d first=%d\n", bui_gallery_entry(i)->id, ws.count, bui_widgets_first_focus(&ws));
        }
        if (v.screen == BUI_SCR_CODE) {
            /* NAME shares the keyboard (no gallery entry of its own) */
            v.screen = BUI_SCR_NAME;
            bui_screen_paint(&v, &ws);
            AuditFocus("name", &ws, 0);
        }
    }
}

/* uipolish (hw: "laps and number of ships arent easily tuned using d-pad (laps actually isnt
 * even highlighted)... cant open a room in multiplayer using the a button"): the row screens'
 * D-pad (bui_screen_nav) and A. */
static int GalleryIndex(const char* id) {
    int i;
    for (i = 0; i < bui_gallery_count(); i++) {
        if (strcmp(bui_gallery_entry(i)->id, id) == 0) {
            return i;
        }
    }
    CHECK(0, "no gallery entry %s", id);
    return 0;
}

static void PaintGalleryView(const char* id, BuiView* v, BuiWidgets* ws) {
    bui_gallery_view(GalleryIndex(id), v);
    v->dialog = BUI_DLG_NONE;
    bui_kit_begin(gFb);
    bui_screen_paint(v, ws);
}

/* One press from `focus`: where focus goes and what runs. */
static BuiNav Press(BuiView* v, const BuiWidgets* ws, int focus, int dx, int dy) {
    BuiNav n;
    v->focus = (int16_t) focus;
    bui_screen_nav(v, ws, dx, dy, &n);
    return n;
}

static void TestHostSheetKeys(void) {
    BuiView v;
    BuiWidgets ws;
    BuiNav n;
    PaintGalleryView("host-sheet", &v, &ws);
    CHECK(bui_screen_key(&v, BUI_KEY_A) == BUI_W_HOST_OPEN, "host: A opens the room from the connection card");
    v.focus = BUI_W_HOST_SIZE3;
    CHECK(bui_screen_key(&v, BUI_KEY_A) == BUI_W_HOST_OPEN, "host: A opens the room from the size row");
    v.focus = BUI_W_HOST_OPEN;
    CHECK(bui_screen_key(&v, BUI_KEY_A) == BUI_W_HOST_OPEN, "host: A on OPEN ROOM");
    v.focus = BUI_W_HOST_CANCEL;
    CHECK(bui_screen_key(&v, BUI_KEY_A) == BUI_W_HOST_CANCEL, "host: A on a focused CANCEL cancels");
    CHECK(bui_screen_key(&v, BUI_KEY_B) == BUI_W_HOST_CANCEL, "host: B cancels");
    n = Press(&v, &ws, BUI_W_HOST_LOCAL, 1, 0);
    CHECK(n.focus == BUI_W_HOST_WIFI && n.activate == BUI_W_HOST_WIFI, "host: right picks WIFI (%d/%d)", n.focus,
          n.activate);
    n = Press(&v, &ws, BUI_W_HOST_WIFI, -1, 0);
    CHECK(n.focus == BUI_W_HOST_LOCAL && n.activate == BUI_W_HOST_LOCAL, "host: left picks LOCAL");
    v.mp.uds = 0;
    n = Press(&v, &ws, BUI_W_HOST_WIFI, -1, 0);
    CHECK(n.focus == BUI_W_HOST_WIFI && n.activate == BUI_W_NONE, "host: LOCAL off stays on WIFI");
    v.mp.uds = 1;
    n = Press(&v, &ws, BUI_W_HOST_LOCAL, 0, 1);
    CHECK(n.focus == BUI_W_HOST_SIZE4 && n.activate == BUI_W_NONE, "host: down lands on the chosen size (%d)", n.focus);
    n = Press(&v, &ws, BUI_W_HOST_SIZE4, -1, 0);
    CHECK(n.focus == BUI_W_HOST_SIZE3 && n.activate == BUI_W_HOST_SIZE3, "host: left sets 3 players");
    n = Press(&v, &ws, BUI_W_HOST_SIZE2, -1, 0);
    CHECK(n.focus == BUI_W_HOST_SIZE2, "host: 2 is the smallest room");
    n = Press(&v, &ws, BUI_W_HOST_SIZE4, 0, 1);
    CHECK(n.focus == BUI_W_HOST_OPEN, "host: down from the size row is OPEN ROOM");
    n = Press(&v, &ws, BUI_W_HOST_OPEN, -1, 0);
    CHECK(n.focus == BUI_W_HOST_CANCEL && n.activate == BUI_W_NONE, "host: left to CANCEL only moves focus");
    n = Press(&v, &ws, BUI_W_HOST_CANCEL, 0, -1);
    CHECK(n.focus == BUI_W_HOST_SIZE4, "host: up from the buttons is the size row");
    n = Press(&v, &ws, BUI_W_HOST_SIZE4, 0, -1);
    CHECK(n.focus == BUI_W_HOST_LOCAL, "host: up again is the chosen connection");
}

static void TestSettingsKeys(void) {
    BuiView v;
    BuiWidgets ws;
    BuiNav n;
    PaintGalleryView("settings", &v, &ws);
    v.mp.racersCap = 16;
    n = Press(&v, &ws, BUI_W_SET_COURSE, 0, 1);
    CHECK(n.focus == BUI_W_SET_LAPS, "settings: down from COURSE is LAPS (was skipped), got %d", n.focus);
    n = Press(&v, &ws, BUI_W_SET_LAPS, 1, 0);
    CHECK(n.focus == BUI_W_SET_LAPS && n.activate == BUI_W_SET_LAPS_UP, "settings: right raises the laps");
    n = Press(&v, &ws, BUI_W_SET_LAPS, -1, 0);
    CHECK(n.focus == BUI_W_SET_LAPS && n.activate == BUI_W_SET_LAPS_DOWN, "settings: left lowers the laps");
    v.focus = BUI_W_SET_LAPS;
    CHECK(bui_screen_key(&v, BUI_KEY_A) == BUI_W_SET_LAPS_UP, "settings: A on LAPS steps it");
    n = Press(&v, &ws, BUI_W_SET_LAPS, 0, 1);
    CHECK(n.focus == BUI_W_SET_RACERS0 + 1, "settings: down lands on the chosen machines (4), got %d", n.focus);
    n = Press(&v, &ws, BUI_W_SET_RACERS0 + 1, 1, 0);
    CHECK(n.focus == BUI_W_SET_RACERS0 + 2 && n.activate == BUI_W_SET_RACERS0 + 2, "settings: right sets 8 machines");
    n = Press(&v, &ws, BUI_W_SET_RACERS0 + 3, 1, 0);
    CHECK(n.focus == BUI_W_SET_RACERS0 + 3 && n.activate == BUI_W_NONE, "settings: 30 is over this room's cap");
    n = Press(&v, &ws, BUI_W_SET_RACERS0, -1, 0);
    CHECK(n.focus == BUI_W_SET_RACERS0 && n.activate == BUI_W_NONE, "settings: OFF is the lowest");
    n = Press(&v, &ws, BUI_W_SET_RACERS0 + 1, 0, 1);
    CHECK(n.focus == BUI_W_SET_RACERS0 + 1, "settings: nothing below MACHINES on a stock-free room");
    n = Press(&v, &ws, BUI_W_SET_RACERS0 + 1, 0, -1);
    CHECK(n.focus == BUI_W_SET_LAPS, "settings: up from MACHINES is LAPS");
    n = Press(&v, &ws, BUI_W_SET_LAPS, 0, -1);
    CHECK(n.focus == BUI_W_SET_COURSE, "settings: up from LAPS is COURSE");
    PaintGalleryView("sp-settings", &v, &ws);
    n = Press(&v, &ws, BUI_W_SET_RACERS0 + 1, 0, 1);
    CHECK(n.focus == BUI_W_SET_REPICK, "sp-settings: down from MACHINES is PICK MACHINES AGAIN");
    n = Press(&v, &ws, BUI_W_SET_REPICK, 0, -1);
    CHECK(n.focus == BUI_W_SET_RACERS0 + 1, "sp-settings: and back up");
}

/* Every other screen keeps the spatial D-pad, and nothing else runs on a press. */
static void TestScreenNavFallback(void) {
    int i, d;
    static const int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (i = 0; i < bui_gallery_count(); i++) {
        BuiView v;
        BuiWidgets ws;
        if (bui_gallery_draw(i) != NULL) {
            continue;
        }
        bui_gallery_view(i, &v);
        v.dialog = BUI_DLG_NONE;
        if (v.screen == BUI_SCR_HOST || v.screen == BUI_SCR_SETTINGS) {
            continue;
        }
        bui_kit_begin(gFb);
        bui_screen_paint(&v, &ws);
        for (d = 0; d < 4; d++) {
            BuiNav n;
            bui_screen_nav(&v, &ws, kDirs[d][0], kDirs[d][1], &n);
            CHECK(n.activate == BUI_W_NONE, "%s: a D-pad press runs widget %d", bui_gallery_entry(i)->id, n.activate);
            CHECK(n.focus == bui_widgets_nav(&ws, v.focus, kDirs[d][0], kDirs[d][1], v.screen == BUI_SCR_LOBBY),
                  "%s: the D-pad left the spatial graph", bui_gallery_entry(i)->id);
        }
    }
}

/* The view key changes with anything drawn and nothing else. */
static void TestViewKey(void) {
    BuiView a, b;
    bui_gallery_view(0, &a);
    bui_gallery_view(0, &b);
    CHECK(bui_view_key(&a) == bui_view_key(&b), "same view, same key");
    b.anim = 3;
    CHECK(bui_view_key(&a) != bui_view_key(&b), "a spinner phase changes the key");
}
#endif

static void TestNav(void) {
    BuiWidgets ws;
    bui_widgets_clear(&ws);
    bui_widgets_add(&ws, 0, 0, 100, 40, 1, BUI_WF_FOCUS);
    bui_widgets_add(&ws, 110, 0, 100, 40, 2, BUI_WF_FOCUS);
    bui_widgets_add(&ws, 0, 50, 100, 40, 3, BUI_WF_FOCUS);
    bui_widgets_add(&ws, 110, 50, 100, 40, 4, BUI_WF_FOCUS | BUI_WF_DISABLED);
    bui_widgets_add(&ws, 0, 100, 210, 40, 5, 0);
    CHECK(bui_widgets_nav(&ws, 1, 1, 0, 0) == 2, "right");
    CHECK(bui_widgets_nav(&ws, 1, 0, 1, 0) == 3, "down");
    CHECK(bui_widgets_nav(&ws, 3, 1, 0, 0) == 2, "a disabled neighbour is skipped (the diagonal one wins)");
    CHECK(bui_widgets_nav(&ws, 3, 1, 0, 1) == 4, "unless allowed");
    CHECK(bui_widgets_nav(&ws, 3, 0, 1, 0) == 3, "an unfocusable widget is never focused");
    CHECK(bui_widgets_nav(&ws, 99, 0, 1, 0) == 1, "an unknown focus restarts at the first");
    CHECK(bui_widgets_hit(&ws, 120, 10) == 2, "hit");
    CHECK(bui_widgets_hit(&ws, 105, 10) == -1, "a gap hits nothing");
    bui_widgets_add(&ws, 0, 0, 320, 240, 9, 0);
    CHECK(bui_widgets_hit(&ws, 120, 10) == 9, "the last widget added wins (a dialog over a screen)");
}

static void TestCombo(void) {
    const uint32_t kCombo = 0x1u | 0x2u | 0x4u;
    BuiCombo c = {0};
    uint32_t heldMs = 0;
    uint32_t t = 1000;
    int fired = 0;
    CHECK(bui_combo_step(&c, kCombo & ~0x4u, kCombo, t, 1500, &heldMs) == BUI_COMBO_NONE, "two of three keys");
    CHECK(bui_combo_step(&c, kCombo, kCombo, t, 1500, &heldMs) == BUI_COMBO_DOWN, "the hold starts");
    for (t = 1016; t < 2500; t += 16) {
        fired += bui_combo_step(&c, kCombo, kCombo, t, 1500, &heldMs) == BUI_COMBO_FIRED;
    }
    CHECK(fired == 0, "not before 1500 ms");
    CHECK(bui_combo_step(&c, kCombo | 0x80u, kCombo, 2500, 1500, &heldMs) == BUI_COMBO_FIRED, "fires at 1500 ms");
    CHECK(heldMs == 1500, "held ms");
    for (t = 2516; t < 8000; t += 16) {
        fired += bui_combo_step(&c, kCombo, kCombo, t, 1500, &heldMs) == BUI_COMBO_FIRED;
    }
    CHECK(fired == 0, "once per hold, extra keys allowed");
    CHECK(bui_combo_step(&c, 0, kCombo, 8000, 1500, &heldMs) == BUI_COMBO_NONE, "a release after firing is quiet");
    CHECK(bui_combo_step(&c, kCombo, kCombo, 9000, 1500, &heldMs) == BUI_COMBO_DOWN, "re-arms after a release");
    CHECK(bui_combo_step(&c, kCombo, kCombo, 9033, 1500, &heldMs) == BUI_COMBO_NONE, "a 30 Hz tick still counts");
    CHECK(bui_combo_step(&c, kCombo & ~0x2u, kCombo, 9400, 1500, &heldMs) == BUI_COMBO_RELEASED_EARLY,
          "let go too early");
    CHECK(heldMs == 400, "early release ms");
    CHECK(bui_combo_step(&c, kCombo, kCombo, 10000, 1500, &heldMs) == BUI_COMBO_DOWN, "a new hold");
    CHECK(bui_combo_step(&c, kCombo, kCombo, 10000 + 1500, 1500, &heldMs) == BUI_COMBO_FIRED,
          "two ticks 1.5 s apart (a stall) fire too");
}

static void TestKeyMask(void) {
    const uint32_t kAll = 0xFFFu;
    BuiKeyMask m = {0};
    CHECK(bui_keymask_step(&m, 1, 0x1u, kAll) == kAll, "a flow hides every key");
    CHECK(bui_keymask_step(&m, 0, 0x3u, kAll) == 0x1u, "the key held through the release stays hidden");
    CHECK(bui_keymask_step(&m, 0, 0x2u, kAll) == 0x0u, "until it is let go");
    CHECK(bui_keymask_step(&m, 0, 0x1u, kAll) == 0x0u, "and a fresh press reaches the game");
}

/* The console bakes the DASH art at boot from the user's archive; so does this test. */
static void BakeArt(void) {
    GdxHudBakeStats st;
    if (!gdx3ds_o2r_host_open()) {
        return;
    }
    gdx3ds_hudbake_run(gdx3ds_o2r_host_read, NULL, NULL, NULL, &st);
    CHECK(st.missing == 0, "hudbake: %d of %d textures missing from the archive", st.missing, st.textures);
    gHaveArt = st.missing == 0;
}

int main(void) {
    BakeArt();
    TestNav();
    TestCombo();
    TestKeyMask();
#ifdef GDX_BUI_HOST_SCREENS
    TestViewKey();
#endif
    TestDirtySpan();
    TestColumnLayout();
    TestQuantise();
    TestMeasure();
    TestModalLayout();
    TestSegRect();
    TestCheckerMatchesFormula();
    TestSpecScreens();
#ifdef GDX_BUI_HOST_SCREENS
    TestProductScreens();
    TestOverlayRepaint();
    TestVersionDialogs();
    TestHintAudit();
    TestFocusGraphs();
    TestHostSheetKeys();
    TestSettingsKeys();
    TestScreenNavFallback();
#endif
    if (gFailures == 0) {
        printf("=== ALL PASS (%d checks) ===\n", gChecks);
        return 0;
    }
    printf("=== %d FAILURES (%d checks) ===\n", gFailures, gChecks);
    return 1;
}
