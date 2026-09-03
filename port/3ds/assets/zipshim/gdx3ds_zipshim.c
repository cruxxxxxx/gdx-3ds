/* port/3ds/assets/zipshim/gdx3ds_zipshim.c -- libzip API surface over miniz.
 *
 * Implements exactly the declarations in port/3ds/lus_stubs/zip.h (the surface
 * libultraship's O2rArchive/Archive consume), so LUS links on 3DS without
 * libzip -- /opt/devkitpro/portlibs/3ds is empty and installing portlibs needs
 * root. Link target: gdx3ds_zipshim (see ../CMakeLists.txt); it publishes the
 * lus_stubs include dir so O2rArchive picks up the same zip.h it was carved
 * against.
 *
 * READ side is complete and mirrors libzip semantics where O2rArchive relies
 * on them:
 *   - zip_name_locate(a, name, 0) is case-SENSITIVE (MZ_ZIP_FLAG_CASE_SENSITIVE).
 *   - zip_fopen_index/zip_fread stream through miniz's extract iterator, so a
 *     partial read of a large record never inflates or buffers the whole entry
 *     beyond miniz's internal 32 KB window (3DS RAM budget).
 *   - zip_stat fills the fields O2rArchive touches: valid, name, index, size,
 *     comp_size.
 *
 * WRITE side (zip_source_buffer / zip_file_add) is deliberately
 * read-only-degraded: O2rArchive::WriteFile is only reached from desktop-side
 * editor/mod flows that do not exist on 3DS. zip_file_add fails cleanly (-1)
 * and O2rArchive::WriteFile logs and returns false; nothing crashes. A
 * ZIP_CREATE open of an EXISTING archive (what O2rArchive::Open does) opens it
 * for reading, matching libzip's behavior of opening an existing archive
 * in-place. A ZIP_CREATE open of a MISSING/unreadable file FAILS (NULL +
 * ZIP_ER_NOENT) instead of libzip's create-new-archive behavior: the shim
 * cannot stage writes, and returning an empty readable handle here once masked
 * a runtime open failure as "archive mounted, zero entries" — every resource
 * lookup then failed silently. Open failures are svc-logged (path, miniz
 * error, errno, cwd) so a bad path is diagnosable from the Azahar log.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __3DS__
#include <3ds.h>
#endif

#include "zip.h"
#include "miniz.h"

/* libzip zip_stat.valid bits (only the ones the surface exposes). */
#define GDX_ZIP_STAT_NAME 0x0001u
#define GDX_ZIP_STAT_INDEX 0x0002u
#define GDX_ZIP_STAT_SIZE 0x0004u
#define GDX_ZIP_STAT_COMP_SIZE 0x0008u

/* libzip error codes used by the shim. */
#define GDX_ZIP_ER_OK 0
#define GDX_ZIP_ER_NOENT 9      /* no such file */
#define GDX_ZIP_ER_OPNOTSUPP 28 /* operation not supported (writes) */
#define GDX_ZIP_ER_MEMORY 14
#define GDX_ZIP_ER_READ 5

struct zip {
    mz_zip_archive mz;
    int readable;    /* 1: mz is an initialized reader */
    char** names;    /* stable storage for zip_get_name/zip_stat */
    mz_uint numEntries;
    zip_error_t err;
};

struct zip_file {
    mz_zip_reader_extract_iter_state* iter;
};

/* zip_source is an inert token: creation succeeds so the caller reaches
 * zip_file_add, which reports the honest "read-only" failure. */
struct zip_source {
    int unused;
};

/* Bounded diagnostic logging: open results must be attributable from the
 * device log (svcOutputDebugString reaches the Azahar log), but zip_open is
 * also the per-handle pool refill path, so cap total lines. */
#define GDX_ZIPSHIM_LOG_CAP 16

static void gdx_zipshim_logf(const char* fmt, ...) {
    static int linesLogged = 0;
    if (linesLogged >= GDX_ZIPSHIM_LOG_CAP) {
        return;
    }
    linesLogged++;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
#ifdef __3DS__
    svcOutputDebugString(buf, strlen(buf));
#else
    fprintf(stderr, "%s\n", buf);
#endif
}

static void gdx_zipshim_set_error(zip_t* archive, int code) {
    if (archive != NULL) {
        archive->err.zip_err = code;
        archive->err.sys_err = 0;
    }
}

static void gdx_zipshim_free_names(zip_t* archive) {
    if (archive->names == NULL) {
        return;
    }
    for (mz_uint i = 0; i < archive->numEntries; i++) {
        free(archive->names[i]);
    }
    free(archive->names);
    archive->names = NULL;
}

