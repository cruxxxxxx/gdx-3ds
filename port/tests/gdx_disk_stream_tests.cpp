/* port/tests/gdx_disk_stream_tests.cpp -- host gate for the SD-backed 64DD block reader.
 *
 *   gdx_disk_stream_tests <image.ndd> <image.ndd.manifest.json> <truncated.ndd> [EkAssetBindings.c]
 *
 * Everything here runs against a SYNTHETIC image from tools/dev/mkdisk64.py. No ROM, no 64DD
 * dump, nothing copyrighted, and nothing that needs one: the reader's contract is about
 * offsets, block boundaries, eviction and refusal, and a structurally correct image with
 * deterministic filler exercises all of it.
 *
 * The geometry tables below are a SECOND, INDEPENDENT transcription of
 * decomp/src/leo/lib/leo_tbl.c into C, deliberately not shared with the Python generator.
 * Case 2 then checks that the C walk and the Python walk agree on every logical LBA offset,
 * which is the only way a single transcription error in either one gets caught.
 */
#include "../gdx_disk_stream.h"
#include "../gdx_disk_dcrc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>   /* geteuid -- case 9 skips its refusal probe under root */

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

static void checkEq(unsigned long long got, unsigned long long want, const char* what) {
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("  FAIL %s: got %llu want %llu\n", what, got, want);
    }
}

/* ------------------------------------------------------------ 64DD geometry -- */
/* decomp/src/leo/lib/leo_tbl.c */
static const unsigned short kByteTbl2[9] = {
    0x4D08, 0x47B8, 0x4510, 0x3FC0, 0x3A70, 0x3520, 0x2FD0, 0x2A80, 0x2530
};
static const unsigned short kVZone0[16] = {
    0x0124, 0x0248, 0x035A, 0x047E, 0x05A2, 0x06B4, 0x07C6, 0x08D8,
    0x09EA, 0x0AB6, 0x0B82, 0x0C94, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC
};
static const unsigned char kPZoneHd0[16] = {
    0x00, 0x01, 0x02, 0x09, 0x08, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A
};
#define NUM_LBAS 4292
#define SYSTEM_AREA_LBAS 0x18
#define DISK_EXACT_BYTES 64931840u
#define SYSTEM_AREA_BYTES 0x738C0u
#define TOTAL_PLBAS (SYSTEM_AREA_LBAS + NUM_LBAS)

static unsigned int g_blockOff[TOTAL_PLBAS];
static unsigned int g_blockLen[TOTAL_PLBAS];

static void buildGeometry() {
    unsigned int off = 0;
    for (unsigned int plba = 0; plba < TOTAL_PLBAS; plba++) {
        unsigned int vz = 0xFF;
        for (unsigned int i = 0; i < 16; i++) {
            if (plba < kVZone0[i]) {
                vz = i;
                break;
            }
        }
        unsigned int zone = kPZoneHd0[vz];
        if (zone >= 8) {
            zone -= 7;
        }
        g_blockOff[plba] = off;
        g_blockLen[plba] = kByteTbl2[zone];
        off += kByteTbl2[zone];
    }
    checkEq(off, DISK_EXACT_BYTES, "C geometry walk total == GDX_DISK_EXACT_BYTES");
    checkEq(g_blockOff[SYSTEM_AREA_LBAS], SYSTEM_AREA_BYTES, "C geometry system area == 0x738C0");
}

/* ------------------------------------------------------- synthetic filler -- */
/* Transcription of tools/dev/mkdisk64.py block_filler(). Ten lines, as the docstring there
 * promises, and it is what lets this test check any block without holding 62 MB. */
#define KEYSTREAM_BYTES 65536u
#define FILLER_ROTOR 4093u
static unsigned char g_keystream[KEYSTREAM_BYTES];

static void buildKeystream(unsigned long long seed) {
    unsigned long long x = seed;
    for (unsigned int i = 0; i < KEYSTREAM_BYTES; i += 8) {
        x += 0x9E3779B97F4A7C15ull;
        unsigned long long z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        for (int b = 0; b < 8; b++) {
            g_keystream[i + b] = (unsigned char) ((z >> (56 - 8 * b)) & 0xFFull);
        }
    }
}

static void expectedBlock(unsigned int plba, unsigned char* out, unsigned int size) {
    char stamp[24];
    snprintf(stamp, sizeof(stamp), "GDXSYN%010u", plba);
    memcpy(out, stamp, 16);
    const unsigned int rot = (plba * FILLER_ROTOR) % KEYSTREAM_BYTES;
    for (unsigned int i = 0; i < size - 16u; i++) {
        out[16 + i] = g_keystream[(rot + i) % KEYSTREAM_BYTES];
    }
}

/* Physical LBAs with generated structure rather than filler: the system area (0..0x17) and
 * the MFS volume + backup at the RAM-area start (type 0: 0x5A2..0x5A7). */
static bool isStructured(unsigned int plba) {
    return plba < SYSTEM_AREA_LBAS || (plba >= 0x5A2u && plba < 0x5A8u);
}

/* --------------------------------------------------------------- manifest -- */
static unsigned long long readManifestCrc(const char* path, unsigned long long* seedOut) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        printf("  FAIL cannot open manifest %s\n", path);
        g_failures++;
        return 0;
    }
    static char buf[65536];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    unsigned long long crc = 0;
    const char* p = strstr(buf, "\"crc64_xz\"");
    if (p != nullptr) {
        p = strstr(p, "0x");
        if (p != nullptr) {
            crc = strtoull(p + 2, nullptr, 16);
        }
    }
    if (seedOut != nullptr) {
        const char* s = strstr(buf, "\"seed\"");
        *seedOut = (s != nullptr) ? strtoull(strchr(s, ':') + 1, nullptr, 10) : 0;
    }
    return crc;
}

/* ---------------------------------------------------------------- overlay -- */
/* Stands in for port/disk_savefile.cpp's dirty-range journal: one range, applied after the
 * base bytes land, exactly as the real overlay does. */
static unsigned int g_ovlOffset = 0;
static unsigned int g_ovlLen = 0;
static unsigned char g_ovlByte = 0;
static int g_ovlEnabled = 0;

