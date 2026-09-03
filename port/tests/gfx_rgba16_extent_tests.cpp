// gfx_rgba16_extent_tests.cpp -- regression test for the RGBA16 TMEM-decode extent
// derivation in libultraship's Interpreter::ImportTextureRgba16 (the 3DS TMEM path).
//
// Standalone console exe: no libultraship, no game objects. It re-implements the exact
// width/height derivation the interpreter runs, in both the PRE-fix and POST-fix forms,
// and asserts the delta on the F-Zero X HUD atlases that render garbled on the 3DS.
//
// Bug: the TMEM decode path derived height from (remaining TMEM / lineBytes) and only
// bounded it to the gDPSetTileSize window (lrs/lrt) when the CLAMP wrap bit was set. The
// HUD sprite atlases are loaded G_TX_WRAP + G_TX_NOMASK, so height inflated to fill TMEM.
// A tall atlas indexed by the T coordinate (the 12x160 speedometer digit strip, the km/h
// glyph, the 8x224/8x72/8x132 symbol/lap/racer strips) then got the wrong padded height,
// so vScale shifted and every UV-selected glyph sampled the wrong row -> garbled digits.
//
// Fix: when an axis carries no mask (G_TX_NOMASK), the tile-size window is authoritative
// (that is what real hardware samples), so bound the extent to it regardless of CLAMP.
//
// Returns 0 iff every check passes; non-zero (and prints [FAIL]) otherwise.

#include <cstdint>
#include <cstdio>

// N64 GBI constants used by the derivation (values from decomp include/PR/gbi.h).
static constexpr uint32_t kTmemBytes = 4096; // sizeof(RDP::tmem)
static constexpr uint8_t G_TX_NOMASK = 0;
static constexpr uint8_t G_TX_WRAP = 0;
static constexpr uint8_t G_TX_CLAMP = 2;

static int g_failures = 0;

static void check_u32(const char* name, uint32_t got, uint32_t want) {
    if (got == want) {
        printf("[ OK ] %-52s got=%u\n", name, got);
    } else {
        printf("[FAIL] %-52s got=%u want=%u\n", name, got, want);
        ++g_failures;
    }
}

// One tile descriptor's worth of the state the derivation reads.
struct TileDesc {
    uint32_t tmemByteOffset; // tmem_index * 8
    uint32_t lineBytes;      // line_size_bytes
    uint16_t lrs, uls;       // gDPSetTileSize S window (fixed 10.2)
    uint16_t lrt, ult;       // gDPSetTileSize T window (fixed 10.2)
    uint8_t cms, cmt;        // clamp/mirror/wrap
    uint8_t masks, maskt;    // wrap-period mask (G_TX_NOMASK = none)
};

// Build a gDPLoadTextureBlock-shaped descriptor: WRAP + NOMASK, tile window = (w,h).
static TileDesc MakeHudAtlas(uint32_t w, uint32_t h) {
    TileDesc d{};
    d.tmemByteOffset = 0;
    d.lineBytes = w * 2; // RGBA16 = 2 bytes/texel
    d.uls = 0;
    d.lrs = (uint16_t)((w - 1) << 2);
    d.ult = 0;
    d.lrt = (uint16_t)((h - 1) << 2);
    d.cms = G_TX_WRAP;
    d.cmt = G_TX_WRAP;
    d.masks = G_TX_NOMASK;
    d.maskt = G_TX_NOMASK;
    return d;
}

// PRE-fix derivation: tile window bounds only under CLAMP.
static uint32_t DeriveHeightOld(const TileDesc& d) {
    uint32_t height = (kTmemBytes - d.tmemByteOffset) / d.lineBytes;
    const uint32_t tileH = (uint32_t)((d.lrt - d.ult + 4) / 4);
    if ((d.cmt & G_TX_CLAMP) != 0 && tileH > 0 && tileH < height) {
        height = tileH;
    }
    if (height == 0) {
        height = 1;
    }
    return height;
}

