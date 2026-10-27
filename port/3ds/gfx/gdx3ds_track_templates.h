/* gdx3ds_track_templates.h — [native] track: the course tessellator's index templates.
 *
 * course.c emits no triangles: every chunk draw is `gSPDisplayList` of one of 18 fixed
 * segment-8 index lists (course_track_gfx/D_8014*), each a G_TRI2 run over the RSP vertex
 * cache (slots 0-20), some carrying a mid-list G_SETCOMBINE pair (tunnel interior/exterior
 * material split). The native path replays those lists itself, so it needs them decoded ONCE
 * into flat index arrays. The decoder is plain C with no dependencies so the same code runs
 * on the device over the segment-8 carve loaded from the user's fzerox.o2r and on the host over
 * the same archive's records (port/tests/track_template_tests.c). No index list is compiled in:
 * gGdxTrkTemplate is filled at runtime (gdxtrk_templates_install) and checked against a digest. */
#ifndef GDX3DS_TRACK_TEMPLATES_H
#define GDX3DS_TRACK_TEMPLATES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDXTRK_TEMPLATE_COUNT 18
#define GDXTRK_MAX_TRIS 16
#define GDXTRK_MAX_EVENTS 2
#define GDXTRK_MAX_SLOT 20 /* highest vertex-cache slot any template references */

/* A G_SETCOMBINE inside a template: applies before tri index `beforeTri` (== ntris means
 * after the last tri, i.e. the trailing restore pair). w0/w1 are the raw command words. */
typedef struct GdxTrkCombineEvent {
    uint8_t beforeTri;
    uint32_t w0;
    uint32_t w1;
} GdxTrkCombineEvent;

typedef struct GdxTrkTemplate {
    uint8_t ntris;
    uint8_t nevents;
    uint8_t maxSlot;
    uint8_t idx[GDXTRK_MAX_TRIS * 3];
    GdxTrkCombineEvent events[GDXTRK_MAX_EVENTS];
} GdxTrkTemplate;

/* Decoder result codes. */
enum {
    GDXTRK_DECODE_OK = 0,
    GDXTRK_DECODE_BAD_ARGS = 1,
    GDXTRK_DECODE_NO_ENDDL = 2,
    GDXTRK_DECODE_UNSUPPORTED_OP = 3,
    GDXTRK_DECODE_TOO_MANY_TRIS = 4,
    GDXTRK_DECODE_TOO_MANY_EVENTS = 5,
    GDXTRK_DECODE_BAD_SLOT = 6,
};

/* Decode `len` bytes of 8-byte GBI commands into `out`. Word order is sniffed from the
 * G_ENDDL terminator (host little-endian as bound on the device / stored in the o2r, or
 * N64 big-endian), so a byte-swapped copy decodes identically. Supported opcodes: G_TRI2,
 * G_TRI1, G_RDPPIPESYNC (ignored: no interpreter state), G_SETCOMBINE (event), G_ENDDL. */
int gdxtrk_decode_template(const uint8_t* raw, size_t len, GdxTrkTemplate* out);

/* Byte-wise equality of two decoded templates (only the populated fields). */
int gdxtrk_template_equal(const GdxTrkTemplate* a, const GdxTrkTemplate* b);

/* The decoded templates, all zero (ntris 0) until gdxtrk_templates_install succeeds; the
 * symbol names and byte lengths, indexed identically. */
extern GdxTrkTemplate gGdxTrkTemplate[GDXTRK_TEMPLATE_COUNT];
extern const char* const kGdxTrkTemplateNames[GDXTRK_TEMPLATE_COUNT];
extern const uint16_t kGdxTrkTemplateLen[GDXTRK_TEMPLATE_COUNT];

/* FNV-1a over the 18 decoded templates (counts, indices, SETCOMBINE events) in index order: the
 * value the retail archive's lists decode to. A different archive refuses to arm the native
 * track path, exactly as a mismatch against a compiled-in table did before. */
#define GDXTRK_TEMPLATES_DIGEST 0x3A6C7A31u
uint32_t gdxtrk_templates_digest(const GdxTrkTemplate* t);

/* Decodes src[i] (kGdxTrkTemplateLen[i] bytes each, either word order) for all 18 templates;
 * when every one decodes and the set matches GDXTRK_TEMPLATES_DIGEST, copies them into
 * gGdxTrkTemplate and returns 1. Otherwise returns 0 and leaves gGdxTrkTemplate untouched;
 * failedIndex and failedRc (either may be NULL) name the first template that failed to decode
 * (-1 when they all decoded and only the digest differed). */
int gdxtrk_templates_install(const uint8_t* const src[GDXTRK_TEMPLATE_COUNT], int* failedIndex, int* failedRc);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_TRACK_TEMPLATES_H */
