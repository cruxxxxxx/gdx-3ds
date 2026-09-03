#ifndef GDIFFUSER_GDX_GHOST_IO_H
#define GDIFFUSER_GDX_GHOST_IO_H

#include <stddef.h>
#include <stdint.h>

/* port/gdx_ghost_io.h -- .gdg ghost replay import/export (G-Diffuser Practice tab).
 *
 * Provides the portable GDG1 container plus G-Diffuser's host-side multi-ghost library. The
 * vanilla single SRAM slot remains intact for compatibility, but it is no longer the only durable
 * location: every distinct validated player replay can coexist under `ghosts/` and up to three
 * entries per exact encoded course can be selected as Time Attack opponents.
 *
 * SCOPE: base-course player ghosts (courses 0..23). This deliberately does NOT touch the
 * 64DD/Expansion-Kit per-course ghost cache (COURSE_CONTEXT()->ghostSave[i], DDSave_* in
 * decomp/src/overlays/ovl_i2/dd_save.c). That path is LIVE on the default build --
 * GDX_EXPANSION_KIT compiles dd_save.c in, the drive is emulated (port/n64_leo.c over the
 * disk image in port/disk_buffer.cpp), and its writes are made durable by the .gdd journal
 * sidecar (port/disk_savefile.cpp) -- so staying out of it avoids a second owner for the
 * same records.
 *
 * .gdg FILE FORMAT (v1)
 * ----------------------
 * All integers in the 20-byte header are little-endian, hand-packed (2/4-byte fields
 * written LSB-first), independent of host struct layout. This container format is new,
 * so its byte order is defined explicitly rather than left to the compiler:
 *
 *   offset  size  field
 *   0       4     magic       ASCII "GDG1" (no NUL terminator)
 *   4       4     version     u32 LE, must be 1
 *   8       4     courseId    s32 LE, redundant copy of (encodedCourseIndex & 0x1F) --
 *                             lets a browser/importer sanity-check a file without fully
 *                             parsing the payload; the importer also cross-checks this
 *                             against the payload's own encodedCourseIndex and rejects
 *                             the file if they disagree.
 *   12      4     payloadSize u32 LE, must equal sizeof(GhostRecord) + sizeof(GhostData)
 *                             = 0x40 + 0x3F80 = 0x3FC0 for v1.
 *   16      4     crc32       u32 LE, IEEE 802.3 CRC-32 of the payload bytes (offset 20
 *                             onward), catching file-transfer corruption/truncation
 *                             independent of the game's own record/data checksums below.
 *   20      0x3FC0 payload    GhostRecord (0x40 bytes) immediately followed by GhostData
 *                             (0x3F80 bytes) -- see decomp/include/fzx_save.h:74-100 --
 *                             copied VERBATIM: the exact bytes Save_ReadGhostRecord /
 *                             Save_ReadGhostData produce, in this port's native in-memory
 *                             struct layout.
 *
 * Total v1 file size: 20 + 0x3FC0 = 0x3FD4 (16340) bytes, always (fixed-size payload;
 * there is no variable-length data in a base-course ghost).
 *
 * ENDIANNESS of the payload: the port persists gSaveContext (and therefore the ghost SRAM
 * slot) to fzerox.sav as a raw, un-byteswapped memcpy of the in-memory struct -- see
 * port/sram_buffer.cpp's gdx_sram_read/gdx_sram_write, which never touch the byte order of
 * what they copy. Only little-endian hosts are targeted, so "native byte order" and
 * "little-endian" are the same thing here. The .gdg payload matches fzerox.sav's convention
 * verbatim rather than re-encoding every u16/s32 by hand, which would be a second,
 * independent place for the byte order to drift out of sync with the SRAM path it mirrors.
 * A big-endian host target would require an explicit per-field re-encode at that point.
 *
 * VALIDATION on import (gdx_ghost_import), in order -- any failure leaves the current
 * SRAM ghost slot and fzerox.sav completely untouched:
 *   1. magic == "GDG1", version == 1, payloadSize == 0x3FC0.
 *   2. crc32 matches the payload bytes actually read (container integrity).
 *   3. GhostRecord.checksum matches Save_CalculateGhostRecordChecksum(record) and
 *      GhostData.replayInfo.checksum matches Save_CalculateGhostDataChecksum(data) --
 *      THE GAME'S OWN checksum routines (decomp/src/overlays/ovl_i2/save.c:1935-1941),
 *      called directly, not reimplemented, so the game's own checksum validation IS the
 *      import sanity check.
 *   4. The derived course (encodedCourseIndex & 0x1F, mirroring save.c's
 *      func_i2_80101590) is in range [0, 24) and matches the header's courseId.
 *   5. ghostType is one of the defined GhostType values (fzx_save.h:7-13), excluding
 *      GHOST_NONE (0), which marks an empty slot, not a real ghost.
 *   6. GhostData.replayInfo.size does not exceed the fixed 16,200-byte replay buffer.
 *
 * OVERWRITE POLICY on import: a validated PLAYER ghost is deduplicated by a stable fingerprint of
 * its complete payload and installed alongside other replays for its exact course. The SRAM
 * compatibility slot is updated only when empty or when it already holds the same encoded course;
 * a different course is preserved without making the import fail.
 * Non-player containers retain the legacy SRAM-only behavior and never enter the player library.
 *
 * SRAM MIRRORING: when compatible, import writes through Save_WriteGhostRecord /
 * Save_WriteGhostData -- the same two calls Save_SaveGhost makes, and therefore the same
 * port/sram_buffer.cpp write-through path to fzerox.sav. It writes the raw record/data
 * verbatim instead of routing through a runtime Ghost struct the way
 * Save_SaveGhost(courseIndex, Ghost*) does, because Save_SaveGhostRecord (save.c:1283-1328)
 * always zeroes GhostRecord.trackName/unk_12 when building a record from a Ghost. Writing
 * the parsed record/data directly keeps export -> import -> export byte-identical for every
 * field, not just the ones a live gameplay save bothers to preserve.
 *
 * ROUND TRIP: exporting the current SRAM ghost and re-importing it preserves the GDG1 payload
 * byte-for-byte. The SRAM slot is reproduced too when it is empty or already holds that encoded
 * course; otherwise the import stays library-only so another course is not evicted.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Pass to gdx_ghost_export() to export whatever course the single SRAM ghost slot holds.
 * Any other value must match the slot's actual course (Save_LoadGhostInfo's
 * GhostInfo.courseIndex) or the export fails with GDX_GHOST_ERR_COURSE_MISMATCH, so
 * course-scoped UI fails safely rather than silently exporting a different course's ghost.
 */
