/* port/3ds/assets/gdx3ds_fs_sd.c -- SD-card implementation of the frozen
 * gdx3ds_fs.h contract (stream D).
 *
 * Asset side: read-on-demand O2R (zip) access over vendored miniz 3.0.2
 * (third_party/miniz/). mz_zip_reader_init_file keeps a FILE* open and reads
 * only the central directory up front (~a few hundred KB for the 3610-record
 * fzerox.o2r), then inflates individual records on demand -- the archive is
 * NEVER loaded whole into RAM (3DS application-region budget, see
 * docs/research/3ds-port-plan.md stream F).
 *
 * Mount order mirrors the desktop port exactly (port/main.cpp
 * findArchivePaths -> libultraship ArchiveManager::AddArchiveUnlocked):
 * archives are mounted gdiffuser.o2r first, then fzerox.o2r, and
 * ArchiveManager's mFileToArchive[hash] = archive overwrite makes the LAST
 * mounted archive win duplicate keys. Lookups here therefore probe the mount
 * list in REVERSE (fzerox.o2r before gdiffuser.o2r), which is the same
 * observable behavior.
 *
 * Record lookup is case-SENSITIVE (MZ_ZIP_FLAG_CASE_SENSITIVE) to match
 * libzip's zip_name_locate(archive, name, 0) used by O2rArchive on desktop.
 * Zero-size records are treated as a miss, again matching O2rArchive.
 *
 * Save side: whole-blob read/write under gdx3ds_fs_base_path()/saves/ with the
 * same stage-to-.tmp + rename pattern as port/sram_buffer.cpp, so a power pull
 * mid-write can never corrupt the previous save. FAT (sdmc:) rename may refuse
 * to replace an existing destination, so a failed rename falls back to
 * unlink-then-rename; the only remaining loss window is between those two
 * calls, and it loses the OLD save only if the NEW one is already complete on
 * disk under the .tmp name (recoverable by hand, never silent corruption of a
 * half-written file).
 *
 * Threading: the contract is called from the single loader thread (LUS
 * resource manager path); no locking here. If integration adds concurrent
 * callers, add a LightLock around the mz_zip_reader_* calls (miniz readers are
 * not internally synchronized).
 *
 * This file is portable C (stdio + miniz only) so the host build
 * compile-checks it via the gdx3ds_assets_sd_checkbuild object target even
 * though the host gdx3ds_assets library links the Phase 0 stub.
 */
#include "gdx3ds_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define gdx3ds_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define gdx3ds_mkdir(path) mkdir((path), 0755)
#endif

#include "miniz.h"

#define GDX3DS_FS_BASE "sdmc:/3ds/gdiffuser/"
#define GDX3DS_FS_SAVE_DIR GDX3DS_FS_BASE "saves/"
#define GDX3DS_FS_MAX_PATH 256

/* Mount order = desktop findArchivePaths order. Lookup order is the reverse
 * (last mounted wins), mirroring ArchiveManager's map-overwrite semantics. */
static const char* const kArchiveNames[] = { "gdiffuser.o2r", "fzerox.o2r" };
#define GDX3DS_FS_ARCHIVE_COUNT (sizeof(kArchiveNames) / sizeof(kArchiveNames[0]))

static mz_zip_archive s_archives[GDX3DS_FS_ARCHIVE_COUNT];
static int s_mounted[GDX3DS_FS_ARCHIVE_COUNT];
static int s_initialized = 0;

const char* gdx3ds_fs_base_path(void) {
    return GDX3DS_FS_BASE;
}

static int gdx3ds_fs_mount_archive(size_t slot) {
    char path[GDX3DS_FS_MAX_PATH];
    if (snprintf(path, sizeof(path), "%s%s", GDX3DS_FS_BASE, kArchiveNames[slot]) >= (int)sizeof(path)) {
        return 0;
    }

    mz_zip_zero_struct(&s_archives[slot]);
    if (!mz_zip_reader_init_file(&s_archives[slot], path, 0)) {
        fprintf(stderr, "[gdx3ds_fs] could not mount %s (miniz error %d)\n", path,
                (int)mz_zip_get_last_error(&s_archives[slot]));
        return 0;
    }

    fprintf(stderr, "[gdx3ds_fs] mounted %s (%u records)\n", path,
            (unsigned)mz_zip_reader_get_num_files(&s_archives[slot]));
    return 1;
}

int gdx3ds_fs_init(void) {
    if (s_initialized) {
        return 0;
    }

    /* Best-effort directory provisioning so first-boot saves have somewhere to
     * land; EEXIST (or a host build where sdmc:/ is meaningless) is fine. */
    gdx3ds_mkdir("sdmc:/3ds");
    gdx3ds_mkdir(GDX3DS_FS_BASE);
    gdx3ds_mkdir(GDX3DS_FS_SAVE_DIR);

    for (size_t i = 0; i < GDX3DS_FS_ARCHIVE_COUNT; i++) {
        s_mounted[i] = gdx3ds_fs_mount_archive(i);
    }

    /* fzerox.o2r carries the game content; without it there is nothing to run.
     * gdiffuser.o2r absence is only a warning (desktop tolerates it too). */
    if (!s_mounted[GDX3DS_FS_ARCHIVE_COUNT - 1]) {
        fprintf(stderr,
                "[gdx3ds_fs] fzerox.o2r missing under %s -- run tools/prebake on PC and copy the "
                "3ds/ folder onto the SD card\n",
                GDX3DS_FS_BASE);
        for (size_t i = 0; i < GDX3DS_FS_ARCHIVE_COUNT; i++) {
            if (s_mounted[i]) {
                mz_zip_reader_end(&s_archives[i]);
                s_mounted[i] = 0;
            }
        }
        return -1;
    }

    s_initialized = 1;
    return 0;
}