/* Build the stable name table once at open: zip_get_name/zip_stat return
 * pointers owned by the archive (libzip contract), while miniz's
 * mz_zip_reader_get_filename copies into a caller buffer. ~3.6k entries at
 * ~40 bytes each is ~150 KB -- the same order as libzip's own in-memory
 * central directory. */
static int gdx_zipshim_index_names(zip_t* archive) {
    archive->numEntries = mz_zip_reader_get_num_files(&archive->mz);
    if (archive->numEntries == 0) {
        return 1;
    }
    archive->names = (char**)calloc(archive->numEntries, sizeof(char*));
    if (archive->names == NULL) {
        return 0;
    }
    for (mz_uint i = 0; i < archive->numEntries; i++) {
        mz_uint len = mz_zip_reader_get_filename(&archive->mz, i, NULL, 0); /* includes NUL */
        char* name = (char*)malloc(len > 0 ? len : 1);
        if (name == NULL) {
            gdx_zipshim_free_names(archive);
            return 0;
        }
        name[0] = '\0';
        if (len > 0) {
            mz_zip_reader_get_filename(&archive->mz, i, name, len);
        }
        archive->names[i] = name;
    }
    return 1;
}

zip_t* zip_open(const char* path, int flags, int* errorp) {
    if (errorp != NULL) {
        *errorp = GDX_ZIP_ER_OK;
    }
    if (path == NULL) {
        if (errorp != NULL) {
            *errorp = GDX_ZIP_ER_NOENT;
        }
        return NULL;
    }

    zip_t* archive = (zip_t*)calloc(1, sizeof(zip_t));
    if (archive == NULL) {
        if (errorp != NULL) {
            *errorp = GDX_ZIP_ER_MEMORY;
        }
        return NULL;
    }

    mz_zip_zero_struct(&archive->mz);
    errno = 0;
    if (mz_zip_reader_init_file(&archive->mz, path, 0)) {
        archive->readable = 1;
        if (!gdx_zipshim_index_names(archive)) {
            mz_zip_reader_end(&archive->mz);
            free(archive);
            if (errorp != NULL) {
                *errorp = GDX_ZIP_ER_MEMORY;
            }
            return NULL;
        }
        gdx_zipshim_logf("[zipshim] open ok path=%s entries=%u", path, (unsigned)archive->numEntries);
        return archive;
    }

    /* Missing/unreadable file. libzip would stage a new empty archive under
     * ZIP_CREATE, but this shim cannot write one, and handing back an empty
     * readable handle masks the failure as "0 entries" (see file header).
     * Fail loudly instead, with enough context to attribute the bad open. */
    {
        int savedErrno = errno;
        char cwd[128];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            strcpy(cwd, "(getcwd failed)");
        }
        gdx_zipshim_logf("[zipshim] open FAIL path=%s flags=0x%x mzerr=%s errno=%d cwd=%s", path, (unsigned)flags,
                         mz_zip_get_error_string(mz_zip_get_last_error(&archive->mz)), savedErrno, cwd);
    }

    free(archive);
    if (errorp != NULL) {
        *errorp = GDX_ZIP_ER_NOENT;
    }
    return NULL;
}

zip_int64_t zip_name_locate(zip_t* archive, const char* fname, zip_flags_t flags) {
    (void)flags; /* libzip default (0) is case-sensitive, no dir-name match */
    if (archive == NULL || fname == NULL || !archive->readable) {
        return -1;
    }
    mz_uint32 index = 0;
    if (!mz_zip_reader_locate_file_v2(&archive->mz, fname, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE, &index)) {
        gdx_zipshim_set_error(archive, GDX_ZIP_ER_NOENT);
        return -1;
    }
    return (zip_int64_t)index;
}

void zip_stat_init(zip_stat_t* st) {
    if (st != NULL) {
        memset(st, 0, sizeof(*st));
    }
}

int zip_stat_index(zip_t* archive, zip_uint64_t index, zip_flags_t flags, zip_stat_t* st) {
    (void)flags;
    if (archive == NULL || st == NULL || !archive->readable || index >= archive->numEntries) {
        return -1;
    }
    mz_zip_archive_file_stat mzStat;
    if (!mz_zip_reader_file_stat(&archive->mz, (mz_uint)index, &mzStat)) {
        gdx_zipshim_set_error(archive, GDX_ZIP_ER_READ);
        return -1;
    }
    st->name = archive->names[index];
    st->index = index;
    st->size = (zip_uint64_t)mzStat.m_uncomp_size;
    st->comp_size = (zip_uint64_t)mzStat.m_comp_size;
    st->valid = GDX_ZIP_STAT_NAME | GDX_ZIP_STAT_INDEX | GDX_ZIP_STAT_SIZE | GDX_ZIP_STAT_COMP_SIZE;
    return 0;
}

