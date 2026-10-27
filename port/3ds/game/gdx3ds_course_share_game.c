/* port/3ds/game/gdx3ds_course_share_game.c -- `[ek] course_share`: the game-side MFS operations
 * on the Expansion Kit's custom courses. Contract: ../gdx3ds_course_share_game.h. Expansion Kit
 * builds only (GDX3DS_EK_SOURCES), so the shipped game links nothing from here.
 *
 * Every operation is the decomp's own MFS call (leo/mfs), made directly rather than through the
 * game's worker thread: the worker exists to keep a real drive's seconds of I/O off the game
 * thread, and on the port the LEO stack completes inline anyway. What the worker also gives the
 * game -- one operation at a time -- the state gate gives here: an op runs only when the worker
 * is provably idle and no mode change is in flight (gdxcs_game_state). The two globals a direct
 * call leaves behind (gMfsError, D_80794CDC) are saved and restored around it, so the game's
 * next look at them sees what it left there. */
#include "global.h"
#include "fzx_course.h"
#include "fzx_game.h"
#include "leo/mfs.h"

#include <stdint.h>

/* global.h declares its own memset (an unsigned long long size), which <string.h> would contradict
 * on a 32-bit target, so the few libc calls this TU needs go through the compiler builtins and two
 * plain prototypes. */
#define memcpy __builtin_memcpy
#define memcmp __builtin_memcmp
#define memchr __builtin_memchr
#define strlen __builtin_strlen
#define strncmp __builtin_strncmp
extern void* memalign(__SIZE_TYPE__ alignment, __SIZE_TYPE__ size);
extern void free(void* ptr);

#include "../gdx3ds_course_share_game.h"

_Static_assert(sizeof(CourseData) == sizeof(GdxCsCourse), "GdxCsCourse mirrors CourseData");
_Static_assert(__builtin_offsetof(CourseData, controlPoint) == __builtin_offsetof(GdxCsCourse, controlPoint), "controlPoint");
_Static_assert(__builtin_offsetof(CourseData, bankAngle) == __builtin_offsetof(GdxCsCourse, bankAngle), "bankAngle");
_Static_assert(__builtin_offsetof(CourseData, sign) == __builtin_offsetof(GdxCsCourse, sign), "sign");
_Static_assert(sizeof(ControlPoint) == sizeof(GdxCsControlPoint), "ControlPoint");

extern s32 gLeoDriveConnectionState;         /* sys/sys_main.c */
extern s32 gGameMode;                        /* game/game.c */
extern s32 gQueuedGameMode;                  /* game/game.c */
extern s16 gGameModeChangeState;             /* game/game.c */
extern bool gInCourseEditTestRun;            /* game/course_gadgets.c */
extern unk_800D6CA0 D_800D6CA0;              /* ovl_i2: .unk_08 = the course editor's dialog / save step, 0 = editing */
extern u8 D_80794E14;                        /* sys/disk/75000.c: worker has a ring entry */
extern u8 D_80794E18;                        /* sys/disk/75000.c: worker is in an MFS op */
extern OSMesgQueue D_807C6E90;               /* sys/disk/75000.c: the worker's request queue */
extern volatile unk_807C6EA8 D_807C6EA8;     /* sys/disk/75000.c: the request in flight */
extern bool D_8076CBB0;                      /* sys/disk/sys_leo_dd.c: "RAM area needs a reload" */
extern char gEditCupTrackNames[6 * 4][9];    /* ovl_i2/course_context.c (EK size) */

#define CS_MFS_DIR MFS_ENTRY_WORKING_DIR
#define CS_LOAD_ALIGN 16
static const char kCupFileName[] = "CRS_ENTRY";
static const char kCupFileExt[] = "CENT";

/* ---- state gate ------------------------------------------------------------------------ */

static int WorkerIsIdle(void) {
    return D_80794E14 == 0 && D_80794E18 == 0 && D_807C6E90.validCount == 0 && D_807C6EA8.unk_08 == 0;
}

static int RamAreaIsLoaded(void) {
    return !D_8076CBB0 && memcmp(gMfsRamArea.id.diskId, "64dd-Multi", 10) == 0;
}

