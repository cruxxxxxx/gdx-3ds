/* port/3ds/gdx3ds_bui_kit.h — the bottom-screen UI kit (M0 of the bottom-menus design,
 * artifacts/bottom-menus-design): the drawing layer every product screen is built from.
 *
 * WHAT IT IS. A C port of the design's own mockup engine (engine.js + kit.js), call for call, so
 * the device draws the pixels the mockups show: fills, 1 px strokes, the DASH checker, RGBA5551
 * glyph blits from the game's own baked atlases (Font1, Font2, timer and position digits, the 30
 * pilot portraits, the G-DIFFUSER logo), the hand-authored Font2-style extra glyphs and 1-bit icons
 * (gdx3ds_bui_glyphs.h), and the components on top: header, footer hints, buttons, list rows,
 * badges, signal bars, spinner, slot cards, toasts, modals, segmented controls, steppers and
 * progress bars. Every colour goes through RGB565 exactly as the mockups quantise it.
 *
 * TARGET. A 320x240 RGB565 surface in the bottom console's own layout: portrait, column-major,
 * index = x * 240 + (239 - y) (gdx3ds_bottomhud.c's convention). The shell points the kit at the
 * DASH's existing off-screen shadow buffer, so the kit owns no full-screen buffer of its own.
 * Every write widens a dirty column span ([x0, x1]); the framebuffer is column-major, so the span
 * is ONE contiguous block and the present is one memcpy of exactly what changed.
 *
 * PURE C. No <3ds.h>, no decomp, no allocation: the host tests (port/3ds/test/
 * gdx3ds_bui_host_tests.c) compile this TU as is and render whole screens with it. The glyph
 * tables come through bui_glyph_table(), which the 3DS build implements in gdx3ds_bottomhud.c (the
 * one TU that holds the baked atlases, so nothing is duplicated) and the host test implements over
 * the same headers.
 *
 * STRINGS are UTF-8. The game fonts have capitals, digits, "." and "-"; lower case falls back to
 * capitals (names display in capitals, F-Zero style), and ' , & / : ! ? + ( ) # % _ ~ = @ " and
 * ✓ × ◀ ▶ ▲ ▼ · … come from the extra glyphs. Anything else advances by a space. */
#ifndef GDX3DS_BUI_KIT_H
#define GDX3DS_BUI_KIT_H

#include <stdint.h>

#include "gdx3ds_hudbake.h"         /* DashGlyph (pixels baked at boot from fzerox.o2r) */
#include "gdx3ds_bui_glyphs.h"       /* enum BuiIcon */

