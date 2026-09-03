/* port/gdx_vi_convert.c — see gdx_vi_convert.h for rationale. */

#include "gdx_vi_convert.h"

/* Must match libultraship/src/fast/interpreter.cpp:
 *   #define SCALE_5_8(VAL_) (((VAL_)*0xFF) / 0x1F)
 * so a fallback present of a framebuffer is indistinguishable from drawing it as a texture. */
static uint8_t gdx_scale_5_8(uint32_t v5) {
    return (uint8_t)((v5 * 0xFFu) / 0x1Fu);
}

void gdx_convert_rgba5551_to_rgba8888(const uint16_t* src, uint8_t* dst, size_t count) {
    size_t i;

    if (src == NULL || dst == NULL) {
        return;
    }

    for (i = 0; i < count; i++) {
        const uint16_t col = src[i];
        /* RGBA5551: RRRRR GGGGG BBBBB A (bit 0 = alpha). */
        dst[4 * i + 0] = gdx_scale_5_8((uint32_t)((col >> 11) & 0x1Fu));
        dst[4 * i + 1] = gdx_scale_5_8((uint32_t)((col >> 6) & 0x1Fu));
        dst[4 * i + 2] = gdx_scale_5_8((uint32_t)((col >> 1) & 0x1Fu));
        dst[4 * i + 3] = (col & 0x1u) ? 0xFFu : 0x00u;
    }
}
