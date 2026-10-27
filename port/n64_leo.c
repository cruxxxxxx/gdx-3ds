/* G-Diffuser 64DD (Leo) drive replacement for the Expansion Kit.
 *
 * The decomp's leo/lib is real drive firmware traffic (command queues, motor,
 * interrupts); on PC the whole drive collapses to reads from a linear disk
 * image. LBA<->byte math comes from the decomp's own pure files
 * (leo/lib/bytetolba.c, lbatobyte.c, leo_tbl.c) compiled alongside this file;
 * everything hardware-shaped is implemented here as an immediate memcpy plus
 * a completion message, mirroring the port's other device managers.
 *
 * Compiled inside the gdiffuser_game target: decomp headers only, no MSVC CRT
 * includes (they clash with the decomp's libc headers). File I/O lives in
 * port/disk_buffer.cpp on the host side.
 */
#include "global.h"
#include "PR/leo.h"
#include "leo/leo_internal.h"

/* From PR/leoappli.h, which cannot be included alongside PR/leo.h (both
   define the LEOCmd packet types). */
#define LEO_STATUS_GOOD 0x00
#define LEO_STATUS_CHECK_CONDITION 0x02

/* Host side (port/disk_buffer.cpp) */
extern unsigned char* gdx_disk_buffer;
extern unsigned int gdx_disk_size;
extern int gdx_disk_load(void);

/* B11c -- the SD stream. gdx_disk_stream.h is declarations only (no includes of its own, and
   only unsigned int / unsigned long long / void* in its signatures), so it is safe inside
   this decomp-headers-only TU. gdx_disk_stream_mode() is 0 unless `[debug] disk_stream`
   armed a mount, so with the lever absent every function below keeps exactly the shape and
   the instruction sequence it had. */
#include "gdx_disk_stream.h"
extern int gdx_disk_stream_mode(void);
extern int gdx_disk_stream_verify_enabled(void);
extern void gdx_ek_note(const char* what, unsigned int a, unsigned int b, unsigned int c,
                        unsigned int d);

/* VERIFY MODE. Serve the read both ways and compare. This is the single most useful thing a
   user's real dump can do on its first boot: it proves the block reader byte-for-byte
   against the memcpy path it replaces, over whatever access pattern the game actually
   produces, with no test to write and no expected-output file. Logging is capped -- a
   systematic mismatch would otherwise emit one line per LEO read. */
static unsigned int sStreamVerifyOk = 0;
static unsigned int sStreamVerifyBad = 0;
static int sStreamVerifyLogs = 0;

unsigned int gdx_disk_stream_verify_ok(void) {
    return sStreamVerifyOk;
}

unsigned int gdx_disk_stream_verify_bad(void) {
    return sStreamVerifyBad;
}

/* Reads `bytes` at file `offset` into `buffer` from whichever source is armed. Returns 1 on
   success, 0 on refusal -- and ON REFUSAL `buffer` IS NOT WRITTEN. That is the contract
   gdx_disk_stream_read states, and the reason it is stated out loud: an error return that
   scribbles over the caller's buffer is precisely how ekdeadlock's first attempt at the
   AudioLoad_Dma fix overwrote its own receipt string inside the program image. */
static int gdx_leo_fetch(u32 offset, u32 bytes, void* buffer) {
    if (gdx_disk_stream_mode() <= 0) {
        if (gdx_disk_buffer == NULL) {
            return 0;
        }
        bcopy(gdx_disk_buffer + offset, buffer, bytes);
        return 1;
    }
    if (!gdx_disk_stream_read(offset, bytes, buffer)) {
        return 0;
    }
    if (gdx_disk_stream_verify_enabled() && gdx_disk_buffer != NULL &&
        (unsigned long long) offset + bytes <= (unsigned long long) gdx_disk_size) {
        /* The resident bytes are the reference here only because mode 1 keeps
           gdx_disk_save_apply's output in the buffer while the stream composes the same
           journal through its overlay. A mismatch therefore means the reader disagrees with
           the memcpy path, which is exactly what this mode exists to find. The caller still
           gets the STREAMED bytes either way; the counter carries the verdict. */
        const unsigned char* residentBytes = gdx_disk_buffer + offset;
        const unsigned char* streamedBytes = (const unsigned char*) buffer;
        u32 i;
        for (i = 0; i < bytes; i++) {
            if (residentBytes[i] != streamedBytes[i]) {
                sStreamVerifyBad++;
                if (sStreamVerifyLogs < 12) {
                    sStreamVerifyLogs++;
                    gdx_ek_note("[diskstream] VERIFY MISMATCH off/len/idx/resident", offset, bytes,
                                i, (unsigned int) residentBytes[i]);
                }
                return 1;
            }
        }
        sStreamVerifyOk++;
    }
    return 1;
}

