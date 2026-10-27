/* port/disk_savefile.cpp -- durable 64DD disk-save sidecar implementation.
 *
 * See disk_savefile.h for the format, validation rules, and boundary rationale.
 *
 * Sidecar file layout (all multi-byte fields little-endian, host-independent):
 *   Header (24 bytes):
 *     0   4   magic "GDD1"
 *     4   4   format version (currently 1)
 *     8   8   sourceDiskHash  -- CRC64 of the pristine .ndd bytes
 *     16  4   recordCount
 *     20  4   fileCrc32       -- CRC32 over every byte except these 4
 *   Records (recordCount of them, packed back to back):
 *     0   4   byteOffset
 *     4   4   length
 *     8   len data
 *
 * Records are the coalesced dirty byte ranges: overlapping and adjacent ranges
 * are merged so one game save (a bounded write burst from Mfs_SaveFile /
 * Mfs_BackupRamArea) collapses into the smallest set of non-overlapping records.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fwrite below; harmless on non-MSVC */

#include "disk_savefile.h"
#include "port_log.h"
#include "gdx_save_replace.h"
#ifdef EXPANSION_KIT
#include "gdx_disk_stream.h"   /* B11c: the streamed-read overlay reports its own byte count */
#ifdef GDX_PLATFORM_3DS
/* Namespace scope on purpose: a linkage specification is ill-formed at block scope. */
extern "C" const char* gdx3ds_fs_base_path(void);
#endif
#endif
#include "libultraship/bridge/consolevariablebridge.h" /* one-shot DD-format CVar (Workshop menu) */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <cerrno>
#endif

