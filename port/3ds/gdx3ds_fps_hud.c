/* port/3ds/gdx3ds_fps_hud.c — FPS-HUD implementation. Contract in the header.
 *
 * Console mechanics (verified against the shipped libctru.a console.o, not the
 * stale "(not implemented)" header comments): consoleSetWindow IS implemented —
 * 1-based x/y, it stores the window rect and homes the cursor, and newRow
 * scrolls WITHIN the window (windowX/Y/Width/Height at struct offsets 36..48).
 * So the HUD reserves rows 1..GDX_FPS_HUD_ROWS by parking the stdout scroll
 * window below them, and repaints its own rows by temporarily swapping the
 * window rect around each ~1 Hz refresh. CSI H addressing is window-relative.
 *
 * Allocation discipline: the refresh path formats with integer-only snprintf
 * (newlib's float formatting can Balloc/malloc via dtoa) into fixed stack/static
 * buffers and writes via fwrite(stdout). consoleInit leaves stdout unbuffered on
 * the console devoptab, and gdx3ds_os_window_swap's per-frame gfxFlushBuffers
 * flushes the console framebuffer cache.
 */
#include "gdx3ds_fps_hud.h"
#include "gdx3ds_menu.h"            /* MENU: row-1 handoff when the menu owns the screen */

#include <stdio.h>

#include <3ds.h>

#include "gdx3ds_config.h"          /* gdx3ds_os PUBLIC include dir */
#include "gfx/gdx3ds_gpu_prof.h"    /* gdx3ds_gpuprof_hud_sample */

#ifndef GDX3DS_BUILD_ID
#define GDX3DS_BUILD_ID "unknown-build"
#endif

#define GDX_FPS_HUD_ROWS 3
#define GDX_FPS_HUD_REFRESH_MS 1000u

static PrintConsole* sHudConsole = NULL;
static int sEnabled = 0;
static u64 sWindowStartTick = 0;
static u32 sWindowHostFrames = 0;
static unsigned sWindowStartGameFrames = 0;

/* Print one HUD row: CSI H to the row's column 1 (window-relative), content
 * padded/truncated to the console width so stale characters never survive. */
extern void gdx3ds_console_passthrough(int on) __attribute__((weak));

static void HudPaintRow(int row, const char* text) {
    char line[64];
    int n = snprintf(line, sizeof(line), "\x1b[%d;1H%-40.40s", row, text);
    if (n > 0) {
        if (&gdx3ds_console_passthrough != NULL) {
            gdx3ds_console_passthrough(1);
        }
        fwrite(line, 1, (size_t)n, stdout);
        if (&gdx3ds_console_passthrough != NULL) {
            gdx3ds_console_passthrough(0);
        }
    }
}

/* Swap the console window to the HUD rows, paint (NULL rows keep their pixels),
 * then restore the scroll window + its cursor so interleaved logs continue
 * exactly where they left off. Main/render thread only. */
static void HudPaint(const char* row1, const char* row2, const char* row3) {
    PrintConsole* con = sHudConsole;
    const int savedCursorX = con->cursorX;
    const int savedCursorY = con->cursorY;
    consoleSetWindow(con, 1, 1, con->consoleWidth, GDX_FPS_HUD_ROWS);
    if (row1 != NULL) {
        HudPaintRow(1, row1);
    }
    if (row2 != NULL) {
        HudPaintRow(2, row2);
    }
    if (row3 != NULL) {
        HudPaintRow(3, row3);
    }
    consoleSetWindow(con, 1, GDX_FPS_HUD_ROWS + 1, con->consoleWidth,
                     con->consoleHeight - GDX_FPS_HUD_ROWS);
    con->cursorX = savedCursorX;
    con->cursorY = savedCursorY;
    fflush(stdout);
}