static int ModeIsSettled(void) {
    return gGameModeChangeState == GAMEMODE_UPDATE && gQueuedGameMode == gGameMode;
}

GdxCsGameState gdxcs_game_state(void) {
    const s32 mode = GET_MODE(gGameMode);
    if (gLeoDriveConnectionState == 0 || !RamAreaIsLoaded()) {
        return GDXCS_GAME_NO_DISK;
    }
    if (!WorkerIsIdle() || !ModeIsSettled()) {
        return GDXCS_GAME_BUSY;
    }
    if (mode == GAMEMODE_MAIN_MENU) {
        return GDXCS_GAME_READ_WRITE;
    }
    if (mode == GAMEMODE_COURSE_EDIT && !gInCourseEditTestRun) {
        return GDXCS_GAME_READ_ONLY;
    }
    return GDXCS_GAME_BUSY;
}

/* The course editor with no dialog, save or load of its own under way: an import may write the
 * library and the edit cup then (the editor re-reads its file list whenever it opens one). */
static int EditorIsIdle(void) {
    return GET_MODE(gGameMode) == GAMEMODE_COURSE_EDIT && !gInCourseEditTestRun && D_800D6CA0.unk_08 == 0;
}

int gdxcs_game_can_import(void) {
    const GdxCsGameState state = gdxcs_game_state();
    if (state == GDXCS_GAME_READ_WRITE) {
        return 1;
    }
    return state == GDXCS_GAME_READ_ONLY && EditorIsIdle();
}

int gdxcs_game_disk_id(GdxCsDiskId* out) {
    if (out == NULL || leoBootID.gameName[0] == 0) {
        return -1;
    }
    *out = gdxcs_disk_id_from_boot(leoBootID.gameName, leoBootID.gameVersion, leoBootID.diskNumber,
                                   leoBootID.company);
    return 0;
}

const char* gdxcs_game_state_name(GdxCsGameState state) {
    switch (state) {
        case GDXCS_GAME_NO_DISK:
            return "no-disk";
        case GDXCS_GAME_BUSY:
            return "busy";
        case GDXCS_GAME_READ_ONLY:
            return "read-only";
        case GDXCS_GAME_READ_WRITE:
            return "read-write";
        default:
            return "?";
    }
}

/* ---- MFS globals a direct call disturbs ------------------------------------------------ */

typedef struct MfsSaved {
    s32 mfsError;
    s32 opKind;
} MfsSaved;

static MfsSaved MfsSave(void) {
    MfsSaved saved;
    saved.mfsError = gMfsError;
    saved.opKind = D_80794CDC;
    return saved;
}

static int MfsRestore(MfsSaved saved, int* mfsError) {
    if (mfsError != NULL) {
        *mfsError = gMfsError;
    }
    gMfsError = saved.mfsError;
    D_80794CDC = saved.opKind;
    return 0;
}

/* ---- listing ----------------------------------------------------------------------------- */

static int ExtensionKind(const char* ext) {
    if (strncmp(ext, "CRSD", 5) == 0) {
        return 1;
    }
    if (strncmp(ext, "CRSE", 5) == 0) {
        return 0;
    }
    return -1;
}

static int CupSlotOf(const char* name) {
    int slot;
    for (slot = 0; slot < GDXCS_CUP_SLOTS; slot++) {
        if (gEditCupTrackNames[slot][0] != '\0' && strncmp(gEditCupTrackNames[slot], name, 9) == 0) {
            return slot;
        }
    }
    return -1;
}

static const MfsRamDirectoryEntry* CourseEntryAt(s32 index, int* kindOut) {
    const MfsRamDirectoryEntry* entry = &gMfsRamArea.directoryEntry[index];
    int kind;
    if (!(entry->attr & MFS_FILE_ATTR_FILE) || (entry->attr & MFS_FILE_ATTR_DIRECTORY)) {
        return NULL;
    }
    if (entry->parentDirId != gWorkingDirectory) {
        return NULL;
    }
    kind = ExtensionKind(entry->extension);
    if (kind < 0) {
        return NULL;
    }
    if (memchr(entry->name, '\0', GDXCS_NAME_MAX + 1) == NULL) {
        return NULL; /* longer than the editor's 8 characters: not one of its courses */
    }
    *kindOut = kind;
    return entry;
}

