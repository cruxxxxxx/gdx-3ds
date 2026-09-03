/* port/3ds/harness/scenes.h — synthetic F3DEX2 test scenes for the citro3d backend.
 *
 * Implemented in scenes.cpp, which is the ONLY TU that includes the decomp's
 * PR/gbi.h (its G_* / gDP* macros collide with fast/lus_gbi.h, so the two header
 * families never meet in one TU). The interface is therefore deliberately
 * opaque: display lists cross this boundary as void* and the caller
 * (dl_tests_main.cpp) casts to Fast::F3DGfx*. kSceneGfxPacketSize lets the
 * caller verify at runtime that both TUs agree on the 8-byte packet layout.
 */
#ifndef GDX3DS_HARNESS_SCENES_H
#define GDX3DS_HARNESS_SCENES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { kSceneCount = 9 };

typedef struct SceneInfo {
    const char* name;     /* short id, e.g. "STRIP" */
    const char* checks;   /* which TODO(citra-verify) item this scene exposes */
    const char* expected; /* one-paragraph expected result (console-printable) */
} SceneInfo;

/* sizeof(Gfx) as seen by scenes.cpp; must equal sizeof(Fast::F3DGfx). */
extern const size_t kSceneGfxPacketSize;

/* Allocates vertex/texture/matrix/DL storage. Texture data goes to linearAlloc
 * (addresses > 0x0FFFFFFF) because the interpreter's G_SETTIMG handler silently
 * drops image pointers <= 0x0FFFFFFF as unresolved N64 segment addresses — the
 * regular 3DS malloc heap (0x08000000+) is below that line. Returns 0 on
 * allocation failure. */
int ScenesInit(void);

const SceneInfo* SceneGetInfo(int scene);

/* Rebuilds and returns scene's display list for this frame (frame only matters
 * for scene 1, the rotation test). The returned buffer stays valid until the
 * next call. */
void* SceneBuildDl(int scene, unsigned frame);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_HARNESS_SCENES_H */
