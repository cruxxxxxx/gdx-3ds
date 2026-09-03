#pragma once
/* Do NOT include <stddef.h> or <stdint.h> here.
   C callers must include global.h first (provides size_t/uint32_t via decomp libc).
   C++ callers must include a standard header before this one (e.g. ship/Context.h). */
#ifdef __cplusplus
extern "C" {
#endif

/* Maps N64 physical addr `phys` to `gdx_rdram + phys`. F-Zero X requires the Expansion Pak, and
   overlay texture offsets reach ~12-13MB physical, so the base 8MB is not enough. */
#define GDX_RDRAM_SIZE           ((size_t)0x1000000u)  /* 16 MB (base 8MB + Expansion Pak) */
#define GDX_RDRAM_GFXPOOL_OFFSET ((size_t)0x4000u)     /* lower bound for bare RDRAM offsets (course sub-DLs start ~0x6000) */

/* Dedicated ALLOC_PEEK staging block, carved right after the GfxPool reservation and before
   gdx_rdram_arena_start (see gdx_rdram_init/gdx_rdram_peek_raw in decomp_port.c). Keeping
   transient peek scratch out of the mode arena is what stops a concurrent ALLOC_FRONT/BACK
   commit from landing on top of a live peek mid-decode. Sized well above any compressed-texture
   header/payload the decomp's texture loader uses (see object.c). */
#define GDX_RDRAM_STAGING_SIZE   ((size_t)0x100000u)   /* 1 MiB */

/* Allocated once at startup by gdx_rdram_init(), before bootproc(). */
extern unsigned char* gdx_rdram;

static inline void* gdx_rdram_ptr(unsigned int phys) {
    return gdx_rdram + phys;
}

/* Allocates the buffer, zeroes it, places the GfxPool pointer and registers the host range.
   Fatal on failure. */
void gdx_rdram_init(void);

/* Bump-allocate from the RDRAM arena. Fatal if exhausted. */
void* gdx_rdram_alloc_raw(size_t size, size_t align);

/* Bump-allocate session-lifetime data from the top of RDRAM, downward. Never reclaimed by the
   per-mode arena rewind. Fatal if exhausted. */
void* gdx_rdram_persist_alloc_raw(size_t size, size_t align);

#ifdef __cplusplus
}
#endif
