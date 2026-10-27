/* port/3ds/game/gdx3ds_ek_nimach_game.c -- ekperf3 `[native] ek_nimach`: the Expansion Kit's machine
 * select and machine settings machines on the NRR. Expansion Kit builds only.
 *
 * `[native] ni_mach` (nimach, decomp-3ds-nimach.patch) draws the 30 machines of the machine select
 * grid, and the machine settings bodies + reflection model, from the NRR's recorded meshes instead of
 * walking every body list through the interpreter's CPU transform. That patch is compiled out of the
 * Expansion Kit (`!defined(EXPANSION_KIT)`): the EK loop differs (a render mode per slot, a grey env
 * colour while gGreyscaleMachinePart is set, player-designed machines whose lists are rebuilt from the
 * design, custom-part reflection models). decomp-3ds-ek-nimach.patch adds the EK versions, and asks
 * this file whether to use them:
 *
 *   gdx_ek_nimach_level()  0 unless [native] ek_nimach = 1; then the live ni_mach bits
 *                          (gdx3ds_nimach_level: bit 1 machine select, bit 2 machine settings / GP
 *                          next machine; 0 while the NRR mach category is off). So the EK path needs
 *                          BOTH keys: ni_mach picks the screens, ek_nimach opts the Expansion Kit in.
 *   gdx_ek_nimach_note()   the per-frame counts behind the [eknimach] receipt.
 *
 * Lever off (the default): level 0, the decomp writes the stock loops, no marker, nothing recorded.
 * Receipts: `[eknimach] init ...` once; `[eknimach] calls= select= settings= nrr= stockonly=` every 64
 * calls (beside the render side's own `[nimach]` window receipt). */
#include "global.h"

#include <stdint.h>

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_native_ready(void) __attribute__((weak));
extern int gdx3ds_native_raw(const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_nimach_level(void) __attribute__((weak));

#define EKNIMACH_WINDOW 64 /* calls per [eknimach] receipt */
#define EKNIMACH_CTX_SELECT 1
#define EKNIMACH_CTX_SETTINGS 2

static int sLever = -1; /* [native] ek_nimach: -1 unlatched, 0 off, 1 on */

static struct {
    uint32_t calls;
    uint32_t select;
    uint32_t settings;
    uint32_t nrr;
    uint32_t stockOnly;
} sWin;

static int EkNimachLatch(void) {
    if (&gdx3ds_native_ready == NULL || &gdx3ds_native_raw == NULL || !gdx3ds_native_ready()) {
        return -1; /* the ini is not in yet: latch at a later frame */
    }
    int level = gdx3ds_native_raw("ek_nimach", 0) != 0 ? 1 : 0;
    int niMach = gdx3ds_native_raw("ni_mach", 0);
    if (level != 0 && &gdx3ds_nimach_level == NULL) {
        level = 0;
    }
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[eknimach] init ek_nimach=%d ni_mach=%d (Expansion Kit machine select / settings machines on "
                     "the NRR; needs the ni_mach bits and [native] nrr_mach -- see [nimach] relatch for the live "
                     "value)%s\n",
                     level, niMach, (level != 0 && niMach == 0) ? " INERT: ni_mach=0" : "");
    }
    return level;
}

static void EkNimachReceipt(void) {
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[eknimach] calls=%lu select=%lu settings=%lu nrr=%lu stockonly=%lu (per %u calls; nrr = machines "
                     "drawn from records, stockonly = player-designed machines kept stock)\n",
                     (unsigned long)sWin.calls, (unsigned long)sWin.select, (unsigned long)sWin.settings,
                     (unsigned long)sWin.nrr, (unsigned long)sWin.stockOnly, (unsigned)EKNIMACH_WINDOW);
    }
    sWin.calls = 0;
    sWin.select = 0;
    sWin.settings = 0;
    sWin.nrr = 0;
    sWin.stockOnly = 0;
}

s32 gdx_ek_nimach_level(void) {
    if (sLever < 0) {
        sLever = EkNimachLatch();
        if (sLever < 0) {
            return 0;
        }
    }
    if (sLever == 0) {
        return 0;
    }
    return gdx3ds_nimach_level();
}

void gdx_ek_nimach_note(s32 ctx, s32 taken, s32 stockOnly) {
    sWin.calls++;
    if (ctx == EKNIMACH_CTX_SELECT) {
        sWin.select++;
    } else if (ctx == EKNIMACH_CTX_SETTINGS) {
        sWin.settings++;
    }
    sWin.nrr += (uint32_t)taken;
    sWin.stockOnly += (uint32_t)stockOnly;
    if (sWin.calls >= EKNIMACH_WINDOW) {
        EkNimachReceipt();
    }
}
