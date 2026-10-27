/* port/3ds/gdx3ds_course_share_qr.c -- `.gdxcourse` <-> QR frames. Format in the header. */
#include "gdx3ds_course_share_qr.h"

#include <string.h>

#include "vendor/qrcodegen/qrcodegen.h"

_Static_assert(GDXCS_QR_BUFFER_BYTES == qrcodegen_BUFFER_LEN_FOR_VERSION(GDXCS_QR_VERSION_MAX), "qr buffer");
_Static_assert(GDXCS_QR_FRAMES_MAX <= 32, "haveMask is 32 bits");
_Static_assert(GDXCS_QR_FRAME_HEADER_V2 + GDXCS_QR_CHUNK_LARGE == 106, "LARGE fills QR version 6 ECC M");
_Static_assert((GDXCS_QR_PACKED_MAX + GDXCS_QR_CHUNK_LARGE - 1) / GDXCS_QR_CHUNK_LARGE <= GDXCS_QR_FRAMES_MAX,
               "a LARGE set always fits");
_Static_assert(GDXCS_FILE_BYTES <= GDXCS_QR_PACKED_MAX, "a raw file must fit a set");

#define PLANE_START (GDXCS_HEADER_BYTES + 0x20u) /* CourseData.controlPoint in the file */
#define PLANE_POINTS 64u
#define PLANE_STRIDE 20u
#define LZ_WINDOW 4096u
#define LZ_MIN 3u
#define LZ_MAX 18u

/* ---- planes transform ------------------------------------------------------------------- */

static void ToPlanes(const uint8_t* in, uint8_t* out) {
    unsigned i;
    unsigned k;
    memcpy(out, in, GDXCS_FILE_BYTES);
    for (i = 0; i < PLANE_POINTS; i++) {
        for (k = 0; k < PLANE_STRIDE; k++) {
            out[PLANE_START + k * PLANE_POINTS + i] = in[PLANE_START + i * PLANE_STRIDE + k];
        }
    }
}

static void FromPlanes(const uint8_t* in, uint8_t* out) {
    unsigned i;
    unsigned k;
    memcpy(out, in, GDXCS_FILE_BYTES);
    for (i = 0; i < PLANE_POINTS; i++) {
        for (k = 0; k < PLANE_STRIDE; k++) {
            out[PLANE_START + i * PLANE_STRIDE + k] = in[PLANE_START + k * PLANE_POINTS + i];
        }
    }
}

/* ---- LZSS ------------------------------------------------------------------------------- */

typedef struct LzMatch {
    unsigned distance;
    unsigned length;
} LzMatch;

static LzMatch LongestMatch(const uint8_t* data, size_t length, size_t pos) {
    LzMatch best = { 0, 0 };
    const size_t start = pos > LZ_WINDOW ? pos - LZ_WINDOW : 0;
    size_t candidate;
    for (candidate = pos; candidate-- > start;) {
        unsigned run = 0;
        while (run < LZ_MAX && pos + run < length && data[candidate + run] == data[pos + run]) {
            run++;
        }
        if (run > best.length) {
            best.length = run;
            best.distance = (unsigned) (pos - candidate);
            if (run == LZ_MAX) {
                break;
            }
        }
    }
    return best;
}

static size_t LzCompress(const uint8_t* data, size_t length, uint8_t* out, size_t cap) {
    size_t pos = 0;
    size_t n = 0;
    while (pos < length) {
        size_t flagAt;
        unsigned bit;
        if (n >= cap) {
            return 0;
        }
        flagAt = n++;
        out[flagAt] = 0;
        for (bit = 0; bit < 8 && pos < length; bit++) {
            const LzMatch m = LongestMatch(data, length, pos);
            if (m.length >= LZ_MIN) {
                const unsigned token = ((m.distance - 1u) << 4) | (m.length - LZ_MIN);
                if (n + 2 > cap) {
                    return 0;
                }
                out[flagAt] |= (uint8_t) (1u << bit);
                out[n++] = (uint8_t) (token >> 8);
                out[n++] = (uint8_t) token;
                pos += m.length;
            } else {
                if (n + 1 > cap) {
                    return 0;
                }
                out[n++] = data[pos++];
            }
        }
    }
    return n;
}

