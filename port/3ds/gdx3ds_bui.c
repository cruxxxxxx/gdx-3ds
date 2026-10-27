/* port/3ds/gdx3ds_bui.c — the bottom-screen product menus: the shell around the pure screens.
 * Contract (levers, ownership states, receipts) in gdx3ds_bui.h.
 *
 * SHAPE. One tick per menu tick while the idle view would own the bottom screen. The tick reads
 * input (touch always; buttons only while a flow owns them), turns it into actions on the MP layer
 * (gdx3ds_mpo_ui_*), rebuilds a plain-data view (BuiView) of everything a screen shows, and
 * repaints only when that view's bytes changed: a screen that is just sitting there costs a view
 * build and a hash per tick, nothing else. A repaint draws the whole screen into the DASH's
 * off-screen buffer (gdx3ds_bottomhud_shadow_fb) and copies the columns it wrote to the live one.
 *
 * MP. The MP layer is polled (gdx3ds_mpo_ui, a full snapshot) every 6 ticks while an MP screen is
 * up, after any action, and once a second otherwise (so a room joined some other way -- the
 * console's MP tab, [net] mp_auto -- still brings up the lobby). Screens follow the session's
 * phase; its user-facing message (GdxMpoUi.message) becomes a dialog or a toast when it changes. */
#include "gdx3ds_bui.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_bottomhud.h"
#include "gdx3ds_bui_gallery.h"
#include "gdx3ds_bui_kit.h"
#include "gdx3ds_bui_layout.h"
#include "gdx3ds_bui_mp30.h"
#include "gdx3ds_bui_screens.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_input_map.h" /* the SELECT shortcut stays off when SELECT is bound to an action */
#include "gdx3ds_menu.h"      /* SETTINGS: the player settings */
#include "gdx3ds_mp_game.h"
#include "gdx3ds_mp_online.h"
#include "gdx3ds_mp_uds.h"
#include "gdx_mp_lan.h"
#include "gdx_determinism.h" /* gdx_title_demo_set_held */
#include "gdx_racemap.h" /* gdx_title_demo_active */
#ifdef GDX3DS_BUI_EK
#include "gdx3ds_course_share.h"
#endif

#if __has_include("gdx3ds_build_id.h")
#include "gdx3ds_build_id.h"
#endif
#ifndef GDX3DS_BUILD_ID
#define GDX3DS_BUILD_ID "unknown"
#endif

/* port/input_bridge.c (GET_MODE(gGameMode)). Weak like gdx3ds_bottomhud.c's. */
extern int gdx_input_game_mode(void) __attribute__((weak));
extern int gdx3ds_mpp_input_locked(void) __attribute__((weak));
extern int gdx3ds_mpp_holding(void) __attribute__((weak));
extern int gdx3ds_mpp_active(void) __attribute__((weak));

/* The game's own data for Home's portrait and the machine picker (decomp/src/game). The Machine
 * layout mirrors decomp/include/unk_structs.h (size 0x16). */
typedef struct BuiGameMachine {
    int16_t customType;
    uint8_t shadowType;
    uint8_t boostersType;
    uint8_t red[4];
    uint8_t green[4];
    uint8_t blue[4];
    uint8_t number;
    int8_t machineStats[3];
    int16_t weight;
} BuiGameMachine;
_Static_assert(sizeof(BuiGameMachine) == 0x16, "Machine layout (decomp unk_structs.h)");
extern short gPlayerCharacters[4];
extern signed char gCupNumDifficultiesCleared[4];
extern signed char gSettingEverythingUnlocked;
extern const signed char kMachineSelectCharacterList[];
extern BuiGameMachine gMachines[30];

#define BUI_KEYS_DPAD (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)
#define BUI_DEV_COMBO (KEY_L | KEY_R | KEY_SELECT)
#define BUI_DEV_COMBO_MS 1500 /* card-4b: wall time (was 90 menu ticks) */
#define BUI_MP_POLL_TICKS 6
#define BUI_IDLE_POLL_TICKS 60
#define BUI_TOAST_TICKS 180
#define BUI_RECEIPT_TICKS 600
#define BUI_WATCHDOG_POLLS 4

enum { SHOWN_NONE = 0, SHOWN_LOGO = -1 };

/* ---- levers ----------------------------------------------------------------------------- */
static int sEnabled = 0;
static int sDevButton = 0;
static int sStereoButton = 0; /* card-4 [debug] bui_3d_button (Toggle3d), default off */
/* gfx/gdx3ds_stereo.cpp (a C++ header): the Home 3D button's state */
int gdx3ds_stereo_user_on(void) __attribute__((weak));
void gdx3ds_stereo_set_user_on(int on) __attribute__((weak));
static int sGallery = 0;
static int sLogPaints = 0;
static int sHoldMs = 1500; /* mp-menus [ui] hold_confirm_ms: START RACE / LEAVE are held this long, 0 = a press */
static int sModel = BUI_MODEL_O3DS;
static char sVersion[24];

/* ---- state ------------------------------------------------------------------------------- */
static int sScreen = BUI_SCR_NONE; /* the open flow; NONE = ambient (Home or the logo) */
static int sFocus = BUI_W_NONE;
static int sFocusMoved = 0;  /* the user moved the focus (d-pad / touch) since the screen opened */
static int sNearbySeen = 0;  /* NEARBY: the room count last tick (FollowNearbyRooms) */
static int sShown = SHOWN_NONE;    /* what the live buffer holds: a screen id, the logo, or nothing known */
static uint32_t sPaintedKey = 0;
static BuiWidgets sWidgets;
static int sReturnScreen = BUI_SCR_MP_CHOOSE; /* where a closed room sends the flow */
/* RESULTS (M3): the race count it was opened for, the pick round then (a new round leaves it), a
 * host action taken while the session still lingers after the race (done once the room is back),
 * and the host's name (the HOST LOST dialog). */
static int sResultsFor = 0;
static int sResultsRound = 0;
static int sResultsPending = BUI_W_NONE;
static char sHostName[BUI_NAME_CAP + 1];

static GdxMpoUi sUi;
static int sUiValid = 0;
static int sUiDirty = 1;
static unsigned long sUiPolledAt = 0;
static char sLastMessage[128];
static int sMessageSeeded = 0;

static int sDialog = BUI_DLG_NONE;
static int sDialogFocus = 0;
static char sDialogName[24];
static char sDialogCode[16];
static char sDialogMine[8];
static char sDialogExtra[44];
static char sToast[48];
static int sToastTone = 0;
static int sToastIcon = 0;
static unsigned long sToastUntil = 0;

static char sEntry[BUI_NAME_CAP + 1];
static int sHostMedium = BUI_MEDIUM_WIFI;
static int sRoomSize = 4;
static int sNearbyPage = 1;
static int sCup = 0;
static int sPickSlot = 0;
static char sJoinName[BUI_NAME_CAP + 1];
static char sJoinCode[BUI_CODE_CAP + 1];
static int sJoinMedium = BUI_MEDIUM_WIFI;
static int sCodeDirect = 0; /* the code screen joins by address (JOIN BY ADDRESS), not by server */
/* The last online attempt, for the SERVER dialog's RETRY. */
enum { ONLINE_NONE = 0, ONLINE_HOST, ONLINE_JOIN, ONLINE_DIRECT };
static int sLastOnline = ONLINE_NONE;
static char sLastOnlineCode[BUI_CODE_CAP + 1];
/* mp-menus: the top screen's VS icon asked for MULTIPLAYER (gdx_mp_vs_menu_open) */
static volatile int sVsPending = 0;
static int sVsWaitTicks = 0; /* card-4b: ticks a VS request has waited for Home's place */
/* card-4b: keys held when a flow takes the buttons over (the A that confirmed a stock pick on the
 * top screen freezes the picker, and the room's buttons are the bottom screen's in that same
 * tick): not presses of the bottom screen's until they are let go. */
static int sOwnedLastTick = 0;
static uint32_t sSwallowKeys = 0;
/* mp-menus hold-to-confirm: the widget held, by which key (0 = a touch), since when */
static int sHoldId = BUI_W_NONE;
static uint32_t sHoldKey = 0;
static u64 sHoldStartMs = 0;
static int sHoldPct = 0;
static int sPresentPending = 0; /* a staged paint's first half is in the shadow, not yet shown */
/* the view on screen (the keys act on what the player sees) */
static BuiView sShownView;
static int sShownViewValid = 0;
/* mp-menus: the course editor's import (gdx3ds_course_share.h) */
static int sCsPage = 1;
#ifdef GDX3DS_BUI_EK
static GdxCsUi sCs;
#endif

static unsigned long sTicks = 0;
static unsigned long sMenuTicks = 0;
static unsigned long sLastBuiTick = 0;
static uint32_t sPrevHeld = 0;
static int sPrevTouching = 0;
static uint32_t sRepeatKey = 0;
static int sRepeatTicks = 0;

/* the game-key mask (read by gdx3ds_os_poll_input) */
static volatile int sOwnsButtons = 0;
static volatile unsigned sPollsSinceTick = 0;
static int sStickHidden = 0;
static BuiKeyMask sMask;
static BuiCombo sCombo;

/* gallery */
static int sGalleryIndex = -1; /* -1 = not started; count = done */

/* receipts */
static unsigned long sRcTicks = 0;
static unsigned long sRcPaints = 0;
static u64 sRcIdleTicks = 0;
static u64 sRcPaintTotal = 0;
static u64 sRcPaintMax = 0;
static u64 sRcPresentTotal = 0;
static u64 sRcPresentMax = 0;

static void BuiLog(const char* fmt, ...) {
    char line[200];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n >= (int) sizeof(line)) {
        n = (int) sizeof(line) - 1;
    }
    svcOutputDebugString(line, n);
    gdx3ds_filelog_write(line, (size_t) n);
}

static unsigned long TicksToUs(u64 t) {
    return (unsigned long) ((t * 1000u) / (u64) CPU_TICKS_PER_MSEC);
}

static void CopyCaps(char* out, size_t cap, const char* in) {
    size_t n = 0;
    if (cap == 0) {
        return;
    }
    for (; in != NULL && *in != '\0' && n + 1 < cap; in++) {
        out[n++] = (char) toupper((unsigned char) *in);
    }
    out[n] = '\0';
}

/* The fonts have no '@' ("bottom-ui@effbcdd"), '[' ... (the MP layer's "[net] mp_uds"): a space
 * reads the same. */
static void ReplaceUndrawable(char* s) {
    for (; *s != '\0'; s++) {
        if (strchr("@$*;<>[\\]^`{|}", *s) != NULL) {
            *s = ' ';
        }
    }
}

/* ---- init / levers ----------------------------------------------------------------------- */

void gdx3ds_bui_init(void) {
    bool isNew = false;
    sEnabled = gdx3ds_config_get_bool("ui", "bottom_menus", 1) ? 1 : 0;
    sDevButton = gdx3ds_config_get_bool("debug", "dev_console", 0) ? 1 : 0;
    gdx3ds_bottomhud_set_bui(sEnabled, sDevButton);
    gdx3ds_bui_script_init();
    if (!sEnabled) {
        BuiLog("[bui] bottom_menus=0: the old bottom screen (idle logo, DEBUG, SELECT long-press)");
        return;
    }
    sGallery = gdx3ds_config_get_bool("debug", "bui_gallery", 0) ? 1 : 0;
    sLogPaints = gdx3ds_config_get_bool("debug", "bui_log", 0) ? 1 : 0;
    sHoldMs = gdx3ds_config_get_int("ui", "hold_confirm_ms", 1500);
    sHoldMs = sHoldMs < 0 ? 0 : sHoldMs > 10000 ? 10000 : sHoldMs;
    APT_CheckNew3DS(&isNew);
    sModel = isNew ? BUI_MODEL_N3DS : BUI_MODEL_O3DS;
    sStereoButton = (gdx3ds_config_get_bool("debug", "bui_3d_button", 0) && &gdx3ds_stereo_user_on != NULL &&
                     &gdx3ds_stereo_set_user_on != NULL) ? 1 : 0;
#ifdef GDX3DS_BUI_EK
    snprintf(sVersion, sizeof(sVersion), "%.20s EK", GDX3DS_BUILD_ID);
#else
    snprintf(sVersion, sizeof(sVersion), "%.23s", GDX3DS_BUILD_ID);
#endif
    CopyCaps(sVersion, sizeof(sVersion), sVersion);
    ReplaceUndrawable(sVersion);
    BuiLog("[bui] init bottom_menus=1 dev_console=%d gallery=%d model=%s version=%s hold_confirm_ms=%d", sDevButton,
           sGallery, sModel == BUI_MODEL_N3DS ? "n3ds" : "o3ds", sVersion, sHoldMs);
}

int gdx3ds_bui_enabled(void) {
    return sEnabled;
}

static int sDemoHeld = 0;

/* A flow open on the title (or the main menu) holds the attract demo back: the flow has the
 * buttons, so the title sees no input and would start the demo under the player. */
static void HoldTitleDemo(int hold) {
    if (hold == sDemoHeld) {
        return;
    }
    sDemoHeld = hold;
    gdx_title_demo_set_held(hold);
    BuiLog("[bui] attract demo %s", hold ? "held (a flow is open)" : "released");
}

/* L+R+SELECT, with a receipt on every edge (card-4b, hw: "i tried doing L+R+Select and saw
 * nothing" and the log could not say whether the chord was ever held, or let go too early). */
