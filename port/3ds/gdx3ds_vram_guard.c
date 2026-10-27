/* port/3ds/gdx3ds_vram_guard.c -- n60boot: no CPU store may target VRAM. Contract and why:
 * port/3ds/include/gdx3ds_vram_guard.h. */
#include "gdx3ds_vram_guard.h"

#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_boot_trail.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_gfx_split.h"

enum { VRAM_GUARD_ALLOW = 0, VRAM_GUARD_REPAIR = 1, VRAM_GUARD_FATAL = 2 };

static int GuardMode(void) {
    const int mode = gdx3ds_config_get_int("debug", "vram_guard", VRAM_GUARD_REPAIR);
    if (mode < VRAM_GUARD_ALLOW || mode > VRAM_GUARD_FATAL) {
        return VRAM_GUARD_REPAIR;
    }
    return mode;
}

/* svc + the trail + the file log, flushed: the line must exist before whatever comes next. */
static void GuardLog(const char* line) {
    svcOutputDebugString(line, strlen(line));
    gdx3ds_boot_trail_file(line);
    gdx3ds_filelog_write(line, strlen(line));
    gdx3ds_filelog_flush_sync();
}

static void ReportClean(const char* who, const void* fb) {
    char line[160];
    snprintf(line, sizeof(line), "[vramguard] %s: bottom framebuffer %p linear heap (ok)", who, fb);
    GuardLog(line);
}

static void ReportViolation(const char* who, const void* fb, int mode) {
    char line[320];
    snprintf(line, sizeof(line),
             "[vramguard] VIOLATION %s: about to CPU-write the bottom framebuffer %p in VRAM (the CIA maps VRAM "
             "read-only: a data abort on hardware; the emulator does not enforce it) vram_guard=%d -> %s",
             who, fb, mode,
             mode == VRAM_GUARD_ALLOW ? "ALLOWED" : (mode == VRAM_GUARD_FATAL ? "FATAL" : "moving it to the linear heap"));
    GuardLog(line);
}

/* svcBreak ends the title on hardware; Azahar returns from it, so exit explicitly after. */
static void DieWith(const char* line) {
    GuardLog(line);
    svcBreak(USERBREAK_ASSERT);
    GuardLog("[vramguard] svcBreak returned (emulator): svcExitProcess");
    svcExitProcess();
}

static void RepairBottom(const char* who) {
    char line[160];
    if (!gdx3ds_gfx_move_screen_to_linear(GFX_BOTTOM)) {
        snprintf(line, sizeof(line), "[vramguard] %s: moving the bottom framebuffers to the linear heap FAILED", who);
        DieWith(line);
        return;
    }
    snprintf(line, sizeof(line), "[vramguard] %s: bottom framebuffers now %p (linear heap)", who,
             (void*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
    GuardLog(line);
}

void gdx3ds_vram_guard_bottom_cpu_write(const char* who) {
    const void* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    if (!gdx3ds_gfx_screen_in_vram(GFX_BOTTOM)) {
        ReportClean(who, fb);
        return;
    }
    const int mode = GuardMode();
    ReportViolation(who, fb, mode);
    if (mode == VRAM_GUARD_FATAL) {
        DieWith("[vramguard] vram_guard=2: stopping here (svcBreak, then svcExitProcess)");
    } else if (mode == VRAM_GUARD_REPAIR) {
        RepairBottom(who);
    }
}
