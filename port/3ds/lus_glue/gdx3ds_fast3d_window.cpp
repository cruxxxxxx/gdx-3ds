// port/3ds/lus_glue/gdx3ds_fast3d_window.cpp — Fast::Fast3dWindow for the 3DS,
// replacing src/fast/Fast3dWindow.cpp at link.
//
// The class DEFINITION is the real header (fast/Fast3dWindow.h): the port bridge
// static_casts Ship::Window* to Fast::Fast3dWindow* and calls GetInterpreterWeak /
// SetRendererUCode / DrawAndRunGraphicsCommands on it, so the object must genuinely be
// a Fast3dWindow. Only the member DEFINITIONS differ from desktop:
//   - the rendering API is stream A's citro3d backend (Gdx3ds_GetCitro3dRenderer)
//   - the window backend is Gdx3dsWindowBackend below, a thin GfxWindowBackend over
//     the gdx3ds_os contract (swap = cache flush + vblank wait + APT pump; the citro3d
//     backend's C3D_FrameEnd owns the actual top-screen buffer swap)
//   - no Gui, no SDL events, no mouse, no fullscreen, no config persistence
// Every virtual member must be defined (the vtable references all of them); the
// desktop-only ones are explicit no-ops.

#include "fast/Fast3dWindow.h"
#include "fast/backends/gfx_window_manager_api.h"
#include "fast/interpreter.h"
#include "ship/Context.h"
#include "gdx3ds_gfx.h"
#include "gdx3ds_os.h"

#include <cstdio>

namespace Fast {

extern void GfxSetInstance(std::shared_ptr<Interpreter> gfx); // interpreter.cpp (carve)

// gfx_set_target_ucode lives in the interpreter TU on desktop builds.
// SetRendererUCode forwards to the instance the same way the desktop free function does.
static constexpr uint32_t kGdx3dsScreenWidth = 400;
static constexpr uint32_t kGdx3dsScreenHeight = 240;

namespace {

/* GfxWindowBackend over port/3ds/os. The interpreter drives this for frame pacing and
 * dimensions; everything keyboard/mouse/fullscreen is inert on 3DS. */
class Gdx3dsWindowBackend final : public GfxWindowBackend {
  public:
    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override {
        // main_3ds.cpp already ran gdx3ds_os_window_init (stream B's contract order:
        // config load -> window init); nothing to do here.
        (void)gameName; (void)apiName; (void)startFullScreen;
        (void)width; (void)height; (void)posX; (void)posY;
    }
    void Close() override {
        mIsRunning = false;
    }
    void SetKeyboardCallbacks(bool (*onKeyDown)(int), bool (*onKeyUp)(int), void (*onAllKeysUp)()) override {
        (void)onKeyDown; (void)onKeyUp; (void)onAllKeysUp;
    }
    void SetMouseCallbacks(bool (*onMouseButtonDown)(int), bool (*onMouseButtonUp)(int)) override {
        (void)onMouseButtonDown; (void)onMouseButtonUp;
    }
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool)) override {
        (void)onFullscreenChanged;
    }
    void SetFullscreen(bool fullscreen) override {
        (void)fullscreen; // the 3DS top screen is always "fullscreen"
    }
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override {
        *refreshRate = 60; // gsp vblank
    }
    void SetCursorVisibility(bool visibility) override {
        (void)visibility;
    }
    void SetMousePos(int32_t x, int32_t y) override {
        (void)x; (void)y;
    }
    void GetMousePos(int32_t* x, int32_t* y) override {
        *x = 0; *y = 0;
    }
    void GetMouseDelta(int32_t* x, int32_t* y) override {
        *x = 0; *y = 0;
    }
    void GetMouseWheel(float* x, float* y) override {
        *x = 0.0f; *y = 0.0f;
    }
    bool GetMouseState(uint32_t btn) override {
        (void)btn;
        return false;
    }
    void SetMouseCapture(bool capture) override {
        (void)capture;
    }
    bool IsMouseCaptured() override {
        return false;
    }
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override {
        *width = kGdx3dsScreenWidth;
        *height = kGdx3dsScreenHeight;
        *posX = 0;
        *posY = 0;
    }
    void SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) override {
        (void)width; (void)height; (void)posX; (void)posY;
    }
    Ship::WindowRect GetPrimaryMonitorRect() override {
        return { 0, 0, (int32_t)kGdx3dsScreenWidth, (int32_t)kGdx3dsScreenHeight };
    }
    void HandleEvents() override {
        // APT close requests surface through gdx3ds_os_window_swap's return value.
    }
    bool IsFrameReady() override {
        return true; // gsp vblank in SwapBuffersEnd is the pacer
    }
    void SwapBuffersBegin() override {
    }
    void SwapBuffersEnd() override {
        if (gdx3ds_os_window_swap() != 0) {
            mIsRunning = false; // APT close/HOME exit request
        }
    }
    double GetTime() override {
        return (double)gdx3ds_os_time_ns() / 1e9;
    }
    int GetTargetFps() override {
        return (int)mTargetFps;
    }
    void SetTargetFps(int fps) override {
        mTargetFps = (uint32_t)fps;
    }
    void SetMaxFrameLatency(int latency) override {
        (void)latency;
    }
    const char* GetKeyName(int scancode) override {
        (void)scancode;
        return "";
    }
    bool CanDisableVsync() override {
        return false;
    }
    bool IsRunning() override {
        return mIsRunning;
    }
    void Destroy() override {
    }
    bool IsFullscreen() override {
        return true;
    }
};

} // namespace

