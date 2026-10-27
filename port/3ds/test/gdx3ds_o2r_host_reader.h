/* port/3ds/test/gdx3ds_o2r_host_reader.h -- host tests: read records from the user's own
 * fzerox.o2r (vendored miniz), for tests whose expected output depends on game data that the
 * repository deliberately does not contain (the DASH art, the track index templates).
 *
 * The archive is $GDX_O2R, else the Azahar virtual SD copy tools/ci-3ds.sh --emu already uses.
 * No archive (CI) -> gdx3ds_o2r_host_open() returns 0 and the caller skips its data checks.
 * Header-only; compile miniz.c (port/3ds/assets/third_party/miniz) into the test. */
#ifndef GDX3DS_O2R_HOST_READER_H
#define GDX3DS_O2R_HOST_READER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

static mz_zip_archive sO2rHostZip;
static int sO2rHostOpen = 0;

static inline const char* gdx3ds_o2r_host_path(void) {
    static char path[1024];
    const char* env = getenv("GDX_O2R");
    const char* home = getenv("HOME");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    snprintf(path, sizeof(path), "%s/Library/Application Support/Azahar/sdmc/3ds/gdiffuser/fzerox.o2r",
             home != NULL ? home : "");
    return path;
}

/* 1 when the archive is open (prints which one), 0 when there is none. */
static inline int gdx3ds_o2r_host_open(void) {
    const char* path = gdx3ds_o2r_host_path();
    if (sO2rHostOpen) {
        return 1;
    }
    memset(&sO2rHostZip, 0, sizeof(sO2rHostZip));
    if (!mz_zip_reader_init_file(&sO2rHostZip, path, 0)) {
        printf("note: no fzerox.o2r at %s (set GDX_O2R); game-data checks skipped\n", path);
        return 0;
    }
    printf("note: game data from %s\n", path);
    sO2rHostOpen = 1;
    return 1;
}

/* GdxHudBakeReadFn-shaped: the whole record, at most `cap` bytes; 0 on a miss. */
static inline size_t gdx3ds_o2r_host_read(const char* key, void* buf, size_t cap, void* user) {
    size_t n = 0;
    void* p;
    (void) user;
    if (!sO2rHostOpen) {
        return 0;
    }
    p = mz_zip_reader_extract_file_to_heap(&sO2rHostZip, key, &n, 0);
    if (p == NULL) {
        return 0;
    }
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, p, n);
    mz_free(p);
    return n;
}

/* GdxHudBakeFingerprintFn-shaped: the entry's CRC-32 and size from the central directory (the
 * console's gdx3ds_zipshim_entry_fingerprint, for one archive). 1 when present, 0 on a miss. */
static inline int gdx3ds_o2r_host_fingerprint(const char* key, unsigned int* fingerprint, void* user) {
    mz_uint32 index = 0;
    mz_zip_archive_file_stat st;
    (void) user;
    *fingerprint = 0;
    if (!sO2rHostOpen || !mz_zip_reader_locate_file_v2(&sO2rHostZip, key, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE, &index) ||
        !mz_zip_reader_file_stat(&sO2rHostZip, index, &st)) {
        return 0;
    }
    *fingerprint = st.m_crc32 ^ (unsigned int) (st.m_uncomp_size * 2654435761u);
    return 1;
}

#endif /* GDX3DS_O2R_HOST_READER_H */
