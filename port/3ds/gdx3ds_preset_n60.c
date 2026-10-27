/* port/3ds/gdx3ds_preset_n60.c — `[perf] preset = n60`: the n60 race stack as built-in defaults.
 *
 * Contract: gdx3ds_preset_n60.h. Rationale and gates: docs/research/native/n3ds-progress.md.
 *
 * The Old 3DS reached a 60 fps race (2D and 3D) with the n60 stack: the NRR native race renderer,
 * stereo frame replay with interleave, end-mode swap pacing, race0 frame skip, the HUD/track/draw
 * levers, finish_mesh and prewarm. Every one of those is an ini lever that defaults to 0, and the
 * Old 3DS card switched them on with ~110 ini keys (the config table holds 128). This file is the
 * same lever set as ONE ini key, for the New 3DS (the default, non-GDX_3DS_O3DS build):
 *
 *   [perf]
 *   preset = n60
 *
 * The values are the Old 3DS card's (n60-3d5 MAIN, gate 21) with three classes of change:
 *   - dropped: everything diagnostic (receipts, census, PMU, hitch, spike, launch variants) and the
 *     Old 3DS memory-fit levers (audio_stream, tex_upload_kb: the New 3DS has the RAM);
 *   - added: renderthread = 0. The whole n60 stack (NRR snapshot/marker path, race0 frame skip,
 *     overlap kicks, stereo replay/interleave, end-mode pacing) is the sequential-host-loop
 *     architecture (rtMode 0); the New 3DS default is the core-2 render thread, which those levers
 *     refuse. At 804 MHz with the L2 the sequential race fits a 16.7 ms frame with room to spare,
 *     and core 2 keeps the audio threads;
 *   - New 3DS headroom: tex_keep_kb 4096 (2x the Old 3DS budget, more linear heap), the Old 3DS
 *     audio model defaults (audio_simd, audio_ctx_narrow) on, full rival detail and draw distance
 *     (the [perf] LOD levers stay stock: the o3ds LOD preset is NOT applied), [stereo] enabled = 1
 *     (the 3D slider works; slider 0 renders byte-identical mono).
 *
 * An explicit ini key always wins over a value here (gdx3ds_config.h, PRESET DEFAULTS).
 */
#include "gdx3ds_preset_n60.h"

#include <stddef.h>
#include <strings.h>

/* preset-fix: the levers the hardware test cards carried as explicit ini keys ([variant.new] of
 * TEST-*-0c2668a, the last known-good cards) and card-2's "shipped defaults" ini dropped -- card-2 on
 * the Old 3DS then ran 2-3x slower (menus misc ~35 ms/frame, no `[nicourse] init ... ni_course=1`).
 * None of them was a built-in default, so a no-ini player silently lost them too. They are rows of
 * BOTH tables now (they replace the GDX3DS_PRESET_NI staging switch, whose rows had never matched
 * the cards: ni_view 0x57A on both models). tools/ci-3ds.sh's gdx3ds_preset_mustship_tests fails if
 * any of them (or the rest of each model's must-ship list) stops resolving from an EMPTY ini.
 *   - native menus S1/S2/S2b/S4 + course select + machine select / settings: ni_menu, ni_menu_root,
 *     ni_menu_split 3, ni_menu_keep, ni_menu_ucode, ni_menu_xition, ni_course, ni_mach 3,
 *     atlas_race_reset (every hardware card since ni-s0 ran them on both models);
 *   - Expansion Kit course editor: nrr_testdrive, ek_editmesh, ek_editmesh_track, ek_nimach (inert
 *     without the EK disk / editor);
 *   - 3D GPU overlap: native stereo_fold, stereo head_kick, stereo resume_fold (3D frames only; hw
 *     52acd44: GPU tail p50 8.6 -> 6.7 ms);
 *   - 3D one submission (ship6, hw card-6 ba0692e A/B, onesubmit-progress.md "Hardware result"):
 *     stereo onesubmit 1 + onesubmit_narrow 1 (arm 3; Old 3DS 3D menu 40.2 -> 44.6 fps, 1P 3D race
 *     ~1.5-1.8 ms/frame headroom: ~1.8 for this arm, ~1.5 onesubmit alone; New 3DS neutral),
 *     onesubmit_kicks 0 (lost CPU/GPU overlap cancels its
 *     CPU saving), onesubmit_ab 0 (the per-block A/B is test cards only). */