Fast3dWindow::Fast3dWindow(std::shared_ptr<Ship::Gui> gui, std::shared_ptr<FastMouseStateManager> mouseStateManager)
    : Ship::Window(gui, mouseStateManager) {
    mWindowManagerApi = nullptr;
    mRenderingApi = nullptr;
    mInterpreter = std::make_shared<Interpreter>();
    GfxSetInstance(mInterpreter);
}

Fast3dWindow::Fast3dWindow(std::shared_ptr<Ship::Gui> gui) : Fast3dWindow(gui, nullptr) {
}

Fast3dWindow::Fast3dWindow(std::vector<std::shared_ptr<Ship::GuiWindow>> guiWindows) : Fast3dWindow(nullptr, nullptr) {
    (void)guiWindows;
}

Fast3dWindow::Fast3dWindow() : Fast3dWindow(nullptr, nullptr) {
}

Fast3dWindow::~Fast3dWindow() {
    mInterpreter->Destroy();
    // mRenderingApi is stream A's process-lifetime singleton (Gdx3ds_GetCitro3dRenderer)
    // — deliberately NOT deleted here, unlike desktop Fast3dWindow.cpp.
    delete mWindowManagerApi;
}

void Fast3dWindow::Init() {
    InitWindowManager();
    mGfxDebugger = std::make_shared<GfxDebugger>();
    mInterpreter->SetGfxDebugger(mGfxDebugger);
    mInterpreter->Init(mWindowManagerApi, mRenderingApi, Ship::Context::GetInstance()->GetName().c_str(),
                       /*start_in_fullscreen=*/true, kGdx3dsScreenWidth, kGdx3dsScreenHeight, 0, 0);
    // Desktop sets mGameWindowViewport from Fast3dGui::DrawGame (the ImGui game window);
    // there is no Gui on 3DS, so without this it stays {0,0,0,0}, which makes
    // ViewportMatchesRendererResolution() false forever -> mRendersToFb == true -> every
    // frame renders into the offscreen mGameFb texture that nothing composites to the
    // screen, while framebuffer 0 is cleared black. That was a guaranteed-black top
    // screen even with a fully working draw path. The DL-test harness proved the fix:
    // it sets the viewport to the full screen explicitly (harness/dl_tests_main.cpp).
    mInterpreter->mGameWindowViewport = { 0, 0, (int16_t)kGdx3dsScreenWidth, (int16_t)kGdx3dsScreenHeight };
    SetTextureFilter(FILTER_THREE_POINT); // no CVar editor on 3DS; three-point is the N64 look
}

void Fast3dWindow::InitWindowManager() {
    mWindowManagerApi = new Gdx3dsWindowBackend();
    mRenderingApi = Gdx3ds_GetCitro3dRenderer();
}

int32_t Fast3dWindow::GetTargetFps() {
    return mInterpreter->GetTargetFps();
}

void Fast3dWindow::SetTargetFps(int32_t fps) {
    mInterpreter->SetTargetFps(fps);
}

void Fast3dWindow::SetMaximumFrameLatency(int32_t latency) {
    mInterpreter->SetMaxFrameLatency(latency);
}

void Fast3dWindow::GetPixelDepthPrepare(float x, float y) {
    mInterpreter->GetPixelDepthPrepare(x, y);
}

uint16_t Fast3dWindow::GetPixelDepth(float x, float y) {
    return mInterpreter->GetPixelDepth(x, y);
}

void Fast3dWindow::SetTextureFilter(FilteringMode filteringMode) {
    mInterpreter->GetCurrentRenderingAPI()->SetTextureFilter(filteringMode);
}

void Fast3dWindow::EnableSRGBMode() {
    // No sRGB toggle on PICA.
}

void Fast3dWindow::SetRendererUCode(UcodeHandlers ucode) {
    gfx_set_target_ucode(ucode);
}

void Fast3dWindow::Close() {
    mWindowManagerApi->Close();
}

void Fast3dWindow::RunGuiOnly() {
    // No GUI-only mode on 3DS (no ImGui).
}

void Fast3dWindow::StartFrame() {
    mInterpreter->StartFrame();
}

void Fast3dWindow::EndFrame() {
    mInterpreter->EndFrame();
}

bool Fast3dWindow::IsFrameReady() {
    return mWindowManagerApi->IsFrameReady();
}

/* Same seam as desktop Fast3dWindow.cpp: the port defines it (n64_gfx_bridge.cpp). */
extern "C" void gdx_gfx_post_run_capture(void);

