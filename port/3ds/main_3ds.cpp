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

#include "gdx3ds_audio.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h" // H-HARDWARE: SD sink for the tracers (svc goes nowhere on hardware)
#include "gdx3ds_fps_hud.h" // FPS-HUD: bottom-screen counter (debug.fps, default ON)
#include "gdx3ds_menu.h"    // MENU: v1 bottom-screen touch menu ([menu] enabled, default ON)
#include "gdx3ds_fs.h"
#include "gdx3ds_os.h"
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
void gdx_fixed_aspect_tick(void);
void gdx_rdram_init(void);
void gdx_register_host_range(void* ptr, size_t size);
void gdx_register_main_module_range(void);
void gdx_disk_save_tick(void);
void gdx_disk_save_flush(void);
int  GdxSegmentSourcePreload(uint32_t romBase);
int  GdxSegmentSourcePayload(uint32_t romBase, void** outPayload, uint32_t* outSize);
int  GdxSegmentSourcePreallocPayload(uint32_t romBase, void** outPayload, uint32_t* outCap);
double gdx_host_ms_now(void);
void gdx_boot_warm_asset_segments(void);
// [audioprime2] (port/3ds/audio/gdx3ds_audio_ndsp.c diagnostic exports): preload ladder
// off core 2 + the boot-phase / preload thread map the under-edge receipt prints.
int  gdx3ds_audio_prime2_enabled(void);
int  gdx3ds_audio_prime2_preload_core(void);
int  gdx3ds_audio_grant_syscore(void);
void gdx3ds_audio_set_main_phase(const char* tag);
void gdx3ds_audio_note_preload_core(int core);
void gdx3ds_audio_note_preload_running(int running);
void gdx_gfx_mem_census(unsigned long out[8]); // M1-MEMORY census (n64_gfx_bridge.cpp)
int  gdx3ds_quit_requested(void);    // lus_glue/gdx3ds_input_glue.c (script QUIT hook)

// AUDIOFIX: ndsp-vs-null-sink outcome for the always-on boot receipt below (diagnostic
// export from port/3ds/audio/gdx3ds_audio_ndsp.c, not part of the frozen contract).
int gdx3ds_audio_output_active(void);

// M1-RACE-FREEZE watchdog counters (port/n64_sched.c, port/n64_audio_hle.c).
extern volatile uint32_t gdx_watch_audio_tick_enter;
extern volatile uint32_t gdx_watch_audio_tick_exit;
extern volatile int      gdx_watch_audio_stage;
extern volatile uint32_t gdx_watch_specwait;
extern volatile uint32_t gdx_watch_hle_runs;
extern volatile uint32_t gdx_watch_hle_op;
extern volatile uint32_t gdx_watch_hle_idx;
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

static void watchdogThreadMain(void*) {
    uint32_t lastFrame = 0;
    uint32_t beat = 0;
    for (;;) {
        svcSleepThread(5000000000LL); // 5 s
        if (sWatchdogQuiet) {
            continue;
        }
        const uint32_t frame = sWatchFrame;
        struct mallinfo mi = mallinfo();
        char msg[224];
        int n = std::snprintf(msg, sizeof(msg),
                              "[watchdog] beat=%lu frame=%lu(+%lu) stage=%d fiber=%d "
                              "aud=%lu/%lu astage=%d spec=%lu hle=%lu op=%02lX idx=%lu "
                              "heapUsed=%lu heapFree=%lu",
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
                              (unsigned long)mi.fordblks);
        if (n > 0) {
            svcOutputDebugString(msg, n);
            gdx3ds_filelog_write(msg, (size_t)n); // sink is thread-safe (recursive lock)
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
                gdx3ds_filelog_write(ttmsg, (size_t)tn);
            }
        }
        lastFrame = frame;
    }
}

