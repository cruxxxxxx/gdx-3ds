/* port/3ds/harness/dl_tests_main.cpp — gdx3ds-dl-tests.3dsx entry point.
 *
 * Drives libultraship's Fast3D interpreter over the hand-built F3DEX2 scenes in
 * scenes.cpp, rendering through stream A's citro3d backend
 * (Gdx3ds_GetCitro3dRenderer()). This is the standalone flavour of the plan
 * §2.6 DL replay rig: it needs neither the game nor streams B/C/D at runtime.
 *
 * Interpreter driving mirrors port/n64_gfx_bridge.cpp's pattern minus
 * Fast3dWindow (which would drag in Ship::Window/Gui): we own the Interpreter
 * instance directly and call StartFrame()/Run()/EndFrame() per frame. Two
 * pieces of glue make that legal outside Fast3dWindow:
 *   - Fast::GfxSetInstance() (free function defined in interpreter.cpp, not
 *     declared in any header — declared locally below) must point at our
 *     instance, since every gfx_*_handler resolves the interpreter through it;
 *   - a GfxDebugger must be attached before Run(), whose command loop
 *     dereferences it unconditionally.
 *
 * The interpreter is initialised at the N64-native 320x240 and
 * mGameWindowViewport is set to match, so:
 *   - ViewportMatchesRendererResolution() is true -> Run() draws directly to
 *     framebuffer 0, the top screen, no offscreen game FB, and
 *   - viewport/scissor rectangles map 1:1 (no widescreen hor+ x-scaling).
 * The citro3d backend's screen target is the full 400x240 top screen, so the
 * 320-wide output leaves an unrendered ~80px band at one edge — see
 * EXPECTED.md ("dead band") for how to read it.
 *
 * Controls: START = next scene, SELECT = quit. Scene label + expected result
 * print on the bottom-screen console (stream B's consoleInit pattern).
 */

#include <3ds.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "fast/interpreter.h"
#include "fast/backends/gfx_window_manager_api.h"
#include "gdx3ds_gfx.h"
#include "scenes.h"

namespace Fast {
/* Defined in libultraship/src/fast/interpreter.cpp; no public header declares it. */
void GfxSetInstance(std::shared_ptr<Interpreter> gfx);
} // namespace Fast

/* Stream S stereo hooks (port/3ds/gfx/gdx3ds_stereo.h; declared locally so the
 * harness include set stays unchanged). The harness links no config library, so
 * stereo is opted into by a marker file: create sdmc:/gdx-harness/stereo.on
 * (host-side: gdx-harness/stereo.on in Azahar's virtual SD) before launch. */
extern "C" void gdx3ds_stereo_request_enable(void);
extern "C" int gdx3ds_stereo_read_right(uint16_t* rgba16Buf, uint32_t width, uint32_t height);

