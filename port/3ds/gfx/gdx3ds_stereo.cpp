/* port/3ds/gfx/gdx3ds_stereo.cpp — stream S: stereoscopic-3D foundation.
 * See gdx3ds_stereo.h for the design contract and STEREO.md for what remains.
 *
 * Verified references (docs/research/stereo3d-research.md):
 *  - devkitPro composite_scene: two 240x400 RGBA8+D24S8 targets, GFX_LEFT /
 *    GFX_RIGHT, gfxSet3D(true), right pass skipped at slider 0.
 *  - sm64-3ds (mkst / Wyatt-James): per-draw dual emission with a raw
 *    C3D_SetFrameBuf target switch (stock C3D_FrameDrawOn resets the viewport),
 *    identity-shear "stereoTilt" placeholder with iodZ=8 / iodW=16 — reproduced
 *    here as the clearly-marked placeholder, WITHOUT its CPU-side second vertex
 *    buffer copy (we re-issue the draw from the already-repacked VBO).
 */
#include "gdx3ds_stereo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* mstereo: the menu layout's per-draw helpers run only in menu frames with the slider up. */
#define GDX_STEREO_COLD __attribute__((cold, noinline))

// Render-thread identity + menu echo gate (both weak: harness builds link neither).
extern "C" int gdx3ds_rt_on_render_thread(void) __attribute__((weak));
extern "C" int gdx3ds_console_echo_enabled __attribute__((weak));
/* StereoLog's SD-filelog sink (weak, like the rest: the DL harness links neither). */
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));

/* Config comes from stream B's OS library; the DL harness links the backend
 * without it, so resolve the getters weakly (same pattern as gGdxRaceActive in
 * gfx_citro3d.cpp). */
extern "C" int gdx3ds_config_get_bool(const char* section, const char* key, int fallback)
    __attribute__((weak));
extern "C" const char* gdx3ds_config_get_string(const char* section, const char* key,
                                                const char* fallback) __attribute__((weak));
/* mstereo: GET_MODE(gGameMode) (port/input_bridge.c; weak: the DL harness links no game). */
extern "C" int gdx_input_game_mode(void) __attribute__((weak));
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" int gdx3ds_config_set_int(const char* section, const char* key, int value) __attribute__((weak));
/* o3ds60 [stereo] mp_3d: this console's player in the online race being driven, -1 outside one. */
extern "C" int gdx3ds_mpo_local_player(void) __attribute__((weak));

namespace {

constexpr int kEyeWidth = 400; // game-space landscape; targets allocate portrait
constexpr int kEyeHeight = 240;

/* Stereo tuning (ini-overridable, see Init):
 *  - kDefaultIodPixels: on-screen parallax, in top-screen pixels, of a point at
 *    the FAR plane with the slider fully up. 12px ≈ 3% of the 400px screen —
 *    inside the comfort budget every 3DS launch title observed (Nintendo's
 *    guidance caps divergence well under the ~10mm physical eye separation the
 *    parallax barrier can address; 12px ≈ 2.3mm on the 77mm-wide panel).
 *  - kDefaultConvergence: NDC depth (z/w in [0,1], 0 = near plane, 1 = far
 *    plane) of the ZERO-PARALLAX plane. 0.25 puts the racing camera's subject
 *    (own craft, near track) at/near screen depth; everything beyond recedes
 *    into the screen, the closest sliver of track pops out slightly. */
constexpr float kDefaultIodPixels = 12.0f;
constexpr float kDefaultConvergence = 0.25f;

bool sEnabled = false;                    // config latch, fixed after Init()
bool sActive = false;                     // this frame: right-eye pass on
float sSlider = 0.0f;                     // osGet3DSliderState(), polled per frame
float sIodPx = kDefaultIodPixels;         // far-plane parallax in px (menu-visible)
float sSepNdc = kDefaultIodPixels / (kEyeWidth * 0.5f); // far-plane parallax, NDC-x units
float sConvergence = kDefaultConvergence; // zero-parallax plane, NDC depth
float sAnchorDepth = 0.5f;                // [anchor] NDC depth for GDX3DS_STEREO_ANCHORED draws
C3D_RenderTarget* sRightTarget = nullptr; // 240x400 RGBA8+D24S8, GFX_RIGHT
int sDrawClassOverride = -1;              // bridge tag channel; -1 = heuristic
bool sForceEnable = false;                // harness hook (no config lib linked)
bool sActivationLogged = false;
/* B11q taskless-hold state. sHoldFrame is set by the bridge for the frame it is
 * re-presenting from the frame mirror and cleared by the backend at EndFrame. */
bool sHoldFrame = false;
int sHoldKeep = -1; // [debug] stereo_hold_keep, read once; -1 = unread
unsigned long sHoldFrames = 0;
bool sHoldLogged = false;

/* mstereo [stereo] menu_depth (gdx3ds_stereo.h). Disparities (R - L) in top-screen px at full slider and
 * the default iod; MenuScale() turns them into this frame's. Latched at Init, menu frames at FrameBegin. */
constexpr float kMenuBackPx = 4.0f;  // backdrop, behind the screen
constexpr float kMenuFrontPx = 2.0f; // text / arrows / cursors, in front of the screen
constexpr float kMenuModelPx = 0.0f; // menu 3D (models, the tiled transitions): flat, at the screen plane
constexpr float kMenuClampPx = 8.0f; // ini values are clamped to +-this: subtle by contract
/* Size breakpoints, px: a draw's size is the largest extent (width or height) of any one of its
 * triangles -- a rectangle's own size, whatever the batching around it: <= kMenuFrontSize front,
 * kMenuPanelLo..kMenuPanelHi the plane, >= kMenuBackSize back; linear between. */
constexpr float kMenuFrontSize = 24.0f;
constexpr float kMenuPanelLo = 32.0f;
constexpr float kMenuPanelHi = 160.0f;
constexpr float kMenuBackSize = 272.0f;
int sMenuDepth = 0;
float sMenuBackPx = kMenuBackPx;
float sMenuFrontPx = kMenuFrontPx;
float sMenuModelPx = kMenuModelPx;
bool sMenuFrame = false;    // the open frame takes the menu layout
float sMenuUiShift = 0.0f;  // right-eye NDC-x shift of the next MENU_UI draw (left eye: negated)
bool sMenuLogged = false;
uint32_t sMenuCensusPeriod = 0; /* [debug] stereo_census: the NRR runs' [stc] receipts (p=N) */
uint32_t sMenuCensusFrame = 0;
uint32_t sMenuCensusDraw = 0;
struct MenuWin {
    unsigned frames, ui, back, front, plane, edge, model;
} sMenuWin = {};

void StereoLog(const char* msg) {
    /* Console echo only from the main thread and only while the touch menu does not own the
     * console: the libctru console is not thread-safe and a newline on the bottom row scrolls
     * the menu's tab bar away. svc + (via the port logger) filelog always. */
    {
        const bool muted = (&gdx3ds_rt_on_render_thread != nullptr) && gdx3ds_rt_on_render_thread();
        const bool echoOff = (&gdx3ds_console_echo_enabled != nullptr) && !gdx3ds_console_echo_enabled;
        if (!muted && !echoOff) {
            std::fprintf(stderr, "[gdx3ds_stereo] %s\n", msg);
        }
    }
    const size_t len = std::strlen(msg);
    svcOutputDebugString(msg, len);
    /* The comment above has always promised the filelog; the call was missing, so the
     * "[stereo] right-eye pass ACTIVE" receipt — the one line that says the pass ran at all
     * — never reached the SD log, i.e. never reached the RetroArch lane, hardware, or
     * hwlink. Three lines per boot at most (init, activation, an allocation failure). */
    if (&gdx3ds_filelog_write != nullptr) {
        gdx3ds_filelog_write(msg, len);
    }
}

float ClampF(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    } else if (v > hi) {
        return hi;
    } else {
        return v;
    }
}

