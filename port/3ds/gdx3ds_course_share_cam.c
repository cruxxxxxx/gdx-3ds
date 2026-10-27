/* port/3ds/gdx3ds_course_share_cam.c -- `[ek] course_share` scanning: camera, worker, APT.
 * Contract: gdx3ds_course_share_cam.h. Expansion Kit builds only. */
#include "gdx3ds_course_share_cam.h"

#include "gdx3ds_filelog.h"

#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <3ds.h>

#include "gdx3ds_config.h"

#define FRAME_W GDXCS_SCAN_W
#define FRAME_H GDXCS_SCAN_H
#define QUEUE_SLOTS 8
#define FILES_MAX 16
#define FILE_LEAF_MAX 48
#define FILE_PATH_MAX 128
#define FILE_PERIOD_MS 200
#define RESULT_TIMEOUT 0x09401BFEu /* svcWaitSynchronizationN: nothing signalled */
#define PREVIEW_MAX 320

/* ---- state ----------------------------------------------------------------------------- */

static int sRunning = 0;
static int sZoom = 2;
static int sFromFiles = 0;
static uint8_t* sFrame = NULL; /* GDXCS_SCAN_YUV_BYTES: the camera's DMA target / a file frame */
static int sFrameValid = 0;
static int sPhase = 1;
static GdxCsCamStats sStats;

static Handle sRecvEvent = 0;
static Handle sErrEvent = 0;
static u32 sTransferBytes = 0;
static int sCamInited = 0;
static int sCamActive = 0;

static GdxCsScanDecoder sDecoder;
static Thread sWorker = NULL;
static LightEvent sWake;
static LightLock sQueueLock;
static volatile int sQuit = 0;
static volatile int sBusy = 0;
static GdxCsScanResult sResult; /* worker-owned while sBusy */
static GdxCsScanCode sQueue[QUEUE_SLOTS];
static int sQueueHead = 0;
static int sQueueCount = 0;

static char sFileDir[FILE_PATH_MAX];
static char sFiles[FILES_MAX][FILE_LEAF_MAX];
static int sFileCount = 0;
static int sFileNext = 0;
static u64 sFileDueTick = 0;
static uint8_t* sBmpScratch = NULL;

static aptHookCookie sAptCookie;
static int sAptHooked = 0;
static volatile int sInterrupted = 0;
static volatile int sResumeArmed = 0;
static volatile int sResume = 0;

static int sPreviewW = 0;
static int sPreviewH = 0;
static uint16_t sMapX[PREVIEW_MAX];
static uint16_t sMapY[PREVIEW_MAX];

/* ---- receipts ---------------------------------------------------------------------------- */

/* svc + filelog only: also called from the APT hook, which must never touch the console. */
static void CamLog(const char* fmt, ...) {
    char buf[220];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int) sizeof(buf) - 1) {
            n = (int) sizeof(buf) - 1;
        }
        svcOutputDebugString(buf, (size_t) n);
        gdx3ds_filelog_write(buf, (size_t) n);
    }
}

static unsigned long HeapUsed(void) {
    const struct mallinfo mi = mallinfo();
    return (unsigned long) mi.uordblks;
}

static unsigned TicksToMs(u64 ticks) {
    return (unsigned) (ticks / (u64) CPU_TICKS_PER_MSEC);
}

