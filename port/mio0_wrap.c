/* port/mio0_wrap.c — mio0Decode wrapper for decomp callers, delegating to
 * torch/lib/libmio0/mio0_decode. The signature must match
 * decomp/include/functions.h:282. torch/lib/libmio0/utils.h is self-contained (macros plus
 * <stdio.h>, no torch-internal headers), so this compiles outside the torch CMake context. */
#include "mio0.h"
#include "n64_rdram.h"

extern void gdx_record_dma_load(unsigned int rdram_phys, unsigned int rom_offset, unsigned int size);

void mio0Decode(unsigned char* src, void* dst) {
#if defined(GDX_PLATFORM_3DS)
    {
        extern void gdx3ds_rt_fence_dma(void) __attribute__((weak)); /* RENDER THREAD (ahead) */
        if (&gdx3ds_rt_fence_dma != NULL) {
            gdx3ds_rt_fence_dma();
        }
    }
#endif
    int written = mio0_decode(src, (unsigned char*)dst, NULL);
    /* The renderer's texture-staleness tracking (HostRangeChanged in n64_gfx_bridge.cpp)
       only sees recorded writes, and a mio0 decode is plain CPU stores. Since the per-mode
       arena rewind reuses addresses across mode transitions, an unrecorded decode leaves
       stale persistent texture copies — previous-mode pixels rendered on race tracks. */
    if (written > 0 && gdx_rdram != NULL) {
        unsigned char* d = (unsigned char*)dst;
        if (d >= gdx_rdram && d < gdx_rdram + GDX_RDRAM_SIZE) {
            gdx_record_dma_load((unsigned int)(d - gdx_rdram), 0u, (unsigned int)written);
        }
    }
}
