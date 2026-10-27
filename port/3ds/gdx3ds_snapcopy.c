/* port/3ds/gdx3ds_snapcopy.c -- snap-fast: the snapshot copy engine. Contract and modes:
 * gdx3ds_snapcopy.h. */
#include "gdx3ds_snapcopy.h"

#include <stdint.h>
#include <string.h>

#define LINE_BYTES 32u

#if defined(__arm__)
/* lines x 32 bytes, dst/src word-aligned, lines > 0. AAPCS: r0 = dst, r1 = src, r2 = lines; r3 and
 * r12 are scratch, r4-r10 are saved. r7 (Thumb fp) and r11 (ARM fp) are not in the burst list so
 * the function is frame-pointer neutral either way. */
__attribute__((naked, noinline)) static void CopyLinesLdm(void* dst, const void* src, unsigned int lines) {
    __asm__ volatile("push {r4, r5, r6, r8, r9, r10}\n"
                     "1:\n"
                     "ldmia r1!, {r3, r4, r5, r6, r8, r9, r10, r12}\n"
                     "subs r2, r2, #1\n"
                     "stmia r0!, {r3, r4, r5, r6, r8, r9, r10, r12}\n"
                     "bne 1b\n"
                     "pop {r4, r5, r6, r8, r9, r10}\n"
                     "bx lr\n");
}

__attribute__((naked, noinline)) static void CopyLinesPld(void* dst, const void* src, unsigned int lines) {
    __asm__ volatile("push {r4, r5, r6, r8, r9, r10}\n"
                     "1:\n"
                     "pld [r1, #128]\n"
                     "ldmia r1!, {r3, r4, r5, r6, r8, r9, r10, r12}\n"
                     "subs r2, r2, #1\n"
                     "stmia r0!, {r3, r4, r5, r6, r8, r9, r10, r12}\n"
                     "bne 1b\n"
                     "pop {r4, r5, r6, r8, r9, r10}\n"
                     "bx lr\n");
}

/* 1 when lines x 32 bytes at a and b differ (word-aligned, lines > 0), else 0. Two 4-register LDMs
 * per 16 bytes, XOR / OR folded, one branch per 16 bytes. pld = 1 prefetches both streams 128 bytes
 * ahead. */
__attribute__((naked, noinline)) static int DiffLinesLdm(const void* a, const void* b, unsigned int lines) {
    __asm__ volatile("push {r4, r5, r6, r8, r9, r10}\n"
                     "1:\n"
                     "ldmia r0!, {r3, r4, r5, r6}\n"
                     "ldmia r1!, {r8, r9, r10, r12}\n"
                     "eor r3, r3, r8\n"
                     "eor r4, r4, r9\n"
                     "eor r5, r5, r10\n"
                     "eor r6, r6, r12\n"
                     "orr r3, r3, r4\n"
                     "orr r5, r5, r6\n"
                     "orrs r3, r3, r5\n"
                     "bne 2f\n"
                     "ldmia r0!, {r3, r4, r5, r6}\n"
                     "ldmia r1!, {r8, r9, r10, r12}\n"
                     "eor r3, r3, r8\n"
                     "eor r4, r4, r9\n"
                     "eor r5, r5, r10\n"
                     "eor r6, r6, r12\n"
                     "orr r3, r3, r4\n"
                     "orr r5, r5, r6\n"
                     "orrs r3, r3, r5\n"
                     "bne 2f\n"
                     "subs r2, r2, #1\n"
                     "bne 1b\n"
                     "mov r0, #0\n"
                     "pop {r4, r5, r6, r8, r9, r10}\n"
                     "bx lr\n"
                     "2:\n"
                     "mov r0, #1\n"
                     "pop {r4, r5, r6, r8, r9, r10}\n"
                     "bx lr\n");
}

