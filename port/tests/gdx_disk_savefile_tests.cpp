/* port/tests/gdx_disk_savefile_tests.cpp -- the 64DD disk-save sidecar (port/disk_savefile.cpp)
 * on the exit path.
 *
 * Card-5 Old 3DS: HOME-close ran gdx_disk_save_flush with the heap at its lowest; the flush
 * assembled the whole sidecar image in one std::vector (73 KB), the allocation failed with
 * 384 KB free in fragments, std::bad_alloc escaped, std::terminate, and the console hard-locked.
 * The flush now streams the file straight out of the journal. This test pins:
 *
 *   1. the streamed file is byte-identical to the image the old flush built (same format, so
 *      every sidecar already on an SD card still loads, and the loader still accepts the new one);
 *   2. the flush performs ZERO C++ heap allocations -- operator new is armed to throw during it;
 *   3. a flush that cannot write leaves the previous .gdd byte-identical (no corruption) and
 *      reports failure instead of throwing;
 *   4. out of memory while journaling a write does not throw out of mark_dirty, and the session
 *      then refuses to flush a journal that is missing a write (it would persist an MFS state the
 *      game never had); the previous .gdd stays as it was.
 *
 * Built and run by tools/ci-3ds.sh (gdx_disk_savefile_tests) with port/disk_savefile.cpp,
 * port/gdx_save_replace.c and port/gdx_dev_gates.c; port/tests/stubs stands in for the LUS CVar
 * bridge. No disk image needed: the "disk" is a synthetic buffer.
 */
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "disk_savefile.h"

/* --- link stubs (port_log.h's gates come from port/gdx_dev_gates.c) ------------- */

extern "C" int32_t CVarGetInteger(const char* name, int32_t defaultValue) {
    (void)name;
    return defaultValue;
}

extern "C" void CVarSetInteger(const char* name, int32_t value) {
    (void)name;
    (void)value;
}

extern "C" void CVarSave(void) {
}

/* --- operator new instrumentation --------------------------------------------- */

static unsigned long g_newCount = 0;
static bool g_newThrows = false;

void* operator new(std::size_t size) {
    g_newCount++;
    if (g_newThrows) {
        throw std::bad_alloc();
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

/* --- checks ------------------------------------------------------------------- */

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
    g_checks++;
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        g_failures++;
    }
}

static bool readFile(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    out.clear();
    unsigned char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    fclose(f);
    return true;
}

static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

/* --- the reference: the pre-fix flush's in-memory image, rebuilt independently -------- */

struct RefRange {
    uint32_t offset;
    std::vector<unsigned char> data;
};

static uint32_t refCrc32(const unsigned char* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void refPut32(std::vector<unsigned char>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) {
        v.push_back((unsigned char)((x >> (8 * i)) & 0xFFu));
    }
}

/* The journal the sidecar must hold: the pristine image vs the written image, as maximal runs of
 * written bytes where adjacent/overlapping writes merge. The test writes non-touching ranges plus
 * explicit overlaps, so the expected records are given by hand below. */
static std::vector<unsigned char> refImage(uint64_t sourceHash, const std::vector<RefRange>& ranges) {
    std::vector<unsigned char> image = { 'G', 'D', 'D', '1' };
    refPut32(image, 1u);
    refPut32(image, (uint32_t)(sourceHash & 0xFFFFFFFFu));
    refPut32(image, (uint32_t)(sourceHash >> 32));
    refPut32(image, (uint32_t)ranges.size());
    refPut32(image, 0u);
    for (const RefRange& r : ranges) {
        refPut32(image, r.offset);
        refPut32(image, (uint32_t)r.data.size());
        image.insert(image.end(), r.data.begin(), r.data.end());
    }
    uint32_t crc = refCrc32(image.data(), image.size());
    image[20] = (unsigned char)(crc & 0xFFu);
    image[21] = (unsigned char)((crc >> 8) & 0xFFu);
    image[22] = (unsigned char)((crc >> 16) & 0xFFu);
    image[23] = (unsigned char)((crc >> 24) & 0xFFu);
    return image;
}

/* --- fixture ------------------------------------------------------------------ */

static const unsigned kDiskSize = 512u * 1024u;
static const char* kDiskName = "test.ndd";
static const std::string kGdd = "./saves/test.ndd.gdd";
static const std::string kTmp = "./saves/test.ndd.gdd.tmp";

static std::vector<unsigned char> g_pristine;
static std::vector<unsigned char> g_written; /* what the game wrote over the pristine disk */

static void makePristine() {
    g_pristine.resize(kDiskSize);
    uint32_t x = 0x1234567u;
    for (unsigned i = 0; i < kDiskSize; i++) {
        x = x * 1103515245u + 12345u;
        g_pristine[i] = (unsigned char)(x >> 16);
    }
    g_written = g_pristine;
}

static void write(unsigned offset, unsigned len, unsigned char seed) {
    std::vector<unsigned char> bytes(len);
    for (unsigned i = 0; i < len; i++) {
        bytes[i] = (unsigned char)(seed + i * 7u);
    }
    memcpy(&g_written[offset], bytes.data(), len);
    gdx_disk_save_mark_dirty(offset, bytes.data(), len);
}

static RefRange slice(unsigned offset, unsigned len) {
    RefRange r;
    r.offset = offset;
    r.data.assign(g_written.begin() + offset, g_written.begin() + offset + len);
    return r;
}

