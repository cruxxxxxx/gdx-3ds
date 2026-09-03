/* port/3ds/assets/test/gdx3ds_fs_host_smoke.c -- host-only functional smoke
 * test for gdx3ds_fs_sd.c and the gdx3ds_zipshim, exercising the exact
 * behaviors the device relies on:
 *
 *   1. mount order / duplicate-key resolution: fzerox.o2r (mounted last on
 *      desktop) must win a key that exists in both archives;
 *   2. case-SENSITIVE record lookup (libzip zip_name_locate parity);
 *   3. malloc'd read-asset semantics (payload bytes + size, miss => NULL+0);
 *   4. atomic-rename save write, including the rename-over-existing path;
 *   5. zipshim read surface (open/locate/stat/fopen/fread/close) against a
 *      real archive, plus the read-only write refusal.
 *
 * On the host, "sdmc:/..." resolves as a relative path under the CWD ("sdmc:"
 * is just a directory name to POSIX), so the test fabricates the SD layout in
 * a scratch directory before calling the contract. The test links its own
 * full-API miniz (writer enabled) to fabricate the fixture archives; the
 * production defines (MINIZ_NO_DEFLATE_APIS) stay untouched on gdx3ds_miniz.
 *
 * Run from an empty scratch CWD:  ./gdx3ds_fs_host_smoke
 * Exit 0 = all checks passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gdx3ds_fs.h"
#include "miniz.h"
#include "zip.h"

static int s_failures = 0;

#define CHECK(cond, what)                                        \
    do {                                                         \
        if (cond) {                                              \
            printf("  ok: %s\n", what);                          \
        } else {                                                 \
            printf("FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            s_failures++;                                        \
        }                                                        \
    } while (0)

static int writeFixtureArchive(const char* path, const char* keys[], const char* payloads[], int count) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_writer_init_file(&zip, path, 0)) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (!mz_zip_writer_add_mem(&zip, keys[i], payloads[i], strlen(payloads[i]), MZ_BEST_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return 0;
        }
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        return 0;
    }
    mz_zip_writer_end(&zip);
    return 1;
}

static void testReadAsset(void) {
    printf("read-asset semantics:\n");

    size_t size = 0;
    void* data = gdx3ds_fs_read_asset("seg/only_in_fzerox", &size);
    CHECK(data != NULL && size == 3 && memcmp(data, "FZX", 3) == 0, "fzerox-only record round-trips");
    free(data);

    data = gdx3ds_fs_read_asset("shaders/only_in_gdiffuser", &size);
    CHECK(data != NULL && size == 2 && memcmp(data, "GD", 2) == 0, "gdiffuser-only record round-trips");
    free(data);

    /* Desktop ArchiveManager is last-wins and fzerox.o2r mounts after
     * gdiffuser.o2r; the 3DS lookup must resolve the duplicate the same way. */
    data = gdx3ds_fs_read_asset("shared/duplicate_key", &size);
    CHECK(data != NULL && size == 6 && memcmp(data, "FZEROX", 6) == 0,
          "duplicate key resolves to fzerox.o2r (last-mounted wins)");
    free(data);

    data = gdx3ds_fs_read_asset("SEG/ONLY_IN_FZEROX", &size);
    CHECK(data == NULL && size == 0, "lookup is case-sensitive (miss returns NULL + size 0)");

    data = gdx3ds_fs_read_asset("no/such/record", &size);
    CHECK(data == NULL && size == 0, "missing record returns NULL + size 0");
}

static void testSaves(void) {
    printf("save persistence:\n");

    unsigned char blobA[64];
    unsigned char blobB[64];
    unsigned char readBack[64];
    memset(blobA, 0xA5, sizeof(blobA));
    memset(blobB, 0x5A, sizeof(blobB));

    CHECK(gdx3ds_fs_read_save("fzerox.sav", readBack, sizeof(readBack)) != 0, "missing save reads as a miss");
    CHECK(gdx3ds_fs_write_save("fzerox.sav", blobA, sizeof(blobA)) == 0, "first save write succeeds");
    memset(readBack, 0, sizeof(readBack));
    CHECK(gdx3ds_fs_read_save("fzerox.sav", readBack, sizeof(readBack)) == 0 &&
              memcmp(readBack, blobA, sizeof(blobA)) == 0,
          "save round-trips");
    CHECK(gdx3ds_fs_write_save("fzerox.sav", blobB, sizeof(blobB)) == 0, "overwrite (rename-over-existing) succeeds");
    memset(readBack, 0, sizeof(readBack));
    CHECK(gdx3ds_fs_read_save("fzerox.sav", readBack, sizeof(readBack)) == 0 &&
              memcmp(readBack, blobB, sizeof(blobB)) == 0,
          "overwritten save round-trips");

    FILE* tmp = fopen("sdmc:/3ds/gdiffuser/saves/fzerox.sav.tmp", "rb");
    CHECK(tmp == NULL, "no .tmp staging file left behind");
    if (tmp != NULL) {
        fclose(tmp);
    }

    CHECK(gdx3ds_fs_write_save("../escape.sav", blobA, sizeof(blobA)) != 0, "path traversal in save name rejected");
}

