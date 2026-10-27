/* port/3ds/gdx3ds_bui_script.c — scripted input for the bottom-screen menus ([debug] bui_input).
 *
 * A text file of steps, run from the first menu tick (one step check per tick, wall-clock time):
 *   <secs> [BUTTON,BUTTON,...|-] [cx cy] [@x,y]   hold buttons / the circle pad (-1..1) / a touch
 *                                                 for <secs> (tools/dev/hw.py's input-script format)
 *   SHOT <label>                                  gdx_request_frame_dump(label): top + bottom BMPs
 *   WAIT <what> [secs]                            until gdx3ds_bui_condition(what) (a screen name,
 *                                                 rooms / startable / ready / race / peers / ambient,
 *                                                 player-settings,
 *                                                 picking / picked / step:<s> / top:<mode>; "!" in
 *                                                 front negates), default 30 s; a timeout logs and
 *                                                 goes on
 *   LOG <text>                                    a `[bui-script]` line
 * Buttons: A B SELECT START RIGHT LEFT UP DOWN R L X Y (bit = KEY_*). Scripted keys are ORed into
 * the real ones, so a console run with a script can still be steered by hand. Comments: '#'. */
#include "gdx3ds_bui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

extern void gdx_request_frame_dump(const char* label) __attribute__((weak));

#define SCRIPT_MAX_STEPS 512
#define SCRIPT_WAIT_DEFAULT_MS 30000

enum { STEP_HOLD = 0, STEP_SHOT, STEP_WAIT, STEP_LOG };

typedef struct ScriptStep {
    uint8_t kind;
    uint8_t touch;
    int16_t tx, ty;
    int16_t cx, cy;
    uint32_t keys;
    uint32_t ms;
    char text[40];
} ScriptStep;

static ScriptStep* sSteps = NULL;
static int sCount = 0;
static int sIndex = 0;
static int sStarted = 0;
static int sActive = 0;
static u64 sStepStart = 0;
static unsigned sStepTicks = 0; /* menu ticks into the current step: a hold lasts >= 3 of them */
static unsigned sWaitFails = 0;

static void ScriptLog(const char* fmt, ...) {
    char line[160];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n >= (int) sizeof(line)) {
        n = (int) sizeof(line) - 1;
    }
    svcOutputDebugString(line, n);
    gdx3ds_filelog_write(line, (size_t) n);
}

static uint32_t ButtonBit(const char* name) {
    static const struct {
        const char* name;
        uint32_t bit;
    } kButtons[] = {
        {"A", KEY_A},         {"B", KEY_B},          {"SELECT", KEY_SELECT}, {"START", KEY_START},
        {"RIGHT", KEY_DRIGHT}, {"LEFT", KEY_DLEFT},  {"UP", KEY_DUP},        {"DOWN", KEY_DDOWN},
        {"R", KEY_R},         {"L", KEY_L},          {"X", KEY_X},           {"Y", KEY_Y},
    };
    size_t i;
    for (i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
        if (strcmp(name, kButtons[i].name) == 0) {
            return kButtons[i].bit;
        }
    }
    return 0;
}