/* --- tests -------------------------------------------------------------------- */

static void testStreamedFlushMatchesImageAndAllocatesNothing() {
    gdx_disk_save_init(kDiskName, g_pristine.data(), kDiskSize);
    write(4096, 512, 0x11);
    write(4600, 100, 0x22);          /* overlaps the first: one record 4096..4700 */
    write(4700, 50, 0x33);           /* adjacent: merges, record 4096..4750 */
    write(65536, 73u * 1024u, 0x44); /* the 73 KB record of the card-5 failure */
    write(300000, 3, 0x55);
    write(1000, 24, 0x66);           /* lands BEFORE the others: kept sorted */

    std::vector<RefRange> want;
    want.push_back(slice(1000, 24));
    want.push_back(slice(4096, 4750 - 4096));
    want.push_back(slice(65536, 73u * 1024u));
    want.push_back(slice(300000, 3));
    const uint64_t hash = gdx_disk_crc64(g_pristine.data(), kDiskSize);
    const std::vector<unsigned char> image = refImage(hash, want);

    g_newCount = 0;
    g_newThrows = true;
    gdx_disk_save_flush();
    g_newThrows = false;
    char line[128];
    snprintf(line, sizeof(line), "flush: zero operator-new calls (got %lu)", g_newCount);
    check(g_newCount == 0, line);
    check(gdx_disk_last_flush_ok() == 1, "flush: reports success");

    std::vector<unsigned char> got;
    check(readFile(kGdd, got), "flush: .gdd written");
    snprintf(line, sizeof(line), "flush: .gdd byte-identical to the pre-fix image (%zu vs %zu bytes)", got.size(),
             image.size());
    check(got == image, line);
    check(!fileExists(kTmp), "flush: no .tmp left behind");

    /* the loader takes it back: a fresh boot applies the journal over the pristine disk */
    gdx_disk_save_init(kDiskName, g_pristine.data(), kDiskSize);
    check(gdx_disk_sidecar_record_count() == 4, "reload: 4 records");
    std::vector<unsigned char> disk = g_pristine;
    gdx_disk_save_apply(disk.data());
    check(disk == g_written, "reload: applied disk equals what the game wrote");
}

static void testFailedFlushKeepsPreviousSidecar() {
    std::vector<unsigned char> before;
    readFile(kGdd, before);

    gdx_disk_save_init(kDiskName, g_pristine.data(), kDiskSize);
    write(200000, 4000, 0x77);
    /* The .tmp cannot be created: a directory sits at its path. */
    check(mkdir(kTmp.c_str(), 0755) == 0, "fail: blocked .tmp path");
    gdx_disk_save_flush();
    rmdir(kTmp.c_str());
    check(gdx_disk_last_flush_ok() == 0, "fail: flush reports failure");
    std::vector<unsigned char> after;
    readFile(kGdd, after);
    check(!before.empty() && after == before, "fail: previous .gdd byte-identical");

    /* the next flush that CAN write persists the whole journal */
    gdx_disk_save_flush();
    check(gdx_disk_last_flush_ok() == 1, "fail: the retry succeeds");
    gdx_disk_save_init(kDiskName, g_pristine.data(), kDiskSize);
    std::vector<unsigned char> disk = g_pristine;
    gdx_disk_save_apply(disk.data());
    check(disk == g_written, "fail: retry persisted every write");
}

static void testOutOfMemoryJournalNeverFlushesAPartialState() {
    std::vector<unsigned char> before;
    readFile(kGdd, before);

    gdx_disk_save_init(kDiskName, g_pristine.data(), kDiskSize);
    write(400000, 64, 0x88);
    std::vector<unsigned char> bytes(2048, 0x99);
    bool threw = false;
    g_newThrows = true;
    try {
        gdx_disk_save_mark_dirty(410000, bytes.data(), (unsigned)bytes.size());
    } catch (...) {
        threw = true;
    }
    g_newThrows = false;
    check(!threw, "oom: mark_dirty does not throw");

    gdx_disk_save_flush();
    check(gdx_disk_last_flush_ok() == 0, "oom: flush refused (journal is missing a write)");
    std::vector<unsigned char> after;
    readFile(kGdd, after);
    check(!before.empty() && after == before, "oom: previous .gdd byte-identical");

    /* a new boot starts a healthy journal again */
    gdx_disk_save_init(kDiskName, g_pristine.data(), kDiskSize);
    write(420000, 16, 0xAA);
    gdx_disk_save_flush();
    check(gdx_disk_last_flush_ok() == 1, "oom: next session flushes again");
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    std::string work = std::string(dir) + "/disk_savefile_work";
    mkdir(work.c_str(), 0755);
    if (chdir(work.c_str()) != 0) {
        printf("FAIL cannot enter %s (errno=%d)\n", work.c_str(), errno);
        return 1;
    }
    remove(kGdd.c_str());
    remove((kGdd + ".bak").c_str());
    remove(kTmp.c_str());
    rmdir(kTmp.c_str());

    makePristine();
    testStreamedFlushMatchesImageAndAllocatesNothing();
    testFailedFlushKeepsPreviousSidecar();
    testOutOfMemoryJournalNeverFlushesAPartialState();

    if (g_failures != 0) {
        printf("%d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
