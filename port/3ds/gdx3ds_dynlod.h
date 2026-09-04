/* port/3ds/gdx3ds_dynlod.h — DYNLOD (LOCKED-60 Task J): automatic rival-detail tier.
 *
 * The decomp's RIVAL-DETAIL hook (decomp-port-rival-detail.patch, Racer_Draw reads
 * gdx_rival_detail_level once per frame) biases the LOD tiers of the non-player machines:
 * 0=NATIVE 1=REDUCED 2=MINIMAL. Hand-set MINIMAL is worth +8-9 fps in 30-machine crowds
 * on hardware but costs distant detail everywhere else. This controller measures the
 * render task's wall time per frame (one svcGetSystemTick pair around gdx_gfx_run — the
 * render thread's job in pipe mode, the inline path otherwise; no gputrace needed) and
 * moves the EFFECTIVE tier between the user's manual setting (the FLOOR) and MINIMAL (the
 * ceiling) with hysteresis:
 *   - raise one tier when the last GDX_DYNLOD_RAISE_FRAMES frames average above
 *     [perf] rival_auto_hi_x10 / 10 ms (default 15.0), at most once per
 *     GDX_DYNLOD_DWELL_FRAMES frames so a change can land before it is judged;
 *   - lower one tier when a whole [perf] rival_auto_lower_frames (default 30) frame block
 *     AVERAGES below [perf] rival_auto_lo_x10 / 10 ms (default 12.0) — a block average,
 *     not a consecutive run, so +-2 ms frame jitter cannot pin the tier at the ceiling.
 * Killswitch [perf] rival_detail_auto=0 (default 1, read live via the menu latch): the
 * effective level is exactly the manual setting and the tick pair is skipped, so the old
 * path is byte-identical.
 *
 * Threads: note_task_* run on whichever thread executes the gfx task (render thread in
 * pipe/sync mode, main inline); the tier is a single aligned int read by Racer_Draw on
 * main once per frame (single writer, benign one-frame lag). The receipt is drained by
 * the bridge on the [race-dl] cadence from the same thread that runs the task. */
#ifndef GDX3DS_DYNLOD_H
#define GDX3DS_DYNLOD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Latch config ([perf] rival_detail_auto + thresholds). Called from gdx3ds_menu_init;
 * also lazily from the first task note if that ever runs first. */
void gdx3ds_dynlod_init(void);

/* Killswitch latch (menu row + config). */
int gdx3ds_dynlod_auto_enabled(void);
void gdx3ds_dynlod_set_auto(int on);

/* The effective rival-detail level for the decomp hook: max(floor, auto tier) with the
 * controller on, exactly `floor` with it off. */
int gdx3ds_dynlod_effective_level(int floorLevel);
/* The controller's current tier (0..2) for the menu's live row. */
int gdx3ds_dynlod_tier(void);

/* One pair per gfx task around gdx_gfx_run. Both are no-ops with the killswitch off. */
void gdx3ds_dynlod_task_begin(void);
void gdx3ds_dynlod_task_end(void);

/* [dynlod] receipt: formats one line (no newline) and resets the window counters.
 * Returns the length, 0 when nothing was measured in the window. */
int gdx3ds_dynlod_receipt(char* buf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_DYNLOD_H */
