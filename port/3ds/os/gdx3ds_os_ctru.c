/* port/3ds/os/gdx3ds_os_ctru.c -- real libctru implementation of gdx3ds_os.h (stream B).
 *
 * Compiled ONLY into the 3DS cross-build (see this directory's CMakeLists.txt); the host
 * build keeps gdx3ds_os_stub.c. Fibers live in gdx_fiber_3ds.c, config in gdx3ds_config.c.
 *
 * Window: gfxInitDefault() brings up both LCDs double-buffered; the contract reports the
 * top screen as 400x240 (the PICA framebuffer is physically 240x400 rotated -- that is
 * stream A's problem, not this file's). Swap blocks on gsp vblank and reports APT close
 * requests (HOME -> close, power) as nonzero so the main loop can unwind.
 *
 * HID -> N64 mapping table. F-Zero X in-race semantics, verified against the decomp
 * (decomp/src/game/racer.c): slide/drift-attack LEFT reads N64 Z, slide/drift RIGHT
 * reads N64 R (racer.c:3867-3879; double-tap attacks 3767-3800), BOOST fires on a fresh
 * N64 B press (racer.c:3612), brake is N64 C-DOWN held (racer.c:3572), and N64 L is read
 * by nothing in-race (its only consumer is the port's photo-camera FOV, camera.c:1115).
 *
 *   3DS            N64        Notes
 *   ------------   --------   ----------------------------------------------------------
 *   A              A          accelerate
 *   B              B          boost (fresh press, racer.c:3612); menus: back/cancel
 *   Y              B          duplicate mapping, on by default (config input.y_maps_to_b).
 *                             Decision: on the N64 pad B sits LEFT of A; on the 3DS the
 *                             left face button is Y. Mapping BOTH keeps N64 muscle memory
 *                             (B) and positional memory (Y) working; F-Zero X has no
 *                             other use for Y, so the duplicate costs nothing.
 *   X              Z          slide left / attacks. Deliberate: Old3DS has no ZL/ZR, so Z
 *                             must be reachable from a face button there.
 *   L              Z          slide/drift LEFT. Was 3DS L -> N64 L, which the game never
 *                             reads in-race (hardware feedback: "L didn't seem to work").
 *                             The game's slide pair is Z+R, so the 3DS shoulders now
 *                             mirror it: L = slide left, R = slide right. N64 L is left
 *                             unmapped (only the photo-camera FOV+ loses a binding).
 *   R              R          slide/drift RIGHT
 *   ZL             B          boost without claw-gripping a face button mid-steer
 *   ZR             B          mirrored so either index finger can boost
 *   D-pad          D-pad      menus
 *   START          START      pause (NOTE: unlike the Phase 0 stub, START no longer
 *                             exits -- the game needs it; only APT close ends the loop)
 *   SELECT         (none)     reserved for a future bottom-screen menu toggle
 *   C-stick        C buttons  New3DS C-stick, digital KEY_CSTICK_* -> C-UP/DOWN/LEFT/RIGHT
 *                             (camera; C-DOWN held is also the game's brake). Old3DS
 *                             simply never reports these keys.
 *   Circle pad     stick      scaled to N64 -80..80, see gdx3ds_scale_axis below
 *
 * ZL/ZR/C-stick reach hidKeysHeld() through the IRRST service on New3DS: this libctru's
 * hidInit() (run from the default __appInit) auto-inits irrst on C-stick hardware
 * (hidShouldUseIrrst), so no explicit irrstInit() is needed here. On Old3DS the
 * KEY_ZL/KEY_ZR/KEY_CSTICK_* bits simply never fire, which degrades gracefully: X still
 * covers Z and B/Y still cover boost.
 */
#include "gdx3ds_os.h"
#include "gdx3ds_config.h"
#include "gdx3ds_input_map.h"

#include <stdio.h>
#include <string.h>

#include <3ds.h>

