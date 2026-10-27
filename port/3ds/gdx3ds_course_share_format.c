/* port/3ds/gdx3ds_course_share_format.c -- the `.gdxcourse` codec. Contract and file layout in
 * the header; the rules' sources (all decomp/src) are cited where each one is applied. */
#include "gdx3ds_course_share_format.h"

#include <stddef.h>
#include <string.h>

/* The checksum recomputation below must round after every float operation, exactly as the
 * game's MIPS (and ARM11 VFPv2) code does. Each operation is its own statement, which already
 * stops a C compiler contracting a multiply-add; the pragma says so to the ones that listen. */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

_Static_assert(sizeof(GdxCsControlPoint) == 0x14, "ControlPoint mirror size");
_Static_assert(sizeof(GdxCsCourse) == GDXCS_COURSE_BYTES, "CourseData mirror size");
_Static_assert(offsetof(GdxCsCourse, checksum) == 0x004, "CourseData.checksum");
_Static_assert(offsetof(GdxCsCourse, flag) == 0x008, "CourseData.flag");
_Static_assert(offsetof(GdxCsCourse, bgm) == 0x01F, "CourseData.bgm");
_Static_assert(offsetof(GdxCsCourse, controlPoint) == 0x020, "CourseData.controlPoint");
_Static_assert(offsetof(GdxCsCourse, bankAngle) == 0x520, "CourseData.bankAngle");
_Static_assert(offsetof(GdxCsCourse, pit) == 0x5A0, "CourseData.pit");
_Static_assert(offsetof(GdxCsCourse, sign) == 0x7A0, "CourseData.sign");

/* fzx_course.h track word layout. */
#define TSI_TYPE_MASK 0x3Fu
#define TSI_TYPE_NONE 0x3Fu
#define TSI_SHAPE_MASK 0x1C0u
#define TSI_JOIN_MASK 0x600u
#define TSI_FORM_MASK 0x38000u
#define TSI_FLAG_8000000 0x8000000u
#define TSI_FLAG_JOINABLE 0x10000000u
#define TSI_FLAG_INSIDE 0x20000000u
#define TSI_FLAG_CONTINUOUS 0x40000000u
/* Course_CalculateChecksum clears exactly these before summing, so a saved record never has
 * them set on a live point. */
#define TSI_CHECKSUM_CLEARED (TSI_JOIN_MASK | TSI_FORM_MASK | TSI_FLAG_CONTINUOUS)

#define SHAPE_ROAD 0x000u
#define SHAPE_WALLED_ROAD 0x040u
#define SHAPE_PIPE 0x080u
#define SHAPE_CYLINDER 0x0C0u
#define SHAPE_HALF_PIPE 0x100u
#define SHAPE_TUNNEL 0x140u
#define SHAPE_AIR 0x180u
#define SHAPE_BORDERLESS_ROAD 0x1C0u

#define RADIUS_MIN 50   /* course_edit/188850.c:1494-1500, 1631-1638 */
#define RADIUS_SUM_MAX 2000 /* 188850.c:1502-1506 */
#define BANK_MAX 360    /* (a + d + 360) % 360 in 188850.c:1535-1557; the disk's own DD-2 holds 360 */
#define POS_XZ_LIMIT 15000.0f /* course.c Course_CalculateChecksum */
#define POS_Y_MIN -250.0f
#define POS_Y_MAX 5000.0f
#define POINTS_MIN_FINISHED 4 /* 1A4210.c:322 and 19DD60.c:424: < 4 points is never "finished" */
#define VENUE_COUNT 10  /* gVenueWidget, 10 items (expansion_kit/A3AE0.c:288) */
#define SKYBOX_COUNT 8  /* gSkyboxWidget, 8 items (A3AE0.c:310) */
#define BGM_MAX 13      /* BGM_NEW_04: the game's own load check (course_gadgets.c, 19C470.c:161) */

