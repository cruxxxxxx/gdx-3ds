/* port/gdx_ghost_io.c -- .gdg ghost replay import/export implementation.
 *
 * See gdx_ghost_io.h for the file format, endianness policy, validation rules, and
 * overwrite policy. This TU belongs to the G-Diffuser host-CRT target, not the
 * gdiffuser_game decomp object library, so the standard file API is available here --
 * the same split port/sram_buffer.cpp uses.
 *
 * PORT/DECOMP BOUNDARY: this file needs the exact GhostRecord/GhostData/GhostInfo layouts
 * (decomp/include/fzx_save.h, decomp/include/unk_structs.h) and calls straight into
 * decomp/src/overlays/ovl_i2/save.c -- but it deliberately does NOT include the decomp
 * headers. fzx_save.h drags in unk_structs.h and the PORT/EXPANSION_KIT/NON_MATCHING/
 * VERSION_US macro-gated declarations that only the gdiffuser_game target is compiled with.
 * It declares byte-for-byte mirror structs and raw `extern` prototypes instead: a C call
 * only needs the pointee's SIZE and FIELD OFFSETS to agree across the two translation
 * units, never the struct tag name. The gdx_ghost_*_size_check typedefs below turn any
 * future drift between mirror and original into a compile error here rather than silent
 * SRAM corruption.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fread/fwrite below; harmless on non-MSVC */

#include "gdx_ghost_io.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ---------------------------------------------------------------------------------
 * Mirrored decomp ghost structs.
 *
 * Each one cites the real declaration so this stays auditable against
 * decomp/include/fzx_save.h and decomp/include/unk_structs.h. Fixed-width <stdint.h>
 * types -- NOT bare long -- are what make the mirrors data-model-independent: the layouts
 * hold identically whether the decomp's u32/s32 resolve to unsigned long/long (MSVC,
 * LLP64) or unsigned int/int (GCC/Clang, LP64). Natural alignment only; no #pragma pack
 * is needed because every struct below already lands on the offsets and total sizes the
 * real headers document, enforced by the static size checks at the end of this block.
 * ------------------------------------------------------------------------------- */

/* Mirrors MachineInfo, decomp/include/unk_structs.h:43-64. 20 fields, all u8 -> no
 * alignment padding possible; size is exactly 0x14 (20) bytes. */
typedef struct GdxMachineInfo {
    uint8_t character;
    uint8_t customType;
    uint8_t frontType;
    uint8_t rearType;
    uint8_t wingType;
    uint8_t logo;
    uint8_t number;
    uint8_t decal;
    uint8_t bodyR;
    uint8_t bodyG;
    uint8_t bodyB;
    uint8_t numberR;
    uint8_t numberG;
    uint8_t numberB;
    uint8_t decalR;
    uint8_t decalG;
    uint8_t decalB;
    uint8_t cockpitR;
    uint8_t cockpitG;
    uint8_t cockpitB;
} GdxMachineInfo;

/* Mirrors unk_80141C88_unk_1D, decomp/include/unk_structs.h:445-448 (MachineInfo +
 * 12 reserved bytes). Size is exactly 0x20 (32) bytes -- all-u8 members, no padding. */
typedef struct GdxMachineInfoPadded {
    GdxMachineInfo info;
    int8_t reserved[12];
} GdxMachineInfoPadded;

/* Mirrors GhostRecord, decomp/include/fzx_save.h:74-84. Size 0x40 (64) bytes: the s32
 * fields at offsets 4/8/12 are 4-byte aligned with no gaps and the total is a multiple
 * of 4, so no trailing padding is inserted. */
typedef struct GdxGhostRecord {
    uint16_t checksum;
    uint16_t ghostType;
    int32_t replayChecksum;
    int32_t encodedCourseIndex;
    int32_t raceTime;
    uint16_t unk10;
    int8_t unk12[5];
    uint8_t trackName[9];
    GdxMachineInfoPadded machine; /* GhostRecord.unk_20 */
} GdxGhostRecord;

/* Mirrors GhostReplayInfo, decomp/include/fzx_save.h:86-94. Size 0x20 (32) bytes. */
typedef struct GdxGhostReplayInfo {
    uint16_t checksum;
    int16_t unk02;
    int32_t lapTimes[3];
    int32_t end;
    uint32_t size;
    int32_t unk18;
    int32_t unk1C;
} GdxGhostReplayInfo;

#define GDX_GHOST_REPLAY_DATA_SIZE 16200

/* Mirrors GhostData, decomp/include/fzx_save.h:96-100. Size 0x3F80 (16256) bytes. */
typedef struct GdxGhostData {
    GdxGhostReplayInfo replayInfo;
    uint8_t replayData[GDX_GHOST_REPLAY_DATA_SIZE];
    int8_t unk3F68[0x18];
} GdxGhostData;

/* Mirrors GhostSave, decomp/include/fzx_save.h:108-111. Size 0x3FC0 (16320) bytes --
 * this is exactly the ".gdg payload" described in gdx_ghost_io.h. */
typedef struct GdxGhostSave {
    GdxGhostRecord record;
    GdxGhostData data;
} GdxGhostSave;

/* Mirrors Ghost, decomp/include/unk_structs.h:369-379. The runtime form, converted to and
 * from the persistent GhostRecord + GhostData pair by the same field rules as save.c. */
typedef struct GdxRuntimeGhost {
    int32_t encodedCourseIndex;
    int32_t raceTime;
    int32_t lapTimes[3];
    int32_t replayEnd;
    int32_t replaySize;
    int8_t replayData[GDX_GHOST_REPLAY_DATA_SIZE];
    int32_t replayChecksum;
    int16_t ghostType;
    GdxMachineInfo machineInfo;
} GdxRuntimeGhost;

/* Mirrors GhostInfo, decomp/include/unk_structs.h:450-459. Real size is 0x40 (64) bytes:
 * the fields sum to 0x3D (61), but the s32 members force the struct size up to the next
 * multiple of 4. Only the leading `courseIndex` is used below; the rest is kept so the
 * mirror stays a faithful copy. */
typedef struct GdxGhostInfo {
    int32_t courseIndex;
    int32_t encodedCourseIndex;
    int32_t raceTime;
    int32_t replayChecksum;
    uint16_t ghostType;
    uint16_t unk12;
    char trackName[9];
    GdxMachineInfoPadded unk1D;
} GdxGhostInfo;

/* A negative array size is a compile error, so drift between these mirrors and the real
 * decomp structs fails loudly here instead of corrupting SRAM through a mis-sized call. */
