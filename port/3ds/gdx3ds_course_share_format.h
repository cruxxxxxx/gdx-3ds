/* port/3ds/gdx3ds_course_share_format.h -- the portable `.gdxcourse` file: one Expansion Kit
 * custom course, self-contained, validated as untrusted input.
 *
 * PURE C99, no <3ds.h>, no decomp headers, no allocation: this TU is compiled unchanged into
 * the EK build and into the host unit test (port/3ds/test/gdx3ds_course_share_host_tests.c),
 * so every byte rule below is exercised on the host before a console ever sees a file.
 *
 * WHAT A COURSE IS. A course the EK
 * course editor saves is an MFS file on the disk's RAM area: NAME.CRSD (finished, raceable,
 * CourseData.flag = 1) or NAME.CRSE (unfinished, flag = 0), whose body is the whole
 * CourseContext (0xC830 bytes: CourseData + 3 GhostSave + SaveCourseRecords). The edit cup
 * holds six of them by name (CRS_ENTRY.CENT). Only the 0x7E0-byte CourseData is the course;
 * ghosts and records are per-console results that an import re-initialises exactly as the
 * editor's own save does. CourseData references nothing else on the disk except by small
 * enum ids (venue, skybox, bgm, segment shape/type, feature kinds), which is why the file
 * carries the disk identity: the ids only mean the same thing on the same disk.
 *
 * FILE LAYOUT (version 1). Every multi-byte field is BIG-ENDIAN, and the payload is the
 * CourseData exactly as it sits in N64 RAM, so the same bytes describe the course on real
 * hardware. Total size is fixed: GDXCS_FILE_BYTES; anything longer or shorter is refused.
 *
 *   0x00  8  magic        "GDXCRS" 0x1A 0x0A (the 0x1A/0x0A pair catches text-mode mangling)
 *   0x08  2  version      1
 *   0x0A  2  headerBytes  0x40
 *   0x0C  4  payloadBytes 0x7E0
 *   0x10  4  payloadCrc32 CRC-32 (ISO-HDLC, the zlib one) of the 0x7E0 payload bytes
 *   0x14  8  diskCrc64    CRC-64/XZ of the disk's identity block (see DISK IDENTITY)
 *   0x1C  1  diskRegion   the game code's region letter: 'J' (the only one v1 accepts)
 *   0x1D  1  kind         0 = unfinished (.CRSE), 1 = finished (.CRSD); == payload flag
 *   0x1E  1  nameLength   1..8
 *   0x1F  1  reserved     0
 *   0x20 16  name         MFS file name, nameLength bytes then zero padding
 *   0x30  1  pointCount   == payload controlPointCount (listing summary, cross-checked)
 *   0x31  1  venue        == payload venue
 *   0x32  1  skybox       == payload skybox
 *   0x33  1  bgm          == payload bgm
 *   0x34  4  courseChecksum == payload checksum
 *   0x38  4  reserved     0
 *   0x3C  4  headerCrc32  CRC-32 of header bytes 0x00..0x3B
 *   0x40 0x7E0 payload    CourseData, big-endian
 *
 * DISK IDENTITY. Not the whole-image CRC-64 the loader keys saves on: that one covers the
 * per-medium defect map and the MFS RAM area, so two dumps of two retail discs never agree
 * (port/disk_buffer.cpp, B11m) and a course could only ever go back to the console it came
 * from. What has to match is the GAME the ids belong to, which is what the 64DD's own disk ID
 * block (LBA 14, LEODiskID) names and what the game itself compares on a disk change
 * (sys_leo_dd.c func_807041C0): diskCrc64 = CRC-64/XZ of its 8 bytes gameName[4], gameVersion,
 * diskNumber, company[2] (gdxcs_disk_id_from_boot). Per-disc fields (serial number, usage) are
 * left out. The fan-translated disk keeps the retail game code, and its courses use the same
 * ids, so it shares courses with the Japanese disk.
 *
 * VALIDATION (gdxcs_decode_file). Nothing in the file is trusted, lengths least of all: the
 * total size is a constant, so no field ever sizes a copy. Every header field is checked,
 * both CRCs, the disk identity, the name against the game's own name-entry alphabet, and the
 * payload against what the editor can produce (gdxcs_validate_course): the game's checksum,
 * recomputed; enum ranges; the exact segment-info words the editor writes; the float rule
 * Course_CalculateChecksum enforces; radius and bank-angle ranges the editor clamps to.
 */