/* Standard N64 OSContPad button bitmask -- identical in the decomp (PR/os_cont.h) and LUS
 * (see the mask table in port/input_bridge.c). Defined locally because the decomp headers
 * are not part of the 3DS stub tree. */
#define GDX_N64_A      0x8000
#define GDX_N64_B      0x4000
#define GDX_N64_Z      0x2000
#define GDX_N64_START  0x1000
#define GDX_N64_DUP    0x0800
#define GDX_N64_DDOWN  0x0400
#define GDX_N64_DLEFT  0x0200
#define GDX_N64_DRIGHT 0x0100
#define GDX_N64_L      0x0020
#define GDX_N64_R      0x0010
#define GDX_N64_CUP    0x0008
#define GDX_N64_CDOWN  0x0004
#define GDX_N64_CLEFT  0x0002
#define GDX_N64_CRIGHT 0x0001

/* Circle pad tuning (config-overridable, [input] section).
 * hidCircleRead() reports roughly -156..156 per axis. Real pads rest a few units off
 * center, hence the deadzone; RANGE is where full N64 deflection (80) is reached --
 * slightly inside the physical max so worn pads can still hit full steer/turn rate. */
#define GDX_CPAD_DEADZONE_DEFAULT 16
#define GDX_CPAD_RANGE_DEFAULT    145

static int sCpadDeadzone = GDX_CPAD_DEADZONE_DEFAULT;
static int sCpadRange = GDX_CPAD_RANGE_DEFAULT;
static int sYMapsToB = 1;

/* INPUT TUNE (menu INP tab; [input] curve / dpad_steer): a response curve on the normalised
 * circle-pad magnitude (0 linear, 1 soft, 2 softer -- a gentler centre for fine steering,
 * full deflection unchanged) and a d-pad-as-stick mode (0 off, 1 full: +-80 immediately,
 * 2 ramp: a tap gives ~30, a hold reaches 80 after GDX_DPAD_RAMP_FRAMES). The game never
 * reads the N64 d-pad bits (menus take direction from the stick via STICK_TO_BUTTON and
 * OR the d-pad bits in), so the bits stay asserted alongside the emulated stick and
 * nothing double-fires. Per axis the larger of pad and d-pad wins. */
#define GDX_CPAD_CURVE_DEFAULT 0
#define GDX_DPAD_STEER_DEFAULT 0
#define GDX_DPAD_RAMP_FRAMES 8
#define GDX_DPAD_RAMP_START 24
static int sCpadCurve = GDX_CPAD_CURVE_DEFAULT;
static int sDpadSteer = GDX_DPAD_STEER_DEFAULT;
static int sDpadHeldX = 0; /* consecutive polls the d-pad x axis has been held (ramp) */
static int sDpadHeldY = 0;
static int sReadRawX = 0, sReadRawY = 0;     /* menu readout: last raw pad + scaled stick */
static int sReadStickX = 0, sReadStickY = 0;

/* MENU rework of the compiled kButtonMap: the same mapping, expressed as game ACTIONS
 * with runtime-mutable 3DS key masks (menu INPUT tab rebinds; [input] bind_<action>
 * hex keys persist). `hid` starts 0 and is filled at window init: compiled default,
 * overridden by a persisted bind. The Y->B duplicate (config input.y_maps_to_b) folds
 * into the boost DEFAULT mask, so the menu shows and rebinding replaces it. */
typedef struct {
    const char* name;   /* config-stable lowercase name */
    uint16_t n64;       /* N64 button(s) this action asserts */
    u32 defaultHid;     /* compiled default (y_maps_to_b folded in at init) */
    u32 hid;            /* live mask */
} Gdx3dsInputAction;