typedef char gdx_ghost_record_size_check[(sizeof(GdxGhostRecord) == 0x40) ? 1 : -1];
typedef char gdx_ghost_data_size_check[(sizeof(GdxGhostData) == 0x3F80) ? 1 : -1];
typedef char gdx_ghost_save_size_check[(sizeof(GdxGhostSave) == 0x3FC0) ? 1 : -1];
typedef char gdx_runtime_ghost_size_check[(sizeof(GdxRuntimeGhost) == 0x3F80) ? 1 : -1];
typedef char gdx_ghost_info_size_check[(sizeof(GdxGhostInfo) == 0x40) ? 1 : -1];

/* GhostType enum values, decomp/include/fzx_save.h:7-13 (GHOST_NONE excluded here --
 * it marks an empty slot, not a real ghost, so importing one is rejected). */
#define GDX_GHOST_TYPE_PLAYER 1
#define GDX_GHOST_TYPE_STAFF 2
#define GDX_GHOST_TYPE_CELEBRITY 3
#define GDX_GHOST_TYPE_CHAMP 4

/* COURSE_EDIT_1, decomp/include/fzx_course.h:74 -- the base-course range is [0, 24), the
 * same bound Gdx_AutosaveGhostOnRecord and Menus_AttemptSaveGhost use to gate
 * Save_SaveGhost (decomp/src/overlays/ovl_i3/menus.c). */
#define GDX_GHOST_MAX_COURSE_INDEX 24

/* ---------------------------------------------------------------------------------
 * Real decomp entry points (decomp/src/overlays/ovl_i2/save.c). Raw extern declarations,
 * no header include -- see the file header above for why. EXPANSION_KIT is always on for
 * this port (port/CMakeLists.txt's GDX_EXPANSION_KIT defaults ON), so the EXPANSION_KIT
 * branch inside each is the one in effect. Save_Read/WriteGhostRecord/Data and
 * Save_CalculateGhost*Checksum never touch gSaveContext directly, only the pointer handed
 * to them, which is what lets this file round-trip a local GdxGhostSave buffer without
 * ever seeing gSaveContext. s32/u16 returns are spelled int/unsigned short here, which is
 * byte-identical to the decomp's types on this port's targets.
 */
extern int Save_LoadGhostInfo(GdxGhostInfo* ghostInfo);
extern void Save_ReadGhostRecord(GdxGhostRecord* ghostRecord);
extern void Save_ReadGhostData(GdxGhostData* ghostData);
extern void Save_WriteGhostRecord(GdxGhostRecord* ghostRecord);
extern void Save_WriteGhostData(GdxGhostData* ghostData);
extern unsigned short Save_CalculateGhostRecordChecksum(GdxGhostRecord* ghostRecord);
extern unsigned short Save_CalculateGhostDataChecksum(GdxGhostData* ghostData);
extern void Save_LoadGhostRecord(GdxGhostRecord* ghostRecord, GdxGhostData* ghostData, void* ghost, int arg3);
extern void Save_LoadGhostData(GdxGhostRecord* ghostRecord, GdxGhostData* ghostData, void* ghost, int arg3);

/* ---------------------------------------------------------------------------------
 * .gdg container header: fixed 20-byte, explicit little-endian, hand-packed (see the
 * format comment in gdx_ghost_io.h). Independent of host struct layout/alignment.
 * ------------------------------------------------------------------------------- */

#define GDX_GHOST_HEADER_SIZE 20
#define GDX_GHOST_FORMAT_VERSION 1u

static void gdx_write_u32le(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
    p[2] = (unsigned char) ((v >> 16) & 0xFFu);
    p[3] = (unsigned char) ((v >> 24) & 0xFFu);
}

static uint32_t gdx_read_u32le(const unsigned char* p) {
    uint32_t v;
    v = (uint32_t) p[0];
    v |= (uint32_t) p[1] << 8;
    v |= (uint32_t) p[2] << 16;
    v |= (uint32_t) p[3] << 24;
    return v;
}

static void gdx_ghost_pack_header(unsigned char* header, int32_t courseId, uint32_t payloadSize, uint32_t crc) {
    header[0] = 'G';
    header[1] = 'D';
    header[2] = 'G';
    header[3] = '1';
    gdx_write_u32le(header + 4, GDX_GHOST_FORMAT_VERSION);
    gdx_write_u32le(header + 8, (uint32_t) courseId);
    gdx_write_u32le(header + 12, payloadSize);
    gdx_write_u32le(header + 16, crc);
}

/* ---------------------------------------------------------------------------------
 * CRC-32 (IEEE 802.3 / zlib polynomial 0xEDB88320), reflected, table-based. Same
 * algorithm as disk_savefile.cpp's container CRC.
 * ------------------------------------------------------------------------------- */

