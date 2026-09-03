/* Spike stub: decl-only libzip surface used by O2rArchive/Archive. Real libzip is a
 * portable C library over zlib and will be cross-compiled for the actual port;
 * for the compile gate only declarations are needed. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zip zip_t;
typedef struct zip_file zip_file_t;
typedef struct zip_source zip_source_t;
typedef struct zip_error {
    int zip_err;
    int sys_err;
} zip_error_t;
typedef uint64_t zip_uint64_t;
typedef int64_t zip_int64_t;
typedef uint32_t zip_uint32_t;
typedef uint32_t zip_flags_t;

struct zip_stat {
    zip_uint64_t valid;
    const char* name;
    zip_uint64_t index;
    zip_uint64_t size;
    zip_uint64_t comp_size;
};
typedef struct zip_stat zip_stat_t;

#define ZIP_RDONLY 16
#define ZIP_CREATE 1
#define ZIP_TRUNCATE 8
#define ZIP_FL_ENC_UTF_8 2048u
#define ZIP_FL_OVERWRITE 8192u

zip_t* zip_open(const char* path, int flags, int* errorp);
zip_int64_t zip_name_locate(zip_t* archive, const char* fname, zip_flags_t flags);
void zip_stat_init(zip_stat_t* st);
int zip_stat_index(zip_t* archive, zip_uint64_t index, zip_flags_t flags, zip_stat_t* st);
zip_file_t* zip_fopen_index(zip_t* archive, zip_uint64_t index, zip_flags_t flags);
zip_int64_t zip_fread(zip_file_t* file, void* buf, zip_uint64_t nbytes);
int zip_fclose(zip_file_t* file);
zip_int64_t zip_get_num_entries(zip_t* archive, zip_flags_t flags);
const char* zip_get_name(zip_t* archive, zip_uint64_t index, zip_flags_t flags);
int zip_close(zip_t* archive);
void zip_discard(zip_t* archive);
zip_source_t* zip_source_buffer(zip_t* archive, const void* data, zip_uint64_t len, int freep);
void zip_source_free(zip_source_t* source);
zip_int64_t zip_file_add(zip_t* archive, const char* name, zip_source_t* source, zip_flags_t flags);
zip_error_t* zip_get_error(zip_t* archive);
const char* zip_error_strerror(zip_error_t* ze);
int zip_error_code_zip(const zip_error_t* ze);

#ifdef __cplusplus
}
#endif
