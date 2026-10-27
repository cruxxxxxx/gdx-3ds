/* port/3ds/game/gdx3ds_fullview_game.c — MP phase 3 (track mp-fullview): one player's view full
 * screen in a 2-4P VS race. Decomp-side contract and the rule: gfx/gdx3ds_fullview_shim.h.
 *
 * Levers (read once, at the first Race_Draw):
 *   [debug] fullview_player = k (default -1 = off): local test lever, no networking.
 *   [net]   mp_fullview     = 1 (the default; 0 = every view): k = this console's MP player
 *           index (gdx3ds_mp_local_player(): the spike session's mp_player, or the seat the
 *           mp-online lobby gave us, read per build). fullview_player wins.
 *
 * Active for a build when the lever names k, the mode is VS 2P/3P/4P outside the attract demo,
 * k < gNumPlayers, and no camera runs the end-of-race viewport transition (Camera_UpdateViewport-
 * Transition: the winner's view grows to full screen under the results; that animation is stock,
 * every view drawn, so every console shows the same results screen).
 *
 * Receipts: `[fullview] init k=<k> source=<debug|mp> ...` once; `[fullview] on k= players= mode=`
 * and `[fullview] off reason=` on every edge; `[fullview] builds=` with the off line.
 *
 * Compiled into gdx3ds_game (decomp include paths, decomp types). */
#include "global.h"
#include "fzx_game.h"
#include "fzx_racer.h"
#include "fzx_camera.h"
#include ASSET_HEADER(course_track_gfx.h)

#include "gdx3ds_fullview_shim.h"

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_mp_local_player(void) __attribute__((weak)); /* net/gdx3ds_mp.c */
extern int gdx3ds_mpg_stage(void) __attribute__((weak));       /* net/gdx3ds_mp_game.c */
extern int gdx_remove_borders(void);                            /* port/input_bridge.c */
extern void* gdx_segmented_to_host_pointer(uintptr_t segmentedAddr);

extern s32 gGameMode;
extern s32 gNumPlayers;
extern s8 gTitleDemoState;
extern Camera gCameras[];
extern ScissorBox gScissorBoxFullScreen;

/* The camera's CPU-side 2D mirrors (Camera_InitViewport): what the race HUD's markers and the
 * stars project through. */
typedef struct GdxFvMirror {
    f32 vpScaleX;
    f32 vpScaleY;
    f32 vpTransX;
    f32 vpTransY;
    f32 scissorLeft;
    f32 scissorTop;
    f32 scissorRight;
    f32 scissorBottom;
} GdxFvMirror;

static s32 sLatched = 0;
static s32 sLeverK = -1;       /* the configured local player, -1 = lever off */
static s32 sDrawK = -1;        /* this build's local view (between race_draw_begin and _end) */
static s32 sLastK = -1;        /* the previous build's answer, for the edge receipts */
static u32 sBuilds = 0;        /* builds with fullview on since the last edge */
static u32 sAudioRewrites = 0; /* audio commands re-mixed for the local player, total */
static s32 sSwapView = -1;     /* view whose mirrors gdx_fv_rhud_begin swapped */
static GdxFvMirror sSaved;
static f32 sOtherSeScale = 0.0f; /* mppolish [net] mp_other_se_pct / 100 (Latch) */

enum {
    FV_OFF_LEVER = 0,
    FV_OFF_MODE = 1,
    FV_OFF_PLAYERS = 2,
    FV_OFF_TRANSITION = 3,
};

static const char* const kReasons[] = { "lever", "mode", "players", "results-transition" };

/* [fvse] receipt (hw: "the client (old 3ds) doesnt play any sfx"): per human p, the SE starts
 * the game queued on his channels (engine 2+p, level SE 6+p, player SE 10+p; SET_IO port 0) and
 * the volume commands on his SE channels this console left alone ("kept") or scaled down
 * ("muted", another human's). Seq player 0 only, while the mixer is armed. Logged at every
 * fullview edge and every FV_SE_RECEIPT_EVERY starts, at most FV_SE_RECEIPTS lines. */