static Gdx3dsInputAction sActions[] = {
    { "accel", GDX_N64_A, KEY_A, 0 },
    { "boost", GDX_N64_B, KEY_B | KEY_ZL | KEY_ZR, 0 }, /* +KEY_Y via y_maps_to_b */
    { "brake", GDX_N64_CDOWN, KEY_CSTICK_DOWN, 0 },
    { "slide_l", GDX_N64_Z, KEY_X | KEY_L, 0 },  /* N64 L is dead in-race, see header */
    { "slide_r", GDX_N64_R, KEY_R, 0 },
    { "camera", GDX_N64_CUP, KEY_CSTICK_UP, 0 },
    { "pause", GDX_N64_START, KEY_START, 0 },
};
#define GDX_INPUT_ACTION_COUNT ((int)(sizeof(sActions) / sizeof(sActions[0])))

/* Non-remappable tail: menu navigation d-pad and the remaining C directions. */
static const struct {
    u32 hid;
    uint16_t n64;
} kFixedMap[] = {
    { KEY_DUP, GDX_N64_DUP },
    { KEY_DDOWN, GDX_N64_DDOWN },
    { KEY_DLEFT, GDX_N64_DLEFT },
    { KEY_DRIGHT, GDX_N64_DRIGHT },
    { KEY_CSTICK_LEFT, GDX_N64_CLEFT },
    { KEY_CSTICK_RIGHT, GDX_N64_CRIGHT },
};

#define GDX_INPUT_BINDABLE                                                                  \
    (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_ZL | KEY_ZR | KEY_START |          \
     KEY_SELECT | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_CSTICK_UP |            \
     KEY_CSTICK_DOWN | KEY_CSTICK_LEFT | KEY_CSTICK_RIGHT)

static void gdx3ds_input_load_binds(void) {
    int i;
    for (i = 0; i < GDX_INPUT_ACTION_COUNT; i++) {
        char key[48];
        int fromCfg;
        if (strcmp(sActions[i].name, "boost") == 0 && sYMapsToB) {
            sActions[i].defaultHid = KEY_B | KEY_ZL | KEY_ZR | KEY_Y;
        }
        snprintf(key, sizeof(key), "bind_%s", sActions[i].name);
        fromCfg = gdx3ds_config_get_int("input", key, (int)sActions[i].defaultHid);
        sActions[i].hid = ((u32)fromCfg & GDX_INPUT_BINDABLE) != 0
                              ? ((u32)fromCfg & GDX_INPUT_BINDABLE)
                              : sActions[i].defaultHid;
    }
}

int gdx3ds_input_action_count(void) {
    return GDX_INPUT_ACTION_COUNT;
}

const char* gdx3ds_input_action_name(int index) {
    if (index < 0 || index >= GDX_INPUT_ACTION_COUNT) {
        return "?";
    }
    return sActions[index].name;
}

unsigned gdx3ds_input_action_mask(int index) {
    if (index < 0 || index >= GDX_INPUT_ACTION_COUNT) {
        return 0;
    }
    return sActions[index].hid;
}

unsigned gdx3ds_input_action_default(int index) {
    if (index < 0 || index >= GDX_INPUT_ACTION_COUNT) {
        return 0;
    }
    return sActions[index].defaultHid;
}

void gdx3ds_input_action_set_mask(int index, unsigned hidMask) {
    if (index < 0 || index >= GDX_INPUT_ACTION_COUNT) {
        return;
    }
    hidMask &= GDX_INPUT_BINDABLE;
    if (hidMask != 0) {
        sActions[index].hid = hidMask;
    }
}

void gdx3ds_input_reset_defaults(void) {
    int i;
    for (i = 0; i < GDX_INPUT_ACTION_COUNT; i++) {
        sActions[i].hid = sActions[i].defaultHid;
    }
}

unsigned gdx3ds_input_bindable_mask(void) {
    return GDX_INPUT_BINDABLE;
}

