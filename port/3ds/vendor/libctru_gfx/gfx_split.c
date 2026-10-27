/* port/3ds/vendor/libctru_gfx/gfx_split.c -- libctru v2.7.0 source/gfx.c, ALTERED (n60boot).
 *
 * Origin: https://github.com/devkitPro/libctru, tag v2.7.0 (36fe1ad), libctru/source/gfx.c,
 * zlib license (LICENSE next to this file). This is an altered version, not the original:
 *
 *   - Framebuffer memory is chosen PER SCREEN (top / bottom) instead of once for both.
 *     Upstream gfxInit(..., vrambuffers) sets one allocator for both screens, so
 *     [native] lcd_vram=1 put the BOTTOM framebuffers in VRAM too; the CIA maps VRAM
 *     read-only (packaging/gdiffuser.rsf), and the first CPU store into the bottom screen
 *     (consoleInit's clear, the launch prompt, the fps HUD, the menu) was a permission data
 *     abort: card n60-3d4 died at boot on the Old 3DS. gdx3ds_gfx_init_split() puts each
 *     screen where the caller says; gfxInit() keeps its upstream meaning (both screens alike).
 *   - The present "DMA size" bits follow the screen's own memory (upstream: one flag).
 *   - A screen whose VRAM allocation fails falls back to the linear heap (upstream: NULL).
 *   - gdx3ds_gfx_move_screen_to_linear(): re-home one screen's framebuffers in the linear
 *     heap at run time (the VRAM guard's repair, gdx3ds_vram_guard.c).
 *   - Brace style reformatted to the project's (K&R).
 *   - o3ds60: an optional THIRD top framebuffer (gdx3ds_gfx_top_triple_request): the back buffer
 *     is then the one that is neither on screen nor waiting for its vblank, so a frame never has
 *     to wait for the previous swap to latch before its display transfer (the pace end guard).
 *
 * Linked into G-Diffuser-3DS as an object, so every gfx* symbol resolves here and libctru.a's
 * gfx.o is never pulled (a missing symbol would pull it and fail the link on duplicates). */
#include <stdbool.h>
#include <string.h>

#include <3ds/types.h>
#include <3ds/allocator/linear.h>
#include <3ds/allocator/vram.h>
#include <3ds/services/gspgpu.h>
#include <3ds/gfx.h>
#include <3ds/os.h>

#include "gdx3ds_gfx_split.h"

static u8* gfxTopFramebuffers[2];
static u8* gfxBottomFramebuffers[2];
static u32 gfxTopFramebufferMaxSize;
static u32 gfxBottomFramebufferMaxSize;
static GSPGPU_FramebufferFormat gfxFramebufferFormats[2];

static enum {
    MODE_2D = 0,
    MODE_3D = 1,
    MODE_WIDE = 2,
} gfxTopMode;
static bool gfxIsVram[2]; /* n60boot: per screen (upstream: one bool) */
static bool gfxInited;    /* upstream used screenFree != NULL */
static u8 gfxCurBuf[2];
static u8 gfxIsDoubleBuf[2];

/* ---- o3ds60 top-screen triple buffering ------------------------------------------------------
 * gfxCurBuf[GFX_TOP] is the latest presented buffer (0..2) while the rotation is on. The swap runs
 * on the GSP event thread (citro3d's onQueueFinish); gfxGetFramebuffer on the main thread reads
 * the latched back index, which only a swap changes. Mode changes are requested from the main
 * thread and applied at a swap:
 *   OFF  -> ON   the third buffer joins: the next back is the one neither shown nor pending.
 *   ON   -> LEAVE the next backs are picked from buffers 0 and 1 only;
 *   LEAVE -> OFF once the latest present and the buffer on screen are both 0 / 1: upstream
 *               double buffering again (cur ^ 1), the pace end guard covers the latch. */