static uint32_t gdx_crc32(const unsigned char* data, size_t length) {
    static uint32_t table[256];
    static int tableReady = 0;
    uint32_t crc;
    size_t i;

    if (!tableReady) {
        uint32_t c;
        unsigned int n;
        unsigned int k;
        for (n = 0; n < 256; n++) {
            c = (uint32_t) n;
            for (k = 0; k < 8; k++) {
                if (c & 1u) {
                    c = 0xEDB88320u ^ (c >> 1);
                } else {
                    c = c >> 1;
                }
            }
            table[n] = c;
        }
        tableReady = 1;
    }

    crc = 0xFFFFFFFFu;
    for (i = 0; i < length; i++) {
        crc = table[(crc ^ (uint32_t) data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint64_t gdx_ghost_fingerprint(const GdxGhostSave* save) {
    uint64_t payloadCrc = (uint64_t) gdx_crc32((const unsigned char*) save, sizeof(*save));
    return (payloadCrc << 32) | (uint32_t) save->record.replayChecksum;
}

static int gdx_ghost_validate_save(GdxGhostSave* save, int expectedCourse, int requirePlayer,
                                   GdxGhostLibraryEntry* outEntry) {
    int32_t courseIndex;
    int i;

    if (save == NULL) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    if (Save_CalculateGhostRecordChecksum(&save->record) != save->record.checksum ||
        Save_CalculateGhostDataChecksum(&save->data) != save->data.replayInfo.checksum) {
        return GDX_GHOST_ERR_BAD_CHECKSUM;
    }
    if (save->record.ghostType < GDX_GHOST_TYPE_PLAYER || save->record.ghostType > GDX_GHOST_TYPE_CHAMP ||
        (requirePlayer && save->record.ghostType != GDX_GHOST_TYPE_PLAYER)) {
        return GDX_GHOST_ERR_BAD_GHOST_TYPE;
    }
    if (save->data.replayInfo.size > GDX_GHOST_REPLAY_DATA_SIZE) {
        return GDX_GHOST_ERR_BAD_SIZE;
    }

    courseIndex = save->record.encodedCourseIndex & 0x1F;
    if (save->record.encodedCourseIndex == 0 || courseIndex < 0 || courseIndex >= GDX_GHOST_MAX_COURSE_INDEX ||
        (expectedCourse != GDX_GHOST_ANY_COURSE && expectedCourse != courseIndex)) {
        return GDX_GHOST_ERR_BAD_COURSE;
    }

    if (outEntry != NULL) {
        memset(outEntry, 0, sizeof(*outEntry));
        outEntry->ghostId = gdx_ghost_fingerprint(save);
        outEntry->courseIndex = courseIndex;
        outEntry->encodedCourseIndex = save->record.encodedCourseIndex;
        outEntry->raceTime = save->record.raceTime;
        outEntry->replayChecksum = save->record.replayChecksum;
        outEntry->ghostType = save->record.ghostType;
        outEntry->character = save->record.machine.info.character;
        outEntry->bodyR = save->record.machine.info.bodyR;
        outEntry->bodyG = save->record.machine.info.bodyG;
        outEntry->bodyB = save->record.machine.info.bodyB;
        for (i = 0; i < 3; i++) {
            outEntry->lapTimes[i] = save->data.replayInfo.lapTimes[i];
        }
    }
    return GDX_GHOST_OK;
}

static int gdx_ghost_read_container(const char* path, GdxGhostSave* save, GdxGhostLibraryEntry* outEntry,
                                    int requirePlayer) {
    FILE* f;
    unsigned char header[GDX_GHOST_HEADER_SIZE];
    uint32_t payloadSize;
    uint32_t storedCrc;
    int32_t headerCourse;
    int rc;

    if (path == NULL || path[0] == '\0' || save == NULL) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return GDX_GHOST_ERR_IO;
    }
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return GDX_GHOST_ERR_IO;
    }
    if (header[0] != 'G' || header[1] != 'D' || header[2] != 'G' || header[3] != '1') {
        fclose(f);
        return GDX_GHOST_ERR_BAD_MAGIC;
    }
    if (gdx_read_u32le(header + 4) != GDX_GHOST_FORMAT_VERSION) {
        fclose(f);
        return GDX_GHOST_ERR_BAD_VERSION;
    }

    headerCourse = (int32_t) gdx_read_u32le(header + 8);
    payloadSize = gdx_read_u32le(header + 12);
    storedCrc = gdx_read_u32le(header + 16);
    if (payloadSize != (uint32_t) sizeof(*save)) {
        fclose(f);
        return GDX_GHOST_ERR_BAD_SIZE;
    }
    if (fread(save, 1, sizeof(*save), f) != sizeof(*save)) {
        fclose(f);
        return GDX_GHOST_ERR_IO;
    }
    if (fgetc(f) != EOF) {
        fclose(f);
        return GDX_GHOST_ERR_BAD_SIZE;
    }
    fclose(f);

    if (gdx_crc32((const unsigned char*) save, sizeof(*save)) != storedCrc) {
        return GDX_GHOST_ERR_BAD_CONTAINER_CRC;
    }
    rc = gdx_ghost_validate_save(save, headerCourse, requirePlayer, outEntry);
    return rc;
}

static int gdx_ghost_write_container(const char* path, GdxGhostSave* save) {
    FILE* f;
    unsigned char header[GDX_GHOST_HEADER_SIZE];
    char tempPath[2048];
    uint32_t crc;
    int courseIndex;
    int rc;
    int n;

    if (path == NULL || path[0] == '\0' || save == NULL) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    rc = gdx_ghost_validate_save(save, GDX_GHOST_ANY_COURSE, 0, NULL);
    if (rc != GDX_GHOST_OK) {
        return rc;
    }
    n = snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);
    if (n < 0 || (size_t) n >= sizeof(tempPath)) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }

    courseIndex = save->record.encodedCourseIndex & 0x1F;
    crc = gdx_crc32((const unsigned char*) save, sizeof(*save));
    gdx_ghost_pack_header(header, courseIndex, (uint32_t) sizeof(*save), crc);

    f = fopen(tempPath, "wb");
    if (f == NULL) {
        return GDX_GHOST_ERR_IO;
    }
    rc = GDX_GHOST_OK;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) || fwrite(save, 1, sizeof(*save), f) != sizeof(*save) ||
        fflush(f) != 0) {
        rc = GDX_GHOST_ERR_IO;
    }
    if (fclose(f) != 0) {
        rc = GDX_GHOST_ERR_IO;
    }
    if (rc != GDX_GHOST_OK) {
        remove(tempPath);
        return rc;
    }

#ifdef _WIN32
    if (!MoveFileExA(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tempPath);
        return GDX_GHOST_ERR_IO;
    }
#else
    if (rename(tempPath, path) != 0) {
        remove(tempPath);
        return GDX_GHOST_ERR_IO;
    }
#endif
    return GDX_GHOST_OK;
}

static int gdx_ghost_executable_directory(char* outPath, size_t outCap) {
    if (outPath == NULL || outCap == 0) {
        return 0;
    }
#ifdef _WIN32
    {
        char* slash;
        DWORD n = GetModuleFileNameA(NULL, outPath, (DWORD) outCap);
        if (n == 0 || n >= outCap) {
            return 0;
        }
        slash = strrchr(outPath, '\\');
        if (slash == NULL) {
            return 0;
        }
        *slash = '\0';
        return 1;
    }
#else
    if (outCap < 2) {
        return 0;
    }
    outPath[0] = '.';
    outPath[1] = '\0';
    return 1;
#endif
}

