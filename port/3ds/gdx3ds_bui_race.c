/* port/3ds/gdx3ds_bui_race.c — the multiplayer race dash's shell. Contract: gdx3ds_bui_race.h. */
#include "gdx3ds_bui_race.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_bottomhud.h"
#include "gdx3ds_bui.h"
#include "gdx3ds_bui_kit.h"
#include "gdx3ds_bui_racedash.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_mp_online.h"
#include "gdx_racemap.h"

#define RACE_FIELD_EVERY 6      /* ticks between field samples on the dash (10 Hz) */
#define RACE_STANDINGS_EVERY 15 /* ticks between overlay updates (4 Hz) */
#define RACE_TOAST_TICKS 180    /* the drop toast's 3 s */
#define RACE_RECEIPT_TICKS 256

static BuiRaceView sView;
static int sViewValid = 0;
static unsigned long sTicks = 0;
static unsigned long sFieldAt = 0;

/* What the column shows now: the "you" cell and each row's (player, place). */
static int sShownMyPlace = -1;
static int sShownCount = -1;
static int sShownRow[BUI_RACE_HUMANS_MAX][3]; /* player, place, removed */
static int sColumnValid = 0;

static int sDropSerialSeen = 0;
static char sToastName[BUI_RACE_NAME_CAP + 1];
static int sToastCpu = 0;
static unsigned long sToastUntil = 0;
static int sToastOn = 0;

static int sOverlayOpen = 0;
static int sOverlayFull = 0;
static int sOverlayPrevTouch = 0;
static unsigned long sOverlayAt = 0;
static BuiRaceRacer sShownStanding[BUI_RACE_FIELD_MAX];
static int sShownStandingMe[BUI_RACE_FIELD_MAX];
static int sShownLeaderLap = -1;

static int sPrevTouch = 0;
static int sRebake = 0;
static BuiRacePause sPauseShownView; /* what the pause card shows now */
static int sPauseOn = 0;
static unsigned long sPauseTicks = 0;
static int sDashShown = 0; /* the MP dash owns the bottom screen (the stall banner may paint) */
static int sStallOn = 0;

/* [debug] bui_race_shots=1 (test lever, default off): SHOTs the emulator gates read --
 * autotest/mp_dash_R.bmp 20 s into each race, mp_toast_R.bmp half a second into a drop toast,
 * mp_standings_R.bmp once the overlay first painted, mp_stall_R.bmp at 1.4 s into a stall. */
static int sShotsLever = -1;
static int sShotsTaken = 0; /* bits: 1 dash, 2 toast, 4 standings, 8 stall */
static int sRaceSerial = 0;

/* The NET chip's input delay ("D8", lockstep frames) is a developer number: shown only with
 * [debug] dev_console=1; players get the link bars alone. */
static int sShowDelay = -1;

static int ShowDelay(void) {
    if (sShowDelay < 0) {
        sShowDelay = gdx3ds_config_get_bool("debug", "dev_console", 0) ? 1 : 0;
    }
    return sShowDelay;
}

static int ShotsOn(void) {
    if (sShotsLever < 0) {
        sShotsLever = gdx3ds_config_get_int("debug", "bui_race_shots", 0) != 0;
    }
    return sShotsLever;
}

/* One SHOT of the shadow as it is now (label_R.bmp), once per race per bit. */
static void AutoShot(int bit, const char* label) {
    char name[32];
    if (!ShotsOn() || (sShotsTaken & bit)) {
        return;
    }
    sShotsTaken |= bit;
    snprintf(name, sizeof(name), "%s%d", label, sRaceSerial);
    gdx3ds_bottomhud_note_shot(name);
    gdx3ds_bottomhud_service_shot(gdx3ds_bottomhud_shadow_fb());
}

static unsigned long sRcDashTicks = 0;
static u64 sRcDashTotal = 0;
static u64 sRcDashMax = 0;
static unsigned long sRcStTicks = 0;
static u64 sRcStTotal = 0;
static u64 sRcStMax = 0;

