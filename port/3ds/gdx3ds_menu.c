/* port/3ds/gdx3ds_menu.c — MENU: v1 bottom-screen touch menu. Contract in the header.
 *
 * Console mechanics: same window-swap trick as gdx3ds_fps_hud.c (consoleSetWindow is
 * real; CSI H is window-relative). Every paint batch swaps the window to the full
 * console, addresses rows absolutely ("\x1b[<row>;1H\x1b[K" — position + clear line, so
 * stale characters never survive without pad math), then parks the stdout scroll
 * window on row 30 and restores the cursor, so stray printf traffic (debug.console)
 * stays confined to the last row.
 *
 * Allocation discipline: integer-only snprintf into fixed buffers, fwrite(stdout)
 * (newlib float formatting can malloc via dtoa; console stdout is unbuffered).
 *
 * Touch: hit zones are console cells (bottom screen 320x240 px / 8px font = 40x30
 * cells). Actions fire on the touch-DOWN edge only; the hid state is whatever
 * gdx3ds_os_poll_input latched this frame (this TU never calls hidScanInput — a
 * second scan per frame would eat the game's key-down edges).
 */
#include "gdx3ds_menu.h"
#include "gdx3ds_fps_hud.h"
#include "gdx3ds_filelog.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"     /* gdx3ds_os PUBLIC include dir */
#include "gdx3ds_input_map.h"
#include "gfx/gdx3ds_gpu_prof.h"

/* Cross-stream live hooks (diagnostic-export idiom: prototypes declared here, see
 * gdx3ds_audio_ndsp.c / gdx3ds_stereo.cpp / gfx_citro3d.cpp). */
extern int gdx3ds_audio_output_active(void);
extern void gdx3ds_audio_set_master_volume(int pct);
extern int gdx3ds_audio_get_master_volume(void);
extern void gdx3ds_audio_set_diag(int on);
extern int gdx3ds_audio_get_diag(void);
extern int gdx3ds_stereo_runtime_enabled(void);
extern int gdx3ds_stereo_get_iod_px(void);
extern void gdx3ds_stereo_set_iod_px(int px);
extern int gdx3ds_stereo_get_conv_x100(void);
extern void gdx3ds_stereo_set_conv_x100(int centi);
extern void gdx3ds_disp_set_mode(int mode);

/* ---- geometry ------------------------------------------------------------------ */
#define MENU_COLS 40
#define MENU_ROWS 30
#define MENU_ROW_FPS 1
#define MENU_ROW_TABS 2
#define MENU_ROW_SCROLL 30 /* stdout scroll window parks here */

enum {
    TAB_STATUS = 0,
    TAB_DISP,
    TAB_3D,
    TAB_AUD,
    TAB_INPUT,
    TAB_LOG,
    TAB_DBG,
    TAB_ABOUT,
    TAB_COUNT
};

/* Tab bar: "STAT DISP 3D AUD INP LOG DBG ABT" + [OFF] right-aligned. 1-based
 * inclusive column spans; keep in sync with the paint string below. */
static const struct {
    const char* label;
    int c0, c1;
} kTabs[TAB_COUNT] = {
    { "STAT", 1, 4 },  { "DISP", 6, 9 },   { "3D", 11, 12 },  { "AUD", 14, 16 },
    { "INP", 18, 20 }, { "LOG", 22, 24 },  { "DBG", 26, 28 }, { "ABT", 30, 32 },
};
#define MENU_OFF_C0 36
#define MENU_OFF_C1 40

/* ---- state --------------------------------------------------------------------- */
static PrintConsole* sCon = NULL;
static int sEnabled = 0;
static int sTab = TAB_STATUS;
static int sDirty = 0;          /* full page repaint requested */
static int sFpsDirty = 0;
static char sFpsLine[44] = "fps --.-  game --.-";
static int sScreenOff = 0;
static int sLcdInited = 0;      /* gspLcdInit succeeded (session kept open) */
static int sPrevTouching = 0;
static int sWakeSwallow = 0;    /* swallow the touch that woke the screen */
static u64 sLastPeriodicTick = 0;
static u32 sPrevHeld = 0;
static int sCaptureAction = -1; /* INPUT tab: action awaiting a key, -1 = none */
static int sLogSkip = 0;        /* LOG tab scroll offset (lines back from live) */
static int sDispMode = 0;       /* mirrors gdx3ds_disp mode for the radio UI */
static int sDbgVerbose = 0;     /* live [debug] verbose latch (main loop reads) */
static int sRivalDetail = 0;    /* [perf] rival_detail latch (Racer_Draw reads) */

