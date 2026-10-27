/* port/3ds/include/gdx3ds_gfx_split.h -- n60boot: per-screen LCD framebuffer memory.
 *
 * Implemented by port/3ds/vendor/libctru_gfx/gfx_split.c, an altered copy of libctru v2.7.0's
 * gfx.c that replaces libctru's gfx.o in G-Diffuser-3DS (every gfx* entry point keeps its
 * upstream meaning). Why: libctru's gfxInit(..., vrambuffers) puts BOTH screens in VRAM, the
 * CIA maps VRAM read-only, and the bottom screen is drawn by the CPU -> data abort (n60-3d4).
 */
#ifndef GDX3DS_GFX_SPLIT_H
#define GDX3DS_GFX_SPLIT_H

#include <stdbool.h>

#include <3ds/gfx.h>
#include <3ds/services/gspgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* gfxInit with the memory chosen per screen. A VRAM screen that does not fit falls back to
 * the linear heap (check gdx3ds_gfx_screen_in_vram afterwards). */
void gdx3ds_gfx_init_split(GSPGPU_FramebufferFormat topFormat, GSPGPU_FramebufferFormat bottomFormat, bool topInVram,
                           bool bottomInVram);

/* True when either of the screen's two framebuffers is in VRAM. */
bool gdx3ds_gfx_screen_in_vram(gfxScreen_t screen);

/* True for an address inside the process's VRAM window (OS_VRAM_VADDR .. +OS_VRAM_SIZE). */
bool gdx3ds_gfx_addr_in_vram(const void* addr);

/* Both framebuffers of the screen (double buffering), for receipts. */
void gdx3ds_gfx_framebuffer_pair(gfxScreen_t screen, const void** out0, const void** out1);

/* Re-home the screen's framebuffers in the linear heap (cleared), present them, free the VRAM
 * ones after a vblank. True when the screen is (now) in the linear heap. Main thread. */
bool gdx3ds_gfx_move_screen_to_linear(gfxScreen_t screen);

/* o3ds60 top-screen triple buffering. request(true) allocates the third top framebuffer (once,
 * in the top screen's memory, gfxTopFramebufferMaxSize bytes: both eyes) and asks the next swap to
 * rotate three buffers; request(false) asks it to return to double buffering (over a swap or two).
 * False when the buffer could not be allocated. Main thread. active() is true while the rotation
 * (or its way out) runs: a frame's display transfer then never targets the buffer on screen or
 * the one waiting for its vblank, so it needs no wait for the previous swap to latch. */
typedef struct GdxGfxTripleStats {
    unsigned long presents; /* top presents under the rotation */
    unsigned long replaced; /* ... that replaced a still-pending present (that frame never showed) */
    unsigned long edges;    /* rotation on / off transitions */
    int state;              /* 0 off, 1 on, 2 leaving */
    unsigned long bytes;    /* the third buffer (0 = none) */
    bool inVram;
} GdxGfxTripleStats;
bool gdx3ds_gfx_top_triple_request(bool on);
bool gdx3ds_gfx_top_triple_active(void);
void gdx3ds_gfx_top_triple_stats(GdxGfxTripleStats* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_GFX_SPLIT_H */
