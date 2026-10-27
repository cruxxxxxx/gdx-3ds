/* port/3ds/gdx3ds_bui_gallery.c — canned views of the bottom-screen screens. Contract in
 * gdx3ds_bui_gallery.h. The data copies the design's mockups (screens.js): the same names, rooms,
 * pings and focus, so a render can be laid next to (and diffed against) its mockup. The masks
 * cover exactly the parts this build draws differently on purpose; each entry's note says why. */
#include "gdx3ds_bui_gallery.h"
#include "gdx3ds_bui_racedash.h"

#include <stdio.h>
#include <string.h>

enum {
    G_HOME,
    G_MP_CHOOSE,
    G_MP_CHOOSE_NOSERVER,
    G_NEARBY,
    G_NEARBY_EMPTY,
    G_NEARBY_RADIO_OFF,
    G_HOST_SHEET,
    G_ONLINE,
    G_CODE,
    G_NAME,
    G_CONNECTING,
    G_LOBBY_GUEST,
    G_LOBBY_GUEST_READY,
    G_LOBBY_HOST,
    G_LOBBY_BLOCKED,
    G_MACHINE,
    G_SETTINGS,
    G_COURSE_PICK,
    G_LAUNCH,
    G_E_WIRELESS,
    G_E_VERSION,
    G_E_DESYNC,
    G_E_LOST,
    G_E_CLOSED,
    G_E_HOME,
    G_E_LEAVE,
    G_E_SERVER,
    G_E_NOSTART,
    G_E_FULL,
    G_SP_HOST_LOBBY,
    G_SP_HOST_PICKING,
    G_SP_GUEST_PICKING,
    G_SP_GUEST_COURSE,
    G_SP_HOST_READY,
    G_SP_SETTINGS,
    G_STANDINGS,
    G_RACE_PIECES,
    G_RESULTS_HOST,
    G_RESULTS_GUEST,
    G_RESULTS_CPU,
    G_RESULTS_CPU4,
    G_E_HOST_LOST,
    G_ONLINE_DIRECT,
    G_LOBBY_DIRECT_HOST,
    G_LOBBY_HOST_HOLD,
    G_E_LEAVE_HOLD,
    G_SP_REJOIN_GUEST,
    G_SP_REJOIN_HOST,
    G_SP_FROZEN_TOUCH,
    G_EDITOR,
    G_EDITOR_BUSY,
    G_CS_FILES,
    G_CS_FILES_EMPTY,
    G_CS_SLOT,
    G_CS_SCAN,
    G_E_CS_REPLACE,
    G_SP_START_WITHOUT,
    G_SP_COURSE_TIMER,
    G_RESULTS_CLOSED,
    G_JOINING_RACE_WAIT,
    G_RACE_PAUSE_OTHER,
    G_RACE_PAUSE_MINE,
    G_RESULTS_ENDED,
    G_SP_GUEST_LOBBY,
    G_HOST_SHEET_UDS_OFF,
    G_RESULTS_LAST_LEFT,
    G_COUNT
};

/* Masks: {x, y, w, h}. Shared ones first. */
#define MASK_HOME_TILES {0, 92, 320, 108}
#define MASK_ROOM_ICONS {6, 74, 36, 120}
#define MASK_ROOM_META {44, 72, 232, 124}
#define MASK_COURSE_CARD {212, 28, 104, 161}
/* mpui: the host's own row says "<pilot> · HOST" where the design has its model (the other rows
 * show their ping on the host's console). */
#define MASK_HOST_ROW_SUB {40, 52, 130, 12}

static void DrawStandings(void);
static void DrawRacePieces(void);
static void DrawPauseOther(void);
static void DrawPauseMine(void);

