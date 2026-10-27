/* port/3ds/gdx3ds_widescreen.c -- `[display] widescreen`. Contract in the header. */
#include "gdx3ds_widescreen.h"

#include <stdio.h>
#include <string.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

/* consolevariablebridge.h, declared locally (C TU): int32_t is `int` on this target. */
extern void CVarSetInteger(const char* name, int value);

#define WIDE_RECEIPT_MAX 64

static int sLever = -1;             /* -1 = ini not read yet */
static volatile int sFrameKind = 0; /* GDX3DS_WIDE_*, read by the backend's StartFrame */
static int sAppliedKind = -1;       /* the kind the CVars were last set for (-1 = never) */
static unsigned sReceipts = 0;
static int sCullMarginPct = -1;     /* -1 = ini not read yet */

static void WideLog(const char* line) {
    gdx3ds_filelog_write(line, strlen(line));
}

int gdx3ds_widescreen_lever(void) {
    if (sLever < 0) {
        if (!gdx3ds_config_loaded()) {
            return 0;
        }
        sLever = gdx3ds_config_get_int("display", "widescreen", 0) != 0 ? 1 : 0;
        if (sLever != 0) {
            WideLog("[widescreen] init widescreen=1: race frames render 5:3 (RemoveBorders 1P, "
                    "UltrawideMode culls x1.25, WidescreenUI anchors); menus unchanged");
        }
    }
    return sLever;
}

static void ApplyKind(int kind) {
    const int active = kind != GDX3DS_WIDE_NONE ? 1 : 0;
    CVarSetInteger("gEnhancements.Graphics.RemoveBorders", kind == GDX3DS_WIDE_1P ? 1 : 0);
    CVarSetInteger("gEnhancements.Graphics.UltrawideMode", active);
    CVarSetInteger("gEnhancements.Graphics.WidescreenUI", active);
    sAppliedKind = kind;
    if (sReceipts < WIDE_RECEIPT_MAX) {
        char line[96];
        sReceipts++;
        snprintf(line, sizeof(line), "[widescreen] frame kind=%d (0 menu/off, 1 race 1P, 2 race split)", kind);
        WideLog(line);
    }
}

void gdx3ds_widescreen_publish(int kind) {
    if (gdx3ds_widescreen_lever() == 0) {
        return; /* lever off: no CVar is ever written, kind stays NONE */
    }
    if (kind != GDX3DS_WIDE_1P && kind != GDX3DS_WIDE_SPLIT) {
        kind = GDX3DS_WIDE_NONE;
    }
    if (kind != sAppliedKind) {
        ApplyKind(kind);
    }
    sFrameKind = kind;
}

int gdx3ds_widescreen_frame_kind(void) {
    return sFrameKind;
}

float gdx3ds_widescreen_cull_margin(void) {
    if (sCullMarginPct < 0) {
        if (!gdx3ds_config_loaded()) {
            return 1.0f;
        }
        int pct = gdx3ds_config_get_int("display", "widescreen_cull_pct", GDX3DS_WIDE_CULL_PCT_DEFAULT);
        if (pct < 100) {
            pct = 100;
        } else if (pct > 200) {
            pct = 200;
        }
        sCullMarginPct = pct;
    }
    return (float) sCullMarginPct / 100.0f;
}
