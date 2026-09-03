// gfx_convert_tests.cpp -- Phase G2 unit tests for the binary N64 -> wide 16-byte
// display-list boundary converter (port/n64_gfx_convert.{h,cpp}).
//
// Standalone console exe: no libultraship, no game objects, no decomp headers.
// Returns 0 iff every check passes; non-zero (and prints [FAIL]) otherwise.

#include "n64_gfx_convert.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using gdx::ClassifyW1;
using gdx::ConvertContext;
using gdx::ConvertList;
using gdx::GfxWideCache;
using gdx::W1Kind;
using gdx::WideGfx;

static int g_failures = 0;

static void check_bool(const char* name, bool got, bool want) {
    if (got == want) {
        printf("[ OK ] %-46s got=%d\n", name, (int)got);
    } else {
        printf("[FAIL] %-46s got=%d want=%d\n", name, (int)got, (int)want);
        ++g_failures;
    }
}

static void check_u32(const char* name, uint32_t got, uint32_t want) {
    if (got == want) {
        printf("[ OK ] %-46s got=0x%08X\n", name, got);
    } else {
        printf("[FAIL] %-46s got=0x%08X want=0x%08X\n", name, got, want);
        ++g_failures;
    }
}

static void check_u64(const char* name, uint64_t got, uint64_t want) {
    if (got == want) {
        printf("[ OK ] %-46s got=0x%016llX\n", name, (unsigned long long)got);
    } else {
        printf("[FAIL] %-46s got=0x%016llX want=0x%016llX\n", name,
               (unsigned long long)got, (unsigned long long)want);
        ++g_failures;
    }
}

static void check_sz(const char* name, size_t got, size_t want) {
    if (got == want) {
        printf("[ OK ] %-46s got=%zu\n", name, got);
    } else {
        printf("[FAIL] %-46s got=%zu want=%zu\n", name, got, want);
        ++g_failures;
    }
}

// ---- synthetic N64 command builders ------------------------------------------

// BIG-ENDIAN, as a command arrives from a .ndd / ROM blob. ConvertList(is_big=true) swaps it back.
static void PushBE(std::vector<uint8_t>& buf, uint32_t w0, uint32_t w1) {
    buf.push_back((uint8_t)(w0 >> 24));
    buf.push_back((uint8_t)(w0 >> 16));
    buf.push_back((uint8_t)(w0 >> 8));
    buf.push_back((uint8_t)(w0));
    buf.push_back((uint8_t)(w1 >> 24));
    buf.push_back((uint8_t)(w1 >> 16));
    buf.push_back((uint8_t)(w1 >> 8));
    buf.push_back((uint8_t)(w1));
}

// Host (little-endian) order, as a host-built N64 command range. Not swapped by ConvertList.
static void PushLE(std::vector<uint8_t>& buf, uint32_t w0, uint32_t w1) {
    uint8_t tmp[8];
    std::memcpy(tmp + 0, &w0, 4);
    std::memcpy(tmp + 4, &w1, 4);
    for (int i = 0; i < 8; ++i) buf.push_back(tmp[i]);
}

// Maps KSEG0 addresses (0x80000000-0x9FFFFFFF) to a fabricated > 4 GB host address; everything
// else is unresolvable and must stay a token.
static const uintptr_t kMockArenaHigh = (uintptr_t)0x00007FF600000000ull;
static bool MockResolvePhysical(void* /*user*/, uint32_t raw, size_t /*req*/, uintptr_t* out) {
    if (raw >= 0x80000000u && raw <= 0x9FFFFFFFu) {
        *out = kMockArenaHigh | (uintptr_t)(raw & 0x1FFFFFFFu);
        return true;
    }
    return false;
}

// Resolves to a low (high32 == 0) address, to prove the converter keeps the token rather than
// committing a non->4GB "pointer" that the bridge would misread as already resolved.
static bool MockResolveLow32(void* /*user*/, uint32_t raw, size_t /*req*/, uintptr_t* out) {
    if (raw >= 0x80000000u && raw <= 0x9FFFFFFFu) {
        *out = (uintptr_t)(raw & 0x1FFFFFFFu);  // high32 == 0
        return true;
    }
    return false;
}

// Opcodes used by the tests.
enum : uint8_t {
    OP_VTX_EX2 = 0x01,
    OP_MTX_EX2 = 0xDA,
    OP_MOVEMEM_EX2 = 0xDC,
    OP_DL = 0xDE,
    OP_ENDDL = 0xDF,
    OP_TRI2 = 0x06,          // value word in F3DEX2 (sub-DL in F3D)
    OP_SETPRIMCOLOR = 0xFA,  // value word
    OP_ENDDL_F3D = 0xB8,
    OP_VTX_F3D = 0x04,
};