/* Strict: every item must be needed, every reference must point inside what is already
 * produced, the unused bits of the last flag byte must be clear, and the input must end
 * exactly where the output does. */
static int LzExpand(const uint8_t* in, size_t length, uint8_t* out, size_t want) {
    size_t i = 0;
    size_t n = 0;
    while (n < want) {
        unsigned flags;
        unsigned bit;
        if (i >= length) {
            return -1;
        }
        flags = in[i++];
        for (bit = 0; bit < 8; bit++) {
            if (n == want) {
                if ((flags >> bit) != 0) {
                    return -1;
                }
                break;
            }
            if (flags & (1u << bit)) {
                unsigned token;
                unsigned distance;
                unsigned run;
                unsigned j;
                if (i + 2 > length) {
                    return -1;
                }
                token = ((unsigned) in[i] << 8) | in[i + 1];
                i += 2;
                distance = (token >> 4) + 1u;
                run = (token & 0xFu) + LZ_MIN;
                if (distance > n || run > want - n) {
                    return -1;
                }
                for (j = 0; j < run; j++) {
                    out[n] = out[n - distance];
                    n++;
                }
            } else {
                if (i >= length) {
                    return -1;
                }
                out[n++] = in[i++];
            }
        }
    }
    return i == length ? 0 : -1;
}

size_t gdxcs_pack(const uint8_t* file, uint8_t* out, size_t cap, uint8_t* codecOut) {
    uint8_t planes[GDXCS_FILE_BYTES];
    size_t packed;
    if (file == NULL || out == NULL || codecOut == NULL) {
        return 0;
    }
    ToPlanes(file, planes);
    packed = LzCompress(planes, GDXCS_FILE_BYTES, out, cap);
    if (packed != 0 && packed < GDXCS_FILE_BYTES) {
        *codecOut = GDXCS_QR_CODEC_LZSS;
        return packed;
    }
    if (cap < GDXCS_FILE_BYTES) {
        return 0;
    }
    memcpy(out, file, GDXCS_FILE_BYTES);
    *codecOut = GDXCS_QR_CODEC_RAW;
    return GDXCS_FILE_BYTES;
}

int gdxcs_unpack(const uint8_t* packed, size_t length, uint8_t codec, uint8_t file[GDXCS_FILE_BYTES]) {
    uint8_t planes[GDXCS_FILE_BYTES];
    if (packed == NULL || file == NULL) {
        return -1;
    }
    if (codec == GDXCS_QR_CODEC_RAW) {
        if (length != GDXCS_FILE_BYTES) {
            return -1;
        }
        memcpy(file, packed, GDXCS_FILE_BYTES);
        return 0;
    }
    if (codec != GDXCS_QR_CODEC_LZSS || LzExpand(packed, length, planes, GDXCS_FILE_BYTES) != 0) {
        return -1;
    }
    FromPlanes(planes, file);
    return 0;
}

/* ---- frames ----------------------------------------------------------------------------- */

static uint16_t SetIdOf(const uint8_t* packed, size_t length) {
    return (uint16_t) (gdxcs_crc32(packed, length) & 0xFFFFu);
}

static size_t ChunkLength(size_t packedLength, int index, size_t chunkBytes) {
    const size_t start = (size_t) index * chunkBytes;
    const size_t rest = packedLength - start;
    return rest < chunkBytes ? rest : chunkBytes;
}

static int FrameCountFor(size_t packedLength, size_t chunkBytes) {
    return (int) ((packedLength + chunkBytes - 1) / chunkBytes);
}

static size_t HeaderBytes(uint8_t version) {
    return version == 1 ? GDXCS_QR_FRAME_HEADER : GDXCS_QR_FRAME_HEADER_V2;
}