/* Float config through the string getter (the config lib has no float API);
 * falls back on missing lib, missing key, or unparsable value. */
float ConfigGetFloat(const char* section, const char* key, float fallback) {
    if (gdx3ds_config_get_string == nullptr) {
        return fallback;
    }
    const char* v = gdx3ds_config_get_string(section, key, nullptr);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const float parsed = std::strtof(v, &end);
    if (end == v) {
        return fallback;
    }
    return parsed;
}

/* ---- mstereo [stereo] menu_depth -------------------------------------------------------------- */

/* GET_MODE values (decomp/include/fzx_game.h) of the screens the layout applies to. Races, the
 * records flyby (6: a course scene under its table), demos, credits and the editors keep the race's
 * stereo. */
bool IsMenuMode(int mode) {
    switch (mode) {
        case 0x00: /* TITLE */
        case 0x07: /* MAIN_MENU */
        case 0x08: /* MACHINE_SELECT */
        case 0x09: /* MACHINE_SETTINGS */
        case 0x0A: /* COURSE_SELECT */
        case 0x0F: /* GP_RACE_NEXT_COURSE: the next-course card */
        case 0x12: /* GP_RACE_NEXT_MACHINE_SETTINGS */
        case 0x13: /* RECORDS_COURSE_SELECT */
        case 0x14: /* OPTIONS_MENU */
            return true;
        default:
            return false;
    }
}

void MenuDepthInit() {
    sMenuDepth = 0;
    if (gdx3ds_config_get_bool != nullptr) {
        sMenuDepth = gdx3ds_config_get_bool("stereo", "menu_depth", 0) ? 1 : 0;
    }
    sMenuBackPx = ClampF(ConfigGetFloat("stereo", "menu_back", kMenuBackPx), -kMenuClampPx, kMenuClampPx);
    sMenuFrontPx = ClampF(ConfigGetFloat("stereo", "menu_front", kMenuFrontPx), -kMenuClampPx, kMenuClampPx);
    sMenuModelPx = ClampF(ConfigGetFloat("stereo", "menu_model", kMenuModelPx), -kMenuClampPx, kMenuClampPx);
    const float census = ConfigGetFloat("debug", "stereo_census", 0.0f);
    sMenuCensusPeriod = census >= 1.0f ? (uint32_t)census : 0u;
    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "[stereo] menu_depth=%d back=%+.1f front=%+.1f model=%+.1f px (R-L disparity at full slider, iod 12)",
                  sMenuDepth, (double)sMenuBackPx, (double)-sMenuFrontPx, (double)sMenuModelPx);
    StereoLog(msg);
}

/* px of disparity at full slider / iod 12 -> this frame's right-eye NDC-x shift: half the disparity
 * per eye, 200 px per NDC unit, scaled by the slider and iod exactly as the scene's parallax is. */
float MenuShiftNdc(float disparityPx) {
    return disparityPx * (sIodPx / kDefaultIodPixels) * sSlider / (2.0f * (kEyeWidth * 0.5f));
}

void MenuWinEmit() {
    if (sMenuWin.frames < 256u) {
        return;
    }
    char msg[200];
    std::snprintf(msg, sizeof(msg),
                  "[stereo] menu frames=%u ui=%u back=%u front=%u plane=%u edge=%u model=%u slider=%.2f iod=%.1f",
                  sMenuWin.frames, sMenuWin.ui, sMenuWin.back, sMenuWin.front, sMenuWin.plane, sMenuWin.edge,
                  sMenuWin.model, (double)sSlider, (double)sIodPx);
    StereoLog(msg);
    sMenuWin = {};
}

void MenuFrameLatch() {
    const bool on = sMenuDepth != 0 && sActive && &gdx_input_game_mode != nullptr &&
                    IsMenuMode(gdx_input_game_mode());
    sMenuFrame = on;
    sMenuCensusFrame++;
    sMenuCensusDraw = 0;
    if (!on) {
        return;
    }
    sMenuWin.frames++;
    if (!sMenuLogged) {
        sMenuLogged = true;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "[stereo] menu_depth ACTIVE (mode=%d slider=%.2f)", gdx_input_game_mode(),
                      (double)sSlider);
        StereoLog(msg);
    }
    MenuWinEmit();
}

/* The menu layout's constant shift for a draw of class `cls` in the open frame: true + *shift (the
 * right eye's NDC-x shift) when the layout owns the draw. */
bool MenuShift(int cls, float* shift) {
    if (cls == GDX3DS_STEREO_MENU_UI) {
        *shift = sActive ? sMenuUiShift : 0.0f;
        return true;
    }
    if (sMenuFrame && cls == GDX3DS_STEREO_SCENE) {
        *shift = MenuShiftNdc(sMenuModelPx);
        return true;
    }
    return false;
}

/* Size (px, top screen) -> layer disparity (px at full slider / iod 12). */
float MenuLayerPx(float size) {
    if (size <= kMenuFrontSize) {
        return -sMenuFrontPx;
    }
    if (size < kMenuPanelLo) {
        return -sMenuFrontPx * (kMenuPanelLo - size) / (kMenuPanelLo - kMenuFrontSize);
    }
    if (size <= kMenuPanelHi) {
        return 0.0f;
    }
    if (size < kMenuBackSize) {
        return sMenuBackPx * (size - kMenuPanelHi) / (kMenuBackSize - kMenuPanelHi);
    }
    return sMenuBackPx;
}