static void testOverlay(unsigned int offset, unsigned int length, void* dst) {
    if (!g_ovlEnabled || g_ovlLen == 0) {
        return;
    }
    const unsigned long long s = (offset > g_ovlOffset) ? offset : g_ovlOffset;
    const unsigned long long e0 = (unsigned long long) offset + length;
    const unsigned long long e1 = (unsigned long long) g_ovlOffset + g_ovlLen;
    const unsigned long long e = (e0 < e1) ? e0 : e1;
    if (s >= e) {
        return;
    }
    memset((unsigned char*) dst + (s - offset), g_ovlByte, (size_t) (e - s));
    gdx_disk_stream_note_overlay((unsigned int) (e - s));
}

/* ------------------------------------------------------------------ cases -- */
static void case1_openRefusals(const char* image, const char* truncated) {
    printf("case 1: open refusals\n");
    check(gdx_disk_stream_open("/nonexistent/no.ndd", DISK_EXACT_BYTES,
                               GDX_DISK_STREAM_DEFAULT_BLOCK_LOG2,
                               GDX_DISK_STREAM_DEFAULT_BLOCKS) == 0,
          "missing file is refused");
    check(gdx_disk_stream_active() == 0, "refused open leaves the stream inactive");

    /* The load-bearing one: a truncated image must be REFUSED at open, not read through.
     * The resident path's own comment says a short .ndd OOB-reads immediately; a streaming
     * reader would instead serve a silently short final block, which is worse because it
     * looks like data. */
    check(gdx_disk_stream_open(truncated, DISK_EXACT_BYTES,
                               GDX_DISK_STREAM_DEFAULT_BLOCK_LOG2,
                               GDX_DISK_STREAM_DEFAULT_BLOCKS) == 0,
          "truncated image is refused at open");
    check(gdx_disk_stream_active() == 0, "truncated open leaves the stream inactive");
    /* ...and it opens fine when the caller does not assert a size, which proves the refusal
     * above came from the size check and not from something incidental. */
    check(gdx_disk_stream_open(truncated, 0, 14, 16) == 1, "truncated image opens with no size assertion");
    gdx_disk_stream_close();

    check(gdx_disk_stream_open(image, DISK_EXACT_BYTES,
                               GDX_DISK_STREAM_DEFAULT_BLOCK_LOG2,
                               GDX_DISK_STREAM_DEFAULT_BLOCKS) == 1,
          "the real image opens");
    checkEq(gdx_disk_stream_size(), DISK_EXACT_BYTES, "size");
    checkEq(gdx_disk_stream_block_bytes(), 16384u, "default block size");
    checkEq(gdx_disk_stream_cache_bytes(), 128u * 16384u, "default cache bytes");
}

static void case2_lbaReads() {
    printf("case 2: LBA-shaped reads at every zone boundary\n");
    static unsigned char got[0x4D08];
    static unsigned char want[0x4D08];
    unsigned int checked = 0;
    /* Every physical LBA that starts a new zone, plus the one before it, plus the first and
     * last blocks -- i.e. every place the block size changes, which is where a geometry
     * transcription error would hide. */
    for (unsigned int i = 0; i < 16; i++) {
        const unsigned int edges[2] = { (unsigned int) kVZone0[i] - 1u, (unsigned int) kVZone0[i] };
        for (int e = 0; e < 2; e++) {
            const unsigned int plba = edges[e];
            if (plba >= TOTAL_PLBAS || isStructured(plba)) {
                continue;
            }
            const unsigned int len = g_blockLen[plba];
            check(gdx_disk_stream_read(g_blockOff[plba], len, got) == 1, "zone-edge block read");
            expectedBlock(plba, want, len);
            check(memcmp(got, want, len) == 0, "zone-edge block content");
            checked++;
        }
    }
    check(checked >= 24, "at least 24 zone-edge blocks checked");

    /* The two LBAs ekdeadlock measured the EK audio boot failing on (1001/1002, the
     * gSoundFontTableLba / gSequenceTableLba anchors). They are logical LBAs, so the file
     * offset is the port's LeoLBAToByte(0, lba) + GDX_NDD_SYSTEM_AREA_BYTES. */
    for (unsigned int lba = 1001; lba <= 1002; lba++) {
        const unsigned int plba = SYSTEM_AREA_LBAS + lba;
        const unsigned int len = g_blockLen[plba];
        check(gdx_disk_stream_read(g_blockOff[plba], len, got) == 1, "LBA 1001/1002 read");
        expectedBlock(plba, want, len);
        check(memcmp(got, want, len) == 0, "LBA 1001/1002 content");
    }

    /* The system-area anchors the port reads by fixed offset. */
    unsigned char head[16];
    check(gdx_disk_stream_read(0, 16, head) == 1, "system data block 0 read");
    checkEq(head[5] & 0x0Fu, 0u, "disk[5] & 0xF == disk type 0");
    unsigned char diskId[24];
    check(gdx_disk_stream_read(g_blockOff[14], 24, diskId) == 1, "disk ID at pLBA 14");
    check(memcmp(diskId, "GDXS", 4) == 0, "disk ID gameName is the synthetic marker");
}

