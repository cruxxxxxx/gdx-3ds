// G-Diffuser — host input bridge for the PORT.
//
// The decomp's N64 SI polling path is disabled under PORT (Controller_Init / Controller_Reset
// return early in decomp/src/sys/controller.c, and every Controller_UpdateInputs call site in
// game.c is #ifndef PORT), so the game never sees a connected controller unless we feed the decomp
// controller globals (gControllers[] / gControllerStatus[] / gPlayerControlPorts[] /
// gControllersConnected / gSharedController) from the host every frame. This file is the whole
// replacement for Controller_Init + Controller_UpdateInputs.
//
// ALL FOUR PORTS, NOT JUST PORT 1:
// -----------------------------------------------------------------------------------------------
// Presence is derived per port from what the LUS ControlDeck can actually read (see
// gdx_sync_controller_ports below), each port's resolved input lands in its own gControllers[i],
// and gSharedController is the OR-aggregate across connected ports exactly as
// Controller_UpdateInputs builds it on hardware. Hardcoding "one controller, in port 1" points
// players 2-4 at PORT_DISCONNECTED (4, controller.h:40), which resolves to the never-written
// gControllers[4] dummy at racer.c:4952 and makes every split-screen VS / Death Race mode
// unplayable.
//
// INPUT SOURCE — libultraship (LUS) ControlDeck:
// -----------------------------------------------------------------------------------------------
// The GAME's input read goes through the LUS ControlDeck, which already does SDL gamepad
// auto-detection, keyboard-as-a-mappable-device, mouse mappings, deadzone/sensitivity curves, and
// the remapping the Input Editor (F1 -> Controls) writes. Reading the physical keyboard directly
// bypasses all of that. Because the ControlDeck is C++ and this translation unit is C, the actual
// read happens in an extern "C" shim in main.cpp (gdx_lus_read_pads) that calls
// ControlDeck::WriteToPad() once and hands back every port's resolved N64 state as plain scalars.
// This file keeps ownership of the decomp side: per-port presence, the digital stick direction,
// pressed/released/retrigger edges, and gSharedController.
//
// N64 BUTTON BITMASK — no translation table needed:
//   The decomp (decomp/include/controller.h -> PR/os_cont.h) and LUS
//   (libultraship/libultra/controller.h) use the IDENTICAL standard N64 OSContPad bitmask:
//     A=0x8000  B=0x4000  Z=0x2000  START=0x1000  DUP=0x0800  DDOWN=0x0400  DLEFT=0x0200
//     DRIGHT=0x0100  L=0x0020  R=0x0010  CUP=0x0008  CDOWN=0x0004  CLEFT=0x0002  CRIGHT=0x0001
//   So LUS's OSContPad.button maps 1:1 onto the decomp's buttonCurrent — we just mask with
//   CONT_MASK. The analog stick (OSContPad.stick_x/stick_y) is already produced by LUS in the
//   N64 -80..80 range the decomp expects, so it copies straight into stickX/stickY.
//
// THREADING: gdx_controller_poll() runs on the host/main thread (main.cpp frame loop) before
// gdx_dispatch() runs the game fibers, which then read the globals we wrote. Single-threaded,
// no synchronization needed.

#ifndef _LANGUAGE_C
#define _LANGUAGE_C 1
#endif

#include "PR/os_pfs.h"
#include "controller.h"
#include "fzx_game.h" // GameMode values + GET_MODE (in-race safety checks)
#include "gdx_input_script.h" // GDX_INPUT_SCRIPT: dev-only deterministic input playback (see file header)
#include "gdx_input_rec.h"    // ORACLE: .gdxrec record/replay stage of the unified injector
#include "gdx_hud_placement.h" // nibottom Phase 2: the HUD placement lever predicates below
#include "gdx_snapshot.h"     // mp-snapshot: GDX_SNAP_STATE_BSS + the pad record/replay hooks
#include "port_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

#ifdef errno
#undef errno
#endif

extern s32 gControllersConnected;

// Implemented in main.cpp (C++). Fills per-port N64 button bitmasks + analog sticks (-80..80) and
// a per-port "this port can produce input right now" flag. Returns 0 if the ControlDeck is
// unavailable, leaving every slot zeroed — degrade to zero input, never crash. The scalar widths
// match the decomp's u16 / s8 exactly.
//
// MUST be a single batched call rather than four per-port reads: ControlDeck::WriteToPad() has
// per-call side effects (mouse-wheel buffer decay, the simulated-input-lag pad deque) that only
// behave correctly when it runs once per frame. See gdx_lus_read_pads() in main.cpp.
extern int gdx_lus_read_pads(int capacity, u16* out_buttons, s8* out_stick_x, s8* out_stick_y,
                             u8* out_connected);

// How many ports mirror port 1 after the scripted-input injector has run. Defined only by a
// backend that reports more ports than it physically has ([debug] pads on 3DS,
// port/3ds/os/gdx3ds_os_ctru.c); weak so the desktop build, which has real pads, links with
// nothing here and never mirrors.
extern int gdx_port_mirror_count(void) __attribute__((weak));
// mp-spike: [net] mp lockstep hook (port/3ds/net/gdx3ds_mp.c). Weak: the desktop build has no
// MP and the 3DS one returns 0 on its first int test with the lever off (the default).
extern int gdx_mp_exchange(unsigned long long tick, u16* buttons, s8* stickX, s8* stickY, u8* connected,
                           int capacity) __attribute__((weak));
// mp-snapshot (port/gdx_snapshot.h, port/3ds/gdx_snapshot.c): record the tick's final pads while a
// snapshot window is open, and feed them back to a resimulated tick. Weak: desktop has no module.
extern void gdx_snap_note_pads(unsigned long long tick, const unsigned short* buttons, const signed char* stickX,
                               const signed char* stickY, const unsigned char* connected, int count)
    __attribute__((weak));
extern int gdx_snap_recorded_pads(unsigned long long tick, unsigned short* buttons, signed char* stickX,
                                  signed char* stickY, unsigned char* connected, int count) __attribute__((weak));