static const BuiGalleryEntry kEntries[G_COUNT] = {
    {"home", "home", "mp-menus: no tile. MULTIPLAYER is the top screen's VS icon now (card-4: Home no longer says so; card-4b: logo centred, no footer note; release: SETTINGS at the footer's right opens the player settings).", 0x4383447Eu, 2,
     {MASK_HOME_TILES, {212, 211, 102, 25}}},
    {"mp-choose", "mp-choose", "The ONLINE badge says SERVER SET (no reachability probe runs before a room).", 0x86F44141u, 1,
     {{196, 110, 114, 16}}},
    {"mp-choose-noserver", "mp-choose-noserver", "", 0x16F4E39Eu, 0, {{0}}},
    {"nearby", "nearby",
     "A beacon carries no machine and no course: rows show the medium's icon and players/host model instead.", 0x089B4413u, 2,
     {MASK_ROOM_ICONS, MASK_ROOM_META}},
    {"nearby-empty", "nearby-empty", "", 0x8572993Du, 0, {{0}}},
    {"nearby-radio-off", "nearby-radio-off", "", 0x68866CF3u, 0, {{0}}},
    {"host-sheet", "host-sheet", "", 0xB71FBDB7u, 0, {{0}}},
    {"online", "online", "No server RTT before a room: the header badge says SERVER SET; the footer names the model.", 0x909DA437u, 2,
     {{150, 0, 170, 24}, {200, 224, 120, 16}}},
    {"code", "code", "", 0x59641DCAu, 0, {{0}}},
    {"name", NULL, "New: the [net] mp_name entry (the design's rename keyboard, mp_name's alphabet, 12 boxes).", 0xED62A955u, 0, {{0}}},
    {"connecting", "connecting", "The host's portrait is unknown before joining: the medium's icon (32 px) stands in.", 0x70CA3578u, 2,
     {{140, 46, 40, 40}, {100, 140, 120, 16}}},
    {"lobby-guest", "lobby-guest", "Guests do not hear the host's course/laps before START: the card says so.", 0x077CB493u, 1,
     {MASK_COURSE_CARD}},
    {"lobby-guest-ready", NULL, "New: the guest's READY toggled on.", 0x1C1051C0u, 0, {{0}}},
    {"lobby-host", "lobby-host", "Course card without a thumbnail (cup + number), no CPU-level row; START counts 4 machines max; the host row says HOST.",
     0xBD0E154Bu, 3, {MASK_COURSE_CARD, {110, 196, 206, 40}, MASK_HOST_ROW_SUB}},
    {"lobby-blocked", "lobby-blocked", "No course transfer (courses are out of scope): ANA is waiting, not GETTING COURSE.", 0xDA10E1C5u, 4,
     {MASK_COURSE_CARD, {4, 69, 204, 79}, {110, 196, 206, 40}, MASK_HOST_ROW_SUB}},
    {"machine", "machine", "The grid is the game's machine-select order with its real unlocks; stats/colors are read-only.", 0x15D9EDCBu,
     3, {{2, 26, 218, 182}, {224, 28, 92, 162}, {0, 206, 224, 34}}},
    {"settings", "settings", "No course thumbnail, no CPU-LEVEL row or page 2; MACHINES above 4 is locked until mp30.", 0x13A9FAA3u, 2,
     {{4, 28, 312, 40}, {0, 132, 320, 108}}},
    {"course-pick", "course-pick", "Stock cups only (no EDIT tab), no thumbnails: the course's number in its cup.", 0x53821D08u, 1,
     {{0, 28, 320, 212}}},
    {"launch", "launch", "No per-console loading receipts yet (M3): a spinner and LOADING rows.", 0xDC471E75u, 2,
     {{0, 30, 320, 120}, {0, 150, 320, 72}}},
    {"e-wireless", "e-wireless", "The list below differs as on 'nearby'.", 0x8386BE34u, 2, {MASK_ROOM_ICONS, MASK_ROOM_META}},
    {"e-version", "e-version", "The other room's version is not in its beacon; the list below as on 'nearby'.", 0xE65480F4u, 3,
     {MASK_ROOM_ICONS, MASK_ROOM_META, {16, 50, 288, 60}}},
    {"e-desync", "e-desync", "Lobby below as on 'lobby-guest'.", 0x32B1FD14u, 1, {MASK_COURSE_CARD}},
    {"e-lost", "e-lost", "A player lost mid-race ends the race, the room stays: the body says so.", 0x557CCAECu, 2,
     {MASK_COURSE_CARD, {16, 84, 288, 48}}},
    {"e-closed", NULL, "New: the host left; the room closed.", 0x729CC38Eu, 0, {{0}}},
    {"e-home", "e-home", "The race is called off for everyone (no CPU takeover before mp30).", 0x7C46C9D5u, 2,
     {MASK_COURSE_CARD, {16, 60, 288, 60}}},
    {"e-leave", "e-leave", "Lobby below as on 'lobby-host'.", 0x40653E8Eu, 3,
     {MASK_COURSE_CARD, {110, 196, 206, 40}, MASK_HOST_ROW_SUB}},
    {"e-server", "e-server", "The ONLINE badge below as on 'mp-choose'.", 0xA8CF8BB8u, 1, {{196, 110, 114, 16}}},
    {"e-nostart", NULL, "New: the race did not load in time.", 0x17384C04u, 0, {{0}}},
    {"e-full", "e-full", "The list below as on 'nearby'.", 0x1C225F9Fu, 2, {MASK_ROOM_ICONS, MASK_ROOM_META}},
    {"sp-host-lobby", NULL, "Stock picks: the host opens machine select on every console.", 0x03A486ACu, 0, {{0}}},
    {"sp-host-picking", NULL, "Stock picks: the round's machine step, one player picking, one not answering (AWAY).", 0x01CD2770u, 0, {{0}}},
    {"sp-guest-picking", NULL, "Stock picks: a guest picking on its top screen, with its countdown.", 0x6D552A83u, 0, {{0}}},
    {"sp-guest-course", NULL, "Stock picks: a guest READY while the host picks the course.", 0x22FB9BD5u, 0, {{0}}},
    {"sp-host-ready", NULL, "Stock picks: every pick in, the host's course on the card, START.", 0xE9B483BFu, 0, {{0}}},
    {"sp-settings", NULL, "Stock picks: CHANGE COURSE opens the stock course select; PICK MACHINES AGAIN.", 0x5DF965DAu, 0, {{0}}},
    {"standings", "standings", "The full standings overlay (30 machines, 4 humans), repainted per row at 4 Hz.", 0xE2DC1821u, 0, {{0}}},
    {"race-pieces", NULL, "New: the MP dash's own pieces over the checker (the DASH draws the rest in a race): column, map dots, "
     "NET chip, field count, STANDINGS, stall banner, drop toast.", 0x1C495A11u, 0, {{0}}},
    {"results-host", "results-host", "LUCA's time reads BY CPU (the mockup's own check misses it and draws nothing there).", 0x6E93BF24u, 1,
     {{160, 151, 70, 34}}},
    {"results-guest", "results-guest", "The wait bar's spinner sits in its first phase (the mockup's has no lit dot).", 0xE9CA08E0u, 2,
     {{160, 151, 70, 34}, {4, 199, 18, 18}}},
    {"results-cpu", NULL, "New: two humans in a 16-machine race: absolute places, the CPUs' places by name; a long course name sets the title small.", 0x778BE80Du, 0, {{0}}},
    {"results-cpu4", NULL, "New: four humans and CPUs: tighter rows and one line of CPU places.", 0x808DC93Bu, 0, {{0}}},
    {"e-host-lost", NULL, "New: the host stopped answering mid-race (the design's e-lost copy, over the results).", 0x7F37F048u, 0,
     {{0}}},
    {"online-direct", NULL, "New (mp-online-hw): [net] mp_direct names a host: JOIN BY ADDRESS under the two server cards.", 0xD28C4930u,
     0, {{0}}},
    {"lobby-direct-host", NULL, "New (mp-online-hw): a serverless host's WiFi room shows ROOM <code> for its joiners to type.",
     0xDD033996u, 0, {{0}}},
    {"lobby-host-hold", NULL, "New (mp-menus): START RACE held, its meter half full ([ui] hold_confirm_ms).", 0xB867C407u, 0, {{0}}},
    {"e-leave-hold", NULL, "New (mp-menus): CLOSE ROOM held to confirm, its meter filling.", 0x54A80F55u, 0, {{0}}},
    {"sp-rejoin-guest", NULL, "New (mp-menus): LUCA rejoined, so a new machine round opened; KEEP confirms the last pick.", 0x494F72A7u, 0,
     {{0}}},
    {"sp-rejoin-host", NULL, "New (mp-menus): the host's view of the automatic round after a rejoin.", 0x25F0BC1Du, 0, {{0}}},
    {"sp-frozen-touch", NULL, "New (mp-menus): the game has the buttons (top screen picking): no key glyphs, touch only.", 0x0FA9C6ADu, 0,
     {{0}}},
    {"editor", NULL, "New (mp-menus): the course editor's bottom menu: import from SD or scan QR pages.", 0xC12A858Fu, 0, {{0}}},
    {"editor-busy", NULL, "New (mp-menus): the editor is mid-edit, so nothing may write a course yet.", 0x81FD2012u, 0, {{0}}},
    {"cs-files", NULL, "New (mp-menus): the .gdxcourse files on the SD card (was the dev console's CRS [SD] view).", 0x4192773Au, 0,
     {{0}}},
    {"cs-files-empty", NULL, "New (mp-menus): no course files on the card.", 0xC3907657u, 0, {{0}}},
    {"cs-slot", NULL, "New (mp-menus): where an import goes: the library or an EDIT CUP slot.", 0x174EB8F1u, 0, {{0}}},
    {"cs-scan", NULL, "New (mp-menus): the QR scan (the camera picture is painted into the black box live).", 0x7B33EF2Du, 0, {{0}}},
    {"e-cs-replace", NULL, "New (mp-menus): an import would replace a cup slot's course.", 0xD8E81654u, 0, {{0}}},
    {"sp-start-without", NULL, "New (mp-menus): KENJI went quiet (HOME) in the lobby: the host may START WITHOUT him, held.", 0x2FBEADDAu, 0,
     {{0}}},
    {"sp-course-timer", NULL, "New (mp-menus): the host's course step counts down; then the highlighted course is the pick.", 0xFD28E2C2u, 0,
     {{0}}},
    {"results-closed", NULL, "New (mp-menus): the host left mid-race; the race finished, the results stay, LEAVE goes idle.", 0x559A32FDu, 0,
     {{0}}},
    {"joining-race-wait", NULL, "New (mp-menus): in the room while it races: no mid-race join, in when the race ends.", 0x07A4AE7Fu, 0, {{0}}},
    {"race-pause-other", NULL, "New (mp-menus): KENJI paused: every console pauses; only KENJI resumes or ends the race.", 0x002DB643u, 0,
     {{0}}},
    {"race-pause-mine", NULL, "New (mp-menus): this console paused, QUIT highlighted: END RACE held, its meter half full.", 0x2CEEA3D6u, 0,
     {{0}}},
    {"results-ended", NULL, "New (mp-menus): KENJI ended the race from the pause menu: no points, back to the room.", 0x712DC0E7u, 0, {{0}}},
    {"sp-guest-lobby", NULL, "New (mpui): a guest waits for the host to open machine select.", 0x3600284Cu, 0, {{0}}},
    {"host-sheet-uds-off", NULL, "New (mpui): [net] mp_uds=0: the LOCAL card is off and says so in player words.", 0xF71E9655u, 0, {{0}}},
    {"results-last-left", NULL, "New (mpui): the others fell out, so the game ended the race with ANA still racing: no time, SURVIVED.",
     0x82BDC9A6u, 0, {{0}}},
};

