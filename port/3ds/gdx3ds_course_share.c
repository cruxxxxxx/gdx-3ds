/* port/3ds/gdx3ds_course_share.c -- `[ek] course_share`: the CRS tab, SD files and the auto
 * lever. Contract: gdx3ds_course_share.h. Expansion Kit builds only.
 *
 * Painting follows the menu's discipline (gdx3ds_menu.c): integer-only snprintf into fixed
 * buffers, rows painted through the menu's own row painter, actions on the touch-down edge.
 * Everything that touches the disk goes through gdx3ds_course_share_game.c, which owns the
 * state gate; everything that touches a file goes through gdxcs_decode_file before a single
 * byte of it is used. */
#include "gdx3ds_course_share.h"

#include "gdx3ds_course_share_cam.h"
#include "gdx3ds_course_share_format.h"
#include "gdx3ds_course_share_game.h"
#include "gdx3ds_course_share_qr.h"
#include "gdx3ds_course_share_scan.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_menu.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <3ds.h>

#include "gdx3ds_config.h"

/* ---- layout (console cells: 40 x 30, rows/cols 1-based) ------------------------------- */
#define ROW_TITLE 3
#define ROW_VIEWS 4
#define ROW_LIST0 6
#define LIST_ROWS 16
#define ROW_ACTIONS 23
#define ROW_STATUS 25
#define STATUS_ROWS 3
#define ROW_SLOT0 6 /* PICK_SLOT: library row, then six cup rows */
#define ROW_CONFIRM_YES 14
/* DENSE pages (version 1, QR v10): 3 px a module, the standard 4-module quiet zone */
#define QR_X0 8
#define QR_Y0 24
#define QR_SCALE 3
#define QR_QUIET 4
/* LARGE pages (version 2, QR v6, the default): 5 px a module, a 1-module quiet zone (the scanner
 * and phone readers need little more; the page around it is the console's black), rows 4..30 */
#define QR_LARGE_X0 2
#define QR_LARGE_Y0 24
#define QR_LARGE_SCALE 5
#define QR_LARGE_QUIET 1
#define QR_PANEL_COL 29
#define QR_ROW_SIZE 22
#define FB_W 320
#define FB_H 240
/* SCAN: the preview (4:3, y 24..189 with its 2-px frame, clear of the status rows at y 192+)
 * and the panel to its right */
#define SCAN_PREVIEW_X0 2
#define SCAN_PREVIEW_Y0 26
#define SCAN_PREVIEW_W 216
#define SCAN_PREVIEW_H 162
#define SCAN_PANEL_COL 31
#define SCAN_ROW_PAGES 4
#define SCAN_ROW_ZOOM 11
#define SCAN_ROW_RESTART 14
#define SCAN_ROW_CANCEL 17
#define SCAN_FLASH_FRAMES 20
#define RGB565_IDLE 0x4208u
#define RGB565_GREEN 0x07E0u
#define RGB565_YELLOW 0xFFE0u
#define RGB565_RED 0xF800u
#define HID_KEY_B (1u << 1) /* libctru KEY_B */
#define HID_KEY_TOUCH (1u << 20)

#define SD_FILES_MAX 64
#define SD_LEAF_MAX 48
#define STATUS_MAX 120
#define AUTO_MAX 256
#define CFG_VALUE_CHARS 63 /* GDX3DS_CFG_VALUE_LEN - 1 (os/gdx3ds_config.c) */
#define AUTO_KEYS 4 /* course_share_auto .. course_share_auto4 */
#define AUTO_SEPARATOR '|' /* not ';': the ini reader cuts a value at ';' or '#' (comments) */

typedef enum ShareView { VIEW_DISK = 0, VIEW_SD, VIEW_PICK_SLOT, VIEW_CONFIRM, VIEW_QR, VIEW_SCAN } ShareView;
typedef enum PendingOp { OP_NONE = 0, OP_EXPORT, OP_IMPORT, OP_DELETE } PendingOp;

typedef struct SdEntry {
    char leaf[SD_LEAF_MAX];
    char name[GDXCS_NAME_MAX + 1];
    GdxCsStatus status;
    int8_t points;
    uint8_t kind;
} SdEntry;

typedef struct Pending {
    PendingOp op;
    int slot; /* import: -1 library only, 0..5 cup slot */
    char name[GDXCS_NAME_MAX + 1];
    uint8_t kind;
    char leaf[SD_LEAF_MAX];
    GdxCsCourse course;
} Pending;

static int sEnabled = 0;
static int sAutoYes = 0;
static ShareView sView = VIEW_DISK;
static ShareView sListView = VIEW_DISK; /* the list a dialog returns to */
static int sSelected = 0;
static int sPage = 0;
static int sWantsTab = 0;
static int sPixelsDirty = 0;
static int sDiskListStale = 1; /* the disk list could not be read when last shown */
static char sStatus[STATUS_MAX] = "";

static GdxCsDiskEntry sDisk[GDXCS_LIBRARY_MAX];
static int sDiskCount = 0;
static SdEntry sSd[SD_FILES_MAX];
static int sSdCount = 0;
static Pending sPending;

static GdxCsQrSet sQrSet;
static uint8_t sQrCode[GDXCS_QR_BUFFER_BYTES];
static int sQrPage = 0;
static int sQrModules = 0;
static int sQrLarge = 1;
static uint8_t sQrKind = 0;
static char sQrName[GDXCS_NAME_MAX + 1];

static GdxCsScanSession sScan;
static int sScanZoom = 2;
static int sScanAutoSlot = 0;   /* auto lever scan: 1 = import when done, into sScanAutoSlotIndex */
static int sScanAutoSlotIndex = -1;
static int sScanFlash = 0;      /* frames left of the preview frame's colour */
static uint16_t sScanFlashColor = RGB565_IDLE;
static u64 sScanStartTick = 0;
static u64 sScanPaintTick = 0;
static uint32_t sGameKeyMask = 0; /* read by the input poll (gdx3ds_os_ctru.c) */

static char sAuto[AUTO_MAX];
static char* sAutoNext = NULL; /* next unparsed action in sAuto, NULL when done */
static int sAutoWait = 0;      /* wait:TICKS -- console ticks left before the next action */

/* mp-menus: the course editor's bottom menu runs the import (gdx3ds_cshare_ui_*) */
static int sUiOwned = 0; /* the import views belong to the bottom-screen menu, not the CRS tab */
static int sUiLive = 0;  /* the menu ticked the import this frame (frame_end keeps a scan on) */

static void ShowList(ShareView view);

/* ---- receipts -------------------------------------------------------------------------- */

static void ShareLog(const char* fmt, ...) {
    char buf[200];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int) sizeof(buf) - 1) {
            n = (int) sizeof(buf) - 1;
        }
        svcOutputDebugString(buf, (size_t) n);
        gdx3ds_filelog_write(buf, (size_t) n);
    }
}

static void SetStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sStatus, sizeof(sStatus), fmt, ap);
    va_end(ap);
}

static uint32_t CourseRecordCrc(const GdxCsCourse* course) {
    uint8_t be[GDXCS_COURSE_BYTES];
    gdxcs_course_to_be(course, be);
    return gdxcs_crc32(be, sizeof(be));
}

/* The mounted disk's identity, with a one-time receipt of what it is. */
static int CurrentDiskId(GdxCsDiskId* disk) {
    static int sLogged = 0;
    if (gdxcs_game_disk_id(disk) != 0) {
        return -1;
    }
    if (!sLogged) {
        sLogged = 1;
        ShareLog("[cshare] disk id=%016llX region=%c", (unsigned long long) disk->crc64, (char) disk->region);
    }
    return 0;
}

/* ---- init ------------------------------------------------------------------------------ */

/* The ini reader keeps at most CFG_VALUE_CHARS characters of a value, silently: a value that
 * long was probably cut, and its last action would run truncated. */
static void WarnIfAutoTruncated(const char* key, const char* value) {
    if (value != NULL && strlen(value) >= CFG_VALUE_CHARS) {
        ShareLog("[cshare] WARNING: %s is %d characters, the ini limit: it may be cut short "
                 "(continue the list in the next course_share_autoN)", key, CFG_VALUE_CHARS);
    }
}

/* course_share_auto2..AUTO_KEYS continue course_share_auto's list (one ini value holds 63
 * characters). */