const char* gdx3ds_input_key_label(unsigned hidMask, char* buf, int cap) {
    static const struct {
        u32 bit;
        const char* name;
    } kKeyNames[] = {
        { KEY_A, "A" },       { KEY_B, "B" },       { KEY_X, "X" },       { KEY_Y, "Y" },
        { KEY_L, "L" },       { KEY_R, "R" },       { KEY_ZL, "ZL" },     { KEY_ZR, "ZR" },
        { KEY_START, "ST" },  { KEY_SELECT, "SEL" },
        { KEY_DUP, "DU" },    { KEY_DDOWN, "DD" },  { KEY_DLEFT, "DL" }, { KEY_DRIGHT, "DR" },
        { KEY_CSTICK_UP, "CU" }, { KEY_CSTICK_DOWN, "CD" },
        { KEY_CSTICK_LEFT, "CL" }, { KEY_CSTICK_RIGHT, "CR" },
    };
    int n = 0;
    int i;
    if (buf == NULL || cap < 1) {
        return "";
    }
    buf[0] = '\0';
    for (i = 0; i < (int)(sizeof(kKeyNames) / sizeof(kKeyNames[0])); i++) {
        if (hidMask & kKeyNames[i].bit) {
            n += snprintf(buf + n, (size_t)(cap - n), "%s%s", n > 0 ? "+" : "",
                          kKeyNames[i].name);
            if (n >= cap - 1) {
                break;
            }
        }
    }
    if (buf[0] == '\0') {
        snprintf(buf, (size_t)cap, "-");
    }
    return buf;
}

/* Deadzone-then-rescale: dead zone is cut out and the REMAINING travel is stretched over
 * the full 0..80 output, so there is no dead "step" -- output ramps smoothly from 0 at the
 * deadzone edge to 80 at sCpadRange, clamped beyond. */
static int8_t gdx3ds_scale_axis(s16 raw) {
    int mag = (raw < 0) ? -raw : raw;
    int span = sCpadRange - sCpadDeadzone;
    int scaled;
    if (mag <= sCpadDeadzone) {
        return 0;
    }
    if (span < 1) {
        span = 1;
    }
    scaled = (mag - sCpadDeadzone) * 80 / span;
    if (scaled > 80) {
        scaled = 80;
    }
    if (sCpadCurve > 0) {
        /* t in [0,1]; soft = t*(0.5+0.5t) (~t^1.5), softer = t^2. Endpoints unchanged. */
        float t = (float) scaled / 80.0f;
        t = (sCpadCurve == 1) ? t * (0.5f + 0.5f * t) : t * t;
        scaled = (int) (t * 80.0f + 0.5f);
    }
    return (int8_t) ((raw < 0) ? -scaled : scaled);
}

/* d-pad axis -> stick value; `analog` is the circle-pad value for the same axis. */
static int8_t gdx3ds_dpad_axis(int dir, int heldPolls, int8_t analog) {
    int mag;
    int amag = analog < 0 ? -analog : analog;
    if (dir == 0) {
        return analog;
    }
    if (sDpadSteer == 2) {
        mag = GDX_DPAD_RAMP_START + heldPolls * ((80 - GDX_DPAD_RAMP_START) / GDX_DPAD_RAMP_FRAMES);
        if (mag > 80) {
            mag = 80;
        }
    } else {
        mag = 80;
    }
    if (amag > mag) {
        return analog;
    }
    return (int8_t) (dir < 0 ? -mag : mag);
}

/* INPUT TUNE accessors (menu INP tab). Setters clamp; values are live on the next poll. */
int gdx3ds_input_get_deadzone(void) { return sCpadDeadzone; }
void gdx3ds_input_set_deadzone(int v) { sCpadDeadzone = v < 0 ? 0 : (v > 80 ? 80 : v); }
int gdx3ds_input_get_range(void) { return sCpadRange; }
void gdx3ds_input_set_range(int v) { sCpadRange = v < 60 ? 60 : (v > 156 ? 156 : v); }
int gdx3ds_input_get_curve(void) { return sCpadCurve; }
void gdx3ds_input_set_curve(int v) { sCpadCurve = v < 0 ? 0 : (v > 2 ? 2 : v); }
int gdx3ds_input_get_dpad_steer(void) { return sDpadSteer; }
void gdx3ds_input_set_dpad_steer(int v) { sDpadSteer = v < 0 ? 0 : (v > 2 ? 2 : v); }
void gdx3ds_input_stick_readout(int* rawX, int* rawY, int* stickX, int* stickY) {
    if (rawX) { *rawX = sReadRawX; }
    if (rawY) { *rawY = sReadRawY; }
    if (stickX) { *stickX = sReadStickX; }
    if (stickY) { *stickY = sReadStickY; }
}