int gdxcs_game_list(GdxCsDiskEntry* out, int cap) {
    s32 count = gDirectoryEntryCount;
    int total = 0;
    s32 i;
    if (count > (s32) ARRAY_COUNT(gMfsRamArea.directoryEntry)) {
        count = ARRAY_COUNT(gMfsRamArea.directoryEntry);
    }
    for (i = 0; i < count; i++) {
        int kind;
        const MfsRamDirectoryEntry* entry = CourseEntryAt(i, &kind);
        if (entry == NULL) {
            continue;
        }
        if (out != NULL && total < cap) {
            GdxCsDiskEntry* e = &out[total];
            memset(e, 0, sizeof(*e));
            memcpy(e->name, entry->name, GDXCS_NAME_MAX);
            e->name[GDXCS_NAME_MAX] = '\0';
            e->kind = (uint8_t) kind;
            e->locked = (entry->attr & MFS_FILE_ATTR_FORBID_W) ? 1 : 0;
            e->cupSlot = (int8_t) CupSlotOf(e->name);
            e->fileSize = entry->fileSize;
        }
        total++;
    }
    return total;
}

void gdxcs_game_cup_names(char out[GDXCS_CUP_SLOTS][GDXCS_NAME_MAX + 1]) {
    int slot;
    for (slot = 0; slot < GDXCS_CUP_SLOTS; slot++) {
        memcpy(out[slot], gEditCupTrackNames[slot], GDXCS_NAME_MAX);
        out[slot][GDXCS_NAME_MAX] = '\0';
    }
}

int32_t gdxcs_game_free_bytes(void) {
    const MfsSaved saved = MfsSave();
    const s32 freeBytes = Mfs_RamGetFreeSize();
    MfsRestore(saved, NULL);
    return freeBytes;
}

int32_t gdxcs_game_course_file_bytes(void) {
    return (int32_t) sizeof(CourseContext);
}

/* ---- read ----------------------------------------------------------------------------- */

static const char* ExtensionFor(uint8_t kind) {
    return gdxcs_extension_for_flag(kind);
}

static int StoredSizeIs(const char* name, const char* ext, s32 want) {
    const u16 index = Mfs_GetFileIndex(CS_MFS_DIR, (char*) name, (char*) ext);
    if (index == MFS_ENTRY_DOES_NOT_EXIST) {
        return 0;
    }
    return gMfsRamArea.directoryEntry[index].fileSize == want;
}

/* A port-written record is native; one a real N64 wrote is big-endian. The validator decides. */
static int AdoptStoredRecord(const CourseData* stored, GdxCsCourse* out, int* reason) {
    GdxCsCourse native;
    GdxCsCourse swapped;
    GdxCsStatus status;
    memcpy(&native, stored, sizeof(native));
    status = gdxcs_validate_course(&native, NULL);
    if (status == GDXCS_OK) {
        *out = native;
        return 0;
    }
    gdxcs_course_from_be((const uint8_t*) stored, &swapped);
    if (gdxcs_validate_course(&swapped, NULL) == GDXCS_OK) {
        *out = swapped;
        return 0;
    }
    *reason = (int) status;
    return -3;
}

