/* port/3ds/include/gdx3ds_gxcost.h -- onesubmit: [debug] gxcost, the CPU cost of GX / GSP dispatch
 * (docs/research/native/onesubmit-progress.md). DEFAULT OFF, diagnostic only, zero heap.
 *
 * How: the executable links with `--wrap` on the libctru / citro3d entry points that talk to the
 * GPU (port/3ds/CMakeLists.txt, next to the threadCreate / svcBreak wraps). Every wrapper is the
 * real call when the lever is off (one load + branch); with it on, the wrapper reads the system
 * tick around the call and charges it to the calling THREAD:
 *   main  the thread that owns the C3D frame (the one that ran the latest C3D_FrameBegin: the host
 *         loop, or the Old 3DS tick-ahead pipe's render side when it renders);
 *   ev    libctru's GSP event thread (identified by its TLS pointer in the first GSP callback);
 *   oth   anything else (audio, disk, the console thread).
 * Wrapped (times are INCLUSIVE: GX_ProcessCommandList contains the gspSubmitGxCommand it makes when
 * the queue is live, C3D_FrameBegin the gxCmdQueueWait it makes):
 *   C3D_FrameBegin (fb) / C3D_FrameEnd (fe) / gxCmdQueueWait (qw) / gxCmdQueueRun (qr) /
 *   GX_ProcessCommandList (pcl) / gspSubmitGxCommand (sub: the shared-memory write + the
 *   TriggerCmdReqQueue IPC; from main when the queue is idle, from ev when an entry completes) /
 *   GSPGPU_FlushDataCache (gfl, bytes) / svcFlushProcessDataCache (sfl, bytes) /
 *   gspWaitForEvent (gw: VBlank waits) / svcWaitSynchronization on the event thread only (its
 *   wakes, and the time from each wake to its next wait = the event thread's handler time: the
 *   interrupt queue pop, gxCmdQueueInterrupt + its submit, the user callbacks, the LightEvent
 *   signals; the kernel's IRQ entry / context switch before the wake is NOT in it).
 * GX interrupts are counted per kind from GSP callbacks (PSC0 / PPF / P3D through gputl's when
 * [debug] gputl owns them, else registered here; PSC1 and DMA always here). GX queue entries per
 * frame = the queue's entry count right after C3D_FrameEnd (the whole frame's queue).
 *
 * Receipt about once a second (and at every change of the [stereo] onesubmit A/B arm, so each line
 * is one arm), per frame (C3D_FrameEnd count):
 *   [gxcost] f=<frames> arm=<a> ent/f=E | main us/f fb=.. fe=.. qw=n/us qr=n/us pcl=n/us sub=n/us
 *            gfl=n/us/KB sfl=n/us/KB gw=n/us | ev wake/f=.. us/f=.. sub=n/us | oth sub=n/us
 *            gfl=n/us | irq/f p3d=.. ppf=.. psc0=.. psc1=.. dma=..
 * (n/us = calls and microseconds per frame). Cost with the lever on: two tick reads (~1.3 us) per
 * wrapped call, a TLS compare per svcWaitSynchronization of any thread. */
#ifndef GDX3DS_GXCOST_H
#define GDX3DS_GXCOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* gputl's GSP callback (PSC0 / PPF / P3D, on the event thread) forwards the event here. Weak in
 * the gfx library: builds without this TU link nothing. */
void gdx3ds_gxcost_on_event(int evt);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_GXCOST_H */
