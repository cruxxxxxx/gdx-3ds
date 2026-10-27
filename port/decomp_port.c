// G-Diffuser — decomp-side port subsystems.
// Compiled WITH the decomp's headers and flags (part of the gdiffuser_game target), so it sees
// the real game types. Holds the port reimplementations and placeholders for symbols that lived
// in N64-platform files excluded from the host build: segment system, save, fixed-address data.

#include "global.h"
#include "fzx_camera.h"
#include "fzx_course.h"
#include "fzx_game.h"
#include "fzx_racer.h" /* gRacers + the ATTACK_STATE_* enum, for the side-attack predicate below */
#include "n64_rdram.h"
/* Last, like n64_rdram.h: it declares size_t parameters and deliberately includes no platform
   headers of its own (see the contract note at the top of it), so global.h must land first. */
#include "gdx_camera_pose.h"
#include "gdx_discord.h" /* GdxDiscordSnapshot, filled by gdx_discord_snapshot below */
#include "gdx_snapshot.h" /* mp-snapshot: GDX_SNAP_STATE_BSS for gSegments */

/* port_log.h pulls in <stdio.h> which clashes with the decomp's libc/stdint.h.
   Use gdx_ck (defined in n64_sched.c, which CAN include stdio.h) for logging. */
extern void gdx_ck(const char* s);
/* Label+value sibling of gdx_ck (prints "<label>=<dec> (0x<hex>)"). Declared at file scope so
   the RDRAM messages in gdx_rdram_init() can report GDX_RDRAM_SIZE itself instead of restating
   the buffer size as a prose literal that has to be kept in sync by hand. */
extern void gdx_cki(const char* s, int v);
extern void gdx_seg_log(const char* kind, int seg, uintptr_t raw, void* resolved);
extern void gdx_addr_log(const char* kind, uintptr_t raw, void* resolved);
/* Printf-style sibling of gdx_ck, same stdio.h-avoidance reason (defined in
   n64_sched.c). Unlike gdx_ck/gdx_cki it is NOT gated behind GDX_TRACE -- it
   always reaches stderr/OutputDebugString and additionally persists to
   gdiffuser-run.log whenever GDX_LOG (or another diagnostic env var) is set
   (see port_log.h's gdx_log_file_enabled). Used below for the segment-9
   activation diagnostic (Course Edit node-info panel scatter investigation). */
extern void gdx_dbg_logf(const char* fmt, ...);
extern void* gdx_host_calloc(size_t count, size_t size);
extern void  gdx_host_exit(int status);
extern void  gdx_host_abort(void);
extern void* gdx_resolve_registered_host_address(unsigned int addr);
extern void* gdx_resolve_module_host_address(unsigned int addr);
extern void* gdx_ensure_asset_segment_for_symbol(unsigned int symLow32, unsigned int* outOffset);
extern int   gdx_load_venue_texture_segment(int venue);
extern s32   gGameMode;
/* Declared locally (not via the port headers, which pull in <stdio.h>/<stdlib.h>
   and clash with the decomp's libc/stdint.h) so the [seg9diag] gate below can
   read the GDX_LOG developer gate. Defined in port/gdx_dev_gates.c; see that
   header for the bucket policy and the environment/setting precedence. */
extern int gdx_dev_gate_log_file(void);

extern unsigned char* gdx_rom_buffer;
extern size_t gdx_rom_size;
/* Single archive-first byte-source shim. The seg-9 machine_models
 * and seg-5 podium loaders below stage their compressed MIO0 span through this
 * instead of reading gdx_rom_buffer directly; it returns verbatim ROM bytes, so
 * mio0Decode sees byte-identical input. See port/gdx_segment_source.{h,c}. */
extern int GdxSegmentSourceRead(unsigned int romBase, unsigned int size, void* dst);
extern unk_80225800 D_80225800;
#ifdef EXPANSION_KIT
extern unk_80128C94* D_80128C90;
extern unk_80128C94* D_80128C94;
#endif

// ---- RDRAM host buffer globals ----------------------------------------------
// gdx_rdram: single 16MB contiguous buffer allocated by gdx_rdram_init().
// All N64 physical addresses resolve to gdx_rdram + phys.
// gdx_gfxpool: pointer to the GfxPool D_1000000 object (alias, not RDRAM-resident).
// gdx_rdram_bump: byte offset of the next free arena byte (carves upward).
// gdx_rdram_arena_start: first arena byte after the GfxPool reservation AND the
//   dedicated ALLOC_PEEK staging block (see gdx_rdram_staging_base below).
// gdx_rdram_staging_base: byte offset of the fixed GDX_RDRAM_STAGING_SIZE block
//   reserved for gdx_rdram_peek_raw, carved between the GfxPool and the arena.

unsigned char* gdx_rdram          = NULL;
/* mp-snapshot: the FRONT/BACK cursor is simulation state -- a race tick can allocate (the texture
   registry's lazily registered glyphs, object.c), and a resimulated tick must carve the same
   addresses the real one did -- so it is restored with the decomp image (GDX_SNAP_STATE_BSS).
   gdx_rdram_persist_top is NOT: its owners are port statics that a restore does not rewind. */
static size_t  gdx_rdram_bump GDX_SNAP_STATE_BSS;
size_t         gdx_rdram_arena_start = 0;
static size_t  gdx_rdram_persist_top = 0;
static size_t  gdx_rdram_staging_base = 0;
#ifdef GDX_MEMPROBE
/* Highest bump cursor ever reached. gdx_rdram_mode_reset rewinds gdx_rdram_bump at every mode
   transition, so the LIVE cursor systematically under-reports what the arena had to hold: the
   high-water is the number a smaller arena has to survive. */
static size_t  gdx_rdram_bump_hwm = 0;
static size_t  gdx_rdram_alloc_fail = 0;
#define GDX_RDRAM_NOTE_BUMP() do { if (gdx_rdram_bump > gdx_rdram_bump_hwm) { gdx_rdram_bump_hwm = gdx_rdram_bump; } } while (0)
#define GDX_RDRAM_NOTE_FAIL() do { gdx_rdram_alloc_fail++; } while (0)
#else
#define GDX_RDRAM_NOTE_BUMP() do { } while (0)
#define GDX_RDRAM_NOTE_FAIL() do { } while (0)
#endif
GfxPool*       gdx_gfxpool        = NULL;

static size_t Gdx_RomOffset(u32 addr) {
    u32 phys = addr & 0x1FFFFFFF;
    return (phys >= 0x10000000) ? (size_t)(phys - 0x10000000) : (size_t)phys;
}

// ---- RDRAM init + bump allocator -------------------------------------------

extern void gdx_register_host_range(void* ptr, size_t size); // defined in n64_gfx_bridge.cpp
extern void gdx_register_host_wide_command_range(void* ptr, size_t size);
// Same "known compiled-in host array" registration the EK disk assets use
// (see its comment in n64_gfx_bridge.cpp), reused here for base-game
// arrays. gdx_register_host_pointer_stub only affects RECOGNITION (silences the
// "[stub-miss] ... taken verbatim" census for a legitimate module-resident
// array); gdx_set_native_rgba16_texture_range additionally marks a range as
// host-endian so the SETTIMG wide-pointer path byte-swaps it into a persistent
// copy before Fast3D reads it (same mechanism transition.c uses for
// sTransitionPalette/backgroundBuffer).
extern void gdx_register_host_pointer_stub(void* dest, size_t size);
extern void gdx_set_native_rgba16_texture_range(void* ptr, size_t size, int enabled);
extern GfxPool D_1000000; // defined below; forward-declared here so gdx_rdram_init() can reference it
extern GfxPool D_8024DCE0[2];

void gdx_rdram_init(void) {
    gdx_rdram = (unsigned char*)gdx_host_calloc(1, GDX_RDRAM_SIZE);
    if (gdx_rdram == NULL) {
        gdx_dbg_logf("[rdram] FATAL: failed to allocate RDRAM buffer, requested bytes %d\n", (int)GDX_RDRAM_SIZE);
        gdx_host_exit(1);
    }

    // Staging block starts right after the GfxPool reservation, 16-byte aligned.
    // The arena then starts after the staging block, so ALLOC_FRONT/BACK commits
    // (gdx_rdram_alloc_raw) can never bump into the staging bytes that a live
    // ALLOC_PEEK (gdx_rdram_peek_raw) is using.
    gdx_rdram_staging_base = GDX_RDRAM_GFXPOOL_OFFSET +
                             ((sizeof(GfxPool) + 15u) & ~(size_t)15u);
    gdx_rdram_arena_start  = gdx_rdram_staging_base + GDX_RDRAM_STAGING_SIZE;
    gdx_rdram_bump         = gdx_rdram_arena_start;
    gdx_rdram_persist_top  = GDX_RDRAM_SIZE;

    // D_1000000 is a linker-symbol BSS global; point gdx_gfxpool at it.
    // The GfxPool stays as a host BSS allocation so all extern GfxPool D_1000000
    // declarations in the decomp source files link correctly without modification.
    gdx_gfxpool = &D_1000000;

    // Register the whole RDRAM buffer once — covers all future arena allocs.
    gdx_register_host_range(gdx_rdram, GDX_RDRAM_SIZE);
    gdx_register_host_range(&D_1000000, sizeof(D_1000000));
    gdx_register_host_range(D_8024DCE0, sizeof(D_8024DCE0));
    // The audio heap arena: every AudioHeap_Alloc* pool (including the aiBuffers the
    // audio HLE's A_LOADBUFF/A_SAVEBUFF ops address by truncated low32) carves from
    // this BSS block. It was never registered — on Windows the module-range
    // reconstruction happened to cover it, but on Linux PIE the module range does
    // not reach this BSS tail, every LOADBUFF/SAVEBUFF resolved NULL, the ops were
    // skipped, and the synthesized output stayed all-zero (the "no audio on Linux"
    // defect). Registration makes the resolution explicit on every platform.
    // Size MUST match the PORT declaration in decomp/src/audio/disk/lib/audio.h
    // ("extern u8 gAudioHeap[0x2ECA00 * 4]" — enlarged 4x on host builds).
    {
        extern unsigned char gAudioHeap[];
        gdx_register_host_range(gAudioHeap, (size_t)0x2ECA00 * 4);
    }
    // Venue material texture banks (road/wall/pipe/cylinder). course.c's
    // TRACK_SHAPE_* material tables (D_800CF528 / D_800CF608 / D_800CF668 / ...)
    // store native pointers to these 1-byte .bss placeholder symbols
    // (port/gen/LinkStubs.c). At draw time the gfx bridge re-routes each to its
    // decoded per-venue segment image via ResolveVenueBankAlias, but only after
    // the pointer is recognized as a known host range. On Windows the module-range
    // reconstruction happened to cover this .bss tail; on Linux PIE it does not
    // reach it (the same defect proven for gAudioHeap above), so the banks read as
    // the raw zero stub byte and the track floor/walls/pipes rendered solid black.
    // Register each bank explicitly so resolution is platform-independent. The
    // symbols are consecutive in .bss only by linker accident and LinkStubs.c
    // exposes no begin/end markers, so register each at its true 1-byte size rather
    // than assuming a contiguous span. This is exactly the set course.c references
    // and that ResolveVenueBankAlias enumerates (D_A000000..D_A008000).
    {
        extern unsigned char D_A000000[];
        extern unsigned char D_A001000[];
        extern unsigned char D_A002000[];
        extern unsigned char D_A003000[];
        extern unsigned char D_A004000[];
        extern unsigned char D_A005000[];
        extern unsigned char D_A006000[];
        extern unsigned char D_A007000[];
        extern unsigned char D_A008000[];
        gdx_register_host_range(D_A000000, 1);
        gdx_register_host_range(D_A001000, 1);
        gdx_register_host_range(D_A002000, 1);
        gdx_register_host_range(D_A003000, 1);
        gdx_register_host_range(D_A004000, 1);
        gdx_register_host_range(D_A005000, 1);
        gdx_register_host_range(D_A006000, 1);
        gdx_register_host_range(D_A007000, 1);
        gdx_register_host_range(D_A008000, 1);
    }
    // Banks 9-11 (decomp's fzx_segmentA.h:15-17), same registration as banks 0-8 above and
    // matching n64_gfx_bridge.cpp's kBankLow32[]. D_A009000/D_A00A000 are unreferenced
    // placeholders always linked via LinkStubs.c; D_A00B000 is live (gRoadTypeMenuItems,
    // overlays/expansion_kit/A3AE0.c:534-544) but exists only in the EK-only EkLinkStubs.c, so it
    // must stay behind EXPANSION_KIT like its bridge-side extern.
    {
        extern unsigned char D_A009000[];
        extern unsigned char D_A00A000[];
        gdx_register_host_range(D_A009000, 1);
        gdx_register_host_range(D_A00A000, 1);
    }
#ifdef EXPANSION_KIT
    {
        extern unsigned char D_A00B000[];
        gdx_register_host_range(D_A00B000, 1);
    }
    {
        extern unsigned char D_A00B240[];
        gdx_register_host_range(D_A00B240, 1);
    }
    {
        extern unsigned char D_A00B480[];
        gdx_register_host_range(D_A00B480, 1);
    }
    {
        extern unsigned char D_A00B6C0[];
        gdx_register_host_range(D_A00B6C0, 1);
    }
    {
        extern unsigned char D_A00B900[];
        gdx_register_host_range(D_A00B900, 1);
    }
    {
        extern unsigned char D_A00BB40[];
        gdx_register_host_range(D_A00BB40, 1);
    }
    {
        extern unsigned char D_A00BD80[];
        gdx_register_host_range(D_A00BD80, 1);
    }
#endif

    // EndingCutsceneEffects_DrawFireworks (decomp/src/overlays/ending/ending_effects.c) SETTIMGs
    // these three compiled-in u16[64] sparkle textures directly. They are real host arrays, not
    // LinkStubs placeholders, so their u16 literals sit in host little-endian order while Fast3D's
    // RGBA16 reader wants a big-endian byte stream — the same endianness class transition.c works
    // around for sTransitionPalette/backgroundBuffer. gdx_set_native_rgba16_texture_range is the
    // pixel fix (byte-swap into a persistent copy before sampling);
    // gdx_register_host_pointer_stub only silences the stub-miss census. 64 * sizeof(u16) each.
    {
        extern u16 D_i7_8014ADA8[];
        extern u16 D_i7_8014AE30[];
        extern u16 D_i7_8014AEB8[];
        gdx_set_native_rgba16_texture_range(D_i7_8014ADA8, 128u, 1);
        gdx_set_native_rgba16_texture_range(D_i7_8014AE30, 128u, 1);
        gdx_set_native_rgba16_texture_range(D_i7_8014AEB8, 128u, 1);
        gdx_register_host_pointer_stub(D_i7_8014ADA8, 128u);
        gdx_register_host_pointer_stub(D_i7_8014AE30, 128u);
        gdx_register_host_pointer_stub(D_i7_8014AEB8, 128u);
    }

    // minimap.c already pre-swaps this TLUT under #ifdef PORT (MINIMAP_TLUT_ENTRY), so its
    // compiled bytes are ALREADY big-endian-correct. Unlike the fireworks arrays above it must NOT
    // go through gdx_set_native_rgba16_texture_range — that would re-swap correct bytes back to
    // broken. Identity registration only, to silence the stub-miss census.
    {
        extern u16 sCourseMinimapPalette[];
        gdx_register_host_pointer_stub(sCourseMinimapPalette, 8u);
    }

    {
        extern void gdx_cki(const char*, int);
        extern void gdx_ckp(const char*, void*);
        gdx_ckp("[rdram] base", (void*)gdx_rdram);
        gdx_cki("[rdram] staging_base", (int)gdx_rdram_staging_base);
        gdx_cki("[rdram] staging_size", (int)GDX_RDRAM_STAGING_SIZE);
        gdx_cki("[rdram] arena_start", (int)gdx_rdram_arena_start);
        gdx_cki("[rdram] sizeof GfxPool", (int)sizeof(GfxPool));
    }

    gdx_cki("[rdram] buffer initialized, bytes", (int)GDX_RDRAM_SIZE);
}

