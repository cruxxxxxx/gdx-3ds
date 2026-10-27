// G-Diffuser — 64DD disk image loader (Expansion Kit). Host-side counterpart of
// port/n64_leo.c: owns the CRT file I/O the gdiffuser_game target cannot include.
#include "port_log.h"
#include "rom_buffer.h"
#include "gdx_extract_launch.h"
#include "gen/EkTranslatedOverrides.h"
// B11m: the build-time ENGLISH TEXT PACK -- translated art and overlay text for a player
// whose own 64DD dump is the retail Japanese one. gEkTextPackRowCount is 0 unless
// tools/gen_ek_text_pack.py was run with a translated image, which is off by default.
#include "gdx_ek_text_pack.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gdx_disk_stream.h"
#include "gdx_disk_dcrc.h"

#if defined(GDX_PLATFORM_3DS)
#include "gdx3ds_fs.h"     // gdx3ds_fs_base_path() -- the 3DS arm of gdx_exe_dir
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback);
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h> // readlink
#endif

extern "C" {
unsigned char* gdx_disk_buffer = nullptr;
unsigned int gdx_disk_size = 0;

// 64DD IPL/drive ROM image (user-supplied): holds the built-in kanji/ANK font the EK
// text renderers read through LeoGetKAdr/LeoGetAAdr + DDROM_FONT_START.
unsigned char* gdx_ddipl_buffer = nullptr;
unsigned int gdx_ddipl_size = 0;

void gdx_ek_assets_fill(const unsigned char* disk, unsigned long long diskSize);
// port/gen/EkAssetPack.c (B11e): the build-time asset bake. gEkAssetPackSize is 0 unless
// tools/gen_ek_assets.py was run with --bake, which is off by default.
extern "C" const unsigned char gEkAssetPack[];
extern "C" const unsigned int gEkAssetPackSize;
extern "C" const unsigned long long gEkAssetPackDiskCrc64;
// port/gdx_ek_strings.c: copies the translated disk's own English into the overlay
// string symbols. Declared here because those are C TUs with no header of their own.
void gdx_ek_strings_apply(const unsigned char* disk, unsigned long long diskSize);
// port/disk_savefile.cpp: init fingerprints the pristine image and loads any existing
// sidecar; apply replays the saved dirty ranges over it.
void gdx_disk_save_init(const char* diskName, const unsigned char* pristine, unsigned int size);
// B11c: one-shot fingerprint hand-over, so the sidecar does not recompute the CRC-64 the
// loader already took over identical bytes. EK-only in disk_savefile.cpp.
void gdx_disk_save_set_source_hash(unsigned long long crc);
void gdx_disk_save_apply(unsigned char* buffer);
// The same CRC-64/XZ implementation disk_savefile.cpp fingerprints with, reused below for
// EK disk-variant detection rather than carrying a second CRC-64 table.
unsigned long long gdx_disk_crc64(const unsigned char* data, unsigned long long length);
// port/gdx_ek_disk_overrides.c: overwrites three garbled I8 glyphs in the translated disk's
// re-authored Create-Machine label sub-block. No-op for Course Edit.
void gdx_ek_disk_overrides_apply(void);
void gdx_leo_on_disk_loaded(const unsigned char* disk);

// 64DD boot-logo source texture (136x39 RGBA16), zero-filled by the EK asset generator at
// build time and populated from the loaded disk image by gdx_ek_assets_fill(). sys_main.c's
// func_806F33D0 CPU-blits it straight into the VI framebuffer with no RDP task, which is what
// the texel swap in gdx_disk_finalize exists for. Declared here to avoid decomp headers.
extern unsigned short D_80769DF0[];

static void gdx_dir_of(const char* path, char* outDir, size_t outSize);
static void gdx_exe_dir(char* outDir, size_t outSize);

// port/AssetLoader.cpp: copies min(fileSize, outSize) bytes of a mounted o2r entry into `out`,
// returns 1 on success.
extern "C" int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);

// Frozen 64DD IPL geometry: the drive-ROM font block starts at 0xA0000 inside a 0x140000-byte
// image. The port allocates that full size and places the font block at its real offset, so every
// consumer's guard (fontAddr >= 0xA0000 && fontAddr + 0x80 <= gdx_ddipl_size) and LeoGetKAdr's
// arithmetic keep holding unchanged.
#define GDX_DDROM_FONT_START 0xA0000u
#define GDX_DDIPL_LOGICAL_SIZE 0x140000u
#define GDX_DDIPL_FONT_BLOCK_BYTES (GDX_DDIPL_LOGICAL_SIZE - GDX_DDROM_FONT_START) // 0xA0000

// The archived font slice is already normalized to big-endian by the gdx-extract `ipl` step, so
// no swap happens here. Returns false when n64ddipl.o2r is absent; the raw-file fallback carries.
static bool gdx_ddipl_load_from_archive(void) {
    unsigned char* buf = static_cast<unsigned char*>(malloc(GDX_DDIPL_LOGICAL_SIZE));
    if (buf == nullptr) {
        return false;
    }
    memset(buf, 0, GDX_DDIPL_LOGICAL_SIZE);
    size_t copied = 0;
    if (!GDiffuser_LoadArchiveFileBytes("ipl/font_block", buf + GDX_DDROM_FONT_START,
                                        GDX_DDIPL_FONT_BLOCK_BYTES, &copied) ||
        copied != GDX_DDIPL_FONT_BLOCK_BYTES) {
        free(buf);
        return false;
    }
    gdx_ddipl_buffer = buf;
    gdx_ddipl_size = GDX_DDIPL_LOGICAL_SIZE;
    gdx_port_logf("[leo] 64DD IPL font block served from n64ddipl.o2r (%u bytes at 0x%X)\n",
                  GDX_DDIPL_FONT_BLOCK_BYTES, GDX_DDROM_FONT_START);
    return true;
}

// Game.DdIplPath from gdx_firstboot.cfg. Both directories are probed because in installed mode
// the process CWD is the data dir, while the dev/portable layout keeps the cfg next to the exe.
static bool gdx_firstboot_ipl_path(char* outPath, size_t outSize) {
    if (outSize == 0) {
        return false;
    }
    outPath[0] = '\0';
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    const char* dirs[] = { "", exeDir }; // "" == current working directory
    for (const char* dir : dirs) {
        char cfgPath[1200];
        snprintf(cfgPath, sizeof(cfgPath), "%sgdx_firstboot.cfg", dir);
        FILE* cf = fopen(cfgPath, "rb");
        if (cf == nullptr) {
            continue;
        }
        char line[2048];
        bool found = false;
        while (fgets(line, sizeof(line), cf) != nullptr) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            const char* kv = "Game.DdIplPath=";
            const size_t kvLen = strlen(kv);
            if (strncmp(line, kv, kvLen) == 0 && line[kvLen] != '\0') {
                strncpy(outPath, line + kvLen, outSize - 1);
                outPath[outSize - 1] = '\0';
                found = true;
                break;
            }
        }
        fclose(cf);
        if (found) {
            return true;
        }
    }
    return false;
}