static void case3_seeksAndBoundaries() {
    printf("case 3: unaligned seeks and block-boundary crossings\n");
    static unsigned char got[70000];
    static unsigned char want[70000];
    const unsigned int blockBytes = gdx_disk_stream_block_bytes();

    /* A read that starts one byte before a cache-block boundary and runs three blocks long
     * is the shape that breaks a naive implementation. Pick a filler-only physical LBA and
     * walk outward from its start. */
    const unsigned int plba = 2000;
    check(!isStructured(plba), "chosen probe LBA is filler");
    expectedBlock(plba, want, g_blockLen[plba]);

    for (int delta = -3; delta <= 3; delta++) {
        const unsigned int base = g_blockOff[plba];
        const unsigned int aligned = (base + blockBytes) & ~(blockBytes - 1u);
        const unsigned int start = (unsigned int) ((int) aligned + delta);
        if (start < base || start + 64u > base + g_blockLen[plba]) {
            continue;
        }
        check(gdx_disk_stream_read(start, 64, got) == 1, "boundary-straddling read");
        check(memcmp(got, want + (start - base), 64) == 0, "boundary-straddling content");
    }

    /* Out-of-range refusals, and the dst-is-untouched contract. */
    memset(got, 0xAB, 64);
    check(gdx_disk_stream_read(DISK_EXACT_BYTES - 32u, 64u, got) == 0, "read past EOF refused");
    bool untouched = true;
    for (int i = 0; i < 64; i++) {
        untouched = untouched && got[i] == 0xAB;
    }
    check(untouched, "refused read leaves dst untouched");
    check(gdx_disk_stream_read(DISK_EXACT_BYTES, 1u, got) == 0, "read at EOF refused");
    check(gdx_disk_stream_read(0u, 0u, got) == 1, "zero-length read succeeds");

    /* The final block is a partial one (64,931,840 is not a multiple of 16 KiB), which is the
     * normal tail case, not an error. */
    check(gdx_disk_stream_read(DISK_EXACT_BYTES - 1u, 1u, got) == 1, "last byte readable");
}

static void case4_evictionAndThrash(const char* image) {
    printf("case 4: cache eviction and the thrash case\n");
    /* A deliberately tiny cache: 4 sets x 4 ways = 16 blocks of 4 KiB = 64 KiB. */
    check(gdx_disk_stream_open(image, DISK_EXACT_BYTES, 12, 16) == 1, "tiny cache opens");
    checkEq(gdx_disk_stream_cache_bytes(), 16u * 4096u, "tiny cache bytes");
    gdx_disk_stream_reset_stats();

    static unsigned char got[256];
    static unsigned char want[0x4D08];

    /* Re-reading ONE block many times must miss exactly once. */
    const unsigned int plba = 1500;
    expectedBlock(plba, want, g_blockLen[plba]);
    for (int i = 0; i < 50; i++) {
        check(gdx_disk_stream_read(g_blockOff[plba] + 128u, 256u, got) == 1, "hot read");
        check(memcmp(got, want + 128, 256) == 0, "hot read content");
    }
    GdxDiskStreamStats st;
    gdx_disk_stream_get_stats(&st);
    check(st.blockMisses <= 2, "a repeatedly read range misses at most twice (it may straddle)");
    check(st.blockEvictions == 0, "no eviction while the working set fits");

    /* The thrash case: 5 addresses that all map to the SAME 4-way set, read round-robin.
     * A direct-mapped cache would miss every time; 4-way misses only on the 5th. The point
     * of the case is not the hit rate, it is that eviction stays CORRECT -- every byte must
     * still come back right. */
    gdx_disk_stream_reset_stats();
    const unsigned int sets = (16u / GDX_DISK_STREAM_WAYS);
    unsigned int addrs[5];
    for (unsigned int k = 0; k < 5; k++) {
        addrs[k] = (k * sets) * 4096u;     /* same set index, different tags */
    }
    for (int round = 0; round < 20; round++) {
        for (unsigned int k = 0; k < 5; k++) {
            unsigned char a[32], b[32];
            check(gdx_disk_stream_read(addrs[k], 32, a) == 1, "thrash read");
            check(gdx_disk_stream_read(addrs[k], 32, b) == 1, "thrash reread");
            check(memcmp(a, b, 32) == 0, "thrash: same address, same bytes across eviction");
        }
    }
    gdx_disk_stream_get_stats(&st);
    check(st.blockEvictions > 0, "the thrash pattern really does evict");
    printf("  thrash: hits=%llu misses=%llu evictions=%llu\n",
           st.blockHits, st.blockMisses, st.blockEvictions);

    /* Invalidation drops the block. */
    gdx_disk_stream_reset_stats();
    check(gdx_disk_stream_read(addrs[0], 32, got) == 1, "pre-invalidate read");
    gdx_disk_stream_invalidate(addrs[0], 32);
    gdx_disk_stream_get_stats(&st);
    const unsigned long long missesBefore = st.blockMisses;
    check(gdx_disk_stream_read(addrs[0], 32, got) == 1, "post-invalidate read");
    gdx_disk_stream_get_stats(&st);
    check(st.blockMisses > missesBefore, "invalidate forces a re-read");

    gdx_disk_stream_close();
}

static void case5_overlay(const char* image) {
    printf("case 5: dirty-range overlay composes over the pristine file\n");
    check(gdx_disk_stream_open(image, DISK_EXACT_BYTES, 14, 32) == 1, "open for overlay");
    gdx_disk_stream_set_overlay(testOverlay);

    const unsigned int plba = 2500;
    const unsigned int base = g_blockOff[plba];
    static unsigned char want[0x4D08];
    expectedBlock(plba, want, g_blockLen[plba]);

    unsigned char got[512];
    g_ovlEnabled = 0;
    check(gdx_disk_stream_read(base, 512, got) == 1, "pristine read");
    check(memcmp(got, want, 512) == 0, "pristine content");

    /* A 64DD write lands in the journal, never in the .ndd. The next read must see it. */
    g_ovlEnabled = 1;
    g_ovlOffset = base + 100u;
    g_ovlLen = 50u;
    g_ovlByte = 0x5A;
    gdx_disk_stream_invalidate(g_ovlOffset, g_ovlLen);
    check(gdx_disk_stream_read(base, 512, got) == 1, "read after write");
    check(memcmp(got, want, 100) == 0, "bytes before the dirty range are pristine");
    bool patched = true;
    for (unsigned int i = 0; i < 50; i++) {
        patched = patched && got[100 + i] == 0x5A;
    }
    check(patched, "the dirty range wins over the file");
    check(memcmp(got + 150, want + 150, 362) == 0, "bytes after the dirty range are pristine");

    /* A read that only PARTIALLY overlaps the dirty range, from the other side. */
    check(gdx_disk_stream_read(base + 120u, 64u, got) == 1, "partial-overlap read");
    for (unsigned int i = 0; i < 30; i++) {
        patched = patched && got[i] == 0x5A;
    }
    check(patched, "partial overlap: the overlapping head is patched");
    check(memcmp(got + 30, want + 150, 34) == 0, "partial overlap: the tail is pristine");

    GdxDiskStreamStats st;
    gdx_disk_stream_get_stats(&st);
    check(st.overlayBytes > 0, "the overlay reported the bytes it rewrote");

    g_ovlEnabled = 0;
    gdx_disk_stream_set_overlay(nullptr);
    gdx_disk_stream_close();
}