__attribute__((naked, noinline)) static int DiffLinesPld(const void* a, const void* b, unsigned int lines) {
    __asm__ volatile("push {r4, r5, r6, r8, r9, r10}\n"
                     "1:\n"
                     "pld [r0, #128]\n"
                     "pld [r1, #128]\n"
                     "ldmia r0!, {r3, r4, r5, r6}\n"
                     "ldmia r1!, {r8, r9, r10, r12}\n"
                     "eor r3, r3, r8\n"
                     "eor r4, r4, r9\n"
                     "eor r5, r5, r10\n"
                     "eor r6, r6, r12\n"
                     "orr r3, r3, r4\n"
                     "orr r5, r5, r6\n"
                     "orrs r3, r3, r5\n"
                     "bne 2f\n"
                     "ldmia r0!, {r3, r4, r5, r6}\n"
                     "ldmia r1!, {r8, r9, r10, r12}\n"
                     "eor r3, r3, r8\n"
                     "eor r4, r4, r9\n"
                     "eor r5, r5, r10\n"
                     "eor r6, r6, r12\n"
                     "orr r3, r3, r4\n"
                     "orr r5, r5, r6\n"
                     "orrs r3, r3, r5\n"
                     "bne 2f\n"
                     "subs r2, r2, #1\n"
                     "bne 1b\n"
                     "mov r0, #0\n"
                     "pop {r4, r5, r6, r8, r9, r10}\n"
                     "bx lr\n"
                     "2:\n"
                     "mov r0, #1\n"
                     "pop {r4, r5, r6, r8, r9, r10}\n"
                     "bx lr\n");
}
#else
/* Host: the same contract in portable C (the host test runs this; the gate runs the ARM bursts). */
static void CopyLinesLdm(void* dst, const void* src, unsigned int lines) {
    uint32_t* d = (uint32_t*)dst;
    const uint32_t* s = (const uint32_t*)src;
    unsigned int i;
    for (i = 0; i < lines * (LINE_BYTES / 4u); i++) {
        d[i] = s[i];
    }
}

static void CopyLinesPld(void* dst, const void* src, unsigned int lines) {
    CopyLinesLdm(dst, src, lines);
}

static int DiffLinesLdm(const void* a, const void* b, unsigned int lines) {
    return memcmp(a, b, lines * LINE_BYTES) != 0;
}

static int DiffLinesPld(const void* a, const void* b, unsigned int lines) {
    return DiffLinesLdm(a, b, lines);
}
#endif

static int WordAligned(const void* a, const void* b) {
    return (((uintptr_t)a | (uintptr_t)b) & 3u) == 0;
}

void gdx3ds_snapcopy(void* dst, const void* src, unsigned int n, int mode) {
    const unsigned int lines = n / LINE_BYTES;
    if (mode == GDX_SNAPCOPY_LIBC || lines == 0 || !WordAligned(dst, src)) {
        memcpy(dst, src, n);
        return;
    }
    if (mode == GDX_SNAPCOPY_PLD) {
        CopyLinesPld(dst, src, lines);
    } else {
        CopyLinesLdm(dst, src, lines);
    }
    const unsigned int done = lines * LINE_BYTES;
    if (done < n) {
        memcpy((unsigned char*)dst + done, (const unsigned char*)src + done, n - done);
    }
}

static int BlockDiffers(const unsigned char* a, const unsigned char* b, unsigned int len, int mode) {
    const unsigned int lines = len / LINE_BYTES;
    if (mode == GDX_SNAPCOPY_LIBC || lines == 0 || !WordAligned(a, b)) {
        return memcmp(a, b, len) != 0;
    }
    const int differs = mode == GDX_SNAPCOPY_PLD ? DiffLinesPld(a, b, lines) : DiffLinesLdm(a, b, lines);
    const unsigned int done = lines * LINE_BYTES;
    if (differs) {
        return 1;
    }
    return done < len && memcmp(a + done, b + done, len - done) != 0;
}

unsigned int gdx3ds_snapsync(void* dst, const void* src, unsigned int n, int mode) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    unsigned int written = 0;
    unsigned int off = 0;
    unsigned int runStart = 0;
    int inRun = 0;
    while (off < n) {
        const unsigned int len = (n - off) < GDX_SNAPSYNC_BLOCK ? (n - off) : GDX_SNAPSYNC_BLOCK;
        const int dirty = BlockDiffers(d + off, s + off, len, mode);
        if (dirty && !inRun) {
            runStart = off;
            inRun = 1;
        } else if (!dirty && inRun) {
            gdx3ds_snapcopy(d + runStart, s + runStart, off - runStart, mode);
            written += off - runStart;
            inRun = 0;
        }
        off += len;
    }
    if (inRun) {
        gdx3ds_snapcopy(d + runStart, s + runStart, n - runStart, mode);
        written += n - runStart;
    }
    return written;
}

const char* gdx3ds_snapcopy_mode_name(int mode) {
    switch (mode) {
        case GDX_SNAPCOPY_LIBC:
            return "libc";
        case GDX_SNAPCOPY_LDM:
            return "ldm";
        case GDX_SNAPCOPY_PLD:
            return "pld";
        default:
            return "?";
    }
}
