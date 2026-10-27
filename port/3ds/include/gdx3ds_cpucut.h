/* port/3ds/include/gdx3ds_cpucut.h -- o3ds-cpucuts: the Old 3DS CPU cuts' shared A/B clock.
 * Research: artifacts/o3ds-3d-research/agent-D-cpu-cuts.md (the ranked cuts, §5 the hardware plan).
 *
 * Every cut is an ini lever with the same three values:
 *   0 = off (the prior path), 1 = on, 2 = a same-run A/B: on in the even BLOCKS of its own bit of
 *   the block counter, off in the odd ones.
 * A block is one [c5cat] window ([debug] receipt_every host iterations, 512 on the hardware card):
 * with the mprof clock running the block ends exactly where a [c5cat] line is emitted, so every
 * [c5cat] window is measured under ONE phase of every lever; without it the module counts host
 * iterations itself. The bits are distinct per lever (gdx3ds_cpucut_lever), so the levers on 2 are
 * orthogonal: over 2^k blocks every on/off combination of k levers runs once.
 *
 * Receipt, at every block end while any lever is registered:
 *   [cpucut] f=<frame> blk=<k> n=<iterations> | <name>=<value>/<phase> ...
 * naming the phase each lever ran with in the block that just ended (the [c5cat] line printed
 * right before it). Rendering-only and diagnostic levers only: nothing here may reach the
 * simulation (lockstep hash, replay parity). Main thread only. */
#ifndef GDX3DS_CPUCUT_H
#define GDX3DS_CPUCUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* The bits each lever's A/B uses (block counter bit). A lever on bit k flips every 2^k blocks, so
 * the levers most worth a fine A/B sit low. Two levers share bit 3: their costs land in different
 * buckets ([c5cat] hud vs the [dash] tick), so one A/B splits both. */
enum {
    GDX3DS_CPUCUT_BIT_HUDKICK = 0,    /* [debug] overlap_hud_kick */
    GDX3DS_CPUCUT_BIT_HEAD1P = 1,     /* [stereo] head_kick_1p */
    GDX3DS_CPUCUT_BIT_SPECK = 2,      /* [native] nrr_speck_batch */
    GDX3DS_CPUCUT_BIT_HUDPD = 3,      /* [native] nrr_hud_pd */
    GDX3DS_CPUCUT_BIT_DASH = 3,       /* [ui] dash_race0_d1 */
    GDX3DS_CPUCUT_BIT_FLUSHRANGE = 4, /* [debug] overlap_flush_ranges */
    GDX3DS_CPUCUT_BIT_LOGDEFER = 5,   /* [debug] filelog_flush_defer */
    /* o3ds-mp16: two MP-only levers share bits with levers that are inert in a 2D lockstep race or
     * that the o3ds-auto preset now pins (overlap_hud_kick 0, nrr_speck_batch 1): a card that puts a
     * pair of bit-sharing levers on 2 together cannot split them. */
    GDX3DS_CPUCUT_BIT_D1DRIFT = 1,    /* [debug] steady_d1_drift (bit of head_kick_1p: 1P 3D only) */
    GDX3DS_CPUCUT_BIT_MPSPECK = 2,    /* [perf] mp_speck_drop (bit of nrr_speck_batch) */
};

/* A lever value clamped to 0..2 (anything else is `fallback`). */
int gdx3ds_cpucut_clamp(int value, int fallback);
/* Register a lever for the [cpucut] receipt (idempotent per name; up to 12 levers). `value` must
 * stay valid for the run (a static). */
void gdx3ds_cpucut_lever(const char* name, const int* value, unsigned bit);
/* Whether a lever of this value is on in the current block. */
int gdx3ds_cpucut_on(int value, unsigned bit);
/* onesubmit: the current block's index (a lever with more than two arms cycles on it). */
unsigned long gdx3ds_cpucut_block(void);
/* The host loop's iteration end (main_3ds.cpp, after gdx3ds_mprof_frame_end): counts the block
 * itself while the mprof clock is off. */
void gdx3ds_cpucut_frame_end(unsigned long frame);
/* The mprof clock's window end (gdx3ds_mainprof.c, right after the [c5cat] line). */
void gdx3ds_cpucut_window_end(unsigned long frame, unsigned iterations);

#ifdef __cplusplus
}
#endif

#endif
