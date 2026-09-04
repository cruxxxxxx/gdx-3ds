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
#include <cstddef>
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));

#include "gdx3ds_gpu_prof.h" // G-GPUPROF [gpu]/[fill] telemetry (gated: debug.gputrace)
#include "gdx3ds_stereo.h" // stream S: stereo foundation (dual targets + per-eye loop)

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib> // std::getenv (GDX_DIAG_SKY backdrop-coverage diagnostic)
#include <cstring>

extern "C" {
#include "gdx3ds_gfx_shader_shbin.h"
}

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
static void GfxC3dLogImpl(const char* fmt, ...) {
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

/* [trectbatch] killswitch (LOCKED-60 Task A): [debug] trectbatch=0 or env GDX_TRECTBATCH_OFF
 * keeps every rect on the legacy path (no atlas arming, no same-page merge); read weakly by
 * the patched interpreter at every task boundary. */
extern "C" int gdx3ds_trectbatch_enabled(void) {
    static const int sEnabled = [] {
        if (std::getenv("GDX_TRECTBATCH_OFF") != nullptr) {
            return 0;
        }
        if (&gdx3ds_config_get_int != nullptr) {
            return gdx3ds_config_get_int("debug", "trectbatch", 1) != 0 ? 1 : 0;
        }
        return 1;
    }();
    return sEnabled;
}

/* [triloop] packed-draw telemetry for the [c3d] line (dPk). */
static unsigned long sPackedDrawCalls = 0;
/* [trectbatch] atlas page-eviction telemetry for the [c3d] line (atlasEv=), see AtlasTryPlace. */
static unsigned long sAtlasEvictions = 0;     /* pages reclaimed */
static unsigned long sAtlasEvictedViews = 0;  /* views copied out to standalone */
static unsigned long sAtlasEvictCopyFail = 0; /* copy-outs that failed C3D_TexInit (view dropped) */
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

void GfxRenderingAPIC3D::Init() {
    if (mInitialized) {
        return;
    }
    // Precondition: stream B's OS layer has run gfxInitDefault() already.
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);

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
    mVboPoolFloats = kVboPoolBytes / sizeof(float);
    mVboOffsetFloats = 0;

    mTextures.reserve(512);
    mShaderPool.reserve(64); // census-predicted variant population (~40/session observed)
    mStereoEnabled = Gdx3dsStereo::Init(); // config-gated; off = zero further cost
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
extern "C" int gdx3ds_config_loaded(void) __attribute__((weak));

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
            int m = gdx3ds_config_get_int("display", "border_mode", 0);
            sDispMode = (m >= 0 && m <= 3) ? m : 0;
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


void GfxRenderingAPIC3D::StartFrame() {
    if (!mInitialized) {
        return;
    }
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
    }
    sGdxFogFrame++;
    gdx3ds_gpuprof_frame_begin(mFrameActive ? 1 : 0); // owns C3D_FrameBegin(SYNCDRAW)
    C3D_FrameDrawOn(mFramebuffers[0].target);
    if (mStereoEnabled) {
        // Slider poll + right-target frame queueing; re-binds the main target.
        Gdx3dsStereo::FrameBegin(mFramebuffers[0].target);
        mFrameDrawsRightEye = 0;
    }
    mCurrentFramebuffer = 0;
    mVboOffsetFloats = 0;
    mVboFlushedFloats = 0;
    mVboExhaustionLogged = false;
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
    if (DispModeLatched() > 0) {
        float sxS, syS, sxU, syU;
        DispModeFactors(sDispMode, &sxS, &syS, &sxU, &syU);
        DispScaleMatrix(&sDispProjScene, mFixupMatrix, sxS, syS);
        DispScaleMatrix(&sDispProjUi, mFixupMatrix, sxU, syU);
    }
    mFrameActive = true;
}

/* Flush the not-yet-flushed tail of the frame's VBO appends in ONE svc, called
 * right before any GPU submission (EndFrame / the FrameSplit paths). Replaces the
 * old per-DrawTriangles GSPGPU_FlushDataCache — an svc kernel round trip per draw,
 * ~190/frame on menus. */