int gdxcs_game_read(const char* name, uint8_t kind, GdxCsCourse* out, int* mfsError) {
    const char* ext = ExtensionFor(kind);
    const MfsSaved saved = MfsSave();
    CourseContext* ctx;
    s32 rc;
    int result;
    if (gdxcs_validate_name(name) != GDXCS_OK || out == NULL) {
        return -1;
    }
    if (gdxcs_game_state() < GDXCS_GAME_READ_ONLY) {
        return -8;
    }
    if (!StoredSizeIs(name, ext, (s32) sizeof(CourseContext))) {
        if (mfsError != NULL) {
            *mfsError = Mfs_GetFileIndex(CS_MFS_DIR, (char*) name, (char*) ext) == MFS_ENTRY_DOES_NOT_EXIST
                            ? N64DD_NOT_FOUND
                            : 0;
        }
        MfsRestore(saved, NULL);
        return -2;
    }
    ctx = (CourseContext*) memalign(CS_LOAD_ALIGN, sizeof(CourseContext));
    if (ctx == NULL) {
        MfsRestore(saved, NULL);
        return -9;
    }
    rc = Mfs_LoadFileInDir(CS_MFS_DIR, (char*) name, (char*) ext, (u8*) ctx, sizeof(CourseContext));
    if (rc < 0) {
        result = -1;
        MfsRestore(saved, mfsError);
    } else {
        int reason = 0;
        MfsRestore(saved, NULL);
        result = AdoptStoredRecord(&ctx->courseData, out, &reason);
        if (result != 0 && mfsError != NULL) {
            *mfsError = reason;
        }
    }
    free(ctx);
    return result;
}

/* ---- write ---------------------------------------------------------------------------- */

/* The editor's own save (course_edit/19C470.c func_xk2_800EAA1C): the three ghost records
 * marked empty with the slowest time, the course record re-initialised. */
static void BuildCourseContext(CourseContext* ctx, const GdxCsCourse* course) {
    s32 i;
    memset(ctx, 0, sizeof(*ctx));
    memcpy(&ctx->courseData, course, sizeof(ctx->courseData));
    for (i = 0; i < 3; i++) {
        ctx->ghostSave[i].record.encodedCourseIndex = 0;
        ctx->ghostSave[i].record.raceTime = MAX_TIMER;
    }
    Save_InitCourseRecord(&ctx->saveCourseRecord, true);
}

static int LibraryHasRoomFor(const char* name) {
    if (Mfs_GetFileIndex(CS_MFS_DIR, (char*) name, "CRSD") != MFS_ENTRY_DOES_NOT_EXIST ||
        Mfs_GetFileIndex(CS_MFS_DIR, (char*) name, "CRSE") != MFS_ENTRY_DOES_NOT_EXIST) {
        return 1; /* replacing: the count does not grow */
    }
    return gdxcs_game_list(NULL, 0) < GDXCS_LIBRARY_MAX;
}

static int ExistingIsLocked(const char* name) {
    static const char* const kExts[2] = { "CRSE", "CRSD" };
    int k;
    for (k = 0; k < 2; k++) {
        const u16 index = Mfs_GetFileIndex(CS_MFS_DIR, (char*) name, (char*) kExts[k]);
        if (index != MFS_ENTRY_DOES_NOT_EXIST && (gMfsRamArea.directoryEntry[index].attr & MFS_FILE_ATTR_FORBID_W)) {
            return 1;
        }
    }
    return 0;
}

static s32 BytesHeldBy(const char* name, const char* ext) {
    const u16 index = Mfs_GetFileIndex(CS_MFS_DIR, (char*) name, (char*) ext);
    if (index == MFS_ENTRY_DOES_NOT_EXIST) {
        return 0;
    }
    return gMfsRamArea.directoryEntry[index].fileSize;
}

static int SpaceIsAvailable(const char* name) {
    const s32 reclaim = BytesHeldBy(name, "CRSD") + BytesHeldBy(name, "CRSE");
    return Mfs_RamGetFreeSize() + reclaim >= (s32) sizeof(CourseContext);
}

static s32 SaveCupNames(void) {
    return Mfs_SaveFile(CS_MFS_DIR, (char*) kCupFileName, (char*) kCupFileExt, (u8*) gEditCupTrackNames,
                        sizeof(gEditCupTrackNames), 0, 0xFF, true);
}

static void ClearCupSlotsNaming(const char* name) {
    int slot;
    for (slot = 0; slot < GDXCS_CUP_SLOTS; slot++) {
        if (strncmp(gEditCupTrackNames[slot], name, 9) == 0) {
            gEditCupTrackNames[slot][0] = '\0';
        }
    }
}

