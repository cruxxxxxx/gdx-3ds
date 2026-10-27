/* port/3ds/net/gdx3ds_mp_netab.c — contract: gdx3ds_mp_netab.h. */
#include "gdx3ds_mp_netab.h"

#include "gdx_mp_netab.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

#include <3ds.h>
#include <stdio.h>
#include <strings.h>

static uint32_t sRaceIndex = 0;

static void NetabLog(const char* line, int n) {
    if (n <= 0) {
        return;
    }
    svcOutputDebugString(line, n);
    gdx3ds_filelog_write(line, (size_t) n);
}

int gdx3ds_mpab_timesync_lever(void) {
    const char* raw = gdx3ds_config_get_string("net", "mp_timesync_max_ms", NULL);
    if (raw != NULL && strcasecmp(raw, "ab") == 0) {
        return GDX_MP_NETAB_TIMESYNC_AB;
    }
    return gdx_mp_netab_timesync_lever(gdx3ds_config_get_int("net", "mp_timesync_max_ms", GDX_MP_NETAB_TIMESYNC_A_MS));
}

int gdx3ds_mpab_ndm_lever(void) {
    return gdx_mp_netab_ndm_lever(gdx3ds_config_get_int("net", "mp_ndm_exclusive", 1));
}

int gdx3ds_mpab_timesync_cap_ms(int lever) {
    return gdx_mp_netab_timesync_cap_ms(lever, sRaceIndex);
}

int gdx3ds_mpab_ndm_on(int lever) {
    return gdx_mp_netab_ndm_on(lever, sRaceIndex);
}

void gdx3ds_mpab_race_start(uint32_t startGen, int infra) {
    const int ts = gdx3ds_mpab_timesync_lever();
    const int ndm = gdx3ds_mpab_ndm_lever();
    char line[160];
    int n;
    sRaceIndex = gdx_mp_netab_race_index(startGen);
    if (!gdx_mp_netab_any(ts, ndm)) {
        return;
    }
    n = gdx_mp_netab_receipt(line, (int) sizeof(line), startGen, ts, ndm, infra);
    if (n >= (int) sizeof(line)) {
        n = (int) sizeof(line) - 1;
    }
    NetabLog(line, n);
}
