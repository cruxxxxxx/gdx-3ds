/* port/3ds/net/gdx3ds_mp_game.c — mp-online: launch, pin and end an online VS race.
 * Contract: gdx3ds_mp_game.h. Decomp globals are declared here by hand (no decomp headers in the
 * net library, as in gdx3ds_mp.c); the two struct-typed ones go through port/decomp_port.c. */
#include "gdx3ds_mp_game.h"
#include "../gdx3ds_mp_pick.h"
#include "gdx_racemap.h"

#include <stdio.h>
#include <string.h>

/* decomp globals (types as defined in decomp/src). */
static int sResultPos[4]; /* the last race's RESULTS (gdx3ds_mpg_result) */
static int sResultMs[4];
static GdxRaceFieldSnapshot sLastField; /* gdx3ds_mpg_last_field */
extern int gGameMode;
extern int gQueuedGameMode;
extern short gGameModeChangeState;
extern short gMenuChangeMode;
extern signed char gTitleDemoState;
extern signed char gGamePaused;
extern int gNumPlayers;
extern int gCourseIndex;
extern int gCupType;
extern int gDifficulty;
extern int gTotalLapCount;
extern short gPlayerCharacters[4];
extern short gPlayerMachineSkins[4];
extern float gPlayerEngine[4];
extern short gSettingVsHandicap;
extern short gSettingVsCom;
extern int gSettingVsSlot;
extern int gVsRacePlayerVictoryCount[4];
extern int gVsRacePlayerPoints[4];
extern int gPlayerControlPorts[4];
extern int gRandSeed1;
extern unsigned int gRandMask1;
extern int gRandSeed2;
extern unsigned int gRandMask2;
extern unsigned int gGameFrameCount;
/* ovl_i3/menus.c: the pause menu (VS: Menus_DrawGeneralPause follows the pauser's pad only). */
extern int sPausePlayerNum;
extern int sPauseMenuOptionIndex;
extern int sPauseMenuScissorBoxTimer;
extern short sMenuIsBusy;
/* port/decomp_port.c */
extern int gdx_mpo_racer_done(int index);
extern void gdx_mpo_pin_cameras(void);
extern void gdx_mpo_pin_racers(int numPlayers);
extern void gdx_dbg_logf(const char* fmt, ...);
/* game/common.c: the stock "leave the menus for a race" BGM stop (ROM, beta and, on the
 * Expansion Kit, the 64DD disk BGM -- whichever is playing). */
extern void func_8007E0CC(void);

#define MODE_MASK 0x1F
#define MODE_VS_2P 3
#define MODE_MAIN_MENU 7
#define GAMEMODE_FLX_MAIN_MENU (MODE_MAIN_MENU | 0x8000)
#define GDX_MPG_HOP_SETTLE 2 /* polls on the main menu, in GAMEMODE_UPDATE, before the race is queued */
#define CHANGE_UPDATE 0
#define CHANGE_START 1
#define MENU_INACTIVE 0
#define MENU_QUIT 2

typedef struct SavedSettings {
    int numPlayers, courseIndex, cupType, difficulty, totalLaps;
    short characters[4], skins[4];
    float engine[4];
    short handicap, vsCom;
    int vsSlot;
    int victories[4], points[4];
    int ports[4];
} SavedSettings;