#ifndef GDX3DS_COURSE_SHARE_FORMAT_H
#define GDX3DS_COURSE_SHARE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDXCS_CONTROL_POINTS 64
#define GDXCS_COURSE_BYTES 0x7E0u
#define GDXCS_HEADER_BYTES 0x40u
#define GDXCS_FILE_BYTES (GDXCS_HEADER_BYTES + GDXCS_COURSE_BYTES)
#define GDXCS_VERSION 1u
#define GDXCS_NAME_MAX 8
#define GDXCS_NAME_FIELD 16
#define GDXCS_REGION_JP 'J'
#define GDXCS_CREATOR_NINTENDO 4
#define GDXCS_FILE_EXT ".gdxcourse"
/* sdmc-relative directory the console exports into and lists for import. */
#define GDXCS_SD_DIR "sdmc:/3ds/gdiffuser/courses"

/* Host-native mirror of the decomp's CourseData (decomp/include/fzx_course.h). Same field
 * order, same natural alignment, no pointers, so the layout is identical on the 3DS (ILP32)
 * and on an LP64 host; the EK-side TU static-asserts it against the real struct. */
typedef struct GdxCsControlPoint {
    float x;
    float y;
    float z;
    int16_t radiusLeft;
    int16_t radiusRight;
    int32_t trackSegmentInfo;
} GdxCsControlPoint; /* 0x14 */

typedef struct GdxCsCourse {
    uint8_t creatorId;
    int8_t controlPointCount;
    int8_t venue;
    int8_t skybox;
    uint32_t checksum;
    uint8_t flag;
    uint8_t fileName[22]; /* opaque to the game; carried byte for byte */
    int8_t bgm;
    GdxCsControlPoint controlPoint[GDXCS_CONTROL_POINTS];
    int16_t bankAngle[GDXCS_CONTROL_POINTS];
    int8_t pit[GDXCS_CONTROL_POINTS];
    int8_t dash[GDXCS_CONTROL_POINTS];
    int8_t dirt[GDXCS_CONTROL_POINTS];
    int8_t ice[GDXCS_CONTROL_POINTS];
    int8_t jump[GDXCS_CONTROL_POINTS];
    int8_t landmine[GDXCS_CONTROL_POINTS];
    int8_t gate[GDXCS_CONTROL_POINTS];
    int8_t building[GDXCS_CONTROL_POINTS];
    int8_t sign[GDXCS_CONTROL_POINTS];
} GdxCsCourse; /* 0x7E0 */

typedef struct GdxCsDiskId {
    uint64_t crc64;
    uint8_t region;
} GdxCsDiskId;

/* Every outcome has a name so a receipt never says just "failed". Order is ABI for nothing;
 * gdxcs_status_name() is the only consumer. */
typedef enum GdxCsStatus {
    GDXCS_OK = 0,
    GDXCS_ERR_ARG,          /* NULL pointer or a too-small output buffer */
    GDXCS_ERR_SIZE,         /* file is not exactly GDXCS_FILE_BYTES */
    GDXCS_ERR_MAGIC,
    GDXCS_ERR_VERSION,      /* a version this build does not read */
    GDXCS_ERR_LAYOUT,       /* headerBytes / payloadBytes are not the v1 constants */
    GDXCS_ERR_HEADER_CRC,
    GDXCS_ERR_PAYLOAD_CRC,
    GDXCS_ERR_RESERVED,     /* a reserved byte or name padding is not zero */
    GDXCS_ERR_REGION,       /* made on a different disk region */
    GDXCS_ERR_DISK,         /* made on a different disk image */
    GDXCS_ERR_NAME,         /* not a name the EK name entry can produce */
    GDXCS_ERR_KIND,         /* header kind disagrees with the payload flag */
    GDXCS_ERR_SUMMARY,      /* header summary bytes disagree with the payload */
    GDXCS_ERR_CREATOR,
    GDXCS_ERR_POINT_COUNT,
    GDXCS_ERR_VENUE,
    GDXCS_ERR_SKYBOX,
    GDXCS_ERR_BGM,
    GDXCS_ERR_FLAG,
    GDXCS_ERR_POSITION,     /* non-finite, denormal, -0.0 or outside the game's box */
    GDXCS_ERR_RADIUS,
    GDXCS_ERR_SEGMENT,      /* a trackSegmentInfo word the editor never writes */
    GDXCS_ERR_BANK,
    GDXCS_ERR_FEATURE,
    GDXCS_ERR_TAIL,         /* an unused entry (index >= count) that is neither zero nor valid */
    GDXCS_ERR_CHECKSUM,     /* the game's own course checksum does not match */
    GDXCS_STATUS_COUNT
} GdxCsStatus;

