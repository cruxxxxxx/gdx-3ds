/* port/tests/test_vi_fallback.c — standalone unit test for the VI-scanout
 * fallback's RGBA5551 -> RGBA8888 conversion routine.
 *
 * Compiles gdx_vi_convert.c UNMODIFIED (no game objects, no libultraship, no
 * decomp headers). Build target: gdx_vi_fallback_tests (see port/CMakeLists.txt).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../gdx_vi_convert.h"

static int g_failures = 0;

/* Independent restatement of SCALE_5_8 (interpreter.cpp / gdx_vi_convert.c), so the expected
 * values below carry their derivation rather than being magic numbers. */
static uint8_t scale_5_8(uint32_t v5) {
    return (uint8_t)((v5 * 0xFFu) / 0x1Fu);
}

static void check_pixel(const char* name, uint16_t src,
                        uint8_t er, uint8_t eg, uint8_t eb, uint8_t ea) {
    uint8_t out[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
    gdx_convert_rgba5551_to_rgba8888(&src, out, 1);
    if (out[0] != er || out[1] != eg || out[2] != eb || out[3] != ea) {
        printf("[FAIL] %-16s src=0x%04X got=(%3u,%3u,%3u,%3u) want=(%3u,%3u,%3u,%3u)\n",
               name, src, out[0], out[1], out[2], out[3], er, eg, eb, ea);
        g_failures++;
    } else {
        printf("[ ok ] %-16s src=0x%04X -> (%3u,%3u,%3u,%3u)\n",
               name, src, out[0], out[1], out[2], out[3]);
    }
}

int main(void) {
    printf("=== VI-fallback RGBA5551->RGBA8888 conversion tests ===\n");

    /* Pure channels (5-bit max = 31 -> 255) and alpha bit. */
    check_pixel("black-opaque",  0x0001, 0, 0, 0, 255);           /* a=1 only */
    check_pixel("black-transp",  0x0000, 0, 0, 0, 0);
    check_pixel("red-max",       (uint16_t)(31u << 11),        255, 0,   0,   0);
    check_pixel("green-max",     (uint16_t)(31u << 6),         0,   255, 0,   0);
    check_pixel("blue-max",      (uint16_t)(31u << 1),         0,   0,   255, 0);
    check_pixel("white-opaque",  0xFFFF,                       255, 255, 255, 255);
    check_pixel("red-max-opaque",(uint16_t)((31u << 11) | 1u), 255, 0,   0,   255);

    /* Mid-range channel values exercise the integer SCALE_5_8 rounding. */
    check_pixel("red-16",  (uint16_t)(16u << 11), scale_5_8(16), 0, 0, 0); /* 131 */
    check_pixel("green-1", (uint16_t)(1u << 6),   0, scale_5_8(1), 0, 0);  /* 8 */
    check_pixel("blue-10", (uint16_t)(10u << 1),  0, 0, scale_5_8(10), 0); /* 82 */

    /* Composite pixel: r=5, g=10, b=20, a=1. */
    {
        uint16_t c = (uint16_t)((5u << 11) | (10u << 6) | (20u << 1) | 1u);
        check_pixel("composite", c, scale_5_8(5), scale_5_8(10), scale_5_8(20), 255);
    }

    /* Catches a wrong dst stride, which a one-pixel check cannot. */
    {
        const uint16_t src[4] = {
            0x0001,                 /* black opaque */
            (uint16_t)(31u << 11),  /* red */
            (uint16_t)(31u << 6),   /* green */
            0xFFFF                  /* white opaque */
        };
        uint8_t out[4 * 4];
        static const uint8_t want[4 * 4] = {
            0,   0,   0,   255,
            255, 0,   0,   0,
            0,   255, 0,   0,
            255, 255, 255, 255,
        };
        gdx_convert_rgba5551_to_rgba8888(src, out, 4);
        if (memcmp(out, want, sizeof(want)) != 0) {
            printf("[FAIL] multi-pixel buffer mismatch\n");
            g_failures++;
        } else {
            printf("[ ok ] multi-pixel buffer (4 px)\n");
        }
    }

    gdx_convert_rgba5551_to_rgba8888(NULL, NULL, 16); /* must not crash */
    printf("[ ok ] null-argument safety\n");

    if (g_failures == 0) {
        printf("=== ALL TESTS PASSED ===\n");
        return 0;
    }
    printf("=== %d TEST(S) FAILED ===\n", g_failures);
    return 1;
}
