/* port/3ds/include/gdx3ds_filelog.h — optional SD-card sink for the boot/watchdog/fatal
 * tracers (H-HARDWARE).
 *
 * On real hardware the svcOutputDebugString channel the tracers rely on goes nowhere
 * useful (Azahar logs it; a retail New3DS does not, unless a Luma debugger is attached).
 * This sink mirrors those lines to sdmc:/3ds/gdiffuser/log.txt so a hardware crash
 * leaves evidence on the SD card.
 *
 * INI (sdmc:/3ds/gdiffuser/gdiffuser.ini):
 *   [debug]
 *   filelog = 1          ; default 0 (off — zero SD traffic when disabled)
 *   filelog_max_kb = 256 ; size cap; logging stops with a truncation marker when hit
 *
 * Contract:
 *   - init() after gdx3ds_config_load(); every call is a no-op when disabled.
 *   - write() is thread-safe (main + watchdog + audio threads) via a recursive lock and
 *     appends one '\n'-terminated line to a FIXED 64 KiB batch ring — never the heap, so
 *     it is safe from the operator-new failure path and can never grow (a line that
 *     does not fit is dropped and the next flush appends "[filelog] dropped=N").
 *   - Batches reach the SD card when the ring is 3/4 full, every 250 ms, on any
 *     "[fatal"-prefixed line, or on an explicit flush() — so per-line SD fflush stalls
 *     are gone while fatal lines still survive svcBreak/abort where no teardown runs.
 *   - The file is truncated on every boot: one boot, one log.
 */
#ifndef GDX3DS_FILELOG_H
#define GDX3DS_FILELOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void gdx3ds_filelog_init(void);
void gdx3ds_filelog_write(const char* msg, size_t len);
/* Extra durability point for the fatal tracers; harmless when disabled/closed. */
void gdx3ds_filelog_flush(void);

/* MENU LOG tab: copy up to maxLines retained lines (oldest first) into `out`, an
 * array of maxLines x lineCap chars, skipping the skipNewest most recent lines.
 * Returns lines written. Retention is always on (independent of debug.filelog). */
int gdx3ds_filelog_tail_lines(unsigned skipNewest, char* out, int maxLines, int lineCap);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_FILELOG_H */