/* Periodic receipt cadence. 512 served reads is roughly one line per few seconds of EK disk
   traffic -- often enough that a run shows the hit rate settling, rare enough that it cannot
   itself become the cost.

   B11d: it is now a LEVER (`[debug] disk_stream_report`, default 512, 0 = only the mount
   line), because 512 turned out to be larger than a whole menu-to-editor session's LEO read
   count -- so the measurement run that was supposed to size the cache produced exactly one
   receipt, taken at the mount, before any of the traffic it was meant to measure. A cadence
   you cannot lower is a cadence that can hide the thing you are measuring. */
extern void gdx_disk_stream_report(void);
extern int gdx_disk_stream_report_reads(void);
static unsigned int sStreamReadsSinceReport = 0;

static void gdx_leo_stream_tick(void) {
    int every;
    if (gdx_disk_stream_mode() <= 0) {
        return;
    }
    every = gdx_disk_stream_report_reads();
    if (every <= 0) {
        return;
    }
    if (++sStreamReadsSinceReport < (unsigned int) every) {
        return;
    }
    sStreamReadsSinceReport = 0;
    gdx_disk_stream_report();
}

/* Durable disk-save sidecar (port/disk_savefile.cpp): records the exact dirty byte
   range of every disk write so it survives to the next boot without mutating the
   pristine .ndd. Raw extern -- this decomp TU cannot include the host header. */
extern void gdx_disk_save_mark_dirty(unsigned int offset, const void* data, unsigned int len);

/* leo/lib internals: the pure translation files (leotranslat.c, leoutil.c,
   leo_tbl.c) index their zone tables by disk type. Read from the loaded
   image's system area by gdx_leo_on_disk_loaded(). */
u8 LEOdisk_type = 0;
bool __leoActive = 0;
/* Hardware-layer globals the translation math reads; the drive manager that
   would populate them is replaced by this file. */
leo_sys_form LEO_sys_data;
tgt_param_form LEOtgt_param;
LEOCmd* LEOcur_command = NULL;
s32 LEO_country_code = 0;

LEODiskID leoBootID = { 0 };

/* B11d: `disk` is NULL in mode 2 (stream-only, no resident buffer). Both reads below are tiny
   and fixed-position, so they go through the residency seam rather than through the pointer --
   one byte of the system area and one LEODiskID at physical LBA 14. A refused read leaves the
   globals exactly as they were rather than seeding the LBA math from garbage. */
extern int gdx_disk_image_read(unsigned int offset, unsigned int length, void* dst);

void gdx_leo_on_disk_loaded(const unsigned char* disk) {
    s32 offset;
    unsigned char sysByte5 = 0;

    if (disk != NULL) {
        sysByte5 = disk[5];
    } else if (!gdx_disk_image_read(5u, 1u, &sysByte5)) {
        gdx_ek_note("[leo] disk type byte unreadable; LEO layer stays inactive", 0u, 0u, 0u, 0u);
        return;
    }

    /* 64DD system area, byte 0x05: disk type (0-6) in the disk ID. */
    LEOdisk_type = sysByte5 & 0xF;
    if (LEOdisk_type > 6) {
        LEOdisk_type = 0;
    }

    /* __leoActive must be set before calling LeoLBAToByte */
    __leoActive = 1;

    /* The N64 IPL normally populates leoBootID before the game boots.
       Since we bypass the IPL, we must populate it here from the disk ID block (LBA 14). */
    if (LeoLBAToByte(0, 14, &offset) == LEO_ERROR_GOOD) {
        if (disk != NULL) {
            bcopy(disk + offset, &leoBootID, sizeof(LEODiskID));
        } else {
            gdx_disk_image_read((unsigned int) offset, (unsigned int) sizeof(LEODiskID), &leoBootID);
        }
    }
}

/* Completion for async-shaped commands: the work is already done synchronously, so post
 * immediately, as the drive manager thread eventually would. The message VALUE is the
 * LEOError -- consumers do `osRecvMesg(mq, &sSLLeoError, ...)` and switch on it
 * (sys_leo_dd.c), so success must be posted as 0/NULL (LEO_ERROR_GOOD), never a pointer. */