static void SetErr(char* err, size_t cap, const char* fmt, ...) {
    va_list ap;
    if (err == NULL || cap == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* ---- worker ------------------------------------------------------------------------------ */

static void QueuePush(const GdxCsScanCode* code) {
    LightLock_Lock(&sQueueLock);
    if (sQueueCount == QUEUE_SLOTS) {
        sQueueHead = (sQueueHead + 1) % QUEUE_SLOTS; /* drop the oldest: the newest view wins */
        sQueueCount--;
    }
    sQueue[(sQueueHead + sQueueCount) % QUEUE_SLOTS] = *code;
    sQueueCount++;
    LightLock_Unlock(&sQueueLock);
}

static void NoteDecode(const GdxCsScanResult* r, u64 ticks) {
    int i;
    const unsigned ms = TicksToMs(ticks);
    sStats.decodes++;
    sStats.codesFound += (unsigned) r->found;
    sStats.codesDecoded += (unsigned) r->decoded;
    sStats.decodeMsLast = ms;
    sStats.decodeMsTotal += ms;
    if (ms > sStats.decodeMsMax) {
        sStats.decodeMsMax = ms;
    }
    for (i = 0; i < r->decoded; i++) {
        if (r->code[i].mirrored) {
            sStats.mirrored++;
        }
        if (r->mode == GDXCS_SCAN_MODE_LOCAL) {
            sStats.localWins++;
        }
    }
}

static void WorkerMain(void* arg) {
    (void) arg;
    for (;;) {
        u64 t0;
        int i;
        LightEvent_Wait(&sWake);
        if (sQuit) {
            break;
        }
        t0 = svcGetSystemTick();
        gdxcs_scan_decoder_run(&sDecoder, &sResult);
        NoteDecode(&sResult, svcGetSystemTick() - t0);
        for (i = 0; i < sResult.decoded; i++) {
            QueuePush(&sResult.code[i]);
        }
        __atomic_store_n(&sBusy, 0, __ATOMIC_RELEASE);
    }
}

/* The finished frame's luma goes straight into quirc's image, only while the worker is idle. */
static void OfferToWorker(void) {
    uint8_t* image;
    if (__atomic_load_n(&sBusy, __ATOMIC_ACQUIRE)) {
        return;
    }
    image = gdxcs_scan_decoder_image(&sDecoder);
    if (image == NULL) {
        return;
    }
    gdxcs_scan_yuv_to_grey(sFrame, FRAME_W, FRAME_H, sPhase, image);
    __atomic_store_n(&sBusy, 1, __ATOMIC_RELEASE);
    LightEvent_Signal(&sWake);
}

static int WorkerPriority(void) {
    s32 mainPrio = 0x30;
    const int lever = gdx3ds_config_get_int("debug", "cshare_scan_prio", 0);
    if (lever >= 0x18 && lever <= 0x3F) {
        return lever;
    }
    svcGetThreadPriority(&mainPrio, CUR_THREAD_HANDLE);
    /* just below the main thread: it decodes whenever the frame loop waits (vsync, GPU) */
    return mainPrio + 1 <= 0x3F ? (int) mainPrio + 1 : 0x3F;
}

static int StartWorker(int* prioOut) {
    const int prio = WorkerPriority();
    sQuit = 0;
    sBusy = 0;
    sQueueHead = 0;
    sQueueCount = 0;
    LightEvent_Init(&sWake, RESET_ONESHOT);
    LightLock_Init(&sQueueLock);
    sWorker = threadCreate(WorkerMain, NULL, GDXCS_CAM_WORKER_STACK, prio, -2, false);
    *prioOut = prio;
    return sWorker != NULL ? 0 : -1;
}

static void StopWorker(void) {
    if (sWorker == NULL) {
        return;
    }
    sQuit = 1;
    LightEvent_Signal(&sWake);
    threadJoin(sWorker, U64_MAX);
    threadFree(sWorker);
    sWorker = NULL;
    sBusy = 0;
}

/* ---- camera -------------------------------------------------------------------------------- */

#define CAM_TRY(call, step)                                                                  \
    do {                                                                                     \
        const Result rc_ = (call);                                                           \
        if (R_FAILED(rc_)) {                                                                 \
            SetErr(err, errCap, "camera setup failed at %s (%08lX)", step, (unsigned long) rc_); \
            CamLog("[cshare] scan camera %s FAILED rc=%08lX", step, (unsigned long) rc_);   \
            return -1;                                                                       \
        }                                                                                    \
    } while (0)

/* Quality settings: worth having, never worth failing the scan over. */
static void CamSoft(Result rc, const char* step) {
    if (R_FAILED(rc)) {
        CamLog("[cshare] scan camera %s refused rc=%08lX (continuing)", step, (unsigned long) rc);
    }
}

static int ConfigureSize(int zoom, char* err, size_t errCap) {
    if (zoom >= 2) {
        /* the sensor's full 640 x 480, trimmed to its centre 320 x 240: twice the pixels per
         * module of the same QR at the same distance, in the same buffer */
        CAM_TRY(CAMU_SetSize(SELECT_OUT1, SIZE_VGA, CONTEXT_A), "SetSize VGA");
        CAM_TRY(CAMU_SetTrimming(PORT_CAM1, true), "SetTrimming");
        CAM_TRY(CAMU_SetTrimmingParamsCenter(PORT_CAM1, FRAME_W, FRAME_H, 640, 480), "SetTrimmingParamsCenter");
    } else {
        CAM_TRY(CAMU_SetSize(SELECT_OUT1, SIZE_QVGA, CONTEXT_A), "SetSize QVGA");
        CAM_TRY(CAMU_SetTrimming(PORT_CAM1, false), "SetTrimming");
    }
    return 0;
}

static int StartCamera(int zoom, char* err, size_t errCap) {
    Result rc;
    if (gdx3ds_config_get_bool("debug", "cshare_scan_fail", 0)) {
        SetErr(err, errCap, "camera unavailable (camInit %08lX, simulated)", 0xC8A0CFFCul);
        CamLog("[cshare] scan camera camInit FAILED rc=C8A0CFFC (simulated: [debug] cshare_scan_fail=1)");
        return -1;
    }
    rc = camInit();
    if (R_FAILED(rc)) {
        SetErr(err, errCap, "camera unavailable (camInit %08lX)", (unsigned long) rc);
        CamLog("[cshare] scan camera camInit FAILED rc=%08lX", (unsigned long) rc);
        return -1;
    }
    sCamInited = 1;
    if (ConfigureSize(zoom, err, errCap) != 0) {
        return -1;
    }
    CAM_TRY(CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_YUV_422, CONTEXT_A), "SetOutputFormat");
    CamSoft(CAMU_SetNoiseFilter(SELECT_OUT1, true), "SetNoiseFilter");
    CamSoft(CAMU_SetAutoExposure(SELECT_OUT1, true), "SetAutoExposure");
    CamSoft(CAMU_SetAutoWhiteBalance(SELECT_OUT1, true), "SetAutoWhiteBalance");
    /* dim rooms: let the exposure stretch down to 5 fps rather than give a dark, noisy frame */
    CamSoft(CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15_TO_5), "SetFrameRate");
    /* the sensor's own mode for black-on-white print: higher contrast and sharpening */
    CamSoft(CAMU_SetPhotoMode(SELECT_OUT1, PHOTO_MODE_LETTER), "SetPhotoMode");
    CAM_TRY(CAMU_GetMaxBytes(&sTransferBytes, FRAME_W, FRAME_H), "GetMaxBytes");
    CAM_TRY(CAMU_SetTransferBytes(PORT_CAM1, sTransferBytes, FRAME_W, FRAME_H), "SetTransferBytes");
    CAM_TRY(CAMU_Activate(SELECT_OUT1), "Activate");
    sCamActive = 1;
    CAM_TRY(CAMU_GetBufferErrorInterruptEvent(&sErrEvent, PORT_CAM1), "GetBufferErrorInterruptEvent");
    CAM_TRY(CAMU_ClearBuffer(PORT_CAM1), "ClearBuffer");
    CAM_TRY(CAMU_StartCapture(PORT_CAM1), "StartCapture");
    return 0;
}

