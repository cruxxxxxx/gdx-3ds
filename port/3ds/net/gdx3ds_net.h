/* port/3ds/net/gdx3ds_net.h — hardware dev link (track `hwlink`).
 *
 * Turns the hardware loop from "carry an SD card" into "push over WiFi": a log link that
 * mirrors the filelog sink to the host, and a control listener that accepts PING / file
 * pushes / pulls (GET, confined to sdmc:/3ds/gdiffuser/) / RELOAD (self-relaunch through
 * Luma's hb:ldr) / CAPTURE / REPLAY / INI / QUIT.
 *
 * KILLSWITCH — sdmc:/3ds/gdiffuser/gdiffuser.ini:
 *
 *   [net]
 *   enabled  = 0                          ; MASTER SWITCH. DEFAULT IS CONDITIONAL: ON when
 *                                         ; hbmenu's netloader (3dslink) launched us
 *                                         ; (__3dslink_host nonzero at gdx3ds_net_init time
 *                                         ; -- see "Bring-up" below), OFF for a plain
 *                                         ; hbmenu/SD/CIA launch with no netloader involved.
 *                                         ; An EXPLICIT `enabled = 0` or `= 1` in the ini
 *                                         ; always wins over either default -- the ini is
 *                                         ; never second-guessed, only its ABSENCE is. 0
 *                                         ; (explicit or defaulted) = this whole TU is
 *                                         ; inert: no SOC buffer, no thread, no socket, no
 *                                         ; [net] receipt, and the [watchdog] line keeps its
 *                                         ; pre-hwlink shape (no net= field).
 *   port     = 17492                      ; control listener (TCP, 0.0.0.0)
 *   log_port = 17491                      ; log link (LINK3DS_COMM_PORT — `3dslink -s`
 *                                         ; and tools/dev/hw.py log both speak it)
 *   host     =                            ; explicit host IPv4 for the log link. Empty =
 *                                         ; use __3dslink_host, i.e. the address the
 *                                         ; netloader injected when 3dslink launched us.
 *   stdio    = 0                          ; 1 also runs link3dsStdio() so process
 *                                         ; stdout/stderr go to the host. OFF by default:
 *                                         ; that redirect makes any printf on the GAME
 *                                         ; thread block on a stalled socket. The ring
 *                                         ; below never can.
 *   poll_slice = 1                        ; the net thread waits by poll(.., 0) + sleep, never
 *                                         ; a blocking poll(): SOC is one session per process,
 *                                         ; so a blocking poll here stalls every socket call
 *                                         ; of the main thread (MP's recvfrom) behind it.
 *                                         ; 0 = the old blocking poll (A/B only).
 *   target   = /3ds/G-Diffuser-3DS.3dsx   ; RELOAD write target, sdmc-relative. This is
 *                                         ; also the hb:ldr path (hb:ldr takes the path
 *                                         ; WITHOUT the "sdmc:" prefix). hbmenu's
 *                                         ; netloader writes "sdmc:/3ds/<basename>.3dsx",
 *                                         ; so keep the pushed basename stable.
 *   soc_kb   = 1024                       ; SOC context buffer, KiB (page-aligned,
 *                                         ; multiple of 4 KiB). This is the only large
 *                                         ; allocation the track makes.
 *
 * MEMORY (only when enabled): SOC buffer soc_kb (1 MiB default, handed to the soc module
 * and unmapped from us for its lifetime) + 32 KiB log ring + 64 KiB receive staging +
 * ~2 KiB of line buffers, all STATIC except the SOC buffer, + a 32 KiB thread stack.
 *
 * THREADING: exactly one extra thread, priority 0x3E (bottom of the libctru app range
 * [0x18;0x3F], below the 0x30 main/game thread) on core 0 (appcore — no
 * APT_SetAppCpuTimeLimit grant needed, unlike core 1; core 2 is the render thread's and
 * is the very core the fork/join model measures). It poll()s with a 50 ms timeout.
 *
 * LOCK DISCIPLINE — the load-bearing rule: no game-thread code ever calls send().
 * gdx3ds_net_log_write() is called from inside gdx3ds_filelog_write(), already under that
 * sink's recursive lock and already allocation-free; it memcpy's into a static SPSC ring
 * and returns. The net thread drains it. Ring full => drop + count, reported as
 * "[net] dropped=N total=M" on the next drain — the filelog's own policy.
 *
 * CONTROL COMMANDS are PARSED on the net thread but EXECUTED on the main thread: the net
 * thread posts a request word and blocks on a done flag, the frame loop drains it in
 * gdx3ds_net_tick() (the sHeapTriggerPending pattern in main_3ds.cpp). That keeps
 * gdx3ds_config_set_*, the capture arm, the replay arm and the quit flag single-threaded.
 */
