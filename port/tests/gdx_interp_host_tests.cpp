/* port/tests/gdx_interp_host_tests.cpp -- host harness for the owned F3DEX2 interpreter
 * (docs/research/native/06-interp-plan.md §6.3). Feeds hand-built Gfx arrays (16-byte tagged
 * and 8-byte forms) into the walker with a mock backend and a mock resolver seam, and asserts
 * walk counts, combiner ids, TMEM spans, decoded texels and packed-VBO floats.
 *
 * Build (tools/ci-3ds.sh): c++ -std=c++20 -O1 port/tests/gdx_interp_host_tests.cpp
 *   port/3ds/gfx/n64/{gdx_dlsrc,gdx_dl,gdx_interp,gdx_cc,gdx_tmem,gdx_texcache,gdx_rsp,gdx_tri,gdx_rect}.cpp
 *   -Iport/3ds/gfx/n64 -Iport/3ds/gfx -Iport -Iport/tests   (n64 first: port/gdx_interp.h exists too) */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "gdx_dlsrc_bridge.h"
#include "gdx_interp.h"
#include "gdx_mock_backend.h"
#include "gdx3ds_hud_record.h"

/* ---- mock seam: a fixed set of host lists/data blocks, resolvable by low32 -------------- */
struct MockRange {
    uintptr_t base;
    size_t size;
    uint8_t stride;
    uint8_t isBig;
    uint8_t isF3d;
    uint8_t isList;
};
static std::vector<MockRange> gRanges;
static uintptr_t gRoot = 0;
static size_t gRootBytes = 0;
static uintptr_t gSegs[16] = {};

static const MockRange* FindRange(uintptr_t p) {
    for (auto& r : gRanges) {
        if (p >= r.base && p < r.base + r.size) return &r;
    }
    return nullptr;
}
/* Host pointers are 64-bit here but the GDX1 scheme carries low32 only (exact on the 32-bit
 * device): the mock maps a tagged low32 back onto its range. */
static uintptr_t ResolveLow32(uint32_t raw) {
    const uint8_t seg = raw >> 24;
    if (seg != 0 && seg < 16 && gSegs[seg] != 0) return gSegs[seg] + (raw & 0xFFFFFF);
    for (auto& r : gRanges) {
        if (raw >= static_cast<uint32_t>(r.base) && raw < static_cast<uint32_t>(r.base + r.size)) return r.base + (raw - static_cast<uint32_t>(r.base));
    }
    return 0;
}

/* [native] ncensus / ownfold knobs for the fold tests (the device reads the ini). */
static int gdx_test_census = 0;
static int gdx_test_ownfold = 0;
static int gdx_test_dspcensus = 0;
/* [native] timgmemo: ON in the host tests, so every TMEM test in this file exercises the memo's
   hit path. The mock bridge's class epoch is a non-zero constant, which is exactly the "nothing
   invalidates" case -- so any test that goes red here is the memo returning a DIFFERENT answer
   than the three calls it replaces, which is the only thing a host test can prove about it. */
static int gdx_test_timgmemo = 1;
/* [native] tmemskip: the LEVEL under test. The mock source-stability seam below is the honest
   mock for a host harness that has no bridge tables: it classifies every mock range STABLE and
   answers "unchanged", so tmemskip=1 here is the STRONGEST form of the skip and any TMEM test
   that goes red is the widened gate skipping a load the shipping gate would have performed.
   The suite runs the TMEM tests at BOTH levels -- see TmemSkipSuite. */
static int gdx_test_tmemskip = 0;
static int gdx_test_tmemfault = 0;   /* [memotear] B54 fault injection */
static int gdx_test_tmemcls = 0;
/* o3dsinterp: [native] o3dsfast (shipping default 1) and trimemo (default 0). TestFastPathParity
   runs one scene under every combination and requires byte-identical batches and uploads. */
static int gdx_test_o3dsfast = 1;
static int gdx_test_trimemo = 0;
/* Mock generations, so a test can move one and prove the skip stops firing, and the class the
   mock seam reports for an in-range source (STABLE unless a test asks for another). */
static uint64_t gdx_test_native_gen = 1;
static uint64_t gdx_test_dma_gen = 1;
static unsigned gdx_test_src_cls = GDX_TMEMSRC_STABLE;

extern "C" {
int gdx_gfx_job_root_list(GdxGfxJob*, GdxDlSrcList* out) {
    const MockRange* r = FindRange(gRoot);
    if (r == nullptr) return 0;
    out->ptr = reinterpret_cast<const void*>(gRoot);
    out->limit = gRootBytes / r->stride;
    out->stride = r->stride;
    out->isBig = r->isBig;
    out->isF3d = r->isF3d;
    return 1;
}
int gdx_gfx_job_resolve_list(GdxGfxJob*, uint32_t raw, int hostTagged, uintptr_t hostFull, GdxDlSrcList* out) {
    const uintptr_t p = hostTagged ? ResolveLow32(static_cast<uint32_t>(hostFull)) : ResolveLow32(raw);
    const MockRange* r = FindRange(p);
    if (r == nullptr || !r->isList) return 0;
    out->ptr = reinterpret_cast<const void*>(p);
    out->limit = (r->base + r->size - p) / r->stride;
    out->stride = r->stride;
    out->isBig = r->isBig;
    out->isF3d = r->isF3d;
    return 1;
}
/* [native] dlmemo (c5dlmemo): the mock resolve is a pure function of the operand and the mock
   tables never move, so every successful resolve is admissible and the key is constant. */
static uint32_t gdx_test_memo_version = 1;
int gdx_gfx_job_resolve_list_memo(GdxGfxJob* job, uint32_t raw, int hostTagged, uintptr_t hostFull, GdxDlSrcList* out,
                                  int* memoOk, uint32_t* sniffCmds) {
    *memoOk = 0;
    *sniffCmds = 0;
    if (gdx_gfx_job_resolve_list(job, raw, hostTagged, hostFull, out) == 0) {
        return 0;
    }
    /* exercise the raw-class capture where it fits: a sniff that reads 5 commands */
    *memoOk = (out->limit >= 64) ? 2 : 1;
    *sniffCmds = (*memoOk == 2) ? 5u : 0u;
    return 1;
}
void gdx_gfx_job_list_memo_key(GdxGfxJob*, GdxDlMemoKey* out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 16; i++) {
        out->seg[i] = gSegs[i];
    }
    out->tablesVersion = gdx_test_memo_version;
}
uintptr_t gdx_gfx_job_resolve_data(GdxGfxJob*, uint32_t raw, int hostTagged, uintptr_t hostFull, size_t need, int, uintptr_t) {
    const uintptr_t p = hostTagged ? ResolveLow32(static_cast<uint32_t>(hostFull)) : ResolveLow32(raw);
    const MockRange* r = FindRange(p);
    if (r == nullptr || p + need > r->base + r->size) return 0;
    return p;
}
uintptr_t gdx_gfx_job_resolve_timg_host(GdxGfxJob*, uintptr_t hostFull, size_t need) {
    const uintptr_t p = ResolveLow32(static_cast<uint32_t>(hostFull));
    const MockRange* r = FindRange(p);
    if (r == nullptr || p + need > r->base + r->size) return 0;
    return p;
}
void gdx_gfx_timg_class_epoch_parts(uint32_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = 0;
}
uint32_t gdx_gfx_timg_class_epoch(void) {
    /* The mock tables never mutate, so one constant epoch is the honest answer and it exercises
       the memo's hit path in the host tests. */
    return 1u;
}
unsigned gdx_gfx_classify_host_ptr(unsigned long) {
    return 0;
}
/* [native] hud_retain: one mock range can be declared an o2r (immutable) resource. */
static uintptr_t gdx_test_imm_lo = 0, gdx_test_imm_hi = 0;
int gdx_gfx_timg_is_resource(uint32_t, int, uintptr_t p, size_t) {
    return (p >= gdx_test_imm_lo && p < gdx_test_imm_hi) ? 1 : 0;
}
/* o3dsinterp: one mock range can be declared native-RGBA16 so the halfword-swap mirror runs. */
static uintptr_t gdx_test_native_lo = 0, gdx_test_native_hi = 0;
int gdx_gfx_texels_are_native_rgba16(uintptr_t p, size_t) {
    return (p >= gdx_test_native_lo && p < gdx_test_native_hi) ? 1 : 0;
}
size_t gdx_gfx_readable_bytes(uintptr_t p) {
    const MockRange* r = FindRange(p);
    return r ? (r->base + r->size - p) : 0;
}
int gdx_gfx_low32_needs_resolver(uint32_t) {
    return 0;
}
/* [tmemblock] source-stability seam. Every mock range is a registered host range that no DMA
   ever writes, so the honest mock answers STABLE; a test that wants to see the skip STOP moves
   gdx_test_native_gen / gdx_test_dma_gen and asks for the matching class. */
unsigned gdx_gfx_tmem_source_class(uintptr_t source, size_t size) {
    const MockRange* r = FindRange(source);
    if (r == nullptr || size == 0 || source + size > r->base + r->size) {
        return GDX_TMEMSRC_UNKNOWN;
    }
    return gdx_test_src_cls;
}
void gdx_gfx_tmem_source_stamp(uint64_t out[2]) {
    out[0] = gdx_test_native_gen;
    out[1] = gdx_test_dma_gen;
}
int gdx_gfx_tmem_source_unchanged(uintptr_t source, size_t size, unsigned clsAtStamp, const uint64_t stamp[2]) {
    const unsigned cls = gdx_gfx_tmem_source_class(source, size);
    if (cls != clsAtStamp || stamp == nullptr) return 0;
    if (cls == GDX_TMEMSRC_NATIVE) return stamp[0] == gdx_test_native_gen ? 1 : 0;
    if (cls == GDX_TMEMSRC_DMA) return stamp[1] == gdx_test_dma_gen ? 1 : 0;
    return cls == GDX_TMEMSRC_STABLE ? 1 : 0;
}
uintptr_t gdx_gfx_job_set_segment(GdxGfxJob*, unsigned seg, uint32_t raw, int hostTagged, uintptr_t hostFull) {
    gSegs[seg & 15] = hostTagged ? ResolveLow32(static_cast<uint32_t>(hostFull)) : ResolveLow32(raw);
    return gSegs[seg & 15];
}
int gdx_gfx_ucode_variant(uint32_t raw, int, uintptr_t, int* variant, int* isLine) {
    *isLine = 0;
    *variant = static_cast<int>(raw & 3);
    return raw >= 0x1000 ? 1 : 0;
}
void gdx_gfx_job_run_lus(GdxGfxJob*) {
}
int gdx_gfx_job_ucode(GdxGfxJob*) {
    return 0;
}
void gdx_gfx_job_bridge_stats(GdxGfxJob*, unsigned long* a, unsigned long* b, unsigned long c[256]) {
    *a = *b = 0;
    memset(c, 0, 256 * sizeof(unsigned long));
}
void gdx_gfx_job_epilogue(GdxGfxJob*) {
}
int gdx_gfx_frame_config(GdxFrameConfig*) {
    return 0;
}
void gdx_gfx_frame_publish(uintptr_t) {
}
int gdx_ninterp_blendflush(void) {
    return 1;
}
int gdx_ninterp_cpuclip(void) {
    return 1;
}
int gdx_ninterp_census(void) {
    return gdx_test_census;
}
int gdx_ninterp_ownfold(void) {
    return gdx_test_ownfold;
}
int gdx3ds_tex_fast_enabled(void) {
    /* N60TEX: GDX_TEST_TEX_FAST=1 runs every decode through the [debug] tex_fast table forms */
    const char* env = getenv("GDX_TEST_TEX_FAST");
    return env != nullptr && env[0] == '1';
}
int gdx_ninterp_palhash(void) {
    return 1;
}
long long gdx_ninterp_now(void) {
    return 0;
}
long long gdx_ninterp_hws_begin(void) {
    return 0;
}
void gdx_ninterp_hws_end(int site, long long t0) {
    (void)site;
    (void)t0;
}
int gdx_ninterp_dspcensus(void) {
    return gdx_test_dspcensus;
}
int gdx_ninterp_timgmemo(void) {
    return gdx_test_timgmemo;
}
int gdx_ninterp_o3dsfast(void) {
    return gdx_test_o3dsfast;
}
int gdx_ninterp_trimemo(void) {
    return gdx_test_trimemo;
}
int gdx_ninterp_tmemskip(void) {
    return gdx_test_tmemskip;
}
int gdx_ninterp_tmemcls(void) {
    return gdx_test_tmemcls;
}
int gdx_ninterp_tmemfault(void) {
    return gdx_test_tmemfault;
}
/* [memotear] B54: the mock's generations are the ones gdx_gfx_tmem_source_unchanged above
   reads, so moving them here is the same fault the real seam injects through the bridge's bump
   helper and dirty-range recorder. STABLE has no generation arm in the real seam either, so the
   mock answers 0 for it and the injector falls back to the class mechanism -- which is the
   behaviour under test, not a mock limitation. */
int gdx_gfx_tmem_fault_move_generation(uintptr_t, size_t, unsigned cls) {
    if (cls == GDX_TMEMSRC_NATIVE) {
        gdx_test_native_gen++;
        return 1;
    }
    if (cls == GDX_TMEMSRC_DMA) {
        gdx_test_dma_gen++;
        return 1;
    }
    return 0;
}
unsigned long long gdx_ninterp_child_ticks(void) {
    return 0;
}
void gdxn_logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
}

/* ---- test scaffolding -------------------------------------------------------------------- */
static int gFails = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
            gFails++;                                                                 \
        }                                                                             \
    } while (0)

/* Wide (16-byte tagged) command builder. */
struct W {
    uint32_t w0, pad, w1lo, w1hi;
};
static W Wv(uint32_t w0, uint32_t w1) {
    return W{ w0, 0, w1, 0 };
}
static W Wp(uint32_t w0, const void* p) {
    return W{ w0, 0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)), gdxn::kHostTag };
}
static void Reg(const void* p, size_t size, uint8_t stride, bool big, bool f3d, bool list) {
    MockRange r;
    r.base = reinterpret_cast<uintptr_t>(p);
    r.size = size;
    r.stride = stride;
    r.isBig = big ? 1 : 0;
    r.isF3d = f3d ? 1 : 0;
    r.isList = list ? 1 : 0;
    gRanges.push_back(r);
}
static GdxFrameConfig Cfg() {
    GdxFrameConfig c;
    memset(&c, 0, sizeof(c));
    c.windowW = c.curW = 400;
    c.windowH = c.curH = 240;
    c.nativeW = 320;
    c.nativeH = 240;
    c.internalMul = 1.0f;
    c.widescreen = 1;
    return c;
}

/* Test 1: flow -- push/branch/ENDDL, histogram, F3D remap on a narrow list. */
static void TestWalk() {
    static W sub[] = { Wv(0xE7000000, 0), Wv(0xDF000000, 0) };
    static W root[8];
    root[0] = Wv(0xE7000000, 0);
    root[1] = Wp(0xDE000000, sub);  /* push */
    root[2] = Wv(0xE6000000, 0);
    root[3] = Wp(0xDE010000, sub);  /* branch: ends the root */
    root[4] = Wv(0xE7000000, 0);
    root[5] = Wv(0xDF000000, 0);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(sub, sizeof(sub), 16, false, false, true);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = sizeof(root);
    MockBackend mb;
    gdxn::Interp it(&mb);
    it.RunFrame(nullptr, Cfg(), 0);
    const gdxn::Counters& s = it.Stats();
    CHECK(s.lists == 3);
    CHECK(s.op[0xE7] == 3); /* root, sub (push), sub (branch) */
    CHECK(s.op[0xE6] == 1);
    CHECK(s.op[0xDF] == 2);
    CHECK(s.op[0xDE] == 2);
    CHECK(s.cmds == 8);
    CHECK(mb.frames == 1 && mb.clears == 1);

    /* narrow legacy-F3D list: 0xB8 ENDDL, 0xBF TRI1 with *10 indices */
    static uint32_t f3d[] = { 0xBF000000u, 0x00000A14u /* v0=0,v1=1,v2=2 (x10) */, 0xB8000000u, 0 };
    gRanges.clear();
    Reg(f3d, sizeof(f3d), 8, false, true, true);
    gRoot = reinterpret_cast<uintptr_t>(f3d);
    gRootBytes = sizeof(f3d);
    it.ResetStats();
    it.RunFrame(nullptr, Cfg(), 0);
    CHECK(it.Stats().op[0xBF] == 1 && it.Stats().op[0xB8] == 1);
    CHECK(it.Stats().listsF3d == 1);
    CHECK(it.Stats().tris == 1); /* submitted (vertices are zero -> rejected/culled downstream) */
}

