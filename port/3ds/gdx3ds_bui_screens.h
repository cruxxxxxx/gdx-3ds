/* port/3ds/gdx3ds_bui_screens.h — the bottom-screen product screens (bottom-menus design, M1-M2):
 * pure painters from a plain-data view (BuiView) to pixels (gdx3ds_bui_kit) plus the widgets each
 * screen registers for touch and D-pad focus (gdx3ds_bui_layout.h).
 *
 * SPLIT. The 3DS shell (gdx3ds_bui.c) owns state, input and the network; it fills a BuiView from
 * the game and the MP layer (gdx3ds_mpo_ui) and calls bui_screen_paint() only when the view's
 * bytes changed (bui_view_key). This file knows nothing about either: no <3ds.h>, no decomp, no
 * MP headers, so the host tests and the gallery ([debug] bui_gallery) render every screen from
 * canned views, and the mockup diff runs on the very code the console runs.
 *
 * Strings are caps (the fonts fall back to capitals anyway); every dynamic string a view carries
 * is a fixed array, so a view is plain bytes and hashes as such. */
#ifndef GDX3DS_BUI_SCREENS_H
#define GDX3DS_BUI_SCREENS_H

#include <stdint.h>

#include "gdx3ds_bui_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

enum BuiScreenId {
    BUI_SCR_NONE = 0,
    BUI_SCR_HOME,
    BUI_SCR_MP_CHOOSE,
    BUI_SCR_NEARBY,
    BUI_SCR_HOST,
    BUI_SCR_ONLINE,
    BUI_SCR_CODE,
    BUI_SCR_NAME,
    BUI_SCR_JOINING,
    BUI_SCR_LOBBY,
    BUI_SCR_MACHINE,
    BUI_SCR_SETTINGS,
    BUI_SCR_COURSE,
    BUI_SCR_LAUNCH,
    BUI_SCR_RESULTS, /* after a race (M3): places, VS points, the host's next step */
    /* mp-menus: the Expansion Kit course editor's bottom menu ([ek] course_share) and its import */
    BUI_SCR_EDITOR,  /* in the course editor: IMPORT FROM SD / SCAN QR */
    BUI_SCR_CS_FILES, /* the .gdxcourse files on the SD card */
    BUI_SCR_CS_SLOT,  /* where an import goes: the library, or an EDIT CUP slot */
    BUI_SCR_CS_SCAN,  /* the camera reading a course's QR pages */
    BUI_SCR_COUNT
};

/* Dialogs drawn over a screen (the design's MP error set). */
enum BuiDialogId {
    BUI_DLG_NONE = 0,
    BUI_DLG_WIRELESS_OFF, /* the switch went off: BACK / TRY AGAIN */
    BUI_DLG_VERSION,      /* can't join: other build */
    BUI_DLG_DESYNC,       /* race stopped: out of sync */
    BUI_DLG_LOST,         /* a player stopped answering: the race ended */
    BUI_DLG_CLOSED,       /* the host left / stopped answering: the room closed */
    BUI_DLG_SUSPEND,      /* HOME during a race */
    BUI_DLG_LEAVE_HOST,   /* close the room? */
    BUI_DLG_LEAVE_GUEST,  /* leave the room? */
    BUI_DLG_SERVER,       /* no server / timeout */
    BUI_DLG_NO_START,     /* the race did not start */
    BUI_DLG_WIFI_OFF,     /* WiFi is off (refused to host/join) */
    BUI_DLG_HOST_LOST,    /* the host stopped answering (or left) during or after a race */
    BUI_DLG_CS_REPLACE,   /* mp-menus: an import would replace a disk course / a cup slot */
    /* card-4 version gating (the server's UPDATE, the host's proto on a mismatch) */
    BUI_DLG_UPDATE,        /* this console is older than the server allows: update the game */
    BUI_DLG_SERVER_OLD,    /* this console is newer than the server knows: the server needs an update */
    BUI_DLG_VERSION_NEWER, /* can't join: the host runs a newer version (update this 3DS) */
    BUI_DLG_VERSION_OLDER, /* can't join: the host runs an older version (the host should update) */
    BUI_DLG_COUNT
};

