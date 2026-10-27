#ifndef GDX_RACEMAP_H
#define GDX_RACEMAP_H

/* Race-map state sample — the source side of the bottom-screen race map
 * (port/3ds/gdx3ds_racemap.c, gdiffuser.ini [debug] bottom_map, DEFAULT OFF).
 *
 * WHY A SNAPSHOT AND NOT A DISPLAY-LIST TAP. The race minimap is not geometry.
 * `decomp/src/overlays/ovl_i3/minimap.c` builds it ONCE per race, on the CPU, as a
 * 64x64 CI8 bitmap in RDRAM (`sCourseMinimapTex`, MINIMAP_MAX_SIZE = 0x1000) with a
 * four-entry TLUT (`sCourseMinimapPalette`: CLEAR / BLACK / WHITE / GREY), and then
 * per frame merely uploads that buffer as two CI8 blocks and stamps 3x3 fill-rects
 * for the racer markers. So the whole picture is readable from plain C globals: no
 * display list has to be intercepted, re-issued or doubled, and the bottom screen
 * costs the top screen exactly zero draws. The three options the brief named —
 * (a) intercept the lists, (b) read game state and draw fresh 2D, (c) render twice —
 * collapse to (b), and (b) is cheap here only because of this one fact about
 * minimap.c. See docs/research/native/bottomhud-progress.md §2.
 *
 * Plain ints/enums so the consumer needs no decomp types (the `gdx_discord.h`
 * idiom). Filled by gdx_racemap_snapshot() in port/decomp_port.c, which is the TU
 * compiled with the decomp's headers.
 *
 * Coordinates are MINIMAP SOURCE PIXELS: column/row in the 64x64 `sCourseMinimapTex`
 * grid, i.e. exactly the `playerMarkerX`/`playerMarkerY` of
 * Minimap_DrawCourseMinimap BEFORE the N64 screen origin (`left`/`top`) is added.
 * Both are derived from the same arithmetic:
 *
 *     col = Math_Round((pos.x * 64 * scale) / 16000.0f + 64 * scale) / 2
 *     row = Math_Round((pos.z * 64 * scale) / 16000.0f + 64 * scale) / 2
 *
 * with `scale` 1.0 for one player and 0.75 otherwise — the same `scale` that
 * Minimap_InitCourseMinimap used when it rasterised the track into the buffer, so
 * markers and track always share a frame of reference.
 *
 * Nothing here is called unless the ini lever is on; with it off the cost of this
 * whole feature is one int test per frame in gdx3ds_menu_tick.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Marker kinds, in Minimap_DrawCourseMinimap's own drawing order. The colours live
 * on the 3DS side; this enum only says which MINIMAP_*_COLOR the game would use. */
typedef enum GdxRaceMapMarkerKind {
    GDX_RACEMAP_MARKER_PLAYER1 = 0, /* MINIMAP_PLAYER1_COLOR, cyan   */
    GDX_RACEMAP_MARKER_PLAYER2,     /* MINIMAP_PLAYER2_COLOR, yellow */
    GDX_RACEMAP_MARKER_PLAYER3,     /* MINIMAP_PLAYER3_COLOR, green  */
    GDX_RACEMAP_MARKER_PLAYER4,     /* MINIMAP_PLAYER4_COLOR, magenta*/
    GDX_RACEMAP_MARKER_LEADER,      /* MINIMAP_LEADER_COLOR,  blue   */
    GDX_RACEMAP_MARKER_GHOST        /* MINIMAP_GHOST_COLOR,   black  */
} GdxRaceMapMarkerKind;

#define GDX_RACEMAP_MAX_MARKERS 6

typedef struct GdxRaceMapMarker {
    int col;   /* 0..63 in the sCourseMinimapTex grid (may be out of range: clip) */
    int row;   /* 0..63, same grid                                               */
    int kind;  /* GdxRaceMapMarkerKind                                           */
    int blink; /* 1 = the game blinks this one at (gGameFrameCount % 16) < 8      */
} GdxRaceMapMarker;

typedef struct GdxRaceMapSnapshot {
    int valid;        /* 1 = a race is live and sCourseMinimapTex holds THIS race */
    int mode;         /* GET_MODE(gGameMode), for the receipt                     */
    int numPlayers;   /* gNumPlayers                                             */
    int scaleNum;     /* minimap scale as a fraction: 1/1 (1P) or 3/4 (2-4P)     */
    int scaleDen;
    int lap;          /* gRacers[0].lap, 1-based                                  */
    int totalLaps;    /* gTotalLapCount                                           */
    int position;     /* gRacers[0].position, 1-based rank                        */
    int totalRacers;  /* gTotalRacers                                             */
    int blinkOn;      /* (gGameFrameCount % 16) < 8 — evaluated game-side         */
    int markerCount;
    GdxRaceMapMarker markers[GDX_RACEMAP_MAX_MARKERS];
} GdxRaceMapSnapshot;

