/* port/3ds/game/gdx3ds_mp_pick_game.c — multiplayer: the stock 1P menus as each player's picker.
 * Contract and rationale: ../gdx3ds_mp_pick.h. The decomp side is decomp-3ds-mp-stockpick.patch
 * (gdx_mp_pick_active / gdx_mp_pick_filter / gdx_mp_pick_course_refused, defined here). */
#include "global.h"
#include "fzx_game.h"
#include "fzx_racer.h"
#include "src/overlays/ovl_i4/ovl_i4.h"

#include "../gdx3ds_mp_pick.h"

/* decomp globals (types as defined in decomp/src). */
extern s32 gGameMode;
extern s32 gQueuedGameMode;
extern s16 gGameModeChangeState;
extern s16 gMenuChangeMode;
extern s8 gTitleDemoState;
extern s32 gSelectedMode;
extern s32 gNumPlayers;
extern s32 gCourseIndex;
extern s32 gCupType;
extern s32 gDifficulty;
extern s16 gPlayerCharacters[4];
extern s16 gPlayerMachineSkins[4];
extern f32 gPlayerEngine[4];
extern f32 gCharacterLastEngine[30];
extern s32 gLastCourseSelectCourseIndex;
extern char* gCurrentTrackName;
extern char* gTrackNames[55];
extern s32 gMachineSelectState;
extern Machine gMachines[30];
#ifdef EXPANSION_KIT
extern CustomMachinesInfo gCustomMachinesInfo;
extern s32 gLeoDriveConnectionState;
extern void func_8008D33C(void);
#endif
/* port/gdx_mp_online.c */
extern int gdx_mpo_course_raceable(int course);
extern uint32_t gdx_mpo_stock_course_key(int course);
extern void gdx_dbg_logf(const char* fmt, ...);

#define PICK_MACHINES 30
#define PICK_RACERS 4
#define PICK_FIELD 30 /* TOTAL_RACER_COUNT: an MP30 VS COM field */
#define REFUSED_FRAMES 180 /* the bottom screen's "can't race that course" note: ~3 s */
#define FIRST_DD_COURSE 30
#define LAST_DD_COURSE 41

typedef enum PickStage {
    STAGE_IDLE = 0,
    STAGE_HOP,      /* the main menu is (to be) queued: the picker is entered from there */
    STAGE_ENTER,    /* the picker's mode is queued: waiting for its init */
    STAGE_ACTIVE,   /* in a picker, the player is picking */
    STAGE_FROZEN,   /* the pick is confirmed: the confirmed screen stays */
    STAGE_LEAVING,  /* the main menu is (to be) queued: everything comes back at its init */
    STAGE_RELEASED, /* a race launches: settings already back; the save guarded until we are out */
} PickStage;

typedef struct UserSettings {
    s32 selectedMode, numPlayers, courseIndex, cupType, difficulty, lastCourseSelect;
    s16 characters[PICK_RACERS], skins[PICK_RACERS];
    f32 engine[PICK_RACERS];
    f32 lastEngine[PICK_MACHINES];
    char* trackName;
    s8 racerCharacter[PICK_RACERS];
    s8 racerSkin[PICK_RACERS];
    f32 racerEngineFactor[PICK_RACERS];
} UserSettings;

typedef struct PickSeed {
    int machine, skin, course;
    float engine;
} PickSeed;

static PickStage sStage = STAGE_IDLE;
static int sKind = GDX_MPP_NONE;   /* the picker the top screen is in (or on its way to) */
static int sWant = GDX_MPP_NONE;   /* a picker to enter once the game settles */
static int sQueued = 0;            /* HOP / LEAVING: our mode change is queued */
static int sHopLanded = 0;         /* HOP: the main menu's init has run */
static int sBooted = 0;            /* a mode init has run: Game_Init can no longer undo ours */
static PickSeed sSeed;
static UserSettings sUser;
static int sHaveUser = 0;
static int sMachineReady = 0;
static int sCourseReady = 0;
static PickSeed sResult;
static unsigned int sFrame = 0;
static unsigned int sRefusedAt = 0;
static int sRefused = 0;
/* FROZEN by the machine step's timer: the screen it froze on still takes input (no confirm froze
 * it), so the game's buttons and stick are held back until the next screen (the bui key mask). */
static volatile int sTimedOutFreeze = 0;

/* ---- the user's settings ------------------------------------------------------------------- */

