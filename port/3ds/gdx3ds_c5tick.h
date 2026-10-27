/* port/3ds/gdx3ds_c5tick.h — C5TICK: the per-logic-tick cost of the display-list BUILD (dlb).
 *
 * THE FACT THIS EXISTS FOR. With frameskip every logic tick pays the game's DL build
 * (func_80069698), rendered or not, and a skipped tick then drops that list unwalked
 * (gdx3ds_frameskip.h). Hardware, release preview 2, GP race: logic 1.5 + dlb 1.5-2.3 (spikes
 * to 5-10) + vi/tail/task ~0.6 ms per tick -- a skipped tick costs ~4 ms, and at D=4 three of
 * every four ticks are skipped. docs/research/native/c5tick-progress.md.
 *
 * THREE PIECES, all default OFF:
 *
 * 1. `[debug] dlbprof = 1` -- the [c5dlb] split. The decomp marks each section of the build
 *    (port/3ds/gfx/gdx3ds_dlb_shim.h, decomp-3ds-c5tick-dlb.patch: Race_Draw's sky / track /
 *    dressing / camera / HUD calls, Racer_Draw's visibility / shadow / body / booster / effects
 *    / HUD sections, Transition_Draw). A stopwatch charges marker-to-marker time to the section
 *    that opened it, separately for rendered and skipped ticks. Receipt every 64 host
 *    iterations, ms per tick of that kind:
 *      [c5dlb] frame=N kind=rend|skip n= tot= hollow= | other= rhead= sky= trk= dress= rmid=
 *              cam= vis= shd= body= boost= efx= rhud= p1hud= menus= trans=
 *    `tot` is the sum (= [c5cat] dlb for that kind, minus the GDX_CK tail).
 *
 * 2. `[debug] dlwrite = 1` -- the write-set check, the dynamic half of the side-effect proof.
 *    Every `dlwrite_every` ticks (default 4) the whole static image (.data + .bss, minus the
 *    two GfxPools, the three framebuffers and the RSP output buffer: the display list's own
 *    scratch) is hashed in `dlwrite_blk`-byte blocks (default 128) at every section marker;
 *    a block whose hash changed across a section is a byte that section WROTE. A back-to-back
 *    re-hash at the end of the tick gives the NOISE set (other threads: the audio producer,
 *    the net thread). Each (section, block) is reported once, the first time it is seen:
 *      [dlw] sec=NAME blk=0xADDR frame=N mode=M
 *    tools/native/c5tick_dlw.py maps the blocks to symbols. A section with no non-noise
 *    block across a replay changed nothing the next tick can read. Diagnostic only: it costs
 *    ~10 ms per marker in the emulator.
 *
 * 3. `[debug] dl_hollow = 1` -- THE LEVER. On a tick frameskip SKIPS, GDX_DL_HOLLOW() is 1
 *    and the decomp skips the pure sections (the ones the write-set check and the static audit
 *    clear) and runs the rest unchanged: see the patch README section for the list and the
 *    proof. The hollow list goes nowhere: osSpTaskStartGo drops the task exactly as before.
 *    Safety net: if a hollow build ever reaches osSpTaskStartGo on a tick that was NOT going
 *    to be dropped, it is dropped anyway and counted (`leak=` in the receipt, must stay 0).
 *    Receipt at init: `[c5tick] dl_hollow=... dlbprof=... dlwrite=...`.
 *
 *    `[debug] dl_hollow_poses = 1` (default): when the body section skips, every visible
 *    machine's model matrix is still built (GfxPool Mtx + racer->modelMatrix), so a skipped tick
 *    keeps producing the per-machine poses (camera matrices come from the camera update) that a
 *    pose-replay stage re-submits the last frame with. 0 drops them too (measurement).
 *
 * 4. `[debug] dl_consume_mask = N` -- MEASUREMENT ONLY, breaks the picture: marks the
 *    skippable sections in bitmask N (bit = GDX_DLB_SEC_*) consumed with no stub on rendered
 *    ticks too, so the [c5dlb] rend columns show what a native takeover of them would save.
 *
 * Every hook is one int test when its lever is off; nothing allocates unless dlwrite=1. */
#ifndef GDX3DS_C5TICK_H
#define GDX3DS_C5TICK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Host loop, once per iteration, after the frameskip decision and before gdx_vi_tick. */
void gdx3ds_c5tick_begin_tick(unsigned long frame, int skipRender);
/* Host loop, once per iteration, at the end (window receipts). */
void gdx3ds_c5tick_end_tick(unsigned long frame);

/* osSpTaskStartGo: 1 if the task being submitted carries a hollow display list (consumes the
 * flag). The caller must then drop the task. */
int gdx3ds_c5tick_take_hollow_task(void);
/* osSpTaskStartGo: a hollow task was dropped although frameskip meant to render it. */
void gdx3ds_c5tick_note_leak(void);

/* 1 while the current tick builds a hollow display list (inside Race_Draw). */
int gdx3ds_c5tick_hollow_now(void);
/* 1 between the end of a hollow build and its task's submission (frameskip must skip). */
int gdx3ds_c5tick_hollow_pending(void);

/* RENDERED-TICK CONSUMERS (task (b); each C5 stage owns its own section). A native path that
 * draws section `sec` (GDX_DLB_SEC_*, gfx/gdx3ds_dlb_shim.h) from game state on rendered
 * ticks, and never reads what the game would have written there, calls this once from its init
 * when its lever is on. From then on the decomp does not build that section inside Race_Draw;
 * `stub` (may be NULL) is called in its place with the display-list cursor and returns the new
 * cursor -- the place to write the one marker the native path hooks; a stub that returns NULL
 * declines that one build (the section is built the stock way). Refused (returns 0, with a
 * receipt) for a section that is not side-effect free (kSkippable in gdx3ds_c5tick.c): those
 * always build. Receipt: `[c5tick] consume sec=NAME stub=0|1`. */
typedef void* (*GdxDlbStub)(void* gfx, int sec, int view);
int gdx3ds_c5tick_consume(int sec, GdxDlbStub stub);

/* n60 NRR: turn the section markers on (once), then ask whether the build is inside Race_Draw. */
void gdx3ds_c5tick_want_scope(void);
int gdx3ds_c5tick_race_scope(void);
/* niview: 1 inside a non-race 3D scene the NRR takes (the records flyby; gdx_dl_scene). */
int gdx3ds_c5tick_scene_scope(void);
void gdx_dl_scene(int on);
/* nimach: same scope flag, armed by [native] nrr_mach instead of niview's records bit -- its only
 * caller is ending.c's EndingCutscene_Draw, kept apart from gdx_dl_scene so the two never leak into
 * each other's scenes. */
void gdx_dl_scene_mach(int on);
/* S5b, Expansion Kit builds only: [native] nrr_testdrive (default 0). The course editor's test drive
 * draws its race outside Race_Draw; its draw calls gdx_dl_testdrive_scope (decomp-3ds-ek-testdrive-
 * scope.patch) to open the race scope, never hollow. gdx3ds_c5tick_testdrive_scope: 1 while the build
 * is inside it (the categories that refuse GAMEMODE_COURSE_EDIT accept that one scope). */
void gdx_dl_testdrive_scope(void);
int gdx3ds_c5tick_testdrive_scope(void);

/* Decomp-facing (gdx3ds_dlb_shim.h). */
void gdx_dlb_mark(int sec);
int gdx_dl_skip(int sec, int view, void** gfx);
int gdx_dl_poses(void);
int gdx_dl_hollow(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_C5TICK_H */