void gdx3ds_fps_hud_init(void) {
    sEnabled = gdx3ds_config_get_bool("debug", "fps", 1);
    if (!sEnabled) {
        return;
    }
    /* Own the bottom-screen console. Re-running consoleInit after the backend's
     * debug.console init is harmless (it re-clears an empty screen — nothing has
     * printed yet at this point in boot) and gives us the PrintConsole*. */
    sHudConsole = consoleInit(GFX_BOTTOM, NULL);
    if (gdx3ds_menu_enabled()) {
        /* MENU owns the layout: it inits right after this and paints everything
         * (fps row included) itself. Skip the legacy 3-row paint. */
        return;
    }
    HudPaint("fps --.-  game --.-",
             "",
             "build " GDX3DS_BUILD_ID);
}

/* ---- MENU integration ---- */

void* gdx3ds_fps_hud_console(void) {
    return (void*)sHudConsole;
}

const char* gdx3ds_fps_hud_build_id(void) {
    return GDX3DS_BUILD_ID;
}

void gdx3ds_fps_hud_set_enabled(int on) {
    on = on ? 1 : 0;
    if (on && !sEnabled) {
        sWindowStartTick = 0; /* re-baseline: off-time must not dilute the rate */
        sWindowHostFrames = 0;
    }
    sEnabled = on;
}

int gdx3ds_fps_hud_get_enabled(void) {
    return sEnabled;
}

void gdx3ds_fps_hud_tick(unsigned gameFrameCount) {
    if (!sEnabled) {
        return;
    }
    const u64 now = svcGetSystemTick();
    if (sWindowStartTick == 0) {
        /* First tick: baseline only (boot time must not dilute the first rate). */
        sWindowStartTick = now;
        sWindowStartGameFrames = gameFrameCount;
        return;
    }
    sWindowHostFrames++;
    const u32 elapsedMs = (u32)((now - sWindowStartTick) / (u64)CPU_TICKS_PER_MSEC);
    if (elapsedMs < GDX_FPS_HUD_REFRESH_MS) {
        return;
    }

    /* Rates in tenths (fps = frames * 1000 / ms, x10 for one decimal). */
    const unsigned long hostFpsX10 = (unsigned long)sWindowHostFrames * 10000ul / elapsedMs;
    const unsigned long gameFpsX10 =
        (unsigned long)(gameFrameCount - sWindowStartGameFrames) * 10000ul / elapsedMs;

    char row1[48];
    snprintf(row1, sizeof(row1), "fps %lu.%lu  game %lu.%lu",
             hostFpsX10 / 10, hostFpsX10 % 10, gameFpsX10 / 10, gameFpsX10 % 10);

    if (gdx3ds_menu_enabled()) {
        /* MENU owns the console: hand the fps line over; it repaints row 1 on its
         * own dirty/1 Hz cadence (and drops it while the screen is off). */
        gdx3ds_menu_set_fps_line(row1);
    } else {
        /* Row 2 only carries data while gputrace is armed; sample failure (off, or
         * an empty prof window right after an emit) keeps the previous pixels. */
        char row2[48];
        const char* row2Text = NULL;
        unsigned buildMsX10 = 0;
        unsigned topOpMsX10 = 0;
        int topOp = -1;
        if (gdx3ds_gpuprof_hud_sample(&buildMsX10, &topOp, &topOpMsX10)) {
            if (topOp >= 0) {
                snprintf(row2, sizeof(row2), "build %u.%ums  top %02X %u.%ums",
                         buildMsX10 / 10, buildMsX10 % 10, (unsigned)topOp,
                         topOpMsX10 / 10, topOpMsX10 % 10);
            } else {
                snprintf(row2, sizeof(row2), "build %u.%ums", buildMsX10 / 10, buildMsX10 % 10);
            }
            row2Text = row2;
        }

        HudPaint(row1, row2Text, NULL); /* row 3 (build id) is static — painted at init */
    }

    sWindowStartTick = now;
    sWindowHostFrames = 0;
    sWindowStartGameFrames = gameFrameCount;
}
