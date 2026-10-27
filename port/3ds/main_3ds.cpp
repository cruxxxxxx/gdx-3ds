// port/3ds/main_3ds.cpp — 3DS entry point. Selected by CMake when GDX_PLATFORM_3DS=ON;
// port/main.cpp is desktop-only and is never ifdef'd for 3DS.
//
// Responsibility map vs port/main.cpp (each item is a contract call, a 3DS
// equivalent, or explicitly dropped):
//   window/renderer init      -> gdx3ds_os_window_init + Fast::Fast3dWindow (lus_glue
//                                replacement TU wiring Gdx3ds_GetCitro3dRenderer)
//   audio device + CVar size  -> gdx3ds_audio_init(0) (4096-frame default ring)
//   controller hotplug (SDL)  -> dropped; gdx3ds_os_poll_input via lus_glue's
//                                gdx_lus_read_pads, pad 0 only
//   CVar/config load (ImGui)  -> stream B INI (gdx3ds_config_load) + in-memory CVars
//   LUS Context bring-up      -> lus_glue/gdx3ds_context_stub.cpp (carved subset)
//   asset archive mount       -> ResourceManager over sdmc:/3ds/gdiffuser/*.o2r
//                                (pre-baked by tools/prebake on PC)
//   first-boot Torch extract  -> dropped on device (tools/prebake)
//   crash handler             -> dropped (Luma exception screen serves on-device)
//   Discord RPC / ImGui menus / ghost browser / FPS overlay -> dropped
//   archive CRC gate          -> M1: mount-or-log only; the desktop version-CRC
//                                enforcement moves here once boot is proven
//
// Boot order follows port/main.cpp's proven sequence with the desktop-only steps
// removed; see the numbered comments.
//
// The host GDX_PLATFORM_3DS build (no devkitARM toolchain) compiles only the Phase 0
// contract stubs — no LUS carve, no game objects — so it keeps the original stub loop
// below (#else branch). __3DS__ comes from the devkitPro toolchain file.

#ifdef __3DS__

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "fast/Fast3dWindow.h"

#include "gfx/gdx3ds_native_cfg.h"
#include "gfx/gdx3ds_replay.h" // C6 [replay]: pose-replay "60 Hz motion" ([native] replay)
#include "gfx/gdx3ds_track_resident.h" // ship6: gdx3ds_track_resident_reserve (the arena at boot)
#include "gdx3ds_audio.h"
#include "gdx3ds_config.h"
#include "gdx3ds_preset_n60.h"
#include "gdx3ds_filelog.h" // H-HARDWARE: SD sink for the tracers (svc goes nowhere on hardware)
#include "gdx3ds_fps_hud.h" // FPS-HUD: bottom-screen counter (debug.fps, default ON)
#include "gdx3ds_mainprof.h" // MAINPROF: [mprof] main-thread section profiler ([debug] mainprof)
#include "gdx3ds_menu.h"    // MENU: v1 bottom-screen touch menu ([menu] enabled, default ON)
#include "gdx3ds_crash.h"     // REL1CRASH: [crash] ARM11 exception handler + svcBreak tracer
#include "gdx3ds_membudget.h" // O3DS BUDGET: [mem-budget] + [mem-res] census ([debug] membudget)
#include "gdx_audio_diet.h"   // C4d: audio_table diet + census ([debug] audio_stream / audiodiet)
#include "gdx_ndsp_hash.h"   // [debug] ndsphash: the content-addressed render digest
#include "gdx_audio_mix_census.h" // [debug] audiomix: the mix-bus headroom census
#include "gdx3ds_cov.h"     // COVERAGE: [cov] per-frame feature census ([debug] cov)
#include "gdx3ds_hitch.h"   // HITCH: per-frame hitch receipt ([debug] hitch)
#include "gdx3ds_cpucut.h"  // o3ds-cpucuts: the CPU cuts' A/B block clock
#include "gdx3ds_spike.h"   // n60st3: frame-spike attribution ([debug] spike_ms)
#include "gdx3ds_oracle.h"  // ORACLE: true frame capture + [fh] receipts (after EndFrame)
#include "gdx3ds_dynlod.h"  // ORACLE: dynlod tier pin under .gdxrec replay
#include "gdx3ds_frameskip.h" // FRAMESKIP (C4l): 60 Hz logic, skipped renders ([debug] frameskip)
#include "gdx3ds_pipe.h"      // o3ds60: top triple buffer + dash_pre_frame levers and their A/B
#include "gdx3ds_core1probe.h" // N60TICK: core-1 probe ([debug] core1_probe)
#include "gdx3ds_tickahead.h"  // N60TICK4: [debug] tick_ahead (Old 3DS: host on core 1, render on core 0)
#include "gdx3ds_launch.h"     // N60LAUNCH: launch variants ([debug] launch_variants)
#include "gdx3ds_boot_trail.h" // n60boot: crash-proof boot trail (boot.txt, DEFAULT ON)
#include "gdx3ds_c5tick.h" // C5TICK: dlb split, write-set check, hollow skipped ticks ([debug] dl_hollow)
#include "gdx_input_rec.h"  // ORACLE: .gdxrec record/replay ([debug] input_record / input_replay)
#include "gdx_determinism.h" // ORACLE: Math_Rand seeds / boot-hold tick pin ([debug] rand_seed1 ...)
#include "gdx_snapshot.h"   // mp-snapshot: the tick-boundary hook (snapshot / restore / synctest)
#include "gdx3ds_bottomhud.h" // gdx3ds_bottomhud_bake_assets: the DASH art from fzerox.o2r
#include "gdx3ds_fs.h"
#include "gdx3ds_os.h"
#include "gdx3ds_net.h"          // HWLINK: WiFi dev link ([net] enabled, DEFAULT OFF)
#include "gdx3ds_mp.h"           // mp-spike: [net] mp lockstep multiplayer (DEFAULT OFF)
#include "gdx3ds_mp_online.h"    // mp-online: NEARBY + online multiplayer (default on; nothing opens at boot)
#include "gdx3ds_renderthread.h" // RENDER THREAD (LOCKED-60 Task H): core-2 fork/join

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <cxxabi.h>
#include <malloc.h>
#include <new>
#include <typeinfo>

extern "C" {
#include "gdx_audio_thread.h"
#include "gdx_dev_gates.h"    // gdx_port_log_tap (svc mirror below)
#include "gdx_frame_pacer.h"
void GDiffuser_LoadAllAssets(void);
void bootproc(void);                 // decomp boot entry (src/sys/sys_main.c)
void gdx_dev_gates_init_env(void);
void gdx_dev_gates_refresh(void);
void gdx_sched_init(void);
void gdx_sched_drain_deferred_wakes(void);
void gdx_init_rom(int argc, char** argv, int archivesValidated);
void gdx_vi_tick(void);
void gdx_dispatch(void);
void gdx_vi_present_fallback(void);
void gdx_controller_poll(void);
unsigned long long gdx_input_tick(void); // ORACLE: input tick (input_bridge.c)
int gdx_input_game_mode(void);           // ORACLE: GET_MODE(gGameMode) (input_bridge.c)
unsigned int gdx_gstate_hash(int* outRacers); // GSTATE: simulated-state digest (decomp_port.c)
unsigned int gdx3ds_gstate_ext_hash(unsigned int* audioCmds); // C5TICK: game/gdx3ds_c5tick_game.c
void gdx_fixed_aspect_tick(void);
void gdx_rdram_init(void);
void gdx_register_host_range(void* ptr, size_t size);
void gdx_register_main_module_range(void);
void gdx_disk_save_tick(void);
void gdx_disk_save_flush(void);
void gdx_sram_flush_pending(unsigned int timeoutMs, const char* why); // N60SAVE: [debug] sram_async
int  GdxSegmentSourcePreload(uint32_t romBase);
int  GdxSegmentSourcePayload(uint32_t romBase, void** outPayload, uint32_t* outSize);
int  GdxSegmentSourcePreallocPayload(uint32_t romBase, void** outPayload, uint32_t* outCap);
double gdx_host_ms_now(void);
void gdx_boot_warm_asset_segments(void);
// A10 LOAD-STALL census (port/gdx_stall.c) — `[debug] stall = 1`. Every entry point is a
// no-op when the key is absent, so the shipped configuration is unchanged.
#include "gdx_stall.h"
// [audioprime2] (port/3ds/audio/gdx3ds_audio_ndsp.c diagnostic exports): preload ladder
// off core 2 + the boot-phase / preload thread map the under-edge receipt prints.
int  gdx3ds_audio_prime2_enabled(void);
int  gdx3ds_audio_prime2_preload_core(void);
int  gdx3ds_audio_grant_syscore(void);
int  gdx3ds_audio_producer_core(void); // O3DSHOME: core-1 receipt at suspend
int  gdx3ds_audio_drain_core(void);    // O3DSHOME: core-1 receipt at suspend
void gdx3ds_audio_set_main_phase(const char* tag);
void gdx3ds_audio_note_preload_core(int core);
void gdx3ds_audio_note_preload_running(int running);
void gdx_gfx_mem_census(unsigned long out[8]); // M1-MEMORY census (n64_gfx_bridge.cpp)
int  gdx3ds_quit_requested(void);    // lus_glue/gdx3ds_input_glue.c (script QUIT hook)

// AUDIOFIX: ndsp-vs-null-sink outcome for the always-on boot receipt below (diagnostic
// export from port/3ds/audio/gdx3ds_audio_ndsp.c, not part of the frozen contract).
int gdx3ds_audio_output_active(void);
// HOMEFIX: is a C3D frame open on the citro3d backend (render-thread state, racy read;
// receipts only). port/3ds/gfx/gfx_citro3d.cpp.
int gdx3ds_gfx_frame_active(void);

// M1-RACE-FREEZE watchdog counters (port/n64_sched.c, port/n64_audio_hle.c).
extern volatile uint32_t gdx_watch_audio_tick_enter;
extern volatile uint32_t gdx_watch_audio_tick_exit;
extern volatile int      gdx_watch_audio_stage;
extern volatile uint32_t gdx_watch_specwait;
extern volatile uint32_t gdx_watch_hle_runs;
extern volatile uint32_t gdx_watch_hle_op;
extern volatile uint32_t gdx_watch_hle_idx;
// B11 EK-DEADLOCK PROBE (port/n64_sched.c). Only the EK build writes these, so in the shipped
// build they stay 0 and the extra fields are omitted entirely -- the pre-probe [watchdog] line
// is byte-identical there.
extern volatile uint32_t gdx_watch_mq_owner;
extern volatile uint32_t gdx_watch_mq_waiters;
extern volatile uint32_t gdx_watch_ctx_owner;
extern volatile uint32_t gdx_watch_ctx_waiters;
extern volatile uint32_t gdx_watch_foreign_q;
extern volatile uint32_t gdx_watch_foreign_ra;
extern volatile uint32_t gdx_watch_foreign_n;
int gdx_watch_running_thread_id(void);
// [tt] TIMETRIAL-FREEZE: per-fiber park-state dump (port/n64_sched.c). Names the exact
// message queue each game thread is blocked on when the watchdog fires.
int gdx_tt_dump_threads(char* buf, int cap);

// CADENCE counters. Task/hold from the bridge (port/n64_gfx_bridge.cpp), game
// ticks from the decomp's own frame counter (sys_gfx.c Game_ThreadEntry loop),
// yields from the fiber scheduler (port/n64_sched.c). All single-writer on this
// same host thread except gGameFrameCount (game fiber — also this OS thread).
extern volatile unsigned long gdx_cadence_gfx_tasks;
extern volatile unsigned long gdx_cadence_task_frames;
extern volatile unsigned long gdx_cadence_hold_frames;
extern unsigned long gdx_yield_count;
extern uint32_t gGameFrameCount;
}

#include "resource/ResourceFactories.h"
#include "rom_buffer.h"

#include <3ds.h>

// ---------------------------------------------------------------------------------------------
// M1-RACE-FREEZE watchdog (docs/research/m1-boot-debug.md): the race-start freeze goes totally
// silent — no svc traffic at all — so nothing in the ordinary logs says WHICH thread stopped.
// This thread runs at priority 0x18 (above the 0x30 main/audio threads), so its 5 s timer wake
// PREEMPTS even a tight guest spin, and its heartbeat reports every progress counter the port
// exports. Reading the frozen line:
//   frame stuck + fiber id  -> the main thread is wedged; `stage` names the frame-loop phase
//                              (1 events, 2 vi_tick, 3 audio_notify, 4 start_frame, 5 dispatch,
//                               6 save_tick, 7 end_frame, 8 present/pacer, 9 render-thread join).
//   aud enter==exit, flat   -> audio thread idle (starved or not being woken).
//   aud enter==exit+1, flat -> audio thread wedged INSIDE a tick; astage 1 = game audio pump /
//                              CreateTaskImpl, 2 = HLE run (op/idx then name the ABI command).
//   spec>0 and climbing     -> a game fiber is in Audio_SetSpec's reset wait (patched to yield).
// ---------------------------------------------------------------------------------------------
static volatile uint32_t sWatchFrame = 0;
static volatile int sWatchStage = 0;
// APT: set right before teardown so a 5 s beat landing mid-teardown (or after main
// returns, while crt0/__appExit unwinds — the thread is detached and never joined)
// cannot write through stdio/SD state that is being torn down under it.
static volatile int sWatchdogQuiet = 0;
// HOMEFIX: the APT hook signals this so the watchdog leaves its 5 s sleep at once and enters
// the suspended-mode poll (aptWatchTick below) while main is inside libctru's transition.
static Handle sWatchdogWake = 0;

// HEAP-WATCH: leak triage without a debugger. The watchdog beat already carries heapUsed;
// this keeps a baseline (taken at beat 12, ~60 s, after the boot loads settle) and a
// high-water mark, and when heapUsed crosses baseline + [debug] heap_watch MB (default 8,
// 0 = off) -- and every further heap_watch_step MB (default 4) -- asks the main thread for
// one [mem-census] owner attribution + [live-hist] dump, so the log says WHICH container
// grew, not just that the heap did. [debug] heap_watch_arm=1 additionally arms the live
// allocation histogram / RA sampler at the FIRST crossing, so normal play pays nothing but
// a suspicious session starts sampling itself. Everything lands in sdmc log.txt.
static volatile unsigned long sHeapBaseline = 0;   // bytes, 0 = not yet taken
static volatile unsigned long sHeapHwm = 0;
static volatile unsigned long sHeapNextTrigger = 0; // bytes
static volatile int sHeapTriggerPending = 0;        // set by watchdog, consumed on main
static volatile int sHeapTriggers = 0;
static volatile uint64_t sHeapBaselineTick = 0;
static int sHeapWatchMb = 8;
static int sHeapWatchStepMb = 4;
static int sHeapWatchArm = 0;
extern "C" void gdx3ds_heapwatch_stats(unsigned long* usedKb, unsigned long* hwmKb, long* perMinKb,
                                       int* triggers) {
    struct mallinfo mi = mallinfo();
    unsigned long used = (unsigned long)mi.uordblks;
    if (usedKb) { *usedKb = used / 1024u; }
    if (hwmKb) { *hwmKb = (sHeapHwm > used ? sHeapHwm : used) / 1024u; }
    if (perMinKb) {
        *perMinKb = 0;
        if (sHeapBaseline != 0 && sHeapBaselineTick != 0) {
            uint64_t ms = (svcGetSystemTick() - sHeapBaselineTick) / (uint64_t)CPU_TICKS_PER_MSEC;
            if (ms > 30000) {
                *perMinKb = (long)(((long long)used - (long long)sHeapBaseline) * 60000 / (long long)ms / 1024);
            }
        }
    }
    if (triggers) { *triggers = sHeapTriggers; }
}

// HOMEFIX (native/homefix): APT transition state, owned by aptLifecycleHook (main), read by
// the watchdog. 0 running, 1 HOME/POWER-suspended, 2 lid-sleeping. The tick is the state
// change; every [apt] receipt carries t=+ms since it.
static volatile int sAptState = 0;
static volatile uint64_t sAptStateTick = 0;
static int sAptWatch = 1;     // [debug] apt_watch (default 1): suspended-mode watchdog poll
static int sAptHookFirst = 1; // [debug] apt_hook_first (default 1): re-arm our hook ahead of citro3d's
// HOMEFIX2 (docs/research/native/homefix2-progress.md): deadlines for the waits that run
// INSIDE an APT transition. An APT hook that never returns is not a hang the user sees as a
// hang: NS force-terminates the title ~10 s after the jump was prepared, with no exception
// and no Luma dump — the log simply stops, which is exactly what hw run E left behind.
static int sAptJoinMs = 1200;  // [debug] apt_join_ms (0 = unbounded, pre-HOMEFIX2 behaviour)
static int sAptDspHook = 1;    // [debug] apt_dsp_hook (default 1): receipt around ndspFinalize
static int sAptDspPark = 0;    // [debug] apt_dsp_park (default 0): also park audio in that hook
// O3DSHOME (docs/research/native/o3dshome-progress.md, NEXT.md C4j): on the Old 3DS the audio
// and preload threads live on core 1 under an APT_SetAppCpuTimeLimit grant -- 60% by default as
// of N60SYSCORE60 (audiofix-progress.md, card n60-3d13; was 30% before that hardware A/B), which
// turns on the kernel's core-1 preemption for every sysmodule thread (GSP, DSP, FS, NS all run
// there). No New 3DS run ever held that grant (`syscore=0%` in every N3DS restore receipt), and
// it is the one HOME-path difference between the models that is ours. [debug] apt_syscore_drop
// (default 1) lowers the share to the 5% floor for the time we are suspended/asleep; restore
// step 1 (aptRestoreCpuState) re-applies whatever percent was actually granted at init
// (gdx3ds_audio_syscore_limit_percent(), not a hardcoded literal), so this already tracks the
// new 60% default with no further change here. Model-gated: it acts only on an Old 3DS that
// holds a grant, so a New 3DS never executes it.
static int sAptSyscoreDrop = 1;
// O3DSHOME3: [debug] apt_watch_file (default 1). 0 = while an APT transition is in progress,
// the watchdog's receipts go to svc only once the "apt inactive" line has been written, so our
// process issues no SD writes while the HOME menu loads (the boot-bisect G3 split).
static int sAptWatchFile = 1;
static volatile int sAptWatchFileMuted = 0;
static const u32 kAptSyscoreFloorPct = 5; // APT:SetApplicationCpuTimeLimit accepts 5..89

static unsigned long aptMsSince(uint64_t tick) {
    return (unsigned long)((svcGetSystemTick() - tick) / (uint64_t)CPU_TICKS_PER_MSEC);
}