/* A screen-space menu draw's size: the largest extent of any one triangle (a rectangle is two). The
 * per-triangle measure, not the draw's bounding box or summed area, because the two renderers batch
 * differently (a legacy draw and an NRR batch of the same screen hold different runs of rectangles)
 * while every rectangle is the same in both: a backdrop drawn as 50 strips 304 px wide is a backdrop,
 * a 50-glyph string is 50 glyphs, however they are grouped. */
float MenuDrawSize(const float* verts, uint32_t nVerts, uint32_t stride, bool* edge) {
    float size = 0.0f;
    const uint32_t tris = nVerts / 3u;
    for (uint32_t t = 0; t < tris; t++) {
        const float* a = verts + (size_t)(t * 3u) * stride;
        const float* b = a + stride;
        const float* c = b + stride;
        const float x0 = a[0] < b[0] ? (a[0] < c[0] ? a[0] : c[0]) : (b[0] < c[0] ? b[0] : c[0]);
        const float x1 = a[0] > b[0] ? (a[0] > c[0] ? a[0] : c[0]) : (b[0] > c[0] ? b[0] : c[0]);
        const float y0 = a[1] < b[1] ? (a[1] < c[1] ? a[1] : c[1]) : (b[1] < c[1] ? b[1] : c[1]);
        const float y1 = a[1] > b[1] ? (a[1] > c[1] ? a[1] : c[1]) : (b[1] > c[1] ? b[1] : c[1]);
        /* NDC -> px: x spans 400 px over 2 NDC, y 240 px over 2 */
        const float w = (x1 - x0) * (kEyeWidth * 0.5f);
        const float h = (y1 - y0) * (kEyeHeight * 0.5f);
        const float m = w > h ? w : h;
        size = m > size ? m : size;
        if (x0 <= -0.995f || x1 >= 0.995f) {
            *edge = true;
        }
    }
    return size;
}

/* One triangle's screen box in NDC: x0, x1, y0, y1. */
void MenuTriBox(const float* a, uint32_t stride, float box[4]) {
    const float* b = a + stride;
    const float* c = b + stride;
    box[0] = a[0] < b[0] ? (a[0] < c[0] ? a[0] : c[0]) : (b[0] < c[0] ? b[0] : c[0]);
    box[1] = a[0] > b[0] ? (a[0] > c[0] ? a[0] : c[0]) : (b[0] > c[0] ? b[0] : c[0]);
    box[2] = a[1] < b[1] ? (a[1] < c[1] ? a[1] : c[1]) : (b[1] < c[1] ? b[1] : c[1]);
    box[3] = a[1] > b[1] ? (a[1] > c[1] ? a[1] : c[1]) : (b[1] > c[1] ? b[1] : c[1]);
}

/* A rectangle's layer class: 1 the backdrop (behind), 0 the plane, -1 the text layer (in front), with
 * the ramps' first half px counted to the plane (a 31 px value box at -0.25 px is a panel next to its
 * 16 px arrows, not text: the paths measure it 31.x or 32). The edge rule puts a rectangle touching
 * the left / right screen edge on the plane. */
int MenuRectClass(const float box[4]) {
    if (box[0] <= -0.995f || box[1] >= 0.995f) {
        return 0;
    }
    const float w = (box[1] - box[0]) * (kEyeWidth * 0.5f);
    const float h = (box[3] - box[2]) * (kEyeHeight * 0.5f);
    const float px = MenuLayerPx(w > h ? w : h);
    return px >= 0.5f ? 1 : (px <= -0.5f ? -1 : 0);
}

/* Do two boxes touch along an edge without overlapping (the pieces of a nine-slice frame)? */
bool MenuBoxesAbut(const float a[4], const float b[4]) {
    constexpr float kEps = 1e-4f;
    const bool xTouch = std::fabs(a[1] - b[0]) <= kEps || std::fabs(b[1] - a[0]) <= kEps;
    const bool yTouch = std::fabs(a[3] - b[2]) <= kEps || std::fabs(b[3] - a[2]) <= kEps;
    const bool xOverlap = a[0] < b[1] - kEps && b[0] < a[1] - kEps;
    const bool yOverlap = a[2] < b[3] - kEps && b[2] < a[3] - kEps;
    return (xTouch && (yOverlap || yTouch)) || (yTouch && xOverlap);
}

/* The end (vertex index) of the run of a screen-space menu draw that starts at vertex `first`.
 * One draw is one layer, and the batchers merge rectangles across texture changes (atlas pages), so a
 * batch can hold rectangles of different layers: the title's copyright line rode in the backdrop's
 * batch through its load-in (top half +4, bottom half -- its own draw -- at the plane: the line looked
 * cut in two), and the NRR menu runner batches a panel with the glyphs drawn on it (SELECT MACHINE's
 * stats: the "1260kg" digits sat on the plane on ni_menu=1, in front on the legacy path, where they are
 * draws of their own). The draw is cut where its rectangles (triangle pairs sharing one box) change
 * layer class (MenuRectClass), with one exception: between the plane and the text layer a rectangle
 * that ABUTS one already in the run stays in it -- the corners and borders of a nine-slice panel are
 * small and touch their neighbours edge to edge, while a glyph sits inside its panel or apart from it,
 * so a frame is never torn. A triangle that is not part of a rectangle (a mesh) never starts a run. */
uint32_t MenuUiRunEndImpl(const float* verts, uint32_t nVerts, uint32_t stride, uint32_t first) {
    constexpr uint32_t kRunBoxes = 64; /* past this the abut test has no history: the class decides */
    static float sBoxes[kRunBoxes][4];
    uint32_t nBoxes = 0;
    const uint32_t tris = nVerts / 3u;
    uint32_t t = first / 3u;
    int runCls = 2; /* unknown until the run's first rectangle */
    while (t < tris) {
        float box[4];
        MenuTriBox(verts + (size_t)(t * 3u) * stride, stride, box);
        if (t + 1u < tris) {
            float next[4];
            MenuTriBox(verts + (size_t)((t + 1u) * 3u) * stride, stride, next);
            if (box[0] == next[0] && box[1] == next[1] && box[2] == next[2] && box[3] == next[3]) {
                const int cls = MenuRectClass(box);
                if (runCls == 2) {
                    runCls = cls;
                } else if (cls != runCls) {
                    if (cls == 1 || runCls == 1) {
                        return t * 3u;
                    }
                    bool abuts = false;
                    for (uint32_t k = 0; k < nBoxes && !abuts; k++) {
                        abuts = MenuBoxesAbut(box, sBoxes[k]);
                    }
                    if (!abuts) {
                        return t * 3u;
                    }
                }
                if (nBoxes < kRunBoxes) {
                    std::memcpy(sBoxes[nBoxes++], box, sizeof(box));
                }
                t += 2u;
                continue;
            }
        }
        t++;
    }
    return tris * 3u;
}