/* Widget ids. Screens use disjoint ranges so a focus id never means two things. */
enum BuiWidgetId {
    BUI_W_NONE = -1,
    BUI_W_HOME_DEBUG = 2, /* retired (uipolish: SETTINGS holds the developer tabs); 1 was Home's MULTIPLAYER tile */
    BUI_W_HOME_3D, /* o3ds60: the 3D ON / OFF button (stereoButton) */
    BUI_W_BACK, /* the header's ◀ B cell */
    BUI_W_HOME_SETTINGS, /* release: the player SETTINGS button (settingsButton) */
    BUI_W_CHOOSE_NEARBY = 10,
    BUI_W_CHOOSE_ONLINE,
    BUI_W_CHOOSE_NAME,
    BUI_W_NEARBY_HOST = 20,
    BUI_W_NEARBY_UP,
    BUI_W_NEARBY_DOWN,
    BUI_W_NEARBY_RETRY,
    BUI_W_NEARBY_ROW0, /* + row on the page (0..2) */
    BUI_W_HOST_LOCAL = 30,
    BUI_W_HOST_WIFI,
    BUI_W_HOST_SIZE2, /* + n - 2 */
    BUI_W_HOST_SIZE3,
    BUI_W_HOST_SIZE4,
    BUI_W_HOST_CANCEL,
    BUI_W_HOST_OPEN,
    BUI_W_ONLINE_HOST = 40,
    BUI_W_ONLINE_JOIN,
    BUI_W_ONLINE_DIRECT, /* mp-online-hw: JOIN BY ADDRESS ([net] mp_direct = a.b.c.d[:port]) */
    BUI_W_KEY0 = 50, /* + key index (code: 32 keys, name: 40 keys) */
    BUI_W_ENTRY_DEL = 95,
    BUI_W_ENTRY_CLEAR,
    BUI_W_ENTRY_OK,
    BUI_W_JOIN_CANCEL = 100,
    BUI_W_LOBBY_SLOT0 = 110, /* + slot */
    BUI_W_LOBBY_SIDE = 115,  /* MACHINE (guest) / SETTINGS (host) */
    BUI_W_LOBBY_MAIN,        /* READY! (guest) / START RACE (host); stock picks: MACHINE SELECT too */
    BUI_W_LOBBY_KEEP,        /* stock picks: KEEP <pilot> (the previous pick, one tap) */
    BUI_W_MACHINE0 = 120,    /* + grid slot (0..29) */
    BUI_W_MACHINE_OK = 150,
    BUI_W_SET_COURSE = 160,
    BUI_W_SET_LAPS,
    BUI_W_SET_LAPS_DOWN,
    BUI_W_SET_LAPS_UP,
    BUI_W_SET_RACERS0, /* + segment (0..4) */
    BUI_W_SET_REPICK = 169, /* stock picks: every console into machine select again */
    BUI_W_COURSE_CUP0 = 170, /* + cup (0..3) */
    BUI_W_COURSE0 = 175,     /* + course in the cup (0..5) */
    BUI_W_RESULTS_REMATCH = 185, /* host: A */
    BUI_W_RESULTS_COURSE,        /* host: X */
    BUI_W_RESULTS_MACHINES,      /* host: Y */
    BUI_W_RESULTS_LEAVE,         /* guest: B */
    BUI_W_DLG_BUTTON0 = 190, /* + dialog button */
    BUI_W_ED_IMPORT = 200,   /* editor: IMPORT FROM SD */
    BUI_W_ED_SCAN,           /* editor: SCAN QR PAGES */
    BUI_W_CS_ROW0 = 205,     /* + row on the page (0..3) */
    BUI_W_CS_UP = 210,
    BUI_W_CS_DOWN,
    BUI_W_CS_RESCAN,
    BUI_W_CS_LIBRARY = 215,  /* the slot picker: library only */
    BUI_W_CS_CUP0,           /* + cup slot (0..5) */
    BUI_W_CS_ZOOM = 225,     /* scan: zoom 1x / 2x */
    BUI_W_CS_RESTART,
    BUI_W_CS_CANCEL,
};

/* The keys a screen can name in a hint (bui_screen_key). */
enum BuiKey { BUI_KEY_NONE = 0, BUI_KEY_A, BUI_KEY_B, BUI_KEY_X, BUI_KEY_Y, BUI_KEY_L, BUI_KEY_R, BUI_KEY_START };

#define BUI_NAME_CAP 12 /* GDX_MPO_NAME_LEN */
#define BUI_CODE_CAP 5  /* GDX_MPO_CODE_LEN */
#define BUI_ROOMS_MAX 6 /* GDX_MPO_UI_NEARBY */
#define BUI_ROOMS_PER_PAGE 3
#define BUI_MACHINES 30

