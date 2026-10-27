/* port/3ds/gdx3ds_bui_racedash.c — the multiplayer race dash's painters. Contract:
 * gdx3ds_bui_racedash.h. Geometry and colours follow the design's mockups (screens.js dashBase,
 * standings, raceStall). */
#include "gdx3ds_bui_racedash.h"

#include "gdx3ds_bui_kit.h"
#include "gdx3ds_bui_screens.h"

#include <stdio.h>
#include <string.h>

#define COL_FILL 0x120A10u
#define CPU_DOT 0x9A8A94u
#define CPU_TAG 0x6A5A64u
#define ME_ROW_FILL 0x4A4000u

static const BuiRgb kPlayerColour[BUI_RACE_HUMANS_MAX] = {BUI_PCOL_1, BUI_PCOL_2, BUI_PCOL_3, BUI_PCOL_4};

uint32_t bui_race_player_colour(int player) {
    return (player >= 0 && player < BUI_RACE_HUMANS_MAX) ? kPlayerColour[player] : BUI_TK_MUTE;
}

static BuiText Tint(BuiRgb tint, int solid, int align) {
    BuiText t;
    memset(&t, 0, sizeof(t));
    t.tint = tint;
    t.solid = solid;
    t.align = align;
    return t;
}

static BuiSprite Scale(int num, int den) {
    BuiSprite s;
    memset(&s, 0, sizeof(s));
    s.num = num;
    s.den = den;
    return s;
}

static const char* Suffix(int place) {
    if (place % 100 >= 11 && place % 100 <= 13) {
        return "TH";
    }
    switch (place % 10) {
        case 1:
            return "ST";
        case 2:
            return "ND";
        case 3:
            return "RD";
        default:
            return "TH";
    }
}

int bui_race_place_of(const BuiRaceView* v, int player) {
    int i;
    for (i = 0; i < v->count && i < BUI_RACE_FIELD_MAX; i++) {
        if (v->byPos[i].human == player + 1) {
            return i + 1;
        }
    }
    return 0;
}

int bui_race_humans_by_place(const BuiRaceView* v, int* out) {
    int n = 0;
    int i;
    for (i = 0; i < v->count && i < BUI_RACE_FIELD_MAX && n < BUI_RACE_HUMANS_MAX; i++) {
        if (v->byPos[i].human > 0) {
            out[n++] = v->byPos[i].human - 1;
        }
    }
    return n;
}

/* ---- the column ---------------------------------------------------------------------------- */

void bui_race_column_frame(void) {
    bui_fill(BUI_RACE_COL_X, BUI_RACE_COL_Y, BUI_RACE_COL_W, BUI_RACE_COL_H, COL_FILL);
    bui_stroke(BUI_RACE_COL_X, BUI_RACE_COL_Y, BUI_RACE_COL_W, BUI_RACE_COL_H, BUI_TK_LINE, 1);
}

/* A place as position digits (scale num/den) and its suffix; returns the x after it. */
static int PlaceDigits(int place, int x, int y, int suffixDy, int num, int den, int squeeze) {
    char digits[4];
    const BuiSprite s = Scale(num, den);
    int i;
    snprintf(digits, sizeof(digits), "%d", place);
    for (i = 0; digits[i] != '\0'; i++) {
        const char key[2] = {digits[i], '\0'};
        x += bui_sprite(BUI_SET_POSDIGIT, key, x, y, &s) - squeeze;
    }
    bui_sprite(BUI_SET_POSSUFFIX, Suffix(place), x, y + suffixDy, &s);
    return x;
}