/* Test 2: combiner ids for the census's most common mode (TEXEL0*SHADE, 1-cycle). */
static void TestCC() {
    gdxn::CombinerKey k;
    /* gDPSetCombineLERP(TEXEL0,0,SHADE,0, TEXEL0,0,SHADE,0, ...) -> color_comb(1,31,4,31)=? */
    const uint32_t cc = (1u & 0xF) | ((31u & 0xF) << 4) | ((4u & 0x1F) << 8) | ((7u & 7) << 13);
    const uint32_t ac = (1u & 7) | ((7u & 7) << 3) | ((4u & 7) << 6) | ((7u & 7) << 9);
    k.combine_mode = static_cast<uint64_t>(cc) | (static_cast<uint64_t>(ac) << 16);
    k.options = 0;
    gdxn::Combiner c;
    gdxn::GenerateCC(&c, k);
    /* rgb: A=TEXEL0(8) B=0 C=SHADE->INPUT_1 D=0 ; alpha: A=TEXEL0 B=0 C=SHADE->INPUT_1 D=0 */
    CHECK(((c.shader_id0 >> 0) & 0xF) == 8);
    CHECK(((c.shader_id0 >> 4) & 0xF) == 0);
    CHECK(((c.shader_id0 >> 8) & 0xF) == 1);
    CHECK(((c.shader_id0 >> 16) & 0xF) == 8);
    CHECK(((c.shader_id0 >> 24) & 0xF) == 1);
    CHECK(c.usedTextures[0] && !c.usedTextures[1]);
    CHECK(c.shader_input_mapping[0][0] == 4 && c.shader_input_mapping[1][0] == 4);
    gdxn::CombinerPool pool;
    bool created = false;
    gdxn::Combiner* a = pool.Lookup(k, &created);
    CHECK(created && a != nullptr);
    gdxn::Combiner* b = pool.Lookup(k, &created);
    CHECK(!created && a == b);
}

/* Test 3: SETTIMG/SETTILE/LOADBLOCK/SETTILESIZE + TEXRECT: TMEM span, RGBA16 decode, one
 * packed batch of two rect tris with the expected NDC corners. */
static void TestRectTexture() {
    static uint8_t texels[16 * 16 * 2];
    for (int i = 0; i < 256; i++) {
        const uint16_t c = static_cast<uint16_t>((31 << 11) | 1); /* red, opaque */
        texels[2 * i] = c >> 8;
        texels[2 * i + 1] = c & 0xFF;
    }
    static W root[16];
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u);           /* othermode_h cycle type 1cyc (shift 20 len 2) */
    root[n++] = Wv(0xFC121824u, 0xFF33FFFFu);           /* combine: TEXEL0 only */
    root[n++] = Wp(0xFD10000Fu, texels);                /* SETTIMG RGBA16 w=16 */
    root[n++] = Wv(0xF5100000u, 0x07000000u);           /* SETTILE RGBA16 line 0 tmem 0 -> tile 7 */
    root[n++] = Wv(0xF3000000u, 0x070FF800u);           /* LOADBLOCK tile 7 lrs=255 dxt=0x800 */
    root[n++] = Wv(0xF5100800u, 0x00000000u);           /* SETTILE RGBA16 line 4 (32B) tmem 0 tile 0 */
    root[n++] = Wv(0xF2000000u, 0x0003C03Cu);           /* SETTILESIZE tile0 0,0..15,15 */
    root[n++] = Wv(0xE4064064u, 0x00000000u);           /* TEXRECT lrx=100 lry=100 (U10.2 -> 25px) ulx=0 uly=0 tile 0 */
    root[n++] = Wv(0xE1000000u, 0x00000000u);           /* RDPHALF_1: s,t = 0 */
    root[n++] = Wv(0xF1000000u, 0x04000400u);           /* RDPHALF_2: dsdx=dtdy=1.0 (S5.10 0x400) */
    root[n++] = Wv(0xDF000000u, 0);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = static_cast<size_t>(n) * 16;
    MockBackend mb;
    gdxn::Interp it(&mb);
    it.RunFrame(nullptr, Cfg(), 0);
    const gdxn::Counters& s = it.Stats();
    CHECK(s.tmemLoads == 1);
    CHECK(s.rects == 1);
    CHECK(s.texImports == 1 && s.texMisses == 1);
    CHECK(mb.uploads.size() == 1);
    if (!mb.uploads.empty()) {
        CHECK(mb.uploads[0].w == 16 && mb.uploads[0].h == 16);
        CHECK(mb.uploads[0].rgba[0] == 255 && mb.uploads[0].rgba[1] == 0 && mb.uploads[0].rgba[3] == 255);
    }
    CHECK(mb.batches.size() == 1);
    if (!mb.batches.empty()) {
        const MockBatch& b = mb.batches[0];
        CHECK(b.numTris == 2);
        /* ul corner: ulx=0 -> x=-1 (then hor+ 4:3/(400/240) = 0.8), uly=0 -> y=+1 */
        CHECK(fabsf(b.vbo[0] - (-0.8f)) < 1e-6f);
        CHECK(fabsf(b.vbo[1] - 1.0f) < 1e-6f);
        CHECK(b.vbo[3] == 1.0f);
        /* z is from 0..1 in the mock: (-1 + 1)/2 = 0 */
        CHECK(b.vbo[2] == 0.0f);
        CHECK(b.tex[0] == 1);
    }
    /* second identical frame: span same-content skip is gated off (raw source) -> a load, a
       cache HIT, no new upload, and identical floats */
    it.RunFrame(nullptr, Cfg(), 0);
    CHECK(it.Stats().texHits == 1);
    CHECK(mb.uploads.size() == 1);
    CHECK(mb.batches.size() == 2);
    if (mb.batches.size() == 2) {
        CHECK(memcmp(mb.batches[0].vbo.data(), mb.batches[1].vbo.data(), mb.batches[0].vbo.size() * 4) == 0);
    }
}

/* Test 4: G_MTX + G_VTX + G_TRI1 on a narrow big-endian list with byte-swapped operands. */
static void TestTri3D() {
    /* identity fixed-point matrix: int parts diag 1, frac 0 -- packed as N64 Mtx (big-endian) */
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    /* int part rows: word i*2 + j/2 holds (m[i][j] << 16) | m[i][j+1] */
    /* word i*2 + j/2 = (m[i][j] << 16) | m[i][j+1] */
    mtx[0] = 0x00010000u; /* m00=1 m01=0 */
    mtx[2] = 0x00000001u; /* m10=0 m11=1 */
    mtx[5] = 0x00010000u; /* m22=1 m23=0 */
    mtx[7] = 0x00000001u; /* m32=0 m33=1 */
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[3];
    memset(vtx, 0, sizeof(vtx));
    const int16_t ob[3][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } }; /* inside |x|,|y| <= w=1 */
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        vtx[v].cn[0] = 255; vtx[v].cn[1] = 128; vtx[v].cn[2] = 0; vtx[v].cn[3] = 255;
    }
    /* narrow big-endian list, segment 6 tokens */
    static uint32_t dl[14];
    int n = 0;
    dl[n++] = 0xDA380003u; dl[n++] = 0x06000000u;   /* MTX modelview|load|nopush (params 2 ^ push=1 -> 3) */
    dl[n++] = 0xDA380007u; dl[n++] = 0x06000000u;   /* MTX projection|load|nopush (params 6 ^ push=1 -> 7) */
    dl[n++] = 0x01003006u; dl[n++] = 0x06000100u;   /* VTX n=3 v0=0 */
    dl[n++] = 0xFC121824u; dl[n++] = 0xFF33FFFFu;   /* combine */
    dl[n++] = 0xD9FFFFFFu; dl[n++] = 0x00000004u;   /* geometry mode set SHADE */
    dl[n++] = 0x05000204u; dl[n++] = 0x00000000u;   /* TRI1 0,1,2 */
    dl[n++] = 0xDF000000u; dl[n++] = 0;
    for (int i = 0; i < n; i++) dl[i] = gdxn::Bswap32(dl[i]);
    alignas(8) static uint8_t seg6[512];
    memcpy(seg6, mtx, 64);
    memcpy(seg6 + 0x100, vtx, sizeof(vtx));
    gRanges.clear();
    Reg(dl, sizeof(dl), 8, true, false, true);
    Reg(seg6, sizeof(seg6), 8, false, false, false);
    gSegs[6] = reinterpret_cast<uintptr_t>(seg6);
    gRoot = reinterpret_cast<uintptr_t>(dl);
    gRootBytes = static_cast<size_t>(n) * 4;
    MockBackend mb;
    gdxn::Interp it(&mb);
    it.RunFrame(nullptr, Cfg(), 0);
    CHECK(it.Stats().vtx == 3);
    CHECK(it.Stats().tris == 1 && it.Stats().trisEmitted == 1);
    CHECK(mb.batches.size() == 1);
    if (!mb.batches.empty()) {
        const MockBatch& b = mb.batches[0];
        /* identity MP: x = ob.x * aspect adj (0.8), y = ob.y, w = 1 */
        CHECK(fabsf(b.vbo[0] - (-0.8f)) < 1e-6f);
        CHECK(fabsf(b.vbo[1] - (-1.0f)) < 1e-6f);
        CHECK(b.vbo[3] == 1.0f);
        /* shade rgb from cn: 255,128,0 -> 1, 128/255, 0 */
        CHECK(b.vbo[6] == 1.0f && fabsf(b.vbo[7] - 128.0f / 255.0f) < 1e-7f && b.vbo[8] == 0.0f);
    }
    gSegs[6] = 0;
}

/* Test 5: [native] ownfold -- the prim/env fold inside the owned batches. Two texrects with a
 * SETPRIMCOLOR value change between them, under a combiner that reads PRIMITIVE. The fold must
 * be taken ONLY when the backend says no stage constant of the open batch reads that register. */
static void TestOwnFold() {
    static uint8_t texels[16 * 16 * 2];
    for (int i = 0; i < 256; i++) {
        const uint16_t c = static_cast<uint16_t>((31 << 11) | 1);
        texels[2 * i] = c >> 8;
        texels[2 * i + 1] = c & 0xFF;
    }
    /* combiner cycle 0: rgb = (TEXEL0 - 0) * PRIMITIVE + 0, alpha = (TEXEL0 - 0) * PRIM + 0.
       SetCombineWords field map: rgbA=w0[20:4] rgbB=w1[28:4] rgbC=w0[15:5] rgbD=w1[15:3],
       alphaA=w0[12:3] alphaB=w1[12:3] alphaC=w0[9:3] alphaD=w1[9:3]. */
    const uint32_t combW0 = 0xFC000000u | (1u << 20) | (3u << 15) | (1u << 12) | (3u << 9);
    const uint32_t combW1 = (8u << 28) | (7u << 15) | (7u << 12) | (7u << 9);
    static W root[24];
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u);
    root[n++] = Wv(combW0, combW1);
    root[n++] = Wp(0xFD10000Fu, texels);
    root[n++] = Wv(0xF5100000u, 0x07000000u);
    root[n++] = Wv(0xF3000000u, 0x070FF800u);
    root[n++] = Wv(0xF5100800u, 0x00000000u);
    root[n++] = Wv(0xF2000000u, 0x0003C03Cu);
    root[n++] = Wv(0xFA000000u, 0xFF0000FFu); /* SETPRIMCOLOR red */
    root[n++] = Wv(0xE4064064u, 0x00000000u);
    root[n++] = Wv(0xE1000000u, 0x00000000u);
    root[n++] = Wv(0xF1000000u, 0x04000400u);
    root[n++] = Wv(0xFA000000u, 0x00FF00FFu); /* SETPRIMCOLOR green -- the split under test */
    root[n++] = Wv(0xE4064064u, 0x00000000u);
    root[n++] = Wv(0xE1000000u, 0x00000000u);
    root[n++] = Wv(0xF1000000u, 0x04000400u);
    root[n++] = Wv(0xDF000000u, 0);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = static_cast<size_t>(n) * 16;

    /* (a) ownfold=0: the prim change always splits. */
    gdx_test_ownfold = 0;
    gdx_test_census = 1;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().rects == 2);
        CHECK(mb.batches.size() == 2);
        CHECK(it.Stats().flushDrain[gdxn::FLUSH_PRIM] == 1);
        /* the census sees it as NOT foldable: the mock binds every input as a stage constant
           and the combiner's input 1 IS prim. */
        CHECK(it.Stats().foldable[0] == 0);
    }
    /* (b) ownfold=1 but the shader still binds prim as a stage constant: no fold. */
    gdx_test_ownfold = 1;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.batches.size() == 2);
        CHECK(it.Stats().folded[0] == 0);
    }
    /* (c) ownfold=1 and the backend reports prim riding the vertex colour attribute: fold,
       one draw, both rects in it. */
    {
        MockBackend mb;
        mb.constRgbMask = 0;
        mb.constAlphaMask = 0;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().rects == 2);
        CHECK(mb.batches.size() == 1);
        CHECK(it.Stats().folded[0] == 1);
        CHECK(it.Stats().flushDrain[gdxn::FLUSH_PRIM] == 0);
        if (mb.batches.size() == 1) {
            CHECK(mb.batches[0].numTris == 4); /* two rects, one draw */
        }
    }
    /* (d) the backend cannot report (no shader bound): never fold. */
    {
        MockBackend mb;
        mb.constInputsOk = false;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.batches.size() == 2);
        CHECK(it.Stats().folded[0] == 0);
    }
    gdx_test_ownfold = 0;
    gdx_test_census = 0;
}

/* Test 6 (GEOMLOSS): a batch that does not fit the pool must be SPLIT, not dropped.
 *
 * The owned interpreter has no legacy variable-stride path, so an all-or-nothing packed
 * begin turned pool pressure into missing triangles. Build one list with 300 TRI1s, run it
 * twice: once with a pool that comfortably holds them, once with a pool sized to exactly
 * the geometry (300 * 3 * 12 floats). The old begin needed room for a whole 256-tri batch
 * up front, so the second run dropped the last 44 triangles; the fixed begin reports the
 * cap and the batch splits there. The assertion is the strong one: the concatenated packed
 * floats must be BYTE-IDENTICAL between the two runs. */
static void BuildTriSpam(std::vector<uint32_t>& dl, int nTris) {
    dl.clear();
    dl.push_back(0xDA380003u); dl.push_back(0x06000000u); /* MTX modelview */
    dl.push_back(0xDA380007u); dl.push_back(0x06000000u); /* MTX projection */
    dl.push_back(0x01003006u); dl.push_back(0x06000100u); /* VTX n=3 v0=0 */
    dl.push_back(0xFC121824u); dl.push_back(0xFF33FFFFu); /* combine */
    dl.push_back(0xD9FFFFFFu); dl.push_back(0x00000004u); /* geometry mode set SHADE */
    for (int i = 0; i < nTris; i++) {
        dl.push_back(0x05000204u); dl.push_back(0x00000000u); /* TRI1 0,1,2 */
    }
    dl.push_back(0xDF000000u); dl.push_back(0u);
    for (auto& w : dl) w = gdxn::Bswap32(w);
}