enum { BUI_MEDIUM_WIFI = 0, BUI_MEDIUM_UDS = 1, BUI_MEDIUM_ONLINE = 2 };
enum { BUI_MODEL_UNKNOWN = 0, BUI_MODEL_O3DS = 1, BUI_MODEL_N3DS = 2 };

typedef struct BuiRoomView {
    char host[BUI_NAME_CAP + 1];
    int8_t players, maxPlayers;
    int8_t model;
    int8_t signal; /* 0..3 */
    int8_t medium; /* BUI_MEDIUM_* */
    int8_t joinable;
    int8_t compatible;
    char whyNot[16];
} BuiRoomView;

typedef struct BuiSlotView {
    int8_t present;
    int8_t me, host;
    int8_t ready, live, versionOk;
    int8_t machine; /* character 0..29 */
    int8_t model;
    int16_t rttMs; /* -1 = not measured */
    char name[BUI_NAME_CAP + 1];
    int8_t picking; /* stock picks: BUI_PICK_* its top screen is in */
    int8_t picked;  /* stock picks: machine is its pick for the round */
} BuiSlotView;

/* Stock picks (a room picking with the game's top-screen menus): what a player's top screen is
 * in, and where the room is (gdx3ds_mp_online.h GDX_LB_PICK_* / GDX_MPO_STEP_*). */
enum { BUI_PICK_NONE = 0, BUI_PICK_MACHINE = 1, BUI_PICK_COURSE = 2 };
enum { BUI_STEP_LOBBY = 0, BUI_STEP_MACHINES = 1, BUI_STEP_COURSE = 2, BUI_STEP_READY = 3 };

/* One human's line on the results screen. */
typedef struct BuiResultView {
    int8_t slot;
    int8_t player;    /* player index in the race: its colour */
    int8_t character;
    int8_t place;     /* in the whole field, 1-based */
    int8_t byCpu;     /* removed mid-race: a CPU finished it */
    int8_t points;    /* this race's VS points */
    int16_t total;    /* the room's running total */
    int32_t timeMs;   /* 0 = did not finish; -1 = placed without a time (still racing when the race ended) */
    char name[BUI_NAME_CAP + 1];
} BuiResultView;

#define BUI_FIELD_MAX 30

/* The MP layer as the screens see it (gdx3ds_bui.c fills it from GdxMpoUi). */
typedef struct BuiMpView {
    int8_t lan, haveServer, uds;
    int8_t radioOff; /* the wireless switch is off (the UDS backend said so) */
    int8_t isHost, isLan, isUds;
    int8_t maxPlayers;
    int8_t ready;
    int8_t startable; /* host: players a START takes now (0 = not startable) */
    int8_t course;    /* 0..23; stock picks: 0..41 (the Expansion Kit's DD cups 30..41) */
    int8_t laps;
    int8_t delay;
    int8_t racers, racersCap; /* MACHINES: 0 = humans only (mp30 plugs in here) */
    int8_t racersLive;        /* 1 = the build can set more than 4 (mp30) */
    int8_t roomCount;
    int8_t page, pages;
    char code[BUI_CODE_CAP + 1];
    char whyNot[40];
    char joinName[BUI_NAME_CAP + 1]; /* JOINING: whose room */
    int8_t joinMedium;
    /* stock picks: 0 = the bottom-screen pickers (the views and goldens before it) */
    int8_t stockPick;
    int8_t pickStep;      /* BUI_STEP_* */
    int8_t pickSecs;      /* this console's machine-step countdown, -1 = none */
    int8_t hostPicking;   /* BUI_PICK_* of the host's top screen */
    int8_t courseRefused; /* the host's course picker just refused a course */
    int8_t courseOk;      /* this console can race the host's course */
    int8_t racesDone;
    /* results (M3): the humans by place, and the whole field (fieldHuman: slot + 1, 0 = a CPU) */
    int8_t resultCount;
    int8_t fieldCount;
    BuiResultView result[4];
    int8_t fieldChar[BUI_FIELD_MAX];
    int8_t fieldHuman[BUI_FIELD_MAX];
    BuiRoomView room[BUI_ROOMS_MAX];
    BuiSlotView slot[4];
    /* mp-online-hw (zero = the views and goldens before it) */
    int8_t haveDirect; /* ONLINE: JOIN BY ADDRESS ([net] mp_direct names a host) */
    int8_t relayOnly;  /* the online room relays every link (the lobby's badge says RELAY) */
    int8_t showCode;   /* serverless host: the WiFi lobby shows ROOM <code> to read out */
    char directAddr[24];
    /* mp-menus edge cases (zero = the views and goldens before them) */
    int8_t courseSecs;    /* host: the course step's countdown, 0 = none */
    int8_t awayStartable; /* host: players a START WITHOUT takes (>= 2), 0 = not offered */
    char awayName[BUI_NAME_CAP + 1]; /* ... the first member that went quiet */
    int8_t roomClosed;    /* the room went away under the last race: its results stay, LEAVE only */
    char closedWhy[24];   /* ... why ("THE HOST LEFT") */
    int8_t raceWait;      /* JOINING: in the room, but it is racing: in when the race ends */
    char endedBy[BUI_NAME_CAP + 1]; /* #10: the race was ended from the pause menu by ("" = none) */
} BuiMpView;

