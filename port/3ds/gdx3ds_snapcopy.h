/* port/3ds/gdx3ds_snapcopy.h -- snap-fast: the snapshot module's copy engine (port/3ds/gdx_snapshot.c).
 *
 * Why it exists. devkitARM's newlib memcpy is the generic C one: a 16-byte loop of single LDR/STR
 * pairs, no LDM/STM, no PLD, and a BYTE loop whenever dst or src is not word-aligned. The ARM11's
 * L1 line is 32 bytes; a whole-line LDM/STM burst with a PLD a few lines ahead is the copy the
 * hardware wants. Measured on the New 3DS (card-4, [debug] snap_probe=1): 892,832 bytes in
 * 4.3-4.9 ms save / 4.6-5.8 ms restore (175-196 MB/s) with newlib.
 *
 * MODES (the probe cycles them so one hardware run A/Bs every one; [debug] snap_copy pins one):
 *   GDX_SNAPCOPY_LIBC   newlib memcpy (card-4's path)
 *   GDX_SNAPCOPY_LDM    32-byte LDMIA/STMIA bursts
 *   GDX_SNAPCOPY_PLD    the same plus PLD [src, #128] per line (default)
 * Any mode falls back to memcpy for a misaligned pair or a sub-32-byte tail.
 *
 * SYNC (dirty copy): gdx3ds_snapsync copies only the GDX_SNAPSYNC_BLOCK-byte blocks of src that
 * differ from dst. Reads both streams, writes the dirty fraction; for a restore after a few ticks
 * (run-ahead / rollback) most of the image is unchanged. The result is byte-identical to a full
 * copy -- port/3ds/test/gdx3ds_snapcopy_host_tests.c proves it against memcpy.
 *
 * Host-buildable: the ARM bursts are under __arm__; elsewhere the modes are portable C with the
 * same semantics, which is what the host test exercises (the gate exercises the ARM path). */
#ifndef GDX3DS_SNAPCOPY_H
#define GDX3DS_SNAPCOPY_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_SNAPCOPY_LIBC = 0,
    GDX_SNAPCOPY_LDM = 1,
    GDX_SNAPCOPY_PLD = 2,
    GDX_SNAPCOPY_MODES = 3
};

#define GDX_SNAPSYNC_BLOCK 256u

/* Copies n bytes (the regions must not overlap). */
void gdx3ds_snapcopy(void* dst, const void* src, unsigned int n, int mode);

/* Makes dst equal to src, writing only the GDX_SNAPSYNC_BLOCK blocks that differ. Returns the
 * number of bytes written. Same mode values as gdx3ds_snapcopy (for the dirty blocks' copy and the
 * compare's prefetch). */
unsigned int gdx3ds_snapsync(void* dst, const void* src, unsigned int n, int mode);

const char* gdx3ds_snapcopy_mode_name(int mode);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_SNAPCOPY_H */