// Tries the firstboot-recorded path first, then the known dump names next to the exe, the chosen
// ROM, and the working directory. A bare CWD-relative open silently loses the drive-ROM font
// whenever the launch CWD is not the data dir. Returns an open FILE* (caller closes) or null.
static FILE* gdx_ddipl_open_raw(char* chosenPath, size_t chosenSize) {
    if (chosenSize > 0) {
        chosenPath[0] = '\0';
    }
    char recorded[1024] = {};
    if (gdx_firstboot_ipl_path(recorded, sizeof(recorded)) && recorded[0] != '\0') {
        FILE* f = fopen(recorded, "rb");
        if (f != nullptr) {
            if (chosenSize > 0) {
                strncpy(chosenPath, recorded, chosenSize - 1);
                chosenPath[chosenSize - 1] = '\0';
            }
            return f;
        }
    }
    // Both known dump filenames are probed in each directory: a dev-tree boot has no
    // firstboot-recorded path, so this loop is the only chance to find the US-prototype dump.
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    char romDir[1024] = {};
    gdx_dir_of(gdx_rom_path, romDir, sizeof(romDir));
    const char* dirs[] = { exeDir, romDir, "" };
    const char* names[] = { "N64DDIPLROM.n64", "64DD_IPL_US_MJR.n64" };
    for (const char* dir : dirs) {
        for (const char* name : names) {
            char path[1200];
            snprintf(path, sizeof(path), "%s%s", dir, name);
            FILE* f = fopen(path, "rb");
            if (f != nullptr) {
                if (chosenSize > 0) {
                    strncpy(chosenPath, path, chosenSize - 1);
                    chosenPath[chosenSize - 1] = '\0';
                }
                return f;
            }
        }
    }
    return nullptr;
}

// Raw-file fallback. IPL dumps circulate as z64 (big-endian), v64 (16-bit swapped) or n64
// (32-bit swapped); the first byte of the PI header is 0x80 in native order, which is what the
// detection below keys off to normalize them.
static void gdx_ddipl_load_from_raw(void) {
    char chosen[1200] = {};
    FILE* f = gdx_ddipl_open_raw(chosen, sizeof(chosen));
    if (f == nullptr) {
        gdx_port_logf("[leo] no N64DDIPLROM.n64 (and no n64ddipl.o2r); drive-ROM font stays blank\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x100000) { // sanity: IPL ROM dumps are 4MB
        fclose(f);
        gdx_port_logf("[leo] %s too small (%ld bytes); ignored\n", chosen, sz);
        return;
    }
    unsigned char* buf = static_cast<unsigned char*>(malloc(static_cast<size_t>(sz)));
    if (buf == nullptr || fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    if (buf[0] == 0x80) {
        // native big-endian
    } else if (buf[1] == 0x80) {
        for (long i = 0; i + 1 < sz; i += 2) { // v64: swap 16-bit pairs
            unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
        }
    } else if (buf[3] == 0x80) {
        for (long i = 0; i + 3 < sz; i += 4) { // n64: reverse 32-bit words
            unsigned char t0 = buf[i], t1 = buf[i + 1];
            buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2];
            buf[i + 2] = t1; buf[i + 3] = t0;
        }
    } else {
        gdx_port_logf("[leo] %s: unrecognized byte order (first bytes %02X %02X %02X %02X); using as-is\n",
                      chosen, buf[0], buf[1], buf[2], buf[3]);
    }
    gdx_ddipl_buffer = buf;
    gdx_ddipl_size = static_cast<unsigned int>(sz);
    gdx_port_logf("[leo] 64DD IPL ROM loaded from %s (%ld bytes)\n", chosen, sz);
}

// Archive first, so a completed setup can delete the user's IPL ROM file.
static void gdx_ddipl_load(void) {
    if (gdx_ddipl_buffer != nullptr) {
        return;
    }
    if (gdx_ddipl_load_from_archive()) {
        return;
    }
    gdx_ddipl_load_from_raw();
}

// Country code from the standard N64 header (offset 0x3E), only readable once the z64 magic
// word confirms native byte order. gdx_init_rom() does not normalize v64/n64 dumps, so those
// answer "not confidently Japanese" and keep the translated-first disk preference.
static bool gdx_rom_is_japanese(void) {
    if (gdx_rom_buffer == nullptr || gdx_rom_size < 0x40) {
        return false;
    }
    if (gdx_rom_buffer[0] != 0x80 || gdx_rom_buffer[1] != 0x37) {
        return false;
    }
    return gdx_rom_buffer[0x3E] == 'J';
}

// Directory (including trailing separator) of a file path, or "" when it has none. Both
// separators are handled: ROM paths arrive from a Windows picker or a hand-typed FZEROX_ROM.
static void gdx_dir_of(const char* path, char* outDir, size_t outSize) {
    outDir[0] = '\0';
    if (path == nullptr || path[0] == '\0' || outSize == 0) {
        return;
    }
    const char* slash = strrchr(path, '\\');
    const char* fwdSlash = strrchr(path, '/');
    if (fwdSlash != nullptr && (slash == nullptr || fwdSlash > slash)) {
        slash = fwdSlash;
    }
    if (slash == nullptr) {
        return;
    }
    size_t len = static_cast<size_t>(slash - path) + 1; // keep the separator
    if (len >= outSize) {
        len = outSize - 1;
    }
    memcpy(outDir, path, len);
    outDir[len] = '\0';
}

static void gdx_exe_dir(char* outDir, size_t outSize) {
    outDir[0] = '\0';
    if (outSize == 0) {
        return;
    }
#if defined(GDX_PLATFORM_3DS)
    /* B11c. A 3dsx has no executable path to read back: no GetModuleFileNameW, no
     * /proc/self/exe. Before this arm existed gdx_exe_dir returned "" on 3DS, and the FIRST
     * entry of gdx_disk_load's search order -- the managed copy under <exeDir>/media/ --
     * silently collapsed to the bare relative path "media/...", i.e. to whatever the launch
     * CWD happened to be. The exe-directory entry collapsed onto the CWD entry, the
     * gdx_firstboot.cfg probe lost one of its two locations, and the IPL-ROM search lost one
     * of its three. The port's own answer to "where does this build's data live" is
     * gdx3ds_fs_base_path() -- "sdmc:/3ds/gdiffuser/", trailing slash included, the same root
     * the ini, the log and the .o2r archives already use (port/3ds/include/gdx3ds_fs.h). */
    const char* base = gdx3ds_fs_base_path();
    if (base == nullptr || base[0] == '\0' || strlen(base) >= outSize) {
        return;
    }
    strcpy(outDir, base);
#elif defined(_WIN32)
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash == nullptr) {
        return;
    }
    slash[1] = L'\0';
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, outDir, static_cast<int>(outSize), nullptr, nullptr);
#else
    char exePath[4096];
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n <= 0) {
        return;
    }
    exePath[n] = '\0';
    char* slash = strrchr(exePath, '/');
    if (slash == nullptr) {
        return;
    }
    slash[1] = '\0'; // keep trailing separator
    if (strlen(exePath) >= outSize) {
        return;
    }
    strcpy(outDir, exePath);
#endif
}

// Canonical retail/translated EK image size, and the .gdd save key for archive-sourced loads.
// gdx_firstboot.cpp copies every disk source to this leaf name, so an archive built from the
// managed copy replays saves under the same key the file path would have produced.
// INVARIANT: .gdd keying is by canonical leaf name, never by path.
#define GDX_DISK_EXACT_BYTES 64931840u
static const char* const kDiskArchiveSaveKey = "baserom.translated.ek.ndd";