static int DevComboStep(uint32_t held) {
    uint32_t heldMs = 0;
    const int ev = bui_combo_step(&sCombo, held, BUI_DEV_COMBO, (uint32_t) osGetTime(), BUI_DEV_COMBO_MS, &heldMs);
    if (ev == BUI_COMBO_DOWN) {
        BuiLog("[menu] chord L+R+SELECT down (hold %d ms for the developer console)", BUI_DEV_COMBO_MS);
    } else if (ev == BUI_COMBO_RELEASED_EARLY) {
        BuiLog("[menu] chord L+R+SELECT released after %lu ms (needs %d): no toggle", (unsigned long) heldMs,
               BUI_DEV_COMBO_MS);
    } else if (ev == BUI_COMBO_FIRED) {
        BuiLog("[menu] chord L+R+SELECT held=%lums -> console toggle", (unsigned long) heldMs);
        return 1;
    }
    return 0;
}

int gdx3ds_bui_dev_combo(uint32_t held) {
    sMenuTicks++;
    if (!sEnabled) {
        return 0;
    }
    if (sDemoHeld && sLastBuiTick + 1 < sMenuTicks) {
        HoldTitleDemo(0); /* the menus stopped ticking (the console, a race): never hold it stale */
    }
    return DevComboStep(held);
}

void gdx3ds_bui_invalidate(void) {
    sShown = SHOWN_NONE;
}

/* ---- the game-key mask -------------------------------------------------------------------- */

/* A timed-out stock machine pick froze its screen: hold the game's buttons back (multiplayer). */
static int PickInputLocked(void) {
    return &gdx3ds_mpp_input_locked != NULL && gdx3ds_mpp_input_locked();
}

/* uipolish: B closes SETTINGS / the developer console outside a race (gdx3ds_menu_b_exits); that
 * B, and the rest of its hold, never reaches the game. */
static BuiKeyMask sMenuBMask;

uint32_t gdx3ds_bui_game_key_mask(uint32_t held) {
    int owns;
    if (!sEnabled) {
        return 0;
    }
    owns = (sOwnsButtons && sPollsSinceTick < BUI_WATCHDOG_POLLS) || PickInputLocked();
    sPollsSinceTick++;
    sStickHidden = owns;
    return bui_keymask_step(&sMask, owns, held, ~(uint32_t) KEY_TOUCH) |
           bui_keymask_step(&sMenuBMask, gdx3ds_menu_b_exits(), held, KEY_B);
}

int gdx3ds_bui_game_stick_hidden(void) {
    return sEnabled && sStickHidden;
}

/* ---- where the game is -------------------------------------------------------------------- */

/* TITLE or MAIN MENU, no attract demo: Home's place, and where a flow may take the buttons. */
static int AmbientMode(void) {
    int mode;
    if (&gdx_input_game_mode == NULL || gdx_title_demo_active()) {
        return 0;
    }
    mode = gdx_input_game_mode();
    return mode == 0x00 || mode == 0x07;
}

/* mp-menus: the Expansion Kit course editor with [ek] course_share on: its bottom menu (import). */
#ifdef GDX3DS_BUI_EK
static int EditorMenuOn(void) {
    return gdx3ds_cshare_enabled() && &gdx_input_game_mode != NULL && gdx_input_game_mode() == 0x0D;
}
#else
static int EditorMenuOn(void) {
    return 0;
}
#endif

static int IsCsScreen(int s) {
    return s == BUI_SCR_CS_FILES || s == BUI_SCR_CS_SLOT || s == BUI_SCR_CS_SCAN;
}

/* A stock pick holds the top screen but nobody picks on it (confirmed and frozen, or on its way
 * in or out): the room's own buttons (START RACE, SETTINGS ...) are the bottom screen's. */
static int PickerParked(void) {
    return &gdx3ds_mpp_holding != NULL && &gdx3ds_mpp_active != NULL && gdx3ds_mpp_holding() &&
           gdx3ds_mpp_active() == 0;
}

static int IsMpScreen(int s) {
    return s >= BUI_SCR_MP_CHOOSE && s <= BUI_SCR_RESULTS;
}

static int IsRoomScreen(int s) {
    return s == BUI_SCR_LOBBY || s == BUI_SCR_MACHINE || s == BUI_SCR_SETTINGS || s == BUI_SCR_COURSE ||
           s == BUI_SCR_LAUNCH || s == BUI_SCR_RESULTS;
}

static int HasSpinner(int s) {
    return s == BUI_SCR_NEARBY || s == BUI_SCR_JOINING || s == BUI_SCR_LAUNCH;
}

/* ---- MP snapshot ------------------------------------------------------------------------- */

static void PollMp(int force) {
    const unsigned long every = IsMpScreen(sScreen) ? BUI_MP_POLL_TICKS : BUI_IDLE_POLL_TICKS;
    if (!force && !sUiDirty && sUiValid && sTicks - sUiPolledAt < every) {
        return;
    }
    gdx3ds_mpo_ui(&sUi);
    sUiValid = 1;
    sUiDirty = 0;
    sUiPolledAt = sTicks;
}

static int MpUnlockedMachines(void) {
    int sum = 0;
    int i;
    int n;
    if (gSettingEverythingUnlocked) {
        return BUI_MACHINES;
    }
    for (i = 0; i < 4; i++) {
        sum += gCupNumDifficultiesCleared[i];
    }
    n = (sum / 3) * 6 + 6;
    return n > BUI_MACHINES ? BUI_MACHINES : n;
}

static int GridSlotOf(int character) {
    int i;
    for (i = 0; i < BUI_MACHINES; i++) {
        if (kMachineSelectCharacterList[i] == character) {
            return i;
        }
    }
    return 0;
}

/* ---- toasts, dialogs and the MP message ---------------------------------------------------- */

static void Toast(const char* text, int tone, int icon) {
    CopyCaps(sToast, sizeof(sToast), text);
    ReplaceUndrawable(sToast);
    sToastTone = tone;
    sToastIcon = icon;
    sToastUntil = sTicks + BUI_TOAST_TICKS;
}

static void OpenDialog(int dialog, const char* name, const char* code) {
    sDialog = dialog;
    sDialogFocus = bui_dialog_buttons(dialog) - 1; /* the A button (the last one); B is always the first */
    if (sDialogFocus < 0) {
        sDialogFocus = 0;
    }
    CopyCaps(sDialogName, sizeof(sDialogName), name != NULL ? name : "");
    CopyCaps(sDialogCode, sizeof(sDialogCode), code != NULL ? code : "");
    sDialogMine[0] = '\0';
    sDialogExtra[0] = '\0';
    BuiLog("[bui] dialog %d name=%s code=%s", dialog, sDialogName, sDialogCode);
}

/* "race ended: KENJI connection lost" -> "KENJI": the text between `after` and `before`. */
static void Between(char* out, size_t cap, const char* msg, const char* after, const char* before) {
    const char* a = strstr(msg, after);
    const char* b;
    size_t n;
    out[0] = '\0';
    if (a == NULL) {
        return;
    }
    a += strlen(after);
    b = before != NULL ? strstr(a, before) : NULL;
    n = b != NULL ? (size_t) (b - a) : strlen(a);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, a, n);
    out[n] = '\0';
}

/* Progress the screens already show (JOINING, the lobby, LAUNCH): no toast. */
static int IsProgressMessage(const char* msg) {
    static const char* const kProgress[] = {"creating a room", "joining ", "opening a ", "hosting ", "joined ",
                                            "starting ", "race over", "left the room",
                                            "race in progress"};
    size_t i;
    for (i = 0; i < sizeof(kProgress) / sizeof(kProgress[0]); i++) {
        if (strncmp(msg, kProgress[i], strlen(kProgress[i])) == 0) {
            return 1;
        }
    }
    return 0;
}

/* "race ended: KENJI connection lost" -> "KENJI'S 3DS" */
static void PeerOf(char* out, size_t cap, const char* msg, const char* tail) {
    char who[24];
    Between(who, sizeof(who), msg, "race ended: ", tail);
    if (who[0] == '\0') {
        snprintf(who, sizeof(who), "A PLAYER");
    }
    snprintf(out, cap, "%.16s'S 3DS", who);
}

static int ClassifyRaceEnd(const char* msg) {
    char who[24];
    if (strstr(msg, "out of sync with ") != NULL) {
        Between(who, sizeof(who), msg, "out of sync with ", NULL);
        OpenDialog(BUI_DLG_DESYNC, who, "SYNC");
    } else if (strstr(msg, " desync") != NULL) {
        Between(who, sizeof(who), msg, "race ended: ", " desync");
        OpenDialog(BUI_DLG_DESYNC, who, "SYNC");
    } else if (strstr(msg, "two players lost") != NULL) {
        OpenDialog(BUI_DLG_LOST, "TWO PLAYERS", "LOST");
    } else if (strstr(msg, "connection lost") != NULL) {
        PeerOf(sDialogName, sizeof(sDialogName), msg, " connection lost");
        OpenDialog(BUI_DLG_LOST, sDialogName, "LOST");
    } else if (strstr(msg, "this console was suspended") != NULL) {
        OpenDialog(BUI_DLG_SUSPEND, NULL, NULL);
    } else if (strstr(msg, " suspended") != NULL) {
        PeerOf(sDialogName, sizeof(sDialogName), msg, " suspended");
        OpenDialog(BUI_DLG_LOST, sDialogName, "SUSPEND");
    } else if (strstr(msg, "could not load the race") != NULL) {
        OpenDialog(BUI_DLG_NO_START, NULL, NULL);
    } else {
        return 0;
    }
    return 1;
}

/* "UPDATE REQUIRED - this server needs version 10 (you have 9) - <url>" /
 * "SERVER NEEDS AN UPDATE - it supports up to version 9 (you have 10)" (gdx_rv_update_message). */
static void OpenUpdateDialog(const char* msg) {
    const int serverOld = strncmp(msg, "SERVER", 6) == 0;
    const char* at = strstr(msg, "version ");
    const char* mine = strstr(msg, "(you have ");
    const char* url = mine != NULL ? strstr(mine, ") - ") : NULL;
    char need[24];
    need[0] = '\0';
    if (at != NULL) {
        Between(need, sizeof(need), at, "version ", " ");
    }
    OpenDialog(serverOld ? BUI_DLG_SERVER_OLD : BUI_DLG_UPDATE, need, NULL);
    if (mine != NULL) {
        Between(sDialogMine, sizeof(sDialogMine), mine, "(you have ", ")");
    }
    if (url != NULL && !serverOld) {
        CopyCaps(sDialogExtra, sizeof(sDialogExtra), url + 4);
    }
}

static void ClassifyMessage(const char* msg) {
    char who[24];
    if (ClassifyRaceEnd(msg)) {
        return;
    }
    if (strstr(msg, "you were dropped") != NULL) {
        OpenDialog(BUI_DLG_LOST, "THIS 3DS", "DROPPED");
    } else if (strncmp(msg, "room closed:", 12) == 0 && sUiValid && sUi.roomClosed) {
        return; /* the results stay up: their ROOM CLOSED bar says why, and LEAVE */
    } else if (strncmp(msg, "room closed:", 12) == 0 && sHostName[0] != '\0' && sResultsFor > 0) {
        OpenDialog(BUI_DLG_HOST_LOST, sHostName, NULL); /* the host went away during or after a race */
    } else if (strncmp(msg, "room closed:", 12) == 0) {
        OpenDialog(BUI_DLG_CLOSED, NULL, "HOST LOST");
    } else if (strncmp(msg, "UPDATE REQUIRED", 15) == 0 || strncmp(msg, "SERVER NEEDS AN UPDATE", 22) == 0) {
        OpenUpdateDialog(msg);
    } else if (strncmp(msg, "version mismatch", 16) == 0) {
        const char* host = sJoinName[0] != '\0' ? sJoinName : "THE HOST";
        if (strstr(msg, "(update this console)") != NULL) {
            OpenDialog(BUI_DLG_VERSION_NEWER, host, NULL);
        } else if (strstr(msg, "(the host needs an update)") != NULL) {
            OpenDialog(BUI_DLG_VERSION_OLDER, host, NULL);
        } else {
            OpenDialog(BUI_DLG_VERSION, host, NULL);
        }
    } else if (strncmp(msg, "WiFi is off", 11) == 0) {
        OpenDialog(BUI_DLG_WIFI_OFF, NULL, NULL);
    } else if (strstr(msg, "did not load in time") != NULL) {
        OpenDialog(BUI_DLG_NO_START, NULL, NULL);
    } else if (strncmp(msg, "no server set", 13) == 0) {
        OpenDialog(BUI_DLG_SERVER, NULL, "NO SERVER");
    } else if (strstr(msg, " ended the race") != NULL) {
        Between(who, sizeof(who), msg, "", " ended the race");
        snprintf(sToast, sizeof(sToast), "%.12s ENDED THE RACE", who);
        CopyCaps(sToast, sizeof(sToast), sToast);
        Toast(sToast, BUI_TONE_WARN, BUI_ICON_FLAG);
    } else if (strstr(msg, " removed at tick ") != NULL) {
        Between(who, sizeof(who), msg, "", " removed at tick ");
        snprintf(sToast, sizeof(sToast), "%.12s LEFT THE RACE", who);
        Toast(sToast, BUI_TONE_WARN, BUI_ICON_CPU);
    } else if (!IsProgressMessage(msg)) {
        Toast(msg, BUI_TONE_WARN, BUI_ICON_WARN);
    }
}

static void WatchMessage(void) {
    if (!sUiValid) {
        return;
    }
    if (!sMessageSeeded) {
        snprintf(sLastMessage, sizeof(sLastMessage), "%s", sUi.message);
        sMessageSeeded = 1;
        return;
    }
    if (strcmp(sUi.message, sLastMessage) == 0) {
        return;
    }
    snprintf(sLastMessage, sizeof(sLastMessage), "%s", sUi.message);
    BuiLog("[bui] mpo message: %s", sUi.message);
    if (sUi.message[0] != '\0') {
        ClassifyMessage(sUi.message);
    }
}

/* ---- screen changes ------------------------------------------------------------------------ */

