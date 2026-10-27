/* port/3ds/gdx3ds_perf_preset.c — [perf] preset. Contract in the header. */
#include "gdx3ds_perf_preset.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_preset_n60.h"

enum {
    PERF_PRESET_OFF = 0,
    PERF_PRESET_O3DS = 1,
    PERF_PRESET_N60 = 2, /* n3ds: the n60 lever set (gdx3ds_preset_n60.c); every [perf] LOD key stock */
};

#define PERF_DRAW_DISTANCE_MIN_PCT 40
#define PERF_DRAW_DISTANCE_STOCK_PCT 100

typedef struct {
    const char* key;
    int value;
} PerfPresetEntry;

/* The Old 3DS preset. Every value is measured, not chosen: the per-lever emulator A/B and
 * the hardware windows behind each one are in docs/research/native/o3dslod-progress.md §3.
 *
 * - rival_detail 1 (REDUCED): the controller's floor. REDUCED measured CHEAPER than MINIMAL
 *   in a 30-machine crowd (MINIMAL exempts the 5 nearest rivals outright, REDUCED biases all
 *   of them), so it is the best floor on both counts.
 * - rival_tier_max 3: lets the controller reach LOW (decomp-port-o3ds-scene-lod.patch), the
 *   cheapest tier, which only this preset opens.
 * - rival_auto_hi_x10 450 / lo_x10 300: the band re-centred on the O3DS's own frame. hw-c4run2:
 *   a 30-machine GP race runs 67-100 ms, a one-machine race ~41, a menu ~27. RAISE above 45 ms
 *   (under ~22 fps) catches every GP crowd and not the one-machine race; LOWER under 30 ms
 *   (30 fps, the target) gives detail back only when there is real headroom. The 15 ms band
 *   is over 2x the per-tier authority measured on this console class (~3.5 ms).
 * - draw_distance: NOT in the preset (stock 100). 60 bought -2.9% of the emulator race frame, and
 *   on the Old 3DS console (2026-09-29, wave-3 card) the user found the track pop-in jarring --
 *   far too high a visual cost for the gain. Still settable by hand via [perf] draw_distance.
 * - lod6_skip 1: do not emit the texture-load list in front of a machine drawn at LOD 6 (the
 *   far speck; decomp-port-o3ds-scene-lod.patch). Pixel-identical on the canonical replay, and
 *   what makes MINIMAL/LOW pay: at those tiers most of the field is LOD 6.
 * - mp_rival_floor 3 (o3dsperf): LOW for every CPU rival while a multiplayer session is live
 *   (render-only there, see gdx_rival_detail_level). hw 158a13e: the MP race's rendered tick
 *   cost 26-30 ms, so the console drew every 3rd tick; the per-tier authority (~3.5 ms) is what
 *   brings it under the 2-tick slot. */
static const PerfPresetEntry kO3dsPreset[] = {
    { "rival_detail", 1 },
    { "rival_tier_max", 3 },
    { "rival_auto_hi_x10", 450 },
    { "rival_auto_lo_x10", 300 },
    { "lod6_skip", 1 },
    { "mp_rival_floor", 3 },
};

static int sInited = 0;
static int sPreset = PERF_PRESET_OFF;
static int sModelOld = 0;
static int sModelKnown = 0;
static int sAuto = 0;
static int sDrawDistancePct = PERF_DRAW_DISTANCE_STOCK_PCT;
static int sLod6Skip = 0;

static void PresetLog(const char* line) {
    svcOutputDebugString(line, (s32)strlen(line));
    gdx3ds_filelog_write(line, strlen(line));
}

static int ConsoleIsOld3ds(void) {
    bool isNew3ds = false;
    sModelKnown = 0;
    if (R_FAILED(APT_CheckNew3DS(&isNew3ds))) {
        return 0; /* unknown model: never degrade a console we cannot identify */
    }
    sModelKnown = 1;
    return isNew3ds ? 0 : 1;
}

static const char* ModelName(void) {
    if (!sModelKnown) {
        return "unknown";
    }
    return sModelOld ? "old3ds" : "new3ds";
}

static int ParsePresetName(const char* name, int modelOld, int* isAuto) {
    *isAuto = 0;
    if (name == NULL || name[0] == '\0' || strcasecmp(name, "auto") == 0) {
        *isAuto = 1;
        /* n60lead: a no-ini player gets the SAME tuned stack the hardware test cards carry an
         * explicit ini key for -- n60 on a New 3DS (gdx3ds_preset_n60.c's "o3ds-auto" table
         * mirrors this for the native/NRR lever layer, set by main_3ds.cpp). Old 3DS keeps its
         * existing LOD-only o3ds preset here (unchanged). */
        return modelOld ? PERF_PRESET_O3DS : PERF_PRESET_N60;
    }
    if (strcasecmp(name, "o3ds") == 0 || strcasecmp(name, "old3ds") == 0) {
        return PERF_PRESET_O3DS;
    }
    if (gdx3ds_preset_n60_name(name)) {
        return PERF_PRESET_N60;
    }
    return PERF_PRESET_OFF; /* "off", "n3ds" and anything unrecognised: the shipped defaults */
}

static const char* PresetName(void) {
    if (sPreset == PERF_PRESET_O3DS) {
        return "o3ds";
    } else if (sPreset == PERF_PRESET_N60) {
        return "n60";
    }
    return "off";
}

static int PresetDefault(const char* key, int stockDefault) {
    if (sPreset != PERF_PRESET_O3DS) {
        return stockDefault;
    }
    for (size_t i = 0; i < sizeof(kO3dsPreset) / sizeof(kO3dsPreset[0]); i++) {
        if (strcmp(kO3dsPreset[i].key, key) == 0) {
            return kO3dsPreset[i].value;
        }
    }
    return stockDefault;
}