static void SaveUser(void) {
    int i;
    sUser.selectedMode = gSelectedMode;
    sUser.numPlayers = gNumPlayers;
    sUser.courseIndex = gCourseIndex;
    sUser.cupType = gCupType;
    sUser.difficulty = gDifficulty;
    sUser.lastCourseSelect = gLastCourseSelectCourseIndex;
    sUser.trackName = gCurrentTrackName;
    for (i = 0; i < PICK_RACERS; i++) {
        sUser.characters[i] = gPlayerCharacters[i];
        sUser.skins[i] = gPlayerMachineSkins[i];
        sUser.engine[i] = gPlayerEngine[i];
        sUser.racerCharacter[i] = gRacers[i].character;
        sUser.racerSkin[i] = gRacers[i].machineSkinIndex;
        sUser.racerEngineFactor[i] = gRacers[i].unk_1A8;
    }
    for (i = 0; i < PICK_MACHINES; i++) {
        sUser.lastEngine[i] = gCharacterLastEngine[i];
    }
    sHaveUser = 1;
}

/* Everything but the racer table: what the race launch saves next (gdx3ds_mp_game.c). */
static void RestoreUserSettings(void) {
    int i;
    gSelectedMode = sUser.selectedMode;
    gNumPlayers = sUser.numPlayers;
    gCourseIndex = sUser.courseIndex;
    gCupType = sUser.cupType;
    gDifficulty = sUser.difficulty;
    gLastCourseSelectCourseIndex = sUser.lastCourseSelect;
    /* NULL until the player's first course select: keep the pinned name, the frozen course select
     * screen still draws it (a NULL read faults on hardware, the emulator reads zeros). */
    if (sUser.trackName != NULL) {
        gCurrentTrackName = sUser.trackName;
    }
    for (i = 0; i < PICK_RACERS; i++) {
        gPlayerCharacters[i] = sUser.characters[i];
        gPlayerMachineSkins[i] = sUser.skins[i];
        gPlayerEngine[i] = sUser.engine[i];
    }
    for (i = 0; i < PICK_MACHINES; i++) {
        gCharacterLastEngine[i] = sUser.lastEngine[i];
    }
}

/* The racer table last: the confirmed screen draws from it until the screen changes. */
static void RestoreUserRacers(void) {
    int i;
    for (i = 0; i < PICK_RACERS; i++) {
        gRacers[i].character = sUser.racerCharacter[i];
        gRacers[i].machineSkinIndex = sUser.racerSkin[i];
        gRacers[i].unk_1A8 = sUser.racerEngineFactor[i];
    }
}

/* ---- Expansion Kit machines ------------------------------------------------------------------ */

static int sStockRefs = 0;
#ifdef EXPANSION_KIT
static s8 sCustomState[PICK_MACHINES];
#endif

static void StockMachinesAcquire(void) {
#ifdef EXPANSION_KIT
    int i;
    if (sStockRefs++ > 0) {
        return;
    }
    for (i = 0; i < PICK_MACHINES; i++) {
        sCustomState[i] = gCustomMachinesInfo.characterCustomState[i];
        gCustomMachinesInfo.characterCustomState[i] = 0;
    }
    func_8008D33C();
#else
    sStockRefs++;
#endif
}

static void StockMachinesRelease(void) {
#ifdef EXPANSION_KIT
    int i;
    if (sStockRefs <= 0 || --sStockRefs > 0) {
        return;
    }
    for (i = 0; i < PICK_MACHINES; i++) {
        gCustomMachinesInfo.characterCustomState[i] = sCustomState[i];
    }
    func_8008D33C();
#else
    if (sStockRefs > 0) {
        sStockRefs--;
    }
#endif
}

static int sRaceHoldsStock = 0;

void gdx3ds_mpp_race_machines(int on) {
    if (on && !sRaceHoldsStock) {
        sRaceHoldsStock = 1;
        StockMachinesAcquire();
    } else if (!on && sRaceHoldsStock) {
        sRaceHoldsStock = 0;
        StockMachinesRelease();
    }
}

/* ---- the picker's globals -------------------------------------------------------------------- */

