/* port/3ds/gdx3ds_course_share_cam.h -- `[ek] course_share` scanning: the 3DS half. Owns the
 * outer camera (libctru cam), the frame buffer, the quirc decoder and its worker thread, and the
 * APT hook that releases all of it on HOME and sleep. Expansion Kit builds only; reached only
 * from the CRS tab's SCAN view (gdx3ds_course_share.c), so with [ek] course_share = 0 nothing
 * here ever runs -- not even camInit. The pure pipeline is gdx3ds_course_share_scan.h.
 *
 * THREADS. Everything here is main-thread except the worker, which only ever touches the
 * decoder (its quirc image and scratch) and the result queue. Hand-off: the main thread fills
 * the decoder's image from a finished camera frame only while the worker is idle, then wakes
 * it; the worker decodes and queues what it read. So one frame is decoded while the camera is
 * already filling the next one, and a slow decode costs frames, never the UI.
 *
 * MEMORY (all heap, all allocated at start and freed at stop; [cshare] scan start/stop receipts
 * print the bytes and mallinfo before/after):
 *   frame buffer   320 x 240 YUV422           153,600 B
 *   quirc          image 320 x 240 + struct    ~88 KB   (gdxcs_scan_decoder_bytes)
 *   decode scratch quirc_code + quirc_data     ~13 KB
 *   worker stack                               GDXCS_CAM_WORKER_STACK
 * 2x zoom (the default) is not a bigger buffer: the sensor runs at 640 x 480 and the camera port
 * trims its centre 320 x 240 (CAMU_SetTrimmingParamsCenter), so a QR on another console's screen
 * covers twice the pixels per module at the same distance.
 *
 * SOURCES. The camera, or with [debug] cshare_scan_file = <path> BMP files from the SD card (a
 * .bmp, or a directory whose .bmp files are cycled in name order), converted to the same YUV422
 * frame and fed through the same buffer, decoder and worker -- the emulator lanes have no
 * camera picture. [debug] cshare_scan_fail = 1 makes the camera start fail (the error path).
 */
#ifndef GDX3DS_COURSE_SHARE_CAM_H
#define GDX3DS_COURSE_SHARE_CAM_H

#include <stddef.h>
#include <stdint.h>

#include "gdx3ds_course_share_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDXCS_CAM_WORKER_STACK (32u * 1024u)

typedef struct GdxCsCamStats {
    unsigned frames;       /* frames received (camera) or loaded (files) */
    unsigned decodes;      /* frames the worker ran quirc on */
    unsigned codesFound;   /* codes located */
    unsigned codesDecoded; /* payloads read */
    unsigned mirrored;     /* of which read only mirrored */
    unsigned localWins;    /* of which read only with the local threshold */
    unsigned bufferErrors; /* camera buffer overruns (recovered) */
    unsigned decodeMsLast;
    unsigned decodeMsMax;
    unsigned long decodeMsTotal;
    size_t heapBytes; /* what this module holds while running */
} GdxCsCamStats;

/* Starts the source. `zoom` 1 or 2. 0 on success; otherwise `err` says what failed, in words a
 * player can act on, and nothing is left running or allocated. */
int gdxcs_cam_start(int zoom, char* err, size_t errCap);
/* Stops everything, frees everything, logs one receipt. Safe when not running. */
void gdxcs_cam_stop(const char* why);
int gdxcs_cam_running(void);
int gdxcs_cam_zoom(void);
int gdxcs_cam_from_files(void); /* 1 when [debug] cshare_scan_file feeds the frames */

typedef struct GdxCsCamView {
    int x0;
    int y0;
    int w;
    int h;
    uint16_t border; /* RGB565 */
} GdxCsCamView;

/* Once per frame, main thread: collects a finished frame, paints it into `view` of the bottom
 * screen's RGB565 portrait framebuffer `fb` (the menu console's), hands it to the worker if the
 * worker is idle, then re-arms the camera for the next one. Returns 1 when a new frame arrived. */
int gdxcs_cam_poll(uint16_t* fb, const GdxCsCamView* view);
/* One decoded payload from the queue, oldest first. 1 when one was taken. */
int gdxcs_cam_take(GdxCsScanCode* out);

/* The latest frame again (a text repaint cleared it), grey, scaled into (x0, y0, w, h), with a
 * 2-pixel frame in `border` around it. May tear if the camera is mid-transfer; cosmetic. */
void gdxcs_cam_paint_preview(uint16_t* fb, int x0, int y0, int w, int h, uint16_t border);

/* 1 once an APT suspend/sleep stopped the camera, until the scan view takes note
 * (gdxcs_cam_take_interrupt returns it and clears it); 1 from gdxcs_cam_take_resume once the
 * console is back, so the view can restart the camera. */
int gdxcs_cam_take_interrupt(void);
int gdxcs_cam_take_resume(void);

void gdxcs_cam_stats(GdxCsCamStats* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_COURSE_SHARE_CAM_H */