/* [debug] stereo_census on the NRR menu runner's draws (p=N; the backend prints p=D / p=R): class,
 * screen box, largest rectangle size and the disparity R - L its row gives, in top-screen px. */
GDX_STEREO_COLD void MenuCensusNrr(int cls, const float* verts, uint32_t nVerts, uint32_t stride, const float row[4]) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t v = 0; v < nVerts; v++) {
        const float* p = verts + (size_t)v * stride;
        x0 = p[0] < x0 ? p[0] : x0;
        x1 = p[0] > x1 ? p[0] : x1;
        y0 = p[1] < y0 ? p[1] : y0;
        y1 = p[1] > y1 ? p[1] : y1;
    }
    bool edge = false;
    const float size = MenuDrawSize(verts, nVerts, stride, &edge);
    const float px = row[3] * (2.0f * (kEyeWidth * 0.5f));
    const int mode = &gdx_input_game_mode != nullptr ? gdx_input_game_mode() : -1;
    char msg[200];
    std::snprintf(msg, sizeof(msg),
                  "[stc] f=%u m=%d i=%u p=N c=%d xf=0 v=%u d=0.000,0.000 bb=%d,%d,%d,%d s=%d px=%.1f,%.1f",
                  sMenuCensusFrame, mode, sMenuCensusDraw++, cls, nVerts, (int)((x0 + 1.0f) * (kEyeWidth * 0.5f)),
                  (int)((1.0f - y1) * (kEyeHeight * 0.5f)), (int)((x1 + 1.0f) * (kEyeWidth * 0.5f)),
                  (int)((1.0f - y0) * (kEyeHeight * 0.5f)), (int)size, (double)px, (double)px);
    StereoLog(msg);
}

/* A screen-space menu draw's layer, latched into sMenuUiShift for the draw that follows. A draw that
 * touches the left or right screen edge stays at the plane: shifted, its edge would uncover a strip of
 * the screen in one eye (the full-screen fades and fills). */
int MenuUiClassImpl(const float* verts, uint32_t nVerts, uint32_t stride) {
    sMenuWin.ui++;
    bool edge = false;
    const float size = MenuDrawSize(verts, nVerts, stride, &edge);
    float px = MenuLayerPx(size);
    if (edge && px != 0.0f) {
        sMenuWin.edge++;
        px = 0.0f;
    }
    if (px > 0.0f) {
        sMenuWin.back++;
    } else if (px < 0.0f) {
        sMenuWin.front++;
    } else {
        sMenuWin.plane++;
        sMenuUiShift = 0.0f;
        return GDX3DS_STEREO_UI_ZERO_PARALLAX;
    }
    sMenuUiShift = MenuShiftNdc(px);
    return GDX3DS_STEREO_MENU_UI;
}

/* o3ds60 the Home screen's 3D button, [stereo] mp_3d and [stereo] mp_ab. hw a3b4531: this Old 3DS
 * reads its 3D slider at 0.39 on every frame ([stereo] slider min=0.39 max=0.39 all session), so
 * the slider cannot turn 3D off, and 3D decides 60 vs 30 fps there (one eye halves the GPU's work).
 *   - sUserOn: [stereo] enabled as this session runs it. The Home button flips it at once (a
 *     session that booted with 3D off creates the right-eye target on the next frame).
 *   - [stereo] mp_3d (default 0 on an Old 3DS, 1 on a New 3DS): an online race runs 2D (0) unless
 *     the 3D button was pressed in this launch (then the button decides), or as the button says (1).
 *   - [stereo] mp_ab (default 0): the one-launch hardware A/B. Online race 1 of the launch runs 2D,
 *     race 2 runs 3D (whatever mp_3d and the button say), later races follow mp_3d. mp_ab = 2: races
 *     1 and 2 run 3D (with [debug] steady_ab: the D = 1 rules new, then old), later races follow mp_3d.
 * 2D = the slider-at-0 path: no right-eye pass, no right-eye display transfer, the left image on
 * both eyes. Rendering only: the simulation, the lockstep pads and the digest never see it.
 * Receipts: [mpab] race #n start / end. */
int sUserOn = -1;     /* -1 = not latched yet (Init or the first query latches [stereo] enabled) */
bool sUserTouched = false; /* the 3D button was pressed in this launch */
bool sWantInit = false;
int sMp3d = -1;       /* [stereo] mp_3d, latched on first use */
int sMpAb = -1;       /* [stereo] mp_ab, latched on first use */
bool sInMpRace = false;
bool sMpRaceOff = false;
unsigned sMpRaces = 0;
unsigned sMpRaceFrames = 0;
unsigned sMpRaceRight = 0;

int UserOn() {
    if (sUserOn < 0) {
        sUserOn = (gdx3ds_config_get_bool != nullptr) ? (gdx3ds_config_get_bool("stereo", "enabled", 0) ? 1 : 0) : 0;
    }
    return sUserOn;
}

bool IsNew3ds() {
    bool isNew = false;
    if (R_FAILED(APT_CheckNew3DS(&isNew))) {
        isNew = false;
    }
    return isNew;
}

void LatchMpLevers() {
    if (sMp3d >= 0) {
        return;
    }
    const int def = IsNew3ds() ? 1 : 0;
    sMp3d = (gdx3ds_config_get_int != nullptr) ? gdx3ds_config_get_int("stereo", "mp_3d", def) : def;
    if (sMp3d < 0 || sMp3d > 1) {
        sMp3d = def;
    }
    sMpAb = gdx3ds_config_get_int != nullptr ? gdx3ds_config_get_int("stereo", "mp_ab", 0) : 0;
    if (sMpAb < 0 || sMpAb > 2) {
        sMpAb = 0;
    }
}

