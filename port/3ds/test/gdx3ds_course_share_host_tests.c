/* port/3ds/test/gdx3ds_course_share_host_tests.c -- `[ek] course_share`: the `.gdxcourse` codec
 * (gdx3ds_course_share_format.c) and its QR transport (gdx3ds_course_share_qr.c), exactly the
 * TUs the EK build compiles.
 *
 * Files arrive from strangers, so most of this is the refusal side: every header field, every
 * course rule, truncation and extension, and a seeded mutation fuzz that fixes the CRCs up so
 * the mutations reach the course validator rather than dying at the checksum. The QR half
 * renders each frame into a bottom-screen-sized RGB565 framebuffer with the console's own blit,
 * decodes the picture with the vendored quirc, and reassembles the file out of order.
 *
 * The SCAN half (gdx3ds_course_share_scan.c) gets synthetic camera frames: a rendered page placed
 * in a 320x240 scene at a scale, rotation and slight keystone, optionally mirrored, lit by a
 * gradient from dim to very dim, blurred, noised, tinted, converted to YUV422 in either byte
 * order -- then the console's own luma pick, grey conversion, decoder (both threshold modes) and
 * session, which must rebuild the identical file across duplicates and a foreign course's pages.
 * The BMP frame source (the emulator's stand-in for a camera) reads the menu's own SHOT format.
 *
 * No disk data: every course here is synthetic. The checksum's golden value was computed by an
 * independent replica of Course_CalculateChecksum (decomp/src/game/course.c), the same replica
 * that matched all 22 courses on the Japanese Expansion Kit disk. */
#include "gdx3ds_course_share_format.h"
#include "gdx3ds_course_share_qr.h"
#include "gdx3ds_course_share_scan.h"