enum { TOP_TRIPLE_OFF = 0, TOP_TRIPLE_ON = 1, TOP_TRIPLE_LEAVE = 2 };
static u8* gfxTopThird = NULL;
static volatile int gfxTopTripleWant = 0;
static volatile int gfxTopTripleState = TOP_TRIPLE_OFF;
static volatile u8 gfxTopBack = 1;  /* the back buffer while the state is not OFF */
static u8 gfxTopShown = 0;          /* the buffer known to be on screen */
static u8 gfxTopSel = 0;            /* the register set (0/1) of the latest top present */
static volatile u32 gfxTopTriplePresents = 0;
static volatile u32 gfxTopTripleReplaced = 0; /* presents that replaced a pending one (a frame never shown) */
static volatile u32 gfxTopTripleEdges = 0;

static u8* TopBuffer(unsigned id) {
    return id < 2u ? gfxTopFramebuffers[id] : gfxTopThird;
}

static void FreeFramebuffer(void* mem);

/* Both buffers of a screen in one memory: a VRAM screen whose second buffer does not fit moves
 * whole to the linear heap (the present mode bits are per screen, not per buffer). */
static void AllocFramebufferPair(gfxScreen_t screen, size_t size, u8** out) {
    if (gfxIsVram[screen]) {
        out[0] = (u8*)vramAlloc(size);
        out[1] = (u8*)vramAlloc(size);
        if (out[0] != NULL && out[1] != NULL) {
            return;
        }
        FreeFramebuffer(out[0]);
        FreeFramebuffer(out[1]);
        gfxIsVram[screen] = false; /* VRAM full: this screen lives in the linear heap */
    }
    out[0] = (u8*)linearAlloc(size);
    out[1] = (u8*)linearAlloc(size);
}

static void FreeFramebuffer(void* mem) {
    if (mem == NULL) {
        return;
    }
    if (gdx3ds_gfx_addr_in_vram(mem)) {
        vramFree(mem);
    } else {
        linearFree(mem);
    }
}

static u8** ScreenFramebuffers(gfxScreen_t screen) {
    return screen == GFX_TOP ? gfxTopFramebuffers : gfxBottomFramebuffers;
}

static u32* ScreenMaxSize(gfxScreen_t screen) {
    return screen == GFX_TOP ? &gfxTopFramebufferMaxSize : &gfxBottomFramebufferMaxSize;
}

void gfxSet3D(bool enable) {
    gfxTopMode = enable ? MODE_3D : MODE_2D;
}

bool gfxIs3D(void) {
    return gfxTopMode == MODE_3D;
}

void gfxSetWide(bool enable) {
    gfxTopMode = enable ? MODE_WIDE : MODE_2D;
}

bool gfxIsWide(void) {
    return gfxTopMode == MODE_WIDE;
}

void gfxSetScreenFormat(gfxScreen_t screen, GSPGPU_FramebufferFormat format) {
    u32 reqSize = GSP_SCREEN_WIDTH * gspGetBytesPerPixel(format);
    u8** framebuffers = ScreenFramebuffers(screen);
    u32* maxSize = ScreenMaxSize(screen);
    reqSize *= screen == GFX_TOP ? GSP_SCREEN_HEIGHT_TOP_2X : GSP_SCREEN_HEIGHT_BOTTOM;

    if (*maxSize < reqSize) {
        FreeFramebuffer(framebuffers[0]);
        FreeFramebuffer(framebuffers[1]);
        AllocFramebufferPair(screen, reqSize, framebuffers);
        *maxSize = reqSize;
    }

    gfxFramebufferFormats[screen] = format;
}

GSPGPU_FramebufferFormat gfxGetScreenFormat(gfxScreen_t screen) {
    return gfxFramebufferFormats[screen];
}

void gfxSetDoubleBuffering(gfxScreen_t screen, bool enable) {
    gfxIsDoubleBuf[screen] = enable ? 1 : 0; // make sure they're the integer values '1' and '0'
}

static void gfxPresentFramebufferSel(gfxScreen_t screen, u8 id, u8 sel, bool hasStereo) {
    u32 stride = GSP_SCREEN_WIDTH * gspGetBytesPerPixel(gfxFramebufferFormats[screen]);
    u32 mode = gfxFramebufferFormats[screen];

    const u8* fb_a;
    const u8* fb_b;
    if (screen == GFX_TOP) {
        fb_a = TopBuffer(id);
        switch (gfxTopMode) {
            default:
            case MODE_2D:
                mode |= BIT(6);
                fb_b = fb_a;
                break;
            case MODE_3D:
                mode |= BIT(5);
                fb_b = hasStereo ? (fb_a + gfxTopFramebufferMaxSize / 2) : fb_a;
                break;
            case MODE_WIDE:
                fb_b = fb_a;
                break;
        }
    } else {
        fb_a = gfxBottomFramebuffers[id];
        fb_b = fb_a;
    }

    if (!gfxIsVram[screen]) {
        mode |= 1 << 8;
    } else {
        mode |= 3 << 8;
    }

    gspPresentBuffer(screen, sel, fb_a, fb_b, stride, mode);
}