/* ===================================================================== B11c ==
 * SD STREAMING -- the lever, the CRC policy, and the residency census.
 *
 * Default OFF. `[debug] disk_stream` in gdiffuser.ini:
 *   0  off (the shipped behaviour): the whole 61.9 MB image is malloc'd and resident.
 *   1  the stream is mounted and SERVES THE LBA PATH (port/n64_leo.c). The resident buffer
 *      is still allocated, because five consumers still index it directly (see the census
 *      below), so this mode saves no memory. What it buys is that every LEO read goes
 *      through the reader against a real disk, which is the thing a user's dump must
 *      validate first.
 *   2  stream-only, no resident buffer. REFUSED while the census below is non-empty --
 *      those consumers would read through a null pointer. The refusal is logged and the
 *      mount degrades to mode 1 rather than half-working.
 * `[debug] disk_stream_verify=1` serves every LBA read BOTH ways and compares, which is how
 * a real dump proves the reader byte-for-byte against the memcpy path it replaces.
 * `[debug] disk_stream_cache_kb` (default 512, RE-DERIVED in B11d -- see below) and
 * `disk_stream_block_kb` (default 16), plus `disk_stream_report` (LEO reads per receipt).
 *
 * CACHE SIZING, MEASURED. diskstream sized 2 MiB against the EK asset-fill trace and said
 * outright that the boot fill "does not size this cache". ekcart then retired that consumer
 * with a build-time bake. B11d measured the real thing, against the retail JP disk streamed
 * off SD in mode 2, over a boot -> main menu -> COURSE EDIT session (and, identically, over
 * menus-longtail): 785 LEO reads, 1,078 KB requested, and a WORKING SET of 89 distinct 16 KiB
 * blocks = 1,424 KB. Sweeping the lever over one identical session:
 *
 *   cacheKB   128    256    512   1024   2048   4096
 *   refetch    13      5      5      4      2      0
 *   hostKB   1808   1520   1504   1488   1456   1424
 *
 * The entire 128 KiB -> 4 MiB span is 384 KB of card reads, ONCE, at boot -- because the
 * traffic is 854 lookups over 89 distinct blocks, i.e. almost all first-touch. Capacity
 * therefore buys almost nothing here, and the right default is the smallest one the memory
 * budget likes: 512 KiB costs 80 KB more card reads per boot than an infinite cache and
 * gives back 1.5 MB of heap, which matters on the projected Old 3DS layout where 2 MiB was
 * 8.4% of an already-short grant. Raise it to 2048 if you have the headroom.
 *
 * WHAT THAT MEASUREMENT DOES NOT COVER, and it is the honest half: no course was loaded or
 * saved, and no MEDIUM_LBA audio streamed (the 31 MB block at user LBA 1004 was never
 * read -- 1,078 KB is the whole session). Audio is sequential and wants read-ahead rather
 * than capacity, so it should not move this number, but that is an argument, not a
 * measurement. The MFS management area is 6 of the 89 blocks (96 KB): two 3-LBA windows at
 * user LBA 3062 and 3065, file blocks 3117..3122.
 *
 * THE RESIDENCY CENSUS -- CLOSED by B11d (docs/research/native/ekdrive-progress.md). Every
 * place that used to need gdx_disk_buffer to be a pointer now goes through ONE seam,
 * gdx_disk_image_read() below, which serves the resident buffer when there is one and the
 * stream when there is not:
 *   1. port/gen/EkAssetBindings.c  gdx_ek_assets_fill()         765 rows, 655,956 B
 *   2. port/gen/EkAssetBindings.c  gdx_ek_segment_image_fill()  (decomp_port.c:988)
 *   3. port/gdx_ek_strings.c       gdx_ek_strings_apply()
 *   4. decomp/src/sys/disk/disk_drive_dd.c   DiskDrive_LoadData's DD-course read
 *   5. decomp/src/sys/disk/disk_mount_dd.c   the MFS byte-order pass's mount test
 *   plus gdx_disk_finalize's own translated-override memcpy, and gdx_disk_save_apply, which
 *   is a no-op in mode 2 by construction (there is no buffer to replay INTO -- the journal
 *   is composed into every read by gdx_disk_save_overlay instead).
 * Rows 4 and 5 are decomp files and live in port/3ds/patches/decomp-3ds-disk-residency-seam.patch.
 * Mode 2 (stream-only, no 61.9 MB resident buffer) is therefore REAL and no longer refuses.
 *
 * THE CRC POLICY. Loading a disk today costs TWO whole-image CRC-64 passes: one in
 * gdx_disk_load (the Japanese-region identity gate plus the layout-variant decision) and one
 * in gdx_disk_save_init (the .gdd sidecar's source fingerprint). disk_buffer.cpp's own
 * comment at gdx_disk_finalize accepts the duplicate because on a desktop it is "a few
 * milliseconds". Streaming makes that unaffordable for a different reason: each pass becomes
 * 61.9 MB of SD reads, which is the entire cost streaming exists to avoid.
 *
 * Neither pass is an INTEGRITY check and neither is anything the retail drive did. The 64DD
 * drive's own integrity is per-sector C1/C2 ECC computed in its sequencer
 * (decomp/src/leo/lib/leoc2ecc.c) -- a .ndd does not carry it and this port does not model
 * it. Copy protection lived in the physical format and the region code, not in a CRC-64/XZ,
 * an algorithm that postdates the hardware. All three consumers ask an IDENTITY question --
 * "which disk is this?" -- and all three ask it of the same bytes and get the same number.
 *
 * So: compute it ONCE, and cache it across boots.
 *   - Deduplicated: gdx_disk_finalize now passes its fingerprint to gdx_disk_save_init
 *     instead of letting it recompute. Two passes become one, on every platform.
 *   - Cached: a `<leaf>.dcrc` sidecar records the image size, a 256 KiB SAMPLE fingerprint
 *     and the full CRC-64. A boot that matches size+sample skips the full pass entirely,
 *     so the steady-state cost is 256 KiB of reads instead of 123.8 MB.
 *   - NOT lazy-per-block, and the reason is structural rather than a preference: the JP gate
 *     must answer BEFORE the buffer is installed, and the variant decision selects the
 *     override table gdx_disk_finalize applies. A fingerprint that is only complete after
 *     the last block is read cannot answer either question, ever.
 *   - NOT dropped. Dropping it drops the Japanese-region gate (which the project deliberately
 *     implemented on identity rather than filename, precisely so a rename cannot walk past
 *     it) and the sidecar keying (which is what stops one disk's saves being replayed onto
 *     another -- a data-loss hazard, not a convenience).
 * The sample check is why the cache VALIDATES rather than TRUSTS: a different image at the
 * same path with the same size still misses.
 *
 * B11m -- WHERE THE RECORD LIVES AND WHY IT IS NOW LEGIBLE. The sidecar itself moved to
 * port/gdx_disk_dcrc.{h,cpp}: this TU needs decomp headers and cannot be compiled for the
 * host, so the one feature whose failure mode is "silently does nothing, forever" had no
 * test. It now has one (port/tests/gdx_disk_stream_tests.cpp case 9), it creates its own
 * directory instead of assuming disk_savefile.cpp already did, it checks every write, and
 * every outcome has a NAME that the receipt below prints. The first hardware boot with a
 * real disk said `crc64=0x...(computed)` and nothing more, and that single token cannot
 * distinguish "no sidecar existed" from "the sidecar was rejected" from "the write failed".
 * NOTE for anyone looking for the file: it is under saves/, NOT beside the image in media/.
 * The receipt prints the full path so nobody has to guess again.
 */
#define GDX_DCRC_SAMPLE_WINDOWS 64u
#define GDX_DCRC_SAMPLE_WINDOW_BYTES 4096u

static int s_diskStreamMode = 0;      /* resolved once per boot, 0 until a mount happens */
static int s_diskStreamVerify = 0;
/* `[debug] disk_stream_report` -- LEO reads per periodic receipt. Resolved once, at the mount,
 * so the read path stays an integer compare. */
static int s_diskStreamReportReads = 512;

extern "C" int gdx_disk_stream_report_reads(void) {
    return s_diskStreamReportReads;
}

static int gdx_disk_lever(const char* key, int fallback) {
#if defined(GDX_PLATFORM_3DS)
    return gdx3ds_config_get_int("debug", key, fallback);
#else
    /* No lever surface on the desktop build: streaming is a 3DS memory answer and the
     * desktop has no shortage it solves. Compiled in so the code stays one code path. */
    (void) key;
    return fallback;
#endif
}