static void StopCamera(void) {
    if (sCamActive) {
        CAMU_StopCapture(PORT_CAM1);
    }
    if (sRecvEvent != 0) {
        svcCloseHandle(sRecvEvent);
        sRecvEvent = 0;
    }
    if (sCamActive) {
        CAMU_Activate(SELECT_NONE);
        sCamActive = 0;
    }
    /* the buffer-error event is the service's handle, closed with the session */
    sErrEvent = 0;
    if (sCamInited) {
        camExit();
        sCamInited = 0;
    }
}

/* Returns 1 when a whole frame landed in sFrame. Never re-arms: gdxcs_cam_poll re-arms after it
 * has painted and handed off the frame, so the DMA never overwrites a frame mid-use. */
static int CollectCameraFrame(void) {
    Handle handles[2];
    s32 index = -1;
    Result rc;
    if (sRecvEvent == 0) {
        return 0;
    }
    handles[0] = sErrEvent;
    handles[1] = sRecvEvent;
    rc = svcWaitSynchronizationN(&index, handles, 2, false, 0);
    if ((u32) rc == RESULT_TIMEOUT || R_FAILED(rc)) {
        return 0;
    }
    svcCloseHandle(sRecvEvent);
    sRecvEvent = 0;
    if (index == 0) {
        /* the frame loop fell behind the sensor: start the next frame clean */
        sStats.bufferErrors++;
        CAMU_ClearBuffer(PORT_CAM1);
        CAMU_StartCapture(PORT_CAM1);
        return 0;
    }
    return 1;
}