static void gfxPresentFramebuffer(gfxScreen_t screen, u8 id, bool hasStereo) {
    if (screen == GFX_TOP) {
        gfxTopSel = id;
    }
    gfxPresentFramebufferSel(screen, id, id, hasStereo);
}

void gdx3ds_gfx_init_split(GSPGPU_FramebufferFormat topFormat, GSPGPU_FramebufferFormat bottomFormat, bool topInVram,
                           bool bottomInVram) {
    gfxIsVram[GFX_TOP] = topInVram;
    gfxIsVram[GFX_BOTTOM] = bottomInVram;
    gfxInited = true;

    // Initialize GSP
    gspInit();

    // Initialize configuration
    gfxSet3D(false);
    gfxSetScreenFormat(GFX_TOP, topFormat);
    gfxSetScreenFormat(GFX_BOTTOM, bottomFormat);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);

    // Present the framebuffers
    gfxCurBuf[0] = gfxCurBuf[1] = 0;
    gfxPresentFramebuffer(GFX_TOP, 0, false);
    gfxPresentFramebuffer(GFX_BOTTOM, 0, false);

    // Wait for VBlank and turn the LCD on
    gspWaitForVBlank();
    GSPGPU_SetLcdForceBlack(0x0);
}

void gfxInit(GSPGPU_FramebufferFormat topFormat, GSPGPU_FramebufferFormat bottomFormat, bool vrambuffers) {
    gdx3ds_gfx_init_split(topFormat, bottomFormat, vrambuffers, vrambuffers);
}

void gfxInitDefault(void) {
    gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, false);
}

void gfxExit(void) {
    if (!gfxInited) {
        return;
    }

    if (gspHasGpuRight()) {
        // Wait for VBlank and turn the LCD off
        gspWaitForVBlank();
        GSPGPU_SetLcdForceBlack(0x1);
    }

    // Free framebuffers
    FreeFramebuffer(gfxTopFramebuffers[0]);
    FreeFramebuffer(gfxTopFramebuffers[1]);
    FreeFramebuffer(gfxBottomFramebuffers[0]);
    FreeFramebuffer(gfxBottomFramebuffers[1]);
    FreeFramebuffer(gfxTopThird); /* o3ds60 */
    gfxTopThird = NULL;
    gfxTopTripleWant = 0;
    gfxTopTripleState = TOP_TRIPLE_OFF;
    gfxTopFramebuffers[0] = gfxTopFramebuffers[1] = NULL;
    gfxBottomFramebuffers[0] = gfxBottomFramebuffers[1] = NULL;
    gfxTopFramebufferMaxSize = gfxBottomFramebufferMaxSize = 0;

    // Deinitialize GSP
    gspExit();

    gfxInited = false;
}

u8* gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height) {
    unsigned id = gfxCurBuf[screen] ^ gfxIsDoubleBuf[screen];
    unsigned scr_width = GSP_SCREEN_WIDTH;
    unsigned scr_height;
    u8* fb;

    if (screen == GFX_TOP) {
        if (gfxTopTripleState != TOP_TRIPLE_OFF) {
            id = gfxTopBack;
        }
        fb = TopBuffer(id);
        scr_height = GSP_SCREEN_HEIGHT_TOP;
        switch (gfxTopMode) {
            default:
            case MODE_2D:
                break;
            case MODE_3D:
                if (side != GFX_LEFT) {
                    fb += gfxTopFramebufferMaxSize / 2;
                }
                break;
            case MODE_WIDE:
                scr_height = GSP_SCREEN_HEIGHT_TOP_2X;
                break;
        }
    } else { // GFX_BOTTOM
        fb = gfxBottomFramebuffers[id];
        scr_height = GSP_SCREEN_HEIGHT_BOTTOM;
    }

    if (width) {
        *width = scr_width;
    }
    if (height) {
        *height = scr_height;
    }

    return fb;
}

