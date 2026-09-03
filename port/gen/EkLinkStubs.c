/* G-Diffuser — Expansion Kit link stubs (EK slice 1, hand-curated).
 *
 * Placeholder definitions for symbols the EK code references but whose real
 * backing is deferred. Grouped by what replaces them later:
 *   [SAVE]   EK save/ghost persistence — arrives with the save-support slice.
 *   [SEG]    ROM segment table / loaders — port loads segments via the asset
 *            binding system instead (decomp_port.c, gdx_load_venue_*).
 *   [ASSET]  cart/disk asset symbols not yet bound — blank (zero) data, real
 *            binding rows come from the JP cart yaml / disk yaml later.
 *            Sized generously, never 1 byte: interior-offset references and
 *            the runtime range resolvers need real storage spans.
 *   [MARKER] overlay linker markers — meaningless on host, data tokens only.
 *   [MISC]   debug/COP0/attract-demo leftovers — safe no-ops.
 */

/* ---- [SAVE] EK save + ghost record surface ------------------------------ */
/* Save_CalculateGhostRecordChecksum/Save_CalculateSaveCourseRecordChecksum/
   Save_ClearCourseRecord/Save_ClearGhostRecord/Save_GetDDStaffGhostCompletion/
   Save_GetDDStaffGhostRecordTime/Save_InitCourseRecord/Save_LoadGhostData/
   Save_ReadGhostData/Save_SaveGhostData/Save_SaveGhostRecord/
   Save_SetDDStaffGhostComplete/Save_WriteGhostData/Save_WriteGhostRecord and the
   real sDDStaffGhostRecordTimes[] data are real now: save.c (save-system slice). */

/* ---- [SEG] ROM segment system (port replaces with asset bindings) ------- */
/* segment.c:968 venue segment DMA — the port loads venue textures through
   gdx_load_venue_texture_segment at race start instead. */
long func_80077AD8() { return 0; }
/* cartridge_offsets.c ROM segment offset pairs; EK HUD/audio code indexes
   [15] etc. Zeroed: dependent Dma_LoadAssets calls become no-ops against
   ROM offset 0. TODO(EK slice 2): fill with real US ROM offsets. */
unsigned int gRomSegmentPairs[29][2];
long func_80708D88() { return 0; }
/* func_i2_800A8CE4 is real now: save.c (save-system slice). */

/* ---- [ASSET] unbound cart/disk asset symbols ---------------------------- */
/* Re-checked 2026-07-24 (Create Machine MARK icon US-cart offset rebase):
   D_4003180/D_40033C0/D_4003600 are RETIRED from this file -- they are NOT
   hud_gfx after all. They are 3 more members of the same JP-cart-named
   8-icon MARK strip as D_4002640/2880/AC0/D00/F40 (same overlay, same array,
   same 0x240 stride), and the earlier "unmapped hud_gfx territory" search
   was looking in the wrong image family. Decoding create_machine_textures at
   (nominal offset - 0x900) produced legible, ares-order-correct icon art for
   all 8 slots (see decomp/assets/yaml/us/rev0/create_machine_textures.yaml
   for the verified offsets); they are now real bindings, generated into
   AssetBindings.c.

   D_4001500 remains genuinely unbound: its nominal offset (0x1500) falls
   mid-texture inside hud_gfx's aThirdPlaceMarkerMPTex (confirmed again here:
   hud_gfx romBase 0x1B8550 + aThirdPlaceMarkerMPTex's offset 0x14E8..0x16E8
   straddles 0x1500), and shifting it by -0x900 (0xC00) instead lands exactly
   on create_machine_textures' own aCreateMachineCockpitTex ("COCKPIT" text),
   not a distinct LOAD glyph -- both hypotheses just collide with unrelated,
   already-correctly-bound textures. TODO(EK slice 2): still requires the
   actual JP EK cart ROM to recover (D_4001500 -> Create Machine FILE menu's
   LOAD entry contentsTex icon, in overlays/expansion_kit/A3AE0.c). */
unsigned char D_4001500[0x2000];
/* D_4011D78 (editor pause menu's TLUT setup display list) is real now: it is
   the base game's own hud_gfx asset aMenuTextTlutSetupDL (already bound in
   gen/AssetBindings.c), referenced directly by name from
   course_edit/1A7D50.c instead of through this zero-filled placeholder. */
/* D_802D0620 (course editor CourseSegment[64] staging buffer) is real now:
   course_gadget_context.c, alongside D_802CB6D0/D_807B6528/D_802CDFD8
   (fixes an undersized-placeholder OOB write, see course_gadget_context.c). */
/* D_i2_80111848 (30-byte custom-unlock flags array) is real now: save.c (save-system slice). */
unsigned char D_34E[8];
unsigned char D_391[8];
/* Venue texture banks 0xB000+ (EK courses address more banks than the base
   game's 0x0000-0x8000). TODO(EK slice 2): extend ResolveVenueBankAlias. */
