/* port/3ds/game/gdx3ds_snapshot_nosnap.c -- snap-fast: the runtime NOSNAP table of the snapshot
 * module (port/3ds/gdx_snapshot.c, CarveHoles; contract port/gdx_snapshot.h). Read only.
 *
 * Objects INSIDE the snapshot blocks that no race writes: carved out of the image at arm time, so a
 * save / restore skips them (card-4 copied all 892,832 bytes of the N3DS-EK image; ~530 KB of it is
 * this table). Not under decomp/src/, so this TU's own (rodata) table is not in the snapshot.
 *
 * WHY IT IS SOUND. A window never spans a scope exit (gdx_snapshot.c: a mode change abandons it), so
 * an object that holds the same bytes through every in-race tick holds, at a restore, exactly what
 * the save would have copied. Each entry was chosen by the dirty census ([debug] snap_census = 1 +
 * tools/dev/snapcensus.py: one bit per 32-byte block, set by any change between two in-race frames)
 * over the replay corpus on both EK builds: every block of every entry stayed clean. The census is
 * evidence, not proof, so the gate keeps watching: under [debug] snap_test every hole must hold the
 * same bytes at the save, at the real T+N and after the resim, or the window is a MISS
 * ("[snap] NOSNAP changed ... hole=<name>"). [debug] snap_holes = 0 restores card-4's full image.
 *
 * Deliberately NOT here although the census found them clean: anything REQUIRED (gCourseCtx, 51 KB,
 * is the largest clean object left), the race-feature tables (gEffects, gCourseEffects,
 * gCourseFeatures, gObjects, gLandmines, gJumps, gPodiumDrawData), the texture registry D_800E33E0
 * (a past gate MISS), file-static objects (no external name) and the small .data tables.
 *
 * THE DECLARATIONS. Only the address and the byte count matter, so every object is declared here as
 * an incomplete byte array -- this TU includes no decomp header, so nothing conflicts with the real
 * types -- and WEAK, so a build without the object (the shipped non-EK config has no course editor,
 * no 64DD file system, no EK textures) carves nothing for it. The byte counts are the ELF's (nm -S,
 * identical on O3DS-EK and N3DS-EK); `tools/dev/snapcensus.py <elf> <log> --check-holes` proves them
 * against a build from the `[snap] hole` receipts every armed run prints. */

typedef struct GdxSnapRequired {
    const void* ptr;
    unsigned int size;
    const char* name;
} GdxSnapRequired;

int gdx3ds_snap_game_nosnap(const GdxSnapRequired** out);

extern unsigned char D_xk2_80105540[] __attribute__((weak));
extern unsigned char D_xk2_80128DF8[] __attribute__((weak));
extern unsigned char D_xk2_800F7408[] __attribute__((weak));
extern unsigned char D_8011C220[] __attribute__((weak));
extern unsigned char D_xk2_80136EF8[] __attribute__((weak));
extern unsigned char D_80033840[] __attribute__((weak));
extern unsigned char D_802CDFD8[] __attribute__((weak));
extern unsigned char D_807B6528[] __attribute__((weak));
extern unsigned char D_802D0620[] __attribute__((weak));
extern unsigned char D_xk2_80119920[] __attribute__((weak));
extern unsigned char D_xk1_8003A5D8[] __attribute__((weak));
extern unsigned char D_xk1_8003B430[] __attribute__((weak));
extern unsigned char D_xk1_80033630[] __attribute__((weak));
extern unsigned char D_80128690[] __attribute__((weak));
extern unsigned char D_xk2_80128990[] __attribute__((weak));
extern unsigned char gMfsRamArea[] __attribute__((weak));
extern unsigned char D_807801E0[] __attribute__((weak));
extern unsigned char D_i1_80415190[] __attribute__((weak));
extern unsigned char gFileAllocationTable[] __attribute__((weak));
extern unsigned char gCourseInfos[] __attribute__((weak));
extern unsigned char gFaultMgr[] __attribute__((weak));
extern unsigned char gCustomMachinesInfoBackup[] __attribute__((weak));
extern unsigned char aCupSelectDD2Tex[] __attribute__((weak));
extern unsigned char aCupSelectDD1Tex[] __attribute__((weak));
extern unsigned char D_xk3_8013B280[] __attribute__((weak));
extern unsigned char aMachineCreateOkTex[] __attribute__((weak));
extern unsigned char D_xk3_80137590[] __attribute__((weak));
extern unsigned char aMachineCreateMachineRegistrationTex[] __attribute__((weak));
extern unsigned char D_xk3_80138930[] __attribute__((weak));
extern unsigned char aMachineCreateSuperMarkerTex[] __attribute__((weak));
extern unsigned char D_8013EB80[] __attribute__((weak));
extern unsigned char D_8013D180[] __attribute__((weak));
extern unsigned char D_xk3_8013A980[] __attribute__((weak));
extern unsigned char D_8013C580[] __attribute__((weak));
extern unsigned char D_8013F380[] __attribute__((weak));
extern unsigned char D_8013E180[] __attribute__((weak));
extern unsigned char D_8013C980[] __attribute__((weak));
extern unsigned char D_8013D980[] __attribute__((weak));
extern unsigned char D_8013C780[] __attribute__((weak));
extern unsigned char D_8013E980[] __attribute__((weak));
extern unsigned char aMachineCreateHighlightedOkTex[] __attribute__((weak));
extern unsigned char aMachineCreateSettingsTex[] __attribute__((weak));
extern unsigned char aDDLoadBlueFalconTex[] __attribute__((weak));
extern unsigned char aDDLoadMachineTrailTex[] __attribute__((weak));
extern unsigned char aMasterDDTrophyTex[] __attribute__((weak));
extern unsigned char aCopyrightDDTex[] __attribute__((weak));
extern unsigned char aN64DDLogoTex[] __attribute__((weak));
extern unsigned char aRecordsWhereToCopyTex[] __attribute__((weak));
extern unsigned char aRecordsDiskGhost3Tex[] __attribute__((weak));
extern unsigned char aRecordsDiskGhost1Tex[] __attribute__((weak));
extern unsigned char aMenuCopyGhostTex[] __attribute__((weak));
extern unsigned char aRecordsDiskGhost2Tex[] __attribute__((weak));
extern unsigned char aRecordsInsertDiskToCopyToTex[] __attribute__((weak));
extern unsigned char aRecordsWhichGhostToCopyTex[] __attribute__((weak));
extern unsigned char aRecordsCassetteGhostTex[] __attribute__((weak));
extern unsigned char aRecordsGhostCopiedTex[] __attribute__((weak));

