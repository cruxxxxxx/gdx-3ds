/* port/3ds/test/gdx3ds_hudbake_host_tests.c — host tests for the DASH art bake (gdx3ds_hudbake.h).
 *
 *   - always: no reader, a missing archive, records of the wrong format and (with an archive)
 *     truncated records all bake without crashing, report and log every texture as missing, leave
 *     every glyph blank (alpha 0) and the backdrop its flat base colour, and give the widgets only
 *     their procedural chrome (panels, frames: no texture pixels), identical in every failure;
 *   - with the user's fzerox.o2r (GDX_O2R, else the Azahar SD copy): the bake finds every texture
 *     and each baked buffer hashes to the value recorded when it matched, byte for byte, the
 *     generated headers the bake replaced (kExpected below); a second bake is identical.
 * Built by tools/ci-3ds.sh. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_hudbake.h"
#include "test/gdx3ds_o2r_host_reader.h"

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond, ...)                                \
    do {                                                \
        gChecks++;                                      \
        if (!(cond)) {                                  \
            gFailures++;                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

#define TEXTURES_PER_BAKE 138

static uint32_t Fnv(const unsigned short* px, size_t n) {
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ (px[i] & 0xFFu)) * 16777619u;
        h = (h ^ (px[i] >> 8)) * 16777619u;
    }
    return h;
}

/* A glyph table's pixels: its entries are consecutive slices of one pool (Font1/Font2 use a
 * fixed slot per glyph, so hash glyph by glyph to stay independent of the slot size). */
static uint32_t FnvTable(const DashGlyph* t, int count) {
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < count; i++) {
        h ^= Fnv(t[i].pixels, (size_t) t[i].w * t[i].h);
        h *= 16777619u;
    }
    return h;
}

typedef struct Expected {
    const char* name;
    uint32_t hash;
} Expected;

static int CollectHashes(Expected* out) {
    int n = 0;
    int i;
    out[n].name = "timer", out[n++].hash = FnvTable(kDashGlyphTimer, KDASHGLYPHTIMER_COUNT);
    out[n].name = "lap", out[n++].hash = FnvTable(kDashGlyphLap, KDASHGLYPHLAP_COUNT);
    out[n].name = "posdigit", out[n++].hash = FnvTable(kDashGlyphPosDigit, KDASHGLYPHPOSDIGIT_COUNT);
    out[n].name = "possuffix", out[n++].hash = FnvTable(kDashGlyphPosSuffix, KDASHGLYPHPOSSUFFIX_COUNT);
    out[n].name = "font1", out[n++].hash = FnvTable(kDashGlyphFont1, KDASHGLYPHFONT1_COUNT);
    out[n].name = "font2", out[n++].hash = FnvTable(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT);
    out[n].name = "portrait", out[n++].hash = FnvTable(kDashGlyphPortrait, KDASHGLYPHPORTRAIT_COUNT);
    out[n].name = "kostar", out[n++].hash = FnvTable(kDashGlyphKoStar, KDASHGLYPHKOSTAR_COUNT);
    out[n].name = "lives", out[n++].hash = FnvTable(kDashGlyphLives, KDASHGLYPHLIVES_COUNT);
    out[n].name = "speeddigit", out[n++].hash = FnvTable(kDashGlyphSpeedDigit, KDASHGLYPHSPEEDDIGIT_COUNT);
    out[n].name = "kmh", out[n++].hash = FnvTable(kDashGlyphKmh, KDASHGLYPHKMH_COUNT);
    out[n].name = "background";
    out[n++].hash = Fnv(gDashAssetBackground, DASH_ASSET_BACKGROUND_W * DASH_ASSET_BACKGROUND_H);
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        const DashAssetWidget* w = &kDashAssetWidgets[i];
        out[n].name = w->id;
        out[n++].hash = Fnv(w->pixels, (size_t) w->w * w->h);
    }
    return n;
}

#define HASH_COUNT (12 + DASH_ASSET_WIDGET_COUNT)

static int SameHashes(const Expected* a, const Expected* b) {
    int i;
    for (i = 0; i < HASH_COUNT; i++) {
        if (strcmp(a[i].name, b[i].name) != 0 || a[i].hash != b[i].hash) {
            return 0;
        }
    }
    return 1;
}