int gdx_disk_stream_mode(void) {
    return s_diskStreamMode;
}

/* B11d -- THE RESIDENCY SEAM. Every consumer that used to spell "copy N bytes at image offset
 * O" as `gdx_disk_buffer + O` now spells it as this call, so mode 2 (stream-only, no 61.9 MB
 * resident buffer) becomes a property of ONE function instead of seven call sites.
 *
 * Resident first, then the stream. In mode 0/1 the buffer exists and this is the memcpy it
 * always was; in mode 2 there is no buffer and the stream serves it.
 *
 * ON REFUSAL `dst` IS NOT WRITTEN -- not zeroed, not partially filled. Same contract and same
 * reason as gdx_disk_stream_read: ekdeadlock's first AudioLoad_Dma fix zero-filled a
 * destination derived from data it had just declared unloadable, and that ramAddr pointed
 * inside the program image. A caller that ignores the return value gets its old bytes, never
 * new wrong ones. */
extern "C" int gdx_disk_image_read(unsigned int offset, unsigned int length, void* dst) {
    if (dst == nullptr) {
        return 0;
    }
    if (length == 0) {
        return 1;
    }
    if (gdx_disk_buffer != nullptr) {
        if ((unsigned long long) offset + length > (unsigned long long) gdx_disk_size) {
            return 0;
        }
        memcpy(dst, gdx_disk_buffer + offset, length);
        return 1;
    }
    if (s_diskStreamMode > 0) {
        return gdx_disk_stream_read(offset, length, dst);
    }
    return 0;
}

/* "Is there an image behind this session at all" -- the question every
 * `gdx_disk_buffer != NULL` test was really asking. */
extern "C" int gdx_disk_image_available(void) {
    return (gdx_disk_buffer != nullptr || (s_diskStreamMode > 0 && gdx_disk_stream_active())) ? 1 : 0;
}

extern "C" unsigned int gdx_disk_image_size(void) {
    return gdx_disk_size;
}

/* The lever, read once and cached. The ARCHIVE path would otherwise reconstruct a 61.9 MB
 * resident image before the stream ever gets a chance, which in mode 2 is exactly the
 * allocation the mode exists to avoid. Returns the REQUESTED mode, not the served one. */
static int s_diskStreamRequested = -1;

/* The 3DS cannot hold a 61.9 MB resident image on either model, so streaming (mode 2) is its
 * default; without it the resident malloc below fails and no disk ever mounts. Desktop keeps
 * the resident image. An explicit [debug] disk_stream key still wins on both. */
#if defined(GDX_PLATFORM_3DS)
#define GDX_DISK_STREAM_DEFAULT 2
#else
#define GDX_DISK_STREAM_DEFAULT 0
#endif

static int gdx_disk_stream_requested(void) {
    if (s_diskStreamRequested < 0) {
        s_diskStreamRequested = gdx_disk_lever("disk_stream", GDX_DISK_STREAM_DEFAULT);
    }
    return s_diskStreamRequested;
}

int gdx_disk_stream_verify_enabled(void) {
    return s_diskStreamVerify;
}

/* The dirty-range overlay: 64DD writes never reach the user's .ndd, so a streamed read of
 * pristine file bytes must have the journal replayed over it to equal what the resident
 * image would have returned after gdx_disk_save_apply(). Implemented in disk_savefile.cpp. */
extern void gdx_disk_save_overlay(unsigned int offset, unsigned int length, void* dst);

/* 64 fixed 4 KiB windows spread evenly over the image, positional so that a different image
 * of the same size at the same path misses the cache instead of being mistaken for it.
 *
 * B11m -- IT WAS NOT 256 KiB, AND NOW IT IS. The windows used to be read through the block
 * cache and then invalidated again, which cost a whole 16 KiB block per 4 KiB window: the
 * mount receipt says so out loud, `reads=64 readKB=256 ... miss=82 ... hostKB=1312`. 1,312 KB
 * off the card, five times the sample it was reporting, to produce bytes it immediately threw
 * away -- because a cache warmed with 64 scattered windows is a cache full of the wrong
 * blocks, which is why the invalidate was there. Reading uncached does the same job, leaves
 * the cache cold by construction, needs no invalidate, and costs exactly the 256 KiB
 * diskstream section 3 promised.
 *
 * It also fixes a disagreement nobody had hit yet: gdx_disk_stream_crc64 sweeps PRISTINE file
 * bytes, while a cached read composes the 64DD write overlay over them. The two halves of one
 * identity must be taken of the same bytes. The journal is empty at mount today, so this is a
 * latent difference rather than a measured bug -- but it is the kind that only shows up once
 * a real user has saved a course. */
static unsigned long long gdx_disk_sample_fingerprint(unsigned int size) {
    unsigned char window[GDX_DCRC_SAMPLE_WINDOW_BYTES];
    unsigned long long acc = 0xCBF29CE484222325ull;
    for (unsigned int i = 0; i < GDX_DCRC_SAMPLE_WINDOWS; i++) {
        unsigned long long off =
            (unsigned long long) i * (size - GDX_DCRC_SAMPLE_WINDOW_BYTES) / GDX_DCRC_SAMPLE_WINDOWS;
        if (!gdx_disk_stream_read_uncached((unsigned int) off, GDX_DCRC_SAMPLE_WINDOW_BYTES,
                                           window)) {
            return 0;
        }
        acc ^= gdx_disk_crc64(window, GDX_DCRC_SAMPLE_WINDOW_BYTES);
        acc *= 0x100000001B3ull;
    }
    return acc ? acc : 1ull; /* 0 is the "no sample" sentinel */
}

/* The sidecar itself now lives in port/gdx_disk_dcrc.{h,cpp} so that it can be host-tested.
 * This is the only thing that stayed behind: the base directory is a port concept
 * (gdx_exe_dir, whose 3DS arm B11c added), and the unit takes it as a parameter rather than
 * reaching for it. */
static void gdx_dcrc_base_dir(char* out, size_t outSize) {
    gdx_exe_dir(out, outSize);
}

/* Mounts `path` as a stream and resolves the pristine fingerprint, cheaply if possible.
 * Returns 1 when the stream is live; 0 leaves everything as it was and the caller falls back
 * to the resident path. On success *outCrc holds the image's CRC-64. */