namespace {

constexpr uint32_t kWinW = 320;
constexpr uint32_t kWinH = 240;

/* Minimal GfxWindowBackend: the 3DS has no window system; the citro3d backend
 * owns the real screen. Only GetDimensions (feeds StartFrame) and the frame
 * pacing no-ops matter — everything else is dead surface on this path. */
class NullWindowBackend final : public Fast::GfxWindowBackend {
  public:
    void Init(const char*, const char*, bool, uint32_t, uint32_t, int32_t, int32_t) override {
    }
    void Close() override {
    }
    void SetKeyboardCallbacks(bool (*)(int), bool (*)(int), void (*)()) override {
    }
    void SetMouseCallbacks(bool (*)(int), bool (*)(int)) override {
    }
    void SetFullscreenChangedCallback(void (*)(bool)) override {
    }
    void SetFullscreen(bool) override {
    }
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override {
        *refreshRate = 60;
    }
    void SetCursorVisibility(bool) override {
    }
    void SetMousePos(int32_t, int32_t) override {
    }
    void GetMousePos(int32_t* x, int32_t* y) override {
        *x = 0;
        *y = 0;
    }
    void GetMouseDelta(int32_t* x, int32_t* y) override {
        *x = 0;
        *y = 0;
    }
    void GetMouseWheel(float* x, float* y) override {
        *x = 0.0f;
        *y = 0.0f;
    }
    bool GetMouseState(uint32_t) override {
        return false;
    }
    void SetMouseCapture(bool) override {
    }
    bool IsMouseCaptured() override {
        return false;
    }
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override {
        *width = kWinW;
        *height = kWinH;
        *posX = 0;
        *posY = 0;
    }
    void SetDimensions(uint32_t, uint32_t, int32_t, int32_t) override {
    }
    Ship::WindowRect GetPrimaryMonitorRect() override {
        return { 0, 0, (int32_t)kWinW, (int32_t)kWinH };
    }
    void HandleEvents() override {
    }
    bool IsFrameReady() override {
        return true;
    }
    void SwapBuffersBegin() override {
    }
    void SwapBuffersEnd() override {
    }
    double GetTime() override {
        return 0.0;
    }
    int GetTargetFps() override {
        return 60;
    }
    void SetTargetFps(int) override {
    }
    void SetMaxFrameLatency(int) override {
    }
    const char* GetKeyName(int) override {
        return "";
    }
    bool CanDisableVsync() override {
        return false;
    }
    bool IsRunning() override {
        return true;
    }
    void Destroy() override {
    }
    bool IsFullscreen() override {
        return true;
    }
};

void PrintScene(int scene) {
    const SceneInfo* info = SceneGetInfo(scene);
    std::printf("\x1b[2J\x1b[H"); /* clear console */
    std::printf("gdx3ds-dl-tests  scene %d/%d\n", scene + 1, kSceneCount);
    std::printf("================================\n");
    std::printf("%s\nchecks: %s\n\nEXPECTED (top screen):\n%s\n", info->name, info->checks,
                info->expected);
    std::printf("\nSTART = next scene, SELECT = quit\n");
    char banner[96];
    int n = std::snprintf(banner, sizeof(banner), "[harness] scene %d/%d %s", scene + 1, kSceneCount,
                          info->name);
    if (n > 0) {
        svcOutputDebugString(banner, n);
    }
}

/* Dump the backend's RGBA5551 CPU readback of framebuffer 0 as a 24-bit BMP at
 * sdmc:/gdx-harness/sceneNN.bmp — the headless-run verification artifact (the
 * Azahar virtual SD is a plain host directory). Row 0 of the readback is the
 * scene TOP; BMP rows are bottom-up, so rows are written in reverse. */
void WriteBmp(const char* path, const uint16_t* pixels) {
    constexpr uint32_t kW = 320, kH = 240;
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        svcOutputDebugString("[harness] BMP fopen failed", 25);
        return;
    }
    const uint32_t rowBytes = kW * 3; /* 960, already 4-aligned */
    const uint32_t dataBytes = rowBytes * kH;
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    const uint32_t fileSize = 54 + dataBytes;
    std::memcpy(&hdr[2], &fileSize, 4);
    const uint32_t dataOff = 54;
    std::memcpy(&hdr[10], &dataOff, 4);
    const uint32_t dibSize = 40;
    std::memcpy(&hdr[14], &dibSize, 4);
    const int32_t w = (int32_t)kW, h = (int32_t)kH;
    std::memcpy(&hdr[18], &w, 4);
    std::memcpy(&hdr[22], &h, 4);
    const uint16_t planes = 1, bpp = 24;
    std::memcpy(&hdr[26], &planes, 2);
    std::memcpy(&hdr[28], &bpp, 2);
    std::memcpy(&hdr[34], &dataBytes, 4);
    std::fwrite(hdr, 1, sizeof(hdr), f);
    static uint8_t sRow[kW * 3];
    for (int y = (int)kH - 1; y >= 0; y--) {
        const uint16_t* src = pixels + (size_t)y * kW;
        for (uint32_t x = 0; x < kW; x++) {
            const uint16_t px = src[x]; /* RGBA5551: R[15:11] G[10:6] B[5:1] A[0] */
            const uint8_t r = (uint8_t)(((px >> 11) & 0x1F) << 3);
            const uint8_t g = (uint8_t)(((px >> 6) & 0x1F) << 3);
            const uint8_t b = (uint8_t)(((px >> 1) & 0x1F) << 3);
            sRow[x * 3 + 0] = b; /* BMP is BGR */
            sRow[x * 3 + 1] = g;
            sRow[x * 3 + 2] = r;
        }
        std::fwrite(sRow, 1, rowBytes, f);
    }
    std::fclose(f);
    char msg[64];
    int n = std::snprintf(msg, sizeof(msg), "[harness] dumped %s", path);
    if (n > 0) {
        svcOutputDebugString(msg, n);
    }
}

