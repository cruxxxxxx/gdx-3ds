/* Spike stub: SDLAudioPlayer.h leaks into the include graph via AudioPlayer.h /
 * classes.h. Only header-level types are needed for the compile gate; the SDL
 * player is never instantiated in the 3DS carve. */
#pragma once
#include <stdint.h>

typedef uint32_t SDL_AudioDeviceID;
typedef uint16_t SDL_AudioFormat;

typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint32_t size;
    void (*callback)(void* userdata, uint8_t* stream, int len);
    void* userdata;
} SDL_AudioSpec;