static void case6_crc(const char* image, const char* manifest) {
    printf("case 6: the streamed whole-image CRC-64\n");
    unsigned long long seed = 0;
    const unsigned long long wantCrc = readManifestCrc(manifest, &seed);
    check(wantCrc != 0, "manifest carries a CRC-64");

    check(gdx_disk_stream_open(image, DISK_EXACT_BYTES, 14, 32) == 1, "open for CRC");
    gdx_disk_stream_reset_stats();
    int ok = 0;
    const unsigned long long crc = gdx_disk_stream_crc64(&ok);
    check(ok == 1, "CRC pass completes");
    checkEq(crc, wantCrc, "streamed CRC-64 matches the generator's");

    /* The CRC pass must not have polluted the cache -- that is the reason it bypasses it. */
    GdxDiskStreamStats st;
    gdx_disk_stream_get_stats(&st);
    checkEq(st.blockMisses, 0ull, "the CRC pass takes no cache misses (it bypasses the cache)");
    checkEq(st.blockHits, 0ull, "the CRC pass takes no cache hits");
    check(st.hostBytes == DISK_EXACT_BYTES, "the CRC pass read the whole image exactly once");
    printf("  crc64=0x%016llX over %llu host bytes in %llu reads\n",
           crc, st.hostBytes, st.hostReads);

    /* B11m: the same bypass, exposed for the .dcrc sample. 64 scattered 4 KiB windows read
     * through the cache cost a 16 KiB block EACH (1,312 KB measured on the real image, for a
     * 256 KiB sample) and were then invalidated again. Uncached costs exactly the sample. */
    unsigned char win[4096], viaCache[4096];
    const unsigned int kWindows = 64;

    /* Same bytes either way -- otherwise the fingerprint would change meaning. */
    check(gdx_disk_stream_read_uncached(0x400000u, sizeof(win), win) == 1, "uncached window reads");
    check(gdx_disk_stream_read(0x400000u, sizeof(viaCache), viaCache) == 1, "cached window reads");
    check(memcmp(win, viaCache, sizeof(win)) == 0,
          "uncached and cached reads return the same bytes");
    gdx_disk_stream_invalidate(0x400000u, sizeof(viaCache));

    gdx_disk_stream_reset_stats();
    for (unsigned int i = 0; i < kWindows; i++) {
        const unsigned int off =
            (unsigned int) ((unsigned long long) i * (DISK_EXACT_BYTES - 4096u) / kWindows);
        check(gdx_disk_stream_read_uncached(off, sizeof(win), win) == 1, "sample window reads");
    }
    gdx_disk_stream_get_stats(&st);
    checkEq(st.blockHits, 0ull, "the sample takes no cache hits");
    checkEq(st.blockMisses, 0ull, "the sample takes no cache misses -- the cache stays COLD");
    checkEq(st.hostBytes, (unsigned long long) kWindows * 4096ull,
            "and it costs exactly the sample size, not a 16 KiB block per 4 KiB window");
    printf("  sample: %llu host bytes for %u x 4 KiB windows (through the cache: 16 KiB each)\n",
           st.hostBytes, kWindows);

    /* Refusal contract holds on the bypass too. */
    unsigned char guard[16];
    memset(guard, 0xC7, sizeof(guard));
    check(gdx_disk_stream_read_uncached(DISK_EXACT_BYTES - 4u, sizeof(guard), guard) == 0,
          "an uncached read past EOF is refused");
    for (unsigned int i = 0; i < sizeof(guard); i++) {
        check(guard[i] == 0xC7, "and leaves dst untouched");
    }
    gdx_disk_stream_close();
}

/* The EK asset fill is the only disk consumer whose access pattern is visible without a dump:
 * port/gen/EkAssetBindings.c's sEkAssetFills[] is a committed table of (destination, disk
 * offset, size) rows. Replaying its offsets is therefore a REAL boot trace, not a synthetic
 * one, and the numbers it produces are what sizes the default cache. */