unsigned char D_A00B000[0x240];
unsigned char D_A00B240[0x240];
unsigned char D_A00B480[0x240];
unsigned char D_A00B6C0[0x240];
unsigned char D_A00B900[0x240];
unsigned char D_A00BB40[0x240];
unsigned char D_A00BD80[0x240];
/* EK course ROM segment markers (Silence 3 staff ghost data). */
unsigned char silence_3_ROM_START[1];
unsigned char silence_3_staff_ghost_ROM_START[1];

/* ---- [MARKER] overlay linker markers ------------------------------------ */
unsigned char course_edit_VRAM[1];
unsigned char course_edit_VRAM_END[1];
unsigned char course_edit_textures_VRAM[1];
unsigned char course_edit_textures_VRAM_END[1];
unsigned char course_select_BSS_END[1];
unsigned char course_select_BSS_START[1];
unsigned char course_select_DATA_START[1];
unsigned char course_select_RODATA_END[1];
unsigned char course_select_ROM_START[1];
unsigned char course_select_TEXT_END[1];
unsigned char course_select_TEXT_START[1];
unsigned char ending_BSS_END[1];
unsigned char ending_BSS_START[1];
unsigned char ending_DATA_START[1];
unsigned char ending_RODATA_END[1];
unsigned char ending_ROM_START[1];
unsigned char ending_TEXT_END[1];
unsigned char ending_TEXT_START[1];
unsigned char expansion_kit_BSS_END[1];
unsigned char expansion_kit_BSS_START[1];
unsigned char expansion_kit_DATA_END[1];
unsigned char expansion_kit_DATA_START[1];
unsigned char expansion_kit_ROM_START[1];
unsigned char expansion_kit_TEXT_END[1];
unsigned char expansion_kit_TEXT_START[1];
unsigned char expansion_kit_VRAM[1];
unsigned char expansion_kit_textures_VRAM[1];
unsigned char expansion_kit_textures_VRAM_END[1];
unsigned char machine_create_VRAM[1];
unsigned char machine_create_VRAM_END[1];
unsigned char ovl_i3_BSS_END[1];
unsigned char ovl_i3_BSS_START[1];
unsigned char ovl_i3_DATA_START[1];
unsigned char ovl_i3_RODATA_END[1];
unsigned char ovl_i3_ROM_START[1];
unsigned char ovl_i3_TEXT_END[1];
unsigned char ovl_i3_TEXT_START[1];
unsigned char ovl_i4_BSS_END[1];
unsigned char ovl_i4_BSS_START[1];
unsigned char ovl_i4_DATA_START[1];
unsigned char ovl_i4_RODATA_END[1];
unsigned char ovl_i4_ROM_START[1];
unsigned char ovl_i4_TEXT_END[1];
unsigned char ovl_i4_TEXT_START[1];
unsigned char ovl_i6_BSS_END[1];
unsigned char ovl_i6_BSS_START[1];
unsigned char ovl_i6_DATA_START[1];
unsigned char ovl_i6_RODATA_END[1];
unsigned char ovl_i6_ROM_START[1];
unsigned char ovl_i6_TEXT_END[1];
unsigned char ovl_i6_TEXT_START[1];
unsigned char ovl_i9_RODATA_END[1];
unsigned char ovl_i9_TEXT_END[1];
unsigned char ovl_i9_TEXT_START[1];
unsigned char records_BSS_END[1];
unsigned char records_BSS_START[1];
unsigned char records_DATA_START[1];
unsigned char records_RODATA_END[1];
unsigned char records_ROM_START[1];
unsigned char records_TEXT_END[1];
unsigned char records_TEXT_START[1];
/* RSP microcode blobs (reject + line variants the EK selects). */
unsigned long long gspF3DEX2_Rej_fifoTextStart[1];
unsigned long long gspF3DEX2_Rej_fifoDataStart[1];
unsigned long long gspL3DEX2_fifoTextStart[1];
unsigned long long gspL3DEX2_fifoDataStart[1];

/* ---- [MISC] -------------------------------------------------------------- */
/* Attract demo excluded (broken WIP C upstream); menu hooks still link.
   EADDemo_Draw is a Gfx* fn(Gfx*) whose return becomes the caller's display
   list cursor (game.c draw dispatch) — pass the cursor through, never 0. */
long EADDemo_Init() { return 0; }
long EADDemo_Update() { return 0; }
void* EADDemo_Draw(void* gfx) { return gfx; }
/* LeoGetKAdr/LeoGetAAdr: real C implementations live in port/n64_leo.c
   (algorithms recovered from the US rev0 ROM's leo library). */
/* debug remote monitor printf + COP0 register writes */
long rmonPrintf() { return 0; }
long __osSetCause() { return 0; }
long __osSetCount() { return 0; }
/* CPU data-cache maintenance (disk audio lib) — meaningless on host. */
long Audio_WritebackDCache() { return 0; }
long Audio_InvalDCache() { return 0; }
