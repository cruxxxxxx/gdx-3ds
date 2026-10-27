/* port/3ds/os/test/gdx3ds_preset_mustship_tests.c — the shipped presets' MUST-SHIP lever defaults.
 *
 * preset-fix: card-2 (462709d) switched its hardware-test inis to "shipped defaults, minimal test
 * knobs" and dropped the [variant.new] block the earlier cards (TEST-*-0c2668a) carried. Those keys
 * were NOT built-in preset defaults, so the shipped build ran without the native menus / course /
 * machine select programs and the Old 3DS ran 2-3x slower. This test pins the contract: with an
 * EMPTY ini, the per-model auto preset (main_3ds.cpp: "o3ds-auto" on an Old 3DS, "n60" on a New
 * 3DS) must resolve every lever below to the value the known-good hardware cards ran, and must NOT
 * carry the keys that are test-only or a user's choice.
 *
 * Adding a lever to the cards that a player must get too? Add it to the preset AND to the list here.
 * Removing one from a preset fails CI on purpose.
 *
 * Build: tools/ci-3ds.sh (gdx3ds_preset_mustship_tests).
 */

#include "gdx3ds_config.h"
#include "gdx3ds_preset_n60.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char* section;
    const char* key;
    const char* value;
} MustShip;

/* Both models: the hardware cards' native renderer + Expansion Kit + 3D overlap levers, plus a few
 * anchors of the existing native stack (a table that lost them would be just as slow). */
static const MustShip kBoth[] = {
    { "native", "all", "1" },
    { "native", "interp", "1" },
    { "native", "nrr", "1" },
    { "native", "stereo_interleave", "1" },
    { "native", "stereo_noreflush", "1" },
    { "debug", "renderthread", "0" },
    { "native", "ni_menu", "1" },
    { "native", "ni_menu_root", "1" },
    { "native", "ni_menu_split", "3" },
    { "native", "ni_menu_keep", "1" },
    { "native", "ni_menu_ucode", "1" },
    { "native", "ni_menu_xition", "1" },
    { "native", "ni_course", "1" },
    { "native", "ni_mach", "3" },
    { "native", "ni_race", "1" },
    { "native", "atlas_race_reset", "1" },
    { "native", "nrr_testdrive", "1" },
    { "native", "ek_editmesh", "1" },
    { "native", "ek_editmesh_track", "1" },
    { "native", "ek_nimach", "1" },
    { "native", "stereo_fold", "1" },
    { "stereo", "head_kick", "1" },
    { "stereo", "resume_fold", "1" },
    { "stereo", "onesubmit", "1" },        /* ship6: hw card-6 A/B (onesubmit-progress.md) */
    { "stereo", "onesubmit_narrow", "1" },
    { "stereo", "onesubmit_kicks", "0" },
    { "stereo", "onesubmit_ab", "0" },
    { "stereo", "enabled", "1" },
    { "ui", "bottom_menus", "1" },
};

/* Old 3DS only (TEST-OLD3DS-0c2668a). */
static const MustShip kOld[] = {
    { "native", "ni_fullview", "0x97A" },
    { "debug", "disk_stream", "2" },
    { "debug", "audio_disk_async", "1" },
    { "debug", "disk_stream_coalesce_kb", "64" },
    { "debug", "audio_disk_readahead_kb", "64" },
    { "debug", "audio_sync_bulk_kb", "256" },
    { "debug", "audio_stream", "1" },
    { "debug", "tex_upload_kb", "512" },
    { "debug", "syscore_pct_safe", "1" },
    /* o3ds-mp16: the o3ds-cpucuts levers card-3's hardware A/B proved (gdx3ds_preset_n60.c). */
    { "debug", "overlap_hud_kick", "0" },
    { "native", "nrr_speck_batch", "1" },
};

/* New 3DS only (TEST-NEW3DS-0c2668a). */
static const MustShip kNew[] = {
    { "native", "ni_view", "0x97A" },
    { "stereo", "menu_depth", "1" },
    { "debug", "tex_keep_kb", "4096" },
};

