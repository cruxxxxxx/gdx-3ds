/* port/3ds/gdx3ds_course_share.h -- `[ek] course_share`: share Expansion Kit custom courses.
 * Expansion Kit builds only (the menu's CRS tab is `#ifdef GDX3DS_COURSE_SHARE`, a definition
 * only the EK build sets, so the shipped game compiles none of it).
 *
 * WHAT IT DOES. The bottom-screen menu gets a CRS tab with two lists: the courses on the
 * mounted 64DD disk (the editor's library) and the `.gdxcourse` files in
 * sdmc:/3ds/gdiffuser/courses/. A disk course can be EXPORTED to a file, or shown as a series of
 * QR codes; a file can be IMPORTED into the library, optionally straight into one of the six
 * edit-cup slots, with a confirmation whenever it would replace something. A disk course can
 * also be DELETED (confirmed), so a course can be exported, removed and brought back. The file format and
 * every rule an imported file must pass: gdx3ds_course_share_format.h. The QR transport:
 * gdx3ds_course_share_qr.h. The MFS side: gdx3ds_course_share_game.h.
 *
 * WHEN. Reading (list, export, QR) works on the main menu and in the course editor. Delete only
 * on the main menu, the one screen with no course loaded. Import (and SCAN, which imports what it
 * reads) on the main menu or in the course editor while the editor is idle
 * (gdxcs_game_can_import).
 *
 * WHERE (mp-menus). The CRS tab of the developer console lists the disk (EXPORT, QR, DELETE).
 * Bringing a course IN -- the SD files and the QR scan -- is the course editor's bottom-screen
 * menu (gdx3ds_bui.c's EDITOR screens) through the gdx3ds_cshare_ui_* calls below.
 *
 * SCAN. The editor menu's SCAN QR PAGES opens the outer camera with a live preview; pages are collected
 * in any order (duplicates and pages of another course are told apart and ignored), and the
 * whole file then goes through the same parser and slot picker as an SD file. B or [CANCEL]
 * stops it; leaving the tab, a takeover of the bottom screen, HOME and sleep stop the camera.
 * Camera, worker and memory: gdx3ds_course_share_cam.h. Pipeline: gdx3ds_course_share_scan.h.
 * Debug levers ([debug]): cshare_scan_file = <bmp or dir> feeds BMP frames instead of the
 * camera; cshare_scan_fail = 1 fails the camera start; cshare_scan_prio = 0x18..0x3F sets the
 * decoder thread's priority (default: one below the main thread).
 *
 * LEVERS (gdiffuser.ini [ek]):
 *   course_share = 0|1        default 0. 0: no CRS tab, no SD access, nothing runs. Receipt
 *                             `[cshare] init ...` either way.
 *   course_share_auto = ...   measurement lever (default empty): actions run in order, each as
 *                             soon as the game state allows it, separated by '|' (the ini
 *                             reader treats ';' and '#' as comments):
 *                               export:NAME          disk course -> SD file
 *                               import:FILE:SLOT     SD file (leaf name) -> library, and cup slot
 *                                                    SLOT 1..6 (0 = library only)
 *                               delete:NAME          remove a disk course (round-trip tests)
 *                               qr:NAME:PAGE[:SIZE]  open the CRS tab on that course's QR page;
 *                                                    SIZE large (default) or dense
 *                               scan:SLOT            open the SCAN view; once a whole set is
 *                                                    read, import it into cup SLOT 1..6 (0 =
 *                                                    library only). Later actions wait for it.
 *                               wait:TICKS           hold the list for TICKS console ticks
 *                               shot:LABEL           bottom screen -> autotest/LABEL_R.bmp (the
 *                                                    replay SHOT format), e.g. a QR page for
 *                                                    another instance's cshare_scan_file
 *                             Every action logs one `[cshare] auto ...` receipt. Actions run
 *                             from the settings console's tick, so only while the console is on
 *                             the bottom screen: pair with [debug] bottom_idle = 0 for an
 *                             unattended run (the idle view otherwise owns the screen at menus).
 *   course_share_auto2..4 = ...  continue the list in order (an ini value holds 63
 *                             characters; a value that long logs a truncation warning).
 *   course_share_auto_yes = 0|1  default 0: auto actions that would replace something are
 *                             refused with a receipt unless this is 1.
 */
#ifndef GDX3DS_COURSE_SHARE_H
#define GDX3DS_COURSE_SHARE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GdxCsRowPainter)(int row, const char* text);

void gdx3ds_cshare_init(void);
int gdx3ds_cshare_enabled(void);

/* Menu hooks, all main thread, all no-ops with the lever off. tick and touch return 1 when the
 * page needs a repaint; wants_tab is 1 once when an auto action asks for the CRS tab. */
