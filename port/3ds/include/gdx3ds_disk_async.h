/* port/3ds/include/gdx3ds_disk_async.h -- o3dsekperf: the 64DD audio loader's device reads,
 * moved off the audio producer's tick onto a small I/O worker thread.
 *
 * WHY. The Expansion Kit's disk audio library loads a course's banks through the async state
 * machine in AudioLoad_DiskLoad (decomp/src/audio/disk/lib/load.c). On a 64DD the drive DMAs
 * each request in the background while that machine polls its message queue. In this port the
 * LEO handler (port/n64_leo.c LeoReadWrite) serves the request INLINE, so the SD read runs on
 * the producer thread inside its tick. `audio_disk_step_kb` already cut the batch to one LBA a
 * tick; this moves that one LBA off the tick too.
 *
 * Measured before (emulator, `gdx-dev retro run --model old`, ek-ta-dd, [debug] audioload=1):
 * every producer tick of the DD-1 race load carries diskld/leorw 4.3-9.8 ms of the tick's
 * 6.0-11.8 ms, for 141 consecutive steps (2.2 MB). Hardware, release-v1@78ff524f, Old 3DS, the
 * JP disk: the same race load logs producer ticks of `for=24-30 ms` against the 16.7 ms of
 * audio a tick produces, and the ring under-runs (edges #4-#8 in log-EK-jp.txt). One 16 KB SD
 * fill costs 18-21 ms on that console ([actx] io_unlock, same log).
 *
 * THE CONTRACT. One request in flight at a time -- the decomp's state machine never issues a
 * second before its queue saw the first complete, and gdx3ds_disk_async_submit() waits out a
 * straggler rather than overlapping one. A job runs to completion on the worker and posts its
 * own completion (LeoReadWrite's osSendMesg; cross-thread sends are what gdx_mq_lock and the
 * deferred-wake path in port/n64_sched.c exist for). The bytes are the bytes LeoReadWrite would
 * have read inline: same call, same arguments, same block cache, now under gdx_disk_stream's
 * lock because a second thread reads it.
 *
 * LEVER. `[debug] audio_disk_async`, model default 1 on the Old 3DS and 0 on the New 3DS (the
 * New 3DS has the headroom and the n3ds-ek binary shipped in v1.0). 0 = every request inline on
 * the producer, the pre-o3dsekperf path. Latched on first use, one receipt either way, and a
 * periodic `[audiodisk] async` receipt with the worker's measured SD time per request.
 *
 * COALESCED READS (ekaudio2, round 2). Off the producer, the worker's job is still one SD
 * request per 16 KB cache block, and that per-request cost is what the hardware log measures
 * (18-21 ms per 16 KB fill): a 19,720 B LBA is two or three of them. Two levers, both model
 * default 64 on the Old 3DS and 0 on the New 3DS:
 *   `[debug] disk_stream_coalesce_kb`  every 64DD read (LeoReadWrite, any thread) fetches each
 *        run of its missing blocks with ONE request of up to this size
 *        (gdx_disk_stream_fill_range). 0 = one request per block, the pre-ekaudio2 path.
 *   `[debug] audio_disk_readahead_kb`  an async audio-load request that misses also fetches the
 *        load's NEXT bytes, up to this window from the request's start (the decomp passes the
 *        load's bytesRemaining), so the following two or three LBAs are cache hits. 0 = off;
 *        forced 0 when coalescing is off.
 * Same bytes either way: the fill only makes blocks resident; LeoReadWrite still copies them
 * out through the cache and the journal overlay. Receipts: `[diskfill] ...=` once each, and
 * `[diskfill] runs= blocks=` beside every `[diskstream]` receipt.
 *
 * Only the EK build compiles this (port/3ds/game/CMakeLists.txt, EK glue sources).
 */
#ifndef GDX3DS_DISK_ASYNC_H
#define GDX3DS_DISK_ASYNC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GdxDiskAsyncJob)(void* arg);

/* 1 when the lever is on and the worker thread is up. The first call latches the lever, starts
 * the worker and logs the receipt; a worker that fails to start reads as 0 (inline fallback). */
int gdx3ds_disk_async_enabled(void);

/* Hands `job(arg)` to the worker. Returns 1 when queued; 0 when the async path is off, and the
 * caller then runs the job inline. Waits for a previous job first (see THE CONTRACT). */
int gdx3ds_disk_async_submit(GdxDiskAsyncJob job, void* arg);

/* Blocks until no job is in flight. Returns at once when none is. */
void gdx3ds_disk_async_wait_idle(void);

/* ekaudio2: the two coalesced-read levers, in BYTES (0 = off). Latched on first call. */
unsigned int gdx3ds_disk_coalesce_bytes(void);
unsigned int gdx3ds_disk_readahead_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_DISK_ASYNC_H */
