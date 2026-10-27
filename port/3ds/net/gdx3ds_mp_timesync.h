/* port/3ds/net/gdx3ds_mp_timesync.h — mp-timesync: the 3DS glue of the frame-advantage pacing
 * (the pure core and its design: port/gdx_mp_timesync.h).
 *
 * Levers:
 *   [net] mp_timesync        (default 1; 0 = off: the lead is still measured for the receipts, the
 *                             console never sleeps -- the A/B)
 *   [net] mp_timesync_max_ms (default 3, 0..16): the stretch ceiling per tick. 3 ms closes a gap of
 *                             up to ~15% between the consoles; the online1 pair (New 3DS ~60 ticks/s
 *                             vs Old 3DS ~32) needs ~15 ms: the host model shows both. card-3:
 *                             "ab" (or -1) = the per-race A/B, 3 ms vs 16 ms (port/gdx_mp_netab.h,
 *                             the [netab] receipt; the race start line then says cap_ms=<n> (A/B)).
 * Every race tick, after our pad went out and before the wait for the others' pads, the console
 * sleeps what the core says (gdx3ds_mpts_pace) and reports the sleep to the frameskip governor as a
 * lockstep wait (gdx3ds_frameskip_note_net_wait): the race0 cost guard and the steady D controller
 * (Old 3DS lock30 included) leave it out of their cost samples, so pacing never moves the render
 * divisor, and a steady slot it makes late is a net-late slot, not a missed one.
 *
 * Receipts:
 *   [mpts] race start on=1 cap_ms=3 delay=6 on/set/off=1.50/1.00/0.50 ticks
 *   [mpts] lt=599 on=1 lead=+1.02 ema=0.81..1.40 vs=p1 engaged=1 stretch_ms=1.66 paced=600/600
 *          applied_ms=996 sleep_ms=5640 max_sleep_ms=12.1 absorbed=31 engages=1 stalls=2 stall_ms=31
 *          rtt=40 pace_ms=18.4
 *     lead = the window's mean raw frame advantage (ticks) over the slowest live peer (vs=), ema =
 *     the smoothed range; stretch_ms = the mean stretch of a paced tick, applied_ms = their sum (what
 *     time sync added to the race), sleep_ms = the pacing sleeps (they include the vblank waits they
 *     replaced); absorbed = paced ticks whose pads were still missing when the sleep began and that
 *     then ran without a stall (stalls avoided, an estimate); stalls / stall_ms = this window's
 *     lockstep stalls (as [mpo] stats counts them).
 *   [mpts] race end: the same over the whole race. */
#ifndef GDX3DS_MP_TIMESYNC_H
#define GDX3DS_MP_TIMESYNC_H

#include <stdint.h>

#include "gdx_mp_lockstep.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Race tick 0: latch the levers, reset the controller. */
void gdx3ds_mpts_race_begin(const GdxMpLockstep* ls);
/* Every race tick, after SendOurPads and before WaitReady: sleeps the pacing delay. */
void gdx3ds_mpts_pace(const GdxMpLockstep* ls, uint32_t lt);
/* The tick's pads are in and it runs; `stalled` = its wait counted as a stall. */
void gdx3ds_mpts_ran(int stalled);
/* With [mpo] stats: the window receipt. `stalls` / `stallUs` are the race's running totals. */
void gdx3ds_mpts_window(uint32_t lt, uint32_t stalls, uint64_t stallUs);
/* With [mpo] race end: the whole-race receipt. */
void gdx3ds_mpts_race_end(uint32_t lt, uint32_t stalls, uint64_t stallUs);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_TIMESYNC_H */