// Zero exactly the input fields Controller_ClearInputs() clears (decomp/src/sys/controller.c:47-51)
// without touching pfs / rumble bookkeeping. Used when a port goes away mid-session so a pad that
// was unplugged mid-corner cannot leave a latched throttle behind, and to initialise the
// gControllers[MAXCONTROLLERS] sentinel that disconnected player slots resolve to.
static void gdx_clear_controller_inputs(Controller* controller) {
    controller->retriggerCurrentButtonPress = 0;
    controller->buttonCurrent = 0;
    controller->buttonPressed = 0;
    controller->buttonReleased = 0;
    controller->buttonPrev = 0;
    controller->stickX = 0;
    controller->stickY = 0;
    controller->stickCurrent = 0;
    controller->stickPressed = 0;
    controller->stickReleased = 0;
    controller->stickPrev = 0;
    controller->sameInputsHeldCounter = 0;
}

// Per-port host-frame edge accumulator state. See the "ALIGNED TO THE GAME FRAME" comment on
// gdx_update_port_inputs() for what each field is for.
typedef struct GdxPortInputState {
    u16 prevHostButtons;
    u16 accumPressed;
    u16 accumReleased;
    u16 prevGameButtons;
    u8 prevHostStick;
    u8 accumStickPressed;
    u8 accumStickReleased;
    u8 prevGameStick;
} GdxPortInputState;

// mp-snapshot: simulation state (the edge accumulators feed buttonPressed/Released), so it lives in
// the snapshot block and a restore rewinds it with the decomp image.
static GdxPortInputState sPortInput[MAXCONTROLLERS] GDX_SNAP_STATE_BSS;
// gdx_sync_controller_ports' per-port presence latch and gdx_controller_apply's first-poll flag
// (both were function statics; file scope so they can carry the snapshot section).
static u8 s_prevConnected[MAXCONTROLLERS] GDX_SNAP_STATE_BSS;
static int s_have_baseline GDX_SNAP_STATE_BSS;

// ── Connected-port bookkeeping ───────────────────────────────────────────────────────────────────
// Publishes the ControlDeck's per-port availability into the three decomp globals the game reads:
//
//   gControllers[i].errno / gControllerStatus[i].errno — per-PORT presence. Read by
//       Controller_ClearInputs (controller.c:47).
//   gPlayerControlPorts[p]                             — PLAYER p -> port index, or
//       PORT_DISCONNECTED (4). Indexed by racer id at racer.c:4952 and by camera index at
//       camera.c:3174/3253, so it is the map that decides which pad drives which car in
//       split-screen VS / Death Race.
//   gControllersConnected                              — number of live ports. The main menu gates
//       VS Battle on it (main_menu.c:205) and clamps the 2/3/4-player selector to it
//       (main_menu.c:323-324, 869), so an inaccurate count is what made split-screen unreachable.
//
// SLOT STABILITY (why this is incremental instead of recomputed): recomputing the compact map
// every frame reshuffles players when a pad drops. With ports 0,1,2 live, losing port 1 would
// renumber player 2 onto port 2 — the human holding pad 3 would suddenly be driving car 2
// mid-race. Instead each port owns its player slot for as long as it stays connected, exactly like
// the hardware disconnect path (controller.c:98-105): a departing port frees only its own slot. A
// returning port takes the lowest free slot, which is the port's hot-plug improvement over
// hardware (where Controller_Init only ever ran once at boot).
//
// PORT 1 IS PINNED CONNECTED by the caller. gControllers[gPlayerControlPorts[0]] is the menu/editor
// input source in dozens of places (e.g. course_edit/188850.c:114), and a host has no
// "please reconnect controller 1" screen to recover from, so letting player 1's slot ever become
// PORT_DISCONNECTED would wedge the UI rather than degrade it.
static void gdx_sync_controller_ports(const u8* connected) {
    // s_prevConnected (file scope, above) is zero-initialised: every port starts out "was
    // disconnected", so the first call runs every live port through the connect path in ascending
    // order and produces exactly the compact assignment Controller_Init builds on hardware
    // (controller.c:220-237).
    static int s_initialized = 0;
    int i;
    int j;

    if (!s_initialized) {
        for (i = 0; i < MAXCONTROLLERS; i++) {
            gPlayerControlPorts[i] = PORT_DISCONNECTED;
        }
        gControllersConnected = 0;

        // The sentinel every disconnected player slot resolves to. Nothing under PORT ever writes
        // it (Controller_UpdateInputs is compiled out in game.c), so zeroing it once guarantees an
        // absent player reads neutral input forever instead of whatever the BSS happened to hold.
        gdx_clear_controller_inputs(&gControllers[MAXCONTROLLERS]);
        gControllers[MAXCONTROLLERS].errno = CONT_NO_RESPONSE_ERROR;

        gdx_port_logf("[input] LUS ControlDeck active — remap in F1 > Controls > Input Editor "
                      "(default keyboard: Space=Start, X=A, C=B, WASD=stick, arrows=C)\n");
        s_initialized = 1;
    }

    for (i = 0; i < MAXCONTROLLERS; i++) {
        const u8 isConnected = (connected[i] != 0);

        gControllers[i].errno = isConnected ? 0 : CONT_NO_RESPONSE_ERROR;
        gControllerStatus[i].errno = isConnected ? 0 : CONT_NO_RESPONSE_ERROR;
        gControllerStatus[i].type = CONT_TYPE_NORMAL;

        if (isConnected == s_prevConnected[i]) {
            continue;
        }

        if (isConnected) {
            for (j = 0; j < MAXCONTROLLERS; j++) {
                if (gPlayerControlPorts[j] == PORT_DISCONNECTED) {
                    gPlayerControlPorts[j] = i;
                    break;
                }
            }
            gControllersConnected++;
            gdx_port_logf("[input] port %d connected (players now %d)\n", i + 1, gControllersConnected);
        } else {
            for (j = 0; j < MAXCONTROLLERS; j++) {
                if (gPlayerControlPorts[j] == i) {
                    gPlayerControlPorts[j] = PORT_DISCONNECTED;
                    break;
                }
            }
            gControllersConnected--;
            // A pad that vanishes mid-race must not leave its last frame latched. The racer it was
            // driving now reads the neutral gControllers[4] sentinel, but the port's own struct is
            // still live for anything holding a direct pointer (e.g. course_edit/188850.c:859).
            gdx_clear_controller_inputs(&gControllers[i]);
            memset(&sPortInput[i], 0, sizeof(sPortInput[i]));
            gdx_port_logf("[input] port %d disconnected (players now %d)\n", i + 1, gControllersConnected);
        }

        s_prevConnected[i] = isConnected;
    }
}