static void LeoPostDone(LEOCmd* cmdBlock, OSMesgQueue* mq) {
    if (cmdBlock != NULL) {
        cmdBlock->header.status = LEO_STATUS_GOOD;
    }
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)(uintptr_t)LEO_ERROR_GOOD, OS_MESG_NOBLOCK);
    }
}

/* Error completions must still post: sync callers (e.g. the audio loader's
   AudioLoad_DiskDrive) issue the request and then osRecvMesg(BLOCK) without
   checking the return value — an error return that never posts parks that
   thread forever. Console code never hits these paths, the port can. */
static void LeoPostError(LEOCmd* cmdBlock, OSMesgQueue* mq, s32 error, u32 lba, u32 nLBAs) {
    /* B11: this used to log through gdx_cki, which rides the GDX_GATE_TRACE dev gate that
     * NOTHING on 3DS ever sets -- so every failed disk read in the EK build was silent, and the
     * garbage it leaves in the caller's buffer surfaced later as a NULL sequence-script read.
     * gdx_ek_note is the always-on sink. Capped, and only ever reached when a read already
     * failed. */
    extern void gdx_ek_note(const char* what, unsigned int a, unsigned int b, unsigned int c,
                            unsigned int d);
    static int sLeoErrLogs = 0;
    if (sLeoErrLogs < 12) {
        sLeoErrLogs++;
        gdx_ek_note("[leo] READ/WRITE ERROR code/lba/nLBAs", (unsigned int)error, lba, nLBAs, 0u);
    }
    if (cmdBlock != NULL) {
        cmdBlock->header.status = LEO_STATUS_CHECK_CONDITION;
    }
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)(uintptr_t)error, OS_MESG_NOBLOCK);
    }
}

u32 LeoDriveExist(void) {
    /* The EK boot probes the drive BEFORE creating any leo manager (sys_main.c:
       gLeoDriveConnectionState = LeoDriveExist()), so this is the first disk touch and
       has to drive the idempotent image load. "A drive with a disk exists" here means
       "a disk image is available". */
    const u32 present = gdx_disk_load() ? 1u : 0u;
    /* B11d: THE receipt for gLeoDriveConnectionState. sys_main.c assigns this return value
       straight into it, and main_menu.c:222 bounces the cursor off COURSE EDIT and CREATE
       MACHINE whenever it is 0 -- so this one number decides whether the editors are
       reachable at all. sys_main's own GDX_CKI(EK3_drive_state) rides the GDX_GATE_TRACE dev
       gate that nothing on 3DS ever sets (ekdeadlock's "two silences"), so it has never
       printed on this platform. Capped at the two calls the boot actually makes. */
    static int sDriveLogs = 0;
    if (sDriveLogs < 4) {
        sDriveLogs++;
        gdx_ek_note("[leo] LeoDriveExist -> gLeoDriveConnectionState", present, 0u, 0u, 0u);
    }
    return present;
}

/* Signatures must match PR/leo.h exactly (OSMesg* cmdBuf, s32 cmdMsgCnt): GCC rejects the
 * OSMesgQueue-pointer / u32 spelling this stub previously used (MSVC tolerated the
 * pointer-type mismatch). The parameters are ignored either way. */
s32 LeoCreateLeoManager(OSPri comPri, OSPri intPri, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    (void)comPri; (void)intPri; (void)cmdBuf; (void)cmdMsgCnt;
    return gdx_disk_load() ? LEO_ERROR_GOOD : LEO_ERROR_DRIVE_NOT_READY;
}

s32 LeoCJCreateLeoManager(OSPri comPri, OSPri intPri, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    return LeoCreateLeoManager(comPri, intPri, cmdBuf, cmdMsgCnt);
}

s32 LeoCACreateLeoManager(OSPri comPri, OSPri intPri, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    return LeoCreateLeoManager(comPri, intPri, cmdBuf, cmdMsgCnt);
}

s32 LeoClearQueue(void) {
    return LEO_ERROR_GOOD;
}

s32 LeoTestUnitReady(LEOStatus* status) {
    /* B11d: "a disk is present" is a question about EITHER source. In mode 2 there is no
       resident buffer and the drive is still ready. */
    extern int gdx_disk_image_available(void);
    const int ready = gdx_disk_image_available();
    if (status != NULL) {
        *status = ready ? LEO_STATUS_GOOD : LEO_STATUS_CHECK_CONDITION;
    }
    return ready ? LEO_ERROR_GOOD : LEO_ERROR_DRIVE_NOT_READY;
}

