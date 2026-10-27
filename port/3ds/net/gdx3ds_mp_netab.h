/* port/3ds/net/gdx3ds_mp_netab.h — card-3: the 3DS glue of the online levers' per-race A/B (the
 * clock, the values and the receipt: port/gdx_mp_netab.h).
 *
 * gdx3ds_mp_online.c latches the race at every START it takes (BeginLaunch, host and joiners
 * alike, before the phase goes to launch), so the arm is known before NDM is applied (the launch
 * phase change runs gdx3ds_mp_radio_update) and before time sync reads its cap at race tick 0
 * (gdx3ds_mpts_race_begin). Between races the last race's arm stays in force; before the room's
 * first START it is race 0 (arm A). Main thread only. */
#ifndef GDX3DS_MP_NETAB_H
#define GDX3DS_MP_NETAB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A START was taken: latch its race and print the [netab] receipt (only with a lever on A/B).
 * infra: the room runs over WiFi infrastructure (online / LAN), not local wireless. */
void gdx3ds_mpab_race_start(uint32_t startGen, int infra);
/* [net] mp_timesync_max_ms as a lever value ("ab" or -1 = A/B; else 0..16, default 3). */
int gdx3ds_mpab_timesync_lever(void);
/* [net] mp_ndm_exclusive as a lever value (0, 1, 2 = A/B; default 1). */
int gdx3ds_mpab_ndm_lever(void);
/* The value each lever runs in the latched race. */
int gdx3ds_mpab_timesync_cap_ms(int lever);
int gdx3ds_mpab_ndm_on(int lever);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_NETAB_H */