static uint32_t ParseButtons(char* list, int lineNo) {
    uint32_t keys = 0;
    char* save = NULL;
    char* tok;
    if (strcmp(list, "-") == 0 || strcmp(list, "NONE") == 0) {
        return 0;
    }
    for (tok = strtok_r(list, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        const uint32_t bit = ButtonBit(tok);
        if (bit == 0) {
            ScriptLog("[bui-script] line %d: unknown button %s (ignored)", lineNo, tok);
        }
        keys |= bit;
    }
    return keys;
}

static int16_t CircleAxis(const char* s) {
    double v = atof(s);
    if (v > 1.0) {
        v = 1.0;
    } else if (v < -1.0) {
        v = -1.0;
    }
    return (int16_t) (v * 156.0);
}

/* "<secs> [buttons] [cx cy] [@x,y]" */
static int ParseHold(ScriptStep* st, char** tok, int n, int lineNo) {
    int i;
    int pos = 0;
    memset(st, 0, sizeof(*st));
    st->kind = STEP_HOLD;
    for (i = 0; i < n; i++) {
        if (tok[i][0] == '@') {
            int x = 0, y = 0;
            if (sscanf(tok[i] + 1, "%d,%d", &x, &y) != 2) {
                ScriptLog("[bui-script] line %d: touch must be @x,y", lineNo);
                return 0;
            }
            st->touch = 1;
            st->tx = (int16_t) x;
            st->ty = (int16_t) y;
            continue;
        }
        if (pos == 0) {
            st->ms = (uint32_t) (atof(tok[i]) * 1000.0);
        } else if (pos == 1) {
            st->keys = ParseButtons(tok[i], lineNo);
        } else if (pos == 2) {
            st->cx = CircleAxis(tok[i]);
        } else if (pos == 3) {
            st->cy = CircleAxis(tok[i]);
        }
        pos++;
    }
    return pos > 0;
}

static int ParseCommand(ScriptStep* st, char** tok, int n) {
    memset(st, 0, sizeof(*st));
    if (strcmp(tok[0], "SHOT") == 0 && n >= 2) {
        st->kind = STEP_SHOT;
    } else if (strcmp(tok[0], "WAIT") == 0 && n >= 2) {
        st->kind = STEP_WAIT;
        st->ms = n >= 3 ? (uint32_t) (atof(tok[2]) * 1000.0) : SCRIPT_WAIT_DEFAULT_MS;
    } else if (strcmp(tok[0], "LOG") == 0 && n >= 2) {
        st->kind = STEP_LOG;
    } else {
        return 0;
    }
    snprintf(st->text, sizeof(st->text), "%s", tok[1]);
    return 1;
}

static void ParseLine(char* line, int lineNo) {
    char* tok[8];
    int n = 0;
    char* save = NULL;
    char* hash = strchr(line, '#');
    char* t;
    if (hash != NULL) {
        *hash = '\0';
    }
    for (t = strtok_r(line, " \t\r\n", &save); t != NULL && n < 8; t = strtok_r(NULL, " \t\r\n", &save)) {
        tok[n++] = t;
    }
    if (n == 0 || sCount >= SCRIPT_MAX_STEPS) {
        return;
    }
    if ((tok[0][0] >= '0' && tok[0][0] <= '9') || tok[0][0] == '.') {
        if (ParseHold(&sSteps[sCount], tok, n, lineNo)) {
            sCount++;
        }
    } else if (ParseCommand(&sSteps[sCount], tok, n)) {
        sCount++;
    } else {
        ScriptLog("[bui-script] line %d: not understood: %s", lineNo, tok[0]);
    }
}

void gdx3ds_bui_script_init(void) {
    const char* path = gdx3ds_config_get_string("debug", "bui_input", "");
    char line[160];
    int lineNo = 0;
    FILE* fp;
    if (path == NULL || path[0] == '\0' || sSteps != NULL) {
        return;
    }
    fp = fopen(path, "r");
    if (fp == NULL) {
        ScriptLog("[bui-script] cannot open %s", path);
        return;
    }
    sSteps = (ScriptStep*) calloc(SCRIPT_MAX_STEPS, sizeof(ScriptStep));
    if (sSteps == NULL) {
        fclose(fp);
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        ParseLine(line, ++lineNo);
    }
    fclose(fp);
    sActive = sCount > 0;
    ScriptLog("[bui-script] %s: %d steps", path, sCount);
}

static int RunCommand(const ScriptStep* st, u64 now) {
    const u64 elapsedMs = (now - sStepStart) / CPU_TICKS_PER_MSEC;
    switch (st->kind) {
        case STEP_SHOT:
            ScriptLog("[bui-script] SHOT %s (screen=%s)", st->text, gdx3ds_bui_screen_name());
            if (&gdx_request_frame_dump != NULL) {
                gdx_request_frame_dump(st->text);
            }
            return 1;
        case STEP_LOG:
            ScriptLog("[bui-script] LOG %s (screen=%s)", st->text, gdx3ds_bui_screen_name());
            return 1;
        case STEP_WAIT:
            if (gdx3ds_bui_condition(st->text)) {
                ScriptLog("[bui-script] WAIT %s ok after %lu ms", st->text, (unsigned long) elapsedMs);
                return 1;
            }
            if (elapsedMs >= st->ms) {
                sWaitFails++;
                ScriptLog("[bui-script] WAIT %s TIMEOUT after %lu ms (screen=%s)", st->text,
                          (unsigned long) elapsedMs, gdx3ds_bui_screen_name());
                return 1;
            }
            return 0;
        default:
            return elapsedMs >= st->ms && sStepTicks >= 3; /* an emulated clock can race ahead */
    }
}

void gdx3ds_bui_script_step(void) {
    const u64 now = svcGetSystemTick();
    int guard;
    if (!sActive) {
        return;
    }
    if (!sStarted) {
        sStarted = 1;
        sStepStart = now;
    }
    sStepTicks++;
    for (guard = 0; guard < 16 && sIndex < sCount; guard++) {
        if (!RunCommand(&sSteps[sIndex], now)) {
            return;
        }
        sIndex++;
        sStepStart = now;
        sStepTicks = 0;
    }
    if (sIndex >= sCount) {
        sActive = 0;
        ScriptLog("[bui-script] done (%d steps, %u wait timeouts)", sCount, sWaitFails);
    }
}

int gdx3ds_bui_script_active(void) {
    return sActive;
}

static const ScriptStep* CurrentHold(void) {
    if (!sActive || !sStarted || sIndex >= sCount || sSteps[sIndex].kind != STEP_HOLD) {
        return NULL;
    }
    return &sSteps[sIndex];
}

uint32_t gdx3ds_bui_script_keys(uint32_t held) {
    const ScriptStep* st = CurrentHold();
    if (st == NULL) {
        return held;
    }
    return held | st->keys | (st->touch ? (uint32_t) KEY_TOUCH : 0u);
}

int gdx3ds_bui_script_touch(int* px, int* py) {
    const ScriptStep* st = CurrentHold();
    if (st == NULL || !st->touch) {
        return 0;
    }
    *px = st->tx;
    *py = st->ty;
    return 1;
}

int gdx3ds_bui_script_circle(int* dx, int* dy) {
    const ScriptStep* st = CurrentHold();
    if (st == NULL || (st->cx == 0 && st->cy == 0)) {
        return 0;
    }
    *dx = st->cx;
    *dy = st->cy;
    return 1;
}