/* Desktop Fast3dWindow.cpp's DXGI-limiter bypass toggle; the bridge's interpolation
 * path calls it. No software limiter exists on 3DS (gsp vblank paces presents), so the
 * flag has nothing to bypass. */
extern "C" void gdx_fast3d_set_subframe_present(int on) {
    (void)on;
}

bool Fast3dWindow::DrawAndRunGraphicsCommands(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtxReplacements) {
    // Desktop brackets this with Gui StartDraw/EndDraw; there is no Gui here.
    mInterpreter->StartFrame();
    mInterpreter->Run(commands, mtxReplacements);
    gdx_gfx_post_run_capture();
    mInterpreter->EndFrame();
    return true;
}

void Fast3dWindow::HandleEvents() {
    mWindowManagerApi->HandleEvents();
}

void Fast3dWindow::SetCursorVisibility(bool visible) {
    (void)visible;
}

uint32_t Fast3dWindow::GetWidth() {
    return kGdx3dsScreenWidth;
}

uint32_t Fast3dWindow::GetHeight() {
    return kGdx3dsScreenHeight;
}

float Fast3dWindow::GetAspectRatio() {
    return mInterpreter->mCurDimensions.aspect_ratio;
}

int32_t Fast3dWindow::GetPosX() {
    return 0;
}

int32_t Fast3dWindow::GetPosY() {
    return 0;
}

void Fast3dWindow::SetMousePos(Ship::Coords pos) {
    (void)pos;
}

Ship::Coords Fast3dWindow::GetMousePos() {
    return { 0, 0 };
}

Ship::Coords Fast3dWindow::GetMouseDelta() {
    return { 0, 0 };
}

Ship::CoordsF Fast3dWindow::GetMouseWheel() {
    return { 0.0f, 0.0f };
}

bool Fast3dWindow::GetMouseState(Ship::MouseBtn btn) {
    (void)btn;
    return false;
}

void Fast3dWindow::SetMouseCapture(bool capture) {
    (void)capture;
}

bool Fast3dWindow::IsMouseCaptured() {
    return false;
}

uint32_t Fast3dWindow::GetCurrentRefreshRate() {
    return 60;
}

bool Fast3dWindow::SupportsWindowedFullscreen() {
    return false;
}

bool Fast3dWindow::CanDisableVerticalSync() {
    return false;
}

void Fast3dWindow::SetResolutionMultiplier(float multiplier) {
    mInterpreter->SetResolutionMultiplier(multiplier);
}

void Fast3dWindow::SetMsaaLevel(uint32_t value) {
    (void)value; // PICA has no MSAA (stream A: ResolveMSAAColorBuffer is a no-op)
}

void Fast3dWindow::SetFullscreen(bool isFullscreen) {
    (void)isFullscreen;
}

bool Fast3dWindow::IsFullscreen() {
    return true;
}

bool Fast3dWindow::IsRunning() {
    return mWindowManagerApi->IsRunning();
}

uintptr_t Fast3dWindow::GetGfxFrameBuffer() {
    return mInterpreter->mGfxFrameBuffer;
}

const char* Fast3dWindow::GetKeyName(int32_t scancode) {
    (void)scancode;
    return "";
}

std::string Fast3dWindow::GetWindowBackendName() {
    return "citro3d";
}

void Fast3dWindow::SetCurrentDimensions(uint32_t width, uint32_t height) {
    (void)width; (void)height;
}

void Fast3dWindow::SetCurrentDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    (void)width; (void)height; (void)posX; (void)posY;
}

void Fast3dWindow::SetCurrentDimensions(bool isFullscreen, uint32_t width, uint32_t height) {
    (void)isFullscreen; (void)width; (void)height;
}

void Fast3dWindow::SetCurrentDimensions(bool isFullscreen, uint32_t width, uint32_t height, int32_t posX,
                                        int32_t posY) {
    (void)isFullscreen; (void)width; (void)height; (void)posX; (void)posY;
}

Ship::WindowRect Fast3dWindow::GetPrimaryMonitorRect() {
    return { 0, 0, (int32_t)kGdx3dsScreenWidth, (int32_t)kGdx3dsScreenHeight };
}

std::weak_ptr<Interpreter> Fast3dWindow::GetInterpreterWeak() const {
    return mInterpreter;
}

std::shared_ptr<GfxDebugger> Fast3dWindow::GetGfxDebugger() const {
    return mGfxDebugger;
}

// Static callback slots from the header — inert on 3DS, defined because the class
// declares them and other TUs could reference them.
bool Fast3dWindow::KeyDown(int32_t scancode) {
    (void)scancode;
    return false;
}

bool Fast3dWindow::KeyUp(int32_t scancode) {
    (void)scancode;
    return false;
}

void Fast3dWindow::AllKeysUp() {
}

bool Fast3dWindow::MouseButtonDown(int button) {
    (void)button;
    return false;
}

bool Fast3dWindow::MouseButtonUp(int button) {
    (void)button;
    return false;
}

void Fast3dWindow::OnFullscreenChanged(bool isNowFullscreen) {
    (void)isNowFullscreen;
}

} // namespace Fast