static int DefaultFocus(int screen) {
    switch (screen) {
        case BUI_SCR_MP_CHOOSE:
            return sUi.lan ? BUI_W_CHOOSE_NEARBY
                           : (sUi.haveServer || sUi.haveDirect) ? BUI_W_CHOOSE_ONLINE : BUI_W_CHOOSE_NAME;
        case BUI_SCR_NEARBY:
            return sUi.nearbyCount > 0 ? BUI_W_NEARBY_ROW0 : BUI_W_NEARBY_HOST;
        case BUI_SCR_HOST:
            return sHostMedium == BUI_MEDIUM_UDS ? BUI_W_HOST_LOCAL : BUI_W_HOST_WIFI;
        case BUI_SCR_ONLINE:
            return sUi.haveServer ? BUI_W_ONLINE_HOST : BUI_W_ONLINE_DIRECT;
        case BUI_SCR_CODE:
        case BUI_SCR_NAME:
            return BUI_W_KEY0;
        case BUI_SCR_JOINING:
            return BUI_W_JOIN_CANCEL;
        case BUI_SCR_LOBBY:
            return BUI_W_LOBBY_MAIN;
        case BUI_SCR_MACHINE:
            return BUI_W_MACHINE0 + sPickSlot;
        case BUI_SCR_SETTINGS:
            return BUI_W_SET_COURSE;
        case BUI_SCR_COURSE:
            return BUI_W_COURSE0 + (sUi.course >= 0 ? sUi.course % 6 : 0);
        case BUI_SCR_RESULTS:
            return sUi.isHost ? BUI_W_RESULTS_REMATCH : BUI_W_NONE;
        case BUI_SCR_CS_FILES:
            return BUI_W_CS_ROW0;
        case BUI_SCR_CS_SLOT:
            return BUI_W_CS_LIBRARY;
        default:
            return BUI_W_NONE;
    }
}

static const char* ScreenName(int s) {
    static const char* const kNames[BUI_SCR_COUNT] = {"none", "home", "mp-choose", "nearby", "host", "online", "code",
                                                       "name", "joining", "lobby", "machine", "settings", "course",
                                                       "launch", "results", "editor", "cs-files", "cs-slot",
                                                       "cs-scan"};
    return (s >= 0 && s < BUI_SCR_COUNT) ? kNames[s] : "?";
}

/* A flow opening (or the results, after a race) builds the dialogs' whole shade table now, in
 * one go: its first dialog then costs what every later one does (on an Old 3DS the table's lazy
 * fill used to make that first dialog ~8 ms). */
static void PrepareDialogs(int from, int to) {
    const u64 t0 = svcGetSystemTick();
    if (!((from == BUI_SCR_NONE && to != BUI_SCR_NONE) || to == BUI_SCR_RESULTS)) {
        return;
    }
    if (bui_modal_prepare(65536)) {
        BuiLog("[bui] dialog shade table ready (%lu us)", TicksToUs(svcGetSystemTick() - t0));
    }
}

static void Go(int screen) {
    if (screen == sScreen) {
        return;
    }
    BuiLog("[bui] screen %s -> %s", ScreenName(sScreen), ScreenName(screen));
    PrepareDialogs(sScreen, screen);
    sScreen = screen;
    sFocus = DefaultFocus(screen);
    sFocusMoved = 0;
    sNearbySeen = sUiValid ? sUi.nearbyCount : 0;
    sRepeatKey = 0;
}

/* NEARBY opened before any room was heard: the focus sat on HOST A ROOM. When the first room
 * shows up, the focus moves to it (as DefaultFocus would have picked), unless the user has
 * moved the focus themselves. */
static void FollowNearbyRooms(void) {
    const int count = sUiValid ? sUi.nearbyCount : 0;
    if (sScreen == BUI_SCR_NEARBY && sDialog == BUI_DLG_NONE && sNearbySeen == 0 && count > 0 && !sFocusMoved &&
        sFocus == BUI_W_NEARBY_HOST) {
        sFocus = BUI_W_NEARBY_ROW0;
        BuiLog("[bui] nearby: first room found, focus -> the first room");
    }
    sNearbySeen = count;
}

static void CloseFlow(void) {
    Go(BUI_SCR_NONE);
    sDialog = BUI_DLG_NONE;
}

/* The session's phase moves the flow: into JOINING, the lobby, the launch, and back out when the
 * room is gone. A room joined some other way (the console's MP tab, [net] mp_auto) opens the
 * lobby too. */
/* After a race (a new one finished since the results were last opened): the results. */
static int ResultsDue(void) {
    return sUiValid && sUi.racesDone > sResultsFor;
}

/* o3ds60 [fvse] results: the race's SE tally per player (game/gdx3ds_fullview_game.c; weak: the
 * host tests link no game). */
void gdx_fv_se_results_receipt(void) __attribute__((weak));

static void OpenResults(void) {
    if (&gdx_fv_se_results_receipt != NULL) {
        gdx_fv_se_results_receipt();
    }
    sResultsFor = sUi.racesDone;
    sResultsRound = sUi.pickRound;
    sResultsPending = BUI_W_NONE;
    Go(BUI_SCR_RESULTS);
}

/* The results stay until the host moves the room on: a new pick round (MACHINES), the course
 * picker (COURSE), or the next START (REMATCH: the LAUNCH rule below). */
static int ResultsLeft(void) {
    return sUi.pickRound != sResultsRound || sUi.pickStep == GDX_MPO_STEP_COURSE ||
           sUi.hostPicking == GDX_LB_PICK_COURSE;
}

static void FollowPhase(void) {
    const int phase = sUiValid ? sUi.phase : GDX_MPO_OFF;
    const int entry = sScreen == BUI_SCR_NEARBY || sScreen == BUI_SCR_HOST || sScreen == BUI_SCR_ONLINE ||
                      sScreen == BUI_SCR_CODE || sScreen == BUI_SCR_MP_CHOOSE;
    if (sUiValid && sUi.phase >= GDX_MPO_ROOM && !sUi.isHost && sUi.player[0].present) {
        CopyCaps(sHostName, sizeof(sHostName), sUi.player[0].name);
    }
    if (phase == GDX_MPO_CONNECTING && (entry || IsRoomScreen(sScreen))) {
        Go(BUI_SCR_JOINING); /* a join, or a rejoin of the room this console was away from too long */
    } else if ((phase == GDX_MPO_ROOM || phase == GDX_MPO_IDLE) && sUi.raceWait) {
        if (sScreen != BUI_SCR_JOINING) {
            Go(BUI_SCR_JOINING); /* the room races without us: JOINING says so until it ends */
        }
        return;
    } else if (phase >= GDX_MPO_ROOM && sScreen == BUI_SCR_JOINING) {
        sResultsFor = sUi.racesDone; /* the races before this console came in are not its results */
        Go(phase == GDX_MPO_LAUNCH || phase == GDX_MPO_RACE ? BUI_SCR_LAUNCH : BUI_SCR_LOBBY);
    } else if ((phase == GDX_MPO_ROOM || phase == GDX_MPO_LINGER) && sScreen != BUI_SCR_RESULTS && ResultsDue()) {
        OpenResults();
    } else if ((phase == GDX_MPO_ROOM || phase == GDX_MPO_LINGER) && sScreen == BUI_SCR_RESULTS && ResultsLeft()) {
        Go(BUI_SCR_LOBBY);
    } else if ((phase == GDX_MPO_ROOM || phase == GDX_MPO_LINGER) && !IsRoomScreen(sScreen)) {
        Go(BUI_SCR_LOBBY);
    } else if ((phase == GDX_MPO_ROOM || phase == GDX_MPO_LINGER) && sScreen == BUI_SCR_LAUNCH) {
        Go(BUI_SCR_LOBBY);
    } else if ((phase == GDX_MPO_LAUNCH || phase == GDX_MPO_RACE) && sScreen != BUI_SCR_LAUNCH) {
        Go(BUI_SCR_LAUNCH);
    } else if ((phase == GDX_MPO_IDLE || phase == GDX_MPO_OFF) && (IsRoomScreen(sScreen) || sScreen == BUI_SCR_JOINING)) {
        Go(sReturnScreen);
    }
    if (phase == GDX_MPO_IDLE || phase == GDX_MPO_OFF) {
        sResultsFor = 0; /* a new room counts its races from one */
    }
    if (!IsRoomScreen(sScreen) && sScreen != BUI_SCR_JOINING && sUiValid && sUi.phase >= GDX_MPO_ROOM) {
        Go(BUI_SCR_LOBBY);
    }
}

/* ---- MP actions ---------------------------------------------------------------------------- */

static void SetMachine(int character) {
    int steps;
    PollMp(1);
    steps = (character - sUi.machine + BUI_MACHINES) % BUI_MACHINES;
    while (steps-- > 0) {
        gdx3ds_mpo_ui_cycle_machine(1);
    }
    sUiDirty = 1;
    BuiLog("[bui] machine -> %d (%s)", character, bui_pilot_name(character));
}

static void SetCourse(int course) {
    int steps;
    PollMp(1);
    steps = (course - sUi.course + 24) % 24;
    while (steps-- > 0) {
        gdx3ds_mpo_ui_cycle_course(1);
    }
    sUiDirty = 1;
    BuiLog("[bui] course -> %d (%s)", course, bui_course_name(course));
}

static void HostRoom(void) {
    gdx3ds_mpo_ui_set_room_size(sRoomSize);
    CopyCaps(sJoinName, sizeof(sJoinName), sUi.myName);
    sJoinCode[0] = '\0';
    sJoinMedium = sHostMedium;
    sReturnScreen = BUI_SCR_NEARBY;
    if (sHostMedium == BUI_MEDIUM_UDS) {
        gdx3ds_mpo_ui_uds_host();
    } else {
        gdx3ds_mpo_ui_lan_host();
    }
    sUiDirty = 1;
    BuiLog("[bui] host %s size=%d", sHostMedium == BUI_MEDIUM_UDS ? "local" : "wifi", sRoomSize);
}

static void JoinRow(int row) {
    const int abs = (sNearbyPage - 1) * BUI_ROOMS_PER_PAGE + row;
    const GdxMpoUiNearby* r;
    if (abs < 0 || abs >= sUi.nearbyCount) {
        return;
    }
    r = &sUi.nearby[abs];
    if (!r->compatible) {
        CopyCaps(sJoinName, sizeof(sJoinName), r->name);
        OpenDialog(BUI_DLG_VERSION, sJoinName, NULL);
        return;
    }
    if (!r->joinable && strcmp(r->whyNot, "racing") != 0) {
        char t[48];
        snprintf(t, sizeof(t), "%s'S ROOM IS FULL", r->name); /* a racing room is waited for (JOINING) */
        Toast(t, BUI_TONE_WARN, BUI_ICON_PEOPLE);
        return;
    }
    CopyCaps(sJoinName, sizeof(sJoinName), r->name);
    sJoinCode[0] = '\0';
    sJoinMedium = r->medium == GDX_NEARBY_MEDIUM_UDS ? BUI_MEDIUM_UDS : BUI_MEDIUM_WIFI;
    sReturnScreen = BUI_SCR_NEARBY;
    gdx3ds_mpo_ui_lan_join(abs);
    sUiDirty = 1;
    BuiLog("[bui] join row %d (%s)", abs, sJoinName);
}

static void LeaveRoom(void) {
    gdx3ds_mpo_ui_leave();
    sUiDirty = 1;
    Go(sReturnScreen);
}

/* ---- per-screen actions -------------------------------------------------------------------- */

static void Back(void);

static void EntryType(char c, int cap) {
    const size_t n = strlen(sEntry);
    if ((int) n < cap && n + 1 < sizeof(sEntry)) {
        if (c == ' ' && n == 0) {
            return; /* no leading space */
        }
        sEntry[n] = c;
        sEntry[n + 1] = '\0';
    }
}

static void EntryDelete(void) {
    const size_t n = strlen(sEntry);
    if (n > 0) {
        sEntry[n - 1] = '\0';
    }
}

static void SubmitName(void) {
    size_t n = strlen(sEntry);
    while (n > 0 && sEntry[n - 1] == ' ') {
        sEntry[--n] = '\0';
    }
    if (n == 0) {
        return;
    }
    if (gdx3ds_mpo_ui_set_name(sEntry)) {
        if (gdx3ds_config_save(GDX3DS_CONFIG_DEFAULT_PATH) != 0) {
            BuiLog("[bui] WARNING: ini save failed (%s)", GDX3DS_CONFIG_DEFAULT_PATH);
        }
        Toast("NAME SAVED", BUI_TONE_OK, BUI_ICON_PENCIL);
    } else {
        Toast("LEAVE THE ROOM TO CHANGE YOUR NAME", BUI_TONE_WARN, BUI_ICON_WARN);
    }
    sUiDirty = 1;
    Go(BUI_SCR_MP_CHOOSE);
}

static void OpenNearby(void) {
    sNearbyPage = 1;
    Go(BUI_SCR_NEARBY);
}

static void OpenHostSheet(void) {
    sHostMedium = (sUi.uds && sUi.udsError == 0) ? BUI_MEDIUM_UDS : BUI_MEDIUM_WIFI;
    sRoomSize = gdx3ds_mpo_ui_room_size();
    Go(BUI_SCR_HOST);
}

static void OpenMachine(void) {
    PollMp(1);
    sPickSlot = GridSlotOf(sUi.machine);
    Go(BUI_SCR_MACHINE);
}

/* Home's 3D ON / OFF button, [stereo] enabled saved to the ini, effective on the next frame
 * (o3ds60, hw a3b4531: that Old 3DS's slider reads 0.39 whatever its position, so 3D could not be
 * turned off). card-4: a developer lever now, [debug] bui_3d_button = 1 (default 0): players turn
 * 3D on and off with the 3D slider ([stereo] enabled, default 1 from [perf] preset = auto). */