#include "vendor/qrcodegen/qrcodegen.h"
#include "vendor/quirc/quirc.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        sChecks++;                                                        \
        if (!(cond)) {                                                    \
            sFailures++;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_STATUS(got, want)                                                                      \
    do {                                                                                             \
        const GdxCsStatus g_ = (got);                                                                \
        sChecks++;                                                                                   \
        if (g_ != (want)) {                                                                          \
            sFailures++;                                                                             \
            fprintf(stderr, "FAIL %s:%d: got %s want %s\n", __FILE__, __LINE__, gdxcs_status_name(g_), \
                    gdxcs_status_name(want));                                                        \
        }                                                                                            \
    } while (0)

static const GdxCsDiskId kDisk = { 0x8D185CE0BD9EDABBull, GDXCS_REGION_JP };
#define GOLDEN_CHECKSUM 0xC9B950A1u

/* ---- course builders --------------------------------------------------------------------- */

static void ClearFeatures(GdxCsCourse* c, int i) {
    c->pit[i] = -1;
    c->dash[i] = -1;
    c->dirt[i] = -1;
    c->ice[i] = -1;
    c->jump[i] = -1;
    c->landmine[i] = -1;
    c->gate[i] = -1;
    c->building[i] = -1;
    c->sign[i] = -1;
}

static void SetPoint(GdxCsCourse* c, int i, float x, float y, float z, int rl, int rr, uint32_t tsi, int bank) {
    c->controlPoint[i].x = x;
    c->controlPoint[i].y = y;
    c->controlPoint[i].z = z;
    c->controlPoint[i].radiusLeft = (int16_t) rl;
    c->controlPoint[i].radiusRight = (int16_t) rr;
    c->controlPoint[i].trackSegmentInfo = (int32_t) tsi;
    c->bankAngle[i] = (int16_t) bank;
    ClearFeatures(c, i);
}

/* Four points, one of every kind of thing: road, walled, tunnel, air; banks up to 360; a feature
 * of every family at its maximum. Mirrored by the replica that produced GOLDEN_CHECKSUM. */
static void BuildGoldenCourse(GdxCsCourse* c) {
    memset(c, 0, sizeof(*c));
    c->creatorId = GDXCS_CREATOR_NINTENDO;
    c->controlPointCount = 4;
    c->venue = 3;
    c->skybox = 5;
    c->bgm = 13;
    c->flag = 1;
    memcpy(c->fileName, "junk-the-game-ignores", 21);
    SetPoint(c, 0, -8750.0f, 0.0f, -6550.0f, 300, 300, 0x18000000u, 0);
    SetPoint(c, 1, 8750.0f, 120.5f, -6550.0f, 250, 400, 0x08000042u, 15);
    SetPoint(c, 2, 8750.0f, 40.25f, 6550.0f, 500, 500, 0x28000141u, 345);
    SetPoint(c, 3, -8750.0f, 0.0f, 6550.0f, 300, 300, 0x100001BFu, 360);
    c->pit[1] = 0;
    c->pit[3] = 3;
    c->dash[0] = 1;
    c->dirt[2] = 2;
    c->ice[3] = 0;
    c->jump[1] = 2;
    c->landmine[2] = 1;
    c->gate[0] = 0;
    c->building[1] = 14;
    c->sign[3] = 4;
    c->checksum = gdxcs_course_checksum(c);
}

static uint32_t sRng = 0x1234ABCDu;

static uint32_t Rand(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static int RandRange(int lo, int hi) {
    return lo + (int) (Rand() % (uint32_t) (hi - lo + 1));
}

static uint32_t RandomSegment(void) {
    static const uint32_t kShapes[8][3] = {
        /* shape, max type, flags */
        { 0x000u, 6u, 0x18000000u }, { 0x040u, 2u, 0x08000000u }, { 0x1C0u, 2u, 0x08000000u },
        { 0x140u, 3u, 0x28000000u }, { 0x080u, 3u, 0x20000000u }, { 0x100u, 3u, 0u },
        { 0x0C0u, 3u, 0u },          { 0x180u, 0x3Fu, 0x10000000u },
    };
    const uint32_t* s = kShapes[Rand() % 8u];
    const uint32_t type = s[1] == 0x3Fu ? 0x3Fu : Rand() % (s[1] + 1u);
    return s[0] | type | s[2];
}

static int8_t RandomFeature(int max) {
    return (int8_t) (Rand() % 3u == 0 ? RandRange(0, max) : -1);
}

/* Anything the editor could save: count 4..64, random geometry inside the box, every family. */
static void BuildRandomCourseOf(GdxCsCourse* c, int count) {
    int i;
    memset(c, 0, sizeof(*c));
    c->creatorId = GDXCS_CREATOR_NINTENDO;
    c->controlPointCount = (int8_t) count;
    c->venue = (int8_t) RandRange(0, 9);
    c->skybox = (int8_t) RandRange(0, 7);
    c->bgm = (int8_t) RandRange(0, 13);
    c->flag = 1;
    for (i = 0; i < c->controlPointCount; i++) {
        const int rl = RandRange(50, 1000);
        SetPoint(c, i, (float) RandRange(-14999, 14999) + 0.25f * (float) RandRange(0, 3),
                 (float) RandRange(-250, 5000) * 0.5f + 0.0f, (float) RandRange(-15000, 15000), rl,
                 RandRange(50, 2000 - rl), RandomSegment(), RandRange(0, 360));
        c->pit[i] = RandomFeature(3);
        c->dash[i] = RandomFeature(2);
        c->dirt[i] = RandomFeature(3);
        c->ice[i] = RandomFeature(3);
        c->jump[i] = RandomFeature(2);
        c->landmine[i] = RandomFeature(2);
        c->gate[i] = RandomFeature(2);
        c->building[i] = RandomFeature(14);
        c->sign[i] = RandomFeature(4);
    }
    c->checksum = gdxcs_course_checksum(c);
}

static void BuildRandomCourse(GdxCsCourse* c) {
    BuildRandomCourseOf(c, RandRange(4, 64));
}

static size_t EncodeOrDie(const GdxCsCourse* c, const char* name, uint8_t* out) {
    size_t length = 0;
    const GdxCsStatus s = gdxcs_encode_file(c, name, &kDisk, out, GDXCS_FILE_BYTES, &length);
    CHECK_STATUS(s, GDXCS_OK);
    CHECK(length == GDXCS_FILE_BYTES);
    return length;
}

static void PutBe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

/* After a deliberate edit: recompute both CRCs so the decoder's deeper rules are what decide. */
static void FixCrcs(uint8_t* file) {
    PutBe32(file + 0x10, gdxcs_crc32(file + GDXCS_HEADER_BYTES, GDXCS_COURSE_BYTES));
    PutBe32(file + 0x3C, gdxcs_crc32(file, 0x3C));
}

/* After a payload edit: also refresh the header summary and the course checksum fields, so only
 * the course rule under test can object. */
static void ResealPayload(uint8_t* file, int refreshChecksum) {
    GdxCsCourse c;
    gdxcs_course_from_be(file + GDXCS_HEADER_BYTES, &c);
    if (refreshChecksum) {
        c.checksum = gdxcs_course_checksum(&c);
        gdxcs_course_to_be(&c, file + GDXCS_HEADER_BYTES);
    }
    file[0x1D] = c.flag;
    file[0x30] = (uint8_t) c.controlPointCount;
    file[0x31] = (uint8_t) c.venue;
    file[0x32] = (uint8_t) c.skybox;
    file[0x33] = (uint8_t) c.bgm;
    PutBe32(file + 0x34, c.checksum);
    FixCrcs(file);
}

static GdxCsStatus Decode(const uint8_t* file, size_t length) {
    GdxCsCourse c;
    char name[GDXCS_NAME_MAX + 1];
    return gdxcs_decode_file(file, length, &kDisk, &c, name);
}

/* ---- tests ---------------------------------------------------------------------------- */

static void TestGoldenChecksum(void) {
    GdxCsCourse c;
    BuildGoldenCourse(&c);
    CHECK(c.checksum == GOLDEN_CHECKSUM);
    CHECK_STATUS(gdxcs_validate_course(&c, NULL), GDXCS_OK);
    /* The JOIN/FORM/CONTINUOUS bits do not count (the game clears them before summing)... */
    c.controlPoint[1].trackSegmentInfo |= 0x40038600;
    CHECK(gdxcs_course_checksum(&c) == GOLDEN_CHECKSUM);
    /* ...but a live point carrying them is not something the editor saves. */
    CHECK_STATUS(gdxcs_validate_course(&c, NULL), GDXCS_ERR_SEGMENT);
}

static void TestCrcsAndDiskId(void) {
    static const uint8_t kGame[4] = { 'D', 'F', 'Z', 'J' };
    static const uint8_t kCompany[2] = { '0', '1' };
    GdxCsDiskId a;
    GdxCsDiskId b;
    CHECK(gdxcs_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(gdxcs_crc64("123456789", 9) == 0x995DC9BBDF1939FAull);
    a = gdxcs_disk_id_from_boot(kGame, 0, 0, kCompany);
    b = gdxcs_disk_id_from_boot(kGame, 1, 0, kCompany);
    CHECK(a.region == 'J');
    CHECK(a.crc64 != b.crc64);
    CHECK(a.crc64 == gdxcs_crc64("DFZJ\0\0" "01", 8));
}

static void TestRoundTrip(void) {
    GdxCsCourse c;
    GdxCsCourse back;
    uint8_t file[GDXCS_FILE_BYTES];
    uint8_t be[GDXCS_COURSE_BYTES];
    char name[GDXCS_NAME_MAX + 1];
    BuildGoldenCourse(&c);
    EncodeOrDie(&c, "MY,CRS&1", file);
    CHECK(memcmp(file, "GDXCRS\x1A\n", 8) == 0);
    CHECK_STATUS(gdxcs_decode_file(file, sizeof(file), &kDisk, &back, name), GDXCS_OK);
    CHECK(memcmp(&c, &back, sizeof(c)) == 0);
    CHECK(strcmp(name, "MY,CRS&1") == 0);
    CHECK_STATUS(gdxcs_decode_file(file, sizeof(file), NULL, &back, name), GDXCS_OK);
    gdxcs_course_to_be(&c, be);
    gdxcs_course_from_be(be, &back);
    CHECK(memcmp(&c, &back, sizeof(c)) == 0);
    CHECK(be[1] == 4 && be[0x1F] == 13);
}

static void TestRandomRoundTrips(void) {
    int n;
    for (n = 0; n < 300; n++) {
        GdxCsCourse c;
        GdxCsCourse back;
        uint8_t file[GDXCS_FILE_BYTES];
        char name[GDXCS_NAME_MAX + 1];
        BuildRandomCourse(&c);
        CHECK_STATUS(gdxcs_validate_course(&c, NULL), GDXCS_OK);
        EncodeOrDie(&c, "RANDOM", file);
        CHECK_STATUS(gdxcs_decode_file(file, sizeof(file), &kDisk, &back, name), GDXCS_OK);
        CHECK(memcmp(&c, &back, sizeof(c)) == 0);
    }
}

static void TestHeaderRefusals(void) {
    GdxCsCourse c;
    uint8_t good[GDXCS_FILE_BYTES + 16];
    uint8_t f[GDXCS_FILE_BYTES + 16];
    GdxCsDiskId other = kDisk;
    GdxCsCourse out;
    char name[GDXCS_NAME_MAX + 1];
    BuildGoldenCourse(&c);
    EncodeOrDie(&c, "SHARE", good);

    CHECK_STATUS(Decode(good, GDXCS_FILE_BYTES - 1), GDXCS_ERR_SIZE);
    CHECK_STATUS(Decode(good, GDXCS_FILE_BYTES + 1), GDXCS_ERR_SIZE);
    CHECK_STATUS(Decode(good, 0), GDXCS_ERR_SIZE);
    CHECK_STATUS(gdxcs_decode_file(NULL, GDXCS_FILE_BYTES, &kDisk, &out, name), GDXCS_ERR_ARG);

#define MUTATE(off, val, fix, want)                  \
    do {                                             \
        memcpy(f, good, GDXCS_FILE_BYTES);           \
        f[(off)] = (uint8_t) (val);                  \
        if (fix) {                                   \
            FixCrcs(f);                              \
        }                                            \
        CHECK_STATUS(Decode(f, GDXCS_FILE_BYTES), (want)); \
    } while (0)

    MUTATE(0, 'X', 1, GDXCS_ERR_MAGIC);
    MUTATE(7, '\r', 1, GDXCS_ERR_MAGIC);
    MUTATE(9, 2, 1, GDXCS_ERR_VERSION);
    MUTATE(8, 1, 1, GDXCS_ERR_VERSION);
    MUTATE(0x0B, 0x41, 1, GDXCS_ERR_LAYOUT);
    MUTATE(0x0F, 0xE1, 1, GDXCS_ERR_LAYOUT);
    MUTATE(0x0E, 0xFF, 1, GDXCS_ERR_LAYOUT);
    MUTATE(0x20, 'T', 0, GDXCS_ERR_HEADER_CRC);
    MUTATE(0x3C, 0, 0, GDXCS_ERR_HEADER_CRC);
    MUTATE(0x100, 0x55, 0, GDXCS_ERR_PAYLOAD_CRC);
    memcpy(f, good, GDXCS_FILE_BYTES);
    f[0x10] ^= 0x55;
    PutBe32(f + 0x3C, gdxcs_crc32(f, 0x3C)); /* header intact, its payload CRC field now wrong */
    CHECK_STATUS(Decode(f, GDXCS_FILE_BYTES), GDXCS_ERR_PAYLOAD_CRC);
    MUTATE(0x1F, 1, 1, GDXCS_ERR_RESERVED);
    MUTATE(0x3A, 1, 1, GDXCS_ERR_RESERVED);
    MUTATE(0x1C, 'E', 1, GDXCS_ERR_REGION);
    MUTATE(0x14, 0x00, 1, GDXCS_ERR_DISK);
    MUTATE(0x1E, 0, 1, GDXCS_ERR_NAME);
    MUTATE(0x1E, 9, 1, GDXCS_ERR_NAME);
    MUTATE(0x1E, 0xFF, 1, GDXCS_ERR_NAME);
    MUTATE(0x1E, 6, 1, GDXCS_ERR_NAME);       /* "SHARE" + a NUL inside the declared length */
    MUTATE(0x2F, 'A', 1, GDXCS_ERR_RESERVED); /* name padding */
    MUTATE(0x21, 'h', 1, GDXCS_ERR_NAME);     /* lower case is not on the keyboard */
    MUTATE(0x20, ' ', 1, GDXCS_ERR_NAME);     /* leading space */
    MUTATE(0x1D, 0, 1, GDXCS_ERR_KIND);
    MUTATE(0x30, 5, 1, GDXCS_ERR_SUMMARY);
    MUTATE(0x31, 4, 1, GDXCS_ERR_SUMMARY);
    MUTATE(0x32, 4, 1, GDXCS_ERR_SUMMARY);
    MUTATE(0x33, 1, 1, GDXCS_ERR_SUMMARY);
    MUTATE(0x37, 0, 1, GDXCS_ERR_SUMMARY);

    other.crc64 ^= 1;
    CHECK_STATUS(gdxcs_decode_file(good, GDXCS_FILE_BYTES, &other, &out, name), GDXCS_ERR_DISK);
    other = kDisk;
    other.region = 'E';
    CHECK_STATUS(gdxcs_decode_file(good, GDXCS_FILE_BYTES, &other, &out, name), GDXCS_ERR_REGION);
#undef MUTATE
}

/* Edits the decoded course, re-encodes the payload, reseals the file, expects `want`. */
typedef void (*CourseEdit)(GdxCsCourse* c);

static void ExpectPayloadRefusal(CourseEdit edit, int refreshChecksum, GdxCsStatus want, int line) {
    GdxCsCourse c;
    uint8_t f[GDXCS_FILE_BYTES];
    GdxCsStatus got;
    BuildGoldenCourse(&c);
    EncodeOrDie(&c, "SHARE", f);
    edit(&c);
    gdxcs_course_to_be(&c, f + GDXCS_HEADER_BYTES);
    ResealPayload(f, refreshChecksum);
    got = Decode(f, sizeof(f));
    sChecks++;
    if (got != want) {
        sFailures++;
        fprintf(stderr, "FAIL payload edit at line %d: got %s want %s\n", line, gdxcs_status_name(got),
                gdxcs_status_name(want));
    }
}

#define EDIT(body)                         \
    static void Edit_##body(GdxCsCourse* c)

EDIT(creator) { c->creatorId = 3; }
EDIT(count_zero) { c->controlPointCount = 0; }
EDIT(count_three) { c->controlPointCount = 3; }
EDIT(count_neg) { c->controlPointCount = -1; }
EDIT(count_65) { c->controlPointCount = 65; }
EDIT(venue) { c->venue = 10; }
EDIT(venue_neg) { c->venue = -1; }
EDIT(skybox) { c->skybox = 8; }
EDIT(bgm) { c->bgm = 14; }
EDIT(bgm_neg) { c->bgm = -1; }
EDIT(flag) { c->flag = 2; }
EDIT(nan) {
    uint32_t bits = 0x7FC00000u;
    memcpy(&c->controlPoint[2].x, &bits, 4);
}
EDIT(inf) {
    uint32_t bits = 0xFF800000u;
    memcpy(&c->controlPoint[0].z, &bits, 4);
}
EDIT(denormal) {
    uint32_t bits = 0x00000001u;
    memcpy(&c->controlPoint[1].y, &bits, 4);
}
EDIT(neg_zero) {
    uint32_t bits = 0x80000000u;
    memcpy(&c->controlPoint[0].y, &bits, 4);
}
EDIT(x_far) { c->controlPoint[1].x = 15000.5f; }
EDIT(y_low) { c->controlPoint[1].y = -250.5f; }
EDIT(y_high) { c->controlPoint[1].y = 5000.5f; }
EDIT(radius_small) { c->controlPoint[0].radiusLeft = 49; }
EDIT(radius_neg) { c->controlPoint[0].radiusRight = -300; }
EDIT(radius_sum) {
    c->controlPoint[0].radiusLeft = 1000;
    c->controlPoint[0].radiusRight = 1001;
}
EDIT(segment_shape_flags) { c->controlPoint[0].trackSegmentInfo = 0x08000000; } /* ROAD without JOINABLE */
EDIT(segment_type) { c->controlPoint[0].trackSegmentInfo = 0x18000007; }       /* ROAD type 7 */
EDIT(segment_air_type) { c->controlPoint[3].trackSegmentInfo = 0x10000180; }   /* AIR with a type */
EDIT(segment_high_bit) { c->controlPoint[1].trackSegmentInfo |= (int32_t) 0x80000000u; }
EDIT(segment_join) { c->controlPoint[1].trackSegmentInfo |= 0x200; }
EDIT(bank_neg) { c->bankAngle[0] = -1; }
EDIT(bank_high) { c->bankAngle[0] = 361; }
EDIT(feature_pit) { c->pit[0] = 4; }
EDIT(feature_sign) { c->sign[0] = 5; }
EDIT(feature_building) { c->building[3] = 15; }
EDIT(feature_low) { c->gate[2] = -2; }
EDIT(tail_garbage) { c->controlPoint[10].radiusLeft = 7; }
EDIT(tail_feature) { c->dash[63] = 9; }
EDIT(tail_bank) { c->bankAngle[40] = 400; }
EDIT(tail_nan) {
    uint32_t bits = 0x7F800001u;
    memcpy(&c->controlPoint[5].x, &bits, 4);
}
EDIT(checksum_only) { c->checksum ^= 1; }
EDIT(geometry_only) { c->controlPoint[2].x += 1.0f; }
EDIT(feature_only) { c->sign[1] = 0; }
/* Accepted: a leftover point of a longer course saved earlier (valid, runtime bits set), and the
 * unfinished (.CRSE) kind with fewer than four points. */
EDIT(tail_leftover) {
    c->controlPoint[4] = c->controlPoint[1];
    c->controlPoint[4].trackSegmentInfo |= 0x00018400;
    c->bankAngle[4] = 30;
    c->pit[4] = 2;
}
EDIT(unfinished_short) {
    c->flag = 0;
    c->controlPointCount = 2;
}

static void TestPayloadRefusals(void) {
    ExpectPayloadRefusal(Edit_creator, 1, GDXCS_ERR_CREATOR, __LINE__);
    ExpectPayloadRefusal(Edit_count_zero, 1, GDXCS_ERR_POINT_COUNT, __LINE__);
    ExpectPayloadRefusal(Edit_count_three, 1, GDXCS_ERR_POINT_COUNT, __LINE__);
    ExpectPayloadRefusal(Edit_count_neg, 1, GDXCS_ERR_POINT_COUNT, __LINE__);
    ExpectPayloadRefusal(Edit_count_65, 1, GDXCS_ERR_POINT_COUNT, __LINE__);
    ExpectPayloadRefusal(Edit_venue, 1, GDXCS_ERR_VENUE, __LINE__);
    ExpectPayloadRefusal(Edit_venue_neg, 1, GDXCS_ERR_VENUE, __LINE__);
    ExpectPayloadRefusal(Edit_skybox, 1, GDXCS_ERR_SKYBOX, __LINE__);
    ExpectPayloadRefusal(Edit_bgm, 1, GDXCS_ERR_BGM, __LINE__);
    ExpectPayloadRefusal(Edit_bgm_neg, 1, GDXCS_ERR_BGM, __LINE__);
    ExpectPayloadRefusal(Edit_flag, 1, GDXCS_ERR_FLAG, __LINE__);
    ExpectPayloadRefusal(Edit_nan, 1, GDXCS_ERR_POSITION, __LINE__);
    ExpectPayloadRefusal(Edit_inf, 1, GDXCS_ERR_POSITION, __LINE__);
    ExpectPayloadRefusal(Edit_denormal, 1, GDXCS_ERR_POSITION, __LINE__);
    ExpectPayloadRefusal(Edit_neg_zero, 1, GDXCS_ERR_POSITION, __LINE__);
    ExpectPayloadRefusal(Edit_x_far, 1, GDXCS_ERR_POSITION, __LINE__);
    ExpectPayloadRefusal(Edit_y_low, 1, GDXCS_ERR_POSITION, __LINE__);
    ExpectPayloadRefusal(Edit_y_high, 1, GDXCS_ERR_POSITION, __LINE__);
    ExpectPayloadRefusal(Edit_radius_small, 1, GDXCS_ERR_RADIUS, __LINE__);
    ExpectPayloadRefusal(Edit_radius_neg, 1, GDXCS_ERR_RADIUS, __LINE__);
    ExpectPayloadRefusal(Edit_radius_sum, 1, GDXCS_ERR_RADIUS, __LINE__);
    ExpectPayloadRefusal(Edit_segment_shape_flags, 1, GDXCS_ERR_SEGMENT, __LINE__);
    ExpectPayloadRefusal(Edit_segment_type, 1, GDXCS_ERR_SEGMENT, __LINE__);
    ExpectPayloadRefusal(Edit_segment_air_type, 1, GDXCS_ERR_SEGMENT, __LINE__);
    ExpectPayloadRefusal(Edit_segment_high_bit, 1, GDXCS_ERR_SEGMENT, __LINE__);
    ExpectPayloadRefusal(Edit_segment_join, 1, GDXCS_ERR_SEGMENT, __LINE__);
    ExpectPayloadRefusal(Edit_bank_neg, 1, GDXCS_ERR_BANK, __LINE__);
    ExpectPayloadRefusal(Edit_bank_high, 1, GDXCS_ERR_BANK, __LINE__);
    ExpectPayloadRefusal(Edit_feature_pit, 1, GDXCS_ERR_FEATURE, __LINE__);
    ExpectPayloadRefusal(Edit_feature_sign, 1, GDXCS_ERR_FEATURE, __LINE__);
    ExpectPayloadRefusal(Edit_feature_building, 1, GDXCS_ERR_FEATURE, __LINE__);
    ExpectPayloadRefusal(Edit_feature_low, 1, GDXCS_ERR_FEATURE, __LINE__);
    ExpectPayloadRefusal(Edit_tail_garbage, 1, GDXCS_ERR_TAIL, __LINE__);
    ExpectPayloadRefusal(Edit_tail_feature, 1, GDXCS_ERR_TAIL, __LINE__);
    ExpectPayloadRefusal(Edit_tail_bank, 1, GDXCS_ERR_TAIL, __LINE__);
    ExpectPayloadRefusal(Edit_tail_nan, 1, GDXCS_ERR_TAIL, __LINE__);
    ExpectPayloadRefusal(Edit_checksum_only, 0, GDXCS_ERR_CHECKSUM, __LINE__);
    ExpectPayloadRefusal(Edit_geometry_only, 0, GDXCS_ERR_CHECKSUM, __LINE__);
    ExpectPayloadRefusal(Edit_feature_only, 0, GDXCS_ERR_CHECKSUM, __LINE__);
    ExpectPayloadRefusal(Edit_tail_leftover, 1, GDXCS_OK, __LINE__);
    ExpectPayloadRefusal(Edit_unfinished_short, 1, GDXCS_OK, __LINE__);
}

static void TestEncodeRefusals(void) {
    GdxCsCourse c;
    uint8_t f[GDXCS_FILE_BYTES];
    size_t length = 0;
    BuildGoldenCourse(&c);
    CHECK_STATUS(gdxcs_encode_file(&c, "TOO LONG1", &kDisk, f, sizeof(f), &length), GDXCS_ERR_NAME);
    CHECK_STATUS(gdxcs_encode_file(&c, "", &kDisk, f, sizeof(f), &length), GDXCS_ERR_NAME);
    CHECK_STATUS(gdxcs_encode_file(&c, "OK", &kDisk, f, sizeof(f) - 1, &length), GDXCS_ERR_ARG);
    c.venue = 12;
    CHECK_STATUS(gdxcs_encode_file(&c, "OK", &kDisk, f, sizeof(f), &length), GDXCS_ERR_VENUE);
}

static void TestNames(void) {
    char out[32];
    CHECK_STATUS(gdxcs_validate_name("A"), GDXCS_OK);
    CHECK_STATUS(gdxcs_validate_name("ZZ 99,'&"), GDXCS_OK);
    CHECK_STATUS(gdxcs_validate_name("ABCDEFGHI"), GDXCS_ERR_NAME);
    CHECK_STATUS(gdxcs_validate_name("A "), GDXCS_ERR_NAME);
    CHECK_STATUS(gdxcs_validate_name("A.B"), GDXCS_ERR_NAME);
    CHECK_STATUS(gdxcs_validate_name("A/B"), GDXCS_ERR_NAME);
    CHECK_STATUS(gdxcs_validate_name("\xB1"), GDXCS_ERR_NAME);
    CHECK_STATUS(gdxcs_validate_name(NULL), GDXCS_ERR_ARG);
    gdxcs_sd_filename("MY,CRS&1", out, sizeof(out));
    CHECK(strcmp(out, "my_crs_1.gdxcourse") == 0);
    gdxcs_sd_filename("A B'C", out, sizeof(out));
    CHECK(strcmp(out, "a_b_c.gdxcourse") == 0);
    gdxcs_sd_filename("LONGNAME", out, 8);
    CHECK(strlen(out) == 7);
    CHECK(strcmp(gdxcs_extension_for_flag(1), "CRSD") == 0);
    CHECK(strcmp(gdxcs_extension_for_flag(0), "CRSE") == 0);
}

/* Seeded mutation fuzz. Three flavours: raw flips (must die at a CRC), flips with CRCs fixed
 * (must die somewhere, never OK unless the bytes decode to a course the validator accepts), and
 * payload flips with everything resealed (exercises the course rules on near-valid input). */
static void TestFuzz(void) {
    GdxCsCourse base;
    uint8_t good[GDXCS_FILE_BYTES];
    int n;
    BuildGoldenCourse(&base);
    EncodeOrDie(&base, "FUZZ", good);
    for (n = 0; n < 20000; n++) {
        uint8_t f[GDXCS_FILE_BYTES];
        GdxCsCourse out;
        char name[GDXCS_NAME_MAX + 1] = "unset";
        const int flavour = n % 3;
        const int flips = RandRange(1, 6);
        GdxCsStatus s;
        int k;
        memcpy(f, good, sizeof(f));
        for (k = 0; k < flips; k++) {
            const size_t at = flavour == 2 ? GDXCS_HEADER_BYTES + Rand() % GDXCS_COURSE_BYTES : Rand() % GDXCS_FILE_BYTES;
            f[at] ^= (uint8_t) (1u << (Rand() % 8u));
        }
        if (flavour == 1) {
            FixCrcs(f);
        } else if (flavour == 2) {
            ResealPayload(f, (int) (Rand() & 1u));
        }
        memset(&out, 0xA5, sizeof(out));
        s = gdxcs_decode_file(f, sizeof(f), &kDisk, &out, name);
        if (memcmp(f, good, sizeof(f)) == 0) {
            CHECK_STATUS(s, GDXCS_OK); /* flips that cancelled out */
            continue;
        }
        if (flavour == 0) {
            CHECK(s == GDXCS_ERR_HEADER_CRC || s == GDXCS_ERR_PAYLOAD_CRC || s == GDXCS_ERR_MAGIC ||
                  s == GDXCS_ERR_VERSION || s == GDXCS_ERR_LAYOUT);
        }
        if (s == GDXCS_OK) {
            CHECK_STATUS(gdxcs_validate_course(&out, NULL), GDXCS_OK);
            CHECK_STATUS(gdxcs_validate_name(name), GDXCS_OK);
        } else {
            CHECK(strcmp(name, "unset") == 0); /* nothing written on refusal */
        }
    }
    for (n = 0; n < 2000; n++) {
        uint8_t junk[GDXCS_FILE_BYTES + 64];
        size_t length = (size_t) RandRange(0, (int) sizeof(junk));
        size_t k;
        for (k = 0; k < length; k++) {
            junk[k] = (uint8_t) Rand();
        }
        CHECK(Decode(junk, length) != GDXCS_OK);
    }
}

/* ---- QR ------------------------------------------------------------------------------- */

static void TestPackRoundTrips(void) {
    int n;
    for (n = 0; n < 200; n++) {
        GdxCsCourse c;
        uint8_t file[GDXCS_FILE_BYTES];
        uint8_t packed[GDXCS_QR_PACKED_MAX];
        uint8_t back[GDXCS_FILE_BYTES];
        uint8_t codec = 0xFF;
        size_t length;
        if (n == 0) {
            BuildGoldenCourse(&c);
        } else {
            BuildRandomCourse(&c);
        }
        EncodeOrDie(&c, "PACK", file);
        length = gdxcs_pack(file, packed, sizeof(packed), &codec);
        CHECK(length > 0 && length <= GDXCS_FILE_BYTES);
        CHECK(gdxcs_unpack(packed, length, codec, back) == 0);
        CHECK(memcmp(file, back, sizeof(file)) == 0);
        if (n == 0) {
            CHECK(codec == GDXCS_QR_CODEC_LZSS);
            CHECK(length < 450);
            printf("golden course packs to %zu bytes (%d frames)\n", length,
                   (int) ((length + GDXCS_QR_CHUNK - 1) / GDXCS_QR_CHUNK));
        }
        if (codec == GDXCS_QR_CODEC_LZSS && length > 2) {
            CHECK(gdxcs_unpack(packed, length - 1, codec, back) != 0); /* truncated */
            packed[length] = 0;
            CHECK(gdxcs_unpack(packed, length + 1, codec, back) != 0); /* trailing byte */
        }
    }
}

static void TestUnpackRefusals(void) {
    uint8_t out[GDXCS_FILE_BYTES];
    uint8_t lz[8];
    size_t n;
    /* A reference before anything was produced. */
    lz[0] = 0x01;
    lz[1] = 0x00;
    lz[2] = 0x00;
    CHECK(gdxcs_unpack(lz, 3, GDXCS_QR_CODEC_LZSS, out) != 0);
    /* Unknown codec; raw of the wrong size. */
    CHECK(gdxcs_unpack(lz, 3, 7, out) != 0);
    CHECK(gdxcs_unpack(lz, 3, GDXCS_QR_CODEC_RAW, out) != 0);
    /* Random streams never overrun (run under ASan in development) and almost never parse. */
    for (n = 0; n < 5000; n++) {
        uint8_t junk[600];
        size_t length = (size_t) RandRange(0, (int) sizeof(junk));
        size_t k;
        for (k = 0; k < length; k++) {
            junk[k] = (uint8_t) Rand();
        }
        (void) gdxcs_unpack(junk, length, GDXCS_QR_CODEC_LZSS, out);
    }
}

#define FB_W 320
#define FB_H 240
#define QR_SCALE 3
#define QR_QUIET 4

/* The bottom screen as the console paints it, back to an 8-bit landscape picture for quirc. */
static int DecodeFramebuffer(struct quirc* q, const uint16_t* fb, uint8_t* out, size_t cap, size_t* outLen) {
    int w = 0;
    int h = 0;
    uint8_t* image;
    int x;
    int y;
    struct quirc_code code;
    struct quirc_data data;
    if (quirc_resize(q, FB_W, FB_H) < 0) {
        return -1;
    }
    image = quirc_begin(q, &w, &h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint16_t p = fb[(size_t) x * FB_H + (size_t) (FB_H - 1 - y)];
            image[y * w + x] = p == 0xFFFFu ? 255 : (p == 0 ? 0 : 128);
        }
    }
    quirc_end(q);
    if (quirc_count(q) != 1) {
        return -1;
    }
    quirc_extract(q, 0, &code);
    if (quirc_decode(&code, &data) != QUIRC_SUCCESS || (size_t) data.payload_len > cap) {
        return -1;
    }
    memcpy(out, data.payload, (size_t) data.payload_len);
    *outLen = (size_t) data.payload_len;
    return 0;
}

typedef struct QrLayout {
    int x0;
    int y0;
    int scale;
    int quiet;
} QrLayout;

static const QrLayout kDenseLayout = { 8, 24, QR_SCALE, QR_QUIET }; /* gdx3ds_course_share.c QR_* */
static const QrLayout kLargeLayout = { 2, 24, 5, 1 };               /* gdx3ds_course_share.c QR_LARGE_* */

static void ScanSetOutOfOrder(const GdxCsQrSet* set, const uint8_t* file, struct quirc* q, const QrLayout* layout) {
    static uint16_t fb[FB_W * FB_H];
    static GdxCsQrAssembler a;
    uint8_t qr[GDXCS_QR_BUFFER_BYTES];
    int order[GDXCS_QR_FRAMES_MAX];
    int i;
    GdxCsQrFeed last = GDXCS_QR_FEED_REJECTED;
    for (i = 0; i < set->count; i++) {
        order[i] = set->count - 1 - i; /* last frame first */
    }
    gdxcs_qr_assembler_reset(&a);
    for (i = 0; i < set->count; i++) {
        const int index = order[i];
        uint8_t scanned[GDXCS_QR_FRAME_MAX + 8];
        size_t scannedLen = 0;
        int modules;
        int side;
        memset(fb, 0x84, sizeof(fb)); /* a mid-grey "menu" background around the code */
        modules = gdxcs_qr_encode(set->frame[index], set->frameLength[index], qr);
        CHECK(modules >= 21 && modules <= 57);
        side = gdxcs_qr_blit_rgb565(qr, fb, FB_W, FB_H, layout->x0, layout->y0, layout->scale, layout->quiet);
        CHECK(side == (modules + 2 * layout->quiet) * layout->scale && layout->y0 + side <= FB_H);
        CHECK(layout->x0 + side <= 8 * 28); /* left of the QR panel (column 29) */
        CHECK(DecodeFramebuffer(q, fb, scanned, sizeof(scanned), &scannedLen) == 0);
        CHECK(scannedLen == set->frameLength[index]);
        CHECK(memcmp(scanned, set->frame[index], set->frameLength[index]) == 0);
        last = gdxcs_qr_assembler_feed(&a, scanned, scannedLen);
        if (i == 0 && set->count > 1) {
            CHECK(gdxcs_qr_assembler_feed(&a, scanned, scannedLen) == GDXCS_QR_FEED_DUPLICATE);
        }
    }
    CHECK(last == GDXCS_QR_FEED_COMPLETE);
    CHECK(memcmp(a.file, file, GDXCS_FILE_BYTES) == 0);
    CHECK_STATUS(Decode(a.file, GDXCS_FILE_BYTES), GDXCS_OK);
}

static void TestQrScan(void) {
    struct quirc* q = quirc_new();
    int n;
    int maxFrames = 0;
    int maxLarge = 0;
    CHECK(q != NULL);
    if (q == NULL) {
        return;
    }
    for (n = 0; n < 12; n++) {
        GdxCsCourse c;
        uint8_t file[GDXCS_FILE_BYTES];
        static GdxCsQrSet set;
        int i;
        if (n == 0) {
            BuildGoldenCourse(&c);
        } else {
            BuildRandomCourse(&c);
        }
        EncodeOrDie(&c, "QRCODE", file);
        CHECK(gdxcs_qr_build(file, sizeof(file), &set) == 0);
        CHECK(set.count >= 1 && set.count <= GDXCS_QR_FRAMES_MAX_V1);
        if (set.count > maxFrames) {
            maxFrames = set.count;
        }
        ScanSetOutOfOrder(&set, file, q, &kDenseLayout);
        CHECK(gdxcs_qr_build_sized(file, sizeof(file), GDXCS_QR_CHUNK_LARGE, &set) == 0);
        CHECK(set.count >= 1 && set.count <= GDXCS_QR_FRAMES_MAX);
        for (i = 0; i < set.count; i++) {
            uint8_t qr[GDXCS_QR_BUFFER_BYTES];
            CHECK(set.frame[i][2] == 2 && set.frame[i][10] == GDXCS_QR_CHUNK_LARGE);
            CHECK(gdxcs_qr_encode(set.frame[i], set.frameLength[i], qr) <= 41); /* QR version 6 at most */
        }
        if (set.count > maxLarge) {
            maxLarge = set.count;
        }
        ScanSetOutOfOrder(&set, file, q, &kLargeLayout);
    }
    printf("qr: 12 sets scanned dense and large, largest %d / %d frames\n", maxFrames, maxLarge);
    quirc_destroy(q);
}

static void TestAssemblerRefusals(void) {
    static GdxCsQrSet set;
    static GdxCsQrSet other;
    static GdxCsQrAssembler a;
    GdxCsCourse c;
    uint8_t file[GDXCS_FILE_BYTES];
    uint8_t f[GDXCS_QR_FRAME_MAX];
    int i;
    BuildRandomCourseOf(&c, 64);
    EncodeOrDie(&c, "SETA", file);
    CHECK(gdxcs_qr_build(file, sizeof(file), &set) == 0);
    CHECK(set.count >= 2);
    BuildGoldenCourse(&c);
    EncodeOrDie(&c, "SETB", file);
    CHECK(gdxcs_qr_build(file, sizeof(file), &other) == 0);

    gdxcs_qr_assembler_reset(&a);
    CHECK(gdxcs_qr_assembler_feed(&a, set.frame[0], set.frameLength[0]) == GDXCS_QR_FEED_ACCEPTED);
    /* A different chunk claiming the same slot. */
    memcpy(f, set.frame[0], set.frameLength[0]);
    f[GDXCS_QR_FRAME_HEADER] ^= 1;
    CHECK(gdxcs_qr_assembler_feed(&a, f, set.frameLength[0]) == GDXCS_QR_FEED_REJECTED);
    CHECK(gdxcs_qr_assembler_have(&a) == 1);
    /* Length lies, bad magic, index out of range, count that disagrees with the length. */
    CHECK(gdxcs_qr_assembler_feed(&a, set.frame[0], set.frameLength[0] - 1) == GDXCS_QR_FEED_REJECTED);
    memcpy(f, set.frame[1], set.frameLength[1]);
    f[0] = 'Q';
    CHECK(gdxcs_qr_assembler_feed(&a, f, set.frameLength[1]) == GDXCS_QR_FEED_REJECTED);
    memcpy(f, set.frame[1], set.frameLength[1]);
    f[3] = (uint8_t) set.count;
    CHECK(gdxcs_qr_assembler_feed(&a, f, set.frameLength[1]) == GDXCS_QR_FEED_REJECTED);
    memcpy(f, set.frame[1], set.frameLength[1]);
    f[4] = (uint8_t) (set.count + 1);
    CHECK(gdxcs_qr_assembler_feed(&a, f, set.frameLength[1]) == GDXCS_QR_FEED_REJECTED);
    CHECK(gdxcs_qr_assembler_feed(&a, NULL, 12) == GDXCS_QR_FEED_REJECTED);
    /* A frame of another set restarts. */
    CHECK(gdxcs_qr_assembler_feed(&a, other.frame[0], other.frameLength[0]) ==
          (other.count == 1 ? GDXCS_QR_FEED_COMPLETE : GDXCS_QR_FEED_NEW_SET));
    /* A forged set id: whole, but the CRC of the stream disagrees. */
    gdxcs_qr_assembler_reset(&a);
    for (i = 0; i < set.count; i++) {
        GdxCsQrFeed r;
        memcpy(f, set.frame[i], set.frameLength[i]);
        f[6] ^= 0x01;
        r = gdxcs_qr_assembler_feed(&a, f, set.frameLength[i]);
        CHECK(r == (i + 1 == set.count ? GDXCS_QR_FEED_REJECTED : GDXCS_QR_FEED_ACCEPTED));
    }
    CHECK(gdxcs_qr_assembler_have(&a) == 0);

    /* Version 2 (LARGE): the chunk size is part of the set; nonsense sizes are refused. */
    {
        static GdxCsQrSet large;
        BuildRandomCourseOf(&c, 64);
        EncodeOrDie(&c, "SETC", file);
        CHECK(gdxcs_qr_build_sized(file, sizeof(file), GDXCS_QR_CHUNK_LARGE, &large) == 0);
        CHECK(gdxcs_qr_build(file, sizeof(file), &set) == 0);
        CHECK(large.count > set.count);
        CHECK(gdxcs_qr_build_sized(file, sizeof(file), GDXCS_QR_CHUNK_MIN - 1, &other) == -1);
        CHECK(gdxcs_qr_build_sized(file, sizeof(file), GDXCS_QR_CHUNK + 1, &other) == -1);
        CHECK(gdxcs_qr_build_sized(file, sizeof(file), GDXCS_QR_CHUNK_MIN, &other) == -1); /* > 32 frames */
        gdxcs_qr_assembler_reset(&a);
        CHECK(gdxcs_qr_assembler_feed(&a, large.frame[1], large.frameLength[1]) == GDXCS_QR_FEED_ACCEPTED);
        memcpy(f, large.frame[0], large.frameLength[0]);
        f[10] = GDXCS_QR_CHUNK; /* a 200-byte set must be written as version 1 */
        CHECK(gdxcs_qr_assembler_feed(&a, f, large.frameLength[0]) == GDXCS_QR_FEED_REJECTED);
        f[10] = GDXCS_QR_CHUNK_MIN - 1;
        CHECK(gdxcs_qr_assembler_feed(&a, f, large.frameLength[0]) == GDXCS_QR_FEED_REJECTED);
        f[10] = GDXCS_QR_CHUNK_LARGE + 1; /* count no longer matches */
        CHECK(gdxcs_qr_assembler_feed(&a, f, large.frameLength[0]) == GDXCS_QR_FEED_REJECTED);
        CHECK(gdxcs_qr_assembler_feed(&a, large.frame[0], GDXCS_QR_FRAME_HEADER) == GDXCS_QR_FEED_REJECTED);
        f[10] = GDXCS_QR_CHUNK_LARGE;
        f[2] = 3;
        CHECK(gdxcs_qr_assembler_feed(&a, f, large.frameLength[0]) == GDXCS_QR_FEED_REJECTED);
        CHECK(gdxcs_qr_assembler_have(&a) == 1);
        /* The same course in the other page size is another set. */
        CHECK(gdxcs_qr_assembler_feed(&a, set.frame[0], set.frameLength[0]) == GDXCS_QR_FEED_NEW_SET);
        gdxcs_qr_assembler_reset(&a);
        for (i = large.count - 1; i >= 0; i--) {
            const GdxCsQrFeed r = gdxcs_qr_assembler_feed(&a, large.frame[i], large.frameLength[i]);
            CHECK(r == (i == 0 ? GDXCS_QR_FEED_COMPLETE : GDXCS_QR_FEED_ACCEPTED));
        }
        CHECK(memcmp(a.file, file, GDXCS_FILE_BYTES) == 0);
    }
}

/* ---- scan pipeline: synthetic camera frames ---------------------------------------------- */

#define CAM_W GDXCS_SCAN_W
#define CAM_H GDXCS_SCAN_H

typedef struct Scene {
    double angleDeg;
    double modulePx;  /* camera pixels per QR module */
    double cx;
    double cy;
    double keystone;  /* perspective: scale change per frame height, a screen tilted away */
    int mirror;
    double gainLeft;  /* lighting: brightness factor at the left edge ... */
    double gainRight; /* ... and at the right */
    int noise;        /* +- amplitude of triangular noise, 8-bit units */
    int blur;         /* 3x3 box passes */
    int phase;        /* YUV byte order */
    uint8_t background;
} Scene;

static Scene DefaultScene(void) {
    Scene sc;
    memset(&sc, 0, sizeof(sc));
    sc.modulePx = 3.0;
    sc.cx = CAM_W / 2.0;
    sc.cy = CAM_H / 2.0;
    sc.gainLeft = 1.0;
    sc.gainRight = 1.0;
    sc.phase = 1;
    sc.background = 70; /* a dark console bezel around the screen */
    return sc;
}

/* Module brightness at code coordinates (mx, my): dark modules, the light quiet zone, then the
 * background. A screen's black is not black nor its white white. */
static double SampleCode(const uint8_t* qrcode, int modules, double mx, double my, uint8_t background) {
    const int quiet = QR_QUIET;
    if (mx < -quiet || my < -quiet || mx >= modules + quiet || my >= modules + quiet) {
        return background;
    }
    if (mx >= 0 && my >= 0 && mx < modules && my < modules && gdxcs_qr_module(qrcode, (int) mx, (int) my)) {
        return 28.0;
    }
    return 228.0;
}

static uint32_t sNoiseRng = 0xC0FFEE11u;

static int NoiseSample(int amplitude) {
    int a;
    int b;
    if (amplitude <= 0) {
        return 0;
    }
    sNoiseRng ^= sNoiseRng << 13;
    sNoiseRng ^= sNoiseRng >> 17;
    sNoiseRng ^= sNoiseRng << 5;
    a = (int) (sNoiseRng % (uint32_t) (2 * amplitude + 1)) - amplitude;
    b = (int) ((sNoiseRng >> 16) % (uint32_t) (2 * amplitude + 1)) - amplitude;
    return (a + b) / 2;
}

static void BoxBlur(double* img) {
    static double tmp[CAM_W * CAM_H];
    int x;
    int y;
    memcpy(tmp, img, sizeof(tmp));
    for (y = 1; y < CAM_H - 1; y++) {
        for (x = 1; x < CAM_W - 1; x++) {
            double sum = 0;
            int dx;
            int dy;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    sum += tmp[(y + dy) * CAM_W + (x + dx)];
                }
            }
            img[y * CAM_W + x] = sum / 9.0;
        }
    }
}