#ifndef GDX3DS_NET_H
#define GDX3DS_NET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring-up. Call AFTER gdx3ds_filelog_init() and BEFORE gdx3ds_os_window_init() so boot
 * logSteps reach the host. argc/argv are main()'s: libctru's arg parser CONSUMES the
 * netloader's "<hex-s_addr>_3DSLINK_" trailer (it decrements argc and NULLs the slot,
 * libctru 2.7.0 __system_initArgv), so the address is read back from __3dslink_host and
 * the trailer is re-synthesised for the RELOAD hand-off; argv is kept only for argv[0].
 * A no-op when [net] enabled = 0. Never fatal: every failure degrades to "no link". */
void gdx3ds_net_init(int argc, char** argv);

/* Main thread, once per frame loop iteration. Drains at most one pending control request.
 * First statement is a load of one file-static, so the disabled/idle cost is a branch. */
void gdx3ds_net_tick(unsigned long frameNum);

/* Log-sink hook. Called from gdx3ds_filelog_write() under its lock; appends msg + '\n' to
 * the SPSC ring. Never blocks, never allocates, safe before init (buffered) and after
 * shutdown (dropped). */
void gdx3ds_net_log_write(const char* msg, size_t len);

/* APT lifecycle. SOC sockets do not survive a suspend cleanly, so the hook drops both
 * connections on ONSUSPEND/ONSLEEP and re-runs the connect path on ONRESTORE/ONWAKEUP.
 * Both are main-thread calls from aptLifecycleHook and are no-ops when disabled. */
/* HOMEFIX2: 1 = the net thread parked inside the 240 ms window, 0 = it did not. */
int gdx3ds_net_suspend(void);
void gdx3ds_net_resume(void);

/* MP quiet (main thread; [net] mp_net_quiet, default 1): while an online / LAN room or race is
 * up the net thread stops relinking and stops polling for a control client: hardware online1's
 * Old 3DS spent the whole race retrying a dead log host, ~95 SOC requests a second on the one
 * soc:U session the race needs. A live log link keeps draining, every NET_QUIET_DRAIN_MS
 * instead of every 50 ms; a connected control client is polled at that rate too. 0 = leave. */
void gdx3ds_net_set_mp_quiet(int quiet);
/* 1 = the net thread makes no socket call at all now (disabled, or quiet with no link and no
 * client, acknowledged by the thread): the mp socket thread may block in poll(). */
int gdx3ds_net_soc_idle(void);

/* Exit chain, AFTER gdx3ds_fs_shutdown(): final ring drain + send, close both sockets,
 * join the net thread, socExit(). Idempotent. */
void gdx3ds_net_shutdown(void);

/* LAST statement of main(), after gdx3ds_net_shutdown() and the final filelog flush.
 * When a RELOAD staged a new image, connects to Luma's "hb:ldr" port and issues
 * HBLDR_SetTarget([net] target) + HBLDR_SetArgv(argv[0] + the 3dslink trailer); Luma then
 * relaunches that target when this process exits. When hb:ldr is absent (non-Luma loader,
 * Azahar) it logs "[net] hbldr unavailable rc=0x..." and returns — the user re-presses A
 * in hbmenu. Never fatal, no-op when nothing was staged. */
void gdx3ds_net_relaunch_if_requested(void);

/* Watchdog field. -1 = disabled (caller omits the field entirely, so a [net] enabled = 0
 * run keeps a byte-identical [watchdog] line), 0 = down, 1 = log link only,
 * 2 = log link + a connected control client. */
int gdx3ds_net_state(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NET_H */