static void SeedMachine(void) {
    int i;
    gRacers[0].character = (s8) sSeed.machine;
    gRacers[0].machineSkinIndex = (s8) sSeed.skin;
    gPlayerCharacters[0] = (s16) sSeed.machine;
    gPlayerMachineSkins[0] = (s16) sSeed.skin;
    gPlayerEngine[0] = sSeed.engine;
    for (i = 0; i < PICK_MACHINES; i++) {
        gCharacterLastEngine[i] = 0.5f; /* the slider's centre for every other machine */
    }
    gCharacterLastEngine[sSeed.machine] = sSeed.engine; /* MachineSettings_Init reads it (the patch) */
}

/* The 1P flow's own entry state: practice, one player. */
static void SetPickGlobals(void) {
    gSelectedMode = MODE_PRACTICE;
    gNumPlayers = 1;
    if (sWant == GDX_MPP_MACHINE) {
        SeedMachine();
    } else {
        gLastCourseSelectCourseIndex = sSeed.course;
    }
}

/* ---- stages -------------------------------------------------------------------------------- */

static const char* const kStageNames[] = { "idle", "hop", "enter", "active", "frozen", "leaving", "released" };

static void SetStage(PickStage stage) {
    if (stage != sStage) {
        gdx_dbg_logf("[mpp] %s -> %s (mode %d, kind %d)\n", kStageNames[sStage], kStageNames[stage], gGameMode,
                     sKind);
    }
    sStage = stage;
    if (stage != STAGE_FROZEN) {
        sTimedOutFreeze = 0;
    }
}

static int Settled(void) {
    return sBooted && gGameModeChangeState == GAMEMODE_UPDATE && gGameMode >= 0 && gGameMode == gQueuedGameMode &&
           gMenuChangeMode == MENU_CHANGE_INACTIVE;
}

static int IsPickerMode(s32 mode) {
    const s32 m = GET_MODE(mode);
    return m == GAMEMODE_MACHINE_SELECT || m == GAMEMODE_MACHINE_SETTINGS || m == GAMEMODE_COURSE_SELECT;
}

static void QueueMode(s32 mode) {
    gTitleDemoState = TITLE_DEMO_INACTIVE;
    gMenuChangeMode = MENU_CHANGE_INACTIVE;
    gQueuedGameMode = mode;
    gGameModeChangeState = GAMEMODE_CHANGE_START;
}

static void QueuePicker(void) {
    SetPickGlobals();
    sKind = sWant;
    sWant = GDX_MPP_NONE;
    QueueMode(sKind == GDX_MPP_COURSE ? GAMEMODE_FLX_COURSE_SELECT : GAMEMODE_FLX_MACHINE_SELECT);
    SetStage(STAGE_ENTER);
}

static void Finish(void) {
    if (sHaveUser) {
        RestoreUserSettings();
        RestoreUserRacers();
        sHaveUser = 0;
    }
    StockMachinesRelease();
    sKind = GDX_MPP_NONE;
    sWant = GDX_MPP_NONE;
    SetStage(STAGE_IDLE);
}

static uint32_t FloatBits(float f) {
    uint32_t u;
    __builtin_memcpy(&u, &f, sizeof(u));
    return u;
}

static void RecordMachine(int machine, int skin, float engine) {
    sResult.machine = (machine >= 0 && machine < PICK_MACHINES) ? machine : 0;
    sResult.skin = (skin >= 0 && skin <= 3) ? skin : 0;
    sResult.engine = (engine >= 0.0f && engine <= 1.0f) ? engine : 0.5f;
    sMachineReady = 1;
    gdx_dbg_logf("[mpp] machine pick: machine=%d skin=%d engine=%08lx\n", sResult.machine, sResult.skin,
                 (unsigned long) FloatBits(sResult.engine));
}

static void RecordCourse(int course) {
    sResult.course = course;
    sCourseReady = 1;
    gdx_dbg_logf("[mpp] course pick: %d\n", course);
}

static void BeginLeaving(void) {
    sQueued = 0;
    sWant = GDX_MPP_NONE;
    SetStage(STAGE_LEAVING);
}

/* ---- API ------------------------------------------------------------------------------------ */