/* mp-menus: the course editor's import (gdx3ds_course_share.c's GdxCsUi as the screens see it). */
#define BUI_CS_FILES_MAX 16
#define BUI_CS_ROWS_PER_PAGE 4
#define BUI_CS_SLOTS 6
#define BUI_CS_NAME_CAP 8
/* SCAN: where the camera's picture goes (4:3, clear of the right-hand panel and the status line) */
#define BUI_CS_PREVIEW_X 4
#define BUI_CS_PREVIEW_Y 28
#define BUI_CS_PREVIEW_W 216
#define BUI_CS_PREVIEW_H 162

typedef struct BuiCsFileView {
    char name[BUI_CS_NAME_CAP + 1]; /* the course's name ("" when the file was refused) */
    char leaf[24];                  /* the file name on the SD card */
    char why[16];                   /* refused: the reason ("BAD CRC" ...) */
    int8_t ok;
    int8_t points;
    int8_t finished; /* CRSD (raceable) vs CRSE (unfinished) */
} BuiCsFileView;

typedef struct BuiCsView {
    int8_t canWrite; /* the editor is idle: an import may write now */
    int8_t fileCount;
    int8_t page, pages;
    BuiCsFileView file[BUI_CS_FILES_MAX];
    /* the pending import (the slot picker and the replace dialog) */
    char pendingName[BUI_CS_NAME_CAP + 1];
    int8_t pendingPoints;
    int8_t pendingFinished;
    int8_t pendingSlot; /* -1 = library */
    int8_t replacesDisk; /* the disk already holds a course of that name */
    char cup[BUI_CS_SLOTS][BUI_CS_NAME_CAP + 1];
    /* scan */
    int8_t scanHave, scanCount;
    uint16_t scanPages; /* bit = page held */
    int8_t scanZoom;
    int8_t cameraOn;
    char status[48]; /* the newest status line */
} BuiCsView;

typedef struct BuiView {
    int16_t screen;  /* BUI_SCR_* */
    int16_t focus;   /* widget id with the yellow ring, BUI_W_NONE = none */
    int8_t anim;     /* spinner phase 0..7 (only screens with a spinner read it) */
    int8_t model;    /* this console: BUI_MODEL_* */
    int8_t ek;
    int8_t stereoButton; /* [debug] bui_3d_button: Home shows the 3D ON / OFF button */
    int8_t stereoOn;     /* ... and what it says */
    char name[BUI_NAME_CAP + 1];
    char version[24]; /* the build string players must share ("V1.0.2 EK") */
    int8_t homePilot;
    /* entry screens (code / name) */
    char entry[BUI_NAME_CAP + 1];
    /* host sheet */
    int8_t hostMedium; /* BUI_MEDIUM_WIFI / _UDS */
    int8_t roomSize;   /* 2..4 */
    /* machine picker */
    int8_t unlocked;   /* machines unlocked on this console (slots 0..n-1 of the grid) */
    int8_t pickSlot;   /* the grid slot the details panel shows */
    int8_t stats[3];   /* BODY / BOOST / GRIP: 0 = A .. 4 = E */
    int8_t skin;       /* 0..3 */
    int8_t engine;     /* 0..100 */
    uint8_t skinRgb[4][3];
    int8_t grid[BUI_MACHINES]; /* grid slot -> character (the game's machine-select order) */
    /* course picker */
    int8_t cup; /* 0..3 */
    /* overlays */
    int8_t dialog; /* BUI_DLG_* */
    int8_t dialogFocus;
    char dialogName[24]; /* who the dialog is about ("KENJI'S 3DS") */
    char dialogCode[16];
    char toast[48];
    int8_t toastTone; /* BUI_TONE_* */
    int8_t toastIcon; /* BUI_ICON_* */
    BuiMpView mp;
    /* mp-menus (zero = the views and goldens before it) */
    int8_t buttonsOff;   /* the game has the buttons now: no key glyph is drawn (touch only) */
    int8_t holdConfirm;  /* [ui] hold_confirm_ms > 0: START RACE / LEAVE are held, the buttons say HOLD */
    int16_t holdWidget;  /* the widget being held (BUI_W_NONE = none) */
    int8_t holdPct;      /* ... and how far (0..100) */
    int8_t keepMachine;  /* stock picks: the previous pick a KEEP button confirms, -1 = none */
    BuiCsView cs;
    /* card-4 version gating (zero = the views and goldens before it) */
    char dialogMine[8];   /* "^" in a dialog line: this console's version number */
    char dialogExtra[44]; /* "%" in a dialog line: the server's --update-url */
    /* release (zero = the views and goldens before it) */
    int8_t settingsButton; /* Home shows the SETTINGS button (the player settings, gdx3ds_menu.c) */
} BuiView;