// Ground truth for the GfxPool inter-pool stride. gdx_interp.cpp's hand-copied kGfxPoolSize
// cannot self-check at parity 0 (the check degenerates to a tautology there — see PrevPoolBase).
// This TU compiles WITH the real decomp GfxPool type, so it is the one place that can answer how
// big GfxPool actually is on the host.
size_t gdx_gfxpool_sizeof(void) {
    return sizeof(GfxPool);
}

/* Ground truth for the effects vertex buffer's position inside the pool, for the same reason as
   gdx_gfxpool_sizeof above: the struct-comment offset (0x2A308) is the N64 layout, and sizeof(Gfx)
   doubling under PORT shifts every member after gfxBuffer by 0x1A008 on the host. The bridge's
   effects-vertex interpolation must test "is this gSPVertex operand inside effectsVtxBuffer", and a
   hand-copied N64 constant would silently aim that test 0x1A008 bytes low — inside courseVtxBuffer,
   lerping static track geometry against a differently-ordered previous tick. offsetof from the real
   type cannot drift. */
size_t gdx_gfxpool_effects_vtx_offset(void) {
    return offsetof(GfxPool, effectsVtxBuffer);
}

size_t gdx_gfxpool_effects_vtx_bytes(void) {
    return sizeof(((GfxPool*) 0)->effectsVtxBuffer);
}

/* Ground truth for the three PER-RACER matrix arrays, all Mtx[30] indexed by racer->id:
     unk_20308 - the machine body model matrix, written every drawn tick
     unk_20A88 - the second per-racer matrix (reflection pass / settings screen)
     unk_21208 - the ATTACK HIGHLIGHT matrix, written ONLY while attackHighlightScale != 0
   The bridge maps a rerouted pool offset back to (field, racer id) with these, so a matrix that
   spawns mid-race (the highlight) can borrow its missing previous keyframe from the same racer's
   body matrix instead of snapping to t=1 out of step with the machine it belongs to.
   offsetof from the real type for the usual reason: the N64 struct-comment offsets are 0x1A008 low
   on the host because sizeof(Gfx) doubles under PORT. */
void gdx_gfxpool_racer_mtx_layout(size_t* outBody, size_t* outSecond, size_t* outHighlight,
                                  size_t* outStride, size_t* outCount) {
    if (outBody != NULL) {
        *outBody = offsetof(GfxPool, unk_20308);
    }
    if (outSecond != NULL) {
        *outSecond = offsetof(GfxPool, unk_20A88);
    }
    if (outHighlight != NULL) {
        *outHighlight = offsetof(GfxPool, unk_21208);
    }
    if (outStride != NULL) {
        *outStride = sizeof(((GfxPool*) 0)->unk_20308[0]);
    }
    if (outCount != NULL) {
        *outCount = sizeof(((GfxPool*) 0)->unk_20308) / sizeof(((GfxPool*) 0)->unk_20308[0]);
    }
}

void* gdx_rdram_alloc_raw(size_t size, size_t align) {
    size_t base = (gdx_rdram_bump + (align - 1u)) & ~(align - 1u);
    if (base + size > gdx_rdram_persist_top) {
        GDX_RDRAM_NOTE_FAIL();
        gdx_dbg_logf("[rdram] FATAL: arena exhausted, want %d align %d bump %d persistTop %d size %d\n",
                     (int)size, (int)align, (int)gdx_rdram_bump, (int)gdx_rdram_persist_top, (int)GDX_RDRAM_SIZE);
        gdx_host_abort();
    }
    gdx_rdram_bump = base + size;
    GDX_RDRAM_NOTE_BUMP();
    return gdx_rdram + base;
}

/* Persistent allocations bump DOWN from the top of RDRAM toward the mode
   arena. gdx_rdram_mode_reset never touches this region, so it survives
   every game-mode rewind. Used for data whose lifetime is the whole session
   (audio soundfont conversions: gAudioCtx.soundFontList keeps pointers into
   these across mode transitions). The two regions grow toward each other;
   exhaustion only when they actually meet. */
void* gdx_rdram_persist_alloc_raw(size_t size, size_t align) {
    size_t base;
    if (size > gdx_rdram_persist_top) {
        GDX_RDRAM_NOTE_FAIL();
        gdx_dbg_logf("[rdram] FATAL: arena exhausted (persist), want %d bump %d persistTop %d size %d\n",
                     (int)size, (int)gdx_rdram_bump, (int)gdx_rdram_persist_top, (int)GDX_RDRAM_SIZE);
        gdx_host_abort();
    }
    base = (gdx_rdram_persist_top - size) & ~(align - 1u);
    if (base < gdx_rdram_bump) {
        GDX_RDRAM_NOTE_FAIL();
        gdx_dbg_logf("[rdram] FATAL: arena exhausted (persist collided), want %d bump %d persistTop %d size %d\n",
                     (int)size, (int)gdx_rdram_bump, (int)gdx_rdram_persist_top, (int)GDX_RDRAM_SIZE);
        gdx_host_abort();
    }
    gdx_rdram_persist_top = base;
    return gdx_rdram + base;
}

/* ALLOC_PEEK on console returns the arena cursor WITHOUT advancing it — transient scratch that
   the next real allocation may overwrite.

   Peeks MUST NOT be served from gdx_rdram_bump. The texture loader (object.c cases 17/18/20/21)
   peeks a staging buffer, DMAs MIO0-compressed data into it, then calls mio0Decode, which
   cooperatively yields every 4096 output bytes (torch/lib/libmio0/mio0.c); during a yield another
   fiber's Arena_Allocate(ALLOC_FRONT/BACK) bumps the cursor forward from exactly where the peek
   sits and the next commit lands on the live compressed source mid-decode. The dedicated
   GDX_RDRAM_STAGING_SIZE block used instead sits BEFORE gdx_rdram_arena_start, where FRONT/BACK
   commits physically cannot reach it.

   INVARIANT: every peek is served from the SAME base offset in the staging block — no bump, no
   accumulation — which matches console PEEK semantics but is only correct if at most one peek is
   live at a time. Verified against every compiled ALLOC_PEEK call site (decomp/src/game/object.c
   and decomp/src/overlays/ovl_i10/1459A0.c; decomp/src/sys/segment.c is excluded from the PORT
   build): each texture load does at most one FRONT commit, then ONE peek that is either consumed
   synchronously before the next peek call or handed once to mio0Decode and not touched again.
   Re-check this census before adding a caller. */
void* gdx_rdram_peek_raw(size_t size, size_t align) {
    size_t base;

    if (size <= GDX_RDRAM_STAGING_SIZE) {
        base = (gdx_rdram_staging_base + (align - 1u)) & ~(align - 1u);
        return gdx_rdram + base;
    }

    // Oversized peek: falls back to the racy cursor path. No compiled caller reaches this (see
    // the census above), so log once per distinct size to make a regression or new caller visible.
    {
        extern void gdx_cki(const char*, int);
        static size_t gdx_rdram_peek_overflow_last = (size_t)-1;
        if (size != gdx_rdram_peek_overflow_last) {
            gdx_rdram_peek_overflow_last = size;
            gdx_ck("[rdram] WARN: peek exceeds staging block");
            gdx_cki("[rdram] WARN peek size", (int)size);
        }
    }

    base = (gdx_rdram_bump + (align - 1u)) & ~(align - 1u);
    if (base + size > gdx_rdram_persist_top) {
        GDX_RDRAM_NOTE_FAIL();
        gdx_dbg_logf("[rdram] FATAL: arena exhausted (peek), want %d bump %d persistTop %d size %d\n",
                     (int)size, (int)gdx_rdram_bump, (int)gdx_rdram_persist_top, (int)GDX_RDRAM_SIZE);
        gdx_host_abort();
    }
    return gdx_rdram + base;
}

#ifdef GDX_MEMPROBE
/* Highest NON-ZERO byte in [0, limit), +1. The buffer is calloc'd at init, so every non-zero
   byte is a byte something wrote -- through the allocators or not. Scans backwards a word at a
   time and stops at the first hit, so the cost is (limit - answer), i.e. the size of the
   untouched tail, which is exactly the number this probe exists to find. Reports at most a
   4-byte over-estimate (the word's last byte rather than the non-zero byte inside it), which is
   deliberate: an occupancy census must never round DOWN.

   Blind spot, stated: a write of 0x00 into virgin space is invisible to it. That is why slot 4
   (the allocator high-water) is reported beside it -- the pair disagreeing is the signal. */
static size_t gdx_rdram_scan_nonzero_below(size_t limit) {
    size_t i;
    if (gdx_rdram == NULL || limit == 0) {
        return 0;
    }
    if (limit > GDX_RDRAM_SIZE) {
        limit = GDX_RDRAM_SIZE;
    }
    i = limit & ~(size_t)3u;
    while (i >= 4u) {
        i -= 4u;
        if (*(const unsigned int*)(const void*)(gdx_rdram + i) != 0u) {
            return i + 4u;
        }
    }
    return 0;
}

/* Defined in n64_gfx_bridge.cpp under the same GDX_MEMPROBE guard; 0 when the bridge was not
   built with it. Declared here rather than in a header because it exists only for this probe. */
extern unsigned int gdx_gfx_probe_rdram_phys_hi(void);

void gdx_rdram_probe_stats(size_t* out) {
    if (out == NULL) {
        return;
    }
    out[0] = GDX_RDRAM_SIZE;
    out[1] = gdx_rdram_staging_base;
    out[2] = gdx_rdram_arena_start;
    out[3] = gdx_rdram_bump;
    out[4] = gdx_rdram_bump_hwm;
    out[5] = gdx_rdram_persist_top;
    out[6] = gdx_rdram_scan_nonzero_below(gdx_rdram_persist_top);
    out[7] = gdx_rdram_scan_nonzero_below(gdx_rdram_arena_start);
    out[8] = (size_t)gdx_gfx_probe_rdram_phys_hi();
    out[9] = gdx_rdram_alloc_fail;
}
#else
void gdx_rdram_probe_stats(size_t* out) {
    int i;
    if (out == NULL) {
        return;
    }
    for (i = 0; i < GDX_RDRAM_PROBE_SLOTS; i++) {
        out[i] = 0;
    }
    out[0] = GDX_RDRAM_SIZE;
}
#endif /* GDX_MEMPROBE */

/* Console Arena_StartInit resets the arena at every game-mode
   transition; the port shim was a no-op, leaking all mode-scoped allocations.
   The first StartInit call captures the post-boot cursor as the baseline
   (protecting boot-time persistent carve-outs made before any mode starts);
   later calls rewind to it. */
static size_t gdx_rdram_mode_baseline = 0;
static int gdx_rdram_baseline_set = 0;

#ifdef PORT
/* Base-game glyph/texture decode cache (object.c: D_800E33E0[] keyed by asset,
   value = decoded buffer pointer; count D_800E3A20). func_80077D44() invalidates
   it by zeroing the count only — cheap and safe to call on every rewind. */
extern void func_80077D44(void);
/* libultraship export (interpreter.cpp): drops every entry in the Fast3D texture
   cache so the next upload re-decodes from CPU memory. Same extern approach as
   port/gdx_workshop.cpp's hot-reload caller. */
extern void gfx_texture_cache_clear(void);
/* RENDER THREAD: n64_gfx_bridge.cpp defers the clear to the render thread when it owns the
   cache (a direct TextureCacheClear from the game thread raced core 2's LRU splice: the
   round-4 hardware data abort in TextureCacheLookup). */
extern void gdx_texcache_request_clear(void);
#endif

void gdx_rdram_mode_reset(void) {
    if (!gdx_rdram_baseline_set) {
        gdx_rdram_baseline_set = 1;
        gdx_rdram_mode_baseline = gdx_rdram_bump;
        return;
    }
    gdx_rdram_bump = gdx_rdram_mode_baseline;

#ifdef PORT
    /* The glyph/texture decode cache stores HOST pointers (gdx_rdram + offset) into this bump
       arena, so rewinding the bump re-issues those exact offsets to the next mode's decodes: any
       surviving cache entry then maps a glyph key to a DIFFERENT glyph's bytes, and a cache HIT
       serves it verbatim — crisp but wrong letters, consistent per screen and random across runs.

       The decomp only resets the cache on the func_80079EC8 transition path (object.c:1417); the
       reload path func_80079F1C (game.c:883) rewinds objects but NOT the cache. Since the arena is
       rewound ONLY through this function, invalidating here makes the two atomic whichever decomp
       path triggered the transition, so no cache entry can outlive the memory it points into. */
    func_80077D44();

    /* Covers the same class on the GPU side: Fast3D keys some texture formats by address alone, so
       a rewind that re-hands an address to different content keeps serving the previous upload.
       Mode transitions are infrequent, so a full clear costs nothing. Safe to call unguarded
       because this branch runs only after the baseline call above — i.e. on a real mode
       transition, by which point the renderer is live. A C TU cannot null-check the C++
       interpreter instance, so that call-site timing IS the guard. */
#if defined(GDX_PLATFORM_3DS)
    /* native-all S1 [native] atlas_race_reset: gGameMode is already the NEW mode here (game.c sets
       it before Arena_StartInit). Entering a race, the owned interpreter's clear below also empties
       the HUD texture atlas, so the race HUD packs onto fresh pages whatever the menus left there
       (nimenu-progress.md 4.1). Asked BEFORE the clear so a deferred (render-thread) clear sees it. */
    {
        extern void gdx_ninterp_atlas_reset_request(void) __attribute__((weak));
        switch (gGameMode) {
            case GAMEMODE_GP_RACE:
            case GAMEMODE_PRACTICE:
            case GAMEMODE_VS_2P:
            case GAMEMODE_VS_3P:
            case GAMEMODE_VS_4P:
            case GAMEMODE_TIME_ATTACK:
            case GAMEMODE_DEATH_RACE:
                if (&gdx_ninterp_atlas_reset_request != NULL) {
                    gdx_ninterp_atlas_reset_request();
                }
                break;
            default:
                break;
        }
    }
#endif
    gdx_texcache_request_clear();
#endif
}

// ---- Physical <-> Virtual address translation (PORT) -----------------------
// physicaltovirtual.c and virtualtophysical.c are excluded from the CMake build
// (they contain N64-platform implementations). Port implementations live here
// where both gdx_rdram and gdx_rom_buffer are visible.

#ifdef PORT
void* osPhysicalToVirtual(u32 addr) {
    u32 phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x10000000u) {
        // Cart ROM region: map into the host ROM buffer.
        if (gdx_rom_buffer != NULL) {
            return gdx_rom_buffer + (phys - 0x10000000u);
        }
        return gdx_rdram; // safe non-NULL fallback
    }
    // RDRAM region.
    return gdx_rdram + phys;
}

u32 osVirtualToPhysical(void* vaddr) {
    unsigned char* c = (unsigned char*)vaddr;
    // RDRAM-resident pointer: return offset from base.
    if (gdx_rdram != NULL && c >= gdx_rdram && c < gdx_rdram + GDX_RDRAM_SIZE) {
        return (u32)(c - gdx_rdram);
    }
    /* Host/BSS pointers cannot fit in libultra's u32 physical-address return.
       Return a low32 token, then Gdx_ResolvePortAddress reconstructs it via
       registered host ranges or the EXE module range before storing gSegments[]. */
    return (u32)(uintptr_t)vaddr;
}
#endif

u32 gdx_rom_read32(u32 addr) {
    /* Single chokepoint for ~27 ROM_READ sites (racer.c/machine_draw.c/
     * E7CF0.c). Read 4 bytes via the byte-source shim (archive-first, byte-identical
     * raw fallback) and assemble big-endian exactly as before. On a total miss the
     * shim returns 0 and leaves tmp untouched, so we return 0 -- the same value the
     * old NULL/OOB guard returned. */
    size_t off = Gdx_RomOffset(addr);
    u8 tmp[4];
    if (!GdxSegmentSourceRead((unsigned int)off, (unsigned int)sizeof(tmp), tmp)) {
        return 0;
    }

    return ((u32)tmp[0] << 24) |
           ((u32)tmp[1] << 16) |
           ((u32)tmp[2] << 8)  |
           ((u32)tmp[3] << 0);
}

u32 gdx_io_read(u32 addr) {
    (void)addr;
    return 0;
}

/* [native] machines: the game rebuilt one or more D_800CDDB0 machine model lists
 * (racer.c func_8008D3C4/func_8008D5F4/func_8008D97C via
 * port/3ds/patches/decomp-native-machines-rebuilt.patch). The 3DS mesh registry bumps
 * its bake generation; every other platform: no-op. */
