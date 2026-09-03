# HOME-press crash audit (read-only, 2026-09-01) — .3dsx under Luma hbldr, instant on press

## Suspend sequence our process executes
Main thread `main_3ds.cpp:815 aptMainLoop` -> libctru `aptJumpToHomeMenu`:
`APT_PrepareToJumpToHomeMenu` -> `aptCallHook(ONSUSPEND)` (LIFO: citro3d `C3Di_AptEventHook`
registered at `gfx_citro3d.cpp:905 C3D_Init` runs first, then ours `main_3ds.cpp:577/219`) ->
`GSPGPU_SaveVramSysArea` -> `aptScreenTransfer(HOME, sysApplet)` -> `aptDspSleep` =
`dspCallHook(ONSLEEP)` -> ndsp `ndspFinalize(true)` (pipe cmd 3, polls DSP_RecvData, then
`bDspReady=false`, closes irqEvent/dspSem) -> `GSPGPU_ReleaseRight` -> `APT_JumpToHomeMenu`.
"Instant" = fault inside that window or in a thread reacting to it.

## Threads still running during the window
- ndsp drain thread `gdx3ds_audio_ndsp.c:346` (prio 0x18, core 2->1->0), polls every 2 ms,
  calls `DSP_FlushDataCache` + `ndspChnWaveBufAdd` (`:268-274`), no suspend check.
- HLE producer `gdx3ds_audio_ndsp.c:653` / `gdx_audio_thread.cpp:238-289`, only backs off on
  `gdx3ds_audio_buffered()`; spins in drop-oldest (`:521-529`) once the ring fills.
- watchdog `main_3ds.cpp:773` (svc + filelog only) — harmless.
- libctru ndsp sync / gsp event / apt event threads; one idle LUS BS::thread_pool worker.
- Game fibers are real threads parked on LightEvent (`gdx_fiber_3ds.c:135-139`); only the main
  thread's gdx_fiber_switch runs one, and main is blocked in APT -> cannot touch GPU/console.

## Ranked causes
1. Drain thread drives the DSP through ndsp's suspend (`:268-284`): DSP_FlushDataCache IPC on
   the shared dspHandle + waveBuf mutation while ndspFinalize(true) runs and ndsp's sync thread
   is in its bEnteringSleep path. libctru contract: only ndsp touches the DSP across sleep.
   Individual calls look like they degrade to error Results, so "top suspect", not proven.
   Also `AudioOutLogf` (`:147-162`) printf()s to the non-thread-safe console from this thread.
   FIX: ONSUSPEND/ONSLEEP -> set sAudioSuspended, drain loop parks on a LightEvent (no DSP
   calls, no printf) until ONRESTORE/ONWAKEUP; on restore `ndspChnWaveBufClear(chn)` + reset
   slot statuses to FREE and let the loop resubmit (ndspInitialize(true) re-dirties channels,
   stale QUEUED slots would otherwise replay).
2. HLE producer thread unthrottled during suspend. FIX: same flag; block in AudioThreadMain
   until restore.
3. GPU/GSP at the pump point: no violation found. No C3D frame open at loop top (EndFrame at
   `gfx_citro3d.cpp:1149`); citro3d's ONSUSPEND hook (base.c:42-46) waits the gx queue idle;
   async GX_TextureCopy (`gfx_citro3d.cpp:2781`) goes through the bound queue; stereo right
   target is just a second linked target. Trap: `gxCmdQueueClear` svcBreak(USERBREAK_PANIC)s if
   the queue is still running -> a "prefetch abort (svcBreak: panic)" dump with pc in
   C3Di_AptEventHook means a GX command was submitted off-main during the hook.
4. Bottom screen / GSPLCD: consoleInit (`gdx3ds_fps_hud.c:82`, `gdx3ds_menu.c:635`) is the
   standard setup; holding gsp::Lcd (`gdx3ds_menu.c:358`) is legal. Issue: screen-off active ->
   HOME menu opens with bottom backlight off (`:367`). FIX: GSPLCD_PowerOnBacklight(BOTTOM) in
   ONSUSPEND, re-apply saved state in ONRESTORE.
5. Restore-side gaps: hbldr exheader has use_cpu_clockrate_804MHz=false, enable_l2c=false
   (Luma hbldr.c:268-269) -> NS re-applies 268 MHz on resume; our one-shot
   osSetSpeedupEnable(true) (`gdx3ds_audio_ndsp.c:324`) is lost, libctru never re-issues it.
   APT_SetAppCpuTimeLimit(30) (`:342`) likewise. FIX: in ONRESTORE re-run osSetSpeedupEnable(true)
   and APT_SetAppCpuTimeLimit if a core-1 rung was taken.
