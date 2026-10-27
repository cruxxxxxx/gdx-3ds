/* port/3ds/gdx3ds_hudbake.h -- the bottom-screen DASH's game art, baked ONCE at boot from the
 * user's own fzerox.o2r (no game pixels are compiled into the binary).
 *
 * What used to be two generated headers full of pixels (gdx3ds_bottomhud_glyphs.h, from
 * tools/gen_nibottom_bottomhud_glyphs.py, and gdx3ds_bottomhud_assets.h, from the DASH template
 * export) is now a RECIPE run against the archive the game already mounts:
 *
 *   - glyph atlases (Font1/Font2, timer/lap/position/speed digits, ordinal suffixes, km/h, the
 *     30 character portraits, the KO star, the spare-machine icon): each table entry names its
 *     o2r texture and the cell it is sliced from;
 *   - the DASH chrome widgets (background, header, standings, lap, timer, gap, energy_bar): the
 *     same drawing steps tools/gen_nibottom_template.py ran to make the template layers
 *     (recolour, tile, nearest-neighbour scale, alpha-composite, rectangles, glyph text), done in
 *     integer/double arithmetic that is bit-exact with the Pillow calls that script made, then
 *     quantised to RGB565 / RGBA5551 the way tools/nibottom_template_import.py did.
 *
 * The tables below keep the shape the painters already read (DashGlyph, DashAssetWidget); only
 * their pixels moved from .rodata to ONE block this module allocates on the first bake or cache
 * load (through gdx3ds_hudbake_set_allocator: the console puts it in the linear heap, off the
 * regular heap the game runs short of). Until then every glyph reads as zero (alpha 0: draws
 * nothing) and gDashAssetBackground / every widget's pixels are NULL (the painters skip them). A texture it cannot load (missing or wrong
 * archive) leaves its glyphs blank and its part of a widget undrawn; the procedural parts (panel
 * fills, frames, the backdrop's base colour) still bake, and nothing crashes.
 *
 * PURE C, no <3ds.h>: the host tests compile it as is, with their own archive reader. */
#ifndef GDX3DS_HUDBAKE_H
#define GDX3DS_HUDBAKE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One glyph: RGBA5551 words (r5 g5 b5 a1, a = LSB, host order). */
typedef struct {
    const char* key;
    unsigned char w, h;
    unsigned char adv; /* proportional advance width (Font1/Font2); w for the rest */
    const unsigned short* pixels;
} DashGlyph;

extern DashGlyph kDashGlyphTimer[];
extern DashGlyph kDashGlyphLap[];
extern DashGlyph kDashGlyphPosDigit[];
extern DashGlyph kDashGlyphPosSuffix[];
extern DashGlyph kDashGlyphFont1[];
extern DashGlyph kDashGlyphFont2[];
extern DashGlyph kDashGlyphPortrait[];
extern DashGlyph kDashGlyphKoStar[];
extern DashGlyph kDashGlyphLives[];
extern DashGlyph kDashGlyphSpeedDigit[];
extern DashGlyph kDashGlyphKmh[];

#define KDASHGLYPHTIMER_COUNT 14
#define KDASHGLYPHLAP_COUNT 6
#define KDASHGLYPHPOSDIGIT_COUNT 10
#define KDASHGLYPHPOSSUFFIX_COUNT 4
#define KDASHGLYPHFONT1_COUNT 38
#define KDASHGLYPHFONT2_COUNT 38
#define KDASHGLYPHPORTRAIT_COUNT 30
#define KDASHGLYPHKOSTAR_COUNT 1
#define KDASHGLYPHLIVES_COUNT 1
#define KDASHGLYPHSPEEDDIGIT_COUNT 10
#define KDASHGLYPHKMH_COUNT 1

/* The template's space advances for Font1/Font2 text (tools/gen_nibottom_template.py). */
#define DASH_FONT1_SPACE 7
#define DASH_FONT2_SPACE 3

/* The DASH backdrop: 320x240 opaque RGB565, row-major. NULL until the first bake/cache load. */
#define DASH_ASSET_BACKGROUND_W 320
#define DASH_ASSET_BACKGROUND_H 240
extern unsigned short* gDashAssetBackground;

/* id -> {pixels, x, y, w, h, hasAlpha} for the compositor loop. RGBA5551, alpha 0 = skip;
 * pixels NULL until the first bake/cache load. */
typedef struct DashAssetWidget {
    const char* id;
    const unsigned short* pixels;
    int x, y, w, h;
    int hasAlpha;
} DashAssetWidget;

extern DashAssetWidget kDashAssetWidgets[];
#define DASH_ASSET_WIDGET_COUNT 6

/* Reads one archive record (the whole file: 0x40-byte resource header, then the payload) into
 * `buf`, at most `cap` bytes. Returns the bytes copied, 0 on a miss. */
typedef size_t (*GdxHudBakeReadFn)(const char* key, void* buf, size_t cap, void* user);
typedef void (*GdxHudBakeLogFn)(const char* line, void* user);

