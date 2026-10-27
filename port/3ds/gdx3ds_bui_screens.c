/* port/3ds/gdx3ds_bui_screens.c — the bottom-screen product screens. Contract in
 * gdx3ds_bui_screens.h. Each painter is the C twin of its screens.js mockup function in
 * artifacts/bottom-menus-design (same coordinates, same calls), with the deviations the shell's
 * data forces noted where they happen (no course thumbnails, no host machine in a NEARBY beacon,
 * guests do not hear the host's settings before START). */
#include "gdx3ds_bui_screens.h"

#include <stdio.h>
#include <string.h>

#include "gdx3ds_bui_kit.h"

/* ---- shared data -------------------------------------------------------------------------- */

static const char* const kCourse[24] = {
    "MUTE CITY", "SILENCE", "SAND OCEAN", "DEVIL'S FOREST", "BIG BLUE", "PORT TOWN",
    "SECTOR ALPHA", "RED CANYON", "DEVIL'S FOREST 2", "MUTE CITY 2", "BIG BLUE 2", "WHITE LAND",
    "FIRE FIELD", "SILENCE 2", "SECTOR BETA", "RED CANYON 2", "WHITE LAND 2", "MUTE CITY 3",
    "RAINBOW ROAD", "DEVIL'S FOREST 3", "SPACE PLANT", "SAND OCEAN 2", "PORT TOWN 2", "BIG HAND",
};
static const char* const kCup[4] = {"JACK", "QUEEN", "KING", "JOKER"};
static const char* const kPilot[BUI_MACHINES] = {
    "FALCON", "STEWART", "PICO", "GOROH", "JODY", "GAZELLE", "MR.EAD", "BABA", "OCTOMAN", "GOMAR",
    "KATE", "ROGER", "JAMES", "LEON", "ANTONIO", "B.SHADOW", "MICHAEL", "JACK", "S.ARROW", "M.ARROW",
    "TANAKA", "BEASTMAN", "ZODA", "DR.CLASH", "SILVER", "BIO REX", "DRAQ", "BILLY", "SKULL", "B.FALCON",
};
static const char* const kMachine[BUI_MACHINES] = {
    "BLUE FALCON", "GOLDEN FOX", "WILD GOOSE", "FIRE STINGRAY", "WHITE CAT", "RED GAZELLE",
    "GREAT STAR", "IRON TIGER", "DEEP CLAW", "TWIN NORITTA", "SUPER PIRANHA", "MIGHTY HURRICANE",
    "LITTLE WYVERN", "SPACE ANGLER", "MIGHTY TYPHOON", "BLACK BULL", "WILD BOAR", "ASTRO ROBIN",
    "KING METEOR", "QUEEN METEOR", "WONDER WASP", "HYPER SPEEDER", "DEATH ANCHOR", "CRAZY BEAR",
    "SONIC PHANTOM", "BIG FANG", "NIGHT THUNDER", "MAD WOLF", "SILVER RAT", "BLOOD HAWK",
};

const char kBuiCodeKeys[33] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const char kBuiNameKeys[41] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ-._ 0123456789";

const char* bui_course_name(int course) {
    return (course >= 0 && course < 24) ? kCourse[course] : "COURSE";
}

const char* bui_cup_name(int cup) {
    return (cup >= 0 && cup < 4) ? kCup[cup] : "";
}

void bui_course_label(int course, char* cup, int cupCap, char* name, int nameCap) {
    if (course >= 30 && course <= 41) {
        snprintf(cup, (size_t) cupCap, "DD-%d CUP", course >= 36 ? 2 : 1);
        snprintf(name, (size_t) nameCap, "COURSE %d", (course - 30) % 6 + 1);
        return;
    }
    if (course < 0 || course >= 24) {
        course = 0;
    }
    snprintf(cup, (size_t) cupCap, "%s CUP", bui_cup_name(course / 6));
    snprintf(name, (size_t) nameCap, "%s", bui_course_name(course));
}

const char* bui_pilot_name(int character) {
    return (character >= 0 && character < BUI_MACHINES) ? kPilot[character] : "";
}

const char* bui_machine_name(int character) {
    return (character >= 0 && character < BUI_MACHINES) ? kMachine[character] : "";
}

void bui_view_init(BuiView* v) {
    memset(v, 0, sizeof(*v));
    v->focus = BUI_W_NONE;
    v->holdWidget = BUI_W_NONE;
    v->keepMachine = -1;
    v->roomSize = 4;
}

