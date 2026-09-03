#pragma once

// =================================================================================================
// G-Diffuser developer gates — the single accessor layer behind every GDX_* diagnostic /
// behavior / logging switch that the Dev Tools menu surfaces.
//
// Replaces the ~60 ad-hoc `static const bool sX = getenv("GDX_...") != nullptr;` gates the port
// grew across the bridge, the scheduler, the audio HLE and the decomp. Those were invisible in
// game, several of them CHANGE RENDERING rather than merely logging, and a shipped build still
// paid a getenv for every one.
//
// THE FOUR BUCKETS
//   A  Diagnostic logging       -> in this table, CVar-backed, always compiled.
//   B  Behavior-altering        -> in this table, CVar-backed, compiled out unless GDX_DEV_TOOLS.
//   C  Boot / tooling / not-ours-> NOT in this table. Plain documented env var, because it is
//                                  consumed before ANY of this exists (GDX_INPUT_SCRIPT,
//                                  FZEROX_ROM) or is a non-boolean value ("start:count").
//   D  Boot-seeded CVar         -> in this table. The logging gates. See below.
//
// BUCKET D — three sources, fixed precedence
// ------------------------------------------
//   1. The CVar is the PERSISTED PREFERENCE and the authority. gdx_dev_gates_boot_seed() reads it
//      immediately after Ship::Context::InitConsoleVariables(), the first moment the config exists
//      and still ahead of essentially all boot logging (ROM load, scheduler init, asset load,
//      bootproc), so ticking the box this run captures boot output on the NEXT launch.
//   2. The ENVIRONMENT VARIABLE, when set, OVERRIDES the CVar FOR THAT RUN ONLY and is never
//      written back, so scripted / CI / headless runs cannot silently rewrite the user's persisted
//      preference. Such a gate is reported as "env-pinned" and its checkbox is disabled.
//   3. Toggling the CVar at runtime changes emission FROM THAT POINT FORWARD; the file sink opens
//      on the next log call and cannot retroactively produce earlier lines. The UI says so.
//
// Buckets A and B use the simpler rule: an env var present at launch SEEDS the CVar once at
// startup, and the CVar is the live source of truth from then on.
//
// THE LOAD-BEARING INVARIANT: **0 ALWAYS MEANS STOCK BEHAVIOR.**
// Some original env vars are opt-in switches (1 turns something ON) and some are default-ON kill
// switches (GDX_HLE_FILTER=0 turns something OFF). Every gate here is normalized so 0 reproduces
// shipping behavior and 1 is the deviation — see GdxGateEnvKind. That is what makes the Release
// compile-out provably safe: with GDX_DEV_TOOLS undefined every Bucket B gate is hard-wired to 0
// with no getenv and no CVar read. (The one exception is GDX_TRACE, whose stock value is 1 in a
// Debug build; it is Bucket D and is never compiled out, so the invariant holds where it has to.)
//
// HOT-PATH CONTRACT
// -----------------
// Several gates are queried per draw call, per display-list command, or per audio frame, so the
// read is deliberately NOT a CVar lookup: gdx_dev_gate() is a static inline load from a plain int
// array. The array is refreshed from the CVars once per frame at the top of the host loop
// (gdx_dev_gates_refresh() in main.cpp, before PerfFrameBegin) and again immediately after a menu
// toggle so a click applies on the same frame. That refresh is deliberately NOT throttled: it is
// what makes a change from the LUS console (`set gDevTools.Diag.SetupDl 1`) take effect, and a
// divisor would trade that immediacy for savings too small to measure.
//
// The cache is written on the main thread and read from the game fiber, the audio thread and the
// render path without a lock. That is a deliberate benign int race: each slot is a naturally
// aligned int holding a self-consistent value, so a reader sees either the old or the new setting
// for at most one frame. Same pattern as the port's live-CVar audio toggles (n64_audio_hle.c,
// gdx_audio_lle.c, libultra/os.cpp).
//
// The cache is statically zero-initialized and 0 is stock for every gate, so a read before
// gdx_dev_gates_init_env() is safe. init_env() is the first statement in main(), ahead of every
// log call the port makes.
//
// ADDING A GATE: add a row to kGates in gdx_dev_gates.c and an id to GdxGateId below. The Dev
// Tools page is table-driven and needs no edit.
// =================================================================================================