static void AppendAutoActions(const char* actions) {
    size_t used = strlen(sAuto);
    if (actions == NULL || actions[0] == '\0') {
        return;
    }
    if (used > 0) {
        snprintf(sAuto + used, sizeof(sAuto) - used, "%c%s", AUTO_SEPARATOR, actions);
    } else {
        snprintf(sAuto, sizeof(sAuto), "%s", actions);
    }
}

/* course_share_auto, course_share_auto2, ... course_share_auto<AUTO_KEYS> */
static const char* AutoKey(int k) {
    static char key[24];
    if (k <= 1) {
        return "course_share_auto";
    }
    snprintf(key, sizeof(key), "course_share_auto%d", k);
    return key;
}

void gdx3ds_cshare_init(void) {
    int k;
    sEnabled = gdx3ds_config_get_bool("ek", "course_share", 0) ? 1 : 0;
    sAutoYes = gdx3ds_config_get_bool("ek", "course_share_auto_yes", 0) ? 1 : 0;
    sAuto[0] = '\0';
    sAutoNext = NULL;
    if (sEnabled) {
        for (k = 1; k <= AUTO_KEYS; k++) {
            AppendAutoActions(gdx3ds_config_get_string("ek", AutoKey(k), ""));
        }
    }
    if (sAuto[0] != '\0') {
        sAutoNext = sAuto;
    }
    ShareLog("[cshare] init enabled=%d dir=%s file_bytes=%u qr_chunk=%d auto=\"%s\" auto_yes=%d", sEnabled,
             GDXCS_SD_DIR, (unsigned) GDXCS_FILE_BYTES, GDXCS_QR_CHUNK, sAuto, sAutoYes);
    for (k = 1; k <= AUTO_KEYS; k++) {
        WarnIfAutoTruncated(AutoKey(k), gdx3ds_config_get_string("ek", AutoKey(k), ""));
    }
}

int gdx3ds_cshare_enabled(void) {
    return sEnabled;
}

int gdx3ds_cshare_wants_tab(void) {
    const int wants = sWantsTab;
    sWantsTab = 0;
    return wants;
}

/* ---- disk list ----------------------------------------------------------------------- */

static int RefreshDiskList(void) {
    if (gdxcs_game_state() < GDXCS_GAME_READ_ONLY) {
        sDiskCount = 0;
        sDiskListStale = 1;
        return -1;
    }
    sDiskListStale = 0;
    sDiskCount = gdxcs_game_list(sDisk, GDXCS_LIBRARY_MAX);
    if (sDiskCount > GDXCS_LIBRARY_MAX) {
        sDiskCount = GDXCS_LIBRARY_MAX;
    }
    return 0;
}

static const GdxCsDiskEntry* FindDiskEntry(const char* name) {
    int i;
    for (i = 0; i < sDiskCount; i++) {
        if (strcmp(sDisk[i].name, name) == 0) {
            return &sDisk[i];
        }
    }
    return NULL;
}

/* ---- SD files -------------------------------------------------------------------------- */

static void EnsureDirectory(const char* path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        ShareLog("[cshare] WARNING: mkdir %s failed errno=%d", path, errno);
    }
}

static void EnsureCourseDirectory(void) {
    EnsureDirectory("sdmc:/3ds");
    EnsureDirectory("sdmc:/3ds/gdiffuser");
    EnsureDirectory(GDXCS_SD_DIR);
}

static int BuildPath(char* out, size_t cap, const char* leaf) {
    const int n = snprintf(out, cap, "%s/%s", GDXCS_SD_DIR, leaf);
    return n > 0 && (size_t) n < cap ? 0 : -1;
}

/* Reads a whole candidate file, refusing anything not exactly the format's size: the length is
 * the one thing an attacker controls before any check has run, so it never sizes a buffer. */
static GdxCsStatus ReadCourseFile(const char* leaf, uint8_t file[GDXCS_FILE_BYTES]) {
    char path[128];
    uint8_t probe;
    FILE* f;
    size_t got;
    if (BuildPath(path, sizeof(path), leaf) != 0) {
        return GDXCS_ERR_ARG;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return GDXCS_ERR_ARG;
    }
    got = fread(file, 1, GDXCS_FILE_BYTES, f);
    if (got == GDXCS_FILE_BYTES && fread(&probe, 1, 1, f) == 1) {
        got++; /* longer than a .gdxcourse */
    }
    fclose(f);
    return got == GDXCS_FILE_BYTES ? GDXCS_OK : GDXCS_ERR_SIZE;
}

static GdxCsStatus DecodeCourseFile(const char* leaf, GdxCsCourse* course, char* name) {
    uint8_t file[GDXCS_FILE_BYTES];
    GdxCsDiskId disk;
    GdxCsStatus status = ReadCourseFile(leaf, file);
    if (status != GDXCS_OK) {
        return status;
    }
    if (CurrentDiskId(&disk) != 0) {
        return GDXCS_ERR_DISK;
    }
    return gdxcs_decode_file(file, sizeof(file), &disk, course, name);
}

static int HasCourseExtension(const char* leaf) {
    const size_t n = strlen(leaf);
    const size_t e = strlen(GDXCS_FILE_EXT);
    return n > e && strcasecmp(leaf + n - e, GDXCS_FILE_EXT) == 0;
}

static void AddSdEntry(const char* leaf) {
    SdEntry* e = &sSd[sSdCount];
    GdxCsCourse course;
    memset(e, 0, sizeof(*e));
    snprintf(e->leaf, sizeof(e->leaf), "%s", leaf);
    e->status = DecodeCourseFile(leaf, &course, e->name);
    if (e->status == GDXCS_OK) {
        e->points = course.controlPointCount;
        e->kind = course.flag;
    }
    sSdCount++;
}

static void RefreshSdList(void) {
    DIR* dir;
    struct dirent* ent;
    sSdCount = 0;
    EnsureCourseDirectory();
    dir = opendir(GDXCS_SD_DIR);
    if (dir == NULL) {
        return;
    }
    while ((ent = readdir(dir)) != NULL && sSdCount < SD_FILES_MAX) {
        if (strlen(ent->d_name) < SD_LEAF_MAX && HasCourseExtension(ent->d_name)) {
            AddSdEntry(ent->d_name);
        }
    }
    closedir(dir);
}

