/* track_template_tests.c — host unit test for the [native] track index-template decoder.
 *
 * Two parts:
 *   - always: the decoder over synthetic GBI lists written here (G_TRI2/G_TRI1/G_RDPPIPESYNC/
 *     G_SETCOMBINE/G_ENDDL), in both word orders, plus its rejections and
 *     gdxtrk_templates_install's refusal of anything but the 18 retail lists;
 *   - with the user's fzerox.o2r (GDX_O2R, else the Azahar SD copy): the 18 course_track_gfx/
 *     D_8014* records decode, match GDXTRK_TEMPLATES_DIGEST, install, decode identically from a
 *     byte-swapped (N64 big-endian) copy, and keep the structural facts the replay relies on:
 *     every template is 10-16 tris, slots <= 20, and every SETCOMBINE-carrying template restores
 *     the track combiner (FC147E28 44FE793C) at its end.
 * No game data is compiled in. Built by tools/ci-3ds.sh like the other host tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../3ds/gfx/gdx3ds_track_templates.h"
#include "../3ds/test/gdx3ds_o2r_host_reader.h"

#define OTR_DL_DATA_OFFSET 88 /* 64-byte OTR header + 3 words of DisplayList resource header */
#define MAX_TEMPLATE_BYTES 96

static int sFailures = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            sFailures++;                                    \
            printf("FAIL: " __VA_ARGS__);                   \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static void ByteSwapCopy(const unsigned char* src, unsigned len, unsigned char* dst) {
    unsigned i;
    for (i = 0; i < len; i += 4) {
        dst[i + 0] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i + 0];
    }
}

/* One 8-byte command in host (little-endian) word order. */
static void PutCmd(unsigned char* p, unsigned w0, unsigned w1) {
    int i;
    for (i = 0; i < 4; i++) {
        p[i] = (unsigned char)(w0 >> (8 * i));
        p[4 + i] = (unsigned char)(w1 >> (8 * i));
    }
}

static unsigned Tri(unsigned a, unsigned b, unsigned c) {
    return ((a * 2u) << 16) | ((b * 2u) << 8) | (c * 2u);
}

static void TestSynthetic(void) {
    unsigned char list[48];
    unsigned char swapped[48];
    GdxTrkTemplate t;
    GdxTrkTemplate tBig;
    PutCmd(list + 0, 0xFC357E6Au, 0x44FE793Cu);                 /* G_SETCOMBINE before tri 0 */
    PutCmd(list + 8, 0x06000000u | Tri(0, 1, 2), Tri(2, 1, 3)); /* G_TRI2 */
    PutCmd(list + 16, 0xE7000000u, 0);                          /* G_RDPPIPESYNC */
    PutCmd(list + 24, 0x05000000u | Tri(20, 3, 4), 0);          /* G_TRI1 */
    PutCmd(list + 32, 0xFC147E28u, 0x44FE793Cu);                /* G_SETCOMBINE after the last */
    PutCmd(list + 40, 0xDF000000u, 0);                          /* G_ENDDL */
    CHECK(gdxtrk_decode_template(list, sizeof(list), &t) == GDXTRK_DECODE_OK, "synthetic list rejected");
    CHECK(t.ntris == 3 && t.nevents == 2 && t.maxSlot == 20, "synthetic: ntris=%d nevents=%d maxSlot=%d", t.ntris,
          t.nevents, t.maxSlot);
    CHECK(t.idx[0] == 0 && t.idx[1] == 1 && t.idx[2] == 2 && t.idx[3] == 2 && t.idx[4] == 1 && t.idx[5] == 3 &&
              t.idx[6] == 20 && t.idx[7] == 3 && t.idx[8] == 4,
          "synthetic: indices");
    CHECK(t.events[0].beforeTri == 0 && t.events[0].w0 == 0xFC357E6Au && t.events[1].beforeTri == 3 &&
              t.events[1].w0 == 0xFC147E28u && t.events[1].w1 == 0x44FE793Cu,
          "synthetic: SETCOMBINE events");
    ByteSwapCopy(list, sizeof(list), swapped);
    CHECK(gdxtrk_decode_template(swapped, sizeof(swapped), &tBig) == GDXTRK_DECODE_OK &&
              gdxtrk_template_equal(&tBig, &t),
          "synthetic: big-endian decode differs");
    PutCmd(list + 24, 0x05000000u | Tri(21, 3, 4), 0);
    CHECK(gdxtrk_decode_template(list, sizeof(list), &t) == GDXTRK_DECODE_BAD_SLOT, "slot 21 accepted");
}

static void TestRejections(void) {
    GdxTrkTemplate t;
    /* Little-endian words: the opcode is byte 3 of each w0 (G_VTX, then G_ENDDL). */
    unsigned char junk[16] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0xDF, 0x00, 0x00, 0x00, 0x00 };
    const uint8_t* none[GDXTRK_TEMPLATE_COUNT] = { NULL };
    int failedIndex = 0;
    CHECK(gdxtrk_decode_template(junk, sizeof(junk), &t) == GDXTRK_DECODE_UNSUPPORTED_OP, "junk opcode accepted");
    junk[11] = 0x00;
    CHECK(gdxtrk_decode_template(junk, sizeof(junk), &t) == GDXTRK_DECODE_NO_ENDDL, "missing ENDDL accepted");
    CHECK(gdxtrk_decode_template(junk, 7, &t) == GDXTRK_DECODE_BAD_ARGS, "odd length accepted");
    CHECK(!gdxtrk_templates_install(none, &failedIndex, NULL) && failedIndex == 0, "install of nothing accepted");
    CHECK(gGdxTrkTemplate[0].ntris == 0, "a refused install wrote the table");
}

