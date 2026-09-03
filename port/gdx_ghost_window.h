#pragma once

#include "gdx_ghost_io.h"
#include "ship/window/gui/GuiWindow.h"

/**
 * @brief Browser for the host-side per-course player-ghost library (see gdx_ghost_io.h).
 *
 * Selection is capped at three entries per exact course, the engine's native ghost count.
 * Staff ghosts stay owned by the original ROM/EK unlock and loading paths.
 */
class GdxGhostWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

  protected:
    void InitElement() override;
    void UpdateElement() override;
    void DrawElement() override;

  private:
    void RefreshLibrary();

    GdxGhostLibraryEntry mEntries[GDX_GHOST_LIBRARY_MAX_ENTRIES] = {};
    int mEntryCount = 0;
    int32_t mSelectedEncodedCourse = 0;
    uint64_t mSelectedGhostId = 0;
    char mStatus[256] = {};
};