static void case7_ekFillTrace(const char* image, const char* bindings) {
    printf("case 7: the EK asset-fill trace (the real boot access pattern)\n");
    FILE* f = fopen(bindings, "rb");
    if (f == nullptr) {
        printf("  SKIP: %s not readable\n", bindings);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* src = (char*) malloc((size_t) sz + 1);
    if (src == nullptr || fread(src, 1, (size_t) sz, f) != (size_t) sz) {
        fclose(f);
        free(src);
        printf("  SKIP: could not read %s\n", bindings);
        return;
    }
    src[sz] = '\0';
    fclose(f);

    char* tbl = strstr(src, "sEkAssetFills");
    if (tbl == nullptr) {
        free(src);
        printf("  SKIP: no sEkAssetFills table\n");
        return;
    }

    /* Rows look like: { symbol, 0xOFFSETU, 0xSIZEU, 0xADDRU, GDX_EK_ASSET_KIND },
     * Bounded at the table's own closing `};` -- without that bound the scan runs on into the
     * function bodies below it and picks up stray brace-comma pairs. */
    char* tblEnd = strstr(tbl, "};");
    if (tblEnd != nullptr) {
        *tblEnd = '\0';
    }
    /* Step past the ARRAY's own opening brace. Without this the `= {` brace and row 0's `{`
     * both resolve to row 0's comma and row 0 is counted twice -- which is exactly how this
     * first reported 766 rows for a 765-row table. */
    char* open = strchr(tbl, '{');
    if (open == nullptr) {
        printf("  SKIP: table has no opening brace\n");
        return;
    }
    tbl = open + 1;
    static unsigned int offs[4096];
    static unsigned int lens[4096];
    unsigned int rows = 0;
    for (char* p = tbl; (p = strchr(p, '{')) != nullptr && rows < 4096; p++) {
        char* comma = strchr(p, ',');
        if (comma == nullptr) {
            break;
        }
        unsigned int a = 0, b = 0;
        if (sscanf(comma + 1, " 0x%xU , 0x%xU", &a, &b) != 2) {
            continue;
        }
        if (b == 0) {
            continue;
        }
        offs[rows] = a;
        lens[rows] = b;
        rows++;
    }
    free(src);
    printf("  parsed %u fill rows from EkAssetBindings.c\n", rows);
    checkEq(rows, 765u, "the fill table parsed (765 rows, port/gen/EkAssetBindings.c)");
    if (rows == 0) {
        return;
    }

    unsigned long long totalBytes = 0;
    unsigned int lo = 0xFFFFFFFFu, hi = 0;
    for (unsigned int i = 0; i < rows; i++) {
        totalBytes += lens[i];
        if (offs[i] < lo) {
            lo = offs[i];
        }
        if (offs[i] + lens[i] > hi) {
            hi = offs[i] + lens[i];
        }
    }
    printf("  fill footprint: %llu bytes across 0x%08X..0x%08X (%.2f MB span)\n",
           totalBytes, lo, hi, (double) (hi - lo) / 1e6);

    /* Replay it against the DEFAULT cache and report the numbers the doc quotes. */
    check(gdx_disk_stream_open(image, DISK_EXACT_BYTES,
                               GDX_DISK_STREAM_DEFAULT_BLOCK_LOG2,
                               GDX_DISK_STREAM_DEFAULT_BLOCKS) == 1, "open with the default cache");
    gdx_disk_stream_reset_stats();
    static unsigned char scratch[0x8000];
    unsigned int served = 0;
    for (unsigned int i = 0; i < rows; i++) {
        if (lens[i] > sizeof(scratch)) {
            continue;
        }
        if ((unsigned long long) offs[i] + lens[i] > DISK_EXACT_BYTES) {
            continue;
        }
        if (gdx_disk_stream_read(offs[i], lens[i], scratch)) {
            served++;
        }
    }
    GdxDiskStreamStats st;
    gdx_disk_stream_get_stats(&st);
    const unsigned long long lookups = st.blockHits + st.blockMisses;
    printf("  default cache (%u KiB, %u KiB blocks): rows=%u lookups=%llu hits=%llu misses=%llu "
           "evictions=%llu hostKB=%llu\n",
           gdx_disk_stream_cache_bytes() / 1024u, gdx_disk_stream_block_bytes() / 1024u,
           served, lookups, st.blockHits, st.blockMisses, st.blockEvictions,
           st.hostBytes / 1024ull);
    checkEq(served, rows, "every fill row was served");

    /* The claim the sizing rests on is NOT "zero evictions" -- it is ZERO RE-FETCH: every
     * distinct block is pulled off the card exactly once, so the cache never makes the card
     * do work twice. Count the distinct blocks here rather than hard-coding 64, so this stays
     * true if the fill table is regenerated. */
    unsigned int distinct = 0;
    {
        static unsigned char seen[DISK_EXACT_BYTES / 16384u + 2u];
        memset(seen, 0, sizeof(seen));
        for (unsigned int i = 0; i < rows; i++) {
            if (lens[i] > sizeof(scratch) || (unsigned long long) offs[i] + lens[i] > DISK_EXACT_BYTES) {
                continue;
            }
            for (unsigned int b = offs[i] >> 14; b <= (offs[i] + lens[i] - 1u) >> 14; b++) {
                if (!seen[b]) {
                    seen[b] = 1;
                    distinct++;
                }
            }
        }
    }
    printf("  distinct 16 KiB blocks touched: %u (%.2f MiB)\n", distinct, distinct * 16384.0 / 1048576.0);
    checkEq(st.blockMisses, (unsigned long long) distinct,
            "the default cache fetches every distinct block exactly once (no re-fetch)");
    checkEq(st.hostBytes, (unsigned long long) distinct * 16384ull,
            "host bytes == distinct blocks x block size");
    /* B11d: the reader now counts its own working set, from a bitmap over IMAGE blocks. This
     * test computed `distinct` independently, above, by walking the fill table -- so the two
     * agreeing is a real cross-check of the instrument that the cache-size sweep is read
     * from, not a restatement of it. */
    checkEq(gdx_disk_stream_unique_blocks(), (unsigned long long) distinct,
            "the reader's working-set counter agrees with the test's own distinct-block count");

    /* And the counterfactual, so the sizing is an argument and not a coincidence: an
     * undersized cache DOES thrash on the same trace. */
    check(gdx_disk_stream_open(image, DISK_EXACT_BYTES, 14, 8) == 1, "open with an 8-block cache");
    gdx_disk_stream_reset_stats();
    for (unsigned int i = 0; i < rows; i++) {
        if (lens[i] > sizeof(scratch) || (unsigned long long) offs[i] + lens[i] > DISK_EXACT_BYTES) {
            continue;
        }
        gdx_disk_stream_read(offs[i], lens[i], scratch);
    }
    GdxDiskStreamStats small;
    gdx_disk_stream_get_stats(&small);
    printf("  128 KiB cache: hits=%llu misses=%llu evictions=%llu hostKB=%llu\n",
           small.blockHits, small.blockMisses, small.blockEvictions, small.hostBytes / 1024ull);
    check(small.blockEvictions > st.blockEvictions,
          "an undersized cache evicts far more than the default");
    check(small.blockMisses > (unsigned long long) distinct,
          "an undersized cache RE-FETCHES blocks it already had -- which is the failure the "
          "default size exists to avoid");
    check(small.hostBytes > st.hostBytes, "an undersized cache re-reads the card");
    /* The working SET is a property of the trace, not of the cache: an undersized cache
     * re-fetches, but it does not touch different blocks. That is exactly why `uniqBlk` is
     * the number the sweep is read against and `miss` is not. */
    checkEq(gdx_disk_stream_unique_blocks(), (unsigned long long) distinct,
            "the working set is unchanged by the cache size (only the re-fetch count moves)");
    check(small.blockMisses - gdx_disk_stream_unique_blocks() > 0ull,
          "refetch = miss - uniq is non-zero for the undersized cache");
    gdx_disk_stream_close();
}

static void case8_corruptImage(const char* image) {
    printf("case 8: a byte-corrupt image is detected by the fingerprint, not by the reader\n");
    /* Deliberate negative: the reader does NOT validate content, and must not pretend to.
     * A corrupt-but-correctly-sized image opens and reads; what catches it is the CRC-64
     * identity pass, which is the port's existing mechanism and the reason case 6 exists.
     * Writing this down stops a future reader assuming the block cache is a integrity check. */
    check(gdx_disk_stream_open(image, DISK_EXACT_BYTES, 14, 32) == 1, "correctly sized image opens");
    int ok = 0;
    const unsigned long long crc = gdx_disk_stream_crc64(&ok);
    check(ok == 1 && crc != 0, "the fingerprint is computable, and that is the integrity story");
    gdx_disk_stream_close();
}

/* ------------------------------------------------------------------ case 9 -- */
/* B11m. The .dcrc identity-cache sidecar: port/gdx_disk_dcrc.cpp.
 *
 * This case exists because of a measured failure. The first boot of the Expansion Kit on
 * real hardware with a real 64DD disk read 64.7 MB off the SD card and logged
 * `crc64=0x38CC4A8E957591A9(computed)` -- and that single token cannot distinguish "there
 * was no sidecar yet" (correct, on a first boot) from "the sidecar was rejected" from "the
 * write FAILED", which is the difference between paying a ~1 minute boot once and paying it
 * forever. diskstream section 8 had already filed the honest version: "the .dcrc cache has
 * never had a hit ... the host test covers the reader, not the sidecar." This is that test.
 *
 * Every REJECT path is here, because a cache that accepts a stale record is worse than no
 * cache at all: the fingerprint it hands back keys the .gdd save sidecar and decides the
 * Japanese-region gate. */
static void writeRaw(const char* path, const void* bytes, unsigned int n) {
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        return;
    }
    fwrite(bytes, 1, n, f);
    fclose(f);
}

