/* port/3ds/os/test/gdx3ds_config_host_tests.c — host-compiled tests for the ini table.
 *
 * gdx3ds_config.c is plain C over stdio/string, so the table logic can be exercised on
 * the build machine. That matters for gdx3ds_config_unset: the hardware console is the
 * only place the net control path runs, and the user is mid-test on it, so this is how
 * the guest-side behaviour of `INI DEL` gets verified without touching the console.
 *
 * Build: cc -O1 -I port/3ds/os port/3ds/os/gdx3ds_config.c <this> -o config_host_tests
 */

#include "gdx3ds_config.h"
#include "gdx3ds_preset_n60.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sFailures = 0;

static void check(int cond, const char* what) {
    printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        sFailures++;
    }
}

static const char* kPath = "gdx3ds_config_host_tests.ini";

static void write_ini(const char* body) {
    FILE* f = fopen(kPath, "w");
    if (f == NULL) {
        printf("FAIL: cannot write %s\n", kPath);
        exit(1);
    }
    fputs(body, f);
    fclose(f);
}

static int file_contains(const char* needle) {
    char buf[4096];
    size_t n;
    FILE* f = fopen(kPath, "r");
    if (f == NULL) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* Whole-file slurp for exact byte-for-byte comparisons (comment/case preservation, and the
 * "nothing dirty -> file untouched" guarantee can't be proven by substring checks alone). */
static size_t slurp(const char* path, char* buf, size_t cap) {
    size_t n;
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return (size_t) -1;
    }
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

int main(void) {
    write_ini("[debug]\ninput_replay = race-mutecity-start\nverbose = 1\n"
              "\n[display]\nborder_mode = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "load the ini");

    /* The bug: setting the empty string leaves the ENTRY in place, so the getter returns
       "" instead of the default and save writes the key back out — the replay stays armed. */
    check(gdx3ds_config_set_string("debug", "input_replay", "") == 0, "set empty string");
    check(strcmp(gdx3ds_config_get_string("debug", "input_replay", "DEFAULT"), "") == 0,
          "an empty value is NOT the default (this is why clearing did not work)");
    check(gdx3ds_config_save(kPath) == 0, "save after the empty set");
    check(file_contains("input_replay"), "the key survives a save when set empty");

    /* The fix. */
    check(gdx3ds_config_unset("debug", "input_replay") == 0, "unset returns 0 when removed");
    check(strcmp(gdx3ds_config_get_string("debug", "input_replay", "DEFAULT"), "DEFAULT") == 0,
          "after unset the getter falls back to the DEFAULT");
    check(gdx3ds_config_save(kPath) == 0, "save after unset");
    check(!file_contains("input_replay"), "the key is gone from the saved ini");

    /* Neighbours, order and idempotence. */
    check(gdx3ds_config_get_int("debug", "verbose", 0) == 1, "a sibling key is untouched");
    check(gdx3ds_config_get_int("display", "border_mode", 0) == 2,
          "another section is untouched");
    check(file_contains("[debug]") && file_contains("[display]"),
          "both sections survive the save");
    check(gdx3ds_config_unset("debug", "input_replay") == 3,
          "unset returns 3 when the key is already absent");
    check(gdx3ds_config_unset("debug", "") == 1, "unset rejects an empty key");
    check(gdx3ds_config_unset("debug", NULL) == 1, "unset rejects a NULL key");

    /* Case folding matches the setter/getter. */
    check(gdx3ds_config_set_string("Debug", "TmemFast", "1") == 0, "set with mixed case");
    check(gdx3ds_config_unset("DEBUG", "tmemfast") == 0, "unset is case-insensitive too");
    check(gdx3ds_config_get_int("debug", "tmemfast", 9) == 9, "and the default returns");

    /* Order is preserved: unsetting the middle key must not reshuffle the rest. */
    write_ini("[debug]\na = 1\nb = 2\nc = 3\n");
    check(gdx3ds_config_load(kPath) == 0, "reload for the ordering check");
    check(gdx3ds_config_unset("debug", "b") == 0, "remove the middle key");
    check(gdx3ds_config_get_int("debug", "a", 0) == 1 &&
          gdx3ds_config_get_int("debug", "c", 0) == 3,
          "the keys either side keep their values");
    check(gdx3ds_config_save(kPath) == 0, "save after the middle removal");
    check(file_contains("a = 1") && file_contains("c = 3") && !file_contains("b = 2"),
          "the saved ini keeps a and c and drops b");

    /* n3ds: the preset defaults layer. No resolver = exactly the old lookups. */
    write_ini("[debug]\nrenderthread = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "preset: load without a preset key");
    check(gdx3ds_config_get_int("native", "nrr", 0) == 0, "preset: no resolver, nrr reads the fallback");
    gdx3ds_config_set_preset_resolver(&gdx3ds_preset_resolve);
    check(gdx3ds_config_preset_count() == 0, "preset: resolver set, no [perf] preset key = no table");
    check(gdx3ds_config_get_int("native", "nrr", 0) == 0, "preset: no table, nrr reads the fallback");
    write_ini("[perf]\npreset = off\n[debug]\nrenderthread = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "preset: load preset=off");
    check(gdx3ds_config_preset_count() == 0 && gdx3ds_config_get_int("native", "nrr", 0) == 0,
          "preset: off selects no table");
    write_ini("[perf]\npreset = N60\n[debug]\nrenderthread = 2\n[native]\nnrr_direct = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "preset: load preset=N60");
    check(gdx3ds_config_preset_count() > 50, "preset: n60 table active (case-insensitive name)");
    check(gdx3ds_config_get_int("native", "nrr", 0) == 1, "preset: nrr defaults to 1");
    check(gdx3ds_config_get_int("native", "nrr_draw", 0) == 15, "preset: nrr_draw defaults to 15");
    check(strcmp(gdx3ds_config_get_string("debug", "frameskip", "0"), "race0") == 0,
          "preset: frameskip defaults to race0");
    check(gdx3ds_config_get_bool("stereo", "enabled", 0) == 1, "preset: get_bool reads the preset");
    check(gdx3ds_config_get_int("debug", "renderthread", 1) == 2, "preset: an explicit ini key wins");
    check(gdx3ds_config_get_int("native", "nrr_direct", 0) == 2, "preset: an explicit ini key wins (native)");
    check(gdx3ds_config_get_int("native", "not_a_preset_key", 7) == 7, "preset: unknown keys read the fallback");
    check(gdx3ds_config_entry_count() == 3, "preset: preset values are not table entries");
    check(gdx3ds_config_override("native", "nrr", "0") == 0 && gdx3ds_config_get_int("native", "nrr", 1) == 0,
          "preset: an override wins");
    check(gdx3ds_config_set_string("perf", "preset", "off") == 0 && gdx3ds_config_preset_count() == 0 &&
              gdx3ds_config_get_int("native", "nrr_draw", 0) == 0,
          "preset: switching the key off at run time drops the table");
    check(gdx3ds_config_set_string("perf", "preset", "n3ds60") == 0 && gdx3ds_config_preset_count() > 50,
          "preset: alias n3ds60");
    check(gdx3ds_config_save(kPath) == 0 && !file_contains("nrr_draw") && file_contains("preset = n3ds60"),
          "preset: save writes the ini table only");
    gdx3ds_config_set_preset_resolver(NULL);
    check(gdx3ds_config_preset_count() == 0 && gdx3ds_config_get_int("native", "nrr_draw", 0) == 0,
          "preset: clearing the resolver drops the layer");

    /* n60lead: AUTO. A no-ini (or [perf] preset = auto) player must resolve the SAME tuned
     * stack main_3ds.cpp's own model probe would select, not no table at all. */
    gdx3ds_config_set_preset_resolver(&gdx3ds_preset_resolve);
    write_ini("[debug]\nrenderthread = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "auto: load with no [perf] preset key");
    check(gdx3ds_config_preset_count() == 0, "auto: no auto name set yet = no table (today's old behaviour)");
    gdx3ds_config_set_auto_preset_name("n60");
    check(gdx3ds_config_preset_count() > 50, "auto: new3ds auto name applies the n60 table");
    /* native.nrr is permanently overridden to 0 above (line ~136, a RAM-only override that
     * gdx3ds_config_load never clears) -- native.interp is untouched by any earlier test. */
    check(gdx3ds_config_get_int("native", "interp", 0) == 1, "auto: new3ds auto gets interp");
    check(gdx3ds_config_get_int("debug", "tex_keep_kb", 0) == 4096, "auto: new3ds auto gets the n60 headroom");
    check(strcmp(gdx3ds_config_preset_name(), "n60") == 0, "auto: preset_name reports n60");
    check(gdx3ds_config_get_int("native", "ni_fullview", 7) == 7, "auto: new3ds auto leaves ni_fullview out (o3ds60)");
    check(strcmp(gdx3ds_config_get_string("perf", "preset", "?"), "?") == 0,
          "auto: get_string on perf.preset itself is untouched by the auto substitution");

    gdx3ds_config_set_auto_preset_name("o3ds-auto");
    check(gdx3ds_config_preset_count() > 50, "auto: old3ds auto name applies the o3ds-auto table");
    check(gdx3ds_config_get_int("native", "interp", 0) == 1, "auto: old3ds auto also gets interp (n60's own table)");
    check(gdx3ds_config_get_int("debug", "audio_stream", 0) == 1, "auto: old3ds auto adds audio_stream");
    check(gdx3ds_config_get_int("debug", "tex_upload_kb", 0) == 512, "auto: old3ds auto adds tex_upload_kb");
    check(gdx3ds_config_get_bool("debug", "syscore_pct_safe", 0) == 1, "auto: old3ds auto adds syscore_pct_safe");
    /* tex_keep_kb's real fallback (port/3ds/gfx/n64/gdx_rt_glue.cpp) is 2048 -- the o3ds-auto
     * table must leave the key out entirely so that fallback is what a caller sees, not n60's
     * New-3DS 4096 headroom. Passing that same fallback here, not 0, is the point of the check. */
    check(gdx3ds_config_get_int("debug", "tex_keep_kb", 2048) == 2048,
          "auto: old3ds auto does NOT carry n60's New-3DS tex_keep_kb headroom (fallback 2048)");
    check(strcmp(gdx3ds_config_preset_name(), "o3ds-auto") == 0, "auto: preset_name reports o3ds-auto");
    check(gdx3ds_config_get_int("native", "ni_fullview", 0) == 0x97A,
          "auto: old3ds auto puts the MP fullview view on the NRR (o3ds60 ni_fullview 0x97A)");

    /* preset-fix: the ni_* levers are rows of both tables (gdx3ds_preset_mustship_tests.c holds the
     * full per-model must-ship list); ni_view is New 3DS only, ni_fullview Old 3DS only. */
    {
        static const char* const kModels[2] = { "o3ds-auto", "n60" };
        int m;
        for (m = 0; m < 2; m++) {
            gdx3ds_config_set_auto_preset_name(kModels[m]);
            check(gdx3ds_config_get_int("native", "ni_course", 0) == 1 &&
                      gdx3ds_config_get_int("native", "ni_mach", 0) == 3 &&
                      gdx3ds_config_get_int("native", "ni_menu", 0) == 1 &&
                      gdx3ds_config_get_int("native", "ni_race", 0) == 1 &&
                      gdx3ds_config_get_int("native", "ni_menu_root", 0) == 1,
                  "ni preset: every ni_* lever defaults on (both models)");
            check(gdx3ds_config_get_int("native", "ni_view", 7) == (m == 0 ? 7 : 0x97A),
                  "ni preset: ni_view 0x97A on the New 3DS only");
        }
        gdx3ds_config_set_auto_preset_name("o3ds-auto");
    }

    write_ini("[perf]\npreset = auto\n[debug]\nrenderthread = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "auto: load with an explicit literal \"auto\"");
    check(gdx3ds_config_preset_count() > 50 && gdx3ds_config_get_int("debug", "audio_stream", 0) == 1,
          "auto: a literal \"auto\" value is treated the same as an absent key");

    write_ini("[perf]\npreset = off\n[debug]\nrenderthread = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "auto: load with an explicit non-auto preset key");
    check(gdx3ds_config_preset_count() == 0, "auto: an explicit preset=off is never overridden by the auto name");
    check(gdx3ds_config_preset_name() == NULL, "auto: preset_name is NULL when no table applied");

    write_ini("[perf]\npreset = n60\n[debug]\nrenderthread = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "auto: load with an explicit n60 (auto name still o3ds-auto)");
    check(gdx3ds_config_get_int("debug", "audio_stream", 0) == 0,
          "auto: an explicit preset=n60 is never overridden by the (old3ds) auto name");

    gdx3ds_config_set_auto_preset_name(NULL);
    write_ini("[debug]\nrenderthread = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "auto: load with no preset key, auto name cleared");
    check(gdx3ds_config_preset_count() == 0, "auto: clearing the auto name restores no-table auto");
    gdx3ds_config_set_preset_resolver(NULL);


    /* n60lead: capacity 256 and LOUD overflow (no silent drop). */
    {
        FILE* fp = fopen(kPath, "w");
        int i;
        const char* first = NULL;
        check(fp != NULL, "capacity: temp ini opens");
        if (fp != NULL) {
            fprintf(fp, "[debug]\n");
            for (i = 0; i < 200; i++) {
                fprintf(fp, "k%03d = %d\n", i, i);
            }
            fprintf(fp, "[variant.z]\n");
            for (i = 200; i < 300; i++) {
                fprintf(fp, "k%03d = %d\n", i, i);
            }
            fclose(fp);
        }
        gdx3ds_config_set_preset_resolver(NULL);
        check(gdx3ds_config_load(kPath) == 0, "capacity: 300-key ini loads");
        check(gdx3ds_config_entry_capacity() == 256, "capacity: table holds 256");
        check(gdx3ds_config_entry_count() == 256, "capacity: 256 entries kept");
        check(gdx3ds_config_get_int("debug", "k199", -1) == 199, "capacity: key 200 of 300 kept (old cap 128 dropped it)");
        check(gdx3ds_config_get_int("variant.z", "k255", -1) == 255, "capacity: key 256 kept");
        check(gdx3ds_config_overflow(&first) == 44, "capacity: the 44 keys past 256 are COUNTED");
        check(first != NULL && strcmp(first, "variant.z.k256") == 0, "capacity: the first dropped key is named");
        check(gdx3ds_config_set_int("debug", "brand_new", 1) == 2 && gdx3ds_config_overflow(NULL) == 45,
              "capacity: a set into a full table is counted too");
        fp = fopen(kPath, "w");
        if (fp != NULL) {
            fprintf(fp, "[debug]\nx = 1\n");
            fclose(fp);
        }
        check(gdx3ds_config_load(kPath) == 0 && gdx3ds_config_overflow(NULL) == 0, "capacity: a reload clears the count");
        {
            int before = gdx3ds_config_overflow(NULL);
            int refused = 0;
            for (i = 0; i < 60; i++) {
                char k[16];
                snprintf(k, sizeof(k), "o%02d", i);
                refused += gdx3ds_config_override("debug", k, "1") == 2 ? 1 : 0;
            }
            check(refused > 0 && refused <= 60 - 48 + 24 && gdx3ds_config_overflow(NULL) == before + refused,
                  "capacity: override table >= 48, every refused override is counted");
        }
    }

    /* --- hw bug n60-3d8, item 2: a legitimate setting save must patch ONLY the changed
     * key's line, preserving every comment, the original section-name case, and every
     * unrelated key -- and must never let [hud] reach this file, new or pre-existing. ---- */
    {
        static const char* kFixture =
            "; G-Diffuser Old 3DS -- this header must survive a save untouched.\n"
            "; second comment line, also untouched.\n"
            "\n"
            "[debug]\n"
            "gputl = 0 ; trailing note\n"
            "verbose = 0\n"
            "\n"
            "[variant.V]\n"
            "native.lcd_vram = 1\n"
            "\n"
            "[hud]\n"
            "placement = 1\n";
        char before[4096];
        char after[4096];

        write_ini(kFixture);
        check(gdx3ds_config_load(kPath) == 0, "n60-3d8#2: load the realistic fixture");

        /* a) nothing dirty -> save must not touch the file AT ALL. */
        check(slurp(kPath, before, sizeof(before)) != (size_t) -1, "n60-3d8#2: slurp before");
        check(gdx3ds_config_save(kPath) == 0, "n60-3d8#2: save with nothing dirty returns 0");
        check(slurp(kPath, after, sizeof(after)) != (size_t) -1, "n60-3d8#2: slurp after no-op save");
        check(strcmp(before, after) == 0,
              "n60-3d8#2: a save with no pending set/unset leaves the file byte-identical");

        /* b) a real, legitimate change (one DISP-tab-style key) touches ONLY that line. */
        check(gdx3ds_config_set_int("debug", "verbose", 1) == 0, "n60-3d8#2: set debug.verbose=1");
        check(gdx3ds_config_save(kPath) == 0, "n60-3d8#2: save after one legitimate set");
        check(file_contains("; G-Diffuser Old 3DS -- this header must survive a save untouched."),
              "n60-3d8#2: the top-of-file comment survives");
        check(file_contains("; second comment line, also untouched."),
              "n60-3d8#2: the second comment line survives");
        check(file_contains("[variant.V]"), "n60-3d8#2: [variant.V]'s ORIGINAL case survives");
        check(file_contains("native.lcd_vram = 1"), "n60-3d8#2: an untouched key in another section survives");
        check(file_contains("verbose = 1"), "n60-3d8#2: the changed key took its new value");
        check(!file_contains("verbose = 0"), "n60-3d8#2: the changed key's old value is gone");

        /* c) [hud] must never appear in the saved file again, even though it was in the
         * fixture on disk -- a real save drops any pre-existing [hud] block entirely. */
        check(!file_contains("[hud]"), "n60-3d8#2: a pre-existing [hud] section is dropped on save");
        check(!file_contains("placement"), "n60-3d8#2: [hud] placement does not survive as a stray key");

        /* d) setting [hud] placement (as gdx3ds_bottomhud.c's cycle_mode does, to keep the
         * shared table consistent for readers) must NOT make it into a later save either,
         * even though that save is for a genuinely unrelated, legitimate key. */
        check(gdx3ds_config_set_int("hud", "placement", 2) == 0, "n60-3d8#2: set hud.placement=2 (RAM)");
        check(gdx3ds_config_set_int("display", "border_mode", 3) == 0, "n60-3d8#2: a real DISP-tab change");
        check(gdx3ds_config_save(kPath) == 0, "n60-3d8#2: save with both a hud key and a real key dirty");
        check(!file_contains("[hud]") && !file_contains("placement"),
              "n60-3d8#2: [hud] still never reaches gdiffuser.ini, even mixed with a real save");
        check(file_contains("border_mode = 3"), "n60-3d8#2: the real key in that same save DID take effect");

        /* e) a hud-only dirty set, with NO other legitimate change, must not touch the file
         * at all (the exact shape of the n60-3d8 hardware bug: cycling the HUD mode, then
         * something else saving, must never be how [hud] reaches gdiffuser.ini). */
        check(slurp(kPath, before, sizeof(before)) != (size_t) -1, "n60-3d8#2: slurp before hud-only set");
        check(gdx3ds_config_set_int("hud", "placement", 0) == 0, "n60-3d8#2: set hud.placement=0 (RAM only)");
        check(gdx3ds_config_save(kPath) == 0, "n60-3d8#2: save with ONLY a hud key dirty returns 0");
        check(slurp(kPath, after, sizeof(after)) != (size_t) -1, "n60-3d8#2: slurp after hud-only save");
        check(strcmp(before, after) == 0,
              "n60-3d8#2: a hud-only dirty set never rewrites the file, byte-identical");

        /* f) a brand new key in a brand new section still gets appended (the loader's own
         * documented "unknown sections/keys are stored, not rejected" contract, unchanged). */
        check(gdx3ds_config_set_int("newsection", "newkey", 7) == 0, "n60-3d8#2: set a brand new key");
        check(gdx3ds_config_save(kPath) == 0, "n60-3d8#2: save with a brand new section/key");
        check(file_contains("[newsection]") && file_contains("newkey = 7"),
              "n60-3d8#2: a brand new section/key is appended");
        check(file_contains("[variant.V]"), "n60-3d8#2: appending a new section still preserves old case");
    }

    remove(kPath);
    if (sFailures == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", sFailures);
    return 1;
}