void gdx_native_machine_lists_rebuilt(void) {
#if defined(GDX_PLATFORM_3DS)
    extern void gdx3ds_mesh_lists_rebuilt(void) __attribute__((weak));
    if (&gdx3ds_mesh_lists_rebuilt != NULL) {
        gdx3ds_mesh_lists_rebuilt();
    }
#endif
}

void gdx_io_write(u32 addr, u32 data) {
    (void)addr;
    (void)data;
}

// ---- Segment system (port reimplementation) --------------------------------
// N64 mapped 16 graphics segments to RDRAM; the game addresses assets as (segment<<24|offset).
// On host we store REAL pointers per segment (populated as assets load) and translate directly
// — no N64 KSEG0 (PHYS_TO_K0) mapping.
/* Pointer-width (uintptr_t), NOT unsigned long long: decomp TUs declare
 * `extern uintptr_t gSegments[]` (ead_demo_engine.c), and the element size must
 * agree on 32-bit hosts too. NOTE decomp/include/libc/stdint.h currently
 * typedefs uintptr_t as u64 for every non-LP64 PORT host; that header needs an
 * ILP32 branch before a 32-bit build is coherent (tracked in
 * docs/research/32bit-sweep.md).
 * mp-snapshot: simulation state (the segment table the race reads); GDX_SNAP_STATE_BSS puts it in
 * the snapshot's .bss block on 3DS (port/gdx_snapshot.h). */
uintptr_t gSegments[16] GDX_SNAP_STATE_BSS;

static void* Gdx_ResolvePortAddress(uintptr_t addr) {
    static int resolveLogs = 0;
    unsigned long long wideAddr = (unsigned long long)addr;
    unsigned int raw;
    unsigned int assetOffset = 0;
    void* assetBase;
    void* registered;
    void* moduleHost;

    if (addr == 0) {
        return NULL;
    }

    /* If a PORT call path already preserved a full host pointer, keep it.  The
       low32 reconstruction below is only for legacy u32 paths such as
       osVirtualToPhysical() and display-list command words.

       One exception: a wide pointer can still be an ASSET PLACEHOLDER. C code taking
       &aVpFullScreen hands us the real host address of a zero BSS stub whose bytes live in the
       decoded segment image, so returning it verbatim zeroes every
       camera->currentVp{Scale,Trans}{X,Y} and silently kills the position markers, the rival
       marker, the ending fireworks and the background stars. Mirrors
       ResolveWideAssetStubPointer on the graphics side; non-placeholder wide pointers fall
       through unchanged (see the bijection argument at that helper). */
    if (wideAddr > 0xFFFFFFFFULL) {
        /* Kill switch: GDX_WIDE_ASSET_RESOLVE=0 returns the pointer verbatim, so a texture
           regression can be attributed to this hook or ruled out at runtime without a rebuild. */
        extern void* gdx_resolve_wide_asset_pointer(const void* full);
        extern int gdx_wide_asset_resolve_enabled(void);
        void* wideAsset = gdx_wide_asset_resolve_enabled()
                              ? gdx_resolve_wide_asset_pointer((const void*)addr)
                              : NULL;
        if (wideAsset != NULL) {
            if (resolveLogs < 12) {
                resolveLogs++;
                gdx_addr_log("wide-asset", addr, wideAsset);
            }
            return wideAsset;
        }
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("full", addr, (void*)addr);
        }
        return (void*)addr;
    }

    raw = (unsigned int)addr;

    /* LinkStubs.c can only provide a one-byte marker for the original linker
       segment symbol. Segment 2, however, addresses the real host-compiled BSS
       object beginning at D_80225800 (not that marker). Bind the start token
       explicitly so segmented pointers such as 0x02000000 resolve to the
       matrix/context storage they reference. */
    if (raw == (unsigned int)(uintptr_t)SEGMENT_VRAM_START(unk_bss_segment)) {
        void* resolved = &D_80225800;
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("unk-bss", addr, resolved);
        }
        return resolved;
    }

    assetBase = gdx_ensure_asset_segment_for_symbol(raw, &assetOffset);
    if (assetBase != NULL) {
        void* resolved = (u8*)assetBase + assetOffset;
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("asset", addr, resolved);
        }
        return resolved;
    }

    registered = gdx_resolve_registered_host_address(raw);
    if (registered != NULL) {
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("registered", addr, registered);
        }
        return registered;
    }

    moduleHost = gdx_resolve_module_host_address(raw);
    if (moduleHost != NULL) {
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("module", addr, moduleHost);
        }
        return moduleHost;
    }

    if ((raw >= 0x80000000u) && (raw <= 0xBFFFFFFFu)) {
        unsigned int phys = raw & 0x1FFFFFFFu;
        if ((gdx_rdram != NULL) && (phys < GDX_RDRAM_SIZE)) {
            void* resolved = gdx_rdram + phys;
            if (resolveLogs < 12) {
                resolveLogs++;
                gdx_addr_log("kseg-rdram", addr, resolved);
            }
            return resolved;
        }
        return NULL;
    }

    if ((gdx_rdram != NULL) && (raw < GDX_RDRAM_SIZE)) {
        void* resolved = gdx_rdram + raw;
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("rdram", addr, resolved);
        }
        return resolved;
    }

    if ((((raw >> 24) & 0xF) < 16) && (gSegments[(raw >> 24) & 0xF] != 0)) {
        void* resolved = (void*)(gSegments[(raw >> 24) & 0xF] + (raw & 0x00FFFFFFu));
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("segmented", addr, resolved);
        }
        return resolved;
    }

    if (resolveLogs < 12) {
        resolveLogs++;
        gdx_addr_log("fallback-low32", addr, (void*)(uintptr_t)raw);
    }
    return (void*)(uintptr_t)raw;
}

void* gdx_segmented_to_host_pointer(uintptr_t segmentedAddr) {
    return Gdx_ResolvePortAddress(segmentedAddr);
}

uintptr_t Segment_SegmentedToVirtual(uintptr_t segmentedAddr) {
    return (uintptr_t)Gdx_ResolvePortAddress(segmentedAddr);
}

/* Carve-backed segments: 3 (setup_gfx), 4 (hud_gfx / create_machine_textures),
 * 5 (podium_gfx), 7 (machine_global_gfx / EK textures), 8 (course_track_gfx) and
 * 9 (machine_models / course_edit_textures) are ALWAYS based at an RDRAM carve on
 * the port, and every caller stores the carve's PHYSICAL OFFSET
 * (gSegmentXXXXXXVramStart = (carve - gdx_rdram), sys_gfx.c's PORT block).
 *
 * On 64-bit hosts Gdx_ResolvePortAddress turns that offset into gdx_rdram+offset
 * via its `raw < GDX_RDRAM_SIZE` branch: the module-identity reconstruction that
 * runs first cannot claim a small offset, because (moduleHigh32 | offset) never
 * lands inside the high-VA module range. On ILP32 (3DS) that protection inverts:
 * the process image spans ~0x00100000..0x009E1000, so
 * gdx_resolve_module_host_address claims any carve offset in that window
 * VERBATIM, gSegments[4/7/9] ended up holding raw offsets (0x0019FF70,
 * 0x001C9E10, 0x00212AC0 observed), and every draw through them read .text
 * bytes -- the [gdl-bad] race=1 storm: gSegments[7]+0x45098 = 0x0020EEA8
 * decodes as ARM code, LooksLikeDisplayList fails, and the race HUD, countdown
 * and machine-part DLs are all dropped.
 *
 * The VALUE cannot be disambiguated (a module data pointer and a carve offset
 * overlap numerically on 3DS), but the segment NUMBER carries exact provenance:
 * these segments' bases are rdram carves by construction on the port, never
 * module pointers. Convert deterministically here. Gated to 32-bit hosts so
 * 64-bit resolution stays bit-identical (there the fallthrough already computes
 * this same pointer). Segments 1/6 (module BSS pointers), 2 (stub token) and
 * 0x0A (bridge-owned) keep the generic path. */
static uintptr_t Gdx_CarveSegmentHostBase(s32 segment, uintptr_t addr) {
    if (sizeof(void*) != 4u) {
        return 0;
    }
    switch (segment) {
        case 3:
        case 4:
        case 5:
        case 7:
        case 8:
        case 9:
            if ((addr != 0) && (gdx_rdram != NULL) && (addr < (uintptr_t)GDX_RDRAM_SIZE)) {
                return (uintptr_t)(gdx_rdram + addr);
            }
            break;
        default:
            break;
    }
    return 0;
}

uintptr_t Segment_SetPhysicalAddress(s32 segment, uintptr_t addr) {
    /* Defensive bounds — gSegments has 16 slots. */
    if ((unsigned)segment >= 16u) {
        return addr;
    }
    uintptr_t carve = Gdx_CarveSegmentHostBase(segment, addr);
    if (carve != 0) {
        gSegments[segment] = carve;
        gdx_seg_log("SetPhys-carve", segment, addr, (void*)carve);
        return addr;
    }
    void* resolved = Gdx_ResolvePortAddress(addr);
    gSegments[segment] = (uintptr_t)resolved;
    gdx_seg_log("SetPhys", segment, addr, resolved);
    return addr;
}

uintptr_t Segment_SetAddress(s32 segment, uintptr_t addr) {
    void* resolved;
    /* Defensive bounds — gSegments has 16 slots. */
    if ((unsigned)segment >= 16u) {
        return addr;
    }
    {
        uintptr_t carve = Gdx_CarveSegmentHostBase(segment, addr);
        if (carve != 0) {
            gSegments[segment] = carve;
            gdx_seg_log("SetAddr-carve", segment, addr, (void*)carve);
            return addr;
        }
    }
    resolved = Gdx_ResolvePortAddress(addr);
    gSegments[segment] = (uintptr_t)resolved;
    gdx_seg_log("SetAddr", segment, addr, resolved);
    return addr;
}

uintptr_t Segment_GetAddress(s32 segment) {
    return (uintptr_t)gSegments[segment];
}

Gfx* Segment_SetTableAddresses(Gfx* gfx) {
    // Emit one gSPSegment per slot so the converted display list carries correct
    // segment bases into the LUS interpreter's mSegmentPointers[]. The adapter's
    // kOpMoveword handler ignores the truncated 32-bit w1 and reads gSegments[]
    // directly, so the full 64-bit host pointers survive the 8→16-byte conversion.
    for (int i = 0; i < 16; i++) {
        gSPSegment(gfx++, i, gSegments[i]);
    }
    return gfx;
}

void Segment_LoadAssets(void) {
    switch (GET_MODE(gGameMode)) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_RECORDS:
        case GAMEMODE_COURSE_EDIT:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_GP_END_CS:
        case GAMEMODE_DEATH_RACE:
            if (!gdx_load_venue_texture_segment(COURSE_CONTEXT()->courseData.venue)) {
                gdx_ck("[segment] venue texture segment load failed");
            }
            /* Race-diagnostics gate: set here (mode-aware) rather than inside
               the venue loader, which the course-select preview also calls. */
            {
                extern int gGdxRaceActive;
                gGdxRaceActive = 1;
            }
            break;
        default:
            break;
    }
}

/*
 * Replaces the tail of the original Segment_LoadOverlays() — Segment_SetupSegment4/7/9/10/5 plus
 * the Segment_LoadSegment* content DMAs — which lives in decomp/src/sys/segment.c, excluded from
 * the port build. Without it segments 4 and 7 stay unset and everything drawn through them
 * (hud_gfx countdown faces, start arc, race HUD; machine_global_gfx machine parts;
 * create_machine_textures) reads a garbage base. The buffers are carved in sys_gfx.c's PORT block;
 * this fills them from the ROM image and points the segment table at them. Loads are synchronous
 * because Dma_LoadAssets is a memcpy on the port, so the console's async split buys nothing.
 */
extern uintptr_t gSegment1B8550VramStart;
extern uintptr_t gSegment1E23F0VramStart;
extern uintptr_t gSegment22B0A0VramStart;
extern uintptr_t gSegment22B0A0VramEnd;
extern uintptr_t gGdxMachineModelsVramStart;
extern uintptr_t gGdxMachineModelsVramEnd;
extern uintptr_t gGdxCourseEditTexturesVramStart;
extern uintptr_t gGdxCourseEditTexturesVramEnd;
/* Segment 5 (podium_gfx) carve markers. Declared in sys_gfx.c but NEVER assigned
 * a real backing buffer in that file's PORT block (unlike seg 4/7/9), so gSegments[5]
 * stayed null on the port -- the GP-ending seg-5 DL drop (see gdx_activate_podium_segment5). */
extern uintptr_t gSegment2738A0VramStart;
extern uintptr_t gSegment2738A0VramEnd;

/* seg4 and seg7 are two FIXED host buffers reused across every mode transition; only their
   CONTENT rotates among a small fixed set of ROM assets. Reloading unconditionally cost 131ms per
   transition (vs 5-20ms for every other mode-change step) because Dma_LoadAssets yields every 32KB
   (decomp/src/sys/dma.c) and each yield round-trips through a full vsync-locked host frame — a few
   hundred KB is ~8 frames. The copied bytes are frequently IDENTICAL to what is resident (race ->
   race retry loads the same hud_gfx). Skipping the DMA when the requested variant already matches
   is behaviourally identical and collapses those transitions to the Segment_SetAddress-only path
   the `default:` case already took. */
typedef enum { GDX_SEG4_CONTENT_NONE, GDX_SEG4_CONTENT_HUD_GFX, GDX_SEG4_CONTENT_CREATE_MACHINE } GdxSeg4Content;
typedef enum { GDX_SEG7_CONTENT_NONE, GDX_SEG7_CONTENT_MACHINE_GLOBAL, GDX_SEG7_CONTENT_EK_TEXTURES } GdxSeg7Content;
typedef enum {
    GDX_SEG9_CONTENT_NONE,
    GDX_SEG9_CONTENT_MACHINE_MODELS,
    GDX_SEG9_CONTENT_COURSE_EDIT
} GdxSeg9Content;
static GdxSeg4Content sGdxSeg4Resident = GDX_SEG4_CONTENT_NONE;
static GdxSeg7Content sGdxSeg7Resident = GDX_SEG7_CONTENT_NONE;
static GdxSeg9Content sGdxSeg9Resident = GDX_SEG9_CONTENT_NONE;
static GdxSeg9Content sGdxSeg9Active = GDX_SEG9_CONTENT_NONE;
static size_t sGdxSeg9ActiveSize = 0;

/* Byte-order fixups must be applied to the CARVES too, not only to the bridge's separate heap
   images: the carves are what gSegments[4]/[7] serve at draw time. Skipping them leaves the
   pause-menu TLUT-setup DL as big-endian garbage (palette mode never enabled -> striped text) and
   breaks the countdown faces / arc screens. Texture regions are absent from the fixup tables, so
   working texture consumers are unaffected, and images with no fixup entries are no-ops. */
extern void gdx_fixup_asset_segment_image(unsigned char segment, unsigned int rom_base,
                                           unsigned char* data, unsigned int size);
extern void gdx_register_asset_segment_command_ranges(unsigned char segment, unsigned int rom_base,
                                                       unsigned char* data, unsigned int size);
/* Segment-reload TOCTOU epoch (defined in n64_gfx_bridge.cpp). The graphics
   thread reads gSegments[] and the seg-4/7/9 carve bytes with no lock while a
   mode transition rewrites them here. Bracketing the reload with begin()/end()
   makes the shared seqlock counter ODD for the duration, so a racing
   graphics-thread resolution detects the window and skips the affected texture
   for that one frame instead of consuming torn state (the Create Machine entry
   strlen crash). See the epoch comment block in n64_gfx_bridge.cpp. */
extern void gdx_segment_epoch_begin(void);
extern void gdx_segment_epoch_end(void);
#ifdef EXPANSION_KIT
extern unsigned char* gdx_disk_buffer;
extern unsigned int gdx_disk_size;
extern unsigned int gdx_ek_segment_image_size(unsigned char segment);
extern int gdx_ek_segment_image_fill(unsigned char segment, const unsigned char* disk,
                                     unsigned long long diskSize, unsigned char* dest,
                                     unsigned int capacity);
#endif