static void ArmCameraReceive(void) {
    Result rc;
    if (sRecvEvent != 0) {
        return;
    }
    rc = CAMU_SetReceiving(&sRecvEvent, sFrame, PORT_CAM1, (u32) GDXCS_SCAN_YUV_BYTES, (s16) sTransferBytes);
    if (R_FAILED(rc)) {
        sRecvEvent = 0;
    }
}

/* ---- file frames ([debug] cshare_scan_file) ----------------------------------------------- */

static size_t FileRead(void* ctx, size_t offset, void* dst, size_t n) {
    FILE* f = (FILE*) ctx;
    if (fseek(f, (long) offset, SEEK_SET) != 0) {
        return 0;
    }
    return fread(dst, 1, n, f);
}

static int IsBmpLeaf(const char* leaf) {
    const size_t n = strlen(leaf);
    return n > 4 && strcasecmp(leaf + n - 4, ".bmp") == 0;
}

static int CompareLeaf(const void* a, const void* b) {
    return strcmp((const char*) a, (const char*) b);
}

/* A directory: its .bmp files in name order. A file: just it. */
static int ListFrameFiles(const char* path) {
    struct stat st;
    DIR* dir;
    struct dirent* ent;
    sFileCount = 0;
    sFileNext = 0;
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(sFileDir, sizeof(sFileDir), "%s", "");
        snprintf(sFiles[0], sizeof(sFiles[0]), "%.47s", path);
        if (strlen(path) >= FILE_LEAF_MAX) {
            snprintf(sFileDir, sizeof(sFileDir), "%s", path); /* long single path: kept whole */
            sFiles[0][0] = '\0';
        }
        sFileCount = 1;
        return 0;
    }
    snprintf(sFileDir, sizeof(sFileDir), "%s", path);
    dir = opendir(path);
    if (dir == NULL) {
        return -1;
    }
    while ((ent = readdir(dir)) != NULL && sFileCount < FILES_MAX) {
        if (strlen(ent->d_name) < FILE_LEAF_MAX && IsBmpLeaf(ent->d_name)) {
            snprintf(sFiles[sFileCount++], FILE_LEAF_MAX, "%s", ent->d_name);
        }
    }
    closedir(dir);
    qsort(sFiles, (size_t) sFileCount, sizeof(sFiles[0]), CompareLeaf);
    return sFileCount > 0 ? 0 : -1;
}

static void FramePath(int index, char* out, size_t cap) {
    if (sFiles[index][0] == '\0') {
        snprintf(out, cap, "%s", sFileDir);
    } else if (sFileDir[0] == '\0') {
        snprintf(out, cap, "%s", sFiles[index]);
    } else {
        snprintf(out, cap, "%s/%s", sFileDir, sFiles[index]);
    }
}