/* This online race's 3D: true = 2D. `why` names the rule for the receipt. */
bool MpRaceOff(const char** why) {
    LatchMpLevers();
    if (sMpAb == 2 && (sMpRaces == 1u || sMpRaces == 2u)) {
        *why = "mp_ab=2 races 1-2: 3D";
        return false;
    }
    if (sMpAb == 1 && sMpRaces == 1u) {
        *why = "mp_ab race 1: 2D";
        return true;
    }
    if (sMpAb == 1 && sMpRaces == 2u) {
        *why = "mp_ab race 2: 3D";
        return false;
    }
    if (sMp3d == 1) {
        *why = "mp_3d=1: the 3D setting";
        return !UserOn();
    }
    if (sUserTouched) {
        *why = "mp_3d=0: the 3D button pressed this launch";
        return !UserOn();
    }
    *why = "mp_3d=0: 2D in online races";
    return true;
}

void MpRaceStart() {
    const char* why = "";
    sInMpRace = true;
    sMpRaces++;
    sMpRaceFrames = 0;
    sMpRaceRight = 0;
    sMpRaceOff = MpRaceOff(&why);
    if (!sMpRaceOff && !sEnabled) {
        sWantInit = true; /* a 2D-booted session: the backend's next StartFrame creates the right eye */
    }
    char msg[200];
    std::snprintf(msg, sizeof(msg), "[mpab] race %u stereo=%s (%s) mp_3d=%d mp_ab=%d button=%s target=%s", sMpRaces,
                  sMpRaceOff ? "off" : "on", why, sMp3d, sMpAb, UserOn() ? "on" : "off",
                  sEnabled ? "bound" : (sMpRaceOff ? "none" : "next frame"));
    StereoLog(msg);
}

void MpRaceEnd() {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "[mpab] race %u end stereo=%s frames=%u right_eye=%u", sMpRaces,
                  sMpRaceOff ? "off" : "on", sMpRaceFrames, sMpRaceRight);
    StereoLog(msg);
    sInMpRace = false;
    sMpRaceOff = false;
}

/* Called every frame, stereo on or off: the online race edges (the receipts and the A/B). True
 * when this frame renders 2D (an online race that runs 2D, or the 3D button off). */
bool FrameWantsMono() {
    const int k = (&gdx3ds_mpo_local_player != nullptr) ? gdx3ds_mpo_local_player() : -1;
    if (k >= 0 && !sInMpRace) {
        MpRaceStart();
    } else if (k < 0 && sInMpRace) {
        MpRaceEnd();
    }
    if (sInMpRace) {
        sMpRaceFrames++;
        return sMpRaceOff;
    }
    return !UserOn();
}

} // namespace

extern "C" void gdx3ds_stereo_set_draw_class(int stereoClass) {
    sDrawClassOverride = (stereoClass >= GDX3DS_STEREO_SCENE && stereoClass <= GDX3DS_STEREO_ANCHORED)
                             ? stereoClass
                             : -1;
}

extern "C" void gdx3ds_stereo_request_enable(void) {
    sForceEnable = true;
}

extern "C" void gdx3ds_stereo_mark_hold_frame(void) {
    sHoldFrame = true;
}

/* ---- MENU live tuning (3D tab) --------------------------------------------------
 * Main/render thread only (the menu tick shares the render thread). The setters are
 * meaningful only while stereo was enabled at Init; harmless otherwise. */
/* hwfix-combined: [stereo] menu_depth on (latched at Init, so only with [stereo] enabled): the machine
 * settings' flat backdrop fills the whole frame (decomp object.c, OBJECT_MACHINE_SETTINGS_BACKGROUND)
 * instead of the stock 12..307 x 8..231 card, which the layout would push behind the screen with its
 * free edges at a different column in each eye. Game fiber; reads a value latched once at Init.
 * mppolish: the fill no longer needs menu_depth. Without it the stock card is a grey 296x224 box in
 * a black frame on every 3DS (hw: the Old 3DS, whose ini has no menu_depth, "still had the
 * non-filled grey background"); [ui] settings_backdrop_full (default 1) fills the frame either way,
 * 0 = the stock card unless menu_depth is on (d9deb37). Read once, on the first settings frame. */
extern "C" int gdx3ds_stereo_menu_backdrop_full(void) {
    static int sBackdropFull = -1;
    if (sMenuDepth != 0) {
        return 1;
    }
    if (sBackdropFull < 0) {
        sBackdropFull = (gdx3ds_config_get_bool != nullptr) ? (gdx3ds_config_get_bool("ui", "settings_backdrop_full", 1) ? 1 : 0)
                                                             : 1;
    }
    return sBackdropFull;
}

/* o3ds60 the Home screen's 3D button (see sUserOn). Main thread; the frame reads it next frame. */
extern "C" int gdx3ds_stereo_user_on(void) {
    return UserOn();
}

extern "C" void gdx3ds_stereo_set_user_on(int on) {
    sUserOn = on ? 1 : 0;
    sUserTouched = true; /* [stereo] mp_3d = 0: the button now decides the online races too */
    if (gdx3ds_config_set_int != nullptr) {
        gdx3ds_config_set_int("stereo", "enabled", sUserOn);
    }
    if (sUserOn && !sEnabled) {
        sWantInit = true; /* the backend's next StartFrame creates the right-eye target */
    }
    char msg[96];
    std::snprintf(msg, sizeof(msg), "[stereo] button: 3D %s (stereo.enabled=%d, target %s)", sUserOn ? "ON" : "OFF",
                  sUserOn, sEnabled ? "bound" : (sUserOn ? "next frame" : "none"));
    StereoLog(msg);
}

extern "C" int gdx3ds_stereo_runtime_enabled(void) {
    return sEnabled ? 1 : 0;
}

/* skystereo: the largest sideways shift (NDC-x, one eye) the eye skew gives a vertex this frame:
 * sep at the far plane, k*dc at the near plane. 0 while the right-eye pass is off, so callers that
 * widen coverage by it (Background_Update's skybox / venue floor / clouds) leave 2D untouched and
 * cost nothing with the slider down. Read by the game thread: a plain latched value, one frame
 * behind at worst while the slider moves. */
extern "C" float gdx3ds_stereo_edge_shift_ndc(void) {
    if (!sActive) {
        return 0.0f;
    }
    const float sep = sSepNdc * sSlider;
    const float nearShift = sep * sConvergence / (1.0f - sConvergence);
    return nearShift > sep ? nearShift : sep;
}

extern "C" int gdx3ds_stereo_get_iod_px(void) {
    return (int)(sIodPx + 0.5f);
}

extern "C" void gdx3ds_stereo_set_iod_px(int px) {
    sIodPx = ClampF((float)px, 0.0f, 40.0f);
    sSepNdc = sIodPx / (kEyeWidth * 0.5f);
}

extern "C" int gdx3ds_stereo_get_conv_x100(void) {
    return (int)(sConvergence * 100.0f + 0.5f);
}

