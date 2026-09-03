// G-Diffuser developer gates — table + cache. See gdx_dev_gates.h for the four-bucket policy, the
// Bucket D three-source precedence and the "0 always means stock" invariant this file preserves.
//
// Deliberately dependency-free: standard C only, no libultraship, no decomp headers, no logging.
// The CVar entry points arrive as function pointers via gdx_dev_gates_boot_seed() /
// gdx_dev_gates_bind_cvars(), so the standalone unit-test executables (gdx_dsp_tests,
// gdx_pcm_capture_tests) link it without pulling in the console-variable bridge.

#include "gdx_dev_gates.h"

#include <stdlib.h>
#include <string.h>

int gGdxDevGateCache[GDX_GATE_COUNT];

void (*gdx_port_log_tap)(const char* message) = NULL;

// GDX_TRACE is the one gate whose stock value is not 0: Debug builds trace ON (developer workflow),
// Release builds trace OFF (end-user performance), and the var overrides in BOTH directions. That
// tri-state is why it carries a compiled default instead of the usual implicit 0.
#ifdef NDEBUG
#define GDX_GATE_TRACE_DEFAULT 0
#else
#define GDX_GATE_TRACE_DEFAULT 1
#endif

// One positional table — no designated initializers, because the port's C sources are built by
// MSVC in its default C mode as well as by GCC/Clang.
typedef struct GdxGateDesc {
    const char* env;   // legacy environment variable
    const char* cvar;  // console variable that is now the persisted source of truth
    const char* label; // checkbox label in the Dev Tools page
    const char* help;  // tooltip: what turning this ON does
    unsigned char envKind;      // GdxGateEnvKind
    unsigned char group;        // GdxGateGroup
    unsigned char bucket;       // GdxGateBucket
    unsigned char defaultValue; // value when neither env nor CVar says otherwise
} GdxGateDesc;