static void RunTriSpam(std::vector<uint32_t>& dl, size_t poolFloats, std::vector<float>& outVbo,
                       size_t* outTris, uint32_t* outRefused, uint32_t* outSplit, size_t* outBatches) {
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0] = 0x00010000u; mtx[2] = 0x00000001u; mtx[5] = 0x00010000u; mtx[7] = 0x00000001u;
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[3];
    memset(vtx, 0, sizeof(vtx));
    const int16_t ob[3][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } };
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        vtx[v].cn[0] = 255; vtx[v].cn[1] = 128; vtx[v].cn[2] = 0; vtx[v].cn[3] = 255;
    }
    alignas(8) static uint8_t seg6[512];
    memcpy(seg6, mtx, 64);
    memcpy(seg6 + 0x100, vtx, sizeof(vtx));
    gRanges.clear();
    Reg(dl.data(), dl.size() * 4, 8, true, false, true);
    Reg(seg6, sizeof(seg6), 8, false, false, false);
    gSegs[6] = reinterpret_cast<uintptr_t>(seg6);
    gRoot = reinterpret_cast<uintptr_t>(dl.data());
    gRootBytes = dl.size() * 4;
    MockBackend mb;
    mb.pool.assign(poolFloats, 0.0f);
    gdxn::Interp it(&mb);
    it.RunFrame(nullptr, Cfg(), 0);
    outVbo.clear();
    for (const MockBatch& b : mb.batches) {
        outVbo.insert(outVbo.end(), b.vbo.begin(), b.vbo.end());
    }
    *outTris = it.Stats().trisEmitted;
    *outRefused = it.Stats().packRefused;
    *outSplit = it.Stats().packSplit;
    *outBatches = mb.batches.size();
    gSegs[6] = 0;
}

static void TestPackSplitPreservesGeometry() {
    const int kTris = 300;
    const size_t kExactFloats = static_cast<size_t>(kTris) * 3u * 12u;
    std::vector<uint32_t> dl;
    BuildTriSpam(dl, kTris);

    std::vector<float> roomy;
    size_t roomyTris = 0, roomyBatches = 0;
    uint32_t roomyRef = 0, roomySplit = 0;
    RunTriSpam(dl, 65536, roomy, &roomyTris, &roomyRef, &roomySplit, &roomyBatches);
    CHECK(roomyTris == static_cast<size_t>(kTris));
    CHECK(roomyRef == 0 && roomySplit == 0);
    CHECK(roomyBatches == 2); /* 256 + 44 */
    CHECK(roomy.size() == kExactFloats);

    std::vector<float> tight;
    size_t tightTris = 0, tightBatches = 0;
    uint32_t tightRef = 0, tightSplit = 0;
    RunTriSpam(dl, kExactFloats, tight, &tightTris, &tightRef, &tightSplit, &tightBatches);
    /* The pool holds the geometry exactly, so nothing may be refused and nothing lost. */
    CHECK(tightRef == 0);
    CHECK(tightSplit == 1); /* the second begin is capped at the 44 tris that still fit */
    CHECK(tightTris == static_cast<size_t>(kTris));
    CHECK(tight.size() == kExactFloats);
    CHECK(tight.size() == roomy.size());
    if (tight.size() == roomy.size()) {
        CHECK(memcmp(tight.data(), roomy.data(), tight.size() * sizeof(float)) == 0);
    }

    /* Genuine exhaustion still refuses — and is still counted, loudly, on the receipt. */
    std::vector<float> starved;
    size_t starvedTris = 0, starvedBatches = 0;
    uint32_t starvedRef = 0, starvedSplit = 0;
    RunTriSpam(dl, 36, starved, &starvedTris, &starvedRef, &starvedSplit, &starvedBatches);
    CHECK(starvedTris == 1);   /* exactly the one triangle the pool can hold */
    CHECK(starvedRef > 0);     /* and the rest is refused, not silently forgotten */
}

/* ---- CI8 address recycling (audit 2026-09-23 finding 26) ---------------------------------
 * A CI8 texture and its TLUT are rewritten IN PLACE at the same addresses -- what the per-race
 * arena rewind does to `sCourseMinimapTex`, and what the GfxPool does to the night-course
 * TLUTs between two courses. The decoded result must follow the bytes, not the address.
 * The two halves of the fix are asserted separately because they answer different producers:
 * the content hash (`[native] texhash_ci`) is self-healing, and TexCacheClear /
 * TexCacheDeleteAddress are the queued gdx_rdram_mode_reset / minimap.c requests replayed onto
 * the owned cache (`[native] texinval`). */
static int BuildCi8List(W* root, const void* tlut, const void* indices) {
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u); /* othermode_h: 1cyc */
    root[n++] = Wv(0xFC121824u, 0xFF33FFFFu); /* combine: TEXEL0 only */
    /* TLUT: 256 RGBA16 entries to tmem word 256, through load tile 7. */
    root[n++] = Wp(0xFD1000FFu, tlut);        /* SETTIMG RGBA16 width 256 */
    root[n++] = Wv(0xF5100100u, 0x07000000u); /* SETTILE RGBA16 tmem=256 -> tile 7 */
    root[n++] = Wv(0xF0000000u, 0x073FC000u); /* LOADTLUT tile 7, highIndex 255 */
    /* CI8 8x8 index image at tmem word 0, through load tile 7. */
    root[n++] = Wp(0xFD480007u, indices);     /* SETTIMG CI 8b width 8 */
    root[n++] = Wv(0xF5480000u, 0x07000000u); /* SETTILE CI 8b line 0 tmem 0 -> tile 7 */
    root[n++] = Wv(0xF3000000u, 0x0703F800u); /* LOADBLOCK tile 7 lrs=63 dxt=0x800 */
    root[n++] = Wv(0xF5480200u, 0x00000000u); /* SETTILE CI 8b line 1 tmem 0 -> tile 0 */
    root[n++] = Wv(0xF2000000u, 0x0001C01Cu); /* SETTILESIZE tile 0 -> 8x8 */
    root[n++] = Wv(0xE401C01Cu, 0x00000000u); /* TEXRECT */
    root[n++] = Wv(0xE1000000u, 0x00000000u); /* RDPHALF_1: s,t = 0 */
    root[n++] = Wv(0xF1000000u, 0x04000400u); /* RDPHALF_2: dsdx=dtdy=1.0 */
    root[n++] = Wv(0xDF000000u, 0);
    return n;
}

static void TestCiRecycle() {
    static uint8_t tlut[512];
    static uint8_t indices[64];
    static W root[24];
    const uint16_t kRed = static_cast<uint16_t>((31u << 11) | 1u);
    const uint16_t kBlue = static_cast<uint16_t>((31u << 1) | 1u);
    const uint16_t kGreen = static_cast<uint16_t>((31u << 6) | 1u);
    const auto put = [](uint8_t* p, uint16_t c) {
        p[0] = static_cast<uint8_t>(c >> 8);
        p[1] = static_cast<uint8_t>(c & 0xFF);
    };
    const int n = BuildCi8List(root, tlut, indices);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(tlut, sizeof(tlut), 8, false, false, false);
    Reg(indices, sizeof(indices), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = static_cast<size_t>(n) * 16;

    /* course 1: every texel index 0, palette entry 0 = red, entry 1 = blue */
    memset(tlut, 0, sizeof(tlut));
    put(tlut + 0, kRed);
    put(tlut + 2, kBlue);
    memset(indices, 0, sizeof(indices));

    /* --- A: content hash ON (the shipping default) --- */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texMisses == 1 && mb.uploads.size() == 1);
        if (!mb.uploads.empty()) {
            CHECK(mb.uploads[0].rgba[0] == 255 && mb.uploads[0].rgba[2] == 0); /* red */
        }
        /* same address, different INDEX bytes -> must re-decode */
        memset(indices, 1, sizeof(indices));
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texMisses == 2 && mb.uploads.size() == 2);
        if (mb.uploads.size() >= 2) {
            CHECK(mb.uploads[1].rgba[0] == 0 && mb.uploads[1].rgba[2] == 255); /* blue */
        }
        /* same address, same indices, different PALETTE bytes -> must re-decode too. This is
           the night-course TLUT case: the palette is half the decoded pixel, and the key
           carries only palette_dram_addr, which the pool re-hands to the next course. */
        put(tlut + 2, kGreen);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texMisses == 3 && mb.uploads.size() == 3);
        if (mb.uploads.size() >= 3) {
            CHECK(mb.uploads[2].rgba[1] == 255 && mb.uploads[2].rgba[2] == 0); /* green */
        }
    }

    /* --- B: content hash OFF ([native] texhash_ci=0) -- identity keying, exactly what the
           owned path did before the fix, with TexCacheClear as the only way back --- */
    put(tlut + 2, kBlue);
    memset(indices, 0, sizeof(indices));
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetCiContentHash(false);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texMisses == 1 && mb.uploads.size() == 1);
        memset(indices, 1, sizeof(indices));
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texHits == 1); /* the bug: a recycled address is a cache HIT ... */
        CHECK(mb.uploads.size() == 1);  /* ... and the previous occupant's decode is served */
        it.TexCacheClear();             /* what gdx_rdram_mode_reset asks for */
        CHECK(it.Stats().texClears == 1);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.uploads.size() == 2);
        if (mb.uploads.size() >= 2) {
            CHECK(mb.uploads[1].rgba[0] == 0 && mb.uploads[1].rgba[2] == 255); /* blue */
        }
    }

    /* --- C: TexCacheDeleteAddress is exact-address, like LUS TextureCacheDelete (which is why
           minimap.c invalidates each CI8 half separately) --- */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetCiContentHash(false);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.uploads.size() == 1);
        memset(indices, 0, sizeof(indices));
        it.TexCacheDeleteAddress(reinterpret_cast<const uint8_t*>(indices) + 1); /* not the key */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texInvalidated == 0);
        CHECK(mb.uploads.size() == 1); /* no match -> still the stale decode */
        it.TexCacheDeleteAddress(reinterpret_cast<const uint8_t*>(indices));
        CHECK(it.Stats().texInvalidated == 1);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.uploads.size() == 2);
        if (mb.uploads.size() >= 2) {
            CHECK(mb.uploads[1].rgba[0] == 255 && mb.uploads[1].rgba[2] == 0); /* red again */
        }
    }
}


/* ---- DROPSITES 1: the texture-cache decode ceiling ---------------------------------------
 * Pre-fix, a decode that did not fit the fixed 256 KiB scratch returned without uploading and
 * ImportTexture marked the entry `uploaded` anyway. The backend texture id had just been handed
 * out of the free list, so it still held the previous texture's pixels -- and the entry being
 * marked uploaded made every later lookup of that key a cache HIT over those pixels.
 * `texDecodeOverflow` counted it and was printed by nothing.
 *
 * [debug] texscratchkib / texscratchmaxkib straddle the 8x8 CI8 decode (256 B) so both arms are
 * reachable without a megabyte display list, exactly as geomloss's vbopoolkib does for the VBO
 * pool. The pre-fix behaviour is [native] texstalefix=0, asserted here as the control. */
static void TestDecodeCeiling() {
    static uint8_t tlut[512];
    static uint8_t indices[64];
    static W root[24];
    const uint16_t kRed = static_cast<uint16_t>((31u << 11) | 1u);
    const int n = BuildCi8List(root, tlut, indices);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(tlut, sizeof(tlut), 8, false, false, false);
    Reg(indices, sizeof(indices), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = static_cast<size_t>(n) * 16;
    memset(tlut, 0, sizeof(tlut));
    tlut[0] = static_cast<uint8_t>(kRed >> 8);
    tlut[1] = static_cast<uint8_t>(kRed & 0xFF);
    memset(indices, 0, sizeof(indices));

    /* --- A: the scratch grows, and a texture that used to be SKIPPED now decodes correctly. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetTexScratchBytes(128, 0); /* 128 B scratch, default 4 MiB ceiling */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texDecodeGrow == 1);
        CHECK(it.Stats().texDecodeOverflow == 0);
        CHECK(it.Stats().texNoUpload == 0);
        CHECK(mb.uploads.size() == 1);
        if (!mb.uploads.empty()) {
            CHECK(mb.uploads[0].w == 8 && mb.uploads[0].h == 8);
            CHECK(mb.uploads[0].rgba[0] == 255 && mb.uploads[0].rgba[2] == 0); /* red, decoded */
        }
        CHECK(it.TexDecodeHwBytes() == 8u * 8u * 4u); /* the receipt's margin number */
    }

    /* --- B: a genuine ceiling (no upload could ever succeed) -> neutral fallback, and the
           entry is NOT cached as uploaded, so the next frame retries instead of serving it. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetTexScratchBytes(128, 128); /* ceiling below the 256 B decode */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texDecodeOverflow == 1);
        CHECK(it.Stats().texNoUpload == 1);
        CHECK(it.Stats().texFallback == 1);
        CHECK(mb.uploads.size() == 1); /* pre-fix this was 0: the id kept its old pixels */
        if (!mb.uploads.empty()) {
            CHECK(mb.uploads[0].w == 8 && mb.uploads[0].h == 8);
            bool allWhite = true;
            for (size_t k = 0; k < mb.uploads[0].rgba.size(); k++) {
                if (mb.uploads[0].rgba[k] != 255) allWhite = false;
            }
            CHECK(allWhite); /* untextured, i.e. defined -- not an arbitrary previous texture */
        }
        it.RunFrame(nullptr, Cfg(), 0);
        /* TexCacheLookup matches the key but sees `uploaded == false`, so it re-arms the import
           instead of serving: not a hit, and not a second miss either -- the entry is still the
           same entry, it just has no content to give. The retry is what [ndrops] texnoup= / texfb=
           count every frame it happens. */
        CHECK(it.Stats().texHits == 0);
        CHECK(it.Stats().texMisses == 1);
        CHECK(it.Stats().texNoUpload == 2);
        CHECK(mb.uploads.size() == 2);
    }

    /* --- C: the pre-fix arm, from the same binary. This is the bug, asserted. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetTexScratchBytes(128, 128);
        it.SetTexStaleFix(false);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texDecodeOverflow == 1);
        CHECK(mb.uploads.empty()); /* the backend was told NOTHING about this id */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texHits == 1); /* and the empty id is now a cache HIT, for good */
        CHECK(mb.uploads.empty());
    }
}

/* ---- DROPSITES 2: G_VTX past the 64-slot window -----------------------------------------
 * Pre-fix: mStats.vtx was incremented by the full n_vertices and then the WHOLE load was
 * dropped, so the receipt over-reported exactly the vertices that were never transformed and
 * nothing counted the drop. [debug] vtxmax shrinks the window so both arms are reachable. */