namespace {

constexpr unsigned kHeaderSize = 24;
constexpr uint32_t kFormatVersion = 1u;
/* Idle host frames (about half a second at 60 Hz) before the journal is flushed. A user
 * save is a bounded write burst, so waiting it out coalesces it into one sidecar write. */
constexpr int kDebounceFrames = 30;

struct DirtyRange {
    uint32_t offset;
    std::vector<uint8_t> data;
};

/* One coalesced journal, keyed to the pristine disk it was built against. */
uint64_t g_sourceHash = 0;
unsigned g_diskSize = 0;
bool g_active = false;
std::vector<DirtyRange> g_ranges; /* sorted by offset, non-overlapping */

bool g_pendingFlush = false;
/* A disk write the journal could not record (out of memory): flushing what it does hold would
 * persist an MFS state the game never had, so the session stops flushing instead. */
bool g_journalBroken = false;
int g_idleFrames = 0;

std::string g_gddPath;
std::string g_bakPath;
std::string g_tmpPath;

/* Workshop-menu status (read-only introspection; see the getters at the bottom). */
bool g_sidecarPresent = false;      /* a valid .gdd/.bak was loaded this boot */
bool g_lastFlushOk = true;
bool g_formatRefusedThisBoot = false; /* MFS uninitialized AND format refused this boot */

/* --- CRC helpers ------------------------------------------------------------ */

/* CRC-32 (zlib polynomial 0xEDB88320), reflected, table-based. Same algorithm
 * as port/gdx_ghost_io.c's container CRC. */
/* Running form: start from kCrc32Init, feed every span in order, finish with ^ kCrc32Init. The
 * streaming flush CRCs the file it writes without ever holding it whole in memory. */
constexpr uint32_t kCrc32Init = 0xFFFFFFFFu;

uint32_t crc32Update(uint32_t crc, const unsigned char* data, size_t length) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (unsigned n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < length; i++) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t crc32(const unsigned char* data, size_t length) {
    return crc32Update(kCrc32Init, data, length) ^ kCrc32Init;
}

/* CRC-64/XZ (ECMA-182 polynomial, reflected form 0xC96C5795D7870F42), used as a
 * cheap collision-resistant fingerprint of the pristine disk image. */
uint64_t crc64(const unsigned char* data, size_t length) {
    static uint64_t table[256];
    static bool ready = false;
    if (!ready) {
        for (unsigned n = 0; n < 256; n++) {
            uint64_t c = n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xC96C5795D7870F42ull ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        ready = true;
    }
    uint64_t crc = 0xFFFFFFFFFFFFFFFFull;
    for (size_t i = 0; i < length; i++) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFFFFFFFFFull;
}

/* --- little-endian pack/unpack ---------------------------------------------- */

uint32_t getU32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t getU64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* --- path resolution (mirrors gdx_ghost_io.c) ------------------------------- */

bool executableDirectory(std::string& out) {
#if defined(EXPANSION_KIT) && defined(GDX_PLATFORM_3DS)
    /* B11c, the same bug gdx_exe_dir had (see port/disk_buffer.cpp): a 3dsx has no executable
     * path, so the "." below put the .gdd sidecar under the launch CWD -- which on a netloader
     * push is not the data directory, and which would then disagree with the .dcrc sidecar
     * disk_buffer.cpp writes beside it. Both now resolve to sdmc:/3ds/gdiffuser/saves/.
     *
     * The EXPANSION_KIT half of the guard is there so this TU, which IS in the shipped 3DS
     * build, preprocesses byte-identically without it. Nothing outside the 64DD disk-save path
     * calls this function, and that path exists only in an EK build, so the guard costs no
     * correctness -- but if a non-EK caller is ever added, it wants this arm too. */
    const char* base = gdx3ds_fs_base_path();
    if (base == nullptr || base[0] == '\0') {
        return false;
    }
    out = base;
    while (!out.empty() && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();   /* savesDirectory() appends its own separator */
    }
    return true;
#elif defined(_WIN32)
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, (DWORD)sizeof(path));
    if (n == 0 || n >= sizeof(path)) {
        return false;
    }
    char* slash = strrchr(path, '\\');
    if (slash == nullptr) {
        return false;
    }
    *slash = '\0';
    out = path;
    return true;
#else
    out = ".";
    return true;
#endif
}

bool savesDirectory(std::string& out) {
    std::string base;
    if (!executableDirectory(base)) {
        return false;
    }
#ifdef _WIN32
    out = base + "\\saves";
    if (!CreateDirectoryA(out.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
#else
    out = base + "/saves";
    if (mkdir(out.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
#endif
    return true;
}

std::string baseName(const char* name) {
    if (name == nullptr) {
        return std::string();
    }
    std::string s(name);
    size_t pos = s.find_last_of("/\\");
    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

/* --- journal coalescing ----------------------------------------------------- */

void insertRange(uint32_t offset, const unsigned char* data, uint32_t len) {
    if (len == 0) {
        return;
    }
    uint64_t newStart = offset;
    uint64_t newEnd = (uint64_t)offset + len;

    /* Ranges are kept sorted by offset, so everything overlapping or touching the new
     * range is one contiguous run and can be collapsed in a single pass. */
    size_t first = g_ranges.size();
    size_t last = 0;
    bool any = false;
    for (size_t i = 0; i < g_ranges.size(); i++) {
        uint64_t rs = g_ranges[i].offset;
        uint64_t re = rs + g_ranges[i].data.size();
        if (re >= newStart && rs <= newEnd) { /* overlapping or adjacent */
            if (!any) {
                first = i;
                any = true;
            }
            last = i;
            if (rs < newStart) {
                newStart = rs;
            }
            if (re > newEnd) {
                newEnd = re;
            }
        }
    }

    DirtyRange merged;
    merged.offset = (uint32_t)newStart;
    merged.data.assign((size_t)(newEnd - newStart), 0);

    if (any) {
        for (size_t i = first; i <= last; i++) {
            const DirtyRange& r = g_ranges[i];
            memcpy(&merged.data[r.offset - newStart], r.data.data(), r.data.size());
        }
    }
    /* Overlay the freshly written bytes last so they win over stale content. */
    memcpy(&merged.data[offset - newStart], data, len);

    /* Exception safety: the only allocations left are above (merged) and this reserve, both
     * before g_ranges changes, so an out-of-memory throw leaves the journal exactly as it was. */
    g_ranges.reserve(g_ranges.size() + 1);
    if (any) {
        g_ranges.erase(g_ranges.begin() + first, g_ranges.begin() + last + 1);
        g_ranges.insert(g_ranges.begin() + first, std::move(merged));
    } else {
        size_t pos = 0;
        while (pos < g_ranges.size() && g_ranges[pos].offset < merged.offset) {
            pos++;
        }
        g_ranges.insert(g_ranges.begin() + pos, std::move(merged));
    }
}

/* --- sidecar replace (n60save) ------------------------------------------------ */

#ifndef _WIN32
int sidecarRename(void* ctx, const char* from, const char* to) {
    (void)ctx;
    return rename(from, to);
}

int sidecarRemove(void* ctx, const char* path) {
    (void)ctx;
    return remove(path);
}

int sidecarExists(void* ctx, const char* path) {
    (void)ctx;
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

/* .tmp -> .gdd keeping the previous .gdd as the rolling .bak. Validity is the loader's CRC. */
bool replaceSidecarFromTmp() {
    GdxSaveFsOps ops;
    ops.rename_file = sidecarRename;
    ops.remove_file = sidecarRemove;
    ops.file_exists = sidecarExists;
    ops.file_valid = nullptr;
    ops.ctx = nullptr;
    return gdx_save_replace(&ops, g_tmpPath.c_str(), g_gddPath.c_str(), g_bakPath.c_str(), 1, 1, nullptr) ==
           GDX_SAVE_REPLACE_OK;
}
#endif

/* --- sidecar load ----------------------------------------------------------- */

enum class LoadStatus { Loaded, Missing, BadMagic, BadVersion, HashMismatch, BadCrc, BadRange, Io };

LoadStatus loadSidecar(const std::string& path, std::vector<DirtyRange>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return LoadStatus::Missing;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < (long)kHeaderSize) {
        fclose(f);
        return LoadStatus::Io;
    }
    std::vector<uint8_t> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        return LoadStatus::Io;
    }
    fclose(f);

    if (memcmp(buf.data(), "GDD1", 4) != 0) {
        return LoadStatus::BadMagic;
    }
    if (getU32(&buf[4]) != kFormatVersion) {
        return LoadStatus::BadVersion;
    }
    uint64_t storedHash = getU64(&buf[8]);
    uint32_t recordCount = getU32(&buf[16]);
    uint32_t storedCrc = getU32(&buf[20]);

    /* The CRC covers every byte except the 4 CRC bytes at offset 20, so recompute over a
     * scratch copy with that field zeroed -- the same shape flush builds. */
    std::vector<uint8_t> scratch(buf);
    scratch[20] = scratch[21] = scratch[22] = scratch[23] = 0;
    if (crc32(scratch.data(), scratch.size()) != storedCrc) {
        return LoadStatus::BadCrc;
    }
    if (storedHash != g_sourceHash) {
        return LoadStatus::HashMismatch;
    }

    std::vector<DirtyRange> parsed;
    size_t pos = kHeaderSize;
    for (uint32_t i = 0; i < recordCount; i++) {
        if (pos + 8 > buf.size()) {
            return LoadStatus::Io;
        }
        uint32_t off = getU32(&buf[pos]);
        uint32_t len = getU32(&buf[pos + 4]);
        pos += 8;
        if (len == 0 || pos + len > buf.size()) {
            return LoadStatus::Io;
        }
        if ((uint64_t)off + len > g_diskSize) {
            return LoadStatus::BadRange;
        }
        DirtyRange r;
        r.offset = off;
        r.data.assign(buf.begin() + pos, buf.begin() + pos + len);
        parsed.push_back(std::move(r));
        pos += len;
    }
    out.swap(parsed);
    return LoadStatus::Loaded;
}

const char* statusText(LoadStatus s) {
    switch (s) {
    case LoadStatus::BadMagic: return "bad magic";
    case LoadStatus::BadVersion: return "unsupported version";
    case LoadStatus::HashMismatch: return "disk fingerprint mismatch";
    case LoadStatus::BadCrc: return "CRC mismatch";
    case LoadStatus::BadRange: return "record out of disk range";
    case LoadStatus::Io: return "truncated or unreadable";
    default: return "unknown";
    }
}

/* The sidecar is written as a stream: header (CRC filled in up front), then each record's
 * offset/length/bytes straight out of g_ranges. Nothing here allocates on the heap -- the exit
 * chain runs this with the heap at its lowest (card-5 Old 3DS: a 73 KB image assembly failed
 * with 384 KB free in fragments, std::terminate, console hard-locked on HOME close). */
/* fopen's stdio buffer, static so the flush never mallocs one; 8 KB keeps the header and the
 * small records to a few FS writes and lets a record larger than it go out in one call. */
unsigned char g_flushStreamBuf[8 * 1024];

void putLe32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

void putLe64(unsigned char* p, uint64_t v) {
    putLe32(p, (uint32_t)(v & 0xFFFFFFFFu));
    putLe32(p + 4, (uint32_t)(v >> 32));
}

/* The 24-byte header with its CRC field zeroed (offset 20), as the loader recomputes it. */
void buildHeader(unsigned char header[kHeaderSize]) {
    memcpy(header, "GDD1", 4);
    putLe32(header + 4, kFormatVersion);
    putLe64(header + 8, g_sourceHash);
    putLe32(header + 16, (uint32_t)g_ranges.size());
    putLe32(header + 20, 0);
}

void buildRecordHeader(unsigned char out[8], const DirtyRange& r) {
    putLe32(out, r.offset);
    putLe32(out + 4, (uint32_t)r.data.size());
}

/* CRC-32 of the whole file image (CRC field zeroed), computed over the journal in place. */
uint32_t sidecarCrc(const unsigned char header[kHeaderSize]) {
    uint32_t crc = crc32Update(kCrc32Init, header, kHeaderSize);
    for (const DirtyRange& r : g_ranges) {
        unsigned char rec[8];
        buildRecordHeader(rec, r);
        crc = crc32Update(crc, rec, sizeof(rec));
        crc = crc32Update(crc, r.data.data(), r.data.size());
    }
    return crc ^ kCrc32Init;
}

bool writeSidecarStream(FILE* f, const unsigned char header[kHeaderSize]) {
    if (fwrite(header, 1, kHeaderSize, f) != kHeaderSize) {
        return false;
    }
    for (const DirtyRange& r : g_ranges) {
        unsigned char rec[8];
        buildRecordHeader(rec, r);
        if (fwrite(rec, 1, sizeof(rec), f) != sizeof(rec)) {
            return false;
        }
        if (!r.data.empty() && fwrite(r.data.data(), 1, r.data.size(), f) != r.data.size()) {
            return false;
        }
    }
    return true;
}

/* .tmp written completely and closed, or removed. Returns true only for a complete file. */
bool writeSidecarTmp() {
    unsigned char header[kHeaderSize];
    buildHeader(header);
    putLe32(header + 20, sidecarCrc(header));

    FILE* f = fopen(g_tmpPath.c_str(), "wb");
    if (f == nullptr) {
        gdx_port_logf("[disk-save] WARNING: could not open %s for writing; save not persisted\n", g_tmpPath.c_str());
        return false;
    }
    setvbuf(f, reinterpret_cast<char*>(g_flushStreamBuf), _IOFBF, sizeof(g_flushStreamBuf));
    bool ok = writeSidecarStream(f, header);
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        gdx_port_logf("[disk-save] WARNING: failed writing %s; save not persisted\n", g_tmpPath.c_str());
        remove(g_tmpPath.c_str());
    }
    return ok;
}

} // namespace

extern "C" {

unsigned long long gdx_disk_crc64(const unsigned char* data, unsigned long long length) {
    return crc64(data, (size_t)length);
}

#ifdef EXPANSION_KIT
/* B11c. The CRC policy (see the long comment in port/disk_buffer.cpp): this sidecar's source
 * fingerprint and the loader's identity fingerprint are THE SAME CRC-64 OVER THE SAME BYTES,
 * and gdx_disk_finalize's own comment already says so -- it accepted the duplicate because a
 * second 62 MB pass is "a few milliseconds" on a desktop. Streaming makes the second pass
 * 61.9 MB of SD reads, which is the cost streaming exists to remove. So the loader hands its
 * value over instead, and the invariant it rests on is the one gdx_disk_finalize already
 * asserts in prose: nothing between the two points mutates the pristine buffer.
 *
 * ONE-SHOT on purpose. The preset is consumed by the next gdx_disk_save_init and cleared, so
 * a second mount with no preset falls back to computing its own rather than silently
 * inheriting the first disk's fingerprint -- which would key one disk's saves to another.
 *
 * Whole block is #ifdef EXPANSION_KIT: this TU is in the SHIPPED 3DS build, and the shipped
 * build must preprocess byte-identically. */
static uint64_t g_presetSourceHash = 0;
static bool g_havePresetSourceHash = false;

void gdx_disk_save_set_source_hash(unsigned long long crc) {
    g_presetSourceHash = (uint64_t) crc;
    g_havePresetSourceHash = true;
}

/* Replays the journal's dirty ranges over freshly read base bytes. This is what makes a
 * STREAMED read equal to what the resident image would have returned after
 * gdx_disk_save_apply(): the user's .ndd is never written, so the only place a 64DD write
 * exists is here. Same clipping rules as gdx_disk_save_apply, plus the read window. */
void gdx_disk_save_overlay(unsigned int offset, unsigned int length, void* dst) {
    if (!g_active || dst == nullptr || length == 0 || g_ranges.empty()) {
        return;
    }
    const uint64_t rs = offset;
    const uint64_t re = (uint64_t) offset + length;
    unsigned int rewritten = 0;
    for (const DirtyRange& r : g_ranges) {
        const uint64_t ds = r.offset;
        const uint64_t de = ds + r.data.size();
        if (de <= rs) {
            continue;
        }
        if (ds >= re) {
            break; /* g_ranges is sorted by offset, so nothing later can overlap */
        }
        const uint64_t s = (ds > rs) ? ds : rs;
        const uint64_t e = (de < re) ? de : re;
        memcpy((unsigned char*) dst + (s - rs), r.data.data() + (s - ds), (size_t) (e - s));
        rewritten += (unsigned int) (e - s);
    }
    if (rewritten != 0) {
        gdx_disk_stream_note_overlay(rewritten);
    }
}
#endif /* EXPANSION_KIT */

void gdx_disk_save_init(const char* diskName, const unsigned char* pristine, unsigned int size) {
    g_active = false;
    g_ranges.clear();
    g_pendingFlush = false;
    g_journalBroken = false;
    g_idleFrames = 0;
    g_sourceHash = 0;
    g_diskSize = size;
    g_sidecarPresent = false;
    /* Reset per boot/disk so a stale FAILED from a prior disk cannot leak into this one.
     * The getter is binary, so "no flush attempted yet" and "last flush succeeded" both
     * read as ok in the Workshop panel. */
    g_lastFlushOk = true;

#ifdef EXPANSION_KIT
    /* B11d -- disk_stream=2 mounts the image with NO resident buffer, so `pristine` is null
     * and yet a disk is mounted. The fingerprint that keys this sidecar is then the one
     * gdx_disk_finalize preset from the streamed CRC-64 over the same bytes, so the journal
     * stays keyed to the right disk. Without a preset there is nothing to fingerprint FROM,
     * and keying a journal to the wrong disk is a data-loss hazard -- so that case still
     * disables the sidecar, exactly as a missing image always did. */
    const bool streamedMount = (pristine == nullptr && size != 0 && g_havePresetSourceHash);
    if (!streamedMount)
#endif
    if (pristine == nullptr || size == 0) {
        gdx_port_logf("[disk-save] no disk image; durable disk save disabled\n");
        return;
    }

#ifdef EXPANSION_KIT
    if (g_havePresetSourceHash) {
        g_sourceHash = g_presetSourceHash;
        g_havePresetSourceHash = false;
    } else
#endif
    g_sourceHash = crc64(pristine, size);

    std::string dir;
    if (!savesDirectory(dir)) {
        gdx_port_logf("[disk-save] WARNING: could not resolve saves directory; disk writes will not persist\n");
        return;
    }
    std::string leaf = baseName(diskName);
    if (leaf.empty()) {
        leaf = "disk";
    }
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    g_gddPath = dir + sep + leaf + ".gdd";
    g_bakPath = g_gddPath + ".bak";
    g_tmpPath = g_gddPath + ".tmp";
    g_active = true;

    /* Primary sidecar, else the rolled backup, else pristine-only. A sidecar whose
     * fingerprint does not match this pristine image is NEVER applied. */
    LoadStatus primary = loadSidecar(g_gddPath, g_ranges);
    if (primary == LoadStatus::Loaded) {
        g_sidecarPresent = true;
        gdx_port_logf("[disk-save] loaded sidecar %s: %zu record(s), disk fingerprint OK\n", g_gddPath.c_str(),
                      g_ranges.size());
        return;
    }
    if (primary != LoadStatus::Missing) {
        gdx_port_logf("[disk-save] sidecar %s rejected (%s); trying backup\n", g_gddPath.c_str(),
                      statusText(primary));
    }

    /* N60SAVE: a power cut between rolling .gdd to .bak and promoting .tmp leaves the newest
     * complete copy in .tmp; the CRC and fingerprint checks make it as safe to load as .gdd. */
    LoadStatus staged = loadSidecar(g_tmpPath, g_ranges);
    if (staged == LoadStatus::Loaded) {
        g_sidecarPresent = true;
        gdx_port_logf("[disk-save] recovered from staged %s: %zu record(s), disk fingerprint OK\n", g_tmpPath.c_str(),
                      g_ranges.size());
        return;
    }
    g_ranges.clear();

    LoadStatus backup = loadSidecar(g_bakPath, g_ranges);
    if (backup == LoadStatus::Loaded) {
        g_sidecarPresent = true;
        gdx_port_logf("[disk-save] recovered from backup %s: %zu record(s), disk fingerprint OK\n", g_bakPath.c_str(),
                      g_ranges.size());
        return;
    }
    g_ranges.clear();
    if (primary == LoadStatus::Missing && backup == LoadStatus::Missing) {
        gdx_port_logf("[disk-save] no disk save found, pristine (sidecar: %s)\n", g_gddPath.c_str());
        /* Fresh install: no sidecar exists at all, so there is nothing to protect. Arm the
         * one-shot DD-format gate for THIS boot so the MFS RAM area initializes without a
         * manual Workshop opt-in or a restart. Transient runtime state only -- do NOT
         * CVarSave() it; gdx_disk_allow_format() consumes and clears it when the format
         * runs. Deliberately NOT armed in the rejected-sidecar branch below: an existing
         * but unreadable save must never be auto-formatted. */
        CVarSetInteger("gEnhancements.Workshop.AllowDDFormatOnce", 1);
    } else {
        gdx_port_logf("[disk-save] backup %s unusable (%s); pristine-only\n", g_bakPath.c_str(),
                      statusText(backup == LoadStatus::Missing ? primary : backup));
    }
}

void gdx_disk_save_apply(unsigned char* buffer) {
    if (!g_active || buffer == nullptr || g_ranges.empty()) {
        return;
    }
    size_t applied = 0;
    for (const DirtyRange& r : g_ranges) {
        if ((uint64_t)r.offset + r.data.size() > g_diskSize) {
            continue; /* defensive: validated on load, but never write past the image */
        }
        memcpy(buffer + r.offset, r.data.data(), r.data.size());
        applied++;
    }
    gdx_port_logf("[disk-save] applied %zu record(s) to disk image\n", applied);
}

void gdx_disk_save_mark_dirty(unsigned int offset, const void* data, unsigned int len) {
    if (!g_active || data == nullptr || len == 0) {
        return;
    }
    if ((uint64_t)offset + len > g_diskSize) {
        gdx_port_logf("[disk-save] WARNING: dirty range out of disk bounds (offset=%u len=%u); ignored\n", offset,
                      len);
        return;
    }
    try {
        insertRange(offset, (const unsigned char*)data, len);
    } catch (...) {
        if (!g_journalBroken) {
            gdx_port_logf("[disk-save] WARNING: out of memory journaling a disk write (offset=%u len=%u); disk saves "
                          "stop for this session, the previous sidecar is kept\n",
                          offset, len);
        }
        g_journalBroken = true;
        return;
    }
    g_pendingFlush = true;
    g_idleFrames = 0;
}

static void gdx_disk_save_flush_unchecked(void) {
    if (g_journalBroken) {
        g_lastFlushOk = false;
        gdx_port_logf("[disk-save] WARNING: a disk write could not be journaled this session; flush skipped so "
                      "the previous sidecar stays consistent\n");
        return;
    }
    if (!writeSidecarTmp()) {
        g_lastFlushOk = false;
        return;
    }

    /* Roll the live sidecar to .bak first, so a failed replace still leaves a good copy
     * behind, then atomically move the temp into place. */
#ifdef _WIN32
    MoveFileExA(g_gddPath.c_str(), g_bakPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!MoveFileExA(g_tmpPath.c_str(), g_gddPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        g_lastFlushOk = false;
        gdx_port_logf("[disk-save] WARNING: could not replace %s; save not persisted (backup intact)\n",
                      g_gddPath.c_str());
        remove(g_tmpPath.c_str());
        return;
    }
#else
    /* N60SAVE: rename() never replaces an existing file on the 3DS SD archive, so the old
     * "rename .gdd -> .bak (best effort), rename .tmp -> .gdd" lost every flush from the third
     * on (.bak existed, so the roll failed, so the promote failed). gdx_save_replace removes the
     * stale .bak first and keeps a complete copy on disk after every step; the load below also
     * accepts a CRC-valid .tmp (a cut between the roll and the promote). */
    if (!replaceSidecarFromTmp()) {
        g_lastFlushOk = false;
        gdx_port_logf("[disk-save] WARNING: could not replace %s (errno=%d); save not persisted (backup intact)\n",
                      g_gddPath.c_str(), errno);
        return;
    }
#endif
    g_lastFlushOk = true;
    g_sidecarPresent = true;
    gdx_port_logf("[disk-save] flushed %zu record(s) to %s\n", g_ranges.size(), g_gddPath.c_str());
}

/* Never throws: the exit chain calls this, and an exception escaping it is std::terminate. A
 * failure of any kind leaves the previous .gdd/.bak untouched (the .tmp is only promoted once it
 * is complete) and is logged. */
void gdx_disk_save_flush(void) {
    if (!g_active) {
        return;
    }
    try {
        gdx_disk_save_flush_unchecked();
    } catch (...) {
        g_lastFlushOk = false;
        remove(g_tmpPath.c_str());
        gdx_port_logf("[disk-save] WARNING: flush failed (exception); save not persisted, previous sidecar intact\n");
    }
}

void gdx_disk_save_tick(void) {
    if (!g_active || !g_pendingFlush) {
        return;
    }
    if (++g_idleFrames < kDebounceFrames) {
        return;
    }
    g_pendingFlush = false;
    g_idleFrames = 0;
    gdx_disk_save_flush();
}

int gdx_disk_allow_format(void) {
    /* DEFAULT FALSE. With the durable sidecar in place, an unprompted format of an
     * uninitialized or foreign MFS RAM area would overwrite the user's prior saved disk
     * state. The Workshop menu's "Initialize DD save area" arms a one-shot flag that this
     * guard consumes exactly once, clearing and re-saving it so a single format runs and
     * the flag never fires again unprompted. The format lands in the in-memory image and,
     * through the dirty-range journal, in the sidecar -- never in the user's .ndd.
     *
     * TERMINAL-ONLY CONSUMPTION: call this only from genuinely terminal not-initialized
     * sites -- func_i1_80404830's second-pass failure (mfs_ram.c) and func_80706518's
     * media-init recovery (sys_leo_dd.c). The transient wiped-cache first pass inside
     * Mfs_ValidateRamVolume must not reach it, or the armed one-shot is consumed
     * spuriously and formats from a stale/empty cache. */
    if (CVarGetInteger("gEnhancements.Workshop.AllowDDFormatOnce", 0) != 0) {
        CVarSetInteger("gEnhancements.Workshop.AllowDDFormatOnce", 0);
        CVarSave();
        gdx_port_logf("[disk-save] one-shot DD format authorized by the Workshop menu; formatting once\n");
        return 1;
    }
    return 0;
}

void gdx_disk_log_format_refused(void) {
    /* Same terminal-only call sites as gdx_disk_allow_format, after that site observed
     * N64DD_MEDIA_NOT_INIT and the guard returned 0. Keeping the transient wiped-cache
     * first-pass validate out of here is what makes the latch truthful for the Workshop
     * button: a genuinely uninitialized disk, not a normal mount's first-pass miss. */
    g_formatRefusedThisBoot = true;
    static int logged = 0;
    if (logged) {
        return;
    }
    logged = 1;
    gdx_port_logf("[leo] MFS RAM area not initialized; refusing auto-format to protect the disk\n");
}

int gdx_disk_sidecar_present(void) {
    return g_sidecarPresent ? 1 : 0;
}

int gdx_disk_sidecar_record_count(void) {
    return static_cast<int>(g_ranges.size());
}

int gdx_disk_last_flush_ok(void) {
    return g_lastFlushOk ? 1 : 0;
}

int gdx_disk_format_refused_this_boot(void) {
    return g_formatRefusedThisBoot ? 1 : 0;
}

} // extern "C"
