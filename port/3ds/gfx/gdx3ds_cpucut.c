/* port/3ds/gfx/gdx3ds_cpucut.c -- o3ds-cpucuts: the cuts' shared A/B block clock and the
 * [cpucut] receipt. Contract: include/gdx3ds_cpucut.h. In the gfx library (not the executable) so
 * the renderer's levers and gdx3ds-dl-tests link it like gdx3ds_hitch_ev.c. */
#include "gdx3ds_cpucut.h"

#include <stdio.h>
#include <string.h>

#if defined(__3DS__)
#include <3ds.h>

/* The executable's log and clocks (absent from gdx3ds-dl-tests; the host tests build this file
 * without them: no receipt, the default block). */
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern int gdx3ds_filelog_receipt_every(void) __attribute__((weak));
extern int gdx3ds_mprof_active __attribute__((weak));
#endif

#define CPUCUT_MAX_LEVERS 12
#define CPUCUT_DEFAULT_BLOCK 512u

typedef struct CpucutLever {
    const char* name;
    const int* value;
    unsigned bit;
} CpucutLever;

static CpucutLever sLevers[CPUCUT_MAX_LEVERS];
static int sNumLevers = 0;
static unsigned long sBlock = 0;
static unsigned sSelfCount = 0;

int gdx3ds_cpucut_clamp(int value, int fallback) {
    if (value == 0 || value == 1 || value == 2) {
        return value;
    }
    return fallback;
}

void gdx3ds_cpucut_lever(const char* name, const int* value, unsigned bit) {
    int i;
    if (name == NULL || value == NULL) {
        return;
    }
    for (i = 0; i < sNumLevers; i++) {
        if (strcmp(sLevers[i].name, name) == 0) {
            sLevers[i].value = value;
            sLevers[i].bit = bit;
            return;
        }
    }
    if (sNumLevers >= CPUCUT_MAX_LEVERS) {
        return;
    }
    sLevers[sNumLevers].name = name;
    sLevers[sNumLevers].value = value;
    sLevers[sNumLevers].bit = bit;
    sNumLevers++;
}

int gdx3ds_cpucut_on(int value, unsigned bit) {
    if (value == 1) {
        return 1;
    } else if (value == 2) {
        return ((sBlock >> bit) & 1ul) == 0ul;
    }
    return 0;
}

unsigned long gdx3ds_cpucut_block(void) {
    return sBlock;
}

static unsigned BlockIterations(void) {
#if defined(__3DS__)
    if (&gdx3ds_filelog_receipt_every != NULL) {
        const int every = gdx3ds_filelog_receipt_every();
        if (every > 0) {
            return (unsigned)every;
        }
    }
#endif
    return CPUCUT_DEFAULT_BLOCK;
}

static int AnyAbLever(void) {
    int i;
    for (i = 0; i < sNumLevers; i++) {
        if (*sLevers[i].value != 0) {
            return 1;
        }
    }
    return 0;
}

static void EmitBlock(unsigned long frame, unsigned iterations) {
    char line[320];
    int n;
    int i;
    if (sNumLevers == 0 || !AnyAbLever()) {
        return;
    }
    n = snprintf(line, sizeof(line), "[cpucut] f=%lu blk=%lu n=%u |", frame, sBlock, iterations);
    for (i = 0; i < sNumLevers && n > 0 && (size_t)n < sizeof(line); i++) {
        const int v = *sLevers[i].value;
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%d/%d", sLevers[i].name, v,
                      gdx3ds_cpucut_on(v, sLevers[i].bit));
    }
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
#if defined(__3DS__)
    svcOutputDebugString(line, n);
    if (&gdx3ds_filelog_write != NULL) {
        gdx3ds_filelog_write(line, (size_t)n);
    }
#endif
}

static void EndBlock(unsigned long frame, unsigned iterations) {
    EmitBlock(frame, iterations);
    sBlock++;
    sSelfCount = 0;
}

static int MprofDrives(void) {
#if defined(__3DS__)
    return &gdx3ds_mprof_active != NULL && gdx3ds_mprof_active != 0;
#else
    return 0;
#endif
}

void gdx3ds_cpucut_frame_end(unsigned long frame) {
    if (MprofDrives()) {
        return; /* gdx3ds_cpucut_window_end closes the block at the [c5cat] window */
    }
    if (++sSelfCount >= BlockIterations()) {
        EndBlock(frame, sSelfCount);
    }
}

void gdx3ds_cpucut_window_end(unsigned long frame, unsigned iterations) {
    EndBlock(frame, iterations);
}