static void gdx_load_seg4_if_needed(GdxSeg4Content want, unsigned char* romStart, size_t size,
                                     const char* label) {
    if (sGdxSeg4Resident != want) {
        Dma_LoadAssets(romStart, osPhysicalToVirtual(gSegment1B8550VramStart), size);
        gdx_fixup_asset_segment_image(0x04u,
                                      (want == GDX_SEG4_CONTENT_HUD_GFX)
                                          ? (unsigned int) PORT_hud_gfx_ROM_START
                                          : (unsigned int) PORT_create_machine_textures_ROM_START,
                                      (unsigned char*) osPhysicalToVirtual(gSegment1B8550VramStart),
                                      (unsigned int) size);
        sGdxSeg4Resident = want;
        gdx_ck(label); // "[transition] seg4 reload: <variant>"
    } else {
        gdx_ck("[transition] seg4 reload skipped (already resident)");
    }
    Segment_SetAddress(4, gSegment1B8550VramStart);
}

static void gdx_load_seg7_if_needed(GdxSeg7Content want, unsigned char* romStart, size_t size,
                                     const char* label) {
    if (sGdxSeg7Resident != want) {
        Dma_LoadAssets(romStart, osPhysicalToVirtual(gSegment1E23F0VramStart), size);
        gdx_fixup_asset_segment_image(0x07u,
                                      (want == GDX_SEG7_CONTENT_MACHINE_GLOBAL)
                                          ? (unsigned int) PORT_machine_global_gfx_ROM_START
                                          : (unsigned int) PORT_expansion_kit_textures_beta_ROM_START,
                                      (unsigned char*) osPhysicalToVirtual(gSegment1E23F0VramStart),
                                      (unsigned int) size);
        sGdxSeg7Resident = want;
        gdx_ck(label); // "[transition] seg7 reload: <variant>"
    } else {
        gdx_ck("[transition] seg7 reload skipped (already resident)");
    }
    Segment_SetAddress(7, gSegment1E23F0VramStart);
}

/* Segment 9 is mode-owned on the original game: decoded cartridge
 * machine_models for Create Machine and the machine-settings/cutscene modes,
 * but disk-resident course_edit_textures for Course Edit. The console loader
 * that performed this switch is excluded from the port build, and treating all
 * 0x09xxxxxx tokens as globally interchangeable makes the two layouts collide.
 * Keep the ownership explicit and expose a narrow resolver hook so the graphics
 * bridge can prefer the active image before its global generated-asset ranges. */
/* Staging buffer for the compressed seg-9 MIO0 span, filled by the byte-source
 * shim. Sized to the full ROM span [ROM_START, ROM_END) so the entire MIO0
 * stream is present before decoding. Game-thread only (mode transitions are
 * sequential), so no additional guard is needed here. */
static unsigned char sGdxSeg9Stage[PORT_machine_models_ROM_END - PORT_machine_models_ROM_START];

static int gdx_activate_machine_models_segment9(void) {
    unsigned char* dest = (unsigned char*)osPhysicalToVirtual(gGdxMachineModelsVramStart);
    size_t capacity = (size_t)(gGdxMachineModelsVramEnd - gGdxMachineModelsVramStart);
    const size_t romStart = (size_t)PORT_machine_models_ROM_START;
    const unsigned int span = (unsigned int)(PORT_machine_models_ROM_END - PORT_machine_models_ROM_START);

    /* Stage the compressed span through the shim (archive-first, raw fallback) and probe the
     * staged bytes; a source miss trips the same invalid path as a bad header. */
    if (dest == NULL || capacity == 0 ||
        !GdxSegmentSourceRead((unsigned int)romStart, span, sGdxSeg9Stage) ||
        sGdxSeg9Stage[0] != 'M' || sGdxSeg9Stage[1] != 'I' ||
        sGdxSeg9Stage[2] != 'O' || sGdxSeg9Stage[3] != '0') {
        gdx_ck("[segment] segment 9 machine_models source/capacity invalid");
        return 0;
    }

    if (sGdxSeg9Resident != GDX_SEG9_CONTENT_MACHINE_MODELS) {
        /* The MIO0 header's decoded size (big-endian u32 at +4) must be checked against the carve
         * capacity: a corrupt ROM would otherwise decompress past it. The EK disk path below does
         * the equivalent required>capacity check. */
        {
            unsigned int decodedSize = ((unsigned int)sGdxSeg9Stage[4] << 24) |
                                       ((unsigned int)sGdxSeg9Stage[5] << 16) |
                                       ((unsigned int)sGdxSeg9Stage[6] << 8) |
                                       (unsigned int)sGdxSeg9Stage[7];
            if (decodedSize > capacity) {
                gdx_ck("[segment] segment 9 machine_models MIO0 decoded size exceeds carve capacity");
                return 0;
            }
        }
        mio0Decode(sGdxSeg9Stage, dest);
        gdx_fixup_asset_segment_image(0x09u, (unsigned int)PORT_machine_models_ROM_START,
                                      dest, (unsigned int)capacity);
        gdx_register_asset_segment_command_ranges(0x09u,
                                                   (unsigned int)PORT_machine_models_ROM_START,
                                                   dest, (unsigned int)capacity);
        sGdxSeg9Resident = GDX_SEG9_CONTENT_MACHINE_MODELS;
        gdx_ck("[transition] seg9 reload: machine_models");
    } else {
        gdx_ck("[transition] seg9 reload skipped (machine_models resident)");
    }

    gSegment22B0A0VramStart = gGdxMachineModelsVramStart;
    gSegment22B0A0VramEnd = gGdxMachineModelsVramEnd;
    Segment_SetAddress(9, gSegment22B0A0VramStart);
    sGdxSeg9Active = GDX_SEG9_CONTENT_MACHINE_MODELS;
    sGdxSeg9ActiveSize = capacity;
    /* Success line to match the gdx_ck on every failure branch above: a GP-ending cutscene that
       silently drops vehicle models is otherwise indistinguishable from one that never got here. */
    {
        extern void gdx_cki(const char*, int);
        gdx_cki("[segment] seg9 active=machine_models size", (int)sGdxSeg9ActiveSize);
        gdx_cki("[segment] seg9 gameMode", (int)GET_MODE(gGameMode));
    }
    return 1;
}

/* gdx_dbg_logf has no built-in opt-out (unlike gdx_ck/gdx_cki), so the [seg9diag] lines below
 * need this gate or they spam stderr on a normal run. Reads the shared developer-gate cache so
 * the Dev Tools log toggle and GDX_LOG cannot disagree. */
static int gdx_seg9diag_enabled(void) {
    return gdx_dev_gate_log_file();
}

#ifdef EXPANSION_KIT
/* Entry/state/result tracing for the seg-9 activation Course Edit depends on. Uses gdx_dbg_logf,
 * not gdx_ck/gdx_cki, so the lines are not silently dropped when GDX_TRACE is unset; gated on
 * gdx_seg9diag_enabled() so a normal run stays silent. Runs only on a mode transition, not per
 * frame. `fillOk` is a separate local purely so its result can be logged — the precheck must
 * still short-circuit BEFORE the fill is attempted. */
static int gdx_activate_course_edit_segment9(void) {
    unsigned char* dest = (unsigned char*)osPhysicalToVirtual(gGdxCourseEditTexturesVramStart);
    size_t capacity = (size_t)(gGdxCourseEditTexturesVramEnd - gGdxCourseEditTexturesVramStart);
    size_t required = (size_t)gdx_ek_segment_image_size(9u);
    int precheckFailed = (dest == NULL || capacity == 0 || required == 0 || required > capacity);
    int fillOk = 0;
    const int diagEnabled = gdx_seg9diag_enabled();

    if (diagEnabled) {
        gdx_dbg_logf("[seg9diag] activate_course_edit entry gameMode=%d diskBuffer=%p diskSize=%u dest=%p "
                     "capacity=%u required=%u\n",
                     (int)gGameMode, (void*)gdx_disk_buffer, (unsigned int)gdx_disk_size, (void*)dest,
                     (unsigned int)capacity, (unsigned int)required);
    }

    if (!precheckFailed) {
        fillOk = gdx_ek_segment_image_fill(9u, gdx_disk_buffer, (unsigned long long)gdx_disk_size, dest,
                                           (unsigned int)capacity);
    }
    if (diagEnabled) {
        gdx_dbg_logf("[seg9diag] activate_course_edit fill precheckFailed=%d fillOk=%d\n", precheckFailed, fillOk);
    }

    /* B11d: an ALWAYS-ON receipt, because this is the single point the whole track turns on.
     * Reaching GAMEMODE_COURSE_EDIT at all requires gLeoDriveConnectionState != 0, and
     * filling segment 9 requires the disk bytes -- which in disk_stream=2 arrive through
     * gdx_disk_image_read with `diskBuffer` NULL. The [seg9diag] lines above ride
     * gdx_dev_gate_log_file(), i.e. the GDX_LOG env var, which a 3DS guest has no way to set:
     * that is ekdeadlock's "every FATAL breadcrumb rode a dev gate nothing on 3DS sets" trap,
     * and a receipt nobody can read is not a receipt. One line per boot. */
    {
        extern void gdx_ek_note(const char* what, unsigned int a, unsigned int b, unsigned int c,
                                unsigned int d);
        static int sCourseEditFillLogs = 0;
        if (sCourseEditFillLogs < 2) {
            sCourseEditFillLogs++;
            gdx_ek_note("[courseedit] seg9 fill precheckFailed/fillOk/required/diskBuffer",
                        (unsigned int)precheckFailed, (unsigned int)fillOk,
                        (unsigned int)required, (unsigned int)(uintptr_t)gdx_disk_buffer);
        }
    }

    if (precheckFailed || !fillOk) {
        gdx_ck("[segment] segment 9 course_edit_textures fill failed");
        return 0;
    }

    if (sGdxSeg9Resident != GDX_SEG9_CONTENT_COURSE_EDIT) {
        gdx_ck("[transition] seg9 reload: course_edit_textures");
    } else {
        gdx_ck("[transition] seg9 reload: course_edit_textures refreshed");
    }
    sGdxSeg9Resident = GDX_SEG9_CONTENT_COURSE_EDIT;
    gSegment22B0A0VramStart = gGdxCourseEditTexturesVramStart;
    gSegment22B0A0VramEnd = gGdxCourseEditTexturesVramEnd;
    Segment_SetAddress(9, gSegment22B0A0VramStart);
    sGdxSeg9Active = GDX_SEG9_CONTENT_COURSE_EDIT;
    sGdxSeg9ActiveSize = required;
    if (diagEnabled) {
        gdx_dbg_logf("[seg9diag] activate_course_edit SUCCESS Segment_SetAddress(9, 0x%08X) size=%u\n",
                     (unsigned int)gSegment22B0A0VramStart, (unsigned int)sGdxSeg9ActiveSize);
    }
    return 1;
}
#endif

static void gdx_load_segment9_for_mode(void) {
    int loaded = 0;
    /* Shared per-boot budget for the entry/exit lines below, plus one guaranteed
     * dispatch per game mode so menu churn cannot starve the late transitions. */
    static int sSeg9DispatchLogs = 0;
    static unsigned int sSeg9ModesLogged = 0;
    const unsigned int modeBit = ((unsigned int)gGameMode < 32u) ? (1u << (unsigned int)gGameMode) : 0u;
    const int firstForMode = (modeBit != 0u) && ((sSeg9ModesLogged & modeBit) == 0u);
    const int diagThisCall = gdx_seg9diag_enabled() && ((sSeg9DispatchLogs < 40) || firstForMode);

    if (diagThisCall) {
        sSeg9ModesLogged |= modeBit;
        if (sSeg9DispatchLogs < 40) {
            sSeg9DispatchLogs++;
        }
        gdx_dbg_logf("[seg9diag] load_segment9_for_mode entry gameMode=%d\n", (int)gGameMode);
    }

    switch (gGameMode) {
        case GAMEMODE_CREATE_MACHINE:
        case GAMEMODE_GP_END_CS:
        case GAMEMODE_LX_MACHINE_SETTINGS:
        case GAMEMODE_LX_GP_RACE_NEXT_MACHINE_SETTINGS:
            loaded = gdx_activate_machine_models_segment9();
            break;
#ifdef EXPANSION_KIT
        case GAMEMODE_COURSE_EDIT:
            loaded = gdx_activate_course_edit_segment9();
            break;
#endif
        default:
            break;
    }

    if (!loaded) {
        /* Match the console default path: keep the last segment address/content
         * resident, but do not make it authoritative outside a mode that owns
         * segment 9. */
        sGdxSeg9Active = GDX_SEG9_CONTENT_NONE;
        sGdxSeg9ActiveSize = 0;
        Segment_SetAddress(9, gSegment22B0A0VramStart);
    }

    if (diagThisCall) {
        gdx_dbg_logf("[seg9diag] load_segment9_for_mode exit gameMode=%d loaded=%d activeContent=%d "
                     "Segment_SetAddress(9)=0x%08X\n",
                     (int)gGameMode, loaded, (int)sGdxSeg9Active, (unsigned int)gSegment22B0A0VramStart);
    }
}

/* [brfast] Opaque snapshot of the mode-owned segment-9 state gdx_resolve_mode_segment9 reads,
   so the bridge's resolve memo can detect a mode switch without knowing the fields. */
unsigned int gdx_mode_segment9_state(void) {
    return (unsigned int)sGdxSeg9Active ^ ((unsigned int)sGdxSeg9ActiveSize << 4) ^
           ((unsigned int)(uintptr_t)gSegment22B0A0VramStart * 2654435761u);
}

int gdx_resolve_mode_segment9(unsigned int raw, size_t requiredBytes, uintptr_t* outAddress) {
    uintptr_t hostBase;
    size_t offset;

    if (outAddress == NULL || sGdxSeg9Active == GDX_SEG9_CONTENT_NONE) {
        return 0;
    }

    hostBase = (uintptr_t)osPhysicalToVirtual(gSegment22B0A0VramStart);
    if ((raw >> 24) == 9u) {
        offset = (size_t)(raw & 0x00FFFFFFu);
    } else {
        /* Course Edit stores some pointers after C's 64-bit host address has
         * passed through an N64-sized command word.  Resolve that truncation
         * only inside the exact buffer owned by the active segment-9 mode;
         * never reconstruct arbitrary process pointers from their high bits. */
        offset = (size_t)(unsigned int)(raw - (unsigned int)hostBase);
    }

    if (offset > sGdxSeg9ActiveSize || requiredBytes > sGdxSeg9ActiveSize - offset) {
        return 0;
    }

    *outAddress = hostBase + offset;
    return 1;
}

/* While a mode owns segment 4/7/9, that live
 * carve is authoritative and the ROM-backed AssetBindings.c table rows for the
 * SAME segment are stale context that must not be treated as a fallback match
 * by the generated-stub lookup in n64_gfx_bridge.cpp (ResolveGeneratedAssetStub,
 * reached from TryResolveAddress). TryResolveAddress already gives
 * gdx_resolve_mode_segment9 first refusal over those rows for segment 9 (see
 * the comment there); this extends the same intent to segments 4/7 and gives
 * the bridge an explicit way to skip a ROM-table hit for any segment currently
 * owned by a mode carve, instead of returning wrong-source bytes. */
int gdx_mode_owns_segment(unsigned int seg) {
    switch (seg) {
        case 4u:
            return sGdxSeg4Resident != GDX_SEG4_CONTENT_NONE;
        case 7u:
            return sGdxSeg7Resident != GDX_SEG7_CONTENT_NONE;
        case 9u:
            return sGdxSeg9Active != GDX_SEG9_CONTENT_NONE;
        default:
            return 0;
    }
}

/* Race-HUD-corruption follow-up (2026-07-2x): gdx_mode_owns_segment's blanket
 * reject above starved every compiled-symbol reference into segments 4/7/9
 * for the FULL duration of any mode that owns them (every race, not just a
 * transition frame), because those segments' AssetBindings.c ROM rows never
 * got a live-carve fallback the way segment 0x0A already has. Generalizing
 * the 0x0A redirect to 4/7/9 needs one extra guard 0x0A does not: those three
 * segments each multiplex MULTIPLE distinct ROM content families onto the
 * same live carve (segment 4: hud_gfx / create_machine_textures / the
 * never-port-loaded course_edit_textures_beta; segment 7: machine_global_gfx
 * / expansion_kit_textures_beta; segment 9: machine_models / the disk-based,
 * table-less course_edit_textures), so a row's offset is only valid in the
 * live carve when the row's ROM family matches whichever content is
 * CURRENTLY resident. Returns true only when `rom_base` is the ROM base of
 * the family actually resident/active for `seg` right now; false rejects a
 * stale-family row (e.g. a machine_models seg-9 row while Course Edit's
 * course_edit_textures is active) back to the caller's normal reject path,
 * preserving the original seg-9 editor-scatter fix intact. */