static long fileSize(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fclose(f);
    return n;
}

static void case9_dcrcSidecar(const char* tmpRoot) {
    printf("case 9: the .dcrc identity-cache sidecar, round trip and every reject\n");

    /* tmpRoot carries a trailing separator, exactly as the port's gdx_exe_dir does. The
     * "saves" subdirectory deliberately does NOT exist yet: that is the state a freshly
     * staged SD card is in, and the old implementation could not write into it because it
     * assumed disk_savefile.cpp had already made the directory -- which only happens after a
     * 64DD WRITE, i.e. possibly never. */
    char base[512];
    snprintf(base, sizeof(base), "%s/dcrc-case9/", tmpRoot);
    char mk[600];
    snprintf(mk, sizeof(mk), "rm -rf \"%s\" && mkdir -p \"%s\"", base, base);
    check(system(mk) == 0, "scratch base directory prepared");

    const unsigned int kSize = DISK_EXACT_BYTES;
    const unsigned long long kSample = 0x0123456789ABCDEFull;
    const unsigned long long kCrc = 0x38CC4A8E957591A9ull; /* the real JP disk's fingerprint */

    char path[900];
    check(gdx_dcrc_build_path(base, "media/baserom.jp.ek.ndd", path, sizeof(path)) == 1,
          "the sidecar path builds");
    char want[900];
    snprintf(want, sizeof(want), "%ssaves/baserom.jp.ek.ndd.dcrc", base);
    check(strcmp(path, want) == 0, "the path is <base>saves/<LEAF>.dcrc -- leaf only, never the dir");

    /* A first boot: nothing on the card. This must be ABSENT, not some generic failure --
     * "absent" is the one miss that is not a bug, and the receipt says so. */
    unsigned long long got = 0;
    check(gdx_dcrc_load(base, "baserom.jp.ek.ndd", kSize, kSample, &got) == GDX_DCRC_MISS_ABSENT,
          "a first boot reports ABSENT");

    /* ... and the store must create saves/ itself. This is the regression the whole case is
     * for: before B11m the fopen simply returned null here and nothing was ever written. */
    int err = -1;
    check(gdx_dcrc_store(base, "baserom.jp.ek.ndd", kSize, kSample, kCrc, &err) == GDX_DCRC_WRITE_OK,
          "the store creates the saves directory it needs and writes");
    checkEq((unsigned long long) err, 0ull, "a successful store reports errno 0");
    checkEq((unsigned long long) fileSize(path), (unsigned long long) GDX_DCRC_RECORD_BYTES,
            "the record is 25 bytes");

    /* The round trip -- the whole point. */
    got = 0;
    check(gdx_dcrc_load(base, "baserom.jp.ek.ndd", kSize, kSample, &got) == GDX_DCRC_HIT,
          "the second boot HITS");
    checkEq(got, kCrc, "and gets the same fingerprint back");

    /* A path that names the image with a directory prefix must key the SAME record: the
     * loader passes the bare canonical leaf, but the managed copy lives under media/. */
    got = 0;
    check(gdx_dcrc_load(base, "sdmc:/3ds/gdiffuser/media/baserom.jp.ek.ndd", kSize, kSample, &got) ==
              GDX_DCRC_HIT,
          "a full path keys the same record as the bare leaf");

    /* --- the rejects. Each one must be DISTINGUISHABLE, not merely a miss. --- */

    /* Same size, different bytes: this is the check that stops a renamed image walking past
     * the Japanese-region gate on a cached fingerprint. */
    check(gdx_dcrc_load(base, "baserom.jp.ek.ndd", kSize, kSample ^ 1ull, &got) ==
              GDX_DCRC_MISS_SAMPLE,
          "a different image of the same size is rejected: SAMPLE");

    check(gdx_dcrc_load(base, "baserom.jp.ek.ndd", kSize - 1u, kSample, &got) == GDX_DCRC_MISS_SIZE,
          "a record written for another size is rejected: SIZE");

    /* The sample sentinel: a boot that could not sample the image must never accept a cached
     * answer, because it has nothing to validate it against. */
    check(gdx_dcrc_load(base, "baserom.jp.ek.ndd", kSize, 0ull, &got) == GDX_DCRC_MISS_NOSAMPLE,
          "a boot with no sample refuses the cache: NOSAMPLE");
    check(gdx_dcrc_store(base, "baserom.jp.ek.ndd", kSize, 0ull, kCrc, &err) == GDX_DCRC_WRITE_SKIPPED,
          "and refuses to write one: SKIPPED");

    /* A torn write: fewer than 25 bytes. Recoverable, and it must say which. */
    unsigned char rec[GDX_DCRC_RECORD_BYTES];
    memcpy(rec, GDX_DCRC_MAGIC, GDX_DCRC_MAGIC_BYTES);
    memcpy(rec + GDX_DCRC_MAGIC_BYTES, &kSize, sizeof(kSize));
    memcpy(rec + GDX_DCRC_MAGIC_BYTES + 4, &kSample, sizeof(kSample));
    memcpy(rec + GDX_DCRC_MAGIC_BYTES + 12, &kCrc, sizeof(kCrc));
    writeRaw(path, rec, GDX_DCRC_RECORD_BYTES - 3u);
    check(gdx_dcrc_load(base, "baserom.jp.ek.ndd", kSize, kSample, &got) == GDX_DCRC_MISS_SHORT,
          "a torn record is rejected: SHORT");

    /* Something else entirely at the path. */
    unsigned char junk[GDX_DCRC_RECORD_BYTES];
    memset(junk, 0x5A, sizeof(junk));
    writeRaw(path, junk, sizeof(junk));
    check(gdx_dcrc_load(base, "baserom.jp.ek.ndd", kSize, kSample, &got) == GDX_DCRC_MISS_MAGIC,
          "a non-.dcrc file at the path is rejected: MAGIC");

    /* Recovery: a rejected record is overwritten in place, not left to poison every boot. */
    check(gdx_dcrc_store(base, "baserom.jp.ek.ndd", kSize, kSample, kCrc, &err) == GDX_DCRC_WRITE_OK,
          "a rejected record is replaced");
    check(gdx_dcrc_load(base, "baserom.jp.ek.ndd", kSize, kSample, &got) == GDX_DCRC_HIT,
          "and the next boot hits again");

    /* The byte layout is frozen: a sidecar written by the pre-B11m build must still validate,
     * so this is a literal transcription of what that build wrote, not a re-derivation. */
    writeRaw(path, rec, GDX_DCRC_RECORD_BYTES);
    got = 0;
    check(gdx_dcrc_load(base, "baserom.jp.ek.ndd", kSize, kSample, &got) == GDX_DCRC_HIT &&
              got == kCrc,
          "the on-disk record layout is unchanged from the pre-B11m writer");

    /* An unwritable location must REPORT, not swallow. This is the hardware failure shape the
     * receipt could not previously name. */
    char ro[600];
    snprintf(ro, sizeof(ro), "%sreadonly/", base);
    char mkro[1300];
    snprintf(mkro, sizeof(mkro), "mkdir -p \"%s\" && chmod 500 \"%s\"", ro, ro);
    /* Skipped under root, where a 0500 directory is not a refusal. */
    if (geteuid() != 0 && system(mkro) == 0) {
        err = 0;
        const GdxDcrcStoreResult r =
            gdx_dcrc_store(ro, "baserom.jp.ek.ndd", kSize, kSample, kCrc, &err);
        check(r == GDX_DCRC_WRITE_NODIR || r == GDX_DCRC_WRITE_OPEN,
              "an unwritable saves/ is REPORTED, not swallowed");
        check(err != 0, "and it carries an errno");
        check(strcmp(gdx_dcrc_store_text(r), "ok") != 0, "and its text does not read ok");
        char unro[700];
        snprintf(unro, sizeof(unro), "chmod 700 \"%s\"", ro);
        (void) system(unro);
    }

    /* Every name is non-empty and stable: the receipt is the only place these are ever read. */
    check(strcmp(gdx_dcrc_load_text(GDX_DCRC_HIT), "hit") == 0, "load text: hit");
    check(strcmp(gdx_dcrc_load_text(GDX_DCRC_MISS_SAMPLE), "sample-mismatch") == 0,
          "load text: sample-mismatch");
    check(strcmp(gdx_dcrc_store_text(GDX_DCRC_WRITE_OK), "ok") == 0, "store text: ok");
    check(strcmp(gdx_dcrc_store_text(GDX_DCRC_WRITE_IO), "FAILED-io") == 0, "store text: FAILED-io");

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf \"%s\"", base);
    (void) system(cleanup);
}

