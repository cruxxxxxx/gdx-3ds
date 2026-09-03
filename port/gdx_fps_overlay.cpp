#include "gdx_fps_overlay.h"

#include <imgui.h>

GdxFpsOverlay::GdxFpsOverlay()
    : Ship::GuiWindow("gOpenWindows.FpsCounter", false, "FPS Counter") {
}

void GdxFpsOverlay::InitElement() {
}

void GdxFpsOverlay::UpdateElement() {
}

void GdxFpsOverlay::Draw() {
    if (!IsVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 topRight = viewport->WorkPos + ImVec2(viewport->WorkSize.x - 12.0f, 12.0f);
    ImGui::SetNextWindowPos(topRight, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.065f, 0.082f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.14f));

    if (ImGui::Begin("FPS Counter##GdxOverlay", nullptr, flags)) {
        DrawElement();
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    SyncVisibilityConsoleVariable();
}

// With Frame Interpolation on the renderer presents several sub-frames per logic tick, so
// io.Framerate is presents/sec and says nothing about whether the SIMULATION is keeping up — hence
// the second figure, "144.0 FPS (sim 59.9 Hz)". It must stay MEASURED (gdx_host_sim_hz, written by
// main.cpp's frame loop); a confident wrong rate here sends people looking in the wrong place.
//
// Declared extern "C" rather than including n64_gfx_bridge.h — the same idiom the menu uses for the
// other gdx_gfx_interp_* accessors.
extern "C" int gdx_gfx_interp_host_active(void);
extern "C" double gdx_host_sim_hz(void);
// Per-tick truth: main.cpp forces interpolation off for a tick (Course Edit's ~20 Hz cursor mode)
// while the raw CVar above is still on. Reading only the CVar would keep printing a sim rate the
// paused path is not producing.
extern "C" int gdx_gfx_interp_tick_active(void);

void GdxFpsOverlay::DrawElement() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("%.1f FPS", io.Framerate);
    const bool hostActive = gdx_gfx_interp_host_active() != 0;
    const bool tickActive = gdx_gfx_interp_tick_active() != 0;
    if (hostActive && !tickActive) {
        ImGui::SameLine();
        ImGui::TextDisabled("(interp paused)");
    } else if (hostActive) {
        // Colour is the point: a sim rate below 60 means the GAME CLOCK is losing time (everything
        // animates and counts down slow), which is a correctness fault, not a smoothness one.
        // 0.0 means the rolling window has not closed yet on this run.
        const double simHz = gdx_host_sim_hz();
        ImGui::SameLine();
        if (simHz <= 0.0) {
            ImGui::TextDisabled("(sim --)");
        } else if (simHz < 50.0) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "(sim %.1f Hz)", simHz);
        } else if (simHz < 58.0) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "(sim %.1f Hz)", simHz);
        } else {
            ImGui::TextDisabled("(sim %.1f Hz)", simHz);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f ms", io.DeltaTime * 1000.0f);
}
