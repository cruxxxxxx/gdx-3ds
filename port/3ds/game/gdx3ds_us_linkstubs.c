/* port/3ds/game/gdx3ds_us_linkstubs.c — link stubs the GDX_EXPANSION_KIT=OFF 3DS build
 * needs. On desktop the default configuration is EK ON, where these come from
 * port/n64_leo.c (leoBootID) and port/gen/EkLinkStubs.c (ucode text markers); with EK
 * OFF neither TU compiles, so the US-only 3DS build supplies them here.
 *
 * Compiled into gdx3ds_game (PORT defines + decomp include path). */

#include "PR/leo.h"

/* n64_leo.c: populated from the disk image on EK builds; zero = no disk, which the
 * US-only title-screen probe path (shims.c LeoTestUnitReady) already reports anyway. */
LEODiskID leoBootID;

/* EkLinkStubs.c: address-compared ucode markers (n64_gfx_bridge.cpp matchesUcodeText);
 * never dereferenced as code. One u64 apiece, matching the EK stub's shape. */
unsigned long long gspF3DEX2_Rej_fifoTextStart[1];
unsigned long long gspL3DEX2_fifoTextStart[1];