// POST-fix derivation: NOMASK axis is bounded by the tile window too.
static uint32_t DeriveHeightNew(const TileDesc& d) {
    uint32_t height = (kTmemBytes - d.tmemByteOffset) / d.lineBytes;
    const uint32_t tileH = (uint32_t)((d.lrt - d.ult + 4) / 4);
    const bool clampT = (d.cmt & G_TX_CLAMP) != 0;
    const bool boundH = clampT || d.maskt == G_TX_NOMASK;
    if (boundH && tileH > 0 && tileH < height) {
        height = tileH;
    }
    if (height == 0) {
        height = 1;
    }
    return height;
}

// ---- content-hash span derivation (Interpreter::ImportTexture's tmem_content_hash) ------------
//
// Second bug (this commit): the content-hash span mirrored the decode's CLAMP bound but NOT the
// NOMASK->tile-window bound the decode-side extent fix added. So for a NOMASK+WRAP tall atlas the
// hash folded MORE rows than the decode read -- reaching past the loaded atlas into whatever else
// streamed through the shared low TMEM. That leftover varies frame-to-frame in a race, so a STATIC
// atlas got a fresh cache key every frame: every import missed, the texture re-decoded and
// re-uploaded every frame, and a re-upload race could serve a rotated texture_id -> the residual
// speedometer green/magenta garbage that survived the decode-side extent fix.
//
// The span is the number of TMEM bytes the hash folds. A CONTENT-STABLE key requires the span to
// equal exactly the bytes the decode reads: (h-1)*lineBytes + 2*w, with h/w bounded like the decode.

// PRE-fix span: NOMASK axis unbounded (only CLAMP bounds it).
static uint32_t DeriveSpanOld(const TileDesc& d) {
    const uint32_t remaining = kTmemBytes - d.tmemByteOffset;
    uint32_t w = d.lineBytes / 2;
    uint32_t h = remaining / d.lineBytes;
    const uint32_t tileW = (uint32_t)((d.lrs - d.uls + 4) / 4);
    const uint32_t tileH = (uint32_t)((d.lrt - d.ult + 4) / 4);
    if ((d.cms & G_TX_CLAMP) != 0 && tileW > 0 && tileW < w) {
        w = tileW;
    }
    if ((d.cmt & G_TX_CLAMP) != 0 && tileH > 0 && tileH < h) {
        h = tileH;
    }
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (d.lineBytes * h > remaining) h = remaining / d.lineBytes;
    return (h - 1) * d.lineBytes + 2 * w;
}

// POST-fix span: NOMASK axis bounded to the tile window, matching the decode extent exactly.
static uint32_t DeriveSpanNew(const TileDesc& d) {
    const uint32_t remaining = kTmemBytes - d.tmemByteOffset;
    uint32_t w = d.lineBytes / 2;
    uint32_t h = remaining / d.lineBytes;
    const uint32_t tileW = (uint32_t)((d.lrs - d.uls + 4) / 4);
    const uint32_t tileH = (uint32_t)((d.lrt - d.ult + 4) / 4);
    const bool boundW = (d.cms & G_TX_CLAMP) != 0 || d.masks == G_TX_NOMASK;
    const bool boundH = (d.cmt & G_TX_CLAMP) != 0 || d.maskt == G_TX_NOMASK;
    if (boundW && tileW > 0 && tileW < w) {
        w = tileW;
    }
    if (boundH && tileH > 0 && tileH < h) {
        h = tileH;
    }
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (d.lineBytes * h > remaining) h = remaining / d.lineBytes;
    return (h - 1) * d.lineBytes + 2 * w;
}

// Bytes the decode actually reads for a fully-bounded extent: the identity the key must capture.
static uint32_t DecodeReadSpan(uint32_t w, uint32_t h, uint32_t lineBytes) {
    return (h - 1) * lineBytes + 2 * w;
}

static void TestSpeedometerDigitStrip() {
    printf("\n-- 12x160 RGBA16 speedometer digit strip (aSpeedDigitsTex) --\n");
    const TileDesc d = MakeHudAtlas(12, 160);
    // PRE-fix: lineBytes=24, 4096/24 = 170 rows -> overshoot past the 160 loaded rows.
    check_u32("old height overshoots loaded image", DeriveHeightOld(d), 170);
    // POST-fix: bounded to the loaded 160-row atlas -> correct vScale, correct digit rows.
    check_u32("new height == loaded atlas height", DeriveHeightNew(d), 160);
}

