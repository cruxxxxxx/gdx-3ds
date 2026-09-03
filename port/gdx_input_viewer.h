#pragma once

#include "ship/window/gui/GuiWindow.h"

/** Input overlay drawn entirely with ImGui primitives from the state delivered to F-Zero X. */
class GdxInputViewer final : public Ship::GuiWindow {
  public:
    GdxInputViewer();

    void Draw() override;
    void DrawElement() override;

  protected:
    void InitElement() override;
    void UpdateElement() override;
};
