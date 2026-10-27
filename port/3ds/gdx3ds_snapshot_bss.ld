/* port/3ds/gdx3ds_snapshot_bss.ld -- mp-snapshot: the simulation's .bss block plus the two
 * not-restored decomp blocks (an INCLUDE fragment).
 *
 * gdx3ds_snapshot_ld.cmake INCLUDEs this file at the TOP of the stock `.bss` output section, so it
 * lies inside [__bss_start__, __bss_end__) and crt0 still zeroes it, and ahead of the stock
 * `*(.bss)` / `*(COMMON)` statements (first match wins).
 *
 *   SNAP BSS     every decomp TU's .bss and COMMON except the EXCLUDED FILES below, then the port's
 *                GDX_SNAP_STATE_BSS statics (`.bss.gdx_snap`).
 *   NOSNAP       `.bss.gdx_nosnap`: decomp objects inside a sim TU that a restore must never touch
 *                (boot-time / course-load tables: gSinTable, gSegmentChunks, the course track
 *                stage, the options-menu buffer), moved by decomp-3ds-snapshot.patch. Bounded, so
 *                the gate proves a race tick never writes them ("[snap] NOSNAP changed").
 *   NOSNAP_DRAW  `.bss.gdx_nosnap_draw`: course-load tables whose fields the draw pass rewrites
 *                every frame before reading them (gSegmentChunks: depth / drawState, course.c's
 *                chunk cull). Not restored, not watched.
 *   NOSCHED      `.bss.gdx_nosched`: the scheduler's thread stacks, OSThread / OSMesgQueue objects
 *                and message buffers (sys_main.c). Never rewound under the scheduler; they change
 *                every tick, so they are not watched.
 *
 * EXCLUDED FILES (they fall through to the stock statements): the audio libraries and external.c
 * (shared with the audio producer thread, which a restore would race; sim -> audio is one-way), the
 * three framebuffers, sys/buffers.c (RSP task output / yield / DRAM stack), unk_gfx_segment.c (the
 * static display-list buffers and both GfxPools: render output, rebuilt every tick), the save
 * buffers and the leo font. (EXCLUDE_FILE sits OUTSIDE the section list: inside it, it would
 * apply to `.bss` only and let the excluded files' COMMON through.) A `.bss.gdx_nosnap` input is not matched by `.bss` / `COMMON`, so a
 * sim TU's excluded objects fall through to the NOSNAP statement. */
. = ALIGN(16);
__gdx_snap_bss_start = .;
EXCLUDE_FILE(*/decomp/src/audio/* */decomp/src/framebuffers/* */decomp/src/sys/buffers.o
             */decomp/src/game/unk_gfx_segment.o */decomp/src/sys/leo_font.o
             */decomp/src/overlays/ovl_i2/save.o */decomp/src/overlays/ovl_i2/save_buffer.o
             */decomp/src/overlays/ovl_i6/save_context.o) */decomp/src/*(.bss COMMON)
*(.bss.gdx_snap)
. = ALIGN(16);
__gdx_snap_bss_end = .;
__gdx_nosnap_bss_start = .;
*(.bss.gdx_nosnap)
. = ALIGN(16);
__gdx_nosnap_bss_end = .;
*(.bss.gdx_nosnap_draw)
. = ALIGN(16);
*(.bss.gdx_nosched)
. = ALIGN(16);
__gdx_nosched_bss_end = .;
