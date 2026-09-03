/* port/3ds/lus_glue/gdx3ds_audiobridge.c — hook R5 (port/3ds/audio/STATUS.md).
 *
 * On desktop, libultraship's os.cpp/audiobridge satisfies the decomp's osAiSetNextBuffer
 * (the capture-tap site in decomp src/audio/<rom|disk>/lib/thread.c) and osAiGetLength.
 * The carved 3DS build has neither, so the produced PCM routes to stream C's ndsp
 * backend here: 4 bytes per interleaved stereo s16 frame, return value ignored by the
 * caller (overrun policy is drop-oldest inside the backend).
 *
 * osAiGetLength mirrors it for the pacing reads (aiSamplesLeft in thread.c and the
 * legacy 2048-frame cushion): frames buffered * 4 = bytes, exactly the AI-DMA length
 * semantics the decomp expects.
 */

#include "gdx3ds_audio.h"

#include <stddef.h>
#include <stdint.h>

int32_t osAiSetNextBuffer(void* buf, size_t sizeBytes) {
    if (buf == NULL || sizeBytes == 0) {
        return 0;
    }
    (void)gdx3ds_audio_push((const int16_t*)buf, sizeBytes / 4u);
    return 0;
}

uint32_t osAiGetLength(void) {
    return (uint32_t)(gdx3ds_audio_buffered() * 4u);
}