#define HOLE(sym, bytes) { (const void*) (sym), (unsigned int) (bytes), #sym }

static const GdxSnapRequired sNosnap[] = {
    /* The course editor (overlays/course_edit, expansion_kit): edit buffers, vertex / display-list scratch, the edited course copies */
    HOLE(D_xk2_80105540, 82080),
    HOLE(D_xk2_80128DF8, 57600),
    HOLE(D_xk2_800F7408, 51248),
    HOLE(D_8011C220, 50288),
    HOLE(D_xk2_80136EF8, 28816),
    HOLE(D_80033840, 27684),
    HOLE(D_802CDFD8, 10500),
    HOLE(D_807B6528, 10500),
    HOLE(D_802D0620, 10496),
    HOLE(D_xk2_80119920, 10496),
    HOLE(D_xk1_8003A5D8, 3672),
    HOLE(D_xk1_8003B430, 1800),
    HOLE(D_xk1_80033630, 1056),
    HOLE(D_80128690, 768),
    HOLE(D_xk2_80128990, 768),
    /* The 64DD file system (leo/mfs): its RAM area, sector buffers and FAT */
    HOLE(gMfsRamArea, 59184),
    HOLE(D_807801E0, 19728),
    HOLE(D_i1_80415190, 19728),
    HOLE(gFileAllocationTable, 5748),
    /* Menu / machine-create / records textures (EK asset buffers, decompressed at menu load) */
    HOLE(aCupSelectDD2Tex, 3412),
    HOLE(aCupSelectDD1Tex, 1176),
    HOLE(D_xk3_8013B280, 3840),
    HOLE(aMachineCreateOkTex, 1024),
    HOLE(D_xk3_80137590, 3584),
    HOLE(aMachineCreateMachineRegistrationTex, 1536),
    HOLE(D_xk3_80138930, 512),
    HOLE(aMachineCreateSuperMarkerTex, 576),
    HOLE(D_8013EB80, 2048),
    HOLE(D_8013D180, 2048),
    HOLE(D_xk3_8013A980, 2304),
    HOLE(D_8013C580, 512),
    HOLE(D_8013F380, 2048),
    HOLE(D_8013E180, 2048),
    HOLE(D_8013C980, 2048),
    HOLE(D_8013D980, 2048),
    HOLE(D_8013C780, 512),
    HOLE(D_8013E980, 512),
    HOLE(aMachineCreateHighlightedOkTex, 1024),
    HOLE(aMachineCreateSettingsTex, 864),
    HOLE(aDDLoadBlueFalconTex, 1024),
    HOLE(aDDLoadMachineTrailTex, 1024),
    HOLE(aMasterDDTrophyTex, 7972),
    HOLE(aCopyrightDDTex, 1784),
    HOLE(aN64DDLogoTex, 708),
    HOLE(aRecordsWhereToCopyTex, 2048),
    HOLE(aRecordsDiskGhost3Tex, 1024),
    HOLE(aRecordsDiskGhost1Tex, 1024),
    HOLE(aMenuCopyGhostTex, 1024),
    HOLE(aRecordsDiskGhost2Tex, 1024),
    HOLE(aRecordsInsertDiskToCopyToTex, 4096),
    HOLE(aRecordsWhichGhostToCopyTex, 2048),
    HOLE(aRecordsCassetteGhostTex, 1024),
    HOLE(aRecordsGhostCopiedTex, 1024),
    /* Boot / course-load / menu tables */
    HOLE(gCourseInfos, 13440),
    HOLE(gFaultMgr, 2520),
    HOLE(gCustomMachinesInfoBackup, 992),
};

int gdx3ds_snap_game_nosnap(const GdxSnapRequired** out) {
    *out = sNosnap;
    return (int) (sizeof(sNosnap) / sizeof(sNosnap[0]));
}