static void TestVtxWindow() {
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0] = 0x00010000u;
    mtx[2] = 0x00000001u;
    mtx[5] = 0x00010000u;
    mtx[7] = 0x00000001u;
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[3];
    memset(vtx, 0, sizeof(vtx));
    const int16_t ob[3][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } };
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) {
            vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        }
        vtx[v].cn[0] = 255; vtx[v].cn[1] = 128; vtx[v].cn[2] = 0; vtx[v].cn[3] = 255;
    }
    static uint32_t dl[14];
    int n = 0;
    dl[n++] = 0xDA380003u; dl[n++] = 0x06000000u;
    dl[n++] = 0xDA380007u; dl[n++] = 0x06000000u;
    dl[n++] = 0x01003006u; dl[n++] = 0x06000100u;   /* VTX n=3 dest=0 */
    dl[n++] = 0xFC121824u; dl[n++] = 0xFF33FFFFu;
    dl[n++] = 0xD9FFFFFFu; dl[n++] = 0x00000004u;
    dl[n++] = 0x05000204u; dl[n++] = 0x00000000u;
    dl[n++] = 0xDF000000u; dl[n++] = 0;
    for (int i = 0; i < n; i++) dl[i] = gdxn::Bswap32(dl[i]);
    alignas(8) static uint8_t seg6[512];
    memcpy(seg6, mtx, 64);
    memcpy(seg6 + 0x100, vtx, sizeof(vtx));
    gRanges.clear();
    Reg(dl, sizeof(dl), 8, true, false, true);
    Reg(seg6, sizeof(seg6), 8, false, false, false);
    gSegs[6] = reinterpret_cast<uintptr_t>(seg6);
    gRoot = reinterpret_cast<uintptr_t>(dl);
    gRootBytes = static_cast<size_t>(n) * 4;

    /* In range: nothing is counted and the high-water is the real end of the load. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().vtx == 3 && it.Stats().vtxOver == 0);
        CHECK(it.VtxHwEnd() == 3);
    }
    /* Over the window: the prefix loads, the tail is refused, and vtx= counts only what was
       actually transformed. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetVtxSoftCap(2);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().vtxOver == 1);
        CHECK(it.Stats().vtxOverDropped == 1);
        CHECK(it.Stats().vtx == 2); /* pre-fix: 3, for a load that transformed none of them */
        CHECK(it.VtxHwEnd() == 3);
    }
    /* The pre-fix arm ([native] vtxclamp=0): the whole load is dropped. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetVtxSoftCap(2);
        it.SetVtxClamp(false);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().vtxOver == 1);
        CHECK(it.Stats().vtxOverDropped == 3);
        CHECK(it.Stats().vtx == 0);
    }
}

/* ---- DROPSITES 3: the clip polygon can never reach its cap -------------------------------
 * Sutherland-Hodgman against a half-space adds at most one vertex to a CONVEX polygon (two
 * crossings cost at least one vertex, and each step's result is convex again), so from a
 * triangle over 7 planes the output is bounded by 3 + 7 = 10 against a 16-slot buffer. The cap
 * is kept (it is the LUS clipper's, expression for expression) and counted instead of raised.
 * This pins the bound on a triangle that crosses every plane it can. */
static void TestClipPolyBound() {
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0] = 0x00010000u;
    mtx[2] = 0x00000001u;
    mtx[5] = 0x00010000u;
    mtx[7] = 0x00000001u;
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[3];
    memset(vtx, 0, sizeof(vtx));
    /* A huge triangle around the origin: every clip plane cuts it. */
    const int16_t ob[3][3] = { { -900, -700, 0 }, { 900, -700, 0 }, { 0, 900, 0 } };
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) {
            vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        }
        vtx[v].cn[0] = 255; vtx[v].cn[1] = 128; vtx[v].cn[2] = 0; vtx[v].cn[3] = 255;
    }
    static uint32_t dl[14];
    int n = 0;
    dl[n++] = 0xDA380003u; dl[n++] = 0x06000000u;
    dl[n++] = 0xDA380007u; dl[n++] = 0x06000000u;
    dl[n++] = 0x01003006u; dl[n++] = 0x06000100u;
    dl[n++] = 0xFC121824u; dl[n++] = 0xFF33FFFFu;
    dl[n++] = 0xD9FFFFFFu; dl[n++] = 0x00000004u;
    dl[n++] = 0x05000204u; dl[n++] = 0x00000000u;
    dl[n++] = 0xDF000000u; dl[n++] = 0;
    for (int i = 0; i < n; i++) dl[i] = gdxn::Bswap32(dl[i]);
    alignas(8) static uint8_t seg6[512];
    memcpy(seg6, mtx, 64);
    memcpy(seg6 + 0x100, vtx, sizeof(vtx));
    gRanges.clear();
    Reg(dl, sizeof(dl), 8, true, false, true);
    Reg(seg6, sizeof(seg6), 8, false, false, false);
    gSegs[6] = reinterpret_cast<uintptr_t>(seg6);
    gRoot = reinterpret_cast<uintptr_t>(dl);
    gRootBytes = static_cast<size_t>(n) * 4;
    MockBackend mb;
    gdxn::Interp it(&mb);
    it.RunFrame(nullptr, Cfg(), 0);
    CHECK(it.Stats().clipTrunc == 0);
    CHECK(it.ClipPolyHw() >= 4);  /* the clipper really ran and really added vertices */
    CHECK(it.ClipPolyHw() <= 10); /* 3 + 7, the structural bound */
    printf("  [clip] polygon high-water %u of the 16-slot buffer\n", it.ClipPolyHw());
}

/* o3dsinterp (NEXT.md C4m): [native] o3dsfast + [native] trimemo must not move a single byte.
 * One big-endian scene that reaches every rewritten path -- the clipper (a triangle every plane
 * cuts), back-face culling, runs of same-state triangles (the memo's hit path), a sampler change
 * and a shader-input change between them (its miss path), overlapping TMEM loads at three bases
 * (the span store's retire + fill ranges), and a native-RGBA16 load whose 30-byte size leaves a
 * 2-byte tail after the word-wise swap. Two frames, so the span store and the memo cross a task.
 * Every lever combination must produce the same batches (vbo floats, aux, bound state, shader
 * ids) and the same uploads as the historic code (o3dsfast=0 trimemo=0). */
struct FastLeg {
    std::vector<MockBatch> batches;
    std::vector<MockUpload> uploads;
    uint32_t memoHits, fullResolves, clipIn, swapBytes, spanWords, emitted, clipAllOut, clipHw;
};
static int gTestNicensus = 0; /* [debug] nicensus leg of RunFastScene */
static FastLeg RunFastScene(int fast, int memo) {
    gdx_test_o3dsfast = fast;
    gdx_test_trimemo = memo;
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0] = 0x00010000u;
    mtx[2] = 0x00000001u;
    mtx[5] = 0x00010000u;
    mtx[7] = 0x00000001u;
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[11];
    memset(vtx, 0, sizeof(vtx));
    const int16_t ob[11][3] = { { -1, -1, 0 },     { 1, -1, 0 },     { 1, 1, 0 },    { -1, 1, 0 },
                                { -900, -700, 0 }, { 900, -700, 0 }, { 0, 900, 0 },  { 0, 0, 0 },
                                { 900, 0, 0 },     { 950, 10, 0 },   { 920, 50, 0 } };
    for (int v = 0; v < 11; v++) {
        for (int k = 0; k < 3; k++) {
            vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        }
        vtx[v].tc[0] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(v * 64)));
        vtx[v].tc[1] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(v * 32)));
        vtx[v].cn[0] = static_cast<uint8_t>(40 * v);
        vtx[v].cn[1] = 128;
        vtx[v].cn[2] = static_cast<uint8_t>(255 - 30 * v);
        vtx[v].cn[3] = 255;
    }
    static uint32_t dl[128];
    int n = 0;
    const auto cmd = [&](uint32_t w0, uint32_t w1) {
        dl[n++] = w0;
        dl[n++] = w1;
    };
    cmd(0xDA380003u, 0x06000000u);             /* MTX modelview load */
    cmd(0xDA380007u, 0x06000000u);             /* MTX projection load */
    cmd(0x0100B016u, 0x06000100u);             /* VTX n=11 v0=0 */
    cmd(0xD7000002u, 0xFFFFFFFFu);             /* TEXTURE on, scale 1, tile 0 */
    cmd(0xFC121824u, 0xFF33FFFFu);             /* combine (texel0) */
    cmd(0xD9FFFFFFu, 0x00000004u);             /* SHADE */
    cmd(0xFD10000Fu, 0x06000400u);             /* SETTIMG A: RGBA16 w=16 */
    cmd(0xF5100000u, 0x07000000u);             /* SETTILE 7 tmem 0 */
    cmd(0xF3000000u, 0x070FF800u);             /* LOADBLOCK 256 texels */
    cmd(0xF5100800u, 0x00000000u);             /* SETTILE 0 line 4 tmem 0 */
    cmd(0xF2000000u, 0x0003C03Cu);             /* SETTILESIZE 0: 16x16 */
    cmd(0x06000204u, 0x00000406u);             /* TRI2 (0,1,2) (0,2,3) */
    cmd(0x05020406u, 0x00000000u);             /* TRI1 (1,2,3) x3: same state */
    cmd(0x05020406u, 0x00000000u);
    cmd(0x05020406u, 0x00000000u);
    cmd(0xD9FFFFFFu, 0x00000400u);             /* CULL_BACK */
    cmd(0x05000204u, 0x00000000u);             /* (0,1,2) */
    cmd(0x05040200u, 0x00000000u);             /* (2,1,0): the other winding, culled */
    cmd(0xD9FFFBFFu, 0x00000000u);             /* clear CULL_BACK */
    cmd(0x05080A0Cu, 0x00000000u);             /* (4,5,6): every plane cuts it */
    cmd(0x05080A0Cu, 0x00000000u);
    cmd(0x05101214u, 0x00000000u);             /* (8,9,10): wholly outside x<=w -> all-out */
    cmd(0x050E0210u, 0x00000000u);             /* (7,1,8): straddles only the x<=w plane */
    cmd(0xFD10000Eu, 0x06000800u);             /* SETTIMG B: w=15, the native-RGBA16 range */
    cmd(0xF5100040u, 0x07000000u);             /* SETTILE 7 tmem 0x40 */
    cmd(0xF3000000u, 0x0700E800u);             /* LOADBLOCK 15 texels = 30 bytes */
    cmd(0xF5100840u, 0x00000000u);             /* SETTILE 0 line 4 tmem 0x40 */
    cmd(0xF2000000u, 0x00038000u);             /* SETTILESIZE 0: 15x1 */
    cmd(0x05000204u, 0x00000000u);
    cmd(0x05000406u, 0x00000000u);
    cmd(0xFD10000Fu, 0x06000400u);             /* A again, at tmem 0x20: overlaps both spans */
    cmd(0xF5100020u, 0x07000000u);
    cmd(0xF3000000u, 0x070FF800u);
    cmd(0xF5100820u, 0x00000000u);
    cmd(0xF2000000u, 0x0003C03Cu);
    cmd(0x05000406u, 0x00000000u);
    cmd(0x05020406u, 0x00000000u);
    cmd(0xE3001201u, 0x00002000u);             /* othermode_h: bilerp -> sampler change */
    cmd(0x05020406u, 0x00000000u);
    cmd(0x05000204u, 0x00000000u);
    cmd(0xF5100830u, 0x00000000u);             /* SETTILE 0 tmem 0x30: INSIDE the 0x20 span */
    cmd(0x05000204u, 0x00000000u);             /* -> LoadedTexAt's interior-view arm */
    cmd(0xFC127E24u, 0xFFFFF3F9u);             /* a second combiner */
    cmd(0x05000204u, 0x00000000u);
    cmd(0x05080A0Cu, 0x00000000u);
    cmd(0xDF000000u, 0x00000000u);
    for (int i = 0; i < n; i++) dl[i] = gdxn::Bswap32(dl[i]);
    alignas(8) static uint8_t seg6[0x1000];
    memset(seg6, 0, sizeof(seg6));
    memcpy(seg6, mtx, 64);
    memcpy(seg6 + 0x100, vtx, sizeof(vtx));
    for (int i = 0; i < 512; i++) seg6[0x400 + i] = static_cast<uint8_t>(i * 7 + 3);
    for (int i = 0; i < 64; i++) seg6[0x800 + i] = static_cast<uint8_t>(0xA0 + i * 5);
    gRanges.clear();
    Reg(dl, sizeof(dl), 8, true, false, true);
    Reg(seg6, sizeof(seg6), 8, false, false, false);
    gSegs[6] = reinterpret_cast<uintptr_t>(seg6);
    gdx_test_native_lo = reinterpret_cast<uintptr_t>(seg6 + 0x800);
    gdx_test_native_hi = gdx_test_native_lo + 64;
    gRoot = reinterpret_cast<uintptr_t>(dl);
    gRootBytes = static_cast<size_t>(n) * 4;
    MockBackend mb;
    FastLeg out;
    {
        gdxn::Interp it(&mb);
        if (gTestNicensus != 0) {
            it.NcSet(gTestNicensus == 2 ? 2 : 1);
            if (gTestNicensus == 2) {
                it.NsSet(1, 1); /* [debug] ni_strict=1 (count), a periodic receipt every frame */
            }
        }
        for (int frame = 0; frame < 2; frame++) {
            if (it.NcOn()) {
                it.NcFrameBegin(gdxn::Interp::kNcSubs * 1 + 2); /* gp_race / racing */
            }
            it.RunFrame(nullptr, Cfg(), 0);
            if (it.NcOn()) {
                const uint32_t zero[9] = {};
                it.NcFrameEnd(zero, zero, zero, 9);
            }
        }
        if (it.NcOn()) {
            it.NcDump("hosttest", 1.0);
            it.NsDump("hosttest", true); /* [debug] ni_strict: the full table (no-op when off) */
        }
        const gdxn::Counters& st = it.Stats();
        out.memoHits = st.triResolveMemo;
        out.fullResolves = st.triResolveFull;
        out.clipIn = st.triClipIn;
        out.swapBytes = st.tmemSwapBytes;
        out.spanWords = st.spanWords;
        out.emitted = st.triEmitAll;
        out.clipAllOut = st.triClipAllOut;
        out.clipHw = it.ClipPolyHw();
    }
    out.batches = mb.batches;
    out.uploads = mb.uploads;
    gSegs[6] = 0;
    gdx_test_native_lo = gdx_test_native_hi = 0;
    gdx_test_o3dsfast = 1;
    gdx_test_trimemo = 0;
    return out;
}

static bool SameBatches(const FastLeg& a, const FastLeg& b) {
    if (a.batches.size() != b.batches.size() || a.uploads.size() != b.uploads.size()) {
        return false;
    }
    for (size_t i = 0; i < a.batches.size(); i++) {
        const MockBatch& x = a.batches[i];
        const MockBatch& y = b.batches[i];
        if (x.id0 != y.id0 || x.id1 != y.id1 || x.tex[0] != y.tex[0] || x.tex[1] != y.tex[1] ||
            x.numTris != y.numTris || x.vbo.size() != y.vbo.size() || x.depthTest != y.depthTest ||
            x.depthMask != y.depthMask || x.decal != y.decal || x.alpha != y.alpha ||
            memcmp(x.vp, y.vp, sizeof(x.vp)) != 0 || memcmp(x.sc, y.sc, sizeof(x.sc)) != 0 ||
            memcmp(&x.aux, &y.aux, sizeof(x.aux)) != 0 || memcmp(&x.primDepth, &y.primDepth, sizeof(float)) != 0 ||
            memcmp(&x.alphaThr, &y.alphaThr, sizeof(float)) != 0 ||
            memcmp(x.vbo.data(), y.vbo.data(), x.vbo.size() * sizeof(float)) != 0) {
            printf("  [fastpath] batch %zu differs\n", i);
            return false;
        }
    }
    for (size_t i = 0; i < a.uploads.size(); i++) {
        if (a.uploads[i].w != b.uploads[i].w || a.uploads[i].h != b.uploads[i].h ||
            a.uploads[i].rgba != b.uploads[i].rgba) {
            printf("  [fastpath] upload %zu differs\n", i);
            return false;
        }
    }
    return true;
}

