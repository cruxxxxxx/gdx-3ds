/* Host-build stand-in for gdx3ds_fs.h. Device builds link gdx3ds_fs_sd.c (miniz
 * O2R read-on-demand + atomic-rename saves); host builds keep this stub because
 * sdmc:/ paths are meaningless off-device, and compile-check the SD sources via
 * the gdx3ds_assets_sd_checkbuild target. Stub misses everything. */
#include "gdx3ds_fs.h"

#include <stdlib.h>

const char* gdx3ds_fs_base_path(void) {
    return "sdmc:/3ds/gdiffuser/";
}

int gdx3ds_fs_init(void) {
    return 0;
}

void gdx3ds_fs_shutdown(void) {
}

void* gdx3ds_fs_read_asset(const char* recordPath, size_t* outSize) {
    (void)recordPath;
    if (outSize != NULL) {
        *outSize = 0;
    }
    return NULL;
}

int gdx3ds_fs_read_save(const char* name, void* buf, size_t size) {
    (void)name;
    (void)buf;
    (void)size;
    return -1;
}

int gdx3ds_fs_write_save(const char* name, const void* buf, size_t size) {
    (void)name;
    (void)buf;
    (void)size;
    return -1;
}