static int gdx_ghost_library_directory(char* outPath, size_t outCap, int create) {
    char base[1024];
    int n;

    if (!gdx_ghost_executable_directory(base, sizeof(base))) {
        return 0;
    }
#ifdef _WIN32
    n = snprintf(outPath, outCap, "%s\\ghosts", base);
#else
    n = snprintf(outPath, outCap, "%s/ghosts", base);
#endif
    if (n < 0 || (size_t) n >= outCap) {
        return 0;
    }
    if (!create) {
        return 1;
    }
#ifdef _WIN32
    if (!CreateDirectoryA(outPath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
#else
    if (mkdir(outPath, 0755) != 0 && errno != EEXIST) {
        return 0;
    }
#endif
    return 1;
}

static int gdx_ghost_library_legacy_path(int32_t encodedCourseIndex, char* outPath, size_t outCap,
                                         int createDirectory) {
    char directory[1200];
    int n;

    if (encodedCourseIndex == 0 || !gdx_ghost_library_directory(directory, sizeof(directory), createDirectory)) {
        return 0;
    }
#ifdef _WIN32
    n = snprintf(outPath, outCap, "%s\\ghost_%08X.gdg", directory, (unsigned int) (uint32_t) encodedCourseIndex);
#else
    n = snprintf(outPath, outCap, "%s/ghost_%08X.gdg", directory, (unsigned int) (uint32_t) encodedCourseIndex);
#endif
    return n >= 0 && (size_t) n < outCap;
}

static int gdx_ghost_library_item_path(int32_t encodedCourseIndex, uint64_t ghostId, char* outPath, size_t outCap,
                                       int createDirectory) {
    char directory[1200];
    int n;

    if (encodedCourseIndex == 0 || ghostId == 0 ||
        !gdx_ghost_library_directory(directory, sizeof(directory), createDirectory)) {
        return 0;
    }
#ifdef _WIN32
    n = snprintf(outPath, outCap, "%s\\ghost_%08X_%016llX.gdg", directory, (unsigned int) (uint32_t) encodedCourseIndex,
                 (unsigned long long) ghostId);
#else
    n = snprintf(outPath, outCap, "%s/ghost_%08X_%016llX.gdg", directory, (unsigned int) (uint32_t) encodedCourseIndex,
                 (unsigned long long) ghostId);
#endif
    return n >= 0 && (size_t) n < outCap;
}

static int gdx_ghost_library_find_item_path(int32_t encodedCourseIndex, uint64_t ghostId, char* outPath,
                                            size_t outCap) {
    GdxGhostSave* save;
    GdxGhostLibraryEntry entry;
    char candidate[1600];
    int found = 0;

    if (encodedCourseIndex == 0 || ghostId == 0 || outPath == NULL || outCap == 0) {
        return 0;
    }
    save = (GdxGhostSave*) malloc(sizeof(*save));
    if (save == NULL) {
        return 0;
    }
    if (gdx_ghost_library_item_path(encodedCourseIndex, ghostId, candidate, sizeof(candidate), 0) &&
        gdx_ghost_read_container(candidate, save, &entry, 1) == GDX_GHOST_OK &&
        entry.encodedCourseIndex == encodedCourseIndex && entry.ghostId == ghostId) {
        found = snprintf(outPath, outCap, "%s", candidate) >= 0 && strlen(candidate) < outCap;
    }
    if (!found && gdx_ghost_library_legacy_path(encodedCourseIndex, candidate, sizeof(candidate), 0) &&
        gdx_ghost_read_container(candidate, save, &entry, 1) == GDX_GHOST_OK &&
        entry.encodedCourseIndex == encodedCourseIndex && entry.ghostId == ghostId) {
        found = snprintf(outPath, outCap, "%s", candidate) >= 0 && strlen(candidate) < outCap;
    }
    free(save);
    return found;
}

#define GDX_GHOST_SELECTION_MAX GDX_GHOST_LIBRARY_MAX_ENTRIES

typedef struct GdxGhostSelection {
    int32_t encodedCourseIndex;
    uint64_t ghostId;
} GdxGhostSelection;

static int gdx_ghost_selection_path(char* outPath, size_t outCap, int createDirectory) {
    char directory[1200];
    int n;

    if (!gdx_ghost_library_directory(directory, sizeof(directory), createDirectory)) {
        return 0;
    }
#ifdef _WIN32
    n = snprintf(outPath, outCap, "%s\\selection.gds", directory);
#else
    n = snprintf(outPath, outCap, "%s/selection.gds", directory);
#endif
    return n >= 0 && (size_t) n < outCap;
}

static int gdx_ghost_selection_load(GdxGhostSelection* selections, int capacity) {
    FILE* f;
    char path[1600];
    char magic[16];
    int count = 0;

    if (selections == NULL || capacity <= 0 || !gdx_ghost_selection_path(path, sizeof(path), 0)) {
        return 0;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return errno == ENOENT ? 0 : GDX_GHOST_ERR_IO;
    }
    if (fgets(magic, sizeof(magic), f) == NULL || strncmp(magic, "GDS1", 4) != 0) {
        fclose(f);
        return GDX_GHOST_ERR_IO;
    }
    while (count < capacity) {
        unsigned int encoded;
        unsigned long long id;
        int read = fscanf(f, "%x %llx", &encoded, &id);
        if (read == EOF) {
            break;
        }
        if (read != 2) {
            fclose(f);
            return GDX_GHOST_ERR_IO;
        }
        {
            char ghostPath[1600];
            int32_t encodedCourseIndex = (int32_t) encoded;
            uint64_t ghostId = (uint64_t) id;
            /* Users can remove shared .gdg files manually. Ignore stale selections so they do not
             * consume one of the three slots or permanently fill the selection table. */
            if (gdx_ghost_library_find_item_path(encodedCourseIndex, ghostId, ghostPath, sizeof(ghostPath))) {
                selections[count].encodedCourseIndex = encodedCourseIndex;
                selections[count].ghostId = ghostId;
                count++;
            }
        }
    }
    fclose(f);
    return count;
}

static int gdx_ghost_selection_write(const GdxGhostSelection* selections, int count) {
    FILE* f;
    char path[1600];
    char tempPath[1640];
    int i;
    int n;
    int rc = GDX_GHOST_OK;

    if (count < 0 || (count > 0 && selections == NULL) || !gdx_ghost_selection_path(path, sizeof(path), 1)) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    n = snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);
    if (n < 0 || (size_t) n >= sizeof(tempPath)) {
        return GDX_GHOST_ERR_IO;
    }
    f = fopen(tempPath, "w");
    if (f == NULL) {
        return GDX_GHOST_ERR_IO;
    }
    if (fprintf(f, "GDS1\n") < 0) {
        rc = GDX_GHOST_ERR_IO;
    }
    for (i = 0; i < count && rc == GDX_GHOST_OK; i++) {
        if (fprintf(f, "%08X %016llX\n", (unsigned int) (uint32_t) selections[i].encodedCourseIndex,
                    (unsigned long long) selections[i].ghostId) < 0) {
            rc = GDX_GHOST_ERR_IO;
        }
    }
    if (fflush(f) != 0) {
        rc = GDX_GHOST_ERR_IO;
    }
    if (fclose(f) != 0) {
        rc = GDX_GHOST_ERR_IO;
    }
    if (rc != GDX_GHOST_OK) {
        remove(tempPath);
        return rc;
    }
#ifdef _WIN32
    if (!MoveFileExA(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tempPath);
        return GDX_GHOST_ERR_IO;
    }
#else
    if (rename(tempPath, path) != 0) {
        remove(tempPath);
        return GDX_GHOST_ERR_IO;
    }
#endif
    return GDX_GHOST_OK;
}

static int gdx_ghost_selection_contains(int32_t encodedCourseIndex, uint64_t ghostId) {
    GdxGhostSelection selections[GDX_GHOST_SELECTION_MAX];
    int count = gdx_ghost_selection_load(selections, GDX_GHOST_SELECTION_MAX);
    int i;

    if (count < 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (selections[i].encodedCourseIndex == encodedCourseIndex && selections[i].ghostId == ghostId) {
            return 1;
        }
    }
    return 0;
}

static int gdx_ghost_build_save_from_runtime(int courseIndex, const GdxRuntimeGhost* ghost, GdxGhostSave* save) {
    int i;

    if (ghost == NULL || save == NULL || courseIndex < 0 || courseIndex >= GDX_GHOST_MAX_COURSE_INDEX ||
        ghost->encodedCourseIndex == 0 || (ghost->encodedCourseIndex & 0x1F) != courseIndex ||
        ghost->ghostType != GDX_GHOST_TYPE_PLAYER || ghost->replaySize < 0 ||
        ghost->replaySize > GDX_GHOST_REPLAY_DATA_SIZE) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }

    memset(save, 0, sizeof(*save));
    save->record.ghostType = (uint16_t) ghost->ghostType;
    save->record.replayChecksum = ghost->replayChecksum;
    save->record.encodedCourseIndex = ghost->encodedCourseIndex;
    save->record.raceTime = ghost->raceTime;
    save->record.machine.info = ghost->machineInfo;
    save->record.checksum = Save_CalculateGhostRecordChecksum(&save->record);

    for (i = 0; i < 3; i++) {
        save->data.replayInfo.lapTimes[i] = ghost->lapTimes[i];
    }
    save->data.replayInfo.end = ghost->replayEnd;
    save->data.replayInfo.size = (uint32_t) ghost->replaySize;
    memcpy(save->data.replayData, ghost->replayData, sizeof(save->data.replayData));
    save->data.replayInfo.checksum = Save_CalculateGhostDataChecksum(&save->data);
    return GDX_GHOST_OK;
}