static void TestFastPathParity() {
    const FastLeg ref = RunFastScene(0, 0);
    CHECK(ref.clipIn > 0);      /* the clipper ran */
    CHECK(ref.swapBytes >= 30); /* the native-RGBA16 mirror ran, with a 2-byte tail */
    CHECK(ref.emitted > 8);
    CHECK(ref.memoHits == 0);
    CHECK(ref.clipAllOut >= 2); /* the wholly-outside triangle, both frames */
    CHECK(!ref.batches.empty() && ref.uploads.size() >= 2);
    const int legs[3][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (const auto& leg : legs) {
        const FastLeg got = RunFastScene(leg[0], leg[1]);
        const bool same = SameBatches(ref, got);
        CHECK(same);
        CHECK(got.spanWords == ref.spanWords);
        CHECK(got.clipAllOut == ref.clipAllOut && got.clipHw == ref.clipHw);
        if (leg[1] != 0) {
            CHECK(got.memoHits > 0); /* the memo really hit, and hitting changed nothing */
            CHECK(got.memoHits + got.fullResolves == ref.fullResolves);
        }
        printf("  [fastpath] o3dsfast=%d trimemo=%d: %zu batches, %zu uploads %s; resolves full=%u memo=%u\n",
               leg[0], leg[1], got.batches.size(), got.uploads.size(), same ? "IDENTICAL" : "DIFFER",
               got.fullResolves, got.memoHits);
    }
}

/* [debug] nicensus is measurement only: the census leg of the fast scene draws the same batches
 * and uploads, byte for byte, as the plain leg (the device proof is the fh/gstate pair in
 * nicensus-progress.md). */
static void TestNicensusParity() {
    const FastLeg ref = RunFastScene(0, 0);
    gTestNicensus = 1;
    const FastLeg got = RunFastScene(0, 0);
    gTestNicensus = 0;
    const bool same = SameBatches(ref, got);
    CHECK(same);
    CHECK(got.emitted == ref.emitted && got.spanWords == ref.spanWords);
    printf("  [nicensus] census leg: %zu batches, %zu uploads %s\n", got.batches.size(), got.uploads.size(),
           same ? "IDENTICAL" : "DIFFER");
    /* [debug] ni_strict=1 counts on the same engine and must not change what is drawn either */
    gTestNicensus = 2;
    const FastLeg strict = RunFastScene(0, 0);
    gTestNicensus = 0;
    const bool sameStrict = SameBatches(ref, strict);
    CHECK(sameStrict);
    CHECK(strict.emitted == ref.emitted && strict.spanWords == ref.spanWords);
    printf("  [nistrict] count leg: %zu batches, %zu uploads %s\n", strict.batches.size(), strict.uploads.size(),
           sameStrict ? "IDENTICAL" : "DIFFER");
}

/* [tmemblock] the widened same-content skip ([native] tmemskip), built on TestRectTexture's
 * scene because that test already documents the shipping behaviour it changes: "second
 * identical frame: span same-content skip is gated off (raw source) -> a load, a cache HIT".
 *
 * Three things are asserted, and the third is the one that matters:
 *   1. tmemskip=0 is byte-for-byte the shipping path: 2 loads, 0 skips.
 *   2. tmemskip=1 skips the second frame's mirror -- 2 loads, 1 skip.
 *   3. the skipped frame draws the SAME BYTES as the mirrored one. A skip that changed a
 *      pixel would show up here as a vbo/upload difference, and a skip that lost the texture
 *      identity would show up as a second upload.
 * Then the generation is moved under a NATIVE-class source and the skip must stop firing:
 * a gate that never says "stale" is not a gate. */
static void TestTmemSkipWidened() {
    static uint8_t texels[16 * 16 * 2];
    for (int i = 0; i < 256; i++) {
        const uint16_t c = static_cast<uint16_t>((31 << 11) | 1);
        texels[2 * i] = c >> 8;
        texels[2 * i + 1] = c & 0xFF;
    }
    static W root[16];
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u);
    root[n++] = Wv(0xFC121824u, 0xFF33FFFFu);
    root[n++] = Wp(0xFD10000Fu, texels);
    root[n++] = Wv(0xF5100000u, 0x07000000u);
    root[n++] = Wv(0xF3000000u, 0x070FF800u);
    root[n++] = Wv(0xF5100800u, 0x00000000u);
    root[n++] = Wv(0xF2000000u, 0x0003C03Cu);
    root[n++] = Wv(0xE4064064u, 0x00000000u);
    root[n++] = Wv(0xE1000000u, 0x00000000u);
    root[n++] = Wv(0xF1000000u, 0x04000400u);
    root[n++] = Wv(0xDF000000u, 0);
    const size_t rootBytes = static_cast<size_t>(n) * 16;

    struct Leg {
        int level;
        unsigned cls;
        uint32_t wantLoads, wantSkips;
        const char* what;
    };
    const Leg legs[] = {
        { 0, GDX_TMEMSRC_STABLE, 2, 0, "tmemskip=0 (shipping: immutable only)" },
        { 1, GDX_TMEMSRC_STABLE, 2, 1, "tmemskip=1, registered ROM-backed source" },
        { 1, GDX_TMEMSRC_NATIVE, 2, 1, "tmemskip=1, native-RGBA16 source" },
        { 1, GDX_TMEMSRC_DMA, 2, 0, "tmemskip=1 does NOT admit the DMA class" },
        { 2, GDX_TMEMSRC_DMA, 2, 1, "tmemskip=2 admits the DMA class" },
        { 1, GDX_TMEMSRC_UNKNOWN, 2, 0, "an UNKNOWN source is never skipped, at any level" },
    };
    std::vector<float> reference;
    for (const Leg& leg : legs) {
        gRanges.clear();
        Reg(root, sizeof(root), 16, false, false, true);
        Reg(texels, sizeof(texels), 8, false, false, false);
        gRoot = reinterpret_cast<uintptr_t>(root);
        gRootBytes = rootBytes;
        gdx_test_tmemskip = leg.level;
        gdx_test_src_cls = leg.cls;
        gdx_test_native_gen = 1;
        gdx_test_dma_gen = 1;
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        it.RunFrame(nullptr, Cfg(), 0);
        const gdxn::Counters& s = it.Stats();
        if (s.tmemLoads != leg.wantLoads || s.tmemSkips != leg.wantSkips) {
            printf("  [tmemskip] %s: loads=%u skips=%u (want %u/%u)\n", leg.what, s.tmemLoads, s.tmemSkips,
                   leg.wantLoads, leg.wantSkips);
        }
        CHECK(s.tmemLoads == leg.wantLoads);
        CHECK(s.tmemSkips == leg.wantSkips);
        /* One upload for two frames in every leg: the second frame must reach the SAME texture
           cache entry whether the mirror was rewritten or skipped. */
        CHECK(mb.uploads.size() == 1);
        CHECK(mb.batches.size() == 2);
        if (mb.batches.size() == 2) {
            CHECK(memcmp(mb.batches[0].vbo.data(), mb.batches[1].vbo.data(), mb.batches[0].vbo.size() * 4) == 0);
        }
        /* And the same bytes as every other leg: a skip must not change what is drawn. */
        if (reference.empty()) {
            reference = mb.batches[0].vbo;
        } else {
            CHECK(reference.size() == mb.batches[0].vbo.size());
            CHECK(memcmp(reference.data(), mb.batches[0].vbo.data(), reference.size() * 4) == 0);
        }
    }

    /* The gate must be able to say STALE. Move the generation the NATIVE class keys on between
       the two frames and the skip has to stop firing -- otherwise the test above proves only
       that a constant returns a constant. */
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = rootBytes;
    gdx_test_tmemskip = 1;
    gdx_test_src_cls = GDX_TMEMSRC_NATIVE;
    gdx_test_native_gen = 1;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        gdx_test_native_gen = 2; /* a range registration / clear landed between the frames */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().tmemLoads == 2);
        CHECK(it.Stats().tmemSkips == 0);
        /* Third frame, generation held still again: the skip comes back. */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().tmemLoads == 3);
        CHECK(it.Stats().tmemSkips == 1);
    }

    /* A CLASS change alone invalidates, even with both generations standing still: a stamp
       taken under one class says nothing about another. */
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = rootBytes;
    gdx_test_tmemskip = 2;
    gdx_test_src_cls = GDX_TMEMSRC_NATIVE;
    gdx_test_native_gen = 1;
    gdx_test_dma_gen = 1;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        gdx_test_src_cls = GDX_TMEMSRC_DMA;
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().tmemSkips == 0);
    }
    gdx_test_tmemskip = 0;
    gdx_test_src_cls = GDX_TMEMSRC_STABLE;
}

/* [memotear] NEXT.md B54 -- the INJECTOR itself, tested before it is trusted in the guest.
 *
 * The two arms above prove the gate CAN say stale when the test drives the mock generations by
 * hand. B54 is the different claim that the shipping fault injector ([debug] tmemfault) drives
 * them, on real content, from inside the walk -- because the corpus never once reached the
 * refusal arm (412,326 skips, stale=0), so the only thing that has ever exercised it is a hand
 * -written frame. A fault injector that silently injected nothing would make the guest run say
 * `stale=0` too, and look exactly like a clean gate. So: every injection is caught, none is
 * missed, and the pixels do not move. */
static void TestTmemFaultInjection() {
    static uint8_t texels[16 * 16 * 2];
    for (int i = 0; i < 256; i++) {
        const uint16_t c = static_cast<uint16_t>((31 << 11) | 1);
        texels[2 * i] = c >> 8;
        texels[2 * i + 1] = c & 0xFF;
    }
    static W root[16];
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u);
    root[n++] = Wv(0xFC121824u, 0xFF33FFFFu);
    root[n++] = Wp(0xFD10000Fu, texels);
    root[n++] = Wv(0xF5100000u, 0x07000000u);
    root[n++] = Wv(0xF3000000u, 0x070FF800u);
    root[n++] = Wv(0xF5100800u, 0x00000000u);
    root[n++] = Wv(0xF2000000u, 0x0003C03Cu);
    root[n++] = Wv(0xE4064064u, 0x00000000u);
    root[n++] = Wv(0xE1000000u, 0x00000000u);
    root[n++] = Wv(0xF1000000u, 0x04000400u);
    root[n++] = Wv(0xDF000000u, 0);
    const size_t rootBytes = static_cast<size_t>(n) * 16;

    /* Both admitted classes, because they fail differently: NATIVE has a generation arm and
       STABLE has none at all (gdx_gfx_tmem_source_unchanged answers it with an unconditional
       1), so STABLE's only refusal is the class arm and the injector must fall back to it. */
    const unsigned classes[] = { GDX_TMEMSRC_NATIVE, GDX_TMEMSRC_STABLE };
    std::vector<float> reference;
    for (unsigned cls : classes) {
        /* Leg A: the lever on, the injector OFF -- the skip fires and nothing is stale. */
        uint32_t cleanSkips = 0;
        for (int inject = 0; inject <= 1; inject++) {
            gRanges.clear();
            Reg(root, sizeof(root), 16, false, false, true);
            Reg(texels, sizeof(texels), 8, false, false, false);
            gRoot = reinterpret_cast<uintptr_t>(root);
            gRootBytes = rootBytes;
            gdx_test_tmemskip = 1;
            gdx_test_src_cls = cls;
            gdx_test_native_gen = 1;
            gdx_test_dma_gen = 1;
            gdx_test_tmemfault = inject ? 1 : 0; /* 1 = every admitted skip */
            MockBackend mb;
            gdxn::Interp it(&mb);
            for (int f = 0; f < 4; f++) {
                it.RunFrame(nullptr, Cfg(), 0);
            }
            const gdxn::Counters& st = it.Stats();
            if (inject == 0) {
                cleanSkips = st.tmemSkips;
                CHECK(cleanSkips > 0);          /* the lever really does skip on this class */
                CHECK(st.tmemStale == 0);       /* ...and nothing was stale */
                CHECK(st.tmemFaultGen == 0 && st.tmemFaultCls == 0);
            } else {
                const uint32_t injections = st.tmemFaultGen + st.tmemFaultCls;
                if (injections == 0 || st.tmemFaultMissed != 0 || st.tmemFaultCaught != injections) {
                    printf("  [tmemfault] cls=%u inj=%u/%u caught=%u missed=%u stale=%u skips=%u\n", cls,
                           st.tmemFaultGen, st.tmemFaultCls, st.tmemFaultCaught, st.tmemFaultMissed,
                           st.tmemStale, st.tmemSkips);
                }
                /* It injected something... */
                CHECK(injections > 0);
                /* ...the gate REFUSED every one of them... */
                CHECK(st.tmemFaultCaught == injections);
                CHECK(st.tmemFaultMissed == 0);
                CHECK(st.tmemStale >= injections);
                /* ...and the refusals really removed skips, so the injector is not a counter
                   that increments beside a gate that was going to refuse anyway. */
                CHECK(st.tmemSkips < cleanSkips);
                /* NATIVE has a generation arm and must use it; STABLE has none and must fall
                   back to the class arm rather than report a fault it did not cause. */
                if (cls == GDX_TMEMSRC_NATIVE) {
                    CHECK(st.tmemFaultGen > 0);
                } else {
                    CHECK(st.tmemFaultGen == 0 && st.tmemFaultCls > 0);
                }
            }
            /* THE FRAME STILL DRAWS CORRECTLY -- the whole second half of B54. A refusal
               re-mirrors, which is what tmemskip=0 does every load, so every leg of every class
               must emit the same vertex bytes. */
            CHECK(!mb.batches.empty());
            if (reference.empty()) {
                reference = mb.batches[0].vbo;
            } else if (!mb.batches.empty()) {
                CHECK(reference.size() == mb.batches[0].vbo.size());
                CHECK(memcmp(reference.data(), mb.batches[0].vbo.data(), reference.size() * 4) == 0);
            }
            for (size_t b = 1; b < mb.batches.size(); b++) {
                CHECK(mb.batches[b].vbo.size() == mb.batches[0].vbo.size());
                CHECK(memcmp(mb.batches[b].vbo.data(), mb.batches[0].vbo.data(),
                             mb.batches[0].vbo.size() * 4) == 0);
            }
        }
    }

    /* The injector is inert with the lever off: it is only ever reached from an admitted skip,
       and tmemskip=0 admits none. This is what keeps it out of every shipped configuration. */
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = rootBytes;
    gdx_test_tmemskip = 0;
    gdx_test_tmemfault = 1;
    gdx_test_src_cls = GDX_TMEMSRC_NATIVE;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().tmemFaultGen == 0);
        CHECK(it.Stats().tmemFaultCls == 0);
        CHECK(it.Stats().tmemStale == 0);
    }
    gdx_test_tmemfault = 0;
    gdx_test_tmemskip = 0;
    gdx_test_src_cls = GDX_TMEMSRC_STABLE;
}

/* [native] hud_direct (C5 Stage 2b, docs/research/native/c5s2b-progress.md): one HUD run replayed
 * through the 0x5E marker with the lever off, on and in verify, three frames each. The direct
 * path defers every LOADBLOCK's TMEM copy as a debt, memoises the TMEM hash per source and emits
 * the rectangles itself -- and must still draw the SAME batches (vertex floats, aux, state), make
 * the same uploads, and leave TMEM and the span store byte-identical at the task end. The scene
 * mixes textures of different sizes at TMEM 0 (a small load over a large one leaves the large
 * one's tail as a partial debt), a flipped rectangle, a CI8 texture with its TLUT, prim colour
 * changes between rectangles, a fill rectangle, and -- after the run, outside it -- a rectangle
 * that samples TMEM no load in its own list wrote, so the debt must be paid to the stock bytes. */
struct HudLeg {
    std::vector<MockBatch> batches;
    std::vector<MockUpload> uploads;
    std::vector<uint8_t> tmem;
    std::vector<uint16_t> spanBase;
    uint32_t tmemCopies = 0;
    char receipt[512] = {};
};