#define GDX_GHOST_ANY_COURSE (-1)

/* Return codes. 0 is success; all error codes are negative. */
#define GDX_GHOST_OK 0
#define GDX_GHOST_ERR_BAD_ARGS (-1)          /* NULL/empty path */
#define GDX_GHOST_ERR_IO (-2)                /* file open/read/write/alloc failure */
#define GDX_GHOST_ERR_BAD_MAGIC (-3)         /* not a "GDG1" file */
#define GDX_GHOST_ERR_BAD_VERSION (-4)       /* unsupported/future container version */
#define GDX_GHOST_ERR_BAD_SIZE (-5)          /* payloadSize header field is wrong */
#define GDX_GHOST_ERR_BAD_CONTAINER_CRC (-6) /* file corrupted/truncated in transit */
#define GDX_GHOST_ERR_BAD_CHECKSUM (-7)      /* the game's own record/data checksum failed */
#define GDX_GHOST_ERR_BAD_COURSE (-8)        /* course out of range, or header/payload disagree */
#define GDX_GHOST_ERR_BAD_GHOST_TYPE (-9)    /* ghostType is GHOST_NONE or out of range */
#define GDX_GHOST_ERR_NO_GHOST (-10)         /* export: SRAM slot is empty */
#define GDX_GHOST_ERR_COURSE_MISMATCH (-11)  /* export: wrong course requested; non-player import:
                                                 SRAM holds a different course */
#define GDX_GHOST_ERR_SELECTION_FULL (-12)   /* three opponents are already selected for this course */
#define GDX_GHOST_ERR_NOT_FOUND (-13)        /* requested library fingerprint is absent */

/* Convenience default for the Practice tab's Export/Import buttons only;
 * gdx_ghost_export/gdx_ghost_import accept any path. */
#define GDX_GHOST_DEFAULT_FILENAME "ghost_export.gdg"

/* Multiple player ghosts per exact encoded course, kept in a host-side library. The full
 * encodedCourseIndex is part of every key, so replays recorded against different course geometry
 * can never be selected accidentally, and a 64-bit payload fingerprint gives every distinct replay
 * a stable local identity. The SRAM slot stays intact for save compatibility: it is archived before
 * any vanilla overwrite and continues to be read by the base game.
 *
 * Library files are ordinary validated GDG1 containers under `ghosts/` next to the executable.
 * Staff ghosts are deliberately not staged from here -- their ROM/EK unlock and loading paths
 * remain the base game's source of truth.
 */