int gdx_mode_segment_content_matches(unsigned int seg, unsigned int rom_base) {
    switch (seg) {
        case 4u:
            switch (sGdxSeg4Resident) {
                case GDX_SEG4_CONTENT_HUD_GFX:
                    return rom_base == (unsigned int) PORT_hud_gfx_ROM_START;
                case GDX_SEG4_CONTENT_CREATE_MACHINE:
                    return rom_base == (unsigned int) PORT_create_machine_textures_ROM_START;
                default:
                    return 0;
            }
        case 7u:
            switch (sGdxSeg7Resident) {
                case GDX_SEG7_CONTENT_MACHINE_GLOBAL:
                    return rom_base == (unsigned int) PORT_machine_global_gfx_ROM_START;
                case GDX_SEG7_CONTENT_EK_TEXTURES:
                    return rom_base == (unsigned int) PORT_expansion_kit_textures_beta_ROM_START;
                default:
                    return 0;
            }
        case 9u:
            switch (sGdxSeg9Active) {
                case GDX_SEG9_CONTENT_MACHINE_MODELS:
                    return rom_base == (unsigned int) PORT_machine_models_ROM_START;
                default:
                    /* Course Edit's course_edit_textures has no AssetBindings.c
                       ROM row family (it is filled from the EK disk image, not
                       decoded from a cartridge ROM span), so no rom_base value
                       can ever legitimately match it here. */
                    return 0;
            }
        default:
            return 0;
    }
}

/* Segment 5 is GP-ending-only ROM data: the podium body meshes (sPodiumDLs,
 * ending.c) and the ending-venue building detail display lists, all reached via
 * 0x05xxxxxx G_DL/segment tokens. The console loaders that owned it
 * (Segment_SetupSegment5 + Segment_LoadSegment5, decomp/src/sys/segment.c) are
 * excluded from the port build, and sys_gfx.c's PORT block carves buffers for
 * segments 4/7/9 but not 5 -- so gSegments[5] was left null and every G_DL into
 * segment 5 resolved to nothing (the ending "[gdl-bad] raw=05xxxxxx first=00000000
 * w0=DE000000" burst): untextured white venue buildings and no podium.
 *
 * podium_gfx is MIO0-compressed in the cartridge ROM (matches the console
 * `if (*(s32*)vram == 'MIO0') mio0Decode(...)` path). Mirror the seg-9
 * machine_models activation: validate the MIO0 header, decode into a persistent
 * RDRAM carve, apply the generated segment fixups + command-range registration
 * (podium_gfx display lists carry 0x05xxxxxx internal pointers), and point
 * gSegments[5] at the decoded image.
 *
 * Lifetime note: unlike the seg 4/7/9 carves (allocated at boot in sys_gfx.c,
 * below the mode-reset baseline, hence rewind-protected), this buffer is carved
 * lazily on the first ending -- AFTER the baseline is captured. gdx_rdram_alloc_raw
 * memory would be rewound by gdx_rdram_mode_reset on the next mode change, so use
 * gdx_rdram_persist_alloc_raw (bumps DOWN from the top of RDRAM, never rewound):
 * the decoded podium image and its in-place fixups survive every mode transition
 * for the whole session, and revisiting the ending only re-points gSegments[5]. */
extern void* gdx_rdram_persist_alloc_raw(size_t size, size_t align);

static unsigned char* sGdxSeg5PodiumBuf = NULL; /* host pointer into gdx_rdram (persist region) */
static size_t sGdxSeg5PodiumSize = 0;
static int sGdxSeg5Resident = 0;
/* Staging buffer for the compressed seg-5 MIO0 span, filled by the byte-source
 * shim (same pattern as seg-9). Sized to the full podium_gfx ROM span. */
static unsigned char sGdxSeg5Stage[PORT_podium_gfx_ROM_END - PORT_podium_gfx_ROM_START];

static int gdx_activate_podium_segment5(void) {
    const size_t romStart = (size_t)PORT_podium_gfx_ROM_START;
    const unsigned int span = (unsigned int)(PORT_podium_gfx_ROM_END - PORT_podium_gfx_ROM_START);
    unsigned int decodedSize;

    /* Stage the compressed span through the shim, then probe the staged bytes for
     * the MIO0 magic -- byte-identical to the old direct gdx_rom_buffer probe. */
    if (!GdxSegmentSourceRead((unsigned int)romStart, span, sGdxSeg5Stage) ||
        sGdxSeg5Stage[0] != 'M' || sGdxSeg5Stage[1] != 'I' ||
        sGdxSeg5Stage[2] != 'O' || sGdxSeg5Stage[3] != '0') {
        gdx_ck("[segment] segment 5 podium_gfx source invalid (not MIO0)");
        return 0;
    }

    /* Authoritative decoded size from the MIO0 header (big-endian u32 at +4), same
     * check the seg-9 machine_models path uses -- a corrupt ROM could otherwise
     * decompress past the carve. */
    decodedSize = ((unsigned int)sGdxSeg5Stage[4] << 24) |
                  ((unsigned int)sGdxSeg5Stage[5] << 16) |
                  ((unsigned int)sGdxSeg5Stage[6] << 8) | (unsigned int)sGdxSeg5Stage[7];
    if (decodedSize == 0u || decodedSize > 0x100000u) {
        gdx_ck("[segment] segment 5 podium_gfx decoded size implausible");
        return 0;
    }

    /* Carve once from the persist region; ROM data is immutable for the process
     * lifetime, so the buffer and its fixups are reused on every later ending. */
    if (sGdxSeg5PodiumBuf == NULL || sGdxSeg5PodiumSize < decodedSize) {
        void* buf = gdx_rdram_persist_alloc_raw((size_t)decodedSize, 16u);
        if (buf == NULL) {
            gdx_ck("[segment] segment 5 podium_gfx carve alloc failed");
            return 0;
        }
        sGdxSeg5PodiumBuf = (unsigned char*)buf;
        sGdxSeg5PodiumSize = (size_t)decodedSize;
        sGdxSeg5Resident = 0;
    }

    gSegment2738A0VramStart = (uintptr_t)(sGdxSeg5PodiumBuf - gdx_rdram);
    gSegment2738A0VramEnd = gSegment2738A0VramStart + sGdxSeg5PodiumSize;

    if (!sGdxSeg5Resident) {
        mio0Decode(sGdxSeg5Stage, sGdxSeg5PodiumBuf);
        /* Rewrite the podium display lists' embedded 0x05xxxxxx pointers in place
         * (idempotency matters: fixups are applied EXACTLY once, guarded by the
         * resident flag -- double-fixup would corrupt the command words). */
        gdx_fixup_asset_segment_image(0x05u, (unsigned int)PORT_podium_gfx_ROM_START, sGdxSeg5PodiumBuf,
                                      (unsigned int)sGdxSeg5PodiumSize);
        gdx_register_asset_segment_command_ranges(0x05u, (unsigned int)PORT_podium_gfx_ROM_START, sGdxSeg5PodiumBuf,
                                                   (unsigned int)sGdxSeg5PodiumSize);
        sGdxSeg5Resident = 1;
        gdx_ck("[transition] seg5 reload: podium_gfx");
    } else {
        gdx_ck("[transition] seg5 reload skipped (podium_gfx resident)");
    }

    Segment_SetAddress(5, gSegment2738A0VramStart);
    {
        extern void gdx_cki(const char*, int);
        gdx_cki("[segment] seg5 active=podium_gfx size", (int)sGdxSeg5PodiumSize);
    }
    return 1;
}

static void gdx_load_segment5_for_mode(void) {
    switch (GET_MODE(gGameMode)) {
        case GAMEMODE_GP_END_CS:
            gdx_activate_podium_segment5();
            break;
        default:
            /* Non-ending modes never own segment 5; leave gSegments[5]/the carve
             * untouched (matches the console Segment_SetupSegment5 default path). */
            break;
    }
}

static void gdx_load_mode_segments(void) {
    extern void gdx_cki(const char*, int);
    /* Frame-interpolation cut epoch: this is the
       single port-side chokepoint every mode/screen transition passes through (Segment_LoadOverlays
       calls it on each mode change). Bumping the cut epoch here snaps the first rendered tick of the
       new mode so nothing streaks across SELECT MACHINE<->race, Course Edit<->play, GRAND PRIX
       standings, race entry, or a Retry reload. Render-only; no-op unless interpolation is on. */
    extern void gdx_interp_mark_cut_src(const char* tag);
    gdx_interp_mark_cut_src("mode-change");
    size_t hudSize = (size_t)(PORT_hud_gfx_ROM_END - PORT_hud_gfx_ROM_START);
    size_t createMachineSize =
        (size_t)(PORT_create_machine_textures_ROM_END - PORT_create_machine_textures_ROM_START);
    size_t machineGlobalSize =
        (size_t)(PORT_machine_global_gfx_ROM_END - PORT_machine_global_gfx_ROM_START);
    size_t ekTexturesSize =
        (size_t)(PORT_expansion_kit_textures_beta_ROM_END - PORT_expansion_kit_textures_beta_ROM_START);
    size_t seg7EkSize = (ekTexturesSize <= machineGlobalSize) ? ekTexturesSize : machineGlobalSize;
    static int sModeSegLogs = 0;

    /* Open the segment-reload epoch window BEFORE any base swap or carve rewrite.
       Everything below -- Dma_LoadAssets/mio0Decode into the carves,
       gdx_fixup_asset_segment_image in-place rewrites, and the Segment_SetAddress
       gSegments[] base swaps for segments 4/7/9 -- mutates state the graphics
       thread reads unsynchronized. Non-nested: this is the single top-level
       mode-transition reload path (gdx_load_segment9_for_mode is called inside
       and never re-enters here). */
    gdx_segment_epoch_begin();

    switch (GET_MODE(gGameMode)) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_GP_END_CS:
        case GAMEMODE_DEATH_RACE:
            /* Races: hud_gfx on segment 4, machine_global_gfx on segment 7. */
            gdx_load_seg4_if_needed(GDX_SEG4_CONTENT_HUD_GFX, SEGMENT_ROM_START(hud_gfx), hudSize,
                                     "[transition] seg4 reload: hud_gfx");
            gdx_load_seg7_if_needed(GDX_SEG7_CONTENT_MACHINE_GLOBAL, SEGMENT_ROM_START(machine_global_gfx),
                                     machineGlobalSize, "[transition] seg7 reload: machine_global_gfx");
            break;

        case GAMEMODE_CREATE_MACHINE:
            /* Create Machine: its texture bank replaces hud_gfx on segment 4;
               segment 7 carries the EK texture set. */
            gdx_load_seg4_if_needed(GDX_SEG4_CONTENT_CREATE_MACHINE, SEGMENT_ROM_START(create_machine_textures),
                                     createMachineSize, "[transition] seg4 reload: create_machine_textures");
            gdx_load_seg7_if_needed(GDX_SEG7_CONTENT_EK_TEXTURES, SEGMENT_ROM_START(expansion_kit_textures_beta),
                                     seg7EkSize, "[transition] seg7 reload: expansion_kit_textures_beta");
            break;

        case GAMEMODE_COURSE_EDIT:
            /* Course Edit (EK): hud_gfx on segment 4, EK textures on 7. */
            gdx_load_seg4_if_needed(GDX_SEG4_CONTENT_HUD_GFX, SEGMENT_ROM_START(hud_gfx), hudSize,
                                     "[transition] seg4 reload: hud_gfx");
            gdx_load_seg7_if_needed(GDX_SEG7_CONTENT_EK_TEXTURES, SEGMENT_ROM_START(expansion_kit_textures_beta),
                                     seg7EkSize, "[transition] seg7 reload: expansion_kit_textures_beta");
            break;

        default:
            /* Console behavior for menus/records/machine-select: segment 4
               keeps pointing at the existing buffer (contents persist from
               the previous mode). */
            Segment_SetAddress(4, gSegment1B8550VramStart);
            break;
    }

    gdx_load_segment9_for_mode();
    /* Segment 5 (podium_gfx) is GP-ending-only; mutated inside the same epoch
       window as the seg 4/7/9 rewrites so a racing graphics-thread snapshot skips
       the frame instead of consuming a torn gSegments[5] base. */
    gdx_load_segment5_for_mode();

    /* Close the epoch window: publishes the settled segment state (even counter)
       so the next graphics-thread snapshot resolves normally. Paired 1:1 with the
       begin() above on every control-flow path (no early returns in between). */
    gdx_segment_epoch_end();

    if (sModeSegLogs < 12) {
        sModeSegLogs++;
        gdx_cki("[segment] mode segments loaded for gameMode", (int)GET_MODE(gGameMode));
    }
}

void Segment_LoadOverlays(void) {
#ifdef EXPANSION_KIT
    if (GET_MODE(gGameMode) == GAMEMODE_COURSE_EDIT) {
        const size_t workBufferSize = 2 * sizeof(unk_80128C94);
        size_t i;

        if (D_80128C90 == NULL) {
            D_80128C90 = (unk_80128C94*)Arena_Allocate(
                ALLOC_FRONT, workBufferSize);
        }
        D_80128C94 = D_80128C90;
        if (D_80128C90 == NULL) {
            gdx_dbg_logf("[segment] FATAL: Course Edit graphics allocation failed\n");
            gdx_host_abort();
        }
        for (i = 0; i < workBufferSize; i++) {
            ((u8*)D_80128C90)[i] = 0;
        }
        /* This scratch allocation is in RDRAM, but its two Gfx subarrays are
         * written by recompiled PORT code and therefore use 16-byte host Gfx
         * packets rather than the original 8-byte N64 layout. */
        gdx_register_host_wide_command_range(D_80128C90, workBufferSize);
    }
#endif
    gdx_load_mode_segments();
}

// ---- Save system -------------------------------------------------------------
// Save_LoadStaffGhostRecord and Save_SaveSettingsProfiles are defined in
// decomp/src/overlays/ovl_i2/save.c, which compiles on the port -- no shim here.
// Save_LoadStaffGhostRecord's #ifdef PORT branch pulls the raw archive-file bytes for the course's
// staff_ghost_records/* o2r entry and parses the Torch payload in place, because no libultraship
// factory is registered for that resource type (see the TODO in
// port/resource/ResourceFactories.cpp). It must NOT be stubbed out: func_i10_8012B580 seeds every
// standard-cup CPU pacing target from ghostInfo.raceTime, so a failing load degenerates CPU pacing
// well beyond the ghost itself.

// ---- Graphics pool ---------------------------------------------------------
// D_1000000: the N64 graphics pool (segment 0x01) — a real runtime buffer (NOT an o2r asset),
// so display-list/matrix allocations have somewhere to live.
// (aVp* viewports and D_80149A0 are real assets provided by the generated asset bindings.)
GfxPool D_1000000;

// ---- Camera pose snapshot + projection-view rebuild -------------------------
// The projection-view matrix (GfxPool::unk_20208[camera->id]) is the one pool matrix that must
// not be lerped element-wise. Camera_UpdateProjectionViewMtx's live EXPANSION_KIT branch
// (camera.c:1249-1276) widens fov from a term of the matrix that fov itself produced -- a
// feedback loop -- so the finished matrix is not an affine function of the camera state and a
// midpoint lerp of two finished matrices is not the matrix the loop would have settled on at
// that midpoint. These entry points let the bridge interpolate the INPUTS instead and re-run
// the exact build per sub-frame. See port/gdx_camera_pose.h for the contract.
//
// Everything here writes to caller-supplied or local storage only: no gCameras write, no
// GfxPool write. Interpolation stays render-only.

/* Camera_MatrixToMtx has NO header declaration -- camera.c:976 defines it above its single
   in-file call site (camera.c:1278) and no header in decomp/include exposes it. Declared here
   rather than in fzx_camera.h because decomp/ is off-limits to this subsystem. It is the sole
   correct MtxF -> Mtx conversion for this matrix: it carries the little-endian `j ^ 1` column
   swap libultraship's GfxSpMatrix needs (see the comment at camera.c:993-996), which the
   generic Matrix_ToMtx does not. Hand-rolling the conversion here would silently transpose
   16-bit halves. */