/* Never in either table: diagnostics, A/B experiments and the player's own choices. */
static const MustShip kNever[] = {
    { "debug", "audioload", NULL },     /* [audioload] census, diagnostic only */
    { "debug", "ndsp_gain_pct", NULL }, /* o3dscrackle headroom A/B: halves the volume */
    { "debug", "ndsp_clip", NULL },     /* o3dscrackle clipper A/B */
    { "display", "widescreen", NULL },  /* the player's choice; documented experimental, default off */
    { "ek", "course_share", NULL },     /* EK course sharing: a feature still on its hardware card */
    { "debug", "launch_variants", NULL },
    { "debug", "mainprof", NULL },
    { "debug", "hitch", NULL },
};

/* Old 3DS: ni_view never ran on an Old 3DS card (its MP view takes ni_fullview instead); New 3DS:
 * the Old 3DS memory-fit levers and the MP-only fullview row. */
static const MustShip kNeverOld[] = {
    { "native", "ni_view", NULL },
    { "stereo", "menu_depth", NULL },
    { "debug", "tex_keep_kb", NULL },
};

static const MustShip kNeverNew[] = {
    { "debug", "overlap_hud_kick", NULL }, /* o3ds-mp16: measured on the Old 3DS only */
    { "native", "ni_fullview", NULL },
    { "debug", "audio_disk_async", NULL },
    { "debug", "disk_stream_coalesce_kb", NULL },
    { "debug", "audio_disk_readahead_kb", NULL },
    { "debug", "audio_sync_bulk_kb", NULL },
    { "debug", "audio_stream", NULL },
};

static int sFailures = 0;
static int sChecks = 0;

static void Check(int cond, const char* model, const MustShip* row, const char* got) {
    sChecks++;
    if (!cond) {
        sFailures++;
        printf("FAIL: %s [%s] %s = %s, expected %s\n", model, row->section, row->key, got != NULL ? got : "(absent)",
               row->value != NULL ? row->value : "(absent)");
    }
}

static void ExpectRows(const char* model, const MustShip* rows, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        const char* got = gdx3ds_config_get_string(rows[i].section, rows[i].key, NULL);
        if (rows[i].value == NULL) {
            Check(got == NULL, model, &rows[i], got);
        } else {
            Check(got != NULL && strcmp(got, rows[i].value) == 0, model, &rows[i], got);
        }
    }
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void CheckModel(const char* autoName) {
    const int old = strcmp(autoName, "o3ds-auto") == 0;
    gdx3ds_config_set_auto_preset_name(autoName);
    if (gdx3ds_config_preset_count() <= 50) {
        sFailures++;
        printf("FAIL: %s: the auto preset resolved %d rows\n", autoName, gdx3ds_config_preset_count());
        return;
    }
    ExpectRows(autoName, kBoth, COUNT(kBoth));
    ExpectRows(autoName, kNever, COUNT(kNever));
    if (old) {
        ExpectRows(autoName, kOld, COUNT(kOld));
        ExpectRows(autoName, kNeverOld, COUNT(kNeverOld));
    } else {
        ExpectRows(autoName, kNew, COUNT(kNew));
        ExpectRows(autoName, kNeverNew, COUNT(kNeverNew));
    }
}

int main(void) {
    /* An EMPTY ini: the file is absent (first boot), exactly the no-ini player. */
    remove("gdx3ds_preset_mustship_tests.ini");
    gdx3ds_config_set_preset_resolver(&gdx3ds_preset_resolve);
    if (gdx3ds_config_load("gdx3ds_preset_mustship_tests.ini") != 1 || gdx3ds_config_entry_count() != 0) {
        printf("FAIL: the empty-ini load is not empty\n");
        return 1;
    }
    CheckModel("o3ds-auto");
    CheckModel("n60");
    /* An explicit `[perf] preset = n60` on any console is the same New 3DS table. */
    gdx3ds_config_set_auto_preset_name(NULL);
    if (gdx3ds_config_preset_count() != 0) {
        sFailures++;
        printf("FAIL: no auto name and no [perf] preset still resolved a table\n");
    }
    if (sFailures != 0) {
        printf("FAIL: %d of %d must-ship checks failed\n", sFailures, sChecks);
        return 1;
    }
    printf("PASS: %d must-ship checks (o3ds-auto + n60, empty ini)\n", sChecks);
    return 0;
}