int gdxcs_qr_build_sized(const uint8_t* file, size_t length, int chunk, GdxCsQrSet* set) {
    uint8_t packed[GDXCS_QR_PACKED_MAX];
    uint8_t codec = 0;
    size_t packedLength;
    uint16_t setId;
    uint8_t version;
    size_t header;
    int index;
    if (file == NULL || set == NULL || length != GDXCS_FILE_BYTES || chunk < GDXCS_QR_CHUNK_MIN ||
        chunk > GDXCS_QR_CHUNK) {
        return -1;
    }
    packedLength = gdxcs_pack(file, packed, sizeof(packed), &codec);
    if (packedLength == 0 || FrameCountFor(packedLength, (size_t) chunk) > GDXCS_QR_FRAMES_MAX) {
        return -1;
    }
    version = chunk == GDXCS_QR_CHUNK ? 1 : 2;
    header = HeaderBytes(version);
    memset(set, 0, sizeof(*set));
    set->count = FrameCountFor(packedLength, (size_t) chunk);
    setId = SetIdOf(packed, packedLength);
    for (index = 0; index < set->count; index++) {
        uint8_t* f = set->frame[index];
        const size_t bytes = ChunkLength(packedLength, index, (size_t) chunk);
        f[0] = 'G';
        f[1] = 'X';
        f[2] = version;
        f[3] = (uint8_t) index;
        f[4] = (uint8_t) set->count;
        f[5] = (uint8_t) (setId >> 8);
        f[6] = (uint8_t) setId;
        f[7] = (uint8_t) (packedLength >> 8);
        f[8] = (uint8_t) packedLength;
        f[9] = codec;
        if (version == 2) {
            f[10] = (uint8_t) chunk;
        }
        memcpy(f + header, packed + (size_t) index * (size_t) chunk, bytes);
        set->frameLength[index] = header + bytes;
    }
    return 0;
}

int gdxcs_qr_build(const uint8_t* file, size_t length, GdxCsQrSet* set) {
    return gdxcs_qr_build_sized(file, length, GDXCS_QR_CHUNK, set);
}

int gdxcs_qr_encode(const uint8_t* frame, size_t length, uint8_t qrcode[GDXCS_QR_BUFFER_BYTES]) {
    uint8_t temp[GDXCS_QR_BUFFER_BYTES];
    if (frame == NULL || qrcode == NULL || length > GDXCS_QR_FRAME_MAX) {
        return 0;
    }
    memcpy(temp, frame, length);
    if (!qrcodegen_encodeBinary(temp, length, qrcode, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                                GDXCS_QR_VERSION_MAX, qrcodegen_Mask_AUTO, false)) {
        return 0;
    }
    return qrcodegen_getSize(qrcode);
}

int gdxcs_qr_module(const uint8_t qrcode[GDXCS_QR_BUFFER_BYTES], int x, int y) {
    return qrcodegen_getModule(qrcode, x, y) ? 1 : 0;
}

#define RGB565_WHITE 0xFFFFu
#define RGB565_BLACK 0x0000u

static void PutPixel(uint16_t* fb, int fbWidth, int fbHeight, int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= fbWidth || y >= fbHeight) {
        return;
    }
    fb[(size_t) x * (size_t) fbHeight + (size_t) (fbHeight - 1 - y)] = color;
}

int gdxcs_qr_blit_rgb565(const uint8_t qrcode[GDXCS_QR_BUFFER_BYTES], uint16_t* fb, int fbWidth, int fbHeight,
                         int x0, int y0, int scale, int quiet) {
    const int modules = qrcodegen_getSize(qrcode);
    const int side = (modules + 2 * quiet) * scale;
    int py;
    if (fb == NULL || scale < 1 || quiet < 0) {
        return 0;
    }
    for (py = 0; py < side; py++) {
        const int my = py / scale - quiet;
        int px;
        for (px = 0; px < side; px++) {
            const int mx = px / scale - quiet;
            const int dark = mx >= 0 && my >= 0 && mx < modules && my < modules && qrcodegen_getModule(qrcode, mx, my);
            PutPixel(fb, fbWidth, fbHeight, x0 + px, y0 + py, dark ? RGB565_BLACK : RGB565_WHITE);
        }
    }
    return side;
}

/* ---- assembler -------------------------------------------------------------------------- */

void gdxcs_qr_assembler_reset(GdxCsQrAssembler* a) {
    memset(a, 0, sizeof(*a));
}

int gdxcs_qr_assembler_have(const GdxCsQrAssembler* a) {
    int n = 0;
    int i;
    for (i = 0; i < GDXCS_QR_FRAMES_MAX; i++) {
        if (a->haveMask & (1u << i)) {
            n++;
        }
    }
    return n;
}

typedef struct FrameHeader {
    uint8_t version;
    uint8_t index;
    uint8_t count;
    uint16_t setId;
    uint16_t packedLength;
    uint8_t codec;
    uint8_t chunk;
    size_t header;
} FrameHeader;