extern "C" void gdx3ds_stereo_set_conv_x100(int centi) {
    sConvergence = ClampF((float)centi / 100.0f, 0.0f, 0.90f);
}

namespace Gdx3dsStereo {

bool Init() {
    int enabled = sForceEnable ? 1 : 0;
    if (!enabled && gdx3ds_config_get_bool != nullptr) {
        enabled = gdx3ds_config_get_bool("stereo", "enabled", 0);
    }
    if (sUserOn < 0) {
        sUserOn = enabled ? 1 : 0; /* o3ds60: the Home 3D button starts from the ini */
    }
    if (sWantInit) {
        enabled = 1; /* o3ds60: the 3D button or [stereo] mp_ab race 2 in a 2D-booted session */
    }
    sWantInit = false;
    if (!enabled) {
        sEnabled = false;
        return false; // zero further cost: no target, no gfxSet3D, no slider poll
    }

    sRightTarget = C3D_RenderTargetCreate(kEyeHeight, kEyeWidth, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (sRightTarget == nullptr) {
        StereoLog("[stereo] right-eye target allocation FAILED - staying mono");
        sEnabled = false;
        return false;
    }
    /* Same transfer flags as the main target (gfx_citro3d.cpp Init). */
    const uint32_t transferFlags =
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(sRightTarget, GFX_TOP, GFX_RIGHT, transferFlags);
    gfxSet3D(true); // mutually exclusive with 800px wide mode (libctru contract)

    /* Tuning: [stereo] iod = far-plane parallax in top-screen pixels at full
     * slider (0..40, clamped for comfort); [stereo] convergence = NDC depth of
     * the zero-parallax plane (clamped away from 1.0 — the shift scales with
     * 1/(1-convergence)). */
    const float iodPx = ClampF(ConfigGetFloat("stereo", "iod", kDefaultIodPixels), 0.0f, 40.0f);
    sConvergence = ClampF(ConfigGetFloat("stereo", "convergence", kDefaultConvergence), 0.0f, 0.90f);
    sIodPx = iodPx;
    sSepNdc = iodPx / (kEyeWidth * 0.5f); // px -> NDC-x: 400px spans [-1,1]

    sEnabled = true;
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "[stereo] enabled: GFX_RIGHT target bound, iod=%.1fpx convergence=%.2f",
                  (double)iodPx, (double)sConvergence);
    StereoLog(msg);
    MenuDepthInit();
    return true;
}

bool Enabled() {
    return sEnabled;
}

/* o3ds60 `[stereo] slider` every 1024 frames: the 3D slider as the system reports it (min/max) and
 * the frames with the right-eye pass on. hw: every Old 3DS log since the first test reads 0.39 on
 * every frame (the right eye always on), the New 3DS logs move between 0 and 1. A slider that
 * never reaches 0 here cannot turn 3D off: set [stereo] enabled = 0 for a 2D session. */
float sSliderMin = 2.0f;
float sSliderMax = -1.0f;
unsigned sSliderFrames = 0;
unsigned sSliderOn = 0;

void SliderWindowNote(float slider) {
    if (slider < sSliderMin) {
        sSliderMin = slider;
    }
    if (slider > sSliderMax) {
        sSliderMax = slider;
    }
    sSliderFrames++;
    if (slider > 0.0f) {
        sSliderOn++;
    }
    if (sSliderFrames >= 1024u) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "[stereo] slider frames=%u min=%.2f max=%.2f right_eye=%u", sSliderFrames,
                      (double)sSliderMin, (double)sSliderMax, sSliderOn);
        StereoLog(msg);
        sSliderMin = 2.0f;
        sSliderMax = -1.0f;
        sSliderFrames = 0;
        sSliderOn = 0;
    }
}

/* preset-fix test hook: [debug] stereo_button_at_frame = N (default 0 = off) presses the Home 3D
 * button ON at the Nth frame of a session that booted 2D, exactly as a tap does
 * (gdx3ds_stereo_set_user_on), so an emulator run can prove a 2D-booted session's late 3D gets the
 * frame replay ([stereo] replay init ... at=late-enable, then [stereo] replay ... replayed>0). */
int sButtonAtFrame = -1;
unsigned sDisabledFrames = 0;

void TestButtonTick() {
    if (sButtonAtFrame < 0) {
        sButtonAtFrame = (gdx3ds_config_get_int != nullptr) ? gdx3ds_config_get_int("debug", "stereo_button_at_frame", 0)
                                                            : 0;
    }
    if (sButtonAtFrame <= 0) {
        return;
    }
    sDisabledFrames++;
    if (sDisabledFrames == (unsigned)sButtonAtFrame) {
        StereoLog("[stereo] test: debug.stereo_button_at_frame reached, pressing 3D ON");
        gdx3ds_stereo_set_user_on(1);
    }
}

bool WantInit() {
    if (!sEnabled) {
        (void) FrameWantsMono(); /* the race receipts (and the A/B's race-2 init) also run in a 2D session */
        TestButtonTick();
    }
    return sWantInit && !sEnabled;
}

bool FrameBegin(C3D_RenderTarget* mainTarget) {
    if (!sEnabled) {
        sActive = false;
        return false;
    }
    sSlider = osGet3DSliderState();
    SliderWindowNote(sSlider);
    const bool mono = FrameWantsMono();
    const bool active = sSlider > 0.0f && sRightTarget != nullptr && !mono;
    if (active && sInMpRace) {
        sMpRaceRight++;
    }
    if (active && !sActive) {
        if (!sActivationLogged) {
            sActivationLogged = true;
            char msg[96];
            std::snprintf(msg, sizeof(msg), "[stereo] right-eye pass ACTIVE (slider=%.2f)", (double)sSlider);
            StereoLog(msg);
        }
    }
    sActive = active;
    MenuFrameLatch();
    if (sActive) {
        /* Queue the right target into this frame so C3D_FrameEnd runs its
         * display transfer, then re-bind the main (left) target. Both binds go
         * through stock C3D_FrameDrawOn: at frame start the viewport reset it
         * performs matches StartFrame's existing state exactly. */
        C3D_FrameDrawOn(sRightTarget);
        C3D_FrameDrawOn(mainTarget);
    }
    return sActive;
}

bool Active() {
    return sActive;
}

C3D_RenderTarget* RightTarget() {
    return sRightTarget;
}

bool RightQueued() {
    return sRightTarget != nullptr && sRightTarget->used;
}