int gdx3ds_os_window_init(int* outWidth, int* outHeight) {
    /* Config first: input tuning below and other streams' keys must be readable before
     * any subsystem init. Missing file is fine -- defaults hold. (main_3ds.cpp calling
     * gdx3ds_config_load() explicitly is a filed request; loading here keeps the backend
     * self-sufficient meanwhile. gdx3ds_config_load is idempotent-safe to call twice.) */
    gdx3ds_config_load(GDX3DS_CONFIG_DEFAULT_PATH);
    sCpadDeadzone = gdx3ds_config_get_int("input", "deadzone", GDX_CPAD_DEADZONE_DEFAULT);
    sCpadRange = gdx3ds_config_get_int("input", "range", GDX_CPAD_RANGE_DEFAULT);
    gdx3ds_input_set_deadzone(sCpadDeadzone);
    gdx3ds_input_set_range(sCpadRange);
    gdx3ds_input_set_curve(gdx3ds_config_get_int("input", "curve", GDX_CPAD_CURVE_DEFAULT));
    gdx3ds_input_set_dpad_steer(gdx3ds_config_get_int("input", "dpad_steer", GDX_DPAD_STEER_DEFAULT));
    sYMapsToB = gdx3ds_config_get_bool("input", "y_maps_to_b", 1);
    gdx3ds_input_load_binds(); /* MENU: compiled defaults + persisted bind_* overrides */

    gfxInitDefault();
    /* Top screen: double-buffered so the game never draws into the scanout buffer.
     * Format stays the gfxInitDefault() default (BGR8); stream A owns the real surface
     * format decision via gdx3ds_gfx. */
    gfxSetDoubleBuffering(GFX_TOP, true);

    if (gdx3ds_config_get_bool("debug", "console", 0)) {
        /* Dev aid only: text console on the bottom screen (which the contract otherwise
         * reserves untouched). Off by default. */
        consoleInit(GFX_BOTTOM, NULL);
    }

    if (outWidth != NULL) {
        *outWidth = 400;
    }
    if (outHeight != NULL) {
        *outHeight = 240;
    }
    return 0;
}

void gdx3ds_os_window_shutdown(void) {
    gfxExit();
}

int gdx3ds_os_window_swap(void) {
    /* NO gfxSwapBuffers() here — the citro3d backend owns top-screen presentation.
     * C3D_FrameEnd queues a display transfer of its render target into the top
     * screen's BACK buffer and citro3d's render queue calls gfxScreenSwapBuffers
     * itself when that transfer completes. A second CPU-side gfxSwapBuffers here
     * double-swapped every frame: the screen stably showed the buffer citro3d
     * NEVER transferred into — a permanently black top screen while every
     * upstream stage (interpreter, backend, RDRAM fb) was healthy. The proven
     * harness loop (port/3ds/harness/dl_tests_main.cpp) never swaps manually;
     * this now matches it. gfxFlushBuffers stays: it only flushes the data cache
     * (bottom-screen console writes need it), it does not swap.
     *
     * NO gspWaitForVBlank() either: C3D_FrameBegin(C3D_FRAME_SYNCDRAW) in the
     * backend's StartFrame already paces the loop against vblank. A second wait
     * here after every C3D_FrameEnd quantized each frame down an extra vblank
     * (measured ~18fps ceiling on 1-draw frames); the harness omits it too. */
    gfxFlushBuffers();
    /* APT: deliberately NO aptMainLoop() here any more (the hardware power-off crash
     * fix). This function runs inside Interpreter::EndFrame — including the bridge's
     * mid-dispatch DrawAndRunGraphicsCommands path — and aptMainLoop() performs the
     * ENTIRE HOME/power suspend transition inline at its call site (APTHOOK_ONSUSPEND
     * hooks, VRAM sys-area save, screen-capture transfer, DSP sleep, GSPGPU_ReleaseRight,
     * then blocks until wake). Executing that deep inside the display-list walk, right
     * after an ASYNC C3D_FrameEnd has just queued GPU work, is the state the transition
     * cannot tolerate: pressing POWER produced a Luma crash screen instead of a clean
     * exit, with the filelog ending mid-run (no [fatal], no atexit tracer). The frame
     * loop (main_3ds.cpp) now pumps aptMainLoop() once per iteration at the loop TOP —
     * the canonical libctru position, no C3D frame open, no bridge walk on the stack.
     * Here only a pure flag read remains, so the window backend's IsRunning close-latch
     * still trips as a backstop on the same frame the close order lands. */
    if (aptShouldClose()) {
        return 1;
    }
    return 0;
}