static void logStep(const char* s) {
    // Console (if enabled) AND the emulator/Luma debug channel — the svc line shows
    // up in Azahar's log even when both screens are dark, which is exactly when a
    // boot trace matters most.
    std::printf("[G-Diffuser-3DS] %s\n", s);
    svcOutputDebugString(s, strlen(s));
    gdx3ds_filelog_write(s, strlen(s)); // hardware fallback: sdmc log (debug.filelog=1)
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
    gdx3ds_filelog_flush();
}

// HOME-crash fix (docs/research/home-crash-audit.md, "## Fix"): everything the suspend
// transition needs from us, in the order libctru requires. Runs on the main thread inside
// aptJumpToHomeMenu / the sleep handler, BEFORE aptDspSleep -> ndspFinalize (suspend) and
// AFTER aptDspWakeup -> ndspInitialize (restore). svc + filelog receipts only.
static void aptAudioSuspend(void) {
    int drainParked = 0;
    int producerParked = 0;
    gdx3ds_audio_suspend(&drainParked, &producerParked); // waits (<= ~50 ms) for both parks
    char msg[96];
    std::snprintf(msg, sizeof(msg), "[apt] audio parked (drain=%d hle=%d)", drainParked,
                  producerParked);
    aptLogReceipt(msg);
}

// RENDER THREAD: the APT pump runs at the loop top AFTER the join, so the render thread is
// parked on its command semaphore with no C3D frame open by construction. The receipt
// proves it; if a job were ever in flight here (a future reordering), wait for it before
// libctru saves the VRAM sys area / releases the GPU right (bounded by one frame).
static void aptRenderThreadPark(void) {
    if (gdx3ds_rt_mode() == 0) {
        return;
    }
    const int idle = gdx3ds_rt_idle();
    if (!idle) {
        gdx3ds_rt_join_idle();
    }
    char msg[64];
    std::snprintf(msg, sizeof(msg), "[apt] render thread parked (idle=%d)", idle);
    aptLogReceipt(msg);
}

static void aptAudioResume(void) {
    gdx3ds_audio_resume();
    aptLogReceipt("[apt] audio resumed");
}

// Cause #4: the HOME menu draws on the bottom screen; if the touch menu's [OFF] holds the
// backlight down, hand it back lit and re-apply the saved state on the way back.
static void aptBacklightForHome(void) {
    if (gdx3ds_menu_backlight_off()) {
        gdx3ds_menu_force_backlight(1);
        aptLogReceipt("[apt] bottom backlight on for HOME menu (menu screen-off saved)");
    }
}

static void aptBacklightRestore(void) {
    if (gdx3ds_menu_backlight_off()) {
        gdx3ds_menu_force_backlight(0);
        aptLogReceipt("[apt] bottom backlight off re-applied (menu screen-off)");
    }
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
    if (pct > 0) {
        rc = APT_SetAppCpuTimeLimit((u32)pct);
    }
    char msg[112];
    std::snprintf(msg, sizeof(msg), "[apt] cpu state re-applied (speedup=%d syscore=%d%% rc=0x%08lX)",
                  isNew3ds ? 1 : 0, pct, (unsigned long)rc);
    aptLogReceipt(msg);
}