#define FV_SE_RECEIPT_EVERY 512
#define FV_SE_RECEIPTS 64
static u32 sSeStart[4][3]; /* [player][engine, level, player SE] */
static u32 sSeOtherStarts = 0; /* seq player 0 starts on non-player channels (0, 1, 14, 15) */
static u32 sSeVolKept[4];
static u32 sSeVolMuted[4];
static u32 sSeStartsTotal = 0;
static u32 sSeReceipts = 0;

int gdx_fv_local_player(void);

static void FvSeReceipt(const char* why) {
    s32 p;
    if (&gdx_dbg_logf == NULL || sSeReceipts >= FV_SE_RECEIPTS) {
        return;
    }
    sSeReceipts++;
    gdx_dbg_logf("[fvse] %s local=%d players=%d other_pct=%d other_ch_starts=%u\n", why, (int) gdx_fv_local_player(),
                 (int) gNumPlayers, (int) (sOtherSeScale * 100.0f + 0.5f), (unsigned) sSeOtherStarts);
    for (p = 0; p < 4; p++) {
        if (p >= gNumPlayers && (sSeStart[p][0] | sSeStart[p][1] | sSeStart[p][2]) == 0) {
            continue;
        }
        gdx_dbg_logf("[fvse]   p%d starts eng=%u lvl=%u se=%u | se vol kept=%u muted=%u\n", (int) p,
                     (unsigned) sSeStart[p][0], (unsigned) sSeStart[p][1], (unsigned) sSeStart[p][2],
                     (unsigned) sSeVolKept[p], (unsigned) sSeVolMuted[p]);
    }
}

static void LogEdge(s32 k, s32 reason) {
    if (&gdx_dbg_logf == NULL) {
        return;
    }
    FvSeReceipt(k >= 0 ? "edge-on" : "edge-off");
    if (k >= 0) {
        gdx_dbg_logf("[fullview] on k=%d players=%d mode=%d\n", (int) k, (int) gNumPlayers,
                     (int) GET_MODE(gGameMode));
    } else {
        gdx_dbg_logf("[fullview] off reason=%s builds=%u audio_rewrites=%u mode=%d\n", kReasons[reason],
                     (unsigned) sBuilds, (unsigned) sAudioRewrites, (int) GET_MODE(gGameMode));
    }
}

/* The MP player is NOT latched: an mp-online console's index is decided per session (the lobby
 * seats players at START), so it is read at every build. Only the two levers are latched. */
static s32 sMpOn = 0;

static s32 MpPlayer(void) {
    const s32 k = (&gdx3ds_mp_local_player != NULL) ? gdx3ds_mp_local_player() : -1;
    return (sMpOn != 0 && k >= 0 && k < 4) ? k : -1;
}

static void Latch(void) {
    s32 debugK;

    sLatched = 1;
    if (&gdx3ds_config_get_int == NULL) {
        return;
    }
    debugK = gdx3ds_config_get_int("debug", "fullview_player", -1);
    /* Default ON: it only ever acts in a live MP session (MpPlayer() is -1 otherwise). */
    sMpOn = gdx3ds_config_get_int("net", "mp_fullview", 1);
    if (debugK >= 0 && debugK < 4) {
        sLeverK = debugK;
    }
    {
        /* mppolish: the other humans' level / player SE on this console, percent (engines: 40) */
        s32 pct = gdx3ds_config_get_int("net", "mp_other_se_pct", 0);
        pct = (pct < 0) ? 0 : ((pct > 100) ? 100 : pct);
        sOtherSeScale = (f32) pct / 100.0f;
    }
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[fullview] init k=%d source=%s debug.fullview_player=%d net.mp_fullview=%d mp_player=%d "
                     "other_se_pct=%d\n",
                     (int) sLeverK, sLeverK >= 0 ? "debug" : (sMpOn != 0 ? "mp" : "off"), (int) debugK, (int) sMpOn,
                     (int) MpPlayer(), (int) (sOtherSeScale * 100.0f + 0.5f));
    }
}

/* The debug lever wins; else this console's MP player; -1 = off. */
static s32 LeverK(void) {
    return sLeverK >= 0 ? sLeverK : MpPlayer();
}