void gfxFlushBuffers(void) {
    const u32 baseSize = GSP_SCREEN_WIDTH * gspGetBytesPerPixel(gfxGetScreenFormat(GFX_TOP));
    const u32 topSize = GSP_SCREEN_HEIGHT_TOP * baseSize;
    const u32 topSize2x = GSP_SCREEN_HEIGHT_TOP_2X * baseSize;
    const u32 bottomSize = GSP_SCREEN_HEIGHT_BOTTOM * baseSize;

    GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), gfxTopMode == MODE_WIDE ? topSize2x : topSize);
    if (gfxTopMode == MODE_3D) {
        GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL), topSize);
    }
    GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), bottomSize);
}

/* The buffer that is neither `a` nor `b`, preferring 0 / 1 when `lowOnly` (two may qualify). */
static u8 OtherTopBuffer(u8 a, u8 b, bool lowOnly) {
    u8 i;
    for (i = 0; i < (lowOnly ? 2u : 3u); i++) {
        if (i != a && i != b) {
            return i;
        }
    }
    return 2u; /* lowOnly with {a, b} = {0, 1}: the third is the only free one */
}

/* o3ds60: one top present under the rotation (state ON or LEAVE), then the next back. */
static void TopTripleSwap(bool hasStereo) {
    const u8 b = gfxTopBack;
    const bool pending = gspIsPresentPending(GSP_SCREEN_TOP);
    u8 sel;
    if (!pending) {
        gfxTopShown = gfxCurBuf[GFX_TOP]; /* the previous present has latched */
        sel = gfxTopSel ^ 1u;
    } else {
        sel = gfxTopSel; /* replaces the pending present: the shown buffer keeps the other set */
        gfxTopTripleReplaced++;
    }
    gfxCurBuf[GFX_TOP] = b;
    gfxTopSel = sel;
    gfxPresentFramebufferSel(GFX_TOP, b, sel, hasStereo);
    gfxTopTriplePresents++;
    if (gfxTopTripleWant && gfxTopThird != NULL) {
        gfxTopTripleState = TOP_TRIPLE_ON;
    } else if (gfxTopTripleState == TOP_TRIPLE_ON) {
        gfxTopTripleState = TOP_TRIPLE_LEAVE;
        gfxTopTripleEdges++;
    }
    if (gfxTopTripleState == TOP_TRIPLE_LEAVE && b < 2u && gfxTopShown < 2u) {
        /* both in use are 0 / 1: upstream double buffering from here (back = cur ^ 1, which the
           end guard keeps off the screen until this present latches) */
        gfxTopTripleState = TOP_TRIPLE_OFF;
        return;
    }
    gfxTopBack = OtherTopBuffer(b, gfxTopShown, gfxTopTripleState == TOP_TRIPLE_LEAVE);
}

void gfxScreenSwapBuffers(gfxScreen_t scr, bool hasStereo) {
    if (scr == GFX_TOP && gfxIsDoubleBuf[GFX_TOP]) {
        if (gfxTopTripleState == TOP_TRIPLE_OFF && gfxTopTripleWant && gfxTopThird != NULL) {
            /* OFF -> ON: this present is the double-buffer back (cur ^ 1); afterwards the rotation */
            gfxTopTripleState = TOP_TRIPLE_ON;
            gfxTopBack = gfxCurBuf[GFX_TOP] ^ 1u;
            gfxTopShown = gspIsPresentPending(GSP_SCREEN_TOP) ? (u8)(gfxCurBuf[GFX_TOP] ^ 1u) : gfxCurBuf[GFX_TOP];
            gfxTopTripleEdges++;
        }
        if (gfxTopTripleState != TOP_TRIPLE_OFF) {
            TopTripleSwap(hasStereo);
            return;
        }
    }
    gfxCurBuf[scr] ^= gfxIsDoubleBuf[scr];
    gfxPresentFramebuffer(scr, gfxCurBuf[scr], hasStereo);
}