static int gdx_ghost_library_store_player(GdxGhostSave* save) {
    char path[1600];
    uint64_t ghostId;
    int rc;

    rc = gdx_ghost_validate_save(save, GDX_GHOST_ANY_COURSE, 1, NULL);
    if (rc != GDX_GHOST_OK) {
        return rc;
    }
    ghostId = gdx_ghost_fingerprint(save);
    if (gdx_ghost_library_find_item_path(save->record.encodedCourseIndex, ghostId, path, sizeof(path))) {
        if (gdx_ghost_library_selected_count(save->record.encodedCourseIndex) == 0) {
            return gdx_ghost_library_set_selected(save->record.encodedCourseIndex, ghostId, 1);
        }
        return GDX_GHOST_OK;
    }
    if (!gdx_ghost_library_item_path(save->record.encodedCourseIndex, ghostId, path, sizeof(path), 1)) {
        return GDX_GHOST_ERR_IO;
    }
    rc = gdx_ghost_write_container(path, save);
    if (rc == GDX_GHOST_OK && gdx_ghost_library_selected_count(save->record.encodedCourseIndex) == 0) {
        /* Preserve the base game's no-browser-required behavior: the first durable player replay
         * for a course is selected automatically. Further local/imported ghosts remain explicit. */
        int selectRc = gdx_ghost_library_set_selected(save->record.encodedCourseIndex, ghostId, 1);
        if (selectRc != GDX_GHOST_OK) {
            return selectRc;
        }
    }
    return rc;
}

/* ---------------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------------- */

int gdx_ghost_export(int courseIndex, const char* path) {
    GdxGhostInfo info;
    GdxGhostSave* save;
    int loadResult;
    unsigned short recordCk;
    unsigned short dataCk;
    int rc;

    if (path == NULL || path[0] == '\0') {
        return GDX_GHOST_ERR_BAD_ARGS;
    }

    memset(&info, 0, sizeof(info));
    loadResult = Save_LoadGhostInfo(&info);
    if (loadResult != 0) {
        /* Non-zero means the slot was empty, or had a bad checksum and Save_LoadGhostInfo
         * self-healed it back to empty -- the same occupancy test Gdx_AutosaveGhostOnRecord
         * uses. Either way there is nothing valid to export. */
        return GDX_GHOST_ERR_NO_GHOST;
    }
    if (courseIndex != GDX_GHOST_ANY_COURSE && courseIndex != info.courseIndex) {
        return GDX_GHOST_ERR_COURSE_MISMATCH;
    }

    save = (GdxGhostSave*) malloc(sizeof(GdxGhostSave));
    if (save == NULL) {
        return GDX_GHOST_ERR_IO;
    }
    memset(save, 0, sizeof(*save));

    Save_ReadGhostRecord(&save->record);
    recordCk = Save_CalculateGhostRecordChecksum(&save->record);
    if (recordCk != save->record.checksum) {
        /* Should not happen right after a successful Save_LoadGhostInfo, but never export
         * a record this port cannot itself validate. */
        free(save);
        return GDX_GHOST_ERR_BAD_CHECKSUM;
    }

    Save_ReadGhostData(&save->data);
    dataCk = Save_CalculateGhostDataChecksum(&save->data);
    if (dataCk != save->data.replayInfo.checksum) {
        free(save);
        return GDX_GHOST_ERR_BAD_CHECKSUM;
    }

    rc = gdx_ghost_write_container(path, save);
    free(save);
    return rc;
}

