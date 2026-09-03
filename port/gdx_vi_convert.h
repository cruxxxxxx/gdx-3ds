#ifndef GDIFFUSER_GDX_VI_CONVERT_H
#define GDIFFUSER_GDX_VI_CONVERT_H

/* port/gdx_vi_convert.h — RGBA5551 (N64 RGBA16) -> RGBA8888 pixel conversion.
 *
 * Used by the VI-scanout fallback (port/n64_gfx_bridge.cpp) for framebuffers whose pixels were
 * CPU-written with no GFX task at all (the boot N64/64DD logo), which the fallback uploads as a
 * single host texture and draws as one fullscreen quad.
 *
 * Its own translation unit (pure <stdint.h>, no decomp/libultraship headers) so it can be
 * unit-tested standalone — see port/tests/test_vi_fallback.c.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `src` points at `count` 16-bit pixels in HOST-NATIVE byte order: the decomp CPU blit writes the
 * framebuffer with plain C u16 stores, so on the host the pixels are little-endian u16 — read
 * them as uint16_t, do NOT byte-assemble. `dst` receives `count * 4` bytes, R,G,B,A per pixel.
 *
 * The 5-bit channel expansion matches libultraship's Fast3D interpreter (SCALE_5_8 ==
 * (v * 0xFF) / 0x1F in interpreter.cpp) so the fallback is pixel-identical to a real textured
 * draw of the same data.
 */
void gdx_convert_rgba5551_to_rgba8888(const uint16_t* src, uint8_t* dst, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* GDIFFUSER_GDX_VI_CONVERT_H */