6. HBL vs CIA: Luma 3dsx loader sets only RUNFLAG_APTCHAINLOAD; APT not crippled, HOME allowed.
   hbldr exheader: ideal core 0, prio 0x30, N3DS PROD memory, core-2 access via exflags.
   CIA rsf differs only in 804MHz/L2 on and RunnableOnSleep:false.
7. Ruled out: main-thread stack overflow (__stacksize__ 0x8000, stack nearly empty at loop top).
8. sm64_3ds: no APT/DSP hooks, same unguarded audio-thread pattern, aptMainLoop at loop top.
   Not evidence of safety.

## Dump discriminators (read first)
"Current process" (ours vs menu/ns = fault outside our address space); core id (2 or 1 =>
drain/producer, #1/#2; 0 => main/ndsp/apt/gsp threads); exception type (svcBreak panic =>
gxCmdQueueClear or our fatalTerminateHandler; data abort FAR~0 => null handle after
ndspFinalize).

## Symbolizing a Luma3DS ARM11 dump (sdmc:/luma/dumps/arm11/crash_dump_*.dmp)
Layout: magic DEADC0DE DEADCAFE; +0x08 version; +0x0C processor (lo16=11, hi16=core); +0x10
type (0 FIQ,1 undef,2 prefetch,3 data abort); +0x14 total; +0x18 register bytes; +0x1C code
size; +0x20 stack size; +0x24 extra size; +0x28 regs r0..r12, sp, lr, pc, cpsr, dfsr, ifsr,
far, fpexc, fpinst, fpinst2; then code bytes ending at pc, stack bytes from sp, then 8-byte
process name + u64 title id.
ELF: build-3ds/port/3ds/G-Diffuser-3DS.elf (.text 0x00100000-0x003217D8 for the Aug 28 build;
must be the exact build on the card — check the HUD build id). Both hbldr and CIA map .text at
0x00100000.
`/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line -e <elf> -f -C -i -p 0x<pc> 0x<lr>`;
backtrace = run every stack word in .text range through the same.
Parser: scratchpad/luma_dump.py crash.dmp [--elf ...]. Official alternative:
pip install git+https://github.com/LumaTeam/luma3ds_exception_dump_parser.git

