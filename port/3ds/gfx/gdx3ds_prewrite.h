/* port/3ds/gfx/gdx3ds_prewrite.h -- o3ds3d: the [prewrite] and [gpumem] receipts (measurement only).
 *
 * hw b52ac08 (Old 3DS guest, 3D, D = 1): the CPU waits 1-3 ms (7.5 max) in C3D_FrameBegin for the
 * previous frame's GPU tail (the right eye's replay, ~4 ms of GPU after the frame end), because
 * citro3d has one command buffer and the 1 MiB VBO pool restarts at offset 0 every frame: nothing of
 * the new frame may be written while the GPU still reads the old one. Two cheaper fixes need numbers
 * first:
 *   - "wait at the first write": move the queue wait from C3D_FrameBegin to the frame's first write
 *     into memory the GPU reads (command buffer, VBO pool, a texture upload, a GX queue entry), so the
 *     CPU work before that write runs beside the GPU tail;
 *   - a second command buffer / VBO pool, so the next frame never waits for the previous one.
 *
 * `[debug] prewrite = 1` (default 0: every hook is one int test): per rendered race frame,
 *   gsp   C3D_FrameBegin's GX queue wait (the pace module's queue ticks);
 *   pre   C3D_FrameBegin's return -> the first write of the frame after StartFrame's own setup
 *         (StartFrame's state words would be deferred too; they are counted as sf=);
 *   hide  min(gsp, pre) on an EXPOSED frame (no vblank wait after the queue wait: a wait the pace
 *         gate makes after the queue absorbs gsp anyway) = what the first-write wait would save.
 *   draw  as pre, to the first write that is NOT a GX entry (a fill / copy is a queue entry that a
 *         deferred wait could hold back cheaply; command words, VBO data and textures cannot);
 *   hided min(gsp, draw) on an exposed frame: the saving if GX entries are held back too.
 * The first write is the earliest of: a VBO handout (DrawTriangles, VboPackBegin, NativeMeshBegin,
 * NativeXfVertices, the native batch cursor), a texture upload / atlas placement, a data-cache flush,
 * and -- polled at those sites, around every framebuffer clear / copy (an elided clear queues nothing)
 * and at every [c5cat] marker --
 * any move of the command-buffer write pointer or the GX queue's entry count (the NRR direct emitters
 * write raw words). A direct stamp is taken at the function's ENTRY (before its write: conservative);
 * a polled write lies between the last clean poll and the poll that saw it (width= is that bracket).
 * Receipt per 256 rendered race frames, stats over every frame (b2b = the frames whose previous
 * iteration rendered too, D = 1: b2b_gsp is their average queue wait):
 *   [prewrite] frame= n= b2b= 3d= exposed= | gsp= pre= hide= (avg/p90/max ms) | draw= hided= |
 *              vbw= b2b_gsp= | first cmd= vbo= tex= gx= flush= none= raw= width= us |
 *              drawfirst cmd= vbo= tex= flush= none= | sf=frames/words
 *
 * `[debug] gpumem = 1` (default 0): per race (the game's race scope), what a second command buffer
 * and VBO pool would have to fit beside:
 *   [gpumem] race=N end|checkpoint frames= 3d= | linear free min= start= contig= | vram free min= |
 *            cmdbuf max= avg= of= | vbo max= avg= of= | halves cmd= vbo=
 * linear free is linearSpaceFree() every 8th frame (the low water), contig= the largest single
 * linearAlloc of 2 / 1.5 / 1 / 0.5 MiB that succeeded (freed at once) 600 frames into the race, the
 * command-buffer words of a frame are C3D_GetCmdBufUsage() after C3D_FrameEnd (every split of the
 * frame), the VBO bytes the pool cursor at EndFrame. halves = whether the race's largest frame fits
 * half of today's buffer (a split in two instead of a new allocation).
 *
 * Threads: begin / startframe_done / note / poll / frame_end run where the C3D frame is built (the
 * main thread on the Old 3DS); set_race is called by the main loop and only stores a flag. */
#ifndef GDX3DS_PREWRITE_H
#define GDX3DS_PREWRITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX3DS_PW_CMD = 0,   /* the command-buffer write pointer moved (polled) */
    GDX3DS_PW_VBO = 1,   /* a VBO pool handout */
    GDX3DS_PW_TEX = 2,   /* a texture upload / atlas placement */
    GDX3DS_PW_GX = 3,    /* a GX queue entry (a fill or a copy) */
    GDX3DS_PW_FLUSH = 4, /* a data-cache flush (a write to linear memory just happened) */
    GDX3DS_PW_NONE = 5,  /* nothing written before EndFrame */
    GDX3DS_PW_KINDS = 6,
};

/* 2 while a frame's first write is still to be seen, 1 inside StartFrame, 0 otherwise. */
extern volatile int gdx3ds_pw_armed;

/* Right after a fresh C3D_FrameBegin (gdx3ds_gpu_prof.c); `tEnter` = the tick before it. */
void gdx3ds_pw_begin(uint64_t tEnter);
/* The end of StartFrame's first begin: its own setup writes are the baseline. */
void gdx3ds_pw_startframe_done(void);
void gdx3ds_pw_note_slow(int kind);
void gdx3ds_pw_poll_slow(void);
/* EndFrame's entry: the frame's VBO bytes and the pool's size. */
void gdx3ds_pw_frame_end(uint32_t vboBytes, uint32_t vboPoolBytes);
/* Right after C3D_FrameEnd: the frame's command-buffer words. */
void gdx3ds_pw_after_frame_end(void);
/* 1 when either lever is on (latches them; main loop, once the config is loaded). */
int gdx3ds_pw_enabled(void);
/* Every main-loop iteration: the game is in race scope; the race is lockstep. */
void gdx3ds_pw_set_race(int inRace, int lockstep);

#define GDX3DS_PW_NOTE(kind)                    \
    do {                                        \
        if (__builtin_expect(gdx3ds_pw_armed != 0, 0)) { \
            gdx3ds_pw_note_slow(kind);          \
        }                                       \
    } while (0)

#define GDX3DS_PW_POLL()                        \
    do {                                        \
        if (__builtin_expect(gdx3ds_pw_armed == 2, 0)) { \
            gdx3ds_pw_poll_slow();              \
        }                                       \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_PREWRITE_H */