static GdxHudWord HudW(uint32_t w0, uint32_t w1) {
    GdxHudWord w;
    w.w0 = w0;
    w.pad = 0;
    w.w1 = w1;
    w.w1hi = 0;
    return w;
}
static GdxHudWord HudP(uint32_t w0, const void* p) {
    GdxHudWord w = HudW(w0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)));
    w.w1hi = gdxn::kHostTag;
    return w;
}

static void HudNoop(void* user, uint32_t w0, uint32_t, gdxn::Interp* it) {
    if (((w0 >> 16) & 0xFFu) == GDXHUD_MARKER_P) {
        it->HudReplayRun(user, w0 & 0xFFFFu); /* the host pointer rides in `user` (w1 is 32-bit) */
    }
}

static HudLeg RunHudScene(int direct, GdxHudRecord* rec, const W* root, size_t rootN, size_t poolTris = 0) {
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = rootN * 16;
    HudLeg out;
    MockBackend mb;
    if (poolTris != 0) {
        mb.pool.assign(poolTris * 3 * 12, 0.0f); /* GEOMLOSS pressure: batches cap (and split) early */
    }
    {
        gdxn::Interp it(&mb);
        it.RegisterNoopHook(HudNoop, rec);
        it.HudSetDirect(direct);
        for (int frame = 0; frame < 3; frame++) {
            memset(rec->rp, 0, sizeof(rec->rp));
            it.RunFrame(nullptr, Cfg(), 0);
            it.HudDirectTaskEnd();
        }
        out.tmem.assign(it.RdpState().tmem, it.RdpState().tmem + sizeof(it.RdpState().tmem));
        out.spanBase.assign(it.RdpState().tmem_span_base, it.RdpState().tmem_span_base + 512);
        out.tmemCopies = it.Stats().tmemCopyBytes;
        it.HudDirectFormat(out.receipt, sizeof(out.receipt), 3);
    }
    out.batches = mb.batches;
    out.uploads = mb.uploads;
    return out;
}

static void TestHudDirectParity() {
    static uint8_t texA[16 * 16 * 2], texB[32 * 8 * 2], texC[8 * 8 * 2], texD[16 * 8], texE[8 * 8], pal[256 * 2];
    for (unsigned i = 0; i < sizeof(texA); i++) texA[i] = static_cast<uint8_t>(i * 7 + 1);
    for (unsigned i = 0; i < sizeof(texB); i++) texB[i] = static_cast<uint8_t>(i * 13 + 5);
    for (unsigned i = 0; i < sizeof(texC); i++) texC[i] = static_cast<uint8_t>(i * 29 + 3);
    for (unsigned i = 0; i < sizeof(texD); i++) texD[i] = static_cast<uint8_t>(i * 3 + 11);
    for (unsigned i = 0; i < sizeof(texE); i++) texE[i] = static_cast<uint8_t>(i % 5);
    for (unsigned i = 0; i < sizeof(pal); i++) pal[i] = static_cast<uint8_t>(i * 11 + 1);

    static GdxHudRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = GDXHUD_MAGIC;
    uint32_t n = 0;
    auto put = [&](GdxHudWord w) { rec.words[n++] = w; };
    auto rect = [&](uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry, uint32_t st, uint32_t d, bool flip) {
        put(HudW((flip ? 0xE5000000u : 0xE4000000u) | (lrx << 12) | lry, (ulx << 12) | uly));
        put(HudW(0xE1000000u, st));
        put(HudW(0xF1000000u, d));
    };
    /* texture group: SETTIMG, SETTILE 7, LOADBLOCK 7, SETTILE 0, SETTILESIZE 0 (gDPLoadTextureBlock) */
    auto group = [&](uint32_t fmtSiz, const void* src, uint32_t lrs, uint32_t dxt, uint32_t renderFmtSiz,
                     uint32_t line, uint32_t w, uint32_t h) {
        put(HudP(0xFD000000u | fmtSiz, src));
        put(HudW(0xF5000000u | fmtSiz, 0x07000000u));
        put(HudW(0xF3000000u, 0x07000000u | (lrs << 12) | dxt));
        put(HudW(0xF5000000u | renderFmtSiz | (line << 9), 0x00000000u));
        put(HudW(0xF2000000u, ((w - 1) << 14) | ((h - 1) << 2)));
    };
    put(HudW(0xE3001001u, 0x00000000u));
    put(HudW(0xFC119623u, 0xFF2FFFFFu)); /* (TEXEL0 - 0) * PRIM + 0, both cycles */
    put(HudW(0xFA000000u, 0xFF8040FFu));
    group(0x100000u, texA, 255, 0x800, 0x100000u, 4, 16, 16); /* RGBA16 16x16, 512 B */
    rect(40, 40, 104, 104, 0, 0x04000400u, false);
    put(HudW(0xFA000000u, 0x40FF80C0u));
    rect(120, 40, 184, 104, 0, 0x04000400u, false);
    group(0x100000u, texC, 63, 0x800, 0x100000u, 2, 8, 8); /* RGBA16 8x8, 128 B: A's tail stays owed */
    rect(200, 40, 232, 72, 0, 0x04000400u, false);
    rect(240, 40, 272, 72, 0, 0x04000400u, true);
    group(0x100000u, texB, 255, 0x400, 0x100000u, 8, 32, 8); /* RGBA16 32x8 */
    rect(40, 120, 168, 152, 0, 0x04000400u, false);
    group(0x480000u, texD, 63, 0x800, 0x880000u, 2, 16, 8); /* IA8 16x8 loaded as 16b */
    rect(40, 160, 104, 192, 0, 0x04000400u, false);
    /* CI8 8x8 with a 256-entry TLUT at TMEM 256 */
    put(HudP(0xFD100000u, pal));
    put(HudW(0xF5000100u, 0x07000000u));
    put(HudW(0xF0000000u, 0x073FC000u));
    put(HudW(0xE3001001u, 0x00008000u));
    group(0x500000u, texE, 31, 0x800, 0x480000u, 1, 8, 8);
    rect(120, 160, 152, 192, 0, 0x04000400u, false);
    put(HudW(0xE3001001u, 0x00000000u));
    group(0x100000u, texC, 63, 0x800, 0x100000u, 2, 8, 8); /* texC again: memo + cache hit */
    rect(160, 160, 192, 192, 0, 0x04000400u, false);
    put(HudW(0xF7000000u, 0xF801F801u));
    put(HudW(0xF6000000u | (300u << 12) | 230u, (280u << 12) | 220u)); /* fill rect, stock */
    rec.nWords = n;
    rec.runs[0].start = 0;
    rec.runs[0].end = static_cast<uint16_t>(n);
    rec.runs[0].kind = GDXHUD_KIND_HUD;
    rec.runs[0].mode = 1;
    rec.nRuns = 1;

    /* The root: the marker, then a rectangle that re-reads TMEM words 32.. (texA's tail, still
     * owed after texC's smaller load) with no load of its own -- the stock bytes or bust. */
    static W root[16];
    int r = 0;
    root[r++] = Wv(0xE7000000u, 0);
    root[r++] = Wv(0x005E0000u, 0);
    root[r++] = Wv(0xF5100420u, 0x00000000u); /* SETTILE RGBA16 line 2 (16 B) tmem 32 tile 0 */
    root[r++] = Wv(0xF2000000u, 0x0001C01Cu); /* SETTILESIZE 8x8 */
    root[r++] = Wv(0xE4000000u | (60u << 12) | 60u, (20u << 12) | 20u);
    root[r++] = Wv(0xE1000000u, 0);
    root[r++] = Wv(0xF1000000u, 0x04000400u);
    root[r++] = Wv(0xDF000000u, 0);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texA, sizeof(texA), 8, false, false, false);
    Reg(texB, sizeof(texB), 8, false, false, false);
    Reg(texC, sizeof(texC), 8, false, false, false);
    Reg(texD, sizeof(texD), 8, false, false, false);
    Reg(texE, sizeof(texE), 8, false, false, false);
    Reg(pal, sizeof(pal), 8, false, false, false);
    gdx_test_native_lo = reinterpret_cast<uintptr_t>(texB); /* texB is native-RGBA16: the swapped debt */
    gdx_test_native_hi = gdx_test_native_lo + sizeof(texB);
    const unsigned clsSaved = gdx_test_src_cls;
    gdx_test_src_cls = GDX_TMEMSRC_DMA;

    const HudLeg off = RunHudScene(0, &rec, root, static_cast<size_t>(r));
    const HudLeg on = RunHudScene(1, &rec, root, static_cast<size_t>(r));
    const HudLeg vfy = RunHudScene(2, &rec, root, static_cast<size_t>(r));
    printf("  [hudd] on:  %s\n  [hudd] vfy: %s\n", on.receipt, vfy.receipt);
    FastLeg a, b, c;
    a.batches = off.batches;
    a.uploads = off.uploads;
    b.batches = on.batches;
    b.uploads = on.uploads;
    c.batches = vfy.batches;
    c.uploads = vfy.uploads;
    CHECK(!off.batches.empty() && off.uploads.size() >= 5);
    CHECK(SameBatches(a, b));
    CHECK(SameBatches(a, c));
    CHECK(off.tmem == on.tmem && off.spanBase == on.spanBase);
    CHECK(off.tmem == vfy.tmem && off.spanBase == vfy.spanBase);
    CHECK(on.tmemCopies < off.tmemCopies);           /* the loads really were deferred */
    CHECK(strstr(on.receipt, "stale=0 ses=0") != nullptr);
    CHECK(strstr(vfy.receipt, "ses_bad=0") != nullptr);
    CHECK(strstr(vfy.receipt, "rect=0/") == nullptr); /* the rectangle check really ran */
    printf("  [hudd] off/on/verify: %zu batches, %zu uploads; tmem copies %u -> %u bytes\n", off.batches.size(),
           off.uploads.size(), off.tmemCopies, on.tmemCopies);

    /* Pool pressure: the batch caps at an odd triangle count, so a rectangle's two triangles land
       in two batches (the second one packed again under the new batch's parameters), and later
       begins are refused outright. Identical either way. */
    for (size_t poolTris : { size_t(5), size_t(9), size_t(15) }) {
        const HudLeg p0 = RunHudScene(0, &rec, root, static_cast<size_t>(r), poolTris);
        const HudLeg p1 = RunHudScene(1, &rec, root, static_cast<size_t>(r), poolTris);
        FastLeg x, y;
        x.batches = p0.batches;
        x.uploads = p0.uploads;
        y.batches = p1.batches;
        y.uploads = p1.uploads;
        CHECK(SameBatches(x, y));
        CHECK(p0.tmem == p1.tmem && p0.spanBase == p1.spanBase);
        printf("  [hudd] pool %zu tris: %zu batches %s\n", poolTris, p0.batches.size(),
               SameBatches(x, y) ? "IDENTICAL" : "DIFFER");
    }

    /* The source moves between the deferred load and the payment: the payment is counted STALE
       (the tripwire), never silently taken as the old bytes. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RegisterNoopHook(HudNoop, &rec);
        it.HudSetDirect(1);
        gRoot = reinterpret_cast<uintptr_t>(root);
        gRootBytes = 2 * 16; /* the marker only: nothing reads TMEM before the task end */
        memset(rec.rp, 0, sizeof(rec.rp));
        it.RunFrame(nullptr, Cfg(), 0);
        gdx_test_dma_gen++;
        it.HudDirectTaskEnd();
        char rc[512] = {};
        it.HudDirectFormat(rc, sizeof(rc), 1);
        CHECK(strstr(rc, "stale=0") == nullptr);
        printf("  [hudd] moved source: %s\n", rc);
    }
    gdx_test_src_cls = clsSaved;
    gdx_test_native_lo = gdx_test_native_hi = 0;
}

/* [native] dlmemo + deadsetup (c5dlmemo): Racer_Draw's effect-class setup lists with no instance,
 * back to back -- the seven lists' command shapes as they are in hud_gfx (the o2r's OTR SETTIMG
 * form spelled as a plain G_SETTIMG), four 512-byte loads of two textures at TMEM 0 -- then
 * textured triangles that read every register the window leaves. Three frames per leg (the memo
 * fills on the first, the window is planned on the next two). Every leg must draw the same
 * batches and uploads as the plain walk and leave the same end state; the skip leg must really
 * skip lists and loads, and the verify leg must find 0 mismatches. Then a mutation: the window's
 * last list loses its combiner, so the earlier combiners stop being dead -- the planner must see
 * it (same pixels again), and moving the memo's tables version must stop the memo hitting. */
struct DeadLeg {
    std::vector<MockBatch> batches;
    std::vector<MockUpload> uploads;
    uint32_t state[64];
    unsigned stateN;
    uint32_t ses[7];
    uint32_t memoHits;
};
/* variant 0: the hud_gfx shapes. 1: the last list without its combiner (earlier combiners
   come alive). 2: SMOKE loads 1 KB at TMEM word 0x80 (not the next load's range: live, and its
   load tile's SETTILE is read by it). 3: a tail list after FALLX puts tiles 0/1 back to their
   earlier values -- textures_changed then depends on FALLX's value-gated tile writes.
   4: triangles are pending when the window opens. */
/* midMutate: before the fourth frame FALLX's combiner becomes a sync -- a list the memo and the
   plan cache both hold changes under them, and CSPARK's combiner comes alive. */