static void Toggle3d(void) {
    const int on = gdx3ds_stereo_user_on() ? 0 : 1;
    gdx3ds_stereo_set_user_on(on);
    if (gdx3ds_config_save(GDX3DS_CONFIG_DEFAULT_PATH) != 0) {
        BuiLog("[bui] WARNING: ini save failed (%s)", GDX3DS_CONFIG_DEFAULT_PATH);
    }
    Toast(on ? "3D ON" : "3D OFF: 2D ON THE TOP SCREEN", BUI_TONE_OK, BUI_ICON_NONE);
    sUiDirty = 1;
}

/* ---- the course editor's import (mp-menus) -------------------------------------------------- */

#ifdef GDX3DS_BUI_EK
static void PollCs(void) {
    gdx3ds_cshare_ui(&sCs);
}

/* The import's step decides the screen (and the REPLACE dialog over the slot picker). */
static void FollowCs(void) {
    if (!IsCsScreen(sScreen)) {
        return;
    }
    if (!EditorMenuOn()) {
        gdx3ds_cshare_ui_close();
        CloseFlow();
        return;
    }
    PollCs();
    if (sCs.step != GDXCS_UI_REPLACE && sDialog == BUI_DLG_CS_REPLACE) {
        sDialog = BUI_DLG_NONE;
    }
    switch (sCs.step) {
        case GDXCS_UI_FILES: Go(BUI_SCR_CS_FILES); break;
        case GDXCS_UI_SLOT: Go(BUI_SCR_CS_SLOT); break;
        case GDXCS_UI_REPLACE:
            Go(BUI_SCR_CS_SLOT);
            if (sDialog != BUI_DLG_CS_REPLACE) {
                OpenDialog(BUI_DLG_CS_REPLACE, sCs.pendingName, sCs.replaceWhat);
            }
            break;
        case GDXCS_UI_SCAN: Go(BUI_SCR_CS_SCAN); break;
        default: CloseFlow(); break;
    }
}

static void OpenCsFiles(void) {
    sCsPage = 1;
    Go(BUI_SCR_CS_FILES);
    gdx3ds_cshare_ui_open_files();
    FollowCs();
}

static void OpenCsScan(void) {
    Go(BUI_SCR_CS_SCAN);
    gdx3ds_cshare_ui_open_scan();
    FollowCs();
}

static int CsPages(void) {
    return sCs.fileCount > 0 ? (sCs.fileCount + BUI_CS_ROWS_PER_PAGE - 1) / BUI_CS_ROWS_PER_PAGE : 1;
}

static void ActivateCsFiles(int id) {
    if (id >= BUI_W_CS_ROW0 && id < BUI_W_CS_ROW0 + BUI_CS_ROWS_PER_PAGE) {
        gdx3ds_cshare_ui_pick_file((sCsPage - 1) * BUI_CS_ROWS_PER_PAGE + (id - BUI_W_CS_ROW0));
    } else if (id == BUI_W_CS_UP && sCsPage > 1) {
        sCsPage--;
    } else if (id == BUI_W_CS_DOWN && sCsPage < CsPages()) {
        sCsPage++;
    } else if (id == BUI_W_CS_RESCAN) {
        gdx3ds_cshare_ui_open_files();
    }
    FollowCs();
}

static void ActivateCsSlot(int id) {
    if (id == BUI_W_CS_LIBRARY) {
        gdx3ds_cshare_ui_import_into(-1);
    } else if (id >= BUI_W_CS_CUP0 && id < BUI_W_CS_CUP0 + BUI_CS_SLOTS) {
        gdx3ds_cshare_ui_import_into(id - BUI_W_CS_CUP0);
    }
    FollowCs();
}

static void ActivateCsScan(int id) {
    if (id == BUI_W_CS_ZOOM) {
        gdx3ds_cshare_ui_scan_zoom();
    } else if (id == BUI_W_CS_RESTART) {
        gdx3ds_cshare_ui_scan_restart();
    } else if (id == BUI_W_CS_CANCEL) {
        gdx3ds_cshare_ui_scan_cancel();
    }
    FollowCs();
}

static void CsBack(void) {
    if (sScreen == BUI_SCR_CS_SCAN) {
        gdx3ds_cshare_ui_scan_cancel();
    } else {
        gdx3ds_cshare_ui_back();
    }
    FollowCs();
}

static void CsReplaceAnswer(int yes) {
    gdx3ds_cshare_ui_confirm(yes);
    FollowCs();
}

static void FillCs(BuiView* v) {
    int i;
    PollCs();
    v->cs.canWrite = sCs.canImport;
    v->cs.fileCount = (int8_t) (sCs.fileCount < BUI_CS_FILES_MAX ? sCs.fileCount : BUI_CS_FILES_MAX);
    if (sCsPage > CsPages()) {
        sCsPage = CsPages();
    }
    v->cs.page = (int8_t) sCsPage;
    v->cs.pages = (int8_t) CsPages();
    for (i = 0; i < v->cs.fileCount; i++) {
        BuiCsFileView* f = &v->cs.file[i];
        CopyCaps(f->name, sizeof(f->name), sCs.file[i].name);
        CopyCaps(f->leaf, sizeof(f->leaf), sCs.file[i].leaf);
        CopyCaps(f->why, sizeof(f->why), sCs.file[i].why);
        f->ok = sCs.file[i].ok;
        f->points = sCs.file[i].points;
        f->finished = sCs.file[i].finished;
    }
    CopyCaps(v->cs.pendingName, sizeof(v->cs.pendingName), sCs.pendingName);
    v->cs.pendingPoints = sCs.pendingPoints;
    v->cs.pendingFinished = sCs.pendingFinished;
    v->cs.pendingSlot = sCs.pendingSlot;
    v->cs.replacesDisk = sCs.replacesDisk;
    for (i = 0; i < BUI_CS_SLOTS; i++) {
        CopyCaps(v->cs.cup[i], sizeof(v->cs.cup[i]), sCs.cup[i]);
    }
    v->cs.scanHave = sCs.scanHave;
    v->cs.scanCount = sCs.scanCount;
    v->cs.scanPages = sCs.scanPages;
    v->cs.scanZoom = sCs.scanZoom;
    v->cs.cameraOn = sCs.cameraOn;
    CopyCaps(v->cs.status, sizeof(v->cs.status), sCs.status);
    ReplaceUndrawable(v->cs.status);
    for (i = 0; v->cs.status[i] != '\0'; i++) {
        if (v->cs.status[i] == '_') {
            v->cs.status[i] = ' ';
        }
    }
}

/* Every tick an import screen is up: the camera and the scan's progress. */
static void ServiceCs(void) {
    if (IsCsScreen(sScreen)) {
        gdx3ds_cshare_ui_tick();
        FollowCs();
    }
}

static void PaintCsPreview(uint16_t* fb, int repainted) {
    if (sShown == BUI_SCR_CS_SCAN) {
        gdx3ds_cshare_ui_scan_paint(fb, BUI_CS_PREVIEW_X, BUI_CS_PREVIEW_Y, BUI_CS_PREVIEW_W, BUI_CS_PREVIEW_H, repainted);
    }
}
#else
static void OpenCsFiles(void) {
}
static void OpenCsScan(void) {
}
static void ActivateCsFiles(int id) {
    (void) id;
}
static void ActivateCsSlot(int id) {
    (void) id;
}
static void ActivateCsScan(int id) {
    (void) id;
}
static void CsBack(void) {
}
static void CsReplaceAnswer(int yes) {
    (void) yes;
}
static void FillCs(BuiView* v) {
    (void) v;
}
static void ServiceCs(void) {
}
static void PaintCsPreview(uint16_t* fb, int repainted) {
    (void) fb;
    (void) repainted;
}
#endif

/* Home (and the course editor's menu, both shown with no flow open). MULTIPLAYER is the top
 * screen's VS icon (gdx_mp_vs_menu_open). */
static void ActivateHome(int id) {
    if (id == BUI_W_HOME_SETTINGS) {
        BuiLog("[bui] SETTINGS -> the player settings");
        gdx3ds_menu_open_settings();
    } else if (id == BUI_W_HOME_3D) {
        Toggle3d();
    } else if (id == BUI_W_ED_IMPORT && EditorMenuOn()) {
        OpenCsFiles();
    } else if (id == BUI_W_ED_SCAN && EditorMenuOn()) {
        OpenCsScan();
    }
}

static void ActivateChoose(int id) {
    if (id == BUI_W_CHOOSE_NEARBY && sUi.lan) {
        OpenNearby();
    } else if (id == BUI_W_CHOOSE_ONLINE && (sUi.haveServer || sUi.haveDirect)) {
        sReturnScreen = BUI_SCR_ONLINE;
        Go(BUI_SCR_ONLINE);
    } else if (id == BUI_W_CHOOSE_NAME) {
        CopyCaps(sEntry, sizeof(sEntry), sUi.myName);
        Go(BUI_SCR_NAME);
    }
}

static void ActivateNearby(int id) {
    const int pages = sUi.nearbyCount > 0 ? (sUi.nearbyCount + BUI_ROOMS_PER_PAGE - 1) / BUI_ROOMS_PER_PAGE : 1;
    if (id == BUI_W_NEARBY_HOST) {
        OpenHostSheet();
    } else if (id == BUI_W_NEARBY_UP && sNearbyPage > 1) {
        sNearbyPage--;
    } else if (id == BUI_W_NEARBY_DOWN && sNearbyPage < pages) {
        sNearbyPage++;
    } else if (id == BUI_W_NEARBY_RETRY) {
        gdx3ds_mpo_ui_nearby_shown();
        sUiDirty = 1;
    } else if (id >= BUI_W_NEARBY_ROW0 && id < BUI_W_NEARBY_ROW0 + BUI_ROOMS_PER_PAGE) {
        JoinRow(id - BUI_W_NEARBY_ROW0);
    }
}

static void ActivateHost(int id) {
    if (id == BUI_W_HOST_LOCAL && sUi.uds) {
        sHostMedium = BUI_MEDIUM_UDS;
    } else if (id == BUI_W_HOST_WIFI) {
        sHostMedium = BUI_MEDIUM_WIFI;
    } else if (id >= BUI_W_HOST_SIZE2 && id <= BUI_W_HOST_SIZE4) {
        sRoomSize = 2 + (id - BUI_W_HOST_SIZE2);
    } else if (id == BUI_W_HOST_CANCEL) {
        Back();
    } else if (id == BUI_W_HOST_OPEN) {
        HostRoom();
    }
}

static void ActivateOnline(int id) {
    if (id == BUI_W_ONLINE_HOST) {
        CopyCaps(sJoinName, sizeof(sJoinName), sUi.myName);
        sJoinMedium = BUI_MEDIUM_ONLINE;
        sReturnScreen = BUI_SCR_ONLINE;
        sLastOnline = ONLINE_HOST;
        gdx3ds_mpo_ui_host();
        sUiDirty = 1;
    } else if (id == BUI_W_ONLINE_JOIN) {
        sEntry[0] = '\0';
        sCodeDirect = 0;
        Go(BUI_SCR_CODE);
    } else if (id == BUI_W_ONLINE_DIRECT && sUi.haveDirect) {
        sEntry[0] = '\0';
        sCodeDirect = 1;
        Go(BUI_SCR_CODE);
    }
}

static void ActivateCode(int id) {
    if (id >= BUI_W_KEY0 && id < BUI_W_KEY0 + 32) {
        EntryType(kBuiCodeKeys[id - BUI_W_KEY0], BUI_CODE_CAP);
    } else if (id == BUI_W_ENTRY_DEL) {
        EntryDelete();
    } else if (id == BUI_W_ENTRY_CLEAR) {
        sEntry[0] = '\0';
    } else if (id == BUI_W_ENTRY_OK && strlen(sEntry) == BUI_CODE_CAP) {
        snprintf(sJoinCode, sizeof(sJoinCode), "%s", sEntry);
        sJoinName[0] = '\0';
        sJoinMedium = BUI_MEDIUM_ONLINE;
        sReturnScreen = BUI_SCR_ONLINE;
        sLastOnline = sCodeDirect ? ONLINE_DIRECT : ONLINE_JOIN;
        snprintf(sLastOnlineCode, sizeof(sLastOnlineCode), "%s", sEntry);
        if (sCodeDirect) {
            gdx3ds_mpo_ui_direct_join(sEntry);
        } else {
            gdx3ds_mpo_ui_join(sEntry);
        }
        sUiDirty = 1;
    }
}

static void ActivateName(int id) {
    if (id >= BUI_W_KEY0 && id < BUI_W_KEY0 + 40) {
        EntryType(kBuiNameKeys[id - BUI_W_KEY0], BUI_NAME_CAP);
    } else if (id == BUI_W_ENTRY_DEL) {
        EntryDelete();
    } else if (id == BUI_W_ENTRY_CLEAR) {
        Go(BUI_SCR_MP_CHOOSE);
    } else if (id == BUI_W_ENTRY_OK) {
        SubmitName();
    }
}

/* Stock picks: the host drives every step; guests pick on the top screen. */
/* Host: START WITHOUT the members that went quiet (the lobby offers it only then). */
static void StartWithoutAway(void) {
    if (sUi.startable > 0 || sUi.awayStartable < 2) {
        return;
    }
    BuiLog("[bui] START WITHOUT %s (%d players)", sUi.awayName, sUi.awayStartable);
    gdx3ds_mpo_ui_start_without_away();
    sUiDirty = 1;
}