static void aptLifecycleHook(APT_HookType hook, void* param) {
    (void)param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
            aptLogReceipt("[apt] suspend (HOME/POWER -> HOME menu)");
            aptRenderThreadPark();
            aptAudioSuspend();
            aptBacklightForHome();
            break;
        case APTHOOK_ONRESTORE:
            aptLogReceipt("[apt] restore (resumed from HOME menu)");
            aptRestoreCpuState();
            aptAudioResume();
            aptBacklightRestore();
            break;
        case APTHOOK_ONSLEEP:
            aptLogReceipt("[apt] sleep (lid closed)");
            aptRenderThreadPark();
            aptAudioSuspend();
            break;
        case APTHOOK_ONWAKEUP:
            aptLogReceipt("[apt] wakeup (lid opened)");
            aptRestoreCpuState();
            aptAudioResume();
            aptBacklightRestore();
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
    gdx3ds_filelog_flush();
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

static inline void tallyLiveAlloc(void* p, void* ra) {
    if (!sMallocHistogramEnabled || p == nullptr) {
        return;
    }
    size_t usable = malloc_usable_size(p);
    int b = gdxMallocSizeBucket(usable);
    gLiveHistCount[b] += 1;
    gLiveHistBytes[b] += (unsigned long)usable;
    if (usable > 128u && usable <= 1024u) {
        static volatile unsigned long sRaTick = 0;
        sRaTick = sRaTick + 1;
        if ((sRaTick & 511u) == 0u) {
            static volatile int sRaLogs = 0;
            if (sRaLogs < 512) {
                sRaLogs = sRaLogs + 1;
                char msg[64];
                int n = std::snprintf(msg, sizeof(msg), "[live-ra] size=%lu ra=%p",
                                      (unsigned long)usable, ra);
                if (n > 0) {
                    svcOutputDebugString(msg, n);
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
static void dumpLiveHistogram(unsigned long frameNum) {
    if (!sMallocHistogramEnabled) {
        return;
    }
    for (int b = 0; b < GDX_MALLOC_HIST_BUCKETS; b++) {
        unsigned long count = gLiveHistCount[b];
        if (count == 0) {
            continue;
        }
        unsigned long upper = (b == 0) ? 1ul : (1ul << b);
        char msg[96];
        int n = std::snprintf(msg, sizeof(msg),
                              "[live-hist] frame=%lu class<=%luB live=%lu bytes=%lu",
                              frameNum, upper, count, gLiveHistBytes[b]);
        if (n > 0) {
            svcOutputDebugString(msg, n);
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

int main(int argc, char** argv) {
    svcOutputDebugString("main entered", 12);
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
    gdx3ds_config_load(GDX3DS_CONFIG_DEFAULT_PATH);

    // S2 MALLOC-CRAWL: arm the allocation-size histogram if requested. env wins over the INI key so
    // a run can enable it without editing sdmc:/3ds/gdiffuser/gdiffuser.ini ([debug] section).
    {
        const char* env = std::getenv("GDX3DS_MALLOC_HISTOGRAM");
        int enabled = (env != nullptr) ? (env[0] != '0' && env[0] != '\0')
                                       : gdx3ds_config_get_bool("debug", "malloc_histogram", 0);
        sMallocHistogramEnabled = enabled;
        if (enabled) {
            logStep("malloc histogram ENABLED (dumps to [malloc-hist] on the mem-census cadence)");
        }
    }

    // File-log sink right after config (sdmc: is mounted by libctru's __appInit, so the
    // INI read above already proved the device works). Earlier logStep lines only reach
    // svc/console; everything from here lands in sdmc:/3ds/gdiffuser/log.txt when
    // debug.filelog=1.
    gdx3ds_filelog_init();
    // APT lifecycle receipts (see aptLifecycleHook above): registered as early as the
    // filelog allows, so even a HOME/POWER press during boot leaves its trace.
    aptHook(&sAptHookCookie, aptLifecycleHook, nullptr);
    logStep("gdx3ds_os_window_init");
    int width = 0;
    int height = 0;
    if (gdx3ds_os_window_init(&width, &height) != 0) {
        return 1;
    }
    // FPS-HUD (debug.fps, default ON): owns the bottom-screen console + its reserved
    // top rows. Before the first post-window logStep so every console line scrolls in
    // the HUD's log window from the start.
    gdx3ds_fps_hud_init();
    // MENU (menu.enabled, default ON): takes over the bottom-screen layout (tab bar +
    // pages, fps line pinned as row 1); the HUD detects it and hands its line over.
    gdx3ds_menu_init();
    logStep("window up; gdx_dev_gates_init_env");

    // 2. Developer gates (desktop parity has these first; safe after window).
    gdx_dev_gates_init_env();

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

    // 5. Window: the lus_glue Fast3dWindow (citro3d renderer + gdx3ds_os backend);
    //    Init() constructs the interpreter against Gdx3ds_GetCitro3dRenderer().
    logStep("Fast3dWindow + InitWindow");
    auto window = std::make_shared<Fast::Fast3dWindow>(nullptr);
    ctx->InitWindow(window);

    // 6. Audio: ndsp backend first (audio STATUS.md R6 ordering), then the dedicated
    //    audio thread (it waits for the decomp's gAudioContextInitialized internally).
    logStep("gdx3ds_audio_init");
    gdx3ds_audio_init(0);
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

    logStep("gdx_sched_init");
    gdx_sched_init();

    logStep("gdx_init_rom");
    {
        // Synthetic trailing ROM path (desktop first-boot parity): rom_buffer scans
        // argv in order, so a real CLI path (Citra) still wins over the SD default.
        static std::string romPath = base + "baserom.us.rev0.z64";
        std::vector<char*> romArgv(argv, argv + argc);
        romArgv.push_back(const_cast<char*>(romPath.c_str()));
        gdx_init_rom(static_cast<int>(romArgv.size()), romArgv.data(), archivesValidated ? 1 : 0);
    }

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
        for (uint32_t baseAddr : kAudioBlobBases) {
            void* payload = nullptr;
            uint32_t cap = 0;
            if (GdxSegmentSourcePreallocPayload(baseAddr, &payload, &cap) && payload != nullptr) {
                gdx_register_host_range(payload, cap);
            }
        }
        ThreadFunc preloadWorker = [](void*) {
            const double t0 = gdx_host_ms_now();
            int resident = 0;
            gdx3ds_audio_note_preload_running(1);
            for (uint32_t baseAddr : kAudioBlobBases) {
                if (GdxSegmentSourcePreload(baseAddr)) {
                    ++resident;
                }
            }
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
                                  "[audio-blob] preload worker: %d/7 families (audio+race) "
                                  "resident in %d ms",
                                  resident, (int)(gdx_host_ms_now() - t0));
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
            const bool syscoreGranted = preloadOffCore2 ? (gdx3ds_audio_grant_syscore() != 0)
                                                        : R_SUCCEEDED(APT_SetAppCpuTimeLimit(30));
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
    rtCb.endFrame = [] { sRtWindow->EndFrame(); };
    const int rtMode = gdx3ds_rt_init(&rtCb);
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
    while (w != nullptr && w->IsRunning() && gdx3ds_quit_requested() == 0) {
        // APT pump — FIRST statement of the iteration, outside every conditional and
        // frame-skip path. HOME/POWER suspends and lid-close sleeps happen INSIDE this
        // call (the aptLifecycleHook receipts bracket them); a false return is the OS
        // close order (power-off menu, HOME-menu X, cart eject) and must break the loop
        // so main() unwinds through the ordinary teardown below — that path logs
        // "exiting" and fires the atexit tracer, which is the proof of a clean exit.
        if (rtMode != 0) {
            // Frame boundary: the previous iteration's END (present) must have completed so
            // the APT pump runs with no C3D frame open and nothing queued (audit section 6).
            sWatchStage = 9;
            gdx3ds_rt_join_idle();
        }
        if (!aptMainLoop()) {
            logStep("[apt] shutdown requested — leaving frame loop");
            gdx3ds_filelog_flush(); // receipt must survive even if teardown wedges
            break;
        }
        if (rtMode != 0) {
            gdx3ds_rt_pace_vblank(); // main owns the vblank alignment (render thread: FrameBegin(0))
        }
        if (GDX3DS_BOOT_TRACE || frameNum < 8 || (verboseTelemetry && (frameNum & 63) == 0)) {
            char hb[48];
            int n = std::snprintf(hb, sizeof(hb), "frame %lu", (unsigned long)frameNum);
            svcOutputDebugString(hb, n);
        }
        ++frameNum;
        sWatchFrame = frameNum;
#if GDX3DS_BOOT_TRACE
#define GDX_HB(tag, stage) do { svcOutputDebugString(tag, strlen(tag)); sWatchStage = (stage); } while (0)
#else
#define GDX_HB(tag, stage) (sWatchStage = (stage))
#endif
        gdx_dev_gates_refresh();
        GDX_HB("hb:events", 1);
        w->HandleEvents();
        gdx_controller_poll();
        gdx_fixed_aspect_tick();

        GDX_HB("hb:vi_tick", 2);
        gdx_vi_tick();   // runs the Main game fiber inline (posts VI retrace)
        GDX_HB("hb:audio_notify", 3);
        gdx_audio_thread_notify_frame();

        GDX_HB("hb:start_frame", 4);
        if (rtMode != 0) {
            gdx3ds_rt_frame_begin(); // BEGIN: StartFrame on the render thread
        } else {
            w->StartFrame();
        }
        gdx_sched_drain_deferred_wakes();
        GDX_HB("hb:dispatch", 5);
        gdx_dispatch();
        // RENDER THREAD join: the game fibers are all blocked; if a GFX task is still in
        // flight, the game is (by its own frame protocol, sys_gfx.c:212) parked on DP-done for
        // it. Wait for the render thread (LightEvent, no spin), post DP-done, and dispatch
        // again so the game finishes swap + SetTask inside this same iteration.
        while (rtMode != 0 && gdx3ds_rt_task_pending()) {
            GDX_HB("hb:rt_join", 9);
            gdx3ds_rt_wait_task(); // waits, merges segment claims, posts the DP-done once
            gdx_sched_drain_deferred_wakes();
            GDX_HB("hb:dispatch", 5);
            gdx_dispatch();
        }
        GDX_HB("hb:save_tick", 6);
        gdx_disk_save_tick();
        if (rtMode != 0) {
            GDX_HB("hb:end_frame", 7);
            gdx3ds_rt_frame_end(); // END: present fallback + EndFrame on the render thread
        } else {
            gdx_vi_present_fallback();
            GDX_HB("hb:end_frame", 7);
            w->EndFrame();
        }

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
        // allocation counters, lin is linearSpaceFree. Main-thread only — the same thread
        // that runs the game fibers and gdx_gfx_run, so the container scan cannot race.
        if (verboseTelemetry && frameNum > 8 && (frameNum & 255) == 2) {
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
            }
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
        gdx3ds_fps_hud_tick(gGameFrameCount);
        // MENU tick (same thread for the same reason): touch dispatch + repaint only
        // on change / ~1 Hz for live pages — no per-frame console traffic.
        gdx3ds_menu_tick();

        GDX_HB("hb:pacer", 8);
        gdx_frame_pacer_tick();
#undef GDX_HB
    }
#undef verboseTelemetry
#undef cadenceTelemetry

    sWatchdogQuiet = 1; // detached 0x18 thread: no beats through (or after) teardown
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
    aptLogReceipt("[exit] audio_thread_stop enter");
    gdx_audio_thread_stop(); // joins the HLE producer (its own park event; never wakes the drain)
    aptLogReceipt("[exit] audio_thread_stop done");
    aptLogReceipt("[exit] audio_shutdown enter");
    gdx3ds_audio_shutdown(); // joins the drain, then ndspExit (cancel-aware when suspended)
    aptLogReceipt("[exit] audio_shutdown done");
    aptLogReceipt("[exit] render_thread_join enter");
    gdx3ds_rt_shutdown(); // QUIT + threadJoin on its own event (no shared-event spin)
    aptLogReceipt("[exit] render_thread_join done");
    aptLogReceipt("[exit] window_shutdown enter");
    gdx3ds_os_window_shutdown(); // gfxExit (rights-aware when suspended)
    aptLogReceipt("[exit] window_shutdown done");
    gdx3ds_fs_shutdown();
    aptLogReceipt("[exit] fs_shutdown done");
    // Final flush: "exiting" and everything teardown logged must be on the SD card
    // before crt0/__appExit — a clean close then shows "[apt] exit hook ..." from the
    // ONEXIT hook inside aptExit() as the very last line.
    gdx3ds_filelog_flush();
    return 0;
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