/* Fills `out`. Safe to call in any game mode: `valid` is 0 outside a race, and the
 * 64x64 bitmap must not be drawn then (it still holds the previous race's track).
 * Main thread only, after the game update — same contract as gdx_discord_snapshot. */
void gdx_racemap_snapshot(GdxRaceMapSnapshot* out);

/* The 64x64 CI8 track bitmap and its 4-entry TLUT, handed over as plain pointers so
 * the drawing TU never declares a decomp symbol itself. `outPalette` entries are
 * RGBA5551 ALREADY BYTE-SWAPPED BACK to host order (minimap.c stores them
 * pre-swapped under PORT for the CI decoder's big-endian TLUT read — see
 * MINIMAP_TLUT_ENTRY there; undoing it here keeps that quirk in the TU that knows
 * about it). Returns 0 and touches nothing when the bitmap is not allocated. */
int gdx_racemap_bitmap(const unsigned char** outTex, unsigned short* outPalette4);

/* ──────────────────────────────────────────────────────────────────────────────────
 * nibottom (Phase 2): the DASH dashboard's data, beyond the map alone.
 *
 * A SIBLING struct/function, not a growth of GdxRaceMapSnapshot/gdx_racemap_snapshot
 * above — deliberately. Those are the already-gated bottom_map contract
 * (bottomhud-progress.md), proven top-screen-identical with its own pixel-compare
 * gate; widening that exact struct would put a second feature's fields inside a
 * contract whose parity claim is specific to the fields that existed when it was
 * gated. This is new state for a new lever ([debug] bottom_race), same read-only-
 * snapshot idiom, same main-thread-only contract, independent gate.
 *
 * Portrait pixels are NOT included here (contrast with the original Phase 1 plan's
 * "gdx_racehud_portrait_bitmap" idea): nibottom-progress.md's asset-pipeline note
 * explains why the art is baked at build time from the real .o2r archive instead —
 * the character id below is all the painter needs to index its own baked table.
 */
#define GDX_RACEHUD_TOP5_MAX 5

typedef struct GdxRaceHudSnapshot {
    int valid;          /* 1 = a race is live (same mode gate as GdxRaceMapSnapshot) */
    int numPlayers;      /* gNumPlayers — the painter trusts the placement-mode       */
                         /* accessor (port/gdx_hud_placement.h) to force ALL_TOP      */
                         /* itself in 2-4P; this field is only for the receipt.       */
    int position;        /* gRacers[0].position, 1-based                             */
    int totalRacers;      /* gTotalRacers                                            */
    int racersRemaining;  /* gTotalRacers - gCpuRacersRetired -- the exact count      */
                           /* hud.c's own Hud_DrawRacePositionExtras shows after the   */
                           /* "/" (item 4, hw bug n60-3d7: the DASH speed/position     */
                           /* readout's "N/M" denominator, not totalRacers, so it      */
                           /* matches what the top screen showed before suppression)   */
    int speedKph;          /* round(gRacers[0].speed * 21.6f) -- same formula and      */
                           /* units as hud.c's Hud_DrawPlayerSpeed (item 4)            */
    int lap;              /* gRacers[0].lap, 1-based                                 */
    int totalLaps;        /* gTotalLapCount                                          */
    int character;        /* gRacers[0].character — indexes the painter's baked       */
                          /* portrait table                                          */
    int energyMilli;      /* round(1000 * energy / maxEnergy), 0..1000 fixed-point;   */
                          /* avoids floats in the drawing TU                         */
    int boostReady;       /* (gRacers[0].stateFlags & RACER_STATE_CAN_BOOST) != 0     */
    int koCount;           /* gRacersKOd -- the same knockout-star count hud.c's own   */
                           /* Hud_DrawKOStars draws on the top screen (item 6, hardware */
                           /* feedback round): a global race-wide counter, not keyed to */
                           /* a specific racer, incremented once per knockout anywhere  */
                           /* in the field (decomp/src/game/racer.c:2280)              */
    int lapTimeMs;         /* raceTime - completedLapsTime: the CURRENT lap's elapsed */
                           /* clock, derived (not a direct field — hud.c computes the */
                           /* same way for its own running-timer row)                 */
    int bestTimeMs;        /* min of the nonzero entries in lapTimes[3] — "best of    */
                           /* the recorded split window", NOT a certified race-best:  */
                           /* lapTimes[3] is a short window, not full race history     */
                           /* (nibottom-plan.md §2's own caveat on this field)         */
    int gapMs;             /* sPlayerLeadInterval[0], signed — raceTime minus the      */
                           /* leader/ghost's time at the same lap count. Shown every   */
                           /* bottom-screen repaint; does NOT reproduce hud.c's own    */
                           /* ~1.5s flash cadence (that reads an opaque per-player     */
                           /* static hud.c doesn't expose a field for) — a deliberate  */
                           /* simplification, documented rather than left silent.      */
    int spareMachines;     /* gPlayerLives[0] (clamped >= 0) in GP, else -1 = hidden:  */
                           /* hud.c's Hud_DrawPlayerLives draws only while gGameMode ==  */
                           /* GAMEMODE_GP_RACE (no stock in practice/TT/VS/death race) */
    int top5Count;         /* min(totalRacers, GDX_RACEHUD_TOP5_MAX)                   */
    int top5Character[GDX_RACEHUD_TOP5_MAX]; /* gRacersByPosition[0..top5Count-1]->character */
    char trackName[24];    /* gCurrentTrackName (game/common.c's sTrackNames[gCourseIndex]),  */
                            /* uppercased ASCII, truncated at the first non-ASCII byte (some   */
                            /* entries end in a FONT_SET_3-only glyph code, e.g. "sector \243\301" */
                            /* for the Greek alpha in "SECTOR ALPHA" -- "SECTOR" alone is the   */
                            /* correct, legible result, not a bug); "" if the game hasn't set   */
                            /* gCurrentTrackName yet.                                           */
} GdxRaceHudSnapshot;

