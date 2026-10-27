/* port/3ds/gdx3ds_course_share_qr.h -- a `.gdxcourse` as a short series of QR codes.
 *
 * PURE C99 (plus the vendored qrcodegen, port/3ds/vendor/qrcodegen): compiled into the EK
 * build, which shows the codes on the bottom screen, and into the host unit test, which renders
 * them, decodes the picture with the vendored quirc (port/3ds/vendor/quirc) and checks the bytes
 * come back. The console scans them with its camera (gdx3ds_course_share_scan.h); the format is
 * fixed so a phone app or a PC tool can read what this writes too.
 *
 * PIPELINE. file (GDXCS_FILE_BYTES) -> pack -> split into frames -> one QR (byte mode, ECC M,
 * version <= GDXCS_QR_VERSION_MAX) per frame. Two page sizes:
 *   DENSE  200-byte chunks, frame version 1: QR version 10 (57 modules), 3 screen px a module.
 *          Fewest pages (2..5 for the disk courses); for a phone camera, which resolves it easily.
 *   LARGE  GDXCS_QR_CHUNK_LARGE-byte chunks, frame version 2: QR version 6 (41 modules), 5 screen
 *          px a module (~1 mm on the bottom LCD). For the 3DS camera, whose VGA sensor needs a
 *          module ~3 camera px wide: a dense page would have to be held ~10 cm away, closer than
 *          it focuses; a large one reads at 15-20 cm. 5..10 pages for the disk courses.
 *
 * PACK (codec 1). The 64 control points (file bytes 0x60..0x55F, 20 bytes each) are transposed
 * to field-major order -- byte k of every point, then byte k+1 -- which puts the similar bytes of
 * neighbouring points side by side; then LZSS over the whole file: a flag byte per 8 items, LSB
 * first, bit set = a 2-byte back-reference ((distance-1) << 4 | (length-3), big-endian; distance
 * 1..4096, length 3..18), bit clear = one literal byte. The 22 Expansion Kit disk courses pack to
 * 391..925 bytes, so 2 to 5 frames. Codec 0 is the file verbatim, used when packing does not win.
 *
 * FRAME (all big-endian):
 *   0  2  'G' 'X'
 *   2  1  version 1 (200-byte chunks) or 2 (chunk size in byte 10)
 *   3  1  index   0..count-1
 *   4  1  count   1..GDXCS_QR_FRAMES_MAX (version 1: 1..GDXCS_QR_FRAMES_MAX_V1)
 *   5  2  setId   low 16 bits of the CRC-32 of the packed stream (ties the frames of one set)
 *   7  2  packedLength
 *   9  1  codec   0 raw, 1 planes+LZSS
 *  version 1:
 *  10  n  chunk   packed bytes [index*GDXCS_QR_CHUNK, +n), n = GDXCS_QR_CHUNK except the last
 *  version 2 (compatible extension: a version-1 reader refuses it at byte 2, nothing else moves):
 *  10  1  chunkBytes  GDXCS_QR_CHUNK_MIN..GDXCS_QR_CHUNK
 *  11  n  chunk   packed bytes [index*chunkBytes, +n), n = chunkBytes except the last
 *
 * The receiver trusts none of it: every frame of a set must repeat the same version, count,
 * setId, length, codec and chunk size; chunk lengths must be exactly what the length implies; the reassembled
 * stream must CRC to the setId and unpack to exactly GDXCS_FILE_BYTES with no slack; and the
 * result then goes through gdxcs_decode_file like any file from the SD card. */
#ifndef GDX3DS_COURSE_SHARE_QR_H
#define GDX3DS_COURSE_SHARE_QR_H

#include <stddef.h>
#include <stdint.h>

#include "gdx3ds_course_share_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDXCS_QR_FRAME_HEADER 10    /* version 1 */
#define GDXCS_QR_FRAME_HEADER_V2 11 /* version 2: + the chunk size */
#define GDXCS_QR_CHUNK 200          /* DENSE, version 1 */
#define GDXCS_QR_CHUNK_LARGE 95     /* LARGE, version 2: 106-byte frames, QR version 6 ECC M exactly */
#define GDXCS_QR_CHUNK_MIN 16
#define GDXCS_QR_FRAME_MAX (GDXCS_QR_FRAME_HEADER + GDXCS_QR_CHUNK) /* <= 213: version 10, ECC M, byte mode */
#define GDXCS_QR_VERSION_MAX 10
#define GDXCS_QR_FRAMES_MAX_V1 12 /* LZSS worst case: 9/8 of the file + 1 = 2341 bytes = 12 chunks */
#define GDXCS_QR_FRAMES_MAX 32    /* any chunk size: a set larger than this is refused */
#define GDXCS_QR_PACKED_MAX (GDXCS_QR_FRAMES_MAX_V1 * GDXCS_QR_CHUNK)
#define GDXCS_QR_CODEC_RAW 0
#define GDXCS_QR_CODEC_LZSS 1
/* qrcodegen's buffer for one code of up to GDXCS_QR_VERSION_MAX. */
#define GDXCS_QR_BUFFER_BYTES ((((GDXCS_QR_VERSION_MAX) * 4 + 17) * ((GDXCS_QR_VERSION_MAX) * 4 + 17) + 7) / 8 + 1)