#define GDX3DS_PRESET_CARD_ROWS                   \
    { "native", "ni_menu", "1" },                \
    { "native", "ni_menu_root", "1" },           \
    { "native", "ni_menu_split", "3" },          \
    { "native", "ni_menu_keep", "1" },           \
    { "native", "ni_menu_ucode", "1" },          \
    { "native", "ni_menu_xition", "1" },         \
    { "native", "ni_course", "1" },              \
    { "native", "ni_mach", "3" },                \
    { "native", "atlas_race_reset", "1" },       \
    { "native", "nrr_testdrive", "1" },          \
    { "native", "ek_editmesh", "1" },            \
    { "native", "ek_editmesh_track", "1" },      \
    { "native", "ek_nimach", "1" },              \
    { "native", "stereo_fold", "1" },            \
    { "stereo", "head_kick", "1" },              \
    { "stereo", "resume_fold", "1" },            \
    { "stereo", "onesubmit", "1" },              \
    { "stereo", "onesubmit_narrow", "1" },       \
    { "stereo", "onesubmit_kicks", "0" },        \
    { "stereo", "onesubmit_ab", "0" },           \
    { "ui", "bottom_menus", "1" },

static const Gdx3dsCfgPresetEntry kN60Preset[] = {
    /* host loop / pacing / frame skip */
    { "debug", "renderthread", "0" },
    { "debug", "frameskip", "race0" },
    { "debug", "race0_arm", "1" },
    { "debug", "race0_dwell", "600" },
    { "debug", "race0_heavy_mode", "steady" },
    { "debug", "pace_swapgate", "1" },
    { "debug", "pace_swapgate_3d", "2" },
    { "debug", "dl_hollow", "1" },
    { "debug", "overlap_kick", "1" },
    { "debug", "swp_lean", "4" },
    { "debug", "mirror_lazy", "1" },
    { "debug", "flush_svc", "1" },
    /* textures */
    { "debug", "tex_fast", "1" },
    { "debug", "tex_keep", "1" },
    { "debug", "tex_keep_kb", "4096" },
    { "debug", "tex_keep_floor_kb", "3072" },
    /* audio + save */
    { "debug", "audio_io_unlock", "1" },
    { "debug", "audio_ctx_narrow", "1" },
    { "debug", "audio_simd", "1" },
    { "debug", "audio_rs_align", "1" },
    { "debug", "sram_async", "1" },
    { "debug", "filelog_race_quiet", "1" },
    /* the native renderer: owned interpreter + C5 retained paths */
    { "native", "all", "1" },
    { "native", "interp", "1" },
    { "native", "gpuxf", "1" },
    { "native", "track_retained", "1" },
    { "native", "track_resident", "1" },
    { "native", "track_mattab", "1" },
    { "native", "track_nearclip", "1" },
    { "native", "track_opfold", "1" },
    { "native", "hud_batch", "1" },
    { "native", "hud_direct", "1" },
    { "native", "hud_retain", "1" },
    { "native", "shadow_batch", "1" },
    { "native", "fx_batch", "1" },
    { "native", "fxshd_direct", "1" },
    { "native", "gadget_batch", "1" },
    { "native", "sky_retain", "1" },
    { "native", "machines_retained", "1" },
    { "native", "machines_texskip", "1" },
    { "native", "machines_mattab", "1" },
    { "native", "machines_loadmemo", "1" },
    { "native", "machines_walkfast", "1" },
    { "native", "machines_mattab_live", "1" },
    { "native", "c3d_diet", "1" },
    { "native", "dlmemo", "1" },
    { "native", "deadsetup", "1" },
    { "native", "replay", "0" },
    { "native", "hudsky_emit", "1" },
    /* NRR */
    { "native", "nrr", "1" },
    { "native", "nrr_hud", "1" },
    { "native", "nrr_trk", "1" },
    { "native", "nrr_mach", "1" },
    { "native", "nrr_sky", "1" },
    { "native", "nrr_shd", "1" },
    { "native", "nrr_fx", "1" },
    { "native", "nrr_dress", "1" },
    { "native", "nrr_deco", "1" },
    { "native", "nrr_direct", "1" },
    { "native", "nrr_hud_memo", "1" },
    { "native", "nrr_trk_joins", "1" },
    { "native", "nrr_hud_late", "7" },
    { "native", "nrr_trk_merge", "1" },
    { "native", "nrr_draw", "15" },
    { "native", "nrr_hud_kept", "1" },
    { "native", "nrr_hud_rr", "3" },
    { "native", "finish_mesh", "1" },
    { "native", "finish_direct", "0" },
    { "native", "prewarm", "7" },
    { "native", "prewarm_draws", "256" },
    { "native", "zclear_elide", "1" }, /* o3ds3d: pinned on in the shipped preset (2 = the 1024-frame A/B) */
    { "native", "lcd_vram", "0" },
    /* preset-fix: the hardware cards' native menus / EK / 3D-overlap levers (GDX3DS_PRESET_CARD_ROWS) */
    GDX3DS_PRESET_CARD_ROWS
    /* preset-fix, New 3DS only (TEST-NEW3DS-0c2668a): the multi-view race / records flyby categories
     * on the NRR + span programs (ni_view 0x97A, as the Old 3DS table's MP-only ni_fullview) and the
     * 1P race's inter-marker walk as span programs (ni_race); 3D menus with depth (menu_depth). */
    { "native", "ni_view", "0x97A" },
    { "native", "ni_race", "1" },
    { "stereo", "menu_depth", "1" },
    /* stereo */
    { "native", "stereo_interleave", "1" },
    { "native", "stereo_noreflush", "1" },
    { "stereo", "enabled", "1" },
    /* bottom screen */
    { "menu", "race_quiet", "1" },
};

