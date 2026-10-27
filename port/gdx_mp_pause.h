/* port/gdx_mp_pause.h — mp-menus #10: the shared pause's hold-to-confirm (pure: no clock, no game).
 *
 * WHY. A multiplayer pause is shared: the pads are exchanged in lockstep, so when any player
 * presses START every console pauses at the same tick, and the stock pause menu then follows the
 * PAUSER's pad only (decomp menus.c: sPausePlayerNum). So the rule is deterministic as it stands:
 * any player may pause; only the player who paused moves the cursor, resumes or ends the race.
 * Every choice but CONTINUE ends the race for everyone (gdx3ds_mp_game.c), so that A must not be
 * a slip of the thumb: it is held for [ui] hold_confirm_ms first.
 *
 * HOW. The gate sits on THIS console's pad before it enters the lockstep (so what every console
 * simulates is still just pads). While `gated` (this console paused, the menu takes input, the
 * highlighted option ends the race), a NEW press of a confirm button (A / START) is held back --
 * the pad says "up" -- until it has been down holdMs on the same option; then the pad says "down"
 * and the game sees the press edge on that tick. Let go early (or move the cursor) and nothing
 * reaches the game. A button already down before the gate saw it press (accelerating into the
 * pause) is never held back and never makes an edge. holdMs <= 0 = no gate. */
#ifndef GDX_MP_PAUSE_H
#define GDX_MP_PAUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxMpPauseGate {
    uint16_t prev;      /* the raw confirm bits last call */
    uint16_t passMask;  /* down and going through (until they lift) */
    uint16_t holdMask;  /* down and held back (the hold is running) */
    int option;         /* the option the hold runs on */
    uint32_t sinceMs;   /* when it started (or the option last changed) */
} GdxMpPauseGate;

void gdx_mp_pause_gate_reset(GdxMpPauseGate* g);

/* The buttons to send this tick. confirmMask: the bits that confirm a menu choice (A | START). */
uint16_t gdx_mp_pause_gate(GdxMpPauseGate* g, uint16_t buttons, uint16_t confirmMask, int gated, int option,
                           uint32_t nowMs, int holdMs);

/* The running hold, 0..1000 (0 = none). */
int gdx_mp_pause_gate_permille(const GdxMpPauseGate* g, int gated, uint32_t nowMs, int holdMs);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_PAUSE_H */