## Fix (feat/3ds-home)
Contract fix, no killswitch. Suspend side (causes #1/#2): `main_3ds.cpp aptLifecycleHook`
ONSUSPEND/ONSLEEP -> `gdx3ds_audio_suspend()` (`gdx3ds_audio_ndsp.c`, declared in
`gdx3ds_audio.h`): clears a sticky LightEvent, raises `sSuspended`, then WAITS (1 ms polls,
<= 50 ms) until the drain / null-sink thread (`ParkDrainWhileSuspended`, top of both loops:
no DSP_FlushDataCache / ndspChnWaveBufAdd / log while parked) and the HLE producer
(`gdx_audio_thread.cpp AudioThreadMain` -> `gdx3ds_audio_producer_park_if_suspended()` once
per 5 ms wake) have both parked, i.e. before libctru's aptDspSleep -> ndspFinalize runs.
Restore side: ONRESTORE/ONWAKEUP -> `gdx3ds_audio_resume()`: `ndspChnWaveBufClear(0)`, every
slot status FREE + realFrames 0, ring readPos/countFrames/inFlightFrames 0, then flag off +
event signal. shutdown()/producer join() signal the event so a parked thread can exit if the
app is closed from the HOME menu (no ONRESTORE). Cause #4: `gdx3ds_menu_backlight_off()` /
`gdx3ds_menu_force_backlight()` (`gdx3ds_menu.c`) -> ONSUSPEND lights the bottom panel,
ONRESTORE/ONWAKEUP re-apply the saved [OFF]. Cause #5: ONRESTORE/ONWAKEUP re-run
osSetSpeedupEnable(true) (New3DS) and APT_SetAppCpuTimeLimit(`gdx3ds_audio_syscore_limit_percent()`,
30 when a core-1 rung was granted at init). `AudioOutLogf` no longer printf()s (drain thread
+ non-thread-safe console); svc + filelog only, the MENU LOG tab shows the lines.

Hardware receipts (sdmc:/3ds/gdiffuser/log.txt, [debug] filelog=1), in order, per HOME press:
`[apt] suspend (HOME/POWER -> HOME menu)` -> `[apt] audio parked (drain=1 hle=1)` (a 0 means
that thread missed the 50 ms window: suspect it) -> optional `[apt] bottom backlight on for
HOME menu ...` -> (resume) `[apt] restore (resumed from HOME menu)` ->
`[apt] cpu state re-applied (speedup=1 syscore=30% rc=0x00000000)` (syscore=0% when no core-1
rung; rc != 0 means NS refused the share) -> `[apt] audio resumed` -> optional
`[apt] bottom backlight off re-applied ...`, then `[audio-out] sub=` keeps increasing and the
fps line keeps ticking. Lid close/open prints the same pair under `[apt] sleep` / `[apt] wakeup`.

## Close-from-HOME hang fix
Symptom (feat/3ds-home @ e23a7f5, .3dsx under hbldr, New3DS): HOME -> resume works; HOME ->
Close (or power menu "Close software") wedges the app with the HOME animation still running.
log.txt ends `[apt] suspend` / `[apt] audio parked (drain=1 hle=1)` /
`[apt] shutdown requested — leaving frame loop`; no `[apt] restore`, no `exiting`.

Path (libctru 2.7.0 source, apt.c/dsp.c/ndsp.c): Close from the HOME menu wakes
`aptWaitForWakeUp(TR_JUMPTOMENU)` with `APTCMD_WAKEUP_CANCEL` + FLAG_ORDERTOCLOSE. That branch
skips `GSPGPU_AcquireRight` and `APTHOOK_ONRESTORE` (hence no restore receipt), runs
`aptDspCancel()` -> ndsp `DSPHOOK_ONCANCEL` (bCancelReceived=1, sync thread idle-polls at
4.9 ms), sets FLAG_CANCELLED and `aptMainLoop()` returns false. main()'s teardown therefore runs
with our audio gate STILL UP (`sSuspended=1`, both audio threads parked), the DSP asleep and no
GPU right.

Cause (ours, not libctru): both audio threads parked on ONE sticky LightEvent (`sResumeEvent`).
Teardown joins the HLE producer first: `gdx3ds_audio_producer_thread_join()` set
`sProducerRelease` and signalled the shared event -> the drain thread woke too, found
`sSuspended && !stopRequested` still true and re-waited. `LightEvent_Wait` on a SIGNALED sticky
event returns immediately with no svc (synchronization.c:321), so the drain became a hot
prio-0x18 spinner on core 2 -- the core the 0x18 producer sits on -- and the producer never got
scheduled again (same-priority threads only rotate at block/yield points). `threadJoin` never
returned; the batched `exiting` line never reached the final flush. Resume never hit this
because `gdx3ds_audio_resume()` clears `sSuspended` before signalling.

libctru teardown verdict (source-checked, NOT the hang): `ndspExit()` joins its sync thread
(idle after ONCANCEL) and skips `ndspFinalize` when `bCancelReceived` -- the only DSP-pipe wait
in that path; `ndspChnWaveBufClear` is CPU-side list surgery under the channel lock; `gfxExit()`
guards the vblank wait + `GSPGPU_SetLcdForceBlack` with `gspHasGpuRight()` (false after the
release) and `gspExit` only joins its event thread + unregisters the relay queue. So the
libctru idiom -- call the normal exits -- is kept; skipping them would leave ndsp's and gsp's
threads/sessions alive through static destructors, which is the less tested state. If a
future log ends at `[exit] audio_shutdown enter` the fallback is to skip
`ndspChnWaveBufClear`+`ndspExit` when `gdx3ds_audio_suspended()`.

Fix (`port/3ds/audio/gdx3ds_audio_ndsp.c`): one sticky park event per thread
(`sDrainParkEvent`: signalled by resume() and shutdown(); `sProducerParkEvent`: by resume() and
producer_thread_join()), each wait condition is only ever changed by that event's signalers,
so a wake always ends the park -- no spurious wake, no re-wait, no spin.
`gdx3ds_audio_producer_park_if_suspended()` now returns 1 on a release-while-suspended and
`gdx_audio_thread.cpp AudioThreadMain` breaks on it before any backend call; the drain's
`ParkDrainWhileSuspended` returns into `while (!s.stopRequested)` and exits without an ndsp
call. New `gdx3ds_audio_suspended()` (header + stub) lets main() record the exit cause.
`main_3ds.cpp` brackets every teardown step with a flushed `aptLogReceipt` (svc + filelog +
flush, no printf).

Receipts a clean HOME-menu Close prints, in order:
`[apt] suspend (HOME/POWER -> HOME menu)` -> `[apt] audio parked (drain=1 hle=1)` ->
(optional `[apt] bottom backlight on for HOME menu ...`) ->
`[apt] shutdown requested — leaving frame loop` -> `[exit] cause=apt close order suspended=1`
-> `[exit] disk_save_flush done` -> `[exit] audio_thread_stop enter` ->
`[exit] audio_thread_stop done` -> `[exit] audio_shutdown enter` -> `[exit] audio_shutdown done`
-> `[exit] window_shutdown enter` -> `[exit] window_shutdown done` -> `[exit] fs_shutdown done`
-> `exiting` (batched, lands with the final flush) ->
`[apt] exit hook (close order acknowledged, aptExit running)`.
A normal in-app quit prints the same `[exit]` chain with `cause=app quit suspended=0`.
A wedge now names its step: the last `[exit]` line on the card is the call that never returned.