static s32 IsVsMode(void) {
    const s32 mode = GET_MODE(gGameMode);
    return (mode == GAMEMODE_VS_2P) || (mode == GAMEMODE_VS_3P) || (mode == GAMEMODE_VS_4P);
}

static s32 AnyTransition(void) {
    s32 i;
    for (i = 0; i < gNumPlayers && i < 4; i++) {
        if (gCameras[i].vpTransitionState == CAMERA_VP_TRANSITION_ACTIVE) {
            return 1;
        }
    }
    return 0;
}

/* -1 and *reason when fullview is off for this build, else k. */
static s32 Decide(s32* reason) {
    const s32 k = LeverK();
    if (k < 0) {
        *reason = FV_OFF_LEVER;
        return -1;
    }
    if (!IsVsMode() || (gTitleDemoState != TITLE_DEMO_INACTIVE)) {
        *reason = FV_OFF_MODE;
        return -1;
    }
    if (gNumPlayers < 2 || gNumPlayers > 4 || k >= gNumPlayers) {
        *reason = FV_OFF_PLAYERS;
        return -1;
    }
    if (AnyTransition()) {
        *reason = FV_OFF_TRANSITION;
        return -1;
    }
    return k;
}

int gdx_fv_race_draw_begin(void) {
    s32 reason = FV_OFF_LEVER;
    s32 k;

    if (!sLatched) {
        Latch();
    }
    if (sLeverK < 0 && sMpOn == 0) {
        sDrawK = -1;
        return -1;
    }
    k = Decide(&reason);
    if (k != sLastK) {
        LogEdge(k, reason);
        sBuilds = 0;
    }
    sLastK = k;
    if (k >= 0) {
        sBuilds++;
    }
    sDrawK = k;
    return k;
}

void gdx_fv_race_draw_end(void) {
    sDrawK = -1;
}

int gdx_fv_k(void) {
    return sDrawK;
}

/* Outside Race_Draw (stereo, the bottom screen, audio): the local player while a VS race runs
 * fullview, else -1. Not latched: reads the live mode. */
int gdx_fv_live_k(void) {
    s32 reason;
    if (!sLatched) {
        return -1;
    }
    if (sLeverK < 0 && sMpOn == 0) {
        return -1;
    }
    return Decide(&reason);
}

/* mp-fullbleed [net] mp_fullview_wide (default 1; 0 = the split frame kind, 06c5264): with
 * [display] widescreen = 1 a VS race publishes the SPLIT frame kind (gdx3ds_widescreen.h): no
 * RemoveBorders, so the one full-screen fullview view kept the 296x224 CRT-safe frame (overscan
 * bands under border_mode 0, a stretched non-5:3 picture under 1-3) while the 1P race renders the
 * full 320x240 frame at 5:3. A fullview frame is one full-screen view, so it publishes the 1P kind.
 * Render-only: RemoveBorders moves the SCISSOR_BOX_FULL_SCREEN viewport / scissor / CPU mirrors
 * (only the local view uses that box in a VS race; every other view keeps its half box) and the
 * backend's border scale; nothing the simulation or the lockstep digests read. Live, like
 * gdx_fv_live_k: the results transition (fullview off) goes back to the split kind. */
static s32 sWideLever = -1;

int gdx_fv_wide_single_view(void) {
    if (sWideLever < 0) {
        if (&gdx3ds_config_get_int == NULL) {
            sWideLever = 1;
        } else {
            sWideLever = gdx3ds_config_get_int("net", "mp_fullview_wide", 1) != 0 ? 1 : 0;
        }
        if (&gdx_dbg_logf != NULL) {
            gdx_dbg_logf("[fullview] wide init mp_fullview_wide=%d (1 = a fullview frame is the 1P widescreen kind)\n",
                         (int) sWideLever);
        }
    }
    if (sWideLever == 0) {
        return 0;
    }
    return gdx_fv_live_k() >= 0;
}

/* mppolish: the player a live VS race on this console belongs to, whether or not fullview draws
 * it this build. -1 outside a VS race or a session. Read live (the seat is per session). */
