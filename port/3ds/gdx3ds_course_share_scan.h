/* port/3ds/gdx3ds_course_share_scan.h -- `[ek] course_share` scanning: camera frames in, a
 * `.gdxcourse` out. The portable half of the 3DS camera scanner (gdx3ds_course_share_cam.c owns
 * the camera, the worker thread and the APT hooks; gdx3ds_course_share.c owns the screen).
 *
 * PURE C99 plus the vendored quirc (port/3ds/vendor/quirc): compiled into the EK build and into
 * the host unit test (port/3ds/test/gdx3ds_course_share_host_tests.c), which feeds it synthetic
 * camera frames -- rendered QR pages scaled, rotated, mirrored, darkened, blurred and noised,
 * converted to the camera's YUV422 -- so every stage below runs on the host first.
 *
 * PIPELINE (one camera frame):
 *   YUV422 frame (GDXCS_SCAN_W x GDXCS_SCAN_H, 2 bytes a pixel)
 *     -> luma phase: which byte of each pair is Y. The camera service writes U Y V Y (Azahar's
 *        conversion, and the order the 3DS hardware documents), but nothing here depends on it:
 *        a QR is black on white, so Y swings across the whole range and U/V sit near 128, and
 *        the byte with the larger spread is Y (gdxcs_scan_luma_phase).
 *     -> grey (the Y bytes), straight into quirc's own image buffer: no intermediate copy.
 *     -> binarize: quirc's own global (Otsu) threshold, or, on alternate frames while nothing
 *        decodes, a local-mean threshold first (gdxcs_scan_binarize_local) for a dim or
 *        unevenly lit picture: a screen photographed at an angle, a glare spot, a vignette.
 *     -> quirc: finds codes at any rotation and under perspective; a code that fails its
 *        format/ECC check is retried mirrored (quirc_flip), which reads a code seen in a mirror
 *        or through the back of a phone camera preview.
 *     -> payloads (GdxCsScanResult) -> GdxCsScanSession.
 *
 * SESSION. Every frame header (gdx3ds_course_share_qr.h) carries the set's identity: a set id
 * (the low 16 bits of the CRC-32 of the packed course) with the frame version, page count,
 * packed length, codec and page size. That is the course id the scanner locks on, so no header
 * extension was needed for it. The session locks to the first valid page it sees; a page whose
 * identity differs is a page of ANOTHER course (or the same course in the other page size) and
 * is counted and ignored -- it never resets the pages already collected (the assembler on its
 * own would start over). [RESTART] is the user's way to switch. Two different courses that agree
 * in every identity field are still caught when the set completes: the stream must CRC to the
 * set id and unpack with no slack, then pass gdxcs_decode_file's two CRC-32s and the validator.
 */
#ifndef GDX3DS_COURSE_SHARE_SCAN_H
#define GDX3DS_COURSE_SHARE_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "gdx3ds_course_share_qr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDXCS_SCAN_W 320
#define GDXCS_SCAN_H 240
#define GDXCS_SCAN_YUV_BYTES ((size_t) GDXCS_SCAN_W * GDXCS_SCAN_H * 2u)
#define GDXCS_SCAN_CODES_MAX 4    /* payloads kept from one frame */
#define GDXCS_SCAN_PAYLOAD_MAX 256 /* longer payloads are kept truncated, flagged */
#define GDXCS_SCAN_LOCAL_TILE 16  /* local threshold: tile side in px */

/* ---- frame conversion -------------------------------------------------------------------- */

/* 0 when Y is the even byte of each pair (Y U Y V), 1 when it is the odd byte (U Y V Y). Looks
 * at a sparse sample; ties (a flat frame) answer 1, the camera service's order. */
int gdxcs_scan_luma_phase(const uint8_t* yuv, int w, int h);

/* The Y bytes of a YUV422 frame (w even). `grey` holds w * h bytes. */
void gdxcs_scan_yuv_to_grey(const uint8_t* yuv, int w, int h, int phase, uint8_t* grey);

/* One row of 8-bit RGB triplets to YUV422 (BT.601 integer, U/V averaged over each pair) in the
 * given phase. `w` even. Used by the BMP frame source and the host test's synthetic camera. */
void gdxcs_scan_rgb_row_to_yuv(const uint8_t* rgb, int w, int phase, uint8_t* yuv);

/* Local-mean binarization in place: each pixel becomes 0 or 255 against the bilinearly
 * interpolated mean of its GDXCS_SCAN_LOCAL_TILE neighbourhood (Bradley's rule, 15% below the
 * mean is dark). w, h <= 640 x 480. */
void gdxcs_scan_binarize_local(uint8_t* grey, int w, int h);

/* A 24- or 32-bit uncompressed BMP (bottom-up or top-down, any size up to 4096 x 4096) fitted
 * into a w x h YUV422 frame: scaled by nearest neighbour to fit with its aspect kept, centred,
 * white around it. `read` returns the bytes read at `offset` (short = error). `rowScratch` holds
 * at least 4 * 4096 + 3 * w bytes. 0 on success, -1 on anything it does not read. */