int bui_gallery_count(void) {
    return G_COUNT;
}

const BuiGalleryEntry* bui_gallery_entry(int i) {
    return (i >= 0 && i < G_COUNT) ? &kEntries[i] : NULL;
}

/* ---- canned data ----------------------------------------------------------------------------- */

static void Base(BuiView* v) {
    bui_view_init(v);
    v->model = BUI_MODEL_O3DS;
    v->ek = 1;
    snprintf(v->name, sizeof(v->name), "MIKE");
    snprintf(v->version, sizeof(v->version), "V1.0.2 EK");
    v->homePilot = 0;
    v->mp.lan = 1;
    v->mp.haveServer = 1;
    v->mp.uds = 1;
    v->mp.page = 1;
    v->mp.pages = 1;
    v->mp.laps = 3;
    v->mp.course = 0;
    v->mp.racersCap = 4;
}

static void Room(BuiRoomView* r, const char* host, int players, int model, int signal, int medium) {
    memset(r, 0, sizeof(*r));
    snprintf(r->host, sizeof(r->host), "%s", host);
    r->players = (int8_t) players;
    r->maxPlayers = 4;
    r->model = (int8_t) model;
    r->signal = (int8_t) signal;
    r->medium = (int8_t) medium;
    r->joinable = 1;
    r->compatible = 1;
}

static void NearbyRooms(BuiView* v) {
    v->screen = BUI_SCR_NEARBY;
    v->anim = 2;
    v->mp.roomCount = 3;
    v->mp.pages = 2;
    Room(&v->mp.room[0], "KENJI", 2, BUI_MODEL_N3DS, 3, BUI_MEDIUM_UDS);
    Room(&v->mp.room[1], "ANA", 1, BUI_MODEL_N3DS, 2, BUI_MEDIUM_WIFI);
    Room(&v->mp.room[2], "LUCA", 1, BUI_MODEL_O3DS, 2, BUI_MEDIUM_WIFI);
    v->mp.room[2].compatible = 0;
    v->mp.room[2].joinable = 0;
    snprintf(v->mp.room[2].whyNot, sizeof(v->mp.room[2].whyNot), "OTHER VERSION");
    v->focus = BUI_W_NEARBY_ROW0;
}

static void Slot(BuiSlotView* s, const char* name, int machine, int model, int ready, int rtt) {
    memset(s, 0, sizeof(*s));
    s->present = 1;
    s->live = 1;
    s->versionOk = 1;
    s->ready = (int8_t) ready;
    s->machine = (int8_t) machine;
    s->model = (int8_t) model;
    s->rttMs = (int16_t) rtt;
    snprintf(s->name, sizeof(s->name), "%s", name);
}

