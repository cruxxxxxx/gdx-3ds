/* port/3ds/include/gdx3ds_fs.h -- 3DS filesystem contract (stream D implements).
 *
 * CONTRACT STATUS: FROZEN (Phase 0). Changes require orchestrator sign-off.
 *
 * Covers the three persistence surfaces the desktop port has today:
 *   - asset archives (O2R read via GDiffuser_LoadArchiveFileBytes, port/AssetLoader.cpp)
 *   - SRAM saves (port/sram_buffer.cpp)
 *   - 64DD disk saves (port/disk_savefile.cpp; post-MVP content but the contract
 *     reserves the surface now so it never forces a header change)
 *
 * All paths live under gdx3ds_fs_base_path() on SD. Assets are pre-baked on PC by
 * tools/prebake (no Torch/Python on device) and copied there by the user.
 */
#ifndef GDX3DS_FS_H
#define GDX3DS_FS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "sdmc:/3ds/gdiffuser/" -- trailing slash included. */
const char* gdx3ds_fs_base_path(void);

int gdx3ds_fs_init(void);
void gdx3ds_fs_shutdown(void);

/* Read one record from a mounted .o2r archive (fzerox.o2r / gdiffuser.o2r beside
 * base path). Returns malloc'd bytes the caller frees, NULL + *outSize=0 on miss.
 * Semantics match GDiffuser_LoadArchiveFileBytes so the bridge code is agnostic. */
void* gdx3ds_fs_read_asset(const char* recordPath, size_t* outSize);

/* Save data: whole-blob read/write, atomic-rename on write so a power pull during
 * save never corrupts the previous file. name is a bare filename, no path. */
int gdx3ds_fs_read_save(const char* name, void* buf, size_t size);
int gdx3ds_fs_write_save(const char* name, const void* buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_FS_H */
