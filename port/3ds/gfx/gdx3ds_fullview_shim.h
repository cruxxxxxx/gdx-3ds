/* gdx3ds_fullview_shim.h — MP phase 3 (track mp-fullview): every console shows only its own
 * player's view, full screen, in a 2-4P VS race. The decomp side of the lever; included by
 * race.c, racer.c, hud.c, menus.c, minimap.c, camera.c and the audio thread
 * (decomp-3ds-mp-fullview.patch) under `#if defined(PORT) && defined(GDX_PLATFORM_3DS)`;
 * everywhere else the macros are constants and the game is stock.
 *
 * LEVERS (both default off; off = every macro below reads -1 / 0 and the build is stock):
 *   [net]   mp_fullview = 1      with [net] mp = host|join: k = this console's MP player index
 *   [debug] fullview_player = k  no networking needed: render player k full screen in a local VS
 *                                race ([debug] pads=4 or a replay). Wins over mp_fullview.
 *
 * THE RULE. The simulation keeps all N players. Every view still runs its camera setup, its
 * racer visibility pass (machineLod feeds the sim's RNG and the CPU AI) and every HUD / menu
 * call (they hold lap announcements, BGM, VS slot, finish logic). Only DISPLAY-LIST OUTPUT
 * changes:
 *   - views j != k: their side-effect-free sections are dropped through GDX_DL_SKIP (c5tick
 *     kSkippable: sky, track, dressing, camera, shadows, bodies, boosters, effects), and their
 *     race HUD / per-player HUD output is rewound (the calls still run);
 *   - view k: viewport + scissor become SCISSOR_BOX_FULL_SCREEN (its projection is already 4:3
 *     for every player count: fovScaleX/Y = SCREEN_WIDTH/HEIGHT), its frustum is untouched, and
 *     its HUD takes the 1P layout.
 * The latch is taken once per build at the top of Race_Draw, and is -1 while any camera runs the
 * VS end-of-race viewport transition (the stock results animation then draws every view).
 *
 * Gate: [gstate] (+ ext: machineLod != 0, soundEffectFlags) per tick identical between the split
 * screen and fullview k, every k. Contract and receipts: gdx3ds_fullview_game.c. */
#ifndef GDX3DS_FULLVIEW_SHIM_H
#define GDX3DS_FULLVIEW_SHIM_H

#if defined(PORT) && defined(GDX_PLATFORM_3DS)
/* Race_Draw top / bottom: latch / release this build's local view (-1 = off). */
extern int gdx_fv_race_draw_begin(void);
extern void gdx_fv_race_draw_end(void);
/* This build's local view k, or -1 (off, outside Race_Draw, or the results transition). */
extern int gdx_fv_k(void);
/* The scissor box view `view` draws with: SCISSOR_BOX_FULL_SCREEN for k, else `scissorBoxType`. */
extern int gdx_fv_scissor(int scissorBoxType, int view);
/* Racer_Draw's race-HUD tail of view `view`: k swaps the camera's 2D mirrors (viewport scale /
 * translation, scissor) to the full-screen ones for the markers, end restores them. */
extern void gdx_fv_rhud_begin(int view);
extern void gdx_fv_rhud_end(int view);
#define GDX_FV_K() gdx_fv_k()
/* mp-fullbleed: 1 while a fullview frame publishes the 1P widescreen frame kind ([net]
 * mp_fullview_wide, game/gdx3ds_fullview_game.c). Draw layout only. */
extern int gdx_fv_wide_single_view(void);
#define GDX_FV_WIDE_1P(j) ((GDX_FV_K() >= 0) && ((j) == GDX_FV_K()) && gdx_fv_wide_single_view())
#else
#define GDX_FV_K() (-1)
#define GDX_FV_WIDE_1P(j) 0
#endif

/* 1 when view / player `j`'s display-list output is dropped this build. */
#define GDX_FV_DROP(j) ((GDX_FV_K() >= 0) && ((j) != GDX_FV_K()))
/* 1 when `j` is the full-screen local view. */
#define GDX_FV_LOCAL(j) ((GDX_FV_K() >= 0) && ((j) == GDX_FV_K()))
/* A HUD layout index: 0 (1P) for the local view, else the stock `numPlayersIndex`. */
#define GDX_FV_NPI(npi, j) (GDX_FV_LOCAL(j) ? 0 : (npi))
/* A HUD layout player count: 1 for the local view, else gNumPlayers. Draw-layout switches only. */
#define GDX_FV_NP(j) (GDX_FV_LOCAL(j) ? 1 : gNumPlayers)

#endif /* GDX3DS_FULLVIEW_SHIM_H */