int gdx_ghost_import(const char* path) {
    FILE* f;
    unsigned char header[GDX_GHOST_HEADER_SIZE];
    GdxGhostSave* save;
    GdxGhostInfo current;
    uint32_t payloadSize;
    uint32_t storedCrc;
    uint32_t computedCrc;
    int32_t headerCourse;
    int validationRc;
    int currentResult;

    if (path == NULL || path[0] == '\0') {
        return GDX_GHOST_ERR_BAD_ARGS;
    }

    f = fopen(path, "rb");
    if (f == NULL) {
        return GDX_GHOST_ERR_IO;
    }

    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return GDX_GHOST_ERR_IO;
    }

    if (header[0] != 'G' || header[1] != 'D' || header[2] != 'G' || header[3] != '1') {
        fclose(f);
        return GDX_GHOST_ERR_BAD_MAGIC;
    }
    if (gdx_read_u32le(header + 4) != GDX_GHOST_FORMAT_VERSION) {
        fclose(f);
        return GDX_GHOST_ERR_BAD_VERSION;
    }

    headerCourse = (int32_t) gdx_read_u32le(header + 8);
    payloadSize = gdx_read_u32le(header + 12);
    storedCrc = gdx_read_u32le(header + 16);

    if (payloadSize != (uint32_t) sizeof(GdxGhostSave)) {
        fclose(f);
        return GDX_GHOST_ERR_BAD_SIZE;
    }

    save = (GdxGhostSave*) malloc(sizeof(GdxGhostSave));
    if (save == NULL) {
        fclose(f);
        return GDX_GHOST_ERR_IO;
    }

    if (fread(save, 1, sizeof(*save), f) != sizeof(*save)) {
        fclose(f);
        free(save);
        return GDX_GHOST_ERR_IO;
    }
    if (fgetc(f) != EOF) {
        fclose(f);
        free(save);
        return GDX_GHOST_ERR_BAD_SIZE;
    }
    if (ferror(f)) {
        fclose(f);
        free(save);
        return GDX_GHOST_ERR_IO;
    }
    fclose(f);

    computedCrc = gdx_crc32((const unsigned char*) save, sizeof(*save));
    if (computedCrc != storedCrc) {
        free(save);
        return GDX_GHOST_ERR_BAD_CONTAINER_CRC;
    }

    /* The real import sanity check: the game's own checksum routines, called by the shared
     * validator rather than reimplemented here, plus type/course/replay bounds. */
    validationRc = gdx_ghost_validate_save(save, headerCourse, 0, NULL);
    if (validationRc != GDX_GHOST_OK) {
        free(save);
        return validationRc;
    }

    /* Player imports enter the per-course host library first. That is what removes the
     * cartridge's one-total-ghost limit; the SRAM mirror below is compatibility state, not
     * the only durable copy. */
    if (save->record.ghostType == GDX_GHOST_TYPE_PLAYER) {
        int libraryRc = gdx_ghost_library_store_player(save);
        if (libraryRc != GDX_GHOST_OK) {
            free(save);
            return libraryRc;
        }
    }

    memset(&current, 0, sizeof(current));
    currentResult = Save_LoadGhostInfo(&current);
    if (currentResult == 0 && current.encodedCourseIndex != save->record.encodedCourseIndex) {
        int playerImport = save->record.ghostType == GDX_GHOST_TYPE_PLAYER;
        /* Keep a different-course SRAM ghost intact. Player imports are already durable in the
         * host library and will be staged when their exact course starts. */
        free(save);
        return playerImport ? GDX_GHOST_OK : GDX_GHOST_ERR_COURSE_MISMATCH;
    }

    /* Install through the game's own SRAM helpers -- the same two calls Save_SaveGhost
     * makes, and therefore the same write-through path to fzerox.sav. Writing the parsed
     * record/data directly, rather than routing through a runtime Ghost struct the way
     * Save_SaveGhost(courseIndex, Ghost*) does, is what keeps trackName/unk_12
     * byte-identical to what was exported; see the header comment. Both calls recompute
     * the checksums, which the validation above already confirmed, so that is a no-op. */
    Save_WriteGhostRecord(&save->record);
    Save_WriteGhostData(&save->data);

    free(save);
    return GDX_GHOST_OK;
}

int gdx_ghost_library_archive_sram(void) {
    GdxGhostInfo info;
    GdxGhostSave* save;
    int rc;

    memset(&info, 0, sizeof(info));
    if (Save_LoadGhostInfo(&info) != 0 || info.ghostType != GDX_GHOST_TYPE_PLAYER) {
        return GDX_GHOST_ERR_NO_GHOST;
    }
    save = (GdxGhostSave*) malloc(sizeof(*save));
    if (save == NULL) {
        free(save);
        return GDX_GHOST_ERR_IO;
    }
    Save_ReadGhostRecord(&save->record);
    Save_ReadGhostData(&save->data);
    rc = gdx_ghost_validate_save(save, info.courseIndex, 1, NULL);
    if (rc != GDX_GHOST_OK) {
        free(save);
        return rc;
    }
    rc = gdx_ghost_library_store_player(save);
    free(save);
    return rc;
}

int gdx_ghost_library_save_player(int courseIndex, const void* ghost) {
    GdxGhostSave* save;
    int rc;

    save = (GdxGhostSave*) malloc(sizeof(*save));
    if (save == NULL) {
        return GDX_GHOST_ERR_IO;
    }
    rc = gdx_ghost_build_save_from_runtime(courseIndex, (const GdxRuntimeGhost*) ghost, save);
    if (rc == GDX_GHOST_OK) {
        rc = gdx_ghost_library_store_player(save);
    }
    free(save);
    return rc;
}

int gdx_ghost_library_get_player_info(int32_t encodedCourseIndex, GdxGhostLibraryEntry* outEntry) {
    GdxGhostLibraryEntry entries[GDX_GHOST_LIBRARY_MAX_ENTRIES];
    int count;
    int i;
    int found = 0;

    if (outEntry == NULL || encodedCourseIndex == 0) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    count = gdx_ghost_library_list(entries, GDX_GHOST_LIBRARY_MAX_ENTRIES);
    if (count < 0) {
        return count;
    }
    for (i = 0; i < count; i++) {
        if (entries[i].encodedCourseIndex == encodedCourseIndex &&
            (!found || entries[i].raceTime < outEntry->raceTime)) {
            *outEntry = entries[i];
            found = 1;
        }
    }
    return found ? GDX_GHOST_OK : GDX_GHOST_ERR_NOT_FOUND;
}