#ifdef __cplusplus
extern "C" {
#endif

// UI grouping. Order here is the order the Dev Tools page draws the groups in.
enum GdxGateGroup {
    GDX_GATE_GROUP_LOGGING = 0,
    GDX_GATE_GROUP_GRAPHICS,
    GDX_GATE_GROUP_AUDIO,
    GDX_GATE_GROUP_ASSETS,
    GDX_GATE_GROUP_SCHEDULING,
    GDX_GATE_GROUP_COUNT
};

// Which of the three managed buckets a gate belongs to (C is by definition not in this table).
enum GdxGateBucket {
    GDX_GATE_BUCKET_DIAG = 0, // A — logging only, always compiled
    GDX_GATE_BUCKET_BEHAVIOR, // B — changes rendering/behavior, compiled out without GDX_DEV_TOOLS
    GDX_GATE_BUCKET_BOOT      // D — boot-seeded logging gate, env-pinned when exported
};

// How the legacy env var maps onto the normalized "1 = deviate from stock" gate value. Each entry
// reproduces EXACTLY what the original call site tested, so a script that exports one of these
// keeps behaving identically.
enum GdxGateEnvKind {
    GDX_GATE_ENV_PRESENCE = 0, // set at all (even to "0") -> 1.   Original: `getenv(x) != NULL`
    GDX_GATE_ENV_OPT_IN,       // set to a non-empty, non-"0" value -> 1
    GDX_GATE_ENV_ONE_ONLY,     // first character is '1' -> 1.     Original: `e[0] == '1'`
    GDX_GATE_ENV_KILL_SWITCH,  // set with first character '0' -> 1 (the feature is default ON and
                               // the var disables it, so "disabled" is the deviation)
    GDX_GATE_ENV_TRISTATE      // non-"0" -> 1, "0" -> 0, unset -> the gate's compiled default
};

// Gate ids. The numeric values are an implementation detail — always use the names.
enum GdxGateId {
    // ── Bucket D — logging, boot-seeded ──────────────────────────────────────────────────────
    GDX_GATE_LOG_FILE = 0,       // GDX_LOG
    GDX_GATE_TRACE,              // GDX_TRACE
    GDX_GATE_DIAG_VERBOSE,       // GDX_DIAG_VERBOSE
    GDX_GATE_DIAG_UNLOCK,        // GDX_DIAG_UNLOCK

    // ── Bucket A — diagnostic logging only ───────────────────────────────────────────────────
    GDX_GATE_PERF,               // GDX_PERF
    GDX_GATE_DIAG_RIVAL,         // GDX_DIAG_RIVAL
    GDX_GATE_DIAG_CUSTOMMACHINE, // GDX_DIAG_CUSTOMMACHINE
    GDX_GATE_DIAG_SETUPDL,       // GDX_DIAG_SETUPDL
    GDX_GATE_DIAG_TRECT,         // GDX_DIAG_TRECT
    GDX_GATE_DIAG_COUNTDOWN,     // GDX_DIAG_COUNTDOWN
    GDX_GATE_PRESENT_PATH_TRACE, // GDX_PRESENT_PATH_TRACE
    GDX_GATE_DIAG_HOLD,          // GDX_DIAG_HOLD
    GDX_GATE_TRANSITION_DUMP,    // GDX_DIAG_TRANSITION_DUMP
    GDX_GATE_CAPTURE_PROBE,      // GDX_DIAG_CAPTURE_PROBE
    GDX_GATE_INTERP_DETERMINISM, // GDX_INTERP_DETERMINISM
    GDX_GATE_DIAG_NODEINFO,      // GDX_DIAG_NODEINFO
    GDX_GATE_DIAG_SETTIMG,       // GDX_DIAG_SETTIMG
    GDX_GATE_DIAG_LOOKAT,        // GDX_DIAG_LOOKAT
    GDX_GATE_DIAG_TEXREG,        // GDX_DIAG_TEXREG
    GDX_GATE_DIAG_CULL,          // GDX_DIAG_CULL

    // ── Bucket B — CHANGES RENDERING OR GAME BEHAVIOR (compiled out unless GDX_DEV_TOOLS) ─────
    GDX_GATE_RAIL_COLOR_TEST,    // GDX_RAIL_COLOR_TEST
    GDX_GATE_NO_SRCWIN,          // GDX_DIAG_NO_SRCWIN
    GDX_GATE_LEGACY_RESOLVE,     // GDX_LEGACY_RESOLVE (default OFF; setting it re-enables guessing)
    GDX_GATE_NO_G2_CONVERT,      // GDX_G2_CONVERT=0
    GDX_GATE_INTERP_P0,          // GDX_INTERP_P0
    GDX_GATE_INTERP_CAMERA,      // GDX_INTERP_CAMERA
    GDX_GATE_NO_REVERB,          // GDX_NO_REVERB=1
    GDX_GATE_NO_HLE_FILTER,      // GDX_HLE_FILTER=0
    GDX_GATE_SEQ_ADPCM,          // GDX_SEQ_ADPCM=1

    GDX_GATE_COUNT
};

// The live cache. Do not touch it directly — read through gdx_dev_gate(), write through
// gdx_dev_gate_force(). Exposed only so the accessor below can stay a single inline load.
extern int gGdxDevGateCache[GDX_GATE_COUNT];

// THE hot-path read. One aligned int load; safe before init (returns 0 = stock).
static inline int gdx_dev_gate(int id) {
    return gGdxDevGateCache[id];
}

// Call once, as the first statement of main() — before the window, the Gui, the config or any log
// call — so the legacy env vars keep working for everything that runs during boot. Idempotent.
// Bucket B gates are skipped entirely (no getenv at all) when GDX_DEV_TOOLS is not defined.
void gdx_dev_gates_init_env(void);

// Adopts the PERSISTED CVar value for every boot-seeded gate whose env var was not exported. Call
// immediately after Ship::Context::InitConsoleVariables(), the earliest moment the config is
// readable — that timing is what lets "enable logging in the menu" survive a restart and capture
// the next boot. Env-pinned gates are left alone.
void gdx_dev_gates_boot_seed(int (*cvarGetInteger)(const char* name, int defaultValue));

// Hands the gate layer the console-variable entry points and takes over from the environment.
// Called once from main.cpp after the Gui exists. Passing the functions as pointers keeps this
// translation unit free of any libultraship link dependency, so the standalone unit-test
// executables compile it unchanged.
//
//   Buckets A/B: an env var present at launch SEEDS the CVar (written back once); the CVar is the
//                live source of truth afterwards.
//   Bucket  D  : the env var is NEVER written back. It pins the gate for this run and the CVar
//                keeps whatever the user persisted.
void gdx_dev_gates_bind_cvars(int (*cvarGetInteger)(const char* name, int defaultValue),
                              void (*cvarSetInteger)(const char* name, int value));

// Call once per frame at a single well-defined point (top of the host loop) and immediately after
// a menu toggle. No-op until bind_cvars has run.
void gdx_dev_gates_refresh(void);

// Overrides one gate for the rest of the process, bypassing env and CVar. Exists for the
// command-line forwarding in n64_gfx_bridge.cpp (`--diag-settimg`), which arms a probe after the
// env has already been sampled. Ignored for Bucket B gates when GDX_DEV_TOOLS is not defined.
void gdx_dev_gate_force(int id, int value);

// ── UI/description metadata (read by port/gdx_menu.cpp; no other consumer) ────────────────────
int gdx_dev_gate_count(void);
const char* gdx_dev_gate_env_name(int id);  // e.g. "GDX_DIAG_SETUPDL"
const char* gdx_dev_gate_cvar_name(int id); // e.g. "gDevTools.Diag.SetupDl"
const char* gdx_dev_gate_label(int id);     // short checkbox label
const char* gdx_dev_gate_help(int id);      // one-line tooltip: what turning it on does
int gdx_dev_gate_group(int id);             // GdxGateGroup
int gdx_dev_gate_bucket(int id);            // GdxGateBucket
int gdx_dev_gate_is_behavior(int id);       // 1 = Bucket B
int gdx_dev_gate_default(int id);           // value with neither env nor CVar set
const char* gdx_dev_gate_group_name(int group);

// 1 when the env var was exported at launch. Bucket D gates are then PINNED for the session (the
// CVar cannot move them and the UI disables the checkbox); Bucket A/B gates merely started from it.
int gdx_dev_gate_from_env(int id);
int gdx_dev_gate_is_env_pinned(int id);

// ── Named accessors for translation units that cannot include this header ─────────────────────
// The decomp game sources compile with the decomp include roots only (port/ is deliberately not on
// their include path), so they declare the one accessor they need locally. Keep this list tiny.
int gdx_dev_gate_no_reverb(void); // GDX_NO_REVERB — decomp/src/audio/disk/lib/synthesis.c
int gdx_dev_gate_log_file(void);  // GDX_LOG      — port/decomp_port.c (avoids <stdio.h>/<stdlib.h>)
int gdx_dev_gate_diag_texreg(void); // GDX_DIAG_TEXREG — decomp/src/game/object.c texture registry

// Optional tap on every gdx_port_write_log line (port_log.h), installed by GdxConsoleLogInstall()
// so the in-game console can show the port log. NULL until then. It lives with the gates because
// gdx_dev_gates.c is the one dependency-free unit every port_log.h consumer already links —
// including the standalone test executables.
extern void (*gdx_port_log_tap)(const char* message);

#ifdef __cplusplus
} // extern "C"
#endif