static int ClampDrawDistance(int pct) {
    if (pct < PERF_DRAW_DISTANCE_MIN_PCT) {
        return PERF_DRAW_DISTANCE_MIN_PCT;
    }
    if (pct > PERF_DRAW_DISTANCE_STOCK_PCT) {
        return PERF_DRAW_DISTANCE_STOCK_PCT;
    }
    return pct;
}

static int ClampLod6Skip(int mode) {
    if (mode < 0) {
        return 0;
    }
    if (mode > 2) {
        return 2;
    }
    return mode;
}

static void PresetReceipt(void) {
    char line[256];
    snprintf(line, sizeof(line),
             "[perf] preset=%s (%s: model=%s) rival_detail=%d rival_tier_max=%d hi_x10=%d lo_x10=%d draw_distance=%d "
             "lod6_skip=%d mp_rival_floor=%d (explicit [perf] keys win)",
             PresetName(), sAuto ? "auto" : "ini", ModelName(),
             gdx3ds_perf_get_int("rival_detail", 0), gdx3ds_perf_get_int("rival_tier_max", 2),
             gdx3ds_perf_get_int("rival_auto_hi_x10", 150), gdx3ds_perf_get_int("rival_auto_lo_x10", 90),
             sDrawDistancePct, sLod6Skip, gdx3ds_perf_get_int("mp_rival_floor", 0));
    PresetLog(line);
}

void gdx3ds_perf_preset_init(void) {
    if (sInited) {
        return;
    }
    sInited = 1;
    sModelOld = ConsoleIsOld3ds();
    sPreset = ParsePresetName(gdx3ds_config_get_string("perf", "preset", "auto"), sModelOld, &sAuto);
    sDrawDistancePct = ClampDrawDistance(gdx3ds_perf_get_int("draw_distance", PERF_DRAW_DISTANCE_STOCK_PCT));
    sLod6Skip = ClampLod6Skip(gdx3ds_perf_get_int("lod6_skip", 0));
    PresetReceipt();
}

int gdx3ds_perf_preset_o3ds(void) {
    gdx3ds_perf_preset_init();
    return sPreset == PERF_PRESET_O3DS;
}

int gdx3ds_perf_get_int(const char* key, int stockDefault) {
    gdx3ds_perf_preset_init();
    return gdx3ds_config_get_int("perf", key, PresetDefault(key, stockDefault));
}

int gdx_track_draw_distance_pct(void) {
    gdx3ds_perf_preset_init();
    return sDrawDistancePct;
}

/* o3ds-mp16 [perf] mp_speck_drop (default 0; 1 = on; 2 = a same-run A/B on the [cpucut] block clock,
 * bit GDX3DS_CPUCUT_BIT_MPSPECK): while a multiplayer session is live, a machine drawn at LOD 6 (the
 * far speck, a few pixels) is not drawn at all -- lod6_skip 2's rule, for the session only. hw card-3
 * (Old 3DS, 2D VS 2P + 14 CPU): the NRR machine pass cost ~0.3 ms with only the player's machine in
 * view and 1.0-1.3 ms with the pack in view (runs/f 17-37 at ~25 us a run); the far specks are the
 * runs that buy the least picture. Render-only: in a session no simulation read sees the draw
 * (mp-vis), and the decomp hook drops only display-list emission (the model matrix is still set). */
#if defined(__3DS__)
extern int gdx3ds_mp_active(void) __attribute__((weak));
extern int gdx3ds_cpucut_clamp(int value, int fallback) __attribute__((weak));
extern void gdx3ds_cpucut_lever(const char* name, const int* value, unsigned bit) __attribute__((weak));
extern int gdx3ds_cpucut_on(int value, unsigned bit) __attribute__((weak));
#define PERF_CPUCUT_BIT_MPSPECK 2u /* GDX3DS_CPUCUT_BIT_MPSPECK (include/gdx3ds_cpucut.h) */
static int sMpSpeckDrop = -1;

static int MpSpeckDropOn(void) {
    if (sMpSpeckDrop < 0) {
        const int v = gdx3ds_perf_get_int("mp_speck_drop", 0);
        sMpSpeckDrop = (v == 1 || v == 2) ? v : 0;
        if (sMpSpeckDrop != 0 && &gdx3ds_cpucut_lever != NULL) {
            gdx3ds_cpucut_lever("mpspeck", &sMpSpeckDrop, PERF_CPUCUT_BIT_MPSPECK);
        }
    }
    if (sMpSpeckDrop == 0 || &gdx3ds_mp_active == NULL || !gdx3ds_mp_active()) {
        return 0;
    }
    if (sMpSpeckDrop == 1) {
        return 1;
    }
    return &gdx3ds_cpucut_on != NULL && gdx3ds_cpucut_on(sMpSpeckDrop, PERF_CPUCUT_BIT_MPSPECK);
}
#endif

int gdx_rival_lod6_skip(void) {
    gdx3ds_perf_preset_init();
#if defined(__3DS__)
    if (sLod6Skip < 2 && MpSpeckDropOn()) {
        return 2;
    }
#endif
    return sLod6Skip;
}

#ifdef GDX3DS_PERF_PRESET_HOST_TEST
void gdx3ds_perf_preset_test_reset(void) {
    sInited = 0;
    sPreset = PERF_PRESET_OFF;
    sModelOld = 0;
    sModelKnown = 0;
    sAuto = 0;
    sDrawDistancePct = PERF_DRAW_DISTANCE_STOCK_PCT;
    sLod6Skip = 0;
}
#endif