/* Scripted input for headless/automated test runs. If gdx-autoinput.txt sits next to the exe it
   is parsed once and combined with the live (LUS-sourced) input every poll.

   FORMAT — two timebases, selected by a marker on the FIRST non-blank line:

   TICK MODE (deterministic — required for the bit-identical PCM gate):
       First non-blank line is the literal word `ticks` (case-insensitive). Every later line is
           <tick> <INPUT> [holdTicks]
       <tick> is a VI-tick index (the counter below, bumped once per gdx_controller_poll(), i.e.
       once per host frame and wall-clock-independent); [holdTicks] defaults to 15. Expressing the
       schedule purely in ticks is what makes a script produce identical input on identical frames
       across runs and machines — the property golden PCM capture needs.

   LEGACY SECONDS MODE (selected whenever the first token is not `ticks`):
           <secondsSinceLoad> <INPUT>
       clocked off the host monotonic wall clock; each input is held for 0.25 s.

   INPUT names (both modes): START A B UP DOWN LEFT RIGHT (buttons); STICK_UP STICK_DOWN
   STICK_LEFT STICK_RIGHT (analog, ±80). Blank lines and lines starting with '#' are ignored.
   Up to 64 events.

   TICK MODE ONLY:
       <tick> SHOT <label>
   arms a one-shot framebuffer dump of the next presented frame when the tick counter reaches
   <tick> (gdx_request_frame_dump -> "autotest/<label>.bmp", the same sink the GDX_INPUT_SCRIPT
   SHOT command uses). Deterministic-tick screenshots are the visual oracle for headless runs
   where no interactive capture exists (3DS under Azahar). */
static unsigned long long gdx_autoinput_now_ms(void) {
#ifdef _WIN32
    return (unsigned long long) GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000ull + (unsigned long long) (ts.tv_nsec / 1000000);
#endif
}

/* ~0.25 s at 60 Hz, matching the legacy seconds-mode hold. */
#define GDX_AUTOINPUT_DEFAULT_HOLD_TICKS 15

/* Returns 1 if the token is recognized. */
static int gdx_autoinput_parse_input(const char* name, u16* out_btn, s8* out_x, s8* out_y) {
    u16 b = 0;
    s8 x = 0;
    s8 y = 0;
    if (strcmp(name, "START") == 0) b = BTN_START;
    else if (strcmp(name, "A") == 0) b = BTN_A;
    else if (strcmp(name, "B") == 0) b = BTN_B;
    else if (strcmp(name, "UP") == 0) b = BTN_UP;
    else if (strcmp(name, "DOWN") == 0) b = BTN_DOWN;
    else if (strcmp(name, "LEFT") == 0) b = BTN_LEFT;
    else if (strcmp(name, "RIGHT") == 0) b = BTN_RIGHT;
    else if (strcmp(name, "STICK_UP") == 0) y = 80;
    else if (strcmp(name, "STICK_DOWN") == 0) y = -80;
    else if (strcmp(name, "STICK_LEFT") == 0) x = -80;
    else if (strcmp(name, "STICK_RIGHT") == 0) x = 80;
    else return 0;
    *out_btn = b;
    *out_x = x;
    *out_y = y;
    return (b != 0 || x != 0 || y != 0);
}

/* n64_gfx_bridge.cpp: arms a one-shot BMP dump ("autotest/<label>.bmp") of the next presented
   frame. Same boundary idiom as the other cross-TU externs in this file. */
extern void gdx_request_frame_dump(const char* label);

/* ORACLE: the input tick. Incremented exactly once per gdx_controller_poll() (the single call
   site in the host loop, one per VI tick), so it is the clock every scripted source, the .gdxrec
   line numbering and the [fh] receipt's tick= field share. Was autoinput's private s_tick. */
static unsigned long long s_input_tick GDX_SNAP_STATE_BSS; /* mp-snapshot: rewound by a restore */

unsigned long long gdx_input_tick(void) {
    return s_input_tick;
}

int gdx_input_game_mode(void); /* defined below the gGameMode extern */