void gfxConfigScreen(gfxScreen_t scr, bool immediate) {
    (void)immediate;
    gfxScreenSwapBuffers(scr, true);
}

void gfxSwapBuffers(void) {
    gfxScreenSwapBuffers(GFX_TOP, true);
    gfxScreenSwapBuffers(GFX_BOTTOM, true);
}

void gfxSwapBuffersGpu(void) {
    gfxScreenSwapBuffers(GFX_TOP, true);
    gfxScreenSwapBuffers(GFX_BOTTOM, true);
}

/* ---------------------------------------------------------------- n60boot additions ---- */

bool gdx3ds_gfx_addr_in_vram(const void* addr) {
    const u32 a = (u32)addr;
    return a >= OS_VRAM_VADDR && a < OS_VRAM_VADDR + OS_VRAM_SIZE;
}

bool gdx3ds_gfx_screen_in_vram(gfxScreen_t screen) {
    const u8* const* framebuffers = (const u8* const*)ScreenFramebuffers(screen);
    return gdx3ds_gfx_addr_in_vram(framebuffers[0]) || gdx3ds_gfx_addr_in_vram(framebuffers[1]);
}

void gdx3ds_gfx_framebuffer_pair(gfxScreen_t screen, const void** out0, const void** out1) {
    const u8* const* framebuffers = (const u8* const*)ScreenFramebuffers(screen);
    *out0 = framebuffers[0];
    *out1 = framebuffers[1];
}

/* Both new buffers are allocated before anything changes, so a failure leaves the screen as it
 * was. The old ones are freed only after a vblank has latched the new scanout address. */
bool gdx3ds_gfx_move_screen_to_linear(gfxScreen_t screen) {
    u8** framebuffers = ScreenFramebuffers(screen);
    const u32 size = *ScreenMaxSize(screen);
    if (!gfxInited || !gdx3ds_gfx_screen_in_vram(screen)) {
        return true;
    }
    u8* fresh0 = (u8*)linearAlloc(size);
    u8* fresh1 = (u8*)linearAlloc(size);
    if (fresh0 == NULL || fresh1 == NULL) {
        linearFree(fresh0);
        linearFree(fresh1);
        return false;
    }
    memset(fresh0, 0, size);
    memset(fresh1, 0, size);
    GSPGPU_FlushDataCache(fresh0, size);
    GSPGPU_FlushDataCache(fresh1, size);
    u8* old0 = framebuffers[0];
    u8* old1 = framebuffers[1];
    framebuffers[0] = fresh0;
    framebuffers[1] = fresh1;
    gfxIsVram[screen] = false;
    gfxPresentFramebuffer(screen, gfxCurBuf[screen], false);
    gspWaitForVBlank();
    FreeFramebuffer(old0);
    FreeFramebuffer(old1);
    return true;
}

/* ---------------------------------------------------------------- o3ds60 additions ---- */

bool gdx3ds_gfx_top_triple_request(bool on) {
    if (on && gfxTopThird == NULL && gfxInited && gfxTopFramebufferMaxSize != 0) {
        u8* third = gfxIsVram[GFX_TOP] ? (u8*)vramAlloc(gfxTopFramebufferMaxSize) : (u8*)linearAlloc(gfxTopFramebufferMaxSize);
        if (third == NULL) {
            return false;
        }
        memset(third, 0, gfxTopFramebufferMaxSize);
        GSPGPU_FlushDataCache(third, gfxTopFramebufferMaxSize);
        gfxTopThird = third;
    }
    gfxTopTripleWant = (on && gfxTopThird != NULL) ? 1 : 0;
    return !on || gfxTopThird != NULL;
}

bool gdx3ds_gfx_top_triple_active(void) {
    return gfxTopTripleState != TOP_TRIPLE_OFF;
}

void gdx3ds_gfx_top_triple_stats(GdxGfxTripleStats* out) {
    out->presents = gfxTopTriplePresents;
    out->replaced = gfxTopTripleReplaced;
    out->edges = gfxTopTripleEdges;
    out->state = gfxTopTripleState;
    out->bytes = gfxTopThird != NULL ? gfxTopFramebufferMaxSize : 0u;
    out->inVram = gfxTopThird != NULL && gdx3ds_gfx_addr_in_vram(gfxTopThird);
}