// Suspended-mode poll: runs on the WATCHDOG thread every 100 ms while sAptState != 0. Main
// is inside libctru (aptJumpToHomeMenu / aptWaitForWakeUp / aptHandleSleep) and cannot log,
// so this is the only receipt writer left. It logs every flip of aptIsActive() (clears when
// APT_JumpToHomeMenu ran, i.e. the jump completed and the HOME menu owns the screen; sets
// again on the wake, BEFORE GSPGPU_AcquireRight / RestoreVramSysArea and the restore hook),
// gspHasGpuRight() (ReleaseRight ran / AcquireRight ran) and aptShouldClose() (a close
// order arrived while parked: the [exit] chain is expected next), plus a 1 s heartbeat.
// Every line is flushed. Reading a log that ends here:
//   ends before "apt inactive"                -> died inside the suspend transition
//                                                 (SaveVramSysArea / screen transfer / DSP sleep)
//   ends on "parked" beats, no "WAKE observed" -> the process died or was killed while parked
//                                                 in the HOME menu (nothing of ours runs there)
//   ends after "WAKE observed", no "[apt] restore" -> GSPGPU_AcquireRight / RestoreVramSysArea
//                                                 or citro3d's ONRESTORE hook (runs before ours)
//   "CLOSE order pending" then nothing          -> the close path: see the [exit] chain
// O3DSHOME: the largest gap between two suspended-mode polls since the last printed watch
// line. The poll asks for 100 ms at priority 0x18 on core 0; a gap far above that says our own
// core-0 threads were starved (by the HOME menu or a system wedge), not that main was.
static void aptWatchTrackGap(uint64_t now, uint64_t* lastTick, unsigned long* maxGapMs) {
    if (*lastTick != 0) {
        const unsigned long gap = (unsigned long)((now - *lastTick) / (uint64_t)CPU_TICKS_PER_MSEC);
        if (gap > *maxGapMs) {
            *maxGapMs = gap;
        }
    }
    *lastTick = now;
}

// O3DSHOME: the core-1 half of the watch line. c1=<drain parked>/<producer parked> (-1 = no
// such thread), pre=<preload worker running>, pct=<syscore share we hold>, lag=<max poll gap>,
// sysFreeK=<free KiB in the SYSTEM region, where the HOME menu allocates>. svc + static reads
// only: no heap, no IPC, legal from the watchdog while main sits inside libctru.
static void aptWatchAppendCore1(char* out, size_t cap, unsigned long maxGapMs) {
    int drainParked = -1;
    int producerParked = -1;
    int preloadRunning = 0;
    gdx3ds_audio_park_state(&drainParked, &producerParked, &preloadRunning);
    std::snprintf(out, cap, " c1=%d/%d pre=%d pct=%d lag=%lums sysFreeK=%lu", drainParked, producerParked,
                  preloadRunning, gdx3ds_audio_syscore_limit_percent(), maxGapMs,
                  (unsigned long)(osGetMemRegionFree(MEMREGION_SYSTEM) / 1024u));
}

// O3DSHOME3: one watch receipt out. With [debug] apt_watch_file=0 the first line that sees
// the app inactive (the jump went through) is the last one written to the SD; later lines
// reach svc only, so the HOME menu loads with no SD traffic from us.
static void aptWatchEmit(const char* msg, int active) {
    svcOutputDebugString(msg, strlen(msg));
    if (sAptWatchFileMuted) {
        return;
    }
    if (!sAptWatchFile && !active) {
        sAptWatchFileMuted = 1;
    }
    gdx3ds_filelog_write(msg, strlen(msg));
    gdx3ds_filelog_flush_sync();
}

static void aptWatchTick(void) {
    static int lastState = 0;
    static int lastActive = -1;
    static int lastGpu = -1;
    static int lastClose = -1;
    static uint64_t lastBeatTick = 0;
    static uint64_t lastTickTick = 0;
    static unsigned long maxGapMs = 0;
    const int state = sAptState;
    const int active = aptIsActive() ? 1 : 0;
    const int gpu = gspHasGpuRight() ? 1 : 0;
    const int close = aptShouldClose() ? 1 : 0;
    char msg[320];
    if (state == 0) {
        sAptWatchFileMuted = 0; // transition over: the SD sink is ours again
        if (lastState != 0) {
            std::snprintf(msg, sizeof(msg),
                          "[apt] watch: transition complete t=+%lums active=%d gpu=%d close=%d",
                          aptMsSince(sAptStateTick), active, gpu, close);
            svcOutputDebugString(msg, strlen(msg));
            gdx3ds_filelog_write(msg, strlen(msg));
            gdx3ds_filelog_flush_sync();
        }
        lastState = 0;
        lastActive = lastGpu = lastClose = -1;
        lastTickTick = 0;
        maxGapMs = 0;
        return;
    }
    const uint64_t now = svcGetSystemTick();
    aptWatchTrackGap(now, &lastTickTick, &maxGapMs);
    const char* why = nullptr;
    const char* alsoGpu = "";
    if (lastState == 0) {
        why = "entered";
    } else if (active != lastActive) {
        why = active ? "WAKE observed (main past the APT wait; AcquireRight/RestoreVramSysArea/restore hook next)"
                     : "apt inactive (jump handed to the HOME menu)";
        if (lastGpu >= 0 && gpu != lastGpu) {
            alsoGpu = gpu ? " +gpu right acquired" : " +gpu right released";
        }
    } else if (gpu != lastGpu) {
        why = gpu ? "gpu right acquired" : "gpu right released";
    } else if (close != lastClose) {
        why = close ? "CLOSE order pending ([exit] chain expected)" : "close order cleared";
    } else if (now - lastBeatTick >= (uint64_t)CPU_TICKS_PER_MSEC * 1000u) {
        why = "parked";
    }
    if (why != nullptr) {
        lastBeatTick = now;
        int n = std::snprintf(msg, sizeof(msg),
                              "[apt] watch %s%s state=%d t=+%lums active=%d gpu=%d close=%d rtIdle=%d frameOpen=%d",
                              why, alsoGpu, state, aptMsSince(sAptStateTick), active, gpu, close,
                              gdx3ds_rt_idle(), gdx3ds_gfx_frame_active());
        if (n > 0 && (size_t)n < sizeof(msg)) {
            aptWatchAppendCore1(msg + n, sizeof(msg) - (size_t)n, maxGapMs);
        }
        maxGapMs = 0;
        aptWatchEmit(msg, active);
    }
    lastState = state;
    lastActive = active;
    lastGpu = gpu;
    lastClose = close;
}

static void watchdogThreadMain(void*) {
    uint32_t lastFrame = 0;
    uint32_t beat = 0;
    const uint64_t beatTicks = (uint64_t)CPU_TICKS_PER_MSEC * 5000u; // 5 s
    uint64_t nextBeat = svcGetSystemTick() + beatTicks;
    for (;;) {
        // HOMEFIX: 5 s sleep as before, but interruptible by the APT hook (sWatchdogWake) and
        // shortened to 100 ms while an APT transition is in progress (suspended-mode poll).
        const bool fine = sAptWatch && sAptState != 0;
        if (sWatchdogWake != 0) {
            svcWaitSynchronization(sWatchdogWake, fine ? 100000000LL : 5000000000LL);
        } else {
            svcSleepThread(fine ? 100000000LL : 5000000000LL);
        }
        if (sWatchdogQuiet) {
            continue;
        }
        if (sAptWatch) {
            aptWatchTick();
        }
        {
            const uint64_t now = svcGetSystemTick();
            if ((int64_t)(now - nextBeat) < 0) {
                continue; // woken early (hook signal / fine poll): not a beat yet
            }
            nextBeat = now + beatTicks;
        }
        const uint32_t frame = sWatchFrame;
        // HAZARD CRUMBS (native/n3dsfix, docs/research/native/hw-n60-3d7-analysis.md): the
        // New3DS attract-demo freeze near frame ~6900-7100 silences even this beat, which can
        // only mean the watchdog thread itself never reaches its own log call -- and the one
        // thing between the beat gate above and that log call is mallinfo(), which takes
        // newlib's shared heap-allocator lock. If some other thread is stuck WHILE HOLDING that
        // lock (e.g. mid malloc/free during the course-4 [trkres] bake/adopt burst that lines up
        // with this freeze window every time), mallinfo() blocks here forever and the beat line
        // never prints -- indistinguishable, from the SD/[net] log alone, from the watchdog
        // thread itself being gone. These two durable (synchronously flushed, so they survive
        // a freeze one line later) crumbs bracket the one call that can do that: if "pre" shows
        // up on the next hardware freeze but "post" never does, mallinfo()/the heap lock is the
        // smoking gun; if neither shows up, the watchdog thread was already stuck earlier than
        // this (rule out the heap lock, look at aptWatchTick() or the beat gate instead).
        // [debug] hazard_crumbs (default 0): zero cost unless armed for a hardware capture.
        static int sHazardCrumbsOn = -1;
        if (sHazardCrumbsOn < 0) {
            sHazardCrumbsOn = gdx3ds_config_get_int("debug", "hazard_crumbs", 0) != 0 ? 1 : 0;
        }
        if (sHazardCrumbsOn) {
            char hz[96];
            int hn = std::snprintf(hz, sizeof(hz), "[hzc] watchdog pre-mallinfo beat=%lu frame=%lu\n",
                                   (unsigned long)(beat + 1), (unsigned long)frame);
            if (hn > 0) {
                gdx3ds_filelog_write(hz, (size_t)hn);
                gdx3ds_filelog_flush_sync();
            }
        }
        struct mallinfo mi = mallinfo();
        if (sHazardCrumbsOn) {
            char hz[64];
            int hn = std::snprintf(hz, sizeof(hz), "[hzc] watchdog post-mallinfo beat=%lu\n",
                                   (unsigned long)(beat + 1));
            if (hn > 0) {
                gdx3ds_filelog_write(hz, (size_t)hn);
                gdx3ds_filelog_flush_sync();
            }
        }
        const unsigned long used = (unsigned long)mi.uordblks;
        if (used > sHeapHwm) {
            sHeapHwm = used;
        }
        if (sHeapBaseline == 0 && beat + 1 >= 12) {
            sHeapBaseline = used;
            sHeapBaselineTick = svcGetSystemTick();
            sHeapNextTrigger = sHeapWatchMb > 0 ? used + (unsigned long)sHeapWatchMb * 1024u * 1024u : 0;
        }
        if (sHeapNextTrigger != 0 && used >= sHeapNextTrigger) {
            sHeapNextTrigger = used + (unsigned long)(sHeapWatchStepMb > 0 ? sHeapWatchStepMb : 1) * 1024u * 1024u;
            sHeapTriggerPending = 1;
        }
        char msg[384]; // 256 + the B11 probe fields below
        int n = std::snprintf(msg, sizeof(msg),
                              "[watchdog] beat=%lu frame=%lu(+%lu) stage=%d fiber=%d "
                              "aud=%lu/%lu astage=%d spec=%lu hle=%lu op=%02lX idx=%lu "
                              "heapUsed=%lu heapFree=%lu hwm=%lu base=%lu",
                              (unsigned long)++beat, (unsigned long)frame,
                              (unsigned long)(frame - lastFrame), sWatchStage,
                              gdx_watch_running_thread_id(),
                              (unsigned long)gdx_watch_audio_tick_enter,
                              (unsigned long)gdx_watch_audio_tick_exit,
                              gdx_watch_audio_stage,
                              (unsigned long)gdx_watch_specwait,
                              (unsigned long)gdx_watch_hle_runs,
                              (unsigned long)gdx_watch_hle_op,
                              (unsigned long)gdx_watch_hle_idx,
                              (unsigned long)mi.uordblks,
                              (unsigned long)mi.fordblks,
                              (unsigned long)sHeapHwm, (unsigned long)sHeapBaseline);
        // B11 EK-DEADLOCK PROBE: the two host-lock owners and their waiter counts (owner is the
        // fiber id + 1, or 0x1000 + the OS thread id for a non-fiber thread). Written only in the
        // EK build, so the shipped build's line is unchanged.
        // A non-host thread busy-waiting on a libultra queue nothing will post to: name the
        // queue, its caller and the spin count. Zero in a healthy build, on any configuration.
        if (n > 0 && gdx_watch_foreign_n != 0) {
            n += std::snprintf(msg + n, sizeof(msg) - (size_t)n, " fq=%08lx fra=%08lx fn=%lu",
                               (unsigned long)gdx_watch_foreign_q,
                               (unsigned long)gdx_watch_foreign_ra,
                               (unsigned long)gdx_watch_foreign_n);
        }
        if (n > 0 && (gdx_watch_mq_owner | gdx_watch_mq_waiters | gdx_watch_ctx_owner |
                      gdx_watch_ctx_waiters) != 0) {
            n += std::snprintf(msg + n, sizeof(msg) - (size_t)n, " mq=%lu+%lu ctx=%lu+%lu",
                               (unsigned long)gdx_watch_mq_owner, (unsigned long)gdx_watch_mq_waiters,
                               (unsigned long)gdx_watch_ctx_owner, (unsigned long)gdx_watch_ctx_waiters);
        }
        // HWLINK: one extra field so a silent host is distinguishable from a wedged game
        // (0 down / 1 log link / 2 log + control client). Omitted entirely when [net]
        // enabled = 0, which keeps the pre-hwlink [watchdog] line byte-identical.
        if (n > 0) {
            const int netState = gdx3ds_net_state();
            if (netState >= 0) {
                n += std::snprintf(msg + n, sizeof(msg) - (size_t)n, " net=%d", netState);
            }
        }
        // HOMEFIX: apt=<state> only while a transition is in progress (0 = line unchanged).
        if (n > 0 && sAptState != 0) {
            n += std::snprintf(msg + n, sizeof(msg) - (size_t)n, " apt=%d active=%d gpu=%d", sAptState,
                               aptIsActive() ? 1 : 0, gspHasGpuRight() ? 1 : 0);
        }
        if (n > 0) {
            svcOutputDebugString(msg, n);
            if (!sAptWatchFileMuted) {
                gdx3ds_filelog_write(msg, (size_t)n); // sink is thread-safe (recursive lock)
            }
        }
        // [tt] park-state companion line: which queue each game fiber is blocked on. Racy
        // cross-thread reads of scheduler state (diagnostic only, same contract as the
        // counters above).
        {
            char ttmsg[224];
            int tn = std::snprintf(ttmsg, sizeof(ttmsg), "[tt] ");
            tn += gdx_tt_dump_threads(ttmsg + tn, (int)sizeof(ttmsg) - tn);
            if (tn > 0) {
                svcOutputDebugString(ttmsg, tn);
                if (!sAptWatchFileMuted) {
                    gdx3ds_filelog_write(ttmsg, (size_t)tn);
                }
            }
        }
        lastFrame = frame;
    }
}

static void logStep(const char* s);

// n60hwdiag (port/3ds/gfx/nrr/gdx3ds_nrr_crumb.h): durable breadcrumbs + the boot-cycling NRR bisect.
extern "C" void gdx3ds_nrr_crumb_boot_select(void);
extern "C" void gdx3ds_nrr_crumb_boot_receipt(void);

// nihudtop Phase 3: dev-only CUSTOM top-screen mask lever ([debug] hud_custom_top_mask/
// hud_custom_active). port/3ds/gdx3ds_hudtop_custom.c is compiled as C; a linkage-specification
// is namespace-scope only in C++, so this is declared here (file scope), not inside main().
extern "C" void gdx3ds_hudtop_custom_init(void);
extern "C" int gdx_title_demo_clear_stale(void); // card-4b: decomp_port.c (gdx_racemap.h)

// ---------------------------------------------------------------------------------------------
// O3DSHOME3 — ini-driven bisect variants for the Old 3DS HOME freeze (NEXT.md C4j, SOLVED:
// docs/research/native/o3dshomefix-progress.md). `[debug] bisect_force = G1..G6` forces a named
// lever set through gdx3ds_config_override (RAM only, never written back by the menu). The
// hold-a-button-at-launch selector this started as NEVER caught a hold on the real console
// (wave-2 card: every run logged `variant=none`), so it was removed; the ini path is what stays.
// ---------------------------------------------------------------------------------------------
struct BisectOverride {
    const char* section;
    const char* key;
    const char* value;
};

struct BisectVariant {
    const char* name;
    const char* summary;
    const BisectOverride* overrides;
};

static const BisectOverride kBisectG1[] = {
    {"debug", "audio_ndsp", "0"}, {nullptr, nullptr, nullptr}};
static const BisectOverride kBisectG2[] = {
    {"debug", "audio_ndsp", "0"}, {"debug", "audio_syscore", "0"}, {nullptr, nullptr, nullptr}};
static const BisectOverride kBisectG3[] = {
    {"debug", "audio_ndsp", "0"}, {"debug", "audio_syscore", "0"}, {"debug", "console", "0"},
    {"debug", "fps", "0"},        {"menu", "enabled", "0"},        {"menu", "lcd", "0"},
    {"menu", "screen_off", "0"},  {"debug", "gputrace", "0"},      {"debug", "mainprof", "0"},
    {"debug", "apt_watch_file", "0"}, {nullptr, nullptr, nullptr}};
static const BisectOverride kBisectG4[] = {
    {"debug", "audio_ndsp", "0"}, {"debug", "audio_syscore", "0"}, {"debug", "console", "0"},
    {"debug", "fps", "0"},        {"menu", "enabled", "0"},        {"menu", "lcd", "0"},
    {"menu", "screen_off", "0"},  {"debug", "gputrace", "0"},      {"debug", "mainprof", "0"},
    {"debug", "apt_watch_file", "0"}, {"native", "all", "0"},      {"native", "interp", "0"},
    {nullptr, nullptr, nullptr}};
static const BisectOverride kBisectG5[] = {
    {"menu", "lcd", "0"}, {"menu", "screen_off", "0"}, {nullptr, nullptr, nullptr}};
static const BisectOverride kBisectG6[] = {
    {"debug", "apt_watch_file", "0"}, {nullptr, nullptr, nullptr}};

static const BisectVariant kBisectVariants[] = {
    {"G1", "no DSP (audio_ndsp=0)", kBisectG1},
    {"G2", "G1 + nothing on core 1 (audio_syscore=0)", kBisectG2},
    {"G3", "G2 + no bottom console/menu/HUD/LCD, no gputrace/mainprof, no SD writes in HOME", kBisectG3},
    {"G4", "G3 + native renderer off (native all=0 interp=0)", kBisectG4},
    {"G5", "ini + never touch gsp::Lcd/backlight (menu lcd=0 screen_off=0)", kBisectG5},
    {"G6", "ini + no SD writes while in HOME (apt_watch_file=0)", kBisectG6},
};

static const BisectVariant* sBisectVariant = nullptr;

static const BisectVariant* bisectVariantForName(const char* name) {
    for (const BisectVariant& variant : kBisectVariants) {
        if (name != nullptr && strcasecmp(name, variant.name) == 0) {
            return &variant;
        }
    }
    return nullptr;
}

static void bisectApplyOverrides(const BisectVariant* variant) {
    for (const BisectOverride* o = variant->overrides; o->key != nullptr; o++) {
        gdx3ds_config_override(o->section, o->key, o->value);
    }
}

static void bisectSelectAtBoot(void) {
    sBisectVariant = bisectVariantForName(gdx3ds_config_get_string("debug", "bisect_force", nullptr));
    if (sBisectVariant != nullptr) {
        bisectApplyOverrides(sBisectVariant);
    }
}