/* The editor deletes the other extension first (func_xk2_800EB938), so one name is one file. */
static s32 WriteCourseFile(const char* name, const CourseContext* ctx) {
    const char* ext = ExtensionFor(ctx->courseData.flag);
    const char* other = ExtensionFor(ctx->courseData.flag ? 0 : 1);
    if (Mfs_GetFileIndex(CS_MFS_DIR, (char*) name, (char*) other) != MFS_ENTRY_DOES_NOT_EXIST) {
        if (Mfs_DeleteFileInDir(CS_MFS_DIR, (char*) name, (char*) other, true) < 0) {
            return -1;
        }
    }
    return Mfs_SaveFile(CS_MFS_DIR, (char*) name, (char*) ext, (u8*) ctx, sizeof(CourseContext), 0, 0xFF, true);
}

static int CheckWritePreconditions(const char* name, const GdxCsCourse* course, int cupSlot) {
    if (gdxcs_validate_name(name) != GDXCS_OK || course == NULL || Mfs_ValidateFileName((char*) name) != 0) {
        return -1;
    }
    if (cupSlot >= GDXCS_CUP_SLOTS || (cupSlot >= 0 && course->flag == 0)) {
        return -7;
    }
    if (!gdxcs_game_can_import()) {
        return -8;
    }
    if (!LibraryHasRoomFor(name)) {
        return -4;
    }
    if (ExistingIsLocked(name)) {
        return -6;
    }
    if (!SpaceIsAvailable(name)) {
        return -5;
    }
    return 0;
}

static int UpdateCupSlot(const char* name, int cupSlot) {
    if (cupSlot < 0) {
        return 0;
    }
    ClearCupSlotsNaming(name);
    memset(gEditCupTrackNames[cupSlot], 0, sizeof(gEditCupTrackNames[cupSlot]));
    memcpy(gEditCupTrackNames[cupSlot], name, strlen(name));
    return SaveCupNames() < 0 ? -1 : 0;
}

int gdxcs_game_write(const char* name, const GdxCsCourse* course, int cupSlot, int* mfsError) {
    MfsSaved saved;
    CourseContext* ctx;
    int result;
    if (mfsError != NULL) {
        *mfsError = 0;
    }
    saved = MfsSave();
    result = CheckWritePreconditions(name, course, cupSlot);
    if (result != 0) {
        MfsRestore(saved, NULL);
        return result;
    }
    ctx = (CourseContext*) memalign(CS_LOAD_ALIGN, sizeof(CourseContext));
    if (ctx == NULL) {
        MfsRestore(saved, NULL);
        return -9;
    }
    BuildCourseContext(ctx, course);
    result = WriteCourseFile(name, ctx) < 0 ? -1 : 0;
    if (result == 0) {
        result = UpdateCupSlot(name, cupSlot);
    }
    free(ctx);
    MfsRestore(saved, result != 0 ? mfsError : NULL);
    return result;
}

int gdxcs_game_delete(const char* name, int* mfsError) {
    static const char* const kExts[2] = { "CRSE", "CRSD" };
    MfsSaved saved;
    int result = 0;
    int k;
    if (gdxcs_validate_name(name) != GDXCS_OK) {
        return -1;
    }
    if (gdxcs_game_state() != GDXCS_GAME_READ_WRITE) {
        return -8;
    }
    saved = MfsSave();
    if (ExistingIsLocked(name)) {
        MfsRestore(saved, NULL);
        return -6;
    }
    for (k = 0; k < 2 && result == 0; k++) {
        if (Mfs_GetFileIndex(CS_MFS_DIR, (char*) name, (char*) kExts[k]) != MFS_ENTRY_DOES_NOT_EXIST &&
            Mfs_DeleteFileInDir(CS_MFS_DIR, (char*) name, (char*) kExts[k], true) < 0) {
            result = -1;
        }
    }
    if (result == 0 && CupSlotOf(name) >= 0) {
        ClearCupSlotsNaming(name);
        result = SaveCupNames() < 0 ? -1 : 0;
    }
    MfsRestore(saved, result != 0 ? mfsError : NULL);
    return result;
}
