/* port/tests/gdx_mp_beacon_host_tests.c — the NEARBY room beacon (port/gdx_mp_beacon.c):
 *   1. encode/decode round trip, the exact v1 size, fitting the UDS appdata (0xC8);
 *   2. the label ("<name>'s room n/4 N3DS" + signal bars), UTF-8 safe at every cut;
 *   3. compatibility: proto / build / content / full / racing, and a newer beacon version that
 *      is listed but greyed out;
 *   4. the 3DS profile name: UTF-16 -> UTF-8 (BMP, surrogate pairs, unpaired surrogates,
 *      control characters, the 10-unit field without a NUL);
 *   5. the UDS passphrase: deterministic, differs per build / content / proto;
 *   6. hostile input: every truncation, every single-bit flip and 300k random / mutated blobs
 *      either decode to a beacon whose strings are terminated, valid UTF-8 and in-range counts,
 *      or are refused -- never a crash, never an over-read (run it under -fsanitize=address). */
#include "gdx_mp_beacon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        sChecks++;                                                                    \
        if (!(cond)) {                                                                \
            sFailures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                             \
    } while (0)

static uint32_t sRng = 4242u;

static uint32_t Rand(void) {
    sRng = sRng * 1103515245u + 12345u;
    return (sRng >> 8) & 0xFFFFFFu;
}

static GdxMpBeacon Sample(void) {
    GdxMpBeacon b;
    memset(&b, 0, sizeof(b));
    b.version = GDX_MP_BEACON_VERSION;
    b.flags = GDX_MP_BEACON_F_OPEN;
    b.players = 1;
    b.maxPlayers = 4;
    b.model = GDX_MP_MODEL_N3DS;
    b.proto = GDX_MP_BEACON_PROTO;
    b.delay = 2;
    b.hashEvery = 1;
    b.buildHash = gdx_mp_beacon_build_hash("v1.0.1-42-gabcdef0");
    b.contentHash = 0x1234ABCDu;
    b.roomId = 0xCAFEF00Du;
    snprintf(b.name, sizeof(b.name), "%s", "Mike");
    snprintf(b.build, sizeof(b.build), "%s", "v1.0.1-42-gabcdef0");
    return b;
}

static int ValidUtf8(const char* s) {
    const unsigned char* p = (const unsigned char*) s;
    while (*p) {
        int len;
        int i;
        uint32_t c;
        if (*p < 0x80) {
            if (*p < 0x20 || *p == 0x7F) {
                return 0;
            }
            p++;
            continue;
        } else if ((*p & 0xE0) == 0xC0) {
            len = 2;
            c = *p & 0x1F;
        } else if ((*p & 0xF0) == 0xE0) {
            len = 3;
            c = *p & 0x0F;
        } else if ((*p & 0xF8) == 0xF0) {
            len = 4;
            c = *p & 0x07;
        } else {
            return 0;
        }
        for (i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                return 0;
            }
            c = (c << 6) | (p[i] & 0x3F);
        }
        if ((len == 2 && c < 0x80) || (len == 3 && c < 0x800) || (len == 4 && c < 0x10000) || c > 0x10FFFF ||
            (c >= 0xD800 && c <= 0xDFFF)) {
            return 0;
        }
        p += len;
    }
    return 1;
}

static int Terminated(const char* s, int size) {
    return memchr(s, 0, (size_t) size) != NULL;
}

static void CheckDecoded(const GdxMpBeacon* b) {
    CHECK(Terminated(b->name, sizeof(b->name)));
    CHECK(Terminated(b->build, sizeof(b->build)));
    CHECK(ValidUtf8(b->name));
    CHECK(ValidUtf8(b->build));
    CHECK(b->maxPlayers >= 2 && b->maxPlayers <= GDX_MP_BEACON_MAX_PLAYERS);
    CHECK(b->players >= 1 && b->players <= b->maxPlayers);
}