static void LobbyGuest(BuiView* v) {
    v->screen = BUI_SCR_LOBBY;
    v->mp.isLan = 1;
    v->mp.isUds = 1;
    v->mp.maxPlayers = 4;
    Slot(&v->mp.slot[0], "KENJI", 3, BUI_MODEL_N3DS, 1, 40);
    v->mp.slot[0].host = 1;
    Slot(&v->mp.slot[1], "MIKE", 0, BUI_MODEL_O3DS, 0, -1);
    v->mp.slot[1].me = 1;
    Slot(&v->mp.slot[2], "ANA", 4, BUI_MODEL_N3DS, 1, 90);
    v->focus = BUI_W_LOBBY_MAIN;
}

static void LobbyHost(BuiView* v) {
    v->screen = BUI_SCR_LOBBY;
    v->mp.isHost = 1;
    v->mp.isLan = 0;
    snprintf(v->mp.code, sizeof(v->mp.code), "K7Q2M");
    v->mp.maxPlayers = 4;
    Slot(&v->mp.slot[0], "MIKE", 0, BUI_MODEL_O3DS, 1, -1);
    v->mp.slot[0].host = 1;
    v->mp.slot[0].me = 1;
    Slot(&v->mp.slot[1], "ANA", 4, BUI_MODEL_N3DS, 1, 46);
    Slot(&v->mp.slot[2], "KENJI", 3, BUI_MODEL_N3DS, 1, 88);
    Slot(&v->mp.slot[3], "LUCA", 2, BUI_MODEL_O3DS, 1, 140);
    v->mp.startable = 4;
    v->mp.racers = 4;
    v->focus = BUI_W_LOBBY_MAIN;
}

static void LobbyBlocked(BuiView* v) {
    v->screen = BUI_SCR_LOBBY;
    v->mp.isHost = 1;
    v->mp.isLan = 1;
    v->mp.isUds = 1;
    v->mp.maxPlayers = 4;
    Slot(&v->mp.slot[0], "MIKE", 0, BUI_MODEL_O3DS, 1, -1);
    v->mp.slot[0].host = 1;
    v->mp.slot[0].me = 1;
    Slot(&v->mp.slot[1], "ANA", 4, BUI_MODEL_N3DS, 0, 60);
    Slot(&v->mp.slot[2], "KENJI", 3, BUI_MODEL_N3DS, 1, 70);
    v->mp.slot[2].versionOk = 0;
    v->mp.startable = 0;
    snprintf(v->mp.whyNot, sizeof(v->mp.whyNot), "A PLAYER NEEDS AN UPDATE");
    v->mp.laps = 5;
    v->focus = BUI_W_NONE;
}

static void StockRoom(BuiView* v, int host, int step) {
    v->screen = BUI_SCR_LOBBY;
    v->mp.isHost = (int8_t) host;
    v->mp.isLan = 1;
    v->mp.maxPlayers = 4;
    v->mp.stockPick = 1;
    v->mp.pickStep = (int8_t) step;
    v->mp.pickSecs = 42;
    v->mp.courseOk = 1;
    v->mp.course = 9;
    v->mp.laps = 3;
    Slot(&v->mp.slot[0], "MIKE", 0, BUI_MODEL_N3DS, 1, -1);
    v->mp.slot[0].host = 1;
    Slot(&v->mp.slot[1], "ANA", 4, BUI_MODEL_O3DS, 0, 46);
    Slot(&v->mp.slot[2], "KENJI", 3, BUI_MODEL_N3DS, 0, 88);
    v->mp.slot[host ? 0 : 1].me = 1;
    v->focus = BUI_W_LOBBY_MAIN;
}

static void StockPicked(BuiSlotView* s, int machine) {
    s->picked = 1;
    s->machine = (int8_t) machine;
}

static void MachineGrid(BuiView* v) {
    static const int8_t kGrid[BUI_MACHINES] = {0, 1, 2, 3, 4, 5, 7, 8, 23, 6, 25, 27, 24, 9, 20, 19,
                                                29, 17, 12, 22, 16, 18, 10, 11, 13, 26, 21, 14, 15, 28};
    static const uint8_t kSkin[4][3] = {{0x20, 0x50, 0xFF}, {0xFF, 0x30, 0x30}, {0x30, 0xC0, 0x40}, {0xFF, 0xD0, 0x20}};
    v->screen = BUI_SCR_MACHINE;
    memcpy(v->grid, kGrid, sizeof(kGrid));
    memcpy(v->skinRgb, kSkin, sizeof(kSkin));
    v->unlocked = 24;
    v->pickSlot = 0;
    v->stats[0] = v->stats[1] = v->stats[2] = 1;
    v->skin = 0;
    v->engine = 50;
    v->focus = BUI_W_MACHINE0;
}

/* The mockups ring no dialog button; the console rings the default one (`focus`) so the D-pad
 * has a start. The gallery follows the mockups. */
static void Dialog(BuiView* v, int dialog, int focus, const char* name, const char* code) {
    (void) focus;
    v->dialog = (int8_t) dialog;
    v->dialogFocus = -1;
    snprintf(v->dialogName, sizeof(v->dialogName), "%s", name != NULL ? name : "");
    snprintf(v->dialogCode, sizeof(v->dialogCode), "%s", code != NULL ? code : "");
}

/* ---- race and results (M3) ------------------------------------------------------------------ */

/* screens.js standings(): ANA 2nd, MIKE (this console) 3rd, KENJI 7th, LUCA 11th of 30, leader on
 * lap 2, CPUs past 16th a lap down. */
static void CannedRace(BuiRaceView* r) {
    static const int8_t kCpu[26] = {18, 29, 24, 10, 11, 7, 8, 5, 21, 16, 12, 26, 13,
                                    27, 19, 20, 22, 15, 6, 9, 23, 14, 25, 28, 17, 1};
    static const char* const kNames[4] = {"ANA", "MIKE", "KENJI", "LUCA"};
    static const int8_t kHumanAt[4] = {1, 2, 6, 10};
    static const int8_t kHumanChar[4] = {4, 0, 3, 2};
    int ci = 0;
    int i;
    int p;
    memset(r, 0, sizeof(*r));
    r->count = 30;
    r->numPlayers = 4;
    r->me = 1;
    r->totalLaps = 3;
    r->linkBars = 3;
    r->delay = 2;
    for (p = 0; p < 4; p++) {
        snprintf(r->name[p], sizeof(r->name[p]), "%s", kNames[p]);
    }
    for (i = 0; i < 30; i++) {
        BuiRaceRacer* e = &r->byPos[i];
        e->human = 0;
        for (p = 0; p < 4; p++) {
            if (kHumanAt[p] == i) {
                e->human = (int8_t) (p + 1);
                e->character = kHumanChar[p];
            }
        }
        if (e->human == 0) {
            e->character = kCpu[ci++ % 26];
        }
        e->lap = (int8_t) ((e->human == 0 && i > 16) ? 1 : 2);
        e->gapMs = i * 800;
        e->col = (int16_t) (10 + (i * 7) % 44);
        e->row = (int16_t) (12 + (i * 11) % 40);
    }
}

