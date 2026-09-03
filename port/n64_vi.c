// port/n64_vi.c — VI (video interface) bridge.
//
// libultraship's own VI stubs (os_vi.cpp) are filtered out of this port's libultraship build, so
// the VI surface lives here instead, shaped so the decomp's frame pacing works under the
// cooperative fiber scheduler:
//
//   - osViGetCurrent/NextFramebuffer() are called by the decomp inside busy-wait loops, so they
//     YIELD to the host before returning the tracked value. Without the yield those spins would
//     deadlock the single cooperative thread.
//   - osViSetEvent records the queue/message the game wants on each retrace; gdx_vi_tick(), called
//     by the host loop, advances current<-next and posts it, waking the Main scheduler thread.

#include "PR/os_vi.h"
#include "PR/os_message.h"
#include "n64_gfx_bridge.h"

extern void gdx_yield(void);

// Set here because decomp initialize.c, which normally sets it, is excluded (N64 hardware I/O).
s32 osViClock = 48681812; // VI_NTSC_CLOCK

static void* sCurrentFb = (void*) 0;
static void* sNextFb = (void*) 0;
static OSMesgQueue* sViQueue = (void*) 0;
static OSMesg sViMsg = (void*) 0;

void osViSwapBuffer(void* fb) {
    sNextFb = fb;
    gdx_vi_set_next_framebuffer(fb);
}

void* osViGetCurrentFramebuffer(void) {
    gdx_yield(); // let the host advance VI between the caller's busy-wait checks
    return sCurrentFb;
}

void* osViGetNextFramebuffer(void) {
    gdx_yield();
    return sNextFb;
}

void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount) {
    (void) retraceCount;
    sViQueue = mq;
    sViMsg = msg;
}

// --- mode/feature setters: no-ops (libultraship owns the actual window/output). ---
void osCreateViManager(OSPri pri) {
    (void) pri;
}
void osViSetMode(OSViMode* mode) {
    (void) mode;
}
void osViBlack(u8 active) {
    (void) active;
}
void osViSetSpecialFeatures(u32 features) {
    (void) features;
}
void osViSetXScale(f32 scale) {
    (void) scale;
}
void osViSetYScale(f32 scale) {
    (void) scale;
}

/* D_800CCFBC is the game's own VI retrace divider (sys_main.c:153): 1 = full 60 Hz, 2 = the EAD
   demo, 3 = Course Edit's ~20 Hz cursor mode. Exposed as an accessor -- same boundary idiom as
   gGameMode elsewhere in port/ -- because main.cpp deliberately avoids the decomp include tree. */
extern s32 D_800CCFBC;
int gdx_vi_divider(void) {
    return (int) D_800CCFBC;
}

// --- host driver: advance the framebuffer rotation and post the VI retrace message. ---
void gdx_vi_tick(void) {
    sCurrentFb = sNextFb;
    gdx_vi_set_current_framebuffer(sCurrentFb);
    if (sViQueue != (void*) 0) {
        osSendMesg(sViQueue, sViMsg, OS_MESG_NOBLOCK);
    }
}