static void gdx_autoinput_apply(u16* io_buttons, s8* io_stick_x, s8* io_stick_y) {
    static int s_state = -1; /* -1 unread, 0 absent, 1 loaded */
    static int s_tickmode = 0;
    /* `at` and `hold` are in the active timebase's native unit: VI-ticks (tick mode) or seconds
       (legacy mode). A nonempty `shot` marks a SHOT event: btn/stick stay 0 and `fired` makes it
       one-shot. */
    static struct { double at; double hold; u16 btn; s8 stick_x; s8 stick_y;
                    char shot[24]; u8 fired; } s_events[64];
    static int s_count = 0;
    static unsigned long long s_start_ms = 0;    /* seconds-mode wall-clock baseline */
    const unsigned long long s_tick = s_input_tick; /* tick-mode timebase (see gdx_input_tick) */

    if (s_state == -1) {
        FILE* f = fopen("gdx-autoinput.txt", "r");
        s_state = 0;
        if (f != NULL) {
            char line[128];
            int sniffed = 0;
            while (s_count < 64 && fgets(line, (int) sizeof(line), f) != NULL) {
                char name[16];
                double at;
                int hold_ticks;
                u16 b;
                s8 x, y;
                /* A line longer than the buffer comes back from fgets WITHOUT its
                   newline, and the next fgets returns its tail. That tail does not start
                   with '#', so an over-long COMMENT used to be parsed as the first data
                   line — and because it is not the word `ticks`, the whole script fell
                   back to the wall-clock seconds timebase and every SHOT was silently
                   lost (symptom: "autoinput script loaded: 1 events (seconds timebase)"
                   from a tick-mode file). Discard the remainder so one physical line is
                   always one logical line, whatever the buffer size. */
                if (strchr(line, '\n') == NULL && !feof(f)) {
                    int ch;
                    while ((ch = fgetc(f)) != EOF && ch != '\n') {
                        /* drain to end of line */
                    }
                }
                {
                    const char* p = line;
                    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                    if (*p == '\0' || *p == '#') continue;
                }
                if (!sniffed) {
                    char first[16];
                    if (sscanf(line, "%15s", first) == 1 &&
                        (strcmp(first, "ticks") == 0 || strcmp(first, "TICKS") == 0 ||
                         strcmp(first, "Ticks") == 0)) {
                        s_tickmode = 1;
                        sniffed = 1;
                        continue; /* the marker line carries no event */
                    }
                    s_tickmode = 0;
                    sniffed = 1;
                    /* fall through: this line is the first event (legacy seconds mode) */
                }
                if (s_tickmode) {
                    int n = sscanf(line, "%lf %15s %d", &at, name, &hold_ticks);
                    if (n < 2) continue;
                    if (strcmp(name, "SHOT") == 0) {
                        /* <tick> SHOT <label> — one-shot frame dump, no pad state. */
                        char label[24];
                        if (sscanf(line, "%*f %*s %23s", label) == 1) {
                            memset(&s_events[s_count], 0, sizeof(s_events[s_count]));
                            s_events[s_count].at = at;
                            strcpy(s_events[s_count].shot, label);
                            s_count++;
                        }
                        continue;
                    }
                    if (n < 3 || hold_ticks <= 0) hold_ticks = GDX_AUTOINPUT_DEFAULT_HOLD_TICKS;
                    if (gdx_autoinput_parse_input(name, &b, &x, &y)) {
                        memset(&s_events[s_count], 0, sizeof(s_events[s_count]));
                        s_events[s_count].at = at;
                        s_events[s_count].hold = (double) hold_ticks;
                        s_events[s_count].btn = b;
                        s_events[s_count].stick_x = x;
                        s_events[s_count].stick_y = y;
                        s_count++;
                    }
                } else {
                    if (sscanf(line, "%lf %15s", &at, name) != 2) continue;
                    if (gdx_autoinput_parse_input(name, &b, &x, &y)) {
                        s_events[s_count].at = at;
                        s_events[s_count].hold = 0.25; /* legacy fixed 0.25 s hold */
                        s_events[s_count].btn = b;
                        s_events[s_count].stick_x = x;
                        s_events[s_count].stick_y = y;
                        s_count++;
                    }
                }
            }
            fclose(f);
            s_state = 1;
            s_start_ms = gdx_autoinput_now_ms();
            gdx_port_logf("[input] autoinput script loaded: %d events (%s timebase)\n", s_count,
                          s_tickmode ? "tick" : "seconds");
        }
    }
    if (s_state != 1) {
        return;
    }
    {
        double now = s_tickmode ? (double) s_tick
                                : (double) (gdx_autoinput_now_ms() - s_start_ms) / 1000.0;
        int i;
        for (i = 0; i < s_count; i++) {
            if (s_events[i].shot[0] != '\0') {
                if (!s_events[i].fired && now >= s_events[i].at) {
                    s_events[i].fired = 1;
                    gdx_request_frame_dump(s_events[i].shot);
                    gdx_input_rec_note_shot(s_events[i].shot); /* recorded on this tick's line */
                    gdx_port_logf("[input] autoinput SHOT '%s' armed at tick %.0f\n",
                                  s_events[i].shot, now);
                }
                continue;
            }
            if (now >= s_events[i].at && now < s_events[i].at + s_events[i].hold) {
                *io_buttons |= s_events[i].btn;
                if (s_events[i].stick_x != 0) *io_stick_x = s_events[i].stick_x;
                if (s_events[i].stick_y != 0) *io_stick_y = s_events[i].stick_y;
            }
        }
    }
}

/* ORACLE: THE injector. Every scripted source lands here, at the rawest point where the pad
   state exists (before the digital-stick derivation and the edge accumulation), so a scripted
   press flows through the identical accumulate-then-finalize path physical input uses. Stages,
   in order: legacy gdx-autoinput.txt (OR-in; skipped while a .gdxrec replay is armed — the
   replay wins), GDX_INPUT_SCRIPT override, .gdxrec replay override, then the .gdxrec recorder
   sees the FINAL pad. hwlink's gdx_replay_start() enters through the replay stage. */
void gdx_input_inject(unsigned long long tick, GdxInputPad* pad) {
    (void) tick; /* the stages read gdx_input_tick(); the parameter documents the contract */
    if (!gdx_input_rec_replay_armed()) {
        gdx_autoinput_apply(&pad->buttons, &pad->stickX, &pad->stickY);
    }
    gdx_input_script_override(pad);
    gdx_input_rec_poll((unsigned long) s_input_tick, pad, gdx_input_game_mode());
}

// Decomp race state. Kept behind small C helpers so every photo-mode consumer uses the same
// definition and the ImGui ghost importer can avoid writing persistence while a race is live.
extern s32 gGameMode;
extern s8 gGamePaused;

/* ORACLE: the [fh] receipt's mode= field and the .gdxrec "# mode" markers. */
int gdx_input_game_mode(void) {
    return (int) GET_MODE(gGameMode);
}

// Declared locally -- same pattern as gdx_lus_read_pads above -- to avoid pulling a LUS/C++
// umbrella header into this decomp-environment C TU. int32_t is `int` on this target, so the
// plain-int prototype is ABI-identical and links to the same undecorated C symbol.
extern int CVarGetInteger(const char* name, int defaultValue);

// True for the game modes that are an actual on-track race (where a retry is meaningful). GET_MODE
// (fzx_game.h:30) strips the high F3D gfx-mode bits so this matches whether or not they are set.
static int gdx_gamemode_is_race(s32 mode) {
    switch (GET_MODE(mode)) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_DEATH_RACE:
            return 1;
        default:
            return 0;
    }
}

// Exposed for the Practice-tab ghost import/export UI (gdx_menu.cpp). Ghost SRAM writes must not
// race the game fiber, so the menu disables Import while an on-track race is live; it is safe to
// touch the ghost save from the menus/attract screens (where the game saves ghosts itself).
int gdx_input_in_gameplay(void) {
    return gdx_gamemode_is_race(gGameMode);
}

#ifdef __3DS__
extern int gdx3ds_mp_session_levers_forced(void) __attribute__((weak)); /* gdx3ds_autodrive.c (MP30) */
#endif

int gdx_photo_mode_active(void) {
#ifdef __3DS__
    /* MP30: photo mode moves camera 0 and skips the pause menu's draw (which draws RNG): never in
     * a lockstep session. */
    if ((&gdx3ds_mp_session_levers_forced != 0) && gdx3ds_mp_session_levers_forced()) {
        return 0;
    }
#endif
    return CVarGetInteger("gEnhancements.Practice.PhotoMode", 0) && (gGamePaused != 0);
}

// Shared predicate for the true-widescreen 2D UI slice. Anchor/stretch extra-geometry-mode
// emitters in decomp draw code must gate on this at the call site so that CVar-off builds emit
// a bit-identical display list to stock, instead of relying solely on the interpreter's own
// CVar check when it consumes the mode bits (defense in depth; see GfxDrawRectangle).
int gdx_widescreen_ui_active(void) {
    return CVarGetInteger("gEnhancements.Graphics.Widescreen", 1) &&
           CVarGetInteger("gEnhancements.Graphics.WidescreenUI", 0);
}