void DumpSceneBmp(int scene, Fast::Interpreter& interp) {
    constexpr uint32_t kW = 320, kH = 240;
    static uint16_t sPixels[kW * kH];
    interp.GetCurrentRenderingAPI()->ReadFramebufferToCPU(0, kW, kH, sPixels);

    mkdir("sdmc:/gdx-harness", 0777);
    char path[64];
    std::snprintf(path, sizeof(path), "sdmc:/gdx-harness/scene%02d.bmp", scene);
    WriteBmp(path, sPixels);

    /* Stereo runs (stereo.on marker): also dump the right eye's color buffer as
     * sceneNN_r.bmp — the per-eye-rendering evidence for the stereo foundation. */
    if (gdx3ds_stereo_read_right(sPixels, kW, kH) == 0) {
        std::snprintf(path, sizeof(path), "sdmc:/gdx-harness/scene%02d_r.bmp", scene);
        WriteBmp(path, sPixels);
    }
}

} // namespace

int main() {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);

    if (kSceneGfxPacketSize != sizeof(Fast::F3DGfx)) {
        std::printf("FATAL: Gfx packet size mismatch\n(scenes %u vs interpreter %u)\n",
                    (unsigned)kSceneGfxPacketSize, (unsigned)sizeof(Fast::F3DGfx));
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_SELECT) {
                break;
            }
            gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }

    if (!ScenesInit()) {
        std::printf("FATAL: scene allocation failed\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_SELECT) {
                break;
            }
            gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }

    /* Stereo opt-in (must precede the backend's Init inside interp->Init). */
    if (FILE* marker = std::fopen("sdmc:/gdx-harness/stereo.on", "rb")) {
        std::fclose(marker);
        gdx3ds_stereo_request_enable();
        std::printf("stereo: ON (marker file)\n");
    }

    static NullWindowBackend sWapi;
    auto interp = std::make_shared<Fast::Interpreter>();
    Fast::GfxSetInstance(interp);
    interp->SetGfxDebugger(std::make_shared<Fast::GfxDebugger>());
    /* Init() calls the backend's Init() (C3D_Init etc.) — gfxInitDefault()
     * above satisfies its documented precondition. */
    interp->Init(&sWapi, Gdx3ds_GetCitro3dRenderer(), "gdx3ds-dl-tests", false, kWinW, kWinH, 0, 0);
    /* Fast3dWindow normally publishes the game viewport; without it the
     * default {0,0,0,0} would fail ViewportMatchesRendererResolution() and
     * bounce every frame through the offscreen game FB. */
    interp->mGameWindowViewport = { 0, 0, kWinW, kWinH };

    /* Headless-friendly driving (input injection into Azahar is unavailable on
     * macOS): scenes auto-advance every ~4 s, each scene dumps a verification
     * BMP once settled, and the app EXITS after the full cycle so a scripted
     * run terminates by itself. Any button press switches to manual mode. */
    int scene = 0;
    unsigned frame = 0;
    unsigned framesInScene = 0;
    bool dumped = false;
    bool autoMode = true;
    constexpr unsigned kDumpFrame = 120;
    constexpr unsigned kAdvanceFrame = 240;
    PrintScene(scene);

    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down & KEY_SELECT) {
            break;
        }
        /* Auto mode stays ON even across manual STARTs: headless Azahar runs have
         * been observed to leak stray window clicks/keystrokes into the guest as
         * START, and a single stray press must not strand the run without its
         * exit or its remaining dumps. START just skips ahead. */
        bool advance = false;
        if (down & KEY_START) {
            advance = true;
        }
        if (autoMode && framesInScene >= kAdvanceFrame) {
            if (scene + 1 >= kSceneCount) {
                svcOutputDebugString("[harness] all scenes done, exiting", 33);
                break;
            }
            advance = true;
        }
        if (advance) {
            scene = (scene + 1) % kSceneCount;
            framesInScene = 0;
            dumped = false;
            PrintScene(scene);
        }

        void* dl = SceneBuildDl(scene, frame);
        interp->StartFrame();
        interp->Run(reinterpret_cast<Gfx*>(dl), {}); /* ::Gfx (libultra/gbi.h), same 8-byte packet */
        interp->EndFrame();
        ++frame;
        ++framesInScene;
        if (!dumped && framesInScene >= kDumpFrame) {
            dumped = true;
            DumpSceneBmp(scene, *interp);
        }
    }

    /* Test app: skip Interpreter::Destroy()/C3D teardown; process exit
     * reclaims everything and the backend keeps no cross-boot state. */
    gfxExit();
    return 0;
}