static int gdx_disk_stream_mount(const char* path, const char* diskName, unsigned long long* outCrc) {
    const int mode = gdx_disk_stream_requested();
    if (mode <= 0) {
        return 0;
    }
    const unsigned int cacheKb = (unsigned int) gdx_disk_lever("disk_stream_cache_kb", 512);
    const unsigned int blockKb = (unsigned int) gdx_disk_lever("disk_stream_block_kb", 16);
    unsigned int blockLog2 = GDX_DISK_STREAM_DEFAULT_BLOCK_LOG2;
    for (unsigned int b = 12; b <= 18; b++) {
        if ((1u << b) == blockKb * 1024u) {
            blockLog2 = b;
        }
    }
    unsigned int blocks = (cacheKb * 1024u) >> blockLog2;
    if (blocks == 0) {
        blocks = GDX_DISK_STREAM_DEFAULT_BLOCKS;
    }

    if (!gdx_disk_stream_open(path, GDX_DISK_EXACT_BYTES, blockLog2, blocks)) {
        gdx_port_logf("[diskstream] REFUSED %s: not a %u-byte 64DD image (or unreadable); "
                      "falling back to the resident loader\n",
                      path, GDX_DISK_EXACT_BYTES);
        return 0;
    }
    gdx_disk_stream_set_overlay(gdx_disk_save_overlay);

    /* The CRC policy, applied. Every branch below is NAMED, because the whole cost this
     * cache exists to remove -- 61.9 MB of card reads, about a minute of boot on a New 3DS --
     * is paid by exactly the branches that used to be silent. */
    char baseDir[1024] = {};
    gdx_dcrc_base_dir(baseDir, sizeof(baseDir));
    char dcrcPath[1200] = {};
    if (!gdx_dcrc_build_path(baseDir, diskName, dcrcPath, sizeof(dcrcPath))) {
        snprintf(dcrcPath, sizeof(dcrcPath), "<unresolved>");
    }

    const unsigned long long sample = gdx_disk_sample_fingerprint(GDX_DISK_EXACT_BYTES);
    unsigned long long crc = 0;
    const GdxDcrcLoadResult loaded =
        gdx_dcrc_load(baseDir, diskName, GDX_DISK_EXACT_BYTES, sample, &crc);
    GdxDcrcStoreResult stored = GDX_DCRC_WRITE_SKIPPED;
    int storeErrno = 0;
    char crcSource[96];
    if (loaded == GDX_DCRC_HIT) {
        /* Kept as the bare token `(cached)` on purpose: diskstream section 10 step 5 and
         * three progress documents tell a reader to grep for exactly this. */
        snprintf(crcSource, sizeof(crcSource), "cached");
    } else {
        int ok = 0;
        crc = gdx_disk_stream_crc64(&ok);
        if (!ok) {
            gdx_port_logf("[diskstream] REFUSED %s: the image could not be read end to end\n", path);
            gdx_disk_stream_close();
            return 0;
        }
        stored = gdx_dcrc_store(baseDir, diskName, GDX_DISK_EXACT_BYTES, sample, crc, &storeErrno);
        snprintf(crcSource, sizeof(crcSource), "computed,dcrc=%s,write=%s",
                 gdx_dcrc_load_text(loaded), gdx_dcrc_store_text(stored));
    }
    *outCrc = crc;

    /* B11d: mode 2 was an explicit REFUSAL here while five consumers still indexed
     * gdx_disk_buffer directly. All five now go through gdx_disk_image_read (three in
     * port/, two in decomp patches), so mode 2 is real and serves with NO resident buffer.
     * Mode 1 keeps both sources, which is what disk_stream_verify=1 compares. */
    s_diskStreamMode = (mode >= 2) ? 2 : 1;
    s_diskStreamVerify = gdx_disk_lever("disk_stream_verify", 0) ? 1 : 0;
    s_diskStreamReportReads = gdx_disk_lever("disk_stream_report", 512);
    if (s_diskStreamMode >= 2 && s_diskStreamVerify) {
        /* Verify compares the stream against the resident buffer. Mode 2 has no resident
         * buffer, so there is nothing to compare against and the counters would read 0/0 --
         * which looks like "verified" rather than "not run". Say which one it is. */
        gdx_port_logf("[diskstream] verify=1 ignored in mode=2: there is no resident buffer to "
                      "compare against. Run disk_stream=1 disk_stream_verify=1 for that\n");
        s_diskStreamVerify = 0;
    }

    char stats[256];
    gdx_disk_stream_format_stats(stats, sizeof(stats));
    gdx_port_logf("[diskstream] mode=%d verify=%d file=%s size=%u crc64=0x%016llX(%s) %s\n",
                  s_diskStreamMode, s_diskStreamVerify, path, gdx_disk_stream_size(), crc,
                  crcSource, stats);
    /* Always-on second line: the PATH, which no receipt has ever carried, and the two
     * verdicts spelled out. The first hardware boot's report went looking for the sidecar in
     * media/ beside the image -- it is in saves/ beside the .gdd -- and the log had no way to
     * say so. `nextBoot=fast` is the single thing a user needs to read off this line. */
    gdx_port_logf("[diskstream] dcrc=%s load=%s write=%s errno=%d sample=0x%016llX nextBoot=%s\n",
                  dcrcPath, gdx_dcrc_load_text(loaded), gdx_dcrc_store_text(stored), storeErrno,
                  sample,
                  (loaded == GDX_DCRC_HIT || stored == GDX_DCRC_WRITE_OK) ? "fast" : "FULL-PASS");
    gdx_disk_stream_reset_stats();
    return 1;
}

/* Periodic receipt, driven from the LBA path (port/n64_leo.c calls this every 512 served
 * reads) so the log carries a MEASURED hit rate and a measured verify verdict rather than a
 * promise. Without it the only [diskstream] line in a run would be the mount, and §10 step 4 --
 * choosing the cache size against the user's dump -- would have no number to read. */
extern unsigned int gdx_disk_stream_verify_ok(void);
extern unsigned int gdx_disk_stream_verify_bad(void);

void gdx_disk_stream_report(void) {
    if (!gdx_disk_stream_active()) {
        return;
    }
    char stats[256];
    gdx_port_logf("[diskstream] %s verify_ok=%u verify_bad=%u\n",
                  gdx_disk_stream_format_stats(stats, sizeof(stats)),
                  gdx_disk_stream_verify_ok(), gdx_disk_stream_verify_bad());
    /* ekaudio2 `[debug] disk_stream_coalesce_kb`: how many of the fills above were coalesced
       runs. hostReads in the line above counts each run once, so `blocks - runs` is the SD
       requests the runs saved. Silent until the first run, so a lever-off log is unchanged. */
    unsigned long long fillRuns = 0;
    unsigned long long fillBlocks = 0;
    gdx_disk_stream_fill_stats(&fillRuns, &fillBlocks);
    if (fillRuns != 0) {
        gdx_port_logf("[diskfill] runs=%llu blocks=%llu saved=%llu SD requests\n", fillRuns,
                      fillBlocks, fillBlocks - fillRuns);
    }
}

// Deletion-gate verdict: 1 only after a boot both reconstructed the disk from fzerox-disk.o2r
// and proved SHA-256(reconstructed) == the managed-copy sha. The Data & Files panel offers
// deletion only on a passed verdict; nothing here ever deletes a user file.
static int s_diskArchiveVerified = 0;

// Loaded 64DD disk-layout variant, decided from the pristine image's CRC64 in gdx_disk_finalize.
// 0 = JP retail layout, and the safe default for an unrecognized disk since gdx_ek_assets_fill's
// JP-derived offset table serves it correctly; 1 = the fan-translated disk, whose reshaped
// Create-Machine label sub-block needs the gEkTranslatedOverrides[] re-copy. decomp-side blit
// sites (machine_create_draw.c) read this through gdx_ek_disk_is_translated() to pick the label
// textures' width/height.
static int s_ekDiskTranslated = 0;

int gdx_ek_disk_is_translated(void) {
    return s_ekDiskTranslated;
}

/* Japanese-region gate (CMake option GDX_ALLOW_JP_INPUTS, default OFF — see port/CMakeLists.txt).
   Callers must run this BEFORE installing the buffer into gdx_disk_buffer, so a refused image
   never reaches gdx_disk_finalize's consumers.

   Identity, not filename: dropping the JP name from the search list below closes the ordinary
   door, but a Japanese image renamed to the translated disk's filename would walk straight past
   a name check. The CRC-64 is the same fingerprint gdx_disk_finalize needs for its layout-variant
   decision, so the two share one pass over the image. */
static int gdx_disk_is_refused_japanese(unsigned long long pristineCrc64) {
#ifdef GDX_ALLOW_JP_INPUTS
    (void) pristineCrc64;
    return 0;
#else
    return pristineCrc64 == GDX_EK_JP_DISK_CRC64;
#endif
}