// clang-format off
static const GdxGateDesc kGates[GDX_GATE_COUNT] = {
    /* ── Bucket D — logging, boot-seeded (env pins for the run, CVar persists) ───────────────── */
    /* LOG_FILE          */ { "GDX_LOG",                  "gDevTools.Log.FileSink",           "Write gdiffuser-run.log",       "Opens the persistent file log next to the executable. Also implied by any gate below.",                 GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_LOGGING,    GDX_GATE_BUCKET_BOOT,     0 },
    /* TRACE             */ { "GDX_TRACE",                "gDevTools.Log.Trace",              "High-frequency trace",          "Unsilences the per-frame gdx_ck/gdx_cki breadcrumbs sprinkled through the decomp. Debug default: on.",  GDX_GATE_ENV_TRISTATE,    GDX_GATE_GROUP_LOGGING,    GDX_GATE_BUCKET_BOOT,     GDX_GATE_TRACE_DEFAULT },
    /* DIAG_VERBOSE      */ { "GDX_DIAG_VERBOSE",         "gDevTools.Log.Verbose",            "Verbose per-frame families",    "Unsilences [gfxdiag] [game] [seg] [sched] [phasegeom] [bigtri] and the bridge's per-frame aggregates.", GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_LOGGING,    GDX_GATE_BUCKET_BOOT,     0 },
    /* DIAG_UNLOCK       */ { "GDX_DIAG_UNLOCK",          "gDevTools.Log.Unlock",             "Unlock-code path",              "Logs the unlock-code / audio-unlock decision path.",                                                   GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_LOGGING,    GDX_GATE_BUCKET_BOOT,     0 },

    /* ── Bucket A — diagnostics (env seeds the CVar once, CVar is live truth) ────────────────── */
    /* PERF              */ { "GDX_PERF",                 "gDevTools.Diag.PerfTelemetry",     "Frame-time telemetry",          "Per-phase spike attribution plus periodic p50/p95/p99 summaries through the port log.",                 GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_SCHEDULING, GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_RIVAL        */ { "GDX_DIAG_RIVAL",           "gDevTools.Diag.RivalIcon",         "In-world marker draw gates",    "Dumps the rival-icon and 1ST/2ND/3RD position-marker draw conditions once per second, plus emitted texrects.", GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_CUSTOMMACHINE*/ { "GDX_DIAG_CUSTOMMACHINE",   "gDevTools.Diag.CustomMachine",     "Create Machine record",         "Dumps the gCustomMachine record the Create Machine draw path actually reads.",                          GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_SETUPDL      */ { "GDX_DIAG_SETUPDL",         "gDevTools.Diag.SetupDl",           "Course setup display lists",    "Dumps how the segment-8 course material setup DLs are classified and converted.",                       GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_TRECT        */ { "GDX_DIAG_TRECT",           "gDevTools.Diag.TexRect",           "Transition TEXRECTs",           "Counts the TEXRECTs each screen-transition instance emits and reports the per-instance total.",         GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_COUNTDOWN    */ { "GDX_DIAG_COUNTDOWN",       "gDevTools.Diag.Countdown",         "Countdown billboard probe",     "Traces the race countdown quad: raw command words, object-space rect and modelview matrix.",            GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* PRESENT_PATH_TRACE*/ { "GDX_PRESENT_PATH_TRACE",   "gDevTools.Diag.PresentPath",       "Present path",                  "Logs which present path each frame took (GPU hold, readback, VI fallback).",                            GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_HOLD         */ { "GDX_DIAG_HOLD",            "gDevTools.Diag.HoldTick",          "Hold-tick readback",            "Logs every hold tick and whether content changed, to prove the readback is gone.",                       GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* TRANSITION_DUMP   */ { "GDX_DIAG_TRANSITION_DUMP", "gDevTools.Diag.TransitionDump",    "Transition frame dump",         "Writes transition-capture.bmp next to the executable (slow: per-pixel conversion plus a file write).",  GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* CAPTURE_PROBE     */ { "GDX_DIAG_CAPTURE_PROBE",   "gDevTools.Diag.CaptureProbe",      "Capture content probe",         "Fingerprints captured framebuffer content to separate a stale capture from a mislaid one.",             GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* INTERP_DETERMINISM*/ { "GDX_INTERP_DETERMINISM",   "gDevTools.Diag.InterpDeterminism", "Interpolation determinism",     "Logs a per-tick RNG fingerprint so an interpolation-induced sim divergence is localized quickly.",       GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_NODEINFO     */ { "GDX_DIAG_NODEINFO",        "gDevTools.Diag.NodeInfo",          "Segment-9 node info",           "Traces segment-9 resolution for the Course Edit node-info overlay.",                                    GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_ASSETS,     GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_SETTIMG      */ { "GDX_DIAG_SETTIMG",         "gDevTools.Diag.SetTimg",           "SETTIMG texture sources",       "Fingerprints the bytes behind every resolved texture source during a race and flags MIO0 streams.",     GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_ASSETS,     GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_LOOKAT       */ { "GDX_DIAG_LOOKAT",          "gDevTools.Diag.LookAt",            "Env-map LookAt basis",          "Logs the LookAt source matrix and resulting texgen basis at each machine reflection-pass setup.",       GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_TEXREG       */ { "GDX_DIAG_TEXREG",          "gDevTools.Diag.TexRegistry",       "Texture registry occupancy",    "Reports the D_800E33E0 texture registry high-water mark as it grows, so a mode that overruns its 200 slots is visible before the overflow guard has to refuse an entry.", GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_ASSETS,     GDX_GATE_BUCKET_DIAG,     0 },
    /* DIAG_CULL         */ { "GDX_DIAG_CULL",            "gDevTools.Diag.CourseCull",        "Course/racer cull census",      "Bounded bursts of the course chunk cull, tessellator walk and racer visibility censuses ([cull]/[cull2]/[grp]/[rvcull]). ~75 log lines every 64th frame while armed — costs fps.", GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_DIAG,     0 },

    /* ── Bucket B — behaviour overrides (compiled out without GDX_DEV_TOOLS) ─────────────────── */
    /* RAIL_COLOR_TEST   */ { "GDX_RAIL_COLOR_TEST",      "gDevTools.Behavior.RailColorTest", "Freeze rail chevron colour",    "Freezes the rail chevron colour sawtooth to a constant to isolate the interpolation strobe.",           GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_BEHAVIOR, 0 },
    /* NO_SRCWIN         */ { "GDX_DIAG_NO_SRCWIN",       "gDevTools.Behavior.NoSourceWindow","Disable source-window matrices","Disables the source-window matrix reconstruction so a regression can be bisected without a rebuild.",   GDX_GATE_ENV_PRESENCE,    GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_BEHAVIOR, 0 },
    /* LEGACY_RESOLVE    */ { "GDX_LEGACY_RESOLVE",       "gDevTools.Behavior.LegacyResolve", "Legacy address guessing",       "Re-enables the bridge resolver's legacy address-guessing branches, retired after a full-race soak showed zero hits.",         GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_ASSETS,     GDX_GATE_BUCKET_BEHAVIOR, 0 },
    /* NO_G2_CONVERT     */ { "GDX_G2_CONVERT",           "gDevTools.Behavior.NoG2Convert",   "Disable wide-Gfx converter",    "Disables the G2 binary-to-wide display-list converter (GDX_G2_CONVERT=0).",                             GDX_GATE_ENV_KILL_SWITCH, GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_BEHAVIOR, 0 },
    /* INTERP_P0         */ { "GDX_INTERP_P0",            "gDevTools.Behavior.InterpP0",      "Force interpolation P0 path",   "Forces the experimental P0 matrix-interpolation path on, independent of the user-facing setting.",      GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_BEHAVIOR, 0 },
    /* INTERP_CAMERA     */ { "GDX_INTERP_CAMERA",        "gDevTools.Behavior.InterpCamera",  "Interpolate camera matrix",     "Also reroutes G_MTX_PROJECTION pool matrices through the interpolation scratch. race.c loads the combined projection*view camera with that flag, and the track has no matrix of its own, so both are frozen at 60 Hz while this is off.", GDX_GATE_ENV_OPT_IN,      GDX_GATE_GROUP_GRAPHICS,   GDX_GATE_BUCKET_BEHAVIOR, 0 },
    /* NO_REVERB         */ { "GDX_NO_REVERB",            "gDevTools.Behavior.NoReverb",      "Disable reverb wet return",     "Removes the reverb wet-to-dry return from the mixed buses (A/B for the recirculating-grain audit).",    GDX_GATE_ENV_ONE_ONLY,    GDX_GATE_GROUP_AUDIO,      GDX_GATE_BUCKET_BEHAVIOR, 0 },
    /* NO_HLE_FILTER     */ { "GDX_HLE_FILTER",           "gDevTools.Behavior.NoHleFilter",   "Disable HLE low-pass filter",   "Disables the HLE synthesis low-pass FIR (GDX_HLE_FILTER=0). Lets the reverb loop diverge.",             GDX_GATE_ENV_KILL_SWITCH, GDX_GATE_GROUP_AUDIO,      GDX_GATE_BUCKET_BEHAVIOR, 0 },
    /* SEQ_ADPCM         */ { "GDX_SEQ_ADPCM",            "gDevTools.Behavior.SeqAdpcm",      "Legacy sequential VADPCM",      "Restores the old sequential VADPCM decode instead of the hardware-correct block convolution.",          GDX_GATE_ENV_ONE_ONLY,    GDX_GATE_GROUP_AUDIO,      GDX_GATE_BUCKET_BEHAVIOR, 0 },
};
// clang-format on

static const char* const kGroupNames[GDX_GATE_GROUP_COUNT] = {
    "Logging", "Graphics", "Audio", "Assets", "Scheduling",
};

static unsigned char sFromEnv[GDX_GATE_COUNT]; // env var was exported at launch
// What that env var RESOLVED to at init. Kept separately from gGdxDevGateCache, which moves as the
// user toggles: pinning must key off the immutable launch decision, or a gate would re-pin itself
// the moment it was switched on.
static unsigned char sEnvValue[GDX_GATE_COUNT];
static int sEnvInitDone;

static int (*sCVarGetInteger)(const char*, int);
static void (*sCVarSetInteger)(const char*, int);

// A Bucket B gate is inert — and never even reads its env var — in a build without GDX_DEV_TOOLS.
// Buckets A and D are always live: gates that only log cost nothing while they are off.
static int GateCompiledIn(int id) {
#ifdef GDX_DEV_TOOLS
    (void) id;
    return 1;
#else
    return kGates[id].bucket != GDX_GATE_BUCKET_BEHAVIOR;
#endif
}

// getenv_s under MSVC (plain getenv is deprecated there and this build does not blanket-define
// _CRT_SECURE_NO_WARNINGS for the port target), plain getenv elsewhere. Returns 1 and fills `buf`
// when the variable exists.
//
// A gate value is decided from at most the first character, so 32 bytes is ample. A longer value
// is reported as PRESENT with an empty string: presence is what all four env kinds key off first.
static int ReadEnv(const char* name, char* buf, size_t bufSize) {
    buf[0] = '\0';
#ifdef _WIN32
    {
        size_t len = 0;
        /* The return type is spelled inline rather than as errno_t so this file needs no
           MSVC-specific typedef; a non-zero result means "exists but did not fit". */
        const int rc = (int) getenv_s(&len, buf, bufSize, name);
        if (len == 0) {
            return 0; // not set
        }
        if (rc != 0) {
            buf[0] = '\0'; // set but did not fit — treat as present with no value
        }
        return 1;
    }
#else
    {
        const char* v = getenv(name);
        if (v == NULL) {
            return 0;
        }
        strncpy(buf, v, bufSize - 1);
        buf[bufSize - 1] = '\0';
        return 1;
    }
#endif
}

// Returns the normalized gate value (1 = deviates from stock).
static int EvaluateEnv(const GdxGateDesc* g, int* outPresent) {
    char v[32];
    *outPresent = ReadEnv(g->env, v, sizeof(v));
    if (!*outPresent) {
        return g->defaultValue;
    }
    switch (g->envKind) {
        case GDX_GATE_ENV_PRESENCE:
            return 1;
        case GDX_GATE_ENV_OPT_IN:
            return (v[0] != '\0' && v[0] != '0') ? 1 : 0;
        case GDX_GATE_ENV_ONE_ONLY:
            return (v[0] == '1') ? 1 : 0;
        case GDX_GATE_ENV_KILL_SWITCH:
            return (v[0] == '0') ? 1 : 0;
        case GDX_GATE_ENV_TRISTATE:
            // Set-but-empty keeps the compiled default, matching the original gdx_trace_enabled().
            return (v[0] == '\0') ? g->defaultValue : ((v[0] != '0') ? 1 : 0);
        default:
            return 0;
    }
}

void gdx_dev_gates_init_env(void) {
    int i;
    if (sEnvInitDone) {
        return;
    }
    sEnvInitDone = 1;
    for (i = 0; i < GDX_GATE_COUNT; i++) {
        int present = 0;
        if (!GateCompiledIn(i)) {
            // Release build, behaviour gate: no getenv at all, hard 0 (stock).
            gGdxDevGateCache[i] = 0;
            sFromEnv[i] = 0;
            sEnvValue[i] = 0;
            continue;
        }
        gGdxDevGateCache[i] = EvaluateEnv(&kGates[i], &present);
        sFromEnv[i] = (unsigned char) (present ? 1 : 0);
        sEnvValue[i] = (unsigned char) (gGdxDevGateCache[i] ? 1 : 0);
    }
}

void gdx_dev_gates_boot_seed(int (*cvarGetInteger)(const char*, int)) {
    int i;
    gdx_dev_gates_init_env();
    if (cvarGetInteger == NULL) {
        return;
    }
    for (i = 0; i < GDX_GATE_COUNT; i++) {
        if (kGates[i].bucket != GDX_GATE_BUCKET_BOOT) {
            continue; // A/B gates are adopted later, in bind_cvars
        }
        if (sFromEnv[i]) {
            continue; // env pins this gate for the run and is never written back
        }
        gGdxDevGateCache[i] = cvarGetInteger(kGates[i].cvar, kGates[i].defaultValue) ? 1 : 0;
    }
}

void gdx_dev_gates_bind_cvars(int (*cvarGetInteger)(const char*, int),
                              void (*cvarSetInteger)(const char*, int)) {
    int i;
    gdx_dev_gates_init_env();
    sCVarGetInteger = cvarGetInteger;
    sCVarSetInteger = cvarSetInteger;
    if (sCVarSetInteger != NULL) {
        for (i = 0; i < GDX_GATE_COUNT; i++) {
            if (!GateCompiledIn(i) || kGates[i].bucket == GDX_GATE_BUCKET_BOOT) {
                continue;
            }
            // Only an env var that was actually exported seeds the CVar. Gates with no env var
            // are left alone: an absent CVar reads as its default and a persisted user toggle
            // survives untouched.
            if (sFromEnv[i]) {
                sCVarSetInteger(kGates[i].cvar, gGdxDevGateCache[i]);
            }
        }
    }
    gdx_dev_gates_refresh();
}

void gdx_dev_gates_refresh(void) {
    int i;
    if (sCVarGetInteger == NULL) {
        return; // pre-Gui boot, or a unit-test harness with no console: keep the env-seeded values
    }
    for (i = 0; i < GDX_GATE_COUNT; i++) {
        if (!GateCompiledIn(i)) {
            continue; // stays 0; never reads a CVar either
        }
        if (gdx_dev_gate_is_env_pinned(i)) {
            continue; // Bucket D + env exported: the run is pinned, the CVar must not move it
        }
        gGdxDevGateCache[i] = sCVarGetInteger(kGates[i].cvar, kGates[i].defaultValue) ? 1 : 0;
    }
}

void gdx_dev_gate_force(int id, int value) {
    if (id < 0 || id >= GDX_GATE_COUNT || !GateCompiledIn(id)) {
        return;
    }
    gGdxDevGateCache[id] = (value != 0) ? 1 : 0;
    if (sCVarSetInteger != NULL && !gdx_dev_gate_is_env_pinned(id)) {
        sCVarSetInteger(kGates[id].cvar, gGdxDevGateCache[id]);
    }
}

int gdx_dev_gate_count(void) {
    return GDX_GATE_COUNT;
}

static int ValidId(int id) {
    return id >= 0 && id < GDX_GATE_COUNT;
}

const char* gdx_dev_gate_env_name(int id) {
    return ValidId(id) ? kGates[id].env : "";
}

const char* gdx_dev_gate_cvar_name(int id) {
    return ValidId(id) ? kGates[id].cvar : "";
}

const char* gdx_dev_gate_label(int id) {
    return ValidId(id) ? kGates[id].label : "";
}

const char* gdx_dev_gate_help(int id) {
    return ValidId(id) ? kGates[id].help : "";
}

int gdx_dev_gate_group(int id) {
    return ValidId(id) ? (int) kGates[id].group : GDX_GATE_GROUP_GRAPHICS;
}

int gdx_dev_gate_bucket(int id) {
    return ValidId(id) ? (int) kGates[id].bucket : GDX_GATE_BUCKET_DIAG;
}

int gdx_dev_gate_is_behavior(int id) {
    return ValidId(id) && kGates[id].bucket == GDX_GATE_BUCKET_BEHAVIOR;
}

int gdx_dev_gate_default(int id) {
    return ValidId(id) ? (int) kGates[id].defaultValue : 0;
}

const char* gdx_dev_gate_group_name(int group) {
    return (group >= 0 && group < GDX_GATE_GROUP_COUNT) ? kGroupNames[group] : "";
}

int gdx_dev_gate_from_env(int id) {
    return ValidId(id) ? (int) sFromEnv[id] : 0;
}

/* A gate is pinned only when the environment actively holds it ON for this run — presence alone
 * is not enough. An exported GDX_LOG=0 (or a set-but-empty GDX_TRACE) reads as PRESENT but
 * resolves to OFF, which is indistinguishable in effect from the default, so there is nothing to
 * protect; keying on presence drew a greyed-out unticked checkbox and locked users out of their
 * own settings. */
int gdx_dev_gate_is_env_pinned(int id) {
    return ValidId(id) && sFromEnv[id] && sEnvValue[id] && kGates[id].bucket == GDX_GATE_BUCKET_BOOT;
}

int gdx_dev_gate_no_reverb(void) {
    return gGdxDevGateCache[GDX_GATE_NO_REVERB];
}

int gdx_dev_gate_log_file(void) {
    return gGdxDevGateCache[GDX_GATE_LOG_FILE];
}

int gdx_dev_gate_diag_texreg(void) {
    return gGdxDevGateCache[GDX_GATE_DIAG_TEXREG];
}