const char* gdxcs_status_name(GdxCsStatus status);

/* CRC-32/ISO-HDLC (poly 0xEDB88320 reflected, init/xorout 0xFFFFFFFF). */
uint32_t gdxcs_crc32(const void* data, size_t length);

/* CRC-64/XZ (poly 0x42F0E1EBA9EA3693 reflected, init/xorout all ones): the loader's own disk CRC. */
uint64_t gdxcs_crc64(const void* data, size_t length);

/* The disk identity (see DISK IDENTITY) from the fields of the disk's LEODiskID. */
GdxCsDiskId gdxcs_disk_id_from_boot(const uint8_t gameName[4], uint8_t gameVersion, uint8_t diskNumber,
                                    const uint8_t company[2]);

/* The game's Course_CalculateChecksum (ovl_i2/course.c), recomputed without mutating `course`
 * (the game clears the JOIN/FORM/CONTINUOUS bits as a side effect; this applies the same masks
 * to a local copy). Returns 0xFFFFFFFF where the game returns -1 (a float-exception position or
 * one outside its box). Every float operation is a separate statement so no compiler can fuse
 * a multiply-add: the game's MIPS and ARM11 builds both round after every operation. */
uint32_t gdxcs_course_checksum(const GdxCsCourse* course);

/* The editor-producible course rules (see the header comment). `outIndex`, if not NULL,
 * receives the control-point index the first failure was found at, or -1. */
GdxCsStatus gdxcs_validate_course(const GdxCsCourse* course, int* outIndex);

/* 1..8 bytes of the name-entry keyboard ("ABCDEFGHIJKLMNOPQRSTUVWXYZ,'& 0123456789",
 * expansion_kit/A6340.c), no leading or trailing space, NUL-terminated within 9 bytes. */
GdxCsStatus gdxcs_validate_name(const char* name);

/* "CRSD" for a finished course (flag != 0), "CRSE" otherwise. */
const char* gdxcs_extension_for_flag(uint8_t flag);

/* Canonical big-endian serialization of a course record and its inverse. */
void gdxcs_course_to_be(const GdxCsCourse* course, uint8_t out[GDXCS_COURSE_BYTES]);
void gdxcs_course_from_be(const uint8_t in[GDXCS_COURSE_BYTES], GdxCsCourse* course);

/* Builds a .gdxcourse. Validates the course and the name first: an invalid record is never
 * written out. `cap` must be >= GDXCS_FILE_BYTES; *outLen receives GDXCS_FILE_BYTES. */
GdxCsStatus gdxcs_encode_file(const GdxCsCourse* course, const char* name, const GdxCsDiskId* disk,
                              uint8_t* out, size_t cap, size_t* outLen);

/* Parses and validates a .gdxcourse of `length` bytes. `expect` is the running console's disk;
 * NULL skips only the disk-identity check (host tools). On GDXCS_OK `course` and `nameOut`
 * (>= GDXCS_NAME_MAX + 1 bytes) are filled; on any error neither is written. */
GdxCsStatus gdxcs_decode_file(const uint8_t* data, size_t length, const GdxCsDiskId* expect, GdxCsCourse* course,
                              char* nameOut);

/* "<name>.gdxcourse" with every character a FAT/SD file name dislikes (space , ' &) mapped to
 * '_'. Lower-cased for readability; the course name inside the file is the authority. */
void gdxcs_sd_filename(const char* name, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_COURSE_SHARE_FORMAT_H */
