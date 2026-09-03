#ifndef GDX_CAMERA_POSE_H
#define GDX_CAMERA_POSE_H

/* port/gdx_camera_pose.h — snapshot of the inputs Camera_UpdateProjectionViewMtx consumes,
   plus a rebuild of its output from a snapshot.

   The projection-view matrix (GfxPool::unk_20208[camera->id]) is the one pool matrix that
   cannot be lerped element-wise: the live Expansion Kit branch of
   Camera_UpdateProjectionViewMtx (decomp/src/game/camera.c:1249-1276) widens fov as a
   function of the matrix that fov itself produced, so the matrix is not an affine function
   of the camera state and interpolating the FINISHED matrix drifts away from the feedback
   loop's fixed point. Interpolating the INPUTS (eye/at/up/fov/frustum) and re-running the
   build per sub-frame reproduces the loop exactly at every t instead.

   Do NOT include <stddef.h> or <stdint.h> here — same contract as port/n64_rdram.h. This
   header is consumed by decomp_port.c, compiled with the decomp's headers, where the decomp's
   own libc/stdint.h is already live and the platform headers clash with it. C callers must
   include global.h first (provides size_t); C++ callers must include a standard header before
   this one. For the same reason the integer fields below are plain `int` rather than int32_t:
   `int` is 32-bit on every host ABI this port targets and needs no header at all. */

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_CAMERA_POSE_MAX 4

/* One camera's build inputs, copied out of gCameras[index] at snapshot time.

   `upX/Y/Z` is camera->basis.y — the UP vector Camera_UpdateProjectionViewMtx passes to
   Matrix_SetLookAt (camera.c:1253-1254), not basis.x (forward) or basis.z (side).

   `nearZ`/`farZ` mirror camera->near / camera->far. Spelled with the Z suffix because `near`
   and `far` are legacy MSVC segment-qualifier macros; the decomp's Camera struct gets away
   with the bare names only because nothing in its include chain defines them.

   `numPlayers` is gNumPlayers SNAPSHOTTED with the rest of the pose. A sub-frame rebuild runs
   against a pose captured on an earlier tick, so reading the live global would let the EK
   branch's `gNumPlayers != 2` gate sample a value the pose was never built under. */
typedef struct GdxCameraPose {
    float eyeX, eyeY, eyeZ;
    float atX, atY, atZ;
    float upX, upY, upZ;          /* camera->basis.y */
    float fov;
    float nearZ, farZ;
    float fovScaleX, fovScaleY;
    float frustrumCenterX, frustrumCenterY;
    int numPlayers;               /* gNumPlayers at snapshot time; gates the EK fov widening */
    /* The fov the EK widening branch settled on for THIS tick, resolved once by
       gdx_camera_resolve_fov. The branch is a THRESHOLD (ABS(projectionViewMtx.m[3][1]) > 30000,
       camera.c:1259) that the game only ever evaluates at 60Hz, where crossing it between two
       ticks is invisible. Re-evaluating it per sub-frame on interpolated inputs makes it flip ON
       mid-tick and OFF again, snapping the fov and jerking everything ~20px vertically for one
       sub-frame. So the threshold is decided per tick and the RESULT is what gets interpolated. */
    float resolvedFov;
    int fovIsResolved;            /* when non-zero, build_projview uses resolvedFov and skips the branch */
    int id;
    int valid;
} GdxCameraPose;

/* Array bound, NOT the number of ACTIVE cameras: gCameras is a fixed 4 (camera.c:17) and the
   live count sNumCameras is file-scope in camera.c with no header declaration. Slots the game
   is not driving hold stale or zero state; the caller decides per slot whether a pose is usable
   (its t=1 snap check). */
int gdx_camera_pose_count(void);

/* Copy gCameras[index] plus the live gNumPlayers into *out and mark it valid.
   Returns 1 on success, 0 on refusal (index out of range, or out == NULL). On refusal *out is
   zeroed when non-NULL, so a caller that ignores the return value gets valid == 0 rather than an
   uninitialised pose that gdx_camera_build_projview would consume. */
int gdx_camera_pose_read(int index, GdxCameraPose* out);

/* Rebuild the projection-view matrix from a pose and write it as an N64 Mtx into outMtx64.

   Reproduces the live (EXPANSION_KIT) branch of Camera_UpdateProjectionViewMtx exactly,
   including the fov feedback loop, against LOCAL scratch only: it never touches gCameras or
   any GfxPool, so a sub-frame rebuild cannot perturb game state (interpolation is render-only).

   outMtx64 must point at 64 writable bytes — the Mtx image Camera_MatrixToMtx writes through
   Mtx::u (two u16[4][4] halves), which is what the stride reported by
   gdx_gfxpool_camera_mtx_layout describes. Only those 64 bytes are written even on an LP64
   host where sizeof(Mtx) is larger because Mtx::m is long[4][4].

   Returns 0 WITHOUT writing outMtx64 when pose or outMtx64 is NULL, when pose->valid is 0, or
   when the lookat input is degenerate (eye == at, or up parallel to at - eye). The degenerate
   cases matter: Matrix_SetLookAt signals them only by returning early without calling
   Matrix_ToMtx (math.c:1013/1027/1041), leaving a half-written view matrix that would otherwise
   be fed to Camera_CalculateProjectionViewMtx. Refusing lets the caller fall back to the game's
   own matrix for that sub-frame. */
int gdx_camera_build_projview(const GdxCameraPose* pose, void* outMtx64);

/* Decide the EK fov-widening threshold ONCE for this tick and record the resulting fov in the
   pose. See the resolvedFov field for why the threshold must never be re-tested per sub-frame. */
int gdx_camera_resolve_fov(GdxCameraPose* pose);

/* Ground truth for the PER-CAMERA projection-view matrix array GfxPool::unk_20208, the slot
   Camera_UpdateProjectionViewMtx writes (camera.c:1278) and the one the bridge must recognise
   to route a camera matrix into the rebuild path. Sibling of gdx_gfxpool_racer_mtx_layout, for
   the same reason: the struct-comment offsets in sys.h are the N64 layout, and sizeof(Gfx)
   doubling under PORT shifts every member after gfxBuffer, so the offset must come from
   offsetof on the real type. Every out-param is optional. */
void gdx_gfxpool_camera_mtx_layout(size_t* outProjView, size_t* outStride, size_t* outCount);

#ifdef __cplusplus
}
#endif

#endif /* GDX_CAMERA_POSE_H */