zip_file_t* zip_fopen_index(zip_t* archive, zip_uint64_t index, zip_flags_t flags) {
    (void)flags;
    if (archive == NULL || !archive->readable || index >= archive->numEntries) {
        return NULL;
    }
    zip_file_t* file = (zip_file_t*)calloc(1, sizeof(zip_file_t));
    if (file == NULL) {
        gdx_zipshim_set_error(archive, GDX_ZIP_ER_MEMORY);
        return NULL;
    }
    file->iter = mz_zip_reader_extract_iter_new(&archive->mz, (mz_uint)index, 0);
    if (file->iter == NULL) {
        gdx_zipshim_set_error(archive, GDX_ZIP_ER_READ);
        free(file);
        return NULL;
    }
    return file;
}

zip_int64_t zip_fread(zip_file_t* file, void* buf, zip_uint64_t nbytes) {
    if (file == NULL || file->iter == NULL || buf == NULL) {
        return -1;
    }
    size_t got = mz_zip_reader_extract_iter_read(file->iter, buf, (size_t)nbytes);
    return (zip_int64_t)got;
}

int zip_fclose(zip_file_t* file) {
    if (file == NULL) {
        return -1;
    }
    if (file->iter != NULL) {
        mz_zip_reader_extract_iter_free(file->iter);
    }
    free(file);
    return 0;
}

zip_int64_t zip_get_num_entries(zip_t* archive, zip_flags_t flags) {
    (void)flags;
    if (archive == NULL) {
        return -1;
    }
    return (zip_int64_t)archive->numEntries;
}

const char* zip_get_name(zip_t* archive, zip_uint64_t index, zip_flags_t flags) {
    (void)flags;
    if (archive == NULL || archive->names == NULL || index >= archive->numEntries) {
        return NULL;
    }
    return archive->names[index];
}

int zip_close(zip_t* archive) {
    if (archive == NULL) {
        return -1;
    }
    if (archive->readable) {
        mz_zip_reader_end(&archive->mz);
    }
    gdx_zipshim_free_names(archive);
    free(archive);
    return 0;
}

void zip_discard(zip_t* archive) {
    (void)zip_close(archive); /* read-only shim: nothing staged to discard */
}

zip_source_t* zip_source_buffer(zip_t* archive, const void* data, zip_uint64_t len, int freep) {
    (void)data;
    (void)len;
    (void)freep;
    if (archive == NULL) {
        return NULL;
    }
    zip_source_t* source = (zip_source_t*)calloc(1, sizeof(zip_source_t));
    if (source == NULL) {
        gdx_zipshim_set_error(archive, GDX_ZIP_ER_MEMORY);
    }
    return source;
}

void zip_source_free(zip_source_t* source) {
    free(source);
}

zip_int64_t zip_file_add(zip_t* archive, const char* name, zip_source_t* source, zip_flags_t flags) {
    (void)name;
    (void)source;
    (void)flags;
    /* Read-only shim: archive mutation is a desktop-only flow. The caller
     * (O2rArchive::WriteFile) frees the source and reports failure. */
    gdx_zipshim_set_error(archive, GDX_ZIP_ER_OPNOTSUPP);
    return -1;
}

zip_error_t* zip_get_error(zip_t* archive) {
    static zip_error_t nullError = { GDX_ZIP_ER_OK, 0 };
    if (archive == NULL) {
        return &nullError;
    }
    return &archive->err;
}

const char* zip_error_strerror(zip_error_t* ze) {
    if (ze == NULL) {
        return "no error";
    }
    switch (ze->zip_err) {
        case GDX_ZIP_ER_OK:
            return "no error";
        case GDX_ZIP_ER_NOENT:
            return "no such file";
        case GDX_ZIP_ER_MEMORY:
            return "out of memory";
        case GDX_ZIP_ER_READ:
            return "read error";
        case GDX_ZIP_ER_OPNOTSUPP:
            return "operation not supported (gdx3ds_zipshim is read-only)";
        default:
            return "unknown error";
    }
}

int zip_error_code_zip(const zip_error_t* ze) {
    return (ze != NULL) ? ze->zip_err : GDX_ZIP_ER_OK;
}
