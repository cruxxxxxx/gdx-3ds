/* Phase 0 stub for gdx3ds_audio.h. Stream C replaces this with the ndsp backend
 * (audio thread on New3DS core 2; Luma >= 10.1.1). Stub swallows samples. */
#include "gdx3ds_audio.h"

int gdx3ds_audio_init(uint32_t bufferFrames) {
    (void)bufferFrames;
    return 0;
}

void gdx3ds_audio_shutdown(void) {
}

size_t gdx3ds_audio_push(const int16_t* samples, size_t frames) {
    (void)samples;
    return frames;
}

size_t gdx3ds_audio_buffered(void) {
    return 0;
}

int gdx3ds_audio_suspend(int* drainParked, int* producerParked) {
    if (drainParked != NULL) {
        *drainParked = 1;
    }
    if (producerParked != NULL) {
        *producerParked = 1;
    }
    return 1;
}

void gdx3ds_audio_resume(void) {
}

int gdx3ds_audio_suspended(void) {
    return 0;
}

int gdx3ds_audio_syscore_limit_percent(void) {
    return 0;
}