int gdx3ds_cshare_tick(void);
int gdx3ds_cshare_touch(int row, int col);
int gdx3ds_cshare_wants_tab(void);
void gdx3ds_cshare_paint_page(GdxCsRowPainter paintRow);
/* The QR picture, when the page shows one; called after the text paint of a frame. */
void gdx3ds_cshare_paint_pixels(uint16_t* fb);
/* Entering the tab re-reads the list it shows; leaving it drops any QR view and pending
 * confirmation. */
void gdx3ds_cshare_on_tab_entered(void);
void gdx3ds_cshare_on_tab_left(void);
/* Buttons pressed this frame (hid down edges), while the tab's own UI runs: B cancels a scan.
 * Returns 1 when the page needs a repaint. */
int gdx3ds_cshare_keys(uint32_t down);
/* Once per menu tick, last: `crsLive` is 1 when the CRS tab's UI ran this tick on a lit screen.
 * A scan the player can no longer see is stopped (camera off, buffers freed). `held` updates the
 * game-input mask below. */
void gdx3ds_cshare_frame_end(int crsLive, uint32_t held);
/* hid keys the game must not see this frame (the input poll, gdx3ds_os_ctru.c, clears them):
 * every button while the SCAN view is up, then each until it is released. 0 with the lever off. */
uint32_t gdx3ds_cshare_game_key_mask(void);

/* ---- mp-menus: the course editor's bottom menu (gdx3ds_bui.c) ---------------------------------
 * The import half of the CRS tab, as data and actions for the bottom-screen menus. All main
 * thread, no-ops with the lever off. The confirmations and refusals are the CRS tab's own. */
#define GDXCS_UI_FILES_MAX 16

enum {
    GDXCS_UI_IDLE = 0, /* nothing of the import open */
    GDXCS_UI_FILES,    /* the .gdxcourse files */
    GDXCS_UI_SLOT,     /* a file / a scanned course picked: where it goes */
    GDXCS_UI_REPLACE,  /* ... and it would replace something: confirm */
    GDXCS_UI_SCAN      /* the camera */
};

typedef struct GdxCsUiFile {
    char name[9];  /* the course's name ("" when refused) */
    char leaf[24]; /* the file name */
    char why[16];  /* refused: the reason */
    int8_t ok;
    int8_t points;
    int8_t finished;
} GdxCsUiFile;

typedef struct GdxCsUi {
    int8_t step;      /* GDXCS_UI_* */
    int8_t canImport; /* gdxcs_game_can_import() now */
    int8_t fileCount;
    GdxCsUiFile file[GDXCS_UI_FILES_MAX];
    char pendingName[9];
    int8_t pendingPoints;
    int8_t pendingFinished;
    int8_t pendingSlot;   /* -1 = library */
    int8_t replacesDisk;  /* the library already holds that name */
    char replaceWhat[16]; /* REPLACE: "CUP SLOT 3" / "LIBRARY" */
    char cup[6][9];
    int8_t scanHave, scanCount;
    uint16_t scanPages;
    int8_t scanZoom;
    int8_t cameraOn;
    char status[120];
} GdxCsUi;

void gdx3ds_cshare_ui(GdxCsUi* out);
/* Actions; each returns 1 when what the menu shows changed. */
int gdx3ds_cshare_ui_open_files(void); /* (re)reads the SD files: FILES */
int gdx3ds_cshare_ui_pick_file(int index); /* FILES -> SLOT (or a refusal in the status) */
int gdx3ds_cshare_ui_import_into(int slot); /* SLOT: -1 library, 0..5 cup slot -> done or REPLACE */
int gdx3ds_cshare_ui_confirm(int yes); /* REPLACE: write it (yes) or back to SLOT */
int gdx3ds_cshare_ui_back(void);       /* SLOT -> FILES, FILES -> IDLE */
int gdx3ds_cshare_ui_open_scan(void);  /* -> SCAN (or a refusal in the status) */
int gdx3ds_cshare_ui_scan_zoom(void);
int gdx3ds_cshare_ui_scan_restart(void);
int gdx3ds_cshare_ui_scan_cancel(void); /* SCAN -> IDLE */
void gdx3ds_cshare_ui_close(void);      /* leave all of it (camera off) */
/* Once per menu tick while the menu shows the import: the camera and the scan's progress.
 * Returns 1 when what the menu shows changed. Marks the scan as seen (frame_end keeps it on). */
int gdx3ds_cshare_ui_tick(void);
/* The camera's picture into fb at (x0, y0, w, h) (4:3), after the menu's own paint; force = the
 * menu just repainted over it. */
void gdx3ds_cshare_ui_scan_paint(uint16_t* fb, int x0, int y0, int w, int h, int force);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_COURSE_SHARE_H */