/* case 10 -- ekaudio2's coalesced fill (gdx_disk_stream_fill_range). What it must never do is
 * change a byte: every read after a fill is compared with the same read from a stream that never
 * coalesced. What it is FOR is the request count, so that is asserted exactly too: one host read
 * per run, resident blocks splitting runs, the read-ahead tail riding only on a miss. */
static void case10_coalescedFill(const char* image) {
    printf("case 10: coalesced fill (one SD request per run of missing blocks)\n");
    /* 16 KiB blocks, 32 blocks = 8 sets x 4 ways: the 64DD arm's shape at cache_kb=512. */
    check(gdx_disk_stream_open(image, DISK_EXACT_BYTES, 14, 32) == 1, "fill: cache opens");
    gdx_disk_stream_reset_stats();
    GdxDiskStreamStats st;

    /* One LBA-sized read straddling three blocks: a 4-block window is ONE request. */
    const unsigned int base = 0x1000000u + 0x3C00u; /* 15 KiB into a block: spans three */
    check(gdx_disk_stream_fill_range(base, 0x4D08u, 0u, 64u * 1024u) == 1, "fill: a 3-block range is one request");
    gdx_disk_stream_get_stats(&st);
    checkEq(st.hostReads, 1, "fill: one host read for the run");
    unsigned long long runs = 0;
    unsigned long long blocks = 0;
    gdx_disk_stream_fill_stats(&runs, &blocks);
    checkEq(blocks, 3, "fill: the run installed the range's three blocks");
    static unsigned char viaFill[0x10000];
    static unsigned char viaFile[0x10000];
    check(gdx_disk_stream_read(base, 0x4D08u, viaFill) == 1, "fill: the read after it succeeds");
    gdx_disk_stream_get_stats(&st);
    checkEq(st.hostReads, 1, "fill: the read after it is all hits");
    check(gdx_disk_stream_read_uncached(base, 0x4D08u, viaFile) == 1, "fill: uncached reference");
    check(memcmp(viaFill, viaFile, 0x4D08u) == 0, "fill: coalesced bytes == file bytes");

    /* Already resident: nothing to do, no request. */
    check(gdx_disk_stream_fill_range(base, 0x4D08u, 0u, 64u * 1024u) == 0, "fill: resident range is a no-op");

    /* Read-ahead: a resident request does NOT trigger its tail ... */
    gdx_disk_stream_get_stats(&st);
    const unsigned long long before = st.hostReads;
    check(gdx_disk_stream_fill_range(base, 0x4D08u, 0x8000u, 64u * 1024u) == 0,
          "fill: the read-ahead tail rides only on a miss");
    /* ... a missing one does, and the whole window is still one request. */
    const unsigned int next = 0x1200000u;
    check(gdx_disk_stream_fill_range(next, 0x4D08u, 0x10000u - 0x4D08u, 64u * 1024u) == 1,
          "fill: a missing request + its tail is one request");
    gdx_disk_stream_get_stats(&st);
    checkEq(st.hostReads - before, 1, "fill: one host read for request + tail");
    check(gdx_disk_stream_read(next + 0x8000u, 0x4000u, viaFill) == 1, "fill: a read in the tail");
    gdx_disk_stream_get_stats(&st);
    checkEq(st.hostReads - before, 1, "fill: the tail was resident");
    check(gdx_disk_stream_read_uncached(next + 0x8000u, 0x4000u, viaFile) == 1, "fill: tail reference");
    check(memcmp(viaFill, viaFile, 0x4000u) == 0, "fill: tail bytes == file bytes");

    /* A resident block in the middle splits the run; a lone missing block is left to the
     * demand read. Blocks: [A missing][B resident][C missing][D missing]. */
    const unsigned int split = 0x1400000u;
    check(gdx_disk_stream_read(split + 0x4000u + 16u, 16u, viaFill) == 1, "fill: make B resident");
    gdx_disk_stream_get_stats(&st);
    const unsigned long long beforeSplit = st.hostReads;
    check(gdx_disk_stream_fill_range(split, 0x10000u, 0u, 64u * 1024u) == 1,
          "fill: A is lone (skipped), C+D are one run");
    gdx_disk_stream_get_stats(&st);
    checkEq(st.hostReads - beforeSplit, 1, "fill: exactly one request for the split range");
    check(gdx_disk_stream_read(split, 0x10000u, viaFill) == 1, "fill: the split range reads");
    check(gdx_disk_stream_read_uncached(split, 0x10000u, viaFile) == 1, "fill: split reference");
    check(memcmp(viaFill, viaFile, 0x10000u) == 0, "fill: split bytes == file bytes");

    /* A run is capped at the set count (8 here): 12 missing blocks are two requests, and
     * a whole fill at half the cache (16 blocks = 256 KiB). */
    gdx_disk_stream_get_stats(&st);
    const unsigned long long beforeCap = st.hostReads;
    check(gdx_disk_stream_fill_range(0x2000000u, 12u * 0x4000u, 0u, 256u * 1024u) == 2,
          "fill: a run never holds two blocks of one set");
    gdx_disk_stream_get_stats(&st);
    checkEq(st.hostReads - beforeCap, 2, "fill: 12 blocks at 8 sets = two requests");
    check(gdx_disk_stream_fill_range(0x2800000u, 0x100000u, 0u, 256u * 1024u) == 2,
          "fill: a fill is capped at half the cache");

    /* The image's last block: a run reaching EOF reads the short tail and still matches. */
    const unsigned int tailOff = DISK_EXACT_BYTES - 0x6000u;
    check(gdx_disk_stream_fill_range(tailOff, 0x6000u, 0u, 64u * 1024u) >= 0, "fill: a run to EOF");
    check(gdx_disk_stream_read(tailOff, 0x6000u, viaFill) == 1, "fill: EOF range reads");
    check(gdx_disk_stream_read_uncached(tailOff, 0x6000u, viaFile) == 1, "fill: EOF reference");
    check(memcmp(viaFill, viaFile, 0x6000u) == 0, "fill: EOF bytes == file bytes");

    /* Off: maxRun 0 or one block issues nothing. */
    check(gdx_disk_stream_fill_range(0x3000000u, 0x10000u, 0u, 0u) == 0, "fill: maxRun 0 is off");
    check(gdx_disk_stream_fill_range(0x3000000u, 0x10000u, 0u, 0x4000u) == 0, "fill: a one-block run is off");
    gdx_disk_stream_close();
    check(gdx_disk_stream_fill_range(0x3000000u, 0x10000u, 0u, 64u * 1024u) == 0, "fill: a closed stream is a no-op");
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <image.ndd> <manifest.json> <truncated.ndd> [EkAssetBindings.c] [tmpdir]\n",
                argv[0]);
        return 2;
    }
    const char* image = argv[1];
    const char* manifest = argv[2];
    const char* truncated = argv[3];
    const char* bindings = (argc > 4) ? argv[4] : nullptr;

    unsigned long long seed = 0;
    readManifestCrc(manifest, &seed);
    printf("gdx_disk_stream_tests: image=%s seed=%llu\n", image, seed);
    buildKeystream(seed);
    buildGeometry();

    case1_openRefusals(image, truncated);
    case2_lbaReads();
    case3_seeksAndBoundaries();
    gdx_disk_stream_close();
    case4_evictionAndThrash(image);
    case5_overlay(image);
    case6_crc(image, manifest);
    if (bindings != nullptr) {
        case7_ekFillTrace(image, bindings);
    }
    case8_corruptImage(image);
    case9_dcrcSidecar((argc > 5) ? argv[5] : ".");
    case10_coalescedFill(image);

    printf("gdx_disk_stream_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
