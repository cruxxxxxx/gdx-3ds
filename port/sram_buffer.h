#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size of the persisted SRAM image. Must stay equal to the decomp's SaveContext
// (decomp/include/fzx_save.h: profileSaves + ghostSave + characterSaves + cupSave),
// which is also the real F-Zero X cart's battery-backed SRAM size. Existing
// fzerox.sav files are rejected on a size mismatch.
#define GDX_SRAM_SIZE 0x8000u

// Loads saves/fzerox.sav into the in-memory image, or zero-fills it when no save
// exists yet or it is the wrong size. A legacy save from the older location is
// migrated in automatically. Idempotent -- called from the decomp's Sram_Init()
// (decomp/src/overlays/ovl_i2/save.c) and from gdx_sram_read/gdx_sram_write, so
// load order never matters.
void gdx_sram_init(void);

// SRAM byte-range primitives backing the decomp's Sram_ReadWrite. offset/size are
// relative to the GDX_SRAM_SIZE-byte SaveContext image; out-of-range requests are
// logged and ignored (read returns zeros) rather than touching memory outside it.
void gdx_sram_read(unsigned int offset, void* dst, unsigned int size);

// Write-through: persists the whole image to fzerox.sav immediately, no debounce
// needed at this size and call frequency.
void gdx_sram_write(unsigned int offset, const void* src, unsigned int size);

#ifdef __cplusplus
}
#endif