// Common post-load tail for all three mount branches. Requires the PRISTINE image to be
// READABLE -- through gdx_disk_buffer in mode 0/1, through the SD stream in mode 2, and in
// both cases through gdx_disk_image_read. B11d: every direct index of gdx_disk_buffer below
// became that call, which is the whole of what made mode 2 possible. `diskName` keys the .gdd
// sidecar (canonical leaf name only, never a path). `pristineCrc64` is the caller's CRC-64 of those
// same bytes: the callers already compute it for the Japanese-region gate, which must run before
// the buffer is installed, so it is passed in rather than recomputed here.
static void gdx_disk_finalize(const char* diskName, const char* sourceLabel,
                              unsigned long long pristineCrc64) {
    gdx_port_logf("[leo] disk source: %s\n", sourceLabel);
    gdx_leo_on_disk_loaded(gdx_disk_buffer);
    gdx_ek_assets_fill(gdx_disk_buffer, static_cast<unsigned long long>(gdx_disk_size));
    gdx_ek_disk_overrides_apply();

    // The fill above used the retail-JP-derived offset table, correct for the JP disk and for
    // nearly all of the fan-translated one -- except the three Create-Machine label textures the
    // translator's recompile moved and/or reshaped. On the translated disk only, re-copy those
    // from their TRANSLATED offsets, overwriting what the fill just wrote. Nothing above mutates
    // the disk buffer, so the caller's pristine CRC64 is still valid here.
    {
        const unsigned long long diskCrc64 = pristineCrc64;
        if (diskCrc64 == GDX_EK_TRANSLATED_DISK_CRC64) {
            s_ekDiskTranslated = 1;
            unsigned int applied = 0;
            for (unsigned int i = 0; i < gEkTranslatedOverrideCount; i++) {
                const GdxEkTranslatedOverride& ov = gEkTranslatedOverrides[i];
                // ov.destCapacity is the destination array's real sizeof, computed at generation
                // time. Skip rather than overflow if a future disk variant reshapes a symbol
                // larger than the table's known destination budget.
                if (ov.size > ov.destCapacity) {
                    gdx_port_logf("[ek-translated] SKIP %s: size %u exceeds dest capacity %u\n",
                                  ov.name, ov.size, ov.destCapacity);
                    continue;
                }
                /* B11d row "gdx_disk_finalize's own translated-override memcpy": through the
                   seam, so mode 2 (no resident buffer) serves it from the stream. */
                if (gdx_disk_image_read(ov.translatedDiskOffset, ov.size, ov.dest)) {
                    applied++;
                }
            }
            gdx_port_logf("[ek-translated] applied %u translated-layout overrides (disk variant: translated)\n",
                          applied);

            // Same idea for what is not a texture: the disk-status messages, Create Machine
            // prompts, driver names and Course Edit tooltips the decomp compiles in as Japanese
            // C initializers. The translated disk carries English for them in its own overlay
            // .data, so the port binds to that rather than shipping a translation of its own.
            // On the JP disk, or any unrecognized one, the compiled-in Japanese stands.
            gdx_ek_strings_apply(gdx_disk_buffer, static_cast<unsigned long long>(gdx_disk_size));
        } else if (diskCrc64 == GDX_EK_JP_DISK_CRC64) {
            s_ekDiskTranslated = 0;
            gdx_port_logf("[ek-translated] 0 translated-layout overrides applied (disk variant: JP retail -- none needed)\n");
        } else {
            s_ekDiskTranslated = 0;
            gdx_port_logf("[ek-translated] unknown variant CRC64=0x%016llX, serving JP layout\n", diskCrc64);
        }

        /* B11m -- the English text pack. The two arms above only ever fire for ONE exact
           image, because their gate is a whole-image CRC-64 and nddconv (B11l) showed that
           fingerprint necessarily covers the system area's per-medium defect map and the MFS
           RAM area: two dumps of two physical discs can never agree. So a player whose own
           dump is the retail Japanese one -- which is every player with a real disc, and the
           dump the drive, the MFS volume and the DD cups all come from -- has no source of
           English bytes at all.

           The pack is that source, and it is applied LAST on purpose: the user's disk is
           authoritative for everything except the words, so the fill above runs unchanged and
           only the rows tools/gen_ek_text_pack.py measured to be text-bearing are overwritten.
           It is skipped on the translated disk itself, where the disk already served them.

           Killswitch `[debug] ek_english` (default 1). It costs nothing when the pack is
           empty, which is the default build. */
        if (gEkTextPackRowCount != 0 && !s_ekDiskTranslated) {
            if (gdx_disk_lever("ek_english", 1)) {
                if (gdx_ek_text_pack_apply() > 0) {
                    /* The pack carries the reshaped Create-Machine labels at their translated
                       geometry, so the three decomp blit sites that read this flag
                       (machine_create_draw.c:402/908, course_edit/191080.c:913) and
                       expansion_kit/A3AE0.c:535's menu repoint must all take the translated
                       arm -- exactly as they would with the translated disk mounted. */
                    s_ekDiskTranslated = 1;
                }
            } else {
                gdx_port_logf("[ek-english] disabled by [debug] ek_english=0; EK text stays "
                              "as the loaded disk wrote it\n");
            }
        }
    }

    /* The fill above copied raw BIG-ENDIAN disk bytes, but func_806F33D0 (sys_main.c) CPU-blits
       these u16s into a VI framebuffer everything else treats as host-order, and that blit is this
       texture's only consumer. Decoded-TEXTURE fixup, NOT a disk-image normalization: the disk
       buffer itself is never byte-swapped, which is why the archive can store it verbatim. */
    for (unsigned int i = 0; i < 5304; i++) {
        const unsigned short v = D_80769DF0[i];
        D_80769DF0[i] = static_cast<unsigned short>((v >> 8) | (v << 8));
    }

    gdx_ddipl_load();

    // gdx_disk_buffer is still the PRISTINE image here (everything above takes it as const or
    // patches decoded C arrays), so the fingerprint init needs is the pristine one -- and it is
    // bit for bit the `pristineCrc64` this function was handed, over the same bytes.
    //
    // B11c: that duplicate used to stand ("a second ~62 MB table-based pass costs a few
    // milliseconds once per boot and is not worth widening gdx_disk_save_init's signature
    // for -- each caller owning its own fingerprint is safer"). It does not stand under
    // streaming, where the second pass is 61.9 MB of SD reads. The value is handed over
    // instead, as a ONE-SHOT preset that gdx_disk_save_init consumes and clears -- so a later
    // mount with no preset computes its own rather than inheriting this disk's fingerprint,
    // which is the failure the old "each caller owns its own" rule was guarding against. The
    // invariant this rests on is the one the paragraph above already asserts in prose: nothing
    // between the two points mutates the buffer. See port/disk_savefile.cpp.
    //
    // B11d: in mode 2 `gdx_disk_buffer` is null and the preset above is what keys the sidecar,
    // so gdx_disk_save_init is handed the null and consumes the preset. gdx_disk_save_apply is
    // then a no-op by construction (there is no buffer to replay INTO) and correctly so: in
    // mode 2 the journal is composed into every read by gdx_disk_save_overlay instead, which
    // is the reader's own contract.
#ifdef EXPANSION_KIT
    gdx_disk_save_set_source_hash(pristineCrc64);
#endif
    gdx_disk_save_init(diskName, gdx_disk_buffer, gdx_disk_size);
    gdx_disk_save_apply(gdx_disk_buffer);
}