static int LoadFrameFile(int index) {
    char path[FILE_PATH_MAX + FILE_LEAF_MAX + 2];
    FILE* f;
    long size;
    int rc;
    FramePath(index, path, sizeof(path));
    f = fopen(path, "rb");
    if (f == NULL) {
        CamLog("[cshare] scan file %s: cannot open", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rc = size > 0 ? gdxcs_scan_bmp_to_yuv(FileRead, f, (size_t) size, sFrame, FRAME_W, FRAME_H, 1, sBmpScratch,
                                          GDXCS_SCAN_BMP_SCRATCH(FRAME_W))
                  : -1;
    fclose(f);
    CamLog("[cshare] scan file frame %d/%d %s bytes=%ld %s", index + 1, sFileCount, path, size,
           rc == 0 ? "ok" : "NOT A READABLE BMP");
    return rc;
}

static int PollFiles(void) {
    const u64 now = svcGetSystemTick();
    int index;
    if (now < sFileDueTick) {
        return 0;
    }
    sFileDueTick = now + (u64) FILE_PERIOD_MS * (u64) CPU_TICKS_PER_MSEC;
    index = sFileNext;
    sFileNext = (sFileNext + 1) % sFileCount;
    return LoadFrameFile(index) == 0 ? 1 : 0;
}

static int StartFiles(const char* path, char* err, size_t errCap) {
    sBmpScratch = (uint8_t*) malloc(GDXCS_SCAN_BMP_SCRATCH(FRAME_W));
    if (sBmpScratch == NULL) {
        SetErr(err, errCap, "not enough memory for the frame reader");
        return -1;
    }
    if (ListFrameFiles(path) != 0) {
        SetErr(err, errCap, "cshare_scan_file %s: no .bmp there", path);
        CamLog("[cshare] scan file source %s: nothing to read", path);
        return -1;
    }
    sFileDueTick = 0;
    return 0;
}

/* ---- APT ------------------------------------------------------------------------------------ */

static void StopAll(const char* why);

/* HOME, the power button's menu and the lid all reach here on the main thread inside
 * aptMainLoop, before libctru hands the system over: the camera is released (the HOME menu's
 * own camera applet needs it), the worker joined and every buffer freed. Registered on the first
 * scan only, so a run that never scans never hooks. */
static void CamAptHook(APT_HookType hook, void* param) {
    (void) param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            if (sRunning) {
                StopAll(hook == APTHOOK_ONSUSPEND ? "apt-suspend" : "apt-sleep");
                sInterrupted = 1;
                sResumeArmed = 1;
            }
            break;
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            if (sResumeArmed) {
                sResumeArmed = 0;
                sResume = 1;
            }
            break;
        case APTHOOK_ONEXIT:
            if (sRunning) {
                StopAll("apt-exit");
            }
            break;
        default:
            break;
    }
}

/* An exit() that bypasses aptMainLoop (a fatal path, [debug] replay_quit) never reaches
 * APTHOOK_ONEXIT: release the camera there too. */
static void CamAtExit(void) {
    if (sRunning) {
        StopAll("exit");
    }
}

static void EnsureAptHook(void) {
    if (!sAptHooked) {
        aptHook(&sAptCookie, CamAptHook, NULL);
        atexit(CamAtExit);
        sAptHooked = 1;
    }
}

int gdxcs_cam_take_interrupt(void) {
    const int was = sInterrupted;
    sInterrupted = 0;
    return was;
}

int gdxcs_cam_take_resume(void) {
    const int was = sResume;
    sResume = 0;
    return was;
}

/* ---- start / stop ---------------------------------------------------------------------------- */

static void FreeBuffers(void) {
    gdxcs_scan_decoder_free(&sDecoder);
    free(sFrame);
    sFrame = NULL;
    free(sBmpScratch);
    sBmpScratch = NULL;
}

static void StopAll(const char* why) {
    const unsigned long before = HeapUsed();
    if (!sRunning) {
        return;
    }
    StopWorker();
    StopCamera();
    FreeBuffers();
    sRunning = 0;
    sFrameValid = 0;
    CamLog("[cshare] scan stop why=%s src=%s frames=%u decodes=%u found=%u decoded=%u mirrored=%u local=%u "
           "buf_err=%u ms_last=%u ms_max=%u ms_avg=%lu heap_used=%lu->%lu",
           why, sFromFiles ? "files" : "camera", sStats.frames, sStats.decodes, sStats.codesFound,
           sStats.codesDecoded, sStats.mirrored, sStats.localWins, sStats.bufferErrors, sStats.decodeMsLast,
           sStats.decodeMsMax, sStats.decodes > 0 ? sStats.decodeMsTotal / sStats.decodes : 0ul, before,
           HeapUsed());
}

static int AllocateBuffers(char* err, size_t errCap) {
    sFrame = (uint8_t*) malloc(GDXCS_SCAN_YUV_BYTES);
    if (sFrame == NULL || gdxcs_scan_decoder_init(&sDecoder, FRAME_W, FRAME_H) != 0) {
        SetErr(err, errCap, "not enough memory for the camera (%u KB)",
               (unsigned) ((GDXCS_SCAN_YUV_BYTES + 100u * 1024u) / 1024u));
        return -1;
    }
    memset(sFrame, 0x80, GDXCS_SCAN_YUV_BYTES);
    return 0;
}

static int StartSource(int zoom, char* err, size_t errCap) {
    const char* filePath = gdx3ds_config_get_string("debug", "cshare_scan_file", "");
    sFromFiles = filePath != NULL && filePath[0] != '\0';
    if (sFromFiles) {
        return StartFiles(filePath, err, errCap);
    }
    return StartCamera(zoom, err, errCap);
}

int gdxcs_cam_start(int zoom, char* err, size_t errCap) {
    const unsigned long before = HeapUsed();
    int prio = 0;
    if (sRunning) {
        return 0;
    }
    memset(&sStats, 0, sizeof(sStats));
    sZoom = zoom >= 2 ? 2 : 1;
    sInterrupted = 0;
    sResume = 0;
    sRunning = 1; /* StopAll below unwinds whatever got started */
    if (AllocateBuffers(err, errCap) != 0 || StartSource(sZoom, err, errCap) != 0) {
        StopAll("start-failed");
        return -1;
    }
    if (StartWorker(&prio) != 0) {
        SetErr(err, errCap, "no worker thread for the decoder");
        StopAll("start-failed");
        return -1;
    }
    EnsureAptHook();
    sStats.heapBytes = GDXCS_SCAN_YUV_BYTES + gdxcs_scan_decoder_bytes(&sDecoder) + GDXCS_CAM_WORKER_STACK +
                       (sBmpScratch != NULL ? GDXCS_SCAN_BMP_SCRATCH(FRAME_W) : 0u);
    CamLog("[cshare] scan start src=%s zoom=%dx frame=%dx%d yuv422 transfer=%lu frame_buf=%u quirc=%u "
           "worker_stack=%u prio=0x%02X module_bytes=%u heap_used=%lu->%lu",
           sFromFiles ? "files" : "camera", sZoom, FRAME_W, FRAME_H, (unsigned long) sTransferBytes,
           (unsigned) GDXCS_SCAN_YUV_BYTES, (unsigned) gdxcs_scan_decoder_bytes(&sDecoder),
           (unsigned) GDXCS_CAM_WORKER_STACK, prio, (unsigned) sStats.heapBytes, before, HeapUsed());
    return 0;
}

void gdxcs_cam_stop(const char* why) {
    StopAll(why != NULL ? why : "stop");
}

int gdxcs_cam_running(void) {
    return sRunning;
}

int gdxcs_cam_zoom(void) {
    return sZoom;
}

int gdxcs_cam_from_files(void) {
    return sFromFiles;
}

/* ---- per frame -------------------------------------------------------------------------------- */

int gdxcs_cam_poll(uint16_t* fb, const GdxCsCamView* view) {
    int got;
    if (!sRunning || sFrame == NULL) {
        return 0;
    }
    got = sFromFiles ? PollFiles() : CollectCameraFrame();
    if (got) {
        sStats.frames++;
        sFrameValid = 1;
        sPhase = gdxcs_scan_luma_phase(sFrame, FRAME_W, FRAME_H);
        if (fb != NULL && view != NULL) {
            gdxcs_cam_paint_preview(fb, view->x0, view->y0, view->w, view->h, view->border);
        }
        OfferToWorker();
    }
    if (!sFromFiles) {
        ArmCameraReceive();
    }
    return got;
}

int gdxcs_cam_take(GdxCsScanCode* out) {
    int took = 0;
    if (out == NULL || sWorker == NULL) {
        return 0;
    }
    LightLock_Lock(&sQueueLock);
    if (sQueueCount > 0) {
        *out = sQueue[sQueueHead];
        sQueueHead = (sQueueHead + 1) % QUEUE_SLOTS;
        sQueueCount--;
        took = 1;
    }
    LightLock_Unlock(&sQueueLock);
    return took;
}

static void BuildPreviewMaps(int w, int h) {
    int i;
    if (w == sPreviewW && h == sPreviewH) {
        return;
    }
    for (i = 0; i < w; i++) {
        sMapX[i] = (uint16_t) ((long) i * FRAME_W / w);
    }
    for (i = 0; i < h; i++) {
        sMapY[i] = (uint16_t) ((long) i * FRAME_H / h);
    }
    sPreviewW = w;
    sPreviewH = h;
}

/* Same portrait layout as gdxcs_qr_blit_rgb565: pixel (x, y) at fb[x * 240 + (239 - y)]. */
static void PutPixel(uint16_t* fb, int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= 320 || y >= 240) {
        return;
    }
    fb[(size_t) x * 240u + (size_t) (239 - y)] = color;
}

