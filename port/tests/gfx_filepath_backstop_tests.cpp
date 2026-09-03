// gfx_filepath_backstop_tests.cpp -- regression test for the OTR-filepath strlen backstop
// in port/n64_gfx_bridge.cpp (ProcessList, just before listPtr->commands.push_back).
//
// Standalone console exe: no game objects, no libultraship. It re-implements the backstop's
// drop predicate in both the PRE-fix and POST-fix forms, parameterized by target pointer
// width, and asserts the delta that garbled the in-race 3DS HUD.
//
// Bug: the backstop dropped any OTR-filepath command (0x24/0x25/0x27/0x28/0x29) whose w1 had
// zero high 32 bits, encoding "a real host string pointer has non-zero high bits". That holds
// on 64-bit desktop targets but is FALSE for EVERY pointer on the 32-bit 3DS, so the backstop
// dropped EVERY legitimate o2r-filepath SETTIMG emit (~13/frame in-race, [race-dl] skip=13).
// With the SETTIMG dropped, the interpreter's previous texture binding persisted and the next
// G_LOADBLOCK re-read the PREVIOUS texture's bytes under the NEW tile geometry: the speed
// digits sampled a stale raw copy and aTimerSymbolsTex's 8x224 (3584-byte) load overread
// aHudTimeTex's 768-byte copy buffer ([texmiss] a=0xa46e1c0 serving 24x16 then 8x224 with
// per-frame content-hash churn) -- the static, sheared/interleaved in-race HUD garble.
//
// Fix: the bridge's OWN two emit sites (o2r key / texture-pack path) set an explicit
// outFilepathEmitTrusted flag and pass unconditionally; only untrusted commands are validated,
// and the high-32-bits test applies only where pointers actually carry high bits (64-bit).
//
// Returns 0 iff every check passes; non-zero (and prints [FAIL]) otherwise.

#include <cstdint>
#include <cstdio>

static int g_failures = 0;

static void check_bool(const char* name, bool got, bool want) {
    if (got == want) {
        printf("[ OK ] %-64s got=%d\n", name, got ? 1 : 0);
    } else {
        printf("[FAIL] %-64s got=%d want=%d\n", name, got ? 1 : 0, want ? 1 : 0);
        ++g_failures;
    }
}

static bool IsOtrFilepathOpcode(uint8_t op) {
    return op == 0x24u || op == 0x25u || op == 0x27u || op == 0x28u || op == 0x29u;
}

// PRE-fix predicate: drop when high bits are zero OR the pointer is unreadable.
// (Readability is stubbed via a parameter; the width assumption is the bug under test.)
static bool DropOld(uint8_t op, uint64_t w1, bool readable) {
    if (!IsOtrFilepathOpcode(op)) {
        return false;
    }
    return (w1 >> 32) == 0 || !readable;
}

// POST-fix predicate: trusted emits pass unconditionally; the high-bits test carries signal
// only when the target's pointers are 64-bit wide.
static bool DropNew(uint8_t op, uint64_t w1, bool readable, bool trusted, bool pointers64) {
    if (!IsOtrFilepathOpcode(op)) {
        return false;
    }
    if (trusted) {
        return false;
    }
    const bool highBitsPlausible = pointers64 ? ((w1 >> 32) != 0) : true;
    return !highBitsPlausible || !readable;
}

int main() {
    // --- The 3DS regression: a legitimate o2r SETTIMG emit on a 32-bit target. ---
    // o2rKey strings live in module rodata (~0x001xxxxx) or heap (0x08xxxxxx): readable,
    // high 32 bits always zero.
    const uint64_t k3dsRodataString = 0x001402A8u;
    check_bool("old predicate DROPS legit 32-bit o2r emit (the bug)",
               DropOld(0x25, k3dsRodataString, /*readable=*/true), true);
    check_bool("new predicate passes legit 32-bit o2r emit (trusted)",
               DropNew(0x25, k3dsRodataString, /*readable=*/true, /*trusted=*/true,
                       /*pointers64=*/false),
               false);

    // --- 64-bit desktop behavior must be preserved. ---
    const uint64_t kDesktopHeapString = 0x00007F31A2B3C4D5u;
    check_bool("new predicate passes legit 64-bit o2r emit (trusted)",
               DropNew(0x25, kDesktopHeapString, true, true, true), false);
    // The original AV: a torn command word whose top byte 0x25 is itself the opcode,
    // zero-extended low32 token routed through default: with no emit.
    const uint64_t kTornToken64 = 0x25820F60u;
    check_bool("old predicate drops torn 64-bit token", DropOld(0x25, kTornToken64, true), true);
    check_bool("new predicate still drops torn 64-bit token (untrusted, zero high bits)",
               DropNew(0x25, kTornToken64, true, false, true), true);

    // --- 32-bit untrusted tokens: readability is the only remaining signal. ---
    check_bool("new predicate drops unreadable untrusted 32-bit token",
               DropNew(0x25, 0x25820F60u, /*readable=*/false, false, false), true);
    // A readable-but-untrusted 32-bit filepath command passes readability alone; this is the
    // pre-existing residual risk class on 32-bit and is deliberately unchanged by the fix
    // (the interpreter's own w1 < 0x10000 guard plus resource-lookup failure still contain it).
    check_bool("new predicate passes readable untrusted 32-bit pointer",
               DropNew(0x25, 0x08123456u, /*readable=*/true, false, false), false);

    // --- Non-filepath opcodes are never the backstop's business. ---
    check_bool("plain SETTIMG (0xFD) untouched by old", DropOld(0xFD, 0x1234u, true), false);
    check_bool("plain SETTIMG (0xFD) untouched by new",
               DropNew(0xFD, 0x1234u, true, false, false), false);

    // --- Every filepath opcode byte is covered, trusted emits pass on all of them. ---
    const uint8_t ops[] = { 0x24u, 0x25u, 0x27u, 0x28u, 0x29u };
    for (uint8_t op : ops) {
        char name[80];
        snprintf(name, sizeof(name), "opcode 0x%02X: old drops 32-bit / new trusts emit", op);
        const bool oldDrops = DropOld(op, k3dsRodataString, true);
        const bool newPasses = !DropNew(op, k3dsRodataString, true, true, false);
        check_bool(name, oldDrops && newPasses, true);
    }

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