static int ParseFrameHeader(const uint8_t* frame, size_t length, FrameHeader* h) {
    if (frame == NULL || length < GDXCS_QR_FRAME_HEADER || length > GDXCS_QR_FRAME_MAX) {
        return -1;
    }
    if (frame[0] != 'G' || frame[1] != 'X' || (frame[2] != 1 && frame[2] != 2)) {
        return -1;
    }
    h->version = frame[2];
    h->header = HeaderBytes(h->version);
    if (length < h->header) {
        return -1;
    }
    h->index = frame[3];
    h->count = frame[4];
    h->setId = (uint16_t) ((frame[5] << 8) | frame[6]);
    h->packedLength = (uint16_t) ((frame[7] << 8) | frame[8]);
    h->codec = frame[9];
    h->chunk = h->version == 1 ? (uint8_t) GDXCS_QR_CHUNK : frame[10];
    if (h->version == 2 && (h->chunk < GDXCS_QR_CHUNK_MIN || h->chunk >= GDXCS_QR_CHUNK)) {
        return -1; /* a 200-byte set is always written as version 1: one spelling per set */
    }
    if (h->codec > GDXCS_QR_CODEC_LZSS || h->packedLength == 0 || h->packedLength > GDXCS_QR_PACKED_MAX) {
        return -1;
    }
    if (h->count < 1 || h->count > GDXCS_QR_FRAMES_MAX || h->count != FrameCountFor(h->packedLength, h->chunk)) {
        return -1;
    }
    if (h->index >= h->count || length - h->header != ChunkLength(h->packedLength, h->index, h->chunk)) {
        return -1;
    }
    return 0;
}

static int SameSet(const GdxCsQrAssembler* a, const FrameHeader* h) {
    return a->active && a->version == h->version && a->count == h->count && a->setId == h->setId &&
           a->packedLength == h->packedLength && a->codec == h->codec && a->chunk == h->chunk;
}

static void StartSet(GdxCsQrAssembler* a, const FrameHeader* h) {
    gdxcs_qr_assembler_reset(a);
    a->active = 1;
    a->version = h->version;
    a->count = h->count;
    a->setId = h->setId;
    a->packedLength = h->packedLength;
    a->codec = h->codec;
    a->chunk = h->chunk;
}

static GdxCsQrFeed FinishIfWhole(GdxCsQrAssembler* a) {
    const uint32_t whole = a->count >= 32 ? 0xFFFFFFFFu : (uint32_t) ((1u << a->count) - 1u);
    if (a->haveMask != whole) {
        return GDXCS_QR_FEED_ACCEPTED;
    }
    if (SetIdOf(a->packed, a->packedLength) != a->setId ||
        gdxcs_unpack(a->packed, a->packedLength, a->codec, a->file) != 0) {
        gdxcs_qr_assembler_reset(a);
        return GDXCS_QR_FEED_REJECTED;
    }
    return GDXCS_QR_FEED_COMPLETE;
}

GdxCsQrFeed gdxcs_qr_assembler_feed(GdxCsQrAssembler* a, const uint8_t* frame, size_t length) {
    FrameHeader h;
    GdxCsQrFeed result = GDXCS_QR_FEED_ACCEPTED;
    uint8_t* slot;
    size_t chunk;
    if (a == NULL || ParseFrameHeader(frame, length, &h) != 0) {
        return GDXCS_QR_FEED_REJECTED;
    }
    chunk = length - h.header;
    if (!SameSet(a, &h)) {
        result = a->active ? GDXCS_QR_FEED_NEW_SET : GDXCS_QR_FEED_ACCEPTED;
        StartSet(a, &h);
    }
    slot = a->packed + (size_t) h.index * h.chunk;
    if (a->haveMask & (1u << h.index)) {
        return memcmp(slot, frame + h.header, chunk) == 0 ? GDXCS_QR_FEED_DUPLICATE : GDXCS_QR_FEED_REJECTED;
    }
    memcpy(slot, frame + h.header, chunk);
    a->haveMask |= 1u << h.index;
    if (FinishIfWhole(a) == GDXCS_QR_FEED_COMPLETE) {
        return GDXCS_QR_FEED_COMPLETE;
    }
    if (!a->active) {
        return GDXCS_QR_FEED_REJECTED;
    }
    return result;
}