// After the filelog is up: silent unless a variant is forced, then one line per lever.
static void bisectLogReceipt(void) {
    if (sBisectVariant == nullptr) {
        return;
    }
    char msg[224];
    std::snprintf(msg, sizeof(msg), "[bisect] variant=%s (debug.bisect_force) -- %s", sBisectVariant->name,
                  sBisectVariant->summary);
    logStep(msg);
    for (const BisectOverride* o = sBisectVariant->overrides; o->key != nullptr; o++) {
        std::snprintf(msg, sizeof(msg), "[bisect]   forced [%s] %s = %s", o->section, o->key, o->value);
        logStep(msg);
    }
}

// n60lead: a key the config tables could not hold used to be dropped silently (multiple cards lost
// launch-variant keys at the old 128 cap). Loud now: boot trail + log, once per checkpoint.
static void logConfigOverflow(const char* where) {
    const char* first = "";
    const int n = gdx3ds_config_overflow(&first);
    if (n == 0) {
        return;
    }
    char msg[200];
    std::snprintf(msg, sizeof(msg), "[config] OVERFLOW at %s: %d key(s) DROPPED (table %d/%d, first %s) -- "
                  "those settings are NOT applied", where, n, gdx3ds_config_entry_count(),
                  gdx3ds_config_entry_capacity(), first);
    logStep(msg);
}

static void logStep(const char* s) {
    // Console (if enabled) AND the emulator/Luma debug channel — the svc line shows
    // up in Azahar's log even when both screens are dark, which is exactly when a
    // boot trace matters most.
    std::printf("[G-Diffuser-3DS] %s\n", s);
    svcOutputDebugString(s, strlen(s));
    gdx3ds_filelog_write(s, strlen(s)); // hardware fallback: sdmc log (debug.filelog=1)
    gdx3ds_boot_trail_file(s);          // n60boot: durable while the boot trail is open
}

// GSTATE (NEXT.md C4p): the gate for VISUAL-ONLY levers. [fh] hashes pixels, so a lever that
// only changes what is drawn diverges on every race frame and [fh] cannot say whether the
// race itself changed. `[debug] gstate_every = N` (DEFAULT 0 = off) emits the simulated-state
// digest (gdx_gstate_hash: RNG streams, logic frame counter, every racer's physics and race
// progress -- never the draw-side LOD fields) every N host frames, keyed by the input tick, so
// two runs of one replay compare with a join on tick=. Main thread, after the game update.
static int sGstateEvery = -1;
static int sGstateExt = 0; // C5TICK [debug] gstate_ext: append ext= (default 0: the line is unchanged)

static void gstateTick(unsigned long frameNum) {
    if (sGstateEvery < 0) {
        sGstateEvery = gdx3ds_config_get_int("debug", "gstate_every", 0);
        sGstateExt = gdx3ds_config_get_int("debug", "gstate_ext", 0) != 0;
        if (sGstateEvery > 0) {
            logStep(sGstateExt ? "[gstate] init: simulated-state digest armed ([debug] gstate_every), "
                                 "+ ext= draw-reachable state ([debug] gstate_ext)"
                               : "[gstate] init: simulated-state digest armed ([debug] gstate_every)");
        }
    }
    if (sGstateEvery <= 0 || (frameNum % (unsigned long)sGstateEvery) != 0) {
        return;
    }
    int racers = 0;
    const unsigned int hash = gdx_gstate_hash(&racers);
    char line[160];
    int n = std::snprintf(line, sizeof(line), "[gstate] frame=%lu tick=%llu mode=%d racers=%d hash=%08x",
                          frameNum, gdx_input_tick(), gdx_input_game_mode(), racers, hash);
    if (sGstateExt && n > 0 && n < (int)sizeof(line)) {
        // C5TICK: the draw-reachable state hash= cannot see (audio command stream, intro timer,
        // clear countdown, racer visibility bits + soundEffectFlags); port/3ds/game/gdx3ds_c5tick_game.c.
        unsigned int audioCmds = 0;
        const unsigned int ext = gdx3ds_gstate_ext_hash(&audioCmds);
        n += std::snprintf(line + n, sizeof(line) - (size_t)n, " ext=%08x acmd=%u", ext, audioCmds);
    }
    if (n > 0) {
        gdx3ds_filelog_write(line, (size_t)n); // one line per call: the filelog adds the newline
    }
}

// ---------------------------------------------------------------------------------------------
// APT lifecycle (hardware power-off crash fix). POWER and HOME both arrive as APT requests:
// POWER sets FLAG_POWERBUTTON, which makes aptMainLoop() jump to the HOME menu (where the
// power menu lives); powering off from there sends the app a close order, aptMainLoop()
// returns false on the wake-back, and the frame loop must then unwind through main()'s
// normal exit path. The previous build pumped aptMainLoop() only from deep inside the
// interpreter's EndFrame (gdx3ds_os_window_swap) — the suspend transition ran mid-dispatch
// with freshly queued async GPU work, which is what crashed on POWER. The pump now lives at
// the TOP of the frame loop (below); this hook logs receipts for every transition so the
// next hardware power-off leaves a provable trace in sdmc log.txt.
//
// Receipt writer: svc + filelog ONLY — no printf. APTHOOK_ONEXIT fires inside aptExit()
// AFTER main() returned, i.e. after gdx3ds_os_window_shutdown() ran gfxExit(); a console
// printf there would write through the freed console framebuffer. The filelog sink is
// static-buffered and thread-safe, so it is legal from every hook context.
// ---------------------------------------------------------------------------------------------
static void aptLogReceipt(const char* msg) {
    svcOutputDebugString(msg, strlen(msg));
    gdx3ds_filelog_write(msg, strlen(msg));
    // Flush every receipt: suspend/sleep may be followed by a power-off that never
    // returns control, and the whole point of these lines is surviving that.
    gdx3ds_filelog_flush_sync();
}

// HOME-crash fix (docs/research/home-crash-audit.md, "## Fix"): everything the suspend
// transition needs from us, in the order libctru requires. Runs on the main thread inside
// aptJumpToHomeMenu / the sleep handler, BEFORE GSPGPU_SaveVramSysArea -> aptScreenTransfer
// -> aptDspSleep (ndspFinalize) -> GSPGPU_ReleaseRight (suspend). The RESTORE hook runs
// inside aptWaitForWakeUp AFTER GSPGPU_AcquireRight + RestoreVramSysArea and BEFORE
// aptDspWakeup -> ndspInitialize (libctru 2.7.0 apt.c aptWaitForWakeUp: hooks first, DSP
// wakeup after — the audit's "after ndspInitialize" was wrong; gdx3ds_audio_resume only
// does CPU-side list surgery + a flag, which is legal there). svc + filelog receipts only.
//
// HOMEFIX (native/homefix, hw run E 2026-09-20, docs/research/native/homefix-progress.md):
// a HOME press with the native levers on ended log.txt after `[apt] audio parked` + two
// frozen watchdog beats — no restore, no [exit] chain, no Luma dump. Nothing between those
// receipts is ours (the hook had finished; the backlight step is a no-op with the bottom
// screen on), so every hook step below is now bracketed by a flushed stage receipt that
// also samples aptIsActive / gspHasGpuRight / render-thread idle / C3D frame open, and the
// watchdog polls the same state every 100 ms while main is inside libctru (aptWatchTick).
static void aptStepReceipt(const char* phase, int step, const char* what) {
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "[apt] %s step=%d %s (t=+%lums active=%d gpu=%d rtIdle=%d frameOpen=%d)", phase, step,
                  what, aptMsSince(sAptStateTick), aptIsActive() ? 1 : 0, gspHasGpuRight() ? 1 : 0,
                  gdx3ds_rt_idle(), gdx3ds_gfx_frame_active());
    aptLogReceipt(msg);
}

static void aptStateSet(int state) {
    if (state != 0) {
        sAptStateTick = svcGetSystemTick(); // t=+0 at the hook entry; kept through "transition complete"
    }
    sAptState = state;
    if (sWatchdogWake != 0) {
        svcSignalEvent(sWatchdogWake); // watchdog: leave the 5 s sleep, start/stop the fine poll
    }
}

// HOMEFIX2: gdx3ds_audio_suspend's wait is already bounded (~50 ms) and proceeds either way;
// what was missing was a receipt that says it gave up. WHO SIGNALS the acks: the drain thread
// (gdx3ds_audio_ndsp.c ParkDrainWhileSuspended) and the HLE producer
// (gdx3ds_audio_producer_park_if_suspended), each on its OWN sticky park event — never a
// shared one (home-crash-audit.md "Close-from-HOME hang fix"). Both can miss the window while
// inside one pass of their loop; libctru then runs aptDspSleep -> ndspFinalize under them.
static void aptAudioSuspend(void) {
    int drainParked = 0;
    int producerParked = 0;
    const int all = gdx3ds_audio_suspend(&drainParked, &producerParked); // bounded, ~50 ms
    char msg[224];
    if (!all) {
        std::snprintf(msg, sizeof(msg),
                      "[apt] audio park TIMEOUT (drain=%d hle=%d) — a 0 means that thread was still "
                      "inside a loop pass; libctru's aptDspSleep -> ndspFinalize runs next and its "
                      "DSP handshake is an unbounded poll",
                      drainParked, producerParked);
    } else {
        std::snprintf(msg, sizeof(msg), "[apt] audio parked (drain=%d hle=%d)", drainParked,
                      producerParked);
    }
    aptLogReceipt(msg);
}

// RENDER THREAD: the frame loop joins the render thread at the loop top BEFORE aptMainLoop
// whenever a HOME/close request is already visible (ahead mode) or always (pipe/sync), so
// the thread is normally parked on its command semaphore with no C3D frame open (idle=1).
// idle=0 is the ahead-mode case where the pump ran without that join: a lid-close sleep
// (aptShouldJumpToHome() is false for sleep), or a HOME flag that landed between the
// loop-top check and aptMainLoop. Then THIS join is the only thing that keeps the render
// thread's C3D_FrameEnd -> gxCmdQueueRun from racing libctru — which is why the hook must
// run BEFORE citro3d's (see the re-arm after InitWindow): citro3d's ONSUSPEND does
// gxCmdQueueWait + gxCmdQueueStop + gxCmdQueueClear on the queue the render thread is
// still filling, and gxCmdQueueClear svcBreak(USERBREAK_PANIC)s if the queue is running.
//
// HOMEFIX2 — THE WAIT AND ITS INVARIANT (the one unbounded wait the 2026-09-23 audit named,
// finding 7). gdx3ds_rt_join_idle() waits on gdx3ds_renderthread.cpp's sDoneEvent.
//   WHO SIGNALS IT: RenderThreadMain, and nothing else, once per command.
//   CAN THE SIGNALLER BE PARKED? Not by a flag — but it can block for an unbounded time
//   inside a command, because kCmdEnd runs EndFrame -> C3D_FrameEnd / C3D_FrameSync and
//   gdx_vi_present_fallback, which wait on GSP vblank/PSC/PPF events. GSP stops delivering
//   those once GSPGPU_ReleaseRight() has run or the shell is closed.
//   THEREFORE: this join is bounded (debug.apt_join_ms, default 1200 ms) and prints a
//   receipt when it gives up. Returning late to libctru is survivable; not returning is not.
static void aptRenderThreadPark(const char* phase) {
    if (gdx3ds_rt_mode() == 0) {
        return;
    }
    const int idle = gdx3ds_rt_idle();
    const uint64_t t0 = svcGetSystemTick();
    int joined = 1;
    if (!idle) {
        if (sAptJoinMs > 0) {
            joined = gdx3ds_rt_join_idle_deadline((unsigned)sAptJoinMs);
        } else {
            gdx3ds_rt_join_idle(); // debug.apt_join_ms=0: the pre-HOMEFIX2 unbounded wait
        }
    }
    char msg[224];
    if (!joined) {
        std::snprintf(msg, sizeof(msg),
                      "[apt] %s render thread park TIMEOUT after %lums (idle=0 rtIdle=%d frameOpen=%d) "
                      "— proceeding into citro3d's gxCmdQueueWait/Clear; a USERBREAK_PANIC dump from "
                      "here means the render thread still owns the GX queue",
                      phase, aptMsSince(t0), gdx3ds_rt_idle(), gdx3ds_gfx_frame_active());
    } else {
        std::snprintf(msg, sizeof(msg), "[apt] render thread parked (idle=%d join=%lums frameOpen=%d)", idle,
                      aptMsSince(t0), gdx3ds_gfx_frame_active());
    }
    aptLogReceipt(msg);
}

static void aptAudioResume(void) {
    gdx3ds_audio_resume();
    aptLogReceipt("[apt] audio resumed");
}

// Cause #4: the HOME menu draws on the bottom screen; if the touch menu's [OFF] holds the
// backlight down, hand it back lit and re-apply the saved state on the way back.
// O3DSHOMEFIX (C4j): gsp::Lcd sessions are now short-lived (gdx3ds_menu.c), so none of ours is
// open across the jump; the receipt says so on every transition (held=1 would be the hazard).
static void aptLcdReceipt(const char* phase, const char* what, int menuOff, int rc) {
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "[apt] %s gsp::Lcd: session held=%d menu screen-off=%d -> %s (rc=0x%08lX)", phase,
                  gdx3ds_menu_lcd_session_open(), menuOff, what, (unsigned long)(unsigned)rc);
    aptLogReceipt(msg);
}

static void aptBacklightForHome(const char* phase) {
    const int menuOff = gdx3ds_menu_backlight_off();
    const int rc = menuOff ? gdx3ds_menu_force_backlight(1) : 0;
    aptLcdReceipt(phase, menuOff ? "bottom backlight ON for the HOME menu" : "backlight untouched (already on)",
                  menuOff, rc);
}

static void aptBacklightRestore(const char* phase) {
    const int menuOff = gdx3ds_menu_backlight_off();
    const int rc = menuOff ? gdx3ds_menu_force_backlight(0) : 0;
    aptLcdReceipt(phase, menuOff ? "bottom backlight OFF re-applied (menu screen-off)" : "backlight untouched",
                  menuOff, rc);
}

// Cause #5: NS re-applies the exheader clock (268 MHz for hbldr) and drops the app's
// syscore share on resume; libctru never re-issues either one-shot from init.
static void aptRestoreCpuState(void) {
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    if (isNew3ds) {
        osSetSpeedupEnable(true);
    }
    const int pct = gdx3ds_audio_syscore_limit_percent();
    Result rc = 0;
    u32 foundPct = 0;
    if (pct > 0) {
        APT_GetAppCpuTimeLimit(&foundPct); // O3DSHOME: what NS / the suspend drop left us
        rc = APT_SetAppCpuTimeLimit((u32)pct);
    }
    char msg[144];
    std::snprintf(msg, sizeof(msg), "[apt] cpu state re-applied (speedup=%d syscore=%d%% rc=0x%08lX found=%lu%%)",
                  isNew3ds ? 1 : 0, pct, (unsigned long)rc, (unsigned long)foundPct);
    aptLogReceipt(msg);
}

// O3DSHOME: which of our threads sit on core 1 and whether they parked, sampled on main after
// the audio park -- the "per-thread state of the core-1 threads at suspend" receipt. Unnumbered
// (a distinct label, never a step), so homefix-progress.md's step table still decodes a log.
static void aptCore1Receipt(const char* phase) {
    int drainParked = -1;
    int producerParked = -1;
    int preloadRunning = 0;
    gdx3ds_audio_park_state(&drainParked, &producerParked, &preloadRunning);
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    char msg[224];
    std::snprintf(msg, sizeof(msg),
                  "[apt] %s core1: model=%s syscore=%d%% drain core=%d parked=%d hle core=%d parked=%d "
                  "preload running=%d sysFreeK=%lu",
                  phase, isNew3ds ? "new3ds" : "old3ds", gdx3ds_audio_syscore_limit_percent(),
                  gdx3ds_audio_drain_core(), drainParked, gdx3ds_audio_producer_core(), producerParked,
                  preloadRunning, (unsigned long)(osGetMemRegionFree(MEMREGION_SYSTEM) / 1024u));
    aptLogReceipt(msg);
}

// O3DSHOME: drop the syscore share to the floor while suspended (see sAptSyscoreDrop). Old 3DS
// with a held grant only; every other configuration returns before touching APT.
static void aptSyscoreDropForSuspend(const char* phase) {
    const int grantedPct = gdx3ds_audio_syscore_limit_percent();
    if (!sAptSyscoreDrop || grantedPct <= (int)kAptSyscoreFloorPct) {
        return;
    }
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    if (isNew3ds) {
        return;
    }
    u32 beforePct = 0;
    const Result getRc = APT_GetAppCpuTimeLimit(&beforePct);
    const Result setRc = APT_SetAppCpuTimeLimit(kAptSyscoreFloorPct);
    char msg[224];
    std::snprintf(msg, sizeof(msg),
                  "[apt] %s syscore share %lu%% -> %lu%% (get rc=0x%08lX set rc=0x%08lX, "
                  "debug.apt_syscore_drop=1; restore step 1 re-applies %d%%) t=+%lums",
                  phase, (unsigned long)beforePct, (unsigned long)kAptSyscoreFloorPct, (unsigned long)getRc,
                  (unsigned long)setRc, grantedPct, aptMsSince(sAptStateTick));
    aptLogReceipt(msg);
}

// N60SAVE: bounded waits for an in-flight [debug] sram_async write (HOMEFIX lesson: every
// suspend-side wait is bounded). A pending write is then done synchronously: one 32 KB file.
static const unsigned kSramSuspendWaitMs = 300;
static const unsigned kSramExitWaitMs = 1000;

// Suspend side (HOME/POWER and lid sleep share it; only HOME lights the bottom panel).
static void aptSuspendSteps(const char* phase, bool forHomeMenu) {
    // N60SAVE: a queued save must not wait out the HOME menu (the console may be powered off
    // from there). No-op unless [debug] sram_async=1 queued something.
    aptStepReceipt(phase, 0, "sram flush");
    gdx_sram_flush_pending(kSramSuspendWaitMs, phase);
    aptStepReceipt(phase, 1, "render thread park");
    aptRenderThreadPark(phase);
    gdx3ds_core1probe_suspend(phase); // N60TICK: probe worker parked, granted share back (one int test off)
    aptStepReceipt(phase, 2, "audio park");
    aptAudioSuspend();
    aptStepReceipt(phase, 3, "net suspend");
    // HOMEFIX2: gdx3ds_net_suspend waits at most 240 ms for the net thread to drop its fds
    // and proceeds either way. WHO SIGNALS the ack: the net thread, at the TOP of its loop
    // only — a thread inside netRecvFileTo misses the window entirely. ack=0 is the receipt.
    {
        const int ack = gdx3ds_net_suspend(); // SOC sockets do not survive a suspend: drop them first
        if (ack == 0) {
            aptLogReceipt("[apt] net suspend TIMEOUT (ack=0) — the net thread did not park in 240 ms; "
                          "SOC IPC may still be in flight across ReleaseRight/JumpToHomeMenu");
        }
    }
    aptCore1Receipt(phase);
    aptSyscoreDropForSuspend(phase);
    if (forHomeMenu) {
        aptStepReceipt(phase, 4, "backlight for HOME menu");
        aptBacklightForHome(phase);
    } else {
        aptLcdReceipt(phase, "lid sleep: backlight left to the system", gdx3ds_menu_backlight_off(), 0);
    }
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  forHomeMenu ? "[apt] %s hook done -> libctru (SaveVramSysArea, screen transfer, DSP sleep, "
                                "ReleaseRight, JumpToHomeMenu, wait) t=+%lums"
                              : "[apt] %s hook done -> libctru (ReplySleepNotificationComplete, wait) t=+%lums",
                  phase, aptMsSince(sAptStateTick));
    aptLogReceipt(msg);
}