static void ActivateStockLobby(int id) {
    if (id == BUI_W_LOBBY_KEEP) {
        BuiLog("[bui] KEEP machine %d (round %d)", sUi.keepMachine, sUi.pickRound);
        gdx3ds_mpo_ui_keep_pick();
        sUiDirty = 1;
        return;
    }
    if (!sUi.isHost) {
        return;
    }
    if (id == BUI_W_LOBBY_SIDE) {
        Go(BUI_SCR_SETTINGS);
    } else if (id == BUI_W_LOBBY_MAIN && sUi.pickStep == GDX_MPO_STEP_LOBBY) {
        BuiLog("[bui] MACHINE SELECT (round %d)", sUi.pickRound + 1);
        gdx3ds_mpo_ui_open_machine_select();
        sUiDirty = 1;
    } else if (id == BUI_W_LOBBY_MAIN && sUi.pickStep == GDX_MPO_STEP_READY && sUi.startable > 0) {
        BuiLog("[bui] START (%d players)", sUi.startable);
        gdx3ds_mpo_ui_start();
        sUiDirty = 1;
    } else if (id == BUI_W_LOBBY_MAIN && sUi.pickStep == GDX_MPO_STEP_READY) {
        StartWithoutAway();
    }
}

static void ActivateLobby(int id) {
    if (sUi.stockPick) {
        ActivateStockLobby(id);
        return;
    }
    if (id == BUI_W_LOBBY_SIDE) {
        if (sUi.isHost) {
            Go(BUI_SCR_SETTINGS);
        } else {
            OpenMachine();
        }
    } else if (id == BUI_W_LOBBY_MAIN) {
        if (sUi.isHost) {
            if (sUi.startable > 0) {
                BuiLog("[bui] START (%d players)", sUi.startable);
                gdx3ds_mpo_ui_start();
            } else {
                StartWithoutAway();
            }
        } else {
            gdx3ds_mpo_ui_toggle_ready();
        }
        sUiDirty = 1;
    } else if (id >= BUI_W_LOBBY_SLOT0 && id < BUI_W_LOBBY_SLOT0 + 4) {
        OpenMachine();
    }
}

static void ActivateMachine(int id) {
    if (id >= BUI_W_MACHINE0 && id < BUI_W_MACHINE0 + BUI_MACHINES) {
        const int slot = id - BUI_W_MACHINE0;
        sPickSlot = slot;
        if (slot < MpUnlockedMachines()) {
            SetMachine(kMachineSelectCharacterList[slot]);
            Go(BUI_SCR_LOBBY);
        }
    } else if (id == BUI_W_MACHINE_OK) {
        SetMachine(kMachineSelectCharacterList[sPickSlot]);
        Go(BUI_SCR_LOBBY);
    }
}

static void ActivateSettings(int id) {
    static const int kRacers[5] = {0, 4, 8, 16, 30};
    if (sUi.stockPick && id == BUI_W_SET_COURSE) {
        BuiLog("[bui] CHANGE COURSE (stock course select)");
        gdx3ds_mpo_ui_pick_course();
        sUiDirty = 1;
        Go(BUI_SCR_LOBBY);
    } else if (sUi.stockPick && id == BUI_W_SET_REPICK) {
        if (sUi.pickStep != GDX_MPO_STEP_MACHINES && sUi.pickStep != GDX_MPO_STEP_COURSE) {
            BuiLog("[bui] PICK MACHINES AGAIN (round %d)", sUi.pickRound + 1);
            gdx3ds_mpo_ui_open_machine_select();
            sUiDirty = 1;
            Go(BUI_SCR_LOBBY);
        }
    } else if (id == BUI_W_SET_COURSE) {
        sCup = sUi.course >= 0 ? sUi.course / 6 : 0;
        Go(BUI_SCR_COURSE);
    } else if (id == BUI_W_SET_LAPS_DOWN || id == BUI_W_SET_LAPS_UP) {
        gdx3ds_mpo_ui_cycle_laps(id == BUI_W_SET_LAPS_UP ? 1 : -1);
        sUiDirty = 1;
    } else if (id >= BUI_W_SET_RACERS0 && id < BUI_W_SET_RACERS0 + 5) {
        const int v = kRacers[id - BUI_W_SET_RACERS0];
        if (v <= BuiMpRacersCap(&sUi)) {
            BuiMpSetRacers(v);
            sUiDirty = 1;
        }
    }
}

static void ActivateCourse(int id) {
    if (id >= BUI_W_COURSE_CUP0 && id < BUI_W_COURSE_CUP0 + 4) {
        sCup = id - BUI_W_COURSE_CUP0;
    } else if (id >= BUI_W_COURSE0 && id < BUI_W_COURSE0 + 6) {
        SetCourse(sCup * 6 + (id - BUI_W_COURSE0));
        Go(BUI_SCR_SETTINGS);
    }
}

/* Host, on the results: REMATCH (the same picks, the same course), COURSE (the stock course select
 * again), MACHINES (everyone back into machine select). Taken at once in the room; while the
 * session still lingers after the race, as soon as the room is back. */
static void RunResultsAction(int id) {
    if (id == BUI_W_RESULTS_REMATCH) {
        gdx3ds_mpo_ui_start();
    } else if (id == BUI_W_RESULTS_COURSE) {
        gdx3ds_mpo_ui_pick_course();
    } else if (id == BUI_W_RESULTS_MACHINES) {
        gdx3ds_mpo_ui_open_machine_select();
    }
    sUiDirty = 1;
    BuiLog("[bui] results: %s", id == BUI_W_RESULTS_REMATCH ? "rematch" : id == BUI_W_RESULTS_COURSE ? "course" : "machines");
}

static void ActivateResults(int id) {
    if (id == BUI_W_RESULTS_LEAVE) {
        Back();
        return;
    }
    if (!sUi.isHost || sUi.roomClosed) {
        return;
    }
    if (sUi.phase == GDX_MPO_ROOM) {
        RunResultsAction(id);
    } else {
        sResultsPending = id;
        BuiLog("[bui] results: action %d waits for the room", id);
    }
}

static void ServiceResultsPending(void) {
    if (sResultsPending == BUI_W_NONE || !sUiValid || sUi.phase != GDX_MPO_ROOM || sScreen != BUI_SCR_RESULTS) {
        return;
    }
    RunResultsAction(sResultsPending);
    sResultsPending = BUI_W_NONE;
}

/* The SERVER dialog's RETRY: the online attempt that failed, again. */
static void RetryOnline(void) {
    BuiLog("[bui] retry online (%d)", sLastOnline);
    if (sLastOnline == ONLINE_HOST) {
        gdx3ds_mpo_ui_host();
    } else if (sLastOnline == ONLINE_JOIN) {
        gdx3ds_mpo_ui_join(sLastOnlineCode);
    } else if (sLastOnline == ONLINE_DIRECT) {
        gdx3ds_mpo_ui_direct_join(sLastOnlineCode);
    }
    sUiDirty = 1;
}

static void ActivateDialog(int button) {
    const int dialog = sDialog;
    sDialog = BUI_DLG_NONE;
    switch (dialog) {
        case BUI_DLG_WIRELESS_OFF:
            if (button == 0) {
                Go(BUI_SCR_MP_CHOOSE);
            } else {
                gdx3ds_mpo_ui_nearby_shown();
                sUiDirty = 1;
            }
            break;
        case BUI_DLG_SERVER:
            if (button == 0) {
                Go(BUI_SCR_MP_CHOOSE);
            } else {
                RetryOnline();
            }
            break;
        case BUI_DLG_CS_REPLACE:
            CsReplaceAnswer(button == 1);
            break;
        case BUI_DLG_DESYNC:
        case BUI_DLG_LEAVE_HOST:
        case BUI_DLG_LEAVE_GUEST:
            if ((dialog == BUI_DLG_DESYNC && button == 0) || (dialog != BUI_DLG_DESYNC && button == 1)) {
                LeaveRoom();
            }
            break;
        default:
            break;
    }
}

static void Activate(int id) {
    if (sDialog != BUI_DLG_NONE) {
        if (id >= BUI_W_DLG_BUTTON0 && id < BUI_W_DLG_BUTTON0 + 3) {
            ActivateDialog(id - BUI_W_DLG_BUTTON0);
        }
        return;
    }
    if (id == BUI_W_BACK) {
        Back();
        return;
    }
    switch (sScreen) {
        case BUI_SCR_NONE: ActivateHome(id); break;
        case BUI_SCR_MP_CHOOSE: ActivateChoose(id); break;
        case BUI_SCR_NEARBY: ActivateNearby(id); break;
        case BUI_SCR_HOST: ActivateHost(id); break;
        case BUI_SCR_ONLINE: ActivateOnline(id); break;
        case BUI_SCR_CODE: ActivateCode(id); break;
        case BUI_SCR_NAME: ActivateName(id); break;
        case BUI_SCR_JOINING:
            if (id == BUI_W_JOIN_CANCEL) {
                Back();
            }
            break;
        case BUI_SCR_LOBBY: ActivateLobby(id); break;
        case BUI_SCR_MACHINE: ActivateMachine(id); break;
        case BUI_SCR_SETTINGS: ActivateSettings(id); break;
        case BUI_SCR_COURSE: ActivateCourse(id); break;
        case BUI_SCR_RESULTS: ActivateResults(id); break;
        case BUI_SCR_CS_FILES: ActivateCsFiles(id); break;
        case BUI_SCR_CS_SLOT: ActivateCsSlot(id); break;
        case BUI_SCR_CS_SCAN: ActivateCsScan(id); break;
        default: break;
    }
}

static void Back(void) {
    if (sDialog != BUI_DLG_NONE) {
        ActivateDialog(0);
        return;
    }
    switch (sScreen) {
        case BUI_SCR_MP_CHOOSE: CloseFlow(); break;
        case BUI_SCR_NEARBY:
        case BUI_SCR_ONLINE: Go(BUI_SCR_MP_CHOOSE); break;
        case BUI_SCR_HOST: Go(BUI_SCR_NEARBY); break;
        case BUI_SCR_CODE:
            if (sEntry[0] != '\0') {
                EntryDelete();
            } else {
                Go(BUI_SCR_ONLINE);
            }
            break;
        case BUI_SCR_NAME:
            if (sEntry[0] != '\0') {
                EntryDelete();
            } else {
                Go(BUI_SCR_MP_CHOOSE);
            }
            break;
        case BUI_SCR_JOINING: LeaveRoom(); break;
        case BUI_SCR_LOBBY:
        case BUI_SCR_RESULTS:
            if (sUi.roomClosed) {
                BuiLog("[bui] results: leave the closed room");
                gdx3ds_mpo_ui_leave(); /* nothing to confirm: the room is gone already */
                sUiDirty = 1;
            } else {
                OpenDialog(sUi.isHost ? BUI_DLG_LEAVE_HOST : BUI_DLG_LEAVE_GUEST, NULL, NULL);
            }
            break;
        case BUI_SCR_MACHINE:
        case BUI_SCR_SETTINGS: Go(BUI_SCR_LOBBY); break;
        case BUI_SCR_COURSE: Go(BUI_SCR_SETTINGS); break;
        case BUI_SCR_CS_FILES:
        case BUI_SCR_CS_SLOT:
        case BUI_SCR_CS_SCAN: CsBack(); break;
        default: break;
    }
}

/* ---- buttons -------------------------------------------------------------------------------- */

/* D-pad edges with auto-repeat (24 ticks, then every 6). */
static uint32_t DpadPresses(uint32_t held, uint32_t down) {
    const uint32_t dirs = held & BUI_KEYS_DPAD;
    if ((down & BUI_KEYS_DPAD) != 0) {
        sRepeatKey = down & BUI_KEYS_DPAD;
        sRepeatTicks = 0;
        return sRepeatKey;
    }
    if (sRepeatKey == 0 || (dirs & sRepeatKey) == 0) {
        sRepeatKey = 0;
        return 0;
    }
    sRepeatTicks++;
    return (sRepeatTicks >= 24 && (sRepeatTicks - 24) % 6 == 0) ? sRepeatKey : 0;
}

static int KeyView(BuiView* v);

static void Navigate(uint32_t press) {
    const int dx = (press & KEY_RIGHT) ? 1 : (press & KEY_LEFT) ? -1 : 0;
    const int dy = (press & KEY_DOWN) ? 1 : (press & KEY_UP) ? -1 : 0;
    if (dx == 0 && dy == 0) {
        return;
    }
    if (sDialog != BUI_DLG_NONE) {
        const int n = bui_dialog_buttons(sDialog);
        if (dx != 0 && n > 1) {
            sDialogFocus = (sDialogFocus + dx + n) % n;
        }
        return;
    }
    {
        BuiView v;
        BuiNav nav;
        if (KeyView(&v)) {
            bui_screen_nav(&v, &sWidgets, dx, dy, &nav);
        } else {
            nav.focus = bui_widgets_nav(&sWidgets, sFocus, dx, dx != 0 ? 0 : dy, sScreen == BUI_SCR_LOBBY);
            nav.activate = BUI_W_NONE;
        }
        if (nav.focus != sFocus) {
            sFocusMoved = 1;
        }
        sFocus = nav.focus;
        if (nav.activate != BUI_W_NONE) {
            const BuiWidget* w = bui_widgets_find(&sWidgets, nav.activate);
            if (w != NULL && !(w->flags & BUI_WF_DISABLED)) {
                BuiLog("[bui] d-pad %d,%d on %s -> widget %d", dx, dy, ScreenName(sScreen), nav.activate);
                Activate(nav.activate);
            }
        }
    }
    if (sScreen == BUI_SCR_MACHINE && sFocus >= BUI_W_MACHINE0 && sFocus < BUI_W_MACHINE0 + BUI_MACHINES) {
        sPickSlot = sFocus - BUI_W_MACHINE0;
    }
}

/* The screen's widget the keys act on is decided on what is on screen (the hints drawn there),
 * with this tick's focus and entry. */
static int ShownScreen(void);