/* Recorded when every buffer matched the replaced headers byte for byte (166,060 words). */
static const Expected kExpected[HASH_COUNT] = {
    {"timer", 0x1EEF1AB3u},      {"lap", 0x58CD03ACu},       {"posdigit", 0x146B05BEu},
    {"possuffix", 0x3B2FAED4u},  {"font1", 0x26A4630Eu},     {"font2", 0x0C89D6B7u},
    {"portrait", 0xEDF79F40u},   {"kostar", 0x57E5F955u},    {"lives", 0x0F4D4648u},
    {"speeddigit", 0xC82FDBBAu}, {"kmh", 0x3CC21495u},       {"background", 0x1B74EE80u},
    {"header", 0x0EAD2D5Eu},     {"standings", 0x119AC89Au}, {"lap", 0xA8E97F4Fu},
    {"timer", 0xD782F9F5u},      {"gap", 0xE532387Bu},       {"energy_bar", 0x1DB70EAEu},
};

static int GlyphsBlank(void) {
    int i;
    size_t j;
    const DashGlyph* const tables[] = {kDashGlyphTimer,  kDashGlyphLap,      kDashGlyphPosDigit,   kDashGlyphPosSuffix,
                                       kDashGlyphFont1,  kDashGlyphFont2,    kDashGlyphPortrait,   kDashGlyphKoStar,
                                       kDashGlyphLives,  kDashGlyphSpeedDigit, kDashGlyphKmh};
    const int counts[] = {KDASHGLYPHTIMER_COUNT,      KDASHGLYPHLAP_COUNT,    KDASHGLYPHPOSDIGIT_COUNT,
                          KDASHGLYPHPOSSUFFIX_COUNT,  KDASHGLYPHFONT1_COUNT,  KDASHGLYPHFONT2_COUNT,
                          KDASHGLYPHPORTRAIT_COUNT,   KDASHGLYPHKOSTAR_COUNT, KDASHGLYPHLIVES_COUNT,
                          KDASHGLYPHSPEEDDIGIT_COUNT, KDASHGLYPHKMH_COUNT};
    for (i = 0; i < (int) (sizeof(counts) / sizeof(counts[0])); i++) {
        int g;
        for (g = 0; g < counts[i]; g++) {
            for (j = 0; j < (size_t) tables[i][g].w * tables[i][g].h; j++) {
                if (tables[i][g].pixels[j] != 0) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

/* The template's #17080f base, RGB565-quantised, everywhere. */
static int BackgroundFlat(unsigned short colour) {
    size_t j;
    for (j = 0; j < DASH_ASSET_BACKGROUND_W * DASH_ASSET_BACKGROUND_H; j++) {
        if (gDashAssetBackground[j] != colour) {
            return 0;
        }
    }
    return 1;
}

/* Before any bake: no arena, every glyph reads the shared blank, nothing else has pixels. */
static int Unbaked(void) {
    int i;
    if (gdx3ds_hudbake_ready() || !GlyphsBlank() || gDashAssetBackground != NULL) {
        return 0;
    }
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        if (kDashAssetWidgets[i].pixels != NULL) {
            return 0;
        }
    }
    return 1;
}

static int AllZero(void) {
    int i;
    size_t j;
    if (!GlyphsBlank() || !BackgroundFlat(0)) {
        return 0;
    }
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        for (j = 0; j < (size_t) kDashAssetWidgets[i].w * kDashAssetWidgets[i].h; j++) {
            if (kDashAssetWidgets[i].pixels[j] != 0) {
                return 0;
            }
        }
    }
    return 1;
}

static size_t ReadNothing(const char* key, void* buf, size_t cap, void* user) {
    (void) key, (void) buf, (void) cap, (void) user;
    return 0;
}

/* Every record is an OTEX header claiming an 8x8 CI8 texture: the wrong format everywhere. */
static size_t ReadWrongFormat(const char* key, void* buf, size_t cap, void* user) {
    static const unsigned char kHeader[0x50] = {
        [0x04] = 'X', [0x05] = 'E', [0x06] = 'T', [0x07] = 'O', /* "OTEX", little-endian */
        [0x40] = 4,   [0x44] = 8,   [0x48] = 8,   [0x4C] = 64,
    };
    (void) key, (void) user;
    if (cap < sizeof(kHeader) + 64) {
        return 0;
    }
    memset(buf, 0x5A, sizeof(kHeader) + 64);
    memcpy(buf, kHeader, sizeof(kHeader));
    return sizeof(kHeader) + 64;
}

/* The real records, cut to their header plus half the payload. */
static size_t ReadTruncated(const char* key, void* buf, size_t cap, void* user) {
    size_t n = gdx3ds_o2r_host_read(key, buf, cap, user);
    return n > 0x50 ? 0x50 + (n - 0x50) / 2 : n;
}

static int gLogLines = 0;
static char gLastLog[160];
static void CountLog(const char* line, void* user) {
    (void) user;
    if (strncmp(line, "[hudbake] ", 10) == 0) {
        gLogLines++;
        snprintf(gLastLog, sizeof(gLastLog), "%s", line);
    }
}

/* The widgets' procedural-only chrome, as every failure bakes it (recorded by the first one). */
static Expected gChromeOnly[HASH_COUNT];
static int gChromeOnlySet = 0;

static void TestFailsGracefully(GdxHudBakeReadFn read, const char* what) {
    GdxHudBakeStats st;
    Expected got[HASH_COUNT];
    int missing;
    gLogLines = 0;
    missing = gdx3ds_hudbake_run(read, NULL, CountLog, NULL, &st);
    CHECK(missing == TEXTURES_PER_BAKE && st.missing == missing && st.textures == TEXTURES_PER_BAKE,
          "%s: missing=%d of %d textures (want all %d)", what, missing, st.textures, TEXTURES_PER_BAKE);
    CHECK(gLogLines >= TEXTURES_PER_BAKE, "%s: %d [hudbake] log lines, want one per texture", what, gLogLines);
    CHECK(GlyphsBlank(), "%s: a glyph is not blank", what);
    CHECK(BackgroundFlat(6210u), "%s: the backdrop is not the flat base colour", what);
    CHECK(gdx3ds_hudbake_done(), "%s: not marked done", what);
    CollectHashes(got);
    if (!gChromeOnlySet) {
        memcpy(gChromeOnly, got, sizeof(got));
        gChromeOnlySet = 1;
    }
    CHECK(SameHashes(got, gChromeOnly), "%s: bakes differently from the other failures", what);
}

static void TestArchive(void) {
    Expected got[HASH_COUNT];
    Expected again[HASH_COUNT];
    GdxHudBakeStats st;
    int i;
    if (!gdx3ds_o2r_host_open()) {
        return;
    }
    TestFailsGracefully(ReadTruncated, "truncated records");
    CHECK(gdx3ds_hudbake_run(gdx3ds_o2r_host_read, NULL, NULL, NULL, &st) == 0, "%d of %d textures missing",
          st.missing, st.textures);
    CHECK(st.textures == TEXTURES_PER_BAKE, "read %d textures", st.textures);
    CHECK(CollectHashes(got) == HASH_COUNT, "hash count");
    for (i = 0; i < HASH_COUNT; i++) {
        CHECK(strcmp(got[i].name, kExpected[i].name) == 0 && got[i].hash == kExpected[i].hash,
              "%s: hash 0x%08Xu, expected 0x%08Xu", got[i].name, got[i].hash, kExpected[i].hash);
    }
    CHECK(gdx3ds_hudbake_digest() == GDX3DS_HUDBAKE_DIGEST_RETAIL, "digest 0x%08Xu, expected 0x%08Xu",
          gdx3ds_hudbake_digest(), GDX3DS_HUDBAKE_DIGEST_RETAIL);
    gdx3ds_hudbake_run(gdx3ds_o2r_host_read, NULL, NULL, NULL, NULL);
    CollectHashes(again);
    CHECK(SameHashes(got, again), "a second bake differs");
}

/* ---- the SD cache ---- */

static int gFingerprintSalt = 0;
static const char* gFingerprintDrop = NULL;

static int SaltedFingerprint(const char* key, unsigned int* fingerprint, void* user) {
    int found = gdx3ds_o2r_host_fingerprint(key, fingerprint, user);
    if (gFingerprintDrop != NULL && strcmp(key, gFingerprintDrop) == 0) {
        return 0;
    }
    if (found && gFingerprintSalt != 0 && strstr(key, "Portrait") == NULL) {
        *fingerprint ^= (unsigned int) gFingerprintSalt;
    }
    return found;
}

static void BakeBlank(void) {
    gdx3ds_hudbake_run(NULL, NULL, NULL, NULL, NULL);
}

static int LoadCache(const unsigned char* data, size_t n) {
    gLogLines = 0;
    return gdx3ds_hudbake_cache_read(data, n, SaltedFingerprint, NULL, CountLog, NULL);
}

static void TestCache(void) {
    unsigned char* data;
    unsigned char* copy;
    size_t need;
    size_t len;
    unsigned blankDigest;
    if (!gdx3ds_o2r_host_open()) {
        return;
    }
    BakeBlank();
    CHECK(gdx3ds_hudbake_cache_write(NULL, 0, SaltedFingerprint, NULL) == 0, "a blank bake is cacheable");
    blankDigest = gdx3ds_hudbake_digest();
    gdx3ds_hudbake_run(gdx3ds_o2r_host_read, NULL, NULL, NULL, NULL);
    need = gdx3ds_hudbake_cache_write(NULL, 0, SaltedFingerprint, NULL);
    CHECK(need > 0, "a complete bake is not cacheable");
    if (need == 0) {
        return;
    }
    data = (unsigned char*) malloc(need);
    copy = (unsigned char*) malloc(need);
    CHECK(gdx3ds_hudbake_cache_write(data, need - 1, SaltedFingerprint, NULL) == 0, "wrote into a short buffer");
    len = gdx3ds_hudbake_cache_write(data, need, SaltedFingerprint, NULL);
    CHECK(len > 0 && len <= need, "cache write %zu of at most %zu bytes", len, need);
    printf("note: cache file %zu bytes\n", len);

    BakeBlank();
    CHECK(LoadCache(data, len) == 1, "a fresh cache was rejected");
    CHECK(gdx3ds_hudbake_digest() == GDX3DS_HUDBAKE_DIGEST_RETAIL, "cache load digest 0x%08X",
          gdx3ds_hudbake_digest());
    CHECK(gLogLines == 0, "an accepted cache logged %d lines", gLogLines);

    /* Every rejection leaves the (blank) buffers as they were and says why. */
    BakeBlank();
    gFingerprintSalt = 0x5A5A;
    CHECK(LoadCache(data, len) == 0 && gLogLines == 1, "a changed archive was accepted");
    gFingerprintSalt = 0;
    gFingerprintDrop = "hud_gfx/aHudLivesLeftIconTex";
    CHECK(LoadCache(data, len) == 0, "a cache naming a missing record was accepted");
    gFingerprintDrop = NULL;
    CHECK(LoadCache(data, len - 2) == 0, "a truncated cache was accepted");
    CHECK(LoadCache(data, 8) == 0, "an 8-byte cache was accepted");
    memcpy(copy, data, len);
    copy[len - 100] ^= 1u;
    CHECK(LoadCache(copy, len) == 0, "a damaged pixel passed the checksum");
    CHECK(!gdx3ds_hudbake_done() && AllZero(), "a damaged cache left pixels behind (it must clear them)");
    memcpy(copy, data, len);
    copy[len / 2] ^= 0x80u;
    CHECK(LoadCache(copy, len) == 0, "a damaged token passed");
    CHECK(!gdx3ds_hudbake_done() && AllZero(), "a damaged token left pixels behind");
    /* A damaged load clears the arena (the caller bakes next); the stale-cache checks below
     * start again from a blank bake and must leave it untouched. */
    BakeBlank();
    memcpy(copy, data, len);
    copy[0] ^= 1u;
    CHECK(LoadCache(copy, len) == 0, "a bad magic was accepted");
    CHECK(strcmp(gLastLog, "[hudbake] cache not used: not a cache file") == 0, "bad magic logged \"%s\"", gLastLog);
    /* ship6: an older build's cache (card-5 wrote v2) is an old format, not "not a cache file" */
    memcpy(copy, data, len);
    {
        uint32_t version;
        char want[96];
        memcpy(&version, copy + 8, sizeof(version));
        snprintf(want, sizeof(want), "[hudbake] cache not used: old cache format (v%lu, want v%lu)",
                 (unsigned long) (version - 1u), (unsigned long) version);
        version -= 1u;
        memcpy(copy + 8, &version, sizeof(version));
        CHECK(LoadCache(copy, len) == 0, "an old-format cache was accepted");
        CHECK(strcmp(gLastLog, want) == 0, "old format logged \"%s\", expected \"%s\"", gLastLog, want);
    }
    memcpy(copy, data, len);
    copy[12] ^= 1u; /* recipeTag */
    CHECK(LoadCache(copy, len) == 0, "another build's cache was accepted");
    CHECK(gdx3ds_hudbake_digest() == blankDigest, "a rejected cache touched the buffers");
    CHECK(gdx3ds_hudbake_cache_read(data, len, NULL, NULL, NULL, NULL) == 0, "accepted without fingerprints");
    free(data);
    free(copy);
}

/* ---- the allocator hook: every byte through it, every transient one returned ---- */

static long gAllocLive = 0;
static long gAllocCalls = 0;
static size_t gAllocBytesLive = 0;

typedef struct AllocHdr {
    size_t n;
    size_t pad;
} AllocHdr;

static void* CountingAlloc(size_t n) {
    AllocHdr* h = (AllocHdr*) malloc(sizeof(AllocHdr) + n);
    if (h == NULL) {
        return NULL;
    }
    h->n = n;
    gAllocLive++;
    gAllocCalls++;
    gAllocBytesLive += n;
    return h + 1;
}

static void CountingFree(void* p) {
    AllocHdr* h = (AllocHdr*) p - 1;
    gAllocLive--;
    gAllocBytesLive -= h->n;
    free(h);
}

/* After any bake / cache operation, only the arena is still allocated. */
static void CheckOnlyArenaLive(const char* what) {
    CHECK(gAllocLive == 1 && gAllocBytesLive == gdx3ds_hudbake_arena_bytes(),
          "%s: %ld blocks / %zu bytes still allocated (want only the %zu-byte arena)", what, gAllocLive,
          gAllocBytesLive, gdx3ds_hudbake_arena_bytes());
}

/* gdx3ds_hudbake_list_keys must be exactly what a bake reads, in order (the console prefetches
 * that list), with or without an archive behind the reader. */
static void TestKeyList(void) {
    static char keys[200][GDX3DS_HUDBAKE_KEY_LEN];
    const int n = gdx3ds_hudbake_list_keys(keys, 200);
    int i;
    gdx3ds_hudbake_run(ReadNothing, NULL, NULL, NULL, NULL);
    CHECK(n == TEXTURES_PER_BAKE, "list_keys gave %d keys, want %d", n, TEXTURES_PER_BAKE);
    for (i = 0; i < n; i++) {
        const char* read = gdx3ds_hudbake_read_key(i);
        CHECK(read != NULL && strcmp(read, keys[i]) == 0, "key %d: listed %s, the bake read %s", i, keys[i],
              read != NULL ? read : "(nothing)");
    }
    CHECK(gdx3ds_hudbake_read_key(n) == NULL, "the bake read more than %d keys", n);
}

int main(void) {
    gdx3ds_hudbake_set_allocator(CountingAlloc, CountingFree);
    CHECK(!gdx3ds_hudbake_done(), "done before any bake");
    CHECK(Unbaked(), "pixels exist before the first bake");
    CHECK(gAllocCalls == 0, "allocated before the first bake");
    TestKeyList();
    CHECK(gdx3ds_hudbake_ready(), "no arena after a bake");
    CheckOnlyArenaLive("a bake with nothing to read");
    TestFailsGracefully(NULL, "no reader");
    TestFailsGracefully(ReadNothing, "no archive");
    TestFailsGracefully(ReadWrongFormat, "wrong format");
    TestFailsGracefully(ReadTruncated, "truncated");
    CheckOnlyArenaLive("the failure bakes");
    TestArchive();
    CheckOnlyArenaLive("the archive bakes");
    TestCache();
    CheckOnlyArenaLive("the cache round trips");
    if (gFailures == 0) {
        printf("=== ALL PASS (%d checks) ===\n", gChecks);
        return 0;
    }
    printf("=== %d FAILURES (%d checks) ===\n", gFailures, gChecks);
    return 1;
}