/* Zeroes a view with no focus, no dialog. */
void bui_view_init(BuiView* v);
/* FNV-1a over the view's bytes: a repaint is due exactly when it changes. */
uint32_t bui_view_key(const BuiView* v);

/* Paints v's screen (and its dialog / toast) over the whole 320x240 surface the kit targets, and
 * rebuilds `ws` with the widgets it drew. */
void bui_screen_paint(const BuiView* v, BuiWidgets* ws);

/* Overlay-only repaints (the shell's cost path). bui_view_base_key: the key of the view without
 * its dialog and toast. bui_screen_paint_dialog: the dialog alone over what the surface holds --
 * shade = 1 over the bare screen (it darkens it first), 0 over the same dialog already drawn
 * (a focus move: the panel repaints itself exactly). Both give the bytes bui_screen_paint does. */
uint32_t bui_view_base_key(const BuiView* v);
void bui_screen_paint_dialog(const BuiView* v, BuiWidgets* ws, int shade);

/* Data the screens share with the shell. */
const char* bui_course_name(int course);     /* 0..23, caps */
/* Any raceable course's card text: the cup ("JACK CUP", "DD-1 CUP") and name ("COURSE 3" for a
 * DD-cup course, whose name lives on the disk). */
void bui_course_label(int course, char* cup, int cupCap, char* name, int nameCap);
const char* bui_cup_name(int cup);           /* JACK QUEEN KING JOKER */
const char* bui_pilot_name(int character);   /* FALCON ... */
const char* bui_machine_name(int character); /* BLUE FALCON ... */
/* The code alphabet (32 keys, no I O 0 1) and the name alphabet (40 keys, SP = space). */
extern const char kBuiCodeKeys[33];
extern const char kBuiNameKeys[41];
/* The dialog's button count, for the shell's focus handling. */
int bui_dialog_buttons(int dialog);
/* v's dialog body as painted ("@" / "#" / "^" / "%" expanded); returns the line count. */
int bui_dialog_text(const BuiView* v, char lines[5][48]);
/* The title of a dialog (NULL for none). */
const char* bui_dialog_title(int dialog);

/* mp-menus: the widget a key activates on v's screen (and dialog), the one the shell runs for that
 * key and the one each hint glyph names; BUI_W_NONE = the key does nothing there. A returns the
 * focused widget (or the screen's one A action). The host tests hold every drawn hint to it. */
int bui_screen_key(const BuiView* v, int key);
/* uipolish: one D-pad press on v's screen (v->focus is the focus now). out->focus is where focus
 * goes; out->activate (BUI_W_NONE = nothing) is a widget the press also runs, as a tap would: on
 * the row screens (HOST, RACE SETTINGS) up/down walk the rows and left/right change the row's
 * value (connection, room size, laps, machines). Elsewhere: bui_widgets_nav. 0 = no direction. */
typedef struct BuiNav {
    int focus;
    int activate;
} BuiNav;
int bui_screen_nav(const BuiView* v, const BuiWidgets* ws, int dx, int dy, BuiNav* out);
/* BUI_KEY_* of a hint glyph's text ("A", "ST", "L" ...), BUI_KEY_NONE for anything else. */
int bui_key_from_glyph(const char* glyph);
/* 1 = activating widget id on v's screen is held to confirm (START RACE, LEAVE / CLOSE the room)
 * when v->holdConfirm is set. */
int bui_screen_hold_widget(const BuiView* v, int id);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_BUI_SCREENS_H */