static void TestRoundTrip(void) {
    GdxMpBeacon b = Sample();
    GdxMpBeacon d;
    uint8_t buf[GDX_MP_BEACON_APPDATA_MAX];
    CHECK(GDX_MP_BEACON_BYTES <= GDX_MP_BEACON_APPDATA_MAX);
    CHECK(gdx_mp_beacon_encode(&b, buf, sizeof(buf)) == GDX_MP_BEACON_BYTES);
    CHECK(gdx_mp_beacon_encode(&b, buf, GDX_MP_BEACON_BYTES - 1) == 0);
    CHECK(gdx_mp_beacon_decode(buf, GDX_MP_BEACON_BYTES, &d) == GDX_MP_BEACON_OK);
    CHECK(memcmp(&b, &d, sizeof(b)) == 0);
    CHECK(gdx_mp_beacon_compat(&d, b.buildHash, b.contentHash, GDX_MP_BEACON_PROTO) == GDX_MP_COMPAT_OK);
    /* Little-endian at the documented offsets. */
    CHECK(buf[0] == 'G' && buf[1] == 'X' && buf[2] == 'B' && buf[3] == 'N');
    CHECK(buf[6] == 1 && buf[7] == 4 && buf[8] == GDX_MP_MODEL_N3DS);
    CHECK(buf[20] == 0x0D && buf[23] == 0xCA);
    CHECK(memcmp(buf + 24, "Mike", 5) == 0);
    /* Counts out of range do not encode. */
    b.players = 5;
    CHECK(gdx_mp_beacon_encode(&b, buf, sizeof(buf)) == 0);
    b.players = 1;
    b.maxPlayers = 1;
    CHECK(gdx_mp_beacon_encode(&b, buf, sizeof(buf)) == 0);
    b.maxPlayers = 9;
    CHECK(gdx_mp_beacon_encode(&b, buf, sizeof(buf)) == 0);
    /* Long strings are cut at a character boundary and stay terminated. */
    b = Sample();
    snprintf(b.name, sizeof(b.name), "%s", "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                                           "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"); /* 30 bytes */
    CHECK(gdx_mp_beacon_encode(&b, buf, sizeof(buf)) == GDX_MP_BEACON_BYTES);
    CHECK(gdx_mp_beacon_decode(buf, GDX_MP_BEACON_BYTES, &d) == GDX_MP_BEACON_OK);
    CHECK(strcmp(d.name, b.name) == 0);
    CheckDecoded(&d);
}

static void TestLabel(void) {
    GdxMpBeacon b = Sample();
    char out[96];
    int cap;
    gdx_mp_beacon_label(&b, 3, out, sizeof(out));
    CHECK(strcmp(out, "Mike's room 1/4 N3DS \xE2\x96\x82\xE2\x96\x84\xE2\x96\x86") == 0);
    gdx_mp_beacon_label(&b, 1, out, sizeof(out));
    CHECK(strcmp(out, "Mike's room 1/4 N3DS \xE2\x96\x82__") == 0);
    gdx_mp_beacon_label(&b, -1, out, sizeof(out));
    CHECK(strcmp(out, "Mike's room 1/4 N3DS") == 0);
    b.model = GDX_MP_MODEL_3DS;
    b.players = 3;
    gdx_mp_beacon_label(&b, 0, out, sizeof(out));
    CHECK(strcmp(out, "Mike's room 3/4 O3DS ___") == 0);
    b.name[0] = '\0';
    gdx_mp_beacon_label(&b, -1, out, sizeof(out));
    CHECK(strcmp(out, "3DS's room 3/4 O3DS") == 0);
    /* Every cap: terminated, valid UTF-8, never past the cap. */
    b = Sample();
    snprintf(b.name, sizeof(b.name), "%s", "\xE3\x83\x9E\xE3\x82\xA4\xE3\x82\xAF"); /* katakana */
    for (cap = 1; cap < 64; cap++) {
        char small[64];
        int n;
        memset(small, 'Z', sizeof(small));
        n = gdx_mp_beacon_label(&b, 3, small, cap);
        CHECK(n < cap);
        CHECK(small[n] == '\0');
        CHECK(ValidUtf8(small));
        CHECK(small[cap] == 'Z' || cap == 63);
    }
    CHECK(gdx_mp_model_is_new(GDX_MP_MODEL_N2DSXL) && !gdx_mp_model_is_new(GDX_MP_MODEL_2DS));
    CHECK(strcmp(gdx_mp_model_name(77), "3DS?") == 0);
    CHECK(gdx_mp_signal_bars(4) == -1 && gdx_mp_signal_bars(-1) == -1 && gdx_mp_signal_bars(2) == 2);
}

static void TestCompat(void) {
    GdxMpBeacon b = Sample();
    const uint32_t bh = b.buildHash;
    const uint32_t ch = b.contentHash;
    uint8_t buf[GDX_MP_BEACON_APPDATA_MAX];
    GdxMpBeacon d;
    CHECK(gdx_mp_beacon_compat(&b, bh, ch, GDX_MP_BEACON_PROTO) == GDX_MP_COMPAT_OK);
    CHECK(gdx_mp_beacon_compat(&b, bh ^ 1u, ch, GDX_MP_BEACON_PROTO) == GDX_MP_COMPAT_BUILD);
    CHECK(gdx_mp_beacon_compat(&b, bh, ch ^ 1u, GDX_MP_BEACON_PROTO) == GDX_MP_COMPAT_CONTENT);
    CHECK(gdx_mp_beacon_compat(&b, bh, ch, GDX_MP_BEACON_PROTO + 1) == GDX_MP_COMPAT_PROTO);
    b.players = 4;
    CHECK(gdx_mp_beacon_compat(&b, bh, ch, GDX_MP_BEACON_PROTO) == GDX_MP_COMPAT_FULL);
    b.players = 2;
    b.flags = GDX_MP_BEACON_F_RACING;
    CHECK(gdx_mp_beacon_compat(&b, bh, ch, GDX_MP_BEACON_PROTO) == GDX_MP_COMPAT_RACING);
    b.flags = 0;
    CHECK(gdx_mp_beacon_compat(&b, bh, ch, GDX_MP_BEACON_PROTO) == GDX_MP_COMPAT_FULL); /* not open */
    CHECK(strcmp(gdx_mp_compat_text(GDX_MP_COMPAT_BUILD), "different build") == 0);
    CHECK(gdx_mp_beacon_build_hash("a") != gdx_mp_beacon_build_hash("b"));
    CHECK(gdx_mp_beacon_build_hash(NULL) == gdx_mp_beacon_build_hash(""));

    /* A v2 beacon (longer, check in its last 4 bytes) still lists, greyed out by proto. */
    b = Sample();
    CHECK(gdx_mp_beacon_encode(&b, buf, sizeof(buf)) == GDX_MP_BEACON_BYTES);
    {
        uint8_t v2[120];
        uint32_t h = 2166136261u;
        int i;
        memcpy(v2, buf, GDX_MP_BEACON_BYTES - 4);
        memset(v2 + GDX_MP_BEACON_BYTES - 4, 0x77, sizeof(v2) - (GDX_MP_BEACON_BYTES - 4));
        v2[4] = 2;
        for (i = 0; i < (int) sizeof(v2) - 4; i++) {
            h ^= v2[i];
            h *= 16777619u;
        }
        v2[116] = (uint8_t) (h & 0xFF);
        v2[117] = (uint8_t) ((h >> 8) & 0xFF);
        v2[118] = (uint8_t) ((h >> 16) & 0xFF);
        v2[119] = (uint8_t) (h >> 24);
        CHECK(gdx_mp_beacon_decode(v2, sizeof(v2), &d) == GDX_MP_BEACON_OK);
        CHECK(d.version == 2 && strcmp(d.name, "Mike") == 0);
        CHECK(gdx_mp_beacon_compat(&d, bh, ch, GDX_MP_BEACON_PROTO) == GDX_MP_COMPAT_PROTO);
        /* A v1 beacon must be exactly 88 bytes. */
        CHECK(gdx_mp_beacon_decode(buf, GDX_MP_BEACON_BYTES + 4, &d) != GDX_MP_BEACON_OK);
    }
}

static void TestUtf16(void) {
    char out[40];
    {
        const uint16_t name[10] = { 'M', 'i', 'k', 'e', 0, 'X', 'X', 'X', 'X', 'X' };
        CHECK(gdx_mp_name_from_utf16(name, 10, out, sizeof(out)) == 4 && strcmp(out, "Mike") == 0);
    }
    {
        const uint16_t full[10] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J' }; /* no NUL */
        CHECK(gdx_mp_name_from_utf16(full, 10, out, sizeof(out)) == 10 && strcmp(out, "ABCDEFGHIJ") == 0);
    }
    {
        const uint16_t jp[4] = { 0x30DE, 0x30A4, 0x30AF, 0 }; /* マイク */
        gdx_mp_name_from_utf16(jp, 4, out, sizeof(out));
        CHECK(strcmp(out, "\xE3\x83\x9E\xE3\x82\xA4\xE3\x82\xAF") == 0);
    }
    {
        const uint16_t emoji[4] = { 0xD83D, 0xDE80, 'x', 0 }; /* U+1F680 */
        gdx_mp_name_from_utf16(emoji, 4, out, sizeof(out));
        CHECK(strcmp(out, "\xF0\x9F\x9A\x80x") == 0);
    }
    {
        const uint16_t bad[5] = { 0xDC00, 'a', 0xD800, 0x0007, 0 }; /* lone low, lone high, BEL */
        gdx_mp_name_from_utf16(bad, 5, out, sizeof(out));
        CHECK(strcmp(out, "?a??") == 0);
    }
    {
        const uint16_t jp[4] = { 0x30DE, 0x30A4, 0x30AF, 0 };
        CHECK(gdx_mp_name_from_utf16(jp, 4, out, 7) == 6); /* two characters fit in 6 bytes + NUL */
        CHECK(ValidUtf8(out));
    }
    CHECK(gdx_mp_name_from_utf16(NULL, 4, out, sizeof(out)) == 0 && out[0] == '\0');
}

static void TestPassphrase(void) {
    char a[GDX_MP_UDS_PASSPHRASE_LEN + 1];
    char b[GDX_MP_UDS_PASSPHRASE_LEN + 1];
    gdx_mp_uds_passphrase("v1.0.1", 0x11u, 1, a);
    gdx_mp_uds_passphrase("v1.0.1", 0x11u, 1, b);
    CHECK(strlen(a) == GDX_MP_UDS_PASSPHRASE_LEN && strcmp(a, b) == 0);
    gdx_mp_uds_passphrase("v1.0.2", 0x11u, 1, b);
    CHECK(strcmp(a, b) != 0);
    gdx_mp_uds_passphrase("v1.0.1", 0x12u, 1, b);
    CHECK(strcmp(a, b) != 0);
    gdx_mp_uds_passphrase("v1.0.1", 0x11u, 2, b);
    CHECK(strcmp(a, b) != 0);
    gdx_mp_uds_passphrase(NULL, 0, 1, b);
    CHECK(strlen(b) == GDX_MP_UDS_PASSPHRASE_LEN);
}

static void TestHostile(void) {
    GdxMpBeacon b = Sample();
    GdxMpBeacon d;
    uint8_t good[GDX_MP_BEACON_APPDATA_MAX];
    uint8_t buf[GDX_MP_BEACON_APPDATA_MAX + 16];
    int len;
    int bit;
    int i;
    int accepted = 0;
    int refused = 0;
    CHECK(gdx_mp_beacon_encode(&b, good, sizeof(good)) == GDX_MP_BEACON_BYTES);
    /* Every truncation is refused. */
    for (len = 0; len < GDX_MP_BEACON_BYTES; len++) {
        CHECK(gdx_mp_beacon_decode(good, len, &d) == GDX_MP_BEACON_E_SHORT);
    }
    CHECK(gdx_mp_beacon_decode(good, GDX_MP_BEACON_APPDATA_MAX + 1, &d) == GDX_MP_BEACON_E_SHORT);
    CHECK(gdx_mp_beacon_decode(NULL, GDX_MP_BEACON_BYTES, &d) != GDX_MP_BEACON_OK);
    /* Every single-bit flip is caught (the check covers every byte before it). */
    for (bit = 0; bit < GDX_MP_BEACON_BYTES * 8; bit++) {
        memcpy(buf, good, GDX_MP_BEACON_BYTES);
        buf[bit / 8] ^= (uint8_t) (1u << (bit % 8));
        CHECK(gdx_mp_beacon_decode(buf, GDX_MP_BEACON_BYTES, &d) != GDX_MP_BEACON_OK);
    }
    /* Re-checked hostile shapes: a correct check over bad fields must still be refused. */
    {
        static const struct {
            int off;
            uint8_t val;
        } kBad[] = {
            { 6, 0 }, { 6, 5 }, { 7, 1 }, { 7, 9 }, { 4, 0 },
        };
        int k;
        for (k = 0; k < (int) (sizeof(kBad) / sizeof(kBad[0])); k++) {
            uint32_t h = 2166136261u;
            int j;
            memcpy(buf, good, GDX_MP_BEACON_BYTES);
            buf[kBad[k].off] = kBad[k].val;
            for (j = 0; j < GDX_MP_BEACON_BYTES - 4; j++) {
                h ^= buf[j];
                h *= 16777619u;
            }
            buf[84] = (uint8_t) (h & 0xFF);
            buf[85] = (uint8_t) ((h >> 8) & 0xFF);
            buf[86] = (uint8_t) ((h >> 16) & 0xFF);
            buf[87] = (uint8_t) (h >> 24);
            CHECK(gdx_mp_beacon_decode(buf, GDX_MP_BEACON_BYTES, &d) != GDX_MP_BEACON_OK);
        }
        /* Unterminated name / build, a control character in the build, malformed UTF-8 in the
         * name (accepted, but shown sanitised). */
        for (k = 0; k < 4; k++) {
            uint32_t h = 2166136261u;
            int j;
            int expectOk = (k == 3);
            memcpy(buf, good, GDX_MP_BEACON_BYTES);
            if (k == 0) {
                memset(buf + 24, 'A', 32);
            } else if (k == 1) {
                memset(buf + 56, 'B', 24);
            } else if (k == 2) {
                buf[57] = 0x07;
            } else {
                buf[24] = 0xC3; /* a lead byte with no continuation */
                buf[25] = 'x';
                buf[26] = 0xFF;
                buf[27] = 0;
            }
            for (j = 0; j < GDX_MP_BEACON_BYTES - 4; j++) {
                h ^= buf[j];
                h *= 16777619u;
            }
            buf[84] = (uint8_t) (h & 0xFF);
            buf[85] = (uint8_t) ((h >> 8) & 0xFF);
            buf[86] = (uint8_t) ((h >> 16) & 0xFF);
            buf[87] = (uint8_t) (h >> 24);
            CHECK((gdx_mp_beacon_decode(buf, GDX_MP_BEACON_BYTES, &d) == GDX_MP_BEACON_OK) == expectOk);
            if (expectOk) {
                CHECK(strcmp(d.name, "?x?") == 0);
                CheckDecoded(&d);
            }
        }
    }
    /* Fuzz: random blobs, mutated beacons, and mutated beacons with a recomputed check (so the
     * field validation, not just the check, is what stands in the way). */
    for (i = 0; i < 300000; i++) {
        const int mode = (int) (Rand() % 3);
        int r;
        len = (int) (Rand() % (GDX_MP_BEACON_APPDATA_MAX + 8));
        if (mode == 0) {
            int j;
            for (j = 0; j < len; j++) {
                buf[j] = (uint8_t) Rand();
            }
            if (len >= 4 && (Rand() % 2)) {
                memcpy(buf, "GXBN", 4);
            }
        } else {
            int flips = 1 + (int) (Rand() % 8);
            len = (Rand() % 4) ? GDX_MP_BEACON_BYTES : len;
            memcpy(buf, good, GDX_MP_BEACON_BYTES);
            if (len > GDX_MP_BEACON_BYTES) {
                memset(buf + GDX_MP_BEACON_BYTES, (int) Rand(), (size_t) (len - GDX_MP_BEACON_BYTES));
            }
            while (flips-- > 0 && len > 0) {
                buf[Rand() % (uint32_t) len] = (uint8_t) Rand();
            }
            if (mode == 2 && len >= GDX_MP_BEACON_BYTES) {
                uint32_t h = 2166136261u;
                int j;
                for (j = 0; j < len - 4; j++) {
                    h ^= buf[j];
                    h *= 16777619u;
                }
                buf[len - 4] = (uint8_t) (h & 0xFF);
                buf[len - 3] = (uint8_t) ((h >> 8) & 0xFF);
                buf[len - 2] = (uint8_t) ((h >> 16) & 0xFF);
                buf[len - 1] = (uint8_t) (h >> 24);
            }
        }
        r = gdx_mp_beacon_decode(buf, len, &d);
        if (r == GDX_MP_BEACON_OK) {
            char label[80];
            int n;
            accepted++;
            if (!Terminated(d.name, sizeof(d.name)) || !Terminated(d.build, sizeof(d.build)) || !ValidUtf8(d.name) ||
                !ValidUtf8(d.build) || d.players < 1 || d.players > d.maxPlayers || d.maxPlayers < 2 ||
                d.maxPlayers > GDX_MP_BEACON_MAX_PLAYERS) {
                CHECK(0 && "fuzz: accepted beacon violates the decode contract");
            }
            n = gdx_mp_beacon_label(&d, (int) (Rand() % 5) - 1, label, (int) (1 + Rand() % sizeof(label)));
            if (!ValidUtf8(label) || label[n] != '\0') {
                CHECK(0 && "fuzz: label not safe");
            }
            gdx_mp_beacon_compat(&d, b.buildHash, b.contentHash, GDX_MP_BEACON_PROTO);
        } else {
            refused++;
            if (d.version != 0 || d.name[0] != '\0') {
                CHECK(0 && "fuzz: refused decode left output non-zero");
            }
        }
    }
    CHECK(accepted > 1000 && refused > 1000);
    printf("  fuzz: %d accepted (all well-formed), %d refused\n", accepted, refused);
}

int main(void) {
    printf("gdx_mp_beacon_host_tests\n");
    TestRoundTrip();
    TestLabel();
    TestCompat();
    TestUtf16();
    TestPassphrase();
    TestHostile();
    printf("%d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