static s32 MpVsPlayer(void) {
    const s32 k = (&gdx3ds_mp_local_player != NULL) ? gdx3ds_mp_local_player() : -1;
    if (k < 0 || k >= gNumPlayers || gNumPlayers < 2 || gNumPlayers > 4) {
        return -1;
    }
    if (!IsVsMode() || (gTitleDemoState != TITLE_DEMO_INACTIVE)) {
        return -1;
    }
    return k;
}

/* mppolish: this console's own player for everything one player sees or hears (the dash, the
 * announcer), or -1 (stock: no local player). Fullview's k while it is on (the debug lever
 * included); else the MP seat, so the results transition (fullview off) and a [net]
 * mp_fullview = 0 console keep their own racer instead of falling back to player 0. */
int gdx_fv_local_player(void) {
    const s32 k = gdx_fv_live_k();
    return k >= 0 ? k : MpVsPlayer();
}

/* The bottom screen and the dash: the racer the local console plays (0 when there is none). */
int gdx_fv_local_racer(void) {
    const s32 k = gdx_fv_local_player();
    return k >= 0 ? k : 0;
}

/* mppolish (decomp-3ds-mp-polish.patch, hud.c): 1 when a race-wide announcer line triggered by
 * player `player` (the lap-2 "you got boost power", the final-lap voice) plays on this console.
 * Stock VS plays each line once, for the FIRST human to reach the lap, on the one shared speaker;
 * a console that is one player's speaker plays it for that player. Audio only: the caller
 * changes nothing but its own once-per-race "played" latch, which nothing else reads.
 * o3ds60: also racer.c's leader voice "pulling away from the pack" (NA_SE_59, when a human leads
 * by 4000) and the wrong-way voice (NA_SE_60, per player): every system SE plays on seq 0
 * channel 1, which the per-player mixer below never scales, so each console heard both for
 * every player. Their latches (RACER_SE_FLAGS_4000, gPlayerReverseTimer) run as stock. */
int gdx_fv_announce_for(int player) {
    const s32 k = gdx_fv_local_player();
    return (k < 0) || (player == k);
}

/* o3ds60: gdx_fv_announce_for with a receipt, for every gated announcer decision (the call sites
 * only reach it once the stock trigger fired):
 *   `[announce] se=NA_SE_59 racer=N local=M players=P played|skipped`.
 * hw 9088d3a: nobody heard NA_SE_59 in a 2-machine race; the line says whether it ever fired. */
#define FV_ANNOUNCE_RECEIPTS 256
static u32 sAnnounceReceipts = 0;

int gdx_fv_announce_se(int se, int player) {
    const int play = gdx_fv_announce_for(player);
    if (&gdx_dbg_logf != NULL && sAnnounceReceipts < FV_ANNOUNCE_RECEIPTS) {
        sAnnounceReceipts++;
        gdx_dbg_logf("[announce] se=NA_SE_%u racer=%d local=%d players=%d %s\n", (unsigned) se, player,
                     (int) gdx_fv_local_player(), (int) gNumPlayers, play ? "played" : "skipped");
    }
    return play;
}

/* ---- mppolish: the MP race camera frames like the 1P camera ------------------------------------
 *
 * A stock VS 2P view is a full 320x240 projection cropped to a half-height strip (aVpTopHalf /
 * aVpBottomHalf + the half scissor), so its race camera is framed for that strip: fov 100 (1P 75)
 * and frustrumCenterY -15 (1P 77; kWidescreenCameraRaceSettings / D_800D544C against the 1P
 * tables). Fullview shows the WHOLE projection full screen: a wide frustum whose centre sits lower
 * than the 1P one, so the view looks tilted down at the track. This rewrites the two projection
 * inputs the camera copies out of its settings every tick (Camera_UpdateFromSettings: camera->fov,
 * camera->frustrumCenterY) to the 1P framing. The camera settings themselves (the lerped fov and
 * centre, distance, pitch, eye, at) are untouched, so every tick starts from the stock values.
 *
 * Who reads the result: the projection (CPU MtxF and the GPU Mtx), the interp pose, the sky and
 * the stars, the course-chunk and machine culls -- all render -- and machineLod, which in a session
 * the simulation never reads (gdx_sim_machine_visible) but the lockstep digest folds as its "draw"
 * mask (gdx_mpo_vis_hash). So the rule is the same on every console: EVERY view of a live 2P
 * session is reframed, not only the local one (the others are not on screen), from sim-side state
 * only (mode, raceSetting, the viewport transition), and the lever is part of the sim-ABI word
 * (gdx3ds_mp_sim_levers_hash): two consoles that disagree on it refuse each other in the lobby.
 * Off a session (a local VS race) machineLod feeds the simulation, so nothing changes there unless
 * the [debug] fullview_cam_local test lever asks for it (screenshots; it moves that race).
 *
 * Levers: [net] mp_fullview_cam = 1 (0 = d9deb37's split-screen framing);
 *         [debug] fullview_cam_local = 0 (1 = also reframe a local VS 2P race that
 *         [debug] fullview_player shows full screen: a measurement lever). */
