/* port/3ds/include/gdx3ds_crash.h -- REL1CRASH: self-describing hardware crashes.
 *
 * WHY. The Old 3DS runs Luma3DS 6.6, which leaves no exception dump: an ARM11 fault or a
 * library svcBreak shows ErrDisp ("An error has occurred") and nothing else, and the file
 * log's 250 ms batch loses the last lines before the death. Release preview 1 died that
 * way at boot with no receipt at all. This module makes the next one say where it died.
 *
 * WHAT (all behind [debug] keys, receipts `[crash] ...`):
 *   crashdump = 0|1      ARM11 CPU exception handler on EVERY thread (libctru
 *                        threadOnException via TLS; threads through --wrap=threadCreate).
 *                        Default: 1 on an Old 3DS, 0 on a New 3DS (keeps modern Luma's own
 *                        dump there). On a fault it APPENDS to sdmc:/3ds/gdiffuser/crash<tag>.txt
 *                        with raw FS calls (no stdio, no heap, no locks): exception type,
 *                        FSR decode, FAR, r0-r12/sp/lr/pc/cpsr, code-looking words from the
 *                        faulting stack, and the file log's retained tail -- including the
 *                        lines the SD batch had not flushed yet. Then the same lines go to
 *                        the log, and it chains to libctru's ERRF_ExceptionHandler, i.e. the
 *                        same ErrDisp screen as before.
 *   crashdump_selftest   1 = fault deliberately (NULL store) right after arming, to prove on
 *                        a given console/kernel that the handler is dispatched at all. The
 *                        kernel only dispatches user handlers under some configurations
 *                        (libctru: "normally necessary that UNITINFO is set"); this is the
 *                        30-second test of that.
 *   svcBreak             (always; --wrap=svcBreak) every libctru/citro3d panic -- GPUCMD_Add
 *                        overflow, getreent, gxCmdQueue, ndsp -- logs `[fatal] svcBreak
 *                        reason=N ra=...` flushed before the break. Not a lever: it only
 *                        runs on the way to a break.
 *
 * Thread safety: one fault is reported; a second concurrent one parks its thread. The handler
 * runs on a per-thread static stack slot (GDX3DS_CRASH_SLOTS) so a stack overflow is
 * reportable; threads beyond the pool run it on their own stack.
 */
#ifndef GDX3DS_CRASH_H
#define GDX3DS_CRASH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Latch [debug] crashdump* and arm the calling (main) thread. Call after
 * gdx3ds_config_load and gdx3ds_filelog_init; prints the `[crash] armed` receipt. */
void gdx3ds_crash_init(void);

/* n60boot: the boot window. arm_boot() arms the CALLING (main) thread whatever [debug]
 * crashdump says and hands every report to `sink` first (the boot trail: one flushed raw-FS
 * write), then crash<tag>.txt as usual, then ErrDisp. disarm_boot() stops the sink and, unless
 * crashdump=1 armed the thread for the run, removes the handler. Main thread only. */
typedef void (*GdxCrashBootSink)(const char* text, size_t len);
void gdx3ds_crash_arm_boot(GdxCrashBootSink sink);
void gdx3ds_crash_disarm_boot(void);
int gdx3ds_crash_boot_armed(void);

/* CRASHFIX: main() is tearing down (the frame loop has ended). A fault from here on still writes
 * crash<tag>.txt, then ends the process quietly instead of raising ErrDisp ("the system will
 * reboot") over a close from HOME. */
void gdx3ds_crash_note_exit(void);

/* Arm the calling thread (used by the threadCreate wrapper; harmless when disabled). */
void gdx3ds_crash_arm_current_thread(void);

/* [debug] boot_breadcrumbs = N (default 0): turn the GDX_DIAG_VERBOSE dev gate on -- the
 * decomp's boot/frame checkpoints ("[game] ckG8_assets_done" ...) -- until host frame N, so a
 * boot death is bracketed by checkpoints. Pair with filelog_sync_ms. arm() after
 * gdx_dev_gates_init_env (which would otherwise overwrite the gate); frame() every iteration. */
void gdx3ds_crash_breadcrumbs_arm(void);
void gdx3ds_crash_breadcrumbs_frame(unsigned long frame);

/* N60CORE1: a one-shot core redirect for the NEXT threadCreate, made through the same
 * --wrap=threadCreate. libctru's ndspInit creates its service thread with core -2 (the
 * process default = core 0) and offers no parameter; [debug] ndsp_core1 brackets ndspInit
 * with begin/end to place that thread on core 1 instead. The first threadCreate inside the
 * window is created on `core`; if the kernel refuses it there, it is created on the core
 * the caller asked for (fallback), so a redirect can never cost the caller its thread.
 * end() closes the window and returns the core the thread was created on (-1 = no thread
 * was created in the window); *fellBack = 1 when the fallback ran. ran_on() = the core the
 * redirected thread reported from its first instruction (svcGetProcessorID), -1 until then,
 * -2 when the svc returned no core id (the emulator does; hardware gives 0..3). */
void gdx3ds_thread_redirect_begin(int core);
int gdx3ds_thread_redirect_end(int* fellBack);
int gdx3ds_thread_redirect_ran_on(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_CRASH_H */