// Restore side. Runs BEFORE libctru's aptDspWakeup (HOME) / after the LCD un-black (sleep).
static void aptRestoreSteps(const char* phase) {
    aptStepReceipt(phase, 1, "cpu state");
    aptRestoreCpuState();
    gdx3ds_core1probe_resume(phase); // N60TICK: restart the probe window (one int test off)
    aptStepReceipt(phase, 2, "audio resume");
    aptAudioResume();
    aptStepReceipt(phase, 3, "net resume");
    gdx3ds_net_resume();
    aptStepReceipt(phase, 4, "backlight restore");
    aptBacklightRestore(phase);
    char msg[160];
    std::snprintf(msg, sizeof(msg), "[apt] %s hook done -> libctru (DSP wakeup), frame loop resumes t=+%lums",
                  phase, aptMsSince(sAptStateTick));
    aptLogReceipt(msg);
}

static void aptLifecycleHook(APT_HookType hook, void* param) {
    (void)param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
            gdx3ds_replay_note_apt(); // C6: never replay across a suspend (VRAM/GPU state restored)
            aptStateSet(1);
            aptLogReceipt("[apt] suspend (HOME/POWER -> HOME menu)");
            gdx3ds_mpo_on_suspend(); // mp-online: end a race so the others are not left waiting
            aptSuspendSteps("suspend", true);
            break;
        case APTHOOK_ONRESTORE:
            aptLogReceipt("[apt] restore (resumed from HOME menu)");
            aptRestoreSteps("restore");
            gdx3ds_mpo_on_resume(); // mp-online: the socket thread may make SOC calls again
            aptStateSet(0);
            break;
        case APTHOOK_ONSLEEP:
            gdx3ds_replay_note_apt();
            aptStateSet(2);
            aptLogReceipt("[apt] sleep (lid closed)");
            gdx3ds_mpo_on_suspend();
            aptSuspendSteps("sleep", false);
            break;
        case APTHOOK_ONWAKEUP:
            aptLogReceipt("[apt] wakeup (lid opened)");
            aptRestoreSteps("wakeup");
            gdx3ds_mpo_on_resume();
            aptStateSet(0);
            break;
        case APTHOOK_ONEXIT:
            aptLogReceipt("[apt] exit hook (close order acknowledged, aptExit running)");
            break;
        default:
            break;
    }
}
static aptHookCookie sAptHookCookie;

// ---------------------------------------------------------------------------------------------
// HOMEFIX2 — the DSP hook: the receipt that covers the one blocking call our APT receipts
// could never bracket.
//
// hw run E's log ends immediately after `[apt] audio parked` with the rest of our hook being
// no-ops, i.e. main was already inside libctru when it stopped. The next blocking call in
// that window is aptDspSleep() (libctru apt.c:826 for HOME, apt.c:583 on the APT EVENT
// THREAD for a lid sleep) -> dsp.c:86 dspCallHook(DSPHOOK_ONSLEEP) -> ndsp.c:341
// ndspFinalize(true), whose DSP handshake is an UNBOUNDED `for(;;)` poll of DSP_RecvData
// (ndsp.c:301-330, 4.888 ms per iteration, no deadline, no escape). If the DSP never answers
// 1, main never returns, APT_JumpToHomeMenu is never sent, and NS kills the title with no
// dump — the exact shape of run E.
//
// dspHook is LIFO like aptHook (dsp.c:63-71: the newest cookie becomes dspFirstHook), so
// registering ours AFTER gdx3ds_audio_init() (which runs ndspInit -> ndsp's own dspHook) puts
// this callback AHEAD of ndsp's. The receipt below is therefore written immediately BEFORE
// ndspFinalize runs: a log that ends on it names the step that no previous build could name.
//
// `[debug] apt_dsp_park = 1` additionally parks our audio threads here. That matters on a LID
// SLEEP, where libctru calls aptDspSleep() from its own event thread long before our ONSLEEP
// aptHook runs on main — i.e. ndsp is finalised while our drain thread is still issuing
// ndspChnWaveBufAdd / DSP_FlushDataCache on the shared dsp handle. Default 0: it parks audio
// from a non-main thread, which is a behaviour change, and the receipt alone is what the next
// hardware run needs.
static dspHookCookie sDspHookCookie;
// O3DSHOME: a second DSP hook registered BEFORE gdx3ds_audio_init, i.e. BEHIND ndsp's (LIFO),
// so it runs right after ndspFinalize(true) returns on the suspend path and right after
// ndspInitialize(true) on the wake path. Together with the head hook this brackets ndsp on the
// main thread's own clock: a log with the head line and no tail line IS the DSP handshake.
static dspHookCookie sDspTailHookCookie;

static void dspTailHook(DSP_HookType hook) {
    char msg[224];
    switch (hook) {
        case DSPHOOK_ONSLEEP:
            std::snprintf(msg, sizeof(msg),
                          "[apt] dsp sleep done (ndspFinalize returned, aptState=%d t=+%lums) -> "
                          "DSP_UnloadComponent, ReleaseRight, JumpToHomeMenu next",
                          sAptState, aptMsSince(sAptStateTick));
            aptLogReceipt(msg);
            break;
        case DSPHOOK_ONWAKEUP:
            std::snprintf(msg, sizeof(msg), "[apt] dsp wakeup done (ndspInitialize(true) returned t=+%lums)",
                          aptMsSince(sAptStateTick));
            aptLogReceipt(msg);
            break;
        default:
            break;
    }
}

static void dspLifecycleHook(DSP_HookType hook) {
    switch (hook) {
        case DSPHOOK_ONSLEEP: {
            if (sAptDspPark) {
                int drainParked = 0;
                int producerParked = 0;
                gdx3ds_audio_suspend(&drainParked, &producerParked);
                char pm[160];
                std::snprintf(pm, sizeof(pm),
                              "[apt] dsp sleep: audio parked early (drain=%d hle=%d, debug.apt_dsp_park=1)",
                              drainParked, producerParked);
                aptLogReceipt(pm);
            }
            char msg[224];
            std::snprintf(msg, sizeof(msg),
                          "[apt] dsp sleep hook (aptState=%d t=+%lums) — ndspFinalize(true) NEXT: its "
                          "DSP_RecvData handshake is an unbounded poll. A log that ends HERE is that "
                          "poll, not our hook.",
                          sAptState, aptMsSince(sAptStateTick));
            aptLogReceipt(msg);
            break;
        }
        case DSPHOOK_ONWAKEUP:
            // O3DSHOME: this head hook runs BEFORE ndsp's (LIFO), so ndspInitialize has not run
            // yet; the old text said "done". dspTailHook prints the real "done".
            aptLogReceipt("[apt] dsp wakeup hook (ndspInitialize(true) NEXT)");
            break;
        case DSPHOOK_ONCANCEL:
            aptLogReceipt("[apt] dsp cancel hook (close order while the DSP slept; [exit] chain expected)");
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// M1-RACE-FREEZE fatal-exit tracers. The race-time "freeze" soak proved the guest EXITS —
// the Azahar log's unflushed tail held a clean libctru app teardown (irrstExit -> APT
// PrepareToCloseApplication/CloseApplication -> heap frees -> Core Shutdown OK) with NO
// "exiting" logStep, i.e. main()'s loop never returned: something called exit()/abort()/
// assert or threw. All of those paths are stderr-only (bottom console), invisible headless —
// so each one gets an svc tracer that lands in the Azahar log before teardown runs.
// ---------------------------------------------------------------------------------------------
static void logFatal(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    svcOutputDebugString(msg, strlen(msg));
    // Hardware fallback: the fatal line must survive svcBreak, where no stdio teardown
    // runs — the sink fflushes per line and its buffers are static (no heap), so this
    // is safe even from the operator-new failure path.
    gdx3ds_filelog_write(msg, strlen(msg));
    gdx3ds_filelog_flush_sync();
}

// newlib assert(): the default __assert_func prints to stderr and aborts — both invisible.
extern "C" __attribute__((noreturn)) void __assert_func(const char* file, int line,
                                                        const char* func, const char* expr) {
    char msg[256];
    std::snprintf(msg, sizeof(msg), "[fatal] assert failed: %s (%s:%d in %s)",
                  expr ? expr : "?", file ? file : "?", line, func ? func : "?");
    logFatal(msg);
    svcBreak(USERBREAK_ASSERT);
    for (;;) {
        svcSleepThread(1000000000LL);
    }
}

// abort(): reached by direct calls and by newlib raise(SIGABRT). Overriding the archive
// symbol is safe — nothing else lives in newlib's abort.o. svcBreak makes Azahar log the
// stop loudly instead of tearing down silently.
extern "C" __attribute__((noreturn)) void abort(void) {
    logFatal("[fatal] abort() called");
    svcBreak(USERBREAK_PANIC);
    for (;;) {
        svcSleepThread(1000000000LL);
    }
}

// Replaceable global operator new: on failure, name the SIZE, an approximate caller (return
// address, symbolizable offline via addr2line) and the heap state before throwing — the
// difference between "steady leak finally exhausted the heap" (used ~= arena, small size) and
// "one bogus ILP32-overflowed allocation" (huge size, plenty free) in a single log line.
static void logAllocFailure(const char* which, size_t size, void* ra) {
    struct mallinfo mi = mallinfo();
    char msg[176];
    std::snprintf(msg, sizeof(msg),
                  "[fatal] %s failed size=%lu heapUsed=%lu heapFree=%lu arena=%lu ra=%p",
                  which, (unsigned long)size, (unsigned long)mi.uordblks,
                  (unsigned long)mi.fordblks, (unsigned long)mi.arena, ra);
    logFatal(msg);
}

// M1-MEMORY big-allocation tracer: the race-time heap growth (44.7 -> 87.2 MB) needs its
// allocation SITES named, not just the final failing one. Every C++ allocation >= 1 MB logs
// size + return address (addr2line vs G-Diffuser-3DS.elf); >= 256 KB allocations are only
// counted, and the cumulative counters ride the [mem-census] frame-loop line. svc-only
// logging (no printf, no heap) so the tracer itself cannot allocate.
volatile unsigned long gBigAllocBytes = 0; // cumulative bytes of allocations >= 256 KB
volatile unsigned long gBigAllocCount = 0;
static void traceBigAlloc(std::size_t size, void* ra) {
    if (size < 256u * 1024u) {
        return;
    }
    gBigAllocBytes += (unsigned long)size;
    gBigAllocCount += 1;
    if (size >= 1024u * 1024u) {
        static int sBigAllocLogs = 0; // benign race: worst case a few extra capped lines
        if (sBigAllocLogs < 96) {
            sBigAllocLogs = sBigAllocLogs + 1;
            char msg[96];
            int n = std::snprintf(msg, sizeof(msg), "[big-alloc] size=%lu ra=%p",
                                  (unsigned long)size, ra);
            if (n > 0) {
                svcOutputDebugString(msg, n);
            }
        }
    }
}

// S2 MALLOC-CRAWL: opt-in allocation-size histogram. Every C++ allocation (the two throwing
// operator new/new[] below funnel through here) is tallied into a power-of-two size class so a
// later pass can see allocation churn without a profiler. Additive and off by default; enabled by
// GDX3DS_MALLOC_HISTOGRAM=1 or the [debug] malloc_histogram INI key (set in main() before the
// frame loop). Like traceBigAlloc, this touches ONLY fixed static counters -- no printf, no heap,
// no lock -- so the instrumentation can run inside the allocator itself; the benign aligned-word
// races on the counters match the rest of the port's telemetry.
//
// Buckets: index i counts allocations with size in [2^(i-1)+1 .. 2^i], i.e. bucket 0 = {0,1},
// bucket 4 = 9..16 bytes, bucket 20 = 512KiB..1MiB. Index 31 is the >2GiB catch-all (never hit).
#define GDX_MALLOC_HIST_BUCKETS 32
static volatile int sMallocHistogramEnabled = 0;
static volatile unsigned long gMallocHistCount[GDX_MALLOC_HIST_BUCKETS];
static volatile unsigned long gMallocHistBytes[GDX_MALLOC_HIST_BUCKETS];

static inline int gdxMallocSizeBucket(std::size_t size) {
    // Ceil(log2(size)) clamped to the array: the smallest bucket holding `size`.
    if (size <= 1u) {
        return 0;
    }
    // 32-bit size_t on the 3DS (ILP32): 31 - clz gives floor(log2); +1 unless already a power of 2.
    unsigned int v = (unsigned int)size;
    int floorLog2 = 31 - __builtin_clz(v);
    int bucket = ((v & (v - 1u)) != 0u) ? floorLog2 + 1 : floorLog2;
    if (bucket >= GDX_MALLOC_HIST_BUCKETS) {
        bucket = GDX_MALLOC_HIST_BUCKETS - 1;
    }
    return bucket;
}

static inline void tallyMallocHistogram(std::size_t size) {
    if (!sMallocHistogramEnabled) {
        return;
    }
    int b = gdxMallocSizeBucket(size);
    gMallocHistCount[b] += 1;
    gMallocHistBytes[b] += (unsigned long)size;
}

// Dump the histogram to the debug log (svc channel). Bounded to buckets that were actually hit so
// the output stays a handful of lines. Called on the mem-census cadence from the frame loop.
static void dumpMallocHistogram(unsigned long frameNum) {
    if (!sMallocHistogramEnabled) {
        return;
    }
    for (int b = 0; b < GDX_MALLOC_HIST_BUCKETS; b++) {
        unsigned long count = gMallocHistCount[b];
        if (count == 0) {
            continue;
        }
        // Human-readable size-class upper bound for this bucket (2^b), 0/1 for bucket 0.
        unsigned long upper = (b == 0) ? 1ul : (1ul << b);
        char msg[96];
        int n = std::snprintf(msg, sizeof(msg),
                              "[malloc-hist] frame=%lu class<=%luB count=%lu bytes=%lu",
                              frameNum, upper, count, gMallocHistBytes[b]);
        if (n > 0) {
            svcOutputDebugString(msg, n);
            gdx3ds_filelog_write(msg, (size_t)n);
        }
    }
}

// LEAK: LIVE per-size-class accounting. The cumulative histogram above names churn but
// cannot separate a retained allocation from a freed one, which is exactly the question a
// monotonic heapUsed climb asks. Every C++ new/delete below also updates a live counter,
// bucketed by malloc_usable_size (stable across the new/delete pair, so the size-less
// plain operator delete still debits the bucket the new credited). Same benign-race
// counter policy as the rest of this tooling; C-side malloc() is not seen, but every STL
// container the growth suspects live in allocates through operator new. Additionally, one
// in every 512 allocations in the 129..1024-byte window (where the race-time churn
// concentrates) logs its return address: "[live-ra] size=N ra=P", cap 512 lines;
// addr2line vs G-Diffuser-3DS.elf names the allocation sites offline.
extern "C" size_t malloc_usable_size(void* ptr);
static volatile unsigned long gLiveHistCount[GDX_MALLOC_HIST_BUCKETS];
static volatile unsigned long gLiveHistBytes[GDX_MALLOC_HIST_BUCKETS];

/* C4c: the RA sampler's window is aimable. It shipped hard-coded to 129..1024 B sampled
 * 1-in-512, which was right for the leak hunt it was built for (race-time churn concentrates
 * there) and useless for a RESIDENCY question, where the bytes that matter are the ones no
 * [mem-res] row owns and they are not all in that window. The four keys below default to
 * exactly the old numbers, so an unaimed run logs byte-identical output; point them at
 * whichever [live-hist] class dominates and that class's sites get named.
 *   [debug] live_ra_min / live_ra_max   inclusive usable-size window (default 129 / 1024)
 *   [debug] live_ra_every               log 1 in N matching allocations (default 512)
 *   [debug] live_ra_cap                 stop after N lines (default 512)
 * All four are read once, in main(), before the histogram is armed. */
static volatile unsigned long sLiveRaMin = 129;
static volatile unsigned long sLiveRaMax = 1024;
static volatile unsigned long sLiveRaEvery = 512;
static volatile int sLiveRaCap = 512;

static inline void tallyLiveAlloc(void* p, void* ra) {
    if (!sMallocHistogramEnabled || p == nullptr) {
        return;
    }
    size_t usable = malloc_usable_size(p);
    int b = gdxMallocSizeBucket(usable);
    gLiveHistCount[b] += 1;
    gLiveHistBytes[b] += (unsigned long)usable;
    if (usable >= sLiveRaMin && usable <= sLiveRaMax) {
        static volatile unsigned long sRaTick = 0;
        sRaTick = sRaTick + 1;
        if ((sRaTick % sLiveRaEvery) == 0u) {
            static volatile int sRaLogs = 0;
            if (sRaLogs < sLiveRaCap) {
                sRaLogs = sRaLogs + 1;
                char msg[64];
                int n = std::snprintf(msg, sizeof(msg), "[live-ra] size=%lu ra=%p",
                                      (unsigned long)usable, ra);
                if (n > 0) {
                    svcOutputDebugString(msg, n);
            gdx3ds_filelog_write(msg, (size_t)n);
                }
            }
        }
    }
}

static inline void tallyLiveFree(void* p) {
    if (!sMallocHistogramEnabled || p == nullptr) {
        return;
    }
    size_t usable = malloc_usable_size(p);
    int b = gdxMallocSizeBucket(usable);
    gLiveHistCount[b] -= 1;
    gLiveHistBytes[b] -= (unsigned long)usable;
}

// [live-hist] dump on the same cadence as [malloc-hist]; live=/bytes= are CURRENT levels,
// so two dumps subtract to exactly the retained growth per class.
// Printed SIGNED: when the histogram is armed mid-run (heap_watch_arm), frees of pre-arm
// allocations drive a class negative -- net frees since arming -- and the dump-to-dump
// difference is still the retained growth.
static void dumpLiveHistogram(unsigned long frameNum) {
    if (!sMallocHistogramEnabled) {
        return;
    }
    long liveTotal = 0;
    long liveCount = 0;
    for (int b = 0; b < GDX_MALLOC_HIST_BUCKETS; b++) {
        unsigned long count = gLiveHistCount[b];
        if (count == 0) {
            continue;
        }
        unsigned long upper = (b == 0) ? 1ul : (1ul << b);
        char msg[96];
        int n = std::snprintf(msg, sizeof(msg),
                              "[live-hist] frame=%lu class<=%luB live=%ld bytes=%ld",
                              frameNum, upper, (long)count, (long)gLiveHistBytes[b]);
        if (n > 0) {
            svcOutputDebugString(msg, n);
            gdx3ds_filelog_write(msg, (size_t)n);
        }
        liveTotal += (long)gLiveHistBytes[b];
        liveCount += (long)count;
    }
    /* C4c: the class rows are only half the answer. This TU hooks operator new/delete and
     * NOTHING else, so plain C malloc() -- libctru thread stacks, the fiber stacks, the
     * RDRAM arena's calloc, newlib's own buffers -- is invisible to every row above. The
     * totals line states that blind spot instead of leaving it to be discovered: `cxx` is
     * what the size classes account for and `cmalloc` is mallinfo's live total minus it,
     * which is the C-side residency nobody has ever measured. */
    {
        struct mallinfo mi = mallinfo();
        const long heapUsed = (long)mi.uordblks;
        char msg[128];
        int n = std::snprintf(msg, sizeof(msg),
                              "[live-hist] totals frame=%lu live=%ld cxx=%ld heapUsed=%ld "
                              "cmalloc=%ld",
                              frameNum, liveCount, liveTotal, heapUsed, heapUsed - liveTotal);
        if (n > 0) {
            svcOutputDebugString(msg, n);
            gdx3ds_filelog_write(msg, (size_t)n);
        }
    }
}

void* operator new(std::size_t size) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (p == nullptr) {
        logAllocFailure("operator new", size, __builtin_return_address(0));
        throw std::bad_alloc();
    }
    traceBigAlloc(size, __builtin_return_address(0));
    tallyMallocHistogram(size);
    tallyLiveAlloc(p, __builtin_return_address(0));
    return p;
}

void* operator new[](std::size_t size) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (p == nullptr) {
        logAllocFailure("operator new[]", size, __builtin_return_address(0));
        throw std::bad_alloc();
    }
    traceBigAlloc(size, __builtin_return_address(0));
    tallyMallocHistogram(size);
    tallyLiveAlloc(p, __builtin_return_address(0));
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size != 0 ? size : 1);
    tallyLiveAlloc(p, __builtin_return_address(0));
    return p;
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size != 0 ? size : 1);
    tallyLiveAlloc(p, __builtin_return_address(0));
    return p;
}