/* Fills `out`. Same safety/threading contract as gdx_racemap_snapshot(). */
void gdx_racehud_snapshot(GdxRaceHudSnapshot* out);

/* ──────────────────────────────────────────────────────────────────────────────────
 * The whole field, for the multiplayer race dash (port/3ds/gdx3ds_bui_race.c): every racer in
 * race order, up to 30 (mp30). A third sibling for the same reason GdxRaceHudSnapshot is one:
 * its own consumer, its own (MP-only) caller, no change to the gated contracts above. Read-only,
 * main thread only, same contract as gdx_racemap_snapshot. */
#define GDX_RACEFIELD_MAX 30

enum {
    GDX_RACEFIELD_FINISHED = 1,
    GDX_RACEFIELD_OUT = 2 /* retired or crashed out */
};

typedef struct GdxRaceFieldEntry {
    signed char racer;     /* gRacers index; in a VS race 0..numPlayers-1 are the humans    */
    signed char character; /* indexes the portrait / pilot-name tables                     */
    signed char lap;       /* 1-based, clamped to totalLaps                                  */
    unsigned char state;   /* GDX_RACEFIELD_* bits                                           */
    short col, row;        /* minimap cell (gdx_racemap.h's grid), only when asked for       */
    int gapMs;             /* behind the leader: race-time difference once both finished,    */
                           /* else the distance gap at the leader's average speed (an        */
                           /* estimate; 0 for the leader)                                    */
    int timeMs;            /* raceTime                                                       */
} GdxRaceFieldEntry;

typedef struct GdxRaceFieldSnapshot {
    int valid;      /* same gate as GdxRaceMapSnapshot.valid                                    */
    int count;      /* gTotalRacers, clamped to GDX_RACEFIELD_MAX                                */
    int numPlayers; /* gNumPlayers                                                              */
    int totalLaps;
    GdxRaceFieldEntry byPos[GDX_RACEFIELD_MAX]; /* byPos[0] leads                             */
} GdxRaceFieldSnapshot;

/* Fills `out`; withMap = 1 also computes every racer's minimap cell (the marker formula above,
 * at the race's own scale). */
void gdx_racefield_snapshot(GdxRaceFieldSnapshot* out, int withMap);

/* 1 while the title screen's attract demo is playing a race (gTitleDemoState). The demo runs in
   an ordinary race game mode, so `valid` above is 1 during it; the 3DS bottom screen uses this
   to keep the idle view up instead of handing a demo race to the race DASH. */
int gdx_title_demo_active(void);

/* card-4b: clears an attract-demo state left armed on a settled MAIN MENU (see decomp_port.c).
   Main thread, between game frames. Returns the cleared state, 0 = none. */
int gdx_title_demo_clear_stale(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_RACEMAP_H */
