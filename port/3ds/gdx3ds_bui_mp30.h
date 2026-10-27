/* port/3ds/gdx3ds_bui_mp30.h — the lobby's MACHINES setting (mp30: up to 30 racers).
 *
 * The design merges VS COM and mp30's racer count into one control: OFF (humans only) or a field
 * of 4 / 8 / 16 / 30 filled with CPU machines. mp30's side is GdxMpoUi.vsCom / .racers /
 * .racersCap, gdx3ds_mpo_ui_set_vscom(on) and gdx3ds_mpo_ui_cycle_racers(dir): racers is the count
 * a VS COM START would carry (always 4..30, whatever vsCom says) and racersCap the most the room
 * allows now (16 while an Old 3DS is in it, unless [net] mp_racers_o3ds_30). The screens
 * (gdx3ds_bui_screens.c) only ever see racers / racersCap / racersLive. With GDX_MPO_MIN_RACERS
 * (mp30 is in the build) the first half compiles, else the 4-machine stub. */
#ifndef GDX3DS_BUI_MP30_H
#define GDX3DS_BUI_MP30_H

#include "gdx3ds_mp_online.h"

#if defined(GDX_MPO_MIN_RACERS)

/* mp30: OFF while VS COM is off, else the field VS COM would fill (never above the cap). */
static inline int BuiMpRacers(const GdxMpoUi* ui) {
    if (!ui->vsCom) {
        return 0;
    }
    return ui->racers;
}

static inline int BuiMpRacersCap(const GdxMpoUi* ui) {
    return ui->racersCap;
}

static inline int BuiMpRacersLive(void) {
    return 1;
}

/* OFF = VS COM off. A count = VS COM on, then mp30's 4 / 8 / 16 / 30 cycle stepped to it; one the
 * cap rules out (an Old 3DS joined between the paint and the tap) stops at the cap. */
static inline void BuiMpSetRacers(int target) {
    GdxMpoUi ui;
    int guard;
    gdx3ds_mpo_ui(&ui);
    if (!ui.isHost) {
        return;
    }
    gdx3ds_mpo_ui_set_vscom(target > 0);
    if (target <= 0) {
        return;
    }
    if (target > ui.racersCap) {
        target = ui.racersCap;
    }
    for (guard = 0; guard < 8; guard++) {
        gdx3ds_mpo_ui(&ui);
        if (ui.racers == target) {
            return;
        }
        gdx3ds_mpo_ui_cycle_racers(target > ui.racers ? 1 : -1);
    }
}

#else

/* Stub: VS COM on = a field of 4 (CPU racers fill the empty seats), off = humans only. */
static inline int BuiMpRacers(const GdxMpoUi* ui) {
    return ui->vsCom ? 4 : 0;
}

static inline int BuiMpRacersCap(const GdxMpoUi* ui) {
    (void) ui;
    return 4;
}

static inline int BuiMpRacersLive(void) {
    return 0;
}

static inline void BuiMpSetRacers(int target) {
    gdx3ds_mpo_ui_set_vscom(target > 0);
}

#endif

#endif /* GDX3DS_BUI_MP30_H */
