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

void gdx_leo_on_disk_loaded(const unsigned char* disk) {
    s32 offset;
    /* 64DD system area, byte 0x05: disk type (0-6) in the disk ID. */
    LEOdisk_type = disk[5] & 0xF;
    if (LEOdisk_type > 6) {
        LEOdisk_type = 0;
    }

    /* __leoActive must be set before calling LeoLBAToByte */
    __leoActive = 1;

    /* The N64 IPL normally populates leoBootID before the game boots.
       Since we bypass the IPL, we must populate it here from the disk ID block (LBA 14). */
    if (LeoLBAToByte(0, 14, &offset) == LEO_ERROR_GOOD) {
        bcopy(disk + offset, &leoBootID, sizeof(LEODiskID));
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
    extern void gdx_cki(const char* s, int v);
    static int sLeoErrLogs = 0;
    if (sLeoErrLogs < 12) {
        sLeoErrLogs++;
        gdx_cki("[leo] READ/WRITE ERROR code", error);
        gdx_cki("[leo] READ/WRITE ERROR lba", (int)lba);
        gdx_cki("[leo] READ/WRITE ERROR nLBAs", (int)nLBAs);
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
    return gdx_disk_load() ? 1u : 0u;
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
    if (status != NULL) {
        *status = (gdx_disk_buffer != NULL) ? LEO_STATUS_GOOD : LEO_STATUS_CHECK_CONDITION;
    }
    return (gdx_disk_buffer != NULL) ? LEO_ERROR_GOOD : LEO_ERROR_DRIVE_NOT_READY;
}

/* System-area size at the front of an SDK-format .ndd: the first 0x18 physical LBAs
   (0..0x17) precede the user data, each a full zone-0 block (0x4D08). The image is
   block-linear, so physical byte offset = system area + user-area offset. Disk-type
   independent (vzone 0 -> pzone 0 -> 0x4D08 for every type). 0x18*0x4D08 = 0x738C0, and
   LeoLBAToByte total + 0x738C0 == the 64,931,840-byte .ndd size exactly, with all DD
   course records landing on LBA boundaries. */
#define GDX_NDD_SYSTEM_AREA_BYTES 0x738C0

s32 LeoReadWrite(LEOCmd* cmdBlock, s32 direction, u32 LBA, void* buffer, u32 nLBAs, OSMesgQueue* mq) {
    s32 offset = 0;
    s32 bytes = 0;

    if (gdx_disk_buffer == NULL) {
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
        bcopy(gdx_disk_buffer + offset, buffer, bytes);
    } else {
        /* Writes land in the in-memory image only; the dirty range goes to the durable
           sidecar (port/disk_savefile.cpp) so it can be replayed over a freshly loaded
           pristine image next boot. The user's .ndd is never touched. */
        bcopy(buffer, gdx_disk_buffer + offset, bytes);
        gdx_disk_save_mark_dirty((unsigned int) offset, buffer, (unsigned int) bytes);
    }

    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
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
        if (gdx_disk_buffer != NULL && LeoLBAToByte(0, 14, &offset) == LEO_ERROR_GOOD) {
            bcopy(gdx_disk_buffer + offset, vaddr, sizeof(*vaddr));
        } else {
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