/* The scene as the sensor would deliver it: YUV422, CAM_W x CAM_H. */
static void RenderScene(const uint8_t* qrcode, int modules, const Scene* sc, uint8_t* yuv) {
    static double lum[CAM_W * CAM_H];
    static uint8_t rgb[CAM_W * 3];
    const double rad = sc->angleDeg * 3.14159265358979 / 180.0;
    const double cs = cos(rad);
    const double sn = sin(rad);
    int x;
    int y;
    int pass;
    for (y = 0; y < CAM_H; y++) {
        for (x = 0; x < CAM_W; x++) {
            double sum = 0;
            int sy;
            int sx;
            for (sy = 0; sy < 3; sy++) { /* 3x3 supersampling: the lens's own softness */
                for (sx = 0; sx < 3; sx++) {
                    const double dx = x + (sx + 0.5) / 3.0 - sc->cx;
                    const double dy = y + (sy + 0.5) / 3.0 - sc->cy;
                    /* one projective divisor for both axes: a plane seen at an angle */
                    const double scale = sc->modulePx * (1.0 + sc->keystone * dy / CAM_H);
                    double u = (dx * cs + dy * sn) / scale + modules / 2.0;
                    const double v = (-dx * sn + dy * cs) / scale + modules / 2.0;
                    if (sc->mirror) {
                        u = modules - u;
                    }
                    sum += SampleCode(qrcode, modules, u, v, sc->background);
                }
            }
            lum[y * CAM_W + x] = sum / 9.0;
        }
    }
    for (pass = 0; pass < sc->blur; pass++) {
        BoxBlur(lum);
    }
    for (y = 0; y < CAM_H; y++) {
        for (x = 0; x < CAM_W; x++) {
            const double gain = sc->gainLeft + (sc->gainRight - sc->gainLeft) * x / (CAM_W - 1);
            int v = (int) (lum[y * CAM_W + x] * gain + 0.5) + NoiseSample(sc->noise);
            v = v < 0 ? 0 : (v > 255 ? 255 : v);
            rgb[x * 3 + 0] = (uint8_t) v;               /* a warm tint, so U/V are not flat */
            rgb[x * 3 + 1] = (uint8_t) (v * 15 / 16);
            rgb[x * 3 + 2] = (uint8_t) (v * 13 / 16);
        }
        gdxcs_scan_rgb_row_to_yuv(rgb, CAM_W, sc->phase, yuv + (size_t) y * CAM_W * 2u);
    }
}

