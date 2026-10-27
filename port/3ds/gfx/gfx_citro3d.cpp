/* port/3ds/gfx/gfx_citro3d.cpp — stream A: citro3d Fast::GfxRenderingAPI backend.
 *
 * See gfx_citro3d.h for the architecture overview. Layout of a repacked vertex
 * (what the PICA sees; shader.v.pica inputs):
 *   v0: position x,y,z,w   — interpreter clip space, fixed up by the projection
 *                            uniform (portrait rotation + depth remap)
 *   v1: texcoord u,v       — texel0 UVs rescaled for pow2 texture padding
 *   v2: color r,g,b,a      — the combiner input routed to GPU_PRIMARY_COLOR
 *
 * Coordinate conventions (TODO(citra-verify): validate signs against the Phase 0
 * DL replay harness under Citra before M1 — orientation bugs are visible and
 * cheap to flip here):
 *   - GetClipParameters() returns z_is_from_0_to_1 = true, so incoming clip z is
 *     in [0, w] with 0 = near. The fixup matrix maps it to PICA's [-w, 0] via
 *     z' = z - w, giving depth = 1 at near / 0 at far after
 *     C3D_DepthMap(true, -1, 0); depth test is therefore GPU_GEQUAL-flavoured
 *     ("greater or equal is nearer") and depth clears to 0.
 *   - 3DS top-screen framebuffers are 240x400 portrait; the fixup matrix rotates
 *     x/y, and viewport/scissor rectangles are swapped to match.
 */
#include "gfx_citro3d.h"
#include "gdx3ds_xfprof.h"
#include "gdx3ds_mesh.h" // [native] machines: indexed backend entry + frame cadence
#include "nrr/gdx3ds_nrr_mach.h" // n60 NRR machines: the backend half (gfx_citro3d_nrrmach.inc)
#include "gdx3ds_vtx_math.h" // [native] gpuxf: GDX3DS_CULL_* (the CPU cull modes the PICA mirrors)
#include <cstddef>
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));

#include "gdx3ds_gpu_prof.h" // G-GPUPROF [gpu]/[fill] telemetry (gated: debug.gputrace)
#include "gdx3ds_stereo.h" // stream S: stereo foundation (dual targets + per-eye loop)
#include "gdx3ds_nativebatch.h" // Gdx_NativeBatch: shared native draw primitive ([native] track/machines)
#include "gdx3ds_texclamp.h" // the pow2-pad / atlas-view UV clamp contract (host-tested)
#include "gdx3ds_cov.h" // COVERAGE: [cov] per-frame feature census ([debug] cov)
#include "gdx3ds_gputl.h" // n60gpu: [debug] gputl (per-eye GPU timeline from GSP completion stamps)
#include "gdx3ds_skylate.h" // n60tail: [native] sky_late (the sky's lists after the opaque track)
#include "gdx3ds_gpucensus.h" // n60gpu: [debug] gpucensus (GPU work census per eye / category)
#include "gdx3ds_overlap.h" // C5 Stage 4 [overlap]: kick the GX queue mid-frame ([debug] overlap_kick)
#include "nrr/gdx3ds_nrr_crumb.h" // n60hwdiag: durable crumbs around C3D_FrameBegin / C3D_FrameEnd
#include "gdx3ds_c3d_diet.h" // c5submit (r60 P3a): [native] c3d_diet, the vendored citro3d submission diet
#include "gdx3ds_replay.h"  // C6 [replay]: pose-replay, the per-draw uniform sites ([native] replay)
#include "gdx3ds_hitch.h" // HITCH: per-frame event counters ([debug] hitch)
#include "gdx3ds_texswz.h" // N60TEX: [debug] tex_fast texel writer
#include "gdx3ds_hwprof.h" // hwprof [hwsite]: per-call micro-timers ([debug] hwsite, default 0)
#include "gdx3ds_hotcold.h" // hotcold: GDX_COLD / GDX_UNLIKELY (per-function source attributes only)

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib> // std::getenv (GDX_DIAG_SKY backdrop-coverage diagnostic)
#include <cstring>

extern "C" {
#include "gdx3ds_gfx_shader_shbin.h"
}
#include "gdx3ds_prewrite.h" /* o3ds3d: [debug] prewrite / gpumem */
/* o3ds3d [prewrite]: a framebuffer clear or copy writes only if it queues a GX entry (a zclear_elide
 * clear queues none), so it is bracketed by two polls instead of stamped at its entry. */
struct GdxPwGxBracket {
    GdxPwGxBracket() { GDX3DS_PW_POLL(); }
    ~GdxPwGxBracket() { GDX3DS_PW_POLL(); }
};

/* nrr/gdx3ds_nrr_internal.h, defined below via gfx_citro3d_nrr.inc (included at the end of
 * namespace Fast): forward-declared here so StartFrame() can call it eagerly (nimenu W1/M2,
 * review finding M2). extern "C" linkage is namespace-independent, so this one declaration and
 * the .inc's later in-namespace definition denote the same function. */
extern "C" int gdx3ds_nrr_bk_frame_active(void);

/* gbi.h (via fast/interpreter.h) defines these, but restate the two bits we use so
 * the mapping is obvious at the call site. */
#ifndef G_TX_MIRROR
#define G_TX_MIRROR 0x1
#endif
#ifndef G_TX_CLAMP
#define G_TX_CLAMP 0x2
#endif

/* Mirror every backend diagnostic onto the svc debug channel: stderr only reaches the
 * bottom-screen console, which is unreadable in headless Azahar runs (m1-boot-debug). */
GDX_COLD static void GfxC3dLogImpl(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    {
        extern __thread int gdx_port_log_console_muted __attribute__((weak));
        extern int gdx3ds_console_echo_enabled __attribute__((weak));
        const bool echoOff = (&gdx3ds_console_echo_enabled != nullptr) && !gdx3ds_console_echo_enabled;
        if ((&gdx_port_log_console_muted == nullptr || !gdx_port_log_console_muted) && !echoOff) {
            std::fprintf(stderr, "[gfx_citro3d] %s", buf); /* console: main thread, menu not owning it */
        }
    }
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        len--;
    }
    if (len > 0) {
        svcOutputDebugString(buf, len);
        /* RENDER THREAD: backend diagnostics ([readback]/[fbparam]/"never landed") must reach
         * the SD filelog too -- svc is invisible on hardware and the console echo is muted on
         * the render thread. Weak: absent in the DL harness link. */
        /* gdx3ds_filelog_write: file-scope extern "C" weak declaration above (a block-scope C++ one
           mangled to a distinct, always-null weak symbol). */
        if (&gdx3ds_filelog_write != nullptr) {
            gdx3ds_filelog_write(buf, len);
        }
    }
}

#define GFX_C3D_LOG(...) GfxC3dLogImpl(__VA_ARGS__)

/* --------------------------------------------------------------------------------
 * Texture-cache telemetry (T-TEXCACHE). The interpreter calls these hooks (see the
 * lus-texcache patch) so the [c3d] line can split texUp into lookups / misses /
 * invalidations, and so race-time misses are named with enough key detail to tell
 * a hash-unstable key from LRU/invalidation churn. Weak gGdxRaceActive: the DL
 * harness links the backend without the port bridge's race latch.
 * -------------------------------------------------------------------------------- */
extern "C" int gGdxRaceActive __attribute__((weak));

// Stream B INI accessor (port/3ds/os/gdx3ds_config.h), weak so the gfx backend still
// links into the DL-test harness which has no config target (same as gdx3ds_gpu_prof.c).
// MUST be extern "C": the real symbol has C linkage; a C++-mangled decl resolves to a
// distinct, unresolved weak symbol (nullptr) and silently disables the INI fallback.
// Used by the GDX_DIAG_SKY diagnostic to read [debug] diag_sky on the 3DS, where ctru
// getenv cannot see azahar's host environment.
extern "C" float gdx_get_widescreen_geometry_xscale(void) __attribute__((weak));
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback)
    __attribute__((weak));
extern "C" int gdx3ds_config_loaded(void) __attribute__((weak));

/* [atlasdiag] frame mirror + window predicate, declared here so the texture-cache miss
 * hook (a free function above the renderer) can share the atlas trace's frame window. */
static unsigned long sAtlasDiagFrame = 0;
/* [atlasdiag] investigation receipt for the frame-1702 residual ([debug] atlasdiag_from /
 * atlasdiag_to, both 0 = off): every atlas placement, release, eviction and page allocation
 * inside the frame window, so two runs' placement SEQUENCES can be diffed line for line. */
static unsigned long sAtlasDiagFrom = 0;
static unsigned long sAtlasDiagTo = 0;
static bool AtlasDiagInit() {
    if (&gdx3ds_config_get_int != nullptr) {
        sAtlasDiagFrom = (unsigned long)gdx3ds_config_get_int("debug", "atlasdiag_from", 0);
        sAtlasDiagTo = (unsigned long)gdx3ds_config_get_int("debug", "atlasdiag_to", 0);
    }
    return sAtlasDiagTo >= sAtlasDiagFrom && sAtlasDiagTo != 0;
}
static bool AtlasDiagArmed();
static bool AtlasDiagInWindow() {
    return AtlasDiagArmed() && sAtlasDiagFrame >= sAtlasDiagFrom && sAtlasDiagFrame <= sAtlasDiagTo;
}
static bool AtlasDiagArmed() {
    static const bool sOn = [] {
        const bool on = AtlasDiagInit();
        GFX_C3D_LOG("[atlasdiag] armed=%d from=%lu to=%lu\n", (int)on, sAtlasDiagFrom, sAtlasDiagTo);
        return on;
    }();
    return sOn;
}

/* [atlasuv] Texcoord receipt: the exact float texcoords this backend hands PICA for texture
 * unit 0, expressed in the BOUND texture's texel units (u * padWidth), so a trectbatch=0 run
 * and a trectbatch=1 run can be compared rect-for-rect. Gated on [debug] uvdiag=1 plus the
 * atlasdiag frame window; batches are paired across runs by their screen-space bbox, which
 * the lever cannot move. `cs` is the CONTENT-relative texel (page texel - cell origin), the
 * quantity the two paths are supposed to agree on exactly. */
static bool UvDiagOn() {
    static const bool sOn = [] {
        const bool on = (&gdx3ds_config_get_int != nullptr) && gdx3ds_config_get_int("debug", "uvdiag", 0) != 0;
        GFX_C3D_LOG("[uvdiag] on=%d\n", (int)on);
        return on;
    }();
    return sOn;
}
static uint32_t UvDiagBits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}


namespace {
unsigned long sTexCacheImports = 0;
unsigned long sTexCacheMisses = 0;
unsigned long sTexCacheDeletes = 0;
/* SELECT-PERF: per-SETTIMG resource-resolution counters. sSettimgLoads counts full
 * LoadResourceProcess resolutions taken by the o2r-filepath SETTIMG handler;
 * sSettimgMemoHits counts resolutions served from the interpreter's SETTIMG memo
 * (lus-3ds-settimg-resolution-memo.patch). On the machine-select screens the memo
 * should absorb nearly all of the ~93/frame resolutions, so a healthy run shows
 * dRl collapsing toward 0 while dRm carries the volume. */
unsigned long sSettimgLoads = 0;
unsigned long sSettimgMemoHits = 0;
/* [texmiss] sampling budget for NON-race phases (menus/attract), refilled at every
 * [c3d] window emit so steady-state churn keeps being sampled all session at a
 * bounded rate (<= 6 lines / 64 frames) instead of burning a lifetime budget on
 * boot. Race phase keeps its original 400-line lifetime budget. */
int sTexMissWindowBudget = 6;

/* QUIET MODE (MENU-PERF): recurring telemetry svc lines ([c3d], [fogdraw] windows,
 * [texmiss] sampling) are emitted only when `[debug] verbose = 1` or
 * `[debug] gputrace = 1` is set in gdiffuser.ini. Azahar's log_filter=*:Debug
 * processes every svcOutputDebugString, so recurring lines are an emulator-side
 * frame tax in normal play; measurement runs always set gputrace=1 and lose
 * nothing. One-shot boot lines, error paths, deduped-per-shader lines and all
 * diag_*-gated output are NOT gated here. The [watchdog] heartbeat
 * (main_3ds.cpp, ~1 line / 5 s) stays unconditional — fps is derived from its
 * frame deltas. In the DL-test harness the weak config accessor is absent; the
 * gate then stays OPEN, preserving the harness's pre-existing [c3d] output
 * (EXPECTED.md references it). */
bool GdxVerboseTelemetry() {
    static int sLatch = -1;
    if (sLatch < 0) {
        if (&gdx3ds_config_get_int != nullptr) {
            sLatch = (gdx3ds_config_get_int("debug", "verbose", 0) != 0 ||
                      gdx3ds_config_get_int("debug", "gputrace", 0) != 0)
                         ? 1
                         : 0;
        } else {
            sLatch = 1; // harness link: no config target, keep legacy output
        }
    }
    return sLatch == 1;
}
} // namespace

/* --------------------------------------------------------------------------------
 * Exact fog line (C2/C3-COURSE-CULL). The interpreter mirrors the RSP fog state here
 * at fog-attribute APPEND time (each tri entering the batch) and on ucode-load reset,
 * so UpdateFogState can build the PICA LUT from the true curve instead of fitting a
 * secant through the batch's (clamped) endpoint fog factors. Append-time (not
 * G_MW_FOG moveword-time) latching matters: movewords do not flush pending tris, so
 * a moveword-time mirror hands the already-batched draw the NEXT draw's fog line —
 * that misbinding painted the entire race track with a later, shallower fog line
 * (solid fog colour: the "invisible road" bug). exact=0 flags constant-factor
 * blend-colour draws; those keep the vertex-scan fit, whose a~0/b~const line IS
 * their correct semantics.
 * -------------------------------------------------------------------------------- */
namespace {
int sGdxFogMul = 0;
int sGdxFogOffset = 0;
bool sGdxFogParamsValid = false;
unsigned sGdxFogFrame = 0; // StartFrame counter, correlates [fogdraw] windows with SHOTs
} // namespace

extern "C" void gdx3ds_fog_note_params(int mul, int off, int exact) {
    sGdxFogMul = mul;
    sGdxFogOffset = off;
    sGdxFogParamsValid = (exact != 0);
}

extern "C" void gdx3ds_texcache_note_import(void) {
    sTexCacheImports++;
    gdx3ds_hitch_add(GDX_HITCH_EV_TEX_IMPORT, 1u); /* HITCH: per-frame event census */
}

extern "C" void gdx3ds_texcache_note_delete(unsigned evictedEntries) {
    sTexCacheDeletes += evictedEntries;
}

/* SELECT-PERF hooks: called by the interpreter's o2r-filepath SETTIMG handler
 * (same strong-link pattern as the texcache notes above). */
extern "C" void gdx3ds_settimg_note_load(void) {
    sSettimgLoads++;
}

extern "C" void gdx3ds_settimg_note_memo(void) {
    sSettimgMemoHits++;
}

/* --------------------------------------------------------------------------------
 * PRIM-COLOR (ship yellow-body) [prim] diag. The interpreter's G_SETPRIMCOLOR /
 * G_SETENVCOLOR handlers (lus-3ds-primenv-flush.patch) now split the batch when the
 * VALUE changes — on this backend those colours are TexEnv stage constants bound
 * from vertex 0 of a flushed batch, so a batch spanning a change painted every
 * pending tri with the FIRST draw's colour (the player's I4 body in a rival's
 * yellow; machine-select draws one machine and never spanned a change). The
 * one-shot diag logs, for 3 consecutive race frames (after a 90-race-frame warmup,
 * matching [interleave]):
 *   [prim] set  p=0|1 rgba=RRGGBBAA pend=N split=0|1  — every value change, with the
 *          pending-tri count it split off (split=1 = the fix fired; on the pre-fix
 *          code pend>0 here is exactly the stale-constant window);
 *   [prim] draw #N id0=LO32 tris=N tex0=WxH c0=AABBGGRR re=MASK — every draw whose
 *          shader consumes an input-driven stage constant: c0 is the constant bound
 *          for this draw in PICA packed layout, r in the LOW byte (machine bodies:
 *          the ENV body colour — the player's must read blue, e.g. b-high/r-low;
 *          a rival yellow reads r=ff g=~cb b=00), re= the stages whose register
 *          actually moved this draw.
 * Gate: INI [debug] diag_prim=1 or env GDX_DIAG_PRIM (3DS getenv cannot see the
 * host env — the INI key is the reachable switch, same bridge as diag_sky).
 * -------------------------------------------------------------------------------- */
namespace {
bool GdxPrimDiagEnabled() {
    static int sLatch = -1;
    if (sLatch < 0) {
        if (std::getenv("GDX_DIAG_PRIM") != nullptr) {
            sLatch = 1;
        } else if (&gdx3ds_config_get_int != nullptr) {
            sLatch = gdx3ds_config_get_int("debug", "diag_prim", 0) != 0 ? 1 : 0;
        } else {
            sLatch = 0;
        }
    }
    return sLatch == 1;
}

constexpr unsigned kPrimDiagWarmupFrames = 90; // race frames before the log window opens
constexpr unsigned kPrimDiagWindowFrames = 3;  // consecutive race frames logged
constexpr int kPrimDiagLineBudget = 160;       // per-frame line cap (set + draw combined)

unsigned sPrimDiagRaceFrames = 0;
bool sPrimDiagArmed = false;
bool sPrimDiagDone = false;
int sPrimDiagLines = 0;
unsigned sPrimDiagDrawIndex = 0;

/* Called once per StartFrame. Arms/retires the one-shot window. */
void GdxPrimDiagFrameBegin() {
    if (sPrimDiagDone || !GdxPrimDiagEnabled()) {
        return;
    }
    const bool raceActive = (&gGdxRaceActive != nullptr) && (gGdxRaceActive != 0);
    if (!raceActive) {
        return;
    }
    ++sPrimDiagRaceFrames;
    if (sPrimDiagRaceFrames <= kPrimDiagWarmupFrames) {
        return;
    }
    if (sPrimDiagRaceFrames > kPrimDiagWarmupFrames + kPrimDiagWindowFrames) {
        if (sPrimDiagArmed) {
            sPrimDiagArmed = false;
            const char kDone[] = "[prim] window done";
            svcOutputDebugString(kDone, sizeof(kDone) - 1);
        }
        sPrimDiagDone = true;
        return;
    }
    sPrimDiagArmed = true;
    sPrimDiagLines = kPrimDiagLineBudget;
    sPrimDiagDrawIndex = 0;
    char msg[64];
    int n = std::snprintf(msg, sizeof(msg), "[prim] frame %u begin",
                          sPrimDiagRaceFrames - kPrimDiagWarmupFrames);
    if (n > 0) {
        svcOutputDebugString(msg, (size_t)n);
    }
}

bool GdxPrimDiagTakeLine() {
    if (!sPrimDiagArmed || sPrimDiagLines <= 0) {
        return false;
    }
    --sPrimDiagLines;
    return true;
}
} // namespace

/* Interpreter hook (lus-3ds-primenv-flush.patch): a prim/env VALUE change, with the
 * batch it split off. Strong link, cheap early-return when the diag is off. */
extern "C" void gdx3ds_prim_note_set(int isPrim, unsigned r, unsigned g, unsigned b, unsigned a,
                                     unsigned pendingTris, int flushed) {
    if (!GdxPrimDiagTakeLine()) {
        return;
    }
    char msg[96];
    int n = std::snprintf(msg, sizeof(msg), "[prim] set p=%d rgba=%02x%02x%02x%02x pend=%u split=%d",
                          isPrim != 0 ? 1 : 0, r & 0xFF, g & 0xFF, b & 0xFF, a & 0xFF, pendingTris,
                          flushed != 0 ? 1 : 0);
    if (n > 0) {
        svcOutputDebugString(msg, (size_t)n);
    }
}

/* --------------------------------------------------------------------------------
 * SHIP-LIVERY-2 [livery] diag — name WHICH texture every in-race draw actually
 * samples. The prior rounds proved delivery/cache-keys/slot-pairing/colour-constants
 * correct for the slots the [interleave] window covered (0x0/0x10/0x50), yet the
 * player's body still renders another machine's yellow decals. This window logs, in
 * true execution order, for 3 race frames after a 90-frame warmup:
 *   [livery] load tmem=0xNNN b=SIZE <path-tail|@addr>   — every TMEM store
 *     (Interpreter::StoreLoadedTexture, lus-3ds-livery-ident.patch);
 *   [livery] imp u=U tile=T tmem=0xNNN fs=F/S id=ID hit=H WxH <path-tail|@addr>
 *     — every ImportTexture resolution with the BOUND texture id + identity;
 *   [livery] draw #N tris=N t0=ID:WxH:<name> [t1=...] uv=(u,v) [c0=CONST] — every
 *     textured draw, its bound texture ids resolved to identity via the id→name
 *     table (maintained on every import, so warm-cache binds still resolve), the
 *     first vertex's unit-0 UV, and the input-driven stage constant when bound
 *     (machine bodies: the ENV body colour, r in the LOW byte — the player reads
 *     blue-high/red-low, a rival yellow reads r=ff g=~cb b=00).
 * The player's machine is drawn LAST of the machines (racer.c walks sLastRacer
 * down to gRacers[0]); its body draws are the I4 16x16 draws whose c0 is blue.
 * Any name in those draws that is not in Machine_DrawLoadBlueFalconTextures'
 * texture set (wingChecker/stripe/arrow/logoBlueFalcon/number7/rearDuct/booster1
 * + the body sheet) IS the culprit, and its neighbouring load/imp lines name the
 * mechanism. On each window frame the weak game-side receipt
 * (port/3ds/game/gdx3ds_livery_diag.c) also dumps gRacers[i] machineIndex /
 * character / customType / machineLod and gMachines customType, discriminating
 * the stale-custom-decal hypothesis without touching the decomp.
 * Gate: INI [debug] diag_livery=1 or env GDX_DIAG_LIVERY (3DS getenv cannot see
 * the host env — the INI key is the reachable switch, same bridge as diag_prim).
 * -------------------------------------------------------------------------------- */
extern "C" void gdx3ds_livery_game_receipt(void) __attribute__((weak));

namespace {
bool GdxLiveryDiagEnabled() {
    static int sLatch = -1;
    if (sLatch < 0) {
        if (std::getenv("GDX_DIAG_LIVERY") != nullptr) {
            sLatch = 1;
        } else if (&gdx3ds_config_get_int != nullptr) {
            sLatch = gdx3ds_config_get_int("debug", "diag_livery", 0) != 0 ? 1 : 0;
        } else {
            sLatch = 0;
        }
    }
    return sLatch == 1;
}

constexpr unsigned kLiveryWarmupFrames = 90; // race frames before the log window opens
constexpr unsigned kLiveryWindowFrames = 3;  // consecutive race frames logged
constexpr int kLiveryLineBudget = 900;       // per-frame line cap (load + imp + draw)

unsigned sLiveryRaceFrames = 0;
bool sLiveryArmed = false;
bool sLiveryDone = false;
int sLiveryLines = 0;
unsigned sLiveryDrawIndex = 0;

/* id→identity table, maintained on EVERY import (window or not) so draws that bind
 * a warm-cache texture id still resolve to a name inside the window. Direct-mapped
 * by texture id: ids come from the interpreter's 1024-entry texture cache, so
 * matching its size makes eviction-reuse overwrite exactly the right slot. */
constexpr unsigned kLiveryNameSlots = 1024;
constexpr unsigned kLiveryNameLen = 28;
struct LiveryName {
    uint32_t id = 0xffffffffu;
    char tail[kLiveryNameLen] = { 0 };
};
LiveryName sLiveryNames[kLiveryNameSlots];

const char* GdxLiveryNameForId(uint32_t texId) {
    const LiveryName& e = sLiveryNames[texId % kLiveryNameSlots];
    return e.id == texId ? e.tail : "?";
}

bool GdxLiveryTakeLine() {
    if (!sLiveryArmed || sLiveryLines <= 0) {
        return false;
    }
    --sLiveryLines;
    return true;
}

/* Called once per StartFrame, next to GdxPrimDiagFrameBegin. */
void GdxLiveryDiagFrameBegin() {
    if (sLiveryDone || !GdxLiveryDiagEnabled()) {
        return;
    }
    const bool raceActive = (&gGdxRaceActive != nullptr) && (gGdxRaceActive != 0);
    if (!raceActive) {
        return;
    }
    ++sLiveryRaceFrames;
    if (sLiveryRaceFrames <= kLiveryWarmupFrames) {
        return;
    }
    if (sLiveryRaceFrames > kLiveryWarmupFrames + kLiveryWindowFrames) {
        if (sLiveryArmed) {
            sLiveryArmed = false;
            const char kDone[] = "[livery] window done";
            svcOutputDebugString(kDone, sizeof(kDone) - 1);
        }
        sLiveryDone = true;
        return;
    }
    sLiveryArmed = true;
    sLiveryLines = kLiveryLineBudget;
    sLiveryDrawIndex = 0;
    char msg[64];
    int n = std::snprintf(msg, sizeof(msg), "[livery] frame %u begin",
                          sLiveryRaceFrames - kLiveryWarmupFrames);
    if (n > 0) {
        svcOutputDebugString(msg, (size_t)n);
    }
    if (&gdx3ds_livery_game_receipt != nullptr) {
        gdx3ds_livery_game_receipt(); // [livery] state receipts (gRacers/gMachines)
    }
}
} // namespace

/* Interpreter hooks (lus-3ds-livery-ident.patch). note_import ALWAYS maintains the
 * id→name table (a bounded strncpy per import, ~100-200/frame — negligible); both
 * only emit lines while the one-shot window is armed. */
extern "C" void gdx3ds_livery_note_import(int unit, int tile, unsigned tmemWord, unsigned fmt,
                                          unsigned siz, unsigned texId, const char* path,
                                          const void* addr, unsigned tileW, unsigned tileH, int hit) {
    if (texId != 0xffffffffu) {
        LiveryName& e = sLiveryNames[texId % kLiveryNameSlots];
        e.id = texId;
        if (path != nullptr) {
            const size_t len = std::strlen(path);
            const char* tail = len >= kLiveryNameLen ? path + (len - (kLiveryNameLen - 1)) : path;
            std::strncpy(e.tail, tail, kLiveryNameLen - 1);
            e.tail[kLiveryNameLen - 1] = '\0';
        } else {
            std::snprintf(e.tail, kLiveryNameLen, "@%08lx", (unsigned long)(uintptr_t)addr);
        }
    }
    if (!GdxLiveryTakeLine()) {
        return;
    }
    char msg[160];
    int n = std::snprintf(msg, sizeof(msg),
                          "[livery] imp u=%d tile=%d tmem=0x%03x fs=%u/%u id=%u hit=%d %ux%u %s",
                          unit, tile, tmemWord, fmt, siz, texId, hit,
                          tileW, tileH, texId != 0xffffffffu ? GdxLiveryNameForId(texId) : "?");
    if (n > 0) {
        svcOutputDebugString(msg, (size_t)n);
    }
}

extern "C" void gdx3ds_livery_note_load(unsigned tmemWord, const char* path, const void* addr,
                                        unsigned sizeBytes) {
    if (!GdxLiveryTakeLine()) {
        return;
    }
    char msg[160];
    int n;
    if (path != nullptr) {
        n = std::snprintf(msg, sizeof(msg), "[livery] load tmem=0x%03x b=%u %s", tmemWord, sizeBytes,
                          path);
    } else {
        n = std::snprintf(msg, sizeof(msg), "[livery] load tmem=0x%03x b=%u @%08lx", tmemWord,
                          sizeBytes, (unsigned long)(uintptr_t)addr);
    }
    if (n > 0) {
        svcOutputDebugString(msg, (size_t)n);
    }
}

/* Cumulative-counter snapshot for gdx3ds_gpu_prof.c's per-window [gpu] columns
 * (imp=/rl=/rm=). Weak-linked from the profiler so the DL harness, which links the
 * profiler without this backend TU in some configurations, degrades to md-only. */
extern "C" void gdx3ds_texcache_prof_totals(unsigned long* imports, unsigned long* settimgLoads,
                                            unsigned long* settimgMemoHits) {
    if (imports != nullptr) {
        *imports = sTexCacheImports;
    }
    if (settimgLoads != nullptr) {
        *settimgLoads = sSettimgLoads;
    }
    if (settimgMemoHits != nullptr) {
        *settimgMemoHits = sSettimgMemoHits;
    }
}

extern "C" void gdx3ds_texcache_note_miss(const void* addr, unsigned fmt, unsigned siz, unsigned tmemWord,
                                          unsigned lineBytes, unsigned sizeBytes, unsigned tileW, unsigned tileH,
                                          unsigned contentHash) {
    sTexCacheMisses++;
    gdx3ds_hitch_add(GDX_HITCH_EV_TEX_MISS, 1u); /* HITCH */
    // Bounded key dumps, verbose/gputrace-gated (quiet mode): repeated lines for one addr
    // with a changing hash = unstable content key; repeated lines with the same hash =
    // eviction/invalidation churn. Race phase keeps the original 400-line lifetime
    // budget; other phases (menus/attract — the MENU-PERF churn question) get a small
    // per-[c3d]-window budget instead, so a menu-idle soak samples its steady-state
    // misses all session rather than burning the budget during boot.
    if (!GdxVerboseTelemetry()) {
        return;
    }
    const bool raceActive = (&gGdxRaceActive != nullptr) && (gGdxRaceActive != 0);
    static int sMissLogBudget = 400;
    bool emit = false;
    if (raceActive) {
        if (sMissLogBudget > 0) {
            --sMissLogBudget;
            emit = true;
        }
    } else if (sTexMissWindowBudget > 0) {
        --sTexMissWindowBudget;
        emit = true;
    }
    if (AtlasDiagInWindow()) {
        GFX_C3D_LOG("[texmissdiag] f=%lu a=%p fmt=%u/%u tm=%u ln=%u sz=%u t=%ux%u h=%08x\n", sAtlasDiagFrame, addr,
                    fmt, siz, tmemWord, lineBytes, sizeBytes, tileW, tileH, contentHash);
    }
    if (emit) {
        char msg[144];
        int n = std::snprintf(msg, sizeof(msg),
                              "[texmiss] a=%p f=%u/%u tm=%u ln=%u sz=%u t=%ux%u h=%08x race=%d",
                              addr, fmt, siz, tmemWord, lineBytes, sizeBytes, tileW, tileH, contentHash,
                              raceActive ? 1 : 0);
        if (n > 0) {
            svcOutputDebugString(msg, (size_t)n < sizeof(msg) - 1 ? (size_t)n : sizeof(msg) - 1);
        }
    }
}

namespace Fast {

namespace {

constexpr int kScreenWidth = 400;  // top screen, landscape (game space)
constexpr int kScreenHeight = 240;
constexpr size_t kOutStrideFloats = 12; // pos4 + uv0 2 + rgba4 + uv1 2 (shader.v.pica)
static_assert(kOutStrideFloats == GDX3DS_VBOPACK_STRIDE,
              "gdx3ds_vbopack.h stride must match the PICA vertex layout");
constexpr size_t kVboPoolBytes = 1 * 1024 * 1024;

/* SKY-WEDGE-3 content-edge UV clamp gate ([debug] sky_clamp_fix, default on).
 * Function-local static so the INI is read after config load; shared by the
 * legacy repack clamp derivation and [triloop] VboPackBegin. */
static bool GdxSkyClampFixEnabled() {
    static const bool sSkyClampFix = [] {
        if (std::getenv("GDX_SKY_CLAMP_FIX_OFF") != nullptr) {
            return false;
        }
        if (&gdx3ds_config_get_int != nullptr) {
            return gdx3ds_config_get_int("debug", "sky_clamp_fix", 1) != 0;
        }
        return true;
    }();
    return sSkyClampFix;
}

/* [padgutter] killswitch: [debug] padgutter=0 or env GDX_PAD_GUTTER_OFF restores the
 * all-zero pow2 padding (see the gutter replication in UploadTexture). Default on. */
/* N60TEX [debug] tex_fast (default 0): the texture upload's texel writer hoists the tile and
 * Morton terms and loads a word per texel (gdx3ds_texswz.h); the decoders take their table
 * forms (gdx_texcache.cpp). Same bytes, fewer instructions. */
extern "C" int gdx3ds_tex_fast_enabled(void);
static bool GdxTexFastEnabled() {
    return gdx3ds_tex_fast_enabled() != 0;
}

static bool GdxPadGutterEnabled() {
    static const bool sOn = [] {
        const char* env = std::getenv("GDX_PAD_GUTTER_OFF");
        const int cfg = (&gdx3ds_config_get_int != nullptr) ? gdx3ds_config_get_int("debug", "padgutter", 1) : 1;
        const int on = env != nullptr ? 0 : (cfg != 0 ? 1 : 0);
        GFX_C3D_LOG("[padgutter] init on=%d env=%p cfg=%d\n", on, (const void*)env, cfg);
        return on != 0;
    }();
    return sOn;
}

/* [atlasuv] killswitch / A-B discriminator: [debug] viewclamp=1 (or env GDX_ATLAS_VIEW_CLAMP)
 * restores the pre-fix behaviour where an atlas VIEW also took the content-edge coordinate
 * clamp at (extent - 0.5) / page. That pulled a pow2 glyph cell's far vertex from u = extent
 * to extent - 0.5, squeezing the rect's whole UV interpolation and shifting the sampled
 * content right by up to half a texel against trectbatch=0 (the 782-px course-name
 * residual). Default 0 = fixed: a view clamps at the cell's far edge, extent / page.
 * See gdx3ds_view_clamp_limit. */
static bool GdxAtlasViewClampLegacy() {
    static const bool sOn = [] {
        const char* env = std::getenv("GDX_ATLAS_VIEW_CLAMP");
        const int cfg = (&gdx3ds_config_get_int != nullptr) ? gdx3ds_config_get_int("debug", "viewclamp", 0) : 0;
        const int on = env != nullptr ? 1 : (cfg != 0 ? 1 : 0);
        GFX_C3D_LOG("[atlasuv] viewclamp legacy=%d env=%p cfg=%d\n", on, (const void*)env, cfg);
        return on != 0;
    }();
    return sOn;
}

/* [triloop] killswitch: [debug] triloop=0 or env GDX_TRILOOP_OFF refuses every
 * pack begin, restoring the legacy two-stage (append + repack) tri path. */
static bool GdxTriloopEnabled() {
    static const bool sEnabled = [] {
        if (std::getenv("GDX_TRILOOP_OFF") != nullptr) {
            return false;
        }
        if (&gdx3ds_config_get_int != nullptr) {
            return gdx3ds_config_get_int("debug", "triloop", 1) != 0;
        }
        return true;
    }();
    return sEnabled;
}

/* GEOMLOSS test knob: [debug] vbopoolkib shrinks the USABLE part of the 1 MiB linear VBO
 * pool so the exhaustion paths (packed-begin refusal, machines poolTrunc) can be forced in
 * a normal build. Default 0 = the whole pool, i.e. exactly the shipped behaviour. The
 * allocation itself is unchanged; only the cursor limit moves, so nothing can overrun. */
static size_t GdxVboPoolUsableFloats(size_t poolFloats) {
    static const int sKib = [] {
        if (&gdx3ds_config_get_int != nullptr) {
            return gdx3ds_config_get_int("debug", "vbopoolkib", 0);
        }
        return 0;
    }();
    if (sKib <= 0) {
        return poolFloats;
    }
    size_t want = (size_t)sKib * 1024u / sizeof(float);
    if (want < 4096u) {
        want = 4096u; /* floor: 16 KiB, still room for a few hundred vertices */
    }
    return want < poolFloats ? want : poolFloats;
}

/* GEOMLOSS killswitch: [debug] vbosplit=0 restores the pre-fix all-or-nothing packed begin
 * (refuse unless a whole 256-tri batch fits) for the owned interpreter too. That is the
 * behaviour that DROPS triangles under pool pressure; it exists only so the two behaviours
 * can be A/B'd from one binary. Default 1 = split the batch, never drop what still fits. */
/* GEOMLOSS test knob: [debug] vbopackcap=N caps every packed batch at N triangles regardless
 * of pool pressure, so "splitting a batch changes no pixels" can be proved on a full pool
 * instead of only in the narrow band where the pool happens to be nearly exhausted.
 * 0 = off = the pool decides. */
static uint32_t GdxVboPackCap() {
    static const uint32_t sCap = [] {
        if (&gdx3ds_config_get_int != nullptr) {
            const int n = gdx3ds_config_get_int("debug", "vbopackcap", 0);
            return n > 0 ? (uint32_t)n : (uint32_t)0;
        }
        return (uint32_t)0;
    }();
    return sCap;
}

static bool GdxVboSplitEnabled() {
    static const bool sOn = [] {
        if (&gdx3ds_config_get_int != nullptr) {
            return gdx3ds_config_get_int("debug", "vbosplit", 1) != 0;
        }
        return true;
    }();
    return sOn;
}

/* [trectbatch] killswitch (LOCKED-60 Task A): [debug] trectbatch=0 or env GDX_TRECTBATCH_OFF
 * keeps every rect on the legacy path (no atlas arming, no same-page merge); read weakly by
 * the patched interpreter at every task boundary. */
extern "C" int gdx3ds_trectbatch_enabled(void) {
    static const int sEnabled = [] {
        const char* env = std::getenv("GDX_TRECTBATCH_OFF");
        const int cfg = (&gdx3ds_config_get_int != nullptr) ? gdx3ds_config_get_int("debug", "trectbatch", 1) : 1;
        const int loaded = (&gdx3ds_config_loaded != nullptr) ? gdx3ds_config_loaded() : -1;
        const int on = env != nullptr ? 0 : (cfg != 0 ? 1 : 0);
        GFX_C3D_LOG("[trectbatch] init on=%d env=%p cfg=%d cfgloaded=%d\n", on, (const void*)env, cfg, loaded);
        return on;
    }();
    return sEnabled;
}

/* [texclr] investigation receipt sink for the vendored interpreter's TextureCacheClear
 * ([debug] texclrdiag=1): the pre-sort / post-sort head of free_texture_ids. */
extern "C" int gdx_texclr_diag_on(void) {
    static const int sOn = (&gdx3ds_config_get_int != nullptr)
                               ? gdx3ds_config_get_int("debug", "texclrdiag", 0)
                               : 0;
    return sOn;
}
extern "C" void gdx_texclr_dump(const char* line) {
    GFX_C3D_LOG("%s\n", line);
}

/* GEOMLOSS telemetry for the [c3d] line: packed begins refused (geometry dropped) and
 * packed begins that had to split the batch at the pool cap (geometry preserved). */
static unsigned long sVboPackRefused = 0;
static unsigned long sVboPackSplit = 0;
/* Draws dropped outright by the pool-exhaustion early-out in DrawTriangles — the frame-wide
 * geometry loss that every path (LUS legacy, machines, track, fx) funnels into. It had a
 * one-shot log and no counter at all, so a run that lost half a frame every frame looked
 * identical to a clean one on the receipt. */
static unsigned long sVboDrawDropped = 0;
static void* sReplayOuterCaller = nullptr; /* C6 [replay]: the packed/mesh/xf entry that led to DrawTriangles */
/* Gdx_NativeBatchCommit refusals: the cursor could not advance, so the caller's appended
 * vertices are outside the flushed range. Unreachable while the begin reservation holds,
 * checked because the failure is silent corruption rather than loss. */
static unsigned long sNbCommitRefused = 0;

/* [triloop] packed-draw telemetry for the [c3d] line (dPk). */
static unsigned long sPackedDrawCalls = 0;
static unsigned long sHsxLeanDraws = 0; /* [native] hudsky_emit: draws through DrawPackedLean */
static unsigned long sHsxRuns = 0;      /* [native] hudsky_emit: programs run by HsxRun */
/* [debug] hsx_gpuhash=1 (hudsky): fold the GPU command words every mono draw appends (state the
 * draw flushes + the draw packets) into a per-frame hash, one [hsxg] line per frame. Two legs
 * that differ only in a lever which claims "same draws, same state" must print the same lines.
 * Read once; default 0 (no cost beyond one branch per draw). */
static int sHsxGpuHash = -1;
static uint32_t sHsxGpuFrameHash = 2166136261u;
static uint32_t sHsxGpuFrameDraws = 0;
static uint32_t sHsxGpuWrap = 0;
static bool HsxGpuHashOn() {
    if (sHsxGpuHash < 0) {
        sHsxGpuHash = (&gdx3ds_config_get_int != nullptr && gdx3ds_config_get_int("debug", "hsx_gpuhash", 0) != 0) ? 1 : 0;
    }
    return sHsxGpuHash != 0;
}
static void HsxGpuHashFold(u32 w0, const float* verts, size_t nFloats) {
    if (gpuCmdBuf == nullptr || gpuCmdBufOffset < w0) {
        sHsxGpuWrap++; /* the buffer was split under the draw: not foldable */
        return;
    }
    uint32_t h = sHsxGpuFrameHash;
    for (u32 i = w0; i < gpuCmdBufOffset; i++) {
        h = (h ^ gpuCmdBuf[i]) * 16777619u;
    }
    /* the vertex floats the draw reads (the command words carry only their address) */
    const uint32_t* vw = reinterpret_cast<const uint32_t*>(verts);
    for (size_t i = 0; verts != nullptr && i < nFloats; i++) {
        h = (h ^ vw[i]) * 16777619u;
    }
    sHsxGpuFrameHash = h;
    sHsxGpuFrameDraws++;
}
/* [native] indexed mesh draws this session (dNat on the [c3d] line). */
static unsigned long sNativeDrawCalls = 0;
/* [native] machines_verify capture hook (gdx3ds_mesh_backend_set_capture). */
static Gdx3dsMeshCaptureFn sMeshCaptureFn = nullptr;
static void* sMeshCaptureUser = nullptr;
static bool sMeshCaptureSkip = false;
static unsigned long sPrevNativeDraws = 0;

/* ---- [nverify] per-batch record/compare (gdx3ds_verify.h) --------------------------------- */
#include "gdx3ds_verify.h"
/* The mode lives at file scope with external linkage so the interpreters read it inline; see
 * the gdx3ds_verify_mode block in gdx3ds_verify.h. 0 off, 1 record, 2 compare. */
extern "C" { int gdx3ds_verify_mode = 0; }
namespace {
std::vector<uint32_t> sVfyRing;         /* 4 words per record: kind, state, vbo, fieldsig */
std::vector<uint32_t> sVfyTexHash;      /* per texture id: FNV of the last upload payload */
/* Per texture id: the global upload serial at which that id was last filled, and the serial
 * itself. A field=3 mismatch is ambiguous between "the two sides decoded different bytes" and
 * "one side served a cache entry minted several frames ago"; the serial separates them. */
std::vector<uint32_t> sVfyTexSeq;
uint32_t sVfyUploadSeq = 0;
/* Per texture id: the first and last 4 bytes of the payload that filled it. An upload-hash
 * mismatch says the two decodes differ; these say where. */
std::vector<uint32_t> sVfyTexEnds;   /* 2 words per id: first, last */
std::vector<uint32_t> sVfyFields;       /* mode-1 per-record state field hashes (16 per record) */
size_t sVfyIdx = 0;
uint32_t sVfyFirst = 0xFFFFFFFFu, sVfyClass = 0, sVfyDetail = 0;
constexpr int kVfyFields = 16;
/* Per-record vertex-0 + tri count + shader ids (mode 1), and the compare side's copy at the
 * first mismatch, for the [nverify] detail line. */
struct VfyBatchInfo {
    float v0[12];
    uint32_t tris;
    uint64_t id0, id1;
    uint32_t texUsed[2], texId[2], texHash[2], texW[2], texH[2], texSeq[2], seqNow, texEnds[2], texEnds0[2];
    float vpf[5];
    int vis[4];
    Gdx3dsVerifyTri tri;
};
Gdx3dsVerifyTri sVfyLastTri = {};
std::vector<VfyBatchInfo> sVfyInfo;
VfyBatchInfo sVfyMismatchOwned = {};
VfyBatchInfo sVfyMismatchLus = {};
bool sVfyMismatchInfoValid = false;
/* Compare-side sequence (kind, id0 low32, tris) of the first records, for the sequence dump. */
constexpr int kVfySeq = 12;
uint32_t sVfySeqOwned[kVfySeq][3];
size_t sVfySeqOwnedN = 0;
inline uint32_t VfyFnv(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, b + i, 4);
        h = (h ^ w) * 16777619u;
    }
    for (; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}
/* command-stream differential */
std::vector<uint32_t> sVfyCmd;      /* mode 1: w0,w1 pairs */
size_t sVfyCmdIdx = 0;
uint32_t sVfyCmdFirst = 0xFFFFFFFFu;
uint32_t sVfyCmdLus[6] = {}, sVfyCmdOwned[6] = {};
uint32_t sVfyCmdPrevOwned[4] = {};
uint32_t sVfyKindState[4] = {};      /* first mismatch: lus kind/state, owned kind/state */
inline bool VfyCmdCounts(uint32_t w0) {
    const uint32_t op = w0 >> 24;
    return (op >= 0x05 && op <= 0x07) || (op >= 0xD7 && op <= 0xD9) || (op >= 0xE2 && op <= 0xEF && op != 0xE4 && op != 0xE5) ||
           (op >= 0xF0 && op <= 0xFC);
}

GDX_COLD void VfyRecord(uint32_t kind, uint32_t state, uint32_t vbo, const uint32_t* fields, const VfyBatchInfo* info = nullptr) {
    if (gdx3ds_verify_mode == 1) {
        sVfyInfo.push_back(info != nullptr ? *info : VfyBatchInfo{});
        sVfyRing.push_back(kind);
        sVfyRing.push_back(state);
        sVfyRing.push_back(vbo);
        sVfyRing.push_back(0);
        for (int i = 0; i < kVfyFields; i++) {
            sVfyFields.push_back(fields != nullptr ? fields[i] : 0);
        }
    } else if (gdx3ds_verify_mode == 2) {
        if (sVfySeqOwnedN < kVfySeq) {
            sVfySeqOwned[sVfySeqOwnedN][0] = kind;
            sVfySeqOwned[sVfySeqOwnedN][1] = info != nullptr ? static_cast<uint32_t>(info->id0) : state;
            sVfySeqOwned[sVfySeqOwnedN][2] = info != nullptr ? info->tris : 0;
            sVfySeqOwnedN++;
        }
        const size_t n = sVfyRing.size() / 4;
        uint32_t cls = 0;
        if (sVfyIdx >= n) {
            cls = 8;
        } else {
            const uint32_t* r = &sVfyRing[sVfyIdx * 4];
            if (r[0] != kind) cls |= 1;
            if (r[1] != state) cls |= 2;
            if (r[2] != vbo) cls |= 4;
            if ((cls & 2) != 0 && fields != nullptr && sVfyFirst == 0xFFFFFFFFu) {
                const uint32_t* f = &sVfyFields[sVfyIdx * kVfyFields];
                for (int i = 0; i < kVfyFields; i++) {
                    if (f[i] != fields[i]) {
                        sVfyDetail = static_cast<uint32_t>(i) + 1u;
                        break;
                    }
                }
            }
        }
        if (cls != 0 && sVfyFirst == 0xFFFFFFFFu) {
            sVfyFirst = static_cast<uint32_t>(sVfyIdx);
            sVfyClass = cls;
            sVfyKindState[0] = sVfyIdx < n ? sVfyRing[sVfyIdx * 4] : 0;
            sVfyKindState[1] = sVfyIdx < n ? sVfyRing[sVfyIdx * 4 + 1] : 0;
            sVfyKindState[2] = kind;
            sVfyKindState[3] = state;
            sVfyMismatchOwned = info != nullptr ? *info : VfyBatchInfo{};
            sVfyMismatchLus = sVfyIdx < sVfyInfo.size() ? sVfyInfo[sVfyIdx] : VfyBatchInfo{};
            sVfyMismatchInfoValid = true;
        }
        sVfyIdx++;
    }
}
} // namespace

extern "C" void gdx3ds_verify_begin(int mode) {
    gdx3ds_verify_mode = mode;
    if (mode == 1) {
        sVfyRing.clear();
        sVfyFields.clear();
        sVfyInfo.clear();
        sVfyCmd.clear();
        sVfyIdx = 0;
    }
    if (mode == 2) {
        sVfyIdx = 0;
        sVfyCmdIdx = 0;
        sVfyCmdFirst = 0xFFFFFFFFu;
        memset(sVfyCmdPrevOwned, 0, sizeof(sVfyCmdPrevOwned));
        memset(sVfyCmdOwned, 0, sizeof(sVfyCmdOwned));
        sVfySeqOwnedN = 0;
        sVfyFirst = 0xFFFFFFFFu;
        sVfyClass = 0;
        sVfyDetail = 0;
        sVfyMismatchInfoValid = false;
    }
    /* mode 0 leaves the counters for gdx3ds_verify_result */
}

extern "C" GDX_COLD void gdx3ds_verify_note_tri(const Gdx3dsVerifyTri* t) {
    sVfyLastTri = *t;
}

extern "C" unsigned gdx_gfx_classify_host_ptr(unsigned long p) __attribute__((weak));
extern "C" int gdx_gfx_rawcopy_origin(unsigned long p, unsigned long* s, unsigned* n) __attribute__((weak));
/* [nverify] the TMEM identity covers the load extent up to kVfyTmemHashCap rather than a
 * 64-byte prefix: a prefix agrees on every texture whose first rows are static and hides a real
 * content divergence further in. TMEM is a fixed 4 KiB mirror, so the span is always in bounds.
 * The SOURCE hash stays at 64 bytes on purpose -- a loaded record's size_bytes is the requested
 * extent, not the readable one. */
static constexpr unsigned kVfySrcHashCap = 4096u;
/* These run on every TRIANGLE while verify is armed, not once per import: hashing the whole
 * 4 KiB TMEM span here stalled the render thread hard enough that the guest stopped advancing
 * at frame ~150 on every renderer. 256 bytes is four RGBA16 rows -- enough to separate two
 * decodes of the same source, cheap enough to keep the frame moving. */
static constexpr unsigned kVfyTmemHashCap = 4096u; /* TEMP diag: full span */
/* The first 8 bytes verbatim: a hash says the two sides differ, these say how. */
static inline void VfyBytes8(unsigned* out, const void* p, unsigned n) {
    out[0] = out[1] = 0u;
    if (p == nullptr) return;
    const unsigned take = n < 8u ? n : 8u;
    unsigned char b[8] = {};
    memcpy(b, p, take);
    memcpy(out, b, 8);
}
extern "C" GDX_COLD void gdx3ds_verify_tri_source(Gdx3dsVerifyTri* t, const void* addr, unsigned sizeBytes, const void* tmem,
                                         unsigned tmemBytes) {
    t->addr0 = reinterpret_cast<unsigned long>(addr);
    t->cls0 = (addr && gdx_gfx_classify_host_ptr) ? gdx_gfx_classify_host_ptr(t->addr0) : 0u;
    t->srcHash0 = addr ? VfyFnv(2166136261u, addr, sizeBytes < 64u ? sizeBytes : 64u) : 0u;
    t->tmemHash0 = tmem ? VfyFnv(2166136261u, tmem, tmemBytes < kVfyTmemHashCap ? tmemBytes : kVfyTmemHashCap) : 0u;
    VfyBytes8(t->srcB0, addr, sizeBytes);
    VfyBytes8(t->tmemB0, tmem, tmemBytes);
    t->copyOrigin0 = 0; t->copyRemain0 = 0;
    if (addr && gdx_gfx_rawcopy_origin) gdx_gfx_rawcopy_origin(t->addr0, &t->copyOrigin0, &t->copyRemain0);
    VfyBytes8(t->srcB0 + 2, addr && sizeBytes >= 8u ? static_cast<const uint8_t*>(addr) + (sizeBytes - 8u) : nullptr, 8u);
    VfyBytes8(t->tmemB0 + 2, tmem && tmemBytes >= 8u ? static_cast<const uint8_t*>(tmem) + (tmemBytes - 8u) : nullptr, 8u);
}

extern "C" GDX_COLD void gdx3ds_verify_tri_source1(Gdx3dsVerifyTri* t, const void* addr, unsigned sizeBytes, const void* tmem,
                                          unsigned tmemBytes) {
    t->addr1 = reinterpret_cast<unsigned long>(addr);
    t->cls1 = (addr && gdx_gfx_classify_host_ptr) ? gdx_gfx_classify_host_ptr(t->addr1) : 0u;
    t->srcHash1 = addr ? VfyFnv(2166136261u, addr, sizeBytes < 64u ? sizeBytes : 64u) : 0u;
    t->tmemHash1 = tmem ? VfyFnv(2166136261u, tmem, tmemBytes < kVfyTmemHashCap ? tmemBytes : kVfyTmemHashCap) : 0u;
}

extern "C" GDX_COLD void gdx3ds_verify_note_cmd(unsigned w0, unsigned w1) {
    if (!VfyCmdCounts(w0)) return;
    if (gdx3ds_verify_mode == 1) {
        sVfyCmd.push_back(w0);
        sVfyCmd.push_back(w1);
    } else if (gdx3ds_verify_mode == 2) {
        const size_t n = sVfyCmd.size() / 2;
        if (sVfyCmdFirst == 0xFFFFFFFFu) {
            const bool same = sVfyCmdIdx < n && sVfyCmd[sVfyCmdIdx * 2] == w0 && sVfyCmd[sVfyCmdIdx * 2 + 1] == w1;
            if (!same) {
                sVfyCmdFirst = static_cast<uint32_t>(sVfyCmdIdx);
                for (int k = 0; k < 3; k++) {
                    const size_t i = sVfyCmdIdx + k;
                    sVfyCmdLus[k * 2] = i < n ? sVfyCmd[i * 2] : 0;
                    sVfyCmdLus[k * 2 + 1] = i < n ? sVfyCmd[i * 2 + 1] : 0;
                }
                sVfyCmdOwned[0] = w0;
                sVfyCmdOwned[1] = w1;
            } else {
                sVfyCmdPrevOwned[2] = sVfyCmdPrevOwned[0];
                sVfyCmdPrevOwned[3] = sVfyCmdPrevOwned[1];
                sVfyCmdPrevOwned[0] = w0;
                sVfyCmdPrevOwned[1] = w1;
            }
        } else if (sVfyCmdIdx == static_cast<size_t>(sVfyCmdFirst) + 1) {
            sVfyCmdOwned[2] = w0;
            sVfyCmdOwned[3] = w1;
        } else if (sVfyCmdIdx == static_cast<size_t>(sVfyCmdFirst) + 2) {
            sVfyCmdOwned[4] = w0;
            sVfyCmdOwned[5] = w1;
        }
        sVfyCmdIdx++;
    }
}

extern "C" GDX_COLD void gdx3ds_verify_cmd_detail(char* buf, unsigned bufSize) {
    if (bufSize == 0) return;
    const size_t n = sVfyCmd.size() / 2;
    snprintf(buf, bufSize,
             "[nverify]   cmds lus=%u owned=%u first_diff=%u | prev owned %08X/%08X %08X/%08X | lus %08X/%08X %08X/%08X "
             "%08X/%08X | owned %08X/%08X %08X/%08X %08X/%08X | first_batch kind/state lus=%u/%08X owned=%u/%08X",
             (unsigned)n, (unsigned)sVfyCmdIdx, sVfyCmdFirst, sVfyCmdPrevOwned[2], sVfyCmdPrevOwned[3],
             sVfyCmdPrevOwned[0], sVfyCmdPrevOwned[1], sVfyCmdLus[0], sVfyCmdLus[1], sVfyCmdLus[2], sVfyCmdLus[3],
             sVfyCmdLus[4], sVfyCmdLus[5], sVfyCmdOwned[0], sVfyCmdOwned[1], sVfyCmdOwned[2], sVfyCmdOwned[3],
             sVfyCmdOwned[4], sVfyCmdOwned[5], sVfyKindState[0], sVfyKindState[1], sVfyKindState[2], sVfyKindState[3]);
}
extern "C" int gdx3ds_verify_active(void) {
    return gdx3ds_verify_mode != 0;
}

extern "C" GDX_COLD void gdx3ds_verify_tri_detail(char* buf, unsigned bufSize) {
    if (!sVfyMismatchInfoValid || bufSize == 0) {
        if (bufSize) buf[0] = 0;
        return;
    }
    const Gdx3dsVerifyTri& a = sVfyMismatchLus.tri;
    const Gdx3dsVerifyTri& b = sVfyMismatchOwned.tri;
    snprintf(buf, bufSize,
             "[nverify]   tri lus: t=%u/%u ft=%u rect=%u win=%.0f,%.0f..%.0f,%.0f tex=%ux%u t2=%ux%u tex1=%ux%u tm=%u "
             "uv=%.0f,%.0f sh=%d/%d ld=%u/%u/%u/%u sc=%X/%X | owned: t=%u/%u ft=%u rect=%u win=%.0f,%.0f..%.0f,%.0f "
             "tex=%ux%u t2=%ux%u tex1=%ux%u tm=%u uv=%.0f,%.0f sh=%d/%d ld=%u/%u/%u/%u sc=%X/%X | "
             "om lus=%08X/%08X geo=%08X var=%u vp=%d,%d %ux%u walk=%u/%u bz=%u noop=%u | owned=%08X/%08X geo=%08X "
             "var=%u vp=%d,%d %ux%u walk=%u/%u bz=%u noop=%u",
             a.tile0, a.tile1, a.firstTile, a.isRect, a.uls0, a.ult0, a.lrs0, a.lrt0, a.texW0, a.texH0, a.texW20, a.texH20,
             a.texW1, a.texH1, a.tm, a.vu, a.vv, a.shifts0, a.shiftt0, a.line0, a.size0, a.full0, a.orig0, a.scaleS,
             a.scaleT, b.tile0, b.tile1, b.firstTile, b.isRect, b.uls0, b.ult0, b.lrs0, b.lrt0, b.texW0, b.texH0,
             b.texW20, b.texH20, b.texW1, b.texH1, b.tm, b.vu, b.vv, b.shifts0, b.shiftt0, b.line0, b.size0, b.full0,
             b.orig0, b.scaleS, b.scaleT, a.omh, a.oml, a.geo, a.variant, a.vpx, a.vpy, a.vpw, a.vph, a.lists, a.cmds,
             a.branchZ, a.noop, b.omh, b.oml, b.geo, b.variant, b.vpx, b.vpy, b.vpw, b.vph, b.lists, b.cmds, b.branchZ,
             b.noop);
}

extern "C" GDX_COLD void gdx3ds_verify_src_detail(char* buf, unsigned bufSize) {
    if (!sVfyMismatchInfoValid || bufSize == 0) {
        if (bufSize) buf[0] = 0;
        return;
    }
    const VfyBatchInfo& ia = sVfyMismatchLus;
    const VfyBatchInfo& ib = sVfyMismatchOwned;
    const Gdx3dsVerifyTri& a = ia.tri;
    const Gdx3dsVerifyTri& b = ib.tri;
    snprintf(buf, bufSize,
             "[nverify]   src lus=%08lX cls=%X h=%08X tm=%08X bA=%u | owned=%08lX cls=%X h=%08X tm=%08X bA=%u | "
             "src1 lus=%08lX cls=%X h=%08X tm=%08X @%u ld=%u/%u/%u/%u | owned=%08lX cls=%X h=%08X tm=%08X @%u ld=%u/%u/%u/%u | "
             "vp lus=%.0f %.4f %.4f %.4f %.4f vis=%d,%d %dx%d | owned=%.0f %.4f %.4f %.4f %.4f vis=%d,%d %dx%d | "
             "span0 lus u=%u/%u @%u base=%u %08lX/%u/%u | owned u=%u/%u @%u base=%u %08lX/%u/%u | "
             "fmtsiz lus=%08X pal=%08X | owned=%08X pal=%08X",
             a.addr0, a.cls0, a.srcHash0, a.tmemHash0, a.blendA, b.addr0, b.cls0, b.srcHash0, b.tmemHash0, b.blendA,
             a.addr1, a.cls1, a.srcHash1, a.tmemHash1, a.tmem1, a.line1, a.size1, a.full1, a.orig1,
             b.addr1, b.cls1, b.srcHash1, b.tmemHash1, b.tmem1, b.line1, b.size1, b.full1, b.orig1,
             ia.vpf[0], ia.vpf[1], ia.vpf[2], ia.vpf[3], ia.vpf[4], ia.vis[0], ia.vis[1], ia.vis[2], ia.vis[3],
             ib.vpf[0], ib.vpf[1], ib.vpf[2], ib.vpf[3], ib.vpf[4], ib.vis[0], ib.vis[1], ib.vis[2], ib.vis[3],
             a.used0, a.used1, a.tmem0, a.base0, a.baseAddr0, a.baseSize0, a.baseOrig0,
             b.used0, b.used1, b.tmem0, b.base0, b.baseAddr0, b.baseSize0, b.baseOrig0,
             a.tc1, a.tc0, b.tc1, b.tc0);
}

/* The first 8 bytes of each side's unit-0 source and TMEM window, verbatim. A hash says the two
 * sides differ; these say HOW: a byte-swapped pair is a byte-order difference (the bridge hands
 * LUS a persistent copy that is 16-bit-swapped for a native-RGBA16 source), anything else is
 * different content. Its own line because the src line is already at its buffer. */
extern "C" GDX_COLD void gdx3ds_verify_bytes_detail(char* buf, unsigned bufSize) {
    if (!sVfyMismatchInfoValid || bufSize == 0) {
        if (bufSize) buf[0] = 0;
        return;
    }
    const VfyBatchInfo& ia = sVfyMismatchLus;
    const VfyBatchInfo& ib = sVfyMismatchOwned;
    const Gdx3dsVerifyTri& a = ia.tri;
    const Gdx3dsVerifyTri& b = ib.tri;
    snprintf(buf, bufSize,
             "[nverify]   b8 lus src=%08X%08X..%08X%08X tmem=%08X%08X..%08X%08X | "
             "owned src=%08X%08X..%08X%08X tmem=%08X%08X..%08X%08X | "
             "copy lus=%08lX/%u owned=%08lX/%u | upl lus=%08X..%08X owned=%08X..%08X | rtline lus=%u owned=%u",
             a.srcB0[0], a.srcB0[1], a.srcB0[2], a.srcB0[3], a.tmemB0[0], a.tmemB0[1], a.tmemB0[2], a.tmemB0[3],
             b.srcB0[0], b.srcB0[1], b.srcB0[2], b.srcB0[3], b.tmemB0[0], b.tmemB0[1], b.tmemB0[2], b.tmemB0[3],
             a.copyOrigin0, a.copyRemain0, b.copyOrigin0, b.copyRemain0, ia.texEnds0[0], ia.texEnds[0],
             ib.texEnds0[0], ib.texEnds[0], a.rtLine0, b.rtLine0);
}

extern "C" GDX_COLD void gdx3ds_verify_seq_detail(char* buf, unsigned bufSize) {
    if (bufSize == 0) return;
    buf[0] = 0;
    unsigned off = 0;
    const int nl = static_cast<int>(sVfyRing.size() / 4) < kVfySeq ? static_cast<int>(sVfyRing.size() / 4) : kVfySeq;
    off += static_cast<unsigned>(snprintf(buf + off, bufSize - off, "[nverify]   seq lus:"));
    for (int i = 0; i < nl && off < bufSize; i++) {
        const uint32_t k = sVfyRing[i * 4];
        const uint32_t id = k == 1 ? static_cast<uint32_t>(sVfyInfo[i].id0) : sVfyRing[i * 4 + 1];
        off += static_cast<unsigned>(snprintf(buf + off, bufSize - off, " %u:%08X/%u", k, id, k == 1 ? sVfyInfo[i].tris : 0u));
    }
    off += static_cast<unsigned>(snprintf(buf + off, bufSize - off, " | owned:"));
    for (size_t i = 0; i < sVfySeqOwnedN && off < bufSize; i++) {
        off += static_cast<unsigned>(snprintf(buf + off, bufSize - off, " %u:%08X/%u", sVfySeqOwned[i][0], sVfySeqOwned[i][1], sVfySeqOwned[i][2]));
    }
}

extern "C" GDX_COLD void gdx3ds_verify_first_detail(char* buf, unsigned bufSize) {
    if (!sVfyMismatchInfoValid || bufSize == 0) {
        if (bufSize) buf[0] = 0;
        return;
    }
    const VfyBatchInfo& a = sVfyMismatchLus;
    const VfyBatchInfo& b = sVfyMismatchOwned;
    snprintf(buf, bufSize,
             "[nverify]   lus tris=%u id=%08X%08X/%08X%08X v0=%.4f %.4f %.4f %.4f uv=%.4f %.4f c=%.3f %.3f %.3f %.3f | "
             "owned tris=%u id=%08X%08X/%08X%08X v0=%.4f %.4f %.4f %.4f uv=%.4f %.4f c=%.3f %.3f %.3f %.3f | "
             "tex lus u0=%u/%u/%08X/%ux%u@%u u1=%u/%u/%08X/%ux%u owned u0=%u/%u/%08X/%ux%u@%u u1=%u/%u/%08X/%ux%u "
             "| upl lus=%u owned=%u",
             a.tris, (unsigned)(a.id0 >> 32), (unsigned)a.id0, (unsigned)(a.id1 >> 32), (unsigned)a.id1, a.v0[0], a.v0[1],
             a.v0[2], a.v0[3], a.v0[4], a.v0[5], a.v0[6], a.v0[7], a.v0[8], a.v0[9], b.tris, (unsigned)(b.id0 >> 32),
             (unsigned)b.id0, (unsigned)(b.id1 >> 32), (unsigned)b.id1, b.v0[0], b.v0[1], b.v0[2], b.v0[3], b.v0[4],
             b.v0[5], b.v0[6], b.v0[7], b.v0[8], b.v0[9], a.texUsed[0], a.texId[0], a.texHash[0], a.texW[0], a.texH[0],
             a.texSeq[0], a.texUsed[1], a.texId[1], a.texHash[1], a.texW[1], a.texH[1], b.texUsed[0], b.texId[0],
             b.texHash[0], b.texW[0], b.texH[0], b.texSeq[0], b.texUsed[1], b.texId[1], b.texHash[1], b.texW[1],
             b.texH[1], a.seqNow, b.seqNow);
}

extern "C" void gdx3ds_verify_result(unsigned* recorded, unsigned* compared, unsigned* first, unsigned* cls,
                                     unsigned* detail) {
    const unsigned n = static_cast<unsigned>(sVfyRing.size() / 4);
    *recorded = n;
    *compared = static_cast<unsigned>(sVfyIdx);
    if (sVfyFirst == 0xFFFFFFFFu && sVfyIdx < n) {
        sVfyFirst = static_cast<uint32_t>(sVfyIdx);
        sVfyClass = 16;
    }
    *first = sVfyFirst;
    *cls = sVfyClass;
    *detail = sVfyDetail;
}

/* [trectbatch] atlas page-eviction telemetry for the [c3d] line (atlasEv=), see AtlasTryPlace. */
static unsigned long sAtlasEvictions = 0;     /* pages reclaimed */
static unsigned long sAtlasEvictedViews = 0;  /* views copied out to standalone */
static unsigned long sAtlasEvictCopyFail = 0; /* copy-outs that failed C3D_TexInit (view dropped) */
/* DROPSITES site 4. texUpFail= lumped three unrelated causes into one number; split them so a
 * non-zero value says WHICH refusal happened. All three are drops with no recovery pre-fix:
 * the id keeps whatever it held, and the draw that follows does not rebind the PICA texunit
 * at all (DrawTriangles' bind-miss branch), so it samples an unrelated texture. */
static unsigned long sTexUpFailId = 0;    /* invalid texture id / zero extent */
static unsigned long sTexUpFailDim = 0;   /* padded extent above the PICA 1024 limit */
static unsigned long sTexUpFailInit = 0;  /* C3D_TexInit refused (linear heap) */
static unsigned long sTexInitReclaimed = 0; /* TexInit retries that SUCCEEDED after a reclaim */
static unsigned long sTexMissingBinds = 0;  /* draws that bound the fallback instead of nothing */
/* [debug] texinitfail=N: fail every Nth C3D_TexInit in UploadTexture so the recovery path is
 * reachable in a normal build. 0 = shipped behaviour (geomloss's vbopoolkib, for this site). */
static unsigned long sTexInitFailEvery = 0;
static unsigned long sTexInitCalls = 0;
/* [anchor] stereo-anchored draw telemetry for the [c3d] line (anchor=n/dmin/dmax per window). */
static unsigned long sAnchorDraws = 0;
static float sAnchorDMin = 2.0f;
static float sAnchorDMax = -1.0f;
static unsigned long sAtlasEvictNone = 0;     /* no safe candidate this frame (standalone fallback) */
constexpr uint32_t kClearDepthFar = 0; // raw D24S8: far plane under the reversed map

constexpr uint32_t kDisplayTransferFlags =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

uint32_t NextPow2(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

/* Morton (Z-order) offset of (x, y) inside an 8x8 PICA tile. */
uint32_t MortonInterleave(uint32_t x, uint32_t y) {
    static const uint32_t xlut[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    static const uint32_t ylut[8] = { 0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a };
    return xlut[x & 7] | ylut[y & 7];
}

/* [padgutter] Replicate the content edge into the 1-texel pow2 padding of a swizzled RGBA8
 * PICA texture, the way the [trectbatch] atlas has always gutted its cells (kAtlasGutter).
 * N64 G_TX_CLAMP returns the EDGE TEXEL past the tile window; PICA's GPU_CLAMP_TO_EDGE
 * clamps at the PADDED edge, and padding written as transparent black makes GPU_LINEAR fade
 * every clamped content edge to black instead. One texel is all that is reachable: an
 * atlas-eligible rect's UV window sits inside the tile window, so the packed texel
 * coordinate is at most extent + 0.5 (port/tests/gfx_atlas_uv_tests.cpp).
 * CLAMPED AXES ONLY — a WRAP/MIRROR axis folds at the PADDED period on this hardware
 * (docs/research/shadow2-mirror-period-audit.md), so its pad texels belong to the wrap
 * image, not to an edge clamp. A non-clamped axis's gutter is ZEROED rather than left
 * alone: a recycled texture id keeps its C3D_Tex when the padded extent is unchanged and
 * would otherwise serve the previous tenant's gutter. Caller flushes. */
unsigned long sPadGutterPadded = 0;   /* uploads with padding on at least one axis */
unsigned long sPadGutterReplicated = 0; /* of those, axes that got a REPLICATED gutter */
unsigned long sPadGutterZeroed = 0;     /* of those, axes that got a ZEROED gutter */
void WritePadGutter(uint32_t* dst, uint32_t w, uint32_t h, uint32_t padW, uint32_t padH, bool clampU,
                    bool clampV) {
    const uint32_t tilesPerRow = padW >> 3;
    const auto at = [dst, tilesPerRow](uint32_t x, uint32_t dy) -> uint32_t& {
        return dst[(((dy >> 3) * tilesPerRow + (x >> 3)) << 6) | MortonInterleave(x, dy)];
    };
    if (w < padW || h < padH) {
        /* [padgutter] receipt: how often padding exists at all, and how many of those axes
         * the N64 tile actually clamps. Without this the fix is unfalsifiable — a run where
         * every padded axis is a WRAP axis is indistinguishable from the code never running. */
        sPadGutterPadded++;
        if (w < padW) {
            (clampU ? sPadGutterReplicated : sPadGutterZeroed)++;
        }
        if (h < padH) {
            (clampV ? sPadGutterReplicated : sPadGutterZeroed)++;
        }
        if (sPadGutterPadded <= 8 || (sPadGutterPadded & 1023) == 0) {
            GFX_C3D_LOG("[padgutter] n=%lu %ux%u -> %ux%u clampU=%d clampV=%d rep=%lu zero=%lu\n",
                        sPadGutterPadded, (unsigned)w, (unsigned)h, (unsigned)padW, (unsigned)padH,
                        (int)clampU, (int)clampV, sPadGutterReplicated, sPadGutterZeroed);
        }
    }
    if (w < padW) {
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t dy = padH - 1 - y;
            at(w, dy) = clampU ? at(w - 1, dy) : 0u;
        }
    }
    if (h < padH) {
        const uint32_t dySrc = padH - h;     /* N64 row h-1 */
        const uint32_t dyDst = padH - 1 - h; /* N64 row h   */
        for (uint32_t x = 0; x < w; x++) {
            at(x, dyDst) = clampV ? at(x, dySrc) : 0u;
        }
        if (w < padW) {
            at(w, dyDst) = (clampU && clampV) ? at(w - 1, dySrc) : 0u;
        }
    }
}

uint32_t PackTexEnvColor(float r, float g, float b, float a) {
    const uint32_t ri = (uint32_t)(r * 255.0f + 0.5f) & 0xFF;
    const uint32_t gi = (uint32_t)(g * 255.0f + 0.5f) & 0xFF;
    const uint32_t bi = (uint32_t)(b * 255.0f + 0.5f) & 0xFF;
    const uint32_t ai = (uint32_t)(a * 255.0f + 0.5f) & 0xFF;
    return ri | (gi << 8) | (bi << 16) | (ai << 24);
}

GPU_TEXTURE_WRAP_PARAM WrapFromN64(uint32_t cm) {
    if (cm & G_TX_CLAMP) {
        return GPU_CLAMP_TO_EDGE;
    } else if (cm & G_TX_MIRROR) {
        return GPU_MIRRORED_REPEAT;
    }
    return GPU_REPEAT;
}

bool ClassifyFormula(const int c[4], GPU_COMBINEFUNC& func, int vals[3], int& numVals, const char** why) {
    const int a = c[0], b = c[1], m = c[2], d = c[3];
    // gfx_cc_get_features/GenerateCC normalize degenerate products to a=b=m=SHADER_0.
    if (a == SHADER_0 && b == SHADER_0 && m == SHADER_0) {
        func = GPU_REPLACE;
        vals[0] = d;
        numVals = 1;
        return true;
    } else if (b == SHADER_0 && d == SHADER_0) {
        func = GPU_MODULATE; // a * m
        vals[0] = a;
        vals[1] = m;
        numVals = 2;
        return true;
    } else if (d == b) {
        func = GPU_INTERPOLATE; // b + (a-b)*m == a*m + b*(1-m)
        vals[0] = a;
        vals[1] = b;
        vals[2] = m;
        numVals = 3;
        return true;
    } else if (b == SHADER_0) {
        func = GPU_MULTIPLY_ADD; // a*m + d
        vals[0] = a;
        vals[1] = m;
        vals[2] = d;
        numVals = 3;
        return true;
    }
    *why = "(a-b)*c+d with b!=0 and d!=b needs multi-stage subtract";
    return false;
}

bool IsShaderInput(int val) {
    return val >= SHADER_INPUT_1 && val <= SHADER_INPUT_7;
}

/* Incremental build state for the multi-stage TexEnv chain. prevRgb/prevAlpha
 * track, symbolically, which SHADER_* value GPU_PREVIOUS holds entering the next
 * stage (kValNone before stage 0; SHADER_COMBINED once a full cycle committed). */
constexpr int kValNone = -1;

struct CombinerBuild {
    int vtxRgb = 0;   // SHADER_INPUT_n riding GPU_PRIMARY_COLOR rgb (pre-assigned)
    int vtxAlpha = 0; // SHADER_INPUT_n riding GPU_PRIMARY_COLOR alpha
    int numStages = 0;
    TexEnvStageC3D stages[kMaxTexEnvStages];
    int prevRgb = kValNone;
    int prevAlpha = kValNone;
};

/* Pre-assign the single per-vertex colour attribute. Later cycles win: the
 * census's five machine 2-cycle modes all multiply cycle 2 by SHADE (per-vertex,
 * must ride the attribute) while cycle 1 mixes PRIM/ENV (draw-constant, safe on
 * per-stage TexEnv constant registers refreshed from vertex 0). */
void ChooseVtxInputs(const CCFeatures& cc, bool twoCycle, int& vtxRgb, int& vtxAlpha) {
    for (int cyc = twoCycle ? 1 : 0; cyc >= 0; cyc--) {
        for (int k = 0; k < 4; k++) {
            const int vr = cc.c[cyc][0][k];
            if (vtxRgb == 0 && IsShaderInput(vr)) {
                vtxRgb = vr;
            }
            const int va = cc.c[cyc][1][k];
            if (vtxAlpha == 0 && IsShaderInput(va)) {
                vtxAlpha = va;
            }
        }
        if (vtxRgb != 0 && vtxAlpha != 0) {
            return;
        }
    }
}

/* Resolve one rgb operand into a stage. Returns false with spillVal set when the
 * value needs the stage constant but the rgb slot is already claimed (the caller
 * may materialize it through a REPLACE prefix stage); returns false with *why set
 * on a hard failure. */
bool ResolveRgb(int val, CombinerBuild& b, TexEnvStageC3D& st, GPU_TEVSRC& src, GPU_TEVOP_RGB& op, int& spillVal,
                const char** why) {
    op = GPU_TEVOP_RGB_SRC_COLOR;
    switch (val) {
        case SHADER_0:
        case SHADER_1: {
            const int fixed = val == SHADER_1 ? 1 : 0;
            if (st.constRgbInput == 0 && (st.constRgbFixed == -1 || st.constRgbFixed == fixed)) {
                st.constRgbFixed = fixed;
                src = GPU_CONSTANT;
                return true;
            }
            spillVal = val;
            return false;
        }
        case SHADER_TEXEL0:
            src = GPU_TEXTURE0;
            return true;
        case SHADER_TEXEL0A:
            src = GPU_TEXTURE0;
            op = GPU_TEVOP_RGB_SRC_ALPHA;
            return true;
        case SHADER_TEXEL1:
            src = GPU_TEXTURE1;
            return true;
        case SHADER_TEXEL1A:
            src = GPU_TEXTURE1;
            op = GPU_TEVOP_RGB_SRC_ALPHA;
            return true;
        case SHADER_COMBINED:
            if (b.prevRgb == SHADER_COMBINED) {
                src = GPU_PREVIOUS;
                return true;
            }
            *why = "COMBINED referenced before cycle 1 produced it";
            return false;
        case SHADER_NOISE:
            *why = "noise";
            return false;
        default:
            if (IsShaderInput(val)) {
                if (val == b.vtxRgb) {
                    src = GPU_PRIMARY_COLOR;
                    return true;
                } else if (val == b.prevRgb) {
                    src = GPU_PREVIOUS;
                    return true;
                } else if (st.constRgbFixed == -1 && (st.constRgbInput == 0 || st.constRgbInput == val)) {
                    st.constRgbInput = val;
                    src = GPU_CONSTANT;
                    return true;
                }
                spillVal = val;
                return false;
            }
            *why = "unknown rgb mux value";
            return false;
    }
}

bool ResolveAlpha(int val, CombinerBuild& b, TexEnvStageC3D& st, GPU_TEVSRC& src, int& spillVal, const char** why) {
    switch (val) {
        case SHADER_0:
        case SHADER_1: {
            const int fixed = val == SHADER_1 ? 1 : 0;
            if (st.constAlphaInput == 0 && (st.constAlphaFixed == -1 || st.constAlphaFixed == fixed)) {
                st.constAlphaFixed = fixed;
                src = GPU_CONSTANT;
                return true;
            }
            spillVal = val;
            return false;
        }
        case SHADER_TEXEL0:
        case SHADER_TEXEL0A:
            src = GPU_TEXTURE0;
            return true;
        case SHADER_TEXEL1:
        case SHADER_TEXEL1A:
            src = GPU_TEXTURE1;
            return true;
        case SHADER_COMBINED:
            if (b.prevAlpha == SHADER_COMBINED) {
                src = GPU_PREVIOUS;
                return true;
            }
            *why = "COMBINED referenced before cycle 1 produced it";
            return false;
        case SHADER_NOISE:
            *why = "noise";
            return false;
        default:
            if (IsShaderInput(val)) {
                if (val == b.vtxAlpha) {
                    src = GPU_PRIMARY_COLOR;
                    return true;
                } else if (val == b.prevAlpha) {
                    src = GPU_PREVIOUS;
                    return true;
                } else if (st.constAlphaFixed == -1 && (st.constAlphaInput == 0 || st.constAlphaInput == val)) {
                    st.constAlphaInput = val;
                    src = GPU_CONSTANT;
                    return true;
                }
                spillVal = val;
                return false;
            }
            *why = "unknown alpha mux value";
            return false;
    }
}

/* Map one combiner cycle (rgb + alpha formulas) into 1-2 TexEnv stages. When a
 * channel needs two distinct constant-register values in one stage (census #16:
 * INTERPOLATE(PRIM, ENV, TEXEL0) with SHADE on the vertex attribute), the first
 * is materialized by a REPLACE prefix stage and consumed as GPU_PREVIOUS. A
 * spill is illegal once PREVIOUS carries the cycle-1 result. */
bool MapCycle(const int rgbC[4], const int alphaC[4], bool alphaOpaque, CombinerBuild& b, const char** why) {
    for (int attempt = 0; attempt < 2; attempt++) {
        CombinerBuild saved = b;
        TexEnvStageC3D st;
        int spillVal = kValNone;
        bool ok = true;

        GPU_COMBINEFUNC func = GPU_REPLACE;
        int vals[3] = { 0, 0, 0 };
        int numVals = 0;
        if (!ClassifyFormula(rgbC, func, vals, numVals, why)) {
            return false;
        }
        st.rgbFunc = func;
        for (int i = 0; i < numVals && ok; i++) {
            ok = ResolveRgb(vals[i], b, st, st.rgbSrc[i], st.rgbOp[i], spillVal, why);
        }

        if (ok) {
            if (alphaOpaque) {
                st.alphaFunc = GPU_REPLACE;
                ok = ResolveAlpha(SHADER_1, b, st, st.alphaSrc[0], spillVal, why);
            } else if (ClassifyFormula(alphaC, func, vals, numVals, why)) {
                st.alphaFunc = func;
                for (int i = 0; i < numVals && ok; i++) {
                    ok = ResolveAlpha(vals[i], b, st, st.alphaSrc[i], spillVal, why);
                }
            } else {
                return false;
            }
        }

        if (ok) {
            if (b.numStages >= kMaxTexEnvStages) {
                *why = "TexEnv stage budget exceeded";
                return false;
            }
            b.stages[b.numStages++] = st;
            b.prevRgb = SHADER_COMBINED;
            b.prevAlpha = SHADER_COMBINED;
            return true;
        }
        if (spillVal == kValNone || attempt == 1) {
            if (spillVal != kValNone) {
                *why = "combiner needs more than one constant spill stage";
            }
            return false;
        }
        if (b.prevRgb == SHADER_COMBINED || b.prevAlpha == SHADER_COMBINED) {
            *why = "constant spill would clobber the cycle-1 result";
            return false;
        }

        // Materialize spillVal via a REPLACE prefix stage on both channels, then retry.
        b = saved;
        TexEnvStageC3D sp;
        sp.rgbFunc = GPU_REPLACE;
        sp.alphaFunc = GPU_REPLACE;
        int dummySpill = kValNone;
        if (!ResolveRgb(spillVal, b, sp, sp.rgbSrc[0], sp.rgbOp[0], dummySpill, why) ||
            !ResolveAlpha(spillVal, b, sp, sp.alphaSrc[0], dummySpill, why)) {
            return false;
        }
        if (b.numStages >= kMaxTexEnvStages) {
            *why = "TexEnv stage budget exceeded";
            return false;
        }
        b.stages[b.numStages++] = sp;
        b.prevRgb = spillVal;
        b.prevAlpha = spillVal;
    }
    return false; // unreachable
}

bool IsCycle1Passthrough(const CCFeatures& cc) {
    const int* rgb = cc.c[1][0];
    const int* alpha = cc.c[1][1];
    const bool rgbPass = rgb[0] == SHADER_0 && rgb[1] == SHADER_0 && rgb[2] == SHADER_0 &&
                         (rgb[3] == SHADER_COMBINED || rgb[3] == SHADER_0);
    const bool alphaPass = alpha[0] == SHADER_0 && alpha[1] == SHADER_0 && alpha[2] == SHADER_0 &&
                           (alpha[3] == SHADER_COMBINED || alpha[3] == SHADER_0);
    return rgbPass && alphaPass;
}

/* Dual of IsCycle1Passthrough: when cycle 1 never reads COMBINED on either channel,
 * GenerateCC has already zeroed cycle 0 (its result is dead), so mapping it would
 * only burn a stage AND poison prevRgb/prevAlpha with SHADER_COMBINED — which
 * forbids the constant-spill prefix stage cycle 1 may need (F-Zero X machine
 * debris: 2-cycle (1-SHADE)*SHADE_ALPHA+SHADE in cycle 1 only). Skip it. */
bool Cycle1ReadsCombined(const CCFeatures& cc) {
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < 4; k++) {
            if (cc.c[1][ch][k] == SHADER_COMBINED) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

/* ------------------------------------------------------------------------------- */
/* Identity / capabilities                                                          */
/* ------------------------------------------------------------------------------- */

const char* GfxRenderingAPIC3D::GetName() {
    return "citro3d";
}

int GfxRenderingAPIC3D::GetMaxTextureSize() {
    return 1024; // PICA200 maximum
}

GfxClipParameters GfxRenderingAPIC3D::GetClipParameters() {
    // z in [0, w]; the fixup matrix owns Y orientation (see file header).
    return { true, false };
}

/* ------------------------------------------------------------------------------- */
/* Init / frame lifecycle                                                           */
/* ------------------------------------------------------------------------------- */

static GfxRenderingAPIC3D* sGdxOracleApi = nullptr; // ORACLE capture entry point (set in Init)

void GfxRenderingAPIC3D::Init() {
    if (mInitialized) {
        return;
    }
    // Precondition: stream B's OS layer has run gfxInitDefault() already.
    // n3dsfix (docs/research/native/hw-n60-3d7-analysis.md): a hardware A/B (preset=n60 +
    // native.track_resident=0) proved this buffer is marginal on the title attract-demo's one
    // full-30-machine GP frame (demo 3, course Big Blue) -- with track_resident off, the track's
    // heavier fallback draw path overflows it and the GDX bounds-checked writer (gdx_submit.h's
    // C3Dgdx_Reserve/PutChunk) correctly panics via svcBreak there. With track_resident on (the
    // normal n60 preset), the same frame is instead where the undiagnosed hardware freeze has
    // always reproduced -- consistent with this buffer being right at its edge either way. New3DS
    // got x4 first (20+ MB linear free mid-race per [tex-keep]/[nrr] receipts, and the
    // hardware-proven fix). Old3DS follow-up (docs/research/native/n3ds-cmdbuf-freeze.md): the
    // [cmdbuf] hwm receipt (base.c's Gdx3dsCmdbufHwmNote) read words=91938/size=131072 (70.1%) on
    // the SAME race-gp-finish GP start+prewarm through the Azahar --model old lane at the prior x2
    // size -- already past the ~65-70% comfort line, and the N3DS case showed the emulator can
    // UNDERSTATE a real hardware peak (it never reproduced that freeze at all), so 70% on the
    // emulator is a lower bound, not the true number. O3DS's own [vram]/linear-free receipts on
    // that same run never dropped below ~6.4 MB free mid-race, so matching New3DS's x4 here costs
    // an affordable extra C3D_DEFAULT_CMDBUF_SIZE bytes of linearAlloc (one more step on top of
    // x2's existing allocation) against that margin, and drops the measured peak to
    // 91938/262144 = 35.1% -- comfortable slack for hardware being worse than the emulator, the
    // same way the N3DS case was.
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    (void)isNew3ds; // both models now take the same x4 multiplier; kept for a future re-divergence.
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
    sGdxOracleApi = this; // ORACLE: the C capture entry point below reaches the backend via this

    // Framebuffer 0: the top screen. 240x400 because 3DS framebuffers are portrait.
    FramebufferC3D main = {};
    main.target = C3D_RenderTargetCreate(kScreenHeight, kScreenWidth, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    main.width = kScreenWidth;
    main.height = kScreenHeight;
    if (main.target == nullptr) {
        GFX_C3D_LOG("FATAL: C3D_RenderTargetCreate failed for the main target\n");
        return;
    }
    C3D_RenderTargetSetOutput(main.target, GFX_TOP, GFX_LEFT, kDisplayTransferFlags);
    mFramebuffers.clear();
    mFramebuffers.push_back(main);
    mCurrentFramebuffer = 0;

    // Vertex shader (picasso-assembled, embedded by bin2s).
    mVshDvlb = DVLB_ParseFile((u32*)(uintptr_t)gdx3ds_gfx_shader_shbin, gdx3ds_gfx_shader_shbin_size);
    if (mVshDvlb == nullptr) {
        GFX_C3D_LOG("FATAL: DVLB_ParseFile failed on the embedded vertex shader\n");
        return;
    }
    shaderProgramInit(&mShaderProgram);
    shaderProgramSetVsh(&mShaderProgram, &mVshDvlb->DVLE[0]);
    C3D_BindProgram(&mShaderProgram);
    mProjectionUniformLoc = shaderInstanceGetUniformLocation(mShaderProgram.vertexShader, "projection");

    // Fixed attribute layout: v0 pos4f, v1 uv0 2f (zw auto-filled), v2 rgba4f,
    // v3 uv1 2f (texel1's coordinates for the 2-cycle adjacent-tile modes).
    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attrInfo, 3, GPU_FLOAT, 2);
    mAttrLegacy = *attrInfo;
    XfInitLayout();
    gdx3ds_replay_backend_bind(mProjectionUniformLoc, mXfUnifLoc); /* C6: the uniforms that carry a pose */

    // Clip fixup: x' = y, y' = -x (landscape → portrait), z' = z - w (see header).
    Mtx_Zeros(&mFixupMatrix);
    mFixupMatrix.r[0].y = 1.0f;
    mFixupMatrix.r[1].x = -1.0f;
    mFixupMatrix.r[2].z = 1.0f;
    mFixupMatrix.r[2].w = -1.0f;
    mFixupMatrix.r[3].w = 1.0f;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &mFixupMatrix);

    C3D_DepthMap(true, -1.0f, 0.0f);
    C3D_CullFace(GPU_CULL_NONE); // Fast3D interpreter culls on the CPU
    C3D_DepthTest(false, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_ColorLogicOp(GPU_LOGICOP_COPY); // blending off
    for (int i = 0; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }

    mVboPool = (float*)linearAlloc(kVboPoolBytes);
    if (mVboPool == nullptr) {
        GFX_C3D_LOG("FATAL: linearAlloc(%zu) for the VBO pool failed\n", kVboPoolBytes);
        return;
    }
    mVboPoolFloats = GdxVboPoolUsableFloats(kVboPoolBytes / sizeof(float));
    mVboOffsetFloats = 0;
    if (mVboPoolFloats != kVboPoolBytes / sizeof(float)) {
        GFX_C3D_LOG("[debug] vbopoolkib: VBO pool capped to %zu KiB of %zu KiB (test knob)\n",
                    mVboPoolFloats * sizeof(float) / 1024, kVboPoolBytes / 1024);
    }

    mTextures.reserve(512);
    mShaderPool.reserve(64); // census-predicted variant population (~40/session observed)
    mStereoEnabled = Gdx3dsStereo::Init(); // config-gated; off = zero further cost
    /* n60stereo: the frame replay's shader registers (zeroed on the GPU in every mode). */
    Gdx3dsStereo::ReplayInit(shaderInstanceGetUniformLocation(mShaderProgram.vertexShader, "stk"),
                             shaderInstanceGetUniformLocation(mShaderProgram.vertexShader, "steye"),
                             mFramebuffers.empty() ? nullptr : mFramebuffers[0].target);
    mInitialized = true;
    GFX_C3D_LOG("initialized (VBO pool %zu KiB, stereo %s)\n", kVboPoolBytes / 1024,
                mStereoEnabled ? "on" : "off");
}

void GfxRenderingAPIC3D::OnResize() {
    // Fixed 400x240 screen — nothing to do.
}

/* ------------------------------------------------------------------------------------
 * MENU DISP tab — border modes. F-Zero X frames every screen inside the CRT-safe area:
 * on the 3DS top screen the scene lands as a ~370x208 content rect (borders 15/15
 * left/right, 16/16 top/bottom) and the HUD/2D rect content as ~370x224 (y 8..232).
 *
 * MEASURED FACT (2026-08-27 emulator receipts): the inset never reaches this backend
 * through SetViewport — the interpreter CPU-pre-transforms vertices to clip space with
 * the RSP viewport already baked in, and every main-target SetViewport/SetScissor call
 * carries the full window (0,0,400x240) ([disp-raw] telemetry: boot AND race frames,
 * remap-armed, zero inset rects). So the border modes are implemented at the ONLY
 * point the backend owns every vertex: the projection fixup matrix. Scaling the
 * incoming clip x/y about the screen center (the content rect is symmetric, so no
 * translation term is needed) magnifies the content; z/w are untouched, so depth
 * testing, PICA fog and the TexEnv fog blend are unchanged.
 *
 * Two matrices per frame: SCENE (perspective draws) and UI (ortho draws, w == 1 —
 * texrects/HUD; classified per draw exactly like the stereo path). Texture-backed
 * targets (transition captures) always use the plain fixup.
 *
 * Modes (gdiffuser.ini [display] border_mode, live via gdx3ds_disp_set_mode — the
 * matrices recompute every StartFrame, so a flip lands on the next frame):
 *   0 AUTHENTIC  no scaling (default — nothing regresses silently).
 *   1 FULL-BLEED scene x*400/370, y*240/208 (fills; ~6.7%% AR change); HUD authentic.
 *                May expose sky-wedge-family edge gaps: 4:3-authored backdrop quads
 *                can fall short of the opened edges (the DISP page says so on screen).
 *   2 ZOOM       everything (scene + HUD) uniformly x400/370 about center: kills the
 *                side borders, top/bottom bands shrink 16 -> ~7px, zero AR change.
 *                (A true crop-zoom is not expressible here without cropping the HUD.)
 *   3 HYBRID     scene x fills, y at the midpoint of ZOOM/FULL-BLEED (~3%% AR change,
 *                ~4px bands); HUD zoomed uniformly like ZOOM.
 * Known transient: the transition-wipe capture redraw is an ortho quad on the main
 * target, so modes 2/3 zoom the (already-scaled) capture ~8%% for the wipe frames.
 * Scissors expand about the center by the mode's scene factors (never shrink), so
 * the game's safe-area scissor cannot clip the magnified content. */
namespace {
int sDispMode = -1; /* -1 = config not latched yet (treated as 0/authentic) */
constexpr float kDispSideBorder = 15.0f; /* left/right border, window px */
constexpr float kDispTopBorder = 16.0f;  /* scene top/bottom border, window px */
constexpr float kDispSxFull = 400.0f / (400.0f - 2.0f * kDispSideBorder); /* ~1.081 */
constexpr float kDispSyFull = 240.0f / (240.0f - 2.0f * kDispTopBorder);  /* ~1.154 */
constexpr float kDispSyHyb = (kDispSxFull + kDispSyFull) * 0.5f;          /* ~1.118 */
C3D_Mtx sDispProjScene;
C3D_Mtx sDispProjUi;
const C3D_Mtx* sDispProjLast = nullptr; /* last matrix uploaded (non-stereo dedupe) */

int DispModeLatched() {
    if (sDispMode < 0) {
        if (&gdx3ds_config_get_int == nullptr) {
            sDispMode = 0; /* DL harness: no config lib, stay authentic forever */
        } else if (&gdx3ds_config_loaded == nullptr || gdx3ds_config_loaded()) {
            /* bleedfix: default FULL-BLEED (1) when the ini has no key yet, matching
             * gdx3ds_menu.c's sDispMode init (5c973d25, "full-bleed display mode is the
             * default (user hardware pick)"). That commit only updated the menu's own
             * fallback, so a fresh/pre-border_mode ini rendered AUTHENTIC here while the
             * menu's DSP tab already showed FULL-BLEED selected -- the menu's toggle is
             * edge-triggered (TouchDisp: `if (sDispMode != m)`), so the first tap on the
             * row that matched the menu's stale "already selected" state was a silent
             * no-op: no gdx3ds_disp_set_mode() call, no ini write, no MenuLog line, and
             * the renderer stayed authentic no matter how the DSP tab looked. */
            int m = gdx3ds_config_get_int("display", "border_mode", 1);
            sDispMode = (m >= 0 && m <= 3) ? m : 1;
        } else {
            return 0; /* INI not parsed yet: authentic until config-ready */
        }
    }
    return sDispMode;
}

/* Per-mode clip-space scale factors ({scene x, scene y}, {ui x, ui y}). */
void DispModeFactors(int mode, float* sxScene, float* syScene, float* sxUi, float* syUi) {
    switch (mode) {
        case 1: /* FULL-BLEED */
            *sxScene = kDispSxFull;
            *syScene = kDispSyFull;
            *sxUi = 1.0f;
            *syUi = 1.0f;
            break;
        case 2: /* ZOOM */
            *sxScene = *syScene = *sxUi = *syUi = kDispSxFull;
            break;
        case 3: /* HYBRID */
            *sxScene = kDispSxFull;
            *syScene = kDispSyHyb;
            *sxUi = *syUi = kDispSxFull;
            break;
        default:
            *sxScene = *syScene = *sxUi = *syUi = 1.0f;
            break;
    }
}

/* [display] widescreen (port/3ds/gdx3ds_widescreen.h): the kind of race frame the game side
 * published, latched at each StartFrame. 0 (lever off, menus) leaves every border mode exactly
 * as above. 1 (1P race): the game already opened the full 320x240 frame (RemoveBorders) and the
 * picture is 5:3, so nothing is scaled. 2 (VS race): each view keeps the N64 split frame and the
 * mode's SCENE scale; the UI is never scaled, so edge-anchored HUD stays on screen. Weak: the DL
 * harness links this backend without the port. */
extern "C" int gdx3ds_widescreen_frame_kind(void) __attribute__((weak));
int sDispWideKind = 0;
/* This frame's factors {scene x, scene y, ui x, ui y}, computed once in StartFrame (the scissor
 * expansion reads them instead of re-deriving the mode: ApplyScissorRect is NRR-hot). */
float sDispFrameFx[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

void DispFrameFactors(float* sxScene, float* syScene, float* sxUi, float* syUi) {
    DispModeFactors(sDispMode, sxScene, syScene, sxUi, syUi);
    if (sDispWideKind == 1) {
        *sxScene = *syScene = *sxUi = *syUi = 1.0f;
    } else if (sDispWideKind == 2) {
        *sxUi = *syUi = 1.0f;
    }
}

/* out = base with the incoming clip-x column scaled by sx and clip-y by sy
 * (out_j = sum_i base[j][i] * in_i: scaling an INPUT axis scales its column). */
void DispScaleMatrix(C3D_Mtx* out, const C3D_Mtx& base, float sx, float sy) {
    *out = base;
    for (int i = 0; i < 4; i++) {
        out->r[i].x *= sx;
        out->r[i].y *= sy;
    }
}
} // namespace

extern "C" void gdx3ds_disp_set_mode(int mode) {
    sDispMode = (mode >= 0 && mode <= 3) ? mode : 0;
    /* Nothing else to poke: StartFrame recomputes the matrices every frame. */
}

extern "C" int gdx3ds_disp_get_mode(void) {
    return sDispMode < 0 ? 0 : sDispMode;
}


/* ------------------------------------------------------------------------------- */
/* B11q frame-level stereo trace ([debug] stereo_trace = max lines, 0 = off).         */
/* Per frame, per eye: cleared? bound? draws issued? queued for the display transfer? */
/* Plus the non-draw content paths (framebuffer copies INTO the screen target), which  */
/* are the only way a frame can present pixels it never drew.                          */
/* ------------------------------------------------------------------------------- */
namespace {
long sStTraceBudget = -1; /* -1 = config unread */
unsigned long sStClearMain = 0;  /* ClearFramebuffer on fb 0 */
unsigned long sStClearOther = 0; /* ClearFramebuffer on a texture-backed fb */
unsigned long sStClearRight = 0; /* ...of those, mirrored onto the right eye */
unsigned long sStBindMain = 0;   /* StartDrawToFramebuffer(0) */
unsigned long sStBindOther = 0;
unsigned long sStCopyToMain = 0; /* CopyFramebuffer(dst == 0): content with no draw */
unsigned long sStCopyOther = 0;

bool StereoTraceOn() {
    if (sStTraceBudget < 0) {
        sStTraceBudget = (&gdx3ds_config_get_int != nullptr)
                             ? (long)gdx3ds_config_get_int("debug", "stereo_trace", 0)
                             : 0;
        if (sStTraceBudget < 0) {
            sStTraceBudget = 0;
        }
    }
    return sStTraceBudget > 0;
}

void StereoTraceFrameReset() {
    sStClearMain = sStClearOther = sStClearRight = 0;
    sStBindMain = sStBindOther = 0;
    sStCopyToMain = sStCopyOther = 0;
}
} // namespace

/* C4I [othx] gsp ledger: every GSPGPU_FlushDataCache in this backend goes through here so a
 * gputrace run can say what the flushes cost (the Old 3DS has no L2, so a flush is a
 * candidate there that it never was on the New 3DS). Off: one int test, then the flush. */
static void GdxHwsFlush(const void* addr, u32 size, int kind = GDX3DS_HWFL_OTHER) {
    if (&gdx3ds_hws_gsp_flush_kind != nullptr) {
        gdx3ds_hws_gsp_flush_kind(addr, size, kind); /* hwprof [hwsite]/[hwflush], [debug] flush_svc */
    } else {
        GSPGPU_FlushDataCache(addr, size);
    }
}

static void GdxFlushDataCache(int kind, const void* addr, u32 size) {
    GDX3DS_PW_NOTE(GDX3DS_PW_FLUSH); /* o3ds3d [prewrite] */
    const int hwfl = kind == GDX3DS_PROF_GSP_VBO ? GDX3DS_HWFL_VBO : GDX3DS_HWFL_TEX;
    if (gdx3ds_prof_active == 0) {
        GdxHwsFlush(addr, size, hwfl);
        return;
    }
    const u64 t0 = svcGetSystemTick();
    GdxHwsFlush(addr, size, hwfl);
    gdx3ds_prof_gsp_ticks[kind] += svcGetSystemTick() - t0;
    gdx3ds_prof_gsp_calls[kind]++;
    gdx3ds_prof_gsp_bytes[kind] += size;
}

/* C5 STAGE 0 [c5cat] (port/3ds/gdx3ds_mainprof.h): C3D_FrameBegin's vblank sync and GPU queue
 * drain get their own timing bucket. Weak: the DL harness links this backend without the clock. */
extern "C" int gdx3ds_c5cat_enter(int cat) __attribute__((weak));
extern "C" void gdx3ds_c5cat_leave(int prev) __attribute__((weak));

static int C5CatEnterFrameBegin() {
    if (&gdx3ds_c5cat_enter == nullptr) {
        return -1;
    }
    return gdx3ds_c5cat_enter(10 /* GDX3DS_C5CAT_FBEG */);
}

static void C5CatLeave(int prev) {
    if (prev >= 0 && &gdx3ds_c5cat_leave != nullptr) {
        gdx3ds_c5cat_leave(prev);
    }
}

void GfxRenderingAPIC3D::StartFrame() {
    if (!mInitialized) {
        return;
    }
    if (gdx3ds_verify_mode == 2) {
        return; /* [nverify] compare pass rides the recorded pass's open frame */
    }
    ElideReset(); /* [native] machines_mattab: the first draw after a frame begin re-issues */
    if (!mFrameActive) {
        // First begin of this frame (the interpreter legitimately re-enters
        // StartFrame mid-frame: Interpreter::Run and the VI-fallback prologue).
        mFrameDrawCalls = 0;
        mFrameTris = 0;
        mFrameFbBinds = 0;
        mFrameDrawsScreenFb = 0;
        mFrameDrawsTexFb = 0;
        mFrameBindMisses = 0;
        GdxPrimDiagFrameBegin();   // [prim] one-shot window arming (PRIM-COLOR diag)
        GdxLiveryDiagFrameBegin(); // [livery] one-shot window arming (SHIP-LIVERY-2 diag)
        StereoTraceFrameReset(); /* B11q */
        gdx3ds_overlap_frame_begin(); /* [overlap]: per-frame kick state (no-op unless armed) */
        gdx3ds_c3d_diet_frame_begin(); /* c5submit: latches [native] c3d_diet once */
    }
    sGdxFogFrame++;
    const int c5Prev = C5CatEnterFrameBegin();
    const bool firstBegin = !mFrameActive;
    if (GDX_NRR_CRUMBS_ON() && firstBegin) {
        gdx3ds_nrr_crumb_frame_begin_pre();
    }
    gdx3ds_gpuprof_frame_begin(mFrameActive ? 1 : 0); // owns C3D_FrameBegin(SYNCDRAW)
    if (GDX_NRR_CRUMBS_ON() && firstBegin) {
        gdx3ds_nrr_crumb_frame_begin_post();
    }
    if (firstBegin && (mFrameIndex == 600 || mFrameIndex == 4000)) { /* n60gpu: VRAM headroom (2 lines a run) */
        GFX_C3D_LOG("[vram] frame=%lu free=%luK linear free=%luK\n", (unsigned long)mFrameIndex,
                    (unsigned long)(vramSpaceFree() / 1024u), (unsigned long)(linearSpaceFree() / 1024u));
    }
    if (firstBegin) {
        gdx3ds_gputl_frame_begin();     /* n60gpu: [debug] gputl accounts the previous frame's GPU timeline */
        gdx3ds_gpucensus_frame_begin(); /* n60gpu: [debug] gpucensus (no-op unless armed) */
    }
    C5CatLeave(c5Prev);
    if (firstBegin && gdx3ds_replay_on) {
        gdx3ds_replay_frame_begin(); /* C6: a self-contained command list, a fresh site table */
    }
    C3D_FrameDrawOn(mFramebuffers[0].target);
    if (!mStereoEnabled && Gdx3dsStereo::WantInit()) {
        mStereoEnabled = Gdx3dsStereo::Init(); /* o3ds60: the Home 3D button, in a 2D-booted session */
        C3D_FrameDrawOn(mFramebuffers[0].target);
    }
    if (mStereoEnabled) {
        // Slider poll + right-target frame queueing; re-binds the main target.
        Gdx3dsStereo::FrameBegin(mFramebuffers[0].target);
        mFrameDrawsRightEye = 0;
    }
    Gdx3dsStereo::ReplayFrameBegin(); /* n60stereo: per-frame reset; steye = 0 when the GPU lost it */
    gdx3ds_skylate_frame_begin((unsigned long)mFrameIndex); /* n60tail: [native] sky_late per-frame state */
    mCurrentFramebuffer = 0;
    /* nimenu W1/M2 (review finding M2): decide THIS leg's n60stereo replay verdict now, instead of
     * waiting for whichever NRR category's marker happens to hand off first. gdx3ds_nrr_bk_frame_active
     * re-derives the exact same guard (mFrameActive/mCurrentFramebuffer/mStereoEnabled/Active()) every
     * category already calls lazily, and ReplayEnter() is idempotent (sInBody), so this adds no new
     * entry, only moves the first one earlier -- the tick-ahead game thread's sticky
     * gdx3ds_stereo_nrr_ok() read is then stale for a shorter window, not a new one. Return value
     * unused here; every real consumer calls this same function again at its own hand-off. */
    (void) gdx3ds_nrr_bk_frame_active();
    mVboOffsetFloats = 0;
    mVboFlushedFloats = 0;
    mVboExhaustionLogged = false;
    gdx3ds_mesh_frame_begin(); /* [native] machines receipt cadence */
    if (mFogLutCacheOverflowPending) {
        // Deferred to the frame boundary: LUT storage must outlive the previous
        // frame's command list (C3D_FrameEnd has serialized it by now).
        mFogLutCache.clear();
        mBoundFogKey = { INT32_MIN, INT32_MIN };
        mFogLutCacheOverflowPending = false;
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &mFixupMatrix);
    sDispProjLast = &mFixupMatrix;
    /* MENU DISP: derive this frame's scene/UI projection variants (live: a menu mode
     * flip lands here on the very next frame; trivial cost — 8 multiplies each). */
    sDispWideKind = (&gdx3ds_widescreen_frame_kind != nullptr) ? gdx3ds_widescreen_frame_kind() : 0;
    if (DispModeLatched() > 0) {
        float sxS, syS, sxU, syU;
        DispFrameFactors(&sxS, &syS, &sxU, &syU);
        sDispFrameFx[0] = sxS;
        sDispFrameFx[1] = syS;
        sDispFrameFx[2] = sxU;
        sDispFrameFx[3] = syU;
        DispScaleMatrix(&sDispProjScene, mFixupMatrix, sxS, syS);
        DispScaleMatrix(&sDispProjUi, mFixupMatrix, sxU, syU);
    }
    mFrameActive = true;
    gdx3ds_pw_startframe_done(); /* o3ds3d [prewrite]: StartFrame's own setup is the baseline */
}

/* Flush the not-yet-flushed tail of the frame's VBO appends in ONE svc, called
 * right before any GPU submission (EndFrame / the FrameSplit paths). Replaces the
 * old per-DrawTriangles GSPGPU_FlushDataCache — an svc kernel round trip per draw,
 * ~190/frame on menus. */
void GfxRenderingAPIC3D::FlushPendingVbo() {
    if (mVboFlushedFloats < mVboOffsetFloats) {
        GdxFlushDataCache(GDX3DS_PROF_GSP_VBO, mVboPool + mVboFlushedFloats,
                          (u32)((mVboOffsetFloats - mVboFlushedFloats) * sizeof(float)));
        mVboFlushedFloats = mVboOffsetFloats;
    }
}

/* o3ds3d [prewrite] (gdx3ds_prewrite.c): the VBO pool cursor, a first-write poll. */
extern "C" uint32_t gdx3ds_c3d_vbo_used_floats(void) {
    GfxRenderingAPIC3D* api = sGdxOracleApi;
    return api != nullptr ? (uint32_t)api->DiagVboUsedFloats() : 0u;
}

/* C5 Stage 4 [overlap] (gdx3ds_overlap.c): before a mid-frame kick submits the command list built
 * so far, the VBO tail must reach RAM. 1 = a C3D frame is open and the tail is flushed. */
extern "C" int gdx3ds_c3d_overlap_prepare(void) {
    GfxRenderingAPIC3D* api = sGdxOracleApi;
    if (api == nullptr || !api->DiagFrameActive()) {
        return 0;
    }
    api->FlushPendingVbo();
    return 1;
}

void GfxRenderingAPIC3D::EndFrame() {
    if (!mInitialized || !mFrameActive) {
        return;
    }
    gdx3ds_pw_frame_end((uint32_t)(mVboOffsetFloats * sizeof(float)), (uint32_t)kVboPoolBytes); /* o3ds3d */
    FlushPendingVbo();
    if (HsxGpuHashOn()) { /* [debug] hsx_gpuhash: this frame's draw command words */
        GFX_C3D_LOG("[hsxg] f=%lu h=%08lx d=%u wrap=%u lean=%lu runs=%lu\n", (unsigned long)mFrameIndex,
                    (unsigned long)sHsxGpuFrameHash, (unsigned)sHsxGpuFrameDraws, (unsigned)sHsxGpuWrap,
                    sHsxLeanDraws, sHsxRuns);
        sHsxGpuFrameHash = 2166136261u;
        sHsxGpuFrameDraws = 0;
    }
    /* B11q: read the queue state BEFORE C3D_FrameEnd consumes (and clears) it. */
    if (StereoTraceOn()) {
        --sStTraceBudget;
        const int qL = (mFramebuffers[0].target != nullptr && mFramebuffers[0].target->used) ? 1 : 0;
        const int qR = mStereoEnabled && Gdx3dsStereo::RightQueued() ? 1 : 0;
        GFX_C3D_LOG("[stereotrace] frame=%lu act=%d bindL=%lu bindT=%lu clrL=%lu clrR=%lu clrT=%lu "
                    "drawL=%lu drawR=%lu cpyL=%lu cpyT=%lu xferL=%d xferR=%d\n",
                    (unsigned long)mFrameIndex,
                    (mStereoEnabled && Gdx3dsStereo::Active()) ? 1 : 0, sStBindMain, sStBindOther,
                    sStClearMain, sStClearRight, sStClearOther,
                    (unsigned long)mFrameDrawsScreenFb, (unsigned long)mFrameDrawsRightEye,
                    sStCopyToMain, sStCopyOther, qL, qR);
    }
    gdx3ds_skylate_frame_end(); /* n60tail: an unclosed sky / region keeps the original order */
    gdx3ds_overlap_before_frame_end(); /* [overlap]: a kicked queue is drained + stopped first */
    if (mStereoEnabled) {
        Gdx3dsStereo::ReplayFrameEnd(); /* n60stereo: the right eye = the body re-submitted */
    }
    gdx3ds_gpuprof_frame_end_pre();
    if (gdx3ds_replay_on) {
        gdx3ds_replay_frame_end_pre(); /* C6: the back buffer A's transfer targets, the swap fence */
    }
    if (GDX_NRR_CRUMBS_ON()) {
        gdx3ds_nrr_crumb_frame_end_pre();
    }
    gdx3ds_gpucensus_before_frame_end(); /* n60gpu: fill-count modes patch the lists here (else no-op) */
    C3D_FrameEnd((u8)gdx3ds_overlap_frame_end_flags()); /* 0 (stock) unless [debug] endflush_cmdlist */
    gdx3ds_pw_after_frame_end(); /* o3ds3d [gpumem]: the frame's command-buffer words */
    if (gdx3ds_gputl_on()) { /* n60gpu: [debug] gputl classifies this frame's GX entries */
        const C3D_RenderTarget* mt = mFramebuffers[0].target;
        const C3D_RenderTarget* rt = mStereoEnabled ? Gdx3dsStereo::RightTarget() : nullptr;
        gdx3ds_gputl_frame_end(mt != nullptr ? mt->frameBuf.colorBuf : nullptr,
                               rt != nullptr ? rt->frameBuf.colorBuf : nullptr);
    }
    if (gdx3ds_gpucensus_mode() != 0) { /* n60gpu: [debug] gpucensus walks the queue just ended */
        const C3D_RenderTarget* mt = mFramebuffers[0].target;
        const C3D_RenderTarget* rt = mStereoEnabled ? Gdx3dsStereo::RightTarget() : nullptr;
        gdx3ds_gpucensus_after_frame_end(mt != nullptr ? mt->frameBuf.colorBuf : nullptr,
                                         mt != nullptr ? mt->frameBuf.depthBuf : nullptr,
                                         rt != nullptr ? rt->frameBuf.colorBuf : nullptr,
                                         rt != nullptr ? rt->frameBuf.depthBuf : nullptr);
    }
    if (GDX_NRR_CRUMBS_ON()) {
        gdx3ds_nrr_crumb_frame_end_post();
    }
    if (gdx3ds_replay_on) {
        gdx3ds_replay_frame_end_post((unsigned)mFrameDrawCalls); /* C6: keep A's GX queue */
    }
    gdx3ds_gpuprof_frame_end_post(mFrameDrawCalls, mFrameTris);
    gdx3ds_c3d_diet_frame_end(); /* c5submit: [c3ddiet]/[c3dprof] windows (verify/profile only) */
    if (mStereoEnabled) {
        /* B11q: the taskless-hold mark lives exactly one present. */
        mStereoHoldFrames = Gdx3dsStereo::EndFrameHold();
    }
    mFrameActive = false;
    // Present-path telemetry (bounded: ~1 svc line/sec at 60fps). C3D_FrameEnd
    // above queued the display transfer + swap for THIS frame's draws.
    mFrameIndex++;
    sAtlasDiagFrame = (unsigned long)mFrameIndex; /* [atlasdiag] window mirror for the texcache hook */
    /* GEOMLOSS: an ALWAYS-ON receipt, because "the renderer is silently dropping geometry"
     * must not be a fact you only learn by having remembered to set [debug] verbose=1 before
     * the run. Silent while every counter is zero (the normal case, zero log cost), loud from
     * the first non-zero one onward. vboHW is the pool high-water mark, which is what says
     * how close a scene is to the cliff BEFORE it goes over. */
    if ((mFrameIndex & 63) == 1 &&
        (sVboPackRefused | sVboPackSplit | sVboDrawDropped | sNbCommitRefused) != 0) {
        GFX_C3D_LOG("[geomloss] frame=%lu vboHW=%luK/%luK vboRef=%lu vboSplit=%lu vboDrop=%lu "
                    "nbCommitRef=%lu\n",
                    (unsigned long)mFrameIndex,
                    (unsigned long)(mVboHighWaterFloats * sizeof(float) / 1024),
                    (unsigned long)(mVboPoolFloats * sizeof(float) / 1024), sVboPackRefused,
                    sVboPackSplit, sVboDrawDropped, sNbCommitRefused);
    }
    if ((mFrameIndex & 63) == 1 && GdxVerboseTelemetry()) {
        // texUp/texImp/texMiss/texDel are CUMULATIVE since boot (they always were);
        // dUp/dImp/dMiss/dDel are the deltas since the previous [c3d] emit (64 frames),
        // so per-frame rates read directly as d*/64 without diffing lines by hand.
        // A menu idling with a healthy texture cache shows dUp=0 dMiss=0.
        static unsigned long sPrevUploads = 0;
        static unsigned long sPrevImports = 0;
        static unsigned long sPrevMisses = 0;
        static unsigned long sPrevDeletes = 0;
        static unsigned long sPrevSettimgLoads = 0;
        static unsigned long sPrevSettimgMemoHits = 0;
        static unsigned long sPrevPackedDraws = 0;
        const unsigned long dUp = (unsigned long)mTexUploads - sPrevUploads;
        const unsigned long dImp = sTexCacheImports - sPrevImports;
        const unsigned long dMiss = sTexCacheMisses - sPrevMisses;
        const unsigned long dDel = sTexCacheDeletes - sPrevDeletes;
        // SELECT-PERF: dRl = full LoadResourceProcess resolutions, dRm = SETTIMG-memo
        // hits, per 64-frame window. Their sum is the o2r SETTIMG volume; the memo is
        // working when dRl stays near 0 on a machine-select dwell.
        const unsigned long dRl = sSettimgLoads - sPrevSettimgLoads;
        const unsigned long dRm = sSettimgMemoHits - sPrevSettimgMemoHits;
        // [triloop]: dPk = packed (repack-free) draws this window; the tri path
        // is fully packed when dPk tracks the window's draw volume.
        const unsigned long dPk = sPackedDrawCalls - sPrevPackedDraws;
        sPrevPackedDraws = sPackedDrawCalls;
        const unsigned long dNat = sNativeDrawCalls - sPrevNativeDraws; /* [native] indexed mesh draws */
        sPrevNativeDraws = sNativeDrawCalls;
        sPrevUploads = (unsigned long)mTexUploads;
        sPrevImports = sTexCacheImports;
        sPrevMisses = sTexCacheMisses;
        sPrevDeletes = sTexCacheDeletes;
        sPrevSettimgLoads = sSettimgLoads;
        sPrevSettimgMemoHits = sSettimgMemoHits;
        sTexMissWindowBudget = 6; // refill the non-race [texmiss] sampling budget
        char msg[448];
        int n = std::snprintf(msg, sizeof(msg),
                              "[c3d] frame=%lu draws=%lu (scr=%lu tex=%lu) tris=%lu fbBinds=%lu "
                              "bindMiss=%lu texUp=%lu texUpFail=%lu texImp=%lu texMiss=%lu texDel=%lu "
                              "dUp=%lu dImp=%lu dMiss=%lu dDel=%lu dRl=%lu dRm=%lu dPk=%lu dNat=%lu "
                              "vboHW=%luK/%luK vboRef=%lu vboSplit=%lu vboDrop=%lu nbCommitRef=%lu",
                              (unsigned long)mFrameIndex, (unsigned long)mFrameDrawCalls,
                              (unsigned long)mFrameDrawsScreenFb, (unsigned long)mFrameDrawsTexFb,
                              (unsigned long)mFrameTris, (unsigned long)mFrameFbBinds,
                              (unsigned long)mFrameBindMisses, (unsigned long)mTexUploads,
                              (unsigned long)mTexUploadFails, sTexCacheImports, sTexCacheMisses,
                              sTexCacheDeletes, dUp, dImp, dMiss, dDel, dRl, dRm, dPk, dNat,
                              (unsigned long)(mVboHighWaterFloats * sizeof(float) / 1024),
                              (unsigned long)(mVboPoolFloats * sizeof(float) / 1024),
                              sVboPackRefused, sVboPackSplit, sVboDrawDropped, sNbCommitRefused);
        if (mTexUploadFails > 0 && n > 0 && n < (int)sizeof(msg)) {
            /* DROPSITES site 4: which refusal, and whether the recovery caught it. Emitted only
             * when something actually failed, so the all-zero [c3d] line stays byte-identical to
             * every receipt other tracks compare against. */
            n += std::snprintf(msg + n, sizeof(msg) - n, " texUpWhy=%lu/%lu/%lu texReclaim=%lu",
                               sTexUpFailId, sTexUpFailDim, sTexUpFailInit, sTexInitReclaimed);
        }
        if (sTexMissingBinds > 0 && n > 0 && n < (int)sizeof(msg)) {
            n += std::snprintf(msg + n, sizeof(msg) - n, " texFallbackBind=%lu", sTexMissingBinds);
        }
        if (mAtlasPlaced > 0 && n > 0 && n < (int)sizeof(msg)) {
            // [trectbatch] atlas-only suffix (never emitted with trectbatch=0): page evictions /
            // views copied out to standalone / copy-out failures / no-safe-candidate fallbacks.
            n += std::snprintf(msg + n, sizeof(msg) - n, " atlasEv=%lu/%lu/%lu/%lu", sAtlasEvictions,
                               sAtlasEvictedViews, sAtlasEvictCopyFail, sAtlasEvictNone);
        }
        if (sPadGutterPadded > 0 && n > 0 && n < (int)sizeof(msg)) {
            // [padgutter] cumulative: padded uploads / axes given a replicated gutter (N64
            // clamp) / axes zeroed (wrap or mirror, where the pad belongs to the wrap image).
            n += std::snprintf(msg + n, sizeof(msg) - n, " padGut=%lu/%lu/%lu", sPadGutterPadded,
                               sPadGutterReplicated, sPadGutterZeroed);
        }
        if (mStereoEnabled && n > 0 && n < (int)sizeof(msg)) {
            // Stereo-only suffix: the off-state [c3d] line stays byte-identical.
            n += std::snprintf(msg + n, sizeof(msg) - n, " eyeR=%lu stereo=%d hold=%lu",
                               (unsigned long)mFrameDrawsRightEye, Gdx3dsStereo::Active() ? 1 : 0,
                               mStereoHoldFrames);
            if (sAnchorDraws > 0 && n > 0 && n < (int)sizeof(msg)) {
                // [anchor] prim-depth ortho draws anchored to scene depth this window: count and
                // the NDC depth range they were pinned to (race position markers -> their ships).
                n += std::snprintf(msg + n, sizeof(msg) - n, " anchor=%lu/%.2f/%.2f", sAnchorDraws,
                                   (double)sAnchorDMin, (double)sAnchorDMax);
                sAnchorDraws = 0;
                sAnchorDMin = 2.0f;
                sAnchorDMax = -1.0f;
            }
        }
        if (n > 0) {
            svcOutputDebugString(msg, n);
            /* [c3d_filelog] killswitch [debug] c3d_filelog (default 0 = off, so every
             * existing log stays byte-identical). svcOutputDebugString reaches Azahar Qt's
             * azahar_log.txt (with log_filter=*:Debug) and NOTHING else: on the RetroArch
             * lane, on hardware and over hwlink the only readable sink is the SD filelog,
             * which every other tracer ([prof], [gpu], [vp], [rt], [fh]) already writes to.
             * Without this the stereo receipts this line carries — `eyeR=<n> stereo=<0|1>`
             * and `anchor=<draws>/<dmin>/<dmax>` — are unreadable on the one lane where the
             * right-eye pass can now be made to run. `gdx-dev --3d` turns it on. */
            static int sC3dFileLog = -1;
            if (sC3dFileLog < 0) {
                /* Weak, like every other config read here: the DL harness links the backend
                 * without the config library. */
                sC3dFileLog = (&gdx3ds_config_get_int != nullptr &&
                               gdx3ds_config_get_int("debug", "c3d_filelog", 0) != 0) ? 1 : 0;
            }
            if (sC3dFileLog != 0 && &gdx3ds_filelog_write != nullptr) {
                gdx3ds_filelog_write(msg, (size_t)n);
            }
        }
    }
}

void GfxRenderingAPIC3D::FinishRender() {
    // No fence needed: C3D_FrameEnd(0) in EndFrame already serializes the frame.
}

/* ------------------------------------------------------------------------------- */
/* Shaders / combiner                                                               */
/* ------------------------------------------------------------------------------- */

void GfxRenderingAPIC3D::ComputeVertexLayout(ShaderProgramC3D& prg) {
    int off = 4; // clip-space position
    for (int t = 0; t < 2; t++) {
        if (prg.cc.usedTextures[t]) {
            prg.uvOffset[t] = off;
            off += 2;
            if (prg.cc.clamp[t][0]) {
                off += 1; // clamp-S limit (skipped at repack: the clamp is expressed
                          // as GPU_CLAMP_TO_EDGE at bind time — see DrawTriangles)
            }
            if (prg.cc.clamp[t][1]) {
                off += 1; // clamp-T limit (same)
            }
        }
    }
    if (prg.cc.opt_fog) {
        prg.fogOffset = off;
        off += 4;
    }
    if (prg.cc.opt_grayscale) {
        prg.grayscaleOffset = off;
        off += 4;
    }
    prg.inputSize = prg.cc.opt_alpha ? 4 : 3;
    for (int j = 0; j < prg.cc.numInputs && j < 7; j++) {
        prg.inputOffset[j] = off;
        off += prg.inputSize;
    }
    prg.inputStride = (uint8_t)off;
}

GDX_COLD void GfxRenderingAPIC3D::LogUnmappedCombiner(const ShaderProgramC3D& prg, const char* reason) {
    const auto key = std::make_pair(prg.id0, prg.id1);
    if (!mLoggedUnmappedIds.insert(key).second) {
        return;
    }
    // One line per unique shader-ID pair: the cross-reference format for stream F's
    // combiner census (id0 = packed SHADER_* muxes, id1 = ShaderOpts bits).
    const CCFeatures& cc = prg.cc;
    GFX_C3D_LOG("unmapped combiner id0=%016llx id1=%016llx "
                "rgb0=(%d,%d,%d,%d) a0=(%d,%d,%d,%d) rgb1=(%d,%d,%d,%d) a1=(%d,%d,%d,%d) "
                "2cyc=%d fog=%d noise=%d gray=%d texedge=%d inputs=%d tex=(%d,%d): %s\n",
                (unsigned long long)prg.id0, (unsigned long long)prg.id1, cc.c[0][0][0], cc.c[0][0][1],
                cc.c[0][0][2], cc.c[0][0][3], cc.c[0][1][0], cc.c[0][1][1], cc.c[0][1][2], cc.c[0][1][3],
                cc.c[1][0][0], cc.c[1][0][1], cc.c[1][0][2], cc.c[1][0][3], cc.c[1][1][0], cc.c[1][1][1],
                cc.c[1][1][2], cc.c[1][1][3], cc.opt_2cyc, cc.opt_fog, cc.opt_noise, cc.opt_grayscale,
                cc.opt_texture_edge, cc.numInputs, cc.usedTextures[0], cc.usedTextures[1], reason);
}

void GfxRenderingAPIC3D::MapCombiner(ShaderProgramC3D& prg) {
    const CCFeatures& cc = prg.cc;
    const char* why = "";

    prg.mapped = false;

    const bool twoCycle = cc.opt_2cyc && !IsCycle1Passthrough(cc);
    const bool cycle0Dead = twoCycle && !Cycle1ReadsCombined(cc);
    CombinerBuild b;
    ChooseVtxInputs(cc, twoCycle, b.vtxRgb, b.vtxAlpha);

    bool ok;
    if (cycle0Dead) {
        // Cycle 0 is unreferenced (GenerateCC zeroed it) — map cycle 1 alone so a
        // constant-spill prefix stage is still legal (spill before any committed cycle).
        ok = MapCycle(cc.c[1][0], cc.c[1][1], !cc.opt_alpha, b, &why);
    } else {
        ok = MapCycle(cc.c[0][0], cc.c[0][1], !cc.opt_alpha, b, &why);
        if (ok && twoCycle) {
            ok = MapCycle(cc.c[1][0], cc.c[1][1], !cc.opt_alpha, b, &why);
        }
    }

    // Per-vertex fog blend: append a final INTERPOLATE stage lerping fogColor into
    // the mapped fragment colour by the baked per-vertex fog factor (which we route
    // through GPU_PRIMARY_COLOR's alpha at repack time). This mirrors the desktop GL
    // shader exactly: rgb = mix(frag.rgb, fogColor, f) with frag.a preserved (see
    // libultraship default.shader.glsl, both the o_alpha and !o_alpha fog branches).
    //
    // The factor rides GPU_PRIMARY_COLOR's alpha, so it is only free when NO mapped
    // stage reads that slot:
    //   - opaque-alpha combiners (!opt_alpha): the interpreter forces shade alpha to
    //     1.0 on fog draws, and the combiner's alpha is REPLACE(1) — slot free.
    //   - translucent combiners (opt_alpha) whose alpha channel does NOT source a
    //     per-vertex input (b.vtxAlpha == 0): the real alpha is texel/const only, so
    //     GPU_PRIMARY_COLOR alpha is still unread and free to carry f. This is the
    //     boost-jet / G_RM_FOG_SHADE_A case — previously routed through the depth-LUT
    //     fog unit, which F-Zero X's [0.95,1.0] race clip-depth band saturates to full
    //     fogColor (washing the flame to the fog/blend colour). The blend stage keeps
    //     the combiner's real alpha via GPU_PREVIOUS below, matching the GL o_alpha path.
    // fogColor rgb rides the stage constant, refreshed per draw from the vertex 0 fog
    // slot; INTERPOLATE src2 = primary-colour alpha (f). See ShaderProgramC3D header.
    const bool fogAlphaSlotFree = !cc.opt_alpha || b.vtxAlpha == 0;
    const bool wantFogBlend = ok && cc.opt_fog && fogAlphaSlotFree &&
                              (b.numStages < kMaxTexEnvStages) && (b.prevRgb == SHADER_COMBINED);
    if (wantFogBlend) {
        TexEnvStageC3D fs;
        fs.rgbFunc = GPU_INTERPOLATE;
        fs.rgbSrc[0] = GPU_CONSTANT;      // fogColor (constColor, refreshed per draw)
        fs.rgbSrc[1] = GPU_PREVIOUS;      // fragment colour from the mapped chain
        fs.rgbSrc[2] = GPU_PRIMARY_COLOR; // fog factor, carried in alpha
        fs.rgbOp[0] = GPU_TEVOP_RGB_SRC_COLOR;
        fs.rgbOp[1] = GPU_TEVOP_RGB_SRC_COLOR;
        fs.rgbOp[2] = GPU_TEVOP_RGB_SRC_ALPHA; // src2 reads primary-colour ALPHA = f
        fs.alphaFunc = GPU_REPLACE;
        fs.alphaSrc[0] = GPU_PREVIOUS; // pass the mapped alpha through untouched
        fs.alphaOp[0] = GPU_TEVOP_A_SRC_ALPHA;
        b.stages[b.numStages] = fs;
        prg.fogBlendStageIndex = b.numStages;
        prg.fogBlendStage = true;
        b.numStages++;
    }

    // Fog-draw path telemetry (deduped per unique shader ID). A fog draw either takes
    // the per-vertex blend stage (blend=1) or falls back to the depth-indexed hardware
    // fog unit (blend=0) — the latter is only correct for wide-depth-range draws, NOT
    // F-Zero X's race band. If the boost jet logs blend=0 here, its combiner routes a
    // per-vertex input to alpha (vtxAlpha!=0) and needs the alternate factor carrier.
    if (cc.opt_fog && mLoggedFogPathIds.insert(std::make_pair(prg.id0, prg.id1)).second) {
        GFX_C3D_LOG("[fogpath] id0=%016llx id1=%016llx opt_alpha=%d vtxAlpha=%d blend=%d\n",
                    (unsigned long long)prg.id0, (unsigned long long)prg.id1, cc.opt_alpha ? 1 : 0,
                    b.vtxAlpha, prg.fogBlendStage ? 1 : 0);
    }

    if (ok) {
        prg.mapped = true;
        prg.vtxColorInput = b.vtxRgb;
        prg.vtxAlphaInput = b.vtxAlpha;
        prg.numStages = b.numStages;
        for (int s = 0; s < b.numStages; s++) {
            TexEnvStageC3D& st = b.stages[s];
            const float cr = st.constRgbFixed == 0 ? 0.0f : 1.0f;
            const float ca = st.constAlphaFixed == 0 ? 0.0f : 1.0f;
            st.constColor = PackTexEnvColor(cr, cr, cr, ca);
            prg.stages[s] = st;
        }
    } else {
        LogUnmappedCombiner(prg, why);
        // Fallback: keep geometry visible — modulate texture with vertex colour,
        // or plain vertex colour when untextured.
        prg.vtxColorInput = cc.numInputs > 0 ? 1 : 0;
        prg.vtxAlphaInput = prg.vtxColorInput;
        prg.numStages = 1;
        TexEnvStageC3D& st = prg.stages[0];
        st = TexEnvStageC3D{};
        if (cc.usedTextures[0]) {
            st.rgbFunc = GPU_MODULATE;
            st.rgbSrc[0] = GPU_TEXTURE0;
            st.rgbSrc[1] = GPU_PRIMARY_COLOR;
            st.alphaFunc = GPU_MODULATE;
            st.alphaSrc[0] = GPU_TEXTURE0;
            st.alphaSrc[1] = GPU_PRIMARY_COLOR;
        } else {
            st.rgbFunc = GPU_REPLACE;
            st.rgbSrc[0] = GPU_PRIMARY_COLOR;
            st.alphaFunc = GPU_REPLACE;
            st.alphaSrc[0] = GPU_PRIMARY_COLOR;
        }
    }

    if (prg.mapped) {
        // Options the TexEnv chain still ignores — log for the census, draw anyway.
        // (Fog is handled natively via the PICA fog LUT; no stage spent, no log.)
        if (cc.opt_grayscale) {
            LogUnmappedCombiner(prg, "grayscale option ignored");
        } else if (cc.opt_noise) {
            LogUnmappedCombiner(prg, "noise option ignored");
        } else if (cc.opt_invisible) {
            LogUnmappedCombiner(prg, "invisible option ignored");
        }
    }
}

void GfxRenderingAPIC3D::ApplyShaderState(const ShaderProgramC3D& prg) {
    for (int i = 0; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i)); // stages past numStages default to passthrough
    }
    for (int s = 0; s < kMaxTexEnvStages; s++) {
        // Keep the constant-register mirror in lockstep: TexEnvInit resets every
        // stage's constant to 0xFFFFFFFF, the per-stage loop below rebinds the
        // mapped stages' baked constants.
        mAppliedStageConst[s] = 0xFFFFFFFF;
    }
    for (int s = 0; s < prg.numStages; s++) {
        C3D_TexEnv* env = C3D_GetTexEnv(s);
        const TexEnvStageC3D& st = prg.stages[s];
        C3D_TexEnvFunc(env, C3D_RGB, st.rgbFunc);
        C3D_TexEnvFunc(env, C3D_Alpha, st.alphaFunc);
        C3D_TexEnvSrc(env, C3D_RGB, st.rgbSrc[0], st.rgbSrc[1], st.rgbSrc[2]);
        C3D_TexEnvSrc(env, C3D_Alpha, st.alphaSrc[0], st.alphaSrc[1], st.alphaSrc[2]);
        C3D_TexEnvOpRgb(env, st.rgbOp[0], st.rgbOp[1], st.rgbOp[2]);
        C3D_TexEnvOpAlpha(env, st.alphaOp[0], st.alphaOp[1], st.alphaOp[2]);
        C3D_TexEnvColor(env, st.constColor);
        mAppliedStageConst[s] = st.constColor;
    }
}

uint32_t GfxRenderingAPIC3D::RefreshStageConstants(const ShaderProgramC3D& prg, const float* vertex0) {
    // Draw-constant combiner inputs (prim/env colour) ride the per-stage TexEnv
    // constant registers: constant across a flush, so vertex 0 is authoritative.
    // The interpreter guarantees the batch never spans a prim/env VALUE change
    // (lus-3ds-primenv-flush.patch splits it), and the mAppliedStageConst mirror
    // makes the per-draw re-apply a compare in the common case — a redundant
    // C3D_TexEnvColor would re-dirty the TexEnv and re-emit its words every draw.
    uint32_t reappliedMask = 0;
    mLastDrawHadInputConst = false;
    for (int s = 0; s < prg.numStages; s++) {
        const TexEnvStageC3D& st = prg.stages[s];
        if (st.constRgbInput == 0 && st.constAlphaInput == 0) {
            continue;
        }
        float r = st.constRgbFixed == 0 ? 0.0f : 1.0f;
        float g = r;
        float bch = r;
        float a = st.constAlphaFixed == 0 ? 0.0f : 1.0f;
        if (st.constRgbInput > 0) {
            const int off = prg.inputOffset[st.constRgbInput - 1];
            r = vertex0[off];
            g = vertex0[off + 1];
            bch = vertex0[off + 2];
        }
        if (st.constAlphaInput > 0 && prg.inputSize == 4) {
            a = vertex0[prg.inputOffset[st.constAlphaInput - 1] + 3];
        }
        const uint32_t packed = PackTexEnvColor(r, g, bch, a);
        if (!mLastDrawHadInputConst) {
            mLastDrawHadInputConst = true;
            mLastInputConstPacked = packed;
        }
        if (packed != mAppliedStageConst[s]) {
            C3D_TexEnvColor(C3D_GetTexEnv(s), packed);
            mAppliedStageConst[s] = packed;
            reappliedMask |= 1u << s;
        }
    }

    // The per-vertex fog blend stage's constant carries fogColor (draw-constant rgb
    // in the vertex fog slot); its lerp weight is primary-colour alpha, so alpha here
    // is a don't-care. Refresh it from vertex 0 like any other draw-constant colour.
    if (prg.fogBlendStage && prg.fogBlendStageIndex >= 0 && prg.fogOffset >= 0) {
        const float* fog = vertex0 + prg.fogOffset;
        const uint32_t packed = PackTexEnvColor(fog[0], fog[1], fog[2], 1.0f);
        if (packed != mAppliedStageConst[prg.fogBlendStageIndex]) {
            C3D_TexEnvColor(C3D_GetTexEnv(prg.fogBlendStageIndex), packed);
            mAppliedStageConst[prg.fogBlendStageIndex] = packed;
            reappliedMask |= 1u << prg.fogBlendStageIndex;
        }
    }
    return reappliedMask;
}

uint32_t GfxRenderingAPIC3D::RefreshStageConstantsPacked(const ShaderProgramC3D& prg,
                                                         const Gdx3dsVboPackAux& aux) {
    // [triloop] identical to RefreshStageConstants, but the vertex-0 input
    // values come from the interpreter's batch aux record: the packed PICA
    // layout only carries the vtxColorInput per-vertex, so every constant-spill
    // input rides the record instead of a variable-layout slot. The interpreter
    // writes it from the same RDP state the legacy vertex-0 append baked, under
    // the same "no value change without a flush" invariant.
    uint32_t reappliedMask = 0;
    mLastDrawHadInputConst = false;
    for (int s = 0; s < prg.numStages; s++) {
        const TexEnvStageC3D& st = prg.stages[s];
        if (st.constRgbInput == 0 && st.constAlphaInput == 0) {
            continue;
        }
        float r = st.constRgbFixed == 0 ? 0.0f : 1.0f;
        float g = r;
        float bch = r;
        float a = st.constAlphaFixed == 0 ? 0.0f : 1.0f;
        if (st.constRgbInput > 0 && st.constRgbInput <= (int)aux.numInputs) {
            const float* in = aux.inputRgba[st.constRgbInput - 1];
            r = in[0];
            g = in[1];
            bch = in[2];
        }
        if (st.constAlphaInput > 0 && st.constAlphaInput <= (int)aux.numInputs && prg.inputSize == 4) {
            a = aux.inputRgba[st.constAlphaInput - 1][3];
        }
        const uint32_t packed = PackTexEnvColor(r, g, bch, a);
        if (!mLastDrawHadInputConst) {
            mLastDrawHadInputConst = true;
            mLastInputConstPacked = packed;
        }
        if (packed != mAppliedStageConst[s]) {
            C3D_TexEnvColor(C3D_GetTexEnv(s), packed);
            mAppliedStageConst[s] = packed;
            reappliedMask |= 1u << s;
        }
    }

    if (prg.fogBlendStage && prg.fogBlendStageIndex >= 0 && prg.fogOffset >= 0) {
        const uint32_t packed = PackTexEnvColor(aux.fogRgb[0], aux.fogRgb[1], aux.fogRgb[2], 1.0f);
        if (packed != mAppliedStageConst[prg.fogBlendStageIndex]) {
            C3D_TexEnvColor(C3D_GetTexEnv(prg.fogBlendStageIndex), packed);
            mAppliedStageConst[prg.fogBlendStageIndex] = packed;
            reappliedMask |= 1u << prg.fogBlendStageIndex;
        }
    }
    return reappliedMask;
}

void GfxRenderingAPIC3D::ApplyAlphaTest() {
    bool on = false;
    int func = GPU_ALWAYS;
    int ref = 0;
    if (mCurrentShader != nullptr) {
        const CCFeatures& cc = mCurrentShader->cc;
        if (cc.opt_texture_edge) {
            on = true;
            func = GPU_GEQUAL;
            ref = 128;
        } else if (cc.opt_alpha_threshold) {
            // G_AC_THRESHOLD: compare against the SETBLENDCOLOR alpha, pushed per flush.
            on = true;
            func = GPU_GEQUAL;
            ref = (int)(mCurrentAlphaCompareThreshold * 255.0f);
        }
        mAlphaCompareThresholdDirty = false;
    }
    bool countOnly = false;
    if (mAtValid && on == mAtOn && func == mAtFunc && ref == mAtRef && ElideOn(&countOnly)) {
        mElideAt++;
        if (!countOnly) {
            return; /* [native] machines_mattab: C3D's effect block already holds exactly this */
        }
    }
    C3D_AlphaTest(on, (GPU_TESTFUNC)func, ref);
    mAtValid = true;
    mAtOn = on;
    mAtFunc = func;
    mAtRef = ref;
}

/* ---- [native] machines_mattab: skip re-issuing identical state (Gdx3dsXfDraw::stateElide) ----
 * Exact by construction: each skipped call would have stored into citro3d's context exactly what
 * it already holds (the tracker records every call of that kind this backend makes, and nothing
 * else in the port calls them), so the only difference is that the next draw does not re-emit an
 * unchanged register block. Reset at every frame begin (the first draw of a frame re-issues). */
void GfxRenderingAPIC3D::ElideReset() {
    mBufLastVerts = nullptr;
    mTexLastBound[0] = mTexLastBound[1] = nullptr;
    mAtValid = false;
}

bool GfxRenderingAPIC3D::ElideOn(bool* countOnly) const {
    const int level = mXf != nullptr ? mXf->stateElide : 0;
    *countOnly = level == 2;
    return level == 1 || level == 2;
}

void GfxRenderingAPIC3D::BindTexUnit(int unit, C3D_Tex* tex) {
    bool countOnly = false;
    if (mTexLastBound[unit] == tex && std::memcmp(tex, &mTexLastSnap[unit], sizeof(C3D_Tex)) == 0 &&
        ElideOn(&countOnly)) {
        mElideTex++;
        if (!countOnly) {
            return;
        }
    }
    C3D_TexBind(unit, tex);
    mTexLastBound[unit] = tex;
    std::memcpy(&mTexLastSnap[unit], tex, sizeof(C3D_Tex));
}

void GfxRenderingAPIC3D::XfBindVertexBuffer(const void* verts, int stride) {
    bool countOnly = false;
    if (mBufLastVerts == verts && mBufLastStride == stride && ElideOn(&countOnly)) {
        mElideVbo++;
        if (!countOnly) {
            return;
        }
    }
    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, verts, stride, 3, 0x210);
    mBufLastVerts = verts;
    mBufLastStride = stride;
}

ShaderProgram* GfxRenderingAPIC3D::CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) {
    ShaderProgramC3D& prg = mShaderPool[std::make_pair(shaderId0, shaderId1)];
    prg.id0 = shaderId0;
    prg.id1 = shaderId1;
    gfx_cc_get_features(shaderId0, shaderId1, &prg.cc);
    ComputeVertexLayout(prg);
    MapCombiner(prg);
    mCurrentShader = &prg;
    ApplyShaderState(prg);
    return (ShaderProgram*)&prg;
}

ShaderProgram* GfxRenderingAPIC3D::LookupShader(uint64_t shaderId0, uint64_t shaderId1) {
    auto it = mShaderPool.find(std::make_pair(shaderId0, shaderId1));
    return it == mShaderPool.end() ? nullptr : (ShaderProgram*)&it->second;
}

void GfxRenderingAPIC3D::LoadShader(ShaderProgram* newPrg) {
    mCurrentShader = (ShaderProgramC3D*)newPrg;
    if (mCurrentShader != nullptr) {
        ApplyShaderState(*mCurrentShader);
    }
}

void GfxRenderingAPIC3D::UnloadShader(ShaderProgram* oldPrg) {
    if (oldPrg != nullptr && oldPrg == (ShaderProgram*)mCurrentShader) {
        mCurrentShader = nullptr;
    }
}

void GfxRenderingAPIC3D::ClearShaderCache() {
    mShaderPool.clear();
    mCurrentShader = nullptr;
}

void GfxRenderingAPIC3D::ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    if (prg == nullptr) {
        *numInputs = 0;
        usedTextures[0] = usedTextures[1] = false;
        return;
    }
    const ShaderProgramC3D* p = (const ShaderProgramC3D*)prg;
    *numInputs = (uint8_t)p->cc.numInputs;
    usedTextures[0] = p->cc.usedTextures[0];
    usedTextures[1] = p->cc.usedTextures[1];
}

/* ------------------------------------------------------------------------------- */
/* Textures                                                                         */
/* ------------------------------------------------------------------------------- */

uint32_t GfxRenderingAPIC3D::NewTexture() {
    mTextures.emplace_back();
    return (uint32_t)(mTextures.size() - 1);
}

void GfxRenderingAPIC3D::SelectTexture(int tile, uint32_t textureId) {
    if (tile >= 0 && tile < 2) {
        mBoundTextureIds[tile] = textureId;
        mLastSelectedTile = tile;
    }
}

/* ------------------------------------------------------------------------------- */
/* [trectbatch] HUD atlas (LOCKED-60 Task A, docs/research/texrect2-progress.md)      */
/* ------------------------------------------------------------------------------- */
namespace {
constexpr uint32_t kAtlasPageW = 512;
constexpr uint32_t kAtlasPageH = 256;
constexpr uint32_t kAtlasMaxPages = 8;    /* 8 x 512 KiB linear (a pow2-padded standalone 304x3
                                             strip costs 16 KiB; its atlas cell 6 KiB) */
constexpr uint32_t kAtlasMaxCellW = 320;  /* the 304x3 / 160x6 HUD gradient strips */
constexpr uint32_t kAtlasMaxCellH = 64;
constexpr uint32_t kAtlasGutter = 1;     /* replicated edge texels around each cell */
/* [trectbatch] page reclamation: a page whose refcount never reaches 0 (the interpreter only
 * recycles ids lazily) used to pin the atlas forever — full=22 pages=8 over one session, every
 * new HUD set falling back to standalone. Pages now stamp the frame of their last bind and,
 * when all pages are allocated, the least recently bound page NOT bound this frame (SYNCDRAW at
 * frame begin means the GPU is done with earlier frames) and not behind a currently bound id
 * (an open batch may still reference it) is evicted: its views are copied out of the page into
 * standalone pow2 textures (their interpreter cache entries stay valid and renderable), the
 * page is reset and reused. */
unsigned long sAtlasPageLastUsed[kAtlasMaxPages] = {};
} // namespace

/* Shelf-pack (w+2g) x (h+2g) into a page; write content + gutter Morton-swizzled at the
 * page's flipped row layout (same convention as UploadTexture: N64 row y -> padded row
 * pageH-1-y) and flush only the touched tile rows. */
bool GfxRenderingAPIC3D::AtlasTryPlace(TextureC3D& t, const uint8_t* rgba32, uint32_t w, uint32_t h) {
    GDX3DS_PW_NOTE(GDX3DS_PW_TEX); /* o3ds3d [prewrite] */
    const bool diag = AtlasDiagArmed() && (unsigned long)mFrameIndex >= sAtlasDiagFrom &&
                      (unsigned long)mFrameIndex <= sAtlasDiagTo;
    const uint32_t diagId = mBoundTextureIds[mLastSelectedTile];
    if (w == 0 || h == 0 || w > kAtlasMaxCellW || h > kAtlasMaxCellH) {
        if (diag) {
            GFX_C3D_LOG("[atlasdiag] f=%lu id=%u reject w=%u h=%u\n", (unsigned long)mFrameIndex, diagId, w, h);
        }
        return false;
    }
    const uint32_t cw = w + 2 * kAtlasGutter;
    const uint32_t ch = h + 2 * kAtlasGutter;
    int pageIdx = -1;
    uint32_t px = 0, py = 0;
    for (size_t p = 0; p < mAtlasPages.size() && pageIdx < 0; p++) {
        AtlasPageC3D& pg = mAtlasPages[p];
        if (!pg.inited) {
            continue;
        }
        if (pg.cursorX + cw <= kAtlasPageW && ch <= pg.shelfH && pg.shelfY + pg.shelfH <= kAtlasPageH) {
            px = pg.cursorX;
            py = pg.shelfY;
            pg.cursorX += (uint16_t)cw;
            pageIdx = (int)p;
        } else if (pg.shelfY + pg.shelfH + ch <= kAtlasPageH) {
            pg.shelfY += pg.shelfH;
            pg.shelfH = (uint16_t)ch;
            pg.cursorX = (uint16_t)cw;
            px = 0;
            py = pg.shelfY;
            pageIdx = (int)p;
        }
    }
    if (pageIdx < 0) {
        /* reuse an emptied page slot, else allocate a new one */
        for (size_t p = 0; p < mAtlasPages.size() && pageIdx < 0; p++) {
            if (!mAtlasPages[p].inited) {
                pageIdx = (int)p;
            }
        }
        if (pageIdx < 0 && mAtlasPages.size() >= kAtlasMaxPages) {
            /* every page allocated: reclaim the LRU page that is safe to rewrite */
            const auto boundPage = [this](int unit) -> int {
                const uint32_t id = mBoundTextureIds[unit];
                return id < mTextures.size() ? mTextures[id].page : -1;
            };
            const int bound0 = boundPage(0);
            const int bound1 = boundPage(1);
            int victim = -1;
            for (size_t p = 0; p < mAtlasPages.size(); p++) {
                if (!mAtlasPages[p].inited || (int)p == bound0 || (int)p == bound1 ||
                    sAtlasPageLastUsed[p] >= (unsigned long)mFrameIndex) {
                    continue;
                }
                if (victim < 0 || sAtlasPageLastUsed[p] < sAtlasPageLastUsed[(size_t)victim]) {
                    victim = (int)p;
                }
            }
            if (victim < 0) {
                sAtlasEvictNone++;
                mAtlasFull++;
                if (diag) {
                    GFX_C3D_LOG("[atlasdiag] f=%lu id=%u full (no evict victim)\n", (unsigned long)mFrameIndex,
                                  diagId);
                }
                return false;
            }
            /* copy every view of the victim page out into its own pow2 texture */
            const auto copyOut = [this](TextureC3D& v) -> bool {
                const AtlasPageC3D& src = mAtlasPages[(size_t)v.page];
                const uint32_t vw = v.width, vh = v.height;
                const uint32_t padW = NextPow2(vw < 8 ? 8 : vw);
                const uint32_t padH = NextPow2(vh < 8 ? 8 : vh);
                C3D_Tex tex = {};
                if (!C3D_TexInit(&tex, (u16)padW, (u16)padH, GPU_RGBA8)) {
                    return false;
                }
                std::memset(tex.data, 0, tex.size);
                const uint32_t* s = (const uint32_t*)src.tex.data;
                uint32_t* d = (uint32_t*)tex.data;
                const uint32_t sTpr = kAtlasPageW >> 3;
                const uint32_t dTpr = padW >> 3;
                for (uint32_t y = 0; y < vh; y++) {
                    const uint32_t sy = kAtlasPageH - 1 - (v.ay + y);
                    const uint32_t dy = padH - 1 - y;
                    for (uint32_t x = 0; x < vw; x++) {
                        const uint32_t sx = v.ax + x;
                        const uint32_t so = (((sy >> 3) * sTpr + (sx >> 3)) << 6) | MortonInterleave(sx, sy);
                        const uint32_t dof = (((dy >> 3) * dTpr + (x >> 3)) << 6) | MortonInterleave(x, dy);
                        d[dof] = s[so];
                    }
                }
                /* [padgutter] the cell this view is leaving HAD a replicated gutter; its new
                 * pow2 padding must carry one too, or an evicted HUD texture's edges change
                 * appearance mid-run purely because the atlas filled up. */
                if (GdxPadGutterEnabled()) {
                    WritePadGutter(d, vw, vh, padW, padH, (v.cms & G_TX_CLAMP) != 0, (v.cmt & G_TX_CLAMP) != 0);
                }
                GdxFlushDataCache(GDX3DS_PROF_GSP_TEX, tex.data, tex.size);
                v.tex = tex;
                v.inited = true;
                v.padWidth = padW;
                v.padHeight = padH;
                v.uScale = (float)vw / (float)padW;
                v.vScale = (float)vh / (float)padH;
                const GPU_TEXTURE_FILTER_PARAM f = v.linearFilter ? GPU_LINEAR : GPU_NEAREST;
                C3D_TexSetFilter(&v.tex, f, f);
                C3D_TexSetWrap(&v.tex, WrapFromN64(v.cms), WrapFromN64(v.cmt));
                return true;
            };
            for (size_t id = 0; id < mTextures.size(); id++) {
                TextureC3D& v = mTextures[id];
                if (v.page != victim) {
                    continue;
                }
                if (copyOut(v)) {
                    sAtlasEvictedViews++;
                } else {
                    sAtlasEvictCopyFail++;
                    v.inited = false; /* dropped: binds miss until the interpreter re-imports */
                }
                v.page = -1;
                v.ax = v.ay = 0;
                v.uOff = v.vOff = 0.0f;
            }
            AtlasPageC3D& vp = mAtlasPages[(size_t)victim];
            vp.refs = 0;
            vp.shelfY = 0;
            vp.shelfH = 0;
            vp.cursorX = 0;
            sAtlasEvictions++;
            mAtlasResets++;
            if (diag) {
                GFX_C3D_LOG("[atlasdiag] f=%lu id=%u evict page=%d\n", (unsigned long)mFrameIndex, diagId, victim);
            }
            pageIdx = victim;
        }
        if (pageIdx < 0) {
            mAtlasPages.emplace_back();
            pageIdx = (int)mAtlasPages.size() - 1;
        }
        AtlasPageC3D& pg = mAtlasPages[(size_t)pageIdx];
        if (!pg.inited) {
            if (!C3D_TexInit(&pg.tex, (u16)kAtlasPageW, (u16)kAtlasPageH, GPU_RGBA8)) {
                mAtlasFull++;
                if (diag) {
                    GFX_C3D_LOG("[atlasdiag] f=%lu id=%u texinit-failed page=%d\n", (unsigned long)mFrameIndex,
                                  diagId, pageIdx);
                }
                GFX_C3D_LOG("[atlas] C3D_TexInit(%ux%u) failed (linear heap exhausted?)\n", kAtlasPageW, kAtlasPageH);
                if ((size_t)pageIdx == mAtlasPages.size() - 1) {
                    mAtlasPages.pop_back();
                }
                return false;
            }
            std::memset(pg.tex.data, 0, pg.tex.size);
            GdxFlushDataCache(GDX3DS_PROF_GSP_TEX, pg.tex.data, pg.tex.size);
            pg.inited = true;
        }
        pg.refs = 0;
        pg.shelfY = 0;
        pg.shelfH = (uint16_t)ch;
        pg.cursorX = (uint16_t)cw;
        px = 0;
        py = 0;
    }
    AtlasPageC3D& pg = mAtlasPages[(size_t)pageIdx];
    /* write content + replicated gutter */
    uint32_t* dst = (uint32_t*)pg.tex.data;
    const uint32_t tilesPerRow = kAtlasPageW >> 3;
    if (GdxTexFastEnabled()) {
        gdx3ds_swz_cell_fast(dst, rgba32, w, h, px, py, kAtlasGutter, kAtlasPageW, kAtlasPageH); /* N60TEX */
    } else {
        gdx3ds_swz_cell_ref(dst, rgba32, w, h, px, py, kAtlasGutter, kAtlasPageW, kAtlasPageH);
    }
    {
        /* flush the touched tile rows only: rows [top, bottom] of the cell in flipped space */
        const uint32_t rowTop = kAtlasPageH - 1 - (py + ch - 1);
        const uint32_t rowBot = kAtlasPageH - 1 - py;
        const uint32_t tileRow0 = rowTop >> 3;
        const uint32_t tileRow1 = rowBot >> 3;
        const size_t bytesPerTileRow = (size_t)tilesPerRow * 64u * 4u;
        GdxFlushDataCache(GDX3DS_PROF_GSP_TEX, (uint8_t*)pg.tex.data + (size_t)tileRow0 * bytesPerTileRow,
                          (u32)((tileRow1 - tileRow0 + 1) * bytesPerTileRow));
    }
    if (t.inited) { /* recycled id that owned a standalone texture */
        C3D_TexDelete(&t.tex);
        t.inited = false;
    }
    t.page = (int16_t)pageIdx;
    t.ax = (uint16_t)(px + kAtlasGutter);
    t.ay = (uint16_t)(py + kAtlasGutter);
    t.width = w;
    t.height = h;
    t.padWidth = kAtlasPageW;
    t.padHeight = kAtlasPageH;
    t.uScale = (float)w / (float)kAtlasPageW;
    t.vScale = (float)h / (float)kAtlasPageH;
    t.uOff = (float)t.ax / (float)kAtlasPageW;
    t.vOff = (float)t.ay / (float)kAtlasPageH;
    pg.refs++;
    if (diag) {
        GFX_C3D_LOG("[atlasdiag] f=%lu id=%u place page=%d ax=%u ay=%u w=%u h=%u refs=%u pages=%u\n",
                      (unsigned long)mFrameIndex, diagId, pageIdx, (unsigned)t.ax, (unsigned)t.ay, w, h,
                      (unsigned)pg.refs, (unsigned)mAtlasPages.size());
    }
    mAtlasPlaced++;
    mTexUploads++;
    gdx3ds_gpuprof_note_tex_upload((unsigned)(cw * ch * 4u));
    return true;
}

void GfxRenderingAPIC3D::AtlasRelease(TextureC3D& t) {
    if (t.page < 0) {
        return;
    }
    if (AtlasDiagArmed() && (unsigned long)mFrameIndex >= sAtlasDiagFrom &&
        (unsigned long)mFrameIndex <= sAtlasDiagTo) {
        GFX_C3D_LOG("[atlasdiag] f=%lu release page=%d ax=%u ay=%u refs=%u\n", (unsigned long)mFrameIndex,
                      (int)t.page, (unsigned)t.ax, (unsigned)t.ay,
                      (size_t)t.page < mAtlasPages.size() ? (unsigned)mAtlasPages[(size_t)t.page].refs : 0u);
    }
    if ((size_t)t.page < mAtlasPages.size()) {
        AtlasPageC3D& pg = mAtlasPages[(size_t)t.page];
        if (pg.refs > 0) {
            pg.refs--;
        }
        if (pg.refs == 0 && pg.inited) {
            /* last view gone: the page's cells are all orphaned, start it over */
            pg.shelfY = 0;
            pg.shelfH = 0;
            pg.cursorX = 0;
            mAtlasResets++;
        }
    }
    t.page = -1;
    t.ax = t.ay = 0;
    t.uOff = t.vOff = 0.0f;
}

/* native-all S1 [native] atlas_race_reset (gdx_texcache.cpp TexCacheAtlasReset): at the mode
 * change into a race, every atlas view is released and every page deleted. Shelf packing places a
 * view after whatever the previous screens left on the pages, so without this the race HUD's
 * cells (page, position, neighbours, the stale texels around them) were a function of the menu
 * path taken to reach the race, and any lever that changes menu texture traffic (ni_menu, tex_keep)
 * moved them (nimenu-progress.md 4.1: 13 px of one race-gp-finish row). Pages are re-created on
 * demand, zeroed (AtlasTryPlace), so the race always packs onto fresh pages in its own order.
 * Mode-change only: the same deferred point ReclaimLinearForTexture deletes pages from. */
GDX_COLD void GfxRenderingAPIC3D::AtlasResetAll() {
    unsigned views = 0;
    for (size_t id = 0; id < mTextures.size(); id++) {
        TextureC3D& v = mTextures[id];
        if (v.page < 0) {
            continue;
        }
        v.page = -1;
        v.ax = v.ay = 0;
        v.uOff = v.vOff = 0.0f;
        v.serial++; /* n60 nrr_trk: a cached state block naming this id is stale */
        views++;
    }
    const unsigned pages = (unsigned)mAtlasPages.size();
    for (size_t p = 0; p < mAtlasPages.size(); p++) {
        if (mAtlasPages[p].inited) {
            C3D_TexDelete(&mAtlasPages[p].tex);
        }
        sAtlasPageLastUsed[p] = 0;
    }
    mAtlasPages.clear();
    mAtlasResets++;
    GFX_C3D_LOG("[atlas] race reset: %u views released, %u pages freed (linfree %lu KiB)\n", views, pages,
                (unsigned long)(linearSpaceFree() / 1024u));
}

void GfxRenderingAPIC3D::AtlasStats(unsigned long out[5]) const {
    out[0] = mAtlasPlaced;
    out[1] = mAtlasFull;
    out[2] = (unsigned long)mAtlasPages.size();
    out[3] = mAtlasResets;
    out[4] = (unsigned long)(linearSpaceFree() / 1024u);
}

/* Texture-dependent pack fields for one unit from the bound id (VboPackBegin's legacy loop
 * body verbatim, plus the [trectbatch] view offset). */
void GfxRenderingAPIC3D::AtlasFillTexParams(int t, Gdx3dsVboPackParams* params, bool skyClampFix) const {
    const ShaderProgramC3D& prg = *mCurrentShader;
    params->uScale[t] = 1.0f;
    params->vScale[t] = 1.0f;
    params->wrapClampU[t] = 1e30f;
    params->wrapClampV[t] = 1e30f;
    params->uOff[t] = 0.0f;
    params->vOff[t] = 0.0f;
    if (!params->used[t]) {
        return;
    }
    const uint32_t texId = mBoundTextureIds[t];
    if (texId < mTextures.size() && TexReady(mTextures[texId])) {
        const TextureC3D& tex = mTextures[texId];
        params->uScale[t] = tex.uScale;
        params->vScale[t] = tex.vScale;
        if (skyClampFix) {
            /* PRE-OFFSET space on purpose — see gdx3ds_texclamp.h. The limit is measured
             * from the CELL's own origin because uOff is added by the packers AFTER this
             * min; adding it here would double it (pinned by
             * port/tests/gfx_atlas_uv_tests.cpp). [atlasuv] an atlas VIEW clamps at its
             * cell's far EDGE instead of the last texel's centre: the (extent - 0.5) limit
             * sheared every pow2 glyph against its unclamped standalone twin, while the
             * edge limit leaves the quad's interpolation alone AND still keeps the sample
             * inside the cell's replicated gutter. See gdx3ds_view_clamp_limit. */
            const bool isView = (tex.page >= 0) && !GdxAtlasViewClampLegacy();
            if (!prg.cc.clamp[t][0] && (tex.cms & G_TX_CLAMP) != 0) {
                if (isView) {
                    params->wrapClampU[t] = gdx3ds_view_clamp_limit(tex.width, tex.padWidth);
                } else if (gdx3ds_pad_clamp_needed(tex.width, tex.padWidth)) {
                    params->wrapClampU[t] = gdx3ds_pad_clamp_limit(tex.width, tex.padWidth);
                }
            }
            if (!prg.cc.clamp[t][1] && (tex.cmt & G_TX_CLAMP) != 0) {
                if (isView) {
                    params->wrapClampV[t] = gdx3ds_view_clamp_limit(tex.height, tex.padHeight);
                } else if (gdx3ds_pad_clamp_needed(tex.height, tex.padHeight)) {
                    params->wrapClampV[t] = gdx3ds_pad_clamp_limit(tex.height, tex.padHeight);
                }
            }
        }
        if (tex.page >= 0) {
            params->uOff[t] = tex.uOff;
            params->vOff[t] = tex.vOff;
            params->hasOff = 1;
        }
    }
}

void GfxRenderingAPIC3D::VboPackRefreshTextures(Gdx3dsVboPackParams* params) {
    if (mCurrentShader == nullptr) {
        return;
    }
    const bool skyClampFix = GdxSkyClampFixEnabled();
    params->hasOff = 0;
    for (int t = 0; t < 2; t++) {
        AtlasFillTexParams(t, params, skyClampFix);
    }
}

/* DROPSITES site 4 helpers. */

/* C3D_TexInit with the [debug] texinitfail=N forcing knob folded in, so the recovery path is
 * reachable from a shipped build without exhausting the linear heap for real. */
bool GfxRenderingAPIC3D::TexInitChecked(C3D_Tex* tex, uint32_t padW, uint32_t padH) {
    static const unsigned long sEvery = [] {
        if (&gdx3ds_config_get_int != nullptr) {
            const int n = gdx3ds_config_get_int("debug", "texinitfail", 0);
            return n > 0 ? (unsigned long)n : 0ul;
        }
        return 0ul;
    }();
    sTexInitFailEvery = sEvery;
    sTexInitCalls++;
    if (sTexInitFailEvery != 0 && (sTexInitCalls % sTexInitFailEvery) == 0) {
        return false; /* forced: nothing is allocated, so nothing needs undoing */
    }
    return C3D_TexInit(tex, (u16)padW, (u16)padH, GPU_RGBA8) != false;
}

/* Free the linear heap a failed texture allocation needs, without destroying anything a live
 * view still references. An atlas page at refs == 0 has already had all of its cells orphaned
 * (AtlasRelease resets such a page's shelf cursor), and AtlasTryPlace re-inits a page on demand,
 * so deleting one is self-healing. Returns true if anything was actually freed. */
bool GfxRenderingAPIC3D::ReclaimLinearForTexture() {
    bool freed = false;
    for (size_t p = 0; p < mAtlasPages.size(); p++) {
        AtlasPageC3D& pg = mAtlasPages[p];
        if (pg.inited && pg.refs == 0) {
            C3D_TexDelete(&pg.tex);
            pg.inited = false;
            pg.shelfY = 0;
            pg.shelfH = 0;
            pg.cursorX = 0;
            freed = true;
        }
    }
    return freed;
}

/* The shared "this id has no content" texture: 8x8 opaque white, so a draw that would otherwise
 * not rebind the texunit at all samples the SHADE colour (TEXEL0 * SHADE with TEXEL0 = white)
 * instead of whichever unrelated texture the unit happened to hold. Allocated once from a
 * healthy heap (the first successful upload), never from the failure path. */
void GfxRenderingAPIC3D::EnsureMissingTexture() {
    if (mMissingTexInited) {
        return;
    }
    if (!C3D_TexInit(&mMissingTex, 8, 8, GPU_RGBA8)) {
        return; /* 256 bytes refused: the heap is far past saving, leave the bind-miss as-is */
    }
    std::memset(mMissingTex.data, 0xFF, mMissingTex.size);
    /* [overlap]/endflush_cmdlist: the one texture writer that relied on C3D_FrameEnd's
     * whole-heap flush. Once, 256 bytes. */
    GdxFlushDataCache(GDX3DS_PROF_GSP_TEX, mMissingTex.data, mMissingTex.size);
    C3D_TexSetFilter(&mMissingTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&mMissingTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    mMissingTexInited = true;
}

/* C4c: the decoded-texture staging high-water.
 *
 * Fast3D's Interpreter::Init allocates ONE `malloc(max_tex_size * max_tex_size * 4)` and
 * holds it for the process lifetime; on this backend GetMaxTextureSize() is 1024, so that
 * is 4,194,304 B -- the single largest heap block the port has that no [mem-res] row names,
 * and 16% of an Old 3DS memtype-0 grant. Every decode writes 4*width*height bytes of it and
 * then lands here, so this is the one place that sees the real demand without touching a
 * vendored decoder. A pure max-update: three compares on a path that is about to do a
 * width*height texture copy.
 *
 * It reads slightly HIGH, deliberately: ImportTextureImg uploads its source buffer directly
 * rather than the staging one, so its extent is counted here although it never touched the
 * staging buffer. A high-water that over-reports is the safe direction for sizing. */
extern "C" {
unsigned long gdx3ds_tex_upload_hwm_bytes = 0;
unsigned long gdx3ds_tex_upload_hwm_w = 0;
unsigned long gdx3ds_tex_upload_hwm_h = 0;
unsigned long gdx3ds_tex_upload_count = 0;
}

namespace {
/* N60TEX [debug] texburst: the backend share (alloc + swizzle + flush) of a miss's decode time. */
struct TexUploadTicksScope {
    uint64_t t0;
    TexUploadTicksScope() : t0(gdx3ds_hitch_now()) {}
    ~TexUploadTicksScope() {
        gdx3ds_hitch_add(GDX_HITCH_EV_TEX_UP_TICKS, (uint32_t)(gdx3ds_hitch_now() - t0));
    }
    TexUploadTicksScope(const TexUploadTicksScope&) = delete;
    TexUploadTicksScope& operator=(const TexUploadTicksScope&) = delete;
};
} // namespace

void GfxRenderingAPIC3D::UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) {
    GDX3DS_PW_NOTE(GDX3DS_PW_TEX); /* o3ds3d [prewrite] */
    GdxHwsScope hws(GDX3DS_HWS_TEXUP); /* hwprof [hwsite] texup */
    TexUploadTicksScope upTicks; /* N60TEX [texburst] upl= */
    {
        const unsigned long bytes = (unsigned long)width * (unsigned long)height * 4ul;
        gdx3ds_tex_upload_count++;
        if (bytes > gdx3ds_tex_upload_hwm_bytes) {
            gdx3ds_tex_upload_hwm_bytes = bytes;
            gdx3ds_tex_upload_hwm_w = width;
            gdx3ds_tex_upload_hwm_h = height;
        }
    }
    const uint32_t id = mBoundTextureIds[mLastSelectedTile];
    if (id < mTextures.size()) {
        mTextures[id].serial = ++mTexSerial; /* n60 nrr_trk: the content of this id changes */
    }
    if (id >= mTextures.size() || width == 0 || height == 0) {
        mTexUploadFails++;
        sTexUpFailId++;
        GFX_C3D_LOG("DROPSITE: UploadTexture invalid texture id %u (%ux%u) -- nothing uploaded\n", id, width,
                    height);
        return;
    }
    TextureC3D& t = mTextures[id];
    if (gdx3ds_verify_mode != 0) { /* [nverify] content identity of what this id holds */
        if (sVfyTexHash.size() <= id) {
            sVfyTexHash.resize(id + 1, 0);
        }
        if (sVfyTexSeq.size() <= id) {
            sVfyTexSeq.resize(id + 1, 0);
        }
        sVfyTexSeq[id] = ++sVfyUploadSeq;
        if (sVfyTexEnds.size() <= id * 2u + 1u) {
            sVfyTexEnds.resize(id * 2u + 2u, 0);
        }
        {
            const size_t payload = static_cast<size_t>(width) * height * 4u;
            uint32_t w0v = 0, wNv = 0;
            if (payload >= 4u) {
                memcpy(&w0v, rgba32Buf, 4);
                memcpy(&wNv, rgba32Buf + payload - 4u, 4);
            }
            sVfyTexEnds[id * 2u] = w0v;
            sVfyTexEnds[id * 2u + 1u] = wNv;
        }
        uint32_t h = 2166136261u;
        h = VfyFnv(h, &width, 4);
        h = VfyFnv(h, &height, 4);
        sVfyTexHash[id] = VfyFnv(h, rgba32Buf, static_cast<size_t>(width) * height * 4u);
    }
    /* [trectbatch] a recycled id that was an atlas view drops its cell reference first; an
     * armed (eligible rect) upload then tries the atlas and falls back to standalone. */
    if (t.page >= 0) {
        AtlasRelease(t);
    }
    gdx3ds_gpucensus_note_upload(rgba32Buf, width, height, mAtlasArm ? 1 : 0); /* n60gpu: no-op unless armed */
    if (mAtlasArm && AtlasTryPlace(t, rgba32Buf, width, height)) {
        return;
    }

    // PICA needs pow2 dimensions, minimum 8. Pad and rescale UVs at repack time.
    const uint32_t padW = NextPow2(width < 8 ? 8 : width);
    const uint32_t padH = NextPow2(height < 8 ? 8 : height);
    if (padW > 1024 || padH > 1024) {
        mTexUploadFails++;
        sTexUpFailDim++;
        GFX_C3D_LOG("DROPSITE: UploadTexture %ux%u (padded %ux%u) exceeds the PICA 1024 limit -- this "
                    "texture can never be uploaded; the draw binds the fallback\n",
                    width, height, padW, padH);
        return;
    }

    if (t.inited && (t.padWidth != padW || t.padHeight != padH)) {
        C3D_TexDelete(&t.tex);
        t.inited = false;
    }
    if (!t.inited) {
        /* DROPSITES site 4: this used to be a bare drop. The id kept the previous texture and
         * the next draw did not rebind the texunit at all, so it sampled something unrelated.
         * Now: reclaim what is safely reclaimable and retry once, then fall back to a defined
         * sample rather than an arbitrary one. */
        bool texInitOk = TexInitChecked(&t.tex, padW, padH);
        if (!texInitOk && ReclaimLinearForTexture()) {
            texInitOk = TexInitChecked(&t.tex, padW, padH);
            if (texInitOk) {
                sTexInitReclaimed++;
                GFX_C3D_LOG("DROPSITE: C3D_TexInit(%ux%u) failed and SUCCEEDED after reclaiming "
                            "orphaned atlas pages (linear free now %u KiB)\n",
                            padW, padH, (unsigned)(linearSpaceFree() / 1024u));
            }
        }
        if (!texInitOk) {
            mTexUploadFails++;
            sTexUpFailInit++;
            GFX_C3D_LOG("DROPSITE: C3D_TexInit(%ux%u) failed with %u KiB linear free and no orphaned "
                        "atlas page to reclaim (or the retry failed too) -- the draw binds the "
                        "fallback, not the id's old contents\n",
                        padW, padH, (unsigned)(linearSpaceFree() / 1024u));
            return;
        }
        std::memset(t.tex.data, 0, t.tex.size); // padding must not bleed under linear filtering
        // [padfill] SKY-WEDGE-3 discriminator (INI [debug] diag_padfill=1): flood the
        // pow2 padding with OPAQUE MAGENTA instead of transparent black, so any draw
        // that samples past the content edge (see the clamp-limit fix in DrawTriangles)
        // is unmistakable in scanout SHOTs. Content texels overwrite their own region
        // below; only true padding stays magenta. Init-time only — zero steady cost.
        static const bool sPadFillDiag = [] {
            if (std::getenv("GDX_DIAG_PADFILL") != nullptr) {
                return true;
            }
            if (&gdx3ds_config_get_int != nullptr) {
                return gdx3ds_config_get_int("debug", "diag_padfill", 0) != 0;
            }
            return false;
        }();
        if (sPadFillDiag) {
            uint32_t* p = (uint32_t*)t.tex.data;
            const size_t n = t.tex.size / sizeof(uint32_t);
            for (size_t k = 0; k < n; k++) {
                p[k] = 0xFF00FFFFu; // R<<24|G<<16|B<<8|A = opaque magenta
            }
        }
        t.inited = true;
    }

    // CPU Morton swizzle into the PICA tiled layout. Texel u32 = R<<24|G<<16|B<<8|A.
    // Row order: PICA T=0 samples the LAST row of tiled memory (bottom-up storage,
    // harness-verified: writing rows top-to-bottom rendered scene 3 upside-down),
    // so N64 row y — V=0 is the texture's top — lands at padded row padH-1-y. The
    // padding rows then sit at the far end of the T range, outside [0, vScale].
    uint32_t* dst = (uint32_t*)t.tex.data;
    if (GdxTexFastEnabled()) {
        gdx3ds_swz_rows_fast(dst, rgba32Buf, width, height, padW, padH); /* N60TEX: same stores */
    } else {
        gdx3ds_swz_rows_ref(dst, rgba32Buf, width, height, padW, padH);
    }
    /* [padgutter] Replicate the content edge into the pow2 padding: the standalone path used
     * to memset(0) it, so GPU_LINEAR faded every clamped content edge into transparent black
     * while the [trectbatch] atlas cell of the same texture showed a replicated gutter. That
     * is the 782-px course-name glyph-edge delta the residuals track measured. t.cms/t.cmt
     * carry the importing tile's modes because TextureCacheLookup seeds a fresh slot from its
     * own cache key. See WritePadGutter. */
    if (GdxPadGutterEnabled()) {
        WritePadGutter(dst, width, height, padW, padH, (t.cms & G_TX_CLAMP) != 0, (t.cmt & G_TX_CLAMP) != 0);
    }
    GdxFlushDataCache(GDX3DS_PROF_GSP_TEX, t.tex.data, t.tex.size);

    t.width = width;
    t.height = height;
    t.padWidth = padW;
    t.padHeight = padH;
    t.uScale = (float)width / (float)padW;
    t.vScale = (float)height / (float)padH;
    mTexUploads++;
    EnsureMissingTexture(); /* DROPSITES site 4: allocate the fallback while the heap is healthy */
    // S12 asset-cost: the swizzled PICA payload just flushed (post pow2-pad) is the
    // real upload/bandwidth cost the ETC1 shift would cut.
    gdx3ds_gpuprof_note_tex_upload((unsigned)t.tex.size);

    const GPU_TEXTURE_FILTER_PARAM filter = t.linearFilter ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(&t.tex, filter, filter);
    C3D_TexSetWrap(&t.tex, WrapFromN64(t.cms), WrapFromN64(t.cmt));
}

void GfxRenderingAPIC3D::SetSamplerParameters(int sampler, bool linearFilter, uint32_t cms, uint32_t cmt) {
    if (sampler < 0 || sampler >= 2) {
        return;
    }
    const uint32_t id = mBoundTextureIds[sampler];
    if (id >= mTextures.size()) {
        return;
    }
    TextureC3D& t = mTextures[id];
    t.linearFilter = linearFilter;
    t.cms = cms;
    t.cmt = cmt;
    if (t.inited) {
        const GPU_TEXTURE_FILTER_PARAM filter = linearFilter ? GPU_LINEAR : GPU_NEAREST;
        C3D_TexSetFilter(&t.tex, filter, filter);
        // NOTE: REPEAT/MIRROR on pow2-padded non-pow2 textures wraps over the padding;
        // acceptable for the SM64-proven subset, revisit with stream F's census.
        // SHADOW-2 audit: the wrap period on this hardware is the PADDED size (uScale
        // maps logical UVs into [0, w/padW]; PICA folds at padded 1.0), so a non-pow2
        // mirrored axis has a displaced seam. Pow2 extents (e.g. the 32x64 I4 machine
        // shadow) are exempt: padding is the identity and the seam sits at the logical
        // edge. See docs/research/shadow2-mirror-period-audit.md.
        C3D_TexSetWrap(&t.tex, WrapFromN64(cms), WrapFromN64(cmt));
    }
}

void GfxRenderingAPIC3D::DeleteTexture(uint32_t texId) {
    if (texId >= mTextures.size()) {
        return;
    }
    TextureC3D& t = mTextures[texId];
    t.serial = ++mTexSerial; /* n60 nrr_trk */
    if (t.page >= 0) {
        AtlasRelease(t); /* [trectbatch] */
    }
    if (t.inited) {
        C3D_TexDelete(&t.tex);
        t.inited = false;
    }
}

void GfxRenderingAPIC3D::SetTextureFilter(FilteringMode mode) {
    // Three-point filtering has no PICA equivalent; treated as linear.
    mTextureFilter = mode;
}

FilteringMode GfxRenderingAPIC3D::GetTextureFilter() {
    return mTextureFilter;
}

/* ------------------------------------------------------------------------------- */
/* Raster state                                                                     */
/* ------------------------------------------------------------------------------- */

void GfxRenderingAPIC3D::SetDepthTestAndMask(bool depthTest, bool zUpd) {
    mDepthTest = depthTest;
    mDepthMask = zUpd;
    // Reversed depth (near = 1): N64 LEQUAL becomes GEQUAL. Depth writes without a
    // test still need the unit enabled with an always-pass function (GL parity).
    C3D_DepthTest(depthTest || zUpd, depthTest ? GPU_GEQUAL : GPU_ALWAYS,
                  zUpd ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
}

void GfxRenderingAPIC3D::SetZmodeDecal(bool decal) {
    mZmodeDecal = decal;
    // Nudge decals toward the viewer so they win the coplanar depth test. Reversed
    // map (near = 1) + GPU_GEQUAL: a decal needs a LARGER window depth than the
    // surface it sits on, so the bias is POSITIVE (desktop uses glPolygonOffset(-2)
    // under GL's 0-near map + GL_LEQUAL — same "toward viewer" intent, opposite sign).
    //
    // Magnitude tuning: F-Zero X compresses the whole race into clip-depth [0.95, 1.0]
    // (window depth [0, 0.05]); the machine drop-shadow is a G_RM_ZB_XLU_DECAL projected
    // onto that band, so too small a bias z-fights/vanishes and too large lifts the
    // shadow off the track. The default 1/1024 is inside the band but unverified on
    // hardware. A "gdx-decalbias.txt" file at the SD root (first float = bias, read once)
    // overrides it so the magnitude/sign can be swept in a single verify session without
    // a rebuild. Delete the file to restore the default.
    static const float sDecalBias = [] {
        // 2026-08-20 night-verify emulator sweep: at the old 1/1024 (~0.00098) the
        // machine drop-shadow sat close to the z-fight edge; 0.004 and 0.008 both placed
        // it cleanly on the track (no z-fight shimmer, no lift). 0.004 is the safer mid
        // value (further from lift). Kept overridable via gdx-decalbias.txt for the
        // hardware pass, which is the real oracle for the final magnitude.
        float bias = 0.004f;
        FILE* f = fopen("gdx-decalbias.txt", "rb");
        if (f != nullptr) {
            float parsed = 0.0f;
            if (fscanf(f, "%f", &parsed) == 1) {
                bias = parsed;
            }
            fclose(f);
            GFX_C3D_LOG("[decalbias] override active: %.6f\n", bias);
        }
        return bias;
    }();
    C3D_DepthMap(true, -1.0f, decal ? sDecalBias : 0.0f);
}

uint32_t GfxRenderingAPIC3D::CurrentTargetFbHeight() const {
    // Extent of the target's long (fb-y) axis — the axis game-x runs along, negated.
    if (mCurrentFramebuffer >= 0 && mCurrentFramebuffer < (int)mFramebuffers.size()) {
        const FramebufferC3D& fb = mFramebuffers[mCurrentFramebuffer];
        if (fb.texBacked) {
            return fb.tex.height;
        }
    }
    return (uint32_t)kScreenWidth; // main target: 240x400 portrait
}

/* Coordinate mapping for the physically 90°-rotated top-screen framebuffer, matching
 * the fixup matrix (x' = y, y' = -x): the interpreter hands GL-convention rects
 * (origin bottom-left of the landscape game screen); in target space game-y runs
 * straight along fb-x while game-x runs along fb-y NEGATED, i.e. fb_y = fbH - x.
 * Harness-verified: the identity swap (fb_y = x) mirrored the scissor rect to the
 * wrong horizontal side (scene 4 yellow at top-right instead of top-left). */


static int VpFixEnabled() {
    static int sOn = -1;
    if (sOn < 0) {
        sOn = gdx3ds_config_get_int("debug", "vpfix", 1); /* 0 off, 1 on, 2 = on with zero offsets (diagnostic) */
        if (sOn < 0) { sOn = 0; }
    }
    return sOn;
}

void GfxRenderingAPIC3D::SetViewport(int x, int y, int width, int height) {
    const int fbH = (int)CurrentTargetFbHeight();
    /* COVERAGE: a distinct viewport rect is a distinct thing the frame draws into — 4P split
     * screen is four of them, and the number of distinct VP keys in a frame IS its viewport
     * count.
     *
     * Quantised to 32 px, and that number is load-bearing. Finer quantisations make a SLIDING
     * viewport mint a new feature on every frame of its animation, and the set cover then has
     * to sample animation frames one at a time: at 4 px it picked frame 1702 of the canonical
     * replay, which is the one frame with a DOCUMENTED non-deterministic render transient (the
     * course-name glyph; fhdiff --known 1702), and at 16 px it picked 30 CONSECUTIVE frames of
     * race-4p-vs's split-screen wipe (97 of that replay's 321 features were viewport rects
     * living 3 frames or fewer). A schedule is only useful if it is small and reproduces, so
     * the feature is "a viewport of roughly this shape here", not "exactly these pixels".
     * 32 px still separates all four 200x120 quadrants of the 4P split and the machine-select
     * grid's 100-px columns, which are what the family exists to catch. */
    gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_VP,
                                   ((uint32_t)((x >> 5) & 0x1F) << 15) | ((uint32_t)((y >> 5) & 0x1F) << 10) |
                                       ((uint32_t)((width >> 5) & 0x1F) << 5) | (uint32_t)((height >> 5) & 0x1F)));
    const bool texBacked = mCurrentFramebuffer >= 0 && mCurrentFramebuffer < (int)mFramebuffers.size() &&
                           mFramebuffers[mCurrentFramebuffer].texBacked;
    const bool inside = (x >= 0 && y >= 0 && x + width <= 400 && y + height <= 240);
    /* [vp] telemetry (verbose gate): non-full-window viewports logged with a frame stamp,
       only after 45 s of uptime (skips the intro), 400 lines max. */
    {
        static int sVpLogs = 0;
        static u64 sVpT0 = 0;
        /* [debug] verbose, LATCHED. gdx3ds_config_get_int linear-scans the whole ini table with
           two strcmp per entry; with verbose=0 sVpLogs never advances, so the unlatched form
           paid that scan on EVERY non-full-window SetViewport for the rest of the run (menus and
           split-screen: several per frame). Same class as audit 2026-09-23 finding 5. The latch
           is only ever taken past the 45 s mark, by which time the ini is long loaded. */
        static int sVpVerbose = -1;
        if (sVpT0 == 0) { sVpT0 = osGetTime(); }
        const bool full = (x == 0 && y == 0 && width == 400 && height == 240);
        if (!full && sVpLogs < 400 && (osGetTime() - sVpT0) > 45000) {
            if (sVpVerbose < 0) {
                sVpVerbose = gdx3ds_config_get_int("debug", "verbose", 0) != 0 ? 1 : 0;
            }
            if (sVpVerbose != 0) {
                [this, x, y, width, height, inside]() GDX_COLD { /* hotcold: [vp] receipt, out of line */
                ++sVpLogs;
                char line[160];
                const int n = snprintf(line, sizeof(line), "[vp] f=%lu viewport x=%d y=%d w=%d h=%d inside=%d\n",
                                       (unsigned long)mFrameIndex, x, y, width, height, (int)inside);
                if (n > 0 && &gdx3ds_filelog_write != nullptr) {
                    gdx3ds_filelog_write(line, (size_t)n);
                }
                }();
            }
        }
    }
    const bool fullWindow = (x == 0 && y == 0 && width == 400 && height == 240);
    /* Fold EVERY non-full-window viewport (inside ones too): a sub-viewport is menu 3D anchored
       to 2D artwork and must take the UI border treatment, which only the folded path gives. */
    if (!fullWindow && !texBacked && VpFixEnabled() != 0 && width > 0 && height > 0) {
        /* Fold the out-of-range rect into the projection: ndc' = ndc * s + t maps the
         * requested viewport's NDC onto the full window's NDC, per axis. */
        mVpAffineActive = true;
        mVpSx = (float)width / 400.0f;
        mVpTx = (2.0f * (float)x + (float)width - 400.0f) / 400.0f;
        /* The interpreter compresses clip x by the hor+ factor (4:3 content in the 5:3 window)
         * about the screen centre; the viewport's translation lives in the same compressed
         * space, so the offset takes the same factor (1.0 when widescreen is off / fixed). */
        /* [display] widescreen, VS race frames (kind 2): the views spread LINEARLY across the
         * 5:3 frame, as the PC port's CalcAndSetViewport places them, each view's content hor+
         * inside it; the split HUD anchors (WidescreenSplitUI) assume that layout. */
        if (&gdx_get_widescreen_geometry_xscale != nullptr && sDispWideKind != 2) {
            mVpTx *= gdx_get_widescreen_geometry_xscale();
        }
        mVpSy = (float)height / 240.0f;
        mVpTy = (2.0f * (float)y + (float)height - 240.0f) / 240.0f;
        if (VpFixEnabled() == 2) { mVpTx = 0.0f; mVpTy = 0.0f; } /* diagnostic: offsets off */
        if (VpFixEnabled() == 3) { mVpTx = -0.5f; mVpTy = 0.0f; } /* diagnostic: probe x = 25% */
        int vx0 = x < 0 ? 0 : x, vy0 = y < 0 ? 0 : y;
        int vx1 = x + width > 400 ? 400 : x + width, vy1 = y + height > 240 ? 240 : y + height;
        if (vx1 <= vx0) { vx0 = 0; vx1 = 1; }
        if (vy1 <= vy0) { vy0 = 0; vy1 = 1; }
        mVpVisX = vx0; mVpVisY = vy0; mVpVisW = vx1 - vx0; mVpVisH = vy1 - vy0;
        C3D_SetViewport(0u, 0u, 240u, 400u);
        sDispProjLast = nullptr; /* the next draw must re-upload with the affine folded in */
        if (mScValid) {
            ApplyScissorRect(mScX, mScY, mScW, mScH); /* re-intersect with the visible rect */
        }
        return;
    }
    if (mVpAffineActive) {
        mVpAffineActive = false;
        sDispProjLast = nullptr;
        if (mScValid) {
            ApplyScissorRect(mScX, mScY, mScW, mScH);
        }
    }
    int fbY = fbH - (x + width);
    if (fbY < 0) {
        fbY = 0;
    }
    C3D_SetViewport((u32)y, (u32)fbY, (u32)height, (u32)width);
}

void GfxRenderingAPIC3D::SetScissor(int x, int y, int width, int height) {
    mScX = x; mScY = y; mScW = width; mScH = height; mScValid = true;
    ApplyScissorRect(x, y, width, height);
}

void GfxRenderingAPIC3D::ApplyScissorRect(int x, int y, int width, int height) {
    if (mVpAffineActive) {
        /* [vpfix] N64 clipping happens against the viewport rect: intersect the scissor with
         * the rect's visible part so a full-screen GPU viewport cannot show more than the RSP. */
        int x0 = x > mVpVisX ? x : mVpVisX, y0 = y > mVpVisY ? y : mVpVisY;
        int x1 = (x + width) < (mVpVisX + mVpVisW) ? (x + width) : (mVpVisX + mVpVisW);
        int y1 = (y + height) < (mVpVisY + mVpVisH) ? (y + height) : (mVpVisY + mVpVisH);
        if (x1 <= x0) { x0 = 0; x1 = 1; }
        if (y1 <= y0) { y0 = 0; y1 = 1; }
        x = x0; y = y0; width = x1 - x0; height = y1 - y0;
    }
    const int fbH = (int)CurrentTargetFbHeight();
    const bool scTexBacked = mCurrentFramebuffer >= 0 &&
                             mCurrentFramebuffer < (int)mFramebuffers.size() &&
                             mFramebuffers[mCurrentFramebuffer].texBacked;
    if (DispModeLatched() > 0 && !scTexBacked) {
        /* MENU DISP: the projection scale magnifies content about the screen center,
         * so the game's safe-area scissor must open up by the same (scene) factors —
         * expansion only (factors >= 1), clamped to the screen: no draw that was
         * visible in authentic mode can be newly clipped. */
        const float sxS = sDispFrameFx[0], syS = sDispFrameFx[1];
        const float cx = 200.0f, cy = 120.0f;
        float x0 = ((float)x - cx) * sxS + cx;
        float y0 = ((float)y - cy) * syS + cy;
        float x1 = ((float)(x + width) - cx) * sxS + cx;
        float y1 = ((float)(y + height) - cy) * syS + cy;
        if (x0 < 0.0f) x0 = 0.0f;
        if (y0 < 0.0f) y0 = 0.0f;
        if (x1 > 400.0f) x1 = 400.0f;
        if (y1 > 240.0f) y1 = 240.0f;
        x = (int)(x0 + 0.5f);
        y = (int)(y0 + 0.5f);
        width = (int)(x1 - x0 + 0.5f);
        height = (int)(y1 - y0 + 0.5f);
    }
    int fbY1 = fbH - (x + width);
    int fbY2 = fbH - x;
    if (fbY1 < 0) {
        fbY1 = 0;
    }
    if (fbY2 < fbY1) {
        fbY2 = fbY1;
    }
    // C3D_SetScissor takes GPU coords: (left, top) = fb x1/y1, (right, bottom) = x2/y2.
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)y, (u32)fbY1, (u32)(y + height), (u32)fbY2);
}

void GfxRenderingAPIC3D::SetUseAlpha(bool useAlpha) {
    mUseAlpha = useAlpha;
    if (useAlpha) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE,
                       GPU_ONE_MINUS_SRC_ALPHA);
    } else {
        C3D_ColorLogicOp(GPU_LOGICOP_COPY); // disables blending
    }
}

void GfxRenderingAPIC3D::SetSrgbMode() {
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;
        GFX_C3D_LOG("SetSrgbMode ignored: PICA has no sRGB framebuffer support\n");
    }
    mSrgbMode = false;
}

ImTextureID GfxRenderingAPIC3D::GetTextureById(int id) {
    // ImGui is not part of the 3DS MVP; hand back the id for any diagnostics caller.
    return (ImTextureID)(uintptr_t)id;
}

void GfxRenderingAPIC3D::SetCurrentPrimDepth(float depth) {
    mCurrentPrimDepth = depth;
    mPrimDepthDirty = true; // consumed once prim-depth combiners land (post-census)
}

void GfxRenderingAPIC3D::SetCurrentAlphaCompareThreshold(float threshold) {
    mCurrentAlphaCompareThreshold = threshold;
    mAlphaCompareThresholdDirty = true;
}

/* ------------------------------------------------------------------------------- */
/* Draw                                                                             */
/* ------------------------------------------------------------------------------- */

/* hudsky: DrawTriangles' three latched switches, hoisted out of the function so the lean packed
 * tail (gfx_citro3d_hsx.inc) reads the same latch. Same first-use latching as before. */
#ifdef __3DS__
static bool GdxDiagSkyOn() {
    // Enable via env (desktop) OR the [debug] diag_sky INI key (see the [sky] block in DrawTriangles).
    static const bool sOn = [] {
        if (std::getenv("GDX_DIAG_SKY") != nullptr) {
            return true;
        }
        if (&gdx3ds_config_get_int != nullptr) {
            return gdx3ds_config_get_int("debug", "diag_sky", 0) != 0;
        }
        return false;
    }();
    return sOn;
}
#endif

// C2 diagnostic kill-switch: a "gdx-nofog.txt" file at the SD root disables the PICA fog unit.
static bool GdxNoFogFile() {
    static const bool sOn = [] {
        FILE* f = fopen("gdx-nofog.txt", "rb");
        if (f != nullptr) {
            fclose(f);
            return true;
        }
        return false;
    }();
    return sOn;
}

/* [anchor] killswitch [debug] stereo_anchor (default 1). */
static int GdxStereoAnchorOn() {
    static int sOn = -1;
    if (sOn < 0) {
        sOn = gdx3ds_config_get_int("debug", "stereo_anchor", 1) != 0 ? 1 : 0;
    }
    return sOn;
}

/* n60stereo frame replay: the draw is emitted ONCE for both eyes, with its stereo class as the
 * shader's stk row (the per-draw loop's classification: bridge override / ortho heuristic, [anchor]
 * prim-depth rects, texture-backed passes mono). citro3d uploads stk with the draw when it changed. */
int GfxRenderingAPIC3D::StereoReplayRow(bool texBacked, float clsW, bool anchoredDraw, const float* dst,
                                        size_t numVerts) {
    int cls = Gdx3dsStereo::ClassifyDraw(clsW == 1.0f);
    if (texBacked) {
        cls = GDX3DS_STEREO_UI_ZERO_PARALLAX;
    } else if (cls == GDX3DS_STEREO_UI_ZERO_PARALLAX && !anchoredDraw && Gdx3dsStereo::MenuFrame()) {
        cls = StereoMenuUiClass(dst, numVerts); /* mstereo [stereo] menu_depth: the draw's layer */
    } else if (cls == GDX3DS_STEREO_SCENE && Gdx3dsStereo::MenuFrame()) {
        Gdx3dsStereo::MenuNoteModel(); /* mstereo: flat at the menu model layer (ClassRow) */
    } else if (anchoredDraw) {
        cls = GDX3DS_STEREO_ANCHORED;
        Gdx3dsStereo::SetAnchorDepth(mCurrentPrimDepth);
        sAnchorDraws++;
        if (mCurrentPrimDepth < sAnchorDMin) {
            sAnchorDMin = mCurrentPrimDepth;
        }
        if (mCurrentPrimDepth > sAnchorDMax) {
            sAnchorDMax = mCurrentPrimDepth;
        }
    }
    float row[4];
    Gdx3dsStereo::ClassRow(cls, row);
    const int loc = Gdx3dsStereo::ReplayStkLoc();
    const C3D_FVec& cur = C3D_FVUnif[GPU_VERTEX_SHADER][loc];
    if (cur.x != row[0] || cur.y != row[1] || cur.z != row[2] || cur.w != row[3]) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, loc, row[0], row[1], row[2], row[3]);
    }
    if (!texBacked) {
        mFrameDrawsRightEye++;
    }
    return cls;
}

/* mstereo [stereo] menu_depth: the layer of a screen-space draw of a menu frame. Its packed triangles
 * are at `dst` (kOutStrideFloats, clip x y z w first); an indexed native draw or a gpuxf draw carries
 * no plain triangle list there and stays at the screen plane. */
GDX_COLD int GfxRenderingAPIC3D::StereoMenuUiClass(const float* dst, size_t numVerts) const {
    if (mXf != nullptr || mNativeActive) {
        return GDX3DS_STEREO_UI_ZERO_PARALLAX;
    }
    return Gdx3dsStereo::MenuUiClass(dst, (uint32_t)numVerts, (uint32_t)kOutStrideFloats);
}

static int GdxStereoCensusPeriod(); /* [debug] stereo_census, below */

/* mstereo [stereo] menu_depth: a menu screen-space draw whose rectangles change layer class
 * (Gdx3dsStereo::MenuUiRunEnd) is drawn run by run, each run at its own layer, both eyes, and the
 * main target re-bound. False (nothing drawn) when the draw is one run: the caller draws it whole.
 * Plain triangle lists only (no gpuxf, no indexed native draw). */
GDX_COLD bool GfxRenderingAPIC3D::StereoMenuUiRuns(const float* dst, size_t numVerts, const C3D_Mtx* dispBase) {
    if (mXf != nullptr || mNativeActive) {
        return false;
    }
    const uint32_t n = (uint32_t)numVerts;
    const uint32_t stride = (uint32_t)kOutStrideFloats;
    uint32_t end = Gdx3dsStereo::MenuUiRunEnd(dst, n, stride, 0u);
    if (end >= n) {
        return false;
    }
    C3D_RenderTarget* back = mFramebuffers[mCurrentFramebuffer].target;
    C3D_RenderTarget* mainTarget = back != nullptr ? back : mFramebuffers[0].target;
    uint32_t first = 0u;
    while (first < n) {
        const float* run = dst + (size_t)first * stride;
        const int cls = Gdx3dsStereo::MenuUiClass(run, end - first, stride);
        if (GDX_UNLIKELY(GdxStereoCensusPeriod() != 0)) {
            StereoCensusDraw(cls, run, end - first, dispBase, 'D');
        }
        StereoDrawRunBothEyes(cls, first, end - first, dispBase, mainTarget);
        first = end;
        if (first < n) {
            end = Gdx3dsStereo::MenuUiRunEnd(dst, n, stride, first);
        }
    }
    return true;
}

/* mstereo [stereo] menu_depth, stereo frame replay: a menu screen-space draw whose rectangles change
 * layer class is drawn here run by run -- the projection uploaded once, each run with its own
 * stk row -- and the caller skips its single emission. False (nothing drawn) when the draw is one
 * run, or while a native replay records (its capture is per emission). */
GDX_COLD bool GfxRenderingAPIC3D::StereoReplayMenuRuns(const float* dst, size_t numVerts, const C3D_Mtx* dispBase) {
    if (mXf != nullptr || mNativeActive || gdx3ds_replay_rec) {
        return false;
    }
    const uint32_t n = (uint32_t)numVerts;
    const uint32_t stride = (uint32_t)kOutStrideFloats;
    uint32_t end = Gdx3dsStereo::MenuUiRunEnd(dst, n, stride, 0u);
    if (end >= n) {
        return false;
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, dispBase);
    sDispProjLast = nullptr; /* dispBase may be a stack temporary: the next draw re-uploads */
    const int loc = Gdx3dsStereo::ReplayStkLoc();
    uint32_t first = 0u;
    while (first < n) {
        const float* run = dst + (size_t)first * stride;
        const int cls = Gdx3dsStereo::MenuUiClass(run, end - first, stride);
        if (GDX_UNLIKELY(GdxStereoCensusPeriod() != 0)) {
            StereoCensusDraw(cls, run, end - first, dispBase, 'R');
        }
        float row[4];
        Gdx3dsStereo::ClassRow(cls, row);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, loc, row[0], row[1], row[2], row[3]);
        C3D_DrawArrays(GPU_TRIANGLES, (int)first, (int)(end - first));
        first = end;
        if (first < n) {
            end = Gdx3dsStereo::MenuUiRunEnd(dst, n, stride, first);
        }
    }
    return true;
}

/* One run [first, first + count) of the bound VBO range in both eyes at class `cls`'s eye matrices;
 * leaves `mainTarget` bound. */
GDX_COLD void GfxRenderingAPIC3D::StereoDrawRunBothEyes(int cls, uint32_t first, uint32_t count,
                                                        const C3D_Mtx* dispBase, C3D_RenderTarget* mainTarget) {
    C3D_Mtx eyeMtx;
    Gdx3dsStereo::ComputeEyeMatrix(dispBase, -1, &eyeMtx, cls);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, (int)count);
    Gdx3dsStereo::BindTargetRaw(Gdx3dsStereo::RightTarget());
    Gdx3dsStereo::ComputeEyeMatrix(dispBase, 1, &eyeMtx, cls);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, (int)count);
    Gdx3dsStereo::BindTargetRaw(mainTarget);
}

/* mstereo [debug] stereo_census = N (default 0 = off): on every N-th frame with the right-eye pass
 * active, one `[stc]` receipt per legacy stereo draw -- what each menu element actually gets:
 *   [stc] f=<frame> m=<gGameMode> i=<draw> p=<D per-draw loop | R frame replay> c=<class> xf=<0|1>
 *         v=<verts> d=<NDC depth min>,<max> bb=<screen px x0>,<y0>,<x1>,<y1> s=<largest triangle extent px>
 *         px=<disparity min>,<max>
 * `px` is the on-screen disparity R - L in top-screen pixels (negative = in front of the screen) the
 * draw's stk row produces at this frame's slider/iod/convergence, through the draw's own projection
 * (DISP mode scale, a folded viewport). gpuxf draws are transformed on the CPU here (first 256
 * vertices) exactly as shader.v.pica does. Cold, and only reached from the stereo branches. */
static int GdxStereoCensusPeriod() {
    static int sPeriod = -1;
    if (sPeriod < 0) {
        sPeriod = 0;
        if (&gdx3ds_config_get_int != nullptr) {
            const int n = gdx3ds_config_get_int("debug", "stereo_census", 0);
            sPeriod = n > 0 ? n : 0;
        }
    }
    return sPeriod;
}

extern "C" int gGameMode __attribute__((weak));

GDX_COLD void GfxRenderingAPIC3D::StereoCensusDraw(int cls, const float* dst, size_t numVerts,
                                                   const C3D_Mtx* dispBase, char path) {
    const int period = GdxStereoCensusPeriod();
    if (period == 0 || (mFrameIndex % (uint32_t)period) != 0) {
        return;
    }
    static uint32_t sLastFrame = 0xFFFFFFFFu;
    static unsigned sDrawNo = 0;
    if (sLastFrame != mFrameIndex) {
        sLastFrame = mFrameIndex;
        sDrawNo = 0;
    }
    float row[4];
    Gdx3dsStereo::ClassRow(cls, row);
    float dMin = 1e30f, dMax = -1e30f, pMin = 1e30f, pMax = -1e30f;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    float triSize = 0.0f; /* the largest extent of one triangle (the menu layer's size, screen draws) */
    float tx0 = 0.0f, ty0 = 0.0f, tx1 = 0.0f, ty1 = 0.0f;
    const bool xf = mXf != nullptr;
    const size_t n = xf ? (mXf->nVerts < 256u ? mXf->nVerts : 256u) : (numVerts < 1024u ? numVerts : 1024u);
    for (size_t v = 0; v < n; v++) {
        float c[4];
        if (xf) {
            float ob[4];
            if (mXfPosFloat) {
                const float* fp = (const float*)((const uint8_t*)mXf->verts + v * 24u);
                ob[0] = fp[0];
                ob[1] = fp[1];
                ob[2] = fp[2];
            } else {
                const int16_t* sp = (const int16_t*)((const uint8_t*)mXf->verts + v * 16u);
                ob[0] = (float)sp[0];
                ob[1] = (float)sp[1];
                ob[2] = (float)sp[2];
            }
            ob[0] += mXf->off[0];
            ob[1] += mXf->off[1];
            ob[2] += mXf->off[2];
            ob[3] = 1.0f;
            float r1[4];
            for (int i = 0; i < 4; i++) {
                r1[i] = mXf->mvp[i][0] * ob[0] + mXf->mvp[i][1] * ob[1] + mXf->mvp[i][2] * ob[2] + mXf->mvp[i][3];
            }
            for (int i = 0; i < 4; i++) {
                c[i] = r1[i] * mXf->conv[i] + r1[3] * mXf->convw[i];
            }
        } else {
            const float* p = dst + v * kOutStrideFloats;
            c[0] = p[0];
            c[1] = p[1];
            c[2] = p[2];
            c[3] = p[3];
        }
        if (c[3] <= 1e-6f) {
            continue; /* behind the eye: clipped */
        }
        const float d = c[2] / c[3];
        const float ow = dispBase->r[3].x * c[0] + dispBase->r[3].y * c[1] + dispBase->r[3].z * c[2] +
                         dispBase->r[3].w * c[3];
        if (ow <= 1e-6f) {
            continue;
        }
        const float o0 = dispBase->r[0].x * c[0] + dispBase->r[0].y * c[1] + dispBase->r[0].z * c[2] +
                         dispBase->r[0].w * c[3];
        const float o1 = dispBase->r[1].x * c[0] + dispBase->r[1].y * c[1] + dispBase->r[1].z * c[2] +
                         dispBase->r[1].w * c[3];
        const float sx = (-o1 / ow + 1.0f) * 200.0f;
        const float sy = (1.0f - o0 / ow) * 120.0f;
        const float skew = row[0] * c[0] + row[1] * c[1] + row[2] * c[2] + row[3] * c[3];
        const float disp = 2.0f * 200.0f * (-dispBase->r[1].x) * skew / ow;
        dMin = d < dMin ? d : dMin;
        dMax = d > dMax ? d : dMax;
        pMin = disp < pMin ? disp : pMin;
        pMax = disp > pMax ? disp : pMax;
        x0 = sx < x0 ? sx : x0;
        x1 = sx > x1 ? sx : x1;
        y0 = sy < y0 ? sy : y0;
        y1 = sy > y1 ? sy : y1;
        if (!xf && !mNativeActive) {
            if (v % 3u == 0u) {
                tx0 = tx1 = sx;
                ty0 = ty1 = sy;
            } else {
                tx0 = sx < tx0 ? sx : tx0;
                tx1 = sx > tx1 ? sx : tx1;
                ty0 = sy < ty0 ? sy : ty0;
                ty1 = sy > ty1 ? sy : ty1;
            }
            if (v % 3u == 2u) {
                const float m = (tx1 - tx0) > (ty1 - ty0) ? (tx1 - tx0) : (ty1 - ty0);
                triSize = m > triSize ? m : triSize;
            }
        }
    }
    if (dMin > dMax) {
        dMin = dMax = pMin = pMax = x0 = y0 = x1 = y1 = 0.0f;
    }
    GFX_C3D_LOG("[stc] f=%lu m=%d i=%u p=%c c=%d xf=%d v=%u d=%.3f,%.3f bb=%d,%d,%d,%d s=%d px=%.1f,%.1f\n",
                (unsigned long)mFrameIndex, (&gGameMode != nullptr) ? gGameMode : -1, sDrawNo++, path, cls,
                xf ? 1 : 0, (unsigned)(xf ? mXf->nVerts : numVerts), (double)dMin, (double)dMax, (int)x0, (int)y0,
                (int)x1, (int)y1, (int)triSize, (double)pMin, (double)pMax);
}

void GfxRenderingAPIC3D::DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) {
    GDX3DS_PW_NOTE(GDX3DS_PW_VBO); /* o3ds3d [prewrite] */
    /* [prof] DRW: repack loop + state application + C3D submission. RAII so the
     * early-outs (no shader / stride mismatch / VBO exhaustion) still close the
     * section; the parent TRI/RUN scope subtracts this via the child counter. */
    struct ProfDrwScope {
        uint64_t t0 = 0, snap = 0;
        bool on;
        ProfDrwScope() : on(gdx3ds_prof_active != 0) {
            if (GDX_UNLIKELY(on)) {
                t0 = gdx3ds_prof_enter(&snap);
            }
        }
        ~ProfDrwScope() {
            if (GDX_UNLIKELY(on)) {
                gdx3ds_prof_exit(GDX3DS_PROF_DRW, t0, snap);
            }
        }
    } profDrwScope;
    /* C6 [replay]: the pose this draw's vertices were produced with (consumed even on an
     * early-out, so a dropped draw never lends its tag to the next one). */
    const uint16_t replayTag = gdx3ds_replay_rec ? gdx3ds_replay_take_draw_tag() : 0;
    if (replayTag == GDX_REPLAY_TAG_UNKNOWN && gdx3ds_replay_rec) {
        gdx3ds_replay_note_untagged(__builtin_return_address(0), sReplayOuterCaller, (unsigned)bufVboNumTris);
    }
    sReplayOuterCaller = nullptr;
    if (!mInitialized || !mFrameActive || bufVboNumTris == 0) {
        return;
    }
    if (mCurrentShader == nullptr) {
        GFX_C3D_LOG("DrawTriangles with no shader loaded — dropping %zu tris\n", bufVboNumTris);
        return;
    }
    const ShaderProgramC3D& prg = *mCurrentShader;
    /* [native] an indexed mesh draw carries its own vertex count (bufVboNumTris = nIdx/3). */
    const size_t numVerts = mNativeActive ? (size_t)mNativeIdx.nVerts : bufVboNumTris * 3;
    const size_t nativeIdxFloats = mNativeActive ? mNativeIdx.idxFloats : 0;
    const size_t inStride = bufVboLen / numVerts;
    if (mPackedAux == nullptr && inStride != prg.inputStride) {
        GFX_C3D_LOG("DrawTriangles stride mismatch: got %zu floats/vtx, shader %016llx/%016llx expects %u — "
                    "dropping %zu tris\n",
                    inStride, (unsigned long long)prg.id0, (unsigned long long)prg.id1, prg.inputStride,
                    bufVboNumTris);
        return;
    }
    /* [native] gpuxf: the raw vertices were reserved once per mesh (NativeXfVertices); a run
     * only consumes its index tail. */
    const size_t vtxPoolFloats = mXf != nullptr ? 0 : numVerts * kOutStrideFloats;
    if (GDX_UNLIKELY(mVboOffsetFloats + vtxPoolFloats + nativeIdxFloats > mVboPoolFloats)) {
        sVboDrawDropped++; /* GEOMLOSS: was logged once and counted nowhere */
        if (!mVboExhaustionLogged) {
            mVboExhaustionLogged = true;
            GFX_C3D_LOG("GEOMLOSS: VBO pool exhausted (%zu of %zu KiB usable) — DROPPING the rest of "
                        "this frame's geometry (%zu tris here); vboDrop on the [c3d] line counts them\n",
                        mVboPoolFloats * sizeof(float) / 1024, kVboPoolBytes / 1024, bufVboNumTris);
        }
        return;
    }

    // Bind up to two texture units (unit t ← tile t: the 2-cycle adjacent-tile
    // TEXEL1 modes get their own texture object per TMEM load, uploaded by the
    // interpreter through SelectTexture(1)/UploadTexture) and grab the UV rescale
    // factors for each unit's pow2 padding.
    float uScale[2] = { 1.0f, 1.0f };
    float vScale[2] = { 1.0f, 1.0f };
    // SKY-WEDGE-3 fix: content-edge UV clamp limits in PADDED-texture space (1e30 =
    // no clamp). CLAMP_TO_EDGE (bound below) clamps at the POW2-PADDED edge, but a
    // padded texture's content stops at uScale/vScale — everything past it is the
    // zeroed init padding (black, alpha 0). Any clamp-requested axis whose UVs
    // overshoot the content therefore sampled BLACK PADDING instead of the N64's
    // edge texel. The in-race skybox is the extreme case: its gradient strip is
    // 64x1 (padded 64x8, vScale = 1/8) and Background_UpdateSkyboxVtx leaves the
    // T axis mathematically wild (|T| up to ~600 texels — real N64 clamps every T
    // to the single row via the tile's lrt=0 window), so parts of the sky quad
    // sampled the padding rows: the hard-edged black wedge photographed on hardware,
    // reshaped by camera pitch/yaw as the ±32000 vertex clamp bends the corner T
    // values (non-planar corners also split the artifact along the quad diagonal —
    // the "weird texture" polygon edges). Pre-clamping the repacked UV to the last
    // content texel's CENTRE — (extent - 0.5) / paddedSize, the interpreter's own
    // clamp-limit formula — reproduces the N64 edge clamp under both filters and
    // keeps linear filtering from bleeding into the padding. The shader-clamp
    // (livery) case reads the interpreter's per-vertex limit instead, so
    // tile-window clamps narrower than the decoded texture stay exact too.
    // [debug] sky_clamp_fix=0 disables (A/B discriminator; default on).
    float uClampMax[2] = { 1e30f, 1e30f };
    float vClampMax[2] = { 1e30f, 1e30f };
    float uOffB[2] = { 0.0f, 0.0f }; /* [trectbatch] atlas view UV offsets (0 = standalone) */
    float vOffB[2] = { 0.0f, 0.0f };
    bool viewB[2] = { false, false };
    const bool sSkyClampFix = GdxSkyClampFixEnabled();
    /* [triloop] packed batch: the interpreter already emitted the final PICA
     * layout (UV scale + clamp applied at append time from VboPackBegin's
     * parameters, batch-constant by the flush invariant), so the repack loop
     * and the shader-clamp limit reads (variable-layout slots that no longer
     * exist) are skipped. Everything else — texture binds, stage constants
     * (from the aux record), alpha test, fog, submission — runs unchanged. */
    const bool packed = mPackedAux != nullptr;
    for (int tile = 0; tile < 2; tile++) {
        if (!prg.cc.usedTextures[tile]) {
            continue;
        }
        const uint32_t texId = mBoundTextureIds[tile];
        if (texId < mTextures.size() && TexReady(mTextures[texId])) {
            TextureC3D& t = mTextures[texId];
            uScale[tile] = t.uScale;
            vScale[tile] = t.vScale;
            /* [trectbatch] atlas VIEW: clamp limits stay in un-offset space (the repack adds
             * uOff after the min); the PAGE is bound with this view's filter. */
            const bool view = t.page >= 0;
            /* [atlasuv] a view clamps at its cell's far EDGE — see gdx3ds_view_clamp_limit. */
            const bool viewClamp = view && !GdxAtlasViewClampLegacy();
            uOffB[tile] = t.uOff;
            vOffB[tile] = t.vOff;
            viewB[tile] = view;
            if (sSkyClampFix) {
                // [triloop] packed batches applied both clamp families at append
                // time (VboPackBegin parameters); the shader-clamp reads below
                // index variable-layout slots that packed buffers do not carry,
                // so the derivation is legacy-only (uClampMax then only feeds
                // the [sky] diag line, which under packed reads the already-
                // clamped UVs directly).
                if (prg.cc.clamp[tile][0]) {
                    if (!packed) {
                        // Interpreter per-vertex clamp-S limit (draw-constant: vertex 0).
                        uClampMax[tile] = bufVbo[prg.uvOffset[tile] + 2] * t.uScale;
                    }
                } else if ((t.cms & G_TX_CLAMP) != 0) {
                    uClampMax[tile] = viewClamp ? gdx3ds_view_clamp_limit(t.width, t.padWidth)
                                      : gdx3ds_pad_clamp_needed(t.width, t.padWidth)
                                          ? gdx3ds_pad_clamp_limit(t.width, t.padWidth)
                                          : uClampMax[tile];
                }
                if (prg.cc.clamp[tile][1]) {
                    if (!packed) {
                        const int slot = prg.uvOffset[tile] + 2 + (prg.cc.clamp[tile][0] ? 1 : 0);
                        vClampMax[tile] = bufVbo[slot] * t.vScale;
                    }
                } else if ((t.cmt & G_TX_CLAMP) != 0) {
                    vClampMax[tile] = viewClamp ? gdx3ds_view_clamp_limit(t.height, t.padHeight)
                                      : gdx3ds_pad_clamp_needed(t.height, t.padHeight)
                                          ? gdx3ds_pad_clamp_limit(t.height, t.padHeight)
                                          : vClampMax[tile];
                }
            }
            // SHIP-LIVERY-2 fix: express the interpreter's SHADER-side clamp at the
            // sampler. Whenever a tile's N64 clamp cannot ride the plain wrap mode
            // (MIRROR|CLAMP, or a tile window narrower than the decoded texture),
            // the interpreter STRIPS G_TX_CLAMP from the flags it hands
            // SetSamplerParameters, sets the shader clamp[t][axis] feature and
            // appends per-vertex clamp limits for a fragment shader to consume —
            // which this backend cannot (the limits are skipped at repack; the old
            // "PICA clamps in hardware" note had it backwards). Exactly those draws
            // therefore sampled MIRRORED_REPEAT/REPEAT with no clamp at all, tiling
            // 16x16 decals across whole surfaces: the in-race player Blue Falcon
            // wore its OWN yellow stripe decal (authentic prim 255,255,0) repeated
            // ~7x over the hull ([livery] draw uv=(6.93,..) on aDecalStripeTex,
            // cms=MIRROR|CLAMP). Machine-select's high-LOD pieces keep their UVs
            // inside the first period, which is why that screen always looked
            // correct. CLAMP_TO_EDGE reproduces the N64 result here: the importers
            // bound the decoded extent to the tile window on clamp axes, and N64
            // clamps BEFORE the mask engages, so the mirror we drop never fires
            // inside the window either (negative coords clamp to the first column —
            // closer to hardware than desktop GL's MIRROR_CLAMP_TO_EDGE).
            // SHADOW-2 caveat: "the mirror never fires inside the window" holds only
            // while the tile window <= one mask period. A MIRROR|CLAMP tile whose
            // window EXCEEDS its mask period would mirror inside the window on N64
            // but edge-smear here. The machine drop-shadow (32x64, window == period,
            // gdx_gfx_shadow_uv_tests) and the livery decals (window narrower) are
            // both safe; no such over-wide tile is currently known in F-Zero X. If
            // one surfaces, the fix is a mirror-unfolded upload (pow2 unfold only).
            // See docs/research/shadow2-mirror-period-audit.md. Applied
            // per draw (two struct-field writes; C3D_DrawArrays emits the params
            // synchronously) so a texture shared between clamping and wrapping
            // draws binds correctly in both.
            const GPU_TEXTURE_WRAP_PARAM wrapS =
                prg.cc.clamp[tile][0] ? GPU_CLAMP_TO_EDGE : WrapFromN64(t.cms);
            const GPU_TEXTURE_WRAP_PARAM wrapT =
                prg.cc.clamp[tile][1] ? GPU_CLAMP_TO_EDGE : WrapFromN64(t.cmt);
            C3D_Tex* bindTex = view ? &mAtlasPages[(size_t)t.page].tex : &t.tex;
            if (view) {
                sAtlasPageLastUsed[(size_t)t.page] = (unsigned long)mFrameIndex; /* eviction LRU stamp */
            }
            if (view) {
                const GPU_TEXTURE_FILTER_PARAM vf = t.linearFilter ? GPU_LINEAR : GPU_NEAREST;
                C3D_TexSetFilter(bindTex, vf, vf);
            }
            C3D_TexSetWrap(bindTex, wrapS, wrapT);
            BindTexUnit(tile, bindTex);
            gdx3ds_gpuprof_note_tex_bind(texId); // S12: distinct textures touched/frame
        } else {
            /* DROPSITES site 4. Pre-fix this branch bound NOTHING, so the PICA texunit kept the
             * last texture bound to it and the draw sampled an unrelated image -- the same
             * stale-content failure as the texture cache's, one layer down. Bind the shared
             * white fallback instead: the primitive draws untextured, which is wrong but
             * DEFINED, and bindMiss= still counts every occurrence. */
            mFrameBindMisses++;
            if (mMissingTexInited) {
                BindTexUnit(tile, &mMissingTex);
                sTexMissingBinds++;
            }
            GFX_C3D_LOG("DROPSITE: shader wants texel%d but texture %u is not uploaded (bound the "
                        "white fallback: %d)\n",
                        tile, mBoundTextureIds[tile], mMissingTexInited ? 1 : 0);
        }
    }

    // Repack the interpreter's variable-stride vertices into the fixed PICA layout.
    float* dst = mVboPool + mVboOffsetFloats;
    const int uvOff0 = prg.uvOffset[0];
    const int uvOff1 = prg.uvOffset[1];
    const int colorRgbOff = prg.vtxColorInput > 0 ? prg.inputOffset[prg.vtxColorInput - 1] : -1;
    const int colorAlphaOff =
        (prg.inputSize == 4 && prg.vtxAlphaInput > 0) ? prg.inputOffset[prg.vtxAlphaInput - 1] + 3 : -1;
    // Per-vertex fog blend: the baked N64 fog factor rides GPU_PRIMARY_COLOR's alpha
    // so the appended INTERPOLATE stage can lerp fogColor by it (see MapCombiner).
    // fogBlendStage is only set when the primary-colour alpha slot is unread by the
    // mapped chain: opaque-alpha combiners, and translucent combiners whose alpha does
    // not source a per-vertex input (b.vtxAlpha == 0). Both leave colorAlphaOff == -1
    // (vtxAlphaInput == 0), so out[9] carries f rather than a real vertex alpha — the
    // combiner still emits the real translucency alpha through its own alpha chain.
    const int fogFactorOff = prg.fogBlendStage && prg.fogOffset >= 0 ? prg.fogOffset + 3 : -1;
    // [triloop] packed batches were written to dst (== bufVbo) by the
    // interpreter in the final layout — no repack.
    for (size_t v = 0; !packed && v < numVerts; v++) {
        const float* src = bufVbo + v * inStride;
        float* out = dst + v * kOutStrideFloats;
        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
        out[3] = src[3];
        if (uvOff0 >= 0 && viewB[0]) { /* [trectbatch] view: offset after the clamp */
            out[4] = fminf(src[uvOff0] * uScale[0], uClampMax[0]) + uOffB[0];
            out[5] = fminf(src[uvOff0 + 1] * vScale[0], vClampMax[0]) + vOffB[0];
        } else if (uvOff0 >= 0) {
            out[4] = fminf(src[uvOff0] * uScale[0], uClampMax[0]);
            out[5] = fminf(src[uvOff0 + 1] * vScale[0], vClampMax[0]);
        } else {
            out[4] = 0.0f;
            out[5] = 0.0f;
        }
        if (colorRgbOff >= 0) {
            out[6] = src[colorRgbOff];
            out[7] = src[colorRgbOff + 1];
            out[8] = src[colorRgbOff + 2];
        } else {
            out[6] = out[7] = out[8] = 1.0f;
        }
        out[9] = fogFactorOff >= 0 ? src[fogFactorOff] : (colorAlphaOff >= 0 ? src[colorAlphaOff] : 1.0f);
        if (uvOff1 >= 0 && viewB[1]) {
            out[10] = fminf(src[uvOff1] * uScale[1], uClampMax[1]) + uOffB[1];
            out[11] = fminf(src[uvOff1 + 1] * vScale[1], vClampMax[1]) + vOffB[1];
        } else if (uvOff1 >= 0) {
            out[10] = fminf(src[uvOff1] * uScale[1], uClampMax[1]);
            out[11] = fminf(src[uvOff1 + 1] * vScale[1], vClampMax[1]);
        } else {
            out[10] = 0.0f;
            out[11] = 0.0f;
        }
    }
    if (mXf == nullptr && UvDiagOn() && AtlasDiagInWindow() && prg.cc.usedTextures[0] && numVerts > 0 &&
        numVerts <= 12) {
        [this, dst, numVerts]() GDX_COLD { /* hotcold: [uvdiag] receipt, out of line */
        const uint32_t texId0 = mBoundTextureIds[0];
        const TextureC3D* td = (texId0 < mTextures.size()) ? &mTextures[texId0] : nullptr;
        const float padW = td != nullptr ? (float)td->padWidth : 0.0f;
        const float padH = td != nullptr ? (float)td->padHeight : 0.0f;
        const float ax = (td != nullptr && td->page >= 0) ? (float)td->ax : 0.0f;
        const float ay = (td != nullptr && td->page >= 0) ? (float)td->ay : 0.0f;
        float x0 = dst[0], x1 = dst[0], y0 = dst[1], y1 = dst[1];
        float s0 = 1e30f, s1 = -1e30f, t0 = 1e30f, t1 = -1e30f;
        uint32_t bs = 0, bt = 0;
        for (size_t v = 0; v < numVerts; v++) {
            const float* o = dst + v * kOutStrideFloats;
            x0 = fminf(x0, o[0]); x1 = fmaxf(x1, o[0]);
            y0 = fminf(y0, o[1]); y1 = fmaxf(y1, o[1]);
            const float sc = o[4] * padW - ax;
            const float tc = o[5] * padH - ay;
            if (sc < s0) { s0 = sc; bs = UvDiagBits(o[4]); }
            if (sc > s1) { s1 = sc; }
            if (tc < t0) { t0 = tc; bt = UvDiagBits(o[5]); }
            if (tc > t1) { t1 = tc; }
        }
        GFX_C3D_LOG("[uvdiag] f=%lu pg=%d id=%u wh=%ux%u pad=%.0fx%.0f a=%.0f,%.0f "
                    "xy=%.4f,%.4f,%.4f,%.4f cs=%.6f,%.6f ct=%.6f,%.6f b=%08lx,%08lx nv=%u\n",
                    (unsigned long)mFrameIndex, td != nullptr ? (int)td->page : -2, texId0,
                    td != nullptr ? (unsigned)td->width : 0u, td != nullptr ? (unsigned)td->height : 0u,
                    padW, padH, ax, ay, x0, y0, x1, y1, s0, s1, t0, t1,
                    (unsigned long)bs, (unsigned long)bt, (unsigned)numVerts);
        }();
    }
    /* [native] machines_verify: hand the final packed stream + material identity to the
     * mesh verifier; in capture-only mode stop here (no GPU state, no submit, no advance). */
    if (sMeshCaptureFn != nullptr && mXf == nullptr) {
        const bool captureSkip = [this, &prg, dst, numVerts, bufVboNumTris]() GDX_COLD -> bool {
        Gdx3dsMeshCapture cap;
        cap.verts = dst;
        cap.nVerts = (uint32_t)numVerts;
        cap.idx = mNativeActive ? mNativeIdx.idx : nullptr;
        cap.nIdx = mNativeActive ? mNativeIdx.nIdx : 0;
        cap.numTris = (uint32_t)bufVboNumTris;
        cap.shaderId0 = prg.id0;
        cap.shaderId1 = prg.id1;
        cap.texId[0] = prg.cc.usedTextures[0] ? mBoundTextureIds[0] : 0xFFFFFFFFu;
        cap.texId[1] = prg.cc.usedTextures[1] ? mBoundTextureIds[1] : 0xFFFFFFFFu;
        cap.aux = mPackedAux;
        cap.vtxColorInput = (int8_t)prg.vtxColorInput;
        cap.vtxAlphaInput = (int8_t)prg.vtxAlphaInput;
        sMeshCaptureFn(&cap, sMeshCaptureUser);
        return sMeshCaptureSkip;
        }();
        if (captureSkip) {
            return;
        }
    }
    // No per-draw cache flush: FlushPendingVbo() flushes the whole appended range in
    // one svc right before submission (EndFrame / FrameSplit).

#ifdef __3DS__
    // [sky] SKY/BACKDROP coverage diagnostic (GDX_DIAG_SKY=1, race-gated, ~few lines/frame).
    // The reported "black triangular wedge" in the distant sky is the framebuffer clear
    // colour (0x000000FF, opaque black — see C3D_RenderTargetClear) showing through ABOVE
    // the backdrop skybox quad: the quad is built at 4:3 logical proportions in
    // Background_UpdateSkyboxVtx (fovScaleX/Y = 320/240, aspectRatio 0.75) but the 3DS
    // top screen is 400x240 (~16:10) after the portrait fixup, so the quad's top edge falls
    // short of the screen edge. It is NOT a fog regression — the flat-pink sky IS the
    // (correct, N64-faithful) per-vertex fog blend to fogColor; this diagnostic only proves
    // whether a far-depth draw reaches the top screen edge. The fixup maps screen_y' = -clip_x
    // (see mFixupMatrix: r0.y=1, r1.x=-1), so post-fixup screen NDC y = -src[0]/src[3].
    // A far-depth draw whose |screen NDC| max stays below ~0.99 at any edge leaves clear
    // colour there. Skybox signature: 2 tris, textured, all verts at far depth (z/w -> +1).
    {
        // Enable via env (desktop) OR the [debug] diag_sky INI key (3DS: ctru getenv does
        // not see azahar's host environment, so env-only gates are unreachable on hardware
        // and in the emulator — mirror the malloc-histogram "env wins over INI" bridge in
        // main_3ds.cpp). gdx3ds_config_get_int is weak so the gfx backend still links into
        // the DL-test harness (which has no config target), exactly as gdx3ds_gpu_prof.c does.
        const bool sDiagSky = GdxDiagSkyOn(); /* hudsky: hoisted so the lean tail can test it */
        const bool raceActiveSky = (&gGdxRaceActive != nullptr) && (gGdxRaceActive != 0);
        if (sDiagSky && raceActiveSky && bufVboNumTris == 2 && mXf == nullptr) {
            const float vScale0 = vScale[0];
            const float vClampMax0 = vClampMax[0];
            [this, &prg, bufVbo, numVerts, inStride, uvOff0, packed, vScale0, vClampMax0]() GDX_COLD {
            float sxMin = 1e9f, sxMax = -1e9f, syMin = 1e9f, syMax = -1e9f;
            float zwMin = 1e9f, zwMax = -1e9f;
            float pvMin = 1e9f, pvMax = -1e9f; // PADDED-space V (post-vScale, pre-fminf)
            bool finite = true;
            for (size_t v = 0; v < numVerts; v++) {
                const float* src = bufVbo + v * inStride;
                const float w = src[3];
                if (!(w > 1e-6f)) { finite = false; break; }
                const float sx = src[1] / w;   // screen NDC x = clip_y / w
                const float sy = -src[0] / w;  // screen NDC y = -clip_x / w
                const float zw = src[2] / w;   // clip depth ratio (far -> +1)
                if (sx < sxMin) sxMin = sx; if (sx > sxMax) sxMax = sx;
                if (sy < syMin) syMin = sy; if (sy > syMax) syMax = sy;
                if (zw < zwMin) zwMin = zw; if (zw > zwMax) zwMax = zw;
                if (uvOff0 >= 0) {
                    // [triloop] packed layout carries the post-scale (and
                    // post-clamp) V at slot 5.
                    const float pv = packed ? src[5] : src[uvOff0 + 1] * vScale0;
                    if (pv < pvMin) pvMin = pv;
                    if (pv > pvMax) pvMax = pv;
                }
            }
            // Only far-depth quads (the backdrop skybox/floor sit at z/w near the far end).
            if (finite && zwMin > 0.80f) {
                static int sSkyLines = 0;
                if (sSkyLines < 240) {
                    ++sSkyLines;
                    // SKY-WEDGE-3 receipts: tex0 identity (content vs pow2-padded extent),
                    // the padded-space V range this draw would sample WITHOUT the clamp
                    // limit, and the limit applied. Content ends at vScale; any pv beyond
                    // vClamp means the unfixed backend sampled black padding — the wedge.
                    unsigned tw = 0, th = 0, tpw = 0, tph = 0;
                    const uint32_t skyTexId = mBoundTextureIds[0];
                    if (prg.cc.usedTextures[0] && skyTexId < mTextures.size()) {
                        tw = mTextures[skyTexId].width;
                        th = mTextures[skyTexId].height;
                        tpw = mTextures[skyTexId].padWidth;
                        tph = mTextures[skyTexId].padHeight;
                    }
                    GFX_C3D_LOG("[sky] tris=2 tex=%d zw=[%.3f,%.3f] ndcX=[%.3f,%.3f] "
                                "ndcY=[%.3f,%.3f] topGap=%.3f botGap=%.3f fogBlend=%d "
                                "tex0=%ux%u pad=%ux%u pv=[%.3f,%.3f] vClamp=%.4f\n",
                                (int)prg.cc.usedTextures[0], zwMin, zwMax, sxMin, sxMax,
                                syMin, syMax, 1.0f - syMax, 1.0f + syMin,
                                (int)prg.fogBlendStage, tw, th, tpw, tph, pvMin, pvMax,
                                vClampMax0);
                }
            }
            }();
        }
    }
#endif

    const bool xfp = mXf != nullptr && gdx3ds_xfprof_inrun != 0; /* [debug] xfprof (c5mach2) */
    if (GDX_UNLIKELY(xfp)) {
        gdx3ds_xfprof_mark(XFP_B_TEX, svcGetSystemTick());
    }
    const uint32_t constReappliedMask =
        packed ? RefreshStageConstantsPacked(prg, *mPackedAux) : RefreshStageConstants(prg, bufVbo);
    if (GDX_UNLIKELY(xfp)) {
        gdx3ds_xfprof_mark(XFP_B_CONST, svcGetSystemTick());
    }

    // [livery] draw receipt (one-shot window, see GdxLiveryDiagFrameBegin): EVERY
    // textured draw's bound texture identities + first-vertex unit-0 UV + the
    // input-driven stage constant when one is bound (machine bodies: ENV colour,
    // r in the LOW byte). The texture names resolve through the id→name table the
    // import hook maintains, so warm-cache binds are named too.
    if (sLiveryArmed && mXf == nullptr && (prg.cc.usedTextures[0] || prg.cc.usedTextures[1])) {
        [this, &prg, bufVbo, bufVboNumTris, uvOff0, packed]() GDX_COLD {
        ++sLiveryDrawIndex;
        if (GdxLiveryTakeLine()) {
            char tbuf[2][64];
            for (int t = 0; t < 2; t++) {
                if (!prg.cc.usedTextures[t]) {
                    tbuf[t][0] = '\0';
                    continue;
                }
                const uint32_t id = mBoundTextureIds[t];
                unsigned w = 0, h = 0;
                if (id < mTextures.size()) {
                    w = mTextures[id].width;
                    h = mTextures[id].height;
                }
                std::snprintf(tbuf[t], sizeof(tbuf[t]), " t%d=%lu:%ux%u:%s", t, (unsigned long)id,
                              w, h, GdxLiveryNameForId(id));
            }
            // [triloop] packed layout: unit-0 UV at fixed slots 4/5 (post-scale).
            const float u0 = uvOff0 >= 0 ? bufVbo[packed ? 4 : uvOff0] : 0.0f;
            const float v0 = uvOff0 >= 0 ? bufVbo[packed ? 5 : uvOff0 + 1] : 0.0f;
            char msg[224];
            int n;
            if (mLastDrawHadInputConst) {
                n = std::snprintf(msg, sizeof(msg),
                                  "[livery] draw #%u tris=%u%s%s uv=(%.2f,%.2f) c0=%08lx",
                                  sLiveryDrawIndex, (unsigned)bufVboNumTris, tbuf[0], tbuf[1], u0,
                                  v0, (unsigned long)mLastInputConstPacked);
            } else {
                n = std::snprintf(msg, sizeof(msg), "[livery] draw #%u tris=%u%s%s uv=(%.2f,%.2f)",
                                  sLiveryDrawIndex, (unsigned)bufVboNumTris, tbuf[0], tbuf[1], u0,
                                  v0);
            }
            if (n > 0) {
                svcOutputDebugString(msg, (size_t)n);
            }
        }
        }();
    }

    // [prim] draw receipt (one-shot window, see GdxPrimDiagFrameBegin): every draw
    // whose shader consumes an input-driven stage constant — machine bodies bind
    // their ENV body colour here (c0), so the player's body draw must log blue.
    if (sPrimDiagArmed && mLastDrawHadInputConst) {
        [this, &prg, bufVboNumTris, constReappliedMask]() GDX_COLD {
        ++sPrimDiagDrawIndex;
        if (GdxPrimDiagTakeLine()) {
            unsigned texW = 0, texH = 0;
            if (prg.cc.usedTextures[0] && mBoundTextureIds[0] < mTextures.size()) {
                texW = mTextures[mBoundTextureIds[0]].width;
                texH = mTextures[mBoundTextureIds[0]].height;
            }
            char msg[128];
            int n = std::snprintf(msg, sizeof(msg),
                                  "[prim] draw #%u id0=%08lx tris=%u tex0=%ux%u c0=%08lx re=%lx",
                                  sPrimDiagDrawIndex, (unsigned long)(uint32_t)prg.id0,
                                  (unsigned)bufVboNumTris, texW, texH,
                                  (unsigned long)mLastInputConstPacked,
                                  (unsigned long)constReappliedMask);
            if (n > 0) {
                svcOutputDebugString(msg, (size_t)n);
            }
        }
        }();
    }

    ApplyAlphaTest();

    // C2 diagnostic kill-switch: a "gdx-nofog.txt" file at the SD root disables the
    // PICA fog unit entirely, discriminating fog-unit output from combiner/shade
    // output in race captures. Checked once; delete the file to restore fog.
    const bool sGdxNoFog = GdxNoFogFile(); /* hudsky: hoisted so the lean tail shares it */
    // Per-vertex fog blend (prg.fogBlendStage) runs entirely in the TexEnv chain and
    // must NOT also drive the depth-indexed hardware fog unit — that unit is the
    // original bug (F-Zero X's [0.95,1.0] clip-depth band saturates its LUT to full
    // fog). Keep UpdateFogState only for the fallback fog draws the blend can't take
    // (opt_alpha fog). gdx-nofog.txt still forces the hardware unit fully off.
    if (!sGdxNoFog && prg.cc.opt_fog && prg.fogOffset >= 0 && !prg.fogBlendStage) {
        UpdateFogState(prg, bufVbo, numVerts, packed ? kOutStrideFloats : inStride, mPackedAux);
    } else {
        DisableFog();
    }

    if (GDX_UNLIKELY(xfp)) {
        gdx3ds_xfprof_mark(XFP_B_ATFOG, svcGetSystemTick());
    }
    const bool texBacked = mCurrentFramebuffer >= 0 && mCurrentFramebuffer < (int)mFramebuffers.size() &&
                           mFramebuffers[mCurrentFramebuffer].texBacked;

    /* [native] every per-eye/mono submission below goes through this: an indexed mesh
     * draw issues C3D_DrawElements over the same bound VBO range, arrays otherwise. */
    const auto IssueDraw = [&]() {
        GdxHwsScope hws(GDX3DS_HWS_DRAW); /* hwprof [hwsite] draw */
        const u32 hsxW0 = gpuCmdBufOffset; /* [debug] hsx_gpuhash */
        if (mNativeActive) {
            C3D_DrawElements(GPU_TRIANGLES, (int)mNativeIdx.nIdx, C3D_UNSIGNED_SHORT, mNativeIdx.idx);
        } else {
            C3D_DrawArrays(GPU_TRIANGLES, 0, (int)numVerts);
        }
        if (HsxGpuHashOn()) {
            HsxGpuHashFold(hsxW0, mXf == nullptr ? dst : nullptr, mXf == nullptr ? vtxPoolFloats + nativeIdxFloats : 0);
        }
    };

    if (gdx3ds_verify_mode != 0) { /* [nverify] one record per submitted batch; compare pass never draws */
        const bool verifyNoDraw = [this, &prg, dst, numVerts, bufVboNumTris, packed]() GDX_COLD -> bool {
        uint32_t f[kVfyFields] = {};
        uint32_t h = 2166136261u;
        f[0] = VfyFnv(h, &prg.id0, 8);
        f[1] = VfyFnv(h, &prg.id1, 8);
        for (int tile = 0; tile < 2; tile++) {
            uint32_t th = 2166136261u;
            if (prg.cc.usedTextures[tile]) {
                const uint32_t texId = mBoundTextureIds[tile];
                const uint32_t ch = texId < sVfyTexHash.size() ? sVfyTexHash[texId] : 0u;
                th = VfyFnv(th, &ch, 4);
                if (texId < mTextures.size()) {
                    const TextureC3D& tt = mTextures[texId];
                    const uint32_t sm[4] = { tt.cms, tt.cmt, tt.linearFilter ? 1u : 0u, tt.page >= 0 ? 1u : 0u };
                    th = VfyFnv(th, sm, sizeof(sm));
                }
            }
            f[2 + tile] = th;
        }
        const uint32_t mode[4] = { mDepthTest ? 1u : 0u, mDepthMask ? 1u : 0u, mZmodeDecal ? 1u : 0u, mUseAlpha ? 1u : 0u };
        f[4] = VfyFnv(h, mode, sizeof(mode));
        f[5] = VfyFnv(h, &mCurrentPrimDepth, 4);
        f[6] = VfyFnv(h, &mCurrentAlphaCompareThreshold, 4);
        /* The fold floats / visible rect are GPU-relevant only while the affine fold is active;
           inactive they hold the last folded viewport of whichever pass ran last. */
        const float vp[5] = { mVpAffineActive ? 1.0f : 0.0f, mVpAffineActive ? mVpSx : 0.0f,
                              mVpAffineActive ? mVpSy : 0.0f, mVpAffineActive ? mVpTx : 0.0f,
                              mVpAffineActive ? mVpTy : 0.0f };
        f[7] = VfyFnv(h, vp, sizeof(vp));
        const int vis[4] = { mVpAffineActive ? mVpVisX : 0, mVpAffineActive ? mVpVisY : 0,
                             mVpAffineActive ? mVpVisW : 0, mVpAffineActive ? mVpVisH : 0 };
        f[8] = VfyFnv(h, vis, sizeof(vis));
        const int sc[5] = { mScX, mScY, mScW, mScH, mScValid ? 1 : 0 };
        f[9] = VfyFnv(h, sc, sizeof(sc));
        f[10] = packed ? VfyFnv(h, mPackedAux, sizeof(Gdx3dsVboPackAux)) : 0u;
        const uint32_t nv = static_cast<uint32_t>(numVerts);
        f[11] = VfyFnv(h, &nv, 4);
        f[12] = packed ? 1u : 0u;
        uint32_t state = 2166136261u;
        for (int i = 0; i < kVfyFields; i++) state = (state ^ f[i]) * 16777619u;
        const uint32_t vboHash = VfyFnv(2166136261u, dst, numVerts * kOutStrideFloats * sizeof(float));
        VfyBatchInfo info;
        memcpy(info.v0, dst, sizeof(info.v0));
        info.tris = static_cast<uint32_t>(bufVboNumTris);
        info.id0 = prg.id0;
        info.id1 = prg.id1;
        info.tri = sVfyLastTri;
        for (int tile = 0; tile < 2; tile++) {
            const uint32_t texId = mBoundTextureIds[tile];
            info.texUsed[tile] = prg.cc.usedTextures[tile] ? 1u : 0u;
            info.texId[tile] = texId;
            info.texHash[tile] = texId < sVfyTexHash.size() ? sVfyTexHash[texId] : 0u;
            info.texW[tile] = texId < mTextures.size() ? mTextures[texId].width : 0u;
            info.texH[tile] = texId < mTextures.size() ? mTextures[texId].height : 0u;
            info.texSeq[tile] = texId < sVfyTexSeq.size() ? sVfyTexSeq[texId] : 0u;
            info.texEnds[tile] = (texId * 2u + 1u) < sVfyTexEnds.size() ? sVfyTexEnds[texId * 2u + 1u] : 0u;
            info.texEnds0[tile] = (texId * 2u) < sVfyTexEnds.size() ? sVfyTexEnds[texId * 2u] : 0u;
        }
        info.seqNow = sVfyUploadSeq;
        memcpy(info.vpf, vp, sizeof(info.vpf));
        memcpy(info.vis, vis, sizeof(info.vis));
        VfyRecord(1, state, vboHash, f, &info);
        return gdx3ds_verify_mode == 2;
        }();
        if (verifyNoDraw) {
            return;
        }
    }
    if (mXf != nullptr) {
        /* [native] gpuxf: the raw 16-byte N64 Vtx (ob s16x4, tc s16x2, cn u8x4) and the
         * shader's gpuxf proc; NativeXfDraw restores the legacy layout after the submit. */
        XfBindVertexBuffer(mXf->verts, mXfPosFloat ? 24 : 16);
        if (!mXfStateOn || mXfAttrF != mXfPosFloat) {
            /* [native] track_retained: its near-plane polygons carry float3 positions */
            C3D_SetAttrInfo(mXfPosFloat ? &mAttrXfF : &mAttrXf);
            mXfAttrF = mXfPosFloat;
        }
        if (!mXfStateOn) {
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, mXfBoolLoc, true);
            mXfStateOn = true;
        }
        XfUploadUniforms(*mXf);
        if (gdx3ds_replay_rec && gdx3ds_replay_xf_needs_pose(replayTag)) {
            ReplayDirtyXfPose(); /* C6: a new pose re-emits xmvp, the lights and xoff */
        }
        XfSetCull(mXf->cullMode);
        if (GDX_UNLIKELY(xfp)) {
            gdx3ds_xfprof_mark(XFP_B_XF, svcGetSystemTick());
        }
    } else {
        if (mXfStateOn) {
            XfRestore();
        }
        C3D_BufInfo* bufInfo = C3D_GetBufInfo();
        BufInfo_Init(bufInfo);
        BufInfo_Add(bufInfo, dst, kOutStrideFloats * sizeof(float), 4, 0x3210);
        mBufLastVerts = nullptr;
    }
    /* Stereo / DISP classification input: the first vertex's packed clip w (a gpuxf draw has
     * no clip-space vertex in the buffer; the mesh path hands the CPU value of the same one). */
    const float clsW = mXf != nullptr ? mXf->clsW : bufVbo[3];
    /* MENU DISP: pick this draw's projection variant (scene vs UI, see the border-mode
     * block above). Texture-backed passes and mode 0 keep the plain fixup. */
    /* [anchor] killswitch [debug] stereo_anchor (default 1). */
    const int sAnchorOn = GdxStereoAnchorOn(); /* hudsky: hoisted so the lean tail shares it */
    const bool anchoredDraw = sAnchorOn != 0 && !texBacked && prg.cc.opt_prim_depth && clsW == 1.0f;
    const C3D_Mtx* dispBase = &mFixupMatrix;
    if (!texBacked && DispModeLatched() > 0) {
        const int dispCls = Gdx3dsStereo::ClassifyDraw(clsW == 1.0f);
        dispBase = (dispCls == GDX3DS_STEREO_UI_ZERO_PARALLAX || anchoredDraw) ? &sDispProjUi : &sDispProjScene;
    }
    C3D_Mtx vpMtx;
    if (mVpAffineActive && !texBacked) {
        /* [vpfix] apply the viewport affine on the game's clip x/y BEFORE the fixup/border
         * scale: M'[i].x = M[i].x*sx, M'[i].y = M[i].y*sy, M'[i].w += M[i].x*tx + M[i].y*ty.
         * A folded viewport is menu 3D anchored to 2D artwork (machine grid, ship detail), so it
         * takes the border mode's UI treatment, never the scene magnification. */
        if (!texBacked && DispModeLatched() > 0) {
            dispBase = &sDispProjUi;
        }
        vpMtx = *dispBase;
        for (int i = 0; i < 4; i++) {
            const float mx = dispBase->r[i].x, my = dispBase->r[i].y;
            vpMtx.r[i].x = mx * mVpSx;
            vpMtx.r[i].y = my * mVpSy;
            vpMtx.r[i].w = dispBase->r[i].w + mx * mVpTx + my * mVpTy;
        }
        dispBase = &vpMtx;
        sDispProjLast = nullptr; /* never dedupe against a stack temporary */
    }
    bool replayRunsDrawn = false; /* mstereo: a menu batch drawn run by run (StereoReplayMenuRuns) */
    if (mStereoEnabled && Gdx3dsStereo::ReplayActive()) {
        /* n60stereo: one emission, the class row */
        const int replayCls = StereoReplayRow(texBacked, clsW, anchoredDraw, dst, numVerts);
        if (GDX_UNLIKELY(replayCls == GDX3DS_STEREO_MENU_UI || replayCls == GDX3DS_STEREO_UI_ZERO_PARALLAX) &&
            Gdx3dsStereo::MenuFrame() && !texBacked && !anchoredDraw) {
            replayRunsDrawn = StereoReplayMenuRuns(dst, numVerts, dispBase);
        }
        if (GDX_UNLIKELY(GdxStereoCensusPeriod() != 0) && !replayRunsDrawn) {
            StereoCensusDraw(replayCls, dst, numVerts, dispBase, 'R');
        }
    }
    if (mStereoEnabled && Gdx3dsStereo::Active() && !Gdx3dsStereo::ReplayActive()) {
        if (gdx3ds_replay_rec) {
            gdx3ds_replay_frame_bad(GDX_REPLAY_BAD_STEREO); /* C6: one eye's replay only */
        }
        /* Per-eye draw loop (stereo foundation): re-issue the SAME repacked VBO
         * range per eye — only the target binding and the projection uniform
         * change (no CPU vertex re-copy; see gdx3ds_stereo.h). Offscreen
         * (texture-backed) passes render once, mono, at the center matrix. */
        C3D_Mtx eyeMtx;
        if (texBacked) {
            if (GDX_UNLIKELY(GdxStereoCensusPeriod() != 0)) {
                StereoCensusDraw(GDX3DS_STEREO_UI_ZERO_PARALLAX, dst, numVerts, dispBase, 'D');
            }
            Gdx3dsStereo::ComputeEyeMatrix(&mFixupMatrix, 0, &eyeMtx);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
            IssueDraw();
        } else {
            // Stereo class: bridge override, else ortho heuristic (clip w == 1
            // on vertex 0 -> HUD/menu content at zero parallax).
            int cls = Gdx3dsStereo::ClassifyDraw(clsW == 1.0f);
            bool menuRunsDrawn = false;
            if (anchoredDraw) {
                /* [anchor] a prim-depth ortho draw (race position markers: the game sets the
                 * machine's projected depth as prim depth before each marker rect) sits at that
                 * machine's depth instead of the screen plane. */
                cls = GDX3DS_STEREO_ANCHORED;
                Gdx3dsStereo::SetAnchorDepth(mCurrentPrimDepth);
                sAnchorDraws++;
                if (mCurrentPrimDepth < sAnchorDMin) {
                    sAnchorDMin = mCurrentPrimDepth;
                }
                if (mCurrentPrimDepth > sAnchorDMax) {
                    sAnchorDMax = mCurrentPrimDepth;
                }
            } else if (cls == GDX3DS_STEREO_UI_ZERO_PARALLAX && Gdx3dsStereo::MenuFrame()) {
                /* mstereo [stereo] menu_depth: the draw's layer -- per run when its rectangles
                 * change layer class (StereoMenuUiRuns draws it, run by run) */
                menuRunsDrawn = StereoMenuUiRuns(dst, numVerts, dispBase);
                if (!menuRunsDrawn) {
                    cls = StereoMenuUiClass(dst, numVerts);
                }
            } else if (cls == GDX3DS_STEREO_SCENE && Gdx3dsStereo::MenuFrame()) {
                Gdx3dsStereo::MenuNoteModel(); /* mstereo: flat at the menu model layer (ComputeEyeMatrix) */
            }
            if (GDX_UNLIKELY(GdxStereoCensusPeriod() != 0) && !menuRunsDrawn) {
                StereoCensusDraw(cls, dst, numVerts, dispBase, 'D');
            }
            if (menuRunsDrawn) {
                /* drawn run by run, both eyes, the main target re-bound */
            } else if (cls == GDX3DS_STEREO_UI_ZERO_PARALLAX) {
                // HUD/2D at screen depth: one center matrix for both eyes —
                // vertex uniforms survive the raw target switch, so upload once.
                Gdx3dsStereo::ComputeEyeMatrix(dispBase, 0, &eyeMtx);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
                IssueDraw();
                Gdx3dsStereo::BindTargetRaw(Gdx3dsStereo::RightTarget());
                IssueDraw();
            } else {
                // Left eye: the current (main) target is already bound.
                Gdx3dsStereo::ComputeEyeMatrix(dispBase, -1, &eyeMtx, cls);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
                IssueDraw();
                // Right eye: raw switch keeps viewport/scissor (gdx3ds_stereo.h).
                Gdx3dsStereo::BindTargetRaw(Gdx3dsStereo::RightTarget());
                Gdx3dsStereo::ComputeEyeMatrix(dispBase, 1, &eyeMtx, cls);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
                IssueDraw();
            }
            C3D_RenderTarget* back = mFramebuffers[mCurrentFramebuffer].target;
            Gdx3dsStereo::BindTargetRaw(back != nullptr ? back : mFramebuffers[0].target);
            mFrameDrawsRightEye++;
        }
        sDispProjLast = nullptr; // eye uploads overwrote the uniform: force re-upload
    } else if (!replayRunsDrawn) {
        if (gdx3ds_replay_rec && mXf == nullptr && gdx3ds_replay_legacy_needs_proj(replayTag)) {
            sDispProjLast = nullptr; /* C6: a new pose re-emits the projection */
        } else if (gdx3ds_replay_rec && mXf != nullptr && gdx3ds_replay_xf_needs_proj()) {
            /* C6: the gpuxf path ends in the same `projection` (outpos = projection * xmvp * v); a
             * replay patches that register per legacy pose, so an xf draw after a posed legacy draw
             * carries its own copy of the unposed map instead of inheriting the patched one. */
            sDispProjLast = nullptr;
        }
        if (dispBase != sDispProjLast) {
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, dispBase);
            sDispProjLast = (dispBase == &vpMtx) ? nullptr : dispBase;
        }
        if (gdx3ds_replay_rec) {
            gdx3ds_replay_note_vtx0(bufVbo);
            gdx3ds_replay_note_range(dst, (unsigned)(vtxPoolFloats + nativeIdxFloats));
            u32* const w0 = gpuCmdBuf + gpuCmdBufOffset;
            IssueDraw();
            gdx3ds_replay_draw_post(replayTag, mXf != nullptr ? 1 : 0, w0, gpuCmdBuf + gpuCmdBufOffset,
                                    mXf != nullptr ? mXfShadow : nullptr);
        } else {
            IssueDraw();
        }
    }

    if (GDX_UNLIKELY(xfp)) {
        gdx3ds_xfprof_mark(XFP_B_SUBMIT, svcGetSystemTick());
    }
    mVboOffsetFloats += vtxPoolFloats + nativeIdxFloats;
    if (mVboOffsetFloats > mVboHighWaterFloats) {
        mVboHighWaterFloats = mVboOffsetFloats; /* GEOMLOSS: peak pool use, [c3d] vboHW */
    }
    mFrameDrawCalls++;
    /* COVERAGE (NEXT.md B15/B43): the one place every draw of every path funnels through, so
     * this is where a frame's combiner / render-mode / texture-format vocabulary is recorded.
     * One branch on gdx3ds_cov_on when the census is off, which is every shipped run. */
    if (GDX_UNLIKELY(gdx3ds_cov_on)) {
        CovNoteDraw(prg, texBacked, anchoredDraw, numVerts, bufVboNumTris);
    }
    if (packed) {
        sPackedDrawCalls++; // [triloop] receipt: dPk on the [c3d] line
    }
    mFrameTris += (uint32_t)bufVboNumTris;
    // S12 asset-cost: vertices actually repacked/transformed this draw (decimation
    // proxy). Counted post-commit so the VBO-exhaustion early-out doesn't inflate it.
    gdx3ds_gpuprof_note_verts((unsigned)numVerts);
    if (texBacked) {
        mFrameDrawsTexFb++;
    } else {
        mFrameDrawsScreenFb++;
    }
}

/* ------------------------------------------------------------------------------- */
/* COVERAGE ([debug] cov) — what this draw exercises                                 */
/* ------------------------------------------------------------------------------- */

static inline uint32_t CovLog2u(uint32_t v) {
    uint32_t n = 0;
    while (v > 1u && n < 15u) {
        v >>= 1;
        n++;
    }
    return n;
}

GDX_COLD void GfxRenderingAPIC3D::CovNoteTexture(int unit, uint32_t texId) {
    /* value: unit | class | PICA format | pow2 extents | filter | wrap. This is the texture
     * VOCABULARY the frame binds, not the texture identity: two different course textures of
     * the same format and extent are the same coverage feature, because a SHOT that samples
     * one samples the decode/bind path of the other. Identity is the texture cache's business
     * (family TC), not the schedule's. */
    uint32_t cls = 2; /* not ready: a draw wanting a texel whose texture never uploaded */
    uint32_t fmt = 0xFu;
    uint32_t w = 0, h = 0, filt = 0, cms = 0, cmt = 0;
    if (texId < mTextures.size()) {
        const TextureC3D& t = mTextures[texId];
        if (t.page >= 0) {
            cls = 1; /* [trectbatch] view into a shared atlas page */
            w = t.padWidth;
            h = t.padHeight;
        } else if (t.inited) {
            cls = 0;
            fmt = (uint32_t)t.tex.fmt & 0xFu;
            w = t.padWidth;
            h = t.padHeight;
        }
        filt = t.linearFilter ? 1u : 0u;
        cms = t.cms & 3u;
        cmt = t.cmt & 3u;
    }
    gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_TEX,
                                   ((uint32_t)(unit & 1) << 23) | (cls << 21) | (fmt << 17) |
                                       (CovLog2u(w) << 13) | (CovLog2u(h) << 9) | (filt << 8) |
                                       (cms << 6) | (cmt << 4)));
}

GDX_COLD void GfxRenderingAPIC3D::CovNoteDraw(const ShaderProgramC3D& prg, bool texBacked, bool anchoredDraw,
                                     size_t numVerts, size_t numTris) {
    /* Combiner: the two 64-bit shader ids folded to 24 bits. A fold, not an identity — a
     * collision would merge two combiners into one coverage feature, which costs a sample, and
     * the alternative (a dense side table) costs a per-draw map probe in the hot path. */
    const uint64_t mix = prg.id0 * 0x9E3779B97F4A7C15ull ^ (prg.id1 + 0x165667B19E3779F9ull);
    gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CC, (uint32_t)((mix >> 24) ^ mix)));

    uint32_t rm = 0;
    rm |= mDepthTest ? GDX3DS_COV_RM_ZTEST : 0u;
    rm |= mDepthMask ? GDX3DS_COV_RM_ZWRITE : 0u;
    rm |= mZmodeDecal ? GDX3DS_COV_RM_DECAL : 0u;
    rm |= mUseAlpha ? GDX3DS_COV_RM_ALPHA : 0u;
    rm |= prg.cc.opt_fog ? GDX3DS_COV_RM_FOG : 0u;
    rm |= prg.cc.opt_prim_depth ? GDX3DS_COV_RM_PDEPTH : 0u;
    rm |= prg.cc.opt_texture_edge ? GDX3DS_COV_RM_TEDGE : 0u;
    rm |= prg.cc.opt_alpha_threshold ? GDX3DS_COV_RM_ATHR : 0u;
    rm |= prg.cc.opt_2cyc ? 0x100u : 0u;
    rm |= prg.cc.opt_noise ? 0x200u : 0u;
    rm |= prg.cc.opt_grayscale ? 0x400u : 0u;
    rm |= prg.fogBlendStage ? 0x800u : 0u;
    rm |= (uint32_t)(prg.cc.numInputs & 7) << 12;
    rm |= mNativeActive ? 0x8000u : 0u;   /* indexed native mesh draw vs arrays */
    gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_RM, rm));

    /* Prim-depth-anchored draws get their own family: a stereo anchor is a distinct visual
     * behaviour, and B15's whole point is that a feature nothing samples is a feature nothing
     * checks. */
    if (anchoredDraw) {
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_RM, rm | 0x10000u));
    }
    gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_FB, texBacked ? 1u : 0u));

    /* Batch MAGNITUDE (NEXT.md B51), log2-bucketed. Every family above this line describes
     * state; this one describes size, which is the only thing a batching or merging
     * regression changes. See gdx3ds_cov.h for why log2 and why (path, verts, tris) is one
     * joint key rather than three. */
    gdx3ds_cov_mark(GDX3DS_COV_KEY(
        GDX3DS_COV_FAM_BATCH,
        GDX3DS_COV_BATCH_VAL(mNativeActive ? GDX3DS_COV_BATCH_INDEXED : GDX3DS_COV_BATCH_ARRAY,
                             CovLog2u((uint32_t)numVerts), CovLog2u((uint32_t)numTris))));

    for (int tile = 0; tile < 2; tile++) {
        if (prg.cc.usedTextures[tile]) {
            CovNoteTexture(tile, mBoundTextureIds[tile]);
        }
    }
}

/* ------------------------------------------------------------------------------- */
/* [triloop] direct PICA-layout emission (gdx3ds_vbopack.h)                          */
/* ------------------------------------------------------------------------------- */

float* GfxRenderingAPIC3D::VboPackBegin(Gdx3dsVboPackParams* params, uint32_t* capTris) {
    GDX3DS_PW_NOTE(GDX3DS_PW_VBO); /* o3ds3d [prewrite] */
    if (capTris != nullptr) {
        *capTris = 0;
    }
    if (!GdxTriloopEnabled()) {
        /* GEOMLOSS: LOUD. The LUS path has a legacy loop to fall back to; the owned
         * interpreter does not, so under [native] interp=1 this killswitch is a
         * draw-nothing switch. Say so once rather than quietly rendering black. */
        if (capTris != nullptr && !mVboPackRefuseLogged) {
            mVboPackRefuseLogged = true;
            GFX_C3D_LOG("GEOMLOSS: packed begin refused — [debug] triloop=0 with no legacy path "
                        "on this interpreter; geometry WILL be dropped\n");
        }
        return nullptr;
    }
    if (!mInitialized || !mFrameActive || mCurrentShader == nullptr) {
        if (capTris != nullptr && !mVboPackRefuseLogged) {
            mVboPackRefuseLogged = true;
            GFX_C3D_LOG("GEOMLOSS: packed begin refused — init=%d frame=%d shader=%d; "
                        "this batch is dropped\n",
                        mInitialized ? 1 : 0, mFrameActive ? 1 : 0, mCurrentShader != nullptr ? 1 : 0);
        }
        return nullptr;
    }
    // A full interpreter batch is MAX_TRI_BUFFER = 256 tris.
    constexpr size_t kBatchReserveFloats = 256 * 3 * kOutStrideFloats;
    const bool splitOn = GdxVboSplitEnabled();
    const size_t freeFloats = mVboPoolFloats > mVboOffsetFloats ? mVboPoolFloats - mVboOffsetFloats : 0;
    if (capTris == nullptr || !splitOn) {
        // Historic all-or-nothing contract: the caller (LUS) answers a refusal by
        // running its legacy variable-stride loop, which reserves what it actually
        // needs at DrawTriangles time. [debug] vbosplit=0 puts the owned caller back
        // on it too (the A/B arm that drops geometry).
        if (freeFloats < kBatchReserveFloats) {
            if (capTris != nullptr) {
                sVboPackRefused++;
                if (!mVboPackRefuseLogged) {
                    mVboPackRefuseLogged = true;
                    GFX_C3D_LOG("GEOMLOSS: [debug] vbosplit=0 — packed begin refused at %zu/%zu KiB with "
                                "%zu tris still fitting; that geometry is DROPPED\n",
                                mVboOffsetFloats * sizeof(float) / 1024, mVboPoolFloats * sizeof(float) / 1024,
                                freeFloats / (3 * kOutStrideFloats));
                }
            }
            return nullptr;
        }
        if (capTris != nullptr) {
            *capTris = 256; /* pre-fix: always a whole batch */
        }
    } else {
        // GEOMLOSS: the owned interpreter has no legacy loop. Tell it how many
        // triangles still fit so it can split the batch there; refuse only when not
        // even one triangle fits, which is exactly where DrawTriangles — and so the
        // LUS path too — drops the geometry.
        size_t fitTris = freeFloats / (3 * kOutStrideFloats);
        if (fitTris == 0) {
            sVboPackRefused++;
            if (!mVboPackRefuseLogged) {
                mVboPackRefuseLogged = true;
                GFX_C3D_LOG("GEOMLOSS: VBO pool exhausted at %zu/%zu KiB — packed begin refused, "
                            "the rest of this frame's owned-interpreter geometry is DROPPED\n",
                            mVboOffsetFloats * sizeof(float) / 1024,
                            mVboPoolFloats * sizeof(float) / 1024);
            }
            return nullptr;
        }
        const uint32_t forced = GdxVboPackCap();
        if (forced != 0 && forced < fitTris) {
            fitTris = forced;
        }
        *capTris = (uint32_t)fitTris;
        if (fitTris < 256) {
            sVboPackSplit++;
        }
        if (fitTris < 256 && !mVboPackSplitLogged) {
            mVboPackSplitLogged = true;
            GFX_C3D_LOG("GEOMLOSS: VBO pool low at %zu/%zu KiB — packed batches now split at %zu tris "
                        "(geometry preserved, draw count rises)\n",
                        mVboOffsetFloats * sizeof(float) / 1024, mVboPoolFloats * sizeof(float) / 1024,
                        fitTris);
        }
    }
    const ShaderProgramC3D& prg = *mCurrentShader;
    const bool skyClampFix = GdxSkyClampFixEnabled();
    params->hasOff = 0;
    for (int t = 0; t < 2; t++) {
        params->used[t] = prg.cc.usedTextures[t] ? 1 : 0;
        AtlasFillTexParams(t, params, skyClampFix);
    }
    params->shaderClampGate = skyClampFix ? 1 : 0;
    params->vtxColorInput = (int8_t)prg.vtxColorInput;
    params->vtxAlphaInput = (int8_t)prg.vtxAlphaInput;
    params->wantVtxAlpha = (prg.inputSize == 4 && prg.vtxAlphaInput > 0) ? 1 : 0;
    params->fogFactorToAlpha = (prg.fogBlendStage && prg.fogOffset >= 0) ? 1 : 0;
    return mVboPool + mVboOffsetFloats;
}

static bool sNbStockArm = false;      /* [native] verify: hash every packed draw */
static uint32_t sNbStockHash = GDX_NATIVEBATCH_HASH_SEED;
static uint32_t sNbStockTris = 0;

void GfxRenderingAPIC3D::DrawPackedBatch(size_t numTris, const Gdx3dsVboPackAux* aux) {
    sReplayOuterCaller = __builtin_return_address(0); /* C6: names an untagged draw's path */
    if (GDX_UNLIKELY(sNbStockArm) && numTris != 0 && mVboOffsetFloats + numTris * 3 * kOutStrideFloats <= mVboPoolFloats) {
        sNbStockHash = Gdx_NativeBatchHashBytes(mVboPool + mVboOffsetFloats,
                                                numTris * 3 * kOutStrideFloats * sizeof(float), sNbStockHash);
        sNbStockTris += (uint32_t)numTris;
    }
    // Route through DrawTriangles so every early-out, diagnostic and the
    // submission tail stay shared; mPackedAux flags the repack-free mode.
    mPackedAux = aux;
    DrawTriangles(mVboPool + mVboOffsetFloats, numTris * 3 * kOutStrideFloats, numTris);
    mPackedAux = nullptr;
}

/* ------------------------------------------------------------------------------- */
/* [native] machines: indexed mesh draws (gdx3ds_mesh.h)                             */
/* ------------------------------------------------------------------------------- */

/* [wobble] floats consumed by an n-entry u16 index tail, rounded up to whole 16-byte units. */
static inline size_t NativeIdxFloats(size_t nIndices) {
    return ((nIndices * 2u + 15u) / 16u) * 4u;
}

float* GfxRenderingAPIC3D::NativeMeshBegin(Gdx3dsVboPackParams* params, uint32_t minVerts, uint32_t maxIndices,
                                           uint32_t* capVerts) {
    GDX3DS_PW_NOTE(GDX3DS_PW_VBO); /* o3ds3d [prewrite] */
    if (!mInitialized || !mFrameActive || mCurrentShader == nullptr) {
        return nullptr;
    }
    /* Vertices first, then the u16 indices rounded up to whole floats; the caller writes
     * only what survives (bounded by capVerts) and NativeMeshDraw advances by the actual
     * counts. Refuse only when even the minimum does not fit. */
    /* [wobble] the u16 index tail is padded to 16 bytes (4 floats) so the pool cursor — and
     * therefore every later draw's attribute-buffer base — keeps the 16-byte alignment the
     * stock 48-byte-stride appends always had; ceil(n*2/4) alone left it 4-byte aligned. Bytes
     * drawn are unchanged (parity-neutral); only the padding between draws differs. */
    const size_t idxFloats = NativeIdxFloats(maxIndices);
    const size_t minFloats = (size_t)minVerts * kOutStrideFloats + idxFloats;
    if (mVboOffsetFloats + minFloats > mVboPoolFloats) {
        return nullptr;
    }
    *capVerts = (uint32_t)((mVboPoolFloats - mVboOffsetFloats - idxFloats) / kOutStrideFloats);
    const ShaderProgramC3D& prg = *mCurrentShader;
    const bool skyClampFix = GdxSkyClampFixEnabled();
    params->hasOff = 0;
    for (int t = 0; t < 2; t++) {
        params->used[t] = prg.cc.usedTextures[t] ? 1 : 0;
        AtlasFillTexParams(t, params, skyClampFix);
    }
    params->shaderClampGate = skyClampFix ? 1 : 0;
    params->vtxColorInput = (int8_t)prg.vtxColorInput;
    params->vtxAlphaInput = (int8_t)prg.vtxAlphaInput;
    params->wantVtxAlpha = (prg.inputSize == 4 && prg.vtxAlphaInput > 0) ? 1 : 0;
    params->fogFactorToAlpha = (prg.fogBlendStage && prg.fogOffset >= 0) ? 1 : 0;
    return mVboPool + mVboOffsetFloats;
}

void GfxRenderingAPIC3D::NativeMeshDraw(uint32_t nVerts, const uint16_t* idx, uint32_t nIdx, const Gdx3dsVboPackAux* aux) {
    sReplayOuterCaller = __builtin_return_address(0); /* C6: names an untagged draw's path */
    if (nIdx == 0 || nVerts == 0 || aux == nullptr) {
        return; /* nothing survived: the reservation is simply not consumed */
    }
    float* base = mVboPool + mVboOffsetFloats;
    uint16_t* idxDst = (uint16_t*)(base + (size_t)nVerts * kOutStrideFloats);
    std::memcpy(idxDst, idx, (size_t)nIdx * sizeof(uint16_t));
    mNativeIdx.idx = idxDst;
    mNativeIdx.nIdx = nIdx;
    mNativeIdx.nVerts = nVerts;
    mNativeIdx.idxFloats = NativeIdxFloats(nIdx);
    mNativeActive = true;
    mPackedAux = aux;
    DrawTriangles(base, (size_t)nVerts * kOutStrideFloats, nIdx / 3u);
    mPackedAux = nullptr;
    mNativeActive = false;
    sNativeDrawCalls++;
}

/* ------------------------------------------------------------------------------- */
/* [native] gpuxf: GPU-transformed mesh runs (gdx3ds_mesh.h Gdx3dsXfDraw)             */
/* ------------------------------------------------------------------------------- */

/* The gpuxf uniforms in shader.v.pica declaration order, with their vec4 counts. The upload
 * writes Gdx3dsXfDraw's float block into consecutive registers, so the shader must have
 * placed them consecutively (picasso allocates in declaration order); XfInitLayout checks. */
static const struct {
    const char* name;
    int count;
} kXfUniforms[] = { { "xmvp", 4 },  { "xconv", 1 }, { "xconvw", 1 }, { "xfog", 1 }, { "xamb", 1 },
                    { "xunlit", 1 }, { "xldir", 3 }, { "xlcol", 3 },  { "xlook", 2 }, { "xtg", 1 },
                    { "xtcs", 1 },  { "xuva", 2 },  { "xuvc", 2 },   { "xcol", 1 },  { "xsel", 1 },
                    { "xseli", 1 }, { "xflx", 1 }, { "xoff", 1 } };
/* c5s1: 26 -> 27 (`xflx`, the retained path's FLX alpha light + select, appended last).
 * c5s3c: 27 -> 28 (`xoff`, [native] track_resident's exact eye offset, appended last). */
constexpr int kXfUniformVec4s = 28;
static_assert(offsetof(Gdx3dsXfDraw, off) - offsetof(Gdx3dsXfDraw, mvp) == (kXfUniformVec4s - 1) * 16,
              "Gdx3dsXfDraw uniform block must be 28 consecutive vec4s in shader order");
static_assert(sizeof(((GfxRenderingAPIC3D*)nullptr)->mXfShadow) == kXfUniformVec4s * 4 * sizeof(float),
              "the gpuxf uniform shadow must hold every gpuxf vec4");

void GfxRenderingAPIC3D::XfInitLayout() {
    AttrInfo_Init(&mAttrXf);
    AttrInfo_AddLoader(&mAttrXf, 0, GPU_SHORT, 4);         /* ob x y z + flag */
    AttrInfo_AddLoader(&mAttrXf, 1, GPU_SHORT, 2);         /* tc s t */
    AttrInfo_AddLoader(&mAttrXf, 2, GPU_UNSIGNED_BYTE, 4); /* cn (colour or normal) + alpha */
    AttrInfo_Init(&mAttrXfF);                                /* [native] track_retained */
    AttrInfo_AddLoader(&mAttrXfF, 0, GPU_FLOAT, 3);          /* position, rebased object space */
    AttrInfo_AddLoader(&mAttrXfF, 1, GPU_FLOAT, 2);          /* tc s t (scaled shorts) */
    AttrInfo_AddLoader(&mAttrXfF, 2, GPU_UNSIGNED_BYTE, 4);  /* cn */
    mXfUnifLoc = -1;
    const int base = shaderInstanceGetUniformLocation(mShaderProgram.vertexShader, "xmvp");
    int expect = base;
    bool ok = base >= 0;
    for (const auto& u : kXfUniforms) {
        const int loc = shaderInstanceGetUniformLocation(mShaderProgram.vertexShader, u.name);
        if (loc != expect) {
            ok = false;
        }
        expect += u.count;
    }
    mXfBoolLoc = shaderInstanceGetUniformLocation(mShaderProgram.vertexShader, "xfon");
    if (mXfBoolLoc < 0) {
        ok = false;
    } else {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, mXfBoolLoc, false);
    }
    mXfUnifLoc = ok ? base : -1;
    GFX_C3D_LOG("[gpuxf] shader layout %s (xmvp=%d xfon=%d)\n", ok ? "ok" : "REFUSED", base, mXfBoolLoc);
}

/* C6 [replay]: the pose vec4s (xmvp 0-3, xldir 9-11, xlook 15-16, xflx 26, xoff 27) go out
 * again at the next draw, so the draw's words carry its own pose. */
void GfxRenderingAPIC3D::ReplayDirtyXfPose() {
    static const int kPose[] = { 0, 1, 2, 3, 9, 10, 11, 15, 16, 26, 27 };
    C3Dgdx_unifTouch = 1; /* n60draw: a hand-marked dirty register (citro3d uniforms.h) */
    for (int k : kPose) {
        C3D_FVUnifDirty[GPU_VERTEX_SHADER][mXfUnifLoc + k] = true;
    }
}

void* GfxRenderingAPIC3D::ReplayMainColorBuf() {
    if (mFramebuffers.empty() || mFramebuffers[0].target == nullptr) {
        return nullptr;
    }
    return mFramebuffers[0].target->frameBuf.colorBuf;
}

void GfxRenderingAPIC3D::XfUploadUniforms(const Gdx3dsXfDraw& xf) {
    const float* f = &xf.mvp[0][0];
    for (int i = 0; i < kXfUniformVec4s; i++) {
        const float* v = f + i * 4;
        if (mXfShadowValid && std::memcmp(v, mXfShadow + i * 4, 4 * sizeof(float)) == 0) {
            continue; /* the register already holds it (only gpuxf draws write these) */
        }
        C3D_FVUnifSet(GPU_VERTEX_SHADER, mXfUnifLoc + i, v[0], v[1], v[2], v[3]);
        std::memcpy(mXfShadow + i * 4, v, 4 * sizeof(float));
    }
    mXfShadowValid = true;
}

/* [native] gpuxf_cullflip (default 0): swap the front/back mapping below if a hardware A/B
 * ever shows the winding mirrored. The mapping: the CPU cull (gdx3ds_tri_cull) keeps CCW in
 * game NDC (y up) for G_CULL_BACK; the fixup is a rotation (orientation-preserving) and the
 * stereo shear / DISP scale / viewport fold are positive affine maps, so CCW survives to the
 * PICA and G_CULL_BACK is GPU_CULL_BACK_CCW (the emulator A/B confirms it pixel for pixel). */
static int GdxXfCullFlip() {
    static int sFlip = -1;
    if (sFlip < 0) {
        sFlip = (&gdx3ds_config_get_int != nullptr && gdx3ds_config_get_int("native", "gpuxf_cullflip", 0) != 0) ? 1
                                                                                                             : 0;
    }
    return sFlip;
}

void GfxRenderingAPIC3D::XfSetCull(int cullMode) {
    GPU_CULLMODE mode = GPU_CULL_NONE;
    if (cullMode == GDX3DS_CULL_BACK || cullMode == GDX3DS_CULL_FRONT) {
        const bool cullFront = (cullMode == GDX3DS_CULL_FRONT) != (GdxXfCullFlip() != 0);
        mode = cullFront ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW;
    }
    if ((int)mode != mXfCull) {
        C3D_CullFace(mode);
        mXfCull = (int)mode;
    }
}

void GfxRenderingAPIC3D::XfRestore() {
    C3D_SetAttrInfo(&mAttrLegacy);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, mXfBoolLoc, false);
    if (mXfCull != (int)GPU_CULL_NONE) {
        C3D_CullFace(GPU_CULL_NONE); /* legacy draws: the interpreter culls on the CPU */
    }
    mXfCull = (int)GPU_CULL_NONE;
    mXfStateOn = false;
}

void* GfxRenderingAPIC3D::NativeXfVertices(uint32_t nVerts) {
    GDX3DS_PW_NOTE(GDX3DS_PW_VBO); /* o3ds3d [prewrite] */
    if (!mInitialized || !mFrameActive || mXfUnifLoc < 0 || nVerts == 0) {
        return nullptr;
    }
    const size_t floats = (size_t)nVerts * 4u; /* 16 bytes per Vtx keeps the pool 16-aligned */
    if (mVboOffsetFloats + floats > mVboPoolFloats) {
        return nullptr;
    }
    float* p = mVboPool + mVboOffsetFloats;
    mVboOffsetFloats += floats;
    if (mVboOffsetFloats > mVboHighWaterFloats) {
        mVboHighWaterFloats = mVboOffsetFloats;
    }
    return p;
}

bool GfxRenderingAPIC3D::NativeXfParams(Gdx3dsVboPackParams* params) {
    if (!mInitialized || !mFrameActive || mCurrentShader == nullptr || mXfUnifLoc < 0 || gdx3ds_verify_mode != 0 ||
        sMeshCaptureFn != nullptr) {
        return false;
    }
    const ShaderProgramC3D& prg = *mCurrentShader;
    const bool skyClampFix = GdxSkyClampFixEnabled();
    params->hasOff = 0;
    for (int t = 0; t < 2; t++) {
        params->used[t] = prg.cc.usedTextures[t] ? 1 : 0;
        AtlasFillTexParams(t, params, skyClampFix);
    }
    params->shaderClampGate = skyClampFix ? 1 : 0;
    params->vtxColorInput = (int8_t)prg.vtxColorInput;
    params->vtxAlphaInput = (int8_t)prg.vtxAlphaInput;
    params->wantVtxAlpha = (prg.inputSize == 4 && prg.vtxAlphaInput > 0) ? 1 : 0;
    params->fogFactorToAlpha = (prg.fogBlendStage && prg.fogOffset >= 0) ? 1 : 0;
    return true;
}

void GfxRenderingAPIC3D::NativeXfDraw(const uint16_t* idx, uint32_t nIdx, const Gdx3dsVboPackAux* aux,
                                      const Gdx3dsXfDraw* xf) {
    sReplayOuterCaller = __builtin_return_address(0); /* C6: names an untagged draw's path */
    if (nIdx == 0 || aux == nullptr || xf == nullptr || xf->verts == nullptr) {
        return;
    }
    /* [native] machines_retained: resident indices are drawn in place (no pool tail). */
    const size_t idxFloats = xf->idxResident ? 0 : NativeIdxFloats(nIdx);
    if (mVboOffsetFloats + idxFloats > mVboPoolFloats) {
        sVboDrawDropped++;
        return;
    }
    uint16_t* idxDst = const_cast<uint16_t*>(idx);
    if (!xf->idxResident) {
        idxDst = (uint16_t*)(mVboPool + mVboOffsetFloats);
        std::memcpy(idxDst, idx, (size_t)nIdx * sizeof(uint16_t));
    }
    mNativeIdx.idx = idxDst;
    mNativeIdx.nIdx = nIdx;
    mNativeIdx.nVerts = xf->nVerts;
    mNativeIdx.idxFloats = idxFloats;
    mNativeActive = true;
    mPackedAux = aux;
    mXf = xf;
    DrawTriangles((float*)idxDst, (size_t)xf->nVerts * kOutStrideFloats, nIdx / 3u);
    mXf = nullptr;
    mPackedAux = nullptr;
    mNativeActive = false;
    sNativeDrawCalls++; /* the gpuxf GPU state stays on until the next legacy draw */
}

/* ------------------------------------------------------------------------------- */
/* Fog (PICA native fog unit)                                                       */
/* ------------------------------------------------------------------------------- */

void GfxRenderingAPIC3D::UpdateFogState(const ShaderProgramC3D& prg, const float* bufVbo, size_t numVerts,
                                        size_t inStride, const Gdx3dsVboPackAux* packedAux) {
    // The interpreter bakes the N64 RSP fog computation into the vertex stream:
    // fog rgb (draw-constant) + a per-vertex factor, both linear in z/w — exactly
    // what the PICA fog unit indexes its LUT with (fragment depth). Recover the
    // line factor = a*depth + b from the batch's extreme vertices and feed it to
    // the native fog unit; no TexEnv stage is spent.
    // [triloop] packed batches carry no per-vertex fog slot; the draw-constant
    // fog rgb and the vertex-0 factor ride the interpreter's aux record instead.
    const float* fog0 = packedAux != nullptr ? packedAux->fogRgb : bufVbo + prg.fogOffset;
    const uint32_t fogColor = ((uint32_t)(fog0[0] * 255.0f + 0.5f) & 0xFF) |
                              (((uint32_t)(fog0[1] * 255.0f + 0.5f) & 0xFF) << 8) |
                              (((uint32_t)(fog0[2] * 255.0f + 0.5f) & 0xFF) << 16);

    float a;
    float b;
    if (sGdxFogParamsValid) {
        // Exact RSP fog line, mirrored from the interpreter's G_MW_FOG state
        // (gdx3ds_fog_note_params). The interpreter bakes the per-vertex factor as
        // clamp(r*mul + off, 0, 255)/255 with r = z/w in GL clip convention
        // ([-1, 1]; interpreter.cpp fogRatio). Our LUT coordinate, however, is the
        // bufVbo z/w — and GetClipParameters() returns z_is_from_0_to_1, so the
        // interpreter hands this backend z' = (z + w)/2, i.e. d = (r + 1)/2 in
        // [0, 1] (harness-verified 0 = near, 1 = far; see the fallback comment).
        // Substituting r = 2d - 1 into the RSP line gives the exact LUT-space line
        //   f(d) = (d*(2*mul) + (off - mul)) / 255.
        // A secant through the batch's endpoint samples is wrong the moment either
        // endpoint clamps — F-Zero X's race band (990..1000) clamps both ways in
        // nearly every course draw.
        a = (float)(2 * sGdxFogMul) / 255.0f;
        b = (float)(sGdxFogOffset - sGdxFogMul) / 255.0f;
    } else if (packedAux != nullptr) {
        // [triloop] exact=0 draws are the constant-factor shroud/blend-colour
        // mode (gdx3ds_fog_note_params(0,0,0)): every baked factor equals
        // fog_color.a, so the legacy secant through a constant series is
        // exactly a=0, b=that constant — no vertex scan needed (the packed
        // layout has no per-vertex factor slot to scan anyway).
        a = 0.0f;
        b = packedAux->fogFactor0;
    } else {
        // Fallback: reconstruct the line from the batch's extreme vertices.
        float dMin = 1e30f, dMax = -1e30f, fAtMin = 0.0f, fAtMax = 0.0f;
        for (size_t v = 0; v < numVerts; v++) {
            const float* src = bufVbo + v * inStride;
            if (src[3] == 0.0f) {
                continue;
            }
            // Fog-LUT input coordinate. Harness-verified (FOG scene): under our
            // DepthMap(true, -1, 0) reversed-depth setup the PICA fog unit indexes
            // the LUT with z/w directly (0 = near, 1 = far), NOT the depth-buffer
            // value 1 - z/w — the first fit produced an exactly inverted gradient.
            const float d = src[2] / src[3];
            const float f = src[prg.fogOffset + 3];
            if (d < dMin) {
                dMin = d;
                fAtMin = f;
            }
            if (d > dMax) {
                dMax = d;
                fAtMax = f;
            }
        }
        a = 0.0f;
        b = fAtMin;
        if (dMax - dMin > 1e-4f) {
            a = (fAtMax - fAtMin) / (dMax - dMin);
            b = fAtMin - a * dMin;
        }
    }

    // C4 discriminator (gdx-fogprobe.txt at the SD root, checked once): override every
    // fogged draw's line with a steep ramp over the race's observed clip-depth band
    // (d 0.96 -> 1.0, i.e. f = 25d - 24). One capture then names the fog unit's true
    // input coordinate at race depths: input == d shows a clean near-clear/far-solid
    // distance gradient; input == 1-d (the depth-buffer value) shows NO fog at all.
    static const bool sGdxFogProbe = [] {
        FILE* f = fopen("gdx-fogprobe.txt", "rb");
        if (f != nullptr) {
            fclose(f);
            return true;
        }
        return false;
    }();
    if (sGdxFogProbe) {
        a = 25.0f;
        b = -24.0f;
    }

    // C3/C4 per-draw dump (race-gated, bounded): the batch's actual clip-space d range and
    // baked factor range, to compare the LUT's idea of the draw against the vertices.
    // C4: rearms 10 draws every 128th frame so the windows also cover SHOT captures
    // minutes into a run, not just the first race frames.
    {
        const bool raceActive2 =
            (&gGdxRaceActive != nullptr) && (gGdxRaceActive != 0) && GdxVerboseTelemetry();
        static unsigned sFogDrawWindowFrame = 0;
        static int sFogDrawBudget = 0;
        if (raceActive2 &&
            (sFogDrawWindowFrame == 0 || sGdxFogFrame - sFogDrawWindowFrame >= 128)) {
            sFogDrawWindowFrame = sGdxFogFrame;
            sFogDrawBudget = 10;
        }
        if (raceActive2 && sFogDrawBudget > 0) {
            --sFogDrawBudget;
            [&prg, bufVbo, numVerts, inStride, packedAux, a, b, fogColor]() GDX_COLD { /* hotcold */
            float ddMin = 1e30f, ddMax = -1e30f, ffMin = 1e30f, ffMax = -1e30f, wwMin = 1e30f,
                  wwMax = -1e30f;
            for (size_t v = 0; v < numVerts; v++) {
                const float* src = bufVbo + v * inStride;
                if (src[3] == 0.0f) {
                    continue;
                }
                const float dd = src[2] / src[3];
                // [triloop] packed stride carries no fog slot; report the aux
                // record's vertex-0 factor (draw-constant on this path).
                const float ff = packedAux != nullptr ? packedAux->fogFactor0 : src[prg.fogOffset + 3];
                ddMin = dd < ddMin ? dd : ddMin;
                ddMax = dd > ddMax ? dd : ddMax;
                ffMin = ff < ffMin ? ff : ffMin;
                ffMax = ff > ffMax ? ff : ffMax;
                wwMin = src[3] < wwMin ? src[3] : wwMin;
                wwMax = src[3] > wwMax ? src[3] : wwMax;
            }
            GFX_C3D_LOG("[fogdraw] fr=%u n=%d %s a=%.2f b=%.2f d=[%.4f %.4f] f=[%.3f %.3f] "
                        "w=[%.0f %.0f] col=%06lx\n",
                        sGdxFogFrame, (int)numVerts, sGdxFogParamsValid ? "ex" : "fit", (double)a,
                        (double)b, (double)ddMin, (double)ddMax, (double)ffMin, (double)ffMax,
                        (double)wwMin, (double)wwMax, (unsigned long)fogColor);
            }();
        }
    }

    // Race-gated, bounded: name each DISTINCT fog line once (not on change — the race
    // alternates a handful of lines per frame, and change-triggered logging burned the
    // whole budget on the first alternation) so a regression here is visible in the
    // log rather than only in screenshots.
    {
        const bool raceActive = (&gGdxRaceActive != nullptr) && (gGdxRaceActive != 0);
        static int32_t sSeenKeys[16][2];
        static int sSeenCount = 0;
        const int32_t qa = (int32_t)lrintf(a * 1024.0f), qb = (int32_t)lrintf(b * 4096.0f);
        if (raceActive && sSeenCount < 16) {
            bool seen = false;
            for (int i = 0; i < sSeenCount; i++) {
                if (sSeenKeys[i][0] == qa && sSeenKeys[i][1] == qb) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                sSeenKeys[sSeenCount][0] = qa;
                sSeenKeys[sSeenCount][1] = qb;
                sSeenCount++;
                GFX_C3D_LOG("[fogdiag] %s a=%.3f b=%.3f mul=%d off=%d col=%06lx\n",
                            sGdxFogParamsValid ? "exact" : "fit", (double)a, (double)b, sGdxFogMul,
                            sGdxFogOffset, (unsigned long)fogColor);
            }
        }
    }

    if (!mFogEnabled) {
        C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
        mFogEnabled = true;
    }
    if (fogColor != mBoundFogColor) {
        C3D_FogColor(fogColor); // 0xBBGGRR, red in the low byte
        mBoundFogColor = fogColor;
    }

    const FogKey key = { (int32_t)lrintf(a * 1024.0f), (int32_t)lrintf(b * 4096.0f) };
    if (key == mBoundFogKey) {
        return;
    }
    auto it = mFogLutCache.find(key);
    if (it == mFogLutCache.end()) {
        it = [this, key, a, b]() GDX_COLD { /* hotcold: LUT-cache miss, out of line */
        if (mFogLutCache.size() >= 64) {
            mFogLutCacheOverflowPending = true; // cleared at the next StartFrame
        }
        float values[129];
        for (int i = 0; i <= 128; i++) {
            float f = a * ((float)i / 128.0f) + b;
            values[i] = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        }
        float data[256];
        for (int i = 0; i < 128; i++) {
            data[i] = values[i];
            data[i + 128] = values[i + 1] - values[i];
        }
        C3D_FogLut lut;
        FogLut_FromArray(&lut, data);
        return mFogLutCache.emplace(key, lut).first;
        }();
    }
    C3D_FogLutBind(&it->second);
    mBoundFogKey = key;
}

void GfxRenderingAPIC3D::DisableFog() {
    if (mFogEnabled) {
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        mFogEnabled = false;
    }
}

/* ------------------------------------------------------------------------------- */
/* Framebuffers                                                                     */
/* ------------------------------------------------------------------------------- */

int GfxRenderingAPIC3D::CreateFramebuffer() {
    mFramebuffers.emplace_back();
    return (int)(mFramebuffers.size() - 1);
}

void GfxRenderingAPIC3D::UpdateFramebufferParameters(int fbId, uint32_t width, uint32_t height,
                                                     uint32_t msaaLevel, bool openglInvertY, bool renderTarget,
                                                     bool hasDepthBuffer, bool canExtractDepth) {
    (void)openglInvertY;
    (void)canExtractDepth;
    if (fbId < 0 || fbId >= (int)mFramebuffers.size()) {
        return;
    }
    if (msaaLevel > 1) {
        static bool sLogged = false;
        if (!sLogged) {
            sLogged = true;
            GFX_C3D_LOG("MSAA level %u requested: PICA has no MSAA, rendering aliased\n", msaaLevel);
        }
    }
    FramebufferC3D& fb = mFramebuffers[fbId];
    if (fbId == 0 || !renderTarget) {
        fb.width = width;
        fb.height = height;
        return; // main screen target is fixed at Init
    }

    /* Texture-backed targets keep the SAME portrait orientation as the screen target
     * (game content is drawn through the fixup matrix on every target): game-y runs
     * along the tex width (fb-x, the stride axis) and game-x along the tex height
     * (fb-y, negated). Allocating landscape-style (padW from game width) put the
     * 320-wide game-x range on a 256px axis — content clipped 64px AND every
     * screen<->texture blit/readback disagreed on layout. */
    const uint32_t padW = NextPow2(height < 8 ? 8 : height); // fb-x extent <- game height
    const uint32_t padH = NextPow2(width < 8 ? 8 : width);   // fb-y extent <- game width
    /* TRANSITION diag (bounded): every texture-backed target resize/relabel. The frame
       mirror's logical size feeds the readback un-rotate, so a rewrite here between a clean
       and a garbled capture is the smoking gun. */
    {
        static int sFbParamLogs = 0;
        if (sFbParamLogs < 96 && (fb.width != width || fb.height != height || !fb.texBacked)) {
            ++sFbParamLogs;
            GFX_C3D_LOG("[fbparam] fb=%d %ux%u -> %ux%u tex=%ux%u pad=%ux%u realloc=%d\n", fbId,
                        (unsigned)fb.width, (unsigned)fb.height, (unsigned)width, (unsigned)height,
                        (unsigned)(fb.texBacked ? fb.tex.width : 0),
                        (unsigned)(fb.texBacked ? fb.tex.height : 0), (unsigned)padW, (unsigned)padH,
                        (fb.texBacked && fb.tex.width == padW && fb.tex.height == padH) ? 0 : 1);
        }
    }
    if (fb.texBacked && fb.tex.width == padW && fb.tex.height == padH) {
        fb.width = width;
        fb.height = height;
        return;
    }
    if (fb.texBacked) {
        C3D_RenderTargetDelete(fb.target);
        C3D_TexDelete(&fb.tex);
        fb.texBacked = false;
        fb.target = nullptr;
    }
    if (!C3D_TexInitVRAM(&fb.tex, (u16)padW, (u16)padH, GPU_RGBA8)) {
        GFX_C3D_LOG("UpdateFramebufferParameters: C3D_TexInitVRAM(%ux%u) failed for fb %d\n", padW, padH, fbId);
        return;
    }
    C3D_TexSetFilter(&fb.tex, GPU_LINEAR, GPU_LINEAR);
    fb.target = C3D_RenderTargetCreateFromTex(&fb.tex, GPU_TEXFACE_2D, 0,
                                              hasDepthBuffer ? (C3D_DEPTHTYPE)GPU_RB_DEPTH24_STENCIL8
                                                             : (C3D_DEPTHTYPE)-1);
    if (fb.target == nullptr) {
        GFX_C3D_LOG("UpdateFramebufferParameters: render target creation failed for fb %d\n", fbId);
        C3D_TexDelete(&fb.tex);
        return;
    }
    fb.texBacked = true;
    fb.width = width;
    fb.height = height;
}

void GfxRenderingAPIC3D::StartDrawToFramebuffer(int fbId, float noiseScale) {
    (void)noiseScale; // noise combiners are post-census work
    if (fbId < 0 || fbId >= (int)mFramebuffers.size() || mFramebuffers[fbId].target == nullptr) {
        GFX_C3D_LOG("StartDrawToFramebuffer: fb %d has no render target\n", fbId);
        return;
    }
    mCurrentFramebuffer = fbId;
    if (gdx3ds_verify_mode != 0) {
        VfyRecord(3, static_cast<uint32_t>(fbId), 0, nullptr);
        if (gdx3ds_verify_mode == 2) {
            return;
        }
    }
    if (mFrameActive) {
        C3D_FrameDrawOn(mFramebuffers[fbId].target);
        if (mStereoEnabled) {
            Gdx3dsStereo::ReplayNoteBind(fbId); /* n60stereo: the main target's words are per eye */
        }
        gdx3ds_skylate_note_bind(fbId); /* n60tail: a texture pass inside the sky / track keeps the order */
        mFrameFbBinds++;
        if (fbId == 0) { sStBindMain++; } else { sStBindOther++; } /* B11q */
        const FramebufferC3D& fb = mFramebuffers[fbId];
        gdx3ds_gpuprof_note_pass(fb.texBacked ? 1 : 0,
                                 fb.texBacked ? (unsigned)fb.tex.width * fb.tex.height : 96000u);
    }
}

void GfxRenderingAPIC3D::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                         int dstX0, int dstY0, int dstX1, int dstY1) {
    const GdxPwGxBracket pwGx; /* o3ds3d [prewrite]: a queued fill / copy is seen by the polls around it */
    /* GX texture-copy path. Every 3DS surface (the screen target AND texture-backed
     * targets, since the portrait-allocation fix above) stores game content in the
     * same rotated tiled-RGBA8 convention: game-y along fb-x from 0, game-x along
     * fb-y NEGATED, i.e. content anchored at the END of the fb-y axis. A raw tiled
     * copy is therefore exact; differing fb-x extents (screen 240 vs pow2 texture)
     * are bridged with the copy engine's per-line gap, and differing fb-y extents
     * with a start offset on the larger surface. Scaled game-extent copies (none in
     * this port: the frame mirror + hold tick are always same-size) stay logged. */
    if (fbSrcId < 0 || fbSrcId >= (int)mFramebuffers.size() || fbDstId < 0 ||
        fbDstId >= (int)mFramebuffers.size() || fbSrcId == fbDstId) {
        return;
    }
    if (fbDstId == 0) { sStCopyToMain++; } else { sStCopyOther++; } /* B11q */
    FramebufferC3D& src = mFramebuffers[fbSrcId];
    FramebufferC3D& dst = mFramebuffers[fbDstId];

    u32* srcBuf = nullptr;
    u32* dstBuf = nullptr;
    uint32_t srcFbW = 0, srcFbH = 0, dstFbW = 0, dstFbH = 0;
    if (src.texBacked) {
        srcBuf = (u32*)src.tex.data;
        srcFbW = src.tex.width;
        srcFbH = src.tex.height;
    } else if (src.target != nullptr) {
        srcBuf = (u32*)src.target->frameBuf.colorBuf;
        srcFbW = (uint32_t)kScreenHeight; // portrait: 240 wide x 400 tall
        srcFbH = (uint32_t)kScreenWidth;
    }
    if (dst.texBacked) {
        dstBuf = (u32*)dst.tex.data;
        dstFbW = dst.tex.width;
        dstFbH = dst.tex.height;
    } else if (dst.target != nullptr) {
        dstBuf = (u32*)dst.target->frameBuf.colorBuf;
        dstFbW = (uint32_t)kScreenHeight;
        dstFbH = (uint32_t)kScreenWidth;
    }
    if (srcBuf == nullptr || dstBuf == nullptr) {
        return;
    }
    (void)srcX0; (void)srcY0; (void)srcX1; (void)srcY1;
    (void)dstX0; (void)dstY0; (void)dstX1; (void)dstY1; // same game extent (see header comment)

    if (mFrameActive) {
        // Submit the queued 3D commands so the copy sees THIS frame's draws, not last frame's.
        FlushPendingVbo();
        C3D_FrameSplit((u8)gdx3ds_overlap_split_flags()); /* [overlap]: 0 unless the queue is live */
    }

    /* Overlapping region, anchored at fb-x 0 AND fb-y 0 — harness-verified: the
     * effective render viewport spans fb-y [vp.x, vp.x + vp.width), i.e. game
     * content starts at fb-y 0 with game-x increasing along fb-y (the dead band
     * is the fb-y tail). All extents are multiples of 8. */
    const uint32_t copyW = srcFbW < dstFbW ? srcFbW : dstFbW;
    const uint32_t copyH = srcFbH < dstFbH ? srcFbH : dstFbH;
    const uint32_t lineBytes = copyW * 8 * 4;          // one 8px-tall row of 8x8 RGBA8 tiles
    const uint32_t srcTileRowBytes = srcFbW * 8 * 4;
    const uint32_t dstTileRowBytes = dstFbW * 8 * 4;
    u32* srcPtr = srcBuf;
    u32* dstPtr = dstBuf;
    const uint32_t srcGap = srcTileRowBytes - lineBytes;
    const uint32_t dstGap = dstTileRowBytes - lineBytes;
    const u32 totalBytes = (copyH / 8) * lineBytes;
    gdx3ds_gpuprof_note_copy((unsigned)copyW * copyH);
    /* ASYNC copy (perf): C3D_SyncTextureCopy enqueues the PPF copy AND blocks the CPU
     * until it completes -- a hard per-frame stall paid by every menu/race frame for the
     * frame mirror. GX_TextureCopy enqueues the same copy on the same GX queue WITHOUT
     * the wait; ordering is preserved (the hold-tick present and EndFrame transfer are
     * queued behind it), and the CPU readback paths do their own sync. */
    if (srcGap == 0 && dstGap == 0) {
        GX_TextureCopy(srcPtr, 0, dstPtr, 0, totalBytes, 8 /* GX texture copy */);
    } else {
        // Line/gap dims are in 16-byte units (3dbrew PPF texture-copy encoding).
        GX_TextureCopy(srcPtr, GX_BUFFER_DIM(lineBytes >> 4, srcGap >> 4), dstPtr,
                       GX_BUFFER_DIM(lineBytes >> 4, dstGap >> 4), totalBytes, 8);
    }
}

/* n60gpu: [native] zclear_elide (default 0). C3D_FrameBufClear queues its GX_MemoryFill WITHOUT
 * splitting the open command list, so a clear runs on the GPU right after the entries queued before
 * it -- not after the draws written since the last split. A depth-only clear of the main target is
 * therefore a no-op when every entry queued since the previous depth clear of the main target is
 * itself a memory fill (no command list ran in between, in GPU order): the depth buffer already holds
 * kClearDepthFar everywhere. The race frame has exactly that shape -- StartFrame's colour+depth clear
 * (+ the right eye's), then the game's z-buffer fill rectangle as a depth-only clear (+ the right
 * eye's) before any list -- so the lever drops one GX entry and one 384 KB VRAM fill per eye per frame.
 * Exact by construction (the skipped fill would rewrite the value already there). */
extern "C" char __C3D_Context[]; /* citro3d's context: its first member is the frame's gxCmdQueue_s */
namespace {
int sZclrLever = -1;
unsigned long sZclrFrame = ~0ul;  /* mFrameIndex of the last main depth clear */
int sZclrEntry = -1;              /* queue index of that clear's fill entry */
u32 sZclrDepthVa = 0;
unsigned long sZclrCalls = 0, sZclrElided = 0, sZclrWin = 0;

/* 1 = on; 2 = on in even blocks of 1024 frames, off in odd ones (a same-run hardware A/B: the
 * [gputl] receipt prints the phase as zc=). */
int ZclrLever() {
    if (sZclrLever < 0) {
        const int v = &gdx3ds_config_get_int != nullptr ? gdx3ds_config_get_int("native", "zclear_elide", 0) : 0;
        sZclrLever = (v == 1 || v == 2) ? v : 0;
        GFX_C3D_LOG("[zclr] init zclear_elide=%d (a main-target depth clear with only fills queued since the previous "
                    "one is skipped%s)\n", sZclrLever, sZclrLever == 2 ? "; A/B: on in even 1024-frame blocks" : "");
    }
    return sZclrLever;
}

const gxCmdQueue_s* ZclrQueue() {
    return reinterpret_cast<const gxCmdQueue_s*>(__C3D_Context);
}

/* The fill entry that cleared `depthVa` most recently, or -1 (scanned backwards from the end). */
int ZclrLastFillOf(u32 depthVa) {
    const gxCmdQueue_s* q = ZclrQueue();
    for (int k = q->numEntries - 1; k >= 0; k--) {
        const gxCmdEntry_s& e = q->entries[k];
        if ((e.data[0] & 0xFFu) == 0x02u && (e.data[1] == depthVa || e.data[4] == depthVa)) {
            return k;
        }
    }
    return -1;
}

bool ZclrRedundant(unsigned long frame, u32 depthVa) {
    const gxCmdQueue_s* q = ZclrQueue();
    if (sZclrFrame != frame || sZclrDepthVa != depthVa || sZclrEntry < 0 || sZclrEntry >= q->numEntries) {
        return false;
    }
    const gxCmdEntry_s& own = q->entries[sZclrEntry];
    if ((own.data[0] & 0xFFu) != 0x02u || (own.data[1] != depthVa && own.data[4] != depthVa)) {
        return false; /* the queue was rebuilt since (a suspend): not our clear any more */
    }
    for (int k = sZclrEntry + 1; k < q->numEntries; k++) {
        if ((q->entries[k].data[0] & 0xFFu) != 0x02u) {
            return false; /* a command list (or a copy) runs between the two clears */
        }
    }
    return true;
}
unsigned long sZclrFrameNow = 0; /* mFrameIndex, for the A/B phase */
} // namespace

/* n60gpu: the zclear_elide phase of the current frame (0 off, 1 on), for the [gputl] receipt. */
extern "C" int gdx3ds_zclr_phase(void) {
    const int lever = ZclrLever();
    return lever == 1 ? 1 : (lever == 2 ? (int)(((sZclrFrameNow >> 10) & 1u) == 0u) : 0);
}

void GfxRenderingAPIC3D::ClearFramebuffer(bool color, bool depth) {
    const GdxPwGxBracket pwGx; /* o3ds3d [prewrite]: a queued fill / copy is seen by the polls around it */
    if (mCurrentFramebuffer >= (int)mFramebuffers.size()) {
        return;
    }
    if (gdx3ds_verify_mode != 0) {
        VfyRecord(2, (color ? 1u : 0u) | (depth ? 2u : 0u) | (static_cast<uint32_t>(mCurrentFramebuffer) << 8), 0, nullptr);
        if (gdx3ds_verify_mode == 2) {
            return;
        }
    }
    C3D_RenderTarget* target = mFramebuffers[mCurrentFramebuffer].target;
    if (target == nullptr) {
        return;
    }
    C3D_ClearBits flags = (C3D_ClearBits)0;
    if (color) {
        flags = (C3D_ClearBits)(flags | C3D_CLEAR_COLOR);
    }
    if (depth) {
        flags = (C3D_ClearBits)(flags | C3D_CLEAR_DEPTH);
    }
    if (flags != 0) {
        sZclrFrameNow = (unsigned long)mFrameIndex;
        const bool mainDepth = depth && mCurrentFramebuffer == 0 && gdx3ds_zclr_phase() != 0 &&
                               target->frameBuf.depthBuf != nullptr;
        const u32 depthVa = mainDepth ? reinterpret_cast<u32>(target->frameBuf.depthBuf) : 0u;
        if (depth && mCurrentFramebuffer == 0) {
            gdx3ds_skylate_note_depth_clear(); /* n60tail: stencil 0 everywhere before the sky */
        }
        if (mainDepth && !color) {
            sZclrCalls++;
            if (ZclrRedundant((unsigned long)mFrameIndex, depthVa)) {
                sZclrElided++; /* the right eye's twin is skipped with it (ClearRight below / the replay) */
                if (++sZclrWin >= 512) {
                    GFX_C3D_LOG("[zclr] frame=%lu depth-only main clears=%lu elided=%lu\n", (unsigned long)mFrameIndex,
                                sZclrCalls, sZclrElided);
                    sZclrWin = 0;
                }
                return;
            }
        }
        C3D_RenderTargetClear(target, flags, 0x000000FF /* opaque black */, kClearDepthFar);
        if (mainDepth) {
            sZclrFrame = (unsigned long)mFrameIndex;
            sZclrDepthVa = depthVa;
            sZclrEntry = ZclrLastFillOf(depthVa);
        }
        if (mCurrentFramebuffer == 0) { sStClearMain++; } else { sStClearOther++; } /* B11q */
        if (mStereoEnabled && mCurrentFramebuffer == 0) {
            Gdx3dsStereo::ClearRight(flags, 0x000000FF, kClearDepthFar); // no-op unless Active()
            if (Gdx3dsStereo::Active()) { sStClearRight++; } /* B11q */
        }
    }
}

bool GfxRenderingAPIC3D::ReadColorBufRgba16(u32* colorBuf, u32 fbW, u32 fbH, u32 gameW, u32 gameH,
                                            uint32_t width, uint32_t height, uint16_t* out, const char* who) {
    const size_t linearBytes = (size_t)fbW * fbH * sizeof(u16);
    u16* linear = (u16*)linearAlloc(linearBytes);
    if (linear == nullptr) {
        GFX_C3D_LOG("%s: linearAlloc(%ux%u) failed\n", who, fbW, fbH);
        return false;
    }
    if (mFrameActive) {
        FlushPendingVbo();
        C3D_FrameSplit((u8)gdx3ds_overlap_split_flags()); // submit queued draws so the readback sees this frame's content
    }
    /* TRANSITION-GLITCH root cause: C3D_SyncDisplayTransfer is NOT synchronous while a C3D
     * frame is active — citro3d's implementation is `if (inFrame) { C3D_FrameSplit(0);
     * GX_DisplayTransfer(...); }` with NO wait (renderqueue.c), the transfer merely joins the
     * gx command queue behind everything else pending (including the frame mirror's async
     * GX_TextureCopy). The CPU then read `linear` before the PPF ever ran: freed/stale linear
     * heap decoded as the capture — the race->menu wipe redrew exactly that noise (and the
     * historical "B2: in-game SHOT/mirror BMPs black while scanout fine" was this same race).
     * Azahar usually drains the queue fast enough to hide it; on real hardware the PPF always
     * takes real time, so captures there were reliably garbled.
     *
     * Fix: pre-fill a two-word sentinel at the END of the output buffer, then poll it with the
     * data cache invalidated until the transfer's writes land (the gx queue executes in
     * submission order, so our transfer completing also proves every earlier async copy did).
     * A PPF wait alone is NOT sufficient: gsp events are latched, and a stale PPF signal from
     * an earlier texture copy satisfies gspWaitForPPF immediately. Bounded (~400 ms) so a
     * hypothetical dropped transfer degrades to a logged stale read instead of a hang. */
    volatile u32* sentinel = (volatile u32*)((u8*)linear + linearBytes - 8);
    sentinel[0] = 0xDEADC0DEu;
    sentinel[1] = 0x5EA7BEEFu ^ 0x0F0F0F0Fu; /* distinct second word */
    GSPGPU_FlushDataCache((void*)((u8*)linear + linearBytes - 8), 8);
    const u32 transferFlags =
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB5A1) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_SyncDisplayTransfer(colorBuf, GX_BUFFER_DIM(fbW, fbH), (u32*)linear, GX_BUFFER_DIM(fbW, fbH),
                            transferFlags);
    bool landed = false;
    {
        for (int spin = 0; spin < 4000; ++spin) { // 4000 * 100us = 400 ms bound
            GSPGPU_InvalidateDataCache((void*)((u8*)linear + linearBytes - 8), 8);
            if (sentinel[0] != 0xDEADC0DEu || sentinel[1] != (0x5EA7BEEFu ^ 0x0F0F0F0Fu)) {
                landed = true;
                break;
            }
            svcSleepThread(100 * 1000); // 100 us
        }
        if (!landed) {
            GFX_C3D_LOG("%s: display transfer never landed\n", who);
        }
    }
    GSPGPU_InvalidateDataCache(linear, linearBytes);
    gdx3ds_gpuprof_note_readback((unsigned)fbW * fbH);

    for (uint32_t yOut = 0; yOut < height; yOut++) {
        // Output row 0 = top of scene; GL-convention game y counts from the bottom.
        const u32 gy = (u32)(((uint64_t)(height - 1 - yOut) * gameH) / height);
        for (uint32_t xOut = 0; xOut < width; xOut++) {
            const u32 gx = (u32)(((uint64_t)xOut * gameW) / width);
            // Harness-verified un-rotation: game y runs along fb-x from 0 and
            // game x along fb-y from 0 (the first cut used fbH-1-gx and read
            // every scene horizontally mirrored + offset by the dead band).
            const u32 fbX = gy;
            const u32 fbY = gx;
            if (fbX >= fbW || fbY >= fbH) {
                continue;
            }
            const u16 px = linear[(size_t)fbY * fbW + fbX];
            out[(size_t)yOut * width + xOut] = (u16)(px | 0x0001u); // force coverage bit
        }
    }
    linearFree(linear);
    return landed;
}

void GfxRenderingAPIC3D::ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    /* Contract (matches the GL/DX11 backends the transition consumer is validated against):
     * output is packed native-endian RGBA5551 (N64 rgba16 -- the caller byteswaps), row 0 is
     * the TOP of the scene, sizes resample from the framebuffer's real extent, and the alpha
     * bit is forced to 1 (coverage, not host alpha: the game redraws captures through
     * alpha-compare passes that would discard alpha-0 texels).
     *
     * Path: GX display transfer untiles the RGBA8 color buffer into a linear RGB5A1 buffer
     * (PICA RGB5A1 has the same R[15:11] G[10:6] B[5:1] A[0] packing as N64 rgba16), then the
     * CPU un-rotates. Effective content layout (harness-verified via the BMP dumps): game-y
     * runs along fb-x from 0 and game-x runs along fb-y from 0; the dead band is the fb-y
     * tail. */
    if (rgba16Buf == nullptr) {
        return;
    }
    std::memset(rgba16Buf, 0, (size_t)width * height * sizeof(uint16_t));
    if (fbId < 0 || fbId >= (int)mFramebuffers.size() || width == 0 || height == 0) {
        return;
    }
    FramebufferC3D& fb = mFramebuffers[fbId];

    u32* colorBuf = nullptr;
    u32 fbW = 0; // fb-x extent (the axis game-y runs along)
    u32 fbH = 0; // fb-y extent (the axis game-x runs along, negated)
    if (fb.texBacked) {
        colorBuf = (u32*)fb.tex.data;
        fbW = fb.tex.width;
        fbH = fb.tex.height;
    } else if (fb.target != nullptr) {
        colorBuf = (u32*)fb.target->frameBuf.colorBuf;
        fbW = (u32)kScreenHeight; // main portrait target: 240 wide ...
        fbH = (u32)kScreenWidth;  // ... x 400 tall
    }
    if (colorBuf == nullptr || fbW == 0 || fbH == 0) {
        return;
    }

    /* Logical game extent of this framebuffer (fb.width/height track the interpreter's sizes;
     * fall back to the padded extents when unset). */
    const u32 gameW = (fb.width != 0) ? fb.width : fbH;
    const u32 gameH = (fb.height != 0) ? fb.height : fbW;
    /* TRANSITION diag (bounded): the exact un-rotate geometry of every readback. A garbled
       transition capture with clean SHOT readbacks around it means fb.width/height (gameW/H
       here) was rewritten between the two. */
    {
        static int sReadbackLogs = 0;
        if (sReadbackLogs < 96) {
            ++sReadbackLogs;
            GFX_C3D_LOG("[readback] fb=%d texBacked=%d fbWxH=%ux%u game=%ux%u out=%ux%u\n",
                        fbId, fb.texBacked ? 1 : 0, (unsigned)fbW, (unsigned)fbH,
                        (unsigned)gameW, (unsigned)gameH, (unsigned)width, (unsigned)height);
        }
    }
    ReadColorBufRgba16(colorBuf, fbW, fbH, gameW, gameH, width, height, rgba16Buf, "ReadFramebufferToCPU");
}

/* ORACLE true capture (contract in gfx_citro3d.h). Reads the target the GPU drew THIS frame
 * into, not the scanout copy one transfer downstream (that off-by-one was the old oracle's
 * bug: docs/research/reference-diff-2026-08-20.md Part 1). No FrameSplit branch: the caller
 * guarantees no frame is open. The transfer joins citro3d's gx command queue BEHIND the frame's
 * P3D list + display transfer (in-order execution), and the sentinel poll proves it — and hence
 * the frame's GPU work — completed. */
int GfxRenderingAPIC3D::CaptureTargetRgba16(int eye, uint16_t* out, uint32_t width, uint32_t height) {
    if (out == nullptr || width == 0 || height == 0) {
        return -1;
    }
    std::memset(out, 0, (size_t)width * height * sizeof(uint16_t));
    if (!mInitialized || mFrameActive || mFramebuffers.empty()) {
        return -1;
    }
    C3D_RenderTarget* target = nullptr;
    if (eye == 1) {
        if (!mStereoEnabled || !Gdx3dsStereo::Active()) {
            return -2;
        }
        target = Gdx3dsStereo::RightTarget();
    } else {
        target = mFramebuffers[0].target;
    }
    if (target == nullptr || target->frameBuf.colorBuf == nullptr) {
        return -1;
    }
    const u32 fbW = (u32)kScreenHeight; // portrait target: 240 wide ...
    const u32 fbH = (u32)kScreenWidth;  // ... x 400 tall
    const bool landed = ReadColorBufRgba16((u32*)target->frameBuf.colorBuf, fbW, fbH, (u32)kScreenWidth,
                                           (u32)kScreenHeight, width, height, out, "CaptureTargetRgba16");
    return landed ? 0 : 1;
}

/* C entry point for port/3ds/gdx3ds_oracle.c. Same return codes as CaptureTargetRgba16;
 * -1 before Init(). */
extern "C" int gdx3ds_gfx_capture_target(int eye, uint16_t* out, uint32_t width, uint32_t height) {
    if (sGdxOracleApi == nullptr) {
        return -1;
    }
    return sGdxOracleApi->CaptureTargetRgba16(eye, out, width, height);
}

void GfxRenderingAPIC3D::ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) {
    (void)fbIdTarget;
    (void)fbIdSrc;
    // PICA has no MSAA; UpdateFramebufferParameters never allocates multisampled
    // buffers, so there is nothing to resolve.
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIC3D::GetPixelDepth(int fbId, const std::set<std::pair<float, float>>& coordinates) {
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;
        GFX_C3D_LOG("TODO GetPixelDepth(fb %d, %zu coords): depth readback unimplemented, returning empty\n",
                    fbId, coordinates.size());
    }
    return {};
}

void* GfxRenderingAPIC3D::GetFramebufferTextureId(int fbId) {
    if (fbId < 0 || fbId >= (int)mFramebuffers.size() || !mFramebuffers[fbId].texBacked) {
        return nullptr;
    }
    return &mFramebuffers[fbId].tex;
}

void GfxRenderingAPIC3D::SelectTextureFb(int fbId) {
    if (fbId < 0 || fbId >= (int)mFramebuffers.size() || !mFramebuffers[fbId].texBacked) {
        GFX_C3D_LOG("SelectTextureFb: fb %d is not texture-backed\n", fbId);
        return;
    }
    BindTexUnit(0, &mFramebuffers[fbId].tex);
}

/* [native] hudsky_emit: the compact emitter's backend program + lean packed tail. */
#include "gfx_citro3d_hsx.inc"
#include "gfx_citro3d_nrr.inc" /* [native] nrr: the NRR core's backend bridge */

} // namespace Fast

/* ------------------------------------------------------------------------------- */
/* Contract factory (port/3ds/include/gdx3ds_gfx.h, frozen)                          */
/* ------------------------------------------------------------------------------- */

static Fast::GfxRenderingAPIC3D& GdxCitro3dRendererSingleton() {
    static Fast::GfxRenderingAPIC3D sRenderer;
    return sRenderer;
}

Fast::GfxRenderingAPI* Gdx3ds_GetCitro3dRenderer() {
    return &GdxCitro3dRendererSingleton();
}

/* HOMEFIX: [apt] receipts sample whether a C3D frame is open on the backend (render-thread
 * state, racy read from main / the watchdog; diagnostic only). */
extern "C" int gdx3ds_gfx_frame_active(void) {
    return GdxCitro3dRendererSingleton().DiagFrameActive() ? 1 : 0;
}

/* [triloop] strong-link hooks for the interpreter's packed tri path (declared
 * in gdx3ds_vbopack.h; the interpreter only ever calls them from its own DL
 * walk, where the renderer singleton exists by construction). */
extern "C" float* gdx3ds_vbopack_begin(Gdx3dsVboPackParams* params) {
    /* LUS: legacy all-or-nothing contract — a refusal is answered by its legacy loop. */
    return GdxCitro3dRendererSingleton().VboPackBegin(params, nullptr);
}

extern "C" void gdx3ds_vbopack_refresh(Gdx3dsVboPackParams* params) {
    GdxCitro3dRendererSingleton().VboPackRefreshTextures(params);
}
extern "C" void gdx3ds_atlas_arm(int on) {
    GdxCitro3dRendererSingleton().AtlasArm(on != 0);
}
extern "C" int gdx3ds_tex_page(uint32_t texId) {
    return GdxCitro3dRendererSingleton().TexPage(texId);
}
extern "C" uint32_t gdx3ds_tex_bytes(uint32_t texId) { /* N60TEX [debug] tex_keep */
    return GdxCitro3dRendererSingleton().TexBytes(texId);
}
extern "C" void gdx3ds_atlas_reset_all(void) { /* native-all S1 [native] atlas_race_reset */
    GdxCitro3dRendererSingleton().AtlasResetAll();
}
extern "C" void gdx3ds_atlas_stats(unsigned long out[5]) {
    GdxCitro3dRendererSingleton().AtlasStats(out);
}
extern "C" void gdx3ds_vbopack_draw(size_t numTris, const Gdx3dsVboPackAux* aux) {
    GdxCitro3dRendererSingleton().DrawPackedBatch(numTris, aux);
}

/* ------------------------------------------------------------------------------- */
/* Gdx_NativeBatch (gdx3ds_nativebatch.h): shared native draw primitive             */
/* ------------------------------------------------------------------------------- */

namespace Fast {

namespace {
struct NativeBatchState {
    bool open = false;
    size_t startFloats = 0;
    int cls = -1;
    Gdx3dsVboPackAux aux = {};
};
NativeBatchState sNb;
uint32_t sNbDraws = 0;
} // namespace

extern "C" uint32_t Gdx_NativeBatchHashBytes(const void* p, size_t n, uint32_t h) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

extern "C" int Gdx_NativeBatchBeginWithAux(const Gdx3dsVboPackAux* aux, int stereoClass) {
    GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    if (!r.mInitialized || !r.mFrameActive || r.mCurrentShader == nullptr || aux == nullptr) {
        return 0;
    }
    if (sNb.open) {
        Gdx_NativeBatchFlush();
    }
    sNb.open = true;
    sNb.startFloats = r.mVboOffsetFloats;
    sNb.cls = stereoClass;
    sNb.aux = *aux;
    return 1;
}

extern "C" int Gdx_NativeBatchBegin(int stereoClass) {
    Gdx3dsVboPackAux aux = {};
    return Gdx_NativeBatchBeginWithAux(&aux, stereoClass);
}

extern "C" int Gdx_NativeBatchMaterial(const Gdx3dsNativeMaterial* m, int stereoClass) {
    GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    if (m == nullptr || !r.mInitialized || !r.mFrameActive) {
        return 0;
    }
    if (sNb.open) {
        Gdx_NativeBatchFlush();
    }
    ShaderProgram* prg = r.LookupShader(m->shaderId0, m->shaderId1);
    if (prg == nullptr) {
        prg = r.CreateAndLoadNewShader(m->shaderId0, m->shaderId1);
    } else {
        r.LoadShader(prg);
    }
    if (prg == nullptr) {
        return 0;
    }
    for (int t = 0; t < 2; t++) {
        if (r.mCurrentShader->cc.usedTextures[t]) {
            r.SelectTexture(t, m->texId[t]);
        }
    }
    r.SetDepthTestAndMask(m->depthTest != 0, m->depthMask != 0);
    r.SetZmodeDecal(m->decal != 0);
    r.SetUseAlpha(m->useAlpha != 0);
    return Gdx_NativeBatchBeginWithAux(&m->aux, stereoClass);
}

extern "C" int Gdx_NativeBatchOpen(void) {
    return sNb.open ? 1 : 0;
}

extern "C" float* Gdx_NativeBatchCursor(void) {
    GDX3DS_PW_NOTE(GDX3DS_PW_VBO); /* o3ds3d [prewrite] */
    if (!sNb.open) {
        return nullptr;
    }
    GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    return r.mVboPool + r.mVboOffsetFloats;
}

extern "C" float* Gdx_NativeBatchVerts(uint32_t nVerts) {
    GDX3DS_PW_NOTE(GDX3DS_PW_VBO); /* o3ds3d [prewrite] */
    if (!sNb.open) {
        return nullptr;
    }
    GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    const size_t floats = (size_t)nVerts * kOutStrideFloats;
    if (r.mVboOffsetFloats + floats > r.mVboPoolFloats) {
        return nullptr;
    }
    float* p = r.mVboPool + r.mVboOffsetFloats;
    r.mVboOffsetFloats += floats;
    return p;
}

extern "C" void Gdx_NativeBatchCommit(uint32_t nVerts) {
    if (!sNb.open) {
        return;
    }
    GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    const size_t floats = (size_t)nVerts * kOutStrideFloats;
    if (r.mVboOffsetFloats + floats <= r.mVboPoolFloats) {
        r.mVboOffsetFloats += floats;
        return;
    }
    /* GEOMLOSS: the cursor could not advance, but the caller has already written nVerts
     * through the batch cursor and popped them from the interpreter. Those vertices are now
     * outside the range Gdx_NativeBatchFlush will draw AND will be overwritten by the next
     * append. The begin reservation makes this unreachable; count and name it rather than
     * let it be the one pool failure with no trace at all. */
    sNbCommitRefused++;
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;
        GFX_C3D_LOG("GEOMLOSS: Gdx_NativeBatchCommit refused %u verts at %zu/%zu KiB — the appended "
                    "vertices fall outside the flushed range\n",
                    nVerts, r.mVboOffsetFloats * sizeof(float) / 1024,
                    r.mVboPoolFloats * sizeof(float) / 1024);
    }
}

extern "C" uint16_t* Gdx_NativeBatchIndices(uint32_t nIndices) {
    (void)nIndices;
    return nullptr;
}

extern "C" uint32_t Gdx_NativeBatchFlush(void) {
    if (!sNb.open) {
        return 0;
    }
    GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    const size_t end = r.mVboOffsetFloats;
    const size_t floats = end > sNb.startFloats ? end - sNb.startFloats : 0;
    const uint32_t tris = (uint32_t)(floats / (3 * kOutStrideFloats));
    if (tris != 0) {
        r.mVboOffsetFloats = sNb.startFloats;
        gdx3ds_stereo_set_draw_class(sNb.cls);
        if (gdx3ds_replay_rec) {
            gdx3ds_replay_set_draw_tag(gdx3ds_replay_nb_tag); /* C6: the native batch's pose */
        }
        r.DrawPackedBatch(tris, &sNb.aux); /* advances the cursor to `end` on success */
        gdx3ds_stereo_set_draw_class(-1);
        r.mVboOffsetFloats = end;          /* the range is consumed even on a backend early-out */
        sNbDraws++;
    }
    sNb.startFloats = end;
    return tris;
}

extern "C" uint32_t Gdx_NativeBatchDiscard(uint32_t* hash) {
    if (!sNb.open) {
        return 0;
    }
    GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    const size_t end = r.mVboOffsetFloats;
    const size_t floats = end > sNb.startFloats ? end - sNb.startFloats : 0;
    const uint32_t tris = (uint32_t)(floats / (3 * kOutStrideFloats));
    if (hash != nullptr && floats != 0) {
        *hash = Gdx_NativeBatchHashBytes(r.mVboPool + sNb.startFloats, floats * sizeof(float), *hash);
    }
    r.mVboOffsetFloats = sNb.startFloats;
    return tris;
}

extern "C" void Gdx_NativeBatchEnd(void) {
    if (sNb.open) {
        Gdx_NativeBatchFlush();
        sNb.open = false;
    }
}

extern "C" void Gdx_NativeBatchStockHashArm(int on) {
    sNbStockArm = on != 0;
}

extern "C" uint32_t Gdx_NativeBatchStockHash(uint32_t* trisOut, int reset) {
    const uint32_t h = sNbStockHash;
    if (trisOut != nullptr) {
        *trisOut = sNbStockTris;
    }
    if (reset) {
        sNbStockHash = GDX_NATIVEBATCH_HASH_SEED;
        sNbStockTris = 0;
    }
    return h;
}

/* [native] track: link anchor — the marker replay object must be pulled from this archive
 * before the LUS carve archive resolves its weak reference to it (static-archive order). */
extern "C" void gdx3ds_track_marker(Fast::Interpreter* interp, uint32_t w0, uintptr_t w1);
extern "C" void* gdx3ds_track_replay_anchor(void) {
    return reinterpret_cast<void*>(&gdx3ds_track_marker);
}

extern "C" int Gdx_NativeBatchConstInputs(uint32_t* rgbMask, uint32_t* alphaMask) {
    GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    const ShaderProgramC3D* prg = r.mCurrentShader;
    uint32_t rgb = 0, alpha = 0;
    if (prg == nullptr) {
        return 0;
    }
    for (int s = 0; s < prg->numStages; s++) {
        const TexEnvStageC3D& st = prg->stages[s];
        if (st.constRgbInput > 0) {
            rgb |= 1u << (st.constRgbInput - 1);
        }
        if (st.constAlphaInput > 0) {
            alpha |= 1u << (st.constAlphaInput - 1);
        }
    }
    if (rgbMask != nullptr) {
        *rgbMask = rgb;
    }
    if (alphaMask != nullptr) {
        *alphaMask = alpha;
    }
    return 1;
}

/* [native] fx: link anchor, same reason as the track one. */
extern "C" void gdx3ds_fx_marker(Fast::Interpreter* interp, uint32_t w0, uintptr_t w1);
extern "C" void* gdx3ds_fx_replay_anchor(void) {
    return reinterpret_cast<void*>(&gdx3ds_fx_marker);
}

extern "C" uint32_t Gdx_NativeBatchDrainDraws(void) {
    const uint32_t d = sNbDraws;
    sNbDraws = 0;
    return d;
}

} // namespace Fast
/* [native] machines backend entry (gdx3ds_mesh.h). */
extern "C" float* gdx3ds_mesh_backend_begin(Gdx3dsVboPackParams* params, uint32_t minVerts, uint32_t maxIndices,
                                            uint32_t* capVerts) {
    return GdxCitro3dRendererSingleton().NativeMeshBegin(params, minVerts, maxIndices, capVerts);
}
/* C6 [replay]: a machine / track draw uses the interpreter's current matrix state (the mesh
 * path latched it at the G_DL, the track pass at its marker); the backend checks that the gpuxf
 * uniform really was built from it (gdx3ds_replay.cpp XfMatchesEntry). */
static void ReplayTagCurrent(float xscale) {
    if (gdx3ds_replay_rec) {
        gdx3ds_replay_set_draw_tag(gdx3ds_replay_cur_tag(xscale));
    }
}
extern "C" void gdx3ds_mesh_backend_draw(uint32_t nVerts, const uint16_t* idx, uint32_t nIdx,
                                         const Gdx3dsVboPackAux* aux) {
    ReplayTagCurrent(gdx3ds_replay_rec ? gdx3ds_replay_mesh_xscale() : 1.0f);
    if (gdx3ds_replay_rec) {
        gdx3ds_replay_check_mesh_mp();
    }
    GdxCitro3dRendererSingleton().NativeMeshDraw(nVerts, idx, nIdx, aux);
}
extern "C" void* gdx3ds_mesh_backend_xf_vertices(uint32_t nVerts) {
    return GdxCitro3dRendererSingleton().NativeXfVertices(nVerts);
}
extern "C" int gdx3ds_mesh_backend_xf_params(Gdx3dsVboPackParams* params) {
    return GdxCitro3dRendererSingleton().NativeXfParams(params) ? 1 : 0;
}
extern "C" void gdx3ds_mesh_backend_xf_draw(const uint16_t* idx, uint32_t nIdx, const Gdx3dsVboPackAux* aux,
                                            const Gdx3dsXfDraw* xf) {
    ReplayTagCurrent(1.0f);
    GdxCitro3dRendererSingleton().NativeXfDraw(idx, nIdx, aux, xf);
}
extern "C" void gdx3ds_mesh_backend_elide_take(unsigned out[3]) {
    Fast::GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    out[0] = r.mElideVbo;
    out[1] = r.mElideTex;
    out[2] = r.mElideAt;
    r.mElideVbo = r.mElideTex = r.mElideAt = 0;
}
extern "C" void gdx3ds_mesh_backend_flush_resident(const void* p, uint32_t bytes) {
    if (p != nullptr && bytes != 0) {
        Fast::GdxHwsFlush(p, bytes, GDX3DS_HWFL_RES);
    }
}
/* [native] track_retained (gdx3ds_track_retained.h): the gpuxf draw with float positions and UVs. */
extern "C" void gdx3ds_track_backend_xf_draw_fpos(const uint16_t* idx, uint32_t nIdx, const Gdx3dsVboPackAux* aux,
                                                  const Gdx3dsXfDraw* xf) {
    Fast::GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    ReplayTagCurrent(1.0f);
    r.mXfPosFloat = true;
    r.NativeXfDraw(idx, nIdx, aux, xf);
    r.mXfPosFloat = false;
}
/* C6 [replay]: the verify lane reads the main target back. */
extern "C" void* gdx3ds_replay_backend_colorbuf(void) {
    Fast::GfxRenderingAPIC3D& r = GdxCitro3dRendererSingleton();
    return r.ReplayMainColorBuf();
}
extern "C" int gdx3ds_track_backend_viewport_folded(void) {
    return GdxCitro3dRendererSingleton().mVpAffineActive ? 1 : 0;
}
extern "C" int gdx3ds_mesh_backend_xf_ready(void) {
    return GdxCitro3dRendererSingleton().XfReady() ? 1 : 0;
}
extern "C" unsigned long gdx3ds_mesh_backend_linear_free_kib(void) {
    return (unsigned long)(linearSpaceFree() / 1024u);
}
extern "C" void gdx3ds_mesh_backend_set_capture(Gdx3dsMeshCaptureFn fn, void* user, int skipSubmit) {
    Fast::sMeshCaptureFn = fn;
    Fast::sMeshCaptureUser = user;
    Fast::sMeshCaptureSkip = skipSubmit != 0;
}

/* n60 NRR machines: the compact draw's backend half (capture + the hot run). */
#include "gfx_citro3d_nrrmach.inc"