// Split-screen (VS Battle / Death Race 2P-4P) variant of the predicate above.
//
// WHY A SEPARATE SWITCH. Every widescreen 2D anchor scope shipped so far is gated on
// `gNumPlayers == 1` at the call site (hud.c:1044 `case 1:`, menus.c:5617-5657,
// machine.c:1226-1884), so extending the anchors to 2P/3P/4P is a NEW visual policy for shipped
// modes. It gets its own opt-out instead of silently riding gEnhancements.Graphics.WidescreenUI --
// a player who likes the 1P corner HUD must still be able to keep split-screen at stock placement.
//
// Strict subset of the 1P predicate (Widescreen AND WidescreenUI must both be on), so there is no
// state where split-screen is anchored while 1P is not.
//
// The CVar has no menu widget yet, so it is never registered; CVarGetInteger's default (1) makes
// the feature work regardless. Registering it would only add the toggle and the config entry.
int gdx_widescreen_split_ui_active(void) {
    return gdx_widescreen_ui_active() && CVarGetInteger("gEnhancements.Graphics.WidescreenSplitUI", 1);
}

// nibottom Phase 2 / nihudtop Phase 3: the bottom-screen DASH HUD's placement lever, the
// per-element top-screen predicate built on it, and the CUSTOM-mask override hook all moved to
// their own translation unit, port/gdx_hud_placement.c -- it has no LUS/controller dependencies,
// unlike the rest of this file, so it (and only it) can be compiled standalone for
// port/tests/gdx_hud_placement_host_tests.c the same way gdx3ds_native_cfg.c / gdx3ds_dynlod.c
// already are. See that file for gdx_hud_placement_mode/_effective_mode/_element_on_top/
// _top_element_mask/_top_suppressed/_minimap_suppressed/_set_custom_top_mask/_name.

// Race-entry wipe suppression. The wipe curtain is what puts the black staircase wedges on the
// screen corners through the intro/countdown (probe-confirmed 2026-08-08: nothing else draws the
// corners once racing starts); at widescreen stretch it reaches the true window corners, which is
// what players notice. Skipping the DRAW leaves the transition state machine untouched, so
// timing, capture, and cleanup run exactly as stock.
int gdx_hide_race_curtain(void) {
    return CVarGetInteger("gEnhancements.Graphics.HideRaceCurtain", 0);
}

// Overscan-border removal. The game frames EVERY screen inside the CRT-safe area — viewport
// vscale 592/448 (a 296x224 render) plus scissor gScissorBoxFullScreen {12,8,308,232} — which a
// TV's overscan used to hide. On a monitor it reads as a black border around the whole game.
// When on, the full-screen viewport/scissor open up to the true 320x240 frame.
int gdx_remove_borders(void) {
    return CVarGetInteger("gEnhancements.Graphics.RemoveBorders", 0);
}

// The Expansion Kit editors are 4:3-authored 2D/3D composites: their 3D layers (Course Edit's
// canvas through the shared Background/Course/Racer_Draw path, Create Machine's preview and parts
// grid) would otherwise pick up the global hor+ widescreen vertex correction while their 2D layers
// stay pillarboxed, splitting one screen across two aspect ratios. The published flag folds into
// the renderer's existing Widescreen==0 pillarbox path (AdjXForAspectRatio, StartFrame's forced
// offscreen target, Fast3dGui::DrawGame's centered 4:3 composite).
//
// The flag is a plain process global in libultraship (interpreter.cpp), NOT a CVar: as a CVar it
// persisted as 1 into gdiffuser.cfg.json whenever the config was saved from inside an editor,
// pillarboxing pre-tick boot frames on the next launch. Runtime state never belongs in the config
// file.
//
// gdx_fixed_aspect_publish() is called from the decomp's mode-flip site under PORT as well as from
// the per-frame tick, so consumers that read the flag live (Transition_Draw, Fast3dGui::DrawGame)
// and the interpreter's per-task re-latch see the post-flip truth within the same frame.
//
// GET_MODE masks off the F3D-variant bits (0xC000), so GAMEMODE_CREATE_MACHINE (0x10) matches
// every sub-state of that editor — the ENTRY "SELECT MACHINE" screen and its file/clear sub-menus
// included — and the classification never has to consult gWorksMachineMode. The in-race
// GAMEMODE_LX_MACHINE_SETTINGS is a different mode value (0x9) and is intentionally NOT covered.
static int gdx_mode_forces_fixed_aspect(s32 gamemode) {
    s32 mode = GET_MODE(gamemode);
    return (mode == GAMEMODE_COURSE_EDIT) || (mode == GAMEMODE_CREATE_MACHINE);
}

#if defined(GDX_PLATFORM_3DS) && defined(EXPANSION_KIT)
/* B11g -- why the Course Editor drew NOTHING on 3DS, and why the flag above is the cause.
 *
 * The flag's desktop implementation is a PAIR of steps that only make sense together
 * (vendored interpreter.cpp): AdjXForAspectRatio returns x unchanged, so the frame renders
 * native 4:3 stretched across the whole window, AND StartFrame forces mRendersToFb, so the
 * whole frame lands in the offscreen mGameFb texture -- which Fast3dGui::DrawGame then
 * composites back into a CENTRED 4:3 sub-rect of the window. Step two un-stretches step one.
 *
 * Neither half is right on 3DS, and the second one is fatal:
 *
 *  - There is no Gui on this platform, so NOTHING consumes the published texture.
 *    Fast3dWindow::GetGfxFrameBuffer (port/3ds/lus_glue/gdx3ds_fast3d_window.cpp:353) has no
 *    caller in the 3DS build at all, and the owned interpreter's epilogue
 *    (port/3ds/gfx/n64/gdx_interp.cpp:235-240) binds framebuffer 0, CLEARS it to opaque black
 *    and publishes the texture id to a consumer that does not exist. That is the whole of
 *    `[c3d] draws=N (scr=0 tex=N)` and of every in-editor SHOT reading 0 non-black of 96,000.
 *    gdx3ds_fast3d_window.cpp:177-183 already names this exact failure ("a guaranteed-black top
 *    screen even with a fully working draw path") for the OTHER disjunct of the same `if`.
 *
 *  - The 3DS does not NEED step one either, because on this platform hor+ IS the pillarbox.
 *    AdjXForAspectRatio is x * (4/3) / aspect; the top screen is a fixed 400x240, so the factor
 *    is exactly (4/3)/(5/3) = 0.8 and 320-space content lands 1:1 in a centred 320-px band with
 *    40-px black bars. That is the same mapping ekdrive measured on the main menu: the slot-4
 *    sign at main_menu.c:612's x = 32 lands at NDC 32/160 - 1 = -0.8, times 0.8 = -0.64, i.e.
 *    screen pixel (1 - 0.64) / 2 * 400 = 72, 64 px wide -- its measured band is exactly 72..135.
 *
 * So on 3DS the request is not just unimplementable, it is unnecessary: the editors already get
 * a true 4:3 pillarbox from the path every other screen uses. Declining it here leaves the
 * editors rendering to framebuffer 0 like everything else. [debug] ek_fixed_aspect=1 restores
 * the desktop behaviour as a named bisect lever (and, on 3DS, restores the black screen).
 *
 * Scope: BOTH guards are load-bearing. GDX_PLATFORM_3DS keeps desktop untouched -- there the
 * pair is coherent and the composite exists. EXPANSION_KIT keeps the shipped 3DS build BYTE
 * IDENTICAL: gdx3ds_glue only gets EXPANSION_KIT=1 under GDX_3DS_EXPANSION_KIT, and the two
 * modes above are unreachable without a 64DD disk anyway.
 *
 * NOT fixed here, and filed in ekdraw-progress.md: the same StartFrame `if` also forces the
 * offscreen target when gEnhancements.Graphics.Widescreen is 0, which on 3DS is the identical
 * guaranteed-black screen for the WHOLE game. It is not reachable from this track's lever and
 * fixing it would move the shipped binary, so it is filed rather than taken in passing.
 */
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback);
extern void gdx_ek_note(const char* what, unsigned int a, unsigned int b, unsigned int c, unsigned int d);

