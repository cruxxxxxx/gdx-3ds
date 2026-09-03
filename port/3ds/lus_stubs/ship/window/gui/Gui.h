/* Spike stub: interpreter.cpp includes Gui.h but never uses it; Window.h needs the
 * type names to exist. */
#pragma once
#include <memory>
#include <string>

namespace Ship {
class GuiWindow {
  public:
    virtual ~GuiWindow() = default;
};
class Gui {
  public:
    virtual ~Gui() = default;
};
} // namespace Ship