void BindTargetRaw(C3D_RenderTarget* target) {
    /* sm64-3ds precedent: C3D_SetFrameBuf alone — no viewport/scissor reset, so
     * the interpreter's current rects stay bound for the second eye. */
    C3D_SetFrameBuf(&target->frameBuf);
}

void ClearRight(C3D_ClearBits flags, uint32_t color, uint32_t depth) {
    if (!sActive || sRightTarget == nullptr) {
        return;
    }
    if (ReplayActive()) {
        return; /* n60stereo: the body's clear is replayed onto the right target in its place */
    }
    if (sHoldFrame) {
        /* B11q: a taskless hold present. The left eye is about to be restored from the
         * frame mirror by a framebuffer copy that has no right-eye counterpart, so
         * clearing the right target here is what blanked it. Keep what it holds. */
        if (sHoldKeep < 0) {
            sHoldKeep = 1;
            if (gdx3ds_config_get_bool != nullptr) {
                sHoldKeep = gdx3ds_config_get_bool("debug", "stereo_hold_keep", 1) ? 1 : 0;
            }
        }
        if (!sHoldLogged) {
            sHoldLogged = true;
            char msg[112];
            std::snprintf(msg, sizeof(msg),
                          "[stereo] taskless hold present: right-eye clear %s (stereo_hold_keep=%d)",
                          sHoldKeep ? "SUPPRESSED" : "kept (pre-fix behaviour)", sHoldKeep);
            StereoLog(msg);
        }
        if (sHoldKeep) {
            sHoldFrames++;
            return;
        }
    }
    C3D_RenderTargetClear(sRightTarget, flags, color, depth);
}

unsigned long EndFrameHold() {
    sHoldFrame = false;
    return sHoldFrames;
}

void ComputeEyeMatrix(const C3D_Mtx* base, int eye, C3D_Mtx* out, int stereoClass) {
    *out = *base;
    if (eye == 0 || !sActive || stereoClass == GDX3DS_STEREO_UI_ZERO_PARALLAX) {
        return; // center matrix: draw lands at screen depth in both eyes
    }
    /* ------------------- Off-axis stereo, clip-space form -------------------
     * DERIVATION. Vertices arrive in the game's clip space (x,y,z,w), with
     * z ∈ [0,w] (interpreter contract: z_is_from_0_to_1), so NDC depth
     * d = z/w ∈ [0,1], 0 = near plane, 1 = far plane. For any perspective
     * projection, d is an AFFINE function of 1/z_view (d = A + B/z_view; the
     * constants come from the game's near/far, which we never need to know).
     *
     * Physically correct stereo = translate the camera by ±s/2 along view-x
     * and skew the frustum so both view axes cross at the convergence depth C
     * (the screen plane). The image shift that pair produces, in NDC-x:
     *     shift(z_view) = ±(f·s/2)·(1/C − 1/z_view)
     * — affine in 1/z_view, zero at z_view = C, saturating toward the eye
     * separation term at infinity. Because d is ALSO affine in 1/z_view, that
     * family is exactly the set of shifts affine in d:
     *     shift(d) = ±sep · (d − dc) / (1 − dc)
     * where dc = d(C) is the convergence plane in NDC depth and sep the
     * parallax at the far plane (d = 1). No approximation: this clip-space
     * skew IS an asymmetric-frustum stereo projection — the same family
     * citro3d's Mtx_PerspStereoTilt generates — applied post-hoc because the
     * game owns the projection matrix (vertices are CPU-pre-transformed).
     *
     * Multiply by w to stay in clip space:  x' = x ± k·(z − dc·w), with
     * k = sep·slider/(1 − dc). As a matrix S on the incoming clip vector,
     * S = I except S[x][z] = ±k, S[x][w] = ∓k·dc; we fold base·S directly:
     * every row picks up (±k, ∓k·dc) times its x-coefficient. (For the fixup
     * base only row 1 — the portrait-rotated horizontal axis, out.y = −x —
     * has a nonzero x-coefficient, the same row Mtx_PerspStereoTilt shears.)
     *
     * Sign: the game's clip +x is screen-right (the mono image is not
     * mirrored, so the fixup→viewport→scanout chain is net-identity on x).
     * For the RIGHT eye (+1), points beyond convergence (d > dc) shift
     * screen-right → uncrossed (positive) parallax, perceived behind the
     * screen; nearer points shift left and pop out. z and w are untouched, so
     * depth testing, the PICA fog unit (indexed on z/w), and the TexEnv fog
     * blend are bit-identical per eye by construction.
     * ------------------------------------------------------------------------ */
    const float sep = sSepNdc * sSlider * (eye < 0 ? -1.0f : 1.0f);
    float menuShift = 0.0f;
    if (MenuShift(stereoClass, &menuShift)) {
        /* mstereo [stereo] menu_depth: a constant per-draw shift (a layer), as ANCHORED's. */
        const float shift = eye < 0 ? -menuShift : menuShift;
        for (int i = 0; i < 4; i++) {
            out->r[i].w += shift * base->r[i].x;
        }
        return;
    }
    if (stereoClass == GDX3DS_STEREO_SKY_DEEP) {
        /* Backgrounds/skybox class: pin to far-plane parallax regardless of
         * the (often near/ortho) depth they are drawn at, so they sit behind
         * all scene geometry: x' = x + sep·w (constant sep in NDC). */
        for (int i = 0; i < 4; i++) {
            out->r[i].w += sep * base->r[i].x;
        }
        return;
    }
    const float k = sep / (1.0f - sConvergence);
    if (stereoClass == GDX3DS_STEREO_ANCHORED) {
        /* [anchor] Ortho draw pinned to a world depth d (its prim depth = the machine it
         * labels): the SAME shift a scene vertex at NDC depth d receives, as a constant per
         * draw since ortho z carries no depth: x' = x + k·(d − dc)·w. */
        const float shift = k * (sAnchorDepth - sConvergence);
        for (int i = 0; i < 4; i++) {
            out->r[i].w += shift * base->r[i].x;
        }
        return;
    }
    for (int i = 0; i < 4; i++) {
        const float bx = base->r[i].x;
        out->r[i].z += k * bx;
        out->r[i].w -= k * sConvergence * bx;
    }
}

void SetAnchorDepth(float d) {
    sAnchorDepth = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
}

bool HoldFrame() {
    return sHoldFrame;
}

bool MenuFrame() {
    return sMenuFrame;
}

void MenuNoteModel() {
    sMenuWin.model++;
}

GDX_STEREO_COLD uint32_t MenuUiRunEnd(const float* verts, uint32_t nVerts, uint32_t stride, uint32_t first) {
    if (!sMenuFrame || verts == nullptr || nVerts < 6u) {
        return nVerts;
    }
    return MenuUiRunEndImpl(verts, nVerts, stride, first);
}

