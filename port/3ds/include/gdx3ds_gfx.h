/* port/3ds/include/gdx3ds_gfx.h -- 3DS renderer contract (stream A implements).
 *
 * CONTRACT STATUS: FROZEN (Phase 0, carve spike PASS -- see
 * docs/research/spike-lus-carve-report.md). Changes require orchestrator sign-off.
 *
 * The spike verdict keeps libultraship's Fast3D interpreter, so the renderer
 * contract IS the pinned LUS interface: stream A implements a citro3d backend
 * subclassing Fast::GfxRenderingAPI
 * (libultraship/include/fast/backends/gfx_rendering_api.h -- C++ virtual class,
 * two-part 64-bit shader IDs via CreateAndLoadNewShader(uint64_t, uint64_t)).
 * This header only adds the factory seam the 3DS bootstrap calls; everything
 * else comes from the LUS header to avoid drift.
 *
 * Build notes for stream A (from the spike):
 *   - include path must provide the spike-derived stub headers (imconfig.h,
 *     Gui.h, spdlog, SDL2 types) ahead of real desktop deps -- see
 *     spike-lus-carve/stubs/ for the proven set
 *   - replicate libultraship/cmake/cvars.cmake compile definitions
 *   - exceptions/RTTI stay ON; do not define ENABLE_OPENGL
 */
#ifndef GDX3DS_GFX_H
#define GDX3DS_GFX_H

#ifdef __cplusplus

namespace Fast {
class GfxRenderingAPI;
}

/* Returns the process-lifetime citro3d backend instance. First call constructs it;
 * the interpreter takes it via Fast3dWindow wiring in the 3DS bootstrap. */
Fast::GfxRenderingAPI* Gdx3ds_GetCitro3dRenderer();

#endif /* __cplusplus */

#endif /* GDX3DS_GFX_H */