/* System-area size at the front of an SDK-format .ndd: the first 0x18 physical LBAs
   (0..0x17) precede the user data, each a full zone-0 block (0x4D08). The image is
   block-linear, so physical byte offset = system area + user-area offset. Disk-type
   independent (vzone 0 -> pzone 0 -> 0x4D08 for every type). 0x18*0x4D08 = 0x738C0, and
   LeoLBAToByte total + 0x738C0 == the 64,931,840-byte .ndd size exactly, with all DD
   course records landing on LBA boundaries. */
#define GDX_NDD_SYSTEM_AREA_BYTES 0x738C0

/* ekaudio2 `[debug] disk_stream_coalesce_kb` (port/3ds/include/gdx3ds_disk_async.h): before a
   streamed read copies its bytes out, fetch the runs of blocks it is missing with one SD request
   each instead of one per block. `extra` is the read-ahead tail (async audio loads only; 0
   everywhere else). Only makes blocks resident -- the read below is unchanged, so are its bytes,
   its overlay and its refusal. No 3DS levers on the desktop: a no-op there. */
#if defined(GDX_PLATFORM_3DS)
extern unsigned int gdx3ds_disk_coalesce_bytes(void);
extern unsigned int gdx3ds_disk_readahead_bytes(void);
#endif

static void gdx_leo_prefill(u32 offset, u32 bytes, u32 extra) {
#if defined(GDX_PLATFORM_3DS)
    const unsigned int runBytes = gdx3ds_disk_coalesce_bytes();
    if (runBytes == 0 || gdx_disk_stream_mode() <= 0) {
        return;
    }
    (void) gdx_disk_stream_fill_range((unsigned int) offset, (unsigned int) bytes, (unsigned int) extra, runBytes);
#else
    (void) offset; (void) bytes; (void) extra;
#endif
}

/* "A disk is mounted": the resident buffer or the SD stream is live (LeoReadWrite's own guard
   below). Cheap and side-effect free, unlike gdx_disk_load(), which re-runs the file search
   while nothing is mounted -- so the EK disk audio library can ask it on every load
   (decomp-3ds-ek-nodisk-audio.patch: no disk, no disk audio). */
int gdx_leo_disk_mounted(void) {
    return (gdx_disk_buffer != NULL || gdx_disk_stream_mode() > 0) ? 1 : 0;
}