static DeadLeg RunDeadScene(int dlmemo, int deadsetup, int variant, bool bumpVersion, bool midMutate = false) {
    const bool mutate = variant == 1;
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0] = 0x00010000u;
    mtx[2] = 0x00000001u;
    mtx[5] = 0x00010000u;
    mtx[7] = 0x00000001u;
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[4];
    memset(vtx, 0, sizeof(vtx));
    const int16_t ob[4][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 } };
    for (int v = 0; v < 4; v++) {
        for (int k = 0; k < 3; k++) {
            vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        }
        vtx[v].tc[0] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(v * 64)));
        vtx[v].tc[1] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(v * 32)));
        vtx[v].cn[0] = static_cast<uint8_t>(60 * v);
        vtx[v].cn[1] = 128;
        vtx[v].cn[2] = 200;
        vtx[v].cn[3] = 255;
    }
    /* the seven setup lists, big-endian narrow, in segment 7 */
    static uint32_t subs[8][72];
    static uint32_t subOff[8];
    uint32_t* all = &subs[0][0];
    memset(subs, 0, sizeof(subs));
    int list = 0, n = 0;
    const auto cmd = [&](uint32_t w0, uint32_t w1) {
        subs[list][n++] = w0;
        subs[list][n++] = w1;
    };
    const auto end = [&]() {
        cmd(0xDF000000u, 0);
        subOff[list] = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(&subs[list][0]) - reinterpret_cast<uint8_t*>(all));
        list++;
        n = 0;
    };
    /* EXPL */
    cmd(0xD9FFFFFEu, 0); cmd(0xE7000000u, 0); cmd(0xE200001Cu, 0x00504240u); end();
    /* DEBRIS */
    cmd(0xD7000000u, 0x80008000u); cmd(0xE7000000u, 0); cmd(0xE200001Cu, 0x0F0A4000u);
    cmd(0xFC65FECBu, 0x44FE7D3Eu); end();
    /* FSPARK */
    cmd(0xD9FFFFFFu, 0x00200000u); cmd(0xE7000000u, 0); cmd(0xE200001Cu, 0x00504240u);
    cmd(0xFCFFFFFFu, 0xFFFE793Cu); end();
    /* CSPARK: modes, a 512-byte load of texture A at TMEM 0, all eight tiles */
    cmd(0xD9000000u, 0); cmd(0xD9FFFFFFu, 0x00000004u); cmd(0xE7000000u, 0); cmd(0xE3000A01u, 0);
    cmd(0xE200001Cu, 0x00504240u); cmd(0xFC309A61u, 0x5536FF7Fu); cmd(0xE3000800u, 0); cmd(0xE2001E01u, 0);
    cmd(0xE3001001u, 0); cmd(0xE3000C00u, 0x00080000u); cmd(0xE3001201u, 0x00002000u);
    cmd(0xF5900000u, 0x00080200u); cmd(0xE6000000u, 0); cmd(0xFD900000u, 0x06000400u);
    cmd(0xF3000000u, 0x000FF400u); cmd(0xE7000000u, 0);
    for (uint32_t t = 0; t < 8; t++) {
        cmd(0xF5800400u | (t * 0x40u), (t << 24) | 0x00080200u);
        cmd(0xF2000000u, (t << 24) | 0x0007C07Cu);
    }
    end();
    /* SMOKE: texture B, same range */
    cmd(0xE7000000u, 0); cmd(0xFA000000u, 0xFFFFC8FFu);
    if (variant == 2) {
        cmd(0xF5900080u, 0x00080200u); cmd(0xE6000000u, 0);
        cmd(0xFD900000u, 0x06000600u); cmd(0xF3000000u, 0x001FF400u); cmd(0xE7000000u, 0);
    } else {
        cmd(0xF5900000u, 0x00080200u); cmd(0xE6000000u, 0);
        cmd(0xFD900000u, 0x06000600u); cmd(0xF3000000u, 0x000FF400u); cmd(0xE7000000u, 0);
    }
    cmd(0xF5800400u, 0x00080200u); cmd(0xF2000000u, 0x0007C07Cu); end();
    /* PITFF: texture A, same range */
    cmd(0xE7000000u, 0); cmd(0xFA000000u, 0xFF32C8FFu); cmd(0xFB000000u, 0x0064FFFFu);
    cmd(0xF5900000u, 0x00080200u); cmd(0xE6000000u, 0); cmd(0xFD900000u, 0x06000400u);
    cmd(0xF3000000u, 0x000FF400u); cmd(0xE7000000u, 0); cmd(0xF5800400u, 0x00080200u);
    cmd(0xF2000000u, 0x0007C07Cu); end();
    /* FALLX: texture B, same range; the combiner is the mutation's target */
    cmd(0xD9000000u, 0); cmd(0xD9FFFFFFu, 0x00000004u); cmd(0xE7000000u, 0); cmd(0xE3001201u, 0x00002000u);
    cmd(0xF5900000u, 0x00080200u); cmd(0xE6000000u, 0); cmd(0xFD900000u, 0x06000600u);
    if (mutate) {
        cmd(0xE7000000u, 0);
    } else {
        cmd(0xFC3087D3u, 0x0BF866AFu);
    }
    cmd(0xF3000000u, 0x000FF200u); cmd(0xE7000000u, 0); cmd(0xF5800800u, 0x00058160u);
    cmd(0xF2000000u, 0x000FC0FCu); cmd(0xF5800900u, 0x01058160u); cmd(0xF2000000u, 0x010FC0FCu); end();
    int nLists = 7;
    if (variant == 3) {
        /* tiles 0 and 1 back to the values they held before FALLX's post-load writes */
        cmd(0xF5900000u, 0x00080200u); cmd(0xF2000000u, 0x0007C07Cu);
        cmd(0xF5480035u, 0x010A5A5Au); cmd(0xF2004008u, 0x01123456u); end();
        nLists = 8;
    }
    for (size_t i = 0; i < sizeof(subs) / 4; i++) all[i] = gdxn::Bswap32(all[i]);

    static uint32_t dl[160];
    int m = 0;
    const auto rc = [&](uint32_t w0, uint32_t w1) {
        dl[m++] = w0;
        dl[m++] = w1;
    };
    rc(0xDA380003u, 0x06000000u);
    rc(0xDA380007u, 0x06000000u);
    rc(0x01004008u, 0x06000100u); /* VTX n=4 */
    /* the parent leaves every register the window writes at a value the window does not, and a
       256-byte span at TMEM word 0x20 inside the window's load range: a command the planner
       wrongly drops leaves one of these behind */
    if (variant == 4) {
        /* triangles pending when the window opens: prim/env changes split the batch */
        rc(0xD7000002u, 0xFFFFFFFFu);
        rc(0x06000204u, 0x00000406u);
    }
    rc(0xD9FFFFFFu, 0x00FF3F0Fu);
    rc(0xEF00F0F0u, 0x1234ABCDu);
    rc(0xFC111111u, 0x22222222u);
    rc(0xFA000000u, 0x11223344u);
    rc(0xFB000000u, 0x55667788u);
    rc(0xD7000A02u, 0x12344321u);
    rc(0xFD900000u, 0x08000000u); /* the whole of TMEM from segment 8, every frame */
    rc(0xF5900000u, 0x00000000u);
    rc(0xF3000000u, 0x007FF400u);
    rc(0xFD900000u, 0x06000800u);
    rc(0xF5900020u, 0x00000000u);
    rc(0xF3000000u, 0x0007F400u); /* 128 texels x 2 = 256 bytes at word 0x20 */
    for (uint32_t t = 0; t < 8; t++) {
        rc(0xF5480000u | (t * 0x30u + 5u), (t << 24) | 0x000A5A5Au);
        rc(0xF2004008u, (t << 24) | 0x00123456u);
    }
    for (int k = 0; k < nLists; k++) {
        rc(0x00620000u | static_cast<uint32_t>(10 + k), 0); /* [c5sub] marker between the lists */
        rc(0xDE000000u, 0x07000000u | subOff[k]);
    }
    rc(0xD7000002u, 0xFFFFFFFFu); /* TEXTURE on, tile 0 */
    rc(0x06000204u, 0x00000406u); /* TRI2 */
    rc(0xFC121824u, 0xFF33FFFFu);
    rc(0x06000204u, 0x00000406u);
    rc(0xDF000000u, 0);
    for (int i = 0; i < m; i++) dl[i] = gdxn::Bswap32(dl[i]);

    alignas(8) static uint8_t seg6[0x1000];
    memset(seg6, 0, sizeof(seg6));
    memcpy(seg6, mtx, 64);
    memcpy(seg6 + 0x100, vtx, sizeof(vtx));
    for (int i = 0; i < 0x200; i++) {
        seg6[0x400 + i] = static_cast<uint8_t>(i * 7 + 3);
        seg6[0x600 + i] = static_cast<uint8_t>(0xF0 - i * 3);
        seg6[0x800 + i] = static_cast<uint8_t>(0x5A ^ i);
        seg6[0xA00 + i] = static_cast<uint8_t>(0x33 + i * 11);
    }
    alignas(8) static uint8_t seg8[0x1000];
    for (int i = 0; i < 0x1000; i++) {
        seg8[i] = static_cast<uint8_t>(0xC3 ^ (i * 13));
    }
    gRanges.clear();
    Reg(dl, sizeof(dl), 8, true, false, true);
    Reg(subs, sizeof(subs), 8, true, false, true);
    Reg(seg6, sizeof(seg6), 8, false, false, false);
    Reg(seg8, sizeof(seg8), 8, false, false, false);
    gSegs[6] = reinterpret_cast<uintptr_t>(seg6);
    gSegs[7] = reinterpret_cast<uintptr_t>(subs);
    gSegs[8] = reinterpret_cast<uintptr_t>(seg8);
    gRoot = reinterpret_cast<uintptr_t>(dl);
    gRootBytes = static_cast<size_t>(m) * 4;
    MockBackend mb;
    DeadLeg out;
    {
        gdxn::Interp it(&mb);
        it.SetDlMemo(dlmemo);
        it.SetDeadSetup(deadsetup);
        for (int frame = 0; frame < 5; frame++) {
            if (bumpVersion) {
                gdx_test_memo_version++;
            }
            /* segment 1 alternates like the double-buffered GfxPool: two memo generations, so
               the plan built under one is reused under the other (word-for-word check) */
            gSegs[1] = (frame & 1) != 0 ? 0x1000u : 0x2000u;
            if (midMutate && frame == 3) {
                for (int w = 0; w + 1 < 72; w += 2) {
                    if (gdxn::Bswap32(subs[6][w]) == 0xFC3087D3u) {
                        subs[6][w] = gdxn::Bswap32(0xE7000000u);
                        subs[6][w + 1] = 0;
                    }
                }
            }
            it.RunFrame(nullptr, Cfg(), 0);
        }
        out.stateN = it.StateSnapshot(out.state, 64);
        it.DeadSetSession(out.ses);
        out.memoHits = it.DlMemoSessionHits();
    }
    out.batches = mb.batches;
    out.uploads = mb.uploads;
    gSegs[1] = gSegs[6] = gSegs[7] = gSegs[8] = 0;
    return out;
}

static bool SameDeadLeg(const DeadLeg& a, const DeadLeg& b) {
    FastLeg x, y;
    x.batches = a.batches;
    x.uploads = a.uploads;
    y.batches = b.batches;
    y.uploads = b.uploads;
    return SameBatches(x, y) && a.stateN == b.stateN && memcmp(a.state, b.state, a.stateN * 4) == 0;
}

static void TestDeadSetup() {
    for (int mut = 0; mut < 5; mut++) {
        const int mutate = mut;
        const DeadLeg ref = RunDeadScene(0, 0, mutate, false);
        CHECK(!ref.batches.empty() && (mutate == 3 || !ref.uploads.empty())); /* 3 samples a load tile */
        const DeadLeg memo = RunDeadScene(1, 0, mutate, false);
        CHECK(SameDeadLeg(ref, memo));
        CHECK(memo.memoHits >= 21); /* seven or eight lists, warm from the third frame (two generations) */
        const DeadLeg skip = RunDeadScene(1, 1, mutate, false);
        const bool same = SameDeadLeg(ref, skip);
        CHECK(same);
        CHECK(skip.ses[0] == 3);  /* one window per warm frame (frames 3-5) */
        CHECK(skip.ses[6] == 2);  /* built once, then served from the plan cache -- frame 4 under
                                     the other generation, through the word-for-word check */
        CHECK(skip.ses[1] >= 2);  /* whole lists not entered */
        CHECK(skip.ses[2] >= 20); /* dead commands */
        CHECK(skip.ses[5] == 0);  /* trips */
        const DeadLeg vfy = RunDeadScene(1, 2, mutate, false);
        CHECK(SameDeadLeg(ref, vfy));
        CHECK(vfy.ses[3] == 3 && vfy.ses[4] == 0);
        printf("  [deadset] variant=%d: %zu batches %s; plans=%u skipped=%u dead=%u | verify ok=%u bad=%u | memo hits=%u\n",
               mut, skip.batches.size(), same ? "IDENTICAL" : "DIFFER", skip.ses[0], skip.ses[1], skip.ses[2],
               vfy.ses[3], vfy.ses[4], memo.memoHits);
    }
    /* a list changing under the memo and the plan cache */
    {
        const DeadLeg mref = RunDeadScene(0, 0, 0, false, true);
        const DeadLeg mskip = RunDeadScene(1, 1, 0, false, true);
        const DeadLeg mvfy = RunDeadScene(1, 2, 0, false, true);
        CHECK(SameDeadLeg(mref, mskip));
        CHECK(SameDeadLeg(mref, mvfy));
        CHECK(mvfy.ses[4] == 0 && mskip.ses[5] == 0);
        printf("  [deadset] list rewritten mid-run: %s; plans=%u reused=%u | verify ok=%u bad=%u\n",
               SameDeadLeg(mref, mskip) ? "IDENTICAL" : "DIFFER", mskip.ses[0], mskip.ses[6], mvfy.ses[3], mvfy.ses[4]);
    }
    /* the tables version moving every frame: the memo must not hit, the walk is unchanged */
    const DeadLeg ref = RunDeadScene(0, 0, 0, false);
    const DeadLeg moved = RunDeadScene(1, 1, 0, true);
    CHECK(moved.memoHits == 0);
    CHECK(moved.ses[0] == 0);
    CHECK(SameDeadLeg(ref, moved));
}

/* [native] hud_retain (c5hud3, docs/research/native/c5hud3-progress.md): several HUD runs
 * replayed over six frames with retention off, on and in verify. From the second frame on the
 * unchanged runs replay from their record (recorded backend calls, vertex floats, span stores,
 * LRU touches, register effects) -- and every leg must draw the SAME batches, make the same
 * uploads and leave TMEM and the span model byte-identical. Frame 3 moves one run's rectangle
 * (a words miss), frame 4 rewrites one texture's bytes and moves the DMA generation (a source
 * miss: the stale record must NOT replay), and one run loads a TLUT (not retainable: it always
 * executes). A second scene squeezes the pool so a replay would not fit (a headroom miss). */
struct RetainLeg {
    std::vector<MockBatch> batches;
    std::vector<MockUpload> uploads;
    std::vector<uint8_t> tmem;
    std::vector<uint16_t> spanBase;
    char receipt[1024] = {};
    char dreceipt[512] = {};
    char hreceipt[512] = {}; /* [native] hudsky_emit ([hsx]) */
};

static RetainLeg RunRetainScene(int retain, GdxHudRecord* rec, const W* root, size_t rootN, int frames,
                                void (*perFrame)(int frame, GdxHudRecord* rec), size_t poolTris = 0,
                                int emit = 0) {
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = rootN * 16;
    RetainLeg out;
    MockBackend mb;
    if (poolTris != 0) {
        mb.pool.assign(poolTris * 3 * 12, 0.0f);
    }
    {
        gdxn::Interp it(&mb);
        it.RegisterNoopHook(HudNoop, rec);
        it.HudSetDirect(1);
        it.HudSetRetain(retain, 0);
        it.HsxSetLevel(emit); /* [native] hudsky_emit (the mock backend runs no programs: the generic issue) */
        for (int frame = 0; frame < frames; frame++) {
            if (perFrame != nullptr) {
                perFrame(frame, rec);
            }
            memset(rec->rp, 0, sizeof(rec->rp));
            it.RunFrame(nullptr, Cfg(), 0);
            it.HudDirectTaskEnd();
            it.HudRetainTaskEnd();
        }
        out.tmem.assign(it.RdpState().tmem, it.RdpState().tmem + sizeof(it.RdpState().tmem));
        out.spanBase.assign(it.RdpState().tmem_span_base, it.RdpState().tmem_span_base + 512);
        it.HudRetainFormat(out.receipt, sizeof(out.receipt), static_cast<uint32_t>(frames));
        it.HudDirectFormat(out.dreceipt, sizeof(out.dreceipt), static_cast<uint32_t>(frames));
        it.HsxFormat(out.hreceipt, sizeof(out.hreceipt), static_cast<uint32_t>(frames));
    }
    out.batches = mb.batches;
    out.uploads = mb.uploads;
    return out;
}

static uint8_t gRetTexA[16 * 16 * 2], gRetTexB[32 * 8 * 2], gRetTexC[8 * 8 * 2], gRetTexE[8 * 8], gRetPal[256 * 2];
static uint8_t gRetTexI[16 * 8 * 2]; /* an o2r (immutable) texture: its loads meet the same-content test */
static uint32_t gRetMoveRun = 0, gRetMoveWord = 0; /* the rectangle frame 3 moves */
static uint32_t gRetFillWord = 0, gRetPrimWord = 0;   /* the marker frame 2 moves and recolours; a prim value */
static W* gRetRoot = nullptr;                          /* the root list: a prim depth set outside the runs */
static int gRetDepthIdx = -1;

