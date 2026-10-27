/* port/gdx_mp_netab.c — card-3 online lever A/B. Contract: gdx_mp_netab.h. */
#include "gdx_mp_netab.h"

#include <stdio.h>

uint32_t gdx_mp_netab_race_index(uint32_t startGen) {
    if (startGen == 0u) {
        return 0u;
    }
    return startGen - 1u;
}

int gdx_mp_netab_arm_a(uint32_t raceIndex, unsigned bit) {
    if (bit >= 32u) {
        return 1;
    }
    return ((raceIndex >> bit) & 1u) == 0u;
}

int gdx_mp_netab_timesync_lever(int iniValue) {
    if (iniValue == GDX_MP_NETAB_TIMESYNC_AB) {
        return GDX_MP_NETAB_TIMESYNC_AB;
    } else if (iniValue < 0) {
        return 0;
    } else if (iniValue > GDX_MP_NETAB_TIMESYNC_MAX_MS) {
        return GDX_MP_NETAB_TIMESYNC_MAX_MS;
    }
    return iniValue;
}

int gdx_mp_netab_timesync_cap_ms(int lever, uint32_t raceIndex) {
    if (lever != GDX_MP_NETAB_TIMESYNC_AB) {
        return lever;
    }
    if (gdx_mp_netab_arm_a(raceIndex, GDX_MP_NETAB_BIT_TIMESYNC)) {
        return GDX_MP_NETAB_TIMESYNC_A_MS;
    }
    return GDX_MP_NETAB_TIMESYNC_B_MS;
}

int gdx_mp_netab_ndm_lever(int iniValue) {
    if (iniValue == 0 || iniValue == GDX_MP_NETAB_NDM_AB) {
        return iniValue;
    }
    return 1;
}

int gdx_mp_netab_ndm_on(int lever, uint32_t raceIndex) {
    if (lever == GDX_MP_NETAB_NDM_AB) {
        return gdx_mp_netab_arm_a(raceIndex, GDX_MP_NETAB_BIT_NDM);
    }
    return lever != 0;
}

int gdx_mp_netab_any(int timesyncLever, int ndmLever) {
    return timesyncLever == GDX_MP_NETAB_TIMESYNC_AB || ndmLever == GDX_MP_NETAB_NDM_AB;
}

static const char* ArmName(int armA) {
    return armA ? "A" : "B";
}

int gdx_mp_netab_receipt(char* buf, int cap, uint32_t startGen, int timesyncLever, int ndmLever, int infra) {
    const uint32_t k = gdx_mp_netab_race_index(startGen);
    char ts[32];
    char ndm[32];
    if (timesyncLever == GDX_MP_NETAB_TIMESYNC_AB) {
        snprintf(ts, sizeof(ts), "ab/%s=%d", ArmName(gdx_mp_netab_arm_a(k, GDX_MP_NETAB_BIT_TIMESYNC)),
                 gdx_mp_netab_timesync_cap_ms(timesyncLever, k));
    } else {
        snprintf(ts, sizeof(ts), "%d/-", timesyncLever);
    }
    if (!infra) {
        snprintf(ndm, sizeof(ndm), "%d/uds", ndmLever);
    } else if (ndmLever == GDX_MP_NETAB_NDM_AB) {
        snprintf(ndm, sizeof(ndm), "2/%s=%d", ArmName(gdx_mp_netab_arm_a(k, GDX_MP_NETAB_BIT_NDM)),
                 gdx_mp_netab_ndm_on(ndmLever, k));
    } else {
        snprintf(ndm, sizeof(ndm), "%d/-", ndmLever);
    }
    return snprintf(buf, (size_t) cap,
                    "[netab] race gen=%lu k=%lu | net.mp_timesync_max_ms=%s net.mp_ndm_exclusive=%s",
                    (unsigned long) startGen, (unsigned long) k, ts, ndm);
}