/* n60lead: `auto` on an Old 3DS (gdx3ds_config_set_auto_preset_name("o3ds-auto"),
 * main_3ds.cpp). This is kN60Preset above, which the hardware-verified Old 3DS card (n60-3d15)
 * matches key for key for every lever BOTH tables name, MINUS the New-3DS-only tex_keep_kb
 * headroom (Old 3DS wants its compiled-in 2048 KB fallback, not n60's 4096 KB; see
 * port/3ds/gfx/n64/gdx_rt_glue.cpp), PLUS the three Old-3DS-only levers below. Every other key
 * n60-3d15's ini carries is diagnostic/telemetry (fps, hitch, spike_ms, receipts, filelog*,
 * membudget, diag_audio, audiocrk*, c5steal, c5pmu, gputl, verbose, gputrace, mainprof,
 * ships_census, logic_split -- a read-only receipts pair with ships_census, launch_variants,
 * pcm_dump_s), a user preference (border_mode, stereo.enabled already matches n60, menu
 * screen_off, the [input] block, bottom_race -- left to the player on every build, same as
 * n60), or already the shipped default with the model it already carries (vram_guard's
 * default IS VRAM_GUARD_REPAIR=1; audio_simd/audio_ctx_narrow/adpcm_scale_clamp already default
 * ON for an Old 3DS at their own call sites, texburst/stereo_xl_early/sky_late already default
 * to what the card sets) -- none of those belong in a performance preset. (preset-fix: the EK
 * disk streaming keys and stereo_fold, once listed here, are rows now -- see
 * GDX3DS_PRESET_CARD_ROWS and the Old-3DS-only block below.) Full
 * key-by-key classification: the commit message that added this table.
 *
 *   - audio_stream = 1 / tex_upload_kb = 512: the Old 3DS memory-fit levers kN60Preset's own
 *     header names as dropped when it built the New 3DS table ("the New 3DS has the RAM").
 *     Both default to 0 (off) in code; the Old 3DS needs them on.
 *   - syscore_pct_safe = 1: clamps a core-1 app-CPU-time request to the measured-safe 60%
 *     ceiling on an Old 3DS specifically (port/3ds/audio/gdx3ds_audio_ndsp.c); defaults to 0
 *     (off). 80% was measured starving the services the audio/loading pipeline depends on. */