const char* gdxcs_status_name(GdxCsStatus status) {
    static const char* const kNames[GDXCS_STATUS_COUNT] = {
        "ok",           "bad-argument",  "bad-size",     "bad-magic",     "unsupported-version",
        "bad-layout",   "header-crc",    "payload-crc",  "reserved-set",  "wrong-region",
        "wrong-disk",   "bad-name",      "kind-mismatch", "summary-mismatch", "bad-creator",
        "bad-point-count", "bad-venue",  "bad-skybox",   "bad-bgm",       "bad-flag",
        "bad-position", "bad-radius",    "bad-segment",  "bad-bank",      "bad-feature",
        "bad-tail",     "bad-checksum",
    };
    const unsigned index = (unsigned) status;
    if (index >= (unsigned) GDXCS_STATUS_COUNT) {
        return "unknown";
    }
    return kNames[index];
}

uint32_t gdxcs_crc32(const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*) data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < length; i++) {
        int bit;
        crc ^= bytes[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

uint64_t gdxcs_crc64(const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*) data;
    uint64_t crc = ~(uint64_t) 0;
    size_t i;
    for (i = 0; i < length; i++) {
        int bit;
        crc ^= bytes[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xC96C5795D7870F42ull;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

GdxCsDiskId gdxcs_disk_id_from_boot(const uint8_t gameName[4], uint8_t gameVersion, uint8_t diskNumber,
                                    const uint8_t company[2]) {
    uint8_t identity[8];
    GdxCsDiskId id;
    memcpy(identity, gameName, 4);
    identity[4] = gameVersion;
    identity[5] = diskNumber;
    identity[6] = company[0];
    identity[7] = company[1];
    id.crc64 = gdxcs_crc64(identity, sizeof(identity));
    id.region = gameName[3];
    return id;
}

/* ---- float rules --------------------------------------------------------------------- */

static uint32_t FloatBits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* func_i2_800BE9D4: the biased exponent must be 1..254 (no denormal, inf or NaN), or the word
 * must be exactly +0.0. Note -0.0 (0x80000000) FAILS this test in the game. */
static int FloatIsGameValid(float value) {
    const uint32_t bits = FloatBits(value);
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (bits == 0) {
        return 1;
    }
    return exponent >= 1u && exponent <= 254u;
}

static int PositionIsValid(const GdxCsControlPoint* point) {
    if (!FloatIsGameValid(point->x) || !FloatIsGameValid(point->y) || !FloatIsGameValid(point->z)) {
        return 0;
    }
    if (point->x < -POS_XZ_LIMIT || point->x > POS_XZ_LIMIT) {
        return 0;
    }
    if (point->y < POS_Y_MIN || point->y > POS_Y_MAX) {
        return 0;
    }
    if (point->z < -POS_XZ_LIMIT || point->z > POS_XZ_LIMIT) {
        return 0;
    }
    return 1;
}

/* ---- checksum ------------------------------------------------------------------------ */

static uint32_t PointTerm(const GdxCsControlPoint* point, int index) {
    const float fi = (float) index;
    float a;
    float b;
    float c;
    float d;
    float e;
    float sum;

    /* (x + ((1.1f + (0.7f * i)) * y)) */
    a = 0.7f * fi;
    a = 1.1f + a;
    a = a * point->y;
    sum = point->x + a;
    /* ((2.2f + (1.2f * i)) * z * (4.4f + (0.9f * i))) */
    b = 1.2f * fi;
    b = 2.2f + b;
    b = b * point->z;
    c = 0.9f * fi;
    c = 4.4f + c;
    b = b * c;
    sum = sum + b;
    /* + radiusLeft */
    d = (float) point->radiusLeft;
    sum = sum + d;
    /* + ((5.5f + (0.8f * i)) * radiusRight * 4.8f) */
    e = 0.8f * fi;
    e = 5.5f + e;
    e = e * (float) point->radiusRight;
    e = e * 4.8f;
    sum = sum + e;
    /* The game converts to s32 here; PositionIsValid bounds |sum| far below 2^31. */
    return (uint32_t) (int32_t) sum;
}

uint32_t gdxcs_course_checksum(const GdxCsCourse* course) {
    const int count = course->controlPointCount;
    uint32_t checksum = (uint32_t) (int32_t) course->controlPointCount;
    int i;

    for (i = 0; i < count && i < GDXCS_CONTROL_POINTS; i++) {
        const GdxCsControlPoint* point = &course->controlPoint[i];
        const uint32_t segment = (uint32_t) point->trackSegmentInfo & ~TSI_CHECKSUM_CLEARED;
        if (!PositionIsValid(point)) {
            return 0xFFFFFFFFu;
        }
        checksum += PointTerm(point, i);
        checksum += segment * (uint32_t) (0xFE - i);
        checksum += (uint32_t) ((int32_t) course->bankAngle[i] * (int32_t) (0x93DE - i * 2));
    }
    for (i = 0; i < count && i < GDXCS_CONTROL_POINTS; i++) {
        checksum += (uint32_t) ((int32_t) course->pit[i] * i);
        checksum += (uint32_t) ((int32_t) course->dash[i] * (i + 0x10));
        checksum += (uint32_t) ((int32_t) course->dirt[i] * (i + 0x80));
        checksum += (uint32_t) ((int32_t) course->ice[i] * (i + 0x100));
        checksum += (uint32_t) ((int32_t) course->jump[i] * (i + 0x800));
        checksum += (uint32_t) ((int32_t) course->landmine[i] * (i + 0x1000));
        checksum += (uint32_t) ((int32_t) course->gate[i] * (i + 0x8000));
        checksum += (uint32_t) ((int32_t) course->building[i] * (i + 0x10000));
        checksum += (uint32_t) ((int32_t) course->sign[i] * (i + 0x80000));
    }
    return checksum;
}

/* ---- per-entry rules ------------------------------------------------------------------- */

/* The exact words the editor's style tables write (course_edit/188850.c:2236-2460), widened to
 * every type of a shape the game defines (the ROM courses use ROAD_1, which no table holds):
 * the type index is free within the shape's enum, the flag bits are fixed per shape. */
static int SegmentIsValid(uint32_t word) {
    const uint32_t shape = word & TSI_SHAPE_MASK;
    const uint32_t type = word & TSI_TYPE_MASK;
    const uint32_t flags = word & ~(TSI_SHAPE_MASK | TSI_TYPE_MASK);
    switch (shape) {
        case SHAPE_ROAD:
            return type <= 6u && flags == (TSI_FLAG_JOINABLE | TSI_FLAG_8000000);
        case SHAPE_WALLED_ROAD:
            return type <= 2u && flags == TSI_FLAG_8000000;
        case SHAPE_BORDERLESS_ROAD:
            return type <= 2u && flags == TSI_FLAG_8000000;
        case SHAPE_TUNNEL:
            return type <= 3u && flags == (TSI_FLAG_INSIDE | TSI_FLAG_8000000);
        case SHAPE_PIPE:
            return type <= 3u && flags == TSI_FLAG_INSIDE;
        case SHAPE_HALF_PIPE:
            return type <= 3u && flags == 0u;
        case SHAPE_CYLINDER:
            return type <= 3u && flags == 0u;
        case SHAPE_AIR:
            return type == TSI_TYPE_NONE && flags == TSI_FLAG_JOINABLE;
        default:
            return 0;
    }
}

static int RadiusIsValid(const GdxCsControlPoint* point) {
    const int left = point->radiusLeft;
    const int right = point->radiusRight;
    if (left < RADIUS_MIN || right < RADIUS_MIN) {
        return 0;
    }
    return left + right <= RADIUS_SUM_MAX;
}

static int InRange(int value, int lo, int hi) {
    return value >= lo && value <= hi;
}

static int FeaturesAreValid(const GdxCsCourse* course, int i) {
    /* fzx_course.h enums: each is NONE (-1) or 0..MAX-1. */
    return InRange(course->pit[i], -1, 3) && InRange(course->dash[i], -1, 2) && InRange(course->dirt[i], -1, 3) &&
           InRange(course->ice[i], -1, 3) && InRange(course->jump[i], -1, 2) && InRange(course->landmine[i], -1, 2) &&
           InRange(course->gate[i], -1, 2) && InRange(course->building[i], -1, 14) && InRange(course->sign[i], -1, 4);
}

static int PointIsZero(const GdxCsControlPoint* point) {
    return FloatBits(point->x) == 0 && FloatBits(point->y) == 0 && FloatBits(point->z) == 0 && point->radiusLeft == 0 &&
           point->radiusRight == 0 && point->trackSegmentInfo == 0;
}

static GdxCsStatus ValidateLivePoint(const GdxCsCourse* course, int i) {
    const GdxCsControlPoint* point = &course->controlPoint[i];
    const uint32_t word = (uint32_t) point->trackSegmentInfo;
    if (!PositionIsValid(point)) {
        return GDXCS_ERR_POSITION;
    }
    if (!RadiusIsValid(point)) {
        return GDXCS_ERR_RADIUS;
    }
    if ((word & TSI_CHECKSUM_CLEARED) != 0 || !SegmentIsValid(word)) {
        return GDXCS_ERR_SEGMENT;
    }
    if (!InRange(course->bankAngle[i], 0, BANK_MAX)) {
        return GDXCS_ERR_BANK;
    }
    if (!FeaturesAreValid(course, i)) {
        return GDXCS_ERR_FEATURE;
    }
    return GDXCS_OK;
}

/* An entry past controlPointCount is never read by the game, but it is still bytes from a
 * stranger. The editor leaves either zero (a fresh CourseContext, every disk course) or a point
 * of a longer course saved earlier, which was valid then -- possibly with the runtime JOIN/FORM
 * bits the checksum pass only clears on live points. Anything else is refused. */
static int TailEntryIsValid(const GdxCsCourse* course, int i) {
    const GdxCsControlPoint* point = &course->controlPoint[i];
    const uint32_t word = (uint32_t) point->trackSegmentInfo & ~TSI_CHECKSUM_CLEARED;
    if (!InRange(course->bankAngle[i], 0, BANK_MAX) || !FeaturesAreValid(course, i)) {
        return 0;
    }
    if (PointIsZero(point)) {
        return 1;
    }
    return PositionIsValid(point) && RadiusIsValid(point) && SegmentIsValid(word);
}

GdxCsStatus gdxcs_validate_course(const GdxCsCourse* course, int* outIndex) {
    int count;
    int i;
    if (outIndex != NULL) {
        *outIndex = -1;
    }
    if (course == NULL) {
        return GDXCS_ERR_ARG;
    }
    if (course->creatorId != GDXCS_CREATOR_NINTENDO) {
        return GDXCS_ERR_CREATOR;
    }
    if (course->flag > 1) {
        return GDXCS_ERR_FLAG;
    }
    count = course->controlPointCount;
    if (count < 1 || count > GDXCS_CONTROL_POINTS || (course->flag != 0 && count < POINTS_MIN_FINISHED)) {
        return GDXCS_ERR_POINT_COUNT;
    }
    if (!InRange(course->venue, 0, VENUE_COUNT - 1)) {
        return GDXCS_ERR_VENUE;
    }
    if (!InRange(course->skybox, 0, SKYBOX_COUNT - 1)) {
        return GDXCS_ERR_SKYBOX;
    }
    if (!InRange(course->bgm, 0, BGM_MAX)) {
        return GDXCS_ERR_BGM;
    }
    for (i = 0; i < count; i++) {
        const GdxCsStatus status = ValidateLivePoint(course, i);
        if (status != GDXCS_OK) {
            if (outIndex != NULL) {
                *outIndex = i;
            }
            return status;
        }
    }
    for (i = count; i < GDXCS_CONTROL_POINTS; i++) {
        if (!TailEntryIsValid(course, i)) {
            if (outIndex != NULL) {
                *outIndex = i;
            }
            return GDXCS_ERR_TAIL;
        }
    }
    if (gdxcs_course_checksum(course) != course->checksum) {
        return GDXCS_ERR_CHECKSUM;
    }
    return GDXCS_OK;
}

GdxCsStatus gdxcs_validate_name(const char* name) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ,'& 0123456789";
    size_t length = 0;
    size_t i;
    if (name == NULL) {
        return GDXCS_ERR_ARG;
    }
    while (length <= GDXCS_NAME_MAX && name[length] != '\0') {
        length++;
    }
    if (length == 0 || length > GDXCS_NAME_MAX) {
        return GDXCS_ERR_NAME;
    }
    for (i = 0; i < length; i++) {
        if (memchr(kAlphabet, name[i], sizeof(kAlphabet) - 1) == NULL) {
            return GDXCS_ERR_NAME;
        }
    }
    if (name[0] == ' ' || name[length - 1] == ' ') {
        return GDXCS_ERR_NAME;
    }
    return GDXCS_OK;
}

const char* gdxcs_extension_for_flag(uint8_t flag) {
    return flag != 0 ? "CRSD" : "CRSE";
}

/* ---- big-endian codec ------------------------------------------------------------------ */

static void PutU16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) v;
}

static void PutU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static void PutU64(uint8_t* p, uint64_t v) {
    PutU32(p, (uint32_t) (v >> 32));
    PutU32(p + 4, (uint32_t) v);
}

static uint16_t GetU16(const uint8_t* p) {
    return (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
}

static uint32_t GetU32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static uint64_t GetU64(const uint8_t* p) {
    return ((uint64_t) GetU32(p) << 32) | GetU32(p + 4);
}

static void PutF32(uint8_t* p, float v) {
    PutU32(p, FloatBits(v));
}

static float GetF32(const uint8_t* p) {
    const uint32_t bits = GetU32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void PutS8Array(uint8_t* p, const int8_t* src) {
    memcpy(p, src, GDXCS_CONTROL_POINTS);
}

void gdxcs_course_to_be(const GdxCsCourse* course, uint8_t out[GDXCS_COURSE_BYTES]) {
    int i;
    out[0x000] = course->creatorId;
    out[0x001] = (uint8_t) course->controlPointCount;
    out[0x002] = (uint8_t) course->venue;
    out[0x003] = (uint8_t) course->skybox;
    PutU32(out + 0x004, course->checksum);
    out[0x008] = course->flag;
    memcpy(out + 0x009, course->fileName, sizeof(course->fileName));
    out[0x01F] = (uint8_t) course->bgm;
    for (i = 0; i < GDXCS_CONTROL_POINTS; i++) {
        const GdxCsControlPoint* point = &course->controlPoint[i];
        uint8_t* p = out + 0x020 + i * 0x14;
        PutF32(p + 0x0, point->x);
        PutF32(p + 0x4, point->y);
        PutF32(p + 0x8, point->z);
        PutU16(p + 0xC, (uint16_t) point->radiusLeft);
        PutU16(p + 0xE, (uint16_t) point->radiusRight);
        PutU32(p + 0x10, (uint32_t) point->trackSegmentInfo);
    }
    for (i = 0; i < GDXCS_CONTROL_POINTS; i++) {
        PutU16(out + 0x520 + i * 2, (uint16_t) course->bankAngle[i]);
    }
    PutS8Array(out + 0x5A0, course->pit);
    PutS8Array(out + 0x5E0, course->dash);
    PutS8Array(out + 0x620, course->dirt);
    PutS8Array(out + 0x660, course->ice);
    PutS8Array(out + 0x6A0, course->jump);
    PutS8Array(out + 0x6E0, course->landmine);
    PutS8Array(out + 0x720, course->gate);
    PutS8Array(out + 0x760, course->building);
    PutS8Array(out + 0x7A0, course->sign);
}

void gdxcs_course_from_be(const uint8_t in[GDXCS_COURSE_BYTES], GdxCsCourse* course) {
    int i;
    memset(course, 0, sizeof(*course));
    course->creatorId = in[0x000];
    course->controlPointCount = (int8_t) in[0x001];
    course->venue = (int8_t) in[0x002];
    course->skybox = (int8_t) in[0x003];
    course->checksum = GetU32(in + 0x004);
    course->flag = in[0x008];
    memcpy(course->fileName, in + 0x009, sizeof(course->fileName));
    course->bgm = (int8_t) in[0x01F];
    for (i = 0; i < GDXCS_CONTROL_POINTS; i++) {
        GdxCsControlPoint* point = &course->controlPoint[i];
        const uint8_t* p = in + 0x020 + i * 0x14;
        point->x = GetF32(p + 0x0);
        point->y = GetF32(p + 0x4);
        point->z = GetF32(p + 0x8);
        point->radiusLeft = (int16_t) GetU16(p + 0xC);
        point->radiusRight = (int16_t) GetU16(p + 0xE);
        point->trackSegmentInfo = (int32_t) GetU32(p + 0x10);
    }
    for (i = 0; i < GDXCS_CONTROL_POINTS; i++) {
        course->bankAngle[i] = (int16_t) GetU16(in + 0x520 + i * 2);
    }
    memcpy(course->pit, in + 0x5A0, GDXCS_CONTROL_POINTS);
    memcpy(course->dash, in + 0x5E0, GDXCS_CONTROL_POINTS);
    memcpy(course->dirt, in + 0x620, GDXCS_CONTROL_POINTS);
    memcpy(course->ice, in + 0x660, GDXCS_CONTROL_POINTS);
    memcpy(course->jump, in + 0x6A0, GDXCS_CONTROL_POINTS);
    memcpy(course->landmine, in + 0x6E0, GDXCS_CONTROL_POINTS);
    memcpy(course->gate, in + 0x720, GDXCS_CONTROL_POINTS);
    memcpy(course->building, in + 0x760, GDXCS_CONTROL_POINTS);
    memcpy(course->sign, in + 0x7A0, GDXCS_CONTROL_POINTS);
}

/* ---- file ------------------------------------------------------------------------------- */

static const uint8_t kMagic[8] = { 'G', 'D', 'X', 'C', 'R', 'S', 0x1A, 0x0A };

GdxCsStatus gdxcs_encode_file(const GdxCsCourse* course, const char* name, const GdxCsDiskId* disk,
                              uint8_t* out, size_t cap, size_t* outLen) {
    GdxCsStatus status;
    size_t nameLength;
    if (course == NULL || name == NULL || disk == NULL || out == NULL || outLen == NULL || cap < GDXCS_FILE_BYTES) {
        return GDXCS_ERR_ARG;
    }
    status = gdxcs_validate_name(name);
    if (status != GDXCS_OK) {
        return status;
    }
    status = gdxcs_validate_course(course, NULL);
    if (status != GDXCS_OK) {
        return status;
    }
    nameLength = strlen(name);
    memset(out, 0, GDXCS_FILE_BYTES);
    memcpy(out, kMagic, sizeof(kMagic));
    PutU16(out + 0x08, GDXCS_VERSION);
    PutU16(out + 0x0A, GDXCS_HEADER_BYTES);
    PutU32(out + 0x0C, GDXCS_COURSE_BYTES);
    gdxcs_course_to_be(course, out + GDXCS_HEADER_BYTES);
    PutU32(out + 0x10, gdxcs_crc32(out + GDXCS_HEADER_BYTES, GDXCS_COURSE_BYTES));
    PutU64(out + 0x14, disk->crc64);
    out[0x1C] = disk->region;
    out[0x1D] = course->flag;
    out[0x1E] = (uint8_t) nameLength;
    memcpy(out + 0x20, name, nameLength);
    out[0x30] = (uint8_t) course->controlPointCount;
    out[0x31] = (uint8_t) course->venue;
    out[0x32] = (uint8_t) course->skybox;
    out[0x33] = (uint8_t) course->bgm;
    PutU32(out + 0x34, course->checksum);
    PutU32(out + 0x3C, gdxcs_crc32(out, 0x3C));
    *outLen = GDXCS_FILE_BYTES;
    return GDXCS_OK;
}

static int BytesAreZero(const uint8_t* p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* Header checks that need no payload decode, in the order a damaged file is most usefully
 * diagnosed: what it is, which version, intact, then whose disk. */
static GdxCsStatus CheckHeader(const uint8_t* data, const GdxCsDiskId* expect) {
    if (memcmp(data, kMagic, sizeof(kMagic)) != 0) {
        return GDXCS_ERR_MAGIC;
    }
    if (GetU16(data + 0x08) != GDXCS_VERSION) {
        return GDXCS_ERR_VERSION;
    }
    if (GetU16(data + 0x0A) != GDXCS_HEADER_BYTES || GetU32(data + 0x0C) != GDXCS_COURSE_BYTES) {
        return GDXCS_ERR_LAYOUT;
    }
    if (GetU32(data + 0x3C) != gdxcs_crc32(data, 0x3C)) {
        return GDXCS_ERR_HEADER_CRC;
    }
    if (GetU32(data + 0x10) != gdxcs_crc32(data + GDXCS_HEADER_BYTES, GDXCS_COURSE_BYTES)) {
        return GDXCS_ERR_PAYLOAD_CRC;
    }
    if (data[0x1F] != 0 || !BytesAreZero(data + 0x38, 4)) {
        return GDXCS_ERR_RESERVED;
    }
    if (data[0x1C] != GDXCS_REGION_JP || (expect != NULL && data[0x1C] != expect->region)) {
        return GDXCS_ERR_REGION;
    }
    if (expect != NULL && GetU64(data + 0x14) != expect->crc64) {
        return GDXCS_ERR_DISK;
    }
    return GDXCS_OK;
}

static GdxCsStatus CheckName(const uint8_t* data, char* name) {
    const size_t length = data[0x1E];
    if (length < 1 || length > GDXCS_NAME_MAX) {
        return GDXCS_ERR_NAME;
    }
    if (!BytesAreZero(data + 0x20 + length, GDXCS_NAME_FIELD - length)) {
        return GDXCS_ERR_RESERVED;
    }
    memcpy(name, data + 0x20, length);
    name[length] = '\0';
    if (strlen(name) != length) {
        return GDXCS_ERR_NAME; /* an embedded NUL */
    }
    return gdxcs_validate_name(name);
}

static GdxCsStatus CheckSummary(const uint8_t* data, const GdxCsCourse* course) {
    if (data[0x1D] != course->flag) {
        return GDXCS_ERR_KIND;
    }
    if (data[0x30] != (uint8_t) course->controlPointCount || data[0x31] != (uint8_t) course->venue ||
        data[0x32] != (uint8_t) course->skybox || data[0x33] != (uint8_t) course->bgm ||
        GetU32(data + 0x34) != course->checksum) {
        return GDXCS_ERR_SUMMARY;
    }
    return GDXCS_OK;
}

GdxCsStatus gdxcs_decode_file(const uint8_t* data, size_t length, const GdxCsDiskId* expect, GdxCsCourse* course,
                              char* nameOut) {
    GdxCsCourse decoded;
    char name[GDXCS_NAME_FIELD + 1];
    GdxCsStatus status;
    if (data == NULL || course == NULL || nameOut == NULL) {
        return GDXCS_ERR_ARG;
    }
    if (length != GDXCS_FILE_BYTES) {
        return GDXCS_ERR_SIZE;
    }
    status = CheckHeader(data, expect);
    if (status != GDXCS_OK) {
        return status;
    }
    status = CheckName(data, name);
    if (status != GDXCS_OK) {
        return status;
    }
    gdxcs_course_from_be(data + GDXCS_HEADER_BYTES, &decoded);
    status = CheckSummary(data, &decoded);
    if (status != GDXCS_OK) {
        return status;
    }
    status = gdxcs_validate_course(&decoded, NULL);
    if (status != GDXCS_OK) {
        return status;
    }
    *course = decoded;
    memcpy(nameOut, name, strlen(name) + 1);
    return GDXCS_OK;
}

void gdxcs_sd_filename(const char* name, char* out, size_t cap) {
    static const char kExt[] = GDXCS_FILE_EXT;
    size_t n = 0;
    size_t i;
    if (out == NULL || cap == 0) {
        return;
    }
    for (i = 0; name != NULL && name[i] != '\0' && i < GDXCS_NAME_MAX && n + 1 < cap; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        } else if (!(c >= '0' && c <= '9')) {
            c = '_';
        }
        out[n++] = c;
    }
    for (i = 0; kExt[i] != '\0' && n + 1 < cap; i++) {
        out[n++] = kExt[i];
    }
    out[n] = '\0';
}