static int FileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int WriteFileVerified(const char* path, const uint8_t* bytes, size_t length) {
    uint8_t back[GDXCS_FILE_BYTES];
    FILE* f = fopen(path, "wb");
    size_t wrote;
    if (f == NULL) {
        return -1;
    }
    wrote = fwrite(bytes, 1, length, f);
    if (fclose(f) != 0 || wrote != length) {
        return -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    wrote = fread(back, 1, sizeof(back), f);
    fclose(f);
    return wrote == length && memcmp(back, bytes, length) == 0 ? 0 : -1;
}

/* ---- operations ------------------------------------------------------------------------- */

static const char* KindExt(uint8_t kind) {
    return gdxcs_extension_for_flag(kind);
}

static int ReadDiskCourse(const char* name, uint8_t kind, GdxCsCourse* course) {
    int mfsError = 0;
    const int rc = gdxcs_game_read(name, kind, course, &mfsError);
    if (rc != 0) {
        SetStatus("read %s.%s failed (%d, %d)", name, KindExt(kind), rc, mfsError);
        ShareLog("[cshare] read %s.%s rc=%d err=%d", name, KindExt(kind), rc, mfsError);
    }
    return rc;
}

static int EncodeDiskCourse(const GdxCsCourse* course, const char* name, uint8_t* file) {
    GdxCsDiskId disk;
    size_t length = 0;
    GdxCsStatus status;
    if (CurrentDiskId(&disk) != 0) {
        SetStatus("no disk id");
        return -1;
    }
    status = gdxcs_encode_file(course, name, &disk, file, GDXCS_FILE_BYTES, &length);
    if (status != GDXCS_OK) {
        SetStatus("%s: not exportable (%s)", name, gdxcs_status_name(status));
        ShareLog("[cshare] export %s refused status=%s", name, gdxcs_status_name(status));
        return -1;
    }
    return 0;
}

/* Returns 1 when it needs a confirmation (the file exists and `confirmed` is 0). */
static int ExportCourse(const char* name, uint8_t kind, int confirmed) {
    GdxCsCourse course;
    uint8_t file[GDXCS_FILE_BYTES];
    char leaf[SD_LEAF_MAX];
    char path[128];
    if (ReadDiskCourse(name, kind, &course) != 0 || EncodeDiskCourse(&course, name, file) != 0) {
        return -1;
    }
    gdxcs_sd_filename(name, leaf, sizeof(leaf));
    EnsureCourseDirectory();
    if (BuildPath(path, sizeof(path), leaf) != 0) {
        return -1;
    }
    if (!confirmed && FileExists(path)) {
        return 1;
    }
    if (WriteFileVerified(path, file, sizeof(file)) != 0) {
        SetStatus("write %s failed", leaf);
        ShareLog("[cshare] export %s -> %s WRITE FAILED", name, path);
        return -1;
    }
    SetStatus("exported %s -> %s", name, leaf);
    ShareLog("[cshare] export %s.%s pts=%d -> %s bytes=%u file_crc=%08lX rec_crc=%08lX ok", name, KindExt(kind),
             course.controlPointCount, path, (unsigned) sizeof(file),
             (unsigned long) gdxcs_crc32(file, sizeof(file)), (unsigned long) CourseRecordCrc(&course));
    return 0;
}

static int ImportNeedsConfirm(const char* name, int slot) {
    char cup[GDXCS_CUP_SLOTS][GDXCS_NAME_MAX + 1];
    if (FindDiskEntry(name) != NULL) {
        return 1;
    }
    if (slot < 0) {
        return 0;
    }
    gdxcs_game_cup_names(cup);
    return cup[slot][0] != '\0' && strcmp(cup[slot], name) != 0;
}

static int VerifyImported(const char* name, const GdxCsCourse* course) {
    GdxCsCourse back;
    int mfsError = 0;
    if (gdxcs_game_read(name, course->flag, &back, &mfsError) != 0) {
        return -1;
    }
    return memcmp(&back, course, sizeof(back)) == 0 ? 0 : -1;
}

/* Returns 1 when it needs a confirmation. */
static int ImportCourse(const char* name, const GdxCsCourse* course, int slot, int confirmed) {
    int mfsError = 0;
    int rc;
    RefreshDiskList();
    if (!confirmed && ImportNeedsConfirm(name, slot)) {
        return 1;
    }
    rc = gdxcs_game_write(name, course, slot, &mfsError);
    if (rc != 0) {
        SetStatus("import %s failed (%d, %d)", name, rc, mfsError);
        ShareLog("[cshare] import %s slot=%d FAILED rc=%d err=%d", name, slot + 1, rc, mfsError);
        return -1;
    }
    rc = VerifyImported(name, course);
    RefreshDiskList();
    if (slot >= 0) {
        SetStatus("imported %s into cup slot %d%s", name, slot + 1, rc == 0 ? "" : " (VERIFY FAILED)");
    } else {
        SetStatus("imported %s into the library%s", name, rc == 0 ? "" : " (VERIFY FAILED)");
    }
    ShareLog("[cshare] import %s.%s pts=%d slot=%d rec_crc=%08lX verify=%s free=%ld", name, KindExt(course->flag),
             course->controlPointCount, slot + 1, (unsigned long) CourseRecordCrc(course), rc == 0 ? "ok" : "MISMATCH",
             (long) gdxcs_game_free_bytes());
    return rc == 0 ? 0 : -1;
}

static int OpenQr(const char* name, uint8_t kind, int page) {
    GdxCsCourse course;
    uint8_t file[GDXCS_FILE_BYTES];
    if (ReadDiskCourse(name, kind, &course) != 0 || EncodeDiskCourse(&course, name, file) != 0) {
        return -1;
    }
    if (gdxcs_qr_build_sized(file, sizeof(file), sQrLarge ? GDXCS_QR_CHUNK_LARGE : GDXCS_QR_CHUNK, &sQrSet) != 0) {
        SetStatus("qr build failed");
        return -1;
    }
    snprintf(sQrName, sizeof(sQrName), "%s", name);
    sQrKind = kind;
    sQrPage = page >= 0 && page < sQrSet.count ? page : 0;
    sQrModules = gdxcs_qr_encode(sQrSet.frame[sQrPage], sQrSet.frameLength[sQrPage], sQrCode);
    sView = VIEW_QR;
    sPixelsDirty = 1;
    ShareLog("[cshare] qr %s size=%s pages=%d page=%d modules=%d file_crc=%08lX", name, sQrLarge ? "large" : "dense",
             sQrSet.count, sQrPage + 1, sQrModules, (unsigned long) gdxcs_crc32(file, sizeof(file)));
    return 0;
}

static void QrTurnPage(int delta) {
    if (sQrSet.count <= 0) {
        return;
    }
    sQrPage = (sQrPage + delta + sQrSet.count) % sQrSet.count;
    sQrModules = gdxcs_qr_encode(sQrSet.frame[sQrPage], sQrSet.frameLength[sQrPage], sQrCode);
    sPixelsDirty = 1;
}

static int DeleteCourse(const char* name) {
    int mfsError = 0;
    const int rc = gdxcs_game_delete(name, &mfsError);
    RefreshDiskList();
    ShareLog("[cshare] delete %s rc=%d err=%d", name, rc, mfsError);
    return rc;
}


/* ---- scan --------------------------------------------------------------------------------- */

static void ScanFlash(uint16_t color) {
    sScanFlashColor = color;
    sScanFlash = SCAN_FLASH_FRAMES;
}

static unsigned ScanSeconds(void) {
    return (unsigned) ((svcGetSystemTick() - sScanStartTick) / (u64) SYSCLOCK_ARM11);
}

static int StartScanSource(void) {
    char err[96];
    if (gdxcs_cam_start(sScanZoom, err, sizeof(err)) != 0) {
        SetStatus("CAMERA: %s", err);
        ShareLog("[cshare] scan source start failed: %s", err);
        return -1;
    }
    return 0;
}

/* The SCAN view. Only where an import may write: what it scans is imported. */
static int OpenScan(int autoImport, int autoSlot) {
    if (!gdxcs_game_can_import()) {
        SetStatus("scan only from the main menu or the idle editor (a scanned course is imported)");
        return -1;
    }
    gdxcs_scan_session_reset(&sScan);
    memset(&sPending, 0, sizeof(sPending));
    sScanAutoSlot = autoImport;
    sScanAutoSlotIndex = autoSlot;
    sListView = VIEW_SD;
    if (StartScanSource() != 0) {
        sView = VIEW_SD;
        return -1;
    }
    sView = VIEW_SCAN;
    sScanFlash = 0;
    sScanStartTick = svcGetSystemTick();
    sScanPaintTick = sScanStartTick;
    sPixelsDirty = 1;
    SetStatus("%s", gdxcs_cam_from_files() ? "scanning BMP frames ([debug] cshare_scan_file)"
                                           : "point the camera at a course QR page");
    ShareLog("[cshare] scan open zoom=%dx src=%s auto=%d slot=%d", sScanZoom,
             gdxcs_cam_from_files() ? "files" : "camera", autoImport, autoSlot + 1);
    return 0;
}

static void CloseScan(const char* why) {
    if (sView != VIEW_SCAN) {
        return;
    }
    gdxcs_cam_stop(why);
    sView = VIEW_SD;
    sScanAutoSlot = 0;
    ShareLog("[cshare] scan close why=%s have=%d/%d pages=%d dup=%d foreign=%d not_course=%d secs=%u", why,
             gdxcs_scan_session_have(&sScan), gdxcs_scan_session_count(&sScan), sScan.pages, sScan.duplicates,
             sScan.foreign, sScan.notCourse, ScanSeconds());
}

static void CancelScan(void) {
    CloseScan("cancel");
    ShowList(VIEW_SD);
    SetStatus("scan cancelled");
}

static void AutoImportScanned(void) {
    const int rc = ImportCourse(sPending.name, &sPending.course, sScanAutoSlotIndex, sAutoYes);
    if (rc == 1) {
        ShareLog("[cshare] auto scan import %s: would replace, needs course_share_auto_yes=1", sPending.name);
    }
    ShareLog("[cshare] auto scan import %s slot=%d rc=%d", sPending.name, sScanAutoSlotIndex + 1, rc);
}

/* The set is whole: stop the camera first (its buffers go back to the heap before the import
 * writes anything), then the file goes through exactly the parser an SD file does. */
static void FinishScan(void) {
    const uint8_t* file = gdxcs_scan_session_file(&sScan);
    const int autoImport = sScanAutoSlot;
    GdxCsDiskId disk;
    GdxCsStatus status = GDXCS_ERR_DISK;
    CloseScan("complete");
    memset(&sPending, 0, sizeof(sPending));
    if (file != NULL && CurrentDiskId(&disk) == 0) {
        status = gdxcs_decode_file(file, GDXCS_FILE_BYTES, &disk, &sPending.course, sPending.name);
    }
    ShareLog("[cshare] scan complete set=%04X pages=%d status=%s name=%s pts=%d file_crc=%08lX rec_crc=%08lX",
             gdxcs_scan_session_set_id(&sScan), gdxcs_scan_session_count(&sScan), gdxcs_status_name(status),
             status == GDXCS_OK ? sPending.name : "-", status == GDXCS_OK ? sPending.course.controlPointCount : 0,
             file != NULL ? (unsigned long) gdxcs_crc32(file, GDXCS_FILE_BYTES) : 0ul,
             status == GDXCS_OK ? (unsigned long) CourseRecordCrc(&sPending.course) : 0ul);
    if (status != GDXCS_OK) {
        ShowList(VIEW_SD);
        SetStatus("scanned course refused: %s", gdxcs_status_name(status));
        return;
    }
    snprintf(sPending.leaf, sizeof(sPending.leaf), "%s", "(scanned)");
    sPending.op = OP_IMPORT;
    if (autoImport) {
        AutoImportScanned();
        ShowList(VIEW_SD);
        return;
    }
    sView = VIEW_PICK_SLOT;
    SetStatus("scanned %s (%dpt): pick where it goes", sPending.name, sPending.course.controlPointCount);
}

static void NoteScanEvent(GdxCsScanEvent ev, int page) {
    const int count = gdxcs_scan_session_count(&sScan);
    const int have = gdxcs_scan_session_have(&sScan);
    switch (ev) {
        case GDXCS_SCAN_EV_PAGE:
            ScanFlash(RGB565_GREEN);
            SetStatus("got page %d of %d (%d of %d held)", page + 1, count, have, count);
            break;
        case GDXCS_SCAN_EV_DUPLICATE:
            ScanFlash(RGB565_YELLOW);
            SetStatus("page %d again: already held (%d of %d)", page + 1, have, count);
            break;
        case GDXCS_SCAN_EV_FOREIGN:
            ScanFlash(RGB565_RED);
            SetStatus("a page of ANOTHER course (id %04X): ignored. [RESTART] to scan that one instead",
                      sScan.lastForeignSet);
            break;
        case GDXCS_SCAN_EV_NOT_COURSE:
            ScanFlash(RGB565_RED);
            SetStatus("that QR is not a course page");
            break;
        case GDXCS_SCAN_EV_BAD_SET:
            ScanFlash(RGB565_RED);
            SetStatus("the pages did not fit together: scan them again");
            break;
        default:
            break;
    }
}

static void HandleScanCode(const GdxCsScanCode* code) {
    int page = -1;
    const size_t length = code->length <= GDXCS_SCAN_PAYLOAD_MAX ? code->length : GDXCS_SCAN_PAYLOAD_MAX + 1u;
    const GdxCsScanEvent ev = gdxcs_scan_session_feed(&sScan, code->payload, length, &page);
    if (ev != GDXCS_SCAN_EV_DUPLICATE) {
        ShareLog("[cshare] scan ev=%s page=%d/%d have=%d set=%04X len=%u v=%d mirrored=%d t=%us",
                 gdxcs_scan_event_name(ev), page + 1, gdxcs_scan_session_count(&sScan),
                 gdxcs_scan_session_have(&sScan), gdxcs_scan_session_set_id(&sScan), (unsigned) code->length,
                 code->version, code->mirrored, ScanSeconds());
    }
    if (ev == GDXCS_SCAN_EV_COMPLETE) {
        FinishScan();
        return;
    }
    NoteScanEvent(ev, page);
}

/* HOME / sleep stopped the camera (the APT hook in gdx3ds_course_share_cam.c); coming back
 * restarts it with the pages already held. */
static int ServiceScanInterrupts(void) {
    int repaint = 0;
    if (gdxcs_cam_take_interrupt()) {
        SetStatus("camera off for HOME/sleep: it comes back with the %d page%s held",
                  gdxcs_scan_session_have(&sScan), gdxcs_scan_session_have(&sScan) == 1 ? "" : "s");
        repaint = 1;
    }
    if (gdxcs_cam_take_resume() && !gdxcs_cam_running()) {
        if (StartScanSource() == 0) {
            SetStatus("camera back on (%d of %d pages held)", gdxcs_scan_session_have(&sScan),
                      gdxcs_scan_session_count(&sScan));
        }
        repaint = 1;
    }
    return repaint;
}

static int ServiceScan(void) {
    GdxCsScanCode code;
    int repaint;
    const u64 now = svcGetSystemTick();
    if (sView != VIEW_SCAN) {
        return 0;
    }
    repaint = ServiceScanInterrupts();
    while (sView == VIEW_SCAN && gdxcs_cam_take(&code)) {
        HandleScanCode(&code);
        repaint = 1;
    }
    if (sView == VIEW_SCAN && now - sScanPaintTick >= (u64) SYSCLOCK_ARM11) {
        sScanPaintTick = now; /* the panel's frame/decode counters, once a second */
        repaint = 1;
    }
    return repaint;
}

static void ScanTurnZoom(void) {
    sScanZoom = sScanZoom >= 2 ? 1 : 2;
    gdxcs_cam_stop("zoom");
    if (StartScanSource() == 0) {
        SetStatus("zoom %dx%s", sScanZoom, sScanZoom == 2 ? " (hold the camera ~15 cm from the screen)" : "");
    }
}

static void ScanRestart(void) {
    gdxcs_scan_session_reset(&sScan);
    if (!gdxcs_cam_running()) {
        if (StartScanSource() != 0) {
            return;
        }
    }
    SetStatus("restarted: scan any page");
    ShareLog("[cshare] scan restart");
}

/* ---- auto lever -------------------------------------------------------------------------- */

static char* NextField(char** cursor, char separator) {
    char* start = *cursor;
    char* end;
    if (start == NULL) {
        return NULL;
    }
    end = strchr(start, separator);
    if (end != NULL) {
        *end = '\0';
        *cursor = end + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

static int KindOnDisk(const char* name, uint8_t* kind) {
    const GdxCsDiskEntry* e;
    RefreshDiskList();
    e = FindDiskEntry(name);
    if (e == NULL) {
        return -1;
    }
    *kind = e->kind;
    return 0;
}

static int AutoExport(char* args) {
    uint8_t kind = 0;
    const char* name = NextField(&args, ':');
    int rc;
    if (name == NULL || KindOnDisk(name, &kind) != 0) {
        ShareLog("[cshare] auto export %s: no such disk course", name != NULL ? name : "?");
        return -1;
    }
    rc = ExportCourse(name, kind, sAutoYes);
    if (rc == 1) {
        ShareLog("[cshare] auto export %s: file exists, needs course_share_auto_yes=1", name);
    }
    return rc;
}

static int ParseSlot(const char* text) {
    const int slot = text != NULL ? (int) strtol(text, NULL, 10) : 0;
    return slot >= 1 && slot <= GDXCS_CUP_SLOTS ? slot - 1 : -1;
}

static int AutoImport(char* args) {
    GdxCsCourse course;
    char name[GDXCS_NAME_MAX + 1];
    const char* leaf = NextField(&args, ':');
    const int slot = ParseSlot(NextField(&args, ':'));
    GdxCsStatus status;
    int rc;
    if (leaf == NULL) {
        return -1;
    }
    status = DecodeCourseFile(leaf, &course, name);
    if (status != GDXCS_OK) {
        ShareLog("[cshare] auto import %s refused status=%s", leaf, gdxcs_status_name(status));
        return -1;
    }
    rc = ImportCourse(name, &course, slot, sAutoYes);
    if (rc == 1) {
        ShareLog("[cshare] auto import %s: would replace, needs course_share_auto_yes=1", leaf);
    }
    return rc;
}

static int AutoQr(char* args) {
    uint8_t kind = 0;
    const char* name = NextField(&args, ':');
    const char* page = NextField(&args, ':');
    const char* size = NextField(&args, ':');
    if (size != NULL) {
        sQrLarge = strcmp(size, "dense") != 0;
    }
    if (name == NULL || KindOnDisk(name, &kind) != 0) {
        ShareLog("[cshare] auto qr %s: no such disk course", name != NULL ? name : "?");
        return -1;
    }
    if (OpenQr(name, kind, page != NULL ? (int) strtol(page, NULL, 10) - 1 : 0) != 0) {
        return -1;
    }
    sWantsTab = 1;
    return 0;
}

/* scan:SLOT -- open the SCAN view; when the set is whole, import it into cup SLOT 1..6 (0 =
 * library only). The actions after it wait until the scan is over. */
static int AutoScan(char* args) {
    const int slot = ParseSlot(NextField(&args, ':'));
    if (OpenScan(1, slot) != 0) {
        ShareLog("[cshare] auto scan: could not start");
        sWantsTab = 1; /* the CRS tab shows the reason, as it does for a tap on [SCAN] */
        return -1;
    }
    sWantsTab = 1;
    return 0;
}

/* shot:LABEL -- the bottom screen as it is at the end of this tick, to autotest/LABEL_R.bmp (the
 * replay `shot` mechanism; a QR page captured this way is what the scan lanes feed back). */
static int AutoShot(char* args) {
    if (args == NULL || args[0] == '\0') {
        return -1;
    }
    gdx3ds_menu_note_shot(args);
    return 0;
}

static GdxCsGameState AutoNeeds(const char* verb) {
    if (strcmp(verb, "import") == 0 || strcmp(verb, "delete") == 0 || strcmp(verb, "scan") == 0) {
        return GDXCS_GAME_READ_WRITE;
    }
    if (strcmp(verb, "wait") == 0 || strcmp(verb, "shot") == 0) {
        return GDXCS_GAME_NO_DISK;
    }
    return GDXCS_GAME_READ_ONLY;
}

static int RunAutoAction(char* action) {
    char* args = action;
    const char* verb = NextField(&args, ':');
    int rc = -1;
    if (strcmp(verb, "export") == 0) {
        rc = AutoExport(args);
    } else if (strcmp(verb, "import") == 0) {
        rc = AutoImport(args);
    } else if (strcmp(verb, "delete") == 0) {
        rc = args != NULL ? DeleteCourse(args) : -1;
    } else if (strcmp(verb, "qr") == 0) {
        rc = AutoQr(args);
    } else if (strcmp(verb, "scan") == 0) {
        rc = AutoScan(args);
    } else if (strcmp(verb, "wait") == 0) {
        sAutoWait = args != NULL ? (int) strtol(args, NULL, 10) : 0;
        rc = sAutoWait >= 0 ? 0 : -1;
    } else if (strcmp(verb, "shot") == 0) {
        rc = AutoShot(args);
    } else {
        ShareLog("[cshare] auto: unknown action \"%s\"", verb);
    }
    return rc;
}

/* One action per tick, each once the game allows it. Returns 1 if one ran. */
static int ServiceAuto(void) {
    char action[AUTO_MAX];
    char* cursor;
    char* end;
    size_t length;
    int rc;
    if (sAutoNext == NULL || sView == VIEW_SCAN) {
        return 0; /* a scan in progress holds the list until it is over */
    }
    if (sAutoWait > 0) {
        sAutoWait--;
        return 0;
    }
    end = strchr(sAutoNext, AUTO_SEPARATOR);
    length = end != NULL ? (size_t) (end - sAutoNext) : strlen(sAutoNext);
    memcpy(action, sAutoNext, length);
    action[length] = '\0';
    cursor = strchr(action, ':');
    if (cursor != NULL) {
        *cursor = '\0';
    }
    if (gdxcs_game_state() < AutoNeeds(action)) {
        return 0;
    }
    if (cursor != NULL) {
        *cursor = ':';
    }
    rc = RunAutoAction(action);
    ShareLog("[cshare] auto action=\"%.*s\" rc=%d", (int) length, sAutoNext, rc);
    sAutoNext = end != NULL ? end + 1 : NULL;
    if (sAutoNext == NULL) {
        ShareLog("[cshare] auto done");
    }
    return 1;
}

/* A disk list shown while the game could not be read fills itself in once it can. */
static int RefreshStaleDiskList(void) {
    if (sView != VIEW_DISK || !sDiskListStale || gdxcs_game_state() < GDXCS_GAME_READ_ONLY) {
        return 0;
    }
    RefreshDiskList();
    SetStatus("%d course%s on the disk", sDiskCount, sDiskCount == 1 ? "" : "s");
    return 1;
}

int gdx3ds_cshare_tick(void) {
    if (!sEnabled) {
        return 0;
    }
    if (ServiceScan()) {
        return 1;
    }
    if (ServiceAuto()) {
        return 1;
    }
    return RefreshStaleDiskList();
}

/* ---- painting ---------------------------------------------------------------------------- */

static void PaintStatusRows(GdxCsRowPainter paintRow) {
    char line[44];
    size_t offset = 0;
    int r;
    const size_t total = strlen(sStatus);
    for (r = 0; r < STATUS_ROWS; r++) {
        snprintf(line, sizeof(line), "%.40s", offset < total ? sStatus + offset : "");
        paintRow(ROW_STATUS + r, line);
        offset += 40;
    }
}

static const char* Highlight(int on) {
    return on ? "\x1b[7m" : "";
}

static const char* Plain(int on) {
    return on ? "\x1b[0m" : "";
}

static void PaintViewsRow(GdxCsRowPainter paintRow, int count) {
    char line[96];
    const int pages = count > 0 ? (count + LIST_ROWS - 1) / LIST_ROWS : 1;
    snprintf(line, sizeof(line), "%s[DISK]%s      [RESCAN]     [<] [>] %d/%d", Highlight(sListView == VIEW_DISK),
             Plain(sListView == VIEW_DISK), sPage + 1, pages);
    paintRow(ROW_VIEWS, line);
}

static void FormatDiskRow(char* line, size_t cap, const GdxCsDiskEntry* e, int selected) {
    char slot[8] = "";
    if (e->cupSlot >= 0) {
        snprintf(slot, sizeof(slot), "cup%d", e->cupSlot + 1);
    }
    snprintf(line, cap, "%s %-8s %s %-4s%s%s", Highlight(selected), e->name, KindExt(e->kind), slot,
             e->locked ? " lock" : "", Plain(selected));
}

static void FormatSdRow(char* line, size_t cap, const SdEntry* e, int selected) {
    if (e->status == GDXCS_OK) {
        snprintf(line, cap, "%s %-8s %2dpt %s %.20s%s", Highlight(selected), e->name, e->points, KindExt(e->kind),
                 e->leaf, Plain(selected));
    } else {
        snprintf(line, cap, "%s %.20s BAD:%s%s", Highlight(selected), e->leaf, gdxcs_status_name(e->status),
                 Plain(selected));
    }
}

static int ListCount(void) {
    return sListView == VIEW_DISK ? sDiskCount : sSdCount;
}

static void PaintList(GdxCsRowPainter paintRow) {
    const GdxCsGameState state = gdxcs_game_state();
    char line[96];
    int r;
    snprintf(line, sizeof(line), "COURSE SHARE  disk %s", gdxcs_game_state_name(state));
    paintRow(ROW_TITLE, line);
    PaintViewsRow(paintRow, ListCount());
    for (r = 0; r < LIST_ROWS; r++) {
        const int index = sPage * LIST_ROWS + r;
        line[0] = '\0';
        if (index < ListCount()) {
            if (sListView == VIEW_DISK) {
                FormatDiskRow(line, sizeof(line), &sDisk[index], index == sSelected);
            } else {
                FormatSdRow(line, sizeof(line), &sSd[index], index == sSelected);
            }
        } else if (r == 0) {
            snprintf(line, sizeof(line), " %s", sListView == VIEW_DISK ? "(no courses, or not readable here)"
                                                                        : "(no .gdxcourse files)");
        }
        paintRow(ROW_LIST0 + r, line);
    }
    /* importing is the course editor's bottom menu now (IMPORT A COURSE / SCAN QR PAGES) */
    paintRow(ROW_ACTIONS, sListView == VIEW_DISK ? "[EXPORT] [QR] [DELETE]" : "import: course editor menu");
    PaintStatusRows(paintRow);
}

static void PaintPickSlot(GdxCsRowPainter paintRow) {
    char cup[GDXCS_CUP_SLOTS][GDXCS_NAME_MAX + 1];
    char line[96];
    int s;
    gdxcs_game_cup_names(cup);
    snprintf(line, sizeof(line), "IMPORT %s (%dpt %s) INTO:", sPending.name, sPending.course.controlPointCount,
             KindExt(sPending.course.flag));
    paintRow(ROW_TITLE, line);
    paintRow(ROW_SLOT0, "[LIBRARY ONLY]");
    for (s = 0; s < GDXCS_CUP_SLOTS; s++) {
        if (sPending.course.flag != 0) {
            snprintf(line, sizeof(line), "[CUP SLOT %d] %s", s + 1, cup[s][0] != '\0' ? cup[s] : "(empty)");
        } else {
            snprintf(line, sizeof(line), " cup slot %d: unfinished courses cannot race", s + 1);
        }
        paintRow(ROW_SLOT0 + 1 + s, line);
    }
    paintRow(ROW_ACTIONS, "[CANCEL]");
    PaintStatusRows(paintRow);
}

static void PaintConfirm(GdxCsRowPainter paintRow) {
    char line[96];
    if (sPending.op == OP_EXPORT) {
        snprintf(line, sizeof(line), "REPLACE the SD file for %s?", sPending.name);
        paintRow(ROW_TITLE, line);
    } else if (sPending.op == OP_DELETE) {
        snprintf(line, sizeof(line), "DELETE %s.%s from the disk?", sPending.name, KindExt(sPending.kind));
        paintRow(ROW_TITLE, line);
        paintRow(ROW_SLOT0, " (export it first if you want to keep it)");
    } else {
        char cup[GDXCS_CUP_SLOTS][GDXCS_NAME_MAX + 1];
        gdxcs_game_cup_names(cup);
        paintRow(ROW_TITLE, "IMPORT WOULD REPLACE:");
        if (FindDiskEntry(sPending.name) != NULL) {
            snprintf(line, sizeof(line), " the disk course %s", sPending.name);
            paintRow(ROW_SLOT0, line);
        }
        if (sPending.slot >= 0 && cup[sPending.slot][0] != '\0' && strcmp(cup[sPending.slot], sPending.name) != 0) {
            snprintf(line, sizeof(line), " cup slot %d (%s)", sPending.slot + 1, cup[sPending.slot]);
            paintRow(ROW_SLOT0 + 1, line);
        }
    }
    paintRow(ROW_CONFIRM_YES, "[YES]  [NO]");
    PaintStatusRows(paintRow);
}

static void PaintQrPanel(GdxCsRowPainter paintRow) {
    char line[96];
    snprintf(line, sizeof(line), "QR  %s  (scan every page, any order)", sQrName);
    paintRow(ROW_TITLE, line);
    snprintf(line, sizeof(line), "%*sPAGE %d/%d", QR_PANEL_COL - 1, "", sQrPage + 1, sQrSet.count);
    paintRow(7, line);
    snprintf(line, sizeof(line), "%*s[NEXT]", QR_PANEL_COL - 1, "");
    paintRow(10, line);
    snprintf(line, sizeof(line), "%*s[PREV]", QR_PANEL_COL - 1, "");
    paintRow(13, line);
    snprintf(line, sizeof(line), "%*s[BACK]", QR_PANEL_COL - 1, "");
    paintRow(16, line);
    snprintf(line, sizeof(line), "%*s%dx%d", QR_PANEL_COL - 1, "", sQrModules, sQrModules);
    paintRow(19, line);
    snprintf(line, sizeof(line), "%*s%s", QR_PANEL_COL - 1, "", sQrLarge ? "[DENSE]" : "[LARGE]");
    paintRow(QR_ROW_SIZE, line);
    snprintf(line, sizeof(line), "%*s%s", QR_PANEL_COL - 1, "", sQrLarge ? "big:3DS" : "phone");
    paintRow(QR_ROW_SIZE + 1, line);
}

static void PaintScanPanelRow(GdxCsRowPainter paintRow, int row, const char* text) {
    char line[96];
    snprintf(line, sizeof(line), "%*s%.10s", SCAN_PANEL_COL - 1, "", text);
    paintRow(row, line);
}

/* Pages 1..count, three to a row, the held ones in reverse video. */
static void PaintScanPages(GdxCsRowPainter paintRow, int count) {
    int r;
    for (r = 0; r * 3 < (count > 0 ? count : 1) && r < 4; r++) {
        char line[96];
        size_t used = (size_t) snprintf(line, sizeof(line), "%*s", SCAN_PANEL_COL - 1, "");
        int k;
        for (k = 0; k < 3; k++) {
            const int page = r * 3 + k;
            const int held = gdxcs_scan_session_has_page(&sScan, page);
            if (page >= count) {
                break;
            }
            used += (size_t) snprintf(line + used, sizeof(line) - used, "%s%2d%s ", Highlight(held), page + 1,
                                      Plain(held));
        }
        paintRow(SCAN_ROW_PAGES + 1 + r, count > 0 ? line : "");
    }
}

static void PaintScan(GdxCsRowPainter paintRow) {
    GdxCsCamStats stats;
    char line[96];
    const int count = gdxcs_scan_session_count(&sScan);
    gdxcs_cam_stats(&stats);
    if (count > 0) {
        snprintf(line, sizeof(line), "SCAN  %d of %d pages  (any order)", gdxcs_scan_session_have(&sScan), count);
    } else {
        snprintf(line, sizeof(line), "SCAN  show the camera any page");
    }
    paintRow(ROW_TITLE, line);
    PaintScanPanelRow(paintRow, SCAN_ROW_PAGES, "PAGES");
    PaintScanPages(paintRow, count);
    snprintf(line, sizeof(line), "[ZOOM %dX]", sScanZoom);
    PaintScanPanelRow(paintRow, SCAN_ROW_ZOOM, line);
    PaintScanPanelRow(paintRow, SCAN_ROW_RESTART, "[RESTART]");
    PaintScanPanelRow(paintRow, SCAN_ROW_CANCEL, "[CANCEL]");
    PaintScanPanelRow(paintRow, SCAN_ROW_CANCEL + 1, " or B");
    snprintf(line, sizeof(line), "fr %u", stats.frames);
    PaintScanPanelRow(paintRow, 21, gdxcs_cam_running() ? line : "CAMERA OFF");
    snprintf(line, sizeof(line), "qr %u/%u", stats.codesDecoded, stats.codesFound);
    PaintScanPanelRow(paintRow, 22, line);
    snprintf(line, sizeof(line), "%ums", stats.decodeMsLast);
    PaintScanPanelRow(paintRow, 23, line);
    PaintStatusRows(paintRow);
}

void gdx3ds_cshare_paint_page(GdxCsRowPainter paintRow) {
    if (!sEnabled || paintRow == NULL) {
        return;
    }
    switch (sView) {
        case VIEW_PICK_SLOT:
            PaintPickSlot(paintRow);
            break;
        case VIEW_CONFIRM:
            PaintConfirm(paintRow);
            break;
        case VIEW_QR:
            PaintQrPanel(paintRow);
            sPixelsDirty = 1;
            break;
        case VIEW_SCAN:
            PaintScan(paintRow);
            sPixelsDirty = 1;
            break;
        default:
            PaintList(paintRow);
            break;
    }
}

/* The camera is polled from here, not from the tick: a finished frame is painted before the
 * camera is re-armed onto the same buffer, and this runs after the text paint of the frame. */
static void PaintScanPixels(uint16_t* fb) {
    GdxCsCamView view;
    view.x0 = SCAN_PREVIEW_X0;
    view.y0 = SCAN_PREVIEW_Y0;
    view.w = SCAN_PREVIEW_W;
    view.h = SCAN_PREVIEW_H;
    view.border = sScanFlash > 0 ? sScanFlashColor : RGB565_IDLE;
    if (sScanFlash > 0) {
        sScanFlash--;
    }
    if (!gdxcs_cam_poll(fb, &view) && sPixelsDirty) {
        gdxcs_cam_paint_preview(fb, view.x0, view.y0, view.w, view.h, view.border);
    }
    sPixelsDirty = 0;
}

void gdx3ds_cshare_paint_pixels(uint16_t* fb) {
    if (!sEnabled || fb == NULL) {
        return;
    }
    if (sView == VIEW_SCAN) {
        PaintScanPixels(fb);
        return;
    }
    if (sView != VIEW_QR || !sPixelsDirty || sQrModules == 0) {
        return;
    }
    if (sQrLarge) {
        gdxcs_qr_blit_rgb565(sQrCode, fb, FB_W, FB_H, QR_LARGE_X0, QR_LARGE_Y0, QR_LARGE_SCALE, QR_LARGE_QUIET);
    } else {
        gdxcs_qr_blit_rgb565(sQrCode, fb, FB_W, FB_H, QR_X0, QR_Y0, QR_SCALE, QR_QUIET);
    }
    sPixelsDirty = 0;
}

int gdx3ds_cshare_keys(uint32_t down) {
    if (!sEnabled || sView != VIEW_SCAN) {
        return 0;
    }
    if (down & HID_KEY_B) {
        CancelScan();
        return 1;
    }
    return 0;
}

/* While the SCAN view is up the game sees no buttons (B cancels the scan, and must not also
 * back out of the main menu); after it, each button stays hidden until it is released, so the
 * press that ended the scan never reaches the game as a fresh press. */
void gdx3ds_cshare_frame_end(int crsLive, uint32_t held) {
    if (!sEnabled) {
        return;
    }
    crsLive = crsLive || sUiLive;
    sUiLive = 0;
    if (sView == VIEW_SCAN && !crsLive) {
        CloseScan("hidden");
        SetStatus("scan stopped: the CRS tab was left");
    }
    if (sView == VIEW_SCAN) {
        sGameKeyMask = ~HID_KEY_TOUCH;
    } else {
        sGameKeyMask &= held;
    }
}

uint32_t gdx3ds_cshare_game_key_mask(void) {
    return sGameKeyMask;
}

void gdx3ds_cshare_on_tab_entered(void) {
    if (!sEnabled) {
        return;
    }
    if (sUiOwned) {
        gdx3ds_cshare_ui_close(); /* the console took the screen from the editor's menu */
    }
    if (sView == VIEW_DISK || sView == VIEW_SD) {
        ShowList(sListView);
    }
}

void gdx3ds_cshare_on_tab_left(void) {
    if (sUiOwned) {
        return; /* the editor's menu has the import, not the tab */
    }
    if (sView == VIEW_SCAN) {
        CloseScan("tab-left");
        SetStatus("scan stopped: the CRS tab was left");
    }
    if (sView == VIEW_QR || sView == VIEW_CONFIRM || sView == VIEW_PICK_SLOT) {
        sView = sListView;
    }
    memset(&sPending, 0, sizeof(sPending));
}

/* ---- touch ------------------------------------------------------------------------------- */

static int Hit(int col, int c0, int c1) {
    return col >= c0 && col <= c1;
}

static void ShowList(ShareView view) {
    sListView = view;
    sView = view;
    sSelected = 0;
    sPage = 0;
    if (view == VIEW_DISK) {
        if (RefreshDiskList() != 0) {
            SetStatus("disk not readable now: open this on the main menu");
        }
    } else {
        RefreshSdList();
    }
}

static void TouchViewsRow(int col) {
    const int pages = ListCount() > 0 ? (ListCount() + LIST_ROWS - 1) / LIST_ROWS : 1;
    if (Hit(col, 1, 6)) {
        ShowList(VIEW_DISK);
    } else if (Hit(col, 13, 20)) {
        ShowList(sListView);
    } else if (Hit(col, 26, 28)) {
        sPage = (sPage + pages - 1) % pages;
    } else if (Hit(col, 30, 32)) {
        sPage = (sPage + 1) % pages;
    }
}

static void BeginConfirm(PendingOp op) {
    sPending.op = op;
    sView = VIEW_CONFIRM;
}

static void TouchExport(void) {
    const GdxCsDiskEntry* e;
    int rc;
    if (sSelected >= sDiskCount) {
        return;
    }
    e = &sDisk[sSelected];
    memset(&sPending, 0, sizeof(sPending));
    snprintf(sPending.name, sizeof(sPending.name), "%s", e->name);
    sPending.kind = e->kind;
    rc = ExportCourse(e->name, e->kind, 0);
    if (rc == 1) {
        BeginConfirm(OP_EXPORT);
    }
}

static void TouchImport(void) {
    const SdEntry* e;
    GdxCsStatus status;
    if (sSelected >= sSdCount) {
        return;
    }
    e = &sSd[sSelected];
    memset(&sPending, 0, sizeof(sPending));
    status = DecodeCourseFile(e->leaf, &sPending.course, sPending.name);
    if (status != GDXCS_OK) {
        SetStatus("%s refused: %s", e->leaf, gdxcs_status_name(status));
        ShareLog("[cshare] import %s refused status=%s", e->leaf, gdxcs_status_name(status));
        return;
    }
    if (!gdxcs_game_can_import()) {
        SetStatus("import only from the main menu or the idle editor");
        return;
    }
    snprintf(sPending.leaf, sizeof(sPending.leaf), "%s", e->leaf);
    sPending.op = OP_IMPORT;
    sView = VIEW_PICK_SLOT;
}

static void TouchDelete(void) {
    if (sSelected >= sDiskCount) {
        return;
    }
    if (gdxcs_game_state() != GDXCS_GAME_READ_WRITE) {
        SetStatus("delete only from the main menu");
        return;
    }
    memset(&sPending, 0, sizeof(sPending));
    snprintf(sPending.name, sizeof(sPending.name), "%s", sDisk[sSelected].name);
    sPending.kind = sDisk[sSelected].kind;
    BeginConfirm(OP_DELETE);
}

static void TouchActions(int col) {
    if (sListView == VIEW_DISK && Hit(col, 1, 8)) {
        TouchExport();
    } else if (sListView == VIEW_DISK && Hit(col, 10, 13) && sSelected < sDiskCount) {
        OpenQr(sDisk[sSelected].name, sDisk[sSelected].kind, 0);
    } else if (sListView == VIEW_DISK && Hit(col, 15, 22)) {
        TouchDelete();
    }
}

static void TouchList(int row, int col) {
    if (row == ROW_VIEWS) {
        TouchViewsRow(col);
    } else if (row >= ROW_LIST0 && row < ROW_LIST0 + LIST_ROWS) {
        const int index = sPage * LIST_ROWS + (row - ROW_LIST0);
        if (index < ListCount()) {
            sSelected = index;
        }
    } else if (row == ROW_ACTIONS) {
        TouchActions(col);
    }
}

static void RunImportInto(int slot) {
    int rc;
    sPending.slot = slot;
    rc = ImportCourse(sPending.name, &sPending.course, slot, 0);
    if (rc == 1) {
        BeginConfirm(OP_IMPORT);
        return;
    }
    sView = sListView;
}

static void TouchPickSlot(int row) {
    if (row == ROW_SLOT0) {
        RunImportInto(-1);
    } else if (row > ROW_SLOT0 && row <= ROW_SLOT0 + GDXCS_CUP_SLOTS && sPending.course.flag != 0) {
        RunImportInto(row - ROW_SLOT0 - 1);
    } else if (row == ROW_ACTIONS) {
        sView = sListView;
    }
}

static void TouchConfirm(int row, int col) {
    if (row != ROW_CONFIRM_YES) {
        return;
    }
    if (Hit(col, 1, 5)) {
        if (sPending.op == OP_EXPORT) {
            ExportCourse(sPending.name, sPending.kind, 1);
        } else if (sPending.op == OP_IMPORT) {
            ImportCourse(sPending.name, &sPending.course, sPending.slot, 1);
        } else if (sPending.op == OP_DELETE) {
            if (DeleteCourse(sPending.name) == 0) {
                SetStatus("deleted %s", sPending.name);
            } else {
                SetStatus("delete %s failed", sPending.name);
            }
            sSelected = 0;
        }
        sView = sListView;
    } else if (Hit(col, 8, 11)) {
        SetStatus("cancelled");
        sView = sListView;
    }
}

static void TouchQr(int row, int col) {
    if (col < QR_PANEL_COL) {
        return;
    }
    if (row == 10) {
        QrTurnPage(1);
    } else if (row == 13) {
        QrTurnPage(-1);
    } else if (row == 16) {
        sView = sListView;
    } else if (row == QR_ROW_SIZE) {
        char name[GDXCS_NAME_MAX + 1];
        snprintf(name, sizeof(name), "%s", sQrName);
        sQrLarge = !sQrLarge;
        OpenQr(name, sQrKind, 0);
    }
}

static void TouchScan(int row, int col) {
    if (col < SCAN_PANEL_COL) {
        return;
    }
    if (row == SCAN_ROW_ZOOM) {
        ScanTurnZoom();
    } else if (row == SCAN_ROW_RESTART) {
        ScanRestart();
    } else if (row == SCAN_ROW_CANCEL || row == SCAN_ROW_CANCEL + 1) {
        CancelScan();
    }
}

int gdx3ds_cshare_touch(int row, int col) {
    if (!sEnabled) {
        return 0;
    }
    switch (sView) {
        case VIEW_SCAN:
            TouchScan(row, col);
            break;
        case VIEW_PICK_SLOT:
            TouchPickSlot(row);
            break;
        case VIEW_CONFIRM:
            TouchConfirm(row, col);
            break;
        case VIEW_QR:
            TouchQr(row, col);
            break;
        default:
            TouchList(row, col);
            break;
    }
    return 1;
}

/* ---- mp-menus: the course editor's bottom menu ---------------------------------------------- */

static int UiStep(void) {
    if (!sUiOwned) {
        return GDXCS_UI_IDLE;
    }
    switch (sView) {
        case VIEW_SD:
            return GDXCS_UI_FILES;
        case VIEW_PICK_SLOT:
            return GDXCS_UI_SLOT;
        case VIEW_CONFIRM:
            return sPending.op == OP_IMPORT ? GDXCS_UI_REPLACE : GDXCS_UI_IDLE;
        case VIEW_SCAN:
            return GDXCS_UI_SCAN;
        default:
            return GDXCS_UI_IDLE;
    }
}

static void UiFile(GdxCsUiFile* out, const SdEntry* e) {
    memset(out, 0, sizeof(*out));
    snprintf(out->leaf, sizeof(out->leaf), "%s", e->leaf);
    out->ok = e->status == GDXCS_OK;
    if (out->ok) {
        snprintf(out->name, sizeof(out->name), "%s", e->name);
        out->points = e->points;
        out->finished = e->kind != 0;
    } else {
        snprintf(out->why, sizeof(out->why), "%s", gdxcs_status_name(e->status));
    }
}

/* REPLACE names what goes: the cup slot's course, else the library's course of that name. */
static void UiReplaceWhat(GdxCsUi* out) {
    const int slot = sPending.slot;
    if (slot >= 0 && slot < GDXCS_CUP_SLOTS && out->cup[slot][0] != '\0' && strcmp(out->cup[slot], sPending.name) != 0) {
        snprintf(out->replaceWhat, sizeof(out->replaceWhat), "CUP SLOT %d", slot + 1);
    } else {
        snprintf(out->replaceWhat, sizeof(out->replaceWhat), "LIBRARY");
    }
}

static void UiScan(GdxCsUi* out) {
    int p;
    out->scanHave = (int8_t) gdxcs_scan_session_have(&sScan);
    out->scanCount = (int8_t) gdxcs_scan_session_count(&sScan);
    for (p = 0; p < out->scanCount && p < 16; p++) {
        if (gdxcs_scan_session_has_page(&sScan, p)) {
            out->scanPages |= (uint16_t) (1u << p);
        }
    }
    out->scanZoom = (int8_t) sScanZoom;
    out->cameraOn = (int8_t) gdxcs_cam_running();
}

void gdx3ds_cshare_ui(GdxCsUi* out) {
    char cup[GDXCS_CUP_SLOTS][GDXCS_NAME_MAX + 1];
    int i;
    memset(out, 0, sizeof(*out));
    out->pendingSlot = -1;
    if (!sEnabled) {
        return;
    }
    out->step = (int8_t) UiStep();
    out->canImport = (int8_t) gdxcs_game_can_import();
    out->fileCount = (int8_t) (sSdCount < GDXCS_UI_FILES_MAX ? sSdCount : GDXCS_UI_FILES_MAX);
    for (i = 0; i < out->fileCount; i++) {
        UiFile(&out->file[i], &sSd[i]);
    }
    gdxcs_game_cup_names(cup);
    for (i = 0; i < GDXCS_CUP_SLOTS; i++) {
        snprintf(out->cup[i], sizeof(out->cup[i]), "%s", cup[i]);
    }
    if (sPending.op == OP_IMPORT) {
        snprintf(out->pendingName, sizeof(out->pendingName), "%s", sPending.name);
        out->pendingPoints = (int8_t) sPending.course.controlPointCount;
        out->pendingFinished = sPending.course.flag != 0;
        out->pendingSlot = (int8_t) sPending.slot;
        out->replacesDisk = FindDiskEntry(sPending.name) != NULL;
        UiReplaceWhat(out);
    }
    if (sView == VIEW_SCAN) {
        UiScan(out);
    }
    snprintf(out->status, sizeof(out->status), "%s", sStatus);
}

int gdx3ds_cshare_ui_open_files(void) {
    if (!sEnabled) {
        return 0;
    }
    if (sView == VIEW_SCAN) {
        CloseScan("files");
    }
    sUiOwned = 1;
    memset(&sPending, 0, sizeof(sPending));
    RefreshDiskList();
    ShowList(VIEW_SD);
    ShareLog("[cshare] ui files=%d can_import=%d", sSdCount, gdxcs_game_can_import());
    return 1;
}

int gdx3ds_cshare_ui_pick_file(int index) {
    if (!sEnabled || sView != VIEW_SD || index < 0 || index >= sSdCount) {
        return 0;
    }
    sSelected = index;
    TouchImport();
    if (sView == VIEW_PICK_SLOT) {
        sPending.slot = -1;
        RefreshDiskList();
        ShareLog("[cshare] ui pick %s -> %s (%dpt)", sSd[index].leaf, sPending.name, sPending.course.controlPointCount);
    }
    return 1;
}

int gdx3ds_cshare_ui_import_into(int slot) {
    if (!sEnabled || sView != VIEW_PICK_SLOT || slot < -1 || slot >= GDXCS_CUP_SLOTS) {
        return 0;
    }
    if (slot >= 0 && sPending.course.flag == 0) {
        SetStatus("an unfinished course cannot race in the edit cup");
        return 1;
    }
    if (!gdxcs_game_can_import()) {
        SetStatus("wait: the editor is busy");
        return 1;
    }
    RunImportInto(slot);
    ShareLog("[cshare] ui import %s slot=%d -> %s", sPending.name, slot + 1,
             sView == VIEW_CONFIRM ? "confirm" : "done");
    return 1;
}

int gdx3ds_cshare_ui_confirm(int yes) {
    if (!sEnabled || sView != VIEW_CONFIRM || sPending.op != OP_IMPORT) {
        return 0;
    }
    if (!yes) {
        sView = VIEW_PICK_SLOT;
        SetStatus("kept what was there");
        return 1;
    }
    ImportCourse(sPending.name, &sPending.course, sPending.slot, 1);
    sView = sListView;
    RefreshSdList();
    return 1;
}

int gdx3ds_cshare_ui_back(void) {
    if (!sEnabled) {
        return 0;
    }
    if (sView == VIEW_PICK_SLOT || sView == VIEW_CONFIRM) {
        sView = VIEW_SD;
        sListView = VIEW_SD;
        memset(&sPending, 0, sizeof(sPending));
        return 1;
    }
    gdx3ds_cshare_ui_close();
    return 1;
}

int gdx3ds_cshare_ui_open_scan(void) {
    if (!sEnabled) {
        return 0;
    }
    sUiOwned = 1;
    if (OpenScan(0, -1) != 0) {
        sView = VIEW_DISK;
        sListView = VIEW_DISK;
        sUiOwned = 0;
    }
    return 1;
}

int gdx3ds_cshare_ui_scan_zoom(void) {
    if (!sEnabled || sView != VIEW_SCAN) {
        return 0;
    }
    ScanTurnZoom();
    return 1;
}

int gdx3ds_cshare_ui_scan_restart(void) {
    if (!sEnabled || sView != VIEW_SCAN) {
        return 0;
    }
    ScanRestart();
    return 1;
}

int gdx3ds_cshare_ui_scan_cancel(void) {
    if (!sEnabled || sView != VIEW_SCAN) {
        return 0;
    }
    gdx3ds_cshare_ui_close();
    SetStatus("scan cancelled");
    return 1;
}

void gdx3ds_cshare_ui_close(void) {
    if (!sEnabled) {
        return;
    }
    if (sView == VIEW_SCAN) {
        CloseScan("menu-closed");
    }
    sView = VIEW_DISK;
    sListView = VIEW_DISK;
    memset(&sPending, 0, sizeof(sPending));
    sUiOwned = 0;
}

int gdx3ds_cshare_ui_tick(void) {
    if (!sEnabled || !sUiOwned) {
        return 0;
    }
    sUiLive = 1;
    return ServiceScan();
}

void gdx3ds_cshare_ui_scan_paint(uint16_t* fb, int x0, int y0, int w, int h, int force) {
    GdxCsCamView view;
    if (!sEnabled || fb == NULL || sView != VIEW_SCAN) {
        return;
    }
    view.x0 = x0;
    view.y0 = y0;
    view.w = w;
    view.h = h;
    view.border = sScanFlash > 0 ? sScanFlashColor : RGB565_IDLE;
    if (sScanFlash > 0) {
        sScanFlash--;
    }
    if (!gdxcs_cam_poll(fb, &view) && force) {
        gdxcs_cam_paint_preview(fb, view.x0, view.y0, view.w, view.h, view.border);
    }
}