/* One camera frame through the console's path. Returns the number of codes decoded. */
static int ScanYuv(GdxCsScanDecoder* d, const uint8_t* yuv, GdxCsScanResult* r) {
    const int phase = gdxcs_scan_luma_phase(yuv, CAM_W, CAM_H);
    gdxcs_scan_yuv_to_grey(yuv, CAM_W, CAM_H, phase, gdxcs_scan_decoder_image(d));
    return gdxcs_scan_decoder_run(d, r);
}

/* The scene held in front of the camera for up to `frames` consecutive frames, each rendered
 * afresh -- new sensor noise, and the hand's sub-pixel shake moving the code by a fraction of a
 * pixel -- while the decoder alternates its threshold mode, as it does on the console while
 * nothing decodes. Returns the 1-based frame that read `want`, 0 if none did. */
static int ScanSceneFrames(GdxCsScanDecoder* d, const uint8_t* qrcode, int modules, const Scene* sc,
                           const uint8_t* want, size_t wantLen, int frames, GdxCsScanResult* r) {
    static uint8_t yuv[GDXCS_SCAN_YUV_BYTES];
    int f;
    for (f = 0; f < frames; f++) {
        Scene shaken = *sc;
        shaken.cx += 0.37 * f;
        shaken.cy += 0.23 * f;
        RenderScene(qrcode, modules, &shaken, yuv);
        CHECK(gdxcs_scan_luma_phase(yuv, CAM_W, CAM_H) == sc->phase);
        if (ScanYuv(d, yuv, r) > 0) {
            if (r->code[0].length != wantLen || memcmp(r->code[0].payload, want, wantLen) != 0) {
                return 0;
            }
            return f + 1;
        }
    }
    return 0;
}