static int gdx_ek_fixed_aspect_lever(void) {
    static int sLever = -1;
    if (sLever < 0) {
        sLever = gdx3ds_config_get_int("debug", "ek_fixed_aspect", 0) != 0 ? 1 : 0;
    }
    return sLever;
}

/* One always-on receipt per transition, not per frame: gdx_fixed_aspect_publish runs every
 * dispatch. a = the mode asked about, b = what the desktop flag would have been, c = what was
 * published, d = the lever. */
static int gdx_ek_fixed_aspect_resolve(s32 effectiveMode, int want) {
    static int sPrevPublished = -1;
    const int published = (want && !gdx_ek_fixed_aspect_lever()) ? 0 : want;
    if (published != sPrevPublished) {
        sPrevPublished = published;
        gdx_ek_note("[fbroute] fixedAspect mode/want/published/lever", (unsigned int) effectiveMode,
                    (unsigned int) want, (unsigned int) published,
                    (unsigned int) gdx_ek_fixed_aspect_lever());
    }
    return published;
}
#endif /* GDX_PLATFORM_3DS && EXPANSION_KIT */

void gdx_fixed_aspect_publish(void) {
    extern void gdx_set_force_fixed_aspect(int on); // libultraship interpreter.cpp
    extern s32 gQueuedGameMode;                     // decomp/src/game/game.c

    // Evaluate the flag on the mode that will actually RENDER this dispatch, not on the mode
    // currently latched in gGameMode.
    //
    // The game flips gGameMode = gQueuedGameMode MID-dispatch (game.c) and then renders the NEW
    // mode's appear transition in that SAME dispatch, but the renderer commits its whole-frame
    // pillarbox target from the PRE-dispatch flag (interpreter.cpp StartFrame) and never
    // recomputes it after the flip. Keying on gGameMode therefore made an editor->menu flip
    // composite the destination menu into a centered 4:3 pillarbox for one frame -- the visible
    // "squeeze then normal" on Main Menu -> Cup Select after being in an editor.
    //
    // While a mode change is pending the frame shown after the flip is gQueuedGameMode, and the
    // outgoing mode's remaining frames are hidden under the transition wipe (Transition_HideSet),
    // so keying on the DESTINATION mode is stable across the whole transition. Settled frames key
    // on gGameMode, so a real editor in steady state -- and the Create Machine sub-mode toggles,
    // which never change gGameMode -- is still forced to 4:3. The accepted cost is rendering the
    // outgoing screen in the destination aspect during its wipe, which the wipe covers.
    s32 effectiveMode = (gGameMode != gQueuedGameMode) ? gQueuedGameMode : gGameMode;
    int want = gdx_mode_forces_fixed_aspect(effectiveMode);
#if defined(GDX_PLATFORM_3DS) && defined(EXPANSION_KIT)
    want = gdx_ek_fixed_aspect_resolve(effectiveMode, want); /* B11g, see above */
#endif
    gdx_set_force_fixed_aspect(want);
#if defined(GDX_PLATFORM_3DS)
    /* [display] widescreen (port/3ds/gdx3ds_widescreen.h): the same destination-mode key, so a
       race's widescreen switches flip with the mode under the transition wipe. No-op when off. */
    {
        extern void gdx3ds_widescreen_publish(int kind);
        extern int gdx_fv_wide_single_view(void) __attribute__((weak)); /* game/gdx3ds_fullview_game.c */
        extern s32 gNumPlayers;
        int wideKind = 0;
        if (gdx_gamemode_is_race(effectiveMode)) {
            wideKind = (gNumPlayers > 1) ? 2 : 1;
        }
        /* mp-fullbleed: an MP fullview race shows ONE view full screen, so it is a 1P frame
           (RemoveBorders, 5:3 at 1:1) rather than a split one; render-only. */
        if (wideKind == 2 && &gdx_fv_wide_single_view != NULL && gdx_fv_wide_single_view()) {
            wideKind = 1;
        }
        gdx3ds_widescreen_publish(wideKind);
    }
#endif
}

void gdx_fixed_aspect_tick(void) {
    gdx_fixed_aspect_publish();
}

// Read-only bridge used by the ImGui input viewer. It exposes the exact mapped N64 state already
// delivered to the game, rather than polling SDL/ControlDeck a second time from the GUI pass.
int gdx_input_viewer_state(u16* out_buttons, s8* out_stick_x, s8* out_stick_y) {
    if (out_buttons != NULL) {
        *out_buttons = gControllers[0].buttonCurrent;
    }
    if (out_stick_x != NULL) {
        *out_stick_x = gControllers[0].stickX;
    }
    if (out_stick_y != NULL) {
        *out_stick_y = gControllers[0].stickY;
    }
    return gControllers[0].errno == 0;
}

