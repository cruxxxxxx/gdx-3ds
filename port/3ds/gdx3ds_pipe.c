/* port/3ds/gdx3ds_pipe.c -- o3ds60 pipeline levers and their in-race A/B. Contract in the header. */
#include "gdx3ds_pipe.h"

#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_gfx_split.h"

#define PIPE_WINDOW_FRAMES 256u

static int sLatched = 0;
static int sTopTriple = 0;    /* [debug] top_triple: 0 off, 1 lockstep races, 2 always */
static int sDashPreFrame = 0; /* [ui] dash_pre_frame */
static unsigned sAbWindows = 0; /* [debug] pipe_ab in 256-frame windows (0 = no A/B) */
static int sAbWhat = 1;       /* [debug] pipe_ab_what: 1 = top_triple + dash_pre_frame, 2 = kick_3d */
static int sKick3d = 1;       /* [debug] kick_3d: mid-frame GPU kicks while the right eye runs */
static int sLockstep = 0;
static unsigned sRaceWindows = 0; /* windows since the lockstep race started */
static int sAllocFailed = 0;
static int sLastWant = -1;
static GdxGfxTripleStats sStatsAtWindow;

static void PipeLog(const char* line) {
    svcOutputDebugString(line, strlen(line));
    gdx3ds_filelog_write(line, strlen(line));
}

static int IsNew3ds(void) {
    bool isNew = false;
    if (R_FAILED(APT_CheckNew3DS(&isNew))) {
        return 0;
    }
    return isNew ? 1 : 0;
}

static void Latch(void) {
    char line[320];
    const int isNew = IsNew3ds();
    int ab;
    if (sLatched || !gdx3ds_config_loaded()) {
        return;
    }
    sLatched = 1;
    sTopTriple = gdx3ds_config_get_int("debug", "top_triple", isNew ? 0 : 1);
    if (sTopTriple < 0 || sTopTriple > 2) {
        sTopTriple = 0;
    }
    sDashPreFrame = gdx3ds_config_get_int("ui", "dash_pre_frame", isNew ? 0 : 1) != 0;
    sKick3d = gdx3ds_config_get_int("debug", "kick_3d", 1) != 0;
    sAbWhat = gdx3ds_config_get_int("debug", "pipe_ab_what", 1) == 2 ? 2 : 1;
    ab = gdx3ds_config_get_int("debug", "pipe_ab", 0);
    sAbWindows = ab > 0 ? (unsigned) ((ab + (int) PIPE_WINDOW_FRAMES / 2) / (int) PIPE_WINDOW_FRAMES) : 0u;
    if (ab > 0 && sAbWindows == 0) {
        sAbWindows = 1;
    }
    snprintf(line, sizeof(line),
             "[pipe] init top_triple=%d dash_pre_frame=%d kick_3d=%d pipe_ab=%u frames pipe_ab_what=%s (blocks: even "
             "= as configured, odd = %s; lockstep races)",
             sTopTriple, sDashPreFrame, sKick3d, sAbWindows * PIPE_WINDOW_FRAMES, sAbWhat == 2 ? "kick_3d" : "triple+dash",
             sAbWhat == 2 ? "kick_3d flipped" : "both off");
    PipeLog(line);
}

/* 1 in an "old" block of the A/B (a lockstep race only). */
static int InOldBlock(void) {
    return sAbWindows != 0 && sLockstep && ((sRaceWindows / sAbWindows) & 1u) != 0;
}

/* The odd blocks flip what pipe_ab_what names; the other levers stay as configured. */
static int OldBlockFor(int what) {
    return sAbWhat == what && InOldBlock();
}

static int Engaged(void) {
    return sLockstep || sTopTriple == 2;
}

static int TripleWanted(void) {
    if (sTopTriple == 0 || sAllocFailed || OldBlockFor(1)) {
        return 0;
    }
    return sTopTriple == 2 || sLockstep;
}

void gdx3ds_pipe_set_lockstep(int on) {
    Latch();
    on = on ? 1 : 0;
    if (on == sLockstep) {
        return;
    }
    sLockstep = on;
    sRaceWindows = 0;
    gdx3ds_gfx_top_triple_stats(&sStatsAtWindow);
}

void gdx3ds_pipe_iteration_begin(void) {
    int want;
    Latch();
    if (!sLatched) {
        return;
    }
    want = TripleWanted();
    if (want == sLastWant) {
        return;
    }
    sLastWant = want;
    if (!gdx3ds_gfx_top_triple_request(want != 0)) {
        char line[160];
        sAllocFailed = 1;
        sLastWant = 0;
        snprintf(line, sizeof(line), "[pipe] top_triple REFUSED: no memory for the third top framebuffer "
                                     "(double buffering + the end guard stay)");
        PipeLog(line);
    }
}

int gdx3ds_pipe_dash_pre_frame(void) {
    return sDashPreFrame && sLockstep && !OldBlockFor(1);
}

int gdx3ds_pipe_kick_3d(void) {
    Latch();
    return OldBlockFor(2) ? !sKick3d : sKick3d;
}

int gdx3ds_pipe_receipt_state(void) {
    if (!Engaged()) {
        return -1;
    }
    return InOldBlock() ? 0 : 1;
}

void gdx3ds_pipe_window(unsigned long frame) {
    GdxGfxTripleStats st;
    char line[256];
    if (!sLatched || !Engaged()) {
        return;
    }
    gdx3ds_gfx_top_triple_stats(&st);
    snprintf(line, sizeof(line),
             "[pipe] frame=%lu block=%u state=%s top_triple=%s dash_pre_frame=%d kick_3d=%d | tb presents=%lu "
             "replaced=%lu edges=%lu buf=%luK%s",
             frame, sAbWindows != 0 ? sRaceWindows / sAbWindows : 0u, InOldBlock() ? "old" : "new",
             st.state == 1 ? "on" : (st.state == 2 ? "leaving" : "off"), gdx3ds_pipe_dash_pre_frame(),
             gdx3ds_pipe_kick_3d(),
             st.presents - sStatsAtWindow.presents, st.replaced - sStatsAtWindow.replaced, st.edges - sStatsAtWindow.edges,
             st.bytes / 1024u, st.bytes != 0 ? (st.inVram ? " vram" : " linear") : "");
    PipeLog(line);
    sStatsAtWindow = st;
    if (sLockstep) {
        sRaceWindows++;
    }
}
