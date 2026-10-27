/* port/3ds/gdx3ds_course_share_scan.c -- camera frame -> QR payloads -> `.gdxcourse`. Contract
 * and pipeline in the header. Pure C; no allocation outside gdxcs_scan_decoder_init. */
#include "gdx3ds_course_share_scan.h"

#include <stdlib.h>
#include <string.h>

#include "vendor/quirc/quirc_internal.h" /* sizeof(struct quirc), for the memory receipt */

#define LOCAL_MAX_W 640
#define LOCAL_MAX_H 480
#define LOCAL_TILES_X ((LOCAL_MAX_W + GDXCS_SCAN_LOCAL_TILE - 1) / GDXCS_SCAN_LOCAL_TILE)
#define LOCAL_TILES_Y ((LOCAL_MAX_H + GDXCS_SCAN_LOCAL_TILE - 1) / GDXCS_SCAN_LOCAL_TILE)
#define LOCAL_DARK_PERCENT 15
#define BMP_MAX_SIDE 4096
#define PHASE_SAMPLE_STEP 7

struct GdxCsScanScratch {
    struct quirc_code code;
    struct quirc_data data;
};

/* ---- frame conversion -------------------------------------------------------------------- */

/* Sum of absolute differences between horizontally neighbouring samples of one byte lane:
 * luma has edges, chroma of a black-and-white picture is flat. */
static unsigned long LaneActivity(const uint8_t* yuv, int w, int h, int lane) {
    unsigned long activity = 0;
    int y;
    for (y = 0; y < h; y += PHASE_SAMPLE_STEP) {
        const uint8_t* row = yuv + (size_t) y * (size_t) w * 2u;
        int x;
        for (x = 2; x < w * 2 - 2; x += 4) {
            const int a = row[x + lane - 2];
            const int b = row[x + lane + 2];
            activity += (unsigned long) (a > b ? a - b : b - a);
        }
    }
    return activity;
}

int gdxcs_scan_luma_phase(const uint8_t* yuv, int w, int h) {
    if (yuv == NULL || w < 8 || h < 1) {
        return 1;
    }
    return LaneActivity(yuv, w, h, 0) > LaneActivity(yuv, w, h, 1) ? 0 : 1;
}

void gdxcs_scan_yuv_to_grey(const uint8_t* yuv, int w, int h, int phase, uint8_t* grey) {
    const size_t pixels = (size_t) w * (size_t) h;
    const uint8_t* src = yuv + (phase ? 1 : 0);
    size_t i;
    for (i = 0; i < pixels; i++) {
        grey[i] = src[i * 2u];
    }
}

static uint8_t Clamp255(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t) v;
}

static int LumaOf(int r, int g, int b) {
    return (77 * r + 150 * g + 29 * b + 128) >> 8;
}