extern int gdx3ds_mp_fullview_cam_lever(void) __attribute__((weak)); /* gdx3ds_autodrive.c */
extern int gdx3ds_mp_active(void) __attribute__((weak));             /* net/gdx3ds_mp.c */
extern const CameraRaceSettingInfo kRegularCameraRaceSettings[];
extern const CameraRaceSettingInfo kWidescreenCameraRaceSettings[];

#define FV_CAM_FOV_MAX 120.0f /* Camera_UpdateRace's speed widening target, both tables */
#define FV_CAM_CENTRE_1P 77.0f
#define FV_CAM_CENTRE_2P -15.0f

static s32 sCamLocal = -1; /* [debug] fullview_cam_local, read once */
static u32 sCamReframes = 0;

static s32 CamLocalLever(void) {
    if (sCamLocal < 0) {
        sCamLocal = (&gdx3ds_config_get_int != NULL) ? gdx3ds_config_get_int("debug", "fullview_cam_local", 0) : 0;
    }
    return sCamLocal;
}

static s32 CamReframeOn(const Camera* camera) {
    if (GET_MODE(gGameMode) != GAMEMODE_VS_2P || gNumPlayers != 2 || gTitleDemoState != TITLE_DEMO_INACTIVE) {
        return 0;
    }
    if (camera->id < 0 || camera->id >= gNumPlayers || AnyTransition()) {
        return 0;
    }
    if (&gdx3ds_mp_active != NULL && gdx3ds_mp_active()) {
        return (&gdx3ds_mp_fullview_cam_lever != NULL) && gdx3ds_mp_fullview_cam_lever();
    }
    if (!sLatched) {
        return 0; /* the debug lever is read at the first Race_Draw */
    }
    return (sLeverK >= 0) && CamLocalLever();
}

/* One race-setting row's 2P fov mapped to the 1P one: base to base, and the speed widening
 * (base -> 120 over the same speed range in both tables) proportionally. */
static f32 CamMapFov(f32 fov2, s32 setting) {
    f32 base1;
    f32 base2;
    if (setting < 0 || setting >= CAMERA_RACE_SETTING_MAX) {
        setting = CAMERA_RACE_SETTING_REGULAR;
    }
    base1 = kRegularCameraRaceSettings[setting].fov;
    base2 = kWidescreenCameraRaceSettings[setting].fov;
    if (fov2 >= base2 && base2 < FV_CAM_FOV_MAX) {
        return base1 + ((fov2 - base2) * ((FV_CAM_FOV_MAX - base1) / (FV_CAM_FOV_MAX - base2)));
    }
    return (base2 > 0.0f) ? (fov2 * (base1 / base2)) : fov2;
}