static void DrawStandings(void) {
    BuiRaceView r;
    CannedRace(&r);
    bui_race_standings(&r);
}

static void DrawRacePieces(void) {
    BuiRaceView r;
    CannedRace(&r);
    r.count = 16;
    r.byPos[10].human = 4; /* LUCA 11th of 16 */
    r.removedMask = 1u << 3;
    bui_checker(0, 0, BUI_W, BUI_H);
    bui_race_column(&r);
    bui_fill(77, 36, 168, 168, BUI_TK_BLACK);
    bui_stroke(76, 35, 170, 170, BUI_TK_WHITE, 1);
    bui_race_map_dots(&r, 77, 36, 168);
    bui_race_net_chip(&r, 77, 36);
    bui_fill(0, 218, 320, 22, BUI_TK_HEADER);
    bui_race_standings_button();
    bui_race_header_field(&r, 232, 316, 219, 20);
    bui_race_stall("KENJI", 1400, 3000);
    bui_race_drop_toast("LUCA", 1);
}

/* A race ended from the pause menu scores nothing. */
static void ZeroRacePoints(BuiView* v) {
    int r;
    for (r = 0; r < v->mp.resultCount; r++) {
        v->mp.result[r].total = (int16_t) (v->mp.result[r].total - v->mp.result[r].points);
        v->mp.result[r].points = 0;
    }
}

static void DrawPauseOther(void) {
    BuiRacePause p;
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "KENJI");
    p.player = 2;
    p.endsRace = 1;
    snprintf(p.option, sizeof(p.option), "QUIT");
    bui_race_pause(&p);
}

static void DrawPauseMine(void) {
    BuiRacePause p;
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "MIKE");
    p.player = 1;
    p.mine = 1;
    p.endsRace = 1;
    snprintf(p.option, sizeof(p.option), "QUIT");
    p.holdPermille = 500;
    bui_race_pause(&p);
}

static void Result(BuiResultView* r, int slot, int player, int character, int place, const char* name, int ms, int pts,
                   int total) {
    memset(r, 0, sizeof(*r));
    r->slot = (int8_t) slot;
    r->player = (int8_t) player;
    r->character = (int8_t) character;
    r->place = (int8_t) place;
    r->timeMs = ms;
    r->points = (int8_t) pts;
    r->total = (int16_t) total;
    snprintf(r->name, sizeof(r->name), "%s", name);
}

/* screens.js resultsRows(): race 2 on MUTE CITY; MIKE (this console) second, LUCA finished by a
 * CPU. */
static void Results(BuiView* v, int host) {
    v->screen = BUI_SCR_RESULTS;
    v->mp.isHost = (int8_t) host;
    v->mp.isLan = 1;
    v->mp.maxPlayers = 4;
    v->mp.racesDone = 2;
    v->mp.course = 0;
    Slot(&v->mp.slot[0], host ? "MIKE" : "ANA", host ? 0 : 4, BUI_MODEL_N3DS, 1, -1);
    v->mp.slot[0].host = 1;
    Slot(&v->mp.slot[1], host ? "ANA" : "MIKE", host ? 4 : 0, BUI_MODEL_O3DS, 1, 46);
    Slot(&v->mp.slot[2], "KENJI", 3, BUI_MODEL_N3DS, 1, 88);
    Slot(&v->mp.slot[3], "LUCA", 2, BUI_MODEL_O3DS, 1, 140);
    v->mp.slot[host ? 0 : 1].me = 1;
    v->mp.resultCount = 4;
    v->mp.fieldCount = 4;
    Result(&v->mp.result[0], host ? 1 : 0, 1, 4, 1, "ANA", 112300, 5, 13);
    Result(&v->mp.result[1], host ? 0 : 1, 0, 0, 2, "MIKE", 113040, 3, 8);
    Result(&v->mp.result[2], 2, 2, 3, 3, "KENJI", 115710, 1, 6);
    Result(&v->mp.result[3], 3, 3, 2, 4, "LUCA", 118000, 0, 1);
    v->mp.result[3].byCpu = 1;
    v->focus = host ? BUI_W_RESULTS_REMATCH : BUI_W_NONE;
}

/* A field of `count`: the humans at the given places, CPUs in the rest. */
static void ResultField(BuiView* v, int count) {
    static const int8_t kCpu[26] = {18, 29, 24, 10, 11, 7, 8, 5, 21, 16, 12, 26, 13,
                                    27, 19, 20, 22, 15, 6, 9, 23, 14, 25, 28, 17, 1};
    int ci = 0;
    int i;
    int k;
    v->mp.fieldCount = (int8_t) count;
    for (i = 0; i < count; i++) {
        v->mp.fieldHuman[i] = 0;
        v->mp.fieldChar[i] = kCpu[ci % 26];
        for (k = 0; k < v->mp.resultCount; k++) {
            if (v->mp.result[k].place == i + 1) {
                v->mp.fieldHuman[i] = (int8_t) (v->mp.result[k].slot + 1);
                v->mp.fieldChar[i] = v->mp.result[k].character;
            }
        }
        ci += v->mp.fieldHuman[i] == 0;
    }
}

static void ResultsCpu(BuiView* v) {
    Results(v, 1);
    v->mp.slot[2].present = 0;
    v->mp.slot[3].present = 0;
    v->mp.resultCount = 2;
    Result(&v->mp.result[0], 0, 0, 0, 3, "MIKE", 113040, 5, 5);
    Result(&v->mp.result[1], 1, 1, 4, 7, "ANA", 115900, 3, 3);
    v->mp.racesDone = 1;
    v->mp.course = 16; /* WHITE LAND 2: the long badge squeezes the title to the small font */
    ResultField(v, 16);
}