static int KeyView(BuiView* v) {
    if (!sShownViewValid || sShownView.screen != ShownScreen()) {
        return 0; /* the screen changed this tick: its keys from the next one */
    }
    *v = sShownView;
    v->focus = (int16_t) sFocus;
    v->dialog = (int8_t) sDialog;
    v->dialogFocus = (int8_t) sDialogFocus;
    v->buttonsOff = 0;
    snprintf(v->entry, sizeof(v->entry), "%s", sEntry);
    return 1;
}

/* ---- hold to confirm (mp-menus: START RACE, LEAVE / CLOSE the room) ------------------------ */

static int HoldWanted(const BuiView* v, int id) {
    return sHoldMs > 0 && bui_screen_hold_widget(v, id);
}

static void BeginHold(int id, uint32_t key) {
    sHoldId = id;
    sHoldKey = key;
    sHoldStartMs = osGetTime();
    sHoldPct = 0;
    BuiLog("[bui] hold %d start (%s, %d ms)", id, key != 0 ? "button" : "touch", sHoldMs);
}

static void EndHold(const char* why) {
    if (sHoldId == BUI_W_NONE) {
        return;
    }
    BuiLog("[bui] hold %d %s at %d%%", sHoldId, why, sHoldPct);
    sHoldId = BUI_W_NONE;
    sHoldKey = 0;
    sHoldPct = 0;
}

static int TouchOnWidget(const BuiWidget* w, int touching, int px, int py) {
    const int slop = 8;
    return touching && px >= w->x - slop && px < w->x + w->w + slop && py >= w->y - slop && py < w->y + w->h + slop;
}

/* Each tick of a hold: let go early (or the widget went away) and nothing happens; held for
 * hold_confirm_ms and it runs, once. */
static void ServiceHold(uint32_t held, int touching, int px, int py) {
    BuiView v;
    const BuiWidget* w;
    u64 elapsed;
    int id;
    if (sHoldId == BUI_W_NONE) {
        return;
    }
    if (sPresentPending) {
        return; /* a staged repaint's first tick: the widgets are the screen's, the dialog's come next */
    }
    w = bui_widgets_find(&sWidgets, sHoldId);
    if (!KeyView(&v) || w == NULL || (w->flags & BUI_WF_DISABLED) || !bui_screen_hold_widget(&v, sHoldId)) {
        EndHold("cancelled (gone)");
        return;
    }
    if (sHoldKey != 0 ? !((held & sHoldKey) && sOwnsButtons) : !TouchOnWidget(w, touching, px, py)) {
        EndHold("cancelled (let go early)");
        return;
    }
    elapsed = osGetTime() - sHoldStartMs;
    if (elapsed < (u64) sHoldMs) {
        sHoldPct = (int) ((elapsed * 20u) / (u64) sHoldMs) * 5;
        return;
    }
    id = sHoldId;
    sHoldPct = 100;
    EndHold("confirmed");
    Activate(id);
}

/* ---- keys ------------------------------------------------------------------------------------ */

static const struct {
    uint32_t mask;
    int key;
    const char* name;
} kBuiKeys[] = {
    {KEY_A, BUI_KEY_A, "A"}, {KEY_B, BUI_KEY_B, "B"}, {KEY_X, BUI_KEY_X, "X"},           {KEY_Y, BUI_KEY_Y, "Y"},
    {KEY_L, BUI_KEY_L, "L"}, {KEY_R, BUI_KEY_R, "R"}, {KEY_START, BUI_KEY_START, "START"},
};

/* A key down: the widget the screen's key map names for it (the one its hint sits on) runs, as
 * a tap on it would. 1 = the key was the screen's. */
static int PressKey(int key, uint32_t mask, const char* name) {
    BuiView v;
    const BuiWidget* w;
    int id;
    if (!KeyView(&v)) {
        BuiLog("[bui] press %s dropped: %s changed this tick", name, ScreenName(sScreen));
        return 0;
    }
    id = bui_screen_key(&v, key);
    if (id == BUI_W_NONE) {
        return 0;
    }
    BuiLog("[bui] press %s screen=%s dlg=%d focus=%d -> widget %d", name, ScreenName(v.screen), (int) sDialog, sFocus,
           id);
    if (id == BUI_W_BACK) {
        Back(); /* the header's ◀ B cell */
        return 1;
    }
    w = bui_widgets_find(&sWidgets, id);
    if (w == NULL || (w->flags & BUI_WF_DISABLED)) {
        BuiLog("[bui] press %s: widget %d is %s", name, id, w == NULL ? "not on the screen" : "disabled");
        return 1;
    }
    if (HoldWanted(&v, id)) {
        BeginHold(id, mask);
        return 1;
    }
    Activate(id);
    return 1;
}

/* Not hinted, kept from before the key map: the host's Y toggles its own READY. */
static void LegacyShortcuts(uint32_t down) {
    if ((down & KEY_Y) && sDialog == BUI_DLG_NONE && sScreen == BUI_SCR_LOBBY && sUi.isHost && !sUi.stockPick) {
        gdx3ds_mpo_ui_toggle_ready();
        sUiDirty = 1;
    }
}

static void HandleButtons(uint32_t rawHeld) {
    const uint32_t held = rawHeld & ~sSwallowKeys; /* FollowButtonOwner */
    const uint32_t down = held & ~sPrevHeld;
    const uint32_t nav = DpadPresses(held, down);
    size_t i;
    if ((held & BUI_DEV_COMBO) == BUI_DEV_COMBO) {
        return; /* the developer-console combo is on its way: no flow actions meanwhile */
    }
    if (sHoldId != BUI_W_NONE) {
        return; /* a hold runs: nothing else until it is let go or done */
    }
    for (i = 0; i < sizeof(kBuiKeys) / sizeof(kBuiKeys[0]); i++) {
        if ((down & kBuiKeys[i].mask) && PressKey(kBuiKeys[i].key, kBuiKeys[i].mask, kBuiKeys[i].name)) {
            return;
        }
    }
    LegacyShortcuts(down);
    Navigate(nav);
}

static void HandleTouch(int touching, int px, int py) {
    int id;
    if (!touching || sPrevTouching) {
        return;
    }
    id = bui_widgets_hit(&sWidgets, px, py);
    if (id < 0) {
        return;
    }
    {
        const BuiWidget* w = bui_widgets_find(&sWidgets, id);
        if (w == NULL || (w->flags & BUI_WF_DISABLED)) {
            return;
        }
        if (w->flags & BUI_WF_FOCUS) {
            if (sDialog != BUI_DLG_NONE) {
                sDialogFocus = id - BUI_W_DLG_BUTTON0;
            } else {
                sFocus = id;
                sFocusMoved = 1;
            }
        }
    }
    BuiLog("[bui] touch %d,%d -> widget %d on %s", px, py, id, ScreenName(sScreen));
    {
        BuiView v;
        if (sHoldId == BUI_W_NONE && KeyView(&v) && HoldWanted(&v, id)) {
            BeginHold(id, 0);
            return;
        }
    }
    Activate(id);
}

/* ---- the view ----------------------------------------------------------------------------- */

static const char* ShortWhyNot(const char* why) {
    if (why == NULL || why[0] == '\0') {
        return "";
    }
    if (strstr(why, "connect") != NULL) {
        return "WAITING FOR A PLAYER";
    }
    if (strstr(why, "update this console") != NULL) {
        return "UPDATE THIS 3DS";
    }
    if (strstr(why, "same one") != NULL) {
        return "EVERYONE NEEDS ONE BUILD";
    }
    if (strstr(why, "edition") != NULL) {
        return "A PLAYER RUNS OTHER EDITION";
    }
    if (strstr(why, "version") != NULL) {
        return "A PLAYER NEEDS AN UPDATE";
    }
    if (strstr(why, "ready") != NULL) {
        return "WAITING FOR READY";
    }
    if (strstr(why, "2 players") != NULL) {
        return "NEEDS 2 PLAYERS";
    }
    if (strstr(why, "pick a machine") != NULL) {
        return "WAITING FOR PICKS";
    }
    if (strstr(why, "machine select") != NULL) {
        return "OPEN MACHINE SELECT";
    }
    if (strstr(why, "your machine") != NULL || strstr(why, "picking a machine") != NULL) {
        return "PICK YOUR MACHINE ON TOP";
    }
    if (strstr(why, "picking a course") != NULL) {
        return "PICK THE COURSE ON TOP";
    }
    if (strstr(why, "checking the course") != NULL) {
        return "CHECKING THE COURSE";
    }
    if (strstr(why, "course") != NULL) {
        return "COURSE NOT ON EVERY 3DS";
    }
    return "NOT READY YET";
}

static void FillRooms(BuiMpView* mp) {
    int i;
    mp->roomCount = (int8_t) sUi.nearbyCount;
    mp->pages = (int8_t) (sUi.nearbyCount > 0 ? (sUi.nearbyCount + BUI_ROOMS_PER_PAGE - 1) / BUI_ROOMS_PER_PAGE : 1);
    if (sNearbyPage > mp->pages) {
        sNearbyPage = mp->pages;
    }
    mp->page = (int8_t) sNearbyPage;
    for (i = 0; i < sUi.nearbyCount && i < BUI_ROOMS_MAX; i++) {
        const GdxMpoUiNearby* n = &sUi.nearby[i];
        BuiRoomView* r = &mp->room[i];
        CopyCaps(r->host, sizeof(r->host), n->name);
        r->players = (int8_t) n->players;
        r->maxPlayers = (int8_t) n->maxPlayers;
        r->model = (int8_t) n->model;
        r->signal = (int8_t) n->signal;
        r->medium = (int8_t) (n->medium == GDX_NEARBY_MEDIUM_UDS ? BUI_MEDIUM_UDS : BUI_MEDIUM_WIFI);
        r->joinable = (int8_t) n->joinable;
        r->compatible = (int8_t) n->compatible;
        CopyCaps(r->whyNot, sizeof(r->whyNot), strcmp(n->whyNot, "other version") == 0 ? "VERSION" : n->whyNot);
    }
}

static void FillSlots(BuiMpView* mp) {
    int s;
    for (s = 0; s < 4 && s < GDX_MPO_MAX_SLOTS; s++) {
        const GdxMpoUiPlayer* p = &sUi.player[s];
        BuiSlotView* v = &mp->slot[s];
        if (!p->present) {
            continue;
        }
        v->present = 1;
        v->me = (int8_t) p->isMe;
        v->host = (int8_t) (s == 0);
        v->ready = (int8_t) p->ready;
        v->live = (int8_t) p->live;
        v->versionOk = (int8_t) p->versionOk;
        v->machine = (int8_t) (p->machine >= 0 && p->machine < BUI_MACHINES ? p->machine : 0);
        v->model = (int8_t) p->model;
        v->rttMs = (int16_t) (p->rttMs > 9999 ? 9999 : p->rttMs);
        CopyCaps(v->name, sizeof(v->name), p->name);
        if (sUi.stockPick) {
            v->picking = (int8_t) p->picking;
            v->picked = (int8_t) p->picked;
        }
    }
}

static void FillStockPick(BuiMpView* mp) {
    if (!sUi.stockPick || sUi.phase < GDX_MPO_ROOM) {
        return;
    }
    mp->stockPick = 1;
    mp->pickStep = (int8_t) sUi.pickStep;
    mp->pickSecs = (int8_t) (sUi.pickSecsLeft > 99 ? 99 : sUi.pickSecsLeft);
    mp->hostPicking = (int8_t) sUi.hostPicking;
    mp->courseRefused = (int8_t) sUi.courseRefused;
    mp->courseOk = (int8_t) sUi.courseOk;
    mp->racesDone = (int8_t) (sUi.racesDone > 99 ? 99 : sUi.racesDone);
    mp->courseSecs = (int8_t) (sUi.courseSecsLeft > 99 ? 99 : sUi.courseSecsLeft > 0 ? sUi.courseSecsLeft : 0);
}

/* The humans of the last race by place (their absolute place in the whole field). */
static int ResultPlace(const GdxMpoUiPlayer* p) {
    return p->place > 0 ? p->place : p->resultPos;
}

static void FillResultRow(BuiResultView* r, int slot) {
    const GdxMpoUiPlayer* p = &sUi.player[slot];
    const int place = ResultPlace(p);
    r->slot = (int8_t) slot;
    r->player = (int8_t) (p->player & 3);
    r->character = (int8_t) (place > 0 && place <= sUi.fieldCount ? sUi.fieldChar[place - 1] : p->machine);
    r->place = (int8_t) place;
    r->byCpu = (int8_t) p->byCpu;
    r->points = (int8_t) p->racePoints;
    r->total = (int16_t) p->points;
    r->timeMs = p->resultPos > 0 ? p->resultMs : 0;
    CopyCaps(r->name, sizeof(r->name), p->resultName[0] != '\0' ? p->resultName : p->name);
}

static void FillResults(BuiMpView* mp) {
    int done[GDX_MPO_MAX_SLOTS] = {0};
    int i;
    int n = 0;
    while (n < 4) {
        int best = -1;
        int s;
        for (s = 0; s < GDX_MPO_MAX_SLOTS && s < 4; s++) {
            const GdxMpoUiPlayer* p = &sUi.player[s];
            if (!p->raced || done[s] || ResultPlace(p) <= 0) {
                continue;
            }
            if (best < 0 || ResultPlace(p) < ResultPlace(&sUi.player[best])) {
                best = s;
            }
        }
        if (best < 0) {
            break;
        }
        done[best] = 1;
        FillResultRow(&mp->result[n++], best);
    }
    mp->resultCount = (int8_t) n;
    mp->fieldCount = (int8_t) (sUi.fieldCount < BUI_FIELD_MAX ? sUi.fieldCount : BUI_FIELD_MAX);
    for (i = 0; i < mp->fieldCount; i++) {
        mp->fieldChar[i] = sUi.fieldChar[i];
        mp->fieldHuman[i] = (int8_t) (sUi.fieldSlot[i] >= 0 ? sUi.fieldSlot[i] + 1 : 0);
    }
    mp->racesDone = (int8_t) (sUi.racesDone > 99 ? 99 : sUi.racesDone);
}