static void RaceLog(const char* fmt, ...) {
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

static unsigned long TicksToUs(u64 t) {
    return (unsigned long) ((t * 1000u) / (u64) CPU_TICKS_PER_MSEC);
}

/* Adds one sample to a receipt window; logs and restarts it every RACE_RECEIPT_TICKS. */
static void ReceiptAdd(const char* what, unsigned long* ticks, u64* total, u64* max, u64 dt) {
    (*ticks)++;
    *total += dt;
    if (dt > *max) {
        *max = dt;
    }
    if ((*ticks % RACE_RECEIPT_TICKS) == 0) {
        RaceLog("[bui] %s ticks=%lu avg_us=%lu max_us=%lu", what, *ticks, TicksToUs(*total / RACE_RECEIPT_TICKS),
                TicksToUs(*max));
        *total = 0;
        *max = 0;
    }
}

/* ---- the view ------------------------------------------------------------------------------- */

static void FillRacers(const GdxRaceFieldSnapshot* f, int numPlayers) {
    int i;
    sView.count = (int8_t) f->count;
    sView.totalLaps = (int8_t) f->totalLaps;
    for (i = 0; i < f->count && i < BUI_RACE_FIELD_MAX; i++) {
        const GdxRaceFieldEntry* e = &f->byPos[i];
        BuiRaceRacer* r = &sView.byPos[i];
        r->character = e->character;
        r->human = (int8_t) ((e->racer >= 0 && e->racer < numPlayers) ? e->racer + 1 : 0);
        r->lap = e->lap;
        r->state = (int8_t) (e->state & GDX_RACEFIELD_FINISHED ? BUI_RACER_FINISHED
                             : e->state & GDX_RACEFIELD_OUT     ? BUI_RACER_OUT
                                                                : 0);
        r->col = e->col;
        r->row = e->row;
        r->gapMs = e->gapMs;
    }
}

static void FillSession(const GdxMpoRaceView* m) {
    int p;
    sView.numPlayers = (int8_t) m->numPlayers;
    sView.me = (int8_t) m->localPlayer;
    sView.linkBars = (int8_t) m->linkBars;
    sView.delay = ShowDelay() ? (int8_t) m->delay : (int8_t) -1;
    sView.removedMask = (uint8_t) m->removedMask;
    for (p = 0; p < BUI_RACE_HUMANS_MAX; p++) {
        snprintf(sView.name[p], sizeof(sView.name[p]), "%s", m->name[p]);
    }
}

/* The drop toast starts when the session's removal count moves. */
static void WatchDrops(const GdxMpoRaceView* m) {
    if (m->dropSerial == sDropSerialSeen) {
        return;
    }
    sDropSerialSeen = m->dropSerial;
    if (m->dropPlayer < 0 || m->dropPlayer >= BUI_RACE_HUMANS_MAX) {
        return;
    }
    snprintf(sToastName, sizeof(sToastName), "%s", m->name[m->dropPlayer]);
    sToastCpu = m->dropCpu;
    sToastUntil = sTicks + RACE_TOAST_TICKS;
    sToastOn = 1;
    RaceLog("[bui] mpdash toast: %s dropped (%s)", sToastName, sToastCpu ? "cpu drives" : "out");
}

static void Refresh(int withMap) {
    GdxRaceFieldSnapshot f;
    GdxMpoRaceView m;
    gdx3ds_mpo_race_view(&m);
    gdx_racefield_snapshot(&f, withMap);
    if (!m.live || !f.valid) {
        return;
    }
    FillSession(&m);
    FillRacers(&f, m.numPlayers);
    WatchDrops(&m);
    sViewValid = 1;
    sFieldAt = sTicks;
}

static void RefreshIfDue(void) {
    if (!sViewValid || sTicks - sFieldAt >= RACE_FIELD_EVERY) {
        Refresh(0);
    }
}

/* ---- the dash ------------------------------------------------------------------------------- */

int gdx3ds_buirace_active(void) {
    return gdx3ds_bui_enabled() && gdx3ds_mpo_local_player() >= 0;
}

void gdx3ds_buirace_on_bake(uint16_t* shadow) {
    bui_kit_begin(shadow);
    Refresh(0);
    bui_race_column_frame();
    bui_race_standings_button();
    if (sViewValid) {
        bui_race_header_field(&sView, 232, 316, 219, 20);
    }
    sColumnValid = 0;
}

void gdx3ds_buirace_paint_map(uint16_t* shadow, int x0, int y0, int px) {
    bui_kit_begin(shadow);
    Refresh(1);
    if (!sViewValid) {
        return;
    }
    bui_race_map_dots(&sView, x0, y0, px);
    bui_race_net_chip(&sView, x0, y0);
}

static int RowChanged(int i, const int* order, int n) {
    const int player = i < n ? order[i] : -1;
    const int place = player >= 0 ? bui_race_place_of(&sView, player) : 0;
    const int removed = player >= 0 ? (sView.removedMask >> player) & 1 : 0;
    if (sColumnValid && sShownRow[i][0] == player && sShownRow[i][1] == place && sShownRow[i][2] == removed) {
        return 0;
    }
    sShownRow[i][0] = player;
    sShownRow[i][1] = place;
    sShownRow[i][2] = removed;
    return 1;
}

static void PaintColumn(void) {
    int order[BUI_RACE_HUMANS_MAX];
    const int n = bui_race_humans_by_place(&sView, order);
    const int myPlace = bui_race_place_of(&sView, sView.me);
    int i;
    if (!sColumnValid || myPlace != sShownMyPlace || sView.count != sShownCount) {
        bui_race_column_you(&sView);
        sShownMyPlace = myPlace;
        sShownCount = sView.count;
    }
    for (i = 0; i < BUI_RACE_HUMANS_MAX; i++) {
        if (RowChanged(i, order, n)) {
            bui_race_column_row(&sView, i);
        }
    }
    sColumnValid = 1;
}

static void PaintToast(void) {
    if (!sToastOn) {
        return;
    }
    if (sTicks >= sToastUntil) {
        sToastOn = 0;
        sRebake = 1; /* the strip under it comes back with the next tick's rebake */
        return;
    }
    bui_race_drop_toast(sToastName, sToastCpu);
    if (sToastUntil - sTicks <= RACE_TOAST_TICKS - 30) {
        AutoShot(2, "mp_toast");
    }
}

void gdx3ds_buirace_paint(uint16_t* shadow) {
    const u64 t0 = svcGetSystemTick();
    sTicks++;
    sDashShown = 1;
    bui_kit_begin(shadow);
    RefreshIfDue();
    if (sViewValid) {
        PaintColumn();
        PaintToast();
    }
    ReceiptAdd("mpdash", &sRcDashTicks, &sRcDashTotal, &sRcDashMax, svcGetSystemTick() - t0);
    if (sTicks >= 1200 && sViewValid && !sToastOn) {
        AutoShot(1, "mp_dash"); /* the shadow is the whole frame the DASH presents this tick */
    }
}

int gdx3ds_buirace_touch(int touching, int px, int py) {
    const int inside = touching && px >= BUI_RACE_STANDINGS_X && px < BUI_RACE_STANDINGS_X + BUI_RACE_STANDINGS_W &&
                       py >= BUI_RACE_STANDINGS_Y && py < BUI_RACE_STANDINGS_Y + BUI_RACE_STANDINGS_H;
    const int down = inside && !sPrevTouch;
    sPrevTouch = touching;
    if (!down) {
        return 0;
    }
    sOverlayOpen = 1;
    sOverlayFull = 1;
    sOverlayPrevTouch = 1; /* the opening tap must lift before a tap closes it */
    RaceLog("[bui] mpdash: standings open");
    return 1;
}

/* ---- the standings overlay ------------------------------------------------------------------ */

static int StandingChanged(int i) {
    const BuiRaceRacer* r = &sView.byPos[i];
    const int me = r->human != 0 && r->human - 1 == sView.me;
    const int tenthsNow = (r->gapMs + 50) / 100;
    const int tenthsWas = (sShownStanding[i].gapMs + 50) / 100;
    if (memcmp(r, &sShownStanding[i], sizeof(*r)) == 0 && sShownStandingMe[i] == me) {
        return 0;
    }
    if (r->character == sShownStanding[i].character && r->human == sShownStanding[i].human &&
        r->lap == sShownStanding[i].lap && r->state == sShownStanding[i].state && tenthsNow == tenthsWas &&
        sShownStandingMe[i] == me) {
        return 0; /* only the map cell moved: nothing the row shows */
    }
    sShownStanding[i] = *r;
    sShownStandingMe[i] = me;
    return 1;
}

static void PaintStandings(void) {
    int i;
    if (sOverlayFull) {
        bui_race_standings_frame(&sView);
        memset(sShownStanding, 0xFF, sizeof(sShownStanding));
        sShownLeaderLap = -1;
        sOverlayFull = 0;
    }
    if (sView.count > 0 && sView.byPos[0].lap != sShownLeaderLap) {
        bui_race_standings_lap(&sView);
        sShownLeaderLap = sView.byPos[0].lap;
    }
    for (i = 0; i < BUI_RACE_FIELD_MAX; i++) {
        if (i >= sView.count) {
            if (sShownStanding[i].human != -2) {
                bui_race_standings_row(&sView, i);
                sShownStanding[i].human = -2;
            }
            continue;
        }
        if (StandingChanged(i)) {
            bui_race_standings_row(&sView, i);
        }
    }
}

static void PresentDirty(uint16_t* live, const uint16_t* shadow) {
    int x0, x1;
    if (!bui_kit_dirty(&x0, &x1)) {
        return;
    }
    memcpy(live + (size_t) x0 * BUI_H, shadow + (size_t) x0 * BUI_H, (size_t) (x1 - x0 + 1) * BUI_H * sizeof(uint16_t));
}

static void CloseOverlay(void) {
    sOverlayOpen = 0;
    sRebake = 1;
    sColumnValid = 0;
    RaceLog("[bui] mpdash: standings closed");
}

int gdx3ds_buirace_overlay_tick(uint16_t* shadow, uint16_t* live, int touching) {
    u64 t0;
    const int down = touching && !sOverlayPrevTouch;
    sOverlayPrevTouch = touching;
    if (!sOverlayOpen) {
        return 0;
    }
    if (down) {
        CloseOverlay();
        return 0;
    }
    t0 = svcGetSystemTick();
    sTicks++;
    sDashShown = 1;
    bui_kit_begin(shadow);
    bui_kit_dirty_clear();
    if (sOverlayFull || !sViewValid || sTicks - sOverlayAt >= RACE_STANDINGS_EVERY) {
        Refresh(0);
        sOverlayAt = sTicks;
        if (sViewValid) {
            PaintStandings();
        }
    }
    PresentDirty(live, shadow);
    if (sViewValid) {
        AutoShot(4, "mp_standings");
    }
    ReceiptAdd("standings", &sRcStTicks, &sRcStTotal, &sRcStMax, svcGetSystemTick() - t0);
    return 1;
}

int gdx3ds_buirace_take_rebake(void) {
    const int r = sRebake;
    sRebake = 0;
    return r;
}

void gdx3ds_buirace_reset(void) {
    sViewValid = 0;
    sColumnValid = 0;
    sShownMyPlace = -1;
    sShownCount = -1;
    sDropSerialSeen = 0;
    sToastOn = 0;
    sOverlayOpen = 0;
    sOverlayFull = 0;
    sRebake = 0;
    sDashShown = 0;
    sStallOn = 0;
    sPauseOn = 0;
    sPrevTouch = 0;
    sShotsTaken = 0;
    sRaceSerial++;
}

/* ---- the shared pause (mp-menus #10) --------------------------------------------------------- */

static void FillPause(const GdxMpoPauseView* m, BuiRacePause* p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", m->name);
    p->player = (int8_t) m->player;
    p->mine = (int8_t) m->mine;
    p->endsRace = (int8_t) m->endsRace;
    snprintf(p->option, sizeof(p->option), "%s", m->option);
    p->holdPermille = (int16_t) m->holdPermille;
}

static int InPauseButton(int touching, int px, int py) {
    return touching && px >= BUI_RACE_PAUSE_BTN_X && px < BUI_RACE_PAUSE_BTN_X + BUI_RACE_PAUSE_BTN_W &&
           py >= BUI_RACE_PAUSE_BTN_Y && py < BUI_RACE_PAUSE_BTN_Y + BUI_RACE_PAUSE_BTN_H;
}

static void EndPause(void) {
    sPauseOn = 0;
    sRebake = 1;
    sColumnValid = 0;
    sOverlayFull = sOverlayOpen;
    gdx3ds_mpo_pause_touch(0);
    RaceLog("[bui] mpdash: pause card closed");
}

/* The pauser's A answers the top screen's menu even while the game has the buttons: the card
 * shows the glyph whatever the kit's glyph switch says. */
static void PaintPauseWithGlyphs(const BuiRacePause* p) {
    const int off = bui_kit_glyphs_off();
    bui_kit_set_glyphs_off(0);
    bui_race_pause(p);
    bui_kit_set_glyphs_off(off);
}

static void PaintPauseIfChanged(const BuiRacePause* p) {
    if (sPauseOn && memcmp(p, &sPauseShownView, sizeof(*p)) == 0) {
        return;
    }
    if (!sPauseOn) {
        RaceLog("[bui] mpdash: pause card (paused by %s%s)", p->name, p->mine ? ", this console" : "");
    }
    sPauseShownView = *p;
    sPauseOn = 1;
    PaintPauseWithGlyphs(p);
}

int gdx3ds_buirace_pause_tick(uint16_t* shadow, uint16_t* live, int touching, int px, int py) {
    GdxMpoPauseView m;
    BuiRacePause p;
    gdx3ds_mpo_pause_view(&m);
    if (!m.paused) {
        if (sPauseOn) {
            EndPause();
        }
        return 0;
    }
    gdx3ds_mpo_pause_touch(m.mine && InPauseButton(touching, px, py));
    FillPause(&m, &p);
    bui_kit_begin(shadow);
    bui_kit_dirty_clear();
    if (!sPauseOn) {
        sPauseTicks = 0;
    }
    sPauseTicks++;
    PaintPauseIfChanged(&p);
    PresentDirty(live, shadow);
    if (sPauseTicks >= 60) {
        AutoShot(16, "mp_pause");
    }
    if (p.holdPermille >= 500) {
        AutoShot(32, "mp_pause_hold");
    }
    return 1;
}

/* ---- the stall banner (called from inside a stalled race tick) ------------------------------ */

/* Paints into the shadow and straight into the live screen: the main loop is blocked in the MP
 * layer's WaitReady, so nothing else will present it. */
static void StallPresent(void) {
    uint16_t* shadow = gdx3ds_bottomhud_shadow_fb();
    uint16_t* live = gdx3ds_bottomhud_live_fb();
    int x0, x1;
    if (live == NULL || !bui_kit_dirty(&x0, &x1)) {
        return;
    }
    memcpy(live + (size_t) x0 * BUI_H, shadow + (size_t) x0 * BUI_H, (size_t) (x1 - x0 + 1) * BUI_H * sizeof(uint16_t));
    GSPGPU_FlushDataCache(live + (size_t) x0 * BUI_H, (u32) ((size_t) (x1 - x0 + 1) * BUI_H * sizeof(uint16_t)));
}

void gdx3ds_bui_race_stall(const char* name, int waitedMs, int dropAfterMs) {
    if (!sDashShown || gdx3ds_bottomhud_dismissed() || !gdx3ds_bui_enabled()) {
        return;
    }
    if (name == NULL) {
        if (sStallOn) {
            sStallOn = 0;
            sRebake = 1;
            sOverlayFull = sOverlayOpen;
            RaceLog("[bui] mpdash: stall over");
        }
        return;
    }
    if (!sStallOn) {
        RaceLog("[bui] mpdash: stall banner (waiting for %s)", name);
    }
    sStallOn = 1;
    bui_kit_begin(gdx3ds_bottomhud_shadow_fb());
    bui_kit_dirty_clear();
    bui_race_stall(name, waitedMs, dropAfterMs);
    StallPresent();
    if (waitedMs >= 1400) {
        AutoShot(8, "mp_stall");
    }
}