static int sStage = GDX_MPG_IDLE;
static GdxMpLbStart sStart;
static SavedSettings sSaved;
static int sHaveSaved = 0;
static int sLaunched = 0;     /* the VS mode was queued */
static int sPinned = 0;       /* the VS mode's init took the START's settings */
static int sGameBooted = 0;   /* a mode init has run (before it, Game_Init would undo a launch) */
static int sMenuQueued = 0;
static int sHopInit = 0;      /* HOP: the main menu's mode init has run */
static int sHopPolls = 0;   /* leaving: the main menu is queued (by us or by the game) */
static int sDoneAt = -1;      /* lockstep tick every human was done, -1 = not yet */
static int sEndAfter = 0;     /* test lever: end the race after this many ticks (0 = off) */
static int sQuitQueuedAt = -1;
static unsigned int sRemovedAt[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
static char sDescribe[48];
static int sEndedBy = -1;     /* mp-menus #10: the player whose pause-menu choice ended the race */
static int sEndedChoice = 0;  /* ... and the game's menu change mode it chose */

static int VsMode(void) {
    return MODE_VS_2P + (sStart.numPlayers - 2);
}

static int InVsMode(void) {
    return (gGameMode & MODE_MASK) == VsMode();
}

static void SaveSettings(void) {
    int i;
    sSaved.numPlayers = gNumPlayers;
    sSaved.courseIndex = gCourseIndex;
    sSaved.cupType = gCupType;
    sSaved.difficulty = gDifficulty;
    sSaved.totalLaps = gTotalLapCount;
    for (i = 0; i < 4; i++) {
        sSaved.characters[i] = gPlayerCharacters[i];
        sSaved.skins[i] = gPlayerMachineSkins[i];
        sSaved.engine[i] = gPlayerEngine[i];
        sSaved.victories[i] = gVsRacePlayerVictoryCount[i];
        sSaved.points[i] = gVsRacePlayerPoints[i];
        sSaved.ports[i] = gPlayerControlPorts[i];
    }
    sSaved.handicap = gSettingVsHandicap;
    sSaved.vsCom = gSettingVsCom;
    sSaved.vsSlot = gSettingVsSlot;
    sHaveSaved = 1;
}

/* mppolish (hw: "the client (old 3ds) doesnt play any sfx"): the audio driver's player mode
 * (external.c sAudioPlayerMode: 0 = 1P, n-1 = n-player VS) is set only by the main menu's A press
 * (main_menu.c, Audio_SetPlayerMode(gNumPlayers - 1)). An online race is pinned past that, so it
 * ran in the 1P mode: every trigger SE was routed for racer 0 alone (channel 10; the other
 * humans' only through the far-enemy channel 15, nearly never) -- so player 2's own boost, hits
 * and voice never played, on any console, and the fullview mixer then muted the host's on the
 * guest. Audio only: nothing the simulation reads. */
extern void Audio_SetPlayerMode(unsigned char numPlayersIndex);

static void SetAudioPlayers(int n, const char* why) {
    const int mode = (n >= 1 && n <= 4) ? (n - 1) : 0;
    Audio_SetPlayerMode((unsigned char) mode);
    gdx_dbg_logf("[mpo] audio: player mode %d (%s)\n", mode, why);
}

static void RestoreSettings(void) {
    int i;
    if (!sHaveSaved) {
        return;
    }
    gNumPlayers = sSaved.numPlayers;
    gCourseIndex = sSaved.courseIndex;
    gCupType = sSaved.cupType;
    gDifficulty = sSaved.difficulty;
    gTotalLapCount = sSaved.totalLaps;
    for (i = 0; i < 4; i++) {
        gPlayerCharacters[i] = sSaved.characters[i];
        gPlayerMachineSkins[i] = sSaved.skins[i];
        gPlayerEngine[i] = sSaved.engine[i];
        gVsRacePlayerVictoryCount[i] = sSaved.victories[i];
        gVsRacePlayerPoints[i] = sSaved.points[i];
        gPlayerControlPorts[i] = sSaved.ports[i];
    }
    gSettingVsHandicap = sSaved.handicap;
    gSettingVsCom = sSaved.vsCom;
    gSettingVsSlot = sSaved.vsSlot;
    gdx3ds_mpp_race_machines(0); /* the Expansion Kit's custom machines come back */
    SetAudioPlayers(gNumPlayers, "user");
    sHaveSaved = 0;
    gdx_dbg_logf("[mpo] game: the user's settings are back (players=%d course=%d)\n", gNumPlayers, gCourseIndex);
}

static void PinRace(void) {
    int i;
    const int n = sStart.numPlayers;
    gNumPlayers = n;
    gdx3ds_mpp_pin_course(sStart.course); /* gCourseIndex, its cup (EK: the DD cups), the track name */
    gTotalLapCount = sStart.laps;
    gDifficulty = sStart.difficulty;
    gSettingVsCom = (short) sStart.vsCom;
    gSettingVsHandicap = (short) sStart.handicap;
    gSettingVsSlot = sStart.vsSlot;
    for (i = 0; i < 4; i++) {
        const int p = (i < n) ? i : 0;
        gPlayerCharacters[i] = (short) sStart.player[p].machine;
        gPlayerMachineSkins[i] = (short) sStart.player[p].skin;
        gPlayerEngine[i] = gdx_mpo_engine_from_bits(sStart.player[p].engineBits); /* the exact slider */
        gVsRacePlayerVictoryCount[i] = 0;
        gVsRacePlayerPoints[i] = 0;
        gPlayerControlPorts[i] = (i < n) ? i : 4; /* PORT_DISCONNECTED */
    }
    gRandSeed1 = (int) sStart.seed1;
    gRandMask1 = sStart.mask1;
    gRandSeed2 = (int) sStart.seed2;
    gRandMask2 = sStart.mask2;
    gGameFrameCount = 0;
    gTitleDemoState = 0;
    gGamePaused = 0;
    gdx_mpo_pin_cameras();
    gdx3ds_mpp_race_machines(1); /* stock machines on every console (EK: no per-save customs) */
    gdx_mpo_pin_racers(n);
    SetAudioPlayers(n, "race");
}

/* n64_sched.c gdx_transition_step_mark, every checkpoint of a mode change. */
void gdx3ds_mp_mode_init_hook(const char* label) {
    if (label == NULL || label[0] != 'G' || label[4] != 'B' || label[7] != 'G' || label[11] != 'C') {
        return;
    }
    sGameBooted = 1; /* the game has initialised a mode of its own: a launch can now stick */
    gdx3ds_mpp_mode_init();
    if (sStage == GDX_MPG_IDLE) {
        return;
    }
    if (sStage == GDX_MPG_HOP) {
        if ((gGameMode & MODE_MASK) == MODE_MAIN_MENU) {
            sHopInit = 1;
        } else {
            sStage = GDX_MPG_QUEUE; /* something else won the change: hop again */
        }
        return;
    }
    if (sStage == GDX_MPG_WAIT_INIT && !InVsMode()) {
        /* Something else won the mode change we queued (the boot's own title, a menu choice
         * landing in the same tick): queue the race again. */
        sStage = GDX_MPG_QUEUE;
        sHopInit = 0; /* from the main menu again, as every console does */
        gdx_dbg_logf("[mpo] game: mode %d initialised instead of the race: queuing it again\n", gGameMode);
        return;
    }
    if (sStage == GDX_MPG_LEAVING && sLaunched && !sPinned && InVsMode()) {
        /* Aborted while the VS mode was already on its way in: let it init with a consistent
         * set of settings (never the user's 1P ones in a VS mode), then leave from there. */
        PinRace();
        sPinned = 1;
        return;
    }
    if (sStage == GDX_MPG_WAIT_INIT && InVsMode()) {
        PinRace();
        sPinned = 1;
        sStage = GDX_MPG_PINNED;
        gdx_dbg_logf("[mpo] game: pinned %s laps=%d (seeds %08x/%08x)\n", sDescribe, (int) sStart.laps,
                     (unsigned) sStart.seed1, (unsigned) sStart.seed2);
        return;
    }
    if (sStage == GDX_MPG_LEAVING && !InVsMode()) {
        RestoreSettings();
        sStage = GDX_MPG_IDLE;
    }
}

void gdx3ds_mpg_arm(const GdxMpLbStart* start, int endAfterTicks) {
    static const char* const kCourse[24] = {
        "Mute City", "Silence", "Sand Ocean", "Devil's Forest", "Big Blue", "Port Town",
        "Sector Alpha", "Red Canyon", "Devil's Forest 2", "Mute City 2", "Big Blue 2", "White Land",
        "Fire Field", "Silence 2", "Sector Beta", "Red Canyon 2", "White Land 2", "Mute City 3",
        "Rainbow Road", "Devil's Forest 3", "Space Plant", "Sand Ocean 2", "Port Town 2", "Big Hand",
    };
    if (sStage == GDX_MPG_LEAVING || sStage == GDX_MPG_IDLE) {
        if (!sHaveSaved) {
            SaveSettings();
        }
    }
    sStart = *start;
    memset(sResultPos, 0, sizeof(sResultPos));
    memset(sResultMs, 0, sizeof(sResultMs));
    memset(&sLastField, 0, sizeof(sLastField));
    sStage = GDX_MPG_QUEUE;
    sLaunched = 0;
    sPinned = 0;
    sMenuQueued = 0;
    sHopInit = 0;
    sHopPolls = 0;
    sDoneAt = -1;
    sRemovedAt[0] = sRemovedAt[1] = sRemovedAt[2] = sRemovedAt[3] = 0xFFFFFFFFu;
    sQuitQueuedAt = -1;
    sEndedBy = -1;
    sEndedChoice = 0;
    sEndAfter = endAfterTicks > 0 ? endAfterTicks : 0;
    if (start->vsCom && start->racers > 4) {
        snprintf(sDescribe, sizeof(sDescribe), "VS %dP+%d / %s", start->numPlayers,
                 start->racers - start->numPlayers, start->course < 24 ? kCourse[start->course] : "a disk course");
    } else {
        snprintf(sDescribe, sizeof(sDescribe), "VS %dP / %s", start->numPlayers,
                 start->course < 24 ? kCourse[start->course] : "a disk course");
    }
}

static int Settled(void) {
    return sGameBooted && gGameModeChangeState == CHANGE_UPDATE && gGameMode >= 0;
}

static void QueueHop(void) {
    gTitleDemoState = 0;
    gMenuChangeMode = MENU_INACTIVE;
    gQueuedGameMode = GAMEMODE_FLX_MAIN_MENU;
    gGameModeChangeState = CHANGE_START;
    sHopInit = 0;
    sHopPolls = 0;
    sStage = GDX_MPG_HOP;
    gdx_dbg_logf("[mpo] game: via the main menu (from mode %d)\n", gGameMode);
}

int gdx3ds_mpg_launch_poll(void) {
    if (sStage == GDX_MPG_HOP) {
        if (!sHopInit || !Settled() || (gGameMode & MODE_MASK) != MODE_MAIN_MENU) {
            return 0;
        }
        if (++sHopPolls < GDX_MPG_HOP_SETTLE) {
            return 0;
        }
        sStage = GDX_MPG_QUEUE;
        sHopInit = 2; /* hopped: queue the race itself now */
    }
    if (sStage == GDX_MPG_QUEUE) {
        if (!Settled()) {
            return 0; /* still booting, or mid-transition: wait for the mode change to land */
        }
        if (sHopInit != 2) {
            QueueHop();
            return 0;
        }
        gTitleDemoState = 0;
        gMenuChangeMode = MENU_INACTIVE;
        /* MPFIX (hw: the menu music played on over Mute City): a stock VS race is entered from
         * machine settings' OK, which stops the menu BGM (ovl_i4/machine.c, func_8007E0CC) before
         * returning the VS mode; the race's mode init only READIES its own BGM
         * (Audio_DDBgmReady / func_80068DCC returns for race modes). Launching from the main
         * menu skipped that stop, so the select BGM kept playing and the course BGM never took
         * over. Same call, same place in the sequence. Audio only: the lockstep is untouched. */
        func_8007E0CC();
        gQueuedGameMode = VsMode();
        gGameModeChangeState = CHANGE_START; /* re-inits even when that VS mode is running now */
        sLaunched = 1;
        sStage = GDX_MPG_WAIT_INIT;
        gdx_dbg_logf("[mpo] game: launch %s from mode %d (menu BGM stopped)\n", sDescribe, gGameMode);
        return 0;
    }
    if (sStage == GDX_MPG_PINNED && gGameModeChangeState == CHANGE_UPDATE && InVsMode()) {
        sStage = GDX_MPG_RACING;
        return 1;
    }
    return 0;
}

static void QueueMainMenu(void) {
    gMenuChangeMode = MENU_INACTIVE;
    gQueuedGameMode = GAMEMODE_FLX_MAIN_MENU;
    gGameModeChangeState = CHANGE_START;
    sMenuQueued = 1;
}

extern int gdx_mpo_racer_result(int index, int* raceTimeMs);

static void NoteResults(void) {
    int i;
    for (i = 0; i < sStart.numPlayers && i < 4; i++) {
        sResultPos[i] = gdx_mpo_racer_result(i, &sResultMs[i]);
    }
}

/* Read-only (no game state changes): the field in race order, for the results screen. */
static void NoteField(void) {
    GdxRaceFieldSnapshot f;
    gdx_racefield_snapshot(&f, 0);
    if (f.valid) {
        sLastField = f;
    }
}

const struct GdxRaceFieldSnapshot* gdx3ds_mpg_last_field(void) {
    return &sLastField;
}

int gdx3ds_mpg_result(int player, int* raceTimeMs) {
    if (player < 0 || player >= 4 || player >= sStart.numPlayers) {
        return 0;
    }
    if (raceTimeMs != NULL) {
        *raceTimeMs = sResultMs[player];
    }
    return sResultPos[player];
}

static int AllHumansDone(unsigned int lt) {
    int i;
    for (i = 0; i < sStart.numPlayers; i++) {
        const int removed = (i < 4) && sRemovedAt[i] != 0xFFFFFFFFu && lt >= sRemovedAt[i];
        if (!removed && !gdx_mpo_racer_done(i)) {
            return 0;
        }
    }
    return 1;
}

void gdx3ds_mpg_set_removed(int player, unsigned int tick) {
    if (player >= 0 && player < 4) {
        sRemovedAt[player] = tick;
    }
}

/* Pause -> quit / retry / settings / change machine / change course: every console sees the same
 * choice at the same tick (the pads are the same) and all of them end the race the same way --
 * back to the room. Who chose it is the pauser (only its pad drives the pause menu). */
static void NoteMenuChoice(unsigned int lt) {
    sEndedBy = gGamePaused ? sPausePlayerNum : -1;
    sEndedChoice = (int) gMenuChangeMode;
    gdx_dbg_logf("[mpo] game: race menu choice %d at tick %u by p%d -> the race ends for everyone\n",
                 (int) gMenuChangeMode, lt, sEndedBy);
    gMenuChangeMode = MENU_QUIT;
}

int gdx3ds_mpg_ended_by(int* choice) {
    if (choice != NULL) {
        *choice = sEndedChoice;
    }
    return sEndedBy;
}

/* The VS pause menu's rows (decomp menus.h PauseGeneralOptions). */
static const char* PauseOptionName(int option) {
    static const char* const kNames[6] = { "CONTINUE", "RETRY", "SETTINGS", "CHANGE MACHINE", "CHANGE COURSE",
                                           "QUIT" };
    return (option >= 0 && option < 6) ? kNames[option] : "";
}

int gdx3ds_mpg_pause(GdxMpgPause* out) {
    memset(out, 0, sizeof(*out));
    out->player = -1;
    if (sStage != GDX_MPG_RACING || !InVsMode() || !gGamePaused) {
        return 0;
    }
    out->paused = 1;
    out->player = sPausePlayerNum;
    out->option = sPauseMenuOptionIndex;
    out->ready = sPauseMenuScissorBoxTimer == 0 && !sMenuIsBusy;
    out->endsRace = sPauseMenuOptionIndex != 0;
    out->optionName = PauseOptionName(sPauseMenuOptionIndex);
    return 1;
}

int gdx3ds_mpg_race_poll(unsigned int lt) {
    if (sStage != GDX_MPG_RACING) {
        return 1;
    }
    if (!InVsMode() || gGameModeChangeState != CHANGE_UPDATE || gQueuedGameMode != gGameMode) {
        gdx_dbg_logf("[mpo] game: the race is over at tick %u (mode %d -> %d, change %d)\n", lt, gGameMode,
                     gQueuedGameMode, (int) gGameModeChangeState);
        return 1;
    }
    if (gMenuChangeMode != MENU_INACTIVE) {
        NoteMenuChoice(lt);
        return 0;
    }
    NoteResults();
    NoteField();
    if (sDoneAt < 0 && AllHumansDone(lt)) {
        sDoneAt = (int) lt;
        gdx_dbg_logf("[mpo] game: every player is done at tick %u (laps=%d)\n", lt, (int) sStart.laps);
    }
    if (sQuitQueuedAt < 0 && ((sDoneAt >= 0 && (int) lt >= sDoneAt + GDX_MPG_END_HOLD) ||
                              (sEndAfter > 0 && (int) lt >= sEndAfter))) {
        QueueMainMenu();
        sQuitQueuedAt = (int) lt;
        gdx_dbg_logf("[mpo] game: main menu queued at tick %u%s\n", lt, sDoneAt >= 0 ? "" : " (mp_end_after)");
    }
    return 0;
}

void gdx3ds_mpg_leave(void) {
    if (sStage == GDX_MPG_IDLE || sStage == GDX_MPG_LEAVING) {
        return;
    }
    if (!sLaunched) {
        RestoreSettings(); /* never got as far as queuing the race */
        sStage = GDX_MPG_IDLE;
        return;
    }
    sStage = GDX_MPG_LEAVING;
}

void gdx3ds_mpg_frame(void) {
    if (sStage != GDX_MPG_LEAVING) {
        return;
    }
    if (InVsMode()) {
        if (gGameModeChangeState != CHANGE_UPDATE || gQueuedGameMode != gGameMode) {
            sMenuQueued = 1; /* already on its way out (the race's own quit, or ours) */
        } else if (!sMenuQueued) {
            QueueMainMenu();
        }
        return;
    }
    if (gGameModeChangeState == CHANGE_UPDATE) {
        /* Out of the VS mode and settled without passing our mode-init hook (cannot happen
         * through the menus, but never stay stuck in LEAVING). */
        RestoreSettings();
        sStage = GDX_MPG_IDLE;
    }
}

int gdx3ds_mpg_stage(void) {
    return sStage;
}

int gdx3ds_mpg_ports(void) {
    if (sStage == GDX_MPG_IDLE || sStage == GDX_MPG_LEAVING) {
        return 0;
    }
    return sStart.numPlayers;
}

const char* gdx3ds_mpg_describe(void) {
    return sDescribe;
}

/* MP30: the VS COM machine count Race_Init fills to (gdx3ds_autodrive.c gdx_mp_vs_total_racers):
 * the START's, once the race is pinned; 0 = stock. */
int gdx3ds_mpg_vs_racers(void) {
    if (!sPinned || !InVsMode() || !sStart.vsCom) {
        return 0;
    }
    return sStart.racers;
}

int gdx3ds_mpo_bot_racer(int racerId) {
    if (sStage != GDX_MPG_RACING && sStage != GDX_MPG_PINNED) {
        return 0;
    }
    return racerId >= 0 && racerId < sStart.numPlayers && (sStart.botMask & (1u << racerId)) != 0;
}

/* MP30: a test bot in ram mode ([net] mp_bot = 2; gdx3ds_autodrive.c MpRamBot). */
int gdx3ds_mpo_ram_racer(int racerId) {
    if (!gdx3ds_mpo_bot_racer(racerId)) {
        return 0;
    }
    return (sStart.ramMask & (1u << racerId)) != 0;
}

void gdx3ds_mpg_current_picks(int* machine, int* skin, int* engine) {
    int m = gPlayerCharacters[0];
    int s = gPlayerMachineSkins[0];
    int e = (int) (gPlayerEngine[0] * 100.0f + 0.5f);
    *machine = (m >= 0 && m <= GDX_MPO_MAX_MACHINE) ? m : 0;
    *skin = (s >= 0 && s <= 3) ? s : 0;
    *engine = (e >= 0 && e <= 100) ? e : 50;
}

void gdx3ds_mpg_current_settings(GdxMpLobbySettings* out) {
    memset(out, 0, sizeof(*out));
    out->course = (uint8_t) ((gCourseIndex >= 0 && gCourseIndex <= GDX_MPO_MAX_COURSE) ? gCourseIndex : 0);
    out->vsCom = (uint8_t) (gSettingVsCom ? 1 : 0);
    out->handicap = (uint8_t) ((gSettingVsHandicap >= 0 && gSettingVsHandicap <= 2) ? gSettingVsHandicap : 0);
    out->vsSlot = (uint8_t) (gSettingVsSlot ? 1 : 0);
    out->difficulty = (uint8_t) ((gDifficulty >= 0 && gDifficulty <= 3) ? gDifficulty : 0);
    out->hashEvery = 1;
}