#define GDX_GHOST_LIBRARY_MAX_ENTRIES 128

typedef struct GdxGhostLibraryEntry {
    uint64_t ghostId;
    int32_t courseIndex;
    int32_t encodedCourseIndex;
    int32_t raceTime;
    int32_t replayChecksum;
    int32_t lapTimes[3];
    uint16_t ghostType;
    uint8_t character;
    uint8_t bodyR;
    uint8_t bodyG;
    uint8_t bodyB;
    uint8_t selected;
} GdxGhostLibraryEntry;

/* Exports the single SRAM ghost slot to a .gdg file at `path` (created/overwritten).
 *
 * `courseIndex` is either GDX_GHOST_ANY_COURSE (export whatever course is currently
 * saved) or a specific course index that must match the saved ghost's course.
 *
 * Returns GDX_GHOST_OK (0) on success, or a negative GDX_GHOST_ERR_* code. On any
 * failure no file is left behind (a partially-written file is removed) and nothing in
 * SRAM/fzerox.sav is modified -- export is read-only with respect to the save.
 */
int gdx_ghost_export(int courseIndex, const char* path);

/* Imports a .gdg file at `path`. Player ghosts enter the exact-course PC library and are mirrored
 * into SRAM only when doing so does not evict a different encoded course.
 *
 * Returns GDX_GHOST_OK (0) on success, or a negative GDX_GHOST_ERR_* code. On any
 * validation failure the library, SRAM ghost slot, and fzerox.sav are left untouched.
 */
int gdx_ghost_import(const char* path);

/* Archives the current checksum-valid PLAYER ghost from the vanilla SRAM slot. Exact duplicates
 * are ignored; distinct same-course replays coexist. */
int gdx_ghost_library_archive_sram(void);

/* Persists a runtime decomp Ghost for its exact encoded course. Exact duplicates are ignored;
 * distinct replays coexist.
 * `ghost` must point to the real decomp Ghost layout (decomp/include/unk_structs.h). This is the
 * port/decomp boundary used by Save_SaveGhost and autosave-on-record. */
int gdx_ghost_library_save_player(int courseIndex, const void* ghost);

/* Compatibility queries for one exact course. When several entries exist these return/load the
 * fastest valid replay. New gameplay staging should use gdx_ghost_library_load_selected(). */
int gdx_ghost_library_get_player_info(int32_t encodedCourseIndex, GdxGhostLibraryEntry* outEntry);
int gdx_ghost_library_get_player_stats(int32_t encodedCourseIndex, int32_t* outRaceTime,
                                       int32_t* outReplayChecksum);
int gdx_ghost_library_load_player(int32_t encodedCourseIndex, void* outGhost);
int gdx_ghost_library_has_player(int32_t encodedCourseIndex);

/* Enumerates all checksum-valid player ghosts currently in the library, sorted by base course and
 * then encoded course. Returns the total copied count (0..capacity), or a negative error code. */
int gdx_ghost_library_list(GdxGhostLibraryEntry* entries, int capacity);

/* Selects the exact local/imported opponents used by Time Attack. Selection is persisted in a
 * small atomic host file, scoped per exact encoded course, and capped at the engine-native three
 * simultaneous ghosts. load_selected writes a contiguous array of real decomp Ghost objects and
 * returns the number loaded (0..capacity), or a negative error code. */
int gdx_ghost_library_set_selected(int32_t encodedCourseIndex, uint64_t ghostId, int selected);
int gdx_ghost_library_selected_count(int32_t encodedCourseIndex);
int gdx_ghost_library_load_selected(int32_t encodedCourseIndex, void* outGhosts, int capacity);

/* Exports one exact fingerprinted library entry to an arbitrary GDG1 path. */
int gdx_ghost_library_export(int32_t encodedCourseIndex, uint64_t ghostId, const char* path);

/* Fills `outPath` (NUL-terminated, at most `outCap` bytes including the terminator) with a
 * default .gdg path: on Windows the executable's own directory plus
 * GDX_GHOST_DEFAULT_FILENAME, mirroring port/sram_buffer.cpp's gdx_sram_path pattern; on
 * other hosts the bare relative filename.
 *
 * Returns 1 on success, 0 if the arguments are invalid or the path would not fit.
 * gdx_ghost_export/gdx_ghost_import never call this themselves.
 */
int gdx_ghost_default_path(char* outPath, size_t outCap);

#ifdef __cplusplus
}
#endif

#endif /* GDIFFUSER_GDX_GHOST_IO_H */