void bui_race_column_you(const BuiRaceView* v) {
    const BuiText t = Tint(BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
    const int place = bui_race_place_of(v, v->me);
    char of[12];
    bui_fill(BUI_RACE_COL_X + 1, BUI_RACE_COL_Y + 1, BUI_RACE_COL_W - 2, 37, COL_FILL);
    if (place <= 0) {
        return;
    }
    if (place < 10) {
        PlaceDigits(place, 2, 28, 2, 1, 1, 0);
    } else {
        PlaceDigits(place, 2, 34, 2, 1, 2, 3);
    }
    snprintf(of, sizeof(of), "OF %d", v->count);
    bui_text(BUI_F2, of, 30, 52, &t);
}

void bui_race_column_row(const BuiRaceView* v, int i) {
    int order[BUI_RACE_HUMANS_MAX];
    const int n = bui_race_humans_by_place(v, order);
    const int y = 66 + i * 37;
    BuiSprite portrait;
    char key[4];
    int player;
    int place;
    bui_fill(BUI_RACE_COL_X + 1, y - 2, BUI_RACE_COL_W - 2, 36, COL_FILL);
    if (i >= n) {
        return;
    }
    player = order[i];
    place = bui_race_place_of(v, player);
    memset(&portrait, 0, sizeof(portrait));
    if ((v->removedMask >> player) & 1u) {
        portrait.dim = 0.45; /* a CPU drives it now */
    }
    snprintf(key, sizeof(key), "%d", v->byPos[place - 1].character);
    bui_sprite(BUI_SET_PORTRAIT, key, 3, y, &portrait);
    if (player == v->me) {
        bui_stroke(1, y - 2, 70, 36, BUI_TK_FOCUS, 2);
    }
    bui_fill(36, y, 3, 32, bui_race_player_colour(player));
    if (place < 10) {
        PlaceDigits(place, 42, y + 2, 2, 1, 2, 2);
    } else {
        PlaceDigits(place, 40, y + 2, 2, 1, 2, 3);
    }
}

void bui_race_column(const BuiRaceView* v) {
    int i;
    bui_race_column_frame();
    bui_race_column_you(v);
    for (i = 0; i < BUI_RACE_HUMANS_MAX; i++) {
        bui_race_column_row(v, i);
    }
}

/* ---- header -------------------------------------------------------------------------------- */

void bui_race_header_field(const BuiRaceView* v, int left, int right, int y, int h) {
    const BuiText t = Tint(0, 0, BUI_ALIGN_RIGHT);
    const int cpu = v->count > v->numPlayers ? v->count - v->numPlayers : 0;
    char text[20];
    bui_fill(left, y, right + 1 - left, h, BUI_TK_HEADER);
    if (cpu > 0) {
        snprintf(text, sizeof(text), "%dP+%d CPU", v->numPlayers, cpu);
    } else {
        snprintf(text, sizeof(text), "%d PLAYERS", v->numPlayers);
    }
    bui_text(BUI_F2, text, right, y + (h - 8) / 2, &t);
}

void bui_race_standings_button(void) {
    BuiButton b;
    memset(&b, 0, sizeof(b));
    b.font = BUI_F2;
    bui_button(BUI_RACE_STANDINGS_X, BUI_RACE_STANDINGS_Y, BUI_RACE_STANDINGS_W, BUI_RACE_STANDINGS_H, "STANDINGS",
               &b);
}

/* ---- map ----------------------------------------------------------------------------------- */

static int CellCentre(int origin, int cell, int px) {
    return origin + ((2 * cell + 1) * px) / 128;
}

static int InBox(int c, int origin, int px, int r) {
    return c - r >= origin && c + r < origin + px;
}

static void CpuDot(const BuiRaceRacer* r, int x0, int y0, int px) {
    const int cx = CellCentre(x0, r->col, px);
    const int cy = CellCentre(y0, r->row, px);
    if (InBox(cx, x0, px, 1) && InBox(cy, y0, px, 1)) {
        bui_fill(cx - 1, cy - 1, 3, 3, CPU_DOT);
    }
}

static void HumanMarker(const BuiRaceRacer* r, int x0, int y0, int px) {
    const int cx = CellCentre(x0, r->col, px);
    const int cy = CellCentre(y0, r->row, px);
    if (InBox(cx, x0, px, 3) && InBox(cy, y0, px, 3)) {
        bui_fill(cx - 3, cy - 3, 7, 7, BUI_TK_BLACK);
        bui_fill(cx - 2, cy - 2, 5, 5, bui_race_player_colour(r->human - 1));
    }
}

void bui_race_map_dots(const BuiRaceView* v, int x0, int y0, int px) {
    const BuiRaceRacer* mine = NULL;
    int i;
    for (i = v->count - 1; i >= 0; i--) {
        if (v->byPos[i].human == 0 && v->byPos[i].state != BUI_RACER_OUT) {
            CpuDot(&v->byPos[i], x0, y0, px);
        }
    }
    for (i = v->count - 1; i >= 0; i--) {
        const BuiRaceRacer* r = &v->byPos[i];
        if (r->human == 0) {
            continue;
        }
        if (r->human - 1 == v->me) {
            mine = r; /* drawn last: always on top */
            continue;
        }
        HumanMarker(r, x0, y0, px);
    }
    if (mine != NULL) {
        HumanMarker(mine, x0, y0, px);
    }
}

void bui_race_net_chip(const BuiRaceView* v, int x, int y) {
    const BuiText d = Tint(BUI_TK_TEXT2, 1, BUI_ALIGN_LEFT);
    char delay[8];
    bui_text(BUI_F2, "NET", x + 5, y + 4, NULL);
    bui_bars(x + 31, y + 2, v->linkBars);
    if (v->delay < 0) {
        return; /* players see the link bars only; the input delay is a developer number */
    }
    snprintf(delay, sizeof(delay), "D%d", v->delay);
    bui_text(BUI_F2, delay, x + 49, y + 4, &d);
}

/* ---- standings overlay --------------------------------------------------------------------- */

#define ST_ROWS_PER_COL 15
#define ST_X0 4
#define ST_COL_DX 158
#define ST_Y0 28
#define ST_ROW_DY 13
#define ST_ROW_W 154
#define ST_LAP_X 132
#define ST_LAP_Y 6
/* Up to where "TAP TO CLOSE" starts: the 4 Hz lap repaint must not clip it. */
#define ST_LAP_CLEAR_W 84

static int LeaderLap(const BuiRaceView* v) {
    return v->count > 0 ? v->byPos[0].lap : 0;
}

void bui_race_standings_lap(const BuiRaceView* v) {
    char lap[16];
    int shown = LeaderLap(v);
    if (v->totalLaps > 0 && shown > v->totalLaps) {
        shown = v->totalLaps;
    }
    bui_fill(ST_LAP_X, ST_LAP_Y - 1, ST_LAP_CLEAR_W, 14, BUI_TK_HEADER);
    snprintf(lap, sizeof(lap), "LAP %d/%d", shown, v->totalLaps);
    bui_badge(ST_LAP_X, ST_LAP_Y, lap, BUI_BADGE_MUTED);
}

void bui_race_standings_frame(const BuiRaceView* v) {
    const BuiText close = Tint(BUI_TK_MUTE, 1, BUI_ALIGN_RIGHT);
    const BuiText foot = Tint(BUI_TK_MUTE, 1, BUI_ALIGN_CENTER);
    bui_checker(0, 0, BUI_W, BUI_H);
    bui_fill(0, 0, 320, 24, BUI_TK_HEADER);
    bui_fill(0, 23, 320, 1, BUI_TK_PINK);
    bui_text(BUI_F1, "STANDINGS", 8, 4, NULL);
    bui_race_standings_lap(v);
    bui_text(BUI_F2, "TAP TO CLOSE", 314, 8, &close);
    bui_fill(0, 225, 320, 15, BUI_TK_HEADER);
    bui_text(BUI_F2, "BUTTONS KEEP DRIVING THE RACE", 160, 229, &foot);
}

static void FormatGap(char* out, size_t cap, const BuiRaceView* v, int i) {
    const BuiRaceRacer* r = &v->byPos[i];
    const int tenths = (r->gapMs + 50) / 100;
    if (i == 0) {
        snprintf(out, cap, "LEADER");
    } else if (r->state == BUI_RACER_OUT) {
        snprintf(out, cap, "OUT");
    } else if (r->lap < LeaderLap(v) && r->state != BUI_RACER_FINISHED) {
        snprintf(out, cap, "LAP %d", r->lap);
    } else if (tenths < 1000) {
        snprintf(out, cap, "+%d.%d", tenths / 10, tenths % 10);
    } else {
        snprintf(out, cap, "+%d", tenths / 10);
    }
}

/* The name, cut to fit `w` pixels of Font2. */
static void FitName(char* out, size_t cap, const char* name, int w) {
    size_t n;
    snprintf(out, cap, "%s", name);
    n = strlen(out);
    while (n > 1 && bui_measure(BUI_F2, out) > w) {
        out[--n] = '\0';
    }
}

void bui_race_standings_row(const BuiRaceView* v, int i) {
    const int x = ST_X0 + (i / ST_ROWS_PER_COL) * ST_COL_DX;
    const int y = ST_Y0 + (i % ST_ROWS_PER_COL) * ST_ROW_DY;
    const BuiRaceRacer* r = &v->byPos[i];
    const int cpu = r->human == 0;
    const int me = !cpu && r->human - 1 == v->me;
    char num[4];
    char gap[12];
    char name[BUI_RACE_NAME_CAP + 1];
    BuiText t;
    int gapW;
    bui_checker(x - 2, y - 2, ST_ROW_W, ST_ROW_DY);
    if (i >= v->count) {
        return;
    }
    if (me) {
        bui_fill(x - 2, y - 2, ST_ROW_W, 12, ME_ROW_FILL);
    }
    snprintf(num, sizeof(num), "%02d", i + 1);
    t = Tint(cpu ? BUI_TK_MUTE : BUI_TK_WHITE, 1, BUI_ALIGN_LEFT);
    bui_text(BUI_F2, num, x, y, &t);
    if (cpu) {
        bui_fill(x + 21, y + 3, 2, 2, CPU_TAG);
    } else {
        bui_fill(x + 20, y, 4, 8, bui_race_player_colour(r->human - 1));
    }
    FormatGap(gap, sizeof(gap), v, i);
    gapW = bui_measure(BUI_F2, gap);
    FitName(name, sizeof(name), cpu ? bui_pilot_name(r->character) : v->name[r->human - 1], 120 - gapW - 4);
    t = Tint(cpu ? BUI_TK_MUTE : (me ? BUI_TK_FOCUS : BUI_TK_WHITE), 1, BUI_ALIGN_LEFT);
    bui_text(BUI_F2, name, x + 30, y, &t);
    t = Tint(cpu ? CPU_TAG : BUI_TK_CYAN, 1, BUI_ALIGN_RIGHT);
    bui_text(BUI_F2, gap, x + 150, y, &t);
}

void bui_race_standings(const BuiRaceView* v) {
    int i;
    bui_race_standings_frame(v);
    for (i = 0; i < BUI_RACE_FIELD_MAX; i++) {
        bui_race_standings_row(v, i);
    }
}

/* ---- stall banner and drop toast ----------------------------------------------------------- */

/* Past this a CPU takeover is a test setting, not a promise worth a line (and would not fit). */
#define STALL_CPU_LINE_MAX_MS 99000

void bui_race_stall(const char* name, int waitedMs, int dropAfterMs) {
    const int cx = BUI_RACE_STALL_X + BUI_RACE_STALL_W / 2;
    const BuiText head = Tint(BUI_TK_ORANGE, 1, BUI_ALIGN_CENTER);
    const BuiText who = Tint(0, 0, BUI_ALIGN_CENTER);
    const BuiText sub = Tint(BUI_TK_TEXT2, 1, BUI_ALIGN_CENTER);
    const int tenths = waitedMs / 100;
    char line[40];
    char nameCut[BUI_RACE_NAME_CAP + 1];
    bui_fill(BUI_RACE_STALL_X, BUI_RACE_STALL_Y, BUI_RACE_STALL_W, BUI_RACE_STALL_H, BUI_TK_HEADER);
    bui_stroke(BUI_RACE_STALL_X, BUI_RACE_STALL_Y, BUI_RACE_STALL_W, BUI_RACE_STALL_H, BUI_TK_ORANGE, 2);
    bui_text(BUI_F2, "WAITING FOR", cx, BUI_RACE_STALL_Y + 8, &head);
    snprintf(nameCut, sizeof(nameCut), "%s", name != NULL ? name : "");
    while (strlen(nameCut) > 1 && bui_measure(BUI_F1, nameCut) > BUI_RACE_STALL_W - 8) {
        nameCut[strlen(nameCut) - 1] = '\0';
    }
    bui_text(BUI_F1, nameCut, cx, BUI_RACE_STALL_Y + 20, &who);
    if (dropAfterMs >= 0 && dropAfterMs <= STALL_CPU_LINE_MAX_MS) {
        snprintf(line, sizeof(line), "%d.%d S · CPU AFTER %d S", tenths / 10, tenths % 10, (dropAfterMs + 999) / 1000);
    } else {
        snprintf(line, sizeof(line), "%d.%d S", tenths / 10, tenths % 10);
    }
    bui_text(BUI_F2, line, cx, BUI_RACE_STALL_Y + 42, &sub);
}

void bui_race_drop_toast(const char* name, int cpu) {
    char text[48];
    snprintf(text, sizeof(text), cpu ? "%.12s DROPPED: A CPU DRIVES" : "%.12s DROPPED", name != NULL ? name : "");
    bui_toast(BUI_RACE_TOAST_Y, text, BUI_TONE_WARN, BUI_ICON_CPU);
}

/* ---- the shared pause (mp-menus #10) -------------------------------------------------------- */

static void PauseWho(const BuiRacePause* p) {
    const BuiText head = Tint(BUI_TK_MUTE, 1, BUI_ALIGN_CENTER);
    const BuiText who = Tint(bui_race_player_colour(p->player), 1, BUI_ALIGN_CENTER);
    char nameCut[BUI_RACE_NAME_CAP + 1];
    bui_fill(16, 36, 288, 76, BUI_TK_DEEP);
    bui_stroke(16, 36, 288, 76, BUI_TK_LINE, 1);
    bui_text(BUI_F2, "PAUSED BY", 160, 46, &head);
    snprintf(nameCut, sizeof(nameCut), "%s", p->name);
    while (strlen(nameCut) > 1 && bui_measure(BUI_F1, nameCut) > 272) {
        nameCut[strlen(nameCut) - 1] = '\0';
    }
    bui_text(BUI_F1, nameCut, 160, 62, &who);
    {
        const BuiText sub = Tint(BUI_TK_TEXT2, 1, BUI_ALIGN_CENTER);
        bui_text(BUI_F2, "EVERY CONSOLE IS PAUSED", 160, 92, &sub);
    }
}

static void PauseOthersCard(const BuiRacePause* p) {
    const BuiText t = Tint(BUI_TK_TEXT2, 1, BUI_ALIGN_CENTER);
    char line[48];
    bui_fill(16, 124, 288, 100, BUI_TK_PANEL);
    bui_stroke(16, 124, 288, 100, BUI_TK_LINE, 1);
    snprintf(line, sizeof(line), "ONLY %.12s CAN RESUME", p->name);
    bui_text(BUI_F2, line, 160, 140, &t);
    bui_text(BUI_F2, "OR END THE RACE", 160, 156, &t);
    if (p->option[0] != '\0') {
        const BuiText sel = Tint(p->endsRace ? BUI_TK_ORANGE : BUI_TK_CYAN, 1, BUI_ALIGN_CENTER);
        snprintf(line, sizeof(line), "ON %.12s'S MENU: %s", p->name, p->option);
        bui_text(BUI_F2, line, 160, 190, &sel);
    }
}

static void PauseMineButton(const BuiRacePause* p) {
    const BuiText t = Tint(BUI_TK_TEXT2, 1, BUI_ALIGN_CENTER);
    char sub[48];
    BuiButton b;
    memset(&b, 0, sizeof(b));
    b.glyph = "A";
    b.font = BUI_F1;
    if (p->endsRace) {
        b.state = BUI_BTN_DANGER;
        b.meter = p->holdPermille / 1000.0;
        snprintf(sub, sizeof(sub), "HOLD · %s ENDS IT FOR EVERYONE", p->option);
        b.sub = sub;
        bui_text(BUI_F2, "YOUR MENU IS ON THE TOP SCREEN", 160, 140, &t);
        bui_button(BUI_RACE_PAUSE_BTN_X, BUI_RACE_PAUSE_BTN_Y, BUI_RACE_PAUSE_BTN_W, BUI_RACE_PAUSE_BTN_H, "END RACE",
                   &b);
        return;
    }
    b.state = BUI_BTN_PRIMARY;
    b.sub = "EVERY CONSOLE RESUMES";
    bui_text(BUI_F2, "YOUR MENU IS ON THE TOP SCREEN", 160, 140, &t);
    bui_button(BUI_RACE_PAUSE_BTN_X, BUI_RACE_PAUSE_BTN_Y, BUI_RACE_PAUSE_BTN_W, BUI_RACE_PAUSE_BTN_H, "CONTINUE", &b);
}

void bui_race_pause(const BuiRacePause* p) {
    BuiHeader h;
    bui_checker(0, 0, BUI_W, BUI_H);
    memset(&h, 0, sizeof(h));
    h.noBack = 1;
    bui_header("RACE PAUSED", &h);
    PauseWho(p);
    if (p->mine) {
        bui_fill(16, 124, 288, 100, BUI_TK_DEEP);
        PauseMineButton(p);
    } else {
        PauseOthersCard(p);
    }
}