static void FillMp(BuiMpView* mp) {
    if (!sUiValid) {
        return;
    }
    mp->lan = (int8_t) sUi.lan;
    mp->haveServer = (int8_t) sUi.haveServer;
    mp->uds = (int8_t) sUi.uds;
    mp->radioOff = (int8_t) (sUi.uds && sUi.udsError == GDX_UDS_E_WIRELESS_OFF);
    mp->isHost = (int8_t) sUi.isHost;
    mp->isLan = (int8_t) sUi.isLan;
    mp->isUds = (int8_t) sUi.isUds;
    mp->maxPlayers = (int8_t) sUi.maxPlayers;
    mp->ready = (int8_t) sUi.ready;
    mp->startable = (int8_t) sUi.startable;
    mp->course = (int8_t) (sUi.course >= 0 && sUi.course < (sUi.stockPick ? 42 : 24) ? sUi.course : 0);
    mp->laps = (int8_t) sUi.laps;
    mp->delay = (int8_t) sUi.delayPreview;
    mp->racers = (int8_t) BuiMpRacers(&sUi);
    mp->racersCap = (int8_t) BuiMpRacersCap(&sUi);
    mp->racersLive = (int8_t) BuiMpRacersLive();
    snprintf(mp->code, sizeof(mp->code), "%s", sUi.code[0] != '\0' ? sUi.code : sJoinCode);
    snprintf(mp->whyNot, sizeof(mp->whyNot), "%s", ShortWhyNot(sUi.whyNot));
    snprintf(mp->joinName, sizeof(mp->joinName), "%s", sJoinName);
    mp->joinMedium = (int8_t) sJoinMedium;
    mp->haveDirect = (int8_t) sUi.haveDirect;
    mp->relayOnly = (int8_t) sUi.relayOnly;
    mp->showCode = (int8_t) sUi.showCode;
    gdx3ds_mpo_direct_addr(mp->directAddr, (int) sizeof(mp->directAddr));
    mp->awayStartable = (int8_t) sUi.awayStartable;
    CopyCaps(mp->awayName, sizeof(mp->awayName), sUi.awayName);
    mp->roomClosed = (int8_t) sUi.roomClosed;
    mp->raceWait = (int8_t) sUi.raceWait;
    CopyCaps(mp->closedWhy, sizeof(mp->closedWhy), sUi.closedWhy);
    CopyCaps(mp->endedBy, sizeof(mp->endedBy), sUi.endedBy);
    FillStockPick(mp);
    FillRooms(mp);
    FillSlots(mp);
}

static void FillMachine(BuiView* v) {
    int machine = 0, skin = 0, engine = 50;
    int i;
    const int character = kMachineSelectCharacterList[sPickSlot];
    gdx3ds_mpg_current_picks(&machine, &skin, &engine);
    v->unlocked = (int8_t) MpUnlockedMachines();
    v->pickSlot = (int8_t) sPickSlot;
    for (i = 0; i < BUI_MACHINES; i++) {
        v->grid[i] = kMachineSelectCharacterList[i];
    }
    for (i = 0; i < 3; i++) {
        v->stats[i] = gMachines[character].machineStats[i];
    }
    for (i = 0; i < 4; i++) {
        v->skinRgb[i][0] = gMachines[character].red[i];
        v->skinRgb[i][1] = gMachines[character].green[i];
        v->skinRgb[i][2] = gMachines[character].blue[i];
    }
    v->skin = (int8_t) (skin & 3);
    v->engine = (int8_t) (engine < 0 ? 0 : engine > 100 ? 100 : engine);
}

static void BuildView(BuiView* v, int screen) {
    bui_view_init(v);
    v->screen = (int16_t) screen;
    v->focus = (int16_t) ((screen == BUI_SCR_HOME || screen == BUI_SCR_EDITOR) ? BUI_W_NONE : sFocus);
    /* the spinner stops under a dialog: the shaded screen does not repaint it 8 times a second */
    v->anim = (int8_t) (HasSpinner(screen) && sDialog == BUI_DLG_NONE ? (sTicks / 8) & 7 : 0);
    v->model = (int8_t) sModel;
#ifdef GDX3DS_BUI_EK
    v->ek = 1;
#endif
    v->settingsButton = (int8_t) (screen == BUI_SCR_HOME && gdx3ds_menu_settings_available());
    v->stereoButton = (int8_t) sStereoButton;
    v->stereoOn = (int8_t) (sStereoButton ? gdx3ds_stereo_user_on() : 0);
    if (sUiValid && sUi.myName[0] != '\0') {
        CopyCaps(v->name, sizeof(v->name), sUi.myName);
    } else {
        CopyCaps(v->name, sizeof(v->name), gdx3ds_config_get_string("net", "mp_name", "PLAYER"));
    }
    snprintf(v->version, sizeof(v->version), "%s", sVersion);
    v->homePilot = (int8_t) (gPlayerCharacters[0] >= 0 && gPlayerCharacters[0] < BUI_MACHINES ? gPlayerCharacters[0] : 0);
    snprintf(v->entry, sizeof(v->entry), "%s", sEntry);
    v->hostMedium = (int8_t) sHostMedium;
    v->roomSize = (int8_t) sRoomSize;
    v->cup = (int8_t) sCup;
    if (screen == BUI_SCR_MACHINE) {
        FillMachine(v);
    }
    v->dialog = (int8_t) sDialog;
    v->dialogFocus = (int8_t) sDialogFocus;
    snprintf(v->dialogName, sizeof(v->dialogName), "%s", sDialogName);
    snprintf(v->dialogCode, sizeof(v->dialogCode), "%s", sDialogCode);
    snprintf(v->dialogMine, sizeof(v->dialogMine), "%s", sDialogMine);
    snprintf(v->dialogExtra, sizeof(v->dialogExtra), "%s", sDialogExtra);
    if (sTicks < sToastUntil) {
        snprintf(v->toast, sizeof(v->toast), "%s", sToast);
        v->toastTone = (int8_t) sToastTone;
        v->toastIcon = (int8_t) sToastIcon;
    }
    if (screen != BUI_SCR_HOME) {
        FillMp(&v->mp);
    }
    if (screen == BUI_SCR_RESULTS && sUiValid) {
        FillResults(&v->mp);
    }
    /* mp-menus */
    v->buttonsOff = (int8_t) !sOwnsButtons;
    v->holdConfirm = (int8_t) (sHoldMs > 0);
    v->holdWidget = (int16_t) sHoldId;
    v->holdPct = (int8_t) sHoldPct;
    v->keepMachine = (int8_t) (sUiValid && sUi.stockPick && sUi.keepMachine >= 0 && sUi.keepMachine < BUI_MACHINES
                                   ? sUi.keepMachine
                                   : -1);
    if (screen == BUI_SCR_EDITOR || IsCsScreen(screen)) {
        FillCs(v);
    }
}

/* ---- paint + present ------------------------------------------------------------------------ */

static void Present(uint16_t* live, const uint16_t* shadow) {
    int x0, x1;
    if (!bui_kit_dirty(&x0, &x1)) {
        return;
    }
    memcpy(live + (size_t) x0 * BUI_H, shadow + (size_t) x0 * BUI_H, (size_t) (x1 - x0 + 1) * BUI_H * sizeof(uint16_t));
}

/* What the shadow buffer holds since the last paint, for the overlay-only repaints. */
enum { PAINT_FULL = 0, PAINT_DIALOG_SHADED, PAINT_DIALOG_PANEL };
static uint32_t sPaintedBaseKey = 0;
static int sPaintedDialog = BUI_DLG_NONE;
static int sPaintedToast = 0;
static char sPaintedDialogText[40];

/* A dialog over an unchanged screen needs no screen repaint: over the bare screen it shades and
 * draws its panel; over itself (a focus move) the panel alone. Anything else repaints it all. */
static int PaintMode(const BuiView* v, uint32_t baseKey, int canReuse) {
    char text[40];
    if (!canReuse || v->dialog == BUI_DLG_NONE || v->toast[0] != '\0' || sPaintedToast ||
        baseKey != sPaintedBaseKey) {
        return PAINT_FULL;
    }
    if (sPaintedDialog == BUI_DLG_NONE) {
        return PAINT_DIALOG_SHADED;
    }
    snprintf(text, sizeof(text), "%s|%s", v->dialogName, v->dialogCode);
    if (sPaintedDialog == v->dialog && strcmp(text, sPaintedDialogText) == 0) {
        return PAINT_DIALOG_PANEL;
    }
    return PAINT_FULL;
}

static void NotePainted(const BuiView* v, uint32_t baseKey) {
    sPaintedBaseKey = baseKey;
    sPaintedDialog = v->dialog;
    sPaintedToast = v->toast[0] != '\0';
    snprintf(sPaintedDialogText, sizeof(sPaintedDialogText), "%s|%s", v->dialogName, v->dialogCode);
}


/* Paints v into the shadow and shows it. A full paint under a dialog is staged over two ticks
 * (the screen, then the shade and panel) so neither tick pays both; the live screen changes
 * once, when both are done. Returns 1 when v is on screen. */
static int Paint(uint16_t* live, const BuiView* v, int canReuse, int canStage) {
    static const char* const kModeNames[4] = {"full", "dialog", "panel", "stage1"};
    uint16_t* const shadow = gdx3ds_bottomhud_shadow_fb();
    const u64 t0 = svcGetSystemTick();
    const uint32_t baseKey = bui_view_base_key(v);
    int mode = PaintMode(v, baseKey, canReuse);
    u64 t1, t2;
    int x0 = 0, x1 = -1;
    bui_kit_begin(shadow);
    if (!sPresentPending) {
        bui_kit_dirty_clear();
    }
    bui_kit_reset_unknown_glyphs();
    if (mode == PAINT_FULL && canStage && v->dialog != BUI_DLG_NONE && v->toast[0] == '\0') {
        BuiView base = *v;
        base.dialog = BUI_DLG_NONE;
        bui_screen_paint(&base, &sWidgets);
        NotePainted(&base, baseKey);
        sPresentPending = 1;
        mode = 3;
    } else if (mode == PAINT_FULL) {
        bui_screen_paint(v, &sWidgets);
        NotePainted(v, baseKey);
        sPresentPending = 0;
    } else {
        bui_screen_paint_dialog(v, &sWidgets, mode == PAINT_DIALOG_SHADED);
        NotePainted(v, baseKey);
        sPresentPending = 0;
    }
    if (bui_kit_unknown_glyphs() > 0) {
        BuiLog("[bui] WARNING: %s drew %d characters without a glyph", ScreenName(v->screen), bui_kit_unknown_glyphs());
    }
    t1 = svcGetSystemTick();
    if (!sPresentPending) {
        Present(live, shadow);
    }
    t2 = svcGetSystemTick();
    bui_kit_dirty(&x0, &x1);
    sRcPaints++;
    sRcPaintTotal += t1 - t0;
    sRcPresentTotal += t2 - t1;
    if (t1 - t0 > sRcPaintMax) {
        sRcPaintMax = t1 - t0;
    }
    if (t2 - t1 > sRcPresentMax) {
        sRcPresentMax = t2 - t1;
    }
    if (sLogPaints || sGallery) {
        BuiLog("[bui] paint %s dlg=%d mode=%s paint_us=%lu present_us=%lu cols=%d..%d", ScreenName(v->screen),
               v->dialog, kModeNames[mode], TicksToUs(t1 - t0), TicksToUs(t2 - t1), x0, x1);
    }
    return !sPresentPending;
}

static void Receipt(void) {
    if (sRcTicks < BUI_RECEIPT_TICKS) {
        return;
    }
    {
        const unsigned long idleTicks = sRcTicks - sRcPaints;
        BuiLog("[bui] ticks=%lu idle_us=%lu paints=%lu paint_us=%lu/%lu present_us=%lu/%lu screen=%s owns=%d demo=%d held=%d",
               sRcTicks, idleTicks > 0 ? TicksToUs(sRcIdleTicks / idleTicks) : 0UL, sRcPaints,
               sRcPaints > 0 ? TicksToUs(sRcPaintTotal / sRcPaints) : 0UL, TicksToUs(sRcPaintMax),
               sRcPaints > 0 ? TicksToUs(sRcPresentTotal / sRcPaints) : 0UL, TicksToUs(sRcPresentMax),
               gdx3ds_bui_screen_name(), sOwnsButtons, gdx_title_demo_active(), sDemoHeld);
    }
    sRcTicks = 0;
    sRcPaints = 0;
    sRcIdleTicks = 0;
    sRcPaintTotal = 0;
    sRcPaintMax = 0;
    sRcPresentTotal = 0;
    sRcPresentMax = 0;
}

/* ---- gallery -------------------------------------------------------------------------------- */

/* A gallery entry that draws the whole surface itself (the race dash's pieces). */
static void GalleryDraw(uint16_t* live, BuiGalleryDraw draw) {
    uint16_t* const shadow = gdx3ds_bottomhud_shadow_fb();
    bui_kit_begin(shadow);
    bui_kit_dirty_clear();
    draw();
    memcpy(live, shadow, 320 * 240 * sizeof(uint16_t));
    sPresentPending = 0;
}