int gdx3ds_mpp_begin(int kind, int machine, int skin, float engine, int course) {
    if (!sBooted || (kind != GDX_MPP_MACHINE && kind != GDX_MPP_COURSE) || sStage == STAGE_RELEASED) {
        return 0;
    }
    sSeed.machine = (machine >= 0 && machine < PICK_MACHINES) ? machine : 0;
    sSeed.skin = (skin >= 0 && skin <= 3) ? skin : 0;
    sSeed.engine = (engine >= 0.0f && engine <= 1.0f) ? engine : 0.5f;
    sSeed.course = (course >= 0 && course <= LAST_DD_COURSE) ? course : 0;
    if (kind == GDX_MPP_MACHINE) {
        sMachineReady = 0;
    } else {
        sCourseReady = 0;
    }
    sWant = kind;
    if (sStage == STAGE_IDLE) {
        SaveUser();
        StockMachinesAcquire();
        sQueued = 0;
        sHopLanded = 0;
        SetStage(STAGE_HOP);
    } else if (sStage == STAGE_LEAVING) {
        sQueued = 0;
        sHopLanded = 0;
        SetStage(STAGE_HOP); /* still in pick mode: enter the new picker from the main menu */
    }
    gdx_dbg_logf("[mpp] begin kind=%d machine=%d skin=%d course=%d (stage %s, mode %d)\n", kind, sSeed.machine,
                 sSeed.skin, sSeed.course, kStageNames[sStage], gGameMode);
    return 1;
}

int gdx3ds_mpp_active(void) {
    return sStage == STAGE_ACTIVE ? sKind : GDX_MPP_NONE;
}

int gdx3ds_mpp_holding(void) {
    return sStage != STAGE_IDLE && sStage != STAGE_RELEASED;
}

int gdx3ds_mpp_take_machine(int* machine, int* skin, float* engine) {
    if (!sMachineReady) {
        return 0;
    }
    sMachineReady = 0;
    *machine = sResult.machine;
    *skin = sResult.skin;
    *engine = sResult.engine;
    return 1;
}

int gdx3ds_mpp_take_course(int* course) {
    if (!sCourseReady) {
        return 0;
    }
    sCourseReady = 0;
    *course = sResult.course;
    return 1;
}

/* The engine the screen shows for the cursor's machine: SETTINGS' live slider, or (MACHINE
 * SELECT) the value SETTINGS would open on. */
static float CursorEngine(int machine) {
    if (GET_MODE(gGameMode) == GAMEMODE_MACHINE_SETTINGS) {
        return gPlayerEngine[0];
    }
    return (machine >= 0 && machine < PICK_MACHINES) ? gCharacterLastEngine[machine] : 0.5f;
}

/* The cursor's pick as the screen shows it: MACHINE SELECT and SETTINGS keep gRacers[0] on the
 * cursor's machine and colour; the engine from CursorEngine. */
static void RecordCursorPick(void) {
    const int machine = gRacers[0].character;
    RecordMachine(machine, gRacers[0].machineSkinIndex, CursorEngine(machine));
    gdx_dbg_logf("[mpp] machine step timed out: the cursor's machine, colour and engine are the pick\n");
}

/* The highlighted course when it can be raced here, else the one the picker opened on. */
static int CourseOrSeed(int course) {
    if (course >= 0 && gdx_mpo_course_raceable(course) && gdx3ds_mpp_course_key(course) != 0) {
        return course;
    }
    return sSeed.course;
}

void gdx3ds_mpp_timeout(void) {
    const int picking = (sKind == GDX_MPP_MACHINE || sWant == GDX_MPP_MACHINE);
    if (!picking || sStage == STAGE_IDLE || sStage == STAGE_FROZEN || sStage == STAGE_RELEASED ||
        sStage == STAGE_LEAVING) {
        return;
    }
    if (sStage == STAGE_ACTIVE) {
        RecordCursorPick();
        SetStage(STAGE_FROZEN); /* the machine screen stays, as after a confirm */
        sTimedOutFreeze = 1;
        return;
    }
    /* The screen is not up yet: the seed is where its cursor would start; nothing to show. */
    RecordMachine(sSeed.machine, sSeed.skin, sSeed.engine);
    gdx_dbg_logf("[mpp] machine step timed out before the picker: its seed is the pick\n");
    BeginLeaving();
}

void gdx3ds_mpp_course_timeout(void) {
    const int picking = (sKind == GDX_MPP_COURSE || sWant == GDX_MPP_COURSE);
    if (!picking || sStage == STAGE_IDLE || sStage == STAGE_FROZEN || sStage == STAGE_RELEASED ||
        sStage == STAGE_LEAVING) {
        return;
    }
    if (sStage == STAGE_ACTIVE) {
        RecordCourse(CourseOrSeed(gCourseIndex));
        gdx_dbg_logf("[mpp] course step timed out: the highlighted course is the pick\n");
        SetStage(STAGE_FROZEN); /* the course screen stays, as after a confirm */
        sTimedOutFreeze = 1;
        return;
    }
    RecordCourse(sSeed.course);
    gdx_dbg_logf("[mpp] course step timed out before the picker: the room's course stays\n");
    BeginLeaving();
}

