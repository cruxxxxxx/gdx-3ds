/* port/3ds/game/gdx3ds_snapshot_game.c -- mp-snapshot: the game-side half of the snapshot module
 * (port/3ds/gdx_snapshot.c, contract port/gdx_snapshot.h). Read only.
 *
 * Compiled into gdx3ds_game (decomp include paths, decomp types), but NOT under decomp/src/, so the
 * snapshot linker fragments (port/3ds/gdx3ds_snapshot_{data,bss}.ld) do not put this TU's own
 * statics in the snapshot -- it has none.
 *
 *   - the window scope: an on-track race, in its update state, no mode change queued;
 *   - the REQUIRED table: the simulation globals the snapshot must cover. COMMON symbols are placed
 *     by whichever object the linker picked for the tentative definition, and some sim globals are
 *     DEFINED by port TUs (gSegments in port/decomp_port.c -- marked GDX_SNAP_STATE_BSS there;
 *     gArenaStartPtrs in the generated port/gen/LinkStubs.c -- not marked). The module checks every
 *     entry against its blocks at arm time and covers a miss as an extra region (with a
 *     "[snap] REQUIRED" receipt) instead of trusting the link. */
#include "global.h"
#include "controller.h"
#include "fzx_camera.h"
#include "fzx_course.h"
#include "fzx_effects.h"
#include "fzx_game.h"
#include "fzx_racer.h"

extern s32 gGameMode;
extern s32 gQueuedGameMode;
extern s16 gGameModeChangeState;
extern s32 gNumPlayers;
extern s32 gTotalRacers;
extern s32 gRaceIntroTimer;
extern u32 gGameFrameCount;
extern s32 gRandSeed1;
extern u32 gRandMask1;
extern s32 gRandSeed2;
extern u32 gRandMask2;
extern uintptr_t gSegments[16];
extern uintptr_t gArenaStartPtrs[];
extern s32 D_800CCFB0;
extern s32 D_800CCFB4;
extern s32 D_800CCFB8;
extern s8 gGamePaused;
extern s16* D_i3_8013DBE8; /* cpu.c: the CPU-AI arena buffer's pointer (the buffer itself is dynamic) */

typedef struct GdxSnapRequired {
    const void* ptr;
    unsigned int size;
    const char* name;
} GdxSnapRequired;

int gdx3ds_snap_game_scope(void);
int gdx3ds_snap_game_mode_word(void);
int gdx3ds_snap_game_single_player(void);
int gdx3ds_snap_game_required(const GdxSnapRequired** out);

#define REQ(sym) { (const void*) &(sym), (unsigned int) sizeof(sym), #sym }

static const GdxSnapRequired sRequired[] = {
    REQ(gRacers),
    REQ(gCameras),
    REQ(gMachineDebris),
    REQ(gCourseCtx),
    REQ(gGameFrameCount),
    REQ(gRandSeed1),
    REQ(gRandMask1),
    REQ(gRandSeed2),
    REQ(gRandMask2),
    REQ(gGameMode),
    REQ(gQueuedGameMode),
    REQ(gGameModeChangeState),
    REQ(gNumPlayers),
    REQ(gTotalRacers),
    REQ(gRaceIntroTimer),
    REQ(gGamePaused),
    REQ(gSegments),
    REQ(D_800CCFB0),
    REQ(D_800CCFB4),
    REQ(D_800CCFB8),
    REQ(D_i3_8013DBE8),
    { (const void*) gArenaStartPtrs, (unsigned int) (ARENA_COUNT * sizeof(uintptr_t)), "gArenaStartPtrs" },
    { (const void*) gControllers, (unsigned int) (5 * sizeof(Controller)), "gControllers" },
    REQ(gSharedController),
    { (const void*) gPlayerControlPorts, (unsigned int) (4 * sizeof(gPlayerControlPorts[0])), "gPlayerControlPorts" },
};

static int IsOnTrackRaceMode(s32 mode) {
    switch (mode) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_DEATH_RACE:
            return 1;
        default:
            return 0;
    }
}

/* 1 while a snapshot window may be open: an on-track race in its update state with no mode change
 * queued. Arena reloads, overlay DMA and the mode-init allocations all happen outside it. */
int gdx3ds_snap_game_scope(void) {
    if (!IsOnTrackRaceMode(GET_MODE(gGameMode))) {
        return 0;
    }
    if (gQueuedGameMode != gGameMode) {
        return 0;
    }
    return gGameModeChangeState == GAMEMODE_UPDATE;
}

/* A word that changes whenever the mode or its change state does: a window that sees it change was
 * crossed by a transition and is abandoned (never restored). */
int gdx3ds_snap_game_mode_word(void) {
    return (int) (((u32) gGameMode & 0xFFFF) | ((u32) (u16) gGameModeChangeState << 16)) ^ (int) gQueuedGameMode;
}

int gdx3ds_snap_game_single_player(void) {
    return gNumPlayers == 1;
}

int gdx3ds_snap_game_required(const GdxSnapRequired** out) {
    *out = sRequired;
    return (int) (sizeof(sRequired) / sizeof(sRequired[0]));
}