extern void Camera_MatrixToMtx(MtxF* mtxF, Mtx* mtx2);
/* Same file-local-extern convention the decomp itself uses for this global: it is defined in
   game.c:22 and every consumer re-declares it (camera.c:555, racer.c:562, course.c:3386...),
   because no header carries it. Matches gGameMode at the top of this file. */
extern s32 gNumPlayers;

int gdx_camera_pose_count(void) {
    /* The live count sNumCameras (camera.c:21) is file-scope in camera.c with no header
       declaration, so it is not reachable from here and camera.c is off-limits. Return the
       gCameras array bound instead (camera.c:17) -- the caller validates each slot anyway, and
       an idle slot simply never matches a matrix the bridge is asked to rebuild. */
    return GDX_CAMERA_POSE_MAX;
}

int gdx_camera_pose_read(int index, GdxCameraPose* out) {
    const Camera* camera;

    if (out == NULL) {
        return 0;
    }
    if ((unsigned)index >= (unsigned)GDX_CAMERA_POSE_MAX) {
        memset(out, 0, sizeof(*out));
        return 0;
    }

    camera = &gCameras[index];

    out->eyeX = camera->eye.x;
    out->eyeY = camera->eye.y;
    out->eyeZ = camera->eye.z;
    out->atX = camera->at.x;
    out->atY = camera->at.y;
    out->atZ = camera->at.z;
    /* basis.y is UP (fzx_math.h:65-68 labels the Mtx3F rows forward/up/side); it is what
       camera.c:1254 hands Matrix_SetLookAt. */
    out->upX = camera->basis.y.x;
    out->upY = camera->basis.y.y;
    out->upZ = camera->basis.y.z;
    out->fov = camera->fov;
    out->nearZ = camera->near;
    out->farZ = camera->far;
    out->fovScaleX = camera->fovScaleX;
    out->fovScaleY = camera->fovScaleY;
    out->frustrumCenterX = camera->frustrumCenterX;
    out->frustrumCenterY = camera->frustrumCenterY;
    /* Snapshotted, not read live at rebuild time: the rebuild runs later, on the host thread,
       and must gate the fov widening on the value this pose was captured under. */
    out->numPlayers = (int)gNumPlayers;
    /* camera->id, not `index`: the id is what selects the GfxPool slot at camera.c:1278, so a
       consumer matching a pool offset back to a pose must compare against the same field.
       They are equal in practice (the sole assignment is camera.c:2177) but the id is the one
       the game actually indexes with. */
    out->id = (int)camera->id;
    out->valid = 1;
    return 1;
}

int gdx_camera_build_projview(const GdxCameraPose* pose, void* outMtx64) {
    MtxF projectionMtx;
    MtxF viewMtx;
    MtxF projectionViewMtx;
    Mtx scratchMtx;
    u16 perspectiveScale;
    f32 lookAtX;
    f32 lookAtY;
    f32 lookAtZ;
    f32 normalX;
    f32 normalY;
    f32 normalZ;
    f32 trueUpX;
    f32 trueUpY;
    f32 trueUpZ;
    f32 magnitude;

    if ((pose == NULL) || (outMtx64 == NULL) || (pose->valid == 0)) {
        return 0;
    }

    /* Matrix_SetLookAt reports degenerate input only by NOT writing: each of its three early
       returns (math.c:1013/1027/1041) leaves the caller's MtxF partially filled and skips
       Matrix_ToMtx entirely. The game survives that because camera->viewMtx is persistent and
       still holds the previous tick's complete matrix; a rebuild from local scratch has no such
       fallback, so detect the same three conditions here -- same expressions, same unnormalized
       operands, same `<= 0.0f` test -- and refuse before anything is built. The caller then
       leaves the game's own matrix in place for that sub-frame. */
    lookAtX = pose->eyeX - pose->atX;
    lookAtY = pose->eyeY - pose->atY;
    lookAtZ = pose->eyeZ - pose->atZ;
    magnitude = SQ(lookAtX) + SQ(lookAtY) + SQ(lookAtZ);
    if (magnitude <= 0.0f) {
        return 0; /* eye == at */
    }

    normalX = (pose->upY * lookAtZ) - (pose->upZ * lookAtY);
    normalY = (pose->upZ * lookAtX) - (pose->upX * lookAtZ);
    normalZ = (pose->upX * lookAtY) - (pose->upY * lookAtX);
    magnitude = SQ(normalX) + SQ(normalY) + SQ(normalZ);
    if (magnitude <= 0.0f) {
        return 0; /* up parallel to (eye - at) */
    }

    trueUpX = (lookAtY * normalZ) - (lookAtZ * normalY);
    trueUpY = (lookAtZ * normalX) - (lookAtX * normalZ);
    trueUpZ = (lookAtX * normalY) - (lookAtY * normalX);
    magnitude = SQ(trueUpX) + SQ(trueUpY) + SQ(trueUpZ);
    if (magnitude <= 0.0f) {
        return 0; /* trueUp underflowed to zero */
    }

    /* Zero every scratch before the first Matrix_Set* call. Both builders write all 16 MtxF
       elements on their success paths, but only Matrix_ToMtx writes the Mtx, and the degenerate
       paths above skip it -- so the Mtx must start defined. Cheap, and it makes the failure mode
       of any future early return a zero matrix rather than stack garbage. */
    memset(&projectionMtx, 0, sizeof(projectionMtx));
    memset(&viewMtx, 0, sizeof(viewMtx));
    memset(&projectionViewMtx, 0, sizeof(projectionViewMtx));
    memset(&scratchMtx, 0, sizeof(scratchMtx));
    perspectiveScale = 0;

    /* NEVER pass NULL for mtx/mtxF/perspectiveScale. Matrix_SetLookAt and Matrix_SetFrustrum
       substitute the SHARED file-scope sDefaultMtx/sDefaultMtxF on NULL (math.c:1001-1006,
       math.c:1067-1072), which would make a render-only rebuild scribble on state the game
       thread also uses; perspectiveScale is dereferenced with no NULL guard at all
       (math.c:1104-1109). scratchMtx exists purely to absorb the Mtx* out-param -- the game
       stores that one in gfxPool->unk_20008/unk_20108, which this must not touch.
       perspectiveScale is likewise local and discarded: it is a pure function of near + far
       (math.c:1101-1110), so it carries no interpolation state, and writing the pose's copy
       back into camera->perspectiveScale would be a game-state write. */
    Matrix_SetFrustrum(&scratchMtx, &projectionMtx, pose->fov, pose->nearZ, pose->farZ, pose->fovScaleX,
                       pose->frustrumCenterX, pose->fovScaleY, pose->frustrumCenterY, &perspectiveScale);

    Matrix_SetLookAt(&scratchMtx, &viewMtx, pose->eyeX, pose->eyeY, pose->eyeZ, pose->atX, pose->atY, pose->atZ,
                     pose->upX, pose->upY, pose->upZ);

    Camera_CalculateProjectionViewMtx(&projectionViewMtx, &projectionMtx, &viewMtx);

    /* The EK fov feedback loop, camera.c:1256-1275, reproduced verbatim. This is why the inputs
       are interpolated and the build re-run instead of the finished matrix being lerped: fov
       depends on projectionViewMtx.m[3][1], which depends on fov. Re-running it per sub-frame
       lands on the loop's fixed point for that sub-frame's inputs; lerping two settled matrices
       does not. gNumPlayers comes from the pose, not the live global (see the read function). */
    if (pose->fovIsResolved) {
        /* Threshold already decided for this tick; rebuild the frustum at the interpolated
           resolved fov and skip the branch entirely. Nothing here may re-test the 30000 cutoff. */
        if (pose->resolvedFov != pose->fov) {
            Matrix_SetFrustrum(&scratchMtx, &projectionMtx, pose->resolvedFov, pose->nearZ, pose->farZ,
                               pose->fovScaleX, pose->frustrumCenterX, pose->fovScaleY, pose->frustrumCenterY,
                               &perspectiveScale);
            Camera_CalculateProjectionViewMtx(&projectionViewMtx, &projectionMtx, &viewMtx);
        }
    } else if (pose->numPlayers != 2) {
        f32 var_fv0;
        f32 fov;

        var_fv0 = ABS(projectionViewMtx.m[3][1]);

        if (var_fv0 > 30000.0f) {
            var_fv0 -= 30000.0f;
            var_fv0 /= SHT_MAX - 30000.0f;
            if (var_fv0 >= 1.0f) {
                var_fv0 = 1.0f;
            }
            fov = pose->fov + ((85.0f - pose->fov) * var_fv0);

            Matrix_SetFrustrum(&scratchMtx, &projectionMtx, fov, pose->nearZ, pose->farZ, pose->fovScaleX,
                               pose->frustrumCenterX, pose->fovScaleY, pose->frustrumCenterY, &perspectiveScale);
            Camera_CalculateProjectionViewMtx(&projectionViewMtx, &projectionMtx, &viewMtx);
        }
    }

    Camera_MatrixToMtx(&projectionViewMtx, (Mtx*) outMtx64);
    return 1;
}

/* Run the build once for a single tick's pose and report the fov the EK widening branch settled
   on, so the caller can interpolate that resolved value instead of re-testing the threshold at
   every sub-frame. Writes pose->resolvedFov / pose->fovIsResolved. Returns 1 on success. */
int gdx_camera_resolve_fov(GdxCameraPose* pose) {
    MtxF projectionMtx;
    MtxF viewMtx;
    MtxF projectionViewMtx;
    Mtx scratchMtx;
    u16 perspectiveScale;

    if (pose == NULL) {
        return 0;
    }
    pose->resolvedFov = pose->fov;
    pose->fovIsResolved = 0;

    memset(&projectionMtx, 0, sizeof(projectionMtx));
    memset(&viewMtx, 0, sizeof(viewMtx));
    memset(&projectionViewMtx, 0, sizeof(projectionViewMtx));
    memset(&scratchMtx, 0, sizeof(scratchMtx));
    perspectiveScale = 0;

    Matrix_SetFrustrum(&scratchMtx, &projectionMtx, pose->fov, pose->nearZ, pose->farZ, pose->fovScaleX,
                       pose->frustrumCenterX, pose->fovScaleY, pose->frustrumCenterY, &perspectiveScale);
    Matrix_SetLookAt(&scratchMtx, &viewMtx, pose->eyeX, pose->eyeY, pose->eyeZ, pose->atX, pose->atY, pose->atZ,
                     pose->upX, pose->upY, pose->upZ);
    Camera_CalculateProjectionViewMtx(&projectionViewMtx, &projectionMtx, &viewMtx);

    if (pose->numPlayers != 2) {
        f32 var_fv0 = ABS(projectionViewMtx.m[3][1]);

        if (var_fv0 > 30000.0f) {
            var_fv0 -= 30000.0f;
            var_fv0 /= SHT_MAX - 30000.0f;
            if (var_fv0 >= 1.0f) {
                var_fv0 = 1.0f;
            }
            pose->resolvedFov = pose->fov + ((85.0f - pose->fov) * var_fv0);
        }
    }
    pose->fovIsResolved = 1;
    return 1;
}

/* Ground truth for GfxPool::unk_20208, the Mtx[4] the finished projection-view matrix lands in
   (camera.c:1278, indexed by camera->id). Sibling of gdx_gfxpool_racer_mtx_layout above and
   offsetof-derived for the same reason: the N64 struct-comment offsets in sys.h are 0x1A008 low
   on the host because sizeof(Gfx) doubles under PORT, so a hand-copied constant would aim the
   bridge's "is this pool offset a camera matrix" test into unk_20108 (the view matrices) and
   rebuild the wrong slot. */
void gdx_gfxpool_camera_mtx_layout(size_t* outProjView, size_t* outStride, size_t* outCount) {
    if (outProjView != NULL) {
        *outProjView = offsetof(GfxPool, unk_20208);
    }
    if (outStride != NULL) {
        *outStride = sizeof(((GfxPool*) 0)->unk_20208[0]);
    }
    if (outCount != NULL) {
        *outCount = sizeof(((GfxPool*) 0)->unk_20208) / sizeof(((GfxPool*) 0)->unk_20208[0]);
    }
}

// ---- Side-attack model-basis discontinuity ----------------------------------
// racer.c:4555-4566 is the only place a racer's model basis changes DISCONTINUOUSLY:
//
//     if (racer->unk_27C != 0) {
//         if (racer->attackState == ATTACK_STATE_SIDE) {
//             racer->modelBasis.x = racer->trueBasis.x;              // hard assignment
//         } else { // ATTACK_STATE_SPIN
//             racer->modelBasis.x = trueBasis.x*COS(unk_27C) + trueBasis.z*SIN(unk_27C);
//         }
//
// The spin branch enters continuously -- unk_27C starts at 0, so COS=1/SIN=0 makes the first
// attack tick identical to the pre-attack basis -- and then ramps. The side branch jumps: one
// tick the basis is the cross-product visual basis (racer.c:4666-4671), the next it is the
// physics basis. racer.c:4621-4638 re-derives modelBasis.z and .y from .x, so the jump reaches
// the whole basis, and racer.c:5985 builds the body matrix from it. Interpolating a pool matrix
// across that jump renders the machine at an orientation it never occupied.
//
// Exposed as the raw predicate rather than a computed "did it transition" flag: the transition
// has an off-by-one on EXIT that only the caller can resolve. On the last attack tick the
// assignment above still runs (unk_27C is non-zero when racer.c:4555 is evaluated) and only THEN
// does racer.c:4569-4574 clear unk_27C and attackState -- so a snapshot taken after the racer
// update reads "inactive" on a tick whose matrix is still the attacked one. The real
// discontinuity is between that tick and the next. See the bridge's exit-pending handling.
int gdx_racer_side_attack_count(void) {
    return (int) (sizeof(gRacers) / sizeof(gRacers[0]));
}

int gdx_racer_side_attack_active(int index) {
    const Racer* racer;

    if ((unsigned) index >= (unsigned) (sizeof(gRacers) / sizeof(gRacers[0]))) {
        return 0;
    }
    racer = &gRacers[index];
    /* Exactly the condition guarding the hard assignment: the outer `unk_27C != 0` test AND the
       SIDE arm of the inner branch. Spin deliberately reads 0 -- it has no discontinuity. */
    return (racer->unk_27C != 0) && (racer->attackState == ATTACK_STATE_SIDE);
}

/* Discord Rich Presence game-state sample (port/gdx_discord.cpp). Lives here because this is the
   TU with the decomp headers; the presence builder stays free of decomp types. Called from the
   host loop's PerfTicks window, after gdx_dispatch, so every field is this frame's post-update
   value (gGameMode flips mid-dispatch — port/input_bridge.c documents the staleness contract). */
void gdx_discord_snapshot(GdxDiscordSnapshot* out) {
    extern s32 gGameMode;
    extern s16 gGameModeChangeState;
    extern s8 gTitleDemoState;
    extern s32 gCourseIndex;
    extern s32 gCupType;
    extern s32 gDifficulty;
    extern s32 gNumPlayers;
    extern s32 gTotalLapCount;
    extern s8 gGamePaused;

    out->mode = GET_MODE(gGameMode);
    out->modeChanging = (gGameModeChangeState != GAMEMODE_UPDATE);
    out->titleDemo = (gTitleDemoState != TITLE_DEMO_INACTIVE);
    out->courseIndex = gCourseIndex;
    out->cupType = gCupType;
    out->difficulty = gDifficulty;
    out->numPlayers = gNumPlayers;
    out->totalLaps = gTotalLapCount;
    out->paused = (gGamePaused != 0);
    out->playerLap = gRacers[0].lap;
    out->playerPosition = gRacers[0].position;
    out->playerFinished = (gRacers[0].stateFlags & RACER_STATE_FINISHED) != 0;
}