void gdx3ds_mpp_keep(int machine, int skin, float engine) {
    const int picking = (sKind == GDX_MPP_MACHINE || sWant == GDX_MPP_MACHINE);
    if (!picking || sStage == STAGE_IDLE || sStage == STAGE_FROZEN || sStage == STAGE_RELEASED ||
        sStage == STAGE_LEAVING) {
        return;
    }
    RecordMachine(machine, skin, engine);
    gdx_dbg_logf("[mpp] machine kept (the previous pick, one tap on the bottom screen)\n");
    if (sStage == STAGE_ACTIVE) {
        SetStage(STAGE_FROZEN); /* the screen stays, as after a confirm, and takes no more input */
        sTimedOutFreeze = 1;
        return;
    }
    BeginLeaving(); /* the screen is not up yet: nothing to show */
}

int gdx3ds_mpp_input_locked(void) {
    return sTimedOutFreeze;
}

void gdx3ds_mpp_close(void) {
    if (sStage == STAGE_IDLE || sStage == STAGE_RELEASED || sStage == STAGE_LEAVING) {
        return;
    }
    BeginLeaving();
}

void gdx3ds_mpp_release(void) {
    if (sStage == STAGE_IDLE || sStage == STAGE_RELEASED) {
        return;
    }
    if (sHaveUser) {
        RestoreUserSettings();
    }
    sWant = GDX_MPP_NONE;
    SetStage(STAGE_RELEASED);
}

static void FrameHop(void) {
    if (!Settled()) {
        return;
    }
    if (GET_MODE(gGameMode) == GAMEMODE_MAIN_MENU && (sHopLanded || !sQueued)) {
        QueuePicker();
        return;
    }
    if (IsPickerMode(gGameMode)) {
        QueuePicker(); /* one picker straight to the next (round after round, machine -> course) */
        return;
    }
    if (!sQueued) {
        QueueMode(GAMEMODE_FLX_MAIN_MENU); /* from the title (or anywhere else): via the main menu */
        sQueued = 1;
    }
}

static void FrameLeaving(void) {
    if (sQueued || !Settled()) {
        return;
    }
    if (GET_MODE(gGameMode) == GAMEMODE_MAIN_MENU) {
        Finish();
        return;
    }
    QueueMode(GAMEMODE_FLX_MAIN_MENU);
    sQueued = 1;
}

void gdx3ds_mpp_frame(void) {
    sFrame++;
    if (sRefused && (sFrame - sRefusedAt) > REFUSED_FRAMES) {
        sRefused = 0;
    }
    if (sStage == STAGE_HOP) {
        FrameHop();
    } else if ((sStage == STAGE_ACTIVE || sStage == STAGE_FROZEN) && sWant != GDX_MPP_NONE && Settled()) {
        QueuePicker();
    } else if (sStage == STAGE_LEAVING) {
        FrameLeaving();
    }
}

void gdx3ds_mpp_mode_init(void) {
    sBooted = 1;
    if (sStage == STAGE_IDLE) {
        return;
    }
    if (sStage == STAGE_HOP) {
        sHopLanded = (GET_MODE(gGameMode) == GAMEMODE_MAIN_MENU);
        sQueued = sHopLanded; /* anything else won the change: queue the main menu again */
        return;
    }
    if (IsPickerMode(gGameMode) && sStage != STAGE_LEAVING && sStage != STAGE_RELEASED) {
        sKind = (GET_MODE(gGameMode) == GAMEMODE_COURSE_SELECT) ? GDX_MPP_COURSE : GDX_MPP_MACHINE;
        SetStage(STAGE_ACTIVE);
        return;
    }
    if (IsPickerMode(gGameMode)) {
        return; /* leaving through another picker screen (cannot happen through the menus) */
    }
    Finish(); /* the main menu, or anything else: the pick is over */
}

int gdx3ds_mpp_course_refused_recently(void) {
    return sRefused;
}