static void TestTallSymbolStrip() {
    printf("\n-- 8x224 RGBA16 timer-symbol strip (aTimerSymbolsTex) --\n");
    const TileDesc d = MakeHudAtlas(8, 224);
    // lineBytes=16, 4096/16 = 256 -> overshoot past 224.
    check_u32("old height overshoots", DeriveHeightOld(d), 256);
    check_u32("new height correct", DeriveHeightNew(d), 224);
}

static void TestShortAtlasUnaffected() {
    printf("\n-- 24x16 RGBA16 short TIME atlas (aHudTimeTex) stays correct --\n");
    const TileDesc d = MakeHudAtlas(24, 16);
    // The fix must not change a texture whose window already fits; new must equal loaded 16.
    check_u32("new height == loaded", DeriveHeightNew(d), 16);
}

static void TestClampedTallStillWorks() {
    printf("\n-- CLAMP-mode tall texture keeps being bounded (no regression) --\n");
    TileDesc d = MakeHudAtlas(12, 160);
    d.cmt = G_TX_CLAMP; // was already bounded pre-fix; must stay bounded post-fix
    check_u32("old bounds under clamp", DeriveHeightOld(d), 160);
    check_u32("new bounds under clamp", DeriveHeightNew(d), 160);
}

static void TestDigitStripHashSpanStable() {
    printf("\n-- 12x160 digit strip: content-hash span must match the decode read span --\n");
    const TileDesc d = MakeHudAtlas(12, 160);
    // lineBytes=24. Decode reads the bounded 160-row atlas.
    const uint32_t decodeSpan = DecodeReadSpan(12, 160, 24); // (160-1)*24 + 2*12 = 3840
    // PRE-fix: NOMASK span unbounded -> folds 170 rows (past the atlas, into shared low TMEM).
    check_u32("old span over-reads past the atlas", DeriveSpanOld(d), DecodeReadSpan(12, 170, 24));
    check_u32("old span != decode read span (unstable key)", DeriveSpanOld(d) != decodeSpan ? 1u : 0u, 1u);
    // POST-fix: span == exactly the bytes the decode reads -> stable, content-only key.
    check_u32("new span == decode read span (stable key)", DeriveSpanNew(d), decodeSpan);
}

static void TestKmhHashSpanStable() {
    printf("\n-- 20x16 km/h glyph: content-hash span must match the decode read span --\n");
    const TileDesc d = MakeHudAtlas(20, 16);
    const uint32_t decodeSpan = DecodeReadSpan(20, 16, 40); // (16-1)*40 + 2*20 = 640
    // PRE-fix: lineBytes=40, 4096/40 = 102 rows folded -> the km/h key smeared in ~86 rows of
    // whatever the digit strip left in TMEM below it.
    check_u32("old span over-reads past km/h", DeriveSpanOld(d), DecodeReadSpan(20, 102, 40));
    check_u32("new span == decode read span (stable key)", DeriveSpanNew(d), decodeSpan);
}

static void TestHashSpanClampUnaffected() {
    printf("\n-- CLAMP tall texture: hash span already matched decode (no regression) --\n");
    TileDesc d = MakeHudAtlas(12, 160);
    d.cmt = G_TX_CLAMP;
    check_u32("old clamp span == new clamp span", DeriveSpanOld(d), DeriveSpanNew(d));
}

int main() {
    printf("== RGBA16 TMEM-decode extent tests (3DS HUD tall-atlas fix) ==\n");
    TestSpeedometerDigitStrip();
    TestTallSymbolStrip();
    TestShortAtlasUnaffected();
    TestClampedTallStillWorks();
    TestDigitStripHashSpanStable();
    TestKmhHashSpanStable();
    TestHashSpanClampUnaffected();

    if (g_failures == 0) {
        printf("\nALL PASS\n");
        return 0;
    }
    printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
