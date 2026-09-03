/* port/3ds/game/gdx3ds_livery_diag.c — SHIP-LIVERY-2 game-state receipts.
 *
 * Called (weak-linked) by the citro3d backend's [livery] one-shot window
 * (port/3ds/gfx/gfx_citro3d.cpp GdxLiveryDiagFrameBegin) on each of its 3 armed
 * race frames. Dumps the racer/machine identity state the wrong-livery hypotheses
 * hinge on, so the backend's per-draw texture-identity log can be read against it:
 *
 *   [livery] state rN id=I ch=C mIdx=M cust=T lod=L body=RR,GG,BB
 *     — gRacers[N]: character vs machineIndex (a mismatch means racer N draws
 *       another machine's texture DL: D_800CDD38[machineIndex] is built for
 *       gMachines[machineIndex]'s character by func_8008D7E8's (i,i) walk), the
 *       racer's customType (CUSTOM_MACHINE_EDITED = 2 means the slot's texture DL
 *       was built by Machine_DrawLoadCustomTextures — the stale-custom-decal
 *       hypothesis), machineLod (selects D_800CDDB0[mIdx*6+lod-1]) and the body
 *       ENV colour fed to gDPSetEnvColor (the player's Blue Falcon is blue-ish;
 *       a rival yellow here would be a state bug, not a texture bug).
 *   [livery] state mach cust=[...] ccs=[...]
 *     — gMachines[0..29].customType and gCustomMachinesInfo.characterCustomState
 *       (>0 = custom machine armed for that character slot; nonzero garbage here
 *       on a fresh 3DS save = the custom path is being taken spuriously).
 *   [livery] state pchar=C
 *     — gPlayerCharacters[0], the player's character selection.
 *
 * Compiled into gdx3ds_game (decomp include paths). No decomp sources touched.
 */
#include "global.h"
#include "fzx_racer.h"
#include "fzx_machine.h"

/* Declared directly instead of <3ds.h>/<stdio.h>: the decomp headers this file
 * needs for the struct layouts clash with newlib's (see gdx_dbg_logf's note in
 * port/n64_sched.c — decomp TUs cannot include <stdio.h>). ILP32: size_t is
 * unsigned int. */
extern int snprintf(char* str, unsigned int size, const char* fmt, ...);
extern void svcOutputDebugString(const char* str, int length);

extern Machine gMachines[30];
extern CustomMachinesInfo gCustomMachinesInfo;
extern s16 gPlayerCharacters[4]; /* defined in racer.c; no header extern exists */

static void LiveryEmit(const char* msg, int n) {
    if (n > 0) {
        svcOutputDebugString(msg, n);
    }
}

void gdx3ds_livery_game_receipt(void) {
    char msg[192];
    int n;
    int i;

    /* Racers 0..7 cover the player (gRacers[0] in GP; drawn LAST by racer.c's
     * sLastRacer→gRacers walk) and enough rivals to see the pattern. */
    for (i = 0; i < 8 && i < TOTAL_RACER_COUNT; i++) {
        Racer* r = &gRacers[i];
        n = snprintf(msg, sizeof(msg),
                     "[livery] state r%d id=%d ch=%d mIdx=%d cust=%d lod=%d body=%02x,%02x,%02x", i,
                     (int)r->id, (int)r->character, (int)r->machineIndex, (int)r->customType,
                     (int)r->machineLod, (unsigned)(r->bodyR & 0xFF), (unsigned)(r->bodyG & 0xFF),
                     (unsigned)(r->bodyB & 0xFF));
        LiveryEmit(msg, n);
    }

    {
        char cust[64];
        char ccs[64];
        int p = 0;
        for (i = 0; i < 30; i++) {
            p += snprintf(cust + p, sizeof(cust) - p, "%x", (unsigned)(gMachines[i].customType & 0xF));
            if (p >= (int)sizeof(cust) - 2) {
                break;
            }
        }
        p = 0;
        for (i = 0; i < 30; i++) {
            int v = gCustomMachinesInfo.characterCustomState[i];
            p += snprintf(ccs + p, sizeof(ccs) - p, "%c",
                          v > 0 ? ('0' + (v % 10)) : (v == 0 ? '.' : '-'));
            if (p >= (int)sizeof(ccs) - 2) {
                break;
            }
        }
        n = snprintf(msg, sizeof(msg), "[livery] state mach cust=[%s] ccs=[%s]", cust, ccs);
        LiveryEmit(msg, n);
    }

    n = snprintf(msg, sizeof(msg), "[livery] state pchar=%d", (int)gPlayerCharacters[0]);
    LiveryEmit(msg, n);
}
