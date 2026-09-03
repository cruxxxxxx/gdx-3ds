/* port/3ds/os/gdx3ds_config.h -- minimal INI config for the 3DS port (stream B).
 *
 * NOT a frozen contract header (those live in port/3ds/include/); this is stream B's own
 * surface, exported via gdx3ds_os's PUBLIC include dir so main_3ds.cpp and sibling
 * streams can read keys once the orchestrator wires the explicit load call.
 *
 * The ImGui menu is dropped on 3DS (port plan section 0); this file-based config is its
 * MVP replacement. Format: classic INI --
 *
 *     ; sdmc:/3ds/gdiffuser/gdiffuser.ini
 *     [input]
 *     deadzone = 16      ; circle-pad units ignored around center (raw range ~±156)
 *     range = 145        ; circle-pad units for full N64 deflection (±80)
 *     y_maps_to_b = 1    ; 3DS Y acts as N64 B (see mapping table in gdx3ds_os_ctru.c)
 *     [audio]
 *     lle = 0            ; 1 routes audio tasks through the cxd4 RSP LLE (TEST ONLY;
 *                        ; HLE is the 3DS default -- see port/gdx_audio_lle.c)
 *     [perf]
 *     rival_detail = 0   ; 0 NATIVE / 1 REDUCED / 2 MINIMAL — distant RIVAL machines drop
 *                        ; to lower LODs earlier and skip shadows/effects at distance
 *                        ; (decomp-port-rival-detail.patch reads gdx_rival_detail_level;
 *                        ; player machines and, in MINIMAL, the 5 nearest rivals are
 *                        ; never reduced). Live row on the menu DISP tab.
 *     [debug]
 *     console = 0        ; text console on the bottom screen
 *     diag_audio = 0     ; 1 = periodic [audio-out] output-path receipts (~5 s cadence:
 *                        ; chunks submitted/played/nonzero, push totals, sample OR) via
 *                        ; svc + filelog — see port/3ds/audio/gdx3ds_audio_ndsp.c
 *     audio_testtone = 0 ; 1 = replace ALL game audio with a 440 Hz sine at the ndsp
 *                        ; submit point: bisects producer-content vs output-plumbing
 *     fps = 1            ; FPS-HUD (port/3ds/gdx3ds_fps_hud.c): bottom-screen fps
 *                        ; counter, DEFAULT ON; 0 (with console = 0) restores the
 *                        ; untouched black bottom screen
 *     verbose = 0        ; 1 re-enables the recurring svc telemetry ("frame N",
 *                        ; [present], [mem-census], [c3d], [fogdraw], [texmiss]) that
 *                        ; quiet mode drops in normal play; gputrace = 1 implies it.
 *                        ; [watchdog] is never gated (fps is derived from its deltas).
 *
 * '#' and ';' start comments (full-line or trailing), whitespace is trimmed, keys and
 * section names are case-insensitive. Unknown sections/keys are stored, not rejected, so
 * other streams can add keys (e.g. [audio]) without touching this file.
 */
#ifndef GDX3DS_CONFIG_H
#define GDX3DS_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX3DS_CONFIG_DEFAULT_PATH "sdmc:/3ds/gdiffuser/gdiffuser.ini"

/* Parse `path` into the (single, global) key/value table. Returns 0 on success, nonzero
 * if the file is missing/unreadable -- callers keep their compiled-in defaults, which is
 * the normal first-boot experience, not an error to surface. Idempotent: reloading
 * replaces the table. */
int gdx3ds_config_load(const char* path);

/* 1 once the first load attempt has completed (missing file included). One-shot
 * consumers that latch a key forever at first use should poll this and defer the
 * latch until it reads 1, instead of freezing the compiled-in fallback. */
int gdx3ds_config_loaded(void);

/* Lookups never fail: absent key (or unparsable value) returns `fallback`. */
int gdx3ds_config_get_int(const char* section, const char* key, int fallback);
/* Accepts 1/0, true/false, yes/no, on/off (case-insensitive). */
int gdx3ds_config_get_bool(const char* section, const char* key, int fallback);
/* Returned pointer aliases the internal table: valid until the next gdx3ds_config_load. */
const char* gdx3ds_config_get_string(const char* section, const char* key, const char* fallback);

/* MENU write-back: update/add a key in the in-memory table (0 = ok; nonzero = bad args
 * or table full), then gdx3ds_config_save rewrites the INI from the table. Unknown
 * keys/sections survive (the loader stores everything it parses); comments do not. */
int gdx3ds_config_set_string(const char* section, const char* key, const char* value);
int gdx3ds_config_set_int(const char* section, const char* key, int value);
int gdx3ds_config_save(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_CONFIG_H */
