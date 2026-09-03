// port/3ds/lus_glue/gdx3ds_window_stub.cpp — Ship::Window base-class members for the
// 3DS build, replacing src/ship/window/Window.cpp at link.
//
// The desktop Window.cpp couples the base class to LUS Config (SetWindowBackend
// persists the backend id) and to the Gui layer (dtor calls ShutDownImGui). Neither
// exists in the carved 3DS build, so this replacement keeps the same state machine
// with the Config/Gui touches removed. The class layout comes from the real header;
// only member definitions live here.

#include "ship/window/Window.h"

#include <algorithm>

namespace Ship {

Window::Window(std::shared_ptr<Gui> gui, std::shared_ptr<MouseStateManager> mouseStateManager) {
    mGui = gui;                             // null on 3DS (no ImGui); nothing here derefs it
    mMouseStateManager = mouseStateManager; // null on 3DS; the 3DS Fast3dWindow never derefs it
    mAvailableWindowBackends = std::make_shared<std::vector<int32_t>>();
    mWindowBackend = 0;
    mFullscreenScancode = -1;
    mMouseCaptureScancode = -1;
}

Window::Window(std::shared_ptr<Gui> gui) : Window(gui, nullptr) {
}

Window::Window(std::vector<std::shared_ptr<GuiWindow>> guiWindows) : Window(nullptr, nullptr) {
    (void)guiWindows;
}

Window::Window() : Window(nullptr, nullptr) {
}

Window::~Window() {
}

void Window::ToggleFullscreen() {
    SetFullscreen(!IsFullscreen());
}

float Window::GetCurrentAspectRatio() {
    return (float)GetWidth() / (float)GetHeight();
}

int32_t Window::GetLastScancode() {
    return mLastScancode;
}

void Window::SetLastScancode(int32_t scanCode) {
    mLastScancode = scanCode;
}

std::shared_ptr<Gui> Window::GetGui() {
    return mGui;
}

void Window::SaveWindowToConfig() {
    // Desktop persists geometry to LUS Config; the 3DS screen is fixed 400x240.
}

int32_t Window::GetWindowBackend() {
    return mWindowBackend;
}

std::shared_ptr<std::vector<int32_t>> Window::GetAvailableWindowBackends() {
    return mAvailableWindowBackends;
}

bool Window::IsAvailableWindowBackend(int32_t backendId) {
    if (backendId < 0) {
        return false;
    }
    return std::find(mAvailableWindowBackends->begin(), mAvailableWindowBackends->end(), backendId) !=
           mAvailableWindowBackends->end();
}

bool Window::ShouldAutoCaptureMouse() {
    return false;
}

void Window::SetAutoCaptureMouse(bool capture) {
    (void)capture;
}

bool Window::ShouldForceCursorVisibility() {
    return false;
}

void Window::SetForceCursorVisibility(bool visible) {
    (void)visible;
}

int32_t Window::GetFullscreenScancode() {
    return mFullscreenScancode;
}

int32_t Window::GetMouseCaptureScancode() {
    return mMouseCaptureScancode;
}

void Window::SetFullscreenScancode(int32_t scancode) {
    mFullscreenScancode = scancode;
}

void Window::SetMouseCaptureScancode(int32_t scancode) {
    mMouseCaptureScancode = scancode;
}

std::shared_ptr<MouseStateManager> Window::GetMouseStateManager() {
    return mMouseStateManager;
}

void Window::SetWindowBackend(int32_t backend) {
    mWindowBackend = backend; // desktop also persists to Config here
}

void Window::AddAvailableWindowBackend(int32_t backend) {
    mAvailableWindowBackends->push_back(backend);
}

int32_t Window::GetSavedWindowBackend() {
    // No Config on 3DS: the first (only) registered backend wins.
    if (mAvailableWindowBackends && !mAvailableWindowBackends->empty()) {
        return mAvailableWindowBackends->front();
    }
    return -1;
}

std::string Window::GetWindowBackendName() {
    return "";
}

} // namespace Ship