// ── One port's raw pad state -> its decomp Controller, ALIGNED TO THE GAME FRAME ─────────────────
// gdx_controller_poll() runs once per HOST frame (main.cpp loop, ~60Hz), but screens that set a VI
// retrace divider run their game loop SLOWER: the scheduler (sys_main.c:428) posts the game's frame
// tick only every D_800CCFB8-th VI. The Expansion Kit Course Edit / Create Machine editors set
// D_800CCFBC=3 (19DD60.c:193), so their game loop consumes input at ~20Hz.
//
// Computing pressed/released EDGES every host frame therefore loses any press edge that lands on a
// host frame the game skips, and the editor feels unresponsive. So edges ACCUMULATE across every
// host frame and are finalized (along with the prev baseline and retrigger counter) only on the
// host frame the scheduler will deliver a game tick on — `gameBoundary`, computed once per poll by
// the caller because the divider is a property of the scheduler, not of any one port. For default
// screens (divider 1) it is true every frame, so behaviour matches plain per-frame edges.
static void gdx_update_port_inputs(int port, u16 buttons, s8 stick_x, s8 stick_y, int gameBoundary) {
    Controller* controller = &gControllers[port];
    GdxPortInputState* state = &sPortInput[port];
    u8 stick = 0;

    // ── Derive the digital stick direction from the analog stick ──────────────────────────────
    // Mirrors the decomp's own SI-read logic (decomp/src/sys/controller.c:119-133): a direction
    // latches once the axis passes ±50 (of 80). N64 convention: +Y is UP. The game reads
    // stickCurrent for menu navigation, so the exact threshold is what preserves menu feel.
    if (stick_x < -50) {
        stick |= STICK_LEFT;
    } else if (stick_x > 50) {
        stick |= STICK_RIGHT;
    }
    if (stick_y < -50) {
        stick |= STICK_DOWN;
    } else if (stick_y > 50) {
        stick |= STICK_UP;
    }

    {
        const u16 cur_buttons = (u16) (buttons & CONT_MASK);
        const u8 cur_stick = stick;

        state->accumPressed |= (u16) (cur_buttons & ~state->prevHostButtons);
        state->accumReleased |= (u16) (state->prevHostButtons & ~cur_buttons);
        state->accumStickPressed |= (u8) (cur_stick & ~state->prevHostStick);
        state->accumStickReleased |= (u8) (state->prevHostStick & ~cur_stick);
        state->prevHostButtons = cur_buttons;
        state->prevHostStick = cur_stick;

        // *current is exposed every host frame, not just on boundaries: the game reads it on its
        // own tick and wants the freshest value there.
        controller->buttonCurrent = cur_buttons;
        controller->stickX = stick_x;
        controller->stickY = stick_y;
        controller->stickCurrent = cur_stick;

        if (gameBoundary) {
            controller->buttonPrev = state->prevGameButtons;
            controller->buttonPressed = state->accumPressed;
            controller->buttonReleased = state->accumReleased;
            controller->stickPrev = state->prevGameStick;
            controller->stickPressed = state->accumStickPressed;
            controller->stickReleased = state->accumStickReleased;

            controller->retriggerCurrentButtonPress = false;
            if (((cur_buttons != 0) || (cur_stick != 0)) &&
                (state->prevGameButtons == cur_buttons) &&
                (state->prevGameStick == cur_stick)) {
                controller->sameInputsHeldCounter++;
                if ((controller->sameInputsHeldCounter >= 15) &&
                    (((controller->sameInputsHeldCounter - 15) % 5) == 0)) {
                    controller->retriggerCurrentButtonPress = true;
                }
            } else {
                controller->sameInputsHeldCounter = 0;
            }

            state->prevGameButtons = cur_buttons;
            state->prevGameStick = cur_stick;
            state->accumPressed = 0;
            state->accumReleased = 0;
            state->accumStickPressed = 0;
            state->accumStickReleased = 0;
        }
        // Non-boundary host frames leave buttonPressed/Released/prev/retrigger untouched: the game
        // only reads them on its tick, which is always a boundary frame.
    }
}

// ── gSharedController: the "any controller" aggregate ────────────────────────────────────────────
// Every menu / title / credits screen drives itself from gSharedController via
// Controller_SetGlobalInputs (common.c:184), NOT from a specific port — on hardware that is what
// lets player 2 press Start on the title screen or back out of a menu, so mirroring port 0 here
// would silently ignore pads 2-4 in the menus.
//
// Reproduces Controller_UpdateInputs' aggregation exactly (controller.c:78-206): button and
// digital-stick fields OR-ed across every CONNECTED port, retrigger OR-ed, analog stick the
// arithmetic mean over connected ports. buttonPrev / stickPrev / sameInputsHeldCounter are never
// written by the hardware aggregate and no decomp code reads them on gSharedController; they are
// mirrored from port 0 purely so a debugger/overlay peeking at the struct sees something coherent.
static void gdx_publish_shared_controller(const u8* connected) {
    s32 connectedCount = 0;
    s32 sumStickX = 0;
    s32 sumStickY = 0;
    int i;

    gSharedController.errno = 0;
    gSharedController.buttonCurrent = 0;
    gSharedController.buttonPressed = 0;
    gSharedController.buttonReleased = 0;
    gSharedController.stickCurrent = 0;
    gSharedController.stickPressed = 0;
    gSharedController.stickReleased = 0;
    gSharedController.retriggerCurrentButtonPress = 0;

    for (i = 0; i < MAXCONTROLLERS; i++) {
        const Controller* controller = &gControllers[i];

        if (connected[i] == 0) {
            continue;
        }
        connectedCount++;
        gSharedController.buttonCurrent |= controller->buttonCurrent;
        gSharedController.buttonPressed |= controller->buttonPressed;
        gSharedController.buttonReleased |= controller->buttonReleased;
        gSharedController.stickCurrent |= controller->stickCurrent;
        gSharedController.stickPressed |= controller->stickPressed;
        gSharedController.stickReleased |= controller->stickReleased;
        gSharedController.retriggerCurrentButtonPress |= controller->retriggerCurrentButtonPress;
        sumStickX += controller->stickX;
        sumStickY += controller->stickY;
    }

    if (connectedCount != 0) {
        gSharedController.stickX = (s8) (sumStickX / connectedCount);
        gSharedController.stickY = (s8) (sumStickY / connectedCount);
    } else {
        gSharedController.stickX = 0;
        gSharedController.stickY = 0;
    }

    gSharedController.buttonPrev = gControllers[0].buttonPrev;
    gSharedController.stickPrev = gControllers[0].stickPrev;
    gSharedController.sameInputsHeldCounter = gControllers[0].sameInputsHeldCounter;
}

