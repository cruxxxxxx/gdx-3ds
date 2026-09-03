/*
 * gfx_pack_tests.c -- Phase G1 unit tests for the wide (pointer-width) display-list
 * word.  Standalone console exe: no libultraship, no game objects, no bridge object.
 *
 * It compiles the REAL decomp gbi.h with the exact defines the game build uses
 * (PORT, F3DEX_GBI_2, _LANGUAGE_C, VERSION_US), so the gSP and gDP macros under
 * test are the same ones the game emits at runtime.
 *
 * Returns 0 iff every check passes; non-zero (and prints [FAIL]) otherwise.
 */

/* mbi.h supplies _SHIFTL / G_MAXZ and then includes <PR/gbi.h>, exactly as the
 * game build reaches gbi.h. */
#include <PR/mbi.h>
/* OSTask::t.data_ptr carries the ROOT display list to the bridge and must stay a full-width u64*,
 * never a truncated low32 -- asserted below. */
#include <PR/sptask.h>
/* K0_TO_PHYS, for the gSPMatrix(gfx, K0_TO_PHYS(&mtx), ...) idiom. */
#include <PR/R4300.h>

#include <stdio.h>
#include <string.h>

/* Must match the bridge's wide read: host-built packets are 16 bytes, w0 a 32-bit little-endian
 * word at byte 0, the pointer word full-width at byte 8. See port/n64_gfx_bridge.cpp
 * (kHostBuiltGfxStride / sourceIsWide). */
#define HOST_GFX_STRIDE 16u

typedef unsigned long long u64_t;

static int g_failures = 0;

static void check_u64(const char* name, u64_t got, u64_t want) {
    if (got == want) {
        printf("[ OK ] %-34s got=0x%016llX\n", name, (unsigned long long)got);
    } else {
        printf("[FAIL] %-34s got=0x%016llX want=0x%016llX\n",
               name, (unsigned long long)got, (unsigned long long)want);
        ++g_failures;
    }
}

static void check_sz(const char* name, size_t got, size_t want) {
    if (got == want) {
        printf("[ OK ] %-34s got=%zu\n", name, got);
    } else {
        printf("[FAIL] %-34s got=%zu want=%zu\n", name, got, want);
        ++g_failures;
    }
}

/* Deliberately does NOT read the typed struct field: reading the raw packet bytes is what proves
 * the on-wire layout the bridge relies on is what the macros actually produce. */
static u64_t read_w1_full(const void* base, size_t index) {
    u64_t w1 = 0;
    memcpy(&w1, (const unsigned char*)base + index * HOST_GFX_STRIDE + 8, sizeof(w1));
    return w1;
}
static unsigned int read_w0(const void* base, size_t index) {
    unsigned int w0 = 0;
    memcpy(&w0, (const unsigned char*)base + index * HOST_GFX_STRIDE, sizeof(w0));
    return w0;
}
static unsigned int read_opcode(const void* base, size_t index) {
    return (read_w0(base, index) >> 24) & 0xFFu;
}

