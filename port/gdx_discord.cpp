// G-Diffuser — Discord Rich Presence.
//
// Vendored discord-rpc (port/third_party/discord-rpc, MIT, upstream HEAD 963aa9f3). The library's
// IO thread owns the socket and the jittered reconnect backoff (500ms..60s), so a machine without
// Discord running costs one non-blocking pipe-open per backoff interval, never on the frame loop.
//
// Two orderings are deliberate, both copied from ports that got them wrong:
//  - Teardown is ClearPresence -> RunCallbacks -> Shutdown (PCSX2's order). The middle call
//    flushes the clear before the socket dies; drop it and the presence stays pinned on Discord
//    after the game exits.
//  - Turning the master toggle off clears the presence immediately rather than merely ceasing
//    updates — a privacy switch that leaves the last state published is a bug.
//
// Privacy model (owner decisions): master CVar gEnhancements.Online.DiscordPresence default OFF;
// per-field toggles default ON once the master is enabled; Course Edit shows a generic label and
// NEVER the user's track name; the timestamp is per-race, not per-session. Nothing derived from
// the filesystem, ROM identity, or save data is ever published.

#include "gdx_discord.h"

#include "discord_rpc.h"
#include "port_log.h" // gdx_port_logf (static inline; an extern declaration does not link)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" int CVarGetInteger(const char* name, int defaultValue);