/* Lists that decode fine but are not the retail set: refused on the digest. */
static void TestDigestRefusal(void) {
    static unsigned char lists[4][MAX_TEMPLATE_BYTES];
    const uint8_t* src[GDXTRK_TEMPLATE_COUNT];
    int failedIndex = 0;
    int i;
    int k;
    for (k = 0; k < 4; k++) {
        const int len = 72 + 8 * k; /* every length a template has: 72, 80, 88, 96 */
        for (i = 0; i < len; i += 8) {
            PutCmd(lists[k] + i, 0xE7000000u, 0);
        }
        PutCmd(lists[k], 0x06000000u | Tri(0, 1, 2), Tri(2, 1, 3));
        PutCmd(lists[k] + len - 8, 0xDF000000u, 0);
    }
    for (i = 0; i < GDXTRK_TEMPLATE_COUNT; i++) {
        src[i] = lists[(kGdxTrkTemplateLen[i] - 72) / 8];
    }
    CHECK(!gdxtrk_templates_install(src, &failedIndex, NULL) && failedIndex == -1,
          "non-retail lists installed (failedIndex=%d)", failedIndex);
    CHECK(gGdxTrkTemplate[0].ntris == 0, "a refused install wrote the table");
}

static void TestArchive(void) {
    static unsigned char rec[GDXTRK_TEMPLATE_COUNT][OTR_DL_DATA_OFFSET + MAX_TEMPLATE_BYTES];
    const uint8_t* src[GDXTRK_TEMPLATE_COUNT];
    GdxTrkTemplate t[GDXTRK_TEMPLATE_COUNT];
    int failedIndex = 0;
    int failedRc = 0;
    int i;
    if (!gdx3ds_o2r_host_open()) {
        return;
    }
    for (i = 0; i < GDXTRK_TEMPLATE_COUNT; i++) {
        char key[64];
        size_t n;
        snprintf(key, sizeof(key), "course_track_gfx/%s", kGdxTrkTemplateNames[i]);
        n = gdx3ds_o2r_host_read(key, rec[i], sizeof(rec[i]), NULL);
        CHECK(n == OTR_DL_DATA_OFFSET + (size_t)kGdxTrkTemplateLen[i], "%s: record is %u bytes, want %u", key,
              (unsigned)n, (unsigned)(OTR_DL_DATA_OFFSET + kGdxTrkTemplateLen[i]));
        src[i] = rec[i] + OTR_DL_DATA_OFFSET;
        CHECK(gdxtrk_decode_template(src[i], kGdxTrkTemplateLen[i], &t[i]) == GDXTRK_DECODE_OK, "%s: decode", key);
    }
    CHECK(gdxtrk_templates_digest(t) == GDXTRK_TEMPLATES_DIGEST, "archive digest 0x%08X != 0x%08X",
          (unsigned)gdxtrk_templates_digest(t), (unsigned)GDXTRK_TEMPLATES_DIGEST);
    CHECK(gdxtrk_templates_install(src, &failedIndex, &failedRc), "install refused (template %d rc %d)", failedIndex,
          failedRc);
    for (i = 0; i < GDXTRK_TEMPLATE_COUNT; i++) {
        const GdxTrkTemplate* g = &gGdxTrkTemplate[i];
        const char* name = kGdxTrkTemplateNames[i];
        unsigned char swapped[MAX_TEMPLATE_BYTES];
        GdxTrkTemplate tBig;
        int rc;
        CHECK(gdxtrk_template_equal(g, &t[i]), "%s: installed != decoded", name);
        CHECK(g->ntris >= 10 && g->ntris <= GDXTRK_MAX_TRIS, "%s: ntris=%d", name, g->ntris);
        CHECK(g->maxSlot <= GDXTRK_MAX_SLOT, "%s: maxSlot=%d", name, g->maxSlot);
        if (g->nevents != 0) {
            const GdxTrkCombineEvent* last = &g->events[g->nevents - 1];
            CHECK(g->nevents == 2, "%s: nevents=%d", name, g->nevents);
            CHECK(last->w0 == 0xFC147E28u && last->w1 == 0x44FE793Cu,
                  "%s: trailing SETCOMBINE %08X %08X is not the track combiner", name, last->w0, last->w1);
            CHECK(g->events[0].w0 == 0xFC357E6Au && g->events[0].w1 == 0x44FE793Cu,
                  "%s: leading SETCOMBINE %08X %08X unexpected", name, g->events[0].w0, g->events[0].w1);
            CHECK(g->events[0].beforeTri == 0, "%s: first event beforeTri=%d", name, g->events[0].beforeTri);
        }
        ByteSwapCopy(src[i], kGdxTrkTemplateLen[i], swapped);
        rc = gdxtrk_decode_template(swapped, kGdxTrkTemplateLen[i], &tBig);
        CHECK(rc == GDXTRK_DECODE_OK, "%s: big-endian decode rc=%d", name, rc);
        CHECK(gdxtrk_template_equal(&tBig, g), "%s: big-endian decode differs", name);
    }
}

int main(void) {
    TestSynthetic();
    TestRejections();
    TestDigestRefusal();
    TestArchive();
    if (sFailures == 0) {
        printf("=== ALL PASS ===\n");
        return 0;
    }
    printf("=== %d FAILURES ===\n", sFailures);
    return 1;
}