static uint32_t MakeW0(uint8_t op, uint32_t low) { return ((uint32_t)op << 24) | (low & 0x00FFFFFFu); }

// ---- tests -------------------------------------------------------------------

static void TestLayout() {
    check_sz("sizeof(WideGfx)", sizeof(WideGfx), 16);
    check_sz("offsetof w0", offsetof(WideGfx, w0), 0);
    check_sz("offsetof w1", offsetof(WideGfx, w1), 8);
}

static void TestClassify() {
    check_bool("EX2 0x01 G_VTX is DataPtr", ClassifyW1(0x01, false) == W1Kind::DataPtr, true);
    check_bool("EX2 0xDA G_MTX is DataPtr", ClassifyW1(0xDA, false) == W1Kind::DataPtr, true);
    check_bool("EX2 0xDC G_MOVEMEM is DataPtr", ClassifyW1(0xDC, false) == W1Kind::DataPtr, true);
    check_bool("EX2 0xDE G_DL is SubDlPtr", ClassifyW1(0xDE, false) == W1Kind::SubDlPtr, true);
    check_bool("EX2 0x06 G_TRI2 is Value", ClassifyW1(0x06, false) == W1Kind::Value, true);
    check_bool("EX2 0xFA SETPRIMCOLOR is Value", ClassifyW1(0xFA, false) == W1Kind::Value, true);
    check_bool("F3D 0x01 G_MTX is DataPtr", ClassifyW1(0x01, true) == W1Kind::DataPtr, true);
    check_bool("F3D 0x03 G_MOVEMEM is DataPtr", ClassifyW1(0x03, true) == W1Kind::DataPtr, true);
    check_bool("F3D 0x04 G_VTX is DataPtr", ClassifyW1(0x04, true) == W1Kind::DataPtr, true);
    check_bool("F3D 0x06 G_DL is SubDlPtr", ClassifyW1(0x06, true) == W1Kind::SubDlPtr, true);
    // 0x06 differs by dialect -- the value-corruption guard.
    check_bool("0x06 dialect-sensitive", (ClassifyW1(0x06, false) == W1Kind::Value) &&
                                             (ClassifyW1(0x06, true) == W1Kind::SubDlPtr), true);
}

static void TestConvertBigEndian() {
    std::vector<uint8_t> src;
    PushBE(src, MakeW0(OP_VTX_EX2, 0x001020), 0x06001000u);      // segmented ptr
    PushBE(src, MakeW0(OP_MTX_EX2, 0x000038), 0x80123450u);      // physical ptr (KSEG0)
    PushBE(src, MakeW0(OP_TRI2, 0x040608), 0x800A0C0Eu);         // VALUE that looks like KSEG0
    PushBE(src, MakeW0(OP_SETPRIMCOLOR, 0x000000), 0xFF80FFFFu); // VALUE
    PushBE(src, MakeW0(OP_ENDDL, 0x000000), 0x00000000u);

    ConvertContext ctx;
    ctx.resolve_physical = &MockResolvePhysical;
    std::vector<WideGfx> out = ConvertList(src.data(), 5, /*is_big=*/true, /*is_f3d=*/false, ctx);

    check_sz("BE convert count", out.size(), 5);

    check_u32("BE cmd0 w0 (VTX)", out[0].w0, MakeW0(OP_VTX_EX2, 0x001020));
    check_u64("BE cmd0 w1 (segmented kept)", out[0].w1, 0x0000000006001000ull);

    check_u32("BE cmd1 w0 (MTX)", out[1].w0, MakeW0(OP_MTX_EX2, 0x000038));
    check_u64("BE cmd1 w1 (physical resolved)", out[1].w1,
              (uint64_t)(kMockArenaHigh | 0x00123450u));

    // TRI2 is a value word in EX2, so w1 must be copied verbatim even though it looks like KSEG0.
    check_u32("BE cmd2 w0 (TRI2)", out[2].w0, MakeW0(OP_TRI2, 0x040608));
    check_u64("BE cmd2 w1 (value NOT resolved)", out[2].w1, 0x00000000800A0C0Eull);

    check_u64("BE cmd3 w1 (color value)", out[3].w1, 0x00000000FF80FFFFull);

    check_u32("BE cmd4 w0 (ENDDL)", out[4].w0 >> 24, OP_ENDDL);
}

static void TestValueSafetyAndLow32() {
    // A DataPtr op whose deterministic host address is NOT > 4 GB must keep the token, so the
    // draw-time path resolves it to the same address.
    std::vector<uint8_t> src;
    PushLE(src, MakeW0(OP_VTX_EX2, 0x001020), 0x80123450u);
    PushLE(src, MakeW0(OP_ENDDL, 0x000000), 0x00000000u);

    ConvertContext ctx;
    ctx.resolve_physical = &MockResolveLow32;
    std::vector<WideGfx> out = ConvertList(src.data(), 2, /*is_big=*/false, /*is_f3d=*/false, ctx);
    check_u64("low32 host addr NOT committed", out[0].w1, 0x0000000080123450ull);
}

