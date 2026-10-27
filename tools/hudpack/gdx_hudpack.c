/* tools/hudpack/gdx_hudpack.c -- gdx-hudpack: bake the 3DS bottom-screen HUD on the PC.
 *
 *   gdx-hudpack --archive gdiffuser.o2r --archive fzerox.o2r --out hudbake.cache
 *
 * Runs gdx3ds_hudbake_run (port/3ds/gdx3ds_hudbake.c, the console's own recipes) over the given
 * archives and writes hudbake.cache, the console's SD cache format, byte for byte what the console
 * writes after baking from the same archives (tools/prebake/README.md has where it goes). The
 * archives behave as the console mounts them: in the order given (gdiffuser.o2r, then
 * fzerox.o2r), the LAST one holding a key serves it, and a key's fingerprint is mixed from every
 * archive that holds it (gdx3ds_zipshim_entry_fingerprint). Exit status: 0 written, 1 the bake was
 * incomplete (a texture missing or the wrong format: not the retail archive?), 2 usage, 3 I/O. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx3ds_hudbake.h"
#include "miniz.h"

#define HUDPACK_MAX_ARCHIVES 8

static mz_zip_archive sArchives[HUDPACK_MAX_ARCHIVES];
static const char* sArchivePaths[HUDPACK_MAX_ARCHIVES];
static int sArchiveCount = 0;

static int OpenArchive(const char* path) {
    mz_zip_archive* zip = &sArchives[sArchiveCount];
    memset(zip, 0, sizeof(*zip));
    if (!mz_zip_reader_init_file(zip, path, 0)) {
        fprintf(stderr, "gdx-hudpack: cannot open %s as a zip archive\n", path);
        return 0;
    }
    sArchivePaths[sArchiveCount++] = path;
    return 1;
}

static int Locate(mz_zip_archive* zip, const char* key, mz_uint32* index) {
    return mz_zip_reader_locate_file_v2(zip, key, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE, index);
}

/* GdxHudBakeReadFn: the whole record from the last-mounted archive holding it. */
static size_t ReadRecord(const char* key, void* buf, size_t cap, void* user) {
    int a;
    (void) user;
    for (a = sArchiveCount - 1; a >= 0; a--) {
        mz_uint32 index = 0;
        size_t n = 0;
        void* data;
        if (!Locate(&sArchives[a], key, &index)) {
            continue;
        }
        data = mz_zip_reader_extract_to_heap(&sArchives[a], index, &n, 0);
        if (data == NULL) {
            return 0;
        }
        if (n > cap) {
            n = cap;
        }
        memcpy(buf, data, n);
        mz_free(data);
        return n;
    }
    return 0;
}

/* GdxHudBakeFingerprintFn: the console's sum over every archive holding the key. */
static int Fingerprint(const char* key, unsigned int* fingerprint, void* user) {
    unsigned int sum = 0;
    int found = 0;
    int a;
    (void) user;
    for (a = 0; a < sArchiveCount; a++) {
        mz_uint32 index = 0;
        mz_zip_archive_file_stat st;
        if (!Locate(&sArchives[a], key, &index) || !mz_zip_reader_file_stat(&sArchives[a], index, &st)) {
            continue;
        }
        sum += gdx3ds_hudbake_fingerprint_entry((unsigned int) st.m_crc32, (unsigned long long) st.m_uncomp_size);
        found++;
    }
    *fingerprint = found != 0 ? sum : 0u;
    return found;
}

static void Log(const char* line, void* user) {
    (void) user;
    fprintf(stderr, "%s\n", line);
}

static int WriteAtomically(const char* path, const void* data, size_t len) {
    char tmp[4096];
    FILE* f;
    int ok;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) {
        return 0;
    }
    f = fopen(tmp, "wb");
    if (f == NULL) {
        return 0;
    }
    ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp);
        return 0;
    }
    remove(path); /* Windows rename() does not replace */
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

static int Usage(void) {
    fprintf(stderr,
            "usage: gdx-hudpack --archive <gdiffuser.o2r> --archive <fzerox.o2r> --out <hudbake.cache>\n"
            "       gdx-hudpack --recipe-id\n"
            "Bakes the 3DS bottom-screen HUD from your own archives (mount order = the order given;\n"
            "the console mounts gdiffuser.o2r, then fzerox.o2r) into the console's SD cache file.\n");
    return 2;
}

int main(int argc, char** argv) {
    const char* out = NULL;
    GdxHudBakeStats st;
    unsigned char* data;
    size_t need;
    size_t len;
    unsigned digest;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--archive") == 0 && i + 1 < argc) {
            if (sArchiveCount >= HUDPACK_MAX_ARCHIVES || !OpenArchive(argv[++i])) {
                return 3;
            }
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--recipe-id") == 0) {
            printf("0x%08X\n", gdx3ds_hudbake_recipe_id());
            return 0;
        } else {
            return Usage();
        }
    }
    if (out == NULL || sArchiveCount == 0) {
        return Usage();
    }
    if (gdx3ds_hudbake_run(ReadRecord, NULL, Log, NULL, &st) != 0) {
        fprintf(stderr, "gdx-hudpack: the bake is incomplete (%d of %d textures missing or the wrong format); "
                        "no pack written -- the console will bake on first boot instead\n",
                st.missing, st.textures);
        return 1;
    }
    digest = gdx3ds_hudbake_digest();
    need = gdx3ds_hudbake_cache_write(NULL, 0, Fingerprint, NULL);
    data = need != 0 ? (unsigned char*) malloc(need) : NULL;
    len = data != NULL ? gdx3ds_hudbake_cache_write(data, need, Fingerprint, NULL) : 0;
    if (len == 0) {
        fprintf(stderr, "gdx-hudpack: could not serialise the bake\n");
        free(data);
        return 3;
    }
    if (!WriteAtomically(out, data, len)) {
        fprintf(stderr, "gdx-hudpack: cannot write %s\n", out);
        free(data);
        return 3;
    }
    free(data);
    printf("gdx-hudpack: %s: %u bytes, %d textures, recipe id 0x%08X, digest 0x%08X (%s)\n", out, (unsigned) len,
           st.textures, gdx3ds_hudbake_recipe_id(), digest,
           digest == GDX3DS_HUDBAKE_DIGEST_RETAIL ? "the retail archive's" : "NOT the retail archive's");
    return 0;
}