// The archive stores the disk bytes VERBATIM (the loader never byte-swaps the disk buffer -- see
// gdx_disk_finalize), so an archive-sourced image is byte-identical to a file-sourced one and the
// .gdd replay, gdx_ek_assets_fill and byte-order handling all run unchanged. Returns 1 on success
// (buffer installed and finalized), 0 to fall through to the managed-copy/raw-file search.
static int gdx_disk_load_from_archive(void) {
    unsigned char* buf = static_cast<unsigned char*>(malloc(GDX_DISK_EXACT_BYTES));
    if (buf == nullptr) {
        return 0;
    }
    size_t copied = 0;
    if (!GDiffuser_LoadArchiveFileBytes("disk/image", buf, GDX_DISK_EXACT_BYTES, &copied) ||
        copied != GDX_DISK_EXACT_BYTES) {
        free(buf);
        return 0;
    }

    // An archive built by a JP-enabled development build, or carried into a release install with
    // the rest of the game folder, must not become the loaded disk. Falling through to the file
    // search is the right recovery: a translated .ndd or managed copy beside the game still boots.
    const unsigned long long pristineCrc64 =
        gdx_disk_crc64(buf, static_cast<unsigned long long>(GDX_DISK_EXACT_BYTES));
    if (gdx_disk_is_refused_japanese(pristineCrc64)) {
        gdx_port_logf("[leo] REJECTED fzerox-disk.o2r: it holds the retail JAPANESE Expansion Kit "
                      "disk, and Japanese-region support is not enabled in this build (configure "
                      "with -DGDX_ALLOW_JP_INPUTS=ON to enable it)\n");
        free(buf);
        return 0;
    }

    // Deletion gate: SHA-256 of the reconstructed image must equal the managed-copy sha (sidecar
    // disk_sha256) before the Data & Files panel may EVER offer deletion. It has to be taken on
    // the PRISTINE bytes here, before gdx_disk_save_apply (inside finalize) replays saves over
    // them. A missing managed copy or any mismatch leaves the verdict false -- the gate never
    // suggests deletion on unproven bytes.
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    std::string archiveSha = gdx::GdxExtractSha256Bytes(buf, static_cast<unsigned long long>(GDX_DISK_EXACT_BYTES));
    std::string recorded = gdx::GdxExtractRecordedDiskSha256(exeDir);
    if (recorded.empty() && exeDir[0] != '\0') {
        char managedPath[1200];
        snprintf(managedPath, sizeof(managedPath), "%smedia/%s", exeDir, kDiskArchiveSaveKey);
        recorded = gdx::GdxExtractFileSha256(managedPath);
    }
    s_diskArchiveVerified = (!recorded.empty() && archiveSha == recorded) ? 1 : 0;
    if (s_diskArchiveVerified) {
        gdx_port_logf("[leo] disk archive verified byte-identical to the managed copy (SHA-256 %s); "
                      "original .ndd and managed copy are deletable\n",
                      archiveSha.c_str());
    } else {
        gdx_port_logf("[leo] disk archive NOT verified (reconstructed %s vs recorded %s); deletion "
                      "stays gated\n",
                      archiveSha.empty() ? "(none)" : archiveSha.c_str(),
                      recorded.empty() ? "(none)" : recorded.c_str());
    }

    gdx_disk_buffer = buf;
    gdx_disk_size = GDX_DISK_EXACT_BYTES;
    gdx_port_logf("[leo] disk image reconstructed from fzerox-disk.o2r (%u bytes)\n", GDX_DISK_EXACT_BYTES);
    gdx_disk_finalize(kDiskArchiveSaveKey, "archive", pristineCrc64);
    return 1;
}

int gdx_disk_archive_verified(void) {
    return s_diskArchiveVerified;
}