static void TestSubDlPolicy() {
    std::vector<uint8_t> src;
    PushBE(src, MakeW0(OP_DL, 0x000000), 0x0A002000u);  // segmented sub-DL
    PushBE(src, MakeW0(OP_DL, 0x000000), 0x80005000u);  // physical sub-DL
    PushBE(src, MakeW0(OP_ENDDL, 0x000000), 0x00000000u);

    ConvertContext ctx;
    ctx.resolve_physical = &MockResolvePhysical;
    std::vector<WideGfx> out = ConvertList(src.data(), 3, /*is_big=*/true, /*is_f3d=*/false, ctx);

    check_u64("subdl segmented kept as token", out[0].w1, 0x000000000A002000ull);
    check_u64("subdl physical resolved to host", out[1].w1, (uint64_t)(kMockArenaHigh | 0x00005000u));
}

static void TestTermination() {
    ConvertContext ctx;  // no resolver
    // Walk hits the command cap without an ENDDL: a terminator must be appended so the interpreter
    // cannot run off the end.
    std::vector<uint8_t> a;
    PushLE(a, MakeW0(OP_SETPRIMCOLOR, 0), 0x11223344u);
    PushLE(a, MakeW0(OP_SETPRIMCOLOR, 0), 0x55667788u);
    std::vector<WideGfx> outA = ConvertList(a.data(), 2, false, false, ctx);
    check_sz("unterminated -> +1 appended ENDDL", outA.size(), 3);
    check_u32("appended terminator opcode", outA.back().w0 >> 24, OP_ENDDL);

    std::vector<uint8_t> b;
    PushLE(b, MakeW0(OP_ENDDL, 0), 0);
    PushLE(b, MakeW0(OP_SETPRIMCOLOR, 0), 0xDEADBEEFu);
    std::vector<WideGfx> outB = ConvertList(b.data(), 2, false, false, ctx);
    check_sz("early ENDDL stops walk", outB.size(), 1);

    std::vector<uint8_t> c;
    PushLE(c, MakeW0(OP_ENDDL_F3D, 0), 0);
    PushLE(c, MakeW0(OP_SETPRIMCOLOR, 0), 0xCAFEBABEu);
    std::vector<WideGfx> outC = ConvertList(c.data(), 2, false, /*is_f3d=*/true, ctx);
    check_sz("F3D 0xB8 terminator stops walk", outC.size(), 1);
    check_u32("F3D terminator preserved", outC.back().w0 >> 24, OP_ENDDL_F3D);
}

static void TestCache() {
    std::vector<uint8_t> src;
    PushLE(src, MakeW0(OP_VTX_EX2, 0x001020), 0x06001000u);
    PushLE(src, MakeW0(OP_ENDDL, 0), 0);

    ConvertContext ctx;
    ctx.resolve_physical = &MockResolvePhysical;
    GfxWideCache cache;
    cache.SetContext(ctx);

    const std::vector<WideGfx>& first = cache.GetOrBuild(src.data(), 2, false, false, /*stamp=*/1);
    const WideGfx* firstData = first.data();
    check_sz("cache size after first build", cache.CachedCount(), 1);

    // Address stability is the contract the bridge relies on, so check the pointer, not just the
    // contents.
    const std::vector<WideGfx>& second = cache.GetOrBuild(src.data(), 2, false, false, /*stamp=*/1);
    check_bool("same stamp -> same buffer address", second.data() == firstData, true);
    check_bool("cache Contains(src)", cache.Contains(src.data()), true);

    const std::vector<WideGfx>& third = cache.GetOrBuild(src.data(), 2, false, false, /*stamp=*/2);
    check_sz("cache still one entry after restamp", cache.CachedCount(), 1);
    check_u32("rebuilt content matches", third[0].w0, MakeW0(OP_VTX_EX2, 0x001020));

    cache.Invalidate(src.data());
    check_bool("Invalidate drops entry", cache.Contains(src.data()), false);
    check_sz("cache empty after invalidate", cache.CachedCount(), 0);

    cache.GetOrBuild(src.data(), 2, false, false, /*stamp=*/3);
    check_sz("cache repopulates after invalidate", cache.CachedCount(), 1);
    cache.Clear();
    check_sz("Clear empties cache", cache.CachedCount(), 0);
}

int main() {
    printf("== Phase G2 binary-DL converter tests ==\n");
    TestLayout();
    TestClassify();
    TestConvertBigEndian();
    TestValueSafetyAndLow32();
    TestSubDlPolicy();
    TestTermination();
    TestCache();

    if (g_failures == 0) {
        printf("\nALL PASS\n");
        return 0;
    }
    printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
