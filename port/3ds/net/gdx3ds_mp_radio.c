/* port/3ds/net/gdx3ds_mp_radio.c — contract: gdx3ds_mp_radio.h. */
#include "gdx3ds_mp_radio.h"
#include "gdx3ds_mp_netab.h"
#include "gdx3ds_net.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int sNdmLever = -1;  /* [net] mp_ndm_exclusive (default 1; 2 = the per-race A/B) */
static int sNdmUp = 0;      /* ndmuInit held */
static int sExclusive = 0;  /* NDMU_EnterExclusiveState held */
static int sSuspended = 0;  /* NDMU_SuspendDaemons held */
static int sAtexit = 0;
static int sInRoom = 0;

static void RadioLog(const char* fmt, ...) {
    char line[256];
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

static void ReadLever(void) {
    if (sNdmLever < 0) {
        sNdmLever = gdx3ds_mpab_ndm_lever();
    }
}

static int ReadNdmState(void) {
    ndmState state = NDM_STATE_INITIAL;
    return R_SUCCEEDED(NDMU_GetCurrentState(&state)) ? (int) ((unsigned) state & 0xFFu) : -1;
}

static int ReadDaemonStatus(ndmDaemon daemon) {
    ndmDaemonStatus status = NDM_DAEMON_STATUS_BUSY;
    return R_SUCCEEDED(NDMU_QueryStatus(daemon, &status)) ? (int) ((unsigned) status & 0xFFu) : -1;
}

/* "state=<ndm state> cec=<status> boss=<status>" (3dbrew NDM_Services; -1 = the query failed;
 * daemon status 3 = suspended). */
static void DescribeNdm(char* buf, int cap) {
    snprintf(buf, (size_t) cap, "state=%d cec=%d boss=%d", ReadNdmState(), ReadDaemonStatus(NDM_DAEMON_CEC),
             ReadDaemonStatus(NDM_DAEMON_BOSS));
}

static void LeaveNdm(const char* why) {
    char after[64];
    Result rcResume = 0;
    Result rcLeave = 0;
    if (!sNdmUp) {
        return;
    }
    if (sSuspended) {
        rcResume = NDMU_ResumeDaemons(NDM_DAEMON_MASK_BACKGROUOND);
        sSuspended = 0;
    }
    if (sExclusive) {
        rcLeave = NDMU_LeaveExclusiveState();
        sExclusive = 0;
    }
    DescribeNdm(after, (int) sizeof(after));
    ndmuExit();
    sNdmUp = 0;
    RadioLog("[mpradio] ndm leave (%s) resume=0x%08lX leave=0x%08lX | %s", why, (unsigned long) rcResume,
             (unsigned long) rcLeave, after);
}

static void LeaveAtExit(void) {
    LeaveNdm("exit");
}

static void EnterNdm(void) {
    char before[64];
    char after[64];
    Result rcEnter;
    Result rcSuspend;
    if (R_FAILED(ndmuInit())) {
        RadioLog("[mpradio] ndm: ndmuInit failed (no ndm:u access): daemons left as they are");
        sNdmLever = 0;
        return;
    }
    sNdmUp = 1;
    if (!sAtexit) {
        atexit(LeaveAtExit);
        sAtexit = 1;
    }
    DescribeNdm(before, (int) sizeof(before));
    rcEnter = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
    sExclusive = R_SUCCEEDED(rcEnter);
    rcSuspend = NDMU_SuspendDaemons(NDM_DAEMON_MASK_BACKGROUOND);
    sSuspended = R_SUCCEEDED(rcSuspend);
    DescribeNdm(after, (int) sizeof(after));
    RadioLog("[mpradio] ndm enter (net.mp_ndm_exclusive=%d) exclusive=0x%08lX suspend=0x%08lX | before %s | after %s",
             sNdmLever, (unsigned long) rcEnter, (unsigned long) rcSuspend, before, after);
}

/* card-3: the lever this race runs (2 = the arm gdx3ds_mpab_race_start latched). */
static int NdmLeverOn(void) {
    ReadLever();
    return gdx3ds_mpab_ndm_on(sNdmLever);
}

static void ApplyNdm(int want) {
    const int leverOn = NdmLeverOn();
    if (want && leverOn && !sNdmUp) {
        EnterNdm();
    } else if (!want && sNdmUp) {
        LeaveNdm("room closed");
    } else if (!leverOn && sNdmUp) {
        LeaveNdm("A/B arm B, net.mp_ndm_exclusive=2");
    }
}

void gdx3ds_mp_radio_update(int inRoom, int infra) {
    inRoom = inRoom ? 1 : 0;
    if (inRoom != sInRoom) {
        sInRoom = inRoom;
        gdx3ds_net_set_mp_quiet(inRoom);
        gdx3ds_filelog_set_mp_session(inRoom);
    }
    ApplyNdm(inRoom && infra);
}

void gdx3ds_mp_radio_release(const char* why) {
    LeaveNdm(why);
    if (sInRoom) {
        sInRoom = 0;
        gdx3ds_net_set_mp_quiet(0);
        gdx3ds_filelog_set_mp_session(0);
    }
}

int gdx3ds_mp_radio_bars(void) {
    return (int) osGetWifiStrength();
}

int gdx3ds_mp_radio_exclusive(void) {
    return sExclusive;
}