s32 LeoReadWrite(LEOCmd* cmdBlock, s32 direction, u32 LBA, void* buffer, u32 nLBAs, OSMesgQueue* mq) {
    s32 offset = 0;
    s32 bytes = 0;

    /* "A disk is mounted" now means EITHER source is live: the resident buffer, or the SD
       stream. Today mode 1 always has both, so this reads the same as it always did; it is
       written this way so that B11d's mode 2 (stream-only, no resident buffer) does not have
       to come back and re-find this guard. */
    if (!gdx_leo_disk_mounted()) {
        LeoPostError(cmdBlock, mq, LEO_ERROR_DRIVE_NOT_READY, LBA, nLBAs);
        return LEO_ERROR_DRIVE_NOT_READY;
    }
    /* LeoLBAToByte gives the LOGICAL user-area offset (data blocks only). The
       SDK .ndd is a physical/block-linear image that also stores the system
       area up front, so add it to land on the real file position. */
    if (LeoLBAToByte(0, LBA, &offset) != LEO_ERROR_GOOD ||
        LeoLBAToByte((s32)LBA, nLBAs, &bytes) != LEO_ERROR_GOOD) {
        LeoPostError(cmdBlock, mq, LEO_ERROR_LBA_OUT_OF_RANGE, LBA, nLBAs);
        return LEO_ERROR_LBA_OUT_OF_RANGE;
    }
    offset += GDX_NDD_SYSTEM_AREA_BYTES;
    if ((u32)offset + (u32)bytes > gdx_disk_size) {
        LeoPostError(cmdBlock, mq, LEO_ERROR_LBA_OUT_OF_RANGE, LBA, nLBAs);
        return LEO_ERROR_LBA_OUT_OF_RANGE;
    }

    if (buffer == NULL) {
        LeoPostError(cmdBlock, mq, LEO_ERROR_MEDIUM_NOT_PRESENT, LBA, nLBAs);
        return LEO_ERROR_MEDIUM_NOT_PRESENT;
    }

    if (direction == OS_READ) {
        /* B11c: the seam. With the stream armed this is an SD read through the block cache
           plus the journal overlay; without it, the original bcopy out of the resident image.
           A refusal posts an error and copies NOTHING, rather than handing the caller a
           half-filled buffer it will read as data. */
        gdx_leo_prefill((u32) offset, (u32) bytes, 0u);
        if (!gdx_leo_fetch((u32) offset, (u32) bytes, buffer)) {
            LeoPostError(cmdBlock, mq, LEO_ERROR_UNRECOVERED_READ_ERROR, LBA, nLBAs);
            return LEO_ERROR_UNRECOVERED_READ_ERROR;
        }
        gdx_leo_stream_tick();
    } else {
        /* Writes land in the in-memory image only; the dirty range goes to the durable
           sidecar (port/disk_savefile.cpp) so it can be replayed over a freshly loaded
           pristine image next boot. The user's .ndd is never touched -- which is also why
           the stream can serve reads straight off the file: the journal is the ONLY place a
           64DD write exists, and gdx_disk_save_overlay replays it into every streamed read.
           The invalidate is what makes the next read of this range re-compose rather than
           return a cache block filled before the write. */
        if (gdx_disk_buffer != NULL) {
            bcopy(buffer, gdx_disk_buffer + offset, bytes);
        }
        gdx_disk_save_mark_dirty((unsigned int) offset, buffer, (unsigned int) bytes);
        if (gdx_disk_stream_mode() > 0) {
            gdx_disk_stream_invalidate((unsigned int) offset, (unsigned int) bytes);
        }
    }

    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

/* o3dsekperf `[debug] audio_disk_async` (port/3ds/include/gdx3ds_disk_async.h): the async disk
   audio loader's reads, served by an I/O worker instead of inline on the audio producer. The
   decomp's AudioLoad_DiskLoad state machine already polls its queue across ticks, exactly as it
   does for a real drive; this only stops the port from finishing the "DMA" before returning.
   The worker runs THIS file's LeoReadWrite, unchanged, so the bytes, the error posts and the
   completion message are the inline path's. Returns 1 when queued (the worker posts `mq`), 0
   when the caller must call LeoReadWrite itself: lever off, a write, or no 3DS worker (desktop).
   One slot, because the state machine has at most one request in flight. */
#if defined(GDX_PLATFORM_3DS)
typedef void (*GdxDiskAsyncJob)(void* arg);
extern int gdx3ds_disk_async_enabled(void);
extern int gdx3ds_disk_async_submit(GdxDiskAsyncJob job, void* arg);
extern void gdx3ds_disk_async_wait_idle(void);

typedef struct GdxLeoAsyncReq {
    LEOCmd* cmdBlock;
    u32 lba;
    void* buffer;
    u32 nLBAs;
    OSMesgQueue* mq;
    u32 loadAhead; /* bytes the load still wants past this request (the decomp's bytesRemaining) */
} GdxLeoAsyncReq;

static GdxLeoAsyncReq sLeoAsyncReq;

/* ekaudio2 `[debug] audio_disk_readahead_kb`: the in-flight load's next bytes ride on this
   request's SD request, up to the window measured from the request's start. The load is
   LBA-contiguous on the image (block-linear .ndd), so "the next bytes" are the next file
   bytes. `loadAhead` over-states the head read by one LBA's worth at most (bytesRemaining
   still counts the head) -- a tail that lands inside the window anyway. */
static void gdx_leo_async_readahead(const GdxLeoAsyncReq* req) {
    s32 offset = 0;
    s32 bytes = 0;
    u32 extra;
    const u32 window = gdx3ds_disk_readahead_bytes();
    if (window == 0 || req->loadAhead == 0) {
        return;
    }
    if (LeoLBAToByte(0, req->lba, &offset) != LEO_ERROR_GOOD ||
        LeoLBAToByte((s32) req->lba, req->nLBAs, &bytes) != LEO_ERROR_GOOD) {
        return; /* LeoReadWrite refuses the same request on its own terms */
    }
    if ((u32) bytes >= window) {
        return; /* the request alone fills the window; its own prefill coalesces it */
    }
    extra = window - (u32) bytes;
    if (extra > req->loadAhead) {
        extra = req->loadAhead;
    }
    gdx_leo_prefill((u32) offset + GDX_NDD_SYSTEM_AREA_BYTES, (u32) bytes, extra);
}

static void gdx_leo_async_job(void* arg) {
    GdxLeoAsyncReq* req = (GdxLeoAsyncReq*) arg;
    gdx_leo_async_readahead(req);
    LeoReadWrite(req->cmdBlock, OS_READ, req->lba, req->buffer, req->nLBAs, req->mq);
}
#endif

int gdx_leo_read_async(LEOCmd* cmdBlock, s32 direction, u32 lba, void* buffer, u32 nLBAs, OSMesgQueue* mq,
                       s32 loadAhead) {
#if defined(GDX_PLATFORM_3DS)
    if (direction != OS_READ || !gdx3ds_disk_async_enabled()) {
        return 0;
    }
    /* The slot is rewritten below, so a job still running must finish first. */
    gdx3ds_disk_async_wait_idle();
    sLeoAsyncReq.cmdBlock = cmdBlock;
    sLeoAsyncReq.lba = lba;
    sLeoAsyncReq.buffer = buffer;
    sLeoAsyncReq.nLBAs = nLBAs;
    sLeoAsyncReq.mq = mq;
    sLeoAsyncReq.loadAhead = loadAhead > 0 ? (u32) loadAhead : 0u;
    return gdx3ds_disk_async_submit(gdx_leo_async_job, &sLeoAsyncReq);
#else
    (void) cmdBlock; (void) direction; (void) lba; (void) buffer; (void) nLBAs; (void) mq; (void) loadAhead;
    return 0;
#endif
}

/* Blocks until the async read in flight (if any) has posted. The reset drain in
   AudioLoad_ProcessAsyncLoadDiskDrive calls this instead of spinning on the state machine. */
void gdx_leo_async_wait_idle(void) {
#if defined(GDX_PLATFORM_3DS)
    gdx3ds_disk_async_wait_idle();
#endif
}

s32 LeoSeek(LEOCmd* cmdBlock, u32 lba, OSMesgQueue* mq) {
    (void)lba;
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoRezero(LEOCmd* cmdBlock, OSMesgQueue* mq) {
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoSpdlMotor(LEOCmd* cmdBlock, LEOSpdlMode mode, OSMesgQueue* mq) {
    (void)mode;
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoReadDiskID(LEOCmd* cmdBlock, LEODiskID* vaddr, OSMesgQueue* mq) {
    s32 offset = 0;
    if (vaddr != NULL) {
        /* Physical LBA 14. LeoLBAToByte(0, 14) gives 14 * 0x4D08 with no system-area bias
           because every system-area block is a zone-0 block, so the logical offset of 14
           blocks and the file offset of physical LBA 14 are the same number. */
        if (LeoLBAToByte(0, 14, &offset) != LEO_ERROR_GOOD ||
            !gdx_leo_fetch((u32) offset, (u32) sizeof(*vaddr), vaddr)) {
            /* Zeroing here is safe and is NOT the pattern ekdeadlock warns about: `vaddr` is
               the caller's own LEODiskID, not a pointer derived from unloadable disk data,
               and a zero disk ID is what the original code already produced with no disk. */
            bzero(vaddr, sizeof(*vaddr));
        }
    }
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoModeSelectAsync(LEOCmd* cmdBlock, u32 standby, u32 sleep, OSMesgQueue* mq) {
    (void)standby; (void)sleep;
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoReadRTC(LEOCmd* cmdBlock, OSMesgQueue* mq) {
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoSetRTC(LEOCmd* cmdBlock, LEODiskTime* RTCdata, OSMesgQueue* mq) {
    (void)RTCdata;
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoInquiry(LEOVersion* ver) {
    if (ver != NULL) {
        bzero(ver, sizeof(*ver));
    }
    return LEO_ERROR_GOOD;
}

extern const u16 LEORAM_START_LBA[7];
extern const s32 LEORAM_BYTE[7];

s32 LeoReadCapacity(LEOCapacity* cmdBlock, s32 dir) {
    /* Mirror decomp/src/leo/lib/readcapacity.c exactly. The writable-area
       startLBA MUST include the -0x18 bias: MFS recovers the disk type by
       matching startLBA against LEO_LBA_RAM_TOPn (= LEORAM_START_LBA[t] - 0x18)
       and derives gDirectoryEntryCount from it. Dropping the bias makes
       Mfs_RamGetDiskType() return 6 (invalid) and corrupts the directory-entry
       count, overrunning gMfsRamArea.directoryEntry[] on the first menu that
       touches saved data. */
    if (!__leoActive) {
        return LEO_ERROR_DRIVE_NOT_READY;
    }
    if (cmdBlock != NULL) {
        if (dir == OS_WRITE) {
            cmdBlock->startLBA = LEORAM_START_LBA[LEOdisk_type] - 0x18;
            cmdBlock->endLBA = LEO_LBA_MAX;
            cmdBlock->nbytes = LEORAM_BYTE[LEOdisk_type];
        } else {
            cmdBlock->startLBA = 0;
            cmdBlock->endLBA = LEO_LBA_MAX;
            cmdBlock->nbytes = 0x3D78F40; /* total capacity, ~64.45 MB */
        }
    }
    return LEO_ERROR_GOOD;
}

void LeoBootGame(void* entry) {
    /* On hardware this jumps into the disk's boot program. Here every disk-side function
       is compiled in, so booting reduces to making sure the image is loaded. */
    (void)entry;
    gdx_disk_load();
}

/* Drive-ROM font address helpers. The decomp ships LeoGetKAdr/LeoGetAAdr only as incbin'd
 * MIPS blobs, so these are C ports of the routines disassembled from the US rev0 ROM (leo
 * overlay, text at ROM 0x80870 / 0x813B0). Both index tables live in that overlay's data
 * section and are read from the loaded ROM image:
 *   kanji index table (s16): VRAM 0x80411850 -> ROM 0x80960 (0xA48 bytes)
 *   ANK metrics table (4B):  VRAM 0x8041231C -> ROM 0x8142C (0x908 entries)
 * Returned offsets are relative to the drive ROM's font block; callers add
 * DDROM_FONT_START and read from the IPL ROM image (port/disk_buffer.cpp). */
extern unsigned char* gdx_rom_buffer;
extern size_t gdx_rom_size;

#define GDX_ROM_KANJI_INDEX_TBL 0x80960u  /* s16[1316], codes 0x81xx-0x87xx */
#define GDX_ROM_ANK_METRICS_TBL 0x8142Cu  /* 4 bytes per entry, 0x908 entries */

static s16 gdx_rom_s16(u32 off) {
    return (s16)((gdx_rom_buffer[off] << 8) | gdx_rom_buffer[off + 1]);
}

/* The kanji index table and the ANK metrics table are contiguous in the cart ROM and covered
 * by a single `kanji_tables` blob [0x80960, 0x8384C) -- the index table (0xA48 B), a small
 * gap, then the ANK table (0x908 entries x 4 B, last entry ending at 0x83848).
 * LeoGetKAdr/LeoGetAAdr are per-glyph HOT paths, so the byte-source shim must NOT be called
 * per glyph: the whole blob is preloaded ONCE into a static buffer and every glyph read is
 * served from it. If that preload fails (no archive AND no/short raw ROM), reads fall back
 * to gdx_rom_buffer per glyph. */
extern int GdxSegmentSourceRead(unsigned int romBase, unsigned int size, void* dst);
#define GDX_KANJI_BLOB_BASE 0x80960u
#define GDX_KANJI_BLOB_SPAN 0x2EECu /* [0x80960, 0x8384C): kanji index + ANK metrics */

/* Same publish discipline as gdx_segment_source.c's SEG_*_FENCE (fill buffer, release-fence,
 * THEN publish the state; readers acquire-load the state before touching the buffer), sized
 * down for this TU's single blob. Forward-declared rather than including <intrin.h>: this TU
 * is decomp-headers-only, and MSVC recognizes _ReadWriteBarrier once pragma'd. */
#if defined(_MSC_VER)
extern void _ReadWriteBarrier(void);
#pragma intrinsic(_ReadWriteBarrier)
#define GDX_KANJI_ACQUIRE_FENCE() _ReadWriteBarrier()
#define GDX_KANJI_RELEASE_FENCE() _ReadWriteBarrier()
#else
#define GDX_KANJI_ACQUIRE_FENCE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define GDX_KANJI_RELEASE_FENCE() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif

static u8 sKanjiBlob[GDX_KANJI_BLOB_SPAN];
static volatile int sKanjiBlobState = 0; /* 0 = unattempted, 1 = loaded, 2 = failed */

/* Returns the resident blob buffer, or NULL when the one-shot preload failed (the caller
 * then reads raw ROM per glyph). The first-use race is benign: concurrent callers copy the
 * same bytes and settle on the same monotonic state, and GdxSegmentSourceRead is itself
 * locked. The acquire fence orders the state load before any buffer read; the release fence
 * between filling sKanjiBlob and publishing sKanjiBlobState is its counterpart, so a reader
 * observing state==1 never sees a torn buffer. */
static const u8* gdx_kanji_blob(void) {
    int state = sKanjiBlobState;
    GDX_KANJI_ACQUIRE_FENCE();
    if (state == 0) {
        int ok = GdxSegmentSourceRead(GDX_KANJI_BLOB_BASE, GDX_KANJI_BLOB_SPAN, sKanjiBlob);
        GDX_KANJI_RELEASE_FENCE();
        state = ok ? 1 : 2;
        sKanjiBlobState = state;
    }
    return (state == 1) ? sKanjiBlob : NULL;
}

/* Spelled `int`, not s32: PR/leo.h declares these with plain int, and ultratypes'
 * s32 is `long` on non-LP64 hosts -- same width on ILP32, but a distinct type,
 * which is a conflicting-declaration error on the 32-bit ARM build. */
int LeoGetKAdr(int sjis) {
    s32 row, cell;

    if (sjis < 0x8140 || sjis >= 0x9873) {
        return -1;
    }
    cell = (sjis & 0xFF) - 0x40;
    if (cell >= 0x40) {
        cell -= 1; /* the 0x7F column does not exist in shift-JIS */
    }
    if (sjis >= 0x8800) {
        /* Kanji block: arithmetic mapping, 188 cells per row, appended after
           the 0x30A glyphs of the symbol/kana block. */
        row = (sjis >> 8) - 0x88;
        return (cell + 0x30A + row * 0xBC) << 7;
    }
    /* Symbol/kana block (0x81xx-0x87xx): sparse, resolved via the ROM's
       s16 glyph-index table. */
    row = (sjis >> 8) - 0x81;
    {
        u32 tbl = GDX_ROM_KANJI_INDEX_TBL + (u32)(cell + row * 0xBC) * 2u;
        const u8* blob = gdx_kanji_blob();
        if (blob != NULL) {
            u32 rel = tbl - GDX_KANJI_BLOB_BASE; /* GDX_ROM_KANJI_INDEX_TBL == blob base */
            if (rel + 2u > GDX_KANJI_BLOB_SPAN) {
                return -1;
            }
            return (s32)(s16)((blob[rel] << 8) | blob[rel + 1]) << 7;
        }
        if (gdx_rom_buffer == NULL || tbl + 2 > gdx_rom_size) {
            return -1;
        }
        return (s32)gdx_rom_s16(tbl) << 7;
    }
}

int LeoGetAAdr(int code, int* dx, int* dy, int* cy) {
    u32 entry;
    u32 off;
    u8 b2;
    s8 b3;

    if (code < 0 || code >= 0x908) {
        return -1;
    }
    entry = GDX_ROM_ANK_METRICS_TBL + (u32)code * 4u;
    {
        const u8* blob = gdx_kanji_blob();
        if (blob != NULL) {
            u32 rel = entry - GDX_KANJI_BLOB_BASE;
            if (rel + 4u > GDX_KANJI_BLOB_SPAN) {
                return -1;
            }
            off = ((u32)blob[rel] << 8) | blob[rel + 1];
            b2 = blob[rel + 2];
            b3 = (s8)blob[rel + 3];
        } else {
            if (gdx_rom_buffer == NULL || entry + 4 > gdx_rom_size) {
                return -1;
            }
            off = ((u32)gdx_rom_buffer[entry] << 8) | gdx_rom_buffer[entry + 1];
            b2 = gdx_rom_buffer[entry + 2];
            b3 = (s8)gdx_rom_buffer[entry + 3];
        }
    }
    /* MFS warm-up calls pass NULL metric pointers. The MIPS original stored blindly, which
       was harmless on the N64 null page but is not here. */
    if (dy != NULL) {
        *dy = (b2 & 0xF) + 1;
    }
    if (dx != NULL) {
        *dx = (s32)(((u32)(b3 & 1) << 4) | ((u32)b2 >> 4));
    }
    if (cy != NULL) {
        *cy = b3 >> 1;
    }
    return (s32)(off << 1) + 0x7EE80;
}

/* MFS version B work buffer — a fixed expansion-RAM address on hardware
   (see include/leo/mfs.h); real storage here. */
u8 D_807801E0[0x4D10];