/* RIVAL-DETAIL port hook: read once per Racer_Draw by the decomp patch
 * (decomp-port-rival-detail.patch). 0=NATIVE 1=REDUCED 2=MINIMAL. Latched from
 * [perf] rival_detail in gdx3ds_menu_init (even with the menu UI disabled) and
 * live-updated by the DISP tab row. */
int gdx_rival_detail_level(void) {
    return sRivalDetail;
}

#define MENU_LOG_LINES 25   /* rows 3..27 */
#define MENU_LOG_ROW0 3
#define MENU_LOG_SKIP_MAX 200
#define MENU_LOG_STEP 20

/* ---- receipts ------------------------------------------------------------------ */
static void MenuLog(const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf) - 1) {
            n = (int)sizeof(buf) - 1;
        }
        svcOutputDebugString(buf, (size_t)n);
        gdx3ds_filelog_write(buf, (size_t)n);
    }
}

static void MenuSaveCfg(void) {
    if (gdx3ds_config_save(GDX3DS_CONFIG_DEFAULT_PATH) != 0) {
        MenuLog("[menu] WARNING: ini save failed (%s)", GDX3DS_CONFIG_DEFAULT_PATH);
    }
}

/* ---- console primitives -------------------------------------------------------- */
static int sPaintDepth = 0;
static int sSavedCursorX = 0;
static int sSavedCursorY = 0;

static void PaintBegin(void) {
    if (sPaintDepth++ == 0) {
        sSavedCursorX = sCon->cursorX;
        sSavedCursorY = sCon->cursorY;
        consoleSetWindow(sCon, 1, 1, MENU_COLS, MENU_ROWS);
    }
}

static void PaintEnd(void) {
    if (--sPaintDepth == 0) {
        consoleSetWindow(sCon, 1, MENU_ROW_SCROLL, MENU_COLS, 1);
        sCon->cursorX = sSavedCursorX;
        sCon->cursorY = sSavedCursorY;
        fflush(stdout);
    }
}

/* Position at (row, col 1), clear the line, print (formatted, integer-only). */
static void gdx3ds_menu_tick_body(void);