void gdx3ds_os_poll_input(Gdx3dsPadState* outPads, int maxPads) {
    u32 held;
    uint16_t buttons = 0;
    circlePosition cpad;
    int i;

    if (outPads == NULL || maxPads < 1) {
        return;
    }

    hidScanInput();
    held = hidKeysHeld();

    for (i = 0; i < GDX_INPUT_ACTION_COUNT; i++) {
        if (held & sActions[i].hid) {
            buttons |= sActions[i].n64;
        }
    }
    for (i = 0; i < (int) (sizeof(kFixedMap) / sizeof(kFixedMap[0])); i++) {
        if (held & kFixedMap[i].hid) {
            buttons |= kFixedMap[i].n64;
        }
    }

    hidCircleRead(&cpad);

    outPads[0].buttons = buttons;
    {
        int8_t sx = gdx3ds_scale_axis(cpad.dx);
        int8_t sy = gdx3ds_scale_axis(cpad.dy);
        if (sDpadSteer != 0) {
            int dx = ((held & KEY_DRIGHT) ? 1 : 0) - ((held & KEY_DLEFT) ? 1 : 0);
            int dy = ((held & KEY_DUP) ? 1 : 0) - ((held & KEY_DDOWN) ? 1 : 0);
            sDpadHeldX = dx != 0 ? sDpadHeldX + 1 : 0;
            sDpadHeldY = dy != 0 ? sDpadHeldY + 1 : 0;
            sx = gdx3ds_dpad_axis(dx, sDpadHeldX, sx);
            sy = gdx3ds_dpad_axis(dy, sDpadHeldY, sy);
        } else {
            sDpadHeldX = sDpadHeldY = 0;
        }
        outPads[0].stickX = sx;
        outPads[0].stickY = sy;
        sReadRawX = cpad.dx;
        sReadRawY = cpad.dy;
        sReadStickX = sx;
        sReadStickY = sy;
    }
    outPads[0].connected = 1;

    /* Only pad 0 exists on 3DS; report the rest explicitly disconnected so the
     * multi-pad consumer never reads stale data. */
    for (i = 1; i < maxPads; i++) {
        outPads[i].buttons = 0;
        outPads[i].stickX = 0;
        outPads[i].stickY = 0;
        outPads[i].connected = 0;
    }
}

uint64_t gdx3ds_os_time_ns(void) {
    /* svcGetSystemTick counts at SYSCLOCK_ARM11 (268 MHz). Naive tick*1e9 overflows u64
     * after ~68 seconds of uptime, so split into whole seconds + remainder: the remainder
     * term peaks below SYSCLOCK_ARM11 * 1e9 ~= 2.7e17, well inside u64. */
    uint64_t ticks = svcGetSystemTick();
    uint64_t sec = ticks / SYSCLOCK_ARM11;
    uint64_t rem = ticks % SYSCLOCK_ARM11;
    return sec * 1000000000ull + rem * 1000000000ull / SYSCLOCK_ARM11;
}