uint32_t bui_view_key(const BuiView* v) {
    const unsigned char* p = (const unsigned char*) v;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < sizeof(*v); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

uint32_t bui_view_base_key(const BuiView* v) {
    BuiView base = *v;
    base.dialog = BUI_DLG_NONE;
    base.dialogFocus = 0;
    memset(base.dialogName, 0, sizeof(base.dialogName));
    memset(base.dialogCode, 0, sizeof(base.dialogCode));
    memset(base.toast, 0, sizeof(base.toast));
    base.toastTone = 0;
    base.toastIcon = 0;
    if (base.holdWidget >= BUI_W_DLG_BUTTON0) {
        base.holdWidget = BUI_W_NONE; /* a dialog button's hold meter is the dialog's */
        base.holdPct = 0;
    }
    return bui_view_key(&base);
}

/* ---- small helpers (screens.js label / kv / card / rail) ----------------------------------- */

static BuiText Txt(BuiRgb tint, int solid, int align) {
    BuiText t;
    memset(&t, 0, sizeof(t));
    t.tint = tint;
    t.solid = solid;
    t.align = align;
    return t;
}

static void Label(const char* s, int x, int y, BuiRgb tint) {
    const BuiText t = Txt(tint != 0 ? tint : BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
    bui_text(BUI_F2, s, x, y, &t);
}

static void Centred(int font, const char* s, double x, int y, BuiRgb tint, int solid) {
    const BuiText t = Txt(tint, solid, BUI_ALIGN_CENTER);
    bui_text(font, s, x, y, &t);
}

static void RightText(const char* s, int x, int y, BuiRgb tint) {
    const BuiText t = Txt(tint, 1, BUI_ALIGN_RIGHT);
    bui_text(BUI_F2, s, x, y, &t);
}

static void Kv(int x, int y, int w, const char* k, const char* v, BuiRgb tint) {
    const BuiText key = Txt(BUI_TK_MUTE, 1, BUI_ALIGN_LEFT);
    const BuiText val = Txt(tint != 0 ? tint : BUI_TK_WHITE, tint != 0, BUI_ALIGN_RIGHT);
    bui_text(BUI_F2, k, x, y, &key);
    bui_text(BUI_F2, v, x + w, y, &val);
}

static int Round2(int v) {
    return v >= 0 ? (v + 1) / 2 : -((-v) / 2);
}

static void Background(void) {
    bui_checker(0, 0, BUI_W, BUI_H);
}

static void Header(const char* title, int noBack, const BuiHeaderItem* right, int n, BuiWidgets* ws) {
    BuiHeader h;
    memset(&h, 0, sizeof(h));
    h.noBack = noBack;
    h.right = right;
    h.rightCount = n;
    bui_header(title, &h);
    if (!noBack) {
        bui_widgets_add(ws, 0, 0, 45, 24, BUI_W_BACK, 0);
    }
}

static BuiHeaderItem Badge(const char* t, int kind) {
    BuiHeaderItem b;
    memset(&b, 0, sizeof(b));
    b.type = BUI_HDR_BADGE;
    b.text = t;
    b.kind = kind;
    return b;
}

static BuiHeaderItem Spin(int phase) {
    BuiHeaderItem b;
    memset(&b, 0, sizeof(b));
    b.type = BUI_HDR_SPIN;
    b.value = phase;
    return b;
}

/* The ring stays under an open dialog too (shaded, as the mockups draw it): the D-pad returns to
 * it when the dialog closes. */
static int Focused(const BuiView* v, int id) {
    return v->focus == id;
}

typedef struct Card {
    int icon;
    BuiRgb iconColor;
    const char* title;
    const char* lines[2];
    const char* badge;
    int badgeKind;
    int off, sel, focus;
} Card;

static void PaintCard(int x, int y, int w, int h, const Card* o) {
    BuiText t;
    int tx = o->icon != 0 ? x + 52 : x + 10;
    int i;
    bui_fill(x, y, w, h, o->off ? BUI_TK_OFF_FILL : BUI_TK_PANEL);
    bui_stroke(x, y, w, h, o->sel ? BUI_TK_PINK : o->off ? BUI_TK_OFF_LINE : BUI_TK_PINK, o->sel ? 2 : 1);
    if (o->focus) {
        bui_stroke(x - 2, y - 2, w + 4, h + 4, BUI_TK_FOCUS, 2);
    }
    if (o->icon != 0) {
        bui_icon(o->icon, x + 10, y + Round2(h - 32), o->off ? BUI_TK_OFF_LINE : (o->iconColor != 0 ? o->iconColor : BUI_TK_CYAN), 2);
    }
    memset(&t, 0, sizeof(t));
    t.dim = o->off ? 0.45 : 0;
    bui_text(BUI_F1, o->title, tx, y + 8, &t);
    for (i = 0; i < 2; i++) {
        if (o->lines[i] != NULL) {
            const BuiText l = Txt(i == 0 ? BUI_TK_TEXT2 : BUI_TK_MUTE, 1, BUI_ALIGN_LEFT);
            bui_text(BUI_F2, o->lines[i], tx, y + 30 + i * 11, &l);
        }
    }
    if (o->badge != NULL) {
        bui_badge(x + w - 6 - bui_badge_width(o->badge), y + 6, o->badge, o->badgeKind);
    }
}

static void RailIds(int x, int y, int h, int page, int pages, int upId, int downId, BuiWidgets* ws) {
    char t[8];
    BuiButton b;
    memset(&b, 0, sizeof(b));
    b.font = BUI_F2;
    b.state = page > 1 ? BUI_BTN_NORMAL : BUI_BTN_DISABLED;
    bui_button(x, y, 36, 40, "▲", &b);
    bui_widgets_add(ws, x, y, 36, 40, upId, page > 1 ? 0 : BUI_WF_DISABLED);
    b.state = page < pages ? BUI_BTN_NORMAL : BUI_BTN_DISABLED;
    bui_button(x, y + h - 40, 36, 40, "▼", &b);
    bui_widgets_add(ws, x, y + h - 40, 36, 40, downId, page < pages ? 0 : BUI_WF_DISABLED);
    snprintf(t, sizeof(t), "%d/%d", page, pages);
    Centred(BUI_F2, t, x + 18, y + Round2(h) - 4, BUI_TK_TEXT2, 1);
}

static void Rail(int x, int y, int h, int page, int pages, BuiWidgets* ws) {
    RailIds(x, y, h, page, pages, BUI_W_NEARBY_UP, BUI_W_NEARBY_DOWN, ws);
}

static const char* ModelBadge(int model) {
    return model == BUI_MODEL_N3DS ? "N3DS" : model == BUI_MODEL_O3DS ? "O3DS" : "3DS";
}

static void Footer2(const char* k0, const char* l0, const char* k1, const char* l1, const char* k2, const char* l2,
                    const char* right) {
    BuiHint h[3];
    int n = 0;
    if (k0 != NULL) {
        h[n].keys = k0;
        h[n++].label = l0;
    }
    if (k1 != NULL) {
        h[n].keys = k1;
        h[n++].label = l1;
    }
    if (k2 != NULL) {
        h[n].keys = k2;
        h[n++].label = l2;
    }
    bui_footer(h, n, right);
}

/* Text that must fit `w`: Font2, else Font1 at half size, else cut with an ellipsis. */
static void FitText(const char* s, int x, int y, int w, BuiRgb tint, int solid) {
    char cut[40];
    const BuiText t = Txt(tint, solid, BUI_ALIGN_LEFT);
    size_t n;
    if (bui_measure(BUI_F2, s) <= w) {
        bui_text(BUI_F2, s, x, y, &t);
        return;
    }
    if (bui_measure(BUI_F1H, s) <= w) {
        bui_text(BUI_F1H, s, x, y, &t);
        return;
    }
    snprintf(cut, sizeof(cut), "%s", s);
    n = strlen(cut);
    while (n > 1) {
        cut[--n] = '\0';
        if (bui_measure(BUI_F1H, cut) + bui_measure(BUI_F1H, "…") <= w) {
            break;
        }
    }
    snprintf(cut + n, sizeof(cut) - n, "%s", "…");
    bui_text(BUI_F1H, cut, x, y, &t);
}

/* ---- HOME ---------------------------------------------------------------------------------- */

/* screens.js home(): the design has two tiles (MULTIPLAYER, COURSES). mp-menus: multiplayer opens
 * from the top screen's own VS icon. card-4: Home no longer points at it either (hw: the
 * MULTIPLAYER notice under the logo "just leads to confusion"), so Home is the logo alone.
 * card-4b (hw: "the logo isn't centered anymore"): the logo's ink (not its 240x76 box: the
 * wordmark sits at x 6..229, y 13..63 inside it) is centred in the space above the pilot footer;
 * with the developer 3D button the logo and the button are centred as one group. */
enum {
    HOME_FOOTER_Y = 206,
    HOME_LOGO_INK_X = 6,
    HOME_LOGO_INK_W = 224,
    HOME_LOGO_INK_Y = 13,
    HOME_LOGO_INK_H = 51,
    HOME_3D_W = 70,
    HOME_3D_H = 30,
    HOME_3D_GAP = 14,
    HOME_SETTINGS_X = 212,
    HOME_SETTINGS_Y = 211,
    HOME_SETTINGS_W = 102,
    HOME_SETTINGS_H = 25
};

static int HomeLogoInkTop(const BuiView* v) {
    const int groupH = HOME_LOGO_INK_H + (v->stereoButton ? HOME_3D_GAP + HOME_3D_H : 0);
    return (HOME_FOOTER_Y - groupH) / 2;
}

/* release: SETTINGS, the player settings (display, 3D, controls, volume: gdx3ds_menu.c's player
 * tabs, no developer tools), at the right of the pilot footer so the centred logo stays put. A
 * tap opens it (Home's buttons belong to the top screen's menu, so no key glyph: SELECT, which
 * the game does not use, opens it too -- gdx3ds_bui.c ServiceHomeSelect). */
static void PaintHomeSettings(BuiWidgets* ws) {
    BuiButton b;
    memset(&b, 0, sizeof(b));
    b.font = BUI_F2;
    b.state = BUI_BTN_NORMAL;
    b.icon = BUI_ICON_GEAR;
    b.iconColor = BUI_TK_CYAN;
    bui_button(HOME_SETTINGS_X, HOME_SETTINGS_Y, HOME_SETTINGS_W, HOME_SETTINGS_H, "SETTINGS", &b);
    bui_widgets_add(ws, HOME_SETTINGS_X, HOME_SETTINGS_Y, HOME_SETTINGS_W, HOME_SETTINGS_H, BUI_W_HOME_SETTINGS, 0);
}

static void PaintHome(const BuiView* v, BuiWidgets* ws) {
    char key[4];
    const int inkTop = HomeLogoInkTop(v);
    Background();
    bui_logo((BUI_W - HOME_LOGO_INK_W) / 2 - HOME_LOGO_INK_X, inkTop - HOME_LOGO_INK_Y);
    bui_fill(0, 206, 320, 1, BUI_TK_LINE);
    bui_fill(0, 207, 320, 33, BUI_TK_HEADER);
    snprintf(key, sizeof(key), "%d", v->homePilot >= 0 && v->homePilot < BUI_MACHINES ? v->homePilot : 0);
    bui_sprite(BUI_SET_PORTRAIT, key, 6, 207, NULL);
    bui_text(BUI_F2, v->name, 44, 213, NULL);
    bui_badge(44, 225, v->model == BUI_MODEL_N3DS ? "NEW 3DS" : "OLD 3DS", BUI_BADGE_MODEL);
    if (v->settingsButton) {
        PaintHomeSettings(ws);
    }
    if (v->stereoButton) {
        /* o3ds60: 3D on / off without the slider (an Old 3DS here reads its slider stuck at 0.39).
         * card-4: developer lever only, [debug] bui_3d_button = 1. card-4b: under the logo. */
        const int x = (BUI_W - HOME_3D_W) / 2;
        const int y = inkTop + HOME_LOGO_INK_H + HOME_3D_GAP;
        BuiButton b;
        memset(&b, 0, sizeof(b));
        b.font = BUI_F2;
        b.state = v->stereoOn ? BUI_BTN_PRIMARY : BUI_BTN_NORMAL;
        bui_button(x, y, HOME_3D_W, HOME_3D_H, v->stereoOn ? "3D: ON" : "3D: OFF", &b);
        bui_widgets_add(ws, x, y, HOME_3D_W, HOME_3D_H, BUI_W_HOME_3D, 0);
    }
}

/* ---- MULTIPLAYER: choose how --------------------------------------------------------------- */

static void PaintMpChoose(const BuiView* v, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    Card c;
    BuiRow r;
    BuiRowBadge badges[2];
    Background();
    Header("MULTIPLAYER", 0, NULL, 0, ws);
    memset(&c, 0, sizeof(c));
    c.icon = BUI_ICON_ANTENNA;
    c.title = "PLAY NEARBY";
    c.focus = Focused(v, BUI_W_CHOOSE_NEARBY);
    if (mp->lan) {
        c.lines[0] = "LOCAL WIRELESS OR SAME WIFI";
        c.lines[1] = "NO INTERNET NEEDED";
        c.badge = "READY";
        c.badgeKind = BUI_BADGE_OK;
    } else {
        c.lines[0] = "NEARBY PLAY IS TURNED OFF";
        c.lines[1] = "SET NET MP_LAN=1 IN THE INI";
        c.badge = "OFF";
        c.badgeKind = BUI_BADGE_MUTED;
        c.off = 1;
    }
    PaintCard(8, 30, 304, 68, &c);
    bui_widgets_add(ws, 8, 30, 304, 68, BUI_W_CHOOSE_NEARBY, BUI_WF_FOCUS | (mp->lan ? 0 : BUI_WF_DISABLED));
    memset(&c, 0, sizeof(c));
    c.icon = BUI_ICON_GLOBE;
    c.title = "PLAY ONLINE";
    c.focus = Focused(v, BUI_W_CHOOSE_ONLINE);
    if (mp->haveServer) {
        c.lines[0] = "ROOM CODE OVER THE INTERNET";
        c.lines[1] = "NEEDS WIFI";
        c.badge = "SERVER SET";
        c.badgeKind = BUI_BADGE_INFO;
    } else if (mp->haveDirect) {
        c.lines[0] = "JOIN A FRIEND'S 3DS BY ADDRESS";
        c.lines[1] = "NEEDS WIFI";
        c.badge = "NO SERVER";
        c.badgeKind = BUI_BADGE_INFO;
    } else {
        c.lines[0] = "NO SERVER IS SET UP";
        c.lines[1] = "ASK THE HOST FOR A SERVER";
        c.badge = "OFF";
        c.badgeKind = BUI_BADGE_MUTED;
        c.off = 1;
    }
    PaintCard(8, 106, 304, 68, &c);
    bui_widgets_add(ws, 8, 106, 304, 68, BUI_W_CHOOSE_ONLINE,
                    BUI_WF_FOCUS | ((mp->haveServer || mp->haveDirect) ? 0 : BUI_WF_DISABLED));
    memset(&r, 0, sizeof(r));
    badges[0].text = "YOUR NAME";
    badges[0].kind = BUI_BADGE_MUTED;
    badges[1].text = ModelBadge(v->model);
    badges[1].kind = BUI_BADGE_MODEL;
    r.icon = BUI_ICON_PENCIL;
    r.iconColor = BUI_TK_PINK;
    r.title = v->name;
    r.titleFont = BUI_F1;
    r.badges = badges;
    r.badgeCount = 2;
    r.focus = Focused(v, BUI_W_CHOOSE_NAME);
    bui_row(8, 182, 304, 36, &r);
    bui_widgets_add(ws, 8, 182, 304, 36, BUI_W_CHOOSE_NAME, BUI_WF_FOCUS);
    Footer2("A", "OPEN", "Y", "NAME", "B", "BACK", NULL);
}

/* ---- NEARBY -------------------------------------------------------------------------------- */

static void HostButton(int w, int state, int focus, BuiWidgets* ws) {
    BuiButton b;
    memset(&b, 0, sizeof(b));
    b.icon = BUI_ICON_PLUS;
    b.iconColor = BUI_TK_GREEN;
    b.state = state;
    b.focus = focus;
    bui_button(4, 28, w, 40, "HOST A ROOM", &b);
    bui_widgets_add(ws, 4, 28, w, 40, BUI_W_NEARBY_HOST, BUI_WF_FOCUS | (state == BUI_BTN_DISABLED ? BUI_WF_DISABLED : 0));
}

static void RoomRow(const BuiView* v, const BuiRoomView* rm, int y, int index, BuiWidgets* ws) {
    char title[32];
    char meta[40];
    BuiRowBadge badge;
    BuiRow r;
    const int id = BUI_W_NEARBY_ROW0 + index;
    memset(&r, 0, sizeof(r));
    snprintf(title, sizeof(title), "%s'S ROOM", rm->host);
    r.icon = rm->medium == BUI_MEDIUM_UDS ? BUI_ICON_ANTENNA : BUI_ICON_WIFI;
    r.title = title;
    r.focus = Focused(v, id);
    r.badges = &badge;
    r.badgeCount = 1;
    if (rm->compatible && rm->joinable) {
        snprintf(meta, sizeof(meta), "%d/%d · %s HOST", rm->players, rm->maxPlayers,
                 rm->model == BUI_MODEL_N3DS ? "NEW 3DS" : "OLD 3DS");
        badge.text = rm->medium == BUI_MEDIUM_UDS ? "LOCAL" : "WIFI";
        badge.kind = rm->medium == BUI_MEDIUM_UDS ? BUI_BADGE_INFO : BUI_BADGE_CYANLINE;
        r.bars = BUI_BARS(rm->signal);
    } else {
        const int version = !rm->compatible;
        snprintf(meta, sizeof(meta), "%s", version ? "ANOTHER BUILD · CAN'T JOIN" : "CAN'T JOIN RIGHT NOW");
        badge.text = version ? "VERSION" : rm->whyNot;
        badge.kind = version ? BUI_BADGE_BAD : (strcmp(rm->whyNot, "RACING") == 0 ? BUI_BADGE_WARN : BUI_BADGE_MUTED);
        r.disabled = 1;
        r.metaTint = BUI_TK_ORANGE;
    }
    r.meta = meta;
    bui_row(4, y, 272, 40, &r);
    bui_widgets_add(ws, 4, y, 272, 40, id, BUI_WF_FOCUS);
}

static void NearbyList(const BuiView* v, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    BuiHeaderItem right[2];
    char rooms[16];
    const int first = (mp->page - 1) * BUI_ROOMS_PER_PAGE;
    int i;
    snprintf(rooms, sizeof(rooms), "%d ROOM%s", mp->roomCount, mp->roomCount == 1 ? "" : "S");
    right[0] = Spin(v->anim);
    right[1] = Badge(rooms, BUI_BADGE_MUTED);
    Header("NEARBY", 0, right, 2, ws);
    HostButton(272, BUI_BTN_NORMAL, Focused(v, BUI_W_NEARBY_HOST), ws);
    bui_btn_glyph(256, 42, "Y", 0, 0);
    for (i = 0; i < BUI_ROOMS_PER_PAGE && first + i < mp->roomCount; i++) {
        RoomRow(v, &mp->room[first + i], 72 + i * 42, i, ws);
    }
    Rail(280, 28, 168, mp->page, mp->pages, ws);
    Centred(BUI_F2, "LOCAL SCAN ON · ROOMS ON THIS WIFI SHOWN TOO", 160, 206, BUI_TK_MUTE, 1);
    Footer2("A", "JOIN", "Y", "HOST", "B", "BACK", NULL);
}

static void NearbyEmpty(const BuiView* v, BuiWidgets* ws) {
    BuiHeaderItem right[2];
    right[0] = Spin(v->anim);
    right[1] = Badge("SEARCHING", BUI_BADGE_MUTED);
    Header("NEARBY", 0, right, 2, ws);
    HostButton(312, BUI_BTN_NORMAL, Focused(v, BUI_W_NEARBY_HOST), ws);
    bui_fill(4, 76, 312, 140, BUI_TK_DEEP);
    bui_stroke(4, 76, 312, 140, BUI_TK_LINE, 1);
    bui_spinner(160, 100, (v->anim + 6) & 7); /* the mockup runs it 2 steps behind the header's */
    Centred(BUI_F1, "LOOKING FOR ROOMS", 160, 116, 0, 0);
    Centred(BUI_F2, "NO ROOMS YET. ASK A FRIEND TO HOST,", 160, 144, BUI_TK_TEXT2, 1);
    Centred(BUI_F2, "OR HOST ONE YOURSELF.", 160, 156, BUI_TK_TEXT2, 1);
    Centred(BUI_F2, "EVERYONE NEEDS THIS SAME VERSION:", 160, 178, BUI_TK_MUTE, 1);
    bui_badge(160 - bui_badge_width(v->version) / 2, 192, v->version, BUI_BADGE_MODEL);
    Footer2("Y", "HOST", "B", "BACK", NULL, NULL, NULL);
}

static void NearbyRadioOff(const BuiView* v, BuiWidgets* ws) {
    BuiHeaderItem right[1];
    BuiButton b;
    right[0] = Badge("RADIO OFF", BUI_BADGE_BAD);
    Header("NEARBY", 0, right, 1, ws);
    HostButton(312, BUI_BTN_DISABLED, 0, ws);
    bui_fill(4, 76, 312, 140, BUI_TK_DEEP);
    bui_stroke(4, 76, 312, 140, BUI_TK_RED, 1);
    bui_icon(BUI_ICON_WARN, 144, 84, BUI_TK_ORANGE, 2);
    Centred(BUI_F1, "WIRELESS IS OFF", 160, 120, 0, 0);
    Centred(BUI_F2, "SLIDE THE WIRELESS SWITCH ON", 160, 142, BUI_TK_TEXT2, 1);
    Centred(BUI_F2, "(AND LEAVE AIRPLANE MODE)", 160, 154, BUI_TK_MUTE, 1);
    memset(&b, 0, sizeof(b));
    b.glyph = "A";
    b.focus = Focused(v, BUI_W_NEARBY_RETRY);
    bui_button(80, 170, 160, 40, "TRY AGAIN", &b);
    bui_widgets_add(ws, 80, 170, 160, 40, BUI_W_NEARBY_RETRY, BUI_WF_FOCUS);
    Footer2("A", "RETRY", "B", "BACK", NULL, NULL, NULL);
}

static void PaintNearby(const BuiView* v, BuiWidgets* ws) {
    Background();
    if (v->mp.radioOff) {
        NearbyRadioOff(v, ws);
    } else if (v->mp.roomCount == 0) {
        NearbyEmpty(v, ws);
    } else {
        NearbyList(v, ws);
    }
}

/* ---- HOST A ROOM --------------------------------------------------------------------------- */

static void HostOption(const BuiView* v, int x, int medium, BuiWidgets* ws) {
    const int sel = v->hostMedium == medium;
    const int uds = medium == BUI_MEDIUM_UDS;
    const int off = uds && !v->mp.uds;
    const int id = uds ? BUI_W_HOST_LOCAL : BUI_W_HOST_WIFI;
    const char* l1 = uds ? "NO ROUTER NEEDED" : "SAME ROUTER";
    const char* l2 = uds ? "PLAYERS IN ONE ROOM" : "WIFI MUST BE ON";
    const char* l3 = uds ? (off ? "LOCAL WIRELESS OFF" : "PAUSES WIFI") : ""; /* off: [net] mp_uds=0 */
    const char* lines[3];
    BuiText t;
    int i;
    lines[0] = l1;
    lines[1] = l2;
    lines[2] = l3;
    bui_fill(x, 42, 152, 80, off ? BUI_TK_OFF_FILL : sel ? BUI_TK_SEL_FILL : BUI_TK_DEEP);
    bui_stroke(x, 42, 152, 80, off ? BUI_TK_OFF_LINE : sel ? BUI_TK_PINK : BUI_TK_LINE, sel ? 2 : 1);
    if (Focused(v, id)) {
        bui_stroke(x - 2, 40, 156, 84, BUI_TK_FOCUS, 2);
    }
    bui_icon(uds ? BUI_ICON_ANTENNA : BUI_ICON_WIFI, x + 8, 50, off ? BUI_TK_OFF_LINE : BUI_TK_CYAN, 1);
    memset(&t, 0, sizeof(t));
    t.dim = off ? 0.45 : 0;
    bui_text(BUI_F1, uds ? "LOCAL" : "WIFI", x + 30, 50, &t);
    if (sel) {
        bui_badge(x + 152 - 8 - bui_badge_width("✓"), 50, "✓", BUI_BADGE_PINK);
    }
    for (i = 0; i < 3; i++) {
        const BuiText l = Txt(i == 2 ? BUI_TK_ORANGE : BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
        bui_text(BUI_F2, lines[i], x + 8, 76 + i * 12, &l);
    }
    bui_widgets_add(ws, x, 42, 152, 80, id, BUI_WF_FOCUS | (off ? BUI_WF_DISABLED : 0));
}

static void PaintHost(const BuiView* v, BuiWidgets* ws) {
    static const char* const kSizes[3] = {"2", "3", "4"};
    BuiSeg seg;
    BuiButton b;
    int i;
    Background();
    Header("HOST A ROOM", 0, NULL, 0, ws);
    Label("CONNECTION", 8, 30, 0);
    HostOption(v, 4, BUI_MEDIUM_UDS, ws);
    HostOption(v, 164, BUI_MEDIUM_WIFI, ws);
    Label("ROOM SIZE", 8, 132, 0);
    memset(&seg, 0, sizeof(seg));
    seg.focus = Focused(v, BUI_W_HOST_SIZE2) || Focused(v, BUI_W_HOST_SIZE3) || Focused(v, BUI_W_HOST_SIZE4);
    bui_seg(4, 144, 312, 36, kSizes, 3, v->roomSize - 2, &seg);
    for (i = 0; i < 3; i++) {
        const BuiRect r = bui_seg_rect(4, 144, 312, 36, 3, i);
        bui_widgets_add(ws, r.x, r.y, r.w, r.h, BUI_W_HOST_SIZE2 + i, BUI_WF_FOCUS);
    }
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_GHOST;
    b.glyph = "B";
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_HOST_CANCEL);
    bui_button(4, 192, 100, 40, "CANCEL", &b);
    bui_widgets_add(ws, 4, 192, 100, 40, BUI_W_HOST_CANCEL, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_PRIMARY;
    b.glyph = "A";
    b.focus = Focused(v, BUI_W_HOST_OPEN);
    bui_button(110, 192, 206, 40, "OPEN ROOM", &b);
    bui_widgets_add(ws, 110, 192, 206, 40, BUI_W_HOST_OPEN, BUI_WF_FOCUS);
}

/* ---- ONLINE ------------------------------------------------------------------------------- */

/* mp-online-hw: with [net] mp_direct naming a host, ONLINE shows three shorter cards: the two server
 * cards (off without a server) and JOIN BY ADDRESS. */
static void PaintOnlineDirect(const BuiView* v, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    char addr[40];
    BuiHeaderItem right[1];
    Card c;
    char who[32];
    Background();
    right[0] = Badge(mp->haveServer ? "SERVER SET" : "NO SERVER", BUI_BADGE_INFO);
    Header("ONLINE", 0, right, 1, ws);
    memset(&c, 0, sizeof(c));
    c.icon = BUI_ICON_GLOBE;
    c.title = "HOST ONLINE ROOM";
    c.lines[0] = mp->haveServer ? "YOU GET A 5-LETTER CODE" : "NEEDS A SERVER";
    c.off = !mp->haveServer;
    c.focus = Focused(v, BUI_W_ONLINE_HOST);
    PaintCard(8, 30, 304, 52, &c);
    bui_widgets_add(ws, 8, 30, 304, 52, BUI_W_ONLINE_HOST, BUI_WF_FOCUS | (mp->haveServer ? 0 : BUI_WF_DISABLED));
    memset(&c, 0, sizeof(c));
    c.icon = BUI_ICON_CHAIN;
    c.iconColor = BUI_TK_PINK;
    c.title = "JOIN WITH CODE";
    c.lines[0] = mp->haveServer ? "THROUGH THE SERVER" : "NEEDS A SERVER";
    c.off = !mp->haveServer;
    c.focus = Focused(v, BUI_W_ONLINE_JOIN);
    PaintCard(8, 86, 304, 52, &c);
    bui_widgets_add(ws, 8, 86, 304, 52, BUI_W_ONLINE_JOIN, BUI_WF_FOCUS | (mp->haveServer ? 0 : BUI_WF_DISABLED));
    memset(&c, 0, sizeof(c));
    snprintf(addr, sizeof(addr), "HOST %s", mp->directAddr);
    c.icon = BUI_ICON_WIFI;
    c.title = "JOIN BY ADDRESS";
    c.lines[0] = addr;
    c.lines[1] = "TYPE THE CODE ON ITS LOBBY";
    c.focus = Focused(v, BUI_W_ONLINE_DIRECT);
    PaintCard(8, 142, 304, 52, &c);
    bui_widgets_add(ws, 8, 142, 304, 52, BUI_W_ONLINE_DIRECT, BUI_WF_FOCUS);
    snprintf(who, sizeof(who), "%s · %s", v->name, ModelBadge(v->model));
    Footer2("A", "OPEN", "B", "BACK", NULL, NULL, who);
}

static void PaintOnline(const BuiView* v, BuiWidgets* ws) {
    BuiHeaderItem right[1];
    Card c;
    char who[32];
    if (v->mp.haveDirect) {
        PaintOnlineDirect(v, ws);
        return;
    }
    Background();
    right[0] = Badge("SERVER SET", BUI_BADGE_INFO);
    Header("ONLINE", 0, right, 1, ws);
    memset(&c, 0, sizeof(c));
    c.icon = BUI_ICON_GLOBE;
    c.title = "HOST ONLINE ROOM";
    c.lines[0] = "YOU GET A 5-LETTER CODE";
    c.lines[1] = "TELL IT TO YOUR FRIENDS";
    c.focus = Focused(v, BUI_W_ONLINE_HOST);
    PaintCard(8, 32, 304, 76, &c);
    bui_widgets_add(ws, 8, 32, 304, 76, BUI_W_ONLINE_HOST, BUI_WF_FOCUS);
    memset(&c, 0, sizeof(c));
    c.icon = BUI_ICON_CHAIN;
    c.iconColor = BUI_TK_PINK;
    c.title = "JOIN WITH CODE";
    c.lines[0] = "TYPE THE CODE YOUR FRIEND";
    c.lines[1] = "SEES ON THEIR BOTTOM SCREEN";
    c.focus = Focused(v, BUI_W_ONLINE_JOIN);
    PaintCard(8, 116, 304, 76, &c);
    bui_widgets_add(ws, 8, 116, 304, 76, BUI_W_ONLINE_JOIN, BUI_WF_FOCUS);
    snprintf(who, sizeof(who), "%s · %s", v->name, ModelBadge(v->model));
    Footer2("A", "OPEN", "B", "BACK", NULL, NULL, who);
}

/* ---- ROOM CODE / YOUR NAME entry ---------------------------------------------------------- */

static void EntryBoxes(const char* text, int cap, int x0, int step, int w, int y, int h, int textY) {
    const int len = (int) strlen(text);
    int i;
    for (i = 0; i < cap; i++) {
        const int x = x0 + i * step;
        const int cursor = i == len;
        bui_fill(x, y, w, h, i < len ? BUI_TK_PANEL : BUI_TK_DEEP);
        bui_stroke(x, y, w, h, cursor ? BUI_TK_FOCUS : BUI_TK_PINK, cursor ? 2 : 1);
        if (i < len) {
            char ch[2];
            ch[0] = text[i] == ' ' ? '_' : text[i];
            ch[1] = '\0';
            Centred(BUI_F1, ch, x + w / 2.0, textY, 0, 0);
        }
    }
}

static void PaintCode(const BuiView* v, BuiWidgets* ws) {
    const int len = (int) strlen(v->entry);
    char sub[12];
    BuiButton b;
    int k;
    Background();
    Header("ROOM CODE", 0, NULL, 0, ws);
    EntryBoxes(v->entry, BUI_CODE_CAP, 54, 44, 36, 28, 32, 36);
    if (len < BUI_CODE_CAP) {
        bui_fill(54 + len * 44 + 10, 52, 16, 2, BUI_TK_FOCUS);
    }
    for (k = 0; k < 32; k++) {
        char ch[2];
        const int x = 2 + (k % 8) * 40;
        const int y = 66 + (k / 8) * 32;
        memset(&b, 0, sizeof(b));
        ch[0] = kBuiCodeKeys[k];
        ch[1] = '\0';
        b.font = BUI_F1;
        b.focus = Focused(v, BUI_W_KEY0 + k);
        bui_button(x, y, 36, 30, ch, &b);
        bui_widgets_add(ws, x, y, 36, 30, BUI_W_KEY0 + k, BUI_WF_FOCUS);
    }
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_GHOST;
    b.glyph = len > 0 ? "B" : NULL; /* B deletes while there is something to delete, else it backs out */
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_ENTRY_DEL);
    bui_button(2, 198, 76, 40, "DEL", &b);
    bui_widgets_add(ws, 2, 198, 76, 40, BUI_W_ENTRY_DEL, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_GHOST;
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_ENTRY_CLEAR);
    bui_button(82, 198, 76, 40, "CLEAR", &b);
    bui_widgets_add(ws, 82, 198, 76, 40, BUI_W_ENTRY_CLEAR, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.glyph = "ST";
    b.focus = Focused(v, BUI_W_ENTRY_OK);
    if (len == BUI_CODE_CAP) {
        b.state = BUI_BTN_PRIMARY;
    } else {
        snprintf(sub, sizeof(sub), "%d OF %d", len, BUI_CODE_CAP);
        b.state = BUI_BTN_DISABLED;
        b.sub = sub;
    }
    bui_button(162, 198, 156, 40, "JOIN", &b);
    bui_widgets_add(ws, 162, 198, 156, 40, BUI_W_ENTRY_OK, BUI_WF_FOCUS | (len == BUI_CODE_CAP ? 0 : BUI_WF_DISABLED));
}

/* The name entry: the design's rename screen (crs-rename) with the [net] mp_name alphabet
 * (A-Z 0-9 - . _ and space; caps only) and 12 boxes. */
static void PaintName(const BuiView* v, BuiWidgets* ws) {
    BuiHeaderItem right[1];
    BuiButton b;
    int k;
    Background();
    right[0] = Badge("MAX 12", BUI_BADGE_MUTED);
    Header("YOUR NAME", 0, right, 1, ws);
    EntryBoxes(v->entry, BUI_NAME_CAP, 5, 26, 24, 28, 30, 35);
    for (k = 0; k < 40; k++) {
        const char c = kBuiNameKeys[k];
        char ch[2];
        const int x = 1 + (k % 10) * 32;
        const int y = 64 + (k / 10) * 32;
        memset(&b, 0, sizeof(b));
        ch[0] = c;
        ch[1] = '\0';
        b.font = c == ' ' ? BUI_F2 : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? BUI_F1 : BUI_F2X2;
        b.focus = Focused(v, BUI_W_KEY0 + k);
        bui_button(x, y, 30, 30, c == ' ' ? "SP" : ch, &b);
        bui_widgets_add(ws, x, y, 30, 30, BUI_W_KEY0 + k, BUI_WF_FOCUS);
    }
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_GHOST;
    b.glyph = v->entry[0] != '\0' ? "B" : NULL;
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_ENTRY_DEL);
    bui_button(2, 196, 76, 40, "DEL", &b);
    bui_widgets_add(ws, 2, 196, 76, 40, BUI_W_ENTRY_DEL, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_GHOST;
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_ENTRY_CLEAR);
    bui_button(82, 196, 76, 40, "CANCEL", &b);
    bui_widgets_add(ws, 82, 196, 76, 40, BUI_W_ENTRY_CLEAR, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.state = v->entry[0] != '\0' ? BUI_BTN_PRIMARY : BUI_BTN_DISABLED;
    b.glyph = "ST";
    b.focus = Focused(v, BUI_W_ENTRY_OK);
    bui_button(162, 196, 156, 40, "OK", &b);
    bui_widgets_add(ws, 162, 196, 156, 40, BUI_W_ENTRY_OK, BUI_WF_FOCUS | (v->entry[0] != '\0' ? 0 : BUI_WF_DISABLED));
}

/* ---- JOINING ------------------------------------------------------------------------------- */

/* mp-menus: in the room while it races: no mid-race joins, so this console waits for the end. */
static void JoiningRaceWait(const BuiView* v) {
    const BuiText t = Txt(BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
    bui_spinner(104, 124, v->anim);
    bui_text(BUI_F2, "RACE IN PROGRESS", 118, 120, &t);
    Centred(BUI_F2, "YOU'LL JOIN WHEN IT ENDS", 160, 146, BUI_TK_TEXT2, 1);
}

static void PaintJoining(const BuiView* v, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    const int medium = mp->joinMedium;
    const char* badge = medium == BUI_MEDIUM_UDS ? "LOCAL WIRELESS" : medium == BUI_MEDIUM_ONLINE ? "ONLINE" : "SAME WIFI";
    const BuiText t = Txt(BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
    char title[32];
    BuiButton b;
    Background();
    Header("JOINING", 1, NULL, 0, ws);
    bui_fill(24, 40, 272, 128, BUI_TK_DEEP);
    bui_stroke(24, 40, 272, 128, BUI_TK_LINE, 1);
    /* The design shows the host's pilot portrait here; a beacon does not carry the host's
     * machine, so the room's medium stands in, at the portrait's size. */
    bui_icon(medium == BUI_MEDIUM_UDS ? BUI_ICON_ANTENNA : medium == BUI_MEDIUM_ONLINE ? BUI_ICON_GLOBE : BUI_ICON_WIFI,
             144, 50, BUI_TK_CYAN, 2);
    if (mp->joinName[0] != '\0') {
        snprintf(title, sizeof(title), "%s'S ROOM", mp->joinName);
    } else {
        snprintf(title, sizeof(title), "ROOM %s", mp->code);
    }
    Centred(BUI_F1, title, 160, 90, 0, 0);
    if (mp->raceWait) {
        JoiningRaceWait(v);
    } else {
        bui_spinner(120, 124, v->anim);
        bui_text(BUI_F2, "CONNECTING…", 134, 120, &t);
        bui_badge(160 - bui_badge_width(badge) / 2, 144, badge, medium == BUI_MEDIUM_UDS ? BUI_BADGE_INFO : BUI_BADGE_CYANLINE);
    }
    memset(&b, 0, sizeof(b));
    b.glyph = "B";
    b.focus = Focused(v, BUI_W_JOIN_CANCEL);
    bui_button(80, 184, 160, 40, "CANCEL", &b);
    bui_widgets_add(ws, 80, 184, 160, 40, BUI_W_JOIN_CANCEL, BUI_WF_FOCUS);
}

/* ---- LOBBY --------------------------------------------------------------------------------- */

static int SignalBars(int rttMs) {
    if (rttMs < 0) {
        return 0;
    }
    return rttMs < 60 ? 3 : rttMs < 120 ? 2 : 1;
}

/* "0:42" */
static void Countdown(char* out, size_t cap, int secs) {
    if (secs < 0) {
        secs = 0;
    }
    snprintf(out, cap, "%d:%02d", secs / 60, secs % 60);
}

/* The course card's "X: SETTINGS" (the host): a hint like a glyph, so it is left out while the game
 * has the buttons (the SETTINGS button below it still takes a tap). */
static void SettingsHint(double cx, int y) {
    if (bui_kit_glyphs_off()) {
        return;
    }
    bui_kit_note_hint("X", (int) cx, y);
    Centred(BUI_F2, "X: SETTINGS", cx, y, BUI_TK_MUTE, 1);
}

/* A stock-pick room's slot line: where that player's top screen is, or its pick. */
static void StockSlotSub(const BuiMpView* mp, const BuiSlotView* p, char* sub, size_t cap, BuiSlot* c) {
    char t[8];
    if (p->picking == BUI_PICK_MACHINE) {
        Countdown(t, sizeof(t), mp->pickSecs);
        snprintf(sub, cap, "PICKING… %s", mp->pickSecs >= 0 ? t : "");
        c->state = BUI_SLOT_WAIT;
    } else if (p->picking == BUI_PICK_COURSE) {
        snprintf(sub, cap, "PICKING COURSE");
        c->state = BUI_SLOT_WAIT;
    } else if (p->picked) {
        snprintf(sub, cap, "%s", bui_machine_name(p->machine)); /* the badge says READY */
        c->state = BUI_SLOT_READY;
    } else if (mp->pickStep == BUI_STEP_MACHINES) {
        snprintf(sub, cap, "NO PICK YET");
        c->state = BUI_SLOT_WAIT;
    } else if (mp->pickStep != BUI_STEP_LOBBY) {
        snprintf(sub, cap, "DEFAULT MACHINE"); /* never reported a pick: Blue Falcon */
        c->pilot = 0;
        c->state = BUI_SLOT_WAIT;
    } else {
        c->state = BUI_SLOT_WAIT;
    }
}

static void LobbySlot(const BuiView* v, int s, int y, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    const BuiSlotView* p = &mp->slot[s];
    char sub[32];
    BuiSlot c;
    memset(&c, 0, sizeof(c));
    if (!p->present) {
        c.empty = 1;
        c.label = s < mp->maxPlayers ? "OPEN · WAITING" : "CLOSED";
        bui_slot(4, y, 204, 38, &c);
        return;
    }
    c.name = p->name;
    c.pilot = p->machine;
    c.me = p->me;
    c.host = p->host;
    c.you = p->me && !p->host;
    if (!p->versionOk) {
        snprintf(sub, sizeof(sub), "OTHER BUILD");
        c.state = BUI_SLOT_MISMATCH;
        c.subTint = BUI_TK_RED;
    } else if (!p->live) {
        snprintf(sub, sizeof(sub), "NOT ANSWERING"); /* the AWAY badge takes the line's right end */
        c.state = BUI_SLOT_AWAY;
        c.away = 1;
        c.subTint = BUI_TK_ORANGE;
    } else {
        if (mp->isHost && p->me) {
            /* the host's console shows every other row's ping: its own row says HOST there */
            snprintf(sub, sizeof(sub), "%s · HOST", bui_pilot_name(p->machine));
        } else if (p->me || p->rttMs < 0 || !mp->isHost) {
            snprintf(sub, sizeof(sub), "%s · %s", bui_pilot_name(p->machine), ModelBadge(p->model));
        } else {
            snprintf(sub, sizeof(sub), "%s · %dMS", bui_pilot_name(p->machine), p->rttMs);
            if (p->rttMs >= 120) {
                c.subTint = BUI_TK_ORANGE;
            }
        }
        c.state = (p->ready || p->host) ? BUI_SLOT_READY : BUI_SLOT_WAIT;
        if (mp->stockPick) {
            StockSlotSub(mp, p, sub, sizeof(sub), &c);
        }
    }
    c.sub = sub;
    if (!p->me && p->rttMs >= 0) {
        c.bars = BUI_BARS(SignalBars(p->rttMs));
    }
    bui_slot(4, y, 204, 38, &c);
    if (p->me && !mp->stockPick) {
        bui_widgets_add(ws, 4, y, 204, 38, BUI_W_LOBBY_SLOT0 + s, BUI_WF_FOCUS);
    }
}

/* The course card without the design's thumbnail (stock courses have no control-point outline
 * to draw from): the cup and the course's place in it stand in. Guests do not hear the host's
 * settings until START (the lobby HELLO does not carry them), so theirs says so. */
/* A stock-pick room's course card: every console hears the host's course (HELLO), so guests see
 * it too -- once the host has picked it, after the machines. */
static void StockCourseCard(const BuiMpView* mp, int x, int y, int w, int h) {
    char cup[16];
    char name[24];
    char laps[4];
    const int picked = mp->pickStep == BUI_STEP_READY || mp->racesDone > 0;
    if (mp->hostPicking == BUI_PICK_COURSE) {
        Centred(BUI_F1, "?", x + w / 2.0, y + 30, 0, 0);
        Centred(BUI_F2, "HOST IS", x + w / 2.0, y + 80, 0, 0);
        Centred(BUI_F2, "PICKING A", x + w / 2.0, y + 92, 0, 0);
        Centred(BUI_F2, "COURSE", x + w / 2.0, y + 104, 0, 0);
    } else if (!picked) {
        Centred(BUI_F1, "?", x + w / 2.0, y + 30, 0, 0);
        Centred(BUI_F2, "AFTER THE", x + w / 2.0, y + 80, BUI_TK_MUTE, 1);
        Centred(BUI_F2, "MACHINES", x + w / 2.0, y + 92, BUI_TK_MUTE, 1);
    } else {
        bui_course_label(mp->course, cup, sizeof(cup), name, sizeof(name));
        Centred(BUI_F2, cup, x + w / 2.0, y + 22, BUI_TK_MUTE, 1);
        snprintf(laps, sizeof(laps), "%d", (mp->course >= 30 ? mp->course - 30 : mp->course) % 6 + 1);
        Centred(BUI_FP, laps, x + w / 2.0, y + 36, 0, 0);
        FitText(name, x + 6, y + 80, w - 12, 0, 0);
        if (mp->isHost) {
            snprintf(laps, sizeof(laps), "%d", mp->laps);
            Kv(x + 6, y + 96, w - 12, "LAPS", laps, 0);
        }
    }
    if (mp->courseRefused) {
        FitText("EDIT CUP: NOT ONLINE", x + 6, y + 120, w - 12, BUI_TK_ORANGE, 1);
    } else if (picked && !mp->courseOk) {
        FitText("NOT ON THIS 3DS", x + 6, y + 120, w - 12, BUI_TK_RED, 1);
    }
    if (mp->isHost) {
        SettingsHint(x + w / 2.0, y + h - 12);
    } else {
        Centred(BUI_F2, "SET BY HOST", x + w / 2.0, y + h - 12, BUI_TK_MUTE, 1);
    }
}

static void CourseCard(const BuiView* v, int x, int y, int w, int h) {
    const BuiMpView* mp = &v->mp;
    bui_fill(x, y, w, h, BUI_TK_DEEP);
    bui_stroke(x, y, w, h, BUI_TK_LINE, 1);
    Label("COURSE", x + 6, y + 5, BUI_TK_MUTE);
    bui_fill(x + 6, y + 16, w - 12, 58, BUI_TK_BLACK);
    bui_stroke(x + 6, y + 16, w - 12, 58, BUI_TK_DASH_LINE, 1);
    if (mp->stockPick) {
        StockCourseCard(mp, x, y, w, h);
        return;
    }
    if (!mp->isHost) {
        Centred(BUI_F1, "?", x + w / 2.0, y + 30, 0, 0);
        Centred(BUI_F2, "HOST PICKS", x + w / 2.0, y + 54, BUI_TK_MUTE, 1);
        FitText("AT THE START", x + 6, y + 80, w - 12, 0, 0);
        Centred(BUI_F2, "SET BY HOST", x + w / 2.0, y + h - 12, BUI_TK_MUTE, 1);
        return;
    }
    {
        char cup[16];
        char num[4];
        char laps[4];
        char racers[8];
        const int c = mp->course >= 0 && mp->course < 24 ? mp->course : 0;
        snprintf(cup, sizeof(cup), "%s CUP", bui_cup_name(c / 6));
        snprintf(num, sizeof(num), "%d", c % 6 + 1);
        Centred(BUI_F2, cup, x + w / 2.0, y + 22, BUI_TK_MUTE, 1);
        Centred(BUI_FP, num, x + w / 2.0, y + 36, 0, 0);
        FitText(bui_course_name(c), x + 6, y + 80, w - 12, 0, 0);
        snprintf(laps, sizeof(laps), "%d", mp->laps);
        Kv(x + 6, y + 96, w - 12, "LAPS", laps, 0);
        if (mp->racers > 0) {
            snprintf(racers, sizeof(racers), "%d", mp->racers);
        } else {
            snprintf(racers, sizeof(racers), "OFF");
        }
        Kv(x + 6, y + 108, w - 12, "MACHINES", racers, 0);
        Kv(x + 6, y + 120, w - 12, "DELAY", "AUTO", 0);
        SettingsHint(x + w / 2.0, y + h - 12);
    }
}

static int CountPresent(const BuiMpView* mp) {
    int s, n = 0;
    for (s = 0; s < 4; s++) {
        n += mp->slot[s].present ? 1 : 0;
    }
    return n;
}

static int MySlotOf(const BuiMpView* mp) {
    int s;
    for (s = 0; s < 4; s++) {
        if (mp->slot[s].present && mp->slot[s].me) {
            return s;
        }
    }
    return 0;
}

/* START RACE's sub line: the players it takes (and the CPUs that fill the field), "HOLD" first
 * when the start is held to confirm. */
static void StartSub(const BuiView* v, char* sub, size_t cap) {
    const BuiMpView* mp = &v->mp;
    const int cpu = mp->racers > mp->startable ? mp->racers - mp->startable : 0;
    const char* hold = v->holdConfirm ? "HOLD · " : "";
    if (cpu > 0) {
        snprintf(sub, cap, "%s%d PLAYERS + %d CPU", hold, mp->startable, cpu);
    } else {
        snprintf(sub, cap, "%s%d PLAYERS", hold, mp->startable);
    }
}

static double HoldMeter(const BuiView* v, int id) {
    return (v->holdWidget == id && v->holdPct > 0) ? v->holdPct / 100.0 : 0;
}

/* Host: only members that went quiet (HOME, sleep) hold the room up -- START WITHOUT them. */
static int StartsWithoutAway(const BuiMpView* mp) {
    return mp->isHost && mp->startable <= 0 && mp->awayStartable >= 2;
}

static void StartWithoutButton(const BuiView* v, BuiButton* b, char* sub, size_t cap, const char** label) {
    *label = "START WITHOUT";
    snprintf(sub, cap, "%s%.12s IS AWAY", v->holdConfirm ? "HOLD · " : "", v->mp.awayName);
    b->state = BUI_BTN_PRIMARY;
    b->glyph = "A";
    b->meter = HoldMeter(v, BUI_W_LOBBY_MAIN);
}

/* Stock picks: KEEP <pilot> -- the previous pick, confirmed with one tap (the top screen's picker
 * opens on it too, for a different one). */
static void KeepButton(const BuiView* v, const char* left, BuiWidgets* ws) {
    char label[24];
    char sub[40];
    BuiButton b;
    memset(&b, 0, sizeof(b));
    snprintf(label, sizeof(label), "KEEP %s", bui_pilot_name(v->keepMachine));
    snprintf(sub, sizeof(sub), "OR PICK ON TOP · %s LEFT", left);
    b.state = BUI_BTN_PRIMARY;
    b.focus = Focused(v, BUI_W_LOBBY_KEEP);
    b.sub = sub;
    bui_button(110, 196, 206, 40, label, &b);
    bui_widgets_add(ws, 110, 196, 206, 40, BUI_W_LOBBY_KEEP, BUI_WF_FOCUS);
}

/* Stock picks: this console still owes the round's machine and has a previous pick to keep. */
static int KeepOffered(const BuiView* v) {
    const BuiMpView* mp = &v->mp;
    const BuiSlotView* me = &mp->slot[MySlotOf(mp)];
    return mp->stockPick && mp->pickStep == BUI_STEP_MACHINES && !me->picked && v->keepMachine >= 0 &&
           v->keepMachine < BUI_MACHINES && mp->pickSecs != 0;
}

/* Stock picks, the host: open machine select on every console, then START once all is in. */
static void StockHostMain(const BuiView* v, BuiButton* b, char* sub, size_t cap, const char** label) {
    const BuiMpView* mp = &v->mp;
    char t[8];
    *label = "START RACE";
    if (mp->pickStep == BUI_STEP_LOBBY) {
        *label = "MACHINE SELECT";
        snprintf(sub, cap, "EVERYONE PICKS ON TOP");
        b->state = BUI_BTN_PRIMARY;
        b->glyph = "A";
    } else if (mp->pickStep == BUI_STEP_MACHINES) {
        Countdown(t, sizeof(t), mp->pickSecs);
        if (mp->pickSecs > 0) {
            snprintf(sub, cap, "PICKING MACHINES · %s", t);
        } else {
            snprintf(sub, cap, "WAITING FOR PICKS");
        }
        b->state = BUI_BTN_DISABLED;
    } else if (mp->pickStep == BUI_STEP_COURSE) {
        if (mp->courseSecs > 0) {
            Countdown(t, sizeof(t), mp->courseSecs);
            snprintf(sub, cap, "PICK THE COURSE ON TOP · %s", t);
        } else {
            snprintf(sub, cap, "PICK THE COURSE ON TOP");
        }
        b->state = BUI_BTN_DISABLED;
    } else if (mp->startable > 0) {
        StartSub(v, sub, cap);
        b->state = BUI_BTN_PRIMARY;
        b->glyph = "A";
        b->meter = HoldMeter(v, BUI_W_LOBBY_MAIN);
    } else if (StartsWithoutAway(mp)) {
        StartWithoutButton(v, b, sub, cap, label);
    } else {
        snprintf(sub, cap, "%s", mp->whyNot);
        b->state = BUI_BTN_DISABLED;
    }
}

/* Stock picks, a guest: where its own pick is (the host drives every step). */
static void StockGuestMain(const BuiMpView* mp, BuiButton* b, char* sub, size_t cap, const char** label) {
    const BuiSlotView* me = &mp->slot[MySlotOf(mp)];
    char t[8];
    b->state = BUI_BTN_DISABLED;
    if (me->picked) {
        *label = "READY ✓";
        b->state = BUI_BTN_GO;
        b->subTint = BUI_TK_GREEN;
        snprintf(sub, cap, "%s", mp->hostPicking == BUI_PICK_COURSE ? "HOST IS PICKING A COURSE" : "WAITING FOR THE HOST");
    } else if (me->picking == BUI_PICK_MACHINE) {
        *label = "PICK ON TOP";
        Countdown(t, sizeof(t), mp->pickSecs);
        snprintf(sub, cap, "%s LEFT", t);
    } else if (mp->pickStep == BUI_STEP_LOBBY) {
        *label = "WAITING";
        snprintf(sub, cap, "HOST OPENS MACHINE SELECT"); /* fits the 206 px button (F2 184 px) */
    } else {
        *label = "WAITING";
        snprintf(sub, cap, "THE HOST IS STARTING");
    }
}

static void StockLobbyButtons(const BuiView* v, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    BuiButton b;
    char sub[40];
    const char* label = "";
    memset(&b, 0, sizeof(b));
    b.state = mp->isHost ? BUI_BTN_GHOST : BUI_BTN_DISABLED;
    b.glyph = mp->isHost ? "X" : NULL;
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_LOBBY_SIDE);
    bui_button(4, 196, 100, 40, mp->isHost ? "SETTINGS" : "MACHINE", &b);
    bui_widgets_add(ws, 4, 196, 100, 40, BUI_W_LOBBY_SIDE, BUI_WF_FOCUS | (mp->isHost ? 0 : BUI_WF_DISABLED));
    if (KeepOffered(v)) {
        char t[8];
        Countdown(t, sizeof(t), mp->pickSecs);
        KeepButton(v, t, ws);
        return;
    }
    memset(&b, 0, sizeof(b));
    b.focus = Focused(v, BUI_W_LOBBY_MAIN);
    if (mp->isHost) {
        StockHostMain(v, &b, sub, sizeof(sub), &label);
    } else {
        StockGuestMain(mp, &b, sub, sizeof(sub), &label);
    }
    b.sub = sub;
    bui_button(110, 196, 206, 40, label, &b);
    bui_widgets_add(ws, 110, 196, 206, 40, BUI_W_LOBBY_MAIN,
                    BUI_WF_FOCUS | (b.state == BUI_BTN_DISABLED || !mp->isHost ? BUI_WF_DISABLED : 0));
}

static void LobbyButtons(const BuiView* v, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    BuiButton b;
    char sub[40];
    if (mp->stockPick) {
        StockLobbyButtons(v, ws);
        return;
    }
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_GHOST;
    b.glyph = "X";
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_LOBBY_SIDE);
    bui_button(4, 196, 100, 40, mp->isHost ? "SETTINGS" : "MACHINE", &b);
    bui_widgets_add(ws, 4, 196, 100, 40, BUI_W_LOBBY_SIDE, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.focus = Focused(v, BUI_W_LOBBY_MAIN);
    if (mp->isHost) {
        const char* label = "START RACE";
        if (mp->startable > 0) {
            StartSub(v, sub, sizeof(sub));
            b.state = BUI_BTN_PRIMARY;
            b.glyph = "A";
            b.meter = HoldMeter(v, BUI_W_LOBBY_MAIN);
        } else if (StartsWithoutAway(mp)) {
            StartWithoutButton(v, &b, sub, sizeof(sub), &label);
        } else {
            snprintf(sub, sizeof(sub), "%s", mp->whyNot);
            b.state = BUI_BTN_DISABLED;
        }
        b.sub = sub;
        bui_button(110, 196, 206, 40, label, &b);
    } else if (mp->ready) {
        b.state = BUI_BTN_GO;
        b.glyph = "A";
        b.sub = "WAITING FOR THE HOST";
        b.subTint = BUI_TK_GREEN;
        bui_button(110, 196, 206, 40, "READY ✓", &b);
    } else {
        b.state = BUI_BTN_PRIMARY;
        b.glyph = "A";
        bui_button(110, 196, 206, 40, "READY!", &b);
    }
    bui_widgets_add(ws, 110, 196, 206, 40, BUI_W_LOBBY_MAIN,
                    BUI_WF_FOCUS | ((mp->isHost && mp->startable <= 0 && !StartsWithoutAway(mp)) ? BUI_WF_DISABLED : 0));
}

static void PaintLobby(const BuiView* v, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    BuiHeaderItem right[2];
    char title[32];
    char count[8];
    int s;
    Background();
    if (!mp->isLan || mp->showCode) {
        snprintf(title, sizeof(title), "ROOM %s", mp->code);
    } else {
        const char* host = mp->slot[0].present ? mp->slot[0].name : mp->joinName;
        snprintf(title, sizeof(title), "%s'S ROOM", host);
    }
    snprintf(count, sizeof(count), "%d/%d", CountPresent(mp), mp->maxPlayers);
    right[0] = mp->isUds ? Badge("LOCAL", BUI_BADGE_INFO)
                         : Badge(mp->isLan ? "WIFI" : (mp->relayOnly ? "RELAY" : "ONLINE"), BUI_BADGE_CYANLINE);
    right[1] = Badge(count, BUI_BADGE_MUTED);
    Header(title, 0, right, 2, ws);
    for (s = 0; s < 4; s++) {
        LobbySlot(v, s, 28 + s * 41, ws);
    }
    CourseCard(v, 212, 28, 104, 161);
    LobbyButtons(v, ws);
}

/* ---- MACHINE ------------------------------------------------------------------------------- */

static void PaintMachine(const BuiView* v, BuiWidgets* ws) {
    static const char kGrade[5] = {'A', 'B', 'C', 'D', 'E'};
    const int pick = v->pickSlot >= 0 && v->pickSlot < BUI_MACHINES ? v->pickSlot : 0;
    const int character = v->grid[pick];
    char key[4];
    char line[32];
    BuiButton b;
    int i;
    Background();
    Header("MACHINE", 0, NULL, 0, ws);
    for (i = 0; i < BUI_MACHINES; i++) {
        const int x = 4 + (i % 6) * 36;
        const int y = 28 + (i / 6) * 36;
        const int locked = i >= v->unlocked;
        BuiSprite sp;
        memset(&sp, 0, sizeof(sp));
        sp.dim = locked ? 0.25 : 0;
        snprintf(key, sizeof(key), "%d", locked ? i : v->grid[i]);
        bui_sprite(BUI_SET_PORTRAIT, key, x, y, &sp);
        if (locked) {
            bui_icon(BUI_ICON_LOCK, x + 8, y + 9, BUI_TK_MUTE, 1);
        }
        if (Focused(v, BUI_W_MACHINE0 + i)) {
            bui_stroke(x - 2, y - 2, 36, 36, BUI_TK_FOCUS, 2);
        }
        bui_widgets_add(ws, x, y, 32, 32, BUI_W_MACHINE0 + i, BUI_WF_FOCUS | (locked ? BUI_WF_DISABLED : 0));
    }
    bui_fill(224, 28, 92, 162, BUI_TK_DEEP);
    bui_stroke(224, 28, 92, 162, BUI_TK_LINE, 1);
    Centred(BUI_F1, bui_pilot_name(character), 270, 32, 0, 0);
    FitText(bui_machine_name(character), 228, 52, 84, BUI_TK_CYAN, 1);
    for (i = 0; i < 3; i++) {
        static const char* const kStat[3] = {"BODY", "BOOST", "GRIP"};
        char g[2];
        g[0] = kGrade[v->stats[i] >= 0 && v->stats[i] < 5 ? v->stats[i] : 2];
        g[1] = '\0';
        Kv(232, 68 + i * 12, 76, kStat[i], g, 0);
    }
    Label("COLOR", 232, 108, BUI_TK_MUTE);
    for (i = 0; i < 4; i++) {
        const BuiRgb c = ((BuiRgb) v->skinRgb[i][0] << 16) | ((BuiRgb) v->skinRgb[i][1] << 8) | v->skinRgb[i][2];
        bui_fill(232 + i * 19, 120, 16, 16, c);
        if (i == v->skin) {
            bui_stroke(230 + i * 19, 118, 20, 20, BUI_TK_FOCUS, 2);
        }
    }
    Label("ENGINE", 232, 144, BUI_TK_MUTE);
    bui_progress(232, 156, 76, 8, v->engine / 100.0, BUI_TK_CYAN);
    bui_fill(232 + (v->engine * 72) / 100, 153, 3, 14, BUI_TK_WHITE);
    Label("ACCEL", 232, 170, BUI_TK_MUTE);
    RightText("MAX", 308, 170, BUI_TK_MUTE);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_PRIMARY;
    b.glyph = "A";
    b.focus = Focused(v, BUI_W_MACHINE_OK);
    bui_button(224, 196, 92, 40, "OK", &b);
    bui_widgets_add(ws, 224, 196, 92, 40, BUI_W_MACHINE_OK, BUI_WF_FOCUS);
    snprintf(line, sizeof(line), "%d OF 30", v->unlocked);
    Centred(BUI_F2, line, 112, 214, BUI_TK_TEXT2, 1);
    Centred(BUI_F2, "UNLOCKED ON THIS 3DS", 112, 226, BUI_TK_MUTE, 1);
}

/* ---- RACE SETTINGS (host) ----------------------------------------------------------------- */

/* Stock picks: every console back into machine select (CHANGE MACHINES). Off until the round's
 * machine step is over. */
static void RepickRow(const BuiView* v, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    const int off = mp->pickStep == BUI_STEP_MACHINES || mp->pickStep == BUI_STEP_COURSE;
    BuiRow r;
    memset(&r, 0, sizeof(r));
    r.icon = BUI_ICON_REFRESH;
    r.iconColor = BUI_TK_CYAN;
    r.title = "PICK MACHINES AGAIN";
    r.meta = "EVERYONE GOES TO MACHINE SELECT";
    r.disabled = off;
    r.focus = Focused(v, BUI_W_SET_REPICK);
    bui_row(4, 170, 312, 40, &r);
    bui_widgets_add(ws, 4, 170, 312, 40, BUI_W_SET_REPICK, BUI_WF_FOCUS | (off ? BUI_WF_DISABLED : 0));
}

static void PaintSettings(const BuiView* v, BuiWidgets* ws) {
    static const char* const kRacers[5] = {"OFF", "4", "8", "16", "30"};
    static const int kRacerValue[5] = {0, 4, 8, 16, 30};
    const BuiMpView* mp = &v->mp;
    char meta[40];
    char laps[4];
    char courseName[24];
    BuiRow r;
    BuiStepper st;
    BuiSeg seg;
    int sel = 0;
    int i;
    Background();
    Header("RACE SETTINGS", 0, NULL, 0, ws);
    memset(&r, 0, sizeof(r));
    if (mp->stockPick) {
        char cup[16];
        bui_course_label(mp->course, cup, sizeof(cup), courseName, sizeof(courseName));
        snprintf(meta, sizeof(meta), "%s · PICK ON THE TOP SCREEN", cup);
        r.title = courseName;
    } else {
        snprintf(meta, sizeof(meta), "%s CUP · TAP TO CHANGE", bui_cup_name(mp->course / 6));
        r.title = bui_course_name(mp->course);
    }
    r.icon = BUI_ICON_FLAG;
    r.iconColor = BUI_TK_PINK;
    r.meta = meta;
    r.chev = 1;
    r.focus = Focused(v, BUI_W_SET_COURSE);
    bui_row(4, 28, 312, 40, &r);
    bui_widgets_add(ws, 4, 28, 312, 40, BUI_W_SET_COURSE, BUI_WF_FOCUS);
    memset(&st, 0, sizeof(st));
    st.focus = Focused(v, BUI_W_SET_LAPS);
    snprintf(laps, sizeof(laps), "%d", mp->laps);
    bui_stepper(4, 74, 312, 36, "LAPS", laps, &st);
    bui_widgets_add(ws, 4, 74, 312 - 150, 36, BUI_W_SET_LAPS, BUI_WF_FOCUS);
    bui_widgets_add(ws, 4 + 312 - 150, 76, 36, 32, BUI_W_SET_LAPS_DOWN, 0);
    bui_widgets_add(ws, 4 + 312 - 38, 76, 36, 32, BUI_W_SET_LAPS_UP, 0);
    Label("MACHINES", 8, 120, 0);
    if (mp->stockPick && mp->racersLive && mp->racersCap < 30) {
        /* The repick row takes the line below: the Old 3DS cap is said up here instead. */
        snprintf(meta, sizeof(meta), "AN OLD 3DS IS HERE: MAX %d", mp->racersCap);
        RightText(meta, 316, 120, BUI_TK_ORANGE);
    } else {
        RightText("CPU MACHINES FILL THE FIELD", 316, 120, BUI_TK_MUTE);
    }
    memset(&seg, 0, sizeof(seg));
    for (i = 0; i < 5; i++) {
        if (kRacerValue[i] > mp->racersCap) {
            seg.offMask |= 1 << i;
        }
        if (kRacerValue[i] == mp->racers) {
            sel = i;
        }
        if (Focused(v, BUI_W_SET_RACERS0 + i)) {
            seg.focus = 1;
        }
    }
    bui_seg(4, 132, 312, 32, kRacers, 5, sel, &seg);
    for (i = 0; i < 5; i++) {
        const BuiRect rr = bui_seg_rect(4, 132, 312, 32, 5, i);
        bui_widgets_add(ws, rr.x, rr.y, rr.w, rr.h, BUI_W_SET_RACERS0 + i,
                        BUI_WF_FOCUS | ((seg.offMask & (1 << i)) ? BUI_WF_DISABLED : 0));
    }
    if (mp->stockPick) {
        RepickRow(v, ws);
    } else if (mp->racersLive) {
        if (mp->racersCap < 30) {
            snprintf(meta, sizeof(meta), "AN OLD 3DS IS HERE: MAX %d MACHINES", mp->racersCap);
            Centred(BUI_F2, meta, 160, 168, BUI_TK_ORANGE, 1);
        }
    } else {
        Centred(BUI_F2, "THIS BUILD RACES UP TO 4 MACHINES", 160, 168, BUI_TK_MUTE, 1);
    }
    Footer2("A", "CHANGE", "B", "DONE", NULL, NULL, NULL);
}

/* ---- COURSE picker -------------------------------------------------------------------------- */

static void PaintCourse(const BuiView* v, BuiWidgets* ws) {
    BuiSeg seg;
    const int cup = v->cup >= 0 && v->cup < 4 ? v->cup : 0;
    int i;
    Background();
    Header("COURSE", 0, NULL, 0, ws);
    memset(&seg, 0, sizeof(seg));
    seg.font = BUI_F2;
    for (i = 0; i < 4; i++) {
        if (Focused(v, BUI_W_COURSE_CUP0 + i)) {
            seg.focus = 1;
        }
    }
    bui_seg(4, 28, 312, 26, kCup, 4, cup, &seg);
    for (i = 0; i < 4; i++) {
        const BuiRect r = bui_seg_rect(4, 28, 312, 26, 4, i);
        bui_widgets_add(ws, r.x, r.y, r.w, r.h, BUI_W_COURSE_CUP0 + i, BUI_WF_FOCUS);
    }
    for (i = 0; i < 6; i++) {
        const int x = 4 + (i % 3) * 106;
        const int y = 60 + (i / 3) * 80;
        const int w = 100, h = 76;
        const int course = cup * 6 + i;
        const int focus = Focused(v, BUI_W_COURSE0 + i);
        const int current = course == v->mp.course;
        char num[4];
        bui_fill(x, y, w, h, BUI_TK_DEEP);
        bui_stroke(x, y, w, h, focus ? BUI_TK_FOCUS : current ? BUI_TK_PINK : BUI_TK_LINE, focus ? 2 : 1);
        bui_fill(x + 4, y + 4, w - 8, 50, BUI_TK_BLACK);
        bui_stroke(x + 4, y + 4, w - 8, 50, BUI_TK_DASH_LINE, 1);
        snprintf(num, sizeof(num), "%d", i + 1);
        Centred(BUI_FP, num, x + w / 2.0, y + 13, 0, 0);
        if (current) {
            bui_badge(x + 6, y + 6, "NOW", BUI_BADGE_PINK);
        }
        {
            const char* name = bui_course_name(course);
            const int fw = bui_measure(BUI_F2, name) <= w - 8 ? BUI_F2 : BUI_F1H;
            Centred(fw, name, x + w / 2.0, y + 61, 0, 0);
        }
        bui_widgets_add(ws, x, y, w, h, BUI_W_COURSE0 + i, BUI_WF_FOCUS);
    }
    Footer2("L/R", "CUP", "A", "PICK", "B", "BACK", NULL);
}

/* ---- LAUNCH -------------------------------------------------------------------------------- */

static void PaintLaunch(const BuiView* v, BuiWidgets* ws) {
    static const BuiRgb kTag[4] = {BUI_PCOL_1, BUI_PCOL_2, BUI_PCOL_3, BUI_PCOL_4};
    const BuiMpView* mp = &v->mp;
    const BuiText cyan = Txt(BUI_TK_CYAN, 0, BUI_ALIGN_CENTER);
    int s, row = 0;
    (void) ws;
    bui_fill(0, 0, 320, 240, BUI_TK_HEADER);
    bui_checker(0, 0, 320, 240);
    Centred(BUI_F1, "GET READY", 160, 10, 0, 0);
    bui_text(BUI_F2X2, mp->isHost ? bui_course_name(mp->course) : "VS RACE", 160, 32, &cyan);
    bui_fill(80, 54, 160, 90, BUI_TK_BLACK);
    bui_stroke(80, 54, 160, 90, BUI_TK_WHITE, 1);
    bui_spinner(160, 99, v->anim);
    for (s = 0; s < 4; s++) {
        const BuiSlotView* p = &mp->slot[s];
        const int y = 152 + row * 17;
        if (!p->present) {
            continue;
        }
        bui_fill(8, y, 4, 14, kTag[row]);
        bui_text(BUI_F2, p->name, 18, y + 3, NULL);
        RightText("LOADING…", 312, y + 3, BUI_TK_TEXT2);
        row++;
    }
    bui_fill(0, 222, 320, 18, BUI_TK_HEADER);
    Centred(BUI_F2, "THE RACE STARTS WHEN ALL HAVE LOADED", 160, 227, BUI_TK_MUTE, 1);
}

/* ---- RESULTS (M3) -------------------------------------------------------------------------- */

#define RES_Y0 40
#define RES_PITCH 37
#define RES_H 34
#define RES_PITCH_TIGHT 34 /* four humans and CPUs: one line of CPU places must fit */
#define RES_H_TIGHT 32
#define RES_BOTTOM 192     /* the buttons start at 196 */
#define RES_CPU_LINE 11
#define RES_CPU_COL_W 104

static const char* PlaceSuffix(int place) {
    if (place % 100 >= 11 && place % 100 <= 13) {
        return "TH";
    }
    return place % 10 == 1 ? "ST" : place % 10 == 2 ? "ND" : place % 10 == 3 ? "RD" : "TH";
}

static void FormatRaceTime(char* out, size_t cap, int ms) {
    const int cs = ms / 10;
    snprintf(out, cap, "%d'%02d\"%02d", cs / 6000, (cs / 100) % 60, cs % 100);
}

static int ResultsHaveCpus(const BuiMpView* mp) {
    return mp->fieldCount > mp->resultCount;
}

static int ResultsPitch(const BuiMpView* mp) {
    return (mp->resultCount >= 4 && ResultsHaveCpus(mp)) ? RES_PITCH_TIGHT : RES_PITCH;
}

static void ResultPlace(int place, int x, int y) {
    BuiSprite half;
    char digits[4];
    int i;
    memset(&half, 0, sizeof(half));
    half.num = 1;
    half.den = 2;
    snprintf(digits, sizeof(digits), "%d", place);
    for (i = 0; digits[i] != '\0'; i++) {
        const char key[2] = {digits[i], '\0'};
        x += bui_sprite(BUI_SET_POSDIGIT, key, x, y, &half) - (digits[1] != '\0' ? 4 : 2);
    }
    bui_sprite(BUI_SET_POSSUFFIX, PlaceSuffix(place), x, y + 2, &half);
}

static void ResultRow(const BuiView* v, const BuiResultView* r, int y, int h) {
    static const BuiRgb kTag[4] = {BUI_PCOL_1, BUI_PCOL_2, BUI_PCOL_3, BUI_PCOL_4};
    const int me = r->slot == MySlotOf(&v->mp);
    const int dy = (h - 16) / 2; /* the 16 px lines, centred */
    const BuiText right = Txt(0, 0, BUI_ALIGN_RIGHT);
    BuiText pts;
    char key[4];
    char t[16];
    bui_fill(4, y, 312, h, me ? BUI_TK_ME_FILL : BUI_TK_DEEP);
    bui_stroke(4, y, 312, h, me ? BUI_TK_FOCUS : BUI_TK_LINE, 1);
    bui_fill(4, y, 3, h, kTag[r->player & 3]);
    ResultPlace(r->place, r->place >= 10 ? 8 : 10, y + dy); /* two digits: clear of the portrait */
    snprintf(key, sizeof(key), "%d", r->character);
    bui_sprite(BUI_SET_PORTRAIT, key, 40, y + (h - 32) / 2, NULL);
    bui_text(BUI_F1, r->name, 76, y + dy, NULL);
    if (r->byCpu) {
        const BuiText cpu = Txt(BUI_TK_ORANGE, 1, BUI_ALIGN_LEFT);
        bui_text(BUI_F2, "BY CPU", 168, y + dy + 4, &cpu);
    } else if (r->timeMs > 0) {
        FormatRaceTime(t, sizeof(t), r->timeMs);
        bui_text(BUI_FT, t, 160, y + dy, NULL);
    } else if (r->timeMs < 0) {
        /* placed, but still racing when the game ended the race (everyone else was out): no time */
        const BuiText left = Txt(BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
        bui_text(BUI_F2, "SURVIVED", 168, y + dy + 4, &left);
    } else {
        const BuiText out = Txt(BUI_TK_MUTE, 1, BUI_ALIGN_LEFT);
        bui_text(BUI_F2, "DNF", 168, y + dy + 4, &out);
    }
    pts = Txt(r->points > 0 ? 0x60FF60u : 0x8A7A84u, 0, BUI_ALIGN_LEFT);
    snprintf(t, sizeof(t), "+%d", r->points);
    bui_text(BUI_F1, t, 236, y + dy, &pts);
    snprintf(t, sizeof(t), "%d", r->total);
    bui_text(BUI_F1, t, 312, y + dy, &right);
}

/* "02 S.ARROW" for each CPU place, three columns, as many lines as fit; the last entry that
 * does not fit becomes "+N MORE". */
static void CpuPlaceGrid(const BuiMpView* mp, int y, int lines) {
    const BuiText t = Txt(BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
    const int slots = lines * 3;
    int cpus = 0;
    int shown = 0;
    int i;
    for (i = 0; i < mp->fieldCount; i++) {
        cpus += mp->fieldHuman[i] == 0;
    }
    for (i = 0; i < mp->fieldCount && shown < slots; i++) {
        char e[24];
        const int x = 6 + (shown % 3) * RES_CPU_COL_W;
        const int ly = y + (shown / 3) * RES_CPU_LINE;
        if (mp->fieldHuman[i] != 0) {
            continue;
        }
        if (shown == slots - 1 && cpus > slots) {
            snprintf(e, sizeof(e), "+%d MORE", cpus - shown);
        } else {
            snprintf(e, sizeof(e), "%02d %s", i + 1, bui_pilot_name(mp->fieldChar[i]));
        }
        bui_text(BUI_F2, e, x, ly, &t);
        shown++;
    }
}

/* One line, no names: "CPU 1 2 4 5 ..." (four humans leave room for nothing more). */
static void CpuPlaceLine(const BuiMpView* mp, int y) {
    const BuiText t = Txt(BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
    char line[96];
    int n = snprintf(line, sizeof(line), "CPU PLACES");
    int i;
    for (i = 0; i < mp->fieldCount && n < (int) sizeof(line) - 6; i++) {
        if (mp->fieldHuman[i] == 0) {
            n += snprintf(line + n, sizeof(line) - (size_t) n, " %d", i + 1);
        }
    }
    while (n > 1 && bui_measure(BUI_F2, line) > 308) {
        line[--n] = '\0';
    }
    bui_text(BUI_F2, line, 6, y, &t);
}

static void ResultsCpus(const BuiMpView* mp, int rowsEnd) {
    int lines;
    if (!ResultsHaveCpus(mp)) {
        return;
    }
    if (mp->resultCount >= 4) {
        CpuPlaceLine(mp, rowsEnd + 4);
        return;
    }
    Label("CPU PLACES", 6, rowsEnd + 3, BUI_TK_MUTE);
    lines = (RES_BOTTOM - (rowsEnd + 3 + RES_CPU_LINE)) / RES_CPU_LINE;
    CpuPlaceGrid(mp, rowsEnd + 3 + RES_CPU_LINE, lines);
}

static void ResultsHostButtons(const BuiView* v, BuiWidgets* ws) {
    BuiButton b;
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_PRIMARY;
    b.glyph = "A";
    b.focus = Focused(v, BUI_W_RESULTS_REMATCH);
    bui_button(4, 196, 140, 40, "REMATCH", &b);
    bui_widgets_add(ws, 4, 196, 140, 40, BUI_W_RESULTS_REMATCH, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.glyph = "X";
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_RESULTS_COURSE);
    bui_button(148, 196, 82, 40, "COURSE", &b);
    bui_widgets_add(ws, 148, 196, 82, 40, BUI_W_RESULTS_COURSE, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_GHOST;
    b.glyph = "Y";
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_RESULTS_MACHINES);
    bui_button(234, 196, 82, 40, "MACHINES", &b);
    bui_widgets_add(ws, 234, 196, 82, 40, BUI_W_RESULTS_MACHINES, BUI_WF_FOCUS);
}

/* screens.js waitBar: guests have no action but LEAVE; the host decides what comes next. */
static void ResultsGuestBar(const BuiView* v, BuiWidgets* ws) {
    const BuiText sub = Txt(BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
    BuiButton b;
    bui_fill(4, 196, 244, 40, BUI_TK_DEEP);
    bui_stroke(4, 196, 244, 40, BUI_TK_LINE, 1);
    bui_spinner(12, 208, 0);
    bui_text(BUI_F1, "WAITING FOR HOST", 34, 200, NULL);
    bui_text(BUI_F2, "REMATCH, COURSE OR MACHINES", 34, 222, &sub);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_GHOST;
    b.glyph = "B";
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_RESULTS_LEAVE);
    bui_button(252, 196, 64, 40, "LEAVE", &b);
    bui_widgets_add(ws, 252, 196, 64, 40, BUI_W_RESULTS_LEAVE, BUI_WF_FOCUS);
}

/* The room went away under the race (the host left, the server went down): the results stay to
 * be read; LEAVE is all there is left to do. */
static void ResultsClosedBar(const BuiView* v, BuiWidgets* ws) {
    const BuiText sub = Txt(BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
    char why[32];
    BuiButton b;
    bui_fill(4, 196, 244, 40, BUI_TK_DEEP);
    bui_stroke(4, 196, 244, 40, BUI_TK_LINE, 1);
    bui_text(BUI_F1, "ROOM CLOSED", 12, 200, NULL);
    snprintf(why, sizeof(why), "%s", v->mp.closedWhy[0] != '\0' ? v->mp.closedWhy : "THE ROOM IS GONE");
    bui_text(BUI_F2, why, 12, 222, &sub);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_PRIMARY;
    b.glyph = "B";
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_RESULTS_LEAVE);
    bui_button(252, 196, 64, 40, "LEAVE", &b);
    bui_widgets_add(ws, 252, 196, 64, 40, BUI_W_RESULTS_LEAVE, BUI_WF_FOCUS);
}

/* The header: the title in the display font unless a long course name's badge would run into
 * it (WHITE LAND 2), then in the small font. */
static void ResultsHeader(const char* title, const BuiHeaderItem* badge, BuiWidgets* ws) {
    const int titleRight = 52 + bui_measure(BUI_F1, title);
    const int badgeLeft = 314 - bui_badge_width(badge->text);
    BuiHeader h;
    memset(&h, 0, sizeof(h));
    h.right = badge;
    h.rightCount = 1;
    if (titleRight > badgeLeft - 4) {
        h.font = BUI_F2;
    }
    bui_header(title, &h);
    bui_widgets_add(ws, 0, 0, 45, 24, BUI_W_BACK, 0);
}

static void PaintResults(const BuiView* v, BuiWidgets* ws) {
    const BuiMpView* mp = &v->mp;
    const int pitch = ResultsPitch(mp);
    const int h = pitch == RES_PITCH ? RES_H : RES_H_TIGHT;
    const BuiText head = Txt(BUI_TK_MUTE, 1, BUI_ALIGN_RIGHT);
    BuiHeaderItem right[1];
    char title[32];
    char cup[16];
    char course[24];
    int i;
    Background();
    if (mp->endedBy[0] != '\0') {
        snprintf(title, sizeof(title), "ENDED BY %s", mp->endedBy); /* #10: from the pause menu */
    } else {
        snprintf(title, sizeof(title), "RACE %d RESULTS", mp->racesDone > 0 ? mp->racesDone : 1);
    }
    bui_course_label(mp->course, cup, sizeof(cup), course, sizeof(course));
    right[0] = Badge(course, BUI_BADGE_MUTED);
    ResultsHeader(title, right, ws);
    Label("PLACE", 6, 28, BUI_TK_MUTE);
    Label("TIME", 168, 28, BUI_TK_MUTE);
    Label("PTS", 236, 28, BUI_TK_MUTE);
    bui_text(BUI_F2, "TOTAL", 314, 28, &head);
    for (i = 0; i < mp->resultCount && i < 4; i++) {
        ResultRow(v, &mp->result[i], RES_Y0 + i * pitch, h);
    }
    ResultsCpus(mp, RES_Y0 + (mp->resultCount > 0 ? (mp->resultCount - 1) * pitch + h : 0));
    if (mp->roomClosed) {
        ResultsClosedBar(v, ws);
    } else if (mp->isHost) {
        ResultsHostButtons(v, ws);
    } else {
        ResultsGuestBar(v, ws);
    }
}

/* ---- COURSE EDITOR (mp-menus: the Expansion Kit editor's bottom menu, [ek] course_share) ------ */

/* The import's newest word ("IMPORTED SHARE1 INTO CUP SLOT 1"), above the bottom edge. */
static void CsStatus(const BuiView* v, int x, int y, int w) {
    if (v->cs.status[0] != '\0') {
        FitText(v->cs.status, x, y, w, BUI_TK_TEXT2, 1);
    }
}

static void PaintEditor(const BuiView* v, BuiWidgets* ws) {
    const int off = !v->cs.canWrite;
    BuiHeaderItem right[1];
    Card c;
    Background();
    right[0] = Badge("EK", BUI_BADGE_EK);
    Header("COURSE EDITOR", 1, right, 1, ws);
    memset(&c, 0, sizeof(c));
    c.icon = BUI_ICON_PLUS;
    c.iconColor = BUI_TK_GREEN;
    c.title = "IMPORT A COURSE";
    c.lines[0] = "A .GDXCOURSE FILE ON THE SD CARD";
    c.lines[1] = off ? "WAIT: THE EDITOR IS BUSY" : "INTO THE LIBRARY OR A CUP SLOT";
    c.off = off;
    c.focus = Focused(v, BUI_W_ED_IMPORT);
    PaintCard(8, 32, 304, 72, &c);
    bui_widgets_add(ws, 8, 32, 304, 72, BUI_W_ED_IMPORT, BUI_WF_FOCUS | (off ? BUI_WF_DISABLED : 0));
    memset(&c, 0, sizeof(c));
    c.icon = BUI_ICON_STAR;
    c.iconColor = BUI_TK_PINK;
    c.title = "SCAN QR PAGES";
    c.lines[0] = "THE CAMERA READS A COURSE";
    c.lines[1] = "SHOWN AS QR CODES ELSEWHERE";
    c.off = off;
    c.focus = Focused(v, BUI_W_ED_SCAN);
    PaintCard(8, 112, 304, 72, &c);
    bui_widgets_add(ws, 8, 112, 304, 72, BUI_W_ED_SCAN, BUI_WF_FOCUS | (off ? BUI_WF_DISABLED : 0));
    CsStatus(v, 10, 194, 300);
    bui_fill(0, 224, 320, 16, BUI_TK_HEADER);
    bui_fill(0, 224, 320, 1, BUI_TK_LINE);
    Centred(BUI_F2, "TAP A CARD · THE BUTTONS STAY WITH THE EDITOR", 160, 228, BUI_TK_MUTE, 1);
}

static void CsFileRow(const BuiView* v, const BuiCsFileView* f, int y, int index, BuiWidgets* ws) {
    char meta[48];
    BuiRowBadge badge;
    BuiRow r;
    const int id = BUI_W_CS_ROW0 + index;
    memset(&r, 0, sizeof(r));
    r.focus = Focused(v, id);
    if (f->ok) {
        snprintf(meta, sizeof(meta), "%d POINTS · %s", f->points, f->leaf);
        r.icon = BUI_ICON_FLAG;
        r.iconColor = BUI_TK_PINK;
        r.title = f->name;
        badge.text = f->finished ? "RACEABLE" : "UNFINISHED";
        badge.kind = f->finished ? BUI_BADGE_OK : BUI_BADGE_MUTED;
        r.badges = &badge;
        r.badgeCount = 1;
        r.chev = 1;
    } else {
        snprintf(meta, sizeof(meta), "REFUSED: %s", f->why);
        r.icon = BUI_ICON_WARN;
        r.iconColor = BUI_TK_ORANGE;
        r.title = f->leaf;
        r.titleFont = BUI_F2;
        r.disabled = 1;
        r.metaTint = BUI_TK_ORANGE;
    }
    r.meta = meta;
    bui_row(4, y, 272, 40, &r);
    bui_widgets_add(ws, 4, y, 272, 40, id, BUI_WF_FOCUS | (f->ok ? 0 : BUI_WF_DISABLED));
}

static void CsFilesEmpty(void) {
    bui_fill(4, 28, 272, 172, BUI_TK_DEEP);
    bui_stroke(4, 28, 272, 172, BUI_TK_LINE, 1);
    bui_icon(BUI_ICON_FLAG, 124, 60, BUI_TK_MUTE, 2);
    Centred(BUI_F1, "NO COURSE FILES", 140, 102, 0, 0);
    Centred(BUI_F2, "PUT .GDXCOURSE FILES IN", 140, 128, BUI_TK_TEXT2, 1);
    Centred(BUI_F2, "SDMC:/3DS/GDIFFUSER/COURSES/", 140, 140, BUI_TK_TEXT2, 1);
    Centred(BUI_F2, "THEN TAP RESCAN", 140, 160, BUI_TK_MUTE, 1);
}

static void PaintCsFiles(const BuiView* v, BuiWidgets* ws) {
    const BuiCsView* cs = &v->cs;
    const int first = (cs->page - 1) * BUI_CS_ROWS_PER_PAGE;
    BuiHeaderItem right[1];
    char count[16];
    BuiButton b;
    int i;
    Background();
    snprintf(count, sizeof(count), "%d FILE%s", cs->fileCount, cs->fileCount == 1 ? "" : "S");
    right[0] = Badge(count, BUI_BADGE_MUTED);
    Header("IMPORT A COURSE", 0, right, 1, ws);
    if (cs->fileCount == 0) {
        CsFilesEmpty();
    }
    for (i = 0; i < BUI_CS_ROWS_PER_PAGE && first + i >= 0 && first + i < cs->fileCount && first + i < BUI_CS_FILES_MAX; i++) {
        CsFileRow(v, &cs->file[first + i], 28 + i * 44, i, ws);
    }
    RailIds(280, 28, 172, cs->page, cs->pages, BUI_W_CS_UP, BUI_W_CS_DOWN, ws);
    memset(&b, 0, sizeof(b));
    b.icon = BUI_ICON_REFRESH;
    b.font = BUI_F2;
    b.state = BUI_BTN_GHOST;
    b.focus = Focused(v, BUI_W_CS_RESCAN);
    bui_button(4, 204, 100, 32, "RESCAN", &b);
    bui_widgets_add(ws, 4, 204, 100, 32, BUI_W_CS_RESCAN, BUI_WF_FOCUS);
    CsStatus(v, 110, 216, 206);
}

static void CsSlotRow(const BuiView* v, int y, int id, const char* title, const char* badgeText, int badgeKind,
                      int off, BuiWidgets* ws) {
    BuiRowBadge badge;
    BuiRow r;
    memset(&r, 0, sizeof(r));
    r.title = title;
    r.focus = Focused(v, id);
    r.disabled = off;
    if (badgeText != NULL) {
        badge.text = badgeText;
        badge.kind = badgeKind;
        r.badges = &badge;
        r.badgeCount = 1;
    }
    bui_row(4, y, 312, 24, &r);
    bui_widgets_add(ws, 4, y, 312, 24, id, BUI_WF_FOCUS | (off ? BUI_WF_DISABLED : 0));
}

static void PaintCsSlot(const BuiView* v, BuiWidgets* ws) {
    const BuiCsView* cs = &v->cs;
    char line[48];
    char title[16];
    int i;
    Background();
    Header("IMPORT INTO", 0, NULL, 0, ws);
    snprintf(line, sizeof(line), "%s · %d POINTS · %s", cs->pendingName, cs->pendingPoints,
             cs->pendingFinished ? "RACEABLE" : "UNFINISHED");
    Label(line, 8, 30, BUI_TK_WHITE);
    CsSlotRow(v, 42, BUI_W_CS_LIBRARY, "LIBRARY ONLY", cs->replacesDisk ? "REPLACES ONE" : NULL, BUI_BADGE_WARN, 0, ws);
    for (i = 0; i < BUI_CS_SLOTS; i++) {
        const int empty = cs->cup[i][0] == '\0';
        snprintf(title, sizeof(title), "EDIT CUP %d", i + 1);
        CsSlotRow(v, 68 + i * 26, BUI_W_CS_CUP0 + i, title,
                  !cs->pendingFinished ? "UNFINISHED: NO" : empty ? "EMPTY" : cs->cup[i],
                  !cs->pendingFinished ? BUI_BADGE_MUTED : empty ? BUI_BADGE_MUTED : BUI_BADGE_INFO,
                  !cs->pendingFinished, ws);
    }
    Centred(BUI_F2, "A CUP SLOT RACES IT IN THE EDIT CUP", 160, 226, BUI_TK_MUTE, 1);
}

/* The page boxes: held ones lit, three to a row. */
static void CsScanPages(const BuiCsView* cs, int x, int y) {
    int p;
    for (p = 0; p < cs->scanCount && p < 12; p++) {
        char n[4];
        const int held = (cs->scanPages >> p) & 1;
        const int bx = x + (p % 3) * 30;
        const int by = y + (p / 3) * 22;
        bui_fill(bx, by, 26, 18, held ? BUI_TK_OK_FILL : BUI_TK_DEEP);
        bui_stroke(bx, by, 26, 18, held ? BUI_TK_GREEN : BUI_TK_LINE, 1);
        snprintf(n, sizeof(n), "%d", p + 1);
        Centred(BUI_F2, n, bx + 13, by + 5, held ? BUI_TK_GREEN : BUI_TK_MUTE, 1);
    }
}

static void PaintCsScan(const BuiView* v, BuiWidgets* ws) {
    const BuiCsView* cs = &v->cs;
    char line[24];
    BuiButton b;
    Background();
    Header("SCAN QR PAGES", 1, NULL, 0, ws);
    /* the camera's picture goes in here, straight to the screen (gdx3ds_cshare_ui_scan_paint) */
    bui_fill(BUI_CS_PREVIEW_X - 2, BUI_CS_PREVIEW_Y - 2, BUI_CS_PREVIEW_W + 4, BUI_CS_PREVIEW_H + 4, BUI_TK_BLACK);
    if (!cs->cameraOn) {
        Centred(BUI_F2, "CAMERA OFF", BUI_CS_PREVIEW_X + BUI_CS_PREVIEW_W / 2.0, BUI_CS_PREVIEW_Y + 76, BUI_TK_MUTE, 1);
    }
    if (cs->scanCount > 0) {
        snprintf(line, sizeof(line), "%d OF %d", cs->scanHave, cs->scanCount);
    } else {
        snprintf(line, sizeof(line), "ANY PAGE");
    }
    Label("PAGES", 226, 30, BUI_TK_MUTE);
    RightText(line, 316, 30, BUI_TK_WHITE);
    CsScanPages(cs, 226, 44);
    memset(&b, 0, sizeof(b));
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_CS_ZOOM);
    snprintf(line, sizeof(line), "ZOOM %dX", cs->scanZoom > 0 ? cs->scanZoom : 1);
    bui_button(224, 132, 92, 28, line, &b);
    bui_widgets_add(ws, 224, 132, 92, 28, BUI_W_CS_ZOOM, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.font = BUI_F2;
    b.icon = BUI_ICON_REFRESH;
    b.focus = Focused(v, BUI_W_CS_RESTART);
    bui_button(224, 164, 92, 28, "RESTART", &b);
    bui_widgets_add(ws, 224, 164, 92, 28, BUI_W_CS_RESTART, BUI_WF_FOCUS);
    memset(&b, 0, sizeof(b));
    b.state = BUI_BTN_GHOST;
    b.glyph = "B";
    b.font = BUI_F2;
    b.focus = Focused(v, BUI_W_CS_CANCEL);
    bui_button(224, 196, 92, 40, "CANCEL", &b);
    bui_widgets_add(ws, 224, 196, 92, 40, BUI_W_CS_CANCEL, BUI_WF_FOCUS);
    CsStatus(v, 4, 200, 214);
    Centred(BUI_F2, "HOLD THE CAMERA ~15 CM AWAY", 110, 216, BUI_TK_MUTE, 1);
}

/* ---- dialogs ------------------------------------------------------------------------------- */

typedef struct DialogDef {
    int icon, tone;
    const char* title;
    const char* body[5];
    const char* code;
    BuiModalButton buttons[2];
    int buttonCount;
} DialogDef;

static const DialogDef kDialogs[BUI_DLG_COUNT] = {
    {0, 0, NULL, {NULL}, NULL, {{NULL, NULL, 0, 0, 0, NULL, 0}}, 0},
    {BUI_ICON_WARN, BUI_TONE_BAD, "WIRELESS IS OFF",
     {"THE WIRELESS SWITCH WENT OFF, SO", "THE NEARBY SEARCH STOPPED.", "", "SLIDE IT ON, THEN TRY AGAIN.", NULL},
     "UDS OFF", {{"BACK", "B", BUI_BTN_NORMAL, 0, 0, NULL, 0}, {"TRY AGAIN", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 2},
    {BUI_ICON_WARN, BUI_TONE_WARN, "CAN'T JOIN",
     {"@'S ROOM RUNS ANOTHER BUILD.", "THIS 3DS HAS #.", "", "EVERYONE NEEDS THE SAME BUILD", "(SAME VERSION, SAME EK SETTING)."},
     "VERSION", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_WARN, BUI_TONE_BAD, "RACE STOPPED",
     {"THE CONSOLES STOPPED AGREEING", "ON THE RACE (OUT OF SYNC WITH @).", "", "A REPORT WAS SAVED TO THE SD CARD.", NULL},
     "$", {{"LEAVE", "B", BUI_BTN_GHOST, 0, BUI_F2, NULL, 0}, {"LOBBY", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 2},
    {BUI_ICON_ANTENNA, BUI_TONE_BAD, "CONNECTION LOST",
     {"@ STOPPED ANSWERING", "AND THE RACE ENDED.", "", "YOU ARE STILL IN THE ROOM.", NULL},
     "$", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_ANTENNA, BUI_TONE_BAD, "ROOM CLOSED",
     {"THE HOST LEFT OR STOPPED", "ANSWERING, SO THE ROOM CLOSED.", NULL, NULL, NULL},
     "$", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_HOME, BUI_TONE_WARN, "YOU LEFT THE RACE",
     {"THE HOME MENU PAUSED THIS 3DS,", "SO THE RACE WAS CALLED OFF.", "", "YOU ARE STILL IN THE ROOM.", NULL},
     "SUSPEND", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_WARN, BUI_TONE_WARN, "CLOSE THE ROOM?",
     {"YOU ARE THE HOST. LEAVING", "CLOSES THE ROOM FOR EVERYONE.", NULL, NULL, NULL},
     NULL, {{"STAY", "B", BUI_BTN_GHOST, 0, 0, NULL, 0}, {"CLOSE", "A", BUI_BTN_DANGER, 0, 0, NULL, 0}}, 2},
    {BUI_ICON_WARN, BUI_TONE_WARN, "LEAVE THE ROOM?",
     {"YOU CAN JOIN AGAIN WHILE", "THE ROOM IS STILL OPEN.", NULL, NULL, NULL},
     NULL, {{"STAY", "B", BUI_BTN_GHOST, 0, 0, NULL, 0}, {"LEAVE", "A", BUI_BTN_DANGER, 0, 0, NULL, 0}}, 2},
    {BUI_ICON_GLOBE, BUI_TONE_BAD, "NO SERVER",
     {"CAN'T REACH THE ONLINE SERVER.", "CHECK WIFI, OR PLAY NEARBY.", NULL, NULL, NULL},
     "$", {{"BACK", "B", BUI_BTN_GHOST, 0, 0, NULL, 0}, {"RETRY", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 2},
    {BUI_ICON_WARN, BUI_TONE_BAD, "RACE DIDN'T START",
     {"A CONSOLE DID NOT LOAD IN TIME,", "SO THE RACE WAS CALLED OFF.", "", "YOU ARE STILL IN THE ROOM.", NULL},
     "LAUNCH", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_WIFI, BUI_TONE_WARN, "WIFI IS OFF",
     {"TURN WIFI ON IN THE HOME MENU,", "OR HOST OVER LOCAL WIRELESS.", NULL, NULL, NULL},
     "WIFI", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_ANTENNA, BUI_TONE_BAD, "CONNECTION LOST",
     {"@'S 3DS STOPPED ANSWERING", "AND THE ROOM CLOSED.", "", "@ WAS THE HOST.", NULL},
     "HOST LOST", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_WARN, BUI_TONE_WARN, "REPLACE?",
     {"@ TAKES THE PLACE OF THE", "COURSE THAT IS THERE NOW.", "", "THE OLD ONE IS GONE FOR GOOD.", NULL},
     "$", {{"KEEP OLD", "B", BUI_BTN_GHOST, 0, 0, NULL, 0}, {"REPLACE", "A", BUI_BTN_DANGER, 0, 0, NULL, 0}}, 2},
    {BUI_ICON_WARN, BUI_TONE_BAD, "UPDATE REQUIRED",
     {"THIS SERVER NEEDS VERSION @.", "THIS 3DS HAS VERSION ^.", "", "UPDATE THE GAME, THEN TRY AGAIN.", "%"},
     "UPDATE", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_GLOBE, BUI_TONE_WARN, "SERVER NEEDS AN UPDATE",
     {"IT KNOWS VERSIONS UP TO @.", "THIS 3DS HAS VERSION ^.", "", "WHOEVER RUNS THE SERVER NEEDS", "TO UPDATE IT."},
     "SERVER", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_WARN, BUI_TONE_WARN, "CAN'T JOIN",
     {"@ RUNS A NEWER VERSION.", "THIS 3DS HAS #.", "", "UPDATE THIS 3DS, THEN", "JOIN AGAIN."},
     "VERSION", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
    {BUI_ICON_WARN, BUI_TONE_WARN, "CAN'T JOIN",
     {"@ RUNS AN OLDER VERSION.", "THIS 3DS HAS #.", "", "THE HOST NEEDS AN UPDATE.", NULL},
     "VERSION", {{"OK", "A", BUI_BTN_PRIMARY, 0, 0, NULL, 0}}, 1},
};

const char* bui_dialog_title(int dialog) {
    return (dialog > 0 && dialog < BUI_DLG_COUNT) ? kDialogs[dialog].title : NULL;
}

int bui_dialog_buttons(int dialog) {
    return (dialog > 0 && dialog < BUI_DLG_COUNT) ? kDialogs[dialog].buttonCount : 0;
}

/* "@" in a body line is the dialog's name, "#" this build's version, "^" / "%" the view's
 * dialogMine / dialogExtra (version gating); "$" as the code is the view's dialogCode. A line that would overflow with the name in it drops the parenthesis. */
static void ExpandLine(char* out, size_t cap, const char* in, const BuiView* v) {
    size_t n = 0;
    for (; *in != '\0' && n + 1 < cap; in++) {
        const char* sub = *in == '@'   ? v->dialogName
                          : *in == '#' ? v->version
                          : *in == '^' ? v->dialogMine
                          : *in == '%' ? v->dialogExtra
                                       : NULL;
        if (sub == NULL) {
            out[n++] = *in;
            continue;
        }
        while (*sub != '\0' && n + 1 < cap) {
            out[n++] = *sub++;
        }
    }
    out[n] = '\0';
}

int bui_dialog_text(const BuiView* v, char lines[5][48]) {
    const DialogDef* d;
    int i;
    if (v->dialog <= BUI_DLG_NONE || v->dialog >= BUI_DLG_COUNT) {
        return 0;
    }
    d = &kDialogs[v->dialog];
    for (i = 0; i < 5 && d->body[i] != NULL; i++) {
        ExpandLine(lines[i], 48, d->body[i], v);
        if (bui_measure(BUI_F2, lines[i]) > 264) {
            ExpandLine(lines[i], 48, v->dialog == BUI_DLG_DESYNC ? "ON THE RACE (OUT OF SYNC)." : d->body[i], v);
        }
        if (v->dialog >= BUI_DLG_UPDATE) {
            size_t len = strlen(lines[i]);
            while (len > 0 && bui_measure(BUI_F2, lines[i]) > 264) {
                lines[i][--len] = '\0'; /* a long --update-url: what fits (the log has all of it) */
            }
        }
    }
    return i;
}

static void PaintDialog(const BuiView* v, BuiWidgets* ws, int shade) {
    const DialogDef* d = &kDialogs[v->dialog];
    char lines[5][48];
    const char* body[5];
    BuiModalButton buttons[2];
    BuiRect rects[3];
    BuiModal m;
    int n = 0, i;
    const int count = bui_dialog_text(v, lines);
    for (i = 0; i < count; i++) {
        body[n++] = lines[i];
    }
    memset(&m, 0, sizeof(m));
    m.tone = d->tone;
    m.icon = d->icon;
    m.title = d->title;
    m.body = body;
    m.bodyCount = n;
    m.code = d->code == NULL ? NULL : (strcmp(d->code, "$") == 0 ? (v->dialogCode[0] != '\0' ? v->dialogCode : NULL) : d->code);
    for (i = 0; i < d->buttonCount; i++) {
        buttons[i] = d->buttons[i];
        buttons[i].focus = i == v->dialogFocus;
        if (v->holdConfirm && bui_screen_hold_widget(v, BUI_W_DLG_BUTTON0 + i)) {
            buttons[i].sub = "HOLD TO CONFIRM";
            buttons[i].meter = HoldMeter(v, BUI_W_DLG_BUTTON0 + i);
        }
    }
    m.buttons = buttons;
    m.buttonCount = d->buttonCount;
    m.noShade = !shade;
    bui_modal(&m, rects);
    bui_widgets_clear(ws);
    for (i = 0; i < d->buttonCount; i++) {
        bui_widgets_add(ws, rects[i].x, rects[i].y, rects[i].w, rects[i].h, BUI_W_DLG_BUTTON0 + i, BUI_WF_FOCUS);
    }
}

/* ---- dispatch ------------------------------------------------------------------------------ */

typedef void (*BuiPainter)(const BuiView* v, BuiWidgets* ws);

static const BuiPainter kPainters[BUI_SCR_COUNT] = {
    NULL, PaintHome, PaintMpChoose, PaintNearby, PaintHost, PaintOnline, PaintCode,
    PaintName, PaintJoining, PaintLobby, PaintMachine, PaintSettings, PaintCourse, PaintLaunch, PaintResults,
    PaintEditor, PaintCsFiles, PaintCsSlot, PaintCsScan,
};

void bui_screen_paint(const BuiView* v, BuiWidgets* ws) {
    bui_kit_set_glyphs_off(v->buttonsOff);
    bui_widgets_clear(ws);
    if (v->screen <= BUI_SCR_NONE || v->screen >= BUI_SCR_COUNT || kPainters[v->screen] == NULL) {
        Background();
        return;
    }
    kPainters[v->screen](v, ws);
    if (v->dialog > BUI_DLG_NONE && v->dialog < BUI_DLG_COUNT) {
        PaintDialog(v, ws, 1);
    }
    if (v->toast[0] != '\0') {
        bui_toast(196, v->toast, v->toastTone, v->toastIcon);
    }
}

void bui_screen_paint_dialog(const BuiView* v, BuiWidgets* ws, int shade) {
    bui_kit_set_glyphs_off(v->buttonsOff);
    if (v->dialog > BUI_DLG_NONE && v->dialog < BUI_DLG_COUNT) {
        PaintDialog(v, ws, shade);
    }
}

/* ---- keys (mp-menus: every hint a screen draws names a key this map runs) -------------------- */

int bui_key_from_glyph(const char* glyph) {
    static const struct {
        const char* glyph;
        int key;
    } kGlyphs[] = {{"A", BUI_KEY_A}, {"B", BUI_KEY_B}, {"X", BUI_KEY_X},      {"Y", BUI_KEY_Y},
                   {"L", BUI_KEY_L}, {"R", BUI_KEY_R}, {"ST", BUI_KEY_START}};
    size_t i;
    for (i = 0; glyph != NULL && i < sizeof(kGlyphs) / sizeof(kGlyphs[0]); i++) {
        if (strcmp(glyph, kGlyphs[i].glyph) == 0) {
            return kGlyphs[i].key;
        }
    }
    return BUI_KEY_NONE;
}

/* A dialog: B is its first button (BACK / STAY / LEAVE ...), A the focused one (the last when
 * nothing is focused yet). */
static int DialogKey(const BuiView* v, int key) {
    const int n = bui_dialog_buttons(v->dialog);
    if (n <= 0) {
        return BUI_W_NONE;
    }
    if (key == BUI_KEY_B) {
        return BUI_W_DLG_BUTTON0;
    }
    if (key == BUI_KEY_A) {
        return BUI_W_DLG_BUTTON0 + ((v->dialogFocus >= 0 && v->dialogFocus < n) ? v->dialogFocus : n - 1);
    }
    return BUI_W_NONE;
}

static int AKey(const BuiView* v) {
    if (v->screen == BUI_SCR_HOST && v->focus != BUI_W_HOST_CANCEL) {
        return BUI_W_HOST_OPEN; /* the A on OPEN ROOM: the D-pad already picked connection + size */
    }
    if (v->screen == BUI_SCR_SETTINGS && v->focus == BUI_W_SET_LAPS) {
        return BUI_W_SET_LAPS_UP; /* A on LAPS steps it up (wraps), as the right arrow does */
    }
    if (v->screen == BUI_SCR_NEARBY && v->mp.radioOff) {
        return BUI_W_NEARBY_RETRY; /* the only thing to do there (HOST is off) */
    }
    if (v->screen == BUI_SCR_RESULTS && v->mp.roomClosed) {
        return BUI_W_RESULTS_LEAVE; /* the only thing to do there */
    }
    return v->focus;
}

static int BKey(const BuiView* v) {
    switch (v->screen) {
        case BUI_SCR_HOME:
        case BUI_SCR_LAUNCH:
        case BUI_SCR_EDITOR:
            return BUI_W_NONE;
        case BUI_SCR_HOST:
            return BUI_W_HOST_CANCEL;
        case BUI_SCR_JOINING:
            return BUI_W_JOIN_CANCEL;
        case BUI_SCR_CODE:
        case BUI_SCR_NAME:
            return v->entry[0] != '\0' ? BUI_W_ENTRY_DEL : BUI_W_BACK;
        case BUI_SCR_RESULTS:
            return (v->mp.isHost && !v->mp.roomClosed) ? BUI_W_BACK : BUI_W_RESULTS_LEAVE;
        case BUI_SCR_CS_SCAN:
            return BUI_W_CS_CANCEL;
        default:
            return BUI_W_BACK;
    }
}

static int XKey(const BuiView* v) {
    if (v->screen == BUI_SCR_LOBBY) {
        return BUI_W_LOBBY_SIDE;
    }
    if (v->screen == BUI_SCR_RESULTS && v->mp.isHost && !v->mp.roomClosed) {
        return BUI_W_RESULTS_COURSE;
    }
    return BUI_W_NONE;
}

static int YKey(const BuiView* v) {
    if (v->screen == BUI_SCR_MP_CHOOSE) {
        return BUI_W_CHOOSE_NAME;
    }
    if (v->screen == BUI_SCR_NEARBY && !v->mp.radioOff) {
        return BUI_W_NEARBY_HOST;
    }
    if (v->screen == BUI_SCR_RESULTS && v->mp.isHost && !v->mp.roomClosed) {
        return BUI_W_RESULTS_MACHINES;
    }
    return BUI_W_NONE;
}

/* COURSE: L / R step through the cups (each is a tab the touch can take too). */
static int CupKey(const BuiView* v, int dir) {
    const int cup = v->cup >= 0 && v->cup < 4 ? v->cup : 0;
    if (v->screen != BUI_SCR_COURSE) {
        return BUI_W_NONE;
    }
    return BUI_W_COURSE_CUP0 + (cup + (dir > 0 ? 1 : 3)) % 4;
}

int bui_screen_key(const BuiView* v, int key) {
    if (v->buttonsOff || key == BUI_KEY_NONE) {
        return BUI_W_NONE;
    }
    if (v->dialog > BUI_DLG_NONE && v->dialog < BUI_DLG_COUNT) {
        return DialogKey(v, key);
    }
    switch (key) {
        case BUI_KEY_A: return AKey(v);
        case BUI_KEY_B: return BKey(v);
        case BUI_KEY_X: return XKey(v);
        case BUI_KEY_Y: return YKey(v);
        case BUI_KEY_L: return CupKey(v, -1);
        case BUI_KEY_R: return CupKey(v, 1);
        case BUI_KEY_START:
            return (v->screen == BUI_SCR_CODE || v->screen == BUI_SCR_NAME) ? BUI_W_ENTRY_OK : BUI_W_NONE;
        default: return BUI_W_NONE;
    }
}

/* ---- D-pad on the row screens (uipolish) ----------------------------------------------------- */

static const int kSetRacerValues[5] = {0, 4, 8, 16, 30};

/* HOST: rows CONNECTION (LOCAL | WIFI), ROOM SIZE (2 | 3 | 4), and the CANCEL | OPEN ROOM bar. */
static int HostRow(int id) {
    if (id == BUI_W_HOST_LOCAL || id == BUI_W_HOST_WIFI) {
        return 0;
    }
    if (id >= BUI_W_HOST_SIZE2 && id <= BUI_W_HOST_SIZE4) {
        return 1;
    }
    if (id == BUI_W_HOST_CANCEL || id == BUI_W_HOST_OPEN) {
        return 2;
    }
    return -1;
}

static int HostRowWidget(const BuiView* v, int row) {
    if (row == 0) {
        return v->hostMedium == BUI_MEDIUM_UDS ? BUI_W_HOST_LOCAL : BUI_W_HOST_WIFI;
    }
    if (row == 1) {
        return BUI_W_HOST_SIZE2 + (v->roomSize >= 2 && v->roomSize <= 4 ? v->roomSize - 2 : 0);
    }
    return BUI_W_HOST_OPEN;
}

static int HostNav(const BuiView* v, int dx, int dy, BuiNav* out) {
    const int row = HostRow(v->focus);
    if (row < 0) {
        return 0;
    }
    if (dy != 0) {
        const int to = row + dy;
        out->focus = (to >= 0 && to <= 2) ? HostRowWidget(v, to) : v->focus;
        return 1;
    }
    if (row == 0) {
        const int to = dx > 0 ? BUI_W_HOST_WIFI : BUI_W_HOST_LOCAL;
        if (to == BUI_W_HOST_LOCAL && !v->mp.uds) {
            out->focus = v->focus;
            return 1;
        }
        out->focus = to;
        out->activate = to;
        return 1;
    }
    if (row == 1) {
        const int at = v->focus - BUI_W_HOST_SIZE2 + dx;
        out->focus = (at >= 0 && at <= 2) ? BUI_W_HOST_SIZE2 + at : v->focus;
        out->activate = out->focus;
        return 1;
    }
    out->focus = dx > 0 ? BUI_W_HOST_OPEN : BUI_W_HOST_CANCEL;
    return 1;
}

/* RACE SETTINGS: rows COURSE, LAPS (left/right steps), MACHINES (left/right picks the next
 * value the room allows), and PICK MACHINES AGAIN on a stock-pick room. */
static int SettingsRacersIndex(const BuiMpView* mp) {
    int i;
    for (i = 0; i < 5; i++) {
        if (kSetRacerValues[i] == mp->racers) {
            return i;
        }
    }
    return 0;
}

static int SettingsRow(const BuiView* v, int id) {
    if (id == BUI_W_SET_COURSE) {
        return 0;
    }
    if (id == BUI_W_SET_LAPS) {
        return 1;
    }
    if (id >= BUI_W_SET_RACERS0 && id < BUI_W_SET_RACERS0 + 5) {
        return 2;
    }
    if (id == BUI_W_SET_REPICK && v->mp.stockPick) {
        return 3;
    }
    return -1;
}

static int SettingsRowWidget(const BuiView* v, int row) {
    static const int kRows[4] = {BUI_W_SET_COURSE, BUI_W_SET_LAPS, BUI_W_SET_RACERS0, BUI_W_SET_REPICK};
    if (row == 2) {
        return BUI_W_SET_RACERS0 + SettingsRacersIndex(&v->mp);
    }
    return kRows[row];
}

static int SettingsRacersStep(const BuiView* v, int dx) {
    int at = v->focus - BUI_W_SET_RACERS0 + dx;
    for (; at >= 0 && at < 5; at += dx) {
        if (kSetRacerValues[at] <= v->mp.racersCap) {
            return BUI_W_SET_RACERS0 + at;
        }
    }
    return v->focus;
}

static int SettingsNav(const BuiView* v, int dx, int dy, BuiNav* out) {
    const int row = SettingsRow(v, v->focus);
    const int last = v->mp.stockPick ? 3 : 2;
    if (row < 0) {
        return 0;
    }
    if (dy != 0) {
        const int to = row + dy;
        out->focus = (to >= 0 && to <= last) ? SettingsRowWidget(v, to) : v->focus;
        return 1;
    }
    out->focus = v->focus;
    if (row == 1) {
        out->activate = dx > 0 ? BUI_W_SET_LAPS_UP : BUI_W_SET_LAPS_DOWN;
    } else if (row == 2) {
        out->focus = SettingsRacersStep(v, dx);
        out->activate = out->focus != v->focus ? out->focus : BUI_W_NONE;
    }
    return 1;
}

int bui_screen_nav(const BuiView* v, const BuiWidgets* ws, int dx, int dy, BuiNav* out) {
    out->focus = v->focus;
    out->activate = BUI_W_NONE;
    if (dx == 0 && dy == 0) {
        return 0;
    }
    if (v->dialog == BUI_DLG_NONE) {
        if (v->screen == BUI_SCR_HOST && HostNav(v, dx, dy, out)) {
            return 1;
        }
        if (v->screen == BUI_SCR_SETTINGS && SettingsNav(v, dx, dy, out)) {
            return 1;
        }
    }
    out->focus = bui_widgets_nav(ws, v->focus, dx, dx != 0 ? 0 : dy, v->screen == BUI_SCR_LOBBY);
    return 1;
}

/* The lobby's main button is START RACE (not READY, not MACHINE SELECT) and can start now. */
static int LobbyMainStarts(const BuiMpView* mp) {
    if (!mp->isHost || (mp->startable <= 0 && !StartsWithoutAway(mp))) {
        return 0;
    }
    return !mp->stockPick || mp->pickStep == BUI_STEP_READY;
}

int bui_screen_hold_widget(const BuiView* v, int id) {
    if (v->dialog == BUI_DLG_LEAVE_HOST || v->dialog == BUI_DLG_LEAVE_GUEST) {
        return id == BUI_W_DLG_BUTTON0 + 1; /* CLOSE / LEAVE */
    }
    if (v->dialog != BUI_DLG_NONE) {
        return 0;
    }
    return v->screen == BUI_SCR_LOBBY && id == BUI_W_LOBBY_MAIN && LobbyMainStarts(&v->mp);
}
