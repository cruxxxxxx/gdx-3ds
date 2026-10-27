/* port/gdx_snapshot.h -- mp-snapshot: the game-state SNAPSHOT / RESTORE module's portable surface.
 *
 * Implementation: port/3ds/gdx_snapshot.c (3DS only; its header comment carries the design, the
 * layout, the levers and the receipts). This header is what the shared port/ TUs see: the section
 * macros that put a port-side static INTO the snapshot, and the hooks the input bridge, the
 * scheduler and the decomp patch call. Every hook is declared weak by its callers on non-3DS
 * builds, so the desktop build links with nothing here.
 *
 * THE SNAPSHOT is two linker-grouped blocks (port/3ds/gdx3ds_snapshot_{data,bss}.ld):
 *   snap .data   every decomp TU's .data (audio excluded) + `.data.gdx_snap` port statics
 *   snap .bss    every decomp TU's .bss/COMMON (audio, framebuffers, RSP buffers, the static
 *                display-list segment, save buffers and the leo font excluded) + `.bss.gdx_snap`
 * plus a few dynamic regions registered at runtime (the CPU-AI arena buffer, cpu.c). Decomp
 * objects that must NOT be restored (thread stacks, OSThread / OSMesgQueue objects, boot tables)
 * are moved out by decomp-3ds-snapshot.patch into `.bss.gdx_nosnap` / `.bss.gdx_nosched`.
 * snap-fast adds RUNTIME holes: port/3ds/game/gdx3ds_snapshot_nosnap.c lists objects inside the
 * blocks that no race writes (course editor, 64DD file system, EK menu textures; chosen by the
 * [debug] snap_census dirty census, watched by the gate), carved out at arm time -- the N3DS-EK
 * image drops from 892,832 to 363,196 bytes. The copies go through port/3ds/gdx3ds_snapcopy.c.
 *
 * DEFAULT OFF. With `[debug] snap_test` and `[debug] snap_probe` both 0 the module allocates
 * nothing and every hook below is one int test. */
#ifndef GDX_SNAPSHOT_H
#define GDX_SNAPSHOT_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__3DS__)
/* A zero-initialised port static that belongs to the simulation (restored with the decomp image). */
#define GDX_SNAP_STATE_BSS __attribute__((section(".bss.gdx_snap")))
/* The same for a static with a non-zero initialiser. */
#define GDX_SNAP_STATE_DATA __attribute__((section(".data.gdx_snap")))
#else
#define GDX_SNAP_STATE_BSS
#define GDX_SNAP_STATE_DATA
#endif

/* Channels of gdx_snap_tick_input: a value the simulation reads from OUTSIDE the snapshot (audio
 * thread state, lockstep session state) once per tick. The real tick records it, a resimulated
 * tick replays it, so a resim sees exactly what the real tick saw. */
enum {
    GDX_SNAP_IN_EK_BGM_LOADING = 0,   /* EK countdown hold: this console's disk BGM load (audio thread) */
    GDX_SNAP_IN_EK_BGM_READY_ALL = 1, /* EK countdown hold: every lockstep pad's "BGM ready" bit */
    GDX_SNAP_IN_DL_BUILD_DROP = 2,    /* C5TICK: the frameskip governor's "this build is dropped"
                                         prediction, which hollows the build's pure draw sections */
    GDX_SNAP_IN_CHANNELS = 4
};

/* Kinds of gdx_snap_se_gate (the one-shot sound triggers, external.c). */
enum {
    GDX_SNAP_SE_SYSTEM = 1,  /* Audio_TriggerSystemSE(sfx) */
    GDX_SNAP_SE_PLAYER = 2,  /* Audio_PlayerTriggerSEStart(racer, sfx) */
    GDX_SNAP_SE_LANDING = 3  /* Audio_Chakuchi(racer, ...) */
};

/* 1 while the module is re-running ticks silently (a resim); 0 otherwise. */
int gdx_snap_resim_active(void);

/* Input bridge: the FINAL pads of the tick being polled (after replay, mirror and the lockstep
 * exchange), recorded while a test/probe window is open. */
void gdx_snap_note_pads(unsigned long long tick, const unsigned short* buttons, const signed char* stickX,
                        const signed char* stickY, const unsigned char* connected, int count);
/* Input bridge (resim): the recorded pads of `tick`. Returns 0 when none were recorded. */
int gdx_snap_recorded_pads(unsigned long long tick, unsigned short* buttons, signed char* stickX,
                           signed char* stickY, unsigned char* connected, int count);

/* See GDX_SNAP_IN_*: returns the value the simulation must use this tick. */
int gdx_snap_tick_input(int channel, int value);

/* Scheduler (osSpTaskStartGo): 1 = ack the GFX task without rendering it (a resim tick). */
int gdx_snap_drop_task(void);

/* Decomp audio (AudioThread_QueueCmd, both libraries): 1 = drop this command (a resim tick). */
int gdx_snap_audio_drop(void);

/* Decomp audio (the one-shot triggers): 1 = suppress the call. Outside a resim it only records
 * (tick, kind, a, b) in the de-dup ring when a probe window is open; inside one it suppresses
 * every trigger and counts whether the real ticks already played it (dedup) or not (new). */
int gdx_snap_se_gate(int kind, int a, int b);

/* Decomp (cpu.c etc.): a sim buffer living OUTSIDE the linked image (the RDRAM arena). `slot`
 * names a fixed slot so a re-allocation at the next race replaces it. size 0 = unregister. */
#define GDX_SNAP_DYN_CPU_AI 0
#define GDX_SNAP_DYN_SLOTS 8
void gdx_snap_register_dynamic(int slot, void* ptr, unsigned int size, const char* name);

#if defined(__3DS__)
/* Host loop (main_3ds.cpp), at the END of every iteration -- the tick boundary. rtMode is the
 * render-thread mode (0 = sequential). Reads the levers once, then one int test while off. */
void gdx3ds_snapshot_frame_end(unsigned long frame, int rtMode);
#endif

#ifdef __cplusplus
}
#endif

#endif /* GDX_SNAPSHOT_H */