int gdx_ghost_library_get_player_stats(int32_t encodedCourseIndex, int32_t* outRaceTime,
                                       int32_t* outReplayChecksum) {
    GdxGhostLibraryEntry entry;
    int rc;

    if (outRaceTime == NULL || outReplayChecksum == NULL) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    rc = gdx_ghost_library_get_player_info(encodedCourseIndex, &entry);
    if (rc == GDX_GHOST_OK) {
        *outRaceTime = entry.raceTime;
        *outReplayChecksum = entry.replayChecksum;
    }
    return rc;
}

int gdx_ghost_library_load_player(int32_t encodedCourseIndex, void* outGhost) {
    GdxGhostSave* save;
    GdxGhostLibraryEntry entry;
    char path[1600];
    int rc;

    if (outGhost == NULL || gdx_ghost_library_get_player_info(encodedCourseIndex, &entry) != GDX_GHOST_OK ||
        !gdx_ghost_library_find_item_path(encodedCourseIndex, entry.ghostId, path, sizeof(path))) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    save = (GdxGhostSave*) malloc(sizeof(*save));
    if (save == NULL) {
        return GDX_GHOST_ERR_IO;
    }
    rc = gdx_ghost_read_container(path, save, &entry, 1);
    if (rc == GDX_GHOST_OK && entry.encodedCourseIndex != encodedCourseIndex) {
        rc = GDX_GHOST_ERR_BAD_COURSE;
    }
    if (rc == GDX_GHOST_OK) {
        /* arg3=0: the container was already checksum-validated above; the self-heal branch
         * (arg3=1) would rewrite the live SRAM ghost slot on mismatch, which must never
         * happen as a side effect of reading a library file. */
        Save_LoadGhostRecord(&save->record, &save->data, outGhost, 0);
        Save_LoadGhostData(&save->record, &save->data, outGhost, 0);
    }
    free(save);
    return rc;
}

int gdx_ghost_library_has_player(int32_t encodedCourseIndex) {
    GdxGhostLibraryEntry entry;
    return gdx_ghost_library_get_player_info(encodedCourseIndex, &entry) == GDX_GHOST_OK;
}

static int gdx_ghost_library_entry_compare(const GdxGhostLibraryEntry* a, const GdxGhostLibraryEntry* b) {
    if (a->courseIndex != b->courseIndex) {
        return a->courseIndex < b->courseIndex ? -1 : 1;
    }
    if (a->encodedCourseIndex != b->encodedCourseIndex) {
        return (uint32_t) a->encodedCourseIndex < (uint32_t) b->encodedCourseIndex ? -1 : 1;
    }
    if (a->raceTime != b->raceTime) {
        return a->raceTime < b->raceTime ? -1 : 1;
    }
    return a->ghostId < b->ghostId ? -1 : a->ghostId > b->ghostId;
}

int gdx_ghost_library_list(GdxGhostLibraryEntry* entries, int capacity) {
    char directory[1200];
    int count = 0;
    int i;
    int j;

    if (capacity < 0 || (capacity > 0 && entries == NULL)) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    if (capacity == 0 || !gdx_ghost_library_directory(directory, sizeof(directory), 0)) {
        return 0;
    }

#ifdef _WIN32
    {
        WIN32_FIND_DATAA findData;
        HANDLE findHandle;
        char pattern[1400];
        int n = snprintf(pattern, sizeof(pattern), "%s\\ghost_*.gdg", directory);
        if (n < 0 || (size_t) n >= sizeof(pattern)) {
            return GDX_GHOST_ERR_IO;
        }
        findHandle = FindFirstFileA(pattern, &findData);
        if (findHandle == INVALID_HANDLE_VALUE) {
            return 0;
        }
        do {
            GdxGhostSave* save;
            GdxGhostLibraryEntry entry;
            char path[1600];
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || count >= capacity) {
                continue;
            }
            n = snprintf(path, sizeof(path), "%s\\%s", directory, findData.cFileName);
            if (n < 0 || (size_t) n >= sizeof(path)) {
                continue;
            }
            save = (GdxGhostSave*) malloc(sizeof(*save));
            if (save == NULL) {
                FindClose(findHandle);
                return GDX_GHOST_ERR_IO;
            }
            if (gdx_ghost_read_container(path, save, &entry, 1) == GDX_GHOST_OK) {
                entries[count++] = entry;
            }
            free(save);
        } while (FindNextFileA(findHandle, &findData));
        FindClose(findHandle);
    }
#else
    {
        DIR* dir = opendir(directory);
        struct dirent* item;
        if (dir == NULL) {
            return errno == ENOENT ? 0 : GDX_GHOST_ERR_IO;
        }
        while ((item = readdir(dir)) != NULL && count < capacity) {
            GdxGhostSave* save;
            GdxGhostLibraryEntry entry;
            char path[1600];
            int n;
            size_t nameLen = strlen(item->d_name);
            if (nameLen < 10 || strncmp(item->d_name, "ghost_", 6) != 0 || strcmp(item->d_name + nameLen - 4, ".gdg") != 0) {
                continue;
            }
            n = snprintf(path, sizeof(path), "%s/%s", directory, item->d_name);
            if (n < 0 || (size_t) n >= sizeof(path)) {
                continue;
            }
            save = (GdxGhostSave*) malloc(sizeof(*save));
            if (save == NULL) {
                closedir(dir);
                return GDX_GHOST_ERR_IO;
            }
            if (gdx_ghost_read_container(path, save, &entry, 1) == GDX_GHOST_OK) {
                entries[count++] = entry;
            }
            free(save);
        }
        closedir(dir);
    }
#endif

    /* A legacy one-file-per-course entry and its new fingerprinted name may coexist during
     * migration. They represent the same payload, so expose them as one browser row. */
    for (i = 0; i < count; i++) {
        entries[i].selected = (uint8_t) gdx_ghost_selection_contains(entries[i].encodedCourseIndex,
                                                                      entries[i].ghostId);
        for (j = i + 1; j < count;) {
            if (entries[i].encodedCourseIndex == entries[j].encodedCourseIndex &&
                entries[i].ghostId == entries[j].ghostId) {
                int k;
                for (k = j; k < count - 1; k++) {
                    entries[k] = entries[k + 1];
                }
                count--;
            } else {
                j++;
            }
        }
    }

    for (i = 0; i < count - 1; i++) {
        for (j = i + 1; j < count; j++) {
            if (gdx_ghost_library_entry_compare(&entries[i], &entries[j]) > 0) {
                GdxGhostLibraryEntry temp = entries[i];
                entries[i] = entries[j];
                entries[j] = temp;
            }
        }
    }
    return count;
}