void operator delete(void* p) noexcept { tallyLiveFree(p); std::free(p); }
void operator delete[](void* p) noexcept { tallyLiveFree(p); std::free(p); }
void operator delete(void* p, std::size_t) noexcept { tallyLiveFree(p); std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { tallyLiveFree(p); std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { tallyLiveFree(p); std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { tallyLiveFree(p); std::free(p); }

static void fatalTerminateHandler() {
    const std::type_info* t = abi::__cxa_current_exception_type();
    char msg[256];
    if (t != nullptr) {
        std::snprintf(msg, sizeof(msg), "[fatal] std::terminate: uncaught exception type=%s",
                      t->name());
        try {
            throw; // re-throw to extract what() when it is a std::exception
        } catch (const std::exception& e) {
            std::snprintf(msg, sizeof(msg),
                          "[fatal] std::terminate: uncaught %s what=%s", t->name(), e.what());
        } catch (...) {
        }
    } else {
        std::snprintf(msg, sizeof(msg), "[fatal] std::terminate (no active exception)");
    }
    logFatal(msg);
    svcBreak(USERBREAK_PANIC);
    for (;;) {
        svcSleepThread(1000000000LL);
    }
}

// Mirror every gdx_port_logf line onto the svc debug channel. The port's diagnostic
// families ([gdl-bad], [segload], [transition], ...) otherwise reach only the
// bottom-screen console (stderr), which is unreadable in headless Azahar runs; the
// svc channel lands in Azahar's log with log_filter=*:Debug (m1-boot-debug.md).
static void portLogSvcTap(const char* message) {
    size_t len = strlen(message);
    while (len > 0 && (message[len - 1] == '\n' || message[len - 1] == '\r')) {
        len--; // Azahar logs one line per svc call already
    }
    if (len > 0) {
        svcOutputDebugString(message, len);
        gdx3ds_filelog_write(message, len);
    }
}

// C4d/audiocache: [debug] audiodiet_report, frames between [audio-diet] receipts (0 = off).
static int sAudioDietReportFrames = 0;

// [mem-census]: heap attributed to its owners (see the comment at the periodic call site).
// Main thread only. Reaches svc AND the sdmc filelog so hardware runs keep it.
static void emitMemCensus(Ship::Context* ctx, unsigned long frameNum, const char* why) {
    unsigned long census[8] = {};
    gdx3ds_rt_join_idle(); // the census scans the bridge's containers: render thread idle first
    gdx_gfx_mem_census(census);
    unsigned long lusBytes = 0;
    if (ctx != nullptr && ctx->GetResourceManager() != nullptr) {
        lusBytes = (unsigned long)ctx->GetResourceManager()->GetCacheByteSize();
    }
    struct mallinfo mi = mallinfo();
    char msg[240];
    int n = std::snprintf(
        msg, sizeof(msg),
        "[mem-census] frame=%lu heapUsed=%lu heapFree=%lu arena=%lu lin=%lu "
        "seg=%lu/%lu tex=%lu/%lu wide=%lu/%lu setup=%lu ranges=%lu lus=%lu "
        "big=%lu/%lu",
        (unsigned long)frameNum, (unsigned long)mi.uordblks, (unsigned long)mi.fordblks,
        (unsigned long)mi.arena, (unsigned long)linearSpaceFree(), census[0], census[1],
        census[2], census[3], census[4], census[5], census[6], census[7], lusBytes,
        gBigAllocBytes, gBigAllocCount);
    if (n > 0) {
        svcOutputDebugString(msg, n);
        gdx3ds_filelog_write(msg, (size_t)n);
    }
    // O3DS BUDGET: the ranked residency census rides this cadence (and this render-thread
    // join) rather than opening a second one. No-op unless [debug] membudget=1.
    gdx3ds_membudget_census(frameNum, why, census, lusBytes, gBigAllocBytes);
    // C4d: the audio_table read census rides the same cadence. No-op unless [debug]
    // audiodiet or [debug] audio_stream is set.
    gdx_audio_diet_receipt();
    (void)why;
}

// ORACLE ([debug] keys, docs/research/native/oracle-progress.md):
//   det=1            master switch: defaults rand_seed1=0x1234abcd rand_seed2=0x0fedcba9
//                    boot_hold_ticks=230 det_audio=1 unless the individual key is set
//   rand_seed1/2     Math_Rand1/2Init seeds (0x-hex or decimal); absent = original osGetTime()
//   boot_hold_ticks  boot-logo hold counted in host ticks instead of wall clock (0 = original)
//   det_audio        deterministic audioRandom counter
//   input_record=1   record rec/<input_record_name|rec0>.gdxrec from tick 0
//   input_replay=X   replay rec/X.gdxrec from tick 0 (wins over gdx-autoinput.txt)
//   replay_quit=1    leave the frame loop at "[rec] end" (self-terminating parity runs)
// Receipts: "[det] ..." and "[rec] ..." lines.
// COVERAGE: the [cov] census's log sink — the same two channels every other receipt uses.
static void covLogSink(const char* line) {
    const size_t n = strlen(line);
    svcOutputDebugString(line, n);
    gdx3ds_filelog_write(line, n);
}

static void oracleReplayEnded() {
    gdx3ds_oracle_request_quit("[rec] end with replay_quit=1");
}

static void oracleInitDeterminismAndRec() {
    const int det = gdx3ds_config_get_bool("debug", "det", 0);
    const char* seed1Str = gdx3ds_config_get_string("debug", "rand_seed1", det ? "0x1234abcd" : "");
    const char* seed2Str = gdx3ds_config_get_string("debug", "rand_seed2", det ? "0x0fedcba9" : "");
    const int seed1Have = seed1Str != nullptr && seed1Str[0] != '\0';
    const int seed2Have = seed2Str != nullptr && seed2Str[0] != '\0';
    const unsigned seed1 = seed1Have ? (unsigned)std::strtoul(seed1Str, nullptr, 0) : 0u;
    const unsigned seed2 = seed2Have ? (unsigned)std::strtoul(seed2Str, nullptr, 0) : 0u;
    const int bootHold = gdx3ds_config_get_int("debug", "boot_hold_ticks", det ? 230 : 0);
    const int audioPin = gdx3ds_config_get_bool("debug", "det_audio", det);
    gdx_det_init(seed1Have, seed1, seed2Have, seed2, bootHold, audioPin);
    const int titleDemo = audioPin ? gdx3ds_config_get_int("debug", "det_title_demo", 0) : 0; /* niview */
    gdx_det_set_title_demo(titleDemo, gdx3ds_config_get_int("debug", "det_title_demo_len", 1500));
    {
        char msg[192];
        int n = std::snprintf(msg, sizeof(msg), "[det] init det=%d seed1=%s%08x seed2=%s%08x boot_hold_ticks=%d audio=%d title_demo=%d",
                              det, seed1Have ? "0x" : "none:", seed1, seed2Have ? "0x" : "none:", seed2, bootHold,
                              audioPin, titleDemo);
        if (n > 0) {
            svcOutputDebugString(msg, n);
            gdx3ds_filelog_write(msg, (size_t)n);
        }
    }

    gdx3ds_native_boot_receipt(); // [native] all= interp= machines= track= fx_sink= fx_sprites= fx_hud= fx=

    const char* replayName = gdx3ds_config_get_string("debug", "input_replay", "");
    const int replayOn = replayName != nullptr && replayName[0] != '\0';
    char pins[128];
    std::snprintf(pins, sizeof(pins), "renderthread=%d rival_detail_auto=%d stereo=%d fh_every=%d",
                  gdx3ds_config_get_int("debug", "renderthread", 1),
                  replayOn ? 0 : (gdx3ds_config_get_bool("perf", "rival_detail_auto", 1) ? 1 : 0),
                  gdx3ds_config_get_bool("stereo", "enabled", 0) ? 1 : 0, gdx3ds_config_get_int("debug", "fh_every", 0));
    GdxInputRecHeader hdr;
    hdr.build = gdx3ds_oracle_build_id();
    hdr.seed1Have = seed1Have;
    hdr.seed1 = seed1;
    hdr.seed2Have = seed2Have;
    hdr.seed2 = seed2;
    hdr.pins = pins;
    gdx_input_rec_init(&hdr);

    /* ORACLE .gdxrec paths are ABSOLUTE (gdx3ds_fs_base_path() == "sdmc:/3ds/gdiffuser/").
     * They used to be the bare relative "rec/<name>.gdxrec", which only resolves when the
     * loader happens to set the process cwd to the gdiffuser directory: true under Azahar
     * Qt, false under the Azahar libretro core (RetroArch lane), where every replay run
     * silently logged "[rec] replay open failed: rec/<name>.gdxrec" and then ran on
     * physical/autoinput input — i.e. a parity gate that compared two non-replays. */
    if (gdx3ds_config_get_bool("debug", "input_record", 0)) {
        char path[192];
        std::snprintf(path, sizeof(path), "%srec/%s.gdxrec", gdx3ds_fs_base_path(),
                      gdx3ds_config_get_string("debug", "input_record_name", "rec0"));
        gdx_input_rec_start_record(path);
    }
    if (replayOn) {
        char path[192];
        std::snprintf(path, sizeof(path), "%srec/%s.gdxrec", gdx3ds_fs_base_path(), replayName);
        if (gdx_replay_start(path) == 0) {
            // DYNLOD tier depends on svcGetSystemTick() deltas (plan §3 item 3): pinned under
            // replay, because a host-speed-dependent tier is not reproducible and would make
            // [fh] parity meaningless. [debug] replay_dynlod=1 keeps the controller LIVE for
            // the one thing a replay can still measure honestly: how often the controller
            // changes tier under a fixed input stream (the A/B in
            // docs/research/native/dynlod-progress-native.md). Such a run is explicitly NOT a
            // parity run -- the receipt says so.
            if (gdx3ds_config_get_bool("debug", "replay_dynlod", 0)) {
                logStep("[rec] replay: [debug] replay_dynlod=1 -- DYNLOD live, this run is NOT parity-valid");
            } else {
                gdx3ds_dynlod_set_auto(0);
                logStep("[rec] replay: [perf] rival_detail_auto pinned to 0 for this run");
            }
            if (gdx3ds_config_get_bool("debug", "replay_quit", 0)) {
                gdx_input_rec_set_end_callback(oracleReplayEnded);
            }
        }
    }
}

int main(int argc, char** argv) {
    svcOutputDebugString("main entered", 12);
    // n60boot: FIRST, before config/gfxInit/the file log: boot.txt (one flushed write per
    // milestone) + the main thread's exception handler for the boot window.
    gdx3ds_boot_trail_open();
    gdx_port_log_tap = &portLogSvcTap;
    // Fatal-exit tracers (see above): name the killer in the Azahar log before teardown.
    std::set_terminate(&fatalTerminateHandler);
    std::atexit([] {
        // Fires on any exit() (LIFO — registered first, runs last), NOT on _exit/svcBreak.
        // The normal return path logs "exiting" first, so an atexit line WITHOUT it means
        // some code called exit() directly.
        logFatal("[fatal-trace] atexit: process exiting via exit()");
    });
    // 1. Stream B config, then window/gfx bring-up FIRST so the debug console (INI
    //    debug.console=1) exists before any step that can hang; boot-trace order
    //    deviation from desktop is deliberate.
    logStep("gdx3ds_config_load");
    gdx3ds_config_set_preset_resolver(&gdx3ds_preset_resolve); // n3ds: [perf] preset = n60 (defaults layer)
    gdx3ds_config_load(GDX3DS_CONFIG_DEFAULT_PATH);
    // n60lead: AUTO per console model, BEFORE any lever is read (same requirement as the
    // bisect select right below). A no-ini player must land on the SAME tuned stack the
    // hardware test cards carry an explicit `[perf] preset` key for, not the shipped/untuned
    // defaults: New 3DS -> n60 (identical to an explicit `preset = n60`); Old 3DS -> the
    // "o3ds-auto" table (gdx3ds_preset_n60.c: n60's own table plus the Old 3DS memory-fit +
    // syscore-safety levers it needs). An explicit non-auto `[perf] preset` key is untouched --
    // this only feeds the layer `[perf] preset` leaves absent/"auto". Model probe failure
    // leaves it unset (no table), same as before this fix: never degrade a console we cannot
    // identify.
    {
        bool isNew3ds = false;
        if (R_SUCCEEDED(APT_CheckNew3DS(&isNew3ds))) {
            gdx3ds_config_set_auto_preset_name(isNew3ds ? "n60" : "o3ds-auto");
        }
    }
    gdx3ds_boot_trail_latch_config(); // n60boot: [debug] boot_trail=0 closes it here
    logConfigOverflow("config load");
    bisectSelectAtBoot(); // O3DSHOME3: before ANY lever is read
    gdx3ds_nrr_crumb_boot_select(); // n60hwdiag: nrr_bisect_cycle forces this boot's NRR subset; names the log
    // N60LAUNCH ([debug] launch_variants, default 0): launch_force / launch_cycle pick + apply the
    // variant HERE, before every lever latch; the hold mode only arms its window (below) and
    // defers the file log so it opens under the variant's name.
    gdx3ds_launch_boot_early(&logStep);

    // S2 MALLOC-CRAWL: arm the allocation-size histogram if requested. env wins over the INI key so
    // a run can enable it without editing sdmc:/3ds/gdiffuser/gdiffuser.ini ([debug] section).
    {
        const char* env = std::getenv("GDX3DS_MALLOC_HISTOGRAM");
        int enabled = (env != nullptr) ? (env[0] != '0' && env[0] != '\0')
                                       : gdx3ds_config_get_bool("debug", "malloc_histogram", 0);
        sMallocHistogramEnabled = enabled;
        sLiveRaMin = (unsigned long)gdx3ds_config_get_int("debug", "live_ra_min", 129);
        sLiveRaMax = (unsigned long)gdx3ds_config_get_int("debug", "live_ra_max", 1024);
        {
            const int every = gdx3ds_config_get_int("debug", "live_ra_every", 512);
            sLiveRaEvery = (every > 0) ? (unsigned long)every : 1ul; /* % 0 would trap */
        }
        sLiveRaCap = gdx3ds_config_get_int("debug", "live_ra_cap", 512);
        if (enabled) {
            logStep("malloc histogram ENABLED (dumps to [malloc-hist] on the mem-census cadence)");
        }
    }
    // O3DS BUDGET: latch [debug] membudget / membudget_top. The boot receipt itself waits
    // until the window and the heaps are up (gdx3ds_membudget_boot, below).
    gdx3ds_membudget_configure();
    sHeapWatchMb = gdx3ds_config_get_int("debug", "heap_watch", 8);
    sHeapWatchStepMb = gdx3ds_config_get_int("debug", "heap_watch_step", 4);
    sHeapWatchArm = gdx3ds_config_get_bool("debug", "heap_watch_arm", 0);

    // File-log sink right after config (sdmc: is mounted by libctru's __appInit, so the
    // INI read above already proved the device works). Earlier logStep lines only reach
    // svc/console; everything from here lands in sdmc:/3ds/gdiffuser/log.txt when
    // debug.filelog=1.
    if (!gdx3ds_launch_defers_filelog()) {
        gdx3ds_filelog_init();
        gdx3ds_boot_trail("[boot] file log open");
    } else {
        gdx3ds_boot_trail("[boot] file log deferred to the end of the launch window");
    }
    // REL1CRASH: CPU exception handler on this (main) thread and, via --wrap=threadCreate,
    // on every thread created from here on; [debug] crashdump. First after the sink so a
    // fault anywhere later leaves crash.txt + the unflushed log tail.
    gdx3ds_crash_init();
    gdx3ds_boot_trail("[boot] gdx3ds_crash_init done");
    bisectLogReceipt();
    {
        // n3ds: the preset defaults layer, one line (absent = no preset, every default shipped).
        // n60lead: gdx3ds_config_preset_name() is the name actually fed to the resolver this
        // generation -- the ini's own literal text, OR the auto substitution when the ini
        // leaves `[perf] preset` absent/"auto" -- never gdx3ds_config_get_string's raw ini
        // value, which auto would print as empty/"auto" even though a real table applied.
        const int presetKeys = gdx3ds_config_preset_count();
        if (presetKeys > 0) {
            char line[160];
            const char* presetName = gdx3ds_config_preset_name();
            std::snprintf(line, sizeof(line),
                          "[preset] %s: %d lever defaults from the built-in table (explicit ini keys win)",
                          presetName != NULL ? presetName : "?", presetKeys);
            gdx3ds_boot_trail(line);
            std::strncat(line, "\n", sizeof(line) - std::strlen(line) - 1);
            gdx3ds_filelog_write(line, std::strlen(line));
        }
    }
    gdx3ds_nrr_crumb_boot_receipt(); // n60hwdiag: [nrrbisect] boot=/subset= + the first durable crumb
    // O3DS BUDGET: the grant receipt, as early as a sink exists for it. The grants are
    // fixed at __system_allocateHeaps (before main), so this is the whole truth about what
    // this console gave us -- and it prints BEFORE the ~10.7 MB audio_table preload below,
    // so a build that dies during that load still says what it was working with.
    gdx3ds_membudget_boot();
    // HWLINK: dev link before the window so boot logSteps reach the host. [net] enabled's
    // default is now conditional on the launch method (see gdx3ds_net.h/.c): off for a plain
    // hbmenu/SD/CIA launch (immediate return -- no SOC buffer, no thread, no receipt), on when
    // 3dslink's netloader launched us. An explicit ini `enabled` key always wins either way.
    gdx3ds_net_init(argc, argv);
    // APT lifecycle receipts (see aptLifecycleHook above): registered as early as the
    // filelog allows, so even a HOME/POWER press during boot leaves its trace.
    aptHook(&sAptHookCookie, aptLifecycleHook, nullptr);
    // HOMEFIX killswitches (receipt below, after the window is up).
    sAptWatch = gdx3ds_config_get_bool("debug", "apt_watch", 1) ? 1 : 0;
    sAptHookFirst = gdx3ds_config_get_bool("debug", "apt_hook_first", 1) ? 1 : 0;
    sAptJoinMs = gdx3ds_config_get_int("debug", "apt_join_ms", 1200);
    if (sAptJoinMs < 0) {
        sAptJoinMs = 0;
    }
    sAptDspHook = gdx3ds_config_get_bool("debug", "apt_dsp_hook", 1) ? 1 : 0;
    sAptDspPark = gdx3ds_config_get_bool("debug", "apt_dsp_park", 0) ? 1 : 0;
    sAptSyscoreDrop = gdx3ds_config_get_bool("debug", "apt_syscore_drop", 1) ? 1 : 0;
    sAptWatchFile = gdx3ds_config_get_bool("debug", "apt_watch_file", 1) ? 1 : 0;
    if (R_FAILED(svcCreateEvent(&sWatchdogWake, RESET_ONESHOT))) {
        sWatchdogWake = 0; // watchdog falls back to plain sleeps (no early wake)
    }
    logStep("gdx3ds_os_window_init");
    int width = 0;
    int height = 0;
    if (gdx3ds_os_window_init(&width, &height) != 0) {
        return 1;
    }
    // N60LAUNCH hold mode: the prompt on the bottom screen, HID every vblank for launch_window_ms,
    // then the variant's overrides and the deferred file log. Before the HUD takes the bottom
    // screen and before audio/mainprof/core1probe latch their levers. No-op otherwise.
    gdx3ds_launch_boot_window();
    logConfigOverflow("launch variants");
    // ship6: the resident-track arena (two course slots + bake scratch, one linearAlloc) now, while
    // the linear heap is unfragmented; every course bake reuses it (no per-course allocation).
    gdx3ds_track_resident_reserve();
    // [net] mp = host|join (DEFAULT OFF: no socket, no receipt). After the launch window so a
    // hardware card can pick mp_delay / the role per launch variant (hold a button).
    gdx3ds_mp_init();
    gdx3ds_boot_trail("[boot] fps hud init");
    // FPS-HUD (debug.fps, default ON): owns the bottom-screen console + its reserved
    // top rows. Before the first post-window logStep so every console line scrolls in
    // the HUD's log window from the start.
    gdx3ds_fps_hud_init();
    // MENU (menu.enabled, default ON): takes over the bottom-screen layout (tab bar +
    // pages, fps line pinned as row 1); the HUD detects it and hands its line over.
    gdx3ds_boot_trail("[boot] menu init");
    gdx3ds_menu_init();
    // nihudtop Phase 3: dev-only CUSTOM top-screen mask lever ([debug] hud_custom_top_mask/
    // hud_custom_active). Independent of the MENU/DASH init above; see the file's own header
    // and the file-scope declaration near the top of this file.
    gdx3ds_hudtop_custom_init();
    // ORACLE: capture oracle, determinism pins and the .gdxrec recorder/replayer — all before the
    // first controller poll (line 0 of a recording == tick 0) and before the game seeds its RNGs.
    gdx3ds_oracle_init();
    // COVERAGE: armed next to the oracle because the two share a frame number by construction
    // (gdx3ds_oracle_after_endframe calls gdx3ds_cov_frame with the tuple it stamps [fh] with).
    gdx3ds_cov_arm(gdx3ds_config_get_bool("debug", "cov", 0),
                   gdx3ds_config_get_bool("debug", "cov_verbose", 0), covLogSink);
    oracleInitDeterminismAndRec();
    logStep("window up; gdx_dev_gates_init_env");

    // 2. Developer gates (desktop parity has these first; safe after window).
    gdx_dev_gates_init_env();
    gdx3ds_crash_breadcrumbs_arm(); // REL1CRASH: [debug] boot_breadcrumbs (default 0)

    // 3. SD filesystem.
    logStep("gdx3ds_fs_init");
    if (gdx3ds_fs_init() != 0) {
        logStep("WARNING: gdx3ds_fs_init failed (no SD?); asset reads will miss");
    }

    // 4. Minimal LUS Context (lus_glue stub): CVars + ResourceManager over the
    //    pre-baked archives. Mount order mirrors desktop findArchivePaths:
    //    gdiffuser.o2r then fzerox.o2r (ArchiveManager is last-mounted-wins).
    logStep("Context::CreateUninitializedInstance");
    auto ctx = Ship::Context::CreateUninitializedInstance("G-Diffuser", "gdiffuser", "gdiffuser.cfg.json");
    if (ctx == nullptr) {
        logStep("FATAL: Context create failed");
        return 1;
    }
    ctx->InitConsoleVariables();

    std::vector<std::string> archives;
    const std::string base = gdx3ds_fs_base_path();
    archives.push_back(base + "gdiffuser.o2r");
    archives.push_back(base + "fzerox.o2r");
    logStep("InitResourceManager");
    const bool archivesValidated = ctx->InitResourceManager(archives, {}, 1, /*allowEmptyPaths=*/true) &&
                                   ctx->GetResourceManager() != nullptr && ctx->GetResourceManager()->IsLoaded();
    if (!archivesValidated) {
        logStep("WARNING: no archive mounted — copy fzerox.o2r/gdiffuser.o2r to sdmc:/3ds/gdiffuser/");
    }
    // The bottom-screen DASH's art (fonts, digits, portraits, chrome): baked ONCE from the archive
    // just mounted (gdx3ds_hudbake.h) -- no game pixels ship in the binary. A missing or wrong
    // fzerox.o2r leaves those elements blank and logs [hudbake] lines; nothing else depends on it.
    logStep("bottomhud bake");
    gdx3ds_bottomhud_bake_assets();

    // 5. Window: the lus_glue Fast3dWindow (citro3d renderer + gdx3ds_os backend);
    //    Init() constructs the interpreter against Gdx3ds_GetCitro3dRenderer().
    logStep("Fast3dWindow + InitWindow");
    auto window = std::make_shared<Fast::Fast3dWindow>(nullptr);
    ctx->InitWindow(window);
    // HOMEFIX: aptHook is LIFO (libctru apt.c: the newest cookie becomes aptFirstHook and
    // runs first). C3D_Init (inside InitWindow above) registered citro3d's hook AFTER ours,
    // so on every suspend citro3d's gxCmdQueueWait/Stop/Clear ran BEFORE our render-thread
    // join — with the render thread mid-frame (ahead mode, idle=0) that clears a queue the
    // thread is still filling and lets its C3D_FrameEnd -> gxCmdQueueRun submit GPU work
    // after libctru's "GPU idle" point (SaveVramSysArea / screen transfer / ReleaseRight).
    // Re-arm ours at the head: join first, then citro3d drains a queue nobody touches.
    // On restore the order is ours first (CPU/audio/net only, no GPU state) then citro3d's
    // dirty-all; nothing of ours depends on citro3d's flags being set first.
    if (sAptHookFirst) {
        aptUnhook(&sAptHookCookie);
        aptHook(&sAptHookCookie, aptLifecycleHook, nullptr);
    }
    {
        char msg[240]; // HOMEFIX2: 160 truncated the line at apt_dsp_hook= and ate park=
        std::snprintf(msg, sizeof(msg),
                      "[apt] hooks: ours %s citro3d's (debug.apt_hook_first=%d), suspended-mode watchdog poll=%d "
                      "(debug.apt_watch), wake event=%d, apt_join_ms=%d, apt_dsp_hook=%d park=%d",
                      sAptHookFirst ? "ahead of" : "behind", sAptHookFirst, sAptWatch, sWatchdogWake != 0 ? 1 : 0,
                      sAptJoinMs, sAptDspHook, sAptDspPark);
        logStep(msg);
    }

    // 6. Audio: ndsp backend first (audio STATUS.md R6 ordering), then the dedicated
    //    audio thread (it waits for the decomp's gAudioContextInitialized internally).
    // O3DSHOME: the tail DSP hook goes in BEFORE ndspInit so it lands behind ndsp's.
    if (sAptDspHook) {
        dspHook(&sDspTailHookCookie, dspTailHook);
    }
    logStep("gdx3ds_audio_init");
    gdx3ds_audio_init(0);
    // HOMEFIX2: AFTER ndspInit (inside gdx3ds_audio_init) so dspHook's LIFO order puts our
    // callback ahead of ndsp's — i.e. our receipt lands BEFORE ndspFinalize's unbounded DSP
    // handshake. See dspLifecycleHook above.
    if (sAptDspHook) {
        dspHook(&sDspHookCookie, dspLifecycleHook);
        logStep("[apt] dsp hook armed ahead of ndsp's (debug.apt_dsp_hook=1)");
        logStep("[apt] dsp tail hook armed behind ndsp's (ndspFinalize/ndspInitialize bracketed)");
    }
    {
        bool isNew3ds = false;
        APT_CheckNew3DS(&isNew3ds);
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "[apt] o3dshome: model=%s syscore=%d%% apt_syscore_drop=%d (acts=%d) audio_syscore=%d",
                      isNew3ds ? "new3ds" : "old3ds", gdx3ds_audio_syscore_limit_percent(), sAptSyscoreDrop,
                      (sAptSyscoreDrop && !isNew3ds) ? 1 : 0,
                      gdx3ds_config_get_bool("debug", "audio_syscore", 1) ? 1 : 0);
        logStep(msg);
    }
    {
        // AUDIOFIX: the null-sink degrade used to be invisible outside the (default-off)
        // bottom-screen console, so "runs fine but silent" never left a trace in log.txt
        // or the Azahar log. Surface the outcome in the standard boot sequence, always.
        logStep(gdx3ds_audio_output_active()
                    ? "audio output ACTIVE (ndsp)"
                    : "audio output NULL SINK — game will be SILENT (sdmc:/3ds/dspfirm.cdc "
                      "missing? Required on hardware AND on Azahar's emulated SD)");
    }
    logStep("gdx_audio_thread_start");
    gdx_audio_thread_start(argc, argv);

    // 7. Resources + game boot chain, in port/main.cpp's order.
    logStep("RegisterResourceFactories");
    GDiffuser::RegisterResourceFactories(ctx->GetResourceManager()->GetResourceLoader());
    logStep("GDiffuser_LoadAllAssets");
    GDiffuser_LoadAllAssets();

    // N60TICK4 ([debug] tick_ahead, default 0; gdx3ds_tickahead.h): mode 1 runs everything from
    // bootproc on (the frame loop, every game fiber, the teardown) on a host thread created on
    // core 1, so the fibers are converted there; the boot steps below stay on core 0.
    const int tickAheadMode = gdx3ds_tickahead_boot();
    bool schedInited = false;
    if (tickAheadMode != GDX3DS_TICKAHEAD_CORE1) {
        logStep("gdx_sched_init");
        gdx_sched_init();
        schedInited = true;
    }

    logStep("gdx_init_rom");
    {
        // Synthetic trailing ROM path (desktop first-boot parity): rom_buffer scans
        // argv in order, so a real CLI path (Citra) still wins over the SD default.
        static std::string romPath = base + "baserom.us.rev0.z64";
        std::vector<char*> romArgv(argv, argv + argc);
        romArgv.push_back(const_cast<char*>(romPath.c_str()));
        gdx_init_rom(static_cast<int>(romArgv.size()), romArgv.data(), archivesValidated ? 1 : 0);
    }

    // A10: arm the load-stall census before the boot preloads, so the boot-window spans
    // (which run on this very thread, i.e. the future main thread) are measured too.
    gdx_stall_configure(gdx3ds_config_get_int("debug", "stall", 0),
                        gdx3ds_config_get_int("debug", "stall_ms", 8),
                        gdx3ds_config_get_int("debug", "stall_frame_ms", 50));

    logStep("gdx_rdram_init");
    gdx_rdram_init();

    if (gdx_rom_buffer != nullptr) {
        gdx_register_host_range(gdx_rom_buffer, gdx_rom_size);
    }
    gdx_register_main_module_range();

    // Audio blob preload (bases from decomp/include/port_segment_addrs.h, duplicated by
    // value like main.cpp). Split in two so the pre-splash boot no longer stalls on the
    // ~10.7 MB audio_table archive read (the New3DS "halts before the splash screen on
    // loading the song data" report): the payload buffers are pre-allocated and
    // host-range-registered HERE on the main thread (the host-range vector is not
    // thread-safe), then the SD-bound archive reads run on a background worker
    // overlapping segment warm-up, bootproc and the game's splash frames. The bank/seq
    // blobs (12 KB / 3 KB) load first so the boot riff's font conversion is served
    // almost immediately; a first audio read racing the audio_table load waits inside
    // gdx_segment_source's in-flight path (audio producer thread only — the game thread
    // never reads audio families, so it never blocks on residency).
    {
        static const uint32_t kAudioBlobBases[3] = {
            0x00524D60u, // PORT_audio_bank_ROM_START  (audio_blob/audio_bank)
            0x00527AF0u, // PORT_audio_seq_ROM_START   (audio_blob/audio_seq)
            0x00528730u, // PORT_audio_table_ROM_START (audio_blob/audio_table, ~10.7 MB)
        };
        // C4d ([debug] audio_stream, default 0): arm the audio_table diet BEFORE the preload
        // list is walked, because arming it is what decides whether audio_table is preloaded
        // at all. The archive list is passed in REVERSE mount order -- ArchiveManager is
        // last-mounted-wins, so fzerox.o2r shadows gdiffuser.o2r and must be looked at first.
        //
        // audioTableCount is the whole lever on this side: 3 keeps the shipped behaviour
        // (all three blobs resident), 2 drops the 10,744,340 B one. Verify mode deliberately
        // keeps it resident as WELL as streamed -- it is the proof, not the product.
        const std::string o2rBase = gdx3ds_fs_base_path();
        const std::string o2rGame = o2rBase + "fzerox.o2r";
        const std::string o2rMod = o2rBase + "gdiffuser.o2r";
        const char* o2rPaths[2] = { o2rGame.c_str(), o2rMod.c_str() };
        // [ndsphash] ([debug] ndsphash, default 0): arm the content-addressed render digest
        // here, beside the diet it exists to gate. It must be armed BEFORE the audio producer
        // thread runs its first tick, and this block already sits on that side of the boot.
        gdx_ndsphash_init();
        // [audiomix] ([debug] audiomix, default 0): the mix-bus headroom census, armed on the
        // same side of the boot and for the same reason -- it counts every bus accumulate
        // inside the HLE, so it must exist before the producer's first tick.
        gdx_audiomix_init();
        GdxAudioDietConfig dietCfg = {};
        dietCfg.stream = gdx3ds_config_get_int("debug", "audio_stream", 0);
        dietCfg.verify = gdx3ds_config_get_int("debug", "audio_stream_verify", 0);
        dietCfg.census = gdx3ds_config_get_int("debug", "audiodiet", 0);
        // audiocache: DEFAULT 512 -> 1024. `audiodiet` sized 512 KiB off a lane sweep whose
        // own miss count said 80% of misses were irreducible first-touch; the user's hardware
        // receipt says otherwise -- `refetch=291` of `miss=495` against a `wsKB=3264` working
        // set in a 512 KiB cache, i.e. 59% of the card reads on the audio producer thread were
        // blocks the cache had already held and dropped. On the lane, 512 -> 1024 takes
        // refetch 18 -> 1, miss 90 -> 73 and hostKB 1440 -> 1168 for 512 KiB of heap, two runs
        // a side, identical to the digit. See audiocache-progress.md section 3.
        dietCfg.cacheKb = (unsigned)gdx3ds_config_get_int("debug", "audio_stream_cache_kb", 1024);
        dietCfg.blockKb = (unsigned)gdx3ds_config_get_int("debug", "audio_stream_block_kb", 16);
        // C4d/audiocache: read-ahead for the audio cache. See gdx_audio_diet.h.
        dietCfg.readAhead =
            (unsigned)gdx3ds_config_get_int("debug", "audio_stream_readahead", 0);
        sAudioDietReportFrames = gdx3ds_config_get_int("debug", "audiodiet_report", 0);
        dietCfg.bootBypass = gdx3ds_config_get_int("debug", "audio_boot_bypass", 1);
        dietCfg.bootBypassKb =
            (unsigned)gdx3ds_config_get_int("debug", "audio_boot_bypass_kb", 64);
        const bool dietStreaming = gdx_audio_diet_init(&dietCfg, o2rPaths, 2) != 0;
        const size_t audioTableCount = (dietStreaming && !dietCfg.verify) ? 2u : 3u;
        // A12b BOOT STALL ([debug] audio_boot_bypass, default 1; 0 = the unchanged path).
        // The preload worker below exists so boot does not stall on the 10.7 MB audio_table
        // archive read -- but the audio producer starts first and its font relocation asks
        // for six small DMAs out of that very blob. gdx_segment_source's in-flight wait is
        // all-or-nothing, so the producer sleeps out the whole read (measured: 1.30 s in one
        // producer tick) while holding sAudioCtxMutex, which the GAME fiber takes in
        // AudioThread_QueueCmd -- so the preload stalls the thing it was meant to unblock.
        // Arming the bypass here lets those six reads come off the .o2r by fseek instead.
        // Released by the worker once audio_table is resident; it self-disarms before that.
        const bool bootBypass =
            gdx_audio_diet_boot_bypass_arm(&dietCfg, o2rPaths, 2) != 0;
        (void)bootBypass;
        // RACE-START-POLISH: the four segment_blob families the FIRST race load touches
        // first (bases from port/gen/AssetBindings.c sSegmentBlobMap). They were the last
        // synchronous SD reads left on the race-start path — measured right at race entry:
        // hud_gfx 33 ms + machine_global_gfx 51 ms + super_textures 4 ms +
        // common_assets_compressed 676 ms (emulator; SD-card hardware pays more) on the
        // GAME thread, a visible halt on top of the venue MIO0 decode. Loaded by the same
        // background worker AFTER the audio families (the boot riff's font conversion
        // keeps first claim on the worker). Net steady-state RAM is unchanged: these
        // family payloads (~3.0 MB total) were already resident for the process lifetime
        // once a race had loaded — only WHEN they load moves.
        static const uint32_t kRaceBlobBases[4] = {
            0x001B8550u, // segment_blob/hud_gfx                  (0x29EA0)
            0x001E23F0u, // segment_blob/machine_global_gfx       (0x48CB0)
            0x002747F0u, // segment_blob/super_textures           (0x4000)
            0x002B9EA0u, // segment_blob/common_assets_compressed (0x26AA80, MIO0 inside)
        };
        for (size_t i = 0; i < audioTableCount; ++i) {
            const uint32_t baseAddr = kAudioBlobBases[i];
            void* payload = nullptr;
            uint32_t cap = 0;
            if (GdxSegmentSourcePreallocPayload(baseAddr, &payload, &cap) && payload != nullptr) {
                gdx_register_host_range(payload, cap);
            }
        }
        static size_t sAudioTableCount = audioTableCount;
        ThreadFunc preloadWorker = [](void*) {
            const double t0 = gdx_host_ms_now();
            int resident = 0;
            gdx3ds_audio_note_preload_running(1);
            for (size_t i = 0; i < sAudioTableCount; ++i) {
                if (GdxSegmentSourcePreload(kAudioBlobBases[i])) {
                    ++resident;
                }
            }
            // A12b: audio_table is resident now, so the bypass has already stopped being
            // reached (gdx_segment_source's lock-free fast path serves every read from the
            // published payload). Retract it and give back its file handle + cache.
            gdx_audio_diet_boot_bypass_release();
            // Race-path gfx families need residency only (no host-range registration:
            // their bytes are consumed through GdxSegmentSourceRead into decoded
            // per-segment images, never resolved by low32 address). A first-touch read
            // racing this preload waits inside gdx_segment_source's in-flight path.
            for (uint32_t baseAddr : kRaceBlobBases) {
                if (GdxSegmentSourcePreload(baseAddr)) {
                    ++resident;
                }
            }
            char line[96];
            int n = std::snprintf(line, sizeof(line),
                                  "[audio-blob] preload worker: %d/%d families (audio+race) "
                                  "resident in %d ms",
                                  resident, (int)(sAudioTableCount + 4u),
                                  (int)(gdx_host_ms_now() - t0));
            if (n > 0) {
                svcOutputDebugString(line, (size_t)n);
                gdx3ds_filelog_write(line, (size_t)n);
            }
            gdx3ds_audio_note_preload_running(0);
        };
        // Core ladder mirror of the audio threads (which started earlier and hold 0x18 on
        // their core, so they preempt this worker): New3DS spare core 2 -> syscore (needs
        // the app share grant) -> appcore at LOW priority (0x3D) so the boot thread keeps
        // core 0 and the worker fills its FS-wait gaps.
        // [audioprime2] (LOCKED-60 round 2): [debug] audioprime2_preload_core picks the first
        // rung. 2 (default) = this round-1 ladder; 1 = skip core 2 (syscore via the recorded
        // grant, else appcore 0x3D); 0 = appcore 0x3D only. The worker INFLATES the 10.7 MB
        // Deflate-stored audio_table, so it is CPU-bound: off core 2 it took 8.3 s instead of
        // 4.3 s in Azahar and the title font's sample load then blocked the audio producer for
        // 2.1 s -- hence the default. Old3DS has no core-2 rung either way.
        bool preloadNew3ds = false;
        APT_CheckNew3DS(&preloadNew3ds);
        const bool prime2 = gdx3ds_audio_prime2_enabled() != 0;
        const int preloadPolicy = gdx3ds_audio_prime2_preload_core();
        const bool preloadOffCore2 = prime2 && preloadPolicy != 2;
        gdx3ds_audio_set_main_phase("preload");
        Thread preloadThread = nullptr;
        int preloadCore = -1;
        if (preloadNew3ds && !preloadOffCore2) {
            preloadThread = threadCreate(preloadWorker, nullptr, 16 * 1024, 0x30, 2, true);
            preloadCore = (preloadThread != nullptr) ? 2 : -1;
        }
        if (preloadThread == nullptr && !(prime2 && preloadPolicy == 0)) {
            // O3DSHOME: both policies take the share through the audio module so
            // [debug] audio_syscore=0 keeps the worker off core 1 too, and the percent is
            // recorded for the restore hook (the direct call here never recorded it).
            const bool syscoreGranted = gdx3ds_audio_grant_syscore() != 0;
            if (syscoreGranted) {
                preloadThread = threadCreate(preloadWorker, nullptr, 16 * 1024, 0x30, 1, true);
                preloadCore = (preloadThread != nullptr) ? 1 : -1;
            }
        }
        if (preloadThread == nullptr) {
            preloadThread = threadCreate(preloadWorker, nullptr, 16 * 1024, 0x3D, 0, true);
            preloadCore = (preloadThread != nullptr) ? 0 : -1;
        }
        gdx3ds_audio_note_preload_core(preloadCore);
        if (prime2) {
            char line[96];
            std::snprintf(line, sizeof(line), "[audioprime2] preload worker on core %d (%s)",
                          preloadCore, preloadCore == 1 ? "syscore, prio 0x30"
                                       : preloadCore == 0 ? "appcore, prio 0x3D"
                                       : preloadCore == 2 ? "core 2, prio 0x30" : "none");
            logStep(line);
        }
        if (preloadThread == nullptr) {
            // No thread grantable at all: previous (synchronous) behavior.
            logStep("audio-blob preload worker unavailable; loading synchronously");
            preloadWorker(nullptr);
        }
    }

    // Segment decode warm-up (keeps course loads off the game fiber; desktop parity).
    gdx3ds_audio_set_main_phase("warm");
    gdx_boot_warm_asset_segments();

    // N60TICK4: the host body. With tick_ahead=1 it runs on the core-1 host thread and the main
    // thread serves its APT pump (gdx3ds_tickahead_apt_main_loop); otherwise it runs right here.
    // The body below is deliberately NOT re-indented (keeps merges with the frame-loop owners small).
    auto hostMain = [&]() -> int {
    if (!schedInited) {
        logStep("gdx_sched_init (tick_ahead host thread)");
        gdx_sched_init();
    }
    logStep("bootproc — starting decomp game threads");
    gdx3ds_audio_set_main_phase("bootproc");
    bootproc();
    logStep("bootproc returned; entering frame loop");
    gdx3ds_audio_set_main_phase("frames");

    // M1-RACE-FREEZE watchdog: priority 0x18 beats the 0x30 main/audio threads so the
    // heartbeat keeps logging through any guest-side spin or deadlock. Detached; runs for
    // the process lifetime (~1 svc line per 5 s).
    if (threadCreate(watchdogThreadMain, nullptr, 8 * 1024, 0x18, -2, true) == nullptr) {
        logStep("WARNING: watchdog thread creation failed");
    }

    // 8. Frame loop — the desktop non-interpolated branch, minus Gui/Discord/perf HUD.
    //    EndFrame() presents through the citro3d backend; the window backend's swap
    //    (gdx3ds_os_window_swap) flips IsRunning when an APT close order is pending.
    //    APT itself is pumped by the aptMainLoop() call at the loop TOP — between frames,
    //    with no C3D frame open — so HOME/sleep suspends and POWER-off close orders are
    //    serviced exactly once per iteration from a state the OS transition tolerates.
    // Boot-debug tracing (docs/research/m1-boot-debug.md). GDX3DS_BOOT_TRACE=1 logs every
    // frame-loop stage through svcOutputDebugString — visible in the Azahar log with
    // log_filter=*:Debug even when the screens are dark. Default: heartbeat only
    // (frames 0-7, then every 64th) so steady state stays cheap (~1 svc per second).
#ifndef GDX3DS_BOOT_TRACE
#define GDX3DS_BOOT_TRACE 0
#endif
    auto w = ctx->GetWindow();
    uint32_t frameNum = 0;
    // RENDER THREAD (docs/research/renderthread-audit.md): StartFrame / gdx_vi_present_fallback /
    // EndFrame move to core 2; osSpTaskStartGo (n64_sched.c) forks the GFX task there and the
    // loop below joins it (DP-done) once the game fibers are all blocked. rtMode 0 = the
    // sequential path below, untouched.
    static Fast::Fast3dWindow* sRtWindow = nullptr;
    sRtWindow = static_cast<Fast::Fast3dWindow*>(w.get());
    GdxRtCallbacks rtCb;
    rtCb.startFrame = [] { sRtWindow->StartFrame(); };
    rtCb.presentFallback = [] { gdx_vi_present_fallback(); };
    rtCb.endFrame = [] {
        sRtWindow->EndFrame();
        gdx3ds_oracle_after_endframe(); // ORACLE: frame N is complete once armed (FrameSync inside)
    };
    const int rtMode = gdx3ds_rt_init(&rtCb); // 0 off, 1 sync, 2 pipe, 3 ahead
    gdx3ds_frameskip_init(rtMode); // FRAMESKIP: rtMode 0 only (the receipt says when refused)
    gdx3ds_replay_init(rtMode);    // C6 [replay]: rtMode 0 + the owned interpreter (receipt says why not)
    // QUIET MODE (MENU-PERF): the recurring svc telemetry below (the every-64th "frame N"
    // heartbeat, the [present] scanout oracle, the [mem-census] line) is gated behind
    // `[debug] verbose = 1` (or `gputrace = 1`, so measurement runs need no extra key).
    // Azahar's log_filter=*:Debug processes every svc line, a real emulator-side tax in
    // normal play. Kept unconditional: frames 0-7 (boot evidence), the [watchdog]
    // heartbeat (its frame deltas are the fps measurement), one-shot boot logSteps, and
    // every error/fatal path.
    // MENU: the gate is now LIVE — gdx3ds_dbg_verbose_active() folds the boot-time
    // config values with the DBG tab's runtime toggles (verbose latch + gputrace),
    // so flipping them in the menu takes effect without a reboot.
#define verboseTelemetry (gdx3ds_dbg_verbose_active() != 0)
    // CADENCE: `[debug] cadence = 1` arms the task/hold ratio line on its own (a
    // cadence soak should not need the whole verbose family); verbose/gputrace
    // also arm it so measurement runs get it for free.
    const bool cadenceCfg = gdx3ds_config_get_int("debug", "cadence", 0) != 0;
#define cadenceTelemetry (verboseTelemetry || cadenceCfg)
    // A10: everything from here runs on THE main thread (the game fibers included: a fiber
    // shares its host thread), so spans opened past this point are frame stalls and spans on
    // the preload worker / audio producer are not. The table separates them on this identity.
    gdx_stall_set_main_thread();
    while (w != nullptr && w->IsRunning() && gdx3ds_quit_requested() == 0) {
        // A10 frame boundary: closes the previous iteration (unattributed remainder -> `other`,
        // one `[stall] frame=` line when it overran) and opens this one.
        gdx_stall_frame((unsigned long)frameNum);
        // APT pump — FIRST statement of the iteration, outside every conditional and
        // frame-skip path. HOME/POWER suspends and lid-close sleeps happen INSIDE this
        // call (the aptLifecycleHook receipts bracket them); a false return is the OS
        // close order (power-off menu, HOME-menu X, cart eject) and must break the loop
        // so main() unwinds through the ordinary teardown below — that path logs
        // "exiting" and fires the atexit tracer, which is the proof of a clean exit.
        // MAINPROF: open this iteration's timeline. Everything from here to
        // gdx3ds_mprof_frame_end below is partitioned into [mprof] buckets; the window
        // emit itself happens inside frame_end, i.e. outside every bucket.
        gdx3ds_mprof_frame_begin();
        gdx3ds_hitch_frame_begin(); // HITCH: [debug] hitch per-frame receipt (one int test when off)
        gdx3ds_spike_frame_begin(); // n60st3: [debug] spike_ms attribution (one int test when off)
        gdx3ds_crash_breadcrumbs_frame((unsigned long)frameNum); // REL1CRASH: one int test when off
        if (rtMode != 0 && (rtMode != 3 || aptShouldJumpToHome() || aptShouldClose())) {
            // Frame boundary: the previous iteration's END (present) must have completed so
            // the APT pump runs with no C3D frame open and nothing queued (audit section 6).
            // ahead mode (3) lets the previous frame's render/present overlap this iteration
            // and joins only when an APT transition is about to run (the hook joins too).
            sWatchStage = 9;
            const int mpPrev = gdx3ds_mprof_enter(GDX3DS_MPROF_JTOP);
            // HOMEFIX2: this join is ALSO on the suspend path — it only runs when a HOME or
            // close request is already visible, i.e. aptMainLoop() below is about to enter
            // the transition. Same invariant as the hook's join (see aptRenderThreadPark):
            // the render thread is the only signaller and it can be inside a GSP wait, so
            // bound it and say so rather than never reach aptMainLoop at all.
            if (sAptJoinMs > 0) {
                if (!gdx3ds_rt_join_idle_deadline((unsigned)sAptJoinMs)) {
                    aptLogReceipt("[apt] pre-pump render join TIMEOUT — entering aptMainLoop with the "
                                  "render thread still busy (the hook's own join retries)");
                }
            } else {
                gdx3ds_rt_join_idle();
            }
            gdx3ds_mprof_leave(mpPrev);
        }
        gdx3ds_mprof_enter(GDX3DS_MPROF_APT);
        if (!gdx3ds_tickahead_apt_main_loop() || gdx3ds_oracle_quit_requested()) { // N60TICK4: aptMainLoop()
            logStep("[apt] shutdown requested — leaving frame loop");
            gdx3ds_filelog_flush_sync(); // receipt must survive even if teardown wedges
            break;
        }
        if (rtMode != 0) {
            gdx3ds_mprof_enter(GDX3DS_MPROF_PACE);
            gdx3ds_rt_pace_vblank(); // main owns the vblank alignment (render thread: FrameBegin(0))
        }
        if (GDX3DS_BOOT_TRACE || frameNum < 8 || (verboseTelemetry && (frameNum & 63) == 0)) {
            char hb[48];
            int n = std::snprintf(hb, sizeof(hb), "frame %lu", (unsigned long)frameNum);
            svcOutputDebugString(hb, n);
        }
        gdx3ds_boot_trail_frame((unsigned long)frameNum); // n60boot: crumbs, then the close
        ++frameNum;
        sWatchFrame = frameNum;
#if GDX3DS_BOOT_TRACE
#define GDX_HB(tag, stage) do { svcOutputDebugString(tag, strlen(tag)); sWatchStage = (stage); } while (0)
#else
#define GDX_HB(tag, stage) (sWatchStage = (stage))
#endif
        gdx3ds_mprof_enter(GDX3DS_MPROF_INP);
        gdx_dev_gates_refresh();
        GDX_HB("hb:events", 1);
        w->HandleEvents();
        gdx_controller_poll();
        gdx_fixed_aspect_tick();
        // FRAMESKIP (gdx3ds_frameskip.h): decided AFTER the poll (a replay SHOT arms there and
        // vetoes the skip) and BEFORE vi_tick (the GFX task is submitted inside it). A skip
        // iteration runs the whole logic tick; only the render + present below are dropped.
        const bool skipRender = gdx3ds_frameskip_begin_iteration((unsigned long)frameNum) != 0;
        gdx3ds_pipe_iteration_begin(); // o3ds60: the top triple buffer this A/B block wants
        bool menuTickedEarly = false;   // o3ds60 [ui] dash_pre_frame
        gdx3ds_replay_iteration_begin((unsigned long)frameNum, skipRender ? 1 : 0); // C6: pool snapshot
        gdx3ds_c5tick_begin_tick((unsigned long)frameNum, skipRender ? 1 : 0); // C5TICK: latch the hollow-build flag (default off)

        if (rtMode != 0) {
            // BEGIN first: gdx_vi_tick's osSendMesg dispatches the game fibers from host
            // context (osStartThread with __osRunningThread == NULL), i.e. SetTask + the next
            // logic tick run INSIDE gdx_vi_tick -- the TASK must find the frame already opened
            // by Interpreter::StartFrame (dimension/fixed-aspect latches precede Run).
            GDX_HB("hb:start_frame", 4);
            gdx3ds_mprof_enter(GDX3DS_MPROF_BEG);
            gdx3ds_rt_frame_begin(); // BEGIN: StartFrame on the render thread
        }
        GDX_HB("hb:vi_tick", 2);
        // MAINPROF: gdx_vi_tick's osSendMesg dispatches the game fibers from host context
        // (renderthread-audit.md section 8), so the game's whole frame function runs INSIDE
        // this call and re-buckets itself at its own checkpoints. What stays in `vi` is the
        // host side plus the few instructions a resumed fiber runs before its next one.
        gdx3ds_mprof_enter(GDX3DS_MPROF_VI);
        gdx_vi_tick();   // runs the Main game fiber inline (posts VI retrace)
        GDX_HB("hb:audio_notify", 3);
        gdx3ds_mprof_enter(GDX3DS_MPROF_AUD);
        gdx_audio_thread_notify_frame();

        if (rtMode == 0 && !skipRender) {
            if (gdx3ds_pipe_dash_pre_frame()) {
                // o3ds60 [ui] dash_pre_frame: the bottom-screen tick where StartFrame would wait
                // for the GPU to finish the previous frame (gdx3ds_pipe.h), not after EndFrame.
                gdx3ds_mprof_enter(GDX3DS_MPROF_TAIL);
                gdx3ds_menu_tick();
                menuTickedEarly = true;
            }
            GDX_HB("hb:start_frame", 4);
            gdx3ds_mprof_enter(GDX3DS_MPROF_BEG);
            w->StartFrame();
        }
        gdx3ds_mprof_enter(GDX3DS_MPROF_DISP);
        gdx_sched_drain_deferred_wakes();
        GDX_HB("hb:dispatch", 5);
        gdx_dispatch();
        // RENDER THREAD join: the game fibers are all blocked; if a GFX task is still in
        // flight, the game is (by its own frame protocol, sys_gfx.c:212) parked on DP-done for
        // it. pipe: wait for the render thread (LightEvent, no spin), post DP-done, dispatch
        // again so the game finishes swap + SetTask inside this same iteration. ahead: post
        // the DP-done at once (the next submit is the backpressure) and dispatch again.
        while (rtMode != 0 && gdx3ds_rt_task_pending()) {
            GDX_HB("hb:rt_join", 9);
            gdx3ds_mprof_enter(GDX3DS_MPROF_JDP);
            gdx_stall_begin(GDX_STALL_GPU_WAIT); // A10: main blocked on the render thread
            gdx3ds_rt_wait_task(); // waits, merges segment claims, posts the DP-done once
            gdx_stall_end();
            gdx3ds_mprof_enter(GDX3DS_MPROF_DISP);
            gdx_sched_drain_deferred_wakes();
            GDX_HB("hb:dispatch", 5);
            gdx_dispatch();
        }
        gdx3ds_frameskip_end_iteration((unsigned long)frameNum); // FRAMESKIP: disarm the task drop
        GDX_HB("hb:save_tick", 6);
        gdx3ds_mprof_enter(GDX3DS_MPROF_TAIL);
        gdx3ds_spike_mark(GDX_SPIKE_MARK_SAVE0);
        gdx_disk_save_tick();
        gdx3ds_spike_mark(GDX_SPIKE_MARK_SAVE1);
        // ORACLE: stamp this frame's (frame, tick, mode) before its EndFrame is issued.
        gdx3ds_oracle_frame_mark((unsigned long)frameNum, (unsigned long)gdx_input_tick() - 1u, gdx_input_game_mode());
        if (rtMode != 0) {
            GDX_HB("hb:end_frame", 7);
            gdx3ds_mprof_enter(GDX3DS_MPROF_END);
            gdx3ds_rt_frame_end(); // END: present fallback + EndFrame on the render thread
        } else if (skipRender) {
            gdx3ds_oracle_skip_frame(); // FRAMESKIP: nothing presented; keep the oracle's frame count
        } else {
            gdx3ds_mprof_enter(GDX3DS_MPROF_END);
            gdx_vi_present_fallback();
            gdx3ds_frameskip_before_present(); // FRAMESKIP steady: hold to the slot's vblank
            gdx3ds_replay_before_present();    // C6: a replayed frame's swap latches first
            GDX_HB("hb:end_frame", 7);
            w->EndFrame();
            gdx3ds_frameskip_after_present(); // o3ds60 [steadyd1] fe=: EndFrame's share of post
            // C5 STAGE 0: the oracle's SHOT/[fh] readback is harness work, not the frame's end
            // ([c5cat] fend); charge it to tail with the other telemetry.
            gdx3ds_mprof_enter(GDX3DS_MPROF_TAIL);
            gdx3ds_oracle_after_endframe(); // ORACLE (rtMode 0 twin of the render-thread hook)
        }
        gdx3ds_mprof_enter(GDX3DS_MPROF_TAIL);
        gdx3ds_spike_mark(GDX_SPIKE_MARK_POST0);
        gdx3ds_replay_iteration_end((unsigned long)frameNum, skipRender ? 1 : 0); // C6: B's poses / A's signals
        gdx3ds_core1probe_tick((unsigned long)frameNum); // N60TICK: [debug] core1_probe (one int test off)
        gdx3ds_tickahead_frame_end((unsigned long)frameNum); // N60TICK4: [tickahead] window (one int test off)
        gdx3ds_spike_mark(GDX_SPIKE_MARK_POST1);

        // Present-path oracle (m1-boot-debug): the top-screen framebuffers live on the
        // linear heap (gfxInitDefault), so the CPU can inspect what actually reaches
        // scanout. gfxGetFramebuffer returns the current BACK buffer, i.e. the buffer
        // citro3d's frame N-1 display transfer landed in before its swap — any non-zero
        // byte here proves GPU content is reaching the screen buffers. Bounded to the
        // heartbeat cadence (~1 svc line/sec).
        if (verboseTelemetry && frameNum > 8 && (frameNum & 63) == 1) {
            u16 fbW = 0;
            u16 fbH = 0;
            const u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
            if (fb != nullptr) {
                const size_t fbBytes = (size_t)fbW * fbH * 3; // BGR8
                size_t firstNz = fbBytes;
                size_t nzCount = 0;
                for (size_t i = 0; i < fbBytes; i++) {
                    if (fb[i] != 0) {
                        nzCount++;
                        if (firstNz == fbBytes) {
                            firstNz = i;
                        }
                    }
                }
                // Center of the portrait 240x400 buffer = center of the landscape screen.
                const size_t centerOff = (((size_t)fbH / 2) * fbW + fbW / 2) * 3;
                char msg[144];
                int n = std::snprintf(msg, sizeof(msg),
                                      "[present] frame=%lu topFb=%p %ux%u nz=%lu/%lu firstOff=%ld center=%02x%02x%02x",
                                      (unsigned long)frameNum, (const void*)fb, (unsigned)fbW, (unsigned)fbH,
                                      (unsigned long)nzCount, (unsigned long)fbBytes,
                                      firstNz < fbBytes ? (long)firstNz : -1L,
                                      fb[centerOff], fb[centerOff + 1], fb[centerOff + 2]);
                if (n > 0) {
                    svcOutputDebugString(msg, n);
                }
            }
        }

        // M1-MEMORY census (~every 10 s at 26 fps): attribute heap growth to the owning
        // container instead of guessing from mallinfo deltas. seg/tex/wide/setup are the
        // bridge's cross-frame containers (gdx_gfx_mem_census), lus is the ResourceManager
        // cache (byte-accounted by the 3DS cap patch), big is the cumulative >=256 KB C++
        // allocation counters, lin is linearSpaceFree. Main-thread only. Periodic under
        // verbose; also fired by HEAP-WATCH when the watchdog sees the heap cross a line.
        // O3DS BUDGET: a budget run wants the census cadence without buying the whole
        // verbose telemetry set, so [debug] membudget=1 arms it on its own.
        if ((verboseTelemetry || gdx3ds_membudget_enabled()) && frameNum > 8 &&
            (frameNum & 255) == 2) {
            emitMemCensus(ctx.get(), (unsigned long)frameNum, "periodic");
        }
        // C4d/audiocache: [debug] audiodiet_report = FRAMES between [audio-diet] receipts,
        // 0 = off (the receipt then rides the 256-frame mem-census cadence above and nothing
        // here runs). 256 frames is ~10 s, which is coarser than the events that matter: the
        // race-start announcement is a ~2 s burst of first-touch sample reads, and on a card
        // every one of those is a seek on the audio producer thread. This makes that burst
        // one grep instead of an inference.
        //
        // Deliberately emitted HERE, on the game thread, and not from the census hook inside
        // gdx_audio_diet.cpp: gdx_port_logf writes the sdmc filelog, and doing that from the
        // audio producer thread would add SD writes to the exact thread whose stalls are
        // being measured -- an instrument that causes what it reports.
        if (sAudioDietReportFrames > 0 && frameNum > 8 &&
            (frameNum % (unsigned long long)sAudioDietReportFrames) == 3) {
            char am[64];
            const int an = std::snprintf(am, sizeof(am), "[audio-diet] at frame=%lu",
                                         (unsigned long)frameNum);
            if (an > 0) {
                svcOutputDebugString(am, an);
                gdx3ds_filelog_write(am, (size_t)an);
            }
            gdx_audio_diet_receipt();
        }
        // HWLINK: execute at most one pending control command on the MAIN thread (the net
        // thread only parses and posts). Idle cost is one file-static load.
        gdx3ds_net_tick((unsigned long)frameNum);
        if (sHeapTriggerPending && frameNum > 8) {
            sHeapTriggerPending = 0;
            sHeapTriggers = sHeapTriggers + 1;
            {
                struct mallinfo mi = mallinfo();
                char hm[160];
                int hn = std::snprintf(hm, sizeof(hm),
                                       "[heap-watch] trigger=%d frame=%lu used=%lu base=%lu hwm=%lu next=%lu",
                                       (int)sHeapTriggers, (unsigned long)frameNum,
                                       (unsigned long)mi.uordblks, (unsigned long)sHeapBaseline,
                                       (unsigned long)sHeapHwm, (unsigned long)sHeapNextTrigger);
                if (hn > 0) {
                    svcOutputDebugString(hm, hn);
                    gdx3ds_filelog_write(hm, (size_t)hn);
                }
            }
            emitMemCensus(ctx.get(), (unsigned long)frameNum, "heap-watch");
            if (sMallocHistogramEnabled) {
                dumpLiveHistogram((unsigned long)frameNum);
            } else if (sHeapWatchArm) {
                sMallocHistogramEnabled = 1;
                logStep("[heap-watch] live allocation histogram + RA sampler ARMED (levels count from now)");
            }
            gdx3ds_filelog_flush();
        }
        // CADENCE ratio line (~every 64 host frames, 1 svc line per ~2-3 s): the
        // direct measurement of "does the game deliver a gfx task every host
        // present?". Read it as per-window deltas over host=64 presents:
        //   task+hold == 64 (every present is classified exactly once);
        //   task ~= 64 and hold ~= 0  -> cadence is 1:1, the fps ceiling is frame
        //     WALL time (see [gpu] wVbl), not tick alternation;
        //   task ~= hold ~= 32        -> the game really does tick every other
        //     present; game= names the logic-loop rate and yield= how often the
        //     game fiber bounced off osViGet*Framebuffer waits in the window.
        // gfxrun can exceed task (several tasks in one present during
        // transitions). dtMs is the window wall time, so hostFps = 64000/dtMs
        // and gameFps = game * 1000/dtMs.
        // A10: a windowed `[stall-table]` every 256 frames, reset each time, so the table can
        // be read per phase (boot / menu / race) instead of as one run-long total that hides
        // where the blocked milliseconds actually landed.
        if (frameNum > 8 && (frameNum & 255) == 4) {
            char stallTag[32];
            std::snprintf(stallTag, sizeof(stallTag), "frames<=%lu", (unsigned long)frameNum);
            gdx_stall_report(stallTag, 1);
        }

        if (cadenceTelemetry && frameNum > 8 && (frameNum & 63) == 3) {
            static uint64_t sCadPrevTick = 0;
            static unsigned long sCadPrevTasks = 0;
            static unsigned long sCadPrevTaskFrames = 0;
            static unsigned long sCadPrevHoldFrames = 0;
            static unsigned long sCadPrevYields = 0;
            static uint32_t sCadPrevGame = 0;
            const uint64_t nowTick = svcGetSystemTick();
            if (sCadPrevTick != 0) {
                const unsigned long dtMs =
                    (unsigned long)((nowTick - sCadPrevTick) / (uint64_t)CPU_TICKS_PER_MSEC);
                char msg[176];
                int n = std::snprintf(
                    msg, sizeof(msg),
                    "[cadence] frame=%lu dtMs=%lu host=64 game=%lu task=%lu hold=%lu "
                    "gfxrun=%lu yield=%lu",
                    (unsigned long)frameNum, dtMs,
                    (unsigned long)(gGameFrameCount - sCadPrevGame),
                    gdx_cadence_task_frames - sCadPrevTaskFrames,
                    gdx_cadence_hold_frames - sCadPrevHoldFrames,
                    gdx_cadence_gfx_tasks - sCadPrevTasks,
                    gdx_yield_count - sCadPrevYields);
                if (n > 0) {
                    svcOutputDebugString(msg, n);
                }
            }
            sCadPrevTick = nowTick;
            sCadPrevTasks = gdx_cadence_gfx_tasks;
            sCadPrevTaskFrames = gdx_cadence_task_frames;
            sCadPrevHoldFrames = gdx_cadence_hold_frames;
            sCadPrevYields = gdx_yield_count;
            sCadPrevGame = gGameFrameCount;
        }
        if (rtMode != 0 && verboseTelemetry && frameNum > 8 && (frameNum & 63) == 5) {
            gdx3ds_rt_emit_receipt((unsigned long)frameNum); // [rt] window receipt
        }
        if (frameNum > 8 && (frameNum & 255) == 2) {
            // S2: opt-in allocation-size histogram, census cadence (no-op unless armed).
            // Deliberately OUTSIDE the verbose gate: it has its own arming key
            // ([debug] malloc_histogram / GDX3DS_MALLOC_HISTOGRAM), which must keep
            // working without also setting verbose.
            dumpMallocHistogram((unsigned long)frameNum);
            dumpLiveHistogram((unsigned long)frameNum);
        }

        // FPS-HUD tick (main thread — the libctru console is not thread-safe): one
        // int test per frame when off, ~1 Hz repaint when on. gGameFrameCount is the
        // decomp's own logic-frame counter (same source as the [cadence] game= column).
        gdx3ds_spike_mark(GDX_SPIKE_MARK_TELE1);
        // card-4b: an attract demo armed on the title's way out stays set on the main menu (the
        // bottom screen then hides Home and drops the VS icon's MULTIPLAYER). decomp_port.c.
        if (int stale = gdx_title_demo_clear_stale()) {
            char line[96];
            std::snprintf(line, sizeof(line), "[title] stale attract-demo state %d on the main menu cleared", stale);
            logStep(line);
        }
        gdx3ds_fps_hud_tick(gGameFrameCount);
        // MENU tick (same thread for the same reason): touch dispatch + repaint only
        // on change / ~1 Hz for live pages — no per-frame console traffic.
        if (!menuTickedEarly) {
            gdx3ds_menu_tick();
        }
        gdx3ds_spike_mark(GDX_SPIKE_MARK_HUD1);
        gstateTick((unsigned long)frameNum); // GSTATE: [debug] gstate_every (default off)
        gdx3ds_snapshot_frame_end((unsigned long)frameNum, rtMode); // mp-snapshot: [debug] snap_test / snap_probe (default off)
        gdx3ds_mp_after_tick();              // mp-spike: digest exchange ([net] mp, default off)

        GDX_HB("hb:pacer", 8);
        gdx_frame_pacer_tick();
        // HITCH: score this iteration (reads its mprof buckets, so before frame_end folds them).
        gdx3ds_hitch_frame_end((unsigned long)frameNum, skipRender ? 0 : 1);
        gdx3ds_spike_frame_end((unsigned long)frameNum);
        // MAINPROF: close the frame, fold it into the window, and (on a window boundary)
        // emit the [mprof] receipt. Deliberately the last statement of the iteration: the
        // emit's own svc/filelog cost then lands in no bucket and cannot inflate the table,
        // and it can only ever happen at a window boundary on the host thread — never
        // inside a game fiber, so the replay's tick order is untouched.
        gdx3ds_c5tick_end_tick((unsigned long)frameNum); // C5TICK: [c5dlb] / [c5tick] windows (default off)
        gdx3ds_mprof_frame_end((unsigned long)frameNum);
        gdx3ds_cpucut_frame_end((unsigned long)frameNum); // o3ds-cpucuts: A/B block clock (mprof off)
        gdx3ds_hitch_after_frame((unsigned long)frameNum); // HITCH: buffered lines out, between iterations
        gdx3ds_spike_after_frame((unsigned long)frameNum); // n60st3: buffered [spike] lines out
        gdx3ds_filelog_frame_end((unsigned long)frameNum); // N60LOG: the [debug] receipt_every clock
#undef GDX_HB
    }
#undef verboseTelemetry
#undef cadenceTelemetry

    sWatchdogQuiet = 1; // detached 0x18 thread: no beats through (or after) teardown
    gdx3ds_crash_note_exit(); // CRASHFIX: a teardown fault ends quietly once its report is written
    logStep("exiting");
    // Close-from-HOME hang fix (docs/research/home-crash-audit.md): every teardown step is
    // bracketed by a FLUSHED receipt (svc + filelog + flush, no printf) so a wedge names
    // its step in sdmc log.txt. The first line records the cause: a Close order delivered
    // while suspended (HOME menu Close / power menu "Close software") wakes us with
    // APTCMD_WAKEUP_CANCEL — no ONRESTORE, the audio gate still up, the DSP cancelled
    // (asleep) and GSP rights released — and the teardown below runs in exactly that
    // state. libctru is built for it: ndspExit() skips ndspFinalize when the DSP was
    // cancelled and gfxExit() skips the vblank wait / LCD-black IPC without GPU rights.
    {
        char cause[112];
        std::snprintf(cause, sizeof(cause), "[exit] cause=%s suspended=%d",
                      aptShouldClose() ? "apt close order" : "app quit",
                      gdx3ds_audio_suspended());
        aptLogReceipt(cause);
    }
    gdx_disk_save_flush();
    aptLogReceipt("[exit] disk_save_flush done");
    // N60SAVE: [debug] sram_async -- anything the writer has not written yet goes out now, after at
    // most kSramExitWaitMs for an in-flight write (no-op with the lever off: writes are synchronous).
    gdx_sram_flush_pending(kSramExitWaitMs, "exit");
    aptLogReceipt("[exit] sram_flush done");
    gdx3ds_core1probe_shutdown(); // N60TICK: join the probe worker before the audio threads go
    aptLogReceipt("[exit] audio_thread_stop enter");
    gdx_audio_thread_stop(); // joins the HLE producer (its own park event; never wakes the drain)
    aptLogReceipt("[exit] audio_thread_stop done");
    aptLogReceipt("[exit] audio_shutdown enter");
    gdx3ds_audio_shutdown(); // joins the drain, then ndspExit (cancel-aware when suspended)
    aptLogReceipt("[exit] audio_shutdown done");
    aptLogReceipt("[exit] render_thread_join enter");
    gdx3ds_cov_finish();  // COVERAGE: the per-key summary, before anything can wedge the join
    gdx3ds_filelog_flush_sync();
    gdx_input_rec_stop(); // ORACLE: close a .gdxrec recording (flush) before teardown
    gdx3ds_rt_shutdown(); // QUIT + threadJoin on its own event (no shared-event spin)
    aptLogReceipt("[exit] render_thread_join done");
    aptLogReceipt("[exit] window_shutdown enter");
    gdx3ds_os_window_shutdown(); // gfxExit (rights-aware when suspended)
    aptLogReceipt("[exit] window_shutdown done");
    gdx3ds_fs_shutdown();
    aptLogReceipt("[exit] fs_shutdown done");
    // HWLINK: last drain of the log ring to the host, then both sockets + socExit.
    gdx3ds_net_shutdown();
    aptLogReceipt("[exit] net_shutdown done");
    // Final flush: "exiting" and everything teardown logged must be on the SD card
    // before crt0/__appExit — a clean close then shows "[apt] exit hook ..." from the
    // ONEXIT hook inside aptExit() as the very last line.
    gdx3ds_filelog_flush_sync();
    // HWLINK: a RELOAD staged a new .3dsx and asked for a self-relaunch. Strictly last —
    // after socExit and the final flush — so hb:ldr sees a process that holds nothing.
    // No-op unless RELOAD ran; degrades to a plain exit when hb:ldr is absent.
    gdx3ds_net_relaunch_if_requested();
    return 0;
    }; // N60TICK4: end of hostMain
    using HostMainFn = decltype(hostMain);
    return gdx3ds_tickahead_run_host([](void* p) -> int { return (*static_cast<HostMainFn*>(p))(); }, &hostMain);
}

#else // !__3DS__ — host stub build (Phase 0 behavior retained)

#include "gdx3ds_os.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    int width = 0;
    int height = 0;
    if (gdx3ds_os_window_init(&width, &height) != 0) {
        return 1;
    }

    Gdx3dsPadState pad = {};
    for (;;) {
        gdx3ds_os_poll_input(&pad, 1);
        if (gdx3ds_os_window_swap() != 0) {
            break; // APT close request (or START mapped by the stub backend)
        }
    }

    gdx3ds_os_window_shutdown();
    return 0;
}

#endif // __3DS__