static void PaintRow(int row, const char* fmt, ...) {
    char text[96];
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[K%s", row, text);
    if (n > 0) {
        fwrite(line, 1, (size_t)n, stdout);
    }
}

static void PaintClearPage(void) {
    for (int r = 3; r <= MENU_ROWS - 1; r++) {
        PaintRow(r, "");
    }
}

/* ---- tab bar ------------------------------------------------------------------- */
static void PaintTabBar(void) {
    char line[160];
    int n = snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[K", MENU_ROW_TABS);
    for (int t = 0; t < TAB_COUNT; t++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%s%s%s", t > 0 ? " " : "",
                      t == sTab ? "\x1b[7m" : "", kTabs[t].label,
                      t == sTab ? "\x1b[0m" : "");
        if (n >= (int)sizeof(line) - 12) {
            break;
        }
    }
    n += snprintf(line + n, sizeof(line) - (size_t)n, "\x1b[%d;%dH[OFF]", MENU_ROW_TABS,
                  MENU_OFF_C0);
    fwrite(line, 1, (size_t)n, stdout);
}

/* ---- pages --------------------------------------------------------------------- */
static void PaintStatus(void) {
    struct mallinfo mi = mallinfo();
    PaintRow(4, "heap used %6lu KB  free %6lu KB", (unsigned long)mi.uordblks / 1024u,
             (unsigned long)mi.fordblks / 1024u);
    PaintRow(5, "linear free %6lu KB", (unsigned long)linearSpaceFree() / 1024u);
    unsigned buildMsX10 = 0, topOpMsX10 = 0;
    int topOp = -1;
    if (gdx3ds_gpuprof_hud_sample(&buildMsX10, &topOp, &topOpMsX10)) {
        if (topOp >= 0) {
            PaintRow(7, "cpu build %u.%u ms  top %02X %u.%u ms", buildMsX10 / 10,
                     buildMsX10 % 10, (unsigned)topOp, topOpMsX10 / 10, topOpMsX10 % 10);
        } else {
            PaintRow(7, "cpu build %u.%u ms", buildMsX10 / 10, buildMsX10 % 10);
        }
    } else {
        PaintRow(7, "cpu build --.- (DBG: gputrace)");
    }
    PaintRow(9, "build %.34s", gdx3ds_fps_hud_build_id());
    PaintRow(11, "audio: %s   3d: %s",
             gdx3ds_audio_output_active() ? "ndsp" : "NULL SINK",
             gdx3ds_stereo_runtime_enabled() ? "on" : "off");
    PaintRow(13, "tap the tabs above; [OFF] sleeps");
    PaintRow(14, "the bottom screen (touch wakes).");
}

static void PaintDisp(void) {
    static const char* kModes[4] = {
        "AUTHENTIC   stock borders",
        "FULL-BLEED  scene fills 400x240",
        "ZOOM        uniform, thin bands",
        "HYBRID      fill + mild stretch",
    };
    static const char* kRivalNames[3] = { "NATIVE", "REDUCED", "MINIMAL" };
    PaintRow(4, "BORDER MODE (live + saved)");
    for (int m = 0; m < 4; m++) {
        PaintRow(6 + m * 2, "(%c) %s", sDispMode == m ? 'x' : ' ', kModes[m]);
    }
    PaintRow(15, "full-bleed/hybrid can expose sky-");
    PaintRow(16, "edge gaps (4:3 backdrops fall");
    PaintRow(17, "short of the opened edges).");
    PaintRow(19, "RIVAL DETAIL: %s  (tap cycles)",
             kRivalNames[(sRivalDetail >= 0 && sRivalDetail <= 2) ? sRivalDetail : 0]);
    PaintRow(21, "reduced/minimal simplify DISTANT");
    PaintRow(22, "rival machines for extra fps.");
}

static void Paint3D(void) {
    int on = gdx3ds_stereo_runtime_enabled();
    int conv = gdx3ds_stereo_get_conv_x100();
    PaintRow(4, "STEREO 3D  (%s)", on ? "enabled" : "disabled");
    PaintRow(6, "IOD     [-]  %2d px [+]  (0-40)", gdx3ds_stereo_get_iod_px());
    PaintRow(9, "CONV    [-]  0.%02d  [+]  (0-.90)", conv);
    PaintRow(12, "live while the 3D slider is up;");
    PaintRow(13, "saved to [stereo] iod/convergence");
    if (!on) {
        PaintRow(15, "enable: [stereo] enabled=1 + boot");
    }
}

static void PaintAud(void) {
    PaintRow(4, "OUTPUT: %s",
             gdx3ds_audio_output_active() ? "ndsp (DSP up)" : "NULL SINK (silent)");
    PaintRow(6, "MASTER  [-]  %3d   [+]  (0-100)", gdx3ds_audio_get_master_volume());
    PaintRow(9, "step 5; saved to [audio]");
    PaintRow(10, "master_volume");
    PaintRow(12, "music/sfx split: not in v1 (the");
    PaintRow(13, "mix lives inside the ROM audio");
    PaintRow(14, "lib, no cheap ndsp-side hook).");
}

static const char* InputDisplayName(int i) {
    /* config-stable names -> display labels */
    static const char* kNames[] = { "ACCEL", "BOOST", "BRAKE", "SLIDE-L",
                                    "SLIDE-R", "CAMERA", "PAUSE" };
    if (i >= 0 && i < (int)(sizeof(kNames) / sizeof(kNames[0]))) {
        return kNames[i];
    }
    return gdx3ds_input_action_name(i);
}

static void PaintInput(void) {
    char keys[24];
    PaintRow(3, "tap an action, then press its key");
    int count = gdx3ds_input_action_count();
    for (int i = 0; i < count && i < 7; i++) {
        if (i == sCaptureAction) {
            PaintRow(4 + i * 2, "\x1b[7m%-8s press a key (tap=cancel)\x1b[0m",
                     InputDisplayName(i));
        } else {
            PaintRow(4 + i * 2, "%-8s %s", InputDisplayName(i),
                     gdx3ds_input_key_label(gdx3ds_input_action_mask(i), keys,
                                            (int)sizeof(keys)));
        }
    }
    PaintRow(19, "RESET DEFAULTS");
    PaintRow(21, "capture keys: ABXY LR ZL/ZR");
    PaintRow(22, "START SEL DPAD C-STICK");
}

static void PaintLog(void) {
    static char lines[MENU_LOG_LINES][MENU_COLS + 1];
    int n = gdx3ds_filelog_tail_lines((unsigned)sLogSkip, &lines[0][0], MENU_LOG_LINES,
                                      MENU_COLS + 1);
    for (int i = 0; i < MENU_LOG_LINES; i++) {
        if (i < n) {
            PaintRow(MENU_LOG_ROW0 + i, "%.40s", lines[i]);
        } else {
            PaintRow(MENU_LOG_ROW0 + i, "");
        }
    }
    if (sLogSkip == 0) {
        PaintRow(29, "[ UP ]  [ DN ]  [LIVE]  tail");
    } else {
        PaintRow(29, "[ UP ]  [ DN ]  [LIVE]  -%d", sLogSkip);
    }
}

static void PaintDbg(void) {
    PaintRow(4, "(%c) GPUTRACE   [gpu]/[prof] telem",
             gdx3ds_gpuprof_get_enabled() ? 'x' : ' ');
    PaintRow(6, "(%c) DIAG-AUDIO [audio-out] receipts",
             gdx3ds_audio_get_diag() ? 'x' : ' ');
    PaintRow(8, "(%c) VERBOSE    frame/present/census", sDbgVerbose ? 'x' : ' ');
    PaintRow(10, "(%c) FPS ROW    top-row fps counter",
             gdx3ds_fps_hud_get_enabled() ? 'x' : ' ');
    PaintRow(12, "(%c) BRFAST     bridge per-list memo",
             gdx3ds_config_get_int("debug", "brfast", 1) ? 'x' : ' ');
    {
        /* LOCKED-60 lever set: trectbatch latches at boot, so this one needs a reboot. */
        int n = gdx3ds_config_get_int("debug", "trectbatch", 1) + gdx3ds_config_get_int("debug", "trifast", 1) +
                gdx3ds_config_get_int("debug", "tmemfast", 1);
        PaintRow(14, "(%c) LEVERS     atlas+tri+tmem (reboot)", n == 3 ? 'x' : (n == 0 ? ' ' : '-'));
    }
    /* LOCKED-60 Task H: core-2 render thread; the thread is created at boot, so a flip
     * needs a relaunch (port/3ds/gdx3ds_renderthread.cpp reads the key once). */
    PaintRow(16, "(%c) RENDER THR core-2 pipeline (reboot)",
             gdx3ds_config_get_int("debug", "renderthread", 1) ? 'x' : ' ');
    PaintRow(18, "toggles apply live and persist");
    PaintRow(19, "to gdiffuser.ini [debug].");
}

static void PaintAbout(void) {
    PaintRow(4, "G-DIFFUSER - F-ZERO X on 3DS");
    PaintRow(6, "build %.34s", gdx3ds_fps_hud_build_id());
    PaintRow(9, "F-ZERO X (C) 1998 NINTENDO");
    PaintRow(10, "decomp: the F-Zero X decomp team");
    PaintRow(11, "base: G-Diffuser + libultraship");
    PaintRow(12, "3DS port: the gdx-3ds effort");
    PaintRow(14, "thanks for playing.");
}

static void PaintPage(void) {
    PaintClearPage();
    switch (sTab) {
        case TAB_STATUS: PaintStatus(); break;
        case TAB_DISP:   PaintDisp(); break;
        case TAB_3D:     Paint3D(); break;
        case TAB_AUD:    PaintAud(); break;
        case TAB_INPUT:  PaintInput(); break;
        case TAB_LOG:    PaintLog(); break;
        case TAB_DBG:    PaintDbg(); break;
        default:         PaintAbout(); break;
    }
}

static void PaintAll(void) {
    PaintBegin();
    PaintRow(MENU_ROW_FPS, "%s", gdx3ds_fps_hud_get_enabled() ? sFpsLine : "");
    PaintTabBar();
    PaintPage();
    PaintEnd();
    sFpsDirty = 0;
    sDirty = 0;
}

/* ---- screen off ---------------------------------------------------------------- */
static void MenuSetBacklight(int on) {
    if (!sLcdInited) {
        if (R_FAILED(gspLcdInit())) {
            MenuLog("[menu] gspLcd unavailable; screen-off blanks only");
            return;
        }
        sLcdInited = 1;
    }
    if (on) {
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    } else {
        GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
    }
}

/* APT suspend/restore accessors (main_3ds.cpp aptLifecycleHook): the HOME menu must not
 * open onto a bottom screen we powered down, and the saved state must come back after.
 * Pure GSPLCD calls on the session MenuSetBacklight already holds -- no menu state
 * change, no log (hook context: svc + filelog only, never the console). */
int gdx3ds_menu_backlight_off(void) {
    return sScreenOff && sLcdInited;
}

void gdx3ds_menu_force_backlight(int on) {
    if (!sLcdInited) {
        return;
    }
    if (on) {
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    } else {
        GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
    }
}

static void MenuEnterOff(int persist) {
    sScreenOff = 1;
    PaintBegin();
    for (int r = 1; r <= MENU_ROWS; r++) {
        PaintRow(r, "");
    }
    PaintEnd();
    MenuSetBacklight(0);
    if (persist) {
        gdx3ds_config_set_int("menu", "screen_off", 1);
        MenuSaveCfg();
    }
    MenuLog("[menu] screen OFF (touch wakes)");
}

static void MenuLeaveOff(void) {
    sScreenOff = 0;
    MenuSetBacklight(1);
    sDirty = 1;
    gdx3ds_config_set_int("menu", "screen_off", 0);
    MenuSaveCfg();
    MenuLog("[menu] screen ON");
}

/* ---- touch handling ------------------------------------------------------------ */
static void TouchTabBar(int col) {
    if (col >= MENU_OFF_C0 && col <= MENU_OFF_C1) {
        MenuEnterOff(1);
        return;
    }
    for (int t = 0; t < TAB_COUNT; t++) {
        if (col >= kTabs[t].c0 && col <= kTabs[t].c1) {
            if (sTab != t) {
                sTab = t;
                sCaptureAction = -1;
                sLogSkip = 0;
                sDirty = 1;
                MenuLog("[menu] tab=%s", kTabs[t].label);
            }
            return;
        }
    }
}

static void TouchDisp(int row, int col) {
    (void)col;
    for (int m = 0; m < 4; m++) {
        if (row == 6 + m * 2 || row == 7 + m * 2) {
            if (sDispMode != m) {
                sDispMode = m;
                gdx3ds_disp_set_mode(m);
                gdx3ds_config_set_int("display", "border_mode", m);
                MenuSaveCfg();
                sDirty = 1;
                MenuLog("[menu] disp border_mode=%d", m);
            }
            return;
        }
    }
    if (row == 19 || row == 20) { /* RIVAL DETAIL row (+1 touch slop) */
        sRivalDetail = (sRivalDetail + 1) % 3;
        gdx3ds_config_set_int("perf", "rival_detail", sRivalDetail);
        MenuSaveCfg();
        sDirty = 1;
        MenuLog("[menu] perf rival_detail=%d", sRivalDetail);
    }
}

/* Shared stepper zones for the 3D/AUD pages: the [-] glyph sits at cols 9-11 and the
 * [+] glyph at cols 20-22 on every stepper row; zones widened one cell each side. */
static int StepperDelta(int col) {
    if (col >= 8 && col <= 13) {
        return -1;
    }
    if (col >= 19 && col <= 25) {
        return 1;
    }
    return 0;
}

static void Touch3D(int row, int col) {
    int d = StepperDelta(col);
    if (d == 0) {
        return;
    }
    if (row >= 5 && row <= 7) { /* IOD row 6 ± 1 */
        int v = gdx3ds_stereo_get_iod_px() + d;
        if (v < 0) v = 0;
        if (v > 40) v = 40;
        gdx3ds_stereo_set_iod_px(v);
        gdx3ds_config_set_int("stereo", "iod", v);
        MenuSaveCfg();
        sDirty = 1;
        MenuLog("[menu] stereo iod=%d", v);
    } else if (row >= 8 && row <= 10) { /* CONV row 9 ± 1 */
        int v = gdx3ds_stereo_get_conv_x100() + d * 5;
        if (v < 0) v = 0;
        if (v > 90) v = 90;
        gdx3ds_stereo_set_conv_x100(v);
        char buf[8];
        snprintf(buf, sizeof(buf), "0.%02d", v);
        gdx3ds_config_set_string("stereo", "convergence", buf);
        MenuSaveCfg();
        sDirty = 1;
        MenuLog("[menu] stereo convergence=%s", buf);
    }
}

static void TouchAud(int row, int col) {
    int d = StepperDelta(col);
    if (d == 0 || row < 5 || row > 7) { /* MASTER row 6 ± 1 */
        return;
    }
    int v = gdx3ds_audio_get_master_volume() + d * 5;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    gdx3ds_audio_set_master_volume(v);
    gdx3ds_config_set_int("audio", "master_volume", v);
    MenuSaveCfg();
    sDirty = 1;
    MenuLog("[menu] audio master_volume=%d", v);
}

static void MenuPersistBind(int action) {
    char key[48];
    char hex[16];
    snprintf(key, sizeof(key), "bind_%s", gdx3ds_input_action_name(action));
    snprintf(hex, sizeof(hex), "0x%X", gdx3ds_input_action_mask(action));
    gdx3ds_config_set_string("input", key, hex);
}

static void TouchInput(int row, int col) {
    (void)col;
    int count = gdx3ds_input_action_count();
    for (int i = 0; i < count && i < 7; i++) {
        if (row == 4 + i * 2 || row == 5 + i * 2) {
            if (sCaptureAction == i) {
                sCaptureAction = -1; /* tap again = cancel */
                MenuLog("[menu] input capture cancel (%s)", gdx3ds_input_action_name(i));
            } else {
                sCaptureAction = i;
                sPrevHeld = hidKeysHeld(); /* edges start from the current state */
                MenuLog("[menu] input capture %s", gdx3ds_input_action_name(i));
            }
            sDirty = 1;
            return;
        }
    }
    if (row == 19 || row == 20) {
        gdx3ds_input_reset_defaults();
        for (int i = 0; i < count; i++) {
            MenuPersistBind(i);
        }
        MenuSaveCfg();
        sCaptureAction = -1;
        sDirty = 1;
        MenuLog("[menu] input reset defaults");
    }
}

static void TouchLog(int row, int col) {
    if (row < 28) {
        return;
    }
    if (col >= 1 && col <= 7) { /* [ UP ] — older */
        if (sLogSkip < MENU_LOG_SKIP_MAX) {
            sLogSkip += MENU_LOG_STEP;
            sDirty = 1;
        }
    } else if (col >= 9 && col <= 15) { /* [ DN ] — newer */
        sLogSkip -= MENU_LOG_STEP;
        if (sLogSkip < 0) {
            sLogSkip = 0;
        }
        sDirty = 1;
    } else if (col >= 17 && col <= 22) { /* [LIVE] */
        sLogSkip = 0;
        sDirty = 1;
    }
}

static void TouchDbg(int row, int col) {
    (void)col;
    if (row == 4 || row == 5) {
        int on = !gdx3ds_gpuprof_get_enabled();
        gdx3ds_gpuprof_set_enabled(on);
        gdx3ds_config_set_int("debug", "gputrace", on);
        MenuLog("[menu] dbg gputrace=%d", on);
    } else if (row == 6 || row == 7) {
        int on = !gdx3ds_audio_get_diag();
        gdx3ds_audio_set_diag(on);
        gdx3ds_config_set_int("debug", "diag_audio", on);
        MenuLog("[menu] dbg diag_audio=%d", on);
    } else if (row == 8 || row == 9) {
        sDbgVerbose = !sDbgVerbose;
        gdx3ds_config_set_int("debug", "verbose", sDbgVerbose);
        MenuLog("[menu] dbg verbose=%d", sDbgVerbose);
    } else if (row == 10 || row == 11) {
        int on = !gdx3ds_fps_hud_get_enabled();
        gdx3ds_fps_hud_set_enabled(on);
        gdx3ds_config_set_int("debug", "fps", on);
        MenuLog("[menu] dbg fps=%d", on);
    } else if (row == 12 || row == 13) {
        int on = !gdx3ds_config_get_int("debug", "brfast", 1);
        gdx3ds_config_set_int("debug", "brfast", on);
        MenuLog("[menu] dbg brfast=%d", on);
    } else if (row == 14 || row == 15) {
        int on = !(gdx3ds_config_get_int("debug", "trectbatch", 1) && gdx3ds_config_get_int("debug", "trifast", 1) &&
                   gdx3ds_config_get_int("debug", "tmemfast", 1));
        gdx3ds_config_set_int("debug", "trectbatch", on);
        gdx3ds_config_set_int("debug", "trifast", on);
        gdx3ds_config_set_int("debug", "tmemfast", on);
        MenuLog("[menu] dbg levers=%d (reboot for atlas)", on);
    } else if (row == 16 || row == 17) {
        int on = !gdx3ds_config_get_int("debug", "renderthread", 1);
        gdx3ds_config_set_int("debug", "renderthread", on);
        MenuLog("[menu] dbg renderthread=%d (relaunch to apply)", on);
    } else {
        return;
    }
    MenuSaveCfg();
    sDirty = 1;
}

static void MenuHandleTouch(int row, int col) {
    if (row <= MENU_ROW_TABS) {
        TouchTabBar(col);
        return;
    }
    switch (sTab) {
        case TAB_DISP:  TouchDisp(row, col); break;
        case TAB_3D:    Touch3D(row, col); break;
        case TAB_AUD:   TouchAud(row, col); break;
        case TAB_INPUT: TouchInput(row, col); break;
        case TAB_LOG:   TouchLog(row, col); break;
        case TAB_DBG:   TouchDbg(row, col); break;
        default: break; /* STATUS/ABOUT: nothing tappable below the tab bar */
    }
}

/* ---- public API ---------------------------------------------------------------- */
int gdx3ds_menu_enabled(void) {
    return sEnabled;
}

void gdx3ds_menu_set_fps_line(const char* line) {
    if (line == NULL) {
        return;
    }
    snprintf(sFpsLine, sizeof(sFpsLine), "%s", line);
    sFpsDirty = 1;
}

int gdx3ds_dbg_verbose_active(void) {
    return sDbgVerbose || gdx3ds_gpuprof_get_enabled();
}

/* Console echo gate read by gdx_port_write_log (port_log.h, weak extern): while the touch
 * menu owns the bottom console, diagnostic stderr echo would scroll the menu page away
 * between repaints — svc + filelog sinks stay live; the LOG tab reads the ring. Legacy
 * console=1-without-menu keeps the echo. */
int gdx3ds_console_echo_enabled = 1;

/* CONSOLE FILTER: while the menu owns the bottom screen, every stdout/stderr write that is not
 * the menu's or the FPS row's own painting is dropped at the device layer. Stray printers
 * (LUS's raw fprintf(stderr, "Unsupported ccmux"), backend one-shots, anything on the render
 * thread) otherwise put a newline on the bottom row and scroll the tab bar away. The svc and
 * filelog sinks are untouched (they never go through stdio). */
#include <sys/iosupport.h>
extern int gdx3ds_rt_on_render_thread(void) __attribute__((weak));
static const devoptab_t* sConInner = NULL;
static devoptab_t sConFilter;
static volatile int sConPassthrough = 0;

static ssize_t ConFilterWrite(struct _reent* r, void* fd, const char* ptr, size_t len) {
    if (&gdx3ds_rt_on_render_thread != NULL && gdx3ds_rt_on_render_thread()) {
        return (ssize_t)len; /* never from the render thread: the console is not thread-safe */
    }
    if (!sConPassthrough && !gdx3ds_console_echo_enabled) {
        return (ssize_t)len; /* menu owns the console: drop */
    }
    return sConInner->write_r(r, fd, ptr, len);
}

static void ConFilterInstall(void) {
    if (sConInner != NULL || devoptab_list[STD_OUT] == NULL) {
        return;
    }
    sConInner = devoptab_list[STD_OUT];
    sConFilter = *sConInner;
    sConFilter.name = "gdxcon";
    sConFilter.write_r = ConFilterWrite;
    devoptab_list[STD_OUT] = &sConFilter;
    devoptab_list[STD_ERR] = &sConFilter;
}

void gdx3ds_console_passthrough(int on) {
    sConPassthrough = on ? 1 : 0;
}

void gdx3ds_menu_init(void) {
    /* Live latches init from config even when the UI is disabled (main_3ds.cpp's
     * telemetry gate reads gdx3ds_dbg_verbose_active unconditionally, Racer_Draw
     * reads gdx_rival_detail_level every frame). */
    sDbgVerbose = gdx3ds_config_get_int("debug", "verbose", 0) != 0;
    sRivalDetail = gdx3ds_config_get_int("perf", "rival_detail", 0);
    if (sRivalDetail < 0 || sRivalDetail > 2) {
        sRivalDetail = 0;
    }
    sEnabled = gdx3ds_config_get_bool("menu", "enabled", 1);
    if (!sEnabled) {
        return;
    }
    gdx3ds_console_echo_enabled = 0; /* menu owns the console from here on */
    sCon = (PrintConsole*)gdx3ds_fps_hud_console();
    if (sCon == NULL) {
        sCon = consoleInit(GFX_BOTTOM, NULL);
    }
    ConFilterInstall();
    sConPassthrough = 1; /* init paints below; cleared at the end of init */
    sDispMode = gdx3ds_config_get_int("display", "border_mode", 1); /* default FULL-BLEED (user pick, HW-verified) */
    if (sDispMode < 0 || sDispMode > 3) {
        sDispMode = 0;
    }
    sLastPeriodicTick = svcGetSystemTick();
    if (gdx3ds_config_get_int("menu", "screen_off", 0)) {
        MenuEnterOff(0); /* restore last state; no redundant save */
    } else {
        PaintAll();
    }
    MenuLog("[menu] init tab=STAT disp=%d off=%d", sDispMode, sScreenOff);
    sConPassthrough = 0;
}

void gdx3ds_menu_tick(void) {
    if (!sEnabled || sCon == NULL) {
        return;
    }
    sConPassthrough = 1; /* all menu painting happens inside this call (main thread) */
    gdx3ds_menu_tick_body();
    sConPassthrough = 0;
}

static void gdx3ds_menu_tick_body(void) {
    const u32 held = hidKeysHeld();
    const int touching = (held & KEY_TOUCH) != 0;

    if (sScreenOff) {
        if (touching && !sPrevTouching) {
            MenuLeaveOff();
            sWakeSwallow = 1; /* this press must not also hit a widget */
        }
        sPrevTouching = touching;
        if (!sScreenOff && sDirty) {
            PaintAll();
        }
        return;
    }

    if (touching && !sPrevTouching && !sWakeSwallow) {
        touchPosition pos;
        hidTouchRead(&pos);
        if (pos.px < 320 && pos.py < 240) {
            const int col = pos.px / 8 + 1;
            const int row = pos.py / 8 + 1;
            MenuHandleTouch(row, col);
        }
    }
    if (!touching) {
        sWakeSwallow = 0;
    }
    sPrevTouching = touching;

    /* INPUT capture: bind the first fresh key press from the bindable set. */
    if (sCaptureAction >= 0) {
        const u32 fresh = held & ~sPrevHeld & gdx3ds_input_bindable_mask();
        if (fresh != 0) {
            const u32 key = fresh & (~fresh + 1u); /* lowest set bit */
            char label[24];
            gdx3ds_input_action_set_mask(sCaptureAction, key);
            MenuPersistBind(sCaptureAction);
            MenuSaveCfg();
            MenuLog("[menu] input bind %s=%s (0x%lX)",
                    gdx3ds_input_action_name(sCaptureAction),
                    gdx3ds_input_key_label(key, label, (int)sizeof(label)),
                    (unsigned long)key);
            sCaptureAction = -1;
            sDirty = 1;
        }
    }
    sPrevHeld = held;

    /* Repaint policy (MENU-PERF): full repaint on change; ~1 Hz refresh for the live
     * pages (STATUS numbers, LOG tail in live mode) and the row-1 fps line. */
    const u64 now = svcGetSystemTick();
    const int periodic = (now - sLastPeriodicTick) >= (u64)(1000.0 * CPU_TICKS_PER_MSEC);
    if (sDirty) {
        PaintAll();
    } else if (periodic &&
               (sTab == TAB_STATUS || (sTab == TAB_LOG && sLogSkip == 0) || sFpsDirty)) {
        PaintBegin();
        PaintRow(MENU_ROW_FPS, "%s", gdx3ds_fps_hud_get_enabled() ? sFpsLine : "");
        if (sTab == TAB_STATUS) {
            PaintStatus();
        } else if (sTab == TAB_LOG) {
            PaintLog();
        }
        PaintEnd();
        sFpsDirty = 0;
    }
    if (periodic) {
        sLastPeriodicTick = now;
    }
}