// The half of a poll that turns one tick's FINAL pads into decomp controller state: the tick
// counter, presence, the per-port edge accumulation (aligned to the game frame) and the shared
// aggregate. A real poll and a resimulated tick (gdx_controller_poll_resim) both end here, so a
// resim reaches the game through exactly the path the recorded tick took.
static void gdx_controller_apply(const u16* buttons, const s8* stick_x, const s8* stick_y, const u8* connected) {
    // See the scheduler-counter comment below; declared here so the boundary predicate can be
    // evaluated once for every port.
    extern s32 D_800CCFB0, D_800CCFB4, D_800CCFB8;
    s32 divider;
    int gameBoundary;
    int i;

    s_input_tick++; /* exactly once per poll: the poll IS the tick */

    // Publish presence BEFORE the per-port update: a port that just went away has its Controller
    // cleared in here, and clearing it after the update would throw away the frame we just read.
    gdx_sync_controller_ports(connected);

    // The scheduler posts the game's frame tick this VI when (++D_800CCFB0 - D_800CCFB4) >=
    // D_800CCFB8. gdx_controller_poll runs before that increment, so the predicate is
    // (D_800CCFB0 + 1 - D_800CCFB4) >= D_800CCFB8. The first poll is forced to be a boundary so the
    // controllers get a baseline before the game's first tick.
    divider = (D_800CCFB8 > 0) ? D_800CCFB8 : 1;
    gameBoundary = !s_have_baseline || ((D_800CCFB0 + 1 - D_800CCFB4) >= divider);
    s_have_baseline = 1;

    for (i = 0; i < MAXCONTROLLERS; i++) {
        if (connected[i] == 0) {
            continue; // gdx_sync_controller_ports() already neutralised the port on the way out.
        }
        gdx_update_port_inputs(i, buttons[i], stick_x[i], stick_y[i], gameBoundary);
    }

    gdx_publish_shared_controller(connected);
}

void gdx_controller_poll(void) {
    u16 buttons[MAXCONTROLLERS];
    s8 stick_x[MAXCONTROLLERS];
    s8 stick_y[MAXCONTROLLERS];
    u8 connected[MAXCONTROLLERS];
    int i;

    // The button field is already the N64 bitmask (identical layout to the decomp — see the file
    // header) and the stick is already in the -80..80 range, so both copy straight across.
    if (!gdx_lus_read_pads(MAXCONTROLLERS, buttons, stick_x, stick_y, connected)) {
        memset(buttons, 0, sizeof(buttons));
        memset(stick_x, 0, sizeof(stick_x));
        memset(stick_y, 0, sizeof(stick_y));
        memset(connected, 0, sizeof(connected));
    }

    // Port 1 is pinned connected — see the PORT 1 IS PINNED note on gdx_sync_controller_ports().
    // It is also what keeps the "ControlDeck unavailable" path usable: zero input, but still a
    // controller the game will accept.
    connected[0] = 1;

    // ── Unified scripted-input injector (autoinput, GDX_INPUT_SCRIPT, .gdxrec) ────────────────
    // Substituted HERE -- at the rawest point where the pad state lands, before the digital-stick
    // derivation and the edge accumulation in gdx_update_port_inputs() -- so a scripted
    // press/release flows through the EXACT same accumulate-then-finalize path physical input uses.
    // Applied after the real controller read so analog events are not overwritten by it. Every dev
    // harness drives PORT 1 only; giving them a port argument would change their file formats.
    {
        GdxInputPad scriptPad;
        scriptPad.buttons = buttons[0];
        scriptPad.stickX = stick_x[0];
        scriptPad.stickY = stick_y[0];
        gdx_input_inject(s_input_tick, &scriptPad);
        buttons[0] = scriptPad.buttons;
        stick_x[0] = scriptPad.stickX;
        stick_y[0] = scriptPad.stickY;
    }

    // Ports spoofed connected by a backend that has fewer real pads than it reports
    // ([debug] pads on 3DS, port/3ds/os/gdx3ds_os_ctru.c) have no input source of their
    // own. Mirror port 1 into them HERE, downstream of the injector, so a .gdxrec replay
    // reaches them too: VS machine select needs every player to confirm, and idle ports
    // wedge it forever. Weak — the desktop backend has real pads and defines nothing, so
    // the loop never runs there.
    if (&gdx_port_mirror_count != NULL) {
        int mirror = gdx_port_mirror_count();
        for (i = 1; i < mirror && i < MAXCONTROLLERS; i++) {
            buttons[i] = buttons[0];
            stick_x[i] = stick_x[0];
            stick_y[i] = stick_y[0];
        }
    }
    // mp-spike ([net] mp, DEFAULT OFF): every console's ports become the session's exchanged
    // pads for this tick (blocks until they are all in). Downstream of the injector and the
    // mirror, so a replay drives the LOCAL player and every console feeds the game the same pads.
    if (&gdx_mp_exchange != NULL) {
        gdx_mp_exchange(s_input_tick, buttons, stick_x, stick_y, connected, MAXCONTROLLERS);
    }
    // mp-snapshot: the tick's final pads, for a later resim of this tick (one int test when off).
    if (&gdx_snap_note_pads != NULL) {
        gdx_snap_note_pads(s_input_tick, buttons, stick_x, stick_y, connected, MAXCONTROLLERS);
    }

    gdx_controller_apply(buttons, stick_x, stick_y, connected);
}

// mp-snapshot: a RESIMULATED tick's poll. No pad read, no injector (the replay cursor and the
// autoinput SHOTs belong to the real ticks), no mirror, no lockstep exchange: the pads are the
// ones gdx_controller_poll recorded for this tick. Returns 0 (and polls nothing) when the module
// holds no pads for it.
int gdx_controller_poll_resim(void) {
    u16 buttons[MAXCONTROLLERS];
    s8 stick_x[MAXCONTROLLERS];
    s8 stick_y[MAXCONTROLLERS];
    u8 connected[MAXCONTROLLERS];

    if (&gdx_snap_recorded_pads == NULL ||
        !gdx_snap_recorded_pads(s_input_tick, buttons, stick_x, stick_y, connected, MAXCONTROLLERS)) {
        return 0;
    }
    gdx_controller_apply(buttons, stick_x, stick_y, connected);
    return 1;
}
