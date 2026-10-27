/* port/3ds/test/gdx3ds_snapcopy_host_tests.c -- snap-fast: the snapshot copy engine
 * (port/3ds/gdx3ds_snapcopy.c) against memcpy.
 *
 *   1. gdx3ds_snapcopy, every mode, every length 0..600 and a 900 KB image, aligned and misaligned
 *      pairs: dst == memcpy's result, and no byte outside [dst, dst+n) is touched.
 *   2. gdx3ds_snapsync, every mode: after the sync dst == src byte for byte, the bytes written are
 *      exactly the dirty blocks (a whole GDX_SNAPSYNC_BLOCK block or the tail), and a clean image
 *      writes nothing. Dirty patterns: none, one byte, the first / last byte, sparse random bytes,
 *      every block, runs that straddle block edges.
 * The ARM LDM/STM bursts are exercised by the emulator determinism gate ([debug] snap_test). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx3ds_snapcopy.h"

static int sFails = 0;
static int sChecks = 0;

static void Check(int ok, const char* what, int mode, unsigned int a, unsigned int b) {
    sChecks++;
    if (!ok) {
        sFails++;
        if (sFails <= 20) {
            printf("FAIL: %s (mode=%s, %u, %u)\n", what, gdx3ds_snapcopy_mode_name(mode), a, b);
        }
    }
}

static unsigned int sRng = 12345u;

static unsigned int Rand(void) {
    sRng = sRng * 1103515245u + 12345u;
    return sRng >> 8;
}

static void Fill(unsigned char* p, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)Rand();
    }
}

#define GUARD 64u

static void TestCopyOne(int mode, unsigned int n, unsigned int dstMis, unsigned int srcMis) {
    unsigned char* src = (unsigned char*)malloc(n + 2u * GUARD);
    unsigned char* dst = (unsigned char*)malloc(n + 2u * GUARD);
    unsigned char* ref = (unsigned char*)malloc(n + 2u * GUARD);
    Fill(src, n + 2u * GUARD);
    Fill(dst, n + 2u * GUARD);
    memcpy(ref, dst, n + 2u * GUARD);
    memcpy(ref + GUARD + dstMis, src + GUARD + srcMis, n);
    gdx3ds_snapcopy(dst + GUARD + dstMis, src + GUARD + srcMis, n, mode);
    Check(memcmp(dst, ref, n + 2u * GUARD) == 0, "snapcopy == memcpy (guards intact)", mode, n, dstMis * 10u + srcMis);
    free(src);
    free(dst);
    free(ref);
}

static void TestCopy(void) {
    int mode;
    unsigned int n;
    for (mode = 0; mode < GDX_SNAPCOPY_MODES; mode++) {
        for (n = 0; n <= 600u; n++) {
            TestCopyOne(mode, n, 0, 0);
            TestCopyOne(mode, n, 4, 8);
            TestCopyOne(mode, n, 1, 0);
            TestCopyOne(mode, n, 0, 3);
        }
        TestCopyOne(mode, 892832u, 0, 0);
        TestCopyOne(mode, 892833u, 2, 2);
    }
}

/* Expected bytes written: every block (GDX_SNAPSYNC_BLOCK, the last one may be short) that differs. */
static unsigned int DirtyBytes(const unsigned char* a, const unsigned char* b, unsigned int n) {
    unsigned int off;
    unsigned int total = 0;
    for (off = 0; off < n; off += GDX_SNAPSYNC_BLOCK) {
        const unsigned int len = (n - off) < GDX_SNAPSYNC_BLOCK ? (n - off) : GDX_SNAPSYNC_BLOCK;
        if (memcmp(a + off, b + off, len) != 0) {
            total += len;
        }
    }
    return total;
}

typedef void (*DirtyFn)(unsigned char* p, unsigned int n);

static void DirtyNone(unsigned char* p, unsigned int n) {
    (void)p;
    (void)n;
}

static void DirtyFirst(unsigned char* p, unsigned int n) {
    if (n > 0) {
        p[0] ^= 0x5A;
    }
}

static void DirtyLast(unsigned char* p, unsigned int n) {
    if (n > 0) {
        p[n - 1u] ^= 0x5A;
    }
}

static void DirtySparse(unsigned char* p, unsigned int n) {
    unsigned int i;
    for (i = 0; n > 0 && i < 1u + n / 2000u; i++) {
        p[Rand() % n] ^= (unsigned char)(1u + Rand() % 255u);
    }
}

static void DirtyAll(unsigned char* p, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        p[i] ^= 0xFF;
    }
}

static void DirtyStraddle(unsigned char* p, unsigned int n) {
    unsigned int edge;
    for (edge = GDX_SNAPSYNC_BLOCK; edge + 4u < n; edge += 3u * GDX_SNAPSYNC_BLOCK) {
        p[edge - 1u] ^= 1;
        p[edge] ^= 1;
        p[edge + 3u] ^= 1;
    }
}

static void TestSyncOne(int mode, unsigned int n, DirtyFn dirty, const char* what) {
    unsigned char* src = (unsigned char*)malloc(n + 1u);
    unsigned char* dst = (unsigned char*)malloc(n + 1u);
    Fill(src, n + 1u);
    memcpy(dst, src, n + 1u);
    dirty(src, n);
    const unsigned int expect = DirtyBytes(dst, src, n);
    const unsigned char sentinel = dst[n];
    const unsigned int written = gdx3ds_snapsync(dst, src, n, mode);
    Check(memcmp(dst, src, n) == 0, what, mode, n, 0);
    Check(written == expect, "snapsync writes exactly the dirty blocks", mode, written, expect);
    Check(dst[n] == sentinel, "snapsync stays inside [dst, dst+n)", mode, n, 1);
    free(src);
    free(dst);
}

static void TestSync(void) {
    static const unsigned int sizes[] = { 0u, 1u, 3u, 4u, 255u, 256u, 257u, 511u, 512u, 1000u, 4096u, 65537u, 892832u };
    int mode;
    unsigned int i;
    for (mode = 0; mode < GDX_SNAPCOPY_MODES; mode++) {
        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            TestSyncOne(mode, sizes[i], DirtyNone, "snapsync clean image");
            TestSyncOne(mode, sizes[i], DirtyFirst, "snapsync first byte");
            TestSyncOne(mode, sizes[i], DirtyLast, "snapsync last byte");
            TestSyncOne(mode, sizes[i], DirtySparse, "snapsync sparse");
            TestSyncOne(mode, sizes[i], DirtyAll, "snapsync all");
            TestSyncOne(mode, sizes[i], DirtyStraddle, "snapsync block-edge runs");
        }
    }
}

int main(void) {
    TestCopy();
    TestSync();
    if (sFails != 0) {
        printf("=== %d FAILURES of %d checks ===\n", sFails, sChecks);
        return 1;
    }
    printf("=== ALL PASS (%d checks) ===\n", sChecks);
    return 0;
}