/* Two frames: enough for the decoder to try both threshold modes. */
static int ScanScene(GdxCsScanDecoder* d, const uint8_t* qrcode, int modules, const Scene* sc,
                     const uint8_t* want, size_t wantLen, GdxCsScanResult* r) {
    return ScanSceneFrames(d, qrcode, modules, sc, want, wantLen, 2, r);
}

static int EncodeFrame(const GdxCsQrSet* set, int index, uint8_t* qrcode) {
    const int modules = gdxcs_qr_encode(set->frame[index], set->frameLength[index], qrcode);
    CHECK(modules >= 21);
    return modules;
}

static void BuildSetOf(int points, const char* name, uint8_t* file, GdxCsQrSet* set) {
    GdxCsCourse c;
    BuildRandomCourseOf(&c, points);
    EncodeOrDie(&c, name, file);
    CHECK(gdxcs_qr_build(file, GDXCS_FILE_BYTES, set) == 0);
}

static void TestScanConversions(void) {
    static uint8_t yuv[GDXCS_SCAN_YUV_BYTES];
    static uint8_t grey[CAM_W * CAM_H];
    uint8_t rgb[6] = { 255, 255, 255, 0, 0, 0 };
    uint8_t pair[4];
    int phase;
    gdxcs_scan_rgb_row_to_yuv(rgb, 2, 0, pair);
    CHECK(pair[0] == 255 && pair[2] == 0 && pair[1] >= 120 && pair[1] <= 136 && pair[3] >= 120 && pair[3] <= 136);
    gdxcs_scan_rgb_row_to_yuv(rgb, 2, 1, pair);
    CHECK(pair[1] == 255 && pair[3] == 0);
    /* A flat frame has no luma to find: the camera service's order. */
    memset(yuv, 0x80, sizeof(yuv));
    CHECK(gdxcs_scan_luma_phase(yuv, CAM_W, CAM_H) == 1);
    CHECK(gdxcs_scan_luma_phase(NULL, CAM_W, CAM_H) == 1);
    for (phase = 0; phase <= 1; phase++) {
        int i;
        for (i = 0; i < CAM_W * CAM_H; i++) {
            yuv[i * 2 + phase] = (uint8_t) ((i * 37) ^ (i >> 5));
            yuv[i * 2 + (1 - phase)] = (uint8_t) (126 + (i / CAM_W) % 3);
        }
        CHECK(gdxcs_scan_luma_phase(yuv, CAM_W, CAM_H) == phase);
        gdxcs_scan_yuv_to_grey(yuv, CAM_W, CAM_H, phase, grey);
        CHECK(grey[0] == yuv[phase] && grey[CAM_W * CAM_H - 1] == yuv[(CAM_W * CAM_H - 1) * 2 + phase]);
    }
    /* The local threshold: two-valued output, any size it accepts, nothing outside it touched. */
    {
        static uint8_t img[640 * 480 + 16];
        int w;
        memset(img, 0x5A, sizeof(img));
        for (w = 1; w <= 640; w += 97) {
            const int h = 1 + (w * 7) % 480;
            int i;
            int twoValued = 1;
            for (i = 0; i < w * h; i++) {
                img[i] = (uint8_t) (i * 13);
            }
            img[w * h] = 0x5A; /* the byte past the picture stays untouched */
            gdxcs_scan_binarize_local(img, w, h);
            for (i = 0; i < w * h; i++) {
                twoValued &= img[i] == 0 || img[i] == 255;
            }
            CHECK(twoValued);
            CHECK(img[w * h] == 0x5A);
        }
        memset(img, 0x5A, sizeof(img));
        gdxcs_scan_binarize_local(img, 641, 10); /* refused: too wide */
        CHECK(img[0] == 0x5A);
        gdxcs_scan_binarize_local(img, 640, 480);
        CHECK(img[640 * 480] == 0x5A);
        CHECK(img[0] == 255); /* uniform: nothing is darker than its neighbourhood */
    }
}

