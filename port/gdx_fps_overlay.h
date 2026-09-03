#pragma once

#include "ship/window/gui/GuiWindow.h"

/** Lightweight on-screen FPS/frame-time counter using the same ImGui metrics as StatsWindow. */
class GdxFpsOverlay final : public Ship::GuiWindow {
  public:
    GdxFpsOverlay();

    void Draw() override;
    void DrawElement() override;

  protected:
    void InitElement() override;
    void UpdateElement() override;
};