void gdxcs_scan_rgb_row_to_yuv(const uint8_t* rgb, int w, int phase, uint8_t* yuv) {
    int x;
    for (x = 0; x + 1 < w; x += 2) {
        const uint8_t* p = rgb + (size_t) x * 3u;
        const int r = (p[0] + p[3]) / 2;
        const int g = (p[1] + p[4]) / 2;
        const int b = (p[2] + p[5]) / 2;
        const uint8_t y0 = Clamp255(LumaOf(p[0], p[1], p[2]));
        const uint8_t y1 = Clamp255(LumaOf(p[3], p[4], p[5]));
        const uint8_t u = Clamp255(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
        const uint8_t v = Clamp255(((128 * r - 107 * g - 21 * b) >> 8) + 128);
        uint8_t* out = yuv + (size_t) x * 2u;
        if (phase) {
            out[0] = u;
            out[1] = y0;
            out[2] = v;
            out[3] = y1;
        } else {
            out[0] = y0;
            out[1] = u;
            out[2] = y1;
            out[3] = v;
        }
    }
}

/* ---- local threshold --------------------------------------------------------------------- */

static void TileMeans(const uint8_t* grey, int w, int h, int tilesX, int tilesY, uint8_t* means) {
    int ty;
    for (ty = 0; ty < tilesY; ty++) {
        int tx;
        for (tx = 0; tx < tilesX; tx++) {
            const int x0 = tx * GDXCS_SCAN_LOCAL_TILE;
            const int y0 = ty * GDXCS_SCAN_LOCAL_TILE;
            const int x1 = x0 + GDXCS_SCAN_LOCAL_TILE < w ? x0 + GDXCS_SCAN_LOCAL_TILE : w;
            const int y1 = y0 + GDXCS_SCAN_LOCAL_TILE < h ? y0 + GDXCS_SCAN_LOCAL_TILE : h;
            unsigned long sum = 0;
            int y;
            for (y = y0; y < y1; y++) {
                int x;
                for (x = x0; x < x1; x++) {
                    sum += grey[(size_t) y * (size_t) w + (size_t) x];
                }
            }
            means[ty * tilesX + tx] = (uint8_t) (sum / (unsigned long) ((x1 - x0) * (y1 - y0)));
        }
    }
}

/* A 3x3 box over the tile means: the window a pixel is judged against spans ~3 tiles, wide
 * enough that a finder pattern's solid 3x3-module core never fills it. */
static void SmoothTiles(const uint8_t* in, int tilesX, int tilesY, uint8_t* out) {
    int ty;
    for (ty = 0; ty < tilesY; ty++) {
        int tx;
        for (tx = 0; tx < tilesX; tx++) {
            unsigned sum = 0;
            unsigned n = 0;
            int dy;
            for (dy = -1; dy <= 1; dy++) {
                int dx;
                for (dx = -1; dx <= 1; dx++) {
                    const int x = tx + dx;
                    const int y = ty + dy;
                    if (x >= 0 && y >= 0 && x < tilesX && y < tilesY) {
                        sum += in[y * tilesX + x];
                        n++;
                    }
                }
            }
            out[ty * tilesX + tx] = (uint8_t) (sum / n);
        }
    }
}

/* Tile-centre coordinate and 8-bit fraction for pixel p along an axis of `tiles` tiles. */
static void AxisWeights(int p, int tiles, int* i0, int* i1, int* frac) {
    const int pos = p * 256 + 128 - GDXCS_SCAN_LOCAL_TILE * 128; /* (p + 0.5 - tile/2) * 256 */
    int base = pos >= 0 ? pos / (GDXCS_SCAN_LOCAL_TILE * 256) : -1;
    int f = pos >= 0 ? (pos % (GDXCS_SCAN_LOCAL_TILE * 256)) / GDXCS_SCAN_LOCAL_TILE : 0;
    if (base < 0) {
        base = 0;
        f = 0;
    }
    if (base >= tiles - 1) {
        base = tiles - 1;
        f = 0;
    }
    *i0 = base;
    *i1 = base + 1 < tiles ? base + 1 : base;
    *frac = f;
}

void gdxcs_scan_binarize_local(uint8_t* grey, int w, int h) {
    uint8_t raw[LOCAL_TILES_X * LOCAL_TILES_Y];
    uint8_t means[LOCAL_TILES_X * LOCAL_TILES_Y];
    int tilesX;
    int tilesY;
    int y;
    if (grey == NULL || w < 1 || h < 1 || w > LOCAL_MAX_W || h > LOCAL_MAX_H) {
        return;
    }
    tilesX = (w + GDXCS_SCAN_LOCAL_TILE - 1) / GDXCS_SCAN_LOCAL_TILE;
    tilesY = (h + GDXCS_SCAN_LOCAL_TILE - 1) / GDXCS_SCAN_LOCAL_TILE;
    TileMeans(grey, w, h, tilesX, tilesY, raw);
    SmoothTiles(raw, tilesX, tilesY, means);
    for (y = 0; y < h; y++) {
        int ty0;
        int ty1;
        int fy;
        int x;
        uint8_t* row = grey + (size_t) y * (size_t) w;
        AxisWeights(y, tilesY, &ty0, &ty1, &fy);
        for (x = 0; x < w; x++) {
            int tx0;
            int tx1;
            int fx;
            int top;
            int bottom;
            int mean;
            AxisWeights(x, tilesX, &tx0, &tx1, &fx);
            top = means[ty0 * tilesX + tx0] * (256 - fx) + means[ty0 * tilesX + tx1] * fx;
            bottom = means[ty1 * tilesX + tx0] * (256 - fx) + means[ty1 * tilesX + tx1] * fx;
            mean = (top * (256 - fy) + bottom * fy) >> 16;
            row[x] = row[x] * 100 < mean * (100 - LOCAL_DARK_PERCENT) ? 0 : 255;
        }
    }
}

/* ---- BMP frames ---------------------------------------------------------------------------- */

static uint32_t Le32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t Le16(const uint8_t* p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

typedef struct BmpInfo {
    size_t pixelOffset;
    size_t rowBytes;
    int width;
    int height; /* positive */
    int topDown;
    int bytesPerPixel;
} BmpInfo;

static int ParseBmpHeader(const uint8_t* hdr, size_t fileBytes, BmpInfo* info) {
    const uint32_t infoSize = Le32(hdr + 14);
    const int32_t width = (int32_t) Le32(hdr + 18);
    const int32_t height = (int32_t) Le32(hdr + 22);
    const uint16_t planes = Le16(hdr + 26);
    const uint16_t bpp = Le16(hdr + 28);
    const uint32_t compression = Le32(hdr + 30);
    if (hdr[0] != 'B' || hdr[1] != 'M' || infoSize < 40 || planes != 1) {
        return -1;
    }
    if ((bpp != 24 && bpp != 32) || (compression != 0 && !(compression == 3 && bpp == 32))) {
        return -1;
    }
    if (width < 2 || width > BMP_MAX_SIDE || height == 0 || height < -BMP_MAX_SIDE || height > BMP_MAX_SIDE) {
        return -1;
    }
    info->pixelOffset = Le32(hdr + 10);
    info->width = (int) width;
    info->height = height < 0 ? (int) -height : (int) height;
    info->topDown = height < 0;
    info->bytesPerPixel = bpp / 8;
    info->rowBytes = ((size_t) width * (size_t) info->bytesPerPixel + 3u) & ~(size_t) 3u;
    if (info->pixelOffset < 54 || info->pixelOffset > fileBytes ||
        info->rowBytes * (size_t) info->height > fileBytes - info->pixelOffset) {
        return -1;
    }
    return 0;
}

static void FillWhiteRow(uint8_t* rgb, int w) {
    memset(rgb, 0xFF, (size_t) w * 3u);
}

int gdxcs_scan_bmp_to_yuv(GdxCsScanReadFn read, void* ctx, size_t fileBytes, uint8_t* yuv, int w, int h, int phase,
                          uint8_t* rowScratch, size_t scratchBytes) {
    uint8_t hdr[54];
    BmpInfo bmp;
    uint8_t* srcRow;
    uint8_t* rgbRow;
    int fitW;
    int fitH;
    int offX;
    int offY;
    int y;
    if (read == NULL || yuv == NULL || rowScratch == NULL || w < 2 || h < 1 || (w & 1) ||
        scratchBytes < GDXCS_SCAN_BMP_SCRATCH(w) || fileBytes < sizeof(hdr)) {
        return -1;
    }
    if (read(ctx, 0, hdr, sizeof(hdr)) != sizeof(hdr) || ParseBmpHeader(hdr, fileBytes, &bmp) != 0) {
        return -1;
    }
    srcRow = rowScratch;
    rgbRow = rowScratch + 4u * BMP_MAX_SIDE;
    /* Fit inside w x h, aspect kept: whichever side binds. */
    if ((long) bmp.width * h >= (long) bmp.height * w) {
        fitW = w;
        fitH = (int) ((long) bmp.height * w / bmp.width);
    } else {
        fitH = h;
        fitW = (int) ((long) bmp.width * h / bmp.height);
    }
    fitW = fitW < 1 ? 1 : fitW;
    fitH = fitH < 1 ? 1 : fitH;
    offX = (w - fitW) / 2;
    offY = (h - fitH) / 2;
    for (y = 0; y < h; y++) {
        FillWhiteRow(rgbRow, w);
        if (y >= offY && y < offY + fitH) {
            const int sy = (int) ((long) (y - offY) * bmp.height / fitH);
            const int fileRow = bmp.topDown ? sy : bmp.height - 1 - sy;
            const size_t at = bmp.pixelOffset + (size_t) fileRow * bmp.rowBytes;
            int x;
            if (read(ctx, at, srcRow, bmp.rowBytes) != bmp.rowBytes) {
                return -1;
            }
            for (x = 0; x < fitW; x++) {
                const int sx = (int) ((long) x * bmp.width / fitW);
                const uint8_t* p = srcRow + (size_t) sx * (size_t) bmp.bytesPerPixel; /* B G R [A] */
                uint8_t* o = rgbRow + (size_t) (offX + x) * 3u;
                o[0] = p[2];
                o[1] = p[1];
                o[2] = p[0];
            }
        }
        gdxcs_scan_rgb_row_to_yuv(rgbRow, w, phase, yuv + (size_t) y * (size_t) w * 2u);
    }
    return 0;
}

/* ---- decoder ------------------------------------------------------------------------------ */

int gdxcs_scan_decoder_init(GdxCsScanDecoder* d, int w, int h) {
    if (d == NULL || w < 1 || h < 1) {
        return -1;
    }
    memset(d, 0, sizeof(*d));
    d->q = quirc_new();
    d->scratch = (struct GdxCsScanScratch*) malloc(sizeof(*d->scratch));
    if (d->q == NULL || d->scratch == NULL || quirc_resize(d->q, w, h) != 0) {
        gdxcs_scan_decoder_free(d);
        return -1;
    }
    d->w = w;
    d->h = h;
    d->nextMode = GDXCS_SCAN_MODE_GLOBAL;
    return 0;
}

void gdxcs_scan_decoder_free(GdxCsScanDecoder* d) {
    if (d == NULL) {
        return;
    }
    if (d->q != NULL) {
        quirc_destroy(d->q);
    }
    free(d->scratch);
    memset(d, 0, sizeof(*d));
}

uint8_t* gdxcs_scan_decoder_image(GdxCsScanDecoder* d) {
    if (d == NULL || d->q == NULL) {
        return NULL;
    }
    return quirc_begin(d->q, NULL, NULL);
}

size_t gdxcs_scan_decoder_bytes(const GdxCsScanDecoder* d) {
    if (d == NULL || d->q == NULL) {
        return 0;
    }
    /* quirc_new's struct, its image (the pixel map aliases it: QUIRC_MAX_REGIONS < 255, so the
     * image is the only w*h block), its flood-fill stack, and the decode scratch. */
    return sizeof(struct quirc) + (size_t) d->w * (size_t) d->h +
           d->q->num_flood_fill_vars * sizeof(struct quirc_flood_fill_vars) + sizeof(struct GdxCsScanScratch);
}

static void KeepCode(GdxCsScanResult* result, const struct quirc_data* data, int mirrored) {
    GdxCsScanCode* out;
    size_t keep;
    if (result->decoded >= GDXCS_SCAN_CODES_MAX) {
        return;
    }
    out = &result->code[result->decoded++];
    out->length = data->payload_len > 0 ? (size_t) data->payload_len : 0;
    out->mirrored = mirrored;
    out->version = data->version;
    keep = out->length < GDXCS_SCAN_PAYLOAD_MAX ? out->length : GDXCS_SCAN_PAYLOAD_MAX;
    memcpy(out->payload, data->payload, keep);
}

static void DecodeLocated(GdxCsScanDecoder* d, GdxCsScanResult* result) {
    struct quirc_code* code = &d->scratch->code;
    struct quirc_data* data = &d->scratch->data;
    int i;
    result->found = quirc_count(d->q);
    for (i = 0; i < result->found; i++) {
        quirc_extract(d->q, i, code);
        if (quirc_decode(code, data) == QUIRC_SUCCESS) {
            KeepCode(result, data, 0);
            continue;
        }
        quirc_flip(code);
        if (quirc_decode(code, data) == QUIRC_SUCCESS) {
            KeepCode(result, data, 1);
        } else {
            result->failed++;
        }
    }
}

int gdxcs_scan_decoder_run(GdxCsScanDecoder* d, GdxCsScanResult* result) {
    uint8_t* image;
    if (d == NULL || d->q == NULL || result == NULL) {
        return 0;
    }
    memset(result, 0, sizeof(*result));
    result->mode = d->nextMode;
    image = quirc_begin(d->q, NULL, NULL);
    if (result->mode == GDXCS_SCAN_MODE_LOCAL) {
        gdxcs_scan_binarize_local(image, d->w, d->h);
    }
    quirc_end(d->q);
    DecodeLocated(d, result);
    if (result->decoded == 0) {
        d->nextMode = result->mode == GDXCS_SCAN_MODE_GLOBAL ? GDXCS_SCAN_MODE_LOCAL : GDXCS_SCAN_MODE_GLOBAL;
    }
    return result->decoded;
}

/* ---- session ------------------------------------------------------------------------------ */

void gdxcs_scan_session_reset(GdxCsScanSession* s) {
    memset(s, 0, sizeof(*s));
    gdxcs_qr_assembler_reset(&s->assembler);
}

static int LooksLikeFrame(const uint8_t* payload, size_t length) {
    return length >= GDXCS_QR_FRAME_HEADER && length <= GDXCS_QR_FRAME_MAX && payload[0] == 'G' &&
           payload[1] == 'X' && (payload[2] == 1 || (payload[2] == 2 && length >= GDXCS_QR_FRAME_HEADER_V2));
}

static uint16_t FrameSetId(const uint8_t* payload) {
    return (uint16_t) ((payload[5] << 8) | payload[6]);
}

/* Same course, same page size, as the locked set: every set-identity field of the header agrees.
 * (The same course shown in the other page size is a different set: its pages do not mix.) */
static int SameCourse(const GdxCsQrAssembler* a, const uint8_t* payload) {
    const uint16_t packedLength = (uint16_t) ((payload[7] << 8) | payload[8]);
    const uint8_t chunk = payload[2] == 1 ? (uint8_t) GDXCS_QR_CHUNK : payload[10];
    return a->version == payload[2] && a->count == payload[4] && a->setId == FrameSetId(payload) &&
           a->packedLength == packedLength && a->codec == payload[9] && a->chunk == chunk;
}

static GdxCsScanEvent FeedAssembler(GdxCsScanSession* s, const uint8_t* payload, size_t length) {
    const int wasActive = s->assembler.active;
    const GdxCsQrFeed feed = gdxcs_qr_assembler_feed(&s->assembler, payload, length);
    switch (feed) {
        case GDXCS_QR_FEED_COMPLETE:
            s->pages++;
            s->complete = 1;
            return GDXCS_SCAN_EV_COMPLETE;
        case GDXCS_QR_FEED_DUPLICATE:
            s->duplicates++;
            return GDXCS_SCAN_EV_DUPLICATE;
        case GDXCS_QR_FEED_ACCEPTED:
        case GDXCS_QR_FEED_NEW_SET:
            s->pages++;
            return GDXCS_SCAN_EV_PAGE;
        default:
            break;
    }
    if (wasActive && !s->assembler.active) {
        /* the last page completed a set that did not reassemble: the assembler dropped it */
        s->badSets++;
        return GDXCS_SCAN_EV_BAD_SET;
    }
    s->notCourse++;
    return GDXCS_SCAN_EV_NOT_COURSE;
}

GdxCsScanEvent gdxcs_scan_session_feed(GdxCsScanSession* s, const uint8_t* payload, size_t length, int* pageOut) {
    if (pageOut != NULL) {
        *pageOut = -1;
    }
    if (s == NULL || payload == NULL || !LooksLikeFrame(payload, length)) {
        if (s != NULL) {
            s->notCourse++;
        }
        return GDXCS_SCAN_EV_NOT_COURSE;
    }
    if (pageOut != NULL) {
        *pageOut = payload[3];
    }
    if (s->complete) {
        s->duplicates++; /* the file is already whole; nothing more can change it */
        return GDXCS_SCAN_EV_DUPLICATE;
    }
    if (s->assembler.active && !SameCourse(&s->assembler, payload)) {
        s->foreign++;
        s->lastForeignSet = FrameSetId(payload);
        return GDXCS_SCAN_EV_FOREIGN;
    }
    return FeedAssembler(s, payload, length);
}

int gdxcs_scan_session_count(const GdxCsScanSession* s) {
    return s != NULL && s->assembler.active ? s->assembler.count : 0;
}

int gdxcs_scan_session_have(const GdxCsScanSession* s) {
    return s != NULL && s->assembler.active ? gdxcs_qr_assembler_have(&s->assembler) : 0;
}

int gdxcs_scan_session_has_page(const GdxCsScanSession* s, int index) {
    if (s == NULL || !s->assembler.active || index < 0 || index >= GDXCS_QR_FRAMES_MAX) {
        return 0;
    }
    return (s->assembler.haveMask >> index) & 1u;
}

uint16_t gdxcs_scan_session_set_id(const GdxCsScanSession* s) {
    return s != NULL ? s->assembler.setId : 0;
}

const uint8_t* gdxcs_scan_session_file(const GdxCsScanSession* s) {
    return s != NULL && s->complete ? s->assembler.file : NULL;
}

const char* gdxcs_scan_event_name(GdxCsScanEvent event) {
    switch (event) {
        case GDXCS_SCAN_EV_PAGE:
            return "page";
        case GDXCS_SCAN_EV_DUPLICATE:
            return "duplicate";
        case GDXCS_SCAN_EV_FOREIGN:
            return "foreign";
        case GDXCS_SCAN_EV_NOT_COURSE:
            return "not_course";
        case GDXCS_SCAN_EV_BAD_SET:
            return "bad_set";
        case GDXCS_SCAN_EV_COMPLETE:
            return "complete";
    }
    return "?";
}