int gdx_disk_load(void) {
    if (gdx_disk_buffer != nullptr) {
        return 1;
    }
    /* B11d: in mode 2 the stream IS the mount, so a second call must report the drive as
       present without re-mounting. `gdx_disk_buffer` stays null for the whole session. */
    if (s_diskStreamMode >= 2 && gdx_disk_stream_active()) {
        return 1;
    }

    /* The archive path reconstructs a 61.9 MB RESIDENT image from fzerox-disk.o2r. In mode 2
       that is precisely the allocation the mode exists to avoid, and taking it would silently
       downgrade the run to mode 1 with no receipt. Skip it and let the file search mount the
       .ndd as a stream. */
    if (gdx_disk_stream_requested() < 2 && gdx_disk_load_from_archive()) {
        return 1;
    }

    // The fan-translated English disk (LuigiBlood/Zoinkity, 64DD.org) keeps the retail disk
    // layout, so loading it instead of the JP dump delivers English EK text/font assets through
    // the same offsets and MFS reads, with no extraction step. It is therefore the default, and
    // the fallback whenever the loaded ROM's region cannot be determined. A confirmed Japanese
    // cartridge prefers the untranslated JP disk so on-disk text matches the cartridge.
    const bool jpRom = gdx_rom_is_japanese();
#ifdef GDX_ALLOW_JP_INPUTS
    static const char* const kUsPreferredNames[] = {
        "baserom.translated.ek.ndd",
        "baserom.jp.ek.ndd",
        "baserom.jp.disk",
    };
    static const char* const kJpPreferredNames[] = {
        "baserom.jp.ek.ndd",
        "baserom.jp.disk",
        "baserom.translated.ek.ndd",
    };
    const char* const* diskNames = jpRom ? kJpPreferredNames : kUsPreferredNames;
    const size_t diskNameCount = 3;
#else
    /* Japanese-region gate, cheap half: the Japanese disk FILENAMES are not searched at all, so a
       .ndd dropped beside the executable under its usual JP name is never opened (no read, no
       62 MB CRC pass, no log noise). The authoritative half is the CRC-64 identity check below.
       `jpRom` stays in use for the provenance log line further down. */
    static const char* const kTranslatedOnlyNames[] = {
        "baserom.translated.ek.ndd",
    };
    const char* const* diskNames = kTranslatedOnlyNames;
    const size_t diskNameCount = 1;
#endif

    // Search order. The managed copy under <exeDir>/media wins: gdx_firstboot.cpp creates it
    // byte-identically from the user's original under the SAME canonical leaf name, so the .gdd
    // save key (leaf name only, never a path) is unaffected by preferring it. Next to the chosen
    // ROM comes second so separate install folders with different .ndd files never
    // cross-pollinate, then the exe directory, then the working directory as a last resort for
    // scripted/dev launches that rely on CWD.
    char romDir[1024] = {};
    gdx_dir_of(gdx_rom_path, romDir, sizeof(romDir));
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    char mediaDir[1040] = {};
    if (exeDir[0] != '\0') {
        snprintf(mediaDir, sizeof(mediaDir), "%smedia/", exeDir);
    }

    struct SearchLocation {
        const char* dir;
        const char* why;
        bool managed;
    };
    const SearchLocation searchLocations[] = {
        { mediaDir, "managed copy (media/)", true },
        { romDir, "next to chosen ROM", false },
        { exeDir, "exe directory", false },
        { "", "current directory", false },
    };

    for (const SearchLocation& loc : searchLocations) {
        for (size_t i = 0; i < diskNameCount; i++) {
            char path[1200];
            snprintf(path, sizeof(path), "%s%s", loc.dir, diskNames[i]);

            FILE* f = fopen(path, "rb");
            if (f == nullptr) {
                continue;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0) {
                fclose(f);
                continue;
            }
            /* gdx_leo_on_disk_loaded and LeoReadDiskID read the LBA-14 disk ID and system area
               at fixed offsets with no size guard, and gdx_ek_assets_fill walks generated
               diskOffset tables, so a truncated .ndd OOB-reads immediately. Retail 64DD images
               are ~64.9 MB. */
            if (sz < (long)(60u * 1024u * 1024u)) {
                gdx_port_logf("[leo] REJECTED %s: %ld bytes (truncated .ndd image, need ~64.9 MB)\n", path, sz);
                fclose(f);
                continue;
            }
            /* B11c: arm the SD stream for this candidate BEFORE the 61.9 MB malloc, because
               the mount is also what resolves the pristine fingerprint -- from the .dcrc
               sidecar when it validates, otherwise by one streamed pass. Either way the
               whole-image CRC below is not taken a second time. Default OFF, so with
               `[debug] disk_stream` absent this is one integer read and a return. */
            unsigned long long streamCrc64 = 0;
            const int streamed = gdx_disk_stream_mount(path, diskNames[i], &streamCrc64);

            /* B11d -- MODE 2: the stream is the only source. No 61.9 MB malloc, no fread, no
               resident image. Every consumer reaches the bytes through gdx_disk_image_read,
               which the mount above has just armed, so the rest of the mount runs unchanged.
               The Japanese-region gate runs FIRST and on the same fingerprint, because it must
               still refuse before anything is installed. */
            if (streamed && s_diskStreamMode >= 2) {
                fclose(f);
                if (gdx_disk_is_refused_japanese(streamCrc64)) {
                    gdx_disk_stream_close();
                    s_diskStreamMode = 0;
                    gdx_port_logf("[leo] REJECTED %s: this is the retail JAPANESE Expansion Kit disk, "
                                  "and Japanese-region support is not enabled in this build (configure "
                                  "with -DGDX_ALLOW_JP_INPUTS=ON to enable it)\n", path);
                    continue;
                }
                gdx_disk_size = static_cast<unsigned int>(sz);
                gdx_port_logf("[leo] disk image STREAMED: %s (%ld bytes) -- picked from %s, region=%s, "
                              "no resident buffer (disk_stream=2)\n", path, sz, loc.why,
                              jpRom ? "JP (matches cartridge ROM)" : "US/unknown (default preference)");
                gdx_disk_finalize(diskNames[i], loc.managed ? "managed (streamed)" : "original (streamed)",
                                  streamCrc64);
                return 1;
            }

            unsigned char* buf = static_cast<unsigned char*>(malloc(static_cast<size_t>(sz)));
            if (buf == nullptr) {
                gdx_port_logf("[leo] FAILED to load %s: no memory for the %ld-byte resident image "
                              "(set [debug] disk_stream = 2 to stream it instead); Expansion Kit "
                              "modes stay disabled\n", path, sz);
                fclose(f);
                if (streamed) {
                    gdx_disk_stream_close();
                    s_diskStreamMode = 0;
                }
                return 0;
            }
            if (fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
                free(buf);
                fclose(f);
                if (streamed) {
                    gdx_disk_stream_close();
                    s_diskStreamMode = 0;
                }
                continue;
            }
            fclose(f);
            /* Identity gate, before the buffer is installed: catches a Japanese image renamed to
               a filename this build does search. Continue the search rather than failing
               outright, so a translated disk in a later location still wins. */
            const unsigned long long pristineCrc64 =
                streamed ? streamCrc64 : gdx_disk_crc64(buf, static_cast<unsigned long long>(sz));
            if (gdx_disk_is_refused_japanese(pristineCrc64)) {
                if (streamed) {
                    /* The gate must also unmount: a refused image left streaming would keep
                       serving the LBA path bytes the loader has just declined. */
                    gdx_disk_stream_close();
                    s_diskStreamMode = 0;
                }
                gdx_port_logf("[leo] REJECTED %s: this is the retail JAPANESE Expansion Kit disk, "
                              "and Japanese-region support is not enabled in this build (configure "
                              "with -DGDX_ALLOW_JP_INPUTS=ON to enable it)\n", path);
                free(buf);
                continue;
            }
            gdx_disk_buffer = buf;
            gdx_disk_size = static_cast<unsigned int>(sz);
            gdx_port_logf("[leo] disk image loaded: %s (%ld bytes) -- picked from %s, region=%s\n", path, sz,
                          loc.why, jpRom ? "JP (matches cartridge ROM)" : "US/unknown (default preference)");
            // No archive reconstruction this boot, so the deletion gate stays unproven.
            gdx_disk_finalize(diskNames[i], loc.managed ? "managed" : "original", pristineCrc64);
            return 1;
        }
    }

    // B11e -- the build-time asset bake. No image was found, but if this build was configured
    // with one (tools/gen_ek_assets.py --bake, OFF by default, gEkAssetPackSize == 0 otherwise)
    // every EK asset symbol can still be populated: the pack is the same 655,956 bytes the fill
    // table reads, sliced at configure time, so there is no resident image, no whole-image
    // CRC-64 pass and no LEO read.
    //
    // WHAT THIS DELIBERATELY DOES NOT DO, so nobody reads more into it than is there:
    //   * it does NOT set gLeoDriveConnectionState. There is still no drive, so
    //     main_menu.c:203's EK arm still bounces the cursor off COURSE EDIT and CREATE MACHINE
    //     and the two editors are still unreachable. This populates textures, not a drive.
    //     What DOES give the game a drive is a mount -- including the streamed mode-2 mount
    //     above, which needs a real .ndd on the card and costs no resident image (B11d).
    //   * it does NOT provide the MFS RAM area the editors read and write, nor any MEDIUM_LBA
    //     audio (every EK sequence and DD sample bank is on the disk -- see ekdeadlock).
    //   * it does NOT return 1. The caller's contract is "is an image mounted", and none is.
    static bool sPackApplied = false;
    if (!sPackApplied && gEkAssetPackSize != 0) {
        sPackApplied = true;
        gdx_ek_assets_fill(nullptr, 0);
        // Same layout-variant decision gdx_disk_finalize makes, from the CRC-64 the generator
        // recorded for the image it baked -- so a baked build reports which disk it carries
        // rather than assuming, and the decomp-side blit sites reading
        // gdx_ek_disk_is_translated() pick the same label geometry they would with that disk
        // mounted.
        s_ekDiskTranslated = (gEkAssetPackDiskCrc64 == GDX_EK_TRANSLATED_DISK_CRC64) ? 1 : 0;
        // B11m: and the same English text pack, for the same reason and in the same place --
        // after the fill, before the byte-swap below, which the pack's own copy of
        // D_80769DF0 would otherwise be written underneath in the wrong order.
        if (gEkTextPackRowCount != 0 && !s_ekDiskTranslated && gdx_disk_lever("ek_english", 1)) {
            if (gdx_ek_text_pack_apply() > 0) {
                s_ekDiskTranslated = 1;
            }
        }
        // Same decoded-TEXTURE fixup gdx_disk_finalize applies for the same reason: the fill
        // copied raw BIG-ENDIAN bytes, and func_806F33D0 (sys_main.c) CPU-blits this texture
        // into a host-order VI framebuffer. Not a disk normalization -- the pack, like the disk
        // buffer, is never byte-swapped.
        for (unsigned int i = 0; i < 5304; i++) {
            const unsigned short v = D_80769DF0[i];
            D_80769DF0[i] = static_cast<unsigned short>((v >> 8) | (v << 8));
        }
        gdx_port_logf("[ek-bake] %u bytes of baked EK assets applied with no disk image "
                      "(source CRC64=0x%016llX, variant: %s)\n",
                      gEkAssetPackSize, gEkAssetPackDiskCrc64,
                      s_ekDiskTranslated ? "translated"
                          : (gEkAssetPackDiskCrc64 == GDX_EK_JP_DISK_CRC64 ? "JP retail" : "unknown"));
    }

    static bool sWarned = false;
    if (!sWarned) {
        sWarned = true;
        gdx_port_logf("[leo] no disk image found; Expansion Kit modes stay disabled%s\n",
                      gEkAssetPackSize != 0 ? " (EK assets served from the build-time bake)" : "");
    }
    return 0;
}
}
