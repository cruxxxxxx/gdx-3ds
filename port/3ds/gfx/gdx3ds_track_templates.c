/* gdx3ds_track_templates.c — see the header. Host-compilable (no platform includes). */
#include "gdx3ds_track_templates.h"

#include <string.h>

GdxTrkTemplate gGdxTrkTemplate[GDXTRK_TEMPLATE_COUNT];

/* Byte length of each index list (course_track_gfx/D_8014*: 9-12 GBI commands). */
const uint16_t kGdxTrkTemplateLen[GDXTRK_TEMPLATE_COUNT] = {
    72, 72, 72, 72, 88, 88, 72, 72, 72, 72, 80, 80, 80, 80, 96, 96, 96, 96,
};

const char* const kGdxTrkTemplateNames[GDXTRK_TEMPLATE_COUNT] = {
    "D_80140F0", "D_8014138", "D_8014180", "D_80141C8", "D_8014210", "D_8014268",
    "D_80142C0", "D_8014308", "D_8014350", "D_8014398", "D_80143E0", "D_8014430",
    "D_8014480", "D_80144D0", "D_8014520", "D_8014580", "D_80145E0", "D_8014640",
};

static uint32_t GdxTrkReadWord(const uint8_t* p, int bigEndian) {
    if (bigEndian) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static int GdxTrkPushTri(GdxTrkTemplate* out, uint32_t a, uint32_t b, uint32_t c) {
    if (out->ntris >= GDXTRK_MAX_TRIS) {
        return GDXTRK_DECODE_TOO_MANY_TRIS;
    }
    if (a > GDXTRK_MAX_SLOT || b > GDXTRK_MAX_SLOT || c > GDXTRK_MAX_SLOT) {
        return GDXTRK_DECODE_BAD_SLOT;
    }
    out->idx[out->ntris * 3 + 0] = (uint8_t)a;
    out->idx[out->ntris * 3 + 1] = (uint8_t)b;
    out->idx[out->ntris * 3 + 2] = (uint8_t)c;
    if (a > out->maxSlot) { out->maxSlot = (uint8_t)a; }
    if (b > out->maxSlot) { out->maxSlot = (uint8_t)b; }
    if (c > out->maxSlot) { out->maxSlot = (uint8_t)c; }
    out->ntris++;
    return GDXTRK_DECODE_OK;
}

int gdxtrk_decode_template(const uint8_t* raw, size_t len, GdxTrkTemplate* out) {
    size_t i;
    int bigEndian;
    if (raw == NULL || out == NULL || len < 8 || (len & 7u) != 0) {
        return GDXTRK_DECODE_BAD_ARGS;
    }
    memset(out, 0, sizeof(*out));
    /* Sniff the word order from where the G_ENDDL opcode byte sits in the last command. */
    if (raw[len - 8] == 0xDF) {
        bigEndian = 1;
    } else if (raw[len - 5] == 0xDF) {
        bigEndian = 0;
    } else {
        return GDXTRK_DECODE_NO_ENDDL;
    }
    for (i = 0; i < len; i += 8) {
        const uint32_t w0 = GdxTrkReadWord(raw + i, bigEndian);
        const uint32_t w1 = GdxTrkReadWord(raw + i + 4, bigEndian);
        const uint32_t op = w0 >> 24;
        int rc;
        if (op == 0x06) { /* G_TRI2: indices are stored ×2 (RSP vertex byte offsets) */
            rc = GdxTrkPushTri(out, ((w0 >> 16) & 0xFFu) / 2u, ((w0 >> 8) & 0xFFu) / 2u, (w0 & 0xFFu) / 2u);
            if (rc != GDXTRK_DECODE_OK) {
                return rc;
            }
            rc = GdxTrkPushTri(out, ((w1 >> 16) & 0xFFu) / 2u, ((w1 >> 8) & 0xFFu) / 2u, (w1 & 0xFFu) / 2u);
            if (rc != GDXTRK_DECODE_OK) {
                return rc;
            }
        } else if (op == 0x05) { /* G_TRI1 */
            rc = GdxTrkPushTri(out, ((w0 >> 16) & 0xFFu) / 2u, ((w0 >> 8) & 0xFFu) / 2u, (w0 & 0xFFu) / 2u);
            if (rc != GDXTRK_DECODE_OK) {
                return rc;
            }
        } else if (op == 0xE7) { /* G_RDPPIPESYNC: the interpreter's handler mutates no state */
            continue;
        } else if (op == 0xFC) { /* G_SETCOMBINE */
            if (out->nevents >= GDXTRK_MAX_EVENTS) {
                return GDXTRK_DECODE_TOO_MANY_EVENTS;
            }
            out->events[out->nevents].beforeTri = out->ntris;
            out->events[out->nevents].w0 = w0;
            out->events[out->nevents].w1 = w1;
            out->nevents++;
        } else if (op == 0xDF) { /* G_ENDDL */
            return out->ntris > 0 ? GDXTRK_DECODE_OK : GDXTRK_DECODE_UNSUPPORTED_OP;
        } else {
            return GDXTRK_DECODE_UNSUPPORTED_OP;
        }
    }
    return GDXTRK_DECODE_NO_ENDDL;
}

int gdxtrk_template_equal(const GdxTrkTemplate* a, const GdxTrkTemplate* b) {
    int e;
    if (a->ntris != b->ntris || a->nevents != b->nevents || a->maxSlot != b->maxSlot) {
        return 0;
    }
    if (memcmp(a->idx, b->idx, (size_t)a->ntris * 3u) != 0) {
        return 0;
    }
    for (e = 0; e < a->nevents; e++) {
        if (a->events[e].beforeTri != b->events[e].beforeTri || a->events[e].w0 != b->events[e].w0 ||
            a->events[e].w1 != b->events[e].w1) {
            return 0;
        }
    }
    return 1;
}

static uint32_t GdxTrkFnvByte(uint32_t h, uint32_t v) {
    return (h ^ (v & 0xFFu)) * 16777619u;
}

static uint32_t GdxTrkFnvWord(uint32_t h, uint32_t v) {
    h = GdxTrkFnvByte(h, v);
    h = GdxTrkFnvByte(h, v >> 8);
    h = GdxTrkFnvByte(h, v >> 16);
    return GdxTrkFnvByte(h, v >> 24);
}

uint32_t gdxtrk_templates_digest(const GdxTrkTemplate* t) {
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < GDXTRK_TEMPLATE_COUNT; i++) {
        int j;
        h = GdxTrkFnvByte(h, t[i].ntris);
        h = GdxTrkFnvByte(h, t[i].nevents);
        h = GdxTrkFnvByte(h, t[i].maxSlot);
        for (j = 0; j < t[i].ntris * 3; j++) {
            h = GdxTrkFnvByte(h, t[i].idx[j]);
        }
        for (j = 0; j < t[i].nevents; j++) {
            h = GdxTrkFnvByte(h, t[i].events[j].beforeTri);
            h = GdxTrkFnvWord(h, t[i].events[j].w0);
            h = GdxTrkFnvWord(h, t[i].events[j].w1);
        }
    }
    return h;
}

int gdxtrk_templates_install(const uint8_t* const src[GDXTRK_TEMPLATE_COUNT], int* failedIndex, int* failedRc) {
    GdxTrkTemplate t[GDXTRK_TEMPLATE_COUNT];
    int i;
    if (failedIndex != NULL) {
        *failedIndex = -1;
    }
    if (failedRc != NULL) {
        *failedRc = GDXTRK_DECODE_OK;
    }
    for (i = 0; i < GDXTRK_TEMPLATE_COUNT; i++) {
        const int rc = src[i] != NULL ? gdxtrk_decode_template(src[i], kGdxTrkTemplateLen[i], &t[i])
                                      : GDXTRK_DECODE_BAD_ARGS;
        if (rc != GDXTRK_DECODE_OK) {
            if (failedIndex != NULL) {
                *failedIndex = i;
            }
            if (failedRc != NULL) {
                *failedRc = rc;
            }
            return 0;
        }
    }
    if (gdxtrk_templates_digest(t) != GDXTRK_TEMPLATES_DIGEST) {
        return 0;
    }
    memcpy(gGdxTrkTemplate, t, sizeof(t));
    return 1;
}
