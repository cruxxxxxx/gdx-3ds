/* port/gdx_mp_netab.h — card-3: the online levers' same-run A/B, one arm per RACE.
 *
 * The render CPU cuts A/B per [cpucut] block (port/3ds/include/gdx3ds_cpucut.h: value 2 = on in
 * the even blocks of the lever's own bit). A network lever cannot: NDM's exclusive state is a room
 * property (entering / leaving it is service IPC, not something to flip mid-race), and time sync
 * paces against the peer, so both consoles must run the SAME arm. The clock here is the race
 * instead: race k = the START's gen - 1 (the lobby's START generation, which the host assigns and
 * every console of the room receives; 0 for the room's first race). A lever on A/B runs arm A in
 * the races whose bit `bit` of k is 0 and arm B where it is 1, so the levers on A/B are orthogonal:
 * over 4 races every combination of the two runs once (the cpucut bit rule, races for blocks).
 *
 * Levers (values other than the A/B one keep their meaning):
 *   [net] mp_timesync_max_ms = ab (or -1): arm A 3 ms, arm B 16 ms; bit 0 (flips every race).
 *   [net] mp_ndm_exclusive   = 2:          arm A 1 (exclusive), arm B 0; bit 1 (every 2 races).
 * Receipt, at every START while either lever is on A/B ("[netab] race ..."): the race index and,
 * per lever, <ini value>/<arm>=<effective value>; the [mpts] race start line (cap_ms=) and the
 * [mpradio] ndm enter / leave lines show each arm taking effect. Network-only: nothing here reaches
 * the simulation (the pads, the lockstep hash). Pure (host-tested: gdx_mp_netab_host_tests). */
#ifndef GDX_MP_NETAB_H
#define GDX_MP_NETAB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_MP_NETAB_BIT_TIMESYNC = 0, /* [net] mp_timesync_max_ms */
    GDX_MP_NETAB_BIT_NDM = 1,      /* [net] mp_ndm_exclusive */
};

#define GDX_MP_NETAB_TIMESYNC_AB (-1) /* the mp_timesync_max_ms value that means A/B ("ab") */
#define GDX_MP_NETAB_TIMESYNC_A_MS 3
#define GDX_MP_NETAB_TIMESYNC_B_MS 16
#define GDX_MP_NETAB_TIMESYNC_MAX_MS 16
#define GDX_MP_NETAB_NDM_AB 2

/* The race index of a START generation (gen 0, never sent, counts as race 0 too). */
uint32_t gdx_mp_netab_race_index(uint32_t startGen);
/* 1 = arm A for a lever on `bit` in race `raceIndex`, 0 = arm B. */
int gdx_mp_netab_arm_a(uint32_t raceIndex, unsigned bit);

/* The ini value -> the lever value (A/B kept as GDX_MP_NETAB_TIMESYNC_AB; else clamped 0..16). */
int gdx_mp_netab_timesync_lever(int iniValue);
/* The lever value -> the cap this race runs. */
int gdx_mp_netab_timesync_cap_ms(int lever, uint32_t raceIndex);

/* The ini value -> the lever value: 0, 1 or GDX_MP_NETAB_NDM_AB (any other nonzero = 1). */
int gdx_mp_netab_ndm_lever(int iniValue);
/* The lever value -> 1 = hold NDM exclusive in this race. */
int gdx_mp_netab_ndm_on(int lever, uint32_t raceIndex);

/* 1 = either lever is on A/B (the receipt is printed). */
int gdx_mp_netab_any(int timesyncLever, int ndmLever);
/* "[netab] race gen=<g> k=<k> | net.mp_timesync_max_ms=ab/A=3 net.mp_ndm_exclusive=2/B=0" (a lever
 * not on A/B prints <value>/-; `infra` 0 = a local wireless room: NDM is never taken there, the
 * field says "uds"). Returns the snprintf length. */
int gdx_mp_netab_receipt(char* buf, int cap, uint32_t startGen, int timesyncLever, int ndmLever, int infra);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MP_NETAB_H */