void gdx_fv_cam_reframe(Camera* camera) {
    if (camera == NULL || !CamReframeOn(camera)) {
        return;
    }
    switch (camera->mode) {
        case CAMERA_MODE_RACE_INTRO:
            camera->fov = CamMapFov(camera->fov, CAMERA_RACE_SETTING_REGULAR);
            camera->frustrumCenterY += FV_CAM_CENTRE_1P - FV_CAM_CENTRE_2P;
            break;
        case CAMERA_MODE_SETUP_RACE:
        case CAMERA_MODE_RACE:
            camera->fov = CamMapFov(camera->fov, camera->raceSetting);
            camera->frustrumCenterY += FV_CAM_CENTRE_1P - FV_CAM_CENTRE_2P;
            break;
        default:
            /* falling off, finished, spectate: the 1P camera keeps the fov and a centred frustum
               (0); the 2P one shifts it down (-50 / -30) for the half-height strip. */
            if (camera->frustrumCenterY < 0.0f) {
                camera->frustrumCenterY = 0.0f;
            }
            break;
    }
    if (sCamReframes++ == 0 && &gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[fullview] camera: 2P framing -> 1P (fov %d centreY %d, view %d mode %d)\n",
                     (int) camera->fov, (int) camera->frustrumCenterY, (int) camera->id, (int) camera->mode);
    }
}

int gdx_fv_scissor(int scissorBoxType, int view) {
    if (sDrawK >= 0 && view == sDrawK) {
        return SCISSOR_BOX_FULL_SCREEN;
    }
    return scissorBoxType;
}

/* Camera_InitViewport's mirrors for SCISSOR_BOX_FULL_SCREEN, overscan removal included. */
static void FullScreenMirror(GdxFvMirror* m) {
    const Vp* vp = (const Vp*) gdx_segmented_to_host_pointer((uintptr_t) &aVpFullScreen);
    m->vpScaleX = vp->vp.vscale[0] * 0.25f;
    m->vpScaleY = vp->vp.vscale[1] * 0.25f;
    m->vpTransX = vp->vp.vtrans[0] * 0.25f;
    m->vpTransY = vp->vp.vtrans[1] * 0.25f;
    if (gdx_remove_borders()) {
        m->vpScaleX = 160.0f;
        m->vpScaleY = 120.0f;
        m->vpTransX = 160.0f;
        m->vpTransY = 120.0f;
    }
    m->scissorLeft = gScissorBoxFullScreen.left;
    m->scissorTop = gScissorBoxFullScreen.top;
    m->scissorRight = gScissorBoxFullScreen.right;
    m->scissorBottom = gScissorBoxFullScreen.bottom;
}

static void ReadMirror(const Camera* c, GdxFvMirror* m) {
    m->vpScaleX = c->currentVpScaleX;
    m->vpScaleY = c->currentVpScaleY;
    m->vpTransX = c->currentVpTransX;
    m->vpTransY = c->currentVpTransY;
    m->scissorLeft = c->currentScissorLeft;
    m->scissorTop = c->currentScissorTop;
    m->scissorRight = c->currentScissorRight;
    m->scissorBottom = c->currentScissorBottom;
}

static void WriteMirror(Camera* c, const GdxFvMirror* m) {
    c->currentVpScaleX = m->vpScaleX;
    c->currentVpScaleY = m->vpScaleY;
    c->currentVpTransX = m->vpTransX;
    c->currentVpTransY = m->vpTransY;
    c->currentScissorLeft = m->scissorLeft;
    c->currentScissorTop = m->scissorTop;
    c->currentScissorRight = m->scissorRight;
    c->currentScissorBottom = m->scissorBottom;
}

/* The race HUD tail of Racer_Draw reads these eight fields and writes none of them; nothing else
 * runs between begin and end, so the sim never observes the swap. */
void gdx_fv_rhud_begin(int view) {
    GdxFvMirror full;
    if (sDrawK < 0 || view != sDrawK || view < 0 || view >= 4) {
        return;
    }
    ReadMirror(&gCameras[view], &sSaved);
    FullScreenMirror(&full);
    WriteMirror(&gCameras[view], &full);
    sSwapView = view;
}

void gdx_fv_rhud_end(int view) {
    if (sSwapView < 0 || view != sSwapView) {
        return;
    }
    WriteMirror(&gCameras[view], &sSaved);
    sSwapView = -1;
}

/* A point a view's mirrors projected (Background_UpdateStars), moved to where the full-screen
 * viewport puts the same NDC: the projection is 4:3 at every player count, so this is exact. */