/* ---------------------------------------------------------------------------------------- */
/* BOTTOMHUD — race-map state sample for the 3DS bottom screen (port/gdx_racemap.h).         */
/*                                                                                          */
/* Lives here for the same reason gdx_discord_snapshot does: this is the TU compiled with    */
/* the decomp's headers, so the consumer (port/3ds/gdx3ds_racemap.c) never declares a game   */
/* symbol or a game type. Both functions below are reached ONLY when                         */
/* gdiffuser.ini [debug] bottom_map is on and the bottom-screen MAP tab is showing; with the */
/* lever off nothing here runs at all.                                                       */
/*                                                                                          */
/* Every number is lifted from decomp/src/overlays/ovl_i3/minimap.c so that the bottom       */
/* screen and the top screen cannot drift: the marker arithmetic is                          */
/* Minimap_DrawCourseMinimap's, the scale rule is Minimap_InitCourseMinimap's, and the       */
/* marker SET is the same one the top screen would draw (3 co-op players, the GP leader or   */
/* the time-attack ghost, and the player's own blinking marker).                              */

#include "gdx_racemap.h"

/* minimap.c's own two globals. Declared, not #included: minimap.h drags in gfx.h and the Gfx
   command macros, and this TU wants only the bytes. The sizes are MINIMAP_MAX_SIZE (0x1000)
   and 4 TLUT entries; both are asserted against the constants below. */
#ifndef EXPANSION_KIT
extern u8 sCourseMinimapTex[0x1000];
#else
extern u8* sCourseMinimapTex; /* EK: Arena_Allocate'd per race, NULL in the course editor */
#endif
extern u16 sCourseMinimapPalette[];

#define GDX_RACEMAP_DIM 64            /* MINIMAP_MAX_DIMENSION */
#define GDX_RACEMAP_WORLD 16000.0f    /* MINIMAP_WORLD_DIMENSION */

/* Minimap_DrawCourseMinimap's marker arithmetic, verbatim, minus the `+= left/top` that puts
   it on the N64 screen. Callers get minimap-source pixels; the 3DS side owns the placement. */
static void gdx_racemap_marker_cell(const Racer* racer, f32 scale, s32* outCol, s32* outRow) {
    const Vec3f* p = &racer->segmentPositionInfo.segmentPos;
    *outCol = Math_Round(((p->x * GDX_RACEMAP_DIM * scale) / GDX_RACEMAP_WORLD) + (GDX_RACEMAP_DIM * scale)) / 2;
    *outRow = Math_Round(((p->z * GDX_RACEMAP_DIM * scale) / GDX_RACEMAP_WORLD) + (GDX_RACEMAP_DIM * scale)) / 2;
}

static void gdx_racemap_push(GdxRaceMapSnapshot* out, const Racer* racer, f32 scale, int kind, int blink) {
    s32 col;
    s32 row;
    if (racer == NULL || out->markerCount >= GDX_RACEMAP_MAX_MARKERS) {
        return;
    }
    gdx_racemap_marker_cell(racer, scale, &col, &row);
    out->markers[out->markerCount].col = (int) col;
    out->markers[out->markerCount].row = (int) row;
    out->markers[out->markerCount].kind = kind;
    out->markers[out->markerCount].blink = blink;
    out->markerCount++;
}

/* The modes in which race.c has run Minimap_InitCourseMinimap and the 64x64 buffer therefore
   describes the course that is on screen. Outside them the buffer still holds the PREVIOUS
   race's track, which is why `valid` exists and why the drawing side must honour it. */
static int gdx_racemap_mode_is_race(int mode) {
    switch (mode) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_DEATH_RACE:
            return 1;
        default:
            return 0;
    }
}

/* MP phase 3 (mp-fullview): the racer the bottom screen describes. 0 (stock) unless
   [net] mp_fullview / [debug] fullview_player shows player k full screen on this console, then k
   (port/3ds/game/gdx3ds_fullview_game.c; weak so every other target links without it). */
#if defined(GDX_PLATFORM_3DS)
extern int gdx_fv_local_racer(void) __attribute__((weak));
#endif
static int gdx_bottom_local_racer(void) {
#if defined(GDX_PLATFORM_3DS)
    if (gdx_fv_local_racer != NULL) {
        const int k = gdx_fv_local_racer();
        if (k >= 0 && k < 4) {
            return k;
        }
    }
#endif
    return 0;
}

void gdx_racemap_snapshot(GdxRaceMapSnapshot* out) {
    extern s32 gNumPlayers;
    extern s32 gTotalRacers;
    extern s32 gTotalLapCount;
    extern u32 gGameFrameCount;
    extern Racer* gRacersByPosition[];
    extern GhostRacer* gFastestGhostRacer;
    extern CourseInfo* gCurrentCourseInfo;

    const int mode = GET_MODE(gGameMode);
    const int local = gdx_bottom_local_racer();
    int numPlayersIndex;
    int otherCount;
    f32 scale;
    int i;

#ifdef EXPANSION_KIT
    {
        static const void* sSeenTex = NULL;
        if ((const void*) sCourseMinimapTex != sSeenTex) {
            gdx_dbg_logf("[racemap] minimap tex %p -> %p (mode %d)\n", sSeenTex, (void*) sCourseMinimapTex, mode);
            sSeenTex = sCourseMinimapTex;
        }
    }
#endif
    memset(out, 0, sizeof(*out));
    out->mode = mode;
    out->numPlayers = gNumPlayers;
    out->scaleNum = 1;
    out->scaleDen = 1;
    if (!gdx_racemap_mode_is_race(mode) || gCurrentCourseInfo == NULL) {
        return;
    }
#ifdef EXPANSION_KIT
    if (sCourseMinimapTex == NULL) {
        return;
    }
#endif

    /* Minimap_InitCourseMinimap: scale is 1 for a single player and 0.75 otherwise, and it is
       baked into the RASTERISED TRACK as well as into the marker formula. Reporting it lets the
       drawing side label the picture; it must not apply a second scale of its own. */
    if (gNumPlayers == 1) {
        scale = 1.0f;
        out->scaleNum = 1;
        out->scaleDen = 1;
    } else {
        scale = 0.75f;
        out->scaleNum = 3;
        out->scaleDen = 4;
    }

    out->valid = 1;
    out->lap = gRacers[local].lap;
    out->totalLaps = gTotalLapCount;
    out->position = gRacers[local].position;
    out->totalRacers = gTotalRacers;
    out->blinkOn = ((gGameFrameCount % 16) < 8) ? 1 : 0;

    /* Marker set, in Minimap_DrawCourseMinimap's order. numPlayersIndex/playerIndex are that
       function's arguments for the player-1 viewport: gNumPlayers-1 and 0. */
    numPlayersIndex = gNumPlayers - 1;
    if (numPlayersIndex < 0) {
        numPlayersIndex = 0;
    }
    if ((mode == GAMEMODE_VS_2P) || (mode == GAMEMODE_VS_3P)) {
        otherCount = gTotalRacers;
    } else {
        otherCount = numPlayersIndex + 1;
    }
    /* The game's own `switch (i)` only assigns a fill colour for i in 0..3, so racers past the
       fourth reuse the previous colour. Clamped rather than reproduced: four is the honest
       number of distinct player markers the minimap has. */
    if (otherCount > 4) {
        otherCount = 4;
    }
    for (i = 0; i < otherCount; i++) {
        if (i == local) {
            continue; /* playerIndex — drawn last, with the blink */
        }
        gdx_racemap_push(out, &gRacers[i], scale, GDX_RACEMAP_MARKER_PLAYER1 + i, 0);
    }
    if (numPlayersIndex == 0) {
        if (mode == GAMEMODE_GP_RACE) {
            const Racer* leader = (gRacers[0].position == 1) ? gRacersByPosition[1] : gRacersByPosition[0];
            gdx_racemap_push(out, leader, scale, GDX_RACEMAP_MARKER_LEADER, 0);
        } else if (gFastestGhostRacer != NULL) {
            gdx_racemap_push(out, gFastestGhostRacer->racer, scale, GDX_RACEMAP_MARKER_GHOST, 0);
        }
    }
    gdx_racemap_push(out, &gRacers[local], scale, GDX_RACEMAP_MARKER_PLAYER1 + local, 1);
}

/* A pointer no host build maps: NULL or the first MB (NULL plus an offset). Hardware faults on a
   read there, the emulator reads zeros and hides it, so callers log "[nullread]" and skip. */
static int gdx_ptr_unmapped(const void* p) {
    return (uintptr_t) p < 0x00100000u;
}

int gdx_racemap_bitmap(const unsigned char** outTex, unsigned short* outPalette4) {
    int i;
#ifdef EXPANSION_KIT
    /* EK only: the buffer is Arena_Allocate'd per race. In the retail build it is a static
       array, and a NULL test on it is a compile-time constant (and a -Waddress warning). */
    if (sCourseMinimapTex == NULL) {
        return 0;
    }
    if (gdx_ptr_unmapped(sCourseMinimapTex)) {
        static int sLogged = 0;
        if (sLogged < 4) {
            sLogged++;
            gdx_dbg_logf("[nullread] racemap bitmap tex=%p mode=%d (#%d)\n", (void*) sCourseMinimapTex,
                         (int) GET_MODE(gGameMode), sLogged);
        }
        return 0;
    }
#endif
    if (outTex != NULL) {
        *outTex = (const unsigned char*) sCourseMinimapTex;
    }
    if (outPalette4 != NULL) {
        for (i = 0; i < 4; i++) {
            const u16 stored = sCourseMinimapPalette[i];
#ifdef PORT
            /* minimap.c's MINIMAP_TLUT_ENTRY stores each entry PRE-SWAPPED under PORT, because
               the CI decoder reads TLUT bytes big-endian. Undo it so the caller gets real
               RGBA5551 and never has to know. */
            outPalette4[i] = (unsigned short) ((u16) ((stored >> 8) | (stored << 8)));
#else
            outPalette4[i] = (unsigned short) stored;
#endif
        }
    }
    return 1;
}

/* nibottom (Phase 2): DASH dashboard data, beyond the map. See gdx_racemap.h's own comment on
   GdxRaceHudSnapshot for why this is a sibling of GdxRaceMapSnapshot/gdx_racemap_snapshot above,
   not a growth of it. `sPlayerLeadInterval` is hud.c's own non-static global (ovl_i3/hud.c:43),
   declared here rather than #included for the same reason minimap.c's globals are above: this TU
   wants only the bytes, not hud.c's drawing-side types. */
int gdx_title_demo_active(void) {
    extern s8 gTitleDemoState;
    return gTitleDemoState != TITLE_DEMO_INACTIVE;
}

/* card-4b (hw, New 3DS: "when i went to VS, the bottom screen just showed the g-diffuser logo and
   didnt show multiplayer menu until i went back to title screen"; the log's receipt on the MAIN
   MENU read demo=1): the audio thread arms the attract demo (external.c, gTitleDemoState =
   TITLE_DEMO_ACTIVE) from the title BGM's sample position while gGameMode still reads TITLE, so a
   START on the title in that window can carry an armed demo into the main menu, where nothing in
   game.c clears it (func_80068F04 only resets it while a mode change is pending or on the title /
   in a race). It then stays set for the whole main-menu visit: the bottom screen thinks the demo
   runs (no Home, the VS icon's MULTIPLAYER request dropped) and a race started from there treats
   A / START as "leave the demo". Called once per host frame between game frames: on a settled
   MAIN MENU a demo state is stale by construction (the demo only ever runs from the title, in a
   race mode), so it is cleared. Returns the state it cleared, 0 = nothing to do. Replays never
   arm the demo from audio (the det audio pin), so they never reach the write. */
int gdx_title_demo_clear_stale(void) {
    extern s8 gTitleDemoState;
    extern s32 gQueuedGameMode;
    const int was = gTitleDemoState;
    if (was == TITLE_DEMO_INACTIVE || gGameMode != GAMEMODE_FLX_MAIN_MENU || gQueuedGameMode != gGameMode) {
        return 0;
    }
    gTitleDemoState = TITLE_DEMO_INACTIVE;
    return was;
}

void gdx_racehud_snapshot(GdxRaceHudSnapshot* out) {
    extern s32 gNumPlayers;
    extern s32 gTotalRacers;
    extern s32 gTotalLapCount;
    extern Racer* gRacersByPosition[];
    extern s32 sPlayerLeadInterval[4];
    extern s16 gCpuRacersRetired; /* item 4 (hw bug n60-3d7): same field hud.c's own  */
                                   /* Hud_DrawRacePositionExtras reads for its "/N"    */
    extern s16 gRacersKOd; /* item 6 (hardware feedback round): same global hud.c's own */
                            /* Hud_DrawKOStars reads (decomp/src/game/racer.c:84/2280)   */
    extern s16 gPlayerLives[]; /* game/racer.c; the same stock hud.c's Hud_DrawPlayerLives draws */
    extern char* gCurrentTrackName; /* game/common.c; set by course_select.c/main_menu.c from  */
                                     /* sTrackNames[gCourseIndex] when the course is picked --  */
                                     /* COORDINATOR FIX (nibottom Phase 2, 3rd round, item 2):  */
                                     /* the DASH header used to bake a frozen "MUTE CITY I"     */
                                     /* string regardless of the actual course; read the real   */
                                     /* one here instead. */

    const int mode = GET_MODE(gGameMode);
    const int local = gdx_bottom_local_racer();
    const Racer* self = &gRacers[local];
    s32 bestLapMs;
    int i;
    int count;

    memset(out, 0, sizeof(*out));
    out->numPlayers = gNumPlayers;
    if (!gdx_racemap_mode_is_race(mode)) {
        return;
    }

    out->valid = 1;
    out->position = self->position;
    out->totalRacers = gTotalRacers;
    out->racersRemaining = gTotalRacers - gCpuRacersRetired;
    out->speedKph = (int) (self->speed * 21.6f + 0.5f); /* hud.c's own Hud_DrawPlayerSpeed formula */
    out->lap = self->lap;
    out->totalLaps = gTotalLapCount;
    out->character = (int) self->character;
    out->energyMilli = (self->maxEnergy > 0.0f)
        ? (int) ((1000.0f * self->energy / self->maxEnergy) + 0.5f)
        : 0;
    if (out->energyMilli < 0) {
        out->energyMilli = 0;
    } else if (out->energyMilli > 1000) {
        out->energyMilli = 1000;
    }
    out->boostReady = (self->stateFlags & RACER_STATE_CAN_BOOST) ? 1 : 0;
    out->koCount = (int) gRacersKOd;
    out->lapTimeMs = (int) (self->raceTime - self->completedLapsTime);

    bestLapMs = 0;
    for (i = 0; i < 3; i++) {
        if (self->lapTimes[i] > 0 && (bestLapMs == 0 || self->lapTimes[i] < bestLapMs)) {
            bestLapMs = self->lapTimes[i];
        }
    }
    out->bestTimeMs = (int) bestLapMs;

    out->gapMs = (int) sPlayerLeadInterval[local];

    /* Same gate as Hud_DrawPlayerLives: the raw gGameMode, not GET_MODE, so only a GP race. */
    out->spareMachines = -1;
    if (gGameMode == GAMEMODE_GP_RACE) {
        out->spareMachines = (gPlayerLives[0] > 0) ? (int) gPlayerLives[0] : 0;
    }

    count = gTotalRacers;
    if (count < 0) {
        count = 0;
    } else if (count > GDX_RACEHUD_TOP5_MAX) {
        count = GDX_RACEHUD_TOP5_MAX;
    }
    out->top5Count = count;
    for (i = 0; i < count; i++) {
        out->top5Character[i] = (gRacersByPosition[i] != NULL) ? (int) gRacersByPosition[i]->character : -1;
    }

    out->trackName[0] = '\0';
    if (gCurrentTrackName != NULL) {
        int j = 0;
        for (; j < (int) sizeof(out->trackName) - 1 && gCurrentTrackName[j] != '\0'; j++) {
            unsigned char c = (unsigned char) gCurrentTrackName[j];
            if (c >= 0x80u) {
                break; /* a FONT_SET_3-only glyph code (e.g. the Greek alpha/beta suffix on the  */
                       /* two SECTOR courses) -- stop cleanly rather than emit garbage */
            }
            if (c >= (unsigned char) 'a' && c <= (unsigned char) 'z') {
                c = (unsigned char) (c - (unsigned char) 'a' + (unsigned char) 'A');
            }
            out->trackName[j] = (char) c;
        }
        out->trackName[j] = '\0';
    }
}

