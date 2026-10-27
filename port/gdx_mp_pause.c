/* port/gdx_mp_pause.c — the shared pause's hold-to-confirm. Contract: gdx_mp_pause.h. */
#include "gdx_mp_pause.h"

#include <string.h>

void gdx_mp_pause_gate_reset(GdxMpPauseGate* g) {
    memset(g, 0, sizeof(*g));
}

static void ForgetLifted(GdxMpPauseGate* g, uint16_t down) {
    g->passMask &= down;
    g->holdMask &= down;
}

static void TakeNewPresses(GdxMpPauseGate* g, uint16_t pressed, int gated, int option, uint32_t nowMs) {
    if (pressed == 0) {
        return;
    }
    if (!gated) {
        g->passMask |= pressed;
        return;
    }
    if (g->holdMask == 0) {
        g->option = option;
        g->sinceMs = nowMs;
    }
    g->holdMask |= pressed;
}

static void RunHold(GdxMpPauseGate* g, int gated, int option, uint32_t nowMs, int holdMs) {
    if (g->holdMask == 0) {
        return;
    }
    if (option != g->option) {
        g->option = option; /* the cursor moved: the hold starts over on the new option */
        g->sinceMs = nowMs;
        return;
    }
    if (gated && (uint32_t) (nowMs - g->sinceMs) >= (uint32_t) holdMs) {
        g->passMask |= g->holdMask; /* this tick's pad carries the press edge */
        g->holdMask = 0;
    }
}

uint16_t gdx_mp_pause_gate(GdxMpPauseGate* g, uint16_t buttons, uint16_t confirmMask, int gated, int option,
                           uint32_t nowMs, int holdMs) {
    const uint16_t down = (uint16_t) (buttons & confirmMask);
    const uint16_t pressed = (uint16_t) (down & ~g->prev);
    g->prev = down;
    if (holdMs <= 0) {
        g->passMask = down;
        g->holdMask = 0;
        return buttons;
    }
    ForgetLifted(g, down);
    TakeNewPresses(g, pressed, gated, option, nowMs);
    RunHold(g, gated, option, nowMs, holdMs);
    return (uint16_t) (buttons & ~g->holdMask);
}

int gdx_mp_pause_gate_permille(const GdxMpPauseGate* g, int gated, uint32_t nowMs, int holdMs) {
    uint32_t held;
    if (!gated || g->holdMask == 0 || holdMs <= 0) {
        return 0;
    }
    held = (uint32_t) (nowMs - g->sinceMs);
    if (held >= (uint32_t) holdMs) {
        return 1000;
    }
    return (int) (held * 1000u / (uint32_t) holdMs);
}