/* [debug] bui_gallery=1: every gallery view, one per tick, each followed by its SHOT. */
static int GalleryTick(uint16_t* live) {
    char label[48];
    BuiView v;
    const BuiGalleryEntry* e;
    if (!sGallery || sGalleryIndex >= bui_gallery_count()) {
        return 0;
    }
    if (sGalleryIndex < 0) {
        sGalleryIndex = 0;
        BuiLog("[bui] gallery: %d screens", bui_gallery_count());
    }
    e = bui_gallery_entry(sGalleryIndex);
    if (bui_gallery_draw(sGalleryIndex) != NULL) {
        GalleryDraw(live, bui_gallery_draw(sGalleryIndex));
    } else {
        bui_gallery_view(sGalleryIndex, &v);
        v.model = (int8_t) sModel;
        sPresentPending = 0;
        Paint(live, &v, 0, 0);
    }
    snprintf(label, sizeof(label), "bui_%s", e->id);
    gdx3ds_bottomhud_note_shot(label);
    gdx3ds_bottomhud_service_shot(gdx3ds_bottomhud_shadow_fb());
    sGalleryIndex++;
    if (sGalleryIndex >= bui_gallery_count()) {
        BuiLog("[bui] gallery done (%d screens)", bui_gallery_count());
        sShown = SHOWN_NONE;
    }
    return 1;
}

/* ---- the tick ------------------------------------------------------------------------------- */

static void ShowLogo(uint16_t* fb, uint32_t held, uint32_t prevHeld, int touching, int px, int py) {
    if (sShown != SHOWN_LOGO) {
        gdx3ds_bottomhud_idle_invalidate();
        sShown = SHOWN_LOGO;
    }
    gdx3ds_bottomhud_idle_tick(fb, held, prevHeld, touching, sPrevTouching, px, py);
}

static void EnsureFocus(void) {
    if (sScreen == BUI_SCR_NONE || sDialog != BUI_DLG_NONE) {
        return;
    }
    if (sFocus == BUI_W_NONE || bui_widgets_find(&sWidgets, sFocus) == NULL) {
        const int first = bui_widgets_first_focus(&sWidgets);
        if (first != sFocus && first != BUI_W_NONE) {
            sFocus = first;
        }
    }
}

/* What the bottom screen shows: the open flow, else Home (title / main menu) or the course
 * editor's menu. */
static int ShownScreen(void) {
    if (sScreen != BUI_SCR_NONE) {
        return sScreen;
    }
    return (!AmbientMode() && EditorMenuOn()) ? BUI_SCR_EDITOR : BUI_SCR_HOME;
}

/* release: SELECT on Home opens SETTINGS, the button's key twin. Home's buttons belong to the top
 * screen's menu, but the game has no SELECT (the N64 pad has none, and no action is bound to it
 * by default), so it is free there. It fires on the RELEASE of a SELECT pressed and let go with
 * no other button held, so the L+R+SELECT chord (in either order) never opens it, and it stays
 * off when the player has bound SELECT to an action ([input] bind_*). */
#define BUI_HOME_SELECT_OTHERS                                                                     \
    (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_ZL | KEY_ZR | KEY_START | BUI_KEYS_DPAD)
static int sHomeSelectSolo = 0;

static int SelectIsGameKey(void) {
    int i;
    for (i = 0; i < gdx3ds_input_action_count(); i++) {
        if (gdx3ds_input_action_mask(i) & KEY_SELECT) {
            return 1;
        }
    }
    return 0;
}

static void ServiceHomeSelect(uint32_t held, int onHome) {
    const int others = (held & BUI_HOME_SELECT_OTHERS) != 0;
    if (!onHome) {
        sHomeSelectSolo = 0;
        return;
    }
    if (held & KEY_SELECT) {
        if (!(sPrevHeld & KEY_SELECT)) {
            sHomeSelectSolo = !others;
        } else if (others) {
            sHomeSelectSolo = 0;
        }
        return;
    }
    if ((sPrevHeld & KEY_SELECT) && sHomeSelectSolo && gdx3ds_menu_settings_available() && !SelectIsGameKey()) {
        BuiLog("[bui] SELECT on Home -> the player settings");
        gdx3ds_menu_open_settings();
    }
    sHomeSelectSolo = 0;
}

/* The buttons are the menus' while a flow is open at TITLE / MAIN MENU, or on a room screen while
 * a stock pick only parks the top screen (confirmed and frozen: START RACE, SETTINGS ... take
 * their keys), and while the camera scans (B cancels it). Never during a session's race. */
static int FlowOwnsButtons(int ambient) {
    if (sScreen == BUI_SCR_CS_SCAN) {
        return 1;
    }
    if (sScreen == BUI_SCR_NONE || gdx3ds_mpo_in_session()) {
        return 0;
    }
    return ambient || (IsRoomScreen(sScreen) && sUiValid && sUi.stockPick && PickerParked());
}

/* The top screen's VS icon (decomp-3ds-mp-vs-menu.patch) asked for MULTIPLAYER. card-4b: a
 * request that arrives while Home's place is not ready (hw: a stale attract-demo state on the
 * main menu, gdx_title_demo_clear_stale) waits up to BUI_VS_WAIT_TICKS for it, and one that is
 * dropped says why -- it used to vanish without a line in the log. */
#define BUI_VS_WAIT_TICKS 30

static void ServiceVsIcon(int ambient) {
    if (!sVsPending) {
        sVsWaitTicks = 0;
        return;
    }
    if (sScreen == BUI_SCR_NONE && ambient) {
        sVsPending = 0;
        BuiLog("[bui] VS icon -> MULTIPLAYER%s", sVsWaitTicks > 0 ? " (after a wait)" : "");
        sVsWaitTicks = 0;
        PollMp(1);
        Go(BUI_SCR_MP_CHOOSE);
        return;
    }
    if (sScreen == BUI_SCR_NONE && ++sVsWaitTicks < BUI_VS_WAIT_TICKS) {
        return;
    }
    sVsPending = 0;
    BuiLog("[bui] VS icon ignored: screen=%s ambient=%d demo=%d mode=0x%02X", ScreenName(sScreen), ambient,
           gdx_title_demo_active(), &gdx_input_game_mode != NULL ? gdx_input_game_mode() : -1);
    sVsWaitTicks = 0;
}

/* card-4b: a fresh key the bottom screen did not take, while a flow is open outside a race and
 * no stock pick is running on top: one line per press, so a "dead" button on hardware says why. */
static void NoteKeysToGame(uint32_t held) {
    static const uint32_t kKeys = KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_START;
    const uint32_t down = held & ~sPrevHeld & kKeys;
    if (down == 0 || sOwnsButtons || sScreen == BUI_SCR_NONE || sScreen == BUI_SCR_LAUNCH ||
        gdx3ds_mpo_in_session() || (&gdx3ds_mpp_active != NULL && gdx3ds_mpp_active())) {
        return;
    }
    BuiLog("[bui] keys 0x%03lX on %s are the game's (ambient=%d pick=%d parked=%d)", (unsigned long) down,
           ScreenName(sScreen), AmbientMode(), sUiValid ? sUi.stockPick : -1, PickerParked());
}

/* card-4b: the tick the buttons become the flow's, every key already held is the previous
 * owner's press (the top screen's A that froze a stock pick): it acts on the bottom screen only
 * after it is let go and pressed again. */
static void FollowButtonOwner(uint32_t held) {
    if (sOwnsButtons && !sOwnedLastTick) {
        sSwallowKeys = held & ~(uint32_t) KEY_TOUCH;
        if (sSwallowKeys != 0) {
            BuiLog("[bui] buttons -> %s with keys 0x%03lX held: they wait for a fresh press", ScreenName(sScreen),
                   (unsigned long) sSwallowKeys);
        }
    }
    sSwallowKeys &= held;
    sOwnedLastTick = sOwnsButtons;
}

int gdx_mp_vs_menu_available(void) {
    return sEnabled;
}

void gdx_mp_vs_menu_open(void) {
    sVsPending = 1;
}

void gdx3ds_bui_tick(uint16_t* fb, uint32_t held, uint32_t prevHeld, int touching, int touchPx, int touchPy) {
    const u64 t0 = svcGetSystemTick();
    const int ambient = AmbientMode();
    int screen;
    BuiView v;
    uint32_t key;
    if (!sEnabled || fb == NULL) {
        return;
    }
    sTicks++;
    sRcTicks++;
    sPollsSinceTick = 0;
    if (sLastBuiTick + 1 != sMenuTicks) {
        sShown = SHOWN_NONE; /* another owner had the screen since our last tick */
    }
    sLastBuiTick = sMenuTicks;
    if (ambient && GalleryTick(fb)) {
        sOwnsButtons = 0;
        sOwnedLastTick = 0;
        return;
    }
    PollMp(0);
    WatchMessage();
    FollowPhase();
    ServiceVsIcon(ambient);
    ServiceCs();
    HoldTitleDemo(sScreen != BUI_SCR_NONE && ambient);
    if (sScreen == BUI_SCR_NONE && !ambient && !EditorMenuOn()) {
        sOwnsButtons = 0;
        sOwnedLastTick = 0;
        sShownViewValid = 0;
        EndHold("cancelled (screen left)");
        ShowLogo(fb, held, prevHeld, touching, touchPx, touchPy);
        sPrevHeld = held;
        sPrevTouching = touching;
        Receipt();
        return;
    }
    sOwnsButtons = FlowOwnsButtons(ambient);
    FollowButtonOwner(held);
    NoteKeysToGame(held);
    HandleTouch(touching, touchPx, touchPy);
    if (sOwnsButtons) {
        HandleButtons(held);
    }
    ServiceHomeSelect(held, !sOwnsButtons && sDialog == BUI_DLG_NONE && ShownScreen() == BUI_SCR_HOME);
    ServiceHold(held, touching, touchPx, touchPy);
    sPrevHeld = held;
    sPrevTouching = touching;
    if (sScreen == BUI_SCR_NEARBY) {
        gdx3ds_mpo_ui_nearby_shown();
    }
    PollMp(0);
    FollowPhase();
    FollowNearbyRooms();
    ServiceResultsPending();
    ServiceCs();
    screen = ShownScreen();
    BuildView(&v, screen);
    key = bui_view_key(&v);
    if (sShown != screen || key != sPaintedKey) {
        const int canReuse = sShown == screen;
        if (!canReuse) {
            sPresentPending = 0; /* someone else drew meanwhile: a half-done stage is gone */
        }
        sShown = screen;
        if (Paint(fb, &v, canReuse, 1)) {
            sPaintedKey = key;
            sShownView = v;
            sShownViewValid = 1;
        }
        EnsureFocus();
        PaintCsPreview(fb, 1);
    } else {
        sRcIdleTicks += svcGetSystemTick() - t0;
        PaintCsPreview(fb, 0);
    }
    gdx3ds_bottomhud_service_shot(gdx3ds_bottomhud_shadow_fb());
    Receipt();
}

/* ---- script hooks --------------------------------------------------------------------------- */

const char* gdx3ds_bui_screen_name(void) {
    if (!sEnabled) {
        return "off";
    }
    if (sShown == SHOWN_LOGO) {
        return "logo";
    }
    if (sShown == SHOWN_NONE) {
        return gdx3ds_bottomhud_race_live() ? "race" : "console";
    }
    return ScreenName(sShown);
}

/* The game's top screen by name, for scripts ("top:machine-select"). */
static int TopIs(const char* name) {
    static const struct {
        const char* name;
        int mode;
    } kModes[] = {{"title", 0x00}, {"main-menu", 0x07}, {"machine-select", 0x08}, {"machine-settings", 0x09},
                  {"course-select", 0x0A}};
    size_t i;
    if (&gdx_input_game_mode == NULL) {
        return 0;
    }
    for (i = 0; i < sizeof(kModes) / sizeof(kModes[0]); i++) {
        if (strcmp(name, kModes[i].name) == 0) {
            return gdx_input_game_mode() == kModes[i].mode;
        }
    }
    return 0;
}

int gdx3ds_bui_condition(const char* what) {
    if (what[0] == '!') {
        return !gdx3ds_bui_condition(what + 1); /* "WAIT !race": until it no longer holds */
    }
    if (strcmp(what, "rooms") == 0) {
        return sUiValid && sUi.nearbyCount > 0;
    }
    if (strcmp(what, "startable") == 0) {
        return sUiValid && sUi.isHost && sUi.startable >= 2;
    }
    if (strcmp(what, "ready") == 0) {
        return sUiValid && sUi.ready;
    }
    if (strcmp(what, "race") == 0) {
        return gdx3ds_bottomhud_race_live();
    }
    if (strcmp(what, "paused") == 0) {
        GdxMpoPauseView pz;
        gdx3ds_mpo_pause_view(&pz); /* mp-menus #10: the shared pause is up */
        return pz.paused;
    }
    if (strcmp(what, "ambient") == 0) {
        return AmbientMode();
    }
    if (strcmp(what, "player-settings") == 0) {
        return gdx3ds_menu_settings_open(); /* the player settings are on screen ("settings" is the room's) */
    }
    if (strcmp(what, "picking") == 0) {
        return sUiValid && sUi.mySlot >= 0 && sUi.player[sUi.mySlot & 3].picking != 0;
    }
    if (strcmp(what, "picked") == 0) {
        return sUiValid && sUi.mySlot >= 0 && sUi.player[sUi.mySlot & 3].picked;
    }
    if (strncmp(what, "step:", 5) == 0) {
        static const char* const kSteps[4] = {"lobby", "machines", "course", "ready"};
        return sUiValid && sUi.pickStep >= 0 && sUi.pickStep < 4 && strcmp(what + 5, kSteps[sUi.pickStep]) == 0;
    }
    if (strncmp(what, "top:", 4) == 0) {
        return TopIs(what + 4);
    }
    if (strcmp(what, "peers") == 0) {
        int s, n = 0;
        for (s = 0; sUiValid && s < GDX_MPO_MAX_SLOTS; s++) {
            n += sUi.player[s].present ? 1 : 0;
        }
        return n >= 2;
    }
    return strcmp(what, gdx3ds_bui_screen_name()) == 0;
}