typedef struct GdxHudBakeStats {
    int textures;      /* records read */
    int missing;       /* records missing, unreadable, or not the expected format/size */
    unsigned long bytesRead;
} GdxHudBakeStats;

/* Where the arena and every transient buffer of a bake come from (default malloc/free). Set it
 * before the first bake or cache load; once the arena exists it is ignored. */
typedef void* (*GdxHudBakeAllocFn)(size_t bytes);
typedef void (*GdxHudBakeFreeFn)(void* p);
void gdx3ds_hudbake_set_allocator(GdxHudBakeAllocFn alloc, GdxHudBakeFreeFn release);

/* The arena's size (every baked pixel, one block), and 1 once it is allocated. */
size_t gdx3ds_hudbake_arena_bytes(void);
int gdx3ds_hudbake_ready(void);

/* Every archive key a bake reads, in the order it reads them (the console prefetches them in
 * archive order). Writes min(count, cap) keys when keys != NULL; returns the count. */
#define GDX3DS_HUDBAKE_KEY_LEN 64
int gdx3ds_hudbake_list_keys(char (*keys)[GDX3DS_HUDBAKE_KEY_LEN], int cap);

/* The i-th key the last bake read (NULL past the end): the tests hold list_keys to it. */
const char* gdx3ds_hudbake_read_key(int i);

/* Runs every recipe once. Safe to call again (it re-bakes). Returns stats.missing (0 = every
 * element baked; -1 = no memory for the arena, nothing baked). `log` may be NULL. */
int gdx3ds_hudbake_run(GdxHudBakeReadFn read, void* readUser, GdxHudBakeLogFn log, void* logUser,
                       GdxHudBakeStats* stats);

/* 1 once a bake has run (whatever it found), else 0. */
int gdx3ds_hudbake_done(void);

/* FNV-1a over every baked pixel (glyphs in table order, backdrop, widgets): one number to compare
 * a console's bake with the host's ([debug] hudbake_digest=1 logs it). */
#define GDX3DS_HUDBAKE_DIGEST_RETAIL 0x31516EB9u
unsigned int gdx3ds_hudbake_digest(void);

/* ---- optional SD cache ----------------------------------------------------------------------
 *
 * A bake costs a few hundred archive-record reads' worth of time on an Old 3DS, so the result is
 * kept on the SD card and reused while it still matches. The cache is keyed by CONTENT: it lists
 * the archive keys the bake read, and is used only if every one of them still has the same
 * CRC-32 and size in the archives mounted now (the zip central directory already in RAM: no
 * record is read to check), it was written by this very build of the recipes, and its pixels
 * pass a checksum. Anything else is a miss: the caller bakes and writes a fresh one.
 *
 * Fingerprint of `key` across the mounted archives (gdx3ds_zipshim_entry_fingerprint on the
 * console); returns how many archives hold it (0: missing). */
typedef int (*GdxHudBakeFingerprintFn)(const char* key, unsigned int* fingerprint, void* user);

/* One archive's contribution to a key's fingerprint: FNV-1a over the entry's CRC-32 (4 bytes LE)
 * and uncompressed size (8 bytes LE). A key's fingerprint is the SUM of this over every distinct
 * mounted archive holding it. Shared by the console (gdx3ds_zipshim_entry_fingerprint) and the
 * PC pack tool (tools/hudpack) so their caches agree byte for byte. */
static inline unsigned int gdx3ds_hudbake_fingerprint_entry(unsigned int crc32, unsigned long long size) {
    unsigned int h = 2166136261u;
    int b;
    for (b = 0; b < 4; b++) {
        h = (h ^ ((crc32 >> (8 * b)) & 0xFFu)) * 16777619u;
    }
    for (b = 0; b < 8; b++) {
        h = (h ^ (unsigned int) ((size >> (8 * b)) & 0xFFu)) * 16777619u;
    }
    return h;
}

/* Serialises the last bake into dst (cap bytes). dst == NULL: returns the size needed. Returns 0
 * when the last bake is not cacheable (it missed a record, or did not run from an archive). */
size_t gdx3ds_hudbake_cache_write(void* dst, size_t cap, GdxHudBakeFingerprintFn fp, void* user);

/* Loads a cache written by gdx3ds_hudbake_cache_write (or by the PC: tools/hudpack writes the
 * same file). 1: every buffer restored (as if baked), decoded straight into the arena; 0: stale
 * (logged with the reason, the buffers untouched) or damaged (logged, the buffers cleared): the
 * caller bakes next either way. */
unsigned int gdx3ds_hudbake_recipe_id(void); /* the recipeTag this build writes and accepts */
int gdx3ds_hudbake_cache_read(const void* src, size_t n, GdxHudBakeFingerprintFn fp, void* user,
                              GdxHudBakeLogFn log, void* logUser);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_HUDBAKE_H */