typedef size_t (*GdxCsScanReadFn)(void* ctx, size_t offset, void* dst, size_t n);
int gdxcs_scan_bmp_to_yuv(GdxCsScanReadFn read, void* ctx, size_t fileBytes, uint8_t* yuv, int w, int h, int phase,
                          uint8_t* rowScratch, size_t scratchBytes);
#define GDXCS_SCAN_BMP_SCRATCH(w) (4u * 4096u + 3u * (size_t) (w))

/* ---- decoder ------------------------------------------------------------------------------ */

typedef enum GdxCsScanMode {
    GDXCS_SCAN_MODE_GLOBAL = 0, /* quirc's Otsu threshold on the grey picture */
    GDXCS_SCAN_MODE_LOCAL = 1   /* gdxcs_scan_binarize_local first */
} GdxCsScanMode;

typedef struct GdxCsScanCode {
    size_t length;  /* full payload length (may exceed GDXCS_SCAN_PAYLOAD_MAX) */
    int mirrored;   /* decoded only after quirc_flip */
    int version;
    uint8_t payload[GDXCS_SCAN_PAYLOAD_MAX];
} GdxCsScanCode;

typedef struct GdxCsScanResult {
    int found;      /* codes quirc located (decoded or not) */
    int decoded;    /* entries in code[] */
    int failed;     /* located but undecodable, even mirrored */
    GdxCsScanMode mode;
    GdxCsScanCode code[GDXCS_SCAN_CODES_MAX];
} GdxCsScanResult;

struct quirc;
struct GdxCsScanScratch; /* quirc_code + quirc_data: ~13 KB, heap, not stack */

typedef struct GdxCsScanDecoder {
    struct quirc* q;
    struct GdxCsScanScratch* scratch;
    int w;
    int h;
    GdxCsScanMode nextMode; /* sticks while it decodes, alternates while nothing does */
} GdxCsScanDecoder;

/* Allocates quirc for w x h (its image buffer, w * h bytes, IS the grey frame) and the decode
 * scratch. 0 on success; on failure nothing stays allocated. */
int gdxcs_scan_decoder_init(GdxCsScanDecoder* d, int w, int h);
void gdxcs_scan_decoder_free(GdxCsScanDecoder* d);
/* The w * h grey buffer to fill before each run (quirc thresholds it in place). */
uint8_t* gdxcs_scan_decoder_image(GdxCsScanDecoder* d);
/* Decodes the filled image. Returns result->decoded. */
int gdxcs_scan_decoder_run(GdxCsScanDecoder* d, GdxCsScanResult* result);
/* Heap bytes the decoder holds (receipts and the memory budget). */
size_t gdxcs_scan_decoder_bytes(const GdxCsScanDecoder* d);

/* ---- session ------------------------------------------------------------------------------ */

typedef enum GdxCsScanEvent {
    GDXCS_SCAN_EV_PAGE = 0,  /* a new page of the locked course */
    GDXCS_SCAN_EV_DUPLICATE, /* a page already held */
    GDXCS_SCAN_EV_FOREIGN,   /* a page of a different course: ignored */
    GDXCS_SCAN_EV_NOT_COURSE,/* a QR that is not a course page */
    GDXCS_SCAN_EV_BAD_SET,   /* the set completed but did not reassemble: pages dropped */
    GDXCS_SCAN_EV_COMPLETE   /* the file is ready: gdxcs_scan_session_file() */
} GdxCsScanEvent;

typedef struct GdxCsScanSession {
    GdxCsQrAssembler assembler;
    int complete;
    int pages;      /* new pages accepted (over the session, across BAD_SET restarts) */
    int duplicates;
    int foreign;
    int notCourse;
    int badSets;
    uint16_t lastForeignSet;
} GdxCsScanSession;

void gdxcs_scan_session_reset(GdxCsScanSession* s);
/* Feeds one decoded payload of `length` bytes. *pageOut (optional) receives the page index the
 * payload claims (0-based) or -1. */
GdxCsScanEvent gdxcs_scan_session_feed(GdxCsScanSession* s, const uint8_t* payload, size_t length, int* pageOut);
int gdxcs_scan_session_count(const GdxCsScanSession* s);    /* pages in the locked set, 0 = none yet */
int gdxcs_scan_session_have(const GdxCsScanSession* s);     /* pages held */
int gdxcs_scan_session_has_page(const GdxCsScanSession* s, int index);
uint16_t gdxcs_scan_session_set_id(const GdxCsScanSession* s);
/* The reassembled file (GDXCS_FILE_BYTES) once an event was COMPLETE, else NULL. */
const uint8_t* gdxcs_scan_session_file(const GdxCsScanSession* s);
const char* gdxcs_scan_event_name(GdxCsScanEvent event);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_COURSE_SHARE_SCAN_H */
