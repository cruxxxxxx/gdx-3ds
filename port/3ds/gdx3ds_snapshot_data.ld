/* port/3ds/gdx3ds_snapshot_data.ld -- mp-snapshot: the simulation's .data block (an INCLUDE fragment).
 *
 * gdx3ds_snapshot_ld.cmake builds the link's script from devkitARM's stock 3dsx.ld and INCLUDEs this
 * file at the TOP of its `.data` output section, ahead of the stock `*(.data)` statement: an input
 * section is placed by the first statement that matches it, so the sim TUs land here as one run.
 *
 *   every decomp TU's .data except the audio libraries (and external.c, also under src/audio/ via
 *   the audio thread's ownership -- see gdx_snapshot.c), then the port's GDX_SNAP_STATE_DATA
 *   statics (`.data.gdx_snap`, port/gdx_snapshot.h).
 *
 * Decomp sources are compiled without -fdata-sections, so a decomp TU's data is exactly `.data`. */
. = ALIGN(16);
__gdx_snap_data_start = .;
EXCLUDE_FILE(*/decomp/src/audio/*) */decomp/src/*(.data)
*(.data.gdx_snap)
. = ALIGN(16);
__gdx_snap_data_end = .;
