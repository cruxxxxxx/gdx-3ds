// G-Diffuser — GDX_INPUT_SCRIPT: dev-only deterministic tick-level input playback.
//
// For unattended automated testing. With GDX_INPUT_SCRIPT=<path> set at process start, the port
// replays a scripted sequence of pad-0 inputs at the game-input-poll cadence (once per
// gdx_controller_poll(), i.e. once per host frame), REPLACING physical/LUS input while the script
// runs. Unset, it costs one cached getenv() per process and nothing else.
//
// Dev-only: no menu/UI surface. See gdx_input_script.c for the script format and the parser.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ABI-compatible with the decomp's u16 buttonCurrent / s8 stickX/stickY, but spelled in raw C
// types so this header has no decomp include dependency.
typedef struct GdxInputPad {
    unsigned short buttons; // N64 OSContPad bitmask (CONT_A/B/G/START/UP/DOWN/LEFT/RIGHT/L/R/E/D/C/F)
    signed char stickX;     // -80..80
    signed char stickY;     // -80..80
} GdxInputPad;

// Called once per gdx_controller_poll(), AFTER the raw LUS pad read (and the legacy
// gdx-autoinput.txt mechanism) land in `pad`, and BEFORE input_bridge.c's digital-stick derivation
// / edge accumulation runs. When active, OVERWRITES *pad and advances the script's cursor by
// exactly one poll, so scripted edges flow through the same accumulate-then-finalize path physical
// input uses and nothing downstream needs to know the source changed.
void gdx_input_script_override(GdxInputPad* pad);

#ifdef __cplusplus
}
#endif