/* Every page of a course, straight on at the default zoom, both byte orders. */
static void TestScanStraight(GdxCsScanDecoder* d) {
    static GdxCsQrSet set;
    uint8_t file[GDXCS_FILE_BYTES];
    uint8_t qrcode[GDXCS_QR_BUFFER_BYTES];
    GdxCsScanResult r;
    int i;
    BuildSetOf(64, "STRAIGHT", file, &set);
    for (i = 0; i < set.count; i++) {
        const int modules = EncodeFrame(&set, i, qrcode);
        Scene sc = DefaultScene();
        sc.phase = i & 1;
        sc.modulePx = 3.4; /* a DENSE page filling most of the 2x-zoom frame */
        CHECK(ScanSceneFrames(d, qrcode, modules, &sc, set.frame[i], set.frameLength[i], 4, &r) > 0);
        CHECK(r.found == 1 && r.decoded == 1 && r.code[0].mirrored == 0);
    }
}

/* LARGE pages at any rotation in 15-degree steps, mirror on and off, three distances; DENSE pages
 * near the four upright orientations (a 57-module code at 3+ px/module does not fit a 240-row
 * frame turned 45 degrees). */
static void TestScanGeometry(GdxCsScanDecoder* d) {
    static GdxCsQrSet large;
    static GdxCsQrSet dense;
    uint8_t file[GDXCS_FILE_BYTES];
    uint8_t qrLarge[GDXCS_QR_BUFFER_BYTES];
    uint8_t qrDense[GDXCS_QR_BUFFER_BYTES];
    static const double kLargePx[] = { 3.0, 3.5, 4.0 };
    static const double kDensePx[] = { 3.3, 3.6 };
    GdxCsScanResult r;
    int modulesLarge;
    int modulesDense;
    int z;
    int ok = 0;
    int total = 0;
    int mirroredOk = 0;
    BuildSetOf(64, "ROTATE", file, &dense);
    CHECK(gdxcs_qr_build_sized(file, sizeof(file), GDXCS_QR_CHUNK_LARGE, &large) == 0);
    modulesLarge = EncodeFrame(&large, 0, qrLarge);
    modulesDense = EncodeFrame(&dense, 0, qrDense); /* a full 210-byte page: version 10, 57 modules */
    CHECK(modulesLarge == 41 && modulesDense == 57);
    for (z = 0; z < 3; z++) {
        int angle;
        for (angle = 0; angle < 360; angle += 15) {
            int mirror;
            for (mirror = 0; mirror <= 1; mirror++) {
                Scene sc = DefaultScene();
                sc.angleDeg = angle + 2.5 * z;
                sc.modulePx = kLargePx[z];
                sc.mirror = mirror;
                sc.cx += (angle % 7) - 3;
                total++;
                if (ScanScene(d, qrLarge, modulesLarge, &sc, large.frame[0], large.frameLength[0], &r)) {
                    ok++;
                    CHECK(r.code[0].mirrored == mirror);
                    mirroredOk += mirror;
                } else {
                    fprintf(stderr, "scan geometry: LARGE no read at %.1f deg, %.1f px/module, mirror=%d\n",
                            sc.angleDeg, sc.modulePx, mirror);
                }
            }
        }
    }
    for (z = 0; z < 2; z++) {
        int quarter;
        for (quarter = 0; quarter < 4; quarter++) {
            int tilt;
            for (tilt = -6; tilt <= 6; tilt += 3) {
                Scene sc = DefaultScene();
                sc.angleDeg = 90.0 * quarter + tilt + 0.7;
                sc.modulePx = kDensePx[z];
                sc.mirror = quarter & 1;
                total++;
                if (ScanSceneFrames(d, qrDense, modulesDense, &sc, dense.frame[0], dense.frameLength[0], 4, &r)) {
                    ok++;
                } else {
                    fprintf(stderr, "scan geometry: DENSE no read at %.1f deg, %.1f px/module\n", sc.angleDeg,
                            sc.modulePx);
                }
            }
        }
    }
    CHECK(ok == total);
    printf("scan geometry: %d/%d scenes read (%d mirrored): LARGE any angle 3-4 px/module, DENSE upright\n", ok,
           total, mirroredOk);
}