static void RetainPerFrame(int frame, GdxHudRecord* rec) {
    if (gRetRoot != nullptr && gRetDepthIdx >= 0) {
        /* a prim depth that moves every frame, set outside the runs (the race's position markers
         * do this): the runs' drains take it live, they do not miss on it */
        gRetRoot[gRetDepthIdx].w1lo = static_cast<uint32_t>((frame * 1111) & 0x7FFF) << 16;
    }
    if (frame >= 2) {
        rec->words[gRetFillWord].w1 ^= 0x003E003Eu; /* a marker changes colour ... */
        rec->words[gRetFillWord + 1].w1 += 4u;      /* ... and moves down 1 px, every frame */
        rec->words[gRetFillWord + 1].w0 += 4u;
    }
    if (frame >= 2) {
        rec->words[gRetMoveWord].w1 += (4u << 12);     /* a rectangle moves 1 px right every frame */
        rec->words[gRetMoveWord].w0 += (4u << 12);
        rec->words[gRetMoveWord + 1].w1 += 0x00200000u; /* and its texture coordinates move */
    }
    if (frame == 6) {
        gRetTexA[5] ^= 0x5Au; /* a texture changes under the same address ... */
        gdx_test_dma_gen++;   /* ... and the DMA generation says so: a source miss */
    }
    if (frame == 8) {
        rec->words[gRetPrimWord].w1 ^= 0x00FF0000u; /* a register value: a words miss */
    }
}

static void TestHudRetainParity() {
    for (unsigned i = 0; i < sizeof(gRetTexA); i++) gRetTexA[i] = static_cast<uint8_t>(i * 7 + 1);
    for (unsigned i = 0; i < sizeof(gRetTexB); i++) gRetTexB[i] = static_cast<uint8_t>(i * 13 + 5);
    for (unsigned i = 0; i < sizeof(gRetTexC); i++) gRetTexC[i] = static_cast<uint8_t>(i * 29 + 3);
    for (unsigned i = 0; i < sizeof(gRetTexE); i++) gRetTexE[i] = static_cast<uint8_t>(i % 5);
    for (unsigned i = 0; i < sizeof(gRetPal); i++) gRetPal[i] = static_cast<uint8_t>(i * 11 + 1);
    for (unsigned i = 0; i < sizeof(gRetTexI); i++) gRetTexI[i] = static_cast<uint8_t>(i * 17 + 9);

    static GdxHudRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = GDXHUD_MAGIC;
    uint32_t n = 0;
    auto put = [&](GdxHudWord w) { rec.words[n++] = w; };
    auto rect = [&](uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry, uint32_t st, uint32_t d, bool flip) {
        put(HudW((flip ? 0xE5000000u : 0xE4000000u) | (lrx << 12) | lry, (ulx << 12) | uly));
        put(HudW(0xE1000000u, st));
        put(HudW(0xF1000000u, d));
    };
    auto group = [&](uint32_t fmtSiz, const void* src, uint32_t lrs, uint32_t dxt, uint32_t renderFmtSiz,
                     uint32_t line, uint32_t w, uint32_t h) {
        put(HudP(0xFD000000u | fmtSiz, src));
        put(HudW(0xF5000000u | fmtSiz, 0x07000000u));
        put(HudW(0xF3000000u, 0x07000000u | (lrs << 12) | dxt));
        put(HudW(0xF5000000u | renderFmtSiz | (line << 9), 0x00000000u));
        put(HudW(0xF2000000u, ((w - 1) << 14) | ((h - 1) << 2)));
    };
    auto endRun = [&](uint32_t start) {
        GdxHudRun& r = rec.runs[rec.nRuns++];
        r.start = static_cast<uint16_t>(start);
        r.end = static_cast<uint16_t>(n);
        r.kind = GDXHUD_KIND_HUD;
        r.mode = 1;
    };
    /* run 0: modes, a texture, two rects with a prim change between them */
    uint32_t s0 = n;
    put(HudW(0xE3001001u, 0x00000000u));
    put(HudW(0xFC119623u, 0xFF2FFFFFu));
    put(HudW(0xFA000000u, 0xFF8040FFu));
    group(0x100000u, gRetTexA, 255, 0x800, 0x100000u, 4, 16, 16);
    rect(40, 40, 104, 104, 0, 0x04000400u, false);
    gRetPrimWord = n;
    put(HudW(0xFA000000u, 0x40FF80C0u));
    rect(120, 40, 184, 104, 0, 0x04000400u, false);
    endRun(s0);
    /* run 1: a small texture over the large one's words, a flipped rect */
    uint32_t s1 = n;
    group(0x100000u, gRetTexC, 63, 0x800, 0x100000u, 2, 8, 8);
    gRetMoveRun = rec.nRuns;
    gRetMoveWord = n;
    rect(200, 40, 232, 72, 0, 0x04000400u, false);
    rect(240, 40, 272, 72, 0, 0x04000400u, true);
    endRun(s1);
    /* run 2: a native-RGBA16 texture */
    uint32_t s2 = n;
    group(0x100000u, gRetTexB, 255, 0x400, 0x100000u, 8, 32, 8);
    rect(40, 120, 168, 152, 0, 0x04000400u, false);
    endRun(s2);
    /* run 3: fill rectangles (markers) of two colours */
    uint32_t s3 = n;
    put(HudW(0xF7000000u, 0xF801F801u));
    put(HudW(0xF6000000u | (300u << 12) | 230u, (280u << 12) | 220u));
    gRetFillWord = n;
    put(HudW(0xF7000000u, 0x07C107C1u));
    put(HudW(0xF6000000u | (270u << 12) | 200u, (260u << 12) | 190u));
    endRun(s3);
    /* run 4: CI8 with its TLUT (not retainable) */
    uint32_t s4 = n;
    put(HudP(0xFD100000u, gRetPal));
    put(HudW(0xF5000100u, 0x07000000u));
    put(HudW(0xF0000000u, 0x073FC000u));
    put(HudW(0xE3001001u, 0x00008000u));
    group(0x500000u, gRetTexE, 31, 0x800, 0x480000u, 1, 8, 8);
    rect(120, 160, 152, 192, 0, 0x04000400u, false);
    put(HudW(0xE3001001u, 0x00000000u));
    endRun(s4);
    /* run 5: texC again (cache hit, memo hit) */
    uint32_t s5 = n;
    group(0x100000u, gRetTexC, 63, 0x800, 0x100000u, 2, 8, 8);
    rect(160, 160, 192, 192, 0, 0x04000400u, false);
    endRun(s5);
    /* runs 6 + 7: a texture loaded by one run and drawn by the next (a setup list between
     * them in the game): the second run samples a span it did not store */
    uint32_t s6 = n;
    group(0x100000u, gRetTexA, 255, 0x800, 0x100000u, 4, 16, 16);
    endRun(s6);
    uint32_t s7 = n;
    rect(200, 120, 232, 152, 0, 0x04000400u, false);
    endRun(s7);
    /* runs 8 + 9: the same immutable texture loaded by two runs in a row -- the second load's
     * same-content test skips it (the span the first stored), the first one's stores */
    uint32_t s8 = n;
    group(0x100000u, gRetTexI, 127, 0x800, 0x100000u, 4, 16, 8);
    rect(40, 200, 104, 232, 0, 0x04000400u, false);
    endRun(s8);
    uint32_t s9 = n;
    group(0x100000u, gRetTexI, 127, 0x800, 0x100000u, 4, 16, 8);
    rect(120, 200, 184, 232, 0, 0x04000400u, true);
    endRun(s9);
    /* run 10: a fill rectangle with ulx > lrx (an empty energy bar): it draws nothing and writes
     * nothing, so the corner colours it leaves are the ones run 3's markers left */
    uint32_t s10 = n;
    put(HudW(0xF7000000u, 0x3E013E01u));
    put(HudW(0xF6000000u | (100u << 12) | 20u, (104u << 12) | 20u));
    endRun(s10);
    rec.nWords = n;

    static W root[48];
    int r = 0;
    root[r++] = Wv(0xFE000000u, 0x00100000u); /* SETZIMG / SETCIMG apart: the fill rects DRAW */
    root[r++] = Wv(0xFF100000u, 0x00200000u);
    gRetRoot = root;
    gRetDepthIdx = r;
    root[r++] = Wv(0xEE000000u, 0x00000000u); /* SETPRIMDEPTH, rewritten per frame */
    for (uint32_t k = 0; k < rec.nRuns; k++) {
        root[r++] = Wv(0xE7000000u, 0);
        root[r++] = Wv(0x005E0000u | k, 0);
        if (k == 2) {
            root[r++] = Wv(0xFA000000u, 0x10203040u); /* a register write between two runs */
        }
    }
    root[r++] = Wv(0xDF000000u, 0);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(gRetTexA, sizeof(gRetTexA), 8, false, false, false);
    Reg(gRetTexB, sizeof(gRetTexB), 8, false, false, false);
    Reg(gRetTexC, sizeof(gRetTexC), 8, false, false, false);
    Reg(gRetTexE, sizeof(gRetTexE), 8, false, false, false);
    Reg(gRetPal, sizeof(gRetPal), 8, false, false, false);
    Reg(gRetTexI, sizeof(gRetTexI), 8, false, false, false);
    gdx_test_imm_lo = reinterpret_cast<uintptr_t>(gRetTexI);
    gdx_test_imm_hi = gdx_test_imm_lo + sizeof(gRetTexI);
    gdx_test_native_lo = reinterpret_cast<uintptr_t>(gRetTexB);
    gdx_test_native_hi = gdx_test_native_lo + sizeof(gRetTexB);
    const unsigned clsSaved = gdx_test_src_cls;
    gdx_test_src_cls = GDX_TMEMSRC_DMA;

    /* Each leg starts from the same words and texture bytes (the per-frame edits are undone). */
    const GdxHudRecord pristine = rec;
    uint8_t texA0[sizeof(gRetTexA)];
    memcpy(texA0, gRetTexA, sizeof(texA0));
    auto reset = [&]() {
        rec = pristine;
        memcpy(gRetTexA, texA0, sizeof(texA0));
    };
    const int kFrames = 10;
    reset();
    const RetainLeg off = RunRetainScene(0, &rec, root, static_cast<size_t>(r), kFrames, RetainPerFrame);
    reset();
    const RetainLeg on = RunRetainScene(1, &rec, root, static_cast<size_t>(r), kFrames, RetainPerFrame);
    reset();
    const RetainLeg vfy = RunRetainScene(2, &rec, root, static_cast<size_t>(r), kFrames, RetainPerFrame);
    printf("  [hudr] on:  %s\n  [hudr] vfy: %s\n", on.receipt, vfy.receipt);
    FastLeg a, b, c;
    a.batches = off.batches;
    a.uploads = off.uploads;
    b.batches = on.batches;
    b.uploads = on.uploads;
    c.batches = vfy.batches;
    c.uploads = vfy.uploads;
    CHECK(!off.batches.empty());
    CHECK(SameBatches(a, b));
    CHECK(SameBatches(a, c));
    CHECK(off.tmem == on.tmem && off.spanBase == on.spanBase);
    CHECK(off.tmem == vfy.tmem && off.spanBase == vfy.spanBase);
    CHECK(strstr(on.receipt, "runs=11.00 hit=0.00") == nullptr); /* the unchanged runs really replayed */
    CHECK(strstr(on.receipt, "src=0.00") == nullptr); /* the moved texture really missed */
    CHECK(strstr(on.receipt, "words=0.00") == nullptr);
    CHECK(strstr(on.receipt, "variant hit=0.00") == nullptr); /* the moved rects replayed from patched floats */
    CHECK(strstr(on.receipt, "selfbad=0 ") != nullptr);
    CHECK(strstr(vfy.receipt, "bad=0 ") != nullptr && strstr(vfy.receipt, "ses=0/") != nullptr);
    CHECK(strstr(vfy.receipt, "vfy runs=0 ") == nullptr); /* the comparison really ran */
    CHECK(strstr(on.dreceipt, "stale=0 ses=0") != nullptr);
    printf("  [hudr] off/on/verify: %zu batches, %zu uploads %s\n", off.batches.size(), off.uploads.size(),
           SameBatches(a, b) && SameBatches(a, c) ? "IDENTICAL" : "DIFFER");

    /* [native] hudsky_emit (hudsky): the retained hits through the compiled plan (1) and the plan
     * checked against the record on every exact hit (2) draw exactly what retention draws. */
    reset();
    const RetainLeg em1 = RunRetainScene(1, &rec, root, static_cast<size_t>(r), kFrames, RetainPerFrame, 0, 1);
    reset();
    const RetainLeg em2 = RunRetainScene(1, &rec, root, static_cast<size_t>(r), kFrames, RetainPerFrame, 0, 2);
    printf("  [hsx] emit: %s\n  [hsx] vfy:  %s\n", em1.hreceipt, em2.hreceipt);
    FastLeg e1, e2;
    e1.batches = em1.batches;
    e1.uploads = em1.uploads;
    e2.batches = em2.batches;
    e2.uploads = em2.uploads;
    CHECK(SameBatches(a, e1));
    CHECK(SameBatches(a, e2));
    CHECK(off.tmem == em1.tmem && off.spanBase == em1.spanBase);
    CHECK(off.tmem == em2.tmem && off.spanBase == em2.spanBase);
    CHECK(strstr(em1.hreceipt, "hits=0.00") == nullptr); /* the plan really served hits */
    CHECK(strstr(em2.hreceipt, "bad=0 ") != nullptr && strstr(em2.hreceipt, "vfy runs=0 ") == nullptr);
    printf("  [hsx] off/emit/verify: %s\n", SameBatches(a, e1) && SameBatches(a, e2) ? "IDENTICAL" : "DIFFER");

    /* Pool pressure: a replay that would not fit is refused (a headroom miss) and the run
     * executes, splitting its batches exactly as the stock path does. */
    for (size_t poolTris : { size_t(7), size_t(13), size_t(40) }) {
        reset();
        const RetainLeg p0 = RunRetainScene(0, &rec, root, static_cast<size_t>(r), kFrames, RetainPerFrame, poolTris);
        reset();
        const RetainLeg p1 = RunRetainScene(1, &rec, root, static_cast<size_t>(r), kFrames, RetainPerFrame, poolTris);
        FastLeg x, y;
        x.batches = p0.batches;
        x.uploads = p0.uploads;
        y.batches = p1.batches;
        y.uploads = p1.uploads;
        CHECK(SameBatches(x, y));
        CHECK(p0.tmem == p1.tmem && p0.spanBase == p1.spanBase);
        reset();
        const RetainLeg pe = RunRetainScene(1, &rec, root, static_cast<size_t>(r), kFrames, RetainPerFrame, poolTris, 1);
        FastLeg z;
        z.batches = pe.batches;
        z.uploads = pe.uploads;
        CHECK(SameBatches(x, z)); /* [native] hudsky_emit under pool pressure */
        CHECK(p0.tmem == pe.tmem && p0.spanBase == pe.spanBase);
        printf("  [hudr] pool %zu tris: %zu batches %s | %s\n", poolTris, p0.batches.size(),
               SameBatches(x, y) ? "IDENTICAL" : "DIFFER", p1.receipt);
    }
    reset();
    gdx_test_src_cls = clsSaved;
    gdx_test_native_lo = gdx_test_native_hi = 0;
    gdx_test_imm_lo = gdx_test_imm_hi = 0;
    gRetRoot = nullptr;
    gRetDepthIdx = -1;
}

int main() {
    TestWalk();
    TestCC();
    TestRectTexture();
    TestTmemSkipWidened();
    TestTmemFaultInjection();
    TestTri3D();
    TestCiRecycle();
    TestOwnFold();
    TestPackSplitPreservesGeometry();
    TestDecodeCeiling();
    TestVtxWindow();
    TestClipPolyBound();
    TestFastPathParity();
    TestNicensusParity();
    TestDeadSetup();
    TestHudDirectParity();
    TestHudRetainParity();
    if (gFails == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURES ===\n", gFails);
    return 1;
}