const char* gdx3ds_mpp_stage_name(void) {
    return kStageNames[sStage];
}

/* ---- the decomp's side (decomp-3ds-mp-stockpick.patch) ----------------------------------- */

int gdx_mp_pick_active(void) {
    return sStage != STAGE_IDLE;
}

static int IsRaceMode(s32 m) {
    return m == GAMEMODE_GP_RACE || m == GAMEMODE_PRACTICE || m == GAMEMODE_TIME_ATTACK || m == GAMEMODE_DEATH_RACE ||
           (m >= GAMEMODE_VS_2P && m <= GAMEMODE_VS_4P);
}

s32 gdx_mp_pick_filter(s32 mode, s32 queued) {
    const s32 m = GET_MODE(mode);
    const s32 q = GET_MODE(queued);
    if (sStage != STAGE_ACTIVE || m == q) {
        return queued;
    }
    if (m == GAMEMODE_MACHINE_SETTINGS && IsRaceMode(q)) {
        RecordMachine(gRacers[0].character, gRacers[0].machineSkinIndex, gPlayerEngine[0]);
        SetStage(STAGE_FROZEN);
        return mode; /* the confirmed screen stays */
    }
    if (m == GAMEMODE_MACHINE_SELECT && q != GAMEMODE_MACHINE_SETTINGS) {
        gMachineSelectState = MACHINE_SELECT_ACTIVE; /* B: a round always ends in a pick */
        return mode;
    }
    if (m == GAMEMODE_COURSE_SELECT && q == GAMEMODE_MACHINE_SELECT) {
        RecordCourse(gCourseIndex);
        SetStage(STAGE_FROZEN);
        return mode;
    }
    return queued;
}

int gdx_mp_pick_course_refused(s32 course) {
    if (sStage != STAGE_ACTIVE || sKind != GDX_MPP_COURSE) {
        return 0;
    }
    if (gdx_mpo_course_raceable(course) && gdx3ds_mpp_course_key(course) != 0) {
        return 0;
    }
    sRefused = 1;
    sRefusedAt = sFrame;
    gdx_dbg_logf("[mpp] course %d refused: multiplayer cannot race it\n", (int) course);
    return 1;
}

/* ---- courses and the race ----------------------------------------------------------------- */

static uint32_t Fnv(uint32_t h, const void* data, int n) {
    const u8* p = (const u8*) data;
    int i;
    for (i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

#ifdef EXPANSION_KIT
/* The DD cups come off the disk (Course_Load's disk arm): the same disk = the same course. */
static uint32_t DiskCourseKey(int course) {
    uint32_t h;
    if (gLeoDriveConnectionState == 0) {
        return 0;
    }
    h = Fnv(2166136261u, &leoBootID, (int) sizeof(leoBootID));
    h = Fnv(h, &course, (int) sizeof(course));
    return h != 0 ? h : 1u;
}
#endif

uint32_t gdx3ds_mpp_course_key(int course) {
    if (course >= 0 && course < 24) {
        return gdx_mpo_stock_course_key(course);
    }
#ifdef EXPANSION_KIT
    if (course >= FIRST_DD_COURSE && course <= LAST_DD_COURSE) {
        return DiskCourseKey(course);
    }
#endif
    return 0;
}

static s32 CupOf(int course) {
#ifdef EXPANSION_KIT
    if (course >= 36) {
        return DD_2_CUP;
    }
    if (course >= FIRST_DD_COURSE) {
        return DD_1_CUP;
    }
    if (course >= 24) {
        return EDIT_CUP;
    }
#endif
    return course / 6;
}

void gdx3ds_mpp_pin_course(int course) {
    gCourseIndex = course;
    gCupType = CupOf(course);
    gCurrentTrackName = gTrackNames[course];
}

uint32_t gdx3ds_mpp_picks_hash(int fieldRacers) {
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < fieldRacers && i < PICK_FIELD; i++) {
        const Racer* r = &gRacers[i];
        const int c = r->character;
        h = Fnv(h, &r->character, (int) sizeof(r->character));
        h = Fnv(h, &r->machineSkinIndex, (int) sizeof(r->machineSkinIndex));
        h = Fnv(h, &r->unk_1A8, (int) sizeof(r->unk_1A8));
        if (c >= 0 && c < PICK_MACHINES) {
            h = Fnv(h, &gMachines[c], (int) sizeof(Machine));
        }
    }
    return h;
}