int main(void) {
    printf("=== Phase G1 wide-Gfx packing tests ===\n");

    check_sz("sizeof(Gfx)", sizeof(Gfx), 16);
    check_sz("sizeof(Gwords)", sizeof(Gwords), 16);
    {
        Gfx probe;
        memset(&probe, 0, sizeof(probe));
        probe.words.w0 = 0xAABBCCDDu;
        probe.words.w1 = 0x1122334455667788ULL;
        check_u64("w0 @ byte 0", (u64_t)read_w0(&probe, 0), 0xAABBCCDDu);
        check_u64("w1 @ byte 8 (full pointer word)", read_w1_full(&probe, 0), 0x1122334455667788ULL);
    }

    /* Bit 46 set, so on a 64-bit host all 64 bits have to survive: an (unsigned int) cast
     * anywhere in the macro chain would truncate this to 0xABCD1234. On a 32-bit host the
     * (size_t) cast collapses the constants to their low32 up front, and every check below
     * compares against the same collapsed pointer, which still proves full pointer-width
     * survival for that host. */
    void* const kHostPtr = (void*)(size_t)0x00007FF6ABCD1234ULL;
    void* const kDlPtr   = (void*)(size_t)0x00007FFEDEADBEEFULL;
    void* const kTexPtr  = (void*)(size_t)0x0000023400001000ULL;
    Mtx   dummyMtx;
    Vtx   dummyVtx[4];

    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPVertex(&cmd, (Vtx*)kHostPtr, 4, 0);
        check_u64("gSPVertex w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)(size_t)kHostPtr);
        check_u64("gSPVertex opcode G_VTX", (u64_t)read_opcode(&cmd, 0), G_VTX);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPDisplayList(&cmd, (Gfx*)kDlPtr);
        check_u64("gSPDisplayList w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)(size_t)kDlPtr);
        check_u64("gSPDisplayList opcode G_DL", (u64_t)read_opcode(&cmd, 0), (u64_t)G_DL);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPBranchList(&cmd, (Gfx*)kDlPtr);
        check_u64("gSPBranchList w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)(size_t)kDlPtr);
        check_u64("gSPBranchList opcode G_DL", (u64_t)read_opcode(&cmd, 0), (u64_t)G_DL);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPMatrix(&cmd, (Mtx*)kHostPtr, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
        check_u64("gSPMatrix w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)(size_t)kHostPtr);
        check_u64("gSPMatrix opcode G_MTX", (u64_t)read_opcode(&cmd, 0), (u64_t)G_MTX);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gDPSetTextureImage(&cmd, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, kTexPtr);
        check_u64("gDPSetTextureImage w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)(size_t)kTexPtr);
    }

    /* Real object addresses, not just fabricated ones. */
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPVertex(&cmd, &dummyVtx[0], 4, 0);
        check_u64("gSPVertex real &Vtx", read_w1_full(&cmd, 0), (u64_t)(size_t)&dummyVtx[0]);
        gSPMatrix(&cmd, &dummyMtx, G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_PUSH);
        check_u64("gSPMatrix real &Mtx", read_w1_full(&cmd, 0), (u64_t)(size_t)&dummyMtx);
    }

    /* The K0_TO_PHYS(...) matrix idiom (course_model.c / course_gadgets.c) must pack the FULL
     * host pointer. A `(u32)(uintptr_t)` PORT macro drops the high 32 bits, the bridge then sees
     * high32==0, runs the guessing resolver, hits fallback_buffer, and the matrix loads garbage
     * -> invisible 3D models (832 [datafail] op=DA/frame). */
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPMatrix(&cmd, (Mtx*)K0_TO_PHYS(kHostPtr), G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
        check_u64("gSPMatrix K0_TO_PHYS full ptr", read_w1_full(&cmd, 0), (u64_t)(size_t)kHostPtr);
        /* Only a 64-bit host can have (and must preserve) nonzero high bits here. */
        check_u64("gSPMatrix K0_TO_PHYS high32!=0", (read_w1_full(&cmd, 0) >> 32) != 0,
                  (sizeof(void*) == 8) ? 1 : 0);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPMatrix(&cmd, (Mtx*)K0_TO_PHYS(&dummyMtx), G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
        check_u64("gSPMatrix K0_TO_PHYS real &Mtx", read_w1_full(&cmd, 0), (u64_t)(size_t)&dummyMtx);
    }

    /* Segmented address round-trip: a 32-bit value stays 32-bit, high half zero -- the signal the
     * bridge uses to route it through the segment table rather than as a host pointer. */
    {
        Gfx cmd;
        const unsigned int kSeg = 0x02000000u; /* segment 2, offset 0 */
        memset(&cmd, 0, sizeof(cmd));
        gSPDisplayList(&cmd, (Gfx*)(size_t)kSeg);
        check_u64("segmented DL low32 preserved", read_w1_full(&cmd, 0) & 0xFFFFFFFFu, kSeg);
        check_u64("segmented DL high32 == 0 (seg path)", read_w1_full(&cmd, 0) >> 32, 0);
    }
    {
        Gfx cmd;
        const unsigned int kSeg = 0x0A001000u; /* segment 0x0A venue texture bank */
        memset(&cmd, 0, sizeof(cmd));
        gDPSetTextureImage(&cmd, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, (void*)(size_t)kSeg);
        check_u64("segmented TIMG low32 preserved", read_w1_full(&cmd, 0) & 0xFFFFFFFFu, kSeg);
        check_u64("segmented TIMG high32 == 0", read_w1_full(&cmd, 0) >> 32, 0);
    }

    {
        Gfx dl[8];
        Gfx* p = dl;
        memset(dl, 0, sizeof(dl));
        gSPMatrix(p++, (Mtx*)kHostPtr, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_PUSH);
        gSPVertex(p++, (Vtx*)kHostPtr, 4, 0);
        gDPSetTextureImage(p++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, kTexPtr);
        gSPDisplayList(p++, (Gfx*)kDlPtr);
        gSPEndDisplayList(p++);
        const size_t built = (size_t)(p - dl);
        check_sz("mixed DL built command count", built, 5);

        size_t walked = 0;
        int sawEnd = 0;
        for (size_t i = 0; i < (sizeof(dl) / sizeof(dl[0])); ++i) {
            unsigned int op = read_opcode(dl, i);
            ++walked;
            if (op == (G_ENDDL & 0xFFu)) { sawEnd = 1; break; }
        }
        check_sz("mixed DL walked to ENDDL", walked, 5);
        check_u64("mixed DL ENDDL seen", (u64_t)sawEnd, 1);
        check_u64("walk: cmd0 (MTX) ptr", read_w1_full(dl, 0), (u64_t)(size_t)kHostPtr);
        check_u64("walk: cmd3 (DL) ptr", read_w1_full(dl, 3), (u64_t)(size_t)kDlPtr);
    }

    /* ROOT display-list pointer carry. Gfx_SetTask assigns
     * `task->t.data_ptr = (u64*) gGfxPool->gfxBuffer;` and osSpTaskStartGo feeds tp->t.data_ptr
     * straight to the bridge, so narrowing that field to 32 bits drops the root DL to the
     * module-window guess -- the first-frame crash. */
    {
        OSTask task;
        memset(&task, 0, sizeof(task));
        check_sz("OSTask_t.data_ptr is pointer-width", sizeof(task.t.data_ptr), sizeof(void*));
        task.t.data_ptr = (u64*)kHostPtr;
        check_u64("root data_ptr carries full pointer",
                  (u64_t)(size_t)task.t.data_ptr, (u64_t)(size_t)kHostPtr);
        /* The shift operand must be widened BEFORE >>32: shifting a 32-bit size_t by 32 is UB. */
        check_u64("root data_ptr high32 preserved",
                  ((u64_t)(size_t)task.t.data_ptr) >> 32, (u64_t)(size_t)kHostPtr >> 32);
    }

    /* gSPSegment wide round-trip. Under F3DEX_GBI_2 gSPSegment(seg, base) expands to
     *   gMoveWd(G_MW_SEGMENT, seg*4, base) -> gDma1p(G_MOVEWORD, base, seg*4, G_MW_SEGMENT)
     * so the segment BASE goes through the SAME widened gDma1p / _GFXW1_PTR path as gSPVertex
     * above. The segment table is the central base for ALL segmented addressing, so a truncated
     * base cascades into missing textures and garbage geometry. The w0 field checks pin the exact
     * layout the bridge parses (index @ w0[23:16] == G_MW_SEGMENT, seg*4 @ w0[15:0]). */
    {
        Gfx cmd;
        const unsigned int kSegNo = 8u; /* segment 8: course_track_gfx base */
        memset(&cmd, 0, sizeof(cmd));
        gSPSegment(&cmd, kSegNo, kHostPtr);
        check_u64("gSPSegment opcode G_MOVEWORD",
                  (u64_t)read_opcode(&cmd, 0), (u64_t)G_MOVEWORD);
        check_u64("gSPSegment index == G_MW_SEGMENT",
                  (u64_t)((read_w0(&cmd, 0) >> 16) & 0xFFu), (u64_t)G_MW_SEGMENT);
        check_u64("gSPSegment offset == seg*4",
                  (u64_t)(read_w0(&cmd, 0) & 0xFFFFu), (u64_t)(kSegNo * 4u));
        check_u64("gSPSegment base == FULL host ptr (no low32 truncation)",
                  read_w1_full(&cmd, 0), (u64_t)(size_t)kHostPtr);
        check_u64("gSPSegment base high32 preserved (>4GB)",
                  read_w1_full(&cmd, 0) >> 32, (u64_t)(size_t)kHostPtr >> 32);
    }
    {
        Gfx cmd;
        const unsigned int kPhysBase = 0x80200000u; /* KSEG0 physical base */
        memset(&cmd, 0, sizeof(cmd));
        gSPSegment(&cmd, 3u, (void*)(size_t)kPhysBase);
        check_u64("gSPSegment 32-bit base low32 preserved",
                  read_w1_full(&cmd, 0) & 0xFFFFFFFFu, kPhysBase);
        check_u64("gSPSegment 32-bit base high32 == 0",
                  read_w1_full(&cmd, 0) >> 32, 0);
    }

    printf("=== %s (%d failure%s) ===\n",
           g_failures == 0 ? "ALL PASS" : "FAILURES",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