static uint16_t GreyRgb565(unsigned v) {
    return (uint16_t) (((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
}

static void PaintBorder(uint16_t* fb, int x0, int y0, int w, int h, uint16_t border) {
    int i;
    for (i = -2; i < w + 2; i++) {
        PutPixel(fb, x0 + i, y0 - 2, border);
        PutPixel(fb, x0 + i, y0 - 1, border);
        PutPixel(fb, x0 + i, y0 + h, border);
        PutPixel(fb, x0 + i, y0 + h + 1, border);
    }
    for (i = 0; i < h; i++) {
        PutPixel(fb, x0 - 2, y0 + i, border);
        PutPixel(fb, x0 - 1, y0 + i, border);
        PutPixel(fb, x0 + w, y0 + i, border);
        PutPixel(fb, x0 + w + 1, y0 + i, border);
    }
}

void gdxcs_cam_paint_preview(uint16_t* fb, int x0, int y0, int w, int h, uint16_t border) {
    int y;
    if (fb == NULL || w < 1 || h < 1 || w > PREVIEW_MAX || h > PREVIEW_MAX) {
        return;
    }
    BuildPreviewMaps(w, h);
    for (y = 0; y < h; y++) {
        const uint8_t* row = sFrameValid && sFrame != NULL ? sFrame + (size_t) sMapY[y] * FRAME_W * 2u + sPhase : NULL;
        int x;
        for (x = 0; x < w; x++) {
            const unsigned v = row != NULL ? row[(size_t) sMapX[x] * 2u] : 0x20u;
            PutPixel(fb, x0 + x, y0 + y, GreyRgb565(v));
        }
    }
    PaintBorder(fb, x0, y0, w, h, border);
}

void gdxcs_cam_stats(GdxCsCamStats* out) {
    if (out != NULL) {
        *out = sStats;
    }
}
