/* port/3ds/gdx3ds_course_share_game.h -- the game-side half of `[ek] course_share`: the 64DD
 * MFS operations on the Expansion Kit's custom courses, implemented in
 * port/3ds/game/gdx3ds_course_share_game.c (the only TU here that sees decomp types).
 * Expansion Kit builds only.
 *
 * THREADING. Every function runs on the main thread from the bottom-screen menu tick, between
 * frames, while every game fiber is parked. The 64DD stack on the port is synchronous (LEO
 * commands complete inside the call), so a direct Mfs_* call is a complete operation. It is
 * still only made when gdxcs_game_state() says the game's own MFS worker is idle and the game
 * is not between modes; see that function. */
#ifndef GDX3DS_COURSE_SHARE_GAME_H
#define GDX3DS_COURSE_SHARE_GAME_H

#include <stdint.h>

#include "gdx3ds_course_share_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDXCS_CUP_SLOTS 6
#define GDXCS_LIBRARY_MAX 100 /* the editor's file list, D_xk1_8003A5D8[100] (course_edit) */

typedef enum GdxCsGameState {
    GDXCS_GAME_NO_DISK = 0, /* no 64DD image mounted, or its RAM area is not loaded */
    GDXCS_GAME_BUSY,        /* MFS worker active, a mode change in flight, or a race/test run */
    GDXCS_GAME_READ_ONLY,   /* listing and export are safe; import only when the editor is idle */
    GDXCS_GAME_READ_WRITE   /* main menu, everything idle: import is safe too */
} GdxCsGameState;

typedef struct GdxCsDiskEntry {
    char name[GDXCS_NAME_MAX + 1];
    uint8_t kind;    /* 1 = CRSD (finished), 0 = CRSE */
    uint8_t locked;  /* MFS_FILE_ATTR_FORBID_W: the game will not overwrite it either */
    int8_t cupSlot;  /* 0..5 if the edit cup names it, else -1 */
    int32_t fileSize;
} GdxCsDiskEntry;

GdxCsGameState gdxcs_game_state(void);
/* mp-menus: 1 when an import may write now: the main menu (READ_WRITE), or the course editor
 * idle (no dialog, save or load of its own; not a test run). Delete stays main-menu only. */
int gdxcs_game_can_import(void);

/* The mounted disk's identity (gdxcs_disk_id_from_boot over the port's leoBootID, the disk ID
 * block it reads at mount). 0 on success, -1 when no disk ID was read. */
int gdxcs_game_disk_id(GdxCsDiskId* out);
const char* gdxcs_game_state_name(GdxCsGameState state);

/* Fills up to `cap` entries (the working directory's CRSD/CRSE files, directory order) and
 * returns how many there are in total (which may exceed cap). */
int gdxcs_game_list(GdxCsDiskEntry* out, int cap);

/* The six edit-cup names (gEditCupTrackNames[0..5]); "" for an empty slot. */
void gdxcs_game_cup_names(char out[GDXCS_CUP_SLOTS][GDXCS_NAME_MAX + 1]);

/* Bytes free in the disk's RAM area, and the size one course file needs. */
int32_t gdxcs_game_free_bytes(void);
int32_t gdxcs_game_course_file_bytes(void);

/* Reads NAME.CRSD/CRSE into `out`. 0 on success; otherwise a negative code and, in *mfsError
 * (may be NULL), the game's gMfsError. The record is returned exactly as stored: in the port's
 * native byte order, or -- a file written by a real N64 -- big-endian, which the reader
 * recognises by validation and converts. -2: the file's size is not one CourseContext. -3: the
 * record validates neither way (*mfsError = the GdxCsStatus of the native attempt). */
int gdxcs_game_read(const char* name, uint8_t kind, GdxCsCourse* out, int* mfsError);

/* Writes `course` as NAME.<CRSD|CRSE> exactly as the editor's own save does (COURSE_CONTEXT
 * layout; ghosts and records re-initialised; the other extension of the same name deleted),
 * then, when cupSlot is 0..5, names it in that edit-cup slot and saves CRS_ENTRY.CENT. The
 * caller has validated the course (gdxcs_decode_file). Same return convention as read; -4: the
 * library already holds GDXCS_LIBRARY_MAX courses, -5: not enough free space, -6: the existing
 * file is write-protected, -7: cupSlot given for an unfinished course (the cup only races
 * CRSD), -8: the state gate refused (gdxcs_game_can_import). */
int gdxcs_game_write(const char* name, const GdxCsCourse* course, int cupSlot, int* mfsError);

/* Deletes NAME.CRSD and NAME.CRSE, clears any cup slot naming it and saves CENT. Used by the
 * round-trip test lever only. Same return convention. */
int gdxcs_game_delete(const char* name, int* mfsError);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_COURSE_SHARE_GAME_H */