int gdx_ghost_library_set_selected(int32_t encodedCourseIndex, uint64_t ghostId, int selected) {
    GdxGhostSelection selections[GDX_GHOST_SELECTION_MAX];
    char path[1600];
    int count;
    int i;
    int existingIndex = -1;
    int courseCount = 0;

    if (encodedCourseIndex == 0 || ghostId == 0) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    count = gdx_ghost_selection_load(selections, GDX_GHOST_SELECTION_MAX);
    if (count < 0) {
        return count;
    }
    for (i = 0; i < count; i++) {
        if (selections[i].encodedCourseIndex == encodedCourseIndex) {
            courseCount++;
            if (selections[i].ghostId == ghostId) {
                existingIndex = i;
            }
        }
    }
    if (selected) {
        if (existingIndex >= 0) {
            return GDX_GHOST_OK;
        }
        if (courseCount >= 3) {
            return GDX_GHOST_ERR_SELECTION_FULL;
        }
        if (count >= GDX_GHOST_SELECTION_MAX) {
            return GDX_GHOST_ERR_IO;
        }
        if (!gdx_ghost_library_find_item_path(encodedCourseIndex, ghostId, path, sizeof(path))) {
            return GDX_GHOST_ERR_NOT_FOUND;
        }
        selections[count].encodedCourseIndex = encodedCourseIndex;
        selections[count].ghostId = ghostId;
        count++;
    } else {
        if (existingIndex < 0) {
            return GDX_GHOST_OK;
        }
        for (i = existingIndex; i < count - 1; i++) {
            selections[i] = selections[i + 1];
        }
        count--;
    }
    return gdx_ghost_selection_write(selections, count);
}

int gdx_ghost_library_selected_count(int32_t encodedCourseIndex) {
    GdxGhostSelection selections[GDX_GHOST_SELECTION_MAX];
    int count;
    int selectedCount = 0;
    int i;

    if (encodedCourseIndex == 0) {
        return 0;
    }
    count = gdx_ghost_selection_load(selections, GDX_GHOST_SELECTION_MAX);
    if (count < 0) {
        return count;
    }
    for (i = 0; i < count; i++) {
        if (selections[i].encodedCourseIndex == encodedCourseIndex) {
            selectedCount++;
        }
    }
    return selectedCount;
}

int gdx_ghost_library_load_selected(int32_t encodedCourseIndex, void* outGhosts, int capacity) {
    GdxGhostSelection selections[GDX_GHOST_SELECTION_MAX];
    GdxRuntimeGhost* ghosts = (GdxRuntimeGhost*) outGhosts;
    GdxGhostSave* save;
    int selectionCount;
    int loaded = 0;
    int i;

    if (encodedCourseIndex == 0 || outGhosts == NULL || capacity < 0 || capacity > 3) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    selectionCount = gdx_ghost_selection_load(selections, GDX_GHOST_SELECTION_MAX);
    if (selectionCount < 0) {
        return selectionCount;
    }
    save = (GdxGhostSave*) malloc(sizeof(*save));
    if (save == NULL) {
        return GDX_GHOST_ERR_IO;
    }
    for (i = 0; i < selectionCount && loaded < capacity; i++) {
        GdxGhostLibraryEntry entry;
        char path[1600];
        int rc;
        if (selections[i].encodedCourseIndex != encodedCourseIndex ||
            !gdx_ghost_library_find_item_path(encodedCourseIndex, selections[i].ghostId, path, sizeof(path))) {
            continue;
        }
        rc = gdx_ghost_read_container(path, save, &entry, 1);
        if (rc != GDX_GHOST_OK || entry.encodedCourseIndex != encodedCourseIndex ||
            entry.ghostId != selections[i].ghostId) {
            continue;
        }
        /* arg3=0 for the same reason as gdx_ghost_library_load_player: never let a library
         * read self-heal into the live SRAM ghost slot. */
        Save_LoadGhostRecord(&save->record, &save->data, &ghosts[loaded], 0);
        Save_LoadGhostData(&save->record, &save->data, &ghosts[loaded], 0);
        loaded++;
    }
    free(save);
    return loaded;
}

int gdx_ghost_library_export(int32_t encodedCourseIndex, uint64_t ghostId, const char* path) {
    GdxGhostSave* save;
    GdxGhostLibraryEntry entry;
    char sourcePath[1600];
    int rc;

    if (path == NULL || path[0] == '\0' ||
        !gdx_ghost_library_find_item_path(encodedCourseIndex, ghostId, sourcePath, sizeof(sourcePath))) {
        return GDX_GHOST_ERR_BAD_ARGS;
    }
    save = (GdxGhostSave*) malloc(sizeof(*save));
    if (save == NULL) {
        return GDX_GHOST_ERR_IO;
    }
    rc = gdx_ghost_read_container(sourcePath, save, &entry, 1);
    if (rc == GDX_GHOST_OK &&
        (entry.encodedCourseIndex != encodedCourseIndex || entry.ghostId != ghostId)) {
        rc = GDX_GHOST_ERR_BAD_COURSE;
    }
    if (rc == GDX_GHOST_OK) {
        rc = gdx_ghost_write_container(path, save);
    }
    free(save);
    return rc;
}

int gdx_ghost_default_path(char* outPath, size_t outCap) {
    if (outPath == NULL || outCap == 0) {
        return 0;
    }

#ifdef _WIN32
    {
        char exePath[MAX_PATH];
        char* slash;
        size_t exeDirLen;
        size_t fileNameLen;
        DWORD n;

        n = GetModuleFileNameA(NULL, exePath, (DWORD) sizeof(exePath));
        if (n == 0 || n >= sizeof(exePath)) {
            return 0;
        }
        slash = strrchr(exePath, '\\');
        if (slash == NULL) {
            return 0;
        }
        exeDirLen = (size_t) (slash - exePath) + 1; /* keep the trailing backslash */
        fileNameLen = strlen(GDX_GHOST_DEFAULT_FILENAME);
        if (exeDirLen + fileNameLen + 1 > outCap) {
            return 0;
        }
        memcpy(outPath, exePath, exeDirLen);
        memcpy(outPath + exeDirLen, GDX_GHOST_DEFAULT_FILENAME, fileNameLen + 1); /* + NUL */
        return 1;
    }
#else
    if (strlen(GDX_GHOST_DEFAULT_FILENAME) + 1 > outCap) {
        return 0;
    }
    strcpy(outPath, GDX_GHOST_DEFAULT_FILENAME); /* CWD-relative fallback */
    return 1;
#endif
}