static const Gdx3dsCfgPresetEntry kO3dsAutoPreset[] = {
    /* host loop / pacing / frame skip -- identical to kN60Preset */
    { "debug", "renderthread", "0" },
    { "debug", "frameskip", "race0" },
    { "debug", "race0_arm", "1" },
    { "debug", "race0_dwell", "600" },
    { "debug", "race0_heavy_mode", "steady" },
    { "debug", "pace_swapgate", "1" },
    { "debug", "pace_swapgate_3d", "2" },
    { "debug", "dl_hollow", "1" },
    { "debug", "overlap_kick", "1" },
    { "debug", "swp_lean", "4" },
    { "debug", "mirror_lazy", "1" },
    { "debug", "flush_svc", "1" },
    /* textures -- identical to kN60Preset except tex_keep_kb (omitted: the Old 3DS wants its
     * own compiled-in 2048 KB fallback, not n60's New-3DS-headroom 4096 KB) */
    { "debug", "tex_fast", "1" },
    { "debug", "tex_keep", "1" },
    { "debug", "tex_keep_floor_kb", "3072" },
    /* audio + save -- identical to kN60Preset, PLUS the two Old 3DS memory-fit levers and the
     * syscore safety clamp (see header comment above) */
    { "debug", "audio_io_unlock", "1" },
    { "debug", "audio_ctx_narrow", "1" },
    { "debug", "audio_simd", "1" },
    { "debug", "audio_rs_align", "1" },
    { "debug", "sram_async", "1" },
    { "debug", "filelog_race_quiet", "1" },
    { "debug", "audio_stream", "1" },
    { "debug", "tex_upload_kb", "512" },
    { "debug", "syscore_pct_safe", "1" },
    /* the native renderer: owned interpreter + C5 retained paths -- identical to kN60Preset */
    { "native", "all", "1" },
    { "native", "interp", "1" },
    { "native", "gpuxf", "1" },
    { "native", "track_retained", "1" },
    { "native", "track_resident", "1" },
    { "native", "track_mattab", "1" },
    { "native", "track_nearclip", "1" },
    { "native", "track_opfold", "1" },
    { "native", "hud_batch", "1" },
    { "native", "hud_direct", "1" },
    { "native", "hud_retain", "1" },
    { "native", "shadow_batch", "1" },
    { "native", "fx_batch", "1" },
    { "native", "fxshd_direct", "1" },
    { "native", "gadget_batch", "1" },
    { "native", "sky_retain", "1" },
    { "native", "machines_retained", "1" },
    { "native", "machines_texskip", "1" },
    { "native", "machines_mattab", "1" },
    { "native", "machines_loadmemo", "1" },
    { "native", "machines_walkfast", "1" },
    { "native", "machines_mattab_live", "1" },
    { "native", "c3d_diet", "1" },
    { "native", "dlmemo", "1" },
    { "native", "deadsetup", "1" },
    { "native", "replay", "0" },
    { "native", "hudsky_emit", "1" },
    /* NRR -- identical to kN60Preset */
    { "native", "nrr", "1" },
    { "native", "nrr_hud", "1" },
    { "native", "nrr_trk", "1" },
    { "native", "nrr_mach", "1" },
    { "native", "nrr_sky", "1" },
    { "native", "nrr_shd", "1" },
    { "native", "nrr_fx", "1" },
    { "native", "nrr_dress", "1" },
    { "native", "nrr_deco", "1" },
    { "native", "nrr_direct", "1" },
    { "native", "nrr_hud_memo", "1" },
    { "native", "nrr_trk_joins", "1" },
    { "native", "nrr_hud_late", "7" },
    { "native", "nrr_trk_merge", "1" },
    { "native", "nrr_draw", "15" },
    { "native", "nrr_hud_kept", "1" },
    { "native", "nrr_hud_rr", "3" },
    { "native", "finish_mesh", "1" },
    { "native", "finish_direct", "0" },
    { "native", "prewarm", "7" },
    { "native", "prewarm_draws", "256" },
    { "native", "zclear_elide", "1" }, /* identical to kN60Preset (o3ds3d: pinned on; 2 = the A/B) */
    { "native", "lcd_vram", "0" },
    /* preset-fix: the hardware cards' levers -- identical to kN60Preset (GDX3DS_PRESET_CARD_ROWS) */
    GDX3DS_PRESET_CARD_ROWS
    /* preset-fix, Old 3DS only: the EK disk / audio streaming levers TEST-OLD3DS-0c2668a pinned.
     * Each equals its compiled-in Old 3DS model default (disk_buffer.cpp, gdx3ds_disk_async.c,
     * gdx3ds_ek_audio_bulk.c); pinned here so the must-ship check guards them and a later change of a
     * call-site default cannot drop them unseen. Inert without a mounted .ndd. */
    { "debug", "disk_stream", "2" },
    { "debug", "audio_disk_async", "1" },
    { "debug", "disk_stream_coalesce_kb", "64" },
    { "debug", "audio_disk_readahead_kb", "64" },
    { "debug", "audio_sync_bulk_kb", "256" },
    /* o3ds60, Old 3DS only: the MP fullview race's one local view takes the NRR categories the 1P
     * race takes (trk sky dress shd fx deco) and the span programs (0x800 with ni_race). hw
     * 158a13e: with ni_view 0 the Old 3DS MP view drew its sky, track and dressing on the
     * interpreter, scene 11-14 ms per drawn frame vs 2.6-4.6 ms for the same categories in a 1P race
     * on the NRR (the New 3DS card has run ni_view 0x97A + ni_race in the same MP race). Render
     * only; split-screen VS and the attract demos keep ni_view. ni_race: the 1P (and, with
     * ni_fullview's 0x800, the MP fullview) race's walk between the NRR markers as span programs,
     * exact by construction (every word checked against the one it was compiled from). */
    { "native", "ni_fullview", "0x97A" },
    { "native", "ni_race", "1" },
    /* o3ds-mp16, Old 3DS only: the o3ds-cpucuts levers the hardware A/B proved (card-3 3c0c7b7, 2D
     * VS 2P + 14 CPU, [cpucut] blocks paired with the [c5cat] window they measured):
     *   - overlap_hud_kick 0: the HUD's mid-frame GPU kick refused. 8 of 9 adjacent on/off block
     *     pairs cost more with the kick: +1.3 +-0.5 ms of render columns per rendered frame (OLS
     *     with race effects), most of it in trk and hud; fbeg (the GPU wait it buys) moved by
     *     +-0.05 ms. A 2D frame's GPU work is small, so the early start bought nothing. 3D MP is
     *     locked at D = 2, where cadence mode never kicks; 1P 3D was estimated -0.6 ms
     *     (agent-D-cpu-cuts.md 3.1a). An ini overlap_hud_kick = 1 (or 2, the A/B) still wins.
     *   - nrr_speck_batch 1: the code default already; pinned so the must-ship check guards it.
     *     5 of 5 block pairs drew the machines cheaper batched (mach -0.30 ms per rendered frame). */
    { "debug", "overlap_hud_kick", "0" },
    { "native", "nrr_speck_batch", "1" },
    /* stereo -- identical to kN60Preset */
    { "native", "stereo_interleave", "1" },
    { "native", "stereo_noreflush", "1" },
    { "stereo", "enabled", "1" },
    /* bottom screen -- identical to kN60Preset */
    { "menu", "race_quiet", "1" },
};

int gdx3ds_preset_n60_name(const char* name) {
    if (name == NULL) {
        return 0;
    }
    return strcasecmp(name, "n60") == 0 || strcasecmp(name, "n3ds60") == 0;
}

int gdx3ds_preset_o3ds_auto_name(const char* name) {
    if (name == NULL) {
        return 0;
    }
    return strcasecmp(name, "o3ds-auto") == 0;
}

const Gdx3dsCfgPresetEntry* gdx3ds_preset_resolve(const char* name, int* count) {
    if (gdx3ds_preset_n60_name(name)) {
        *count = (int)(sizeof(kN60Preset) / sizeof(kN60Preset[0]));
        return kN60Preset;
    }
    if (gdx3ds_preset_o3ds_auto_name(name)) {
        *count = (int)(sizeof(kO3dsAutoPreset) / sizeof(kO3dsAutoPreset[0]));
        return kO3dsAutoPreset;
    }
    *count = 0;
    return NULL;
}