static void ResultsCpu4(BuiView* v) {
    Results(v, 1);
    v->mp.result[0].place = 2;
    v->mp.result[1].place = 5;
    v->mp.result[2].place = 9;
    v->mp.result[3].place = 14;
    ResultField(v, 16);
}

/* mp-menus: the course editor's import, two good files and one refused. */
static void CsFile(BuiCsFileView* f, const char* name, const char* leaf, int points, int finished) {
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    snprintf(f->leaf, sizeof(f->leaf), "%s", leaf);
    f->ok = 1;
    f->points = (int8_t) points;
    f->finished = (int8_t) finished;
}

static void CsFiles(BuiView* v) {
    v->screen = BUI_SCR_CS_FILES;
    v->cs.canWrite = 1;
    v->cs.fileCount = 3;
    v->cs.page = 1;
    v->cs.pages = 1;
    CsFile(&v->cs.file[0], "SHARE1", "SHARE1.GDXCOURSE", 24, 1);
    CsFile(&v->cs.file[1], "LOOPY", "LOOPY.GDXCOURSE", 12, 0);
    memset(&v->cs.file[2], 0, sizeof(v->cs.file[2]));
    snprintf(v->cs.file[2].leaf, sizeof(v->cs.file[2].leaf), "OLD.GDXCOURSE");
    snprintf(v->cs.file[2].why, sizeof(v->cs.file[2].why), "BAD CRC");
    v->focus = BUI_W_CS_ROW0;
}

static void CsPending(BuiView* v) {
    v->cs.canWrite = 1;
    snprintf(v->cs.pendingName, sizeof(v->cs.pendingName), "SHARE1");
    v->cs.pendingPoints = 24;
    v->cs.pendingFinished = 1;
    v->cs.pendingSlot = -1;
    snprintf(v->cs.cup[0], sizeof(v->cs.cup[0]), "RINGS");
    snprintf(v->cs.cup[2], sizeof(v->cs.cup[2]), "SHARE1");
}

/* Stock picks after LUCA dropped and came back: a fresh machine round for everyone. */
static void RejoinRound(BuiView* v, int host) {
    StockRoom(v, host, BUI_STEP_MACHINES);
    Slot(&v->mp.slot[3], "LUCA", 2, BUI_MODEL_O3DS, 0, 120);
    v->mp.slot[3].picking = BUI_PICK_MACHINE;
    v->mp.slot[2].picking = BUI_PICK_MACHINE;
    v->mp.slot[host ? 1 : 0].picking = BUI_PICK_MACHINE;
    StockPicked(&v->mp.slot[host ? 0 : 1], host ? 0 : 4);
    v->mp.slot[host ? 0 : 1].picked = 0;
    v->mp.pickSecs = 27;
    v->keepMachine = host ? 0 : 4;
    v->focus = BUI_W_LOBBY_KEEP;
}

BuiGalleryDraw bui_gallery_draw(int i) {
    switch (i) {
        case G_STANDINGS:
            return DrawStandings;
        case G_RACE_PIECES:
            return DrawRacePieces;
        case G_RACE_PAUSE_OTHER:
            return DrawPauseOther;
        case G_RACE_PAUSE_MINE:
            return DrawPauseMine;
        default:
            return NULL;
    }
}