void gdx_fv_map_point(int view, f32* x, f32* y) {
    GdxFvMirror full;
    const Camera* c;
    if (sDrawK < 0 || view != sDrawK || view < 0 || view >= 4) {
        return;
    }
    c = &gCameras[view];
    if (c->currentVpScaleX == 0.0f || c->currentVpScaleY == 0.0f) {
        return;
    }
    FullScreenMirror(&full);
    *x = full.vpTransX + ((*x - c->currentVpTransX) * (full.vpScaleX / c->currentVpScaleX));
    *y = full.vpTransY + ((*y - c->currentVpTransY) * (full.vpScaleY / c->currentVpScaleY));
}

/* Audio (decomp-3ds-mp-fullview.patch, AudioThread_QueueCmd): in a VS race the game gives each
 * human his own channels on seqPlayer 0 -- engine 2+p, level SE 6+p, player SE 10+p -- panned
 * left / right by quadrant (Audio_SetPlayerMode, external.c). A fullview console is one player's
 * speaker: the local player's channels are centred and the other humans' are scaled down. The
 * game's own bookkeeping (sChannelVolume, the queued command count) never sees the change, and
 * audio is outside the simulation ([gstate] acmd is not part of the gate). */
#define FV_AUDIO_OP_SET_VOL_SCALE 0x01 /* AUDIOCMD_OP_CHANNEL_SET_VOL_SCALE (audiothread_cmd.h) */
#define FV_AUDIO_OP_SET_PAN 0x03       /* AUDIOCMD_OP_CHANNEL_SET_PAN */
#define FV_AUDIO_OP_SET_IO 0x06        /* AUDIOCMD_OP_CHANNEL_SET_IO ([fvse] counts port-0 writes: SE starts) */
#define FV_AUDIO_PAN_CENTRE 0x3F
#define FV_AUDIO_OTHER_SCALE 0.4f


#define FV_AUDIO_OP_INIT_SEQPLAYER 0x82    /* AUDIOCMD_OP_GLOBAL_INIT_SEQPLAYER */
#define FV_AUDIO_OP_DISABLE_SEQPLAYER 0x83 /* AUDIOCMD_OP_GLOBAL_DISABLE_SEQPLAYER */
#define FV_AUDIO_BGM_PLAYER 1              /* the BGM sequence player (ROM and disk) */
#define FV_BGM_RECEIPTS 24

/* MPFIX receipt: while an online race is being launched or run (gdx3ds_mpg_stage() != IDLE), every
 * start / stop of the BGM sequence player, so a log shows which music the race got (hw: the
 * menu's music played on over Mute City). Off a session: one compare per audio command. */
static void BgmReceipt(u32 op, u32 seqPlayer, u32 opArgs, void* data) {
    static s32 sBgmReceipts = 0;
    if (seqPlayer != FV_AUDIO_BGM_PLAYER || &gdx3ds_mpg_stage == NULL || gdx3ds_mpg_stage() == 0 ||
        &gdx_dbg_logf == NULL || sBgmReceipts >= FV_BGM_RECEIPTS) {
        return;
    }
    sBgmReceipts++;
    if (op == FV_AUDIO_OP_INIT_SEQPLAYER) {
        gdx_dbg_logf("[mpo] bgm: start seq=%u fade_in=%u (mpg stage %d)\n", (unsigned) ((opArgs >> 8) & 0xFF),
                     (unsigned) (uintptr_t) data, gdx3ds_mpg_stage());
    } else {
        gdx_dbg_logf("[mpo] bgm: stop fade_out=%u (mpg stage %d)\n", (unsigned) (uintptr_t) data,
                     gdx3ds_mpg_stage());
    }
}

/* [fvse] bookkeeping for one seq-player-0 command (see FvSeReceipt). */
static void FvSeCount(u32 op, u32 channel, u32 ioPort, s32 player, s32 muted) {
    if (op == FV_AUDIO_OP_SET_IO) {
        if (ioPort != 0) {
            return; /* port 5 = stop */
        }
        if (channel >= 2 && channel < 14) {
            sSeStart[(channel - 2) & 3][(channel - 2) >> 2]++;
        } else {
            sSeOtherStarts++;
        }
        sSeStartsTotal++;
        if ((sSeStartsTotal % FV_SE_RECEIPT_EVERY) == 0) {
            FvSeReceipt("tally");
        }
        return;
    }
    if (op == FV_AUDIO_OP_SET_VOL_SCALE && channel >= 6 && player >= 0) {
        if (muted) {
            sSeVolMuted[player]++;
        } else {
            sSeVolKept[player]++;
        }
    }
}

