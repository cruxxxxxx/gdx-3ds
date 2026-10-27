/* port/3ds/gdx3ds_bui_racedash.h — the multiplayer race dash (bottom-menus design, M3): pure
 * painters for what a multiplayer race adds to the DASH (gdx3ds_bottomhud.c): the left column
 * (you, then every human by place), the field count in the header, the STANDINGS button, CPUs as
 * grey dots and humans in their player colours on the map, the NET chip, the full 30-row standings
 * overlay, the stall banner and the drop toast.
 *
 * SPLIT. Same as gdx3ds_bui_screens.h: the 3DS shell (gdx3ds_bui_race.c) fills a BuiRaceView from
 * the game (gdx_racefield_snapshot) and the MP layer (gdx3ds_mpo_race_view) and decides WHEN to
 * paint; this file only draws, through the kit (gdx3ds_bui_kit.h), into whatever surface the kit
 * targets. No <3ds.h>, no decomp, no MP headers: the host tests and the gallery render it from
 * canned views.
 *
 * Each piece paints a fixed rectangle completely (its own background included), so the shell can
 * repaint one piece alone when only its data changed. */
#ifndef GDX3DS_BUI_RACEDASH_H
#define GDX3DS_BUI_RACEDASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUI_RACE_FIELD_MAX 30
#define BUI_RACE_HUMANS_MAX 4
#define BUI_RACE_NAME_CAP 12

enum { BUI_RACER_FINISHED = 1, BUI_RACER_OUT = 2 };

typedef struct BuiRaceRacer {
    int8_t character;
    int8_t human;  /* player index + 1 for a human, 0 = a CPU */
    int8_t lap;
    int8_t state;  /* BUI_RACER_* */
    int16_t col, row; /* minimap cell (64x64 grid) */
    int32_t gapMs; /* behind the leader (0 for the leader) */
} BuiRaceRacer;

typedef struct BuiRaceView {
    int8_t count;      /* machines in the race */
    int8_t numPlayers; /* humans */
    int8_t me;         /* this console's player index */
    int8_t totalLaps;
    int8_t linkBars;   /* NET chip: 0..3 */
    int8_t delay;      /* NET chip: input delay D in frames; -1 = not shown ([debug] dev_console=0) */
    uint8_t removedMask; /* bit = player index: a CPU drives it now */
    char name[BUI_RACE_HUMANS_MAX][BUI_RACE_NAME_CAP + 1]; /* by player index */
    BuiRaceRacer byPos[BUI_RACE_FIELD_MAX];                 /* byPos[0] leads */
} BuiRaceView;

/* The left column's rectangle (the DASH's portrait + standings boxes, widened). */
#define BUI_RACE_COL_X 0
#define BUI_RACE_COL_Y 26
#define BUI_RACE_COL_W 72
#define BUI_RACE_COL_H 190
/* The STANDINGS button: the DASH's DEBUG button rect. */
#define BUI_RACE_STANDINGS_X 84
#define BUI_RACE_STANDINGS_Y 221
#define BUI_RACE_STANDINGS_W 70
#define BUI_RACE_STANDINGS_H 16
/* The stall banner, over the map. */
#define BUI_RACE_STALL_X 84
#define BUI_RACE_STALL_Y 92
#define BUI_RACE_STALL_W 154
#define BUI_RACE_STALL_H 56
/* The toast strip. */
#define BUI_RACE_TOAST_Y 192
#define BUI_RACE_TOAST_H 24

/* 1-based place of player p in the field, 0 = not in it. */
int bui_race_place_of(const BuiRaceView* v, int player);
/* The column's humans in place order: out[i] = player index; returns how many. */
int bui_race_humans_by_place(const BuiRaceView* v, int* out);

/* The column: frame (panel + border), "you" cell (big place + "OF N"), and row i (0..3) of the
 * humans by place. bui_race_column paints all three. */
void bui_race_column_frame(void);
void bui_race_column_you(const BuiRaceView* v);
void bui_race_column_row(const BuiRaceView* v, int i);
void bui_race_column(const BuiRaceView* v);

/* The header's right-hand field count ("2P+14 CPU"), right-aligned at x = right, cleared from
 * x = left first; and the STANDINGS button. */
void bui_race_header_field(const BuiRaceView* v, int left, int right, int y, int h);
void bui_race_standings_button(void);

/* Map layers drawn over the track: every CPU as a grey 3x3 dot, then the humans as 5x5 squares
 * in their player colours with a black edge. The map box is px square at (x0, y0) and shows the
 * 64x64 minimap grid. */
void bui_race_map_dots(const BuiRaceView* v, int x0, int y0, int px);
/* The NET chip (link bars and input delay D) at the map box's top-left corner (x, y). */
void bui_race_net_chip(const BuiRaceView* v, int x, int y);

/* The full standings overlay: frame (backdrop, header, footer) and row i (0..29; a row past the
 * field paints empty). bui_race_standings paints all of it. */
void bui_race_standings_frame(const BuiRaceView* v);
void bui_race_standings_row(const BuiRaceView* v, int i);
void bui_race_standings(const BuiRaceView* v);
/* The header's "LAP n/m" badge (the leader's lap), repainted when it changes. */
void bui_race_standings_lap(const BuiRaceView* v);

/* "WAITING FOR <name>" after 0.5 s of stall: the seconds, and when a CPU takes over (dropAfterMs
 * < 0: no CPU line). */
void bui_race_stall(const char* name, int waitedMs, int dropAfterMs);
/* The drop toast: "<NAME> DROPPED: A CPU DRIVES" (cpu = 1), else "<NAME> DROPPED". */
void bui_race_drop_toast(const char* name, int cpu);

/* mp-menus #10: the shared pause, over the whole dash. Every console shows who paused; only the
 * pauser's pad drives the top screen's menu, so only its card has the button (A / a touch:
 * CONTINUE at once, a row that ends the race held, its meter filling). */
typedef struct BuiRacePause {
    char name[BUI_RACE_NAME_CAP + 1]; /* who paused */
    int8_t player;       /* ... its player index (its colour) */
    int8_t mine;         /* this console paused */
    int8_t endsRace;     /* the highlighted row ends the race (all but CONTINUE) */
    char option[20];     /* the highlighted row ("QUIT") */
    int16_t holdPermille; /* the confirm hold, 0..1000 */
} BuiRacePause;
/* The pauser's button. */
#define BUI_RACE_PAUSE_BTN_X 16
#define BUI_RACE_PAUSE_BTN_Y 172
#define BUI_RACE_PAUSE_BTN_W 288
#define BUI_RACE_PAUSE_BTN_H 52
void bui_race_pause(const BuiRacePause* p);

/* Player colours P1..P4. */
uint32_t bui_race_player_colour(int player);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_BUI_RACEDASH_H */