void bui_gallery_view(int i, BuiView* v) {
    Base(v);
    switch (i) {
        case G_HOME:
            v->screen = BUI_SCR_HOME;
            v->settingsButton = 1;
            break;
        case G_MP_CHOOSE:
            v->screen = BUI_SCR_MP_CHOOSE;
            v->focus = BUI_W_CHOOSE_NEARBY;
            break;
        case G_MP_CHOOSE_NOSERVER:
            v->screen = BUI_SCR_MP_CHOOSE;
            v->focus = BUI_W_CHOOSE_NEARBY;
            v->mp.haveServer = 0;
            break;
        case G_NEARBY:
            NearbyRooms(v);
            break;
        case G_NEARBY_EMPTY:
            v->screen = BUI_SCR_NEARBY;
            v->anim = 5;
            v->focus = BUI_W_NEARBY_HOST;
            break;
        case G_NEARBY_RADIO_OFF:
            v->screen = BUI_SCR_NEARBY;
            v->mp.radioOff = 1;
            v->focus = BUI_W_NEARBY_RETRY;
            break;
        case G_HOST_SHEET:
            v->screen = BUI_SCR_HOST;
            v->hostMedium = BUI_MEDIUM_UDS;
            v->roomSize = 4;
            v->focus = BUI_W_HOST_LOCAL;
            break;
        case G_ONLINE:
            v->screen = BUI_SCR_ONLINE;
            v->focus = BUI_W_ONLINE_HOST;
            break;
        case G_CODE:
            v->screen = BUI_SCR_CODE;
            snprintf(v->entry, sizeof(v->entry), "K7Q");
            v->focus = BUI_W_KEY0 + 11; /* "M" */
            break;
        case G_NAME:
            v->screen = BUI_SCR_NAME;
            snprintf(v->entry, sizeof(v->entry), "MIKE");
            v->focus = BUI_W_KEY0 + 12;
            break;
        case G_CONNECTING:
            v->screen = BUI_SCR_JOINING;
            v->anim = 1;
            snprintf(v->mp.joinName, sizeof(v->mp.joinName), "KENJI");
            v->mp.joinMedium = BUI_MEDIUM_UDS;
            break;
        case G_LOBBY_GUEST:
            LobbyGuest(v);
            break;
        case G_LOBBY_GUEST_READY:
            LobbyGuest(v);
            v->mp.ready = 1;
            v->mp.slot[1].ready = 1;
            break;
        case G_LOBBY_HOST:
            LobbyHost(v);
            break;
        case G_LOBBY_BLOCKED:
            LobbyBlocked(v);
            break;
        case G_MACHINE:
            MachineGrid(v);
            break;
        case G_SETTINGS:
            v->screen = BUI_SCR_SETTINGS;
            v->mp.isHost = 1;
            v->mp.racers = 4;
            v->focus = BUI_W_SET_COURSE;
            break;
        case G_COURSE_PICK:
            v->screen = BUI_SCR_COURSE;
            v->cup = 0;
            v->mp.course = 0;
            v->focus = BUI_W_COURSE0;
            break;
        case G_LAUNCH:
            LobbyHost(v);
            v->screen = BUI_SCR_LAUNCH;
            v->focus = BUI_W_NONE;
            break;
        case G_E_WIRELESS:
            NearbyRooms(v);
            Dialog(v, BUI_DLG_WIRELESS_OFF, 1, NULL, NULL);
            break;
        case G_E_VERSION:
            NearbyRooms(v);
            Dialog(v, BUI_DLG_VERSION, 0, "LUCA", NULL);
            break;
        case G_E_DESYNC:
            LobbyGuest(v);
            Dialog(v, BUI_DLG_DESYNC, 1, "ANA", "SYNC T4120");
            break;
        case G_E_LOST:
            LobbyGuest(v);
            Dialog(v, BUI_DLG_LOST, 0, "KENJI'S 3DS", "LOST");
            break;
        case G_E_CLOSED:
            LobbyGuest(v);
            Dialog(v, BUI_DLG_CLOSED, 0, "KENJI", "HOST LOST");
            break;
        case G_E_HOME:
            LobbyGuest(v);
            Dialog(v, BUI_DLG_SUSPEND, 0, NULL, NULL);
            break;
        case G_E_LEAVE:
            LobbyHost(v);
            Dialog(v, BUI_DLG_LEAVE_HOST, 1, NULL, NULL);
            break;
        case G_E_SERVER:
            v->screen = BUI_SCR_MP_CHOOSE;
            v->focus = BUI_W_CHOOSE_NEARBY;
            Dialog(v, BUI_DLG_SERVER, 1, NULL, "TIMEOUT");
            break;
        case G_E_NOSTART:
            LobbyHost(v);
            Dialog(v, BUI_DLG_NO_START, 0, NULL, NULL);
            break;
        case G_SP_HOST_LOBBY:
            StockRoom(v, 1, BUI_STEP_LOBBY);
            break;
        case G_SP_HOST_PICKING:
            StockRoom(v, 1, BUI_STEP_MACHINES);
            StockPicked(&v->mp.slot[0], 0);
            v->mp.slot[1].picking = BUI_PICK_MACHINE;
            v->mp.slot[2].picking = BUI_PICK_MACHINE;
            v->mp.slot[2].live = 0; /* dropped mid-pick: AWAY, NOT ANSWERING clear of the badge */
            v->focus = BUI_W_NONE;
            break;
        case G_SP_GUEST_PICKING:
            StockRoom(v, 0, BUI_STEP_MACHINES);
            StockPicked(&v->mp.slot[0], 0);
            v->mp.slot[1].picking = BUI_PICK_MACHINE;
            StockPicked(&v->mp.slot[2], 3);
            v->mp.pickSecs = 9;
            v->focus = BUI_W_NONE;
            break;
        case G_SP_GUEST_COURSE:
            StockRoom(v, 0, BUI_STEP_COURSE);
            v->mp.hostPicking = BUI_PICK_COURSE;
            v->mp.slot[0].picking = BUI_PICK_COURSE;
            StockPicked(&v->mp.slot[0], 0);
            StockPicked(&v->mp.slot[1], 4);
            StockPicked(&v->mp.slot[2], 3);
            v->mp.pickSecs = -1;
            v->focus = BUI_W_NONE;
            break;
        case G_SP_HOST_READY:
            StockRoom(v, 1, BUI_STEP_READY);
            StockPicked(&v->mp.slot[0], 0);
            StockPicked(&v->mp.slot[1], 4);
            v->mp.slot[2].ready = 0; /* never reported a pick: the default */
            v->mp.startable = 3;
            v->mp.racers = 3;
            v->mp.pickSecs = -1;
            break;
        case G_SP_SETTINGS:
            v->screen = BUI_SCR_SETTINGS;
            v->mp.isHost = 1;
            v->mp.racers = 4;
            v->mp.stockPick = 1;
            v->mp.pickStep = BUI_STEP_READY;
            v->mp.course = 31;
            v->focus = BUI_W_SET_REPICK;
            break;
        case G_STANDINGS:
        case G_RACE_PIECES:
            break; /* drawn by the entry's own draw() */
        case G_RESULTS_HOST:
            Results(v, 1);
            break;
        case G_RESULTS_GUEST:
            Results(v, 0);
            break;
        case G_RESULTS_CPU:
            ResultsCpu(v);
            break;
        case G_RESULTS_CPU4:
            ResultsCpu4(v);
            break;
        case G_ONLINE_DIRECT:
            v->screen = BUI_SCR_ONLINE;
            v->mp.haveServer = 1;
            v->mp.haveDirect = 1;
            snprintf(v->mp.directAddr, sizeof(v->mp.directAddr), "192.168.1.20:27300");
            v->focus = BUI_W_ONLINE_DIRECT;
            break;
        case G_LOBBY_DIRECT_HOST:
            LobbyHost(v);
            v->mp.isLan = 1;
            v->mp.showCode = 1;
            v->mp.maxPlayers = 2;
            memset(&v->mp.slot[2], 0, sizeof(v->mp.slot[2]));
            memset(&v->mp.slot[3], 0, sizeof(v->mp.slot[3]));
            v->mp.startable = 2;
            break;
        case G_E_HOST_LOST:
            Results(v, 0);
            Dialog(v, BUI_DLG_HOST_LOST, 0, "KENJI", NULL);
            break;
        case G_LOBBY_HOST_HOLD:
            LobbyHost(v);
            v->holdConfirm = 1;
            v->holdWidget = BUI_W_LOBBY_MAIN;
            v->holdPct = 55;
            break;
        case G_E_LEAVE_HOLD:
            LobbyHost(v);
            Dialog(v, BUI_DLG_LEAVE_HOST, 1, NULL, NULL);
            v->dialogFocus = 1;
            v->holdConfirm = 1;
            v->holdWidget = BUI_W_DLG_BUTTON0 + 1;
            v->holdPct = 40;
            break;
        case G_SP_REJOIN_GUEST:
            RejoinRound(v, 0);
            break;
        case G_SP_REJOIN_HOST:
            RejoinRound(v, 1);
            break;
        case G_SP_FROZEN_TOUCH:
            StockRoom(v, 0, BUI_STEP_COURSE);
            v->mp.hostPicking = BUI_PICK_COURSE;
            v->mp.slot[0].picking = BUI_PICK_COURSE;
            StockPicked(&v->mp.slot[0], 0);
            StockPicked(&v->mp.slot[1], 4);
            StockPicked(&v->mp.slot[2], 3);
            v->mp.pickSecs = -1;
            v->buttonsOff = 1;
            v->focus = BUI_W_NONE;
            break;
        case G_SP_START_WITHOUT:
            StockRoom(v, 1, BUI_STEP_READY);
            StockPicked(&v->mp.slot[0], 0);
            StockPicked(&v->mp.slot[1], 4);
            StockPicked(&v->mp.slot[2], 3);
            v->mp.slot[2].live = 0;
            v->mp.pickSecs = -1;
            v->mp.awayStartable = 2;
            snprintf(v->mp.awayName, sizeof(v->mp.awayName), "KENJI");
            snprintf(v->mp.whyNot, sizeof(v->mp.whyNot), "WAITING FOR KENJI");
            v->holdConfirm = 1;
            v->holdWidget = BUI_W_LOBBY_MAIN;
            v->holdPct = 30;
            break;
        case G_SP_COURSE_TIMER:
            StockRoom(v, 1, BUI_STEP_COURSE);
            v->mp.hostPicking = BUI_PICK_COURSE;
            v->mp.slot[0].picking = BUI_PICK_COURSE;
            StockPicked(&v->mp.slot[0], 0);
            StockPicked(&v->mp.slot[1], 4);
            StockPicked(&v->mp.slot[2], 3);
            v->mp.pickSecs = -1;
            v->mp.courseSecs = 24;
            v->buttonsOff = 1;
            v->focus = BUI_W_NONE;
            break;
        case G_RESULTS_CLOSED:
            Results(v, 0);
            v->mp.roomClosed = 1;
            snprintf(v->mp.closedWhy, sizeof(v->mp.closedWhy), "THE HOST LEFT");
            v->focus = BUI_W_RESULTS_LEAVE;
            break;
        case G_RESULTS_ENDED:
            Results(v, 0);
            snprintf(v->mp.endedBy, sizeof(v->mp.endedBy), "KENJI");
            ZeroRacePoints(v);
            v->focus = BUI_W_RESULTS_LEAVE;
            break;
        case G_SP_GUEST_LOBBY:
            StockRoom(v, 0, BUI_STEP_LOBBY);
            v->focus = BUI_W_NONE;
            break;
        case G_HOST_SHEET_UDS_OFF:
            v->screen = BUI_SCR_HOST;
            v->mp.uds = 0;
            v->hostMedium = BUI_MEDIUM_WIFI;
            v->roomSize = 4;
            v->focus = BUI_W_HOST_WIFI;
            break;
        case G_RESULTS_LAST_LEFT:
            Results(v, 1);
            v->mp.racesDone = 1;
            Result(&v->mp.result[0], 1, 1, 4, 1, "ANA", -1, 5, 5);
            Result(&v->mp.result[1], 0, 0, 0, 2, "MIKE", 0, 3, 3);
            Result(&v->mp.result[2], 2, 2, 3, 3, "KENJI", 0, 1, 1);
            Result(&v->mp.result[3], 3, 3, 2, 4, "LUCA", 0, 0, 0);
            break;
        case G_JOINING_RACE_WAIT:
            v->screen = BUI_SCR_JOINING;
            v->anim = 3;
            snprintf(v->mp.joinName, sizeof(v->mp.joinName), "KENJI");
            v->mp.joinMedium = BUI_MEDIUM_ONLINE;
            v->mp.raceWait = 1;
            break;
        case G_EDITOR:
            v->screen = BUI_SCR_EDITOR;
            v->cs.canWrite = 1;
            v->focus = BUI_W_NONE;
            break;
        case G_EDITOR_BUSY:
            v->screen = BUI_SCR_EDITOR;
            v->cs.canWrite = 0;
            snprintf(v->cs.status, sizeof(v->cs.status), "IMPORTED SHARE1 INTO CUP SLOT 3");
            v->focus = BUI_W_NONE;
            break;
        case G_CS_FILES:
            CsFiles(v);
            break;
        case G_CS_FILES_EMPTY:
            v->screen = BUI_SCR_CS_FILES;
            v->cs.canWrite = 1;
            v->cs.page = 1;
            v->cs.pages = 1;
            v->focus = BUI_W_CS_RESCAN;
            break;
        case G_CS_SLOT:
            v->screen = BUI_SCR_CS_SLOT;
            CsPending(v);
            v->focus = BUI_W_CS_CUP0 + 1;
            break;
        case G_CS_SCAN:
            v->screen = BUI_SCR_CS_SCAN;
            v->cs.canWrite = 1;
            v->cs.cameraOn = 1;
            v->cs.scanHave = 2;
            v->cs.scanCount = 5;
            v->cs.scanPages = 0x5;
            v->cs.scanZoom = 1;
            snprintf(v->cs.status, sizeof(v->cs.status), "PAGE 3 OF 5 READ");
            v->focus = BUI_W_NONE;
            break;
        case G_E_CS_REPLACE:
            v->screen = BUI_SCR_CS_SLOT;
            CsPending(v);
            v->focus = BUI_W_CS_CUP0 + 2;
            Dialog(v, BUI_DLG_CS_REPLACE, 1, "SHARE1", "CUP SLOT 3");
            break;
        case G_E_FULL:
            NearbyRooms(v);
            snprintf(v->toast, sizeof(v->toast), "KENJI'S ROOM IS FULL (4/4)");
            v->toastTone = BUI_TONE_WARN;
            v->toastIcon = BUI_ICON_PEOPLE;
            break;
        default:
            break;
    }
}