/* o3ds60 [fvse] race-end: the tally when an online race stops being driven (hw a3b4531: one
 * 2-player race made fewer than FV_SE_RECEIPT_EVERY starts after edge-on, so the log never said
 * whether this console played its SFX). Fullview's own edge-off needs a race draw after the race,
 * which never comes. */
int gdx3ds_mpo_local_player(void) __attribute__((weak));
static s32 sFvRaceLive = 0;

static void FvSeTallyReset(void) {
    s32 p;
    for (p = 0; p < 4; p++) {
        sSeStart[p][0] = 0;
        sSeStart[p][1] = 0;
        sSeStart[p][2] = 0;
        sSeVolKept[p] = 0;
        sSeVolMuted[p] = 0;
    }
    sSeOtherStarts = 0;
}

/* The tallies count one online race: cleared when it starts, printed when it ends ("race-end")
 * and again when the bottom screen opens its results ("results", gdx_fv_se_results_receipt). */
static void FvSeRaceWatch(void) {
    const s32 live = (&gdx3ds_mpo_local_player != NULL && gdx3ds_mpo_local_player() >= 0) ? 1 : 0;
    if (!sFvRaceLive && live) {
        FvSeTallyReset();
    } else if (sFvRaceLive && !live) {
        FvSeReceipt("race-end");
    }
    sFvRaceLive = live;
}

/* o3ds60: the bottom screen's RESULTS screen opened (gdx3ds_bui.c OpenResults). */
void gdx_fv_se_results_receipt(void) {
    FvSeReceipt("results");
}

void gdx_fv_audio_cmd(u32 opArgs, void** data) {
    const u32 op = (opArgs >> 24) & 0xFF;
    const u32 seqPlayer = (opArgs >> 16) & 0xFF;
    const u32 channel = (opArgs >> 8) & 0xFF;
    s32 k;
    s32 player;

    FvSeRaceWatch();

    if (op == FV_AUDIO_OP_INIT_SEQPLAYER || op == FV_AUDIO_OP_DISABLE_SEQPLAYER) {
        BgmReceipt(op, seqPlayer, opArgs, *data);
        return;
    }

    if (sLeverK < 0 && sMpOn == 0) {
        return;
    }
    if (seqPlayer != 0) {
        return;
    }
    if (op == FV_AUDIO_OP_SET_IO) {
        FvSeCount(op, channel, opArgs & 0xFF, -1, 0);
        return;
    }
    if ((op != FV_AUDIO_OP_SET_VOL_SCALE && op != FV_AUDIO_OP_SET_PAN) || channel < 2 || channel >= 14) {
        return;
    }
    k = gdx_fv_local_player(); /* mppolish: also through the results transition */
    if (k < 0) {
        return;
    }
    player = (s32) ((channel - 2) & 3);
    if (player >= gNumPlayers) {
        return;
    }
    if (op == FV_AUDIO_OP_SET_PAN) {
        if (player == k) {
            /* QueueCmdS8 stores the byte at offset 0 of the u32 payload under PORT. */
            u32 word = FV_AUDIO_PAN_CENTRE;
            *data = (void*) (uintptr_t) word;
            sAudioRewrites++;
        }
    } else if (player != k) {
        union {
            void* p;
            f32 f;
        } v;
        v.p = *data;
        /* mppolish: another human's engine stays audible (quieter); his level SE (6+p: reverse
           alarm, boost / slide loops) and player SE (10+p: low-energy alarm, finish, voice,
           hits) are his own console's cues, at [net] mp_other_se_pct (default 0). */
        v.f *= (channel >= 6) ? sOtherSeScale : FV_AUDIO_OTHER_SCALE;
        *data = v.p;
        sAudioRewrites++;
        FvSeCount(op, channel, 0, player, 1);
    } else {
        FvSeCount(op, channel, 0, player, 0);
    }
}