namespace {

constexpr const char* kClientId = "1535153869141704796"; // Discord application "G-Diffuser"

// Course display names, indexed by the COURSE_* enum (decomp/include/fzx_course.h). X Cup tracks
// are procedurally generated and Edit tracks are user content — both get generic labels.
constexpr int kCourseMax = 56;
const char* const kCourseNames[kCourseMax] = {
    "Mute City",         "Silence",          "Sand Ocean",       "Devil's Forest",  "Big Blue",
    "Port Town",         "Sector Alpha",     "Red Canyon",       "Devil's Forest 2", "Mute City 2",
    "Big Blue 2",        "White Land",       "Fire Field",       "Silence 2",       "Sector Beta",
    "Red Canyon 2",      "White Land 2",     "Mute City 3",      "Rainbow Road",    "Devil's Forest 3",
    "Space Plant",       "Sand Ocean 2",     "Port Town 2",      "Big Hand",        "Edit track",
    "Edit track",        "Edit track",       "Edit track",       "Edit track",      "Edit track",
    "Silence 3",         "Sand Ocean 3",     "Devil's Forest 4", "Port Town 3",     "Devil's Forest 5",
    "Big Blue 3",        "Mute City 4",      "Space Plant 2",    "Port Town 4",     "Fire Field 2",
    "White Land 3",      "Big Foot",         nullptr,            nullptr,           nullptr,
    nullptr,             nullptr,            nullptr,            "X Cup track",     "X Cup track",
    "X Cup track",       "X Cup track",      "X Cup track",      "X Cup track",     "Death Race",
    "Ending",
};

const char* const kCupNames[] = { "Jack Cup", "Queen Cup", "King Cup", "Joker Cup",
                                  "X Cup",    "Edit Cup",  "DD-1 Cup", "DD-2 Cup" };
const char* const kDifficultyNames[] = { "Novice", "Standard", "Expert", "Master" };

// GameMode values (decomp/include/fzx_game.h GameMode enum), redeclared as plain constants so
// this TU stays free of decomp headers.
enum {
    kModeTitle = 0x0,
    kModeGpRace = 0x1,
    kModePractice = 0x2,
    kModeVs2P = 0x3,
    kModeVs3P = 0x4,
    kModeVs4P = 0x5,
    kModeRecords = 0x6,
    kModeMainMenu = 0x7,
    kModeMachineSelect = 0x8,
    kModeMachineSettings = 0x9,
    kModeCourseSelect = 0xA,
    kModeSkippableCredits = 0xB,
    kModeUnskippableCredits = 0xC,
    kModeCourseEdit = 0xD,
    kModeTimeAttack = 0xE,
    kModeGpNextCourse = 0xF,
    kModeCreateMachine = 0x10,
    kModeGpEndCs = 0x11,
    kModeGpNextMachineSettings = 0x12,
    kModeRecordsCourseSelect = 0x13,
    kModeOptions = 0x14,
    kModeDeathRace = 0x15,
    kModeEadDemo = 0x16,
};

bool ModeIsRace(int m) {
    return m == kModeGpRace || m == kModePractice || m == kModeVs2P || m == kModeVs3P ||
           m == kModeVs4P || m == kModeTimeAttack || m == kModeDeathRace;
}

const char* CourseName(int idx) {
    if (idx >= 0 && idx < kCourseMax && kCourseNames[idx] != nullptr) {
        return kCourseNames[idx];
    }
    return "Unknown track";
}

const char* CupName(int cup) {
    if (cup >= 0 && cup < (int)(sizeof(kCupNames) / sizeof(kCupNames[0]))) {
        return kCupNames[cup];
    }
    return "";
}

const char* DifficultyName(int d) {
    if (d >= 0 && d < (int)(sizeof(kDifficultyNames) / sizeof(kDifficultyNames[0]))) {
        return kDifficultyNames[d];
    }
    return "";
}

bool sActive = false;        // Discord_Initialize has run and Shutdown has not
bool sEnabledMirror = false; // last CVar read of the master toggle
int sPollCounter = 0;
uint64_t sLastKey = ~0ull;
time_t sLastPush = 0;
bool sPending = false;
time_t sRaceStart = 0;
int sWasRaceMode = -1;

void HandleReady(const DiscordUser* user) {
    gdx_port_logf("[discord] connected as %s\n", (user != nullptr) ? user->username : "?");
}

void HandleDisconnected(int errcode, const char* message) {
    gdx_port_logf("[discord] disconnected (%d: %s)\n", errcode, (message != nullptr) ? message : "");
}

void HandleError(int errcode, const char* message) {
    gdx_port_logf("[discord] error (%d: %s)\n", errcode, (message != nullptr) ? message : "");
}

void EnsureInit() {
    if (sActive) {
        return;
    }
    DiscordEventHandlers handlers;
    std::memset(&handlers, 0, sizeof(handlers));
    handlers.ready = HandleReady;
    handlers.disconnected = HandleDisconnected;
    handlers.errored = HandleError;
    // join/spectate/joinRequest deliberately null: presence only, no lifecycle complexity.
    Discord_Initialize(kClientId, &handlers, /*autoRegister=*/0, nullptr);
    sActive = true;
    gdx_port_logf("[discord] presence enabled (app %s)\n", kClientId);
}

void TearDown(bool clearFirst) {
    if (!sActive) {
        return;
    }
    sActive = false; // first, so a concurrent entry no-ops
    if (clearFirst) {
        Discord_ClearPresence();
        Discord_RunCallbacks(); // flush the clear before the socket dies
    }
    Discord_Shutdown();
    sLastKey = ~0ull;
    sPending = false;
}

// One integer that changes iff the published state would. Field toggles are folded in so
// flipping a checkbox re-pushes without waiting for game state to move.
uint64_t StateKey(const GdxDiscordSnapshot& s, int showCourse, int showLap, int showPos,
                  int showMode, int showTimer) {
    uint64_t k = 1469598103934665603ull; // FNV-1a
    const int fields[] = { s.mode,      s.titleDemo,       s.courseIndex,   s.cupType,
                           s.difficulty, s.numPlayers,     s.totalLaps,     s.paused,
                           s.playerLap, s.playerPosition,  s.playerFinished, showCourse,
                           showLap,     showPos,           showMode,        showTimer };
    for (int f : fields) {
        k = (k ^ (uint64_t)(uint32_t)f) * 1099511628211ull;
    }
    return k;
}

void BuildAndPush(const GdxDiscordSnapshot& s) {
    const int showCourse = CVarGetInteger("gEnhancements.Online.DiscordShowCourse", 1);
    const int showLap = CVarGetInteger("gEnhancements.Online.DiscordShowLap", 1);
    const int showPos = CVarGetInteger("gEnhancements.Online.DiscordShowPosition", 1);
    const int showMode = CVarGetInteger("gEnhancements.Online.DiscordShowMode", 1);
    const int showTimer = CVarGetInteger("gEnhancements.Online.DiscordShowTimer", 1);

    char details[128] = "Playing F-Zero X";
    char state[128] = "";

    const int m = s.mode;
    const bool race = ModeIsRace(m);

    if (s.titleDemo) {
        std::snprintf(details, sizeof(details), "In the menus");
        std::snprintf(state, sizeof(state), "Attract mode");
    } else if (race) {
        const char* course = showCourse ? CourseName(s.courseIndex) : nullptr;
        const char* modeName = (m == kModeTimeAttack)  ? "Time Attack"
                               : (m == kModePractice)  ? "Practice"
                               : (m == kModeDeathRace) ? "Death Race"
                               : (m >= kModeVs2P && m <= kModeVs4P) ? "VS Battle"
                                                                    : "Grand Prix";
        if (showMode && course != nullptr) {
            std::snprintf(details, sizeof(details), "%s — %s", modeName, course);
        } else if (showMode) {
            std::snprintf(details, sizeof(details), "%s", modeName);
        } else if (course != nullptr) {
            std::snprintf(details, sizeof(details), "Racing on %s", course);
        } else {
            std::snprintf(details, sizeof(details), "In a race");
        }

        char lapPart[32] = "";
        char posPart[32] = "";
        if (showLap && s.playerLap >= 1 && s.totalLaps >= 1 && m != kModeDeathRace) {
            int lap = s.playerLap;
            if (lap > s.totalLaps) {
                lap = s.totalLaps;
            }
            std::snprintf(lapPart, sizeof(lapPart), "Lap %d/%d", lap, s.totalLaps);
        }
        // Position is meaningless with no opponents (Time Attack / Practice).
        if (showPos && m != kModeTimeAttack && m != kModePractice && s.playerPosition >= 1) {
            std::snprintf(posPart, sizeof(posPart), "P%d", s.playerPosition);
        }
        if (s.playerFinished && posPart[0] != '\0') {
            std::snprintf(state, sizeof(state), "Finished %s", posPart);
        } else if (lapPart[0] != '\0' && posPart[0] != '\0') {
            std::snprintf(state, sizeof(state), "%s · %s", posPart, lapPart);
        } else if (lapPart[0] != '\0') {
            std::snprintf(state, sizeof(state), "%s", lapPart);
        } else if (posPart[0] != '\0') {
            std::snprintf(state, sizeof(state), "%s", posPart);
        }
        if (s.paused && state[0] != '\0') {
            const size_t len = std::strlen(state);
            std::snprintf(state + len, sizeof(state) - len, " (Paused)");
        }
    } else if (m == kModeCourseEdit || m == kModeCreateMachine) {
        // User content: generic labels only, never the user's track/machine names.
        std::snprintf(details, sizeof(details), "%s",
                      (m == kModeCourseEdit) ? "Course Edit" : "Create Machine");
    } else if (m == kModeGpEndCs) {
        std::snprintf(details, sizeof(details), "Grand Prix results");
        if (showMode) {
            std::snprintf(state, sizeof(state), "%s — %s", CupName(s.cupType),
                          DifficultyName(s.difficulty));
        }
    } else if (m == kModeMachineSelect || m == kModeMachineSettings ||
               m == kModeGpNextMachineSettings) {
        std::snprintf(details, sizeof(details), "Machine select");
        if (showMode) {
            std::snprintf(state, sizeof(state), "%s — %s", CupName(s.cupType),
                          DifficultyName(s.difficulty));
        }
    } else if (m == kModeCourseSelect || m == kModeGpNextCourse) {
        std::snprintf(details, sizeof(details), "Choosing a course");
        if (showMode) {
            std::snprintf(state, sizeof(state), "%s — %s", CupName(s.cupType),
                          DifficultyName(s.difficulty));
        }
    } else if (m == kModeRecords || m == kModeRecordsCourseSelect) {
        std::snprintf(details, sizeof(details), "Browsing records");
    } else if (m == kModeSkippableCredits || m == kModeUnskippableCredits) {
        std::snprintf(details, sizeof(details), "Ending credits");
    } else if (m == kModeTitle || m == kModeMainMenu || m == kModeOptions || m == kModeEadDemo) {
        std::snprintf(details, sizeof(details), "In the menus");
    }

    DiscordRichPresence presence;
    std::memset(&presence, 0, sizeof(presence));
    presence.details = details;
    presence.state = (state[0] != '\0') ? state : nullptr;
    presence.largeImageKey = "logo"; // optional Art Asset named "logo"; absent = no image, no error
    presence.largeImageText = "G-Diffuser";
    if (showTimer && race && !s.titleDemo && sRaceStart != 0) {
        presence.startTimestamp = (int64_t)sRaceStart; // per-race, never per-session
    }
    Discord_UpdatePresence(&presence);
    Discord_RunCallbacks();
}

} // namespace