/* Packs a whole .gdxcourse (exactly GDXCS_FILE_BYTES) into `out`; returns the length (> 0), or
 * 0 if `cap` is too small. *codecOut receives the codec chosen. */
size_t gdxcs_pack(const uint8_t* file, uint8_t* out, size_t cap, uint8_t* codecOut);

/* Inverse of gdxcs_pack. 0 on success with exactly GDXCS_FILE_BYTES written to `file`;
 * -1 on any malformed input (never reads past `length`, never writes past the file). */
int gdxcs_unpack(const uint8_t* packed, size_t length, uint8_t codec, uint8_t file[GDXCS_FILE_BYTES]);

typedef struct GdxCsQrSet {
    int count;
    size_t frameLength[GDXCS_QR_FRAMES_MAX];
    uint8_t frame[GDXCS_QR_FRAMES_MAX][GDXCS_QR_FRAME_MAX];
} GdxCsQrSet;

/* Splits a .gdxcourse into DENSE (version 1) frames. 0 on success. */
int gdxcs_qr_build(const uint8_t* file, size_t length, GdxCsQrSet* set);
/* Same, with `chunk` bytes a frame: GDXCS_QR_CHUNK gives version 1 frames (== gdxcs_qr_build),
 * GDXCS_QR_CHUNK_MIN..GDXCS_QR_CHUNK-1 version 2. -1 when the set would exceed
 * GDXCS_QR_FRAMES_MAX frames. */
int gdxcs_qr_build_sized(const uint8_t* file, size_t length, int chunk, GdxCsQrSet* set);

/* Renders frame bytes as a QR code into `qrcode` (GDXCS_QR_BUFFER_BYTES). Returns the module
 * count per side (21..57), or 0 on failure. */
int gdxcs_qr_encode(const uint8_t* frame, size_t length, uint8_t qrcode[GDXCS_QR_BUFFER_BYTES]);
int gdxcs_qr_module(const uint8_t qrcode[GDXCS_QR_BUFFER_BYTES], int x, int y);

/* Paints the code, `scale` px per module with a `quiet`-module white border, into an RGB565
 * framebuffer in the 3DS's portrait layout (pixel (x, y) of a fbWidth x fbHeight landscape
 * screen at fb[x * fbHeight + (fbHeight - 1 - y)], as gdx3ds_racemap.c paints the bottom screen).
 * (x0, y0) is the top-left of the border. Pixels outside the screen are skipped. Returns the
 * painted square's side in px. */
int gdxcs_qr_blit_rgb565(const uint8_t qrcode[GDXCS_QR_BUFFER_BYTES], uint16_t* fb, int fbWidth, int fbHeight,
                         int x0, int y0, int scale, int quiet);

typedef enum GdxCsQrFeed {
    GDXCS_QR_FEED_ACCEPTED = 0, /* a new frame of the current set */
    GDXCS_QR_FEED_DUPLICATE,    /* a frame already held (same bytes) */
    GDXCS_QR_FEED_COMPLETE,     /* the set is whole and unpacked: the file is ready */
    GDXCS_QR_FEED_NEW_SET,      /* a frame of a different set: the old one was dropped */
    GDXCS_QR_FEED_REJECTED      /* not a frame, or inconsistent: ignored */
} GdxCsQrFeed;

typedef struct GdxCsQrAssembler {
    int active;
    uint8_t version;
    uint8_t count;
    uint16_t setId;
    uint16_t packedLength;
    uint8_t codec;
    uint8_t chunk;
    uint32_t haveMask;
    uint8_t packed[GDXCS_QR_PACKED_MAX];
    uint8_t file[GDXCS_FILE_BYTES];
} GdxCsQrAssembler;

void gdxcs_qr_assembler_reset(GdxCsQrAssembler* a);
GdxCsQrFeed gdxcs_qr_assembler_feed(GdxCsQrAssembler* a, const uint8_t* frame, size_t length);
int gdxcs_qr_assembler_have(const GdxCsQrAssembler* a); /* frames held */

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_COURSE_SHARE_QR_H */