void GfxRenderingAPIC3D::FlushPendingVbo() {
    if (mVboFlushedFloats < mVboOffsetFloats) {
        GSPGPU_FlushDataCache(mVboPool + mVboFlushedFloats,
                              (u32)((mVboOffsetFloats - mVboFlushedFloats) * sizeof(float)));
        mVboFlushedFloats = mVboOffsetFloats;
    }
}

void GfxRenderingAPIC3D::EndFrame() {
    if (!mInitialized || !mFrameActive) {
        return;
    }
    FlushPendingVbo();
    gdx3ds_gpuprof_frame_end_pre();
    C3D_FrameEnd(0);
    gdx3ds_gpuprof_frame_end_post(mFrameDrawCalls, mFrameTris);
    mFrameActive = false;
    // Present-path telemetry (bounded: ~1 svc line/sec at 60fps). C3D_FrameEnd
    // above queued the display transfer + swap for THIS frame's draws.
    mFrameIndex++;
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
        sPrevUploads = (unsigned long)mTexUploads;
        sPrevImports = sTexCacheImports;
        sPrevMisses = sTexCacheMisses;
        sPrevDeletes = sTexCacheDeletes;
        sPrevSettimgLoads = sSettimgLoads;
        sPrevSettimgMemoHits = sSettimgMemoHits;
        sTexMissWindowBudget = 6; // refill the non-race [texmiss] sampling budget
        char msg[288];
        int n = std::snprintf(msg, sizeof(msg),
                              "[c3d] frame=%lu draws=%lu (scr=%lu tex=%lu) tris=%lu fbBinds=%lu "
                              "bindMiss=%lu texUp=%lu texUpFail=%lu texImp=%lu texMiss=%lu texDel=%lu "
                              "dUp=%lu dImp=%lu dMiss=%lu dDel=%lu dRl=%lu dRm=%lu dPk=%lu",
                              (unsigned long)mFrameIndex, (unsigned long)mFrameDrawCalls,
                              (unsigned long)mFrameDrawsScreenFb, (unsigned long)mFrameDrawsTexFb,
                              (unsigned long)mFrameTris, (unsigned long)mFrameFbBinds,
                              (unsigned long)mFrameBindMisses, (unsigned long)mTexUploads,
                              (unsigned long)mTexUploadFails, sTexCacheImports, sTexCacheMisses,
                              sTexCacheDeletes, dUp, dImp, dMiss, dDel, dRl, dRm, dPk);
        if (mAtlasPlaced > 0 && n > 0 && n < (int)sizeof(msg)) {
            // [trectbatch] atlas-only suffix (never emitted with trectbatch=0): page evictions /
            // views copied out to standalone / copy-out failures / no-safe-candidate fallbacks.
            n += std::snprintf(msg + n, sizeof(msg) - n, " atlasEv=%lu/%lu/%lu/%lu", sAtlasEvictions,
                               sAtlasEvictedViews, sAtlasEvictCopyFail, sAtlasEvictNone);
        }
        if (mStereoEnabled && n > 0 && n < (int)sizeof(msg)) {
            // Stereo-only suffix: the off-state [c3d] line stays byte-identical.
            n += std::snprintf(msg + n, sizeof(msg) - n, " eyeR=%lu stereo=%d",
                               (unsigned long)mFrameDrawsRightEye, Gdx3dsStereo::Active() ? 1 : 0);
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

void GfxRenderingAPIC3D::LogUnmappedCombiner(const ShaderProgramC3D& prg, const char* reason) {
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
    if (mCurrentShader == nullptr) {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        return;
    }
    const CCFeatures& cc = mCurrentShader->cc;
    if (cc.opt_texture_edge) {
        C3D_AlphaTest(true, GPU_GEQUAL, 128);
    } else if (cc.opt_alpha_threshold) {
        // G_AC_THRESHOLD: compare against the SETBLENDCOLOR alpha, pushed per flush.
        C3D_AlphaTest(true, GPU_GEQUAL, (int)(mCurrentAlphaCompareThreshold * 255.0f));
    } else {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    }
    mAlphaCompareThresholdDirty = false;
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
    if (w == 0 || h == 0 || w > kAtlasMaxCellW || h > kAtlasMaxCellH) {
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
                GSPGPU_FlushDataCache(tex.data, tex.size);
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
                GFX_C3D_LOG("[atlas] C3D_TexInit(%ux%u) failed (linear heap exhausted?)\n", kAtlasPageW, kAtlasPageH);
                if ((size_t)pageIdx == mAtlasPages.size() - 1) {
                    mAtlasPages.pop_back();
                }
                return false;
            }
            std::memset(pg.tex.data, 0, pg.tex.size);
            GSPGPU_FlushDataCache(pg.tex.data, pg.tex.size);
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
    for (int gy = -(int)kAtlasGutter; gy < (int)(h + kAtlasGutter); gy++) {
        const uint32_t sy = (uint32_t)(gy < 0 ? 0 : (gy >= (int)h ? (int)h - 1 : gy));
        const uint8_t* srcRow = rgba32 + (size_t)sy * w * 4;
        const uint32_t rowFromTop = py + kAtlasGutter + (uint32_t)gy;
        const uint32_t dy = kAtlasPageH - 1 - rowFromTop;
        const uint32_t tileY = dy >> 3;
        for (int gx = -(int)kAtlasGutter; gx < (int)(w + kAtlasGutter); gx++) {
            const uint32_t sx = (uint32_t)(gx < 0 ? 0 : (gx >= (int)w ? (int)w - 1 : gx));
            const uint8_t* p = srcRow + (size_t)sx * 4;
            const uint32_t texel = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
            const uint32_t x = px + kAtlasGutter + (uint32_t)gx;
            const uint32_t offset = ((tileY * tilesPerRow + (x >> 3)) << 6) | MortonInterleave(x, dy);
            dst[offset] = texel;
        }
    }
    {
        /* flush the touched tile rows only: rows [top, bottom] of the cell in flipped space */
        const uint32_t rowTop = kAtlasPageH - 1 - (py + ch - 1);
        const uint32_t rowBot = kAtlasPageH - 1 - py;
        const uint32_t tileRow0 = rowTop >> 3;
        const uint32_t tileRow1 = rowBot >> 3;
        const size_t bytesPerTileRow = (size_t)tilesPerRow * 64u * 4u;
        GSPGPU_FlushDataCache((uint8_t*)pg.tex.data + (size_t)tileRow0 * bytesPerTileRow,
                              (tileRow1 - tileRow0 + 1) * bytesPerTileRow);
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
    mAtlasPlaced++;
    mTexUploads++;
    gdx3ds_gpuprof_note_tex_upload((unsigned)(cw * ch * 4u));
    return true;
}

void GfxRenderingAPIC3D::AtlasRelease(TextureC3D& t) {
    if (t.page < 0) {
        return;
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
            if (!prg.cc.clamp[t][0] && (tex.cms & G_TX_CLAMP) != 0 && tex.width < tex.padWidth) {
                params->wrapClampU[t] = ((float)tex.width - 0.5f) / (float)tex.padWidth;
            }
            if (!prg.cc.clamp[t][1] && (tex.cmt & G_TX_CLAMP) != 0 && tex.height < tex.padHeight) {
                params->wrapClampV[t] = ((float)tex.height - 0.5f) / (float)tex.padHeight;
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

void GfxRenderingAPIC3D::UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) {
    const uint32_t id = mBoundTextureIds[mLastSelectedTile];
    if (id >= mTextures.size() || width == 0 || height == 0) {
        mTexUploadFails++;
        GFX_C3D_LOG("UploadTexture: invalid texture id %u (%ux%u)\n", id, width, height);
        return;
    }
    TextureC3D& t = mTextures[id];
    /* [trectbatch] a recycled id that was an atlas view drops its cell reference first; an
     * armed (eligible rect) upload then tries the atlas and falls back to standalone. */
    if (t.page >= 0) {
        AtlasRelease(t);
    }
    if (mAtlasArm && AtlasTryPlace(t, rgba32Buf, width, height)) {
        return;
    }

    // PICA needs pow2 dimensions, minimum 8. Pad and rescale UVs at repack time.
    const uint32_t padW = NextPow2(width < 8 ? 8 : width);
    const uint32_t padH = NextPow2(height < 8 ? 8 : height);
    if (padW > 1024 || padH > 1024) {
        mTexUploadFails++;
        GFX_C3D_LOG("UploadTexture: %ux%u exceeds the PICA 1024 limit\n", width, height);
        return;
    }

    if (t.inited && (t.padWidth != padW || t.padHeight != padH)) {
        C3D_TexDelete(&t.tex);
        t.inited = false;
    }
    if (!t.inited) {
        if (!C3D_TexInit(&t.tex, (u16)padW, (u16)padH, GPU_RGBA8)) {
            mTexUploadFails++;
            GFX_C3D_LOG("UploadTexture: C3D_TexInit(%ux%u) failed (linear heap exhausted?)\n", padW, padH);
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
    const uint32_t tilesPerRow = padW >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* srcRow = rgba32Buf + (size_t)y * width * 4;
        const uint32_t dy = padH - 1 - y;
        const uint32_t tileY = dy >> 3;
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* px = srcRow + (size_t)x * 4;
            const uint32_t texel =
                ((uint32_t)px[0] << 24) | ((uint32_t)px[1] << 16) | ((uint32_t)px[2] << 8) | px[3];
            const uint32_t offset = ((tileY * tilesPerRow + (x >> 3)) << 6) | MortonInterleave(x, dy);
            dst[offset] = texel;
        }
    }
    GSPGPU_FlushDataCache(t.tex.data, t.tex.size);

    t.width = width;
    t.height = height;
    t.padWidth = padW;
    t.padHeight = padH;
    t.uScale = (float)width / (float)padW;
    t.vScale = (float)height / (float)padH;
    mTexUploads++;
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
    const bool texBacked = mCurrentFramebuffer >= 0 && mCurrentFramebuffer < (int)mFramebuffers.size() &&
                           mFramebuffers[mCurrentFramebuffer].texBacked;
    const bool inside = (x >= 0 && y >= 0 && x + width <= 400 && y + height <= 240);
    /* [vp] telemetry (verbose gate): non-full-window viewports logged with a frame stamp,
       only after 45 s of uptime (skips the intro), 400 lines max. */
    {
        static int sVpLogs = 0;
        static u64 sVpT0 = 0;
        if (sVpT0 == 0) { sVpT0 = osGetTime(); }
        const bool full = (x == 0 && y == 0 && width == 400 && height == 240);
        if (!full && sVpLogs < 400 && (osGetTime() - sVpT0) > 45000 && gdx3ds_config_get_int("debug", "verbose", 0) != 0) {
            ++sVpLogs;
            char line[160];
            const int n = snprintf(line, sizeof(line), "[vp] f=%lu viewport x=%d y=%d w=%d h=%d inside=%d\n",
                                   (unsigned long)mFrameIndex, x, y, width, height, (int)inside);
            if (n > 0 && &gdx3ds_filelog_write != nullptr) {
                gdx3ds_filelog_write(line, (size_t)n);
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
        if (&gdx_get_widescreen_geometry_xscale != nullptr) {
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
        float sxS, syS, sxU, syU;
        DispModeFactors(sDispMode, &sxS, &syS, &sxU, &syU);
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

void GfxRenderingAPIC3D::DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) {
    /* [prof] DRW: repack loop + state application + C3D submission. RAII so the
     * early-outs (no shader / stride mismatch / VBO exhaustion) still close the
     * section; the parent TRI/RUN scope subtracts this via the child counter. */
    struct ProfDrwScope {
        uint64_t t0 = 0, snap = 0;
        bool on;
        ProfDrwScope() : on(gdx3ds_prof_active != 0) {
            if (on) {
                t0 = gdx3ds_prof_enter(&snap);
            }
        }
        ~ProfDrwScope() {
            if (on) {
                gdx3ds_prof_exit(GDX3DS_PROF_DRW, t0, snap);
            }
        }
    } profDrwScope;
    if (!mInitialized || !mFrameActive || bufVboNumTris == 0) {
        return;
    }
    if (mCurrentShader == nullptr) {
        GFX_C3D_LOG("DrawTriangles with no shader loaded — dropping %zu tris\n", bufVboNumTris);
        return;
    }
    const ShaderProgramC3D& prg = *mCurrentShader;
    const size_t numVerts = bufVboNumTris * 3;
    const size_t inStride = bufVboLen / numVerts;
    if (mPackedAux == nullptr && inStride != prg.inputStride) {
        GFX_C3D_LOG("DrawTriangles stride mismatch: got %zu floats/vtx, shader %016llx/%016llx expects %u — "
                    "dropping %zu tris\n",
                    inStride, (unsigned long long)prg.id0, (unsigned long long)prg.id1, prg.inputStride,
                    bufVboNumTris);
        return;
    }
    if (mVboOffsetFloats + numVerts * kOutStrideFloats > mVboPoolFloats) {
        if (!mVboExhaustionLogged) {
            mVboExhaustionLogged = true;
            GFX_C3D_LOG("VBO pool exhausted (%zu KiB) — dropping the rest of the frame's geometry\n",
                        kVboPoolBytes / 1024);
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
                } else if ((t.cms & G_TX_CLAMP) != 0 && t.width < t.padWidth) {
                    uClampMax[tile] = ((float)t.width - 0.5f) / (float)t.padWidth;
                }
                if (prg.cc.clamp[tile][1]) {
                    if (!packed) {
                        const int slot = prg.uvOffset[tile] + 2 + (prg.cc.clamp[tile][0] ? 1 : 0);
                        vClampMax[tile] = bufVbo[slot] * t.vScale;
                    }
                } else if ((t.cmt & G_TX_CLAMP) != 0 && t.height < t.padHeight) {
                    vClampMax[tile] = ((float)t.height - 0.5f) / (float)t.padHeight;
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
            C3D_TexBind(tile, bindTex);
            gdx3ds_gpuprof_note_tex_bind(texId); // S12: distinct textures touched/frame
        } else {
            mFrameBindMisses++;
            GFX_C3D_LOG("DrawTriangles: shader wants texel%d but texture %u is not uploaded\n", tile,
                        mBoundTextureIds[tile]);
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
        static const bool sDiagSky = [] {
            if (std::getenv("GDX_DIAG_SKY") != nullptr) {
                return true;
            }
            if (&gdx3ds_config_get_int != nullptr) {
                return gdx3ds_config_get_int("debug", "diag_sky", 0) != 0;
            }
            return false;
        }();
        const bool raceActiveSky = (&gGdxRaceActive != nullptr) && (gGdxRaceActive != 0);
        if (sDiagSky && raceActiveSky && bufVboNumTris == 2) {
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
                    const float pv = packed ? src[5] : src[uvOff0 + 1] * vScale[0];
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
                                vClampMax[0]);
                }
            }
        }
    }
#endif

    const uint32_t constReappliedMask =
        packed ? RefreshStageConstantsPacked(prg, *mPackedAux) : RefreshStageConstants(prg, bufVbo);

    // [livery] draw receipt (one-shot window, see GdxLiveryDiagFrameBegin): EVERY
    // textured draw's bound texture identities + first-vertex unit-0 UV + the
    // input-driven stage constant when one is bound (machine bodies: ENV colour,
    // r in the LOW byte). The texture names resolve through the id→name table the
    // import hook maintains, so warm-cache binds are named too.
    if (sLiveryArmed && (prg.cc.usedTextures[0] || prg.cc.usedTextures[1])) {
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
    }

    // [prim] draw receipt (one-shot window, see GdxPrimDiagFrameBegin): every draw
    // whose shader consumes an input-driven stage constant — machine bodies bind
    // their ENV body colour here (c0), so the player's body draw must log blue.
    if (sPrimDiagArmed && mLastDrawHadInputConst) {
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
    }

    ApplyAlphaTest();

    // C2 diagnostic kill-switch: a "gdx-nofog.txt" file at the SD root disables the
    // PICA fog unit entirely, discriminating fog-unit output from combiner/shade
    // output in race captures. Checked once; delete the file to restore fog.
    static const bool sGdxNoFog = [] {
        FILE* f = fopen("gdx-nofog.txt", "rb");
        if (f != nullptr) {
            fclose(f);
            return true;
        }
        return false;
    }();
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

    const bool texBacked = mCurrentFramebuffer >= 0 && mCurrentFramebuffer < (int)mFramebuffers.size() &&
                           mFramebuffers[mCurrentFramebuffer].texBacked;

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, dst, kOutStrideFloats * sizeof(float), 4, 0x3210);
    /* MENU DISP: pick this draw's projection variant (scene vs UI, see the border-mode
     * block above). Texture-backed passes and mode 0 keep the plain fixup. */
    /* [anchor] killswitch [debug] stereo_anchor (default 1). */
    static int sAnchorOn = -1;
    if (sAnchorOn < 0) {
        sAnchorOn = gdx3ds_config_get_int("debug", "stereo_anchor", 1) != 0 ? 1 : 0;
    }
    const bool anchoredDraw = sAnchorOn != 0 && !texBacked && prg.cc.opt_prim_depth && bufVbo[3] == 1.0f;
    const C3D_Mtx* dispBase = &mFixupMatrix;
    if (!texBacked && DispModeLatched() > 0) {
        const int dispCls = Gdx3dsStereo::ClassifyDraw(bufVbo[3] == 1.0f);
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
    if (mStereoEnabled && Gdx3dsStereo::Active()) {
        /* Per-eye draw loop (stereo foundation): re-issue the SAME repacked VBO
         * range per eye — only the target binding and the projection uniform
         * change (no CPU vertex re-copy; see gdx3ds_stereo.h). Offscreen
         * (texture-backed) passes render once, mono, at the center matrix. */
        C3D_Mtx eyeMtx;
        if (texBacked) {
            Gdx3dsStereo::ComputeEyeMatrix(&mFixupMatrix, 0, &eyeMtx);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
            C3D_DrawArrays(GPU_TRIANGLES, 0, (int)numVerts);
        } else {
            // Stereo class: bridge override, else ortho heuristic (clip w == 1
            // on vertex 0 -> HUD/menu content at zero parallax).
            int cls = Gdx3dsStereo::ClassifyDraw(bufVbo[3] == 1.0f);
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
            }
            if (cls == GDX3DS_STEREO_UI_ZERO_PARALLAX) {
                // HUD/2D at screen depth: one center matrix for both eyes —
                // vertex uniforms survive the raw target switch, so upload once.
                Gdx3dsStereo::ComputeEyeMatrix(dispBase, 0, &eyeMtx);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
                C3D_DrawArrays(GPU_TRIANGLES, 0, (int)numVerts);
                Gdx3dsStereo::BindTargetRaw(Gdx3dsStereo::RightTarget());
                C3D_DrawArrays(GPU_TRIANGLES, 0, (int)numVerts);
            } else {
                // Left eye: the current (main) target is already bound.
                Gdx3dsStereo::ComputeEyeMatrix(dispBase, -1, &eyeMtx, cls);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
                C3D_DrawArrays(GPU_TRIANGLES, 0, (int)numVerts);
                // Right eye: raw switch keeps viewport/scissor (gdx3ds_stereo.h).
                Gdx3dsStereo::BindTargetRaw(Gdx3dsStereo::RightTarget());
                Gdx3dsStereo::ComputeEyeMatrix(dispBase, 1, &eyeMtx, cls);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, &eyeMtx);
                C3D_DrawArrays(GPU_TRIANGLES, 0, (int)numVerts);
            }
            C3D_RenderTarget* back = mFramebuffers[mCurrentFramebuffer].target;
            Gdx3dsStereo::BindTargetRaw(back != nullptr ? back : mFramebuffers[0].target);
            mFrameDrawsRightEye++;
        }
        sDispProjLast = nullptr; // eye uploads overwrote the uniform: force re-upload
    } else {
        if (dispBase != sDispProjLast) {
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, dispBase);
            sDispProjLast = (dispBase == &vpMtx) ? nullptr : dispBase;
        }
        C3D_DrawArrays(GPU_TRIANGLES, 0, (int)numVerts);
    }

    mVboOffsetFloats += numVerts * kOutStrideFloats;
    mFrameDrawCalls++;
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
/* [triloop] direct PICA-layout emission (gdx3ds_vbopack.h)                          */
/* ------------------------------------------------------------------------------- */

float* GfxRenderingAPIC3D::VboPackBegin(Gdx3dsVboPackParams* params) {
    if (!GdxTriloopEnabled()) {
        return nullptr;
    }
    if (!mInitialized || !mFrameActive || mCurrentShader == nullptr) {
        return nullptr;
    }
    // Reserve room for a full interpreter batch (MAX_TRI_BUFFER = 256 tris);
    // refusing near exhaustion falls back to the legacy path, which drops at
    // draw time exactly as before.
    constexpr size_t kBatchReserveFloats = 256 * 3 * kOutStrideFloats;
    if (mVboOffsetFloats + kBatchReserveFloats > mVboPoolFloats) {
        return nullptr;
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

void GfxRenderingAPIC3D::DrawPackedBatch(size_t numTris, const Gdx3dsVboPackAux* aux) {
    // Route through DrawTriangles so every early-out, diagnostic and the
    // submission tail stay shared; mPackedAux flags the repack-free mode.
    mPackedAux = aux;
    DrawTriangles(mVboPool + mVboOffsetFloats, numTris * 3 * kOutStrideFloats, numTris);
    mPackedAux = nullptr;
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
        it = mFogLutCache.emplace(key, lut).first;
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
    if (mFrameActive) {
        C3D_FrameDrawOn(mFramebuffers[fbId].target);
        mFrameFbBinds++;
        const FramebufferC3D& fb = mFramebuffers[fbId];
        gdx3ds_gpuprof_note_pass(fb.texBacked ? 1 : 0,
                                 fb.texBacked ? (unsigned)fb.tex.width * fb.tex.height : 96000u);
    }
}

void GfxRenderingAPIC3D::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                         int dstX0, int dstY0, int dstX1, int dstY1) {
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
        C3D_FrameSplit(0);
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

void GfxRenderingAPIC3D::ClearFramebuffer(bool color, bool depth) {
    if (mCurrentFramebuffer >= (int)mFramebuffers.size()) {
        return;
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
        C3D_RenderTargetClear(target, flags, 0x000000FF /* opaque black */, kClearDepthFar);
        if (mStereoEnabled && mCurrentFramebuffer == 0) {
            Gdx3dsStereo::ClearRight(flags, 0x000000FF, kClearDepthFar); // no-op unless Active()
        }
    }
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

    const size_t linearBytes = (size_t)fbW * fbH * sizeof(u16);
    u16* linear = (u16*)linearAlloc(linearBytes);
    if (linear == nullptr) {
        GFX_C3D_LOG("ReadFramebufferToCPU: linearAlloc(%ux%u) failed\n", fbW, fbH);
        return;
    }
    if (mFrameActive) {
        FlushPendingVbo();
        C3D_FrameSplit(0); // submit queued draws so the readback sees this frame's content
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
    {
        bool landed = false;
        for (int spin = 0; spin < 4000; ++spin) { // 4000 * 100us = 400 ms bound
            GSPGPU_InvalidateDataCache((void*)((u8*)linear + linearBytes - 8), 8);
            if (sentinel[0] != 0xDEADC0DEu || sentinel[1] != (0x5EA7BEEFu ^ 0x0F0F0F0Fu)) {
                landed = true;
                break;
            }
            svcSleepThread(100 * 1000); // 100 us
        }
        if (!landed) {
            GFX_C3D_LOG("ReadFramebufferToCPU: display transfer never landed (fb %d)\n", fbId);
        }
    }
    GSPGPU_InvalidateDataCache(linear, linearBytes);
    gdx3ds_gpuprof_note_readback((unsigned)fbW * fbH);

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
            rgba16Buf[(size_t)yOut * width + xOut] = (u16)(px | 0x0001u); // force coverage bit
        }
    }
    linearFree(linear);
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
    C3D_TexBind(0, &mFramebuffers[fbId].tex);
}

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

/* [triloop] strong-link hooks for the interpreter's packed tri path (declared
 * in gdx3ds_vbopack.h; the interpreter only ever calls them from its own DL
 * walk, where the renderer singleton exists by construction). */
extern "C" float* gdx3ds_vbopack_begin(Gdx3dsVboPackParams* params) {
    return GdxCitro3dRendererSingleton().VboPackBegin(params);
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
extern "C" void gdx3ds_atlas_stats(unsigned long out[5]) {
    GdxCitro3dRendererSingleton().AtlasStats(out);
}
extern "C" void gdx3ds_vbopack_draw(size_t numTris, const Gdx3dsVboPackAux* aux) {
    GdxCitro3dRendererSingleton().DrawPackedBatch(numTris, aux);
}