/* A tilted, dim, unevenly lit, blurred and noisy frame: what a phone photo of a screen in a dim
 * room gives. The local threshold has to win where the global one cannot. */
static void TestScanPoorLight(GdxCsScanDecoder* d) {
    static GdxCsQrSet set;
    uint8_t file[GDXCS_FILE_BYTES];
    uint8_t qrcode[GDXCS_QR_BUFFER_BYTES];
    GdxCsScanResult r;
    int modules;
    enum { kLightFrames = 6 }; /* well under a second of camera */
    int localOnly = 0;
    int cases = 0;
    int ok = 0;
    int k;
    {
        GdxCsCourse c;
        BuildRandomCourseOf(&c, 40);
        EncodeOrDie(&c, "DIMROOM", file);
        CHECK(gdxcs_qr_build_sized(file, sizeof(file), GDXCS_QR_CHUNK_LARGE, &set) == 0);
    }
    modules = EncodeFrame(&set, 0, qrcode);
    for (k = 0; k < 12; k++) {
        Scene sc = DefaultScene();
        int got;
        sc.angleDeg = 7.0 * k;
        sc.modulePx = 4.0;
        sc.keystone = (k % 3) * 0.08;
        sc.gainLeft = 0.18 + 0.02 * (k % 4); /* the dim side: ~5..50 on 8 bits */
        sc.gainRight = k < 6 ? 0.55 : 0.9;   /* lit side */
        sc.noise = 4 + (k % 4) * 2;
        sc.blur = 1 + (k % 2);
        sc.phase = k & 1;
        sc.background = (uint8_t) (k % 2 ? 20 : 200);
        got = ScanSceneFrames(d, qrcode, modules, &sc, set.frame[0], set.frameLength[0], kLightFrames, &r);
        cases++;
        if (got) {
            ok++;
            localOnly += r.mode == GDXCS_SCAN_MODE_LOCAL;
        } else {
            fprintf(stderr, "scan light: no read for case %d (gain %.2f..%.2f noise %d blur %d)\n", k, sc.gainLeft,
                    sc.gainRight, sc.noise, sc.blur);
        }
    }
    CHECK(ok == cases);
    CHECK(localOnly > 0); /* the gradient really does defeat the global threshold somewhere */
    printf("scan light: %d/%d dim/gradient/noisy scenes read, %d only with the local threshold\n", ok, cases,
           localOnly);
}

static void FeedScene(GdxCsScanDecoder* d, GdxCsScanSession* s, const GdxCsQrSet* set, int index, int angle,
                      GdxCsScanEvent want) {
    uint8_t qrcode[GDXCS_QR_BUFFER_BYTES];
    GdxCsScanResult r;
    Scene sc = DefaultScene();
    const int modules = EncodeFrame(set, index, qrcode);
    int page = -1;
    sc.modulePx = 3.3;
    sc.angleDeg = angle;
    sc.mirror = angle % 2;
    CHECK(ScanScene(d, qrcode, modules, &sc, set->frame[index], set->frameLength[index], &r) > 0);
    CHECK(gdxcs_scan_session_feed(s, r.code[0].payload, r.code[0].length, &page) == want);
    CHECK(page == index);
}

/* Any order, duplicates, pages of another course in between: the identical file. */
static void TestScanSession(GdxCsScanDecoder* d) {
    static GdxCsQrSet set;
    static GdxCsQrSet other;
    static GdxCsScanSession s;
    uint8_t file[GDXCS_FILE_BYTES];
    uint8_t otherFile[GDXCS_FILE_BYTES];
    int i;
    BuildSetOf(64, "SESSIONA", file, &set);
    CHECK(gdxcs_qr_build_sized(file, sizeof(file), GDXCS_QR_CHUNK_LARGE, &set) == 0);
    BuildSetOf(48, "SESSIONB", otherFile, &other);
    CHECK(set.count >= 3 && other.count >= 2);
    gdxcs_scan_session_reset(&s);
    CHECK(gdxcs_scan_session_count(&s) == 0 && gdxcs_scan_session_file(&s) == NULL);
    FeedScene(d, &s, &set, 1, 30, GDXCS_SCAN_EV_PAGE);
    CHECK(gdxcs_scan_session_count(&s) == set.count && gdxcs_scan_session_have(&s) == 1);
    CHECK(gdxcs_scan_session_has_page(&s, 1) && !gdxcs_scan_session_has_page(&s, 0));
    FeedScene(d, &s, &set, 1, 75, GDXCS_SCAN_EV_DUPLICATE);
    FeedScene(d, &s, &other, 0, 180, GDXCS_SCAN_EV_FOREIGN);
    FeedScene(d, &s, &other, 1, 181, GDXCS_SCAN_EV_FOREIGN);
    CHECK(gdxcs_scan_session_have(&s) == 1 && s.foreign == 2 && s.duplicates == 1); /* nothing lost */
    for (i = set.count - 1; i >= 0; i--) {
        if (i == 1) {
            continue;
        }
        FeedScene(d, &s, &set, i, 15 * i + 3, i == 0 ? GDXCS_SCAN_EV_COMPLETE : GDXCS_SCAN_EV_PAGE);
    }
    CHECK(gdxcs_scan_session_file(&s) != NULL);
    CHECK(gdxcs_scan_session_file(&s) != NULL && memcmp(gdxcs_scan_session_file(&s), file, GDXCS_FILE_BYTES) == 0);
    CHECK_STATUS(Decode(gdxcs_scan_session_file(&s), GDXCS_FILE_BYTES), GDXCS_OK);
    FeedScene(d, &s, &set, 0, 0, GDXCS_SCAN_EV_DUPLICATE); /* after COMPLETE nothing changes */
    CHECK(s.pages == set.count);

    /* Not course pages at all. */
    {
        uint8_t qrcode[GDXCS_QR_BUFFER_BYTES];
        uint8_t temp[GDXCS_QR_BUFFER_BYTES];
        GdxCsScanResult r;
        Scene sc = DefaultScene();
        static const char kUrl[] = "https://example.invalid/not-a-course";
        int modules;
        gdxcs_scan_session_reset(&s);
        CHECK(qrcodegen_encodeText(kUrl, temp, qrcode, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true));
        modules = qrcodegen_getSize(qrcode);
        CHECK(ScanScene(d, qrcode, modules, &sc, (const uint8_t*) kUrl, strlen(kUrl), &r) > 0);
        CHECK(gdxcs_scan_session_feed(&s, r.code[0].payload, r.code[0].length, NULL) == GDXCS_SCAN_EV_NOT_COURSE);
        CHECK(gdxcs_scan_session_feed(&s, NULL, 10, NULL) == GDXCS_SCAN_EV_NOT_COURSE);
        CHECK(gdxcs_scan_session_feed(&s, set.frame[0], GDXCS_SCAN_PAYLOAD_MAX + 1, NULL) == GDXCS_SCAN_EV_NOT_COURSE);
        CHECK(gdxcs_scan_session_count(&s) == 0 && s.notCourse == 3);
    }
    /* A whole set that does not reassemble (a forged set id): dropped, then a real scan works. */
    {
        uint8_t f[GDXCS_QR_FRAME_MAX];
        GdxCsScanEvent last = GDXCS_SCAN_EV_PAGE;
        gdxcs_scan_session_reset(&s);
        for (i = 0; i < set.count; i++) {
            memcpy(f, set.frame[i], set.frameLength[i]);
            f[6] ^= 0x01;
            last = gdxcs_scan_session_feed(&s, f, set.frameLength[i], NULL);
        }
        CHECK(last == GDXCS_SCAN_EV_BAD_SET && s.badSets == 1 && gdxcs_scan_session_count(&s) == 0);
        for (i = 0; i < set.count; i++) {
            last = gdxcs_scan_session_feed(&s, set.frame[i], set.frameLength[i], NULL);
        }
        CHECK(last == GDXCS_SCAN_EV_COMPLETE);
        CHECK(memcmp(gdxcs_scan_session_file(&s), file, GDXCS_FILE_BYTES) == 0);
    }
    CHECK(strcmp(gdxcs_scan_event_name(GDXCS_SCAN_EV_FOREIGN), "foreign") == 0);
}

/* ---- the BMP frame source ([debug] cshare_scan_file) ---------------------------------------- */

typedef struct MemFile {
    const uint8_t* data;
    size_t size;
} MemFile;

static size_t MemRead(void* ctx, size_t offset, void* dst, size_t n) {
    const MemFile* m = (const MemFile*) ctx;
    if (offset > m->size) {
        return 0;
    }
    if (n > m->size - offset) {
        n = m->size - offset;
    }
    memcpy(dst, m->data + offset, n);
    return n;
}

