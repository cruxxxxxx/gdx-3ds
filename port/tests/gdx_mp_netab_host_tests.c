/* port/tests/gdx_mp_netab_host_tests.c — card-3: the online levers' per-race A/B
 * (port/gdx_mp_netab.c): the race clock from the START gen, the two levers' orthogonal arms (every
 * combination once in 4 races), the values that are not A/B keep their meaning, and the receipt. */
#include "gdx_mp_netab.h"

#include <stdio.h>
#include <string.h>

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

static void TestRaceIndex(void) {
    CHECK(gdx_mp_netab_race_index(0u) == 0u);
    CHECK(gdx_mp_netab_race_index(1u) == 0u);
    CHECK(gdx_mp_netab_race_index(2u) == 1u);
    CHECK(gdx_mp_netab_race_index(0xFFFFFFFFu) == 0xFFFFFFFEu);
}

static void TestTimesyncLever(void) {
    uint32_t k;
    CHECK(gdx_mp_netab_timesync_lever(3) == 3);
    CHECK(gdx_mp_netab_timesync_lever(0) == 0);
    CHECK(gdx_mp_netab_timesync_lever(16) == 16);
    CHECK(gdx_mp_netab_timesync_lever(99) == 16);
    CHECK(gdx_mp_netab_timesync_lever(-7) == 0);
    CHECK(gdx_mp_netab_timesync_lever(-1) == GDX_MP_NETAB_TIMESYNC_AB);
    for (k = 0; k < 8u; k++) {
        CHECK(gdx_mp_netab_timesync_cap_ms(5, k) == 5);
        CHECK(gdx_mp_netab_timesync_cap_ms(GDX_MP_NETAB_TIMESYNC_AB, k) == ((k & 1u) ? 16 : 3));
    }
}

static void TestNdmLever(void) {
    uint32_t k;
    CHECK(gdx_mp_netab_ndm_lever(0) == 0);
    CHECK(gdx_mp_netab_ndm_lever(1) == 1);
    CHECK(gdx_mp_netab_ndm_lever(2) == GDX_MP_NETAB_NDM_AB);
    CHECK(gdx_mp_netab_ndm_lever(7) == 1);
    for (k = 0; k < 8u; k++) {
        CHECK(gdx_mp_netab_ndm_on(0, k) == 0);
        CHECK(gdx_mp_netab_ndm_on(1, k) == 1);
        CHECK(gdx_mp_netab_ndm_on(GDX_MP_NETAB_NDM_AB, k) == ((k & 2u) ? 0 : 1));
    }
}

/* Over every 4 consecutive races from a multiple of 4 each (cap, ndm) pair runs exactly once. */
static void TestOrthogonal(void) {
    uint32_t base;
    for (base = 0; base < 64u; base += 4u) {
        int seen[2][2] = { { 0, 0 }, { 0, 0 } };
        uint32_t k;
        for (k = base; k < base + 4u; k++) {
            const int cap = gdx_mp_netab_timesync_cap_ms(GDX_MP_NETAB_TIMESYNC_AB, k);
            const int ndm = gdx_mp_netab_ndm_on(GDX_MP_NETAB_NDM_AB, k);
            seen[cap == 16][ndm]++;
        }
        CHECK(seen[0][0] == 1 && seen[0][1] == 1 && seen[1][0] == 1 && seen[1][1] == 1);
    }
}

static void TestReceipt(void) {
    char line[160];
    CHECK(!gdx_mp_netab_any(3, 1));
    CHECK(gdx_mp_netab_any(GDX_MP_NETAB_TIMESYNC_AB, 1));
    CHECK(gdx_mp_netab_any(3, GDX_MP_NETAB_NDM_AB));
    gdx_mp_netab_receipt(line, (int) sizeof(line), 1u, GDX_MP_NETAB_TIMESYNC_AB, GDX_MP_NETAB_NDM_AB, 1);
    CHECK(strcmp(line, "[netab] race gen=1 k=0 | net.mp_timesync_max_ms=ab/A=3 net.mp_ndm_exclusive=2/A=1") == 0);
    gdx_mp_netab_receipt(line, (int) sizeof(line), 4u, GDX_MP_NETAB_TIMESYNC_AB, GDX_MP_NETAB_NDM_AB, 1);
    CHECK(strcmp(line, "[netab] race gen=4 k=3 | net.mp_timesync_max_ms=ab/B=16 net.mp_ndm_exclusive=2/B=0") == 0);
    gdx_mp_netab_receipt(line, (int) sizeof(line), 2u, 3, GDX_MP_NETAB_NDM_AB, 0);
    CHECK(strcmp(line, "[netab] race gen=2 k=1 | net.mp_timesync_max_ms=3/- net.mp_ndm_exclusive=2/uds") == 0);
    printf("receipt: %s\n", line);
}

int main(void) {
    TestRaceIndex();
    TestTimesyncLever();
    TestNdmLever();
    TestOrthogonal();
    TestReceipt();
    printf("gdx_mp_netab_host_tests: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