/* The multiplayer race dash's whole-field view (gdx_racemap.h GdxRaceFieldSnapshot). */
static int gdx_racefield_state(const Racer* r) {
    if (r->stateFlags & RACER_STATE_FINISHED) {
        return GDX_RACEFIELD_FINISHED;
    }
    if (r->stateFlags & (RACER_STATE_RETIRED | RACER_STATE_CRASHED)) {
        return GDX_RACEFIELD_OUT;
    }
    return 0;
}

static int gdx_racefield_gap_ms(const Racer* leader, const Racer* r) {
    f32 msPerUnit;
    f32 gap;
    if (r == leader) {
        return 0;
    }
    if ((leader->stateFlags & RACER_STATE_FINISHED) && (r->stateFlags & RACER_STATE_FINISHED)) {
        return (int) (r->raceTime - leader->raceTime);
    }
    if (leader->raceDistance <= 0.0f || leader->raceTime <= 0) {
        return 0;
    }
    msPerUnit = (f32) leader->raceTime / leader->raceDistance;
    gap = (leader->raceDistance - r->raceDistance) * msPerUnit;
    return gap > 0.0f ? (int) (gap + 0.5f) : 0;
}

static void gdx_racefield_entry(GdxRaceFieldEntry* e, const Racer* leader, const Racer* r, int totalLaps, f32 scale,
                                int withMap) {
    s32 col = 0;
    s32 row = 0;
    int lap = r->lap;
    if (totalLaps > 0 && lap > totalLaps) {
        lap = totalLaps;
    }
    e->racer = (signed char) (r - gRacers);
    e->character = (signed char) r->character;
    e->lap = (signed char) lap;
    e->state = (unsigned char) gdx_racefield_state(r);
    e->gapMs = gdx_racefield_gap_ms(leader, r);
    e->timeMs = (int) r->raceTime;
    if (withMap) {
        gdx_racemap_marker_cell(r, scale, &col, &row);
    }
    e->col = (short) col;
    e->row = (short) row;
}

void gdx_racefield_snapshot(GdxRaceFieldSnapshot* out, int withMap) {
    extern s32 gNumPlayers;
    extern s32 gTotalRacers;
    extern s32 gTotalLapCount;
    extern Racer* gRacersByPosition[];
    const int mode = GET_MODE(gGameMode);
    const f32 scale = (gNumPlayers == 1) ? 1.0f : 0.75f;
    const Racer* leader;
    int count = gTotalRacers;
    int i;

    memset(out, 0, sizeof(*out));
    out->numPlayers = gNumPlayers;
    if (!gdx_racemap_mode_is_race(mode)) {
        return;
    }
    if (count < 0) {
        count = 0;
    } else if (count > GDX_RACEFIELD_MAX) {
        count = GDX_RACEFIELD_MAX;
    }
    leader = gRacersByPosition[0];
    if (leader == NULL) {
        return;
    }
    out->valid = 1;
    out->totalLaps = gTotalLapCount;
    for (i = 0; i < count; i++) {
        const Racer* r = gRacersByPosition[i];
        if (r == NULL) {
            break;
        }
        gdx_racefield_entry(&out->byPos[i], leader, r, gTotalLapCount, scale, withMap);
        out->count = i + 1;
    }
}

/* ── GSTATE: a game-state digest for visual-only levers (NEXT.md C4p) ─────────────────────
 *
 * The [fh] parity gate hashes PIXELS, so it cannot tell a lever that changes only what is
 * drawn (a rival's LOD tier, how far the track is drawn) from one that changes the race: both
 * diverge on every race frame. This digest is the gate for the first kind. It hashes what
 * the game SIMULATES and nothing it merely draws -- both RNG streams, the logic frame counter,
 * and per racer its state flags, position, velocity, speed, energy, race distance, lap, rank
 * and race time -- and deliberately leaves out machineLod / unk_2B2 / unk_2B3, the three
 * per-racer fields a draw-side LOD lever writes. Two runs of one replay whose digests agree
 * at every tick simulated the same race, whatever they drew. FNV-1a over the raw bits, so a
 * float that differs in the last ulp differs here. Main thread, after the game update. */
static u32 gdx_gstate_mix(u32 h, const void* data, size_t len) {
    const u8* p = (const u8*) data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static u32 gdx_gstate_mix_racer(u32 h, const Racer* racer) {
    h = gdx_gstate_mix(h, &racer->stateFlags, sizeof(racer->stateFlags));
    h = gdx_gstate_mix(h, &racer->segmentPositionInfo.pos, sizeof(racer->segmentPositionInfo.pos));
    h = gdx_gstate_mix(h, &racer->velocity, sizeof(racer->velocity));
    h = gdx_gstate_mix(h, &racer->speed, sizeof(racer->speed));
    h = gdx_gstate_mix(h, &racer->energy, sizeof(racer->energy));
    h = gdx_gstate_mix(h, &racer->raceDistance, sizeof(racer->raceDistance));
    h = gdx_gstate_mix(h, &racer->lap, sizeof(racer->lap));
    h = gdx_gstate_mix(h, &racer->position, sizeof(racer->position));
    h = gdx_gstate_mix(h, &racer->raceTime, sizeof(racer->raceTime));
    return h;
}

unsigned int gdx_gstate_hash(int* outRacers) {
    extern s32 gTotalRacers;
    extern u32 gGameFrameCount;
    extern s32 gRandSeed1;
    extern u32 gRandMask1;
    extern s32 gRandSeed2;
    extern u32 gRandMask2;
    u32 h = 2166136261u;
    s32 count = gTotalRacers;
    s32 i;

    if (count < 0) {
        count = 0;
    } else if (count > TOTAL_RACER_COUNT) {
        count = TOTAL_RACER_COUNT;
    }
    h = gdx_gstate_mix(h, &gRandSeed1, sizeof(gRandSeed1));
    h = gdx_gstate_mix(h, &gRandMask1, sizeof(gRandMask1));
    h = gdx_gstate_mix(h, &gRandSeed2, sizeof(gRandSeed2));
    h = gdx_gstate_mix(h, &gRandMask2, sizeof(gRandMask2));
    h = gdx_gstate_mix(h, &gGameFrameCount, sizeof(gGameFrameCount));
    h = gdx_gstate_mix(h, &count, sizeof(count));
    for (i = 0; i < count; i++) {
        h = gdx_gstate_mix_racer(h, &gRacers[i]);
    }
    if (outRacers != NULL) {
        *outRacers = (int) count;
    }
    return (unsigned int) h;
}

#ifdef __3DS__
/* MP30 (port/3ds/net/gdx3ds_mp.c, gdx3ds_mp_online.c): the visibility the simulation reads, per
 * racer, folded into a lockstep session's digest after gdx_gstate_hash (single-player [gstate]
 * is unchanged: this is only ever called in a session). Two masks of gTotalRacers bits:
 *   sim  = gdx_sim_machine_visible(machineLod), what the racer update and the CPU AI branch on
 *          (port/3ds/gdx3ds_autodrive.c; all ones in a session by construction);
 *   draw = machineLod != 0 as the last view's Racer_Draw left it. Every console runs every
 *          view's visibility pass with the same cameras (fullview and dl_hollow never drop it),
 *          so this agrees too; a lever that ever skipped another view's pass, or a view whose
 *          camera diverged, now fails loudly at the tick it happens instead of seconds later.
 * The LOD tier itself (1..6) is left out: rival_detail / lod6_skip / the o3ds preset change it
 * per console by design and nothing simulated reads it. */
unsigned int gdx_mpo_vis_hash(unsigned int h) {
    extern s32 gTotalRacers;
    extern int gdx_sim_machine_visible(int machineLod);
    u32 sim[1] = { 0 };
    u32 draw[1] = { 0 };
    s32 count = gTotalRacers;
    s32 i;

    if (count < 0) {
        count = 0;
    } else if (count > TOTAL_RACER_COUNT) {
        count = TOTAL_RACER_COUNT;
    }
    for (i = 0; i < count; i++) {
        if (gdx_sim_machine_visible(gRacers[i].machineLod)) {
            sim[0] |= 1u << i;
        }
        if (gRacers[i].machineLod != 0) {
            draw[0] |= 1u << i;
        }
    }
    h = gdx_gstate_mix(h, sim, sizeof(sim));
    h = gdx_gstate_mix(h, draw, sizeof(draw));
    return h;
}
#endif

/* ---- mp-online (port/3ds/net/gdx3ds_mp_game.c): race-state accessors --------------------------- */
/* The fields gdx_gstate_hash covers, readable (floats as raw bits): mp_hash_log's first ticks. */
static unsigned int gdx_mpo_bits(f32 f) {
    union {
        f32 f;
        u32 u;
    } v;
    v.f = f;
    return v.u;
}

/* The raw bytes of racer `index` minus the draw-written fields (desync triage: mp_hash_deep and
 * mp_hash_log's ring), 0 outside the table. out must be suitably aligned for a Racer. */
int gdx_mpo_racer_raw(int index, void* out, int cap) {
    extern s32 gTotalRacers;
    if (out == NULL || index < 0 || index >= gTotalRacers || index >= TOTAL_RACER_COUNT || cap < (int) sizeof(Racer)) {
        return 0;
    }
    memcpy(out, &gRacers[index], sizeof(Racer));
    {
        /* Zero what Racer_Draw writes (per view, and not at all on a skipped frame): two consoles
         * that simulate the same race still differ there. */
        Racer* r = (Racer*) out;
        memset(&r->modelMatrix, 0, sizeof(r->modelMatrix));
        r->modelMatrixInit = r->shadowMatrixInit = r->attackHighlightMatrixInit = 0;
        r->machineLod = 0;
        r->unk_2B2 = r->unk_2B3 = 0;
    }
    return (int) sizeof(Racer);
}

int gdx_mpo_racer_describe(int index, char* out, int cap) {
    extern s32 gTotalRacers;
    const Racer* r;
    if (out == NULL || cap <= 0 || index < 0 || index >= gTotalRacers || index >= TOTAL_RACER_COUNT) {
        return 0;
    }
    r = &gRacers[index];
    return snprintf(out, (size_t) cap,
                    "r%d st=%08x pos=%08x,%08x,%08x vel=%08x,%08x,%08x spd=%08x en=%08x dist=%08x lap=%d pos#=%d "
                    "t=%d ch=%d m=%d sk=%d",
                    index, (unsigned) r->stateFlags, gdx_mpo_bits(r->segmentPositionInfo.pos.x),
                    gdx_mpo_bits(r->segmentPositionInfo.pos.y), gdx_mpo_bits(r->segmentPositionInfo.pos.z),
                    gdx_mpo_bits(r->velocity.x), gdx_mpo_bits(r->velocity.y), gdx_mpo_bits(r->velocity.z),
                    gdx_mpo_bits(r->speed), gdx_mpo_bits(r->energy), gdx_mpo_bits(r->raceDistance), (int) r->lap,
                    (int) r->position, (int) r->raceTime, (int) r->character, (int) r->machineIndex,
                    (int) r->machineSkinIndex);
}

/* One racer's share of gdx_gstate_hash (0 outside the table): the [net] mp_hash_log lever logs
 * these per tick so a desync can be traced to the racer -- and the tick -- that diverged first. */
unsigned int gdx_mpo_racer_hash(int index) {
    extern s32 gTotalRacers;
    if (index < 0 || index >= gTotalRacers || index >= TOTAL_RACER_COUNT) {
        return 0;
    }
    return (unsigned int) gdx_gstate_mix_racer(2166136261u, &gRacers[index]);
}

/* The racer's stateFlags (RACER_STATE_*), 0 for an index outside the racer table. */
unsigned int gdx_mpo_racer_state(int index) {
    extern s32 gTotalRacers;
    if (index < 0 || index >= gTotalRacers || index >= TOTAL_RACER_COUNT) {
        return 0;
    }
    return (unsigned int) gRacers[index].stateFlags;
}

/* 1 when racer `index` is done racing: finished, retired or crashed out. */
int gdx_mpo_racer_done(int index) {
    const unsigned int f = gdx_mpo_racer_state(index);
    return (f & (RACER_STATE_FINISHED | RACER_STATE_RETIRED | RACER_STATE_CRASHED)) != 0;
}

/* mp-online RESULTS: a player's finishing position (1-based; 0 = did not finish) and race time.
 * The time is -1 for a racer the game finished without its laps: VS ends the race when one
 * machine is left (racer.c Racer_Update) and flags the ones still running FINISHED where they
 * are, so their raceTime is when the others went out, not a race time. */
int gdx_mpo_racer_result(int index, int* raceTimeMs) {
    extern s32 gTotalLapCount;
    const unsigned int f = gdx_mpo_racer_state(index);
    if (index < 0 || index >= TOTAL_RACER_COUNT) {
        return 0;
    }
    if (raceTimeMs != NULL) {
        const int ranLaps = gRacers[index].lap > gTotalLapCount;
        *raceTimeMs = ((f & RACER_STATE_FINISHED) && !ranLaps) ? -1 : (int) gRacers[index].raceTime;
    }
    return (f & RACER_STATE_FINISHED) ? gRacers[index].position : 0;
}

/* The per-player camera distance survives from the previous race on each console: pin it so
 * every console starts the session's race from the same camera state. */
/* The VS race init does NOT read gPlayerCharacters: the machine-select screen writes the player
 * racers' character / skin / engine factor straight into gRacers[] (machine.c), and Race_Init
 * keeps whatever is there -- the title demo's shuffle, a previous race's picks. Pin them from the
 * (already pinned) player globals, as func_8008969C + func_80089724 do on the menu path. */
void gdx_mpo_pin_racers(int numPlayers) {
    extern s16 gPlayerCharacters[4];
    extern s16 gPlayerMachineSkins[4];
    extern f32 gPlayerEngine[4];
    s32 i;
    /* Race_Init does not set every Racer field: some keep what the console's last race, title demo
     * or menu left (unk_352, the CPU's rival index, is only ever set by a 1P race's CPU init and is
     * read in VS too; the shadow colours; ...). So the table starts from the state a console has on
     * its first race after boot -- zeroed, then func_8008DA68's characters and engine factor -- the
     * same on every console whatever it did before. */
    memset(gRacers, 0, sizeof(gRacers));
    for (i = 0; i < TOTAL_RACER_COUNT; i++) {
        gRacers[i].id = i;
        gRacers[i].character = (s8) (i % TOTAL_RACER_COUNT);
        gRacers[i].machineSkinIndex = 0;
        gRacers[i].unk_1A8 = func_8008960C(0.5f);
        gRacers[i].unk_350 = gRacers[i].unk_352 = 0xFF;
    }
    for (i = 0; i < numPlayers && i < 4; i++) {
        gRacers[i].character = (s8) gPlayerCharacters[i];
        gRacers[i].machineSkinIndex = gPlayerMachineSkins[i];
        gRacers[i].unk_1A8 = func_8008960C(gPlayerEngine[i]);
    }
}

void gdx_mpo_pin_cameras(void) {
    s32 i;
    for (i = 0; i < 4; i++) {
        gCameras[i].raceSetting = CAMERA_RACE_SETTING_REGULAR;
    }
}

/* ---- mp-uds drop-out (port/3ds/net/gdx3ds_mp.c, [net] mp_drop=retire) ----------------------------- */
/* The machine of a player the session removed at lockstep tick T retires at the first race tick
 * >= T, through the game's own Racer_RetireRacer, so every console retires it at the SAME tick
 * (the session calls this from the poll, before that tick's game update, on every console).
 * Outside an on-track race nothing happens (the menus need no seat). Returns 1 on the tick it
 * actually retired the machine. Its pad is neutral from T either way (the coast). */
int gdx_mp_retire_player(int player) {
    extern s32 gTotalRacers;
    extern s32 gNumPlayers;
    extern void Racer_RetireRacer(Racer * racer);
    extern int gdx_input_in_gameplay(void);
    s32 i;
    if (player < 0 || player >= gNumPlayers || !gdx_input_in_gameplay()) {
        return 0;
    }
    for (i = 0; i < gTotalRacers && i < TOTAL_RACER_COUNT; i++) {
        Racer* racer = &gRacers[i];
        if (racer->id != player) {
            continue;
        }
        if ((racer->stateFlags & RACER_STATE_ACTIVE) &&
            !(racer->stateFlags & (RACER_STATE_FINISHED | RACER_STATE_RETIRED))) {
            Racer_RetireRacer(racer);
            return 1;
        }
        return 0;
    }
    return 0;
}