/* The menu's SHOT writer (gdx3ds_menu.c MenuWriteShot): 24-bit, bottom-up, from the portrait
 * RGB565 framebuffer. */
static size_t WriteShotBmp(const uint16_t* fb, uint8_t* out) {
    const unsigned rowBytes = (FB_W * 3u + 3u) & ~3u;
    const uint32_t fileBytes = 54u + rowBytes * FB_H;
    unsigned y;
    memset(out, 0, 54);
    out[0] = 'B';
    out[1] = 'M';
    out[2] = (uint8_t) fileBytes;
    out[3] = (uint8_t) (fileBytes >> 8);
    out[4] = (uint8_t) (fileBytes >> 16);
    out[5] = (uint8_t) (fileBytes >> 24);
    out[10] = 54;
    out[14] = 40;
    out[18] = (uint8_t) (FB_W & 0xFF);
    out[19] = (uint8_t) (FB_W >> 8);
    out[22] = (uint8_t) (FB_H & 0xFF);
    out[23] = (uint8_t) (FB_H >> 8);
    out[26] = 1;
    out[28] = 24;
    for (y = 0; y < FB_H; y++) {
        const unsigned sy = FB_H - 1u - y;
        uint8_t* row = out + 54 + (size_t) y * rowBytes;
        unsigned x;
        memset(row, 0, rowBytes);
        for (x = 0; x < FB_W; x++) {
            const uint16_t p = fb[(size_t) x * FB_H + (FB_H - 1u - sy)];
            const unsigned r = (p >> 11) & 31u;
            const unsigned g = (p >> 5) & 63u;
            const unsigned b = p & 31u;
            row[x * 3u + 0] = (uint8_t) ((b << 3) | (b >> 2));
            row[x * 3u + 1] = (uint8_t) ((g << 2) | (g >> 4));
            row[x * 3u + 2] = (uint8_t) ((r << 3) | (r >> 2));
        }
    }
    return fileBytes;
}

static void TestScanBmp(GdxCsScanDecoder* d) {
    static GdxCsQrSet set;
    static uint16_t fb[FB_W * FB_H];
    static uint8_t bmp[54 + 960 * 240 + 64];
    static uint8_t yuv[GDXCS_SCAN_YUV_BYTES];
    static uint8_t scratch[GDXCS_SCAN_BMP_SCRATCH(CAM_W)];
    static GdxCsScanSession s;
    uint8_t file[GDXCS_FILE_BYTES];
    uint8_t qrcode[GDXCS_QR_BUFFER_BYTES];
    GdxCsScanResult r;
    MemFile m;
    size_t size = 0;
    int i;
    int fuzz;
    GdxCsScanEvent last = GDXCS_SCAN_EV_PAGE;
    BuildSetOf(64, "SHOTBMP", file, &set);
    gdxcs_scan_session_reset(&s);
    for (i = set.count - 1; i >= 0; i--) {
        memset(fb, 0x84, sizeof(fb));
        gdxcs_qr_encode(set.frame[i], set.frameLength[i], qrcode);
        gdxcs_qr_blit_rgb565(qrcode, fb, FB_W, FB_H, 8, 24, QR_SCALE, QR_QUIET); /* the QR view's spot */
        size = WriteShotBmp(fb, bmp);
        m.data = bmp;
        m.size = size;
        CHECK(gdxcs_scan_bmp_to_yuv(MemRead, &m, size, yuv, CAM_W, CAM_H, 1, scratch, sizeof(scratch)) == 0);
        CHECK(ScanYuv(d, yuv, &r) > 0 || ScanYuv(d, yuv, &r) > 0);
        last = gdxcs_scan_session_feed(&s, r.code[0].payload, r.code[0].length, NULL);
    }
    CHECK(last == GDXCS_SCAN_EV_COMPLETE);
    CHECK(gdxcs_scan_session_file(&s) != NULL && memcmp(gdxcs_scan_session_file(&s), file, GDXCS_FILE_BYTES) == 0);

    /* Refusals: truncated, a lying size, bad magic, 16-bit, RLE, zero / huge sides, tiny scratch. */
    m.data = bmp;
    m.size = size;
    CHECK(gdxcs_scan_bmp_to_yuv(MemRead, &m, size - 1, yuv, CAM_W, CAM_H, 1, scratch, sizeof(scratch)) == -1);
    m.size = size - 100;
    CHECK(gdxcs_scan_bmp_to_yuv(MemRead, &m, size, yuv, CAM_W, CAM_H, 1, scratch, sizeof(scratch)) == -1);
    m.size = size;
    CHECK(gdxcs_scan_bmp_to_yuv(MemRead, &m, 20, yuv, CAM_W, CAM_H, 1, scratch, sizeof(scratch)) == -1);
    CHECK(gdxcs_scan_bmp_to_yuv(MemRead, &m, size, yuv, CAM_W, CAM_H, 1, scratch, 100) == -1);
    CHECK(gdxcs_scan_bmp_to_yuv(MemRead, &m, size, yuv, 3, CAM_H, 1, scratch, sizeof(scratch)) == -1);
    {
        static const struct {
            int at;
            uint8_t value;
        } kBreaks[] = { { 0, 'X' }, { 28, 16 }, { 30, 1 }, { 26, 2 }, { 18, 0 }, { 19, 0x40 }, { 23, 0x80 }, { 10, 4 } };
        size_t k;
        for (k = 0; k < sizeof(kBreaks) / sizeof(kBreaks[0]); k++) {
            const uint8_t keep = bmp[kBreaks[k].at];
            const uint8_t keep19 = bmp[19];
            if (kBreaks[k].at == 18) {
                bmp[19] = 0; /* width 0 */
            }
            bmp[kBreaks[k].at] = kBreaks[k].value;
            CHECK(gdxcs_scan_bmp_to_yuv(MemRead, &m, size, yuv, CAM_W, CAM_H, 1, scratch, sizeof(scratch)) == -1);
            bmp[kBreaks[k].at] = keep;
            bmp[19] = keep19;
        }
    }
    /* A top-down 32-bit picture of another size is fitted, not refused. */
    {
        static uint8_t big[54 + 400 * 4 * 300];
        const int w = 400;
        const int h = 300;
        int y;
        memset(big, 0, 54);
        big[0] = 'B';
        big[1] = 'M';
        big[10] = 54;
        big[14] = 40;
        big[18] = (uint8_t) (w & 0xFF);
        big[19] = (uint8_t) (w >> 8);
        big[22] = (uint8_t) ((-h) & 0xFF);
        big[23] = (uint8_t) (((-h) >> 8) & 0xFF);
        big[24] = 0xFF;
        big[25] = 0xFF;
        big[26] = 1;
        big[28] = 32;
        for (y = 0; y < h; y++) {
            int x;
            for (x = 0; x < w; x++) {
                uint8_t* p = big + 54 + ((size_t) y * w + x) * 4;
                p[0] = p[1] = p[2] = (uint8_t) (y < h / 2 ? 0 : 255);
                p[3] = 0xFF;
            }
        }
        m.data = big;
        m.size = sizeof(big);
        CHECK(gdxcs_scan_bmp_to_yuv(MemRead, &m, sizeof(big), yuv, CAM_W, CAM_H, 0, scratch, sizeof(scratch)) == 0);
        CHECK(yuv[(size_t) (CAM_H / 2 - 20) * CAM_W * 2] < 16);       /* top half black (top-down) */
        CHECK(yuv[(size_t) (CAM_H / 2 + 20) * CAM_W * 2] > 240);      /* bottom half white */
    }
    /* Header fuzz: never a read outside the file, never a crash (run under ASan/UBSan). */
    for (fuzz = 0; fuzz < 3000; fuzz++) {
        static uint8_t junk[4096];
        size_t j;
        const size_t len = 54 + Rand() % (sizeof(junk) - 54);
        for (j = 0; j < len; j++) {
            junk[j] = (uint8_t) Rand();
        }
        junk[0] = 'B';
        junk[1] = 'M';
        if (fuzz & 1) {
            junk[14] = 40;
            junk[15] = junk[16] = junk[17] = 0;
            junk[26] = 1;
            junk[27] = 0;
            junk[28] = (fuzz & 2) ? 24 : 32;
            junk[29] = 0;
            junk[30] = junk[31] = junk[32] = junk[33] = 0;
        }
        m.data = junk;
        m.size = len;
        (void) gdxcs_scan_bmp_to_yuv(MemRead, &m, len, yuv, CAM_W, CAM_H, 1, scratch, sizeof(scratch));
    }
}

static void TestScanPipeline(void) {
    static GdxCsScanDecoder d;
    CHECK(gdxcs_scan_decoder_init(&d, CAM_W, CAM_H) == 0);
    CHECK(gdxcs_scan_decoder_image(&d) != NULL);
    CHECK(gdxcs_scan_decoder_bytes(&d) > (size_t) CAM_W * CAM_H);
    printf("scan decoder: %u bytes for %dx%d (quirc + scratch)\n", (unsigned) gdxcs_scan_decoder_bytes(&d), CAM_W,
           CAM_H);
    TestScanConversions();
    TestScanStraight(&d);
    TestScanGeometry(&d);
    TestScanPoorLight(&d);
    TestScanSession(&d);
    TestScanBmp(&d);
    gdxcs_scan_decoder_free(&d);
    CHECK(gdxcs_scan_decoder_image(&d) == NULL);
    CHECK(gdxcs_scan_decoder_init(NULL, CAM_W, CAM_H) == -1);
}

int main(void) {
    TestGoldenChecksum();
    TestCrcsAndDiskId();
    TestRoundTrip();
    TestRandomRoundTrips();
    TestHeaderRefusals();
    TestPayloadRefusals();
    TestEncodeRefusals();
    TestNames();
    TestFuzz();
    TestPackRoundTrips();
    TestUnpackRefusals();
    TestQrScan();
    TestAssemblerRefusals();
    TestScanPipeline();
    if (sFailures != 0) {
        fprintf(stderr, "gdx3ds_course_share_host_tests: %d of %d checks FAILED\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx3ds_course_share_host_tests: all %d checks passed\n", sChecks);
    return 0;
}