GDX_STEREO_COLD int MenuUiClass(const float* verts, uint32_t nVerts, uint32_t stride) {
    if (!sMenuFrame || verts == nullptr || nVerts < 3u) {
        return GDX3DS_STEREO_UI_ZERO_PARALLAX;
    }
    return MenuUiClassImpl(verts, nVerts, stride);
}

/* n60stereo: the frame replay's per-draw row. The SAME coefficients ComputeEyeMatrix folds into
 * the matrix for the right eye (eye = +1); the shader applies the sign (steye), so the row is
 * eye-independent: x' = x + steye * dot(row, clip). */
void ClassRow(int stereoClass, float row[4]) {
    row[0] = 0.0f;
    row[1] = 0.0f;
    row[2] = 0.0f;
    row[3] = 0.0f;
    if (!sActive) {
        return;
    }
    float menuShift = 0.0f;
    if (MenuShift(stereoClass, &menuShift)) {
        row[3] = menuShift; /* mstereo [stereo] menu_depth: the layer's constant shift */
        return;
    }
    const float sep = sSepNdc * sSlider;
    const float k = sep / (1.0f - sConvergence);
    if (stereoClass == GDX3DS_STEREO_SCENE) {
        row[2] = k;
        row[3] = -k * sConvergence;
    } else if (stereoClass == GDX3DS_STEREO_SKY_DEEP) {
        row[3] = sep;
    } else if (stereoClass == GDX3DS_STEREO_ANCHORED) {
        row[3] = k * (sAnchorDepth - sConvergence);
    }
}

int ClassifyDraw(bool orthoDraw) {
    if (sDrawClassOverride >= 0) {
        return sDrawClassOverride;
    }
    /* Heuristic default (foundation only; real per-drawcall tagging is bridge
     * work): orthographic draws (clip w == 1) are HUD/menu content and belong
     * at screen depth. */
    return orthoDraw ? GDX3DS_STEREO_UI_ZERO_PARALLAX : GDX3DS_STEREO_SCENE;
}

} // namespace Gdx3dsStereo

extern "C" int gdx3ds_stereo_menu_frame(void) {
    return sMenuFrame ? 1 : 0;
}

extern "C" GDX_STEREO_COLD void gdx3ds_stereo_menu_ui_row(const float* verts, uint32_t nVerts, uint32_t stride,
                                                          float row[4]) {
    const int cls = Gdx3dsStereo::MenuUiClass(verts, nVerts, stride);
    Gdx3dsStereo::ClassRow(cls, row);
    if (sMenuCensusPeriod != 0u && sMenuFrame && (sMenuCensusFrame % sMenuCensusPeriod) == 0u) {
        MenuCensusNrr(cls, verts, nVerts, stride, row);
    }
}

extern "C" GDX_STEREO_COLD uint32_t gdx3ds_stereo_menu_run_end(const float* verts, uint32_t nVerts, uint32_t stride,
                                                               uint32_t first) {
    return Gdx3dsStereo::MenuUiRunEnd(verts, nVerts, stride, first);
}

/* ---------------------------------------------------------------------------------
 * Right-eye verification readback (harness evidence path). Mirrors the main
 * target branch of GfxRenderingAPIC3D::ReadFramebufferToCPU — same GX display
 * transfer (RGBA8 -> linear RGB5A1) and the same harness-verified un-rotation
 * (game-y along fb-x from 0, game-x along fb-y from 0).
 * --------------------------------------------------------------------------------- */
extern "C" int gdx3ds_stereo_read_right(uint16_t* rgba16Buf, uint32_t width, uint32_t height) {
    if (rgba16Buf == nullptr || width == 0 || height == 0) {
        return 1;
    }
    std::memset(rgba16Buf, 0, (size_t)width * height * sizeof(uint16_t));
    if (!sEnabled || sRightTarget == nullptr) {
        return 2;
    }
    u32* colorBuf = (u32*)sRightTarget->frameBuf.colorBuf;
    if (colorBuf == nullptr) {
        return 3;
    }
    const u32 fbW = (u32)kEyeHeight; // portrait: 240 wide ...
    const u32 fbH = (u32)kEyeWidth;  // ... x 400 tall
    const size_t linearBytes = (size_t)fbW * fbH * sizeof(u16);
    u16* linear = (u16*)linearAlloc(linearBytes);
    if (linear == nullptr) {
        return 4;
    }
    /* Same completion sentinel as ReadFramebufferToCPU: C3D_SyncDisplayTransfer does NOT wait
       while a C3D frame is active (see the TRANSITION-GLITCH note in gfx_citro3d.cpp), so poll
       the transfer's final words before consuming the buffer. */
    volatile u32* sentinel = (volatile u32*)((u8*)linear + linearBytes - 8);
    sentinel[0] = 0xDEADC0DEu;
    sentinel[1] = 0x5EA7BEEFu ^ 0x0F0F0F0Fu;
    GSPGPU_FlushDataCache((void*)((u8*)linear + linearBytes - 8), 8);
    const u32 transferFlags =
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB5A1) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_SyncDisplayTransfer(colorBuf, GX_BUFFER_DIM(fbW, fbH), (u32*)linear, GX_BUFFER_DIM(fbW, fbH),
                            transferFlags);
    for (int spin = 0; spin < 4000; ++spin) { // 400 ms bound
        GSPGPU_InvalidateDataCache((void*)((u8*)linear + linearBytes - 8), 8);
        if (sentinel[0] != 0xDEADC0DEu || sentinel[1] != (0x5EA7BEEFu ^ 0x0F0F0F0Fu)) {
            break;
        }
        svcSleepThread(100 * 1000);
    }
    GSPGPU_InvalidateDataCache(linear, linearBytes);

    for (uint32_t yOut = 0; yOut < height; yOut++) {
        const u32 gy = (u32)(((uint64_t)(height - 1 - yOut) * kEyeHeight) / height);
        for (uint32_t xOut = 0; xOut < width; xOut++) {
            const u32 gx = (u32)(((uint64_t)xOut * kEyeWidth) / width);
            const u32 fbX = gy;
            const u32 fbY = gx;
            if (fbX >= fbW || fbY >= fbH) {
                continue;
            }
            const u16 px = linear[(size_t)fbY * fbW + fbX];
            rgba16Buf[(size_t)yOut * width + xOut] = (u16)(px | 0x0001u);
        }
    }
    linearFree(linear);
    return 0;
}