extern "C" void gdx_discord_tick(void) {
    // Master toggle, mirrored on a 30-tick cadence so the per-frame cost with presence off is one
    // counter increment and a branch — the CVar string-hash lookup never runs on the hot path.
    if ((sPollCounter++ % 30) == 0) {
        sEnabledMirror = CVarGetInteger("gEnhancements.Online.DiscordPresence", 0) != 0;
    }
    if (!sEnabledMirror) {
        if (sActive) {
            TearDown(/*clearFirst=*/true); // toggle-off clears immediately (privacy expectation)
            gdx_port_logf("[discord] presence disabled by user\n");
        }
        return;
    }
    EnsureInit();

    GdxDiscordSnapshot s;
    gdx_discord_snapshot(&s);
    if (s.modeChanging) {
        return; // transition in flight: hold the previous presence rather than publish a half-state
    }

    const int raceNow = ModeIsRace(s.mode) && !s.titleDemo;
    if (raceNow && sWasRaceMode != 1) {
        sRaceStart = std::time(nullptr);
    } else if (!raceNow) {
        sRaceStart = 0;
    }
    sWasRaceMode = raceNow ? 1 : 0;

    const uint64_t key =
        StateKey(s, CVarGetInteger("gEnhancements.Online.DiscordShowCourse", 1),
                 CVarGetInteger("gEnhancements.Online.DiscordShowLap", 1),
                 CVarGetInteger("gEnhancements.Online.DiscordShowPosition", 1),
                 CVarGetInteger("gEnhancements.Online.DiscordShowMode", 1),
                 CVarGetInteger("gEnhancements.Online.DiscordShowTimer", 1));

    const time_t now = std::time(nullptr);
    if (key == sLastKey && !sPending) {
        return; // nothing changed; presence persists server-side, no periodic refresh needed
    }
    // 5-second floor between pushes. A change inside the window is coalesced, not dropped:
    // sPending guarantees the newest state lands on the first eligible tick.
    if (now - sLastPush < 5) {
        sPending = true;
        sLastKey = key; // remember what we owe so an unchanged tick still flushes it
        return;
    }
    BuildAndPush(s);
    sLastKey = key;
    sLastPush = now;
    sPending = false;
}

extern "C" void gdx_discord_shutdown(void) {
    TearDown(/*clearFirst=*/true);
}