void gdx3ds_fs_shutdown(void) {
    for (size_t i = 0; i < GDX3DS_FS_ARCHIVE_COUNT; i++) {
        if (s_mounted[i]) {
            mz_zip_reader_end(&s_archives[i]);
            s_mounted[i] = 0;
        }
    }
    s_initialized = 0;
}

void* gdx3ds_fs_read_asset(const char* recordPath, size_t* outSize) {
    if (outSize != NULL) {
        *outSize = 0;
    }
    if (recordPath == NULL || recordPath[0] == '\0' || !s_initialized) {
        return NULL;
    }

    /* Reverse mount order: last mounted archive wins duplicate keys, exactly
     * like ArchiveManager's virtual file system on desktop. */
    for (size_t i = GDX3DS_FS_ARCHIVE_COUNT; i-- > 0;) {
        if (!s_mounted[i]) {
            continue;
        }

        mz_zip_archive* zip = &s_archives[i];
        mz_uint32 index = 0;
        if (!mz_zip_reader_locate_file_v2(zip, recordPath, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE, &index)) {
            continue;
        }
        if (mz_zip_reader_is_file_a_directory(zip, index)) {
            continue;
        }

        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(zip, index, &st)) {
            continue;
        }
        if (st.m_uncomp_size == 0) {
            continue; /* zero-size entry == miss, matching O2rArchive::LoadFile */
        }

        size_t size = (size_t)st.m_uncomp_size;
        void* buffer = malloc(size);
        if (buffer == NULL) {
            fprintf(stderr, "[gdx3ds_fs] out of memory reading %s (%u bytes)\n", recordPath, (unsigned)size);
            return NULL;
        }
        if (!mz_zip_reader_extract_to_mem(zip, index, buffer, size, 0)) {
            fprintf(stderr, "[gdx3ds_fs] inflate failed for %s in %s (miniz error %d)\n", recordPath,
                    kArchiveNames[i], (int)mz_zip_get_last_error(zip));
            free(buffer);
            return NULL;
        }

        if (outSize != NULL) {
            *outSize = size;
        }
        return buffer;
    }

    return NULL;
}

/* Bare filename only: the contract forbids paths, and enforcing it here keeps
 * every save inside the saves/ folder no matter what a caller passes. */
static int gdx3ds_fs_save_path(const char* name, char* outPath, size_t outCap) {
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL || strstr(name, "..") != NULL) {
        return -1;
    }
    if (snprintf(outPath, outCap, "%s%s", GDX3DS_FS_SAVE_DIR, name) >= (int)outCap) {
        return -1;
    }
    return 0;
}

int gdx3ds_fs_read_save(const char* name, void* buf, size_t size) {
    char path[GDX3DS_FS_MAX_PATH];
    if (buf == NULL || size == 0 || gdx3ds_fs_save_path(name, path, sizeof(path)) != 0) {
        return -1;
    }

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return -1; /* no save yet: caller starts fresh, matching gdx_sram_init */
    }

    size_t got = fread(buf, 1, size, f);
    fclose(f);
    if (got != size) {
        fprintf(stderr, "[gdx3ds_fs] short read on %s (%u of %u bytes)\n", path, (unsigned)got, (unsigned)size);
        return -1;
    }
    return 0;
}

int gdx3ds_fs_write_save(const char* name, const void* buf, size_t size) {
    char path[GDX3DS_FS_MAX_PATH];
    char tempPath[GDX3DS_FS_MAX_PATH + 8];
    if (buf == NULL || size == 0 || gdx3ds_fs_save_path(name, path, sizeof(path)) != 0) {
        return -1;
    }
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);

    /* First write on a fresh card: the directories may not exist yet if init
     * never ran (contract does not order init before writes). Best effort. */
    gdx3ds_mkdir("sdmc:/3ds");
    gdx3ds_mkdir(GDX3DS_FS_BASE);
    gdx3ds_mkdir(GDX3DS_FS_SAVE_DIR);

    FILE* f = fopen(tempPath, "wb");
    if (f == NULL) {
        fprintf(stderr, "[gdx3ds_fs] could not open %s for writing; save not persisted\n", tempPath);
        return -1;
    }

    int ok = fwrite(buf, 1, size, f) == size;
    if (fflush(f) != 0) {
        ok = 0;
    }
    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "[gdx3ds_fs] failed writing %s; save not persisted\n", tempPath);
        remove(tempPath);
        return -1;
    }

    /* Stage-then-rename, as in port/sram_buffer.cpp. POSIX rename replaces the
     * destination atomically; the 3DS FAT layer can instead fail with an
     * existing destination, so fall back to unlink+rename. The .tmp file is
     * complete at this point, so even the fallback's worst case leaves a fully
     * written save on disk under one of the two names. */
    if (rename(tempPath, path) != 0) {
        remove(path);
        if (rename(tempPath, path) != 0) {
            fprintf(stderr, "[gdx3ds_fs] could not replace %s; save left at %s\n", path, tempPath);
            return -1;
        }
    }
    return 0;
}