#ifdef __cplusplus
extern "C" {
#endif

#define BUI_W 320
#define BUI_H 240

/* 0xRRGGBB. 0 means "no tint" in the option structs (no component tints with pure black). */
typedef uint32_t BuiRgb;

/* Option-struct encodings where 0 must mean "none": a portrait is its character + 1, signal bars
 * are their count + 1. */
#define BUI_PORTRAIT(character) ((character) + 1)
#define BUI_BARS(n) ((n) + 1)

/* ---- colour tokens (the design's section 9; RGB565 when drawn) ---- */
#define BUI_TK_CHECK_A 0x180810u
#define BUI_TK_CHECK_B 0x391420u
#define BUI_TK_HEADER 0x210818u
#define BUI_TK_PANEL 0x391029u
#define BUI_TK_DEEP 0x100808u
#define BUI_TK_LINE 0x5A2A44u
#define BUI_TK_PINK 0xFF6DB5u
#define BUI_TK_WHITE 0xFFFFFFu
#define BUI_TK_TEXT2 0xC8B4C0u
#define BUI_TK_MUTE 0x8A6A7Eu
#define BUI_TK_FOCUS 0xFFFF00u
#define BUI_TK_CYAN 0x00FFFFu
#define BUI_TK_GREEN 0x00FF00u
#define BUI_TK_RED 0xFF2020u
#define BUI_TK_ORANGE 0xFFA600u
#define BUI_TK_MAGENTA 0xFF00FFu
#define BUI_TK_OFF_FILL 0x1E0F17u
#define BUI_TK_OFF_LINE 0x4A3040u
#define BUI_TK_DARK 0x210818u
#define BUI_TK_BLACK 0x080808u
#define BUI_TK_OK_FILL 0x0E3A14u
#define BUI_TK_BAD_FILL 0x4A0A10u
/* secondary literals the components use */
#define BUI_TK_SLOT_EMPTY 0x120A0Fu
#define BUI_TK_DASH_LINE 0x6A4A5Au
#define BUI_TK_ME_FILL 0x2A0E20u
#define BUI_TK_SEL_FILL 0x3A0E2Au
#define BUI_TK_BARS_OFF 0x4A3A44u

/* Player tags P1..P4 (the race and results screens). */
#define BUI_PCOL_1 0xFF3030u
#define BUI_PCOL_2 0x30A0FFu
#define BUI_PCOL_3 0x30FF60u
#define BUI_PCOL_4 0xFFD000u

/* ---- glyph sets and fonts ---- */
enum BuiGlyphSet {
    BUI_SET_FONT1 = 0,
    BUI_SET_FONT2,
    BUI_SET_TIMER,
    BUI_SET_POSDIGIT,
    BUI_SET_POSSUFFIX,
    BUI_SET_PORTRAIT,
    BUI_SET_KOSTAR,
    BUI_SET_LIVES,
    BUI_SET_LAP,
    BUI_SET_LOGO,
    BUI_SET_COUNT
};

/* Implemented by the TU that owns the atlases (gdx3ds_bottomhud.c on 3DS). NULL = no such set. */
const DashGlyph* bui_glyph_table(int set, int* count);

/* Fonts start at 1 so a zero-initialised option struct means "the default font". */
enum BuiFont {
    BUI_F1 = 1, /* Font1, 16 px italic gradient: titles, labels, names */
    BUI_F1H,    /* Font1 at 1/2 */
    BUI_F2,     /* Font2, 8 px: body, meta, badges */
    BUI_F2X2,   /* Font2 at 2x */
    BUI_FT,     /* timer digits 8x16 (cyan, baked) */
    BUI_FP,     /* position digits 28x32 */
    BUI_FPH,    /* position digits at 1/2 */
    BUI_FONT_COUNT
};

enum { BUI_ALIGN_LEFT = 0, BUI_ALIGN_CENTER = 1, BUI_ALIGN_RIGHT = 2 };

typedef struct BuiText {
    BuiRgb tint; /* 0 = the glyphs' own colours; else multiplied in (or replaces them: solid) */
    int solid;
    double dim;  /* 0 = none; else every colour is scaled by it */
    int align;
} BuiText;

typedef struct BuiSprite {
    int num, den; /* scale num/den (0/0 = 1:1) */
    BuiRgb tint;
    int solid;
    double dim;
} BuiSprite;

/* ---- the surface ---- */
void bui_kit_begin(uint16_t* fb);
uint16_t* bui_kit_target(void);
void bui_kit_dirty_clear(void);
/* 0 when nothing was written since the clear; else the written column span [*x0, *x1]. */
int bui_kit_dirty(int* x0, int* x1);
/* o3ds60: while y0/y1 (BUI_W entries each) are set, every write also widens its column's
 * [y0, y1] row range (callers initialise them; NULL, NULL = off). The dash's map box uses it to
 * restore and present only the pixels its dots and NET chip covered. */
void bui_kit_row_track(int16_t* y0, int16_t* y1);

/* ---- primitives ---- */
void bui_set(int x, int y, int r, int g, int b);
void bui_get(int x, int y, int* r, int* g, int* b);
void bui_fill(int x, int y, int w, int h, BuiRgb c);
void bui_stroke(int x, int y, int w, int h, BuiRgb c, int t);
void bui_dashed(int x, int y, int w, int h, BuiRgb c);
void bui_shade(int x, int y, int w, int h, double k, BuiRgb tint);
void bui_line(int x0, int y0, int x1, int y1, BuiRgb c, int t);
/* The DASH backdrop: the 8 px plum/black checker, dimmed as DashDimBackground dims it. */
void bui_checker(int x, int y, int w, int h);
/* One glyph of a set by key ("A", "7", "ST", "12", "star", "logo"); returns its advance. */
int bui_sprite(int set, const char* key, int x, int y, const BuiSprite* o);
int bui_sprite_index(int set, int index, int x, int y, const BuiSprite* o);
void bui_icon(int icon, int x, int y, BuiRgb c, int scale);

/* ---- text ---- */
int bui_measure(int font, const char* s);
/* Draws s; x is a double so a centred label keeps the design's half-pixel rounding. Returns the
 * drawn width. */
int bui_text(int font, const char* s, double x, int y, const BuiText* o);
/* Code points bui_text could not draw (no font glyph, no extra glyph) since the last reset: the
 * host tests' string lint. */
int bui_kit_unknown_glyphs(void);
void bui_kit_reset_unknown_glyphs(void);

/* ---- button hints ---- */
/* Every button glyph drawn ("A", "B", "X", "L" ... the footer splits "L/R" into two) is reported
 * to the sink with where it was drawn: the host tests check that each one has a handler (the
 * screen's key map, bui_screen_key) and a widget to tap. NULL = no sink. */
typedef void (*BuiHintSink)(const char* key, int x, int y, void* ctx);
void bui_kit_set_hint_sink(BuiHintSink sink, void* ctx);
void bui_kit_note_hint(const char* key, int x, int y);
/* 1 = the screen's buttons belong to the game right now (touch only): every button glyph -- the
 * footer's hints, the header's B, a button's glyph -- is left out, so no hint shows a key that
 * would do nothing. */
void bui_kit_set_glyphs_off(int off);
int bui_kit_glyphs_off(void);

/* ---- components (kit.js) ---- */
int bui_btn_glyph(int x, int y, const char* letter, BuiRgb color, BuiRgb fg);

enum BuiBadgeKind {
    BUI_BADGE_INFO = 0,
    BUI_BADGE_CYANLINE,
    BUI_BADGE_OK,
    BUI_BADGE_WARN,
    BUI_BADGE_BAD,
    BUI_BADGE_MUTED,
    BUI_BADGE_EK,
    BUI_BADGE_MODEL,
    BUI_BADGE_PINK,
    BUI_BADGE_YELLOW,
    BUI_BADGE_KIND_COUNT
};
int bui_badge_width(const char* t);
int bui_badge(int x, int y, const char* t, int kind);
void bui_bars(int x, int y, int n);
void bui_spinner(int cx, int cy, int phase);

/* A header's right-hand items, drawn right to left from x = 314. */
enum { BUI_HDR_BADGE = 0, BUI_HDR_BARS, BUI_HDR_SPIN };
typedef struct BuiHeaderItem {
    int type;
    const char* text;
    int kind;  /* BUI_BADGE_* */
    int value; /* bars count / spinner phase */
} BuiHeaderItem;
typedef struct BuiHeader {
    int noBack; /* 1 = no back cell (◀ B) */
    int icon;   /* BUI_ICON_*; 0 = none */
    BuiRgb iconColor;
    int font;   /* BUI_F1 (default) or BUI_F2 */
    const BuiHeaderItem* right;
    int rightCount;
} BuiHeader;
void bui_header(const char* title, const BuiHeader* o);

typedef struct BuiHint {
    const char* keys; /* "A", "L/R", "ST" */
    const char* label;
} BuiHint;
void bui_footer(const BuiHint* hints, int n, const char* right);

enum BuiButtonState {
    BUI_BTN_NORMAL = 0,
    BUI_BTN_PRIMARY,
    BUI_BTN_PRESSED,
    BUI_BTN_GO,
    BUI_BTN_DANGER,
    BUI_BTN_DISABLED,
    BUI_BTN_GHOST
};
typedef struct BuiButton {
    int state;
    int focus;
    int font;  /* 0 = from the height (F1 at 32 px and up, else F2) */
    int icon;  /* BUI_ICON_*; 0 = none */
    BuiRgb iconColor;
    const char* glyph; /* button glyph drawn before the label ("A", "ST"), NULL = none */
    int alignLeft;
    const char* sub; /* Font2 reason line under the label */
    BuiRgb subTint;
    BuiRgb tint;
    double meter; /* hold-to-confirm: 0..1 of the button filled from the left (0 = none) */
} BuiButton;
void bui_button(int x, int y, int w, int h, const char* label, const BuiButton* o);

typedef struct BuiRowBadge {
    const char* text;
    int kind;
} BuiRowBadge;
typedef struct BuiRow {
    BuiRgb fill, line;
    int focus;
    int disabled;
    int portrait; /* character + 1; 0 = none */
    int icon;     /* BUI_ICON_*; 0 = none */
    BuiRgb iconColor, iconFill;
    const char* title;
    int titleFont; /* 0 = BUI_F1 */
    BuiRgb titleTint;
    const char* meta;
    BuiRgb metaTint;
    const BuiRowBadge* badges;
    int badgeCount;
    int bars; /* signal bars + 1; 0 = none */
    int chev;
} BuiRow;
void bui_row(int x, int y, int w, int h, const BuiRow* o);

enum BuiSlotState {
    BUI_SLOT_NONE = 0,
    BUI_SLOT_READY,
    BUI_SLOT_WAIT,
    BUI_SLOT_MISMATCH,
    BUI_SLOT_AWAY,
    BUI_SLOT_CPU,
    BUI_SLOT_NOCOURSE,
    BUI_SLOT_GETTING,
    BUI_SLOT_LOADING,
    BUI_SLOT_LOADED,
    BUI_SLOT_HOST
};
typedef struct BuiSlot {
    int empty;
    const char* label; /* the empty card's text ("OPEN") */
    int me, host, you, away;
    int pilot;
    const char* name;
    const char* sub;
    BuiRgb subTint;
    int state; /* BUI_SLOT_* */
    int pct;
    int bars; /* signal bars + 1; 0 = none */
} BuiSlot;
void bui_slot(int x, int y, int w, int h, const BuiSlot* p);

enum BuiTone { BUI_TONE_INFO = 0, BUI_TONE_OK, BUI_TONE_WARN, BUI_TONE_BAD, BUI_TONE_PINK };
void bui_toast(int y, const char* t, int tone, int icon /* BUI_ICON_* */);

typedef struct BuiModalButton {
    const char* text;
    const char* glyph;
    int state;
    int focus;
    int font; /* 0 = automatic */
    const char* sub; /* Font2 line under the label (hold-to-confirm: "HOLD TO CONFIRM") */
    double meter;    /* hold-to-confirm fill, 0..1 */
} BuiModalButton;
typedef struct BuiModal {
    int tone;           /* BUI_TONE_*; INFO/PINK = a pink frame */
    int icon;           /* BUI_ICON_* */
    const char* title;
    const char* const* body;
    int bodyCount;
    BuiRgb leadTint;
    const char* code;
    const BuiModalButton* buttons;
    int buttonCount;
    int w;              /* 0 = 288 */
    int extraH;
    double progress;    /* > 0 (with extraH) = a progress bar under the body (the e-radio dialog) */
    int noShade;        /* 1 = the screen behind is already shaded (a repaint of the panel alone) */
} BuiModal;
typedef struct BuiRect {
    int x, y, w, h;
} BuiRect;
/* Draws the dialog over a darkened screen. Returns its frame; buttonRects (may be NULL) gets each
 * button's rect, for hit-testing. */
BuiRect bui_modal(const BuiModal* o, BuiRect* buttonRects);
/* Builds up to `budget` more entries of the dialogs' cached shade table (128 KB, one table load
 * per shaded pixel once ready; until then the shade computes per channel, same bytes). Call it
 * a little per idle tick; 1 = ready (or 0 for good if the table could not be allocated). */
int bui_modal_prepare(int budget);
/* The geometry bui_modal uses, without drawing: the frame and each button's rect. */
BuiRect bui_modal_layout(const BuiModal* o, BuiRect* buttonRects);

typedef struct BuiSeg {
    int font; /* 0 = BUI_F1 */
    int offMask; /* bit i = segment i is locked */
    int focus;
} BuiSeg;
void bui_seg(int x, int y, int w, int h, const char* const* opts, int n, int sel, const BuiSeg* o);
/* Segment i's rect, for hit-testing (equal widths, the last one takes the remainder). */
BuiRect bui_seg_rect(int x, int y, int w, int h, int n, int i);

typedef struct BuiStepper {
    int focus;
    int font; /* 0 = BUI_F1 */
    BuiRgb tint;
} BuiStepper;
void bui_stepper(int x, int y, int w, int h, const char* label, const char* value, const BuiStepper* o);
void bui_progress(int x, int y, int w, int h, double frac, BuiRgb col);

/* The G-DIFFUSER wordmark (240x76) at x, y. */
void bui_logo(int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_BUI_KIT_H */