static void testZipshim(void) {
    printf("zipshim (libzip surface over miniz):\n");

    int zerr = 0;
    zip_t* archive = zip_open("sdmc:/3ds/gdiffuser/fzerox.o2r", ZIP_RDONLY, &zerr);
    CHECK(archive != NULL && zerr == 0, "zip_open on a real archive");
    if (archive == NULL) {
        return;
    }

    CHECK(zip_get_num_entries(archive, 0) == 2, "zip_get_num_entries");
    CHECK(zip_name_locate(archive, "SEG/ONLY_IN_FZEROX", 0) < 0, "zip_name_locate is case-sensitive");

    zip_int64_t index = zip_name_locate(archive, "seg/only_in_fzerox", 0);
    CHECK(index >= 0, "zip_name_locate hit");

    zip_stat_t st;
    zip_stat_init(&st);
    CHECK(zip_stat_index(archive, (zip_uint64_t)index, 0, &st) == 0 && st.size == 3 &&
              strcmp(st.name, "seg/only_in_fzerox") == 0,
          "zip_stat_index size + stable name");

    zip_file_t* file = zip_fopen_index(archive, (zip_uint64_t)index, 0);
    CHECK(file != NULL, "zip_fopen_index");
    if (file != NULL) {
        char buf[8] = { 0 };
        /* Partial read first: GDiffuser_LoadArchiveFileBytes relies on short
         * reads of large records (ghost headers), streamed, not whole-entry. */
        CHECK(zip_fread(file, buf, 2) == 2 && memcmp(buf, "FZ", 2) == 0, "partial zip_fread streams");
        CHECK(zip_fread(file, buf, 4) == 1 && buf[0] == 'X', "tail zip_fread returns remaining bytes");
        CHECK(zip_fclose(file) == 0, "zip_fclose");
    }

    zip_source_t* source = zip_source_buffer(archive, "x", 1, 0);
    CHECK(source != NULL, "zip_source_buffer hands back a token");
    CHECK(zip_file_add(archive, "new/record", source, 0) < 0, "zip_file_add refuses (read-only shim)");
    CHECK(zip_error_code_zip(zip_get_error(archive)) != 0, "zip_get_error reports the refusal");
    zip_source_free(source);

    CHECK(zip_close(archive) == 0, "zip_close");

    zip_t* missing = zip_open("sdmc:/3ds/gdiffuser/nope.o2r", ZIP_RDONLY, &zerr);
    CHECK(missing == NULL && zerr != 0, "zip_open miss without ZIP_CREATE fails");

    missing = zip_open("sdmc:/3ds/gdiffuser/nope.o2r", ZIP_CREATE, &zerr);
    CHECK(missing == NULL && zerr != 0, "ZIP_CREATE on a missing file fails loudly (no silent empty handle)");
    if (missing != NULL) {
        zip_close(missing);
    }
}

int main(void) {
    /* Fabricate the SD layout under the CWD ("sdmc:" is a plain directory to
     * the host). gdx3ds_fs_init only provisions from sdmc:/3ds down. */
    mkdir("sdmc:", 0755);
    mkdir("sdmc:/3ds", 0755);
    mkdir("sdmc:/3ds/gdiffuser", 0755);

    const char* fzKeys[] = { "seg/only_in_fzerox", "shared/duplicate_key" };
    const char* fzPayloads[] = { "FZX", "FZEROX" };
    const char* gdKeys[] = { "shaders/only_in_gdiffuser", "shared/duplicate_key" };
    const char* gdPayloads[] = { "GD", "GDIFF" };
    if (!writeFixtureArchive("sdmc:/3ds/gdiffuser/fzerox.o2r", fzKeys, fzPayloads, 2) ||
        !writeFixtureArchive("sdmc:/3ds/gdiffuser/gdiffuser.o2r", gdKeys, gdPayloads, 2)) {
        printf("FAIL: could not fabricate fixture archives\n");
        return 1;
    }

    if (gdx3ds_fs_init() != 0) {
        printf("FAIL: gdx3ds_fs_init\n");
        return 1;
    }
    printf("  ok: gdx3ds_fs_init mounted the fixture archives\n");

    testReadAsset();
    testSaves();
    testZipshim();

    gdx3ds_fs_shutdown();

    if (s_failures != 0) {
        printf("%d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
