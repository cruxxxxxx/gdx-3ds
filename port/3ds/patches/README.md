# port/3ds/patches — submodule working-tree patches

Submodule changes are never committed inside the submodules (plan rule); they live here
as patch files applied to the working tree, and land in the respective forks at
integration sign-off.

Apply after `git submodule update --init --recursive`:

```sh
git -C libultraship apply ../port/3ds/patches/lus-newlib-portability.patch
git -C libultraship apply ../port/3ds/patches/lus-resource-cache-cap.patch
git -C libultraship apply ../port/3ds/patches/lus-device-path-archives.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-settimg-low-address.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-hud-tall-atlas-extent.patch
git -C libultraship apply ../port/3ds/patches/lus-texcache-content-hash-span.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-hud-speedtex-hash-span.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-fog-exact-params.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-texcache-resource-stable-key.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-settimg-resolution-memo.patch
git -C libultraship apply ../port/3ds/patches/lus-s7-raw-instance-dispatch.patch
git -C libultraship apply ../port/3ds/patches/lus-s7-geo-diag-gate.patch
git -C libultraship apply ../port/3ds/patches/lus-s7-tri-state-memo.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-primenv-flush.patch
git -C libultraship apply ../port/3ds/patches/lus-prof-sections.patch
git -C libultraship apply ../port/3ds/patches/lus-flat-dispatch.patch
git -C libultraship apply ../port/3ds/patches/lus-vtx-mtx-hoist.patch
git -C libultraship apply ../port/3ds/patches/lus-profop.patch
git -C libultraship apply ../port/3ds/patches/lus-tmem-diag-race-latch.patch
git -C libultraship apply ../port/3ds/patches/lus-tmem-span-store.patch
git -C libultraship apply ../port/3ds/patches/lus-tmem-same-content-skip.patch
git -C libultraship apply ../port/3ds/patches/lus-texrect-run-memo.patch
git -C libultraship apply ../port/3ds/patches/lus-texrect-viewport-hoist.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-livery-ident.patch
git -C libultraship apply ../port/3ds/patches/lus-traffic-pipesync-noop.patch
git -C libultraship apply ../port/3ds/patches/lus-traffic-tri-memo-whitelist.patch
git -C libultraship apply ../port/3ds/patches/lus-traffic-vtx-clipmask.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-shade-alpha-ccmux.patch
git -C libultraship apply ../port/3ds/patches/lus-currentdir-reset-churn.patch
git -C libultraship apply ../port/3ds/patches/lus-cc-key-uninit-shader-id.patch
git -C libultraship apply ../port/3ds/patches/lus-crowd2-tilestate-value-gate.patch
git -C libultraship apply ../port/3ds/patches/lus-3ds-triloop-packed-vbo.patch
git -C libultraship apply ../port/3ds/patches/lus-tri2-phase-census.patch
git -C libultraship apply ../port/3ds/patches/lus-trifast-tri-memo-pack.patch
git -C libultraship apply ../port/3ds/patches/lus-tmem2-tmemfast.patch
git -C libultraship apply ../port/3ds/patches/lus-trect-census.patch
git -C libultraship apply ../port/3ds/patches/lus-trectbatch-atlas.patch
git -C decomp       apply ../port/3ds/patches/decomp-ilp32.patch
git -C decomp       apply ../port/3ds/patches/decomp-port-segment-bzero.patch
git -C decomp       apply ../port/3ds/patches/decomp-3ds-dma-low-address.patch
git -C decomp       apply ../port/3ds/patches/decomp-port-audio-specwait-yield.patch
git -C decomp       apply ../port/3ds/patches/decomp-race-cull-diagnostics.patch
git -C decomp       apply ../port/3ds/patches/decomp-3ds-sky-overscan-deadend-note.patch
git -C decomp       apply ../port/3ds/patches/decomp-3ds-machineselect-gradient-coalesce.patch
git -C decomp       apply ../port/3ds/patches/decomp-3ds-rom-audio-port.patch
git -C decomp       apply ../port/3ds/patches/decomp-port-course-select-state-reset.patch
git -C decomp       apply ../port/3ds/patches/decomp-port-rival-detail.patch
```

Note: `lus-prof-sections.patch` ([prof] sectioned CPU-build profiler — RUN/VTX/TRI/
IMP/MTX exclusive-time hooks in src/fast/interpreter.cpp, port counterpart in
port/3ds/gfx/gdx3ds_gpu_prof.[ch]) is generated against the FULL libultraship stack
above and applied LAST of the libultraship series before `lus-flat-dispatch.patch`.
Its section indices must match the `gdx3ds_gpu_prof.h` enum (BR=0 RUN=1 VTX=2 TRI=3
IMP=4 DRW=5 MTX=6).

Note: `lus-flat-dispatch.patch` is generated against the FULL libultraship stack
above (its context includes the prof-sections and s7 hunks in gfx_step's
neighbourhood), so it is applied after everything before it;
`lus-vtx-mtx-hoist.patch` is generated against the stack WITH flat-dispatch
applied and closes the libultraship series.

Note: `lus-profop.patch` ([profop] per-opcode handler timing at the flat-dispatch
call site — accumulator arrays defined in src/fast/interpreter.cpp, window emit in
port/3ds/gfx/gdx3ds_gpu_prof.c's ProfEmitWindow) is generated against the stack WITH
flat-dispatch + vtx-mtx-hoist applied (its gfx_step hunk wraps the flat `entry.fn`
call), so it now closes the libultraship series. This folds the previously
uncommitted dispatch-worktree working-tree edits that produced the measured
[profop] F3=22.34ms baseline.

## lus-tmem-diag-race-latch.patch (LOADBLOCK-OPT — per-call getenv in the load/import path)

The [tmem] store/lookup diagnostics deliberately used a LIVE `std::getenv("GDX_DIAG_SETTIMG")`
per call (the bridge only _putenv-exports the flag at the first real GFX task, after boot-time
imports), but both consumers are ALSO race-gated on `gGdxRaceActive` — so every TMEM store
(~82/frame in menus) and every ImportTexture (~100-200/frame) paid an environ scan with the
gate off. The patch tests the race flag (a plain int) first and latches the env probe in a
function-local static on the first race-active call; race activation always postdates the
bridge's _putenv, so the latch sees the final value. Behaviour with the gate on is unchanged.
Applied after `lus-profop.patch`.

## lus-tmem-span-store.patch (LOADBLOCK-OPT — per-word StoreLoadedTexture materialization)

The #1 CPU cost on the device: [profop] F3(G_LOADBLOCK) = 22.34 ms/frame over 82 calls
(~272 µs/load) in steady menus. `StoreLoadedTexture` materialized a full `LoadedTexture` at
EVERY covered TMEM word — a 4 KiB load was 512 ~48-byte struct copies, each carrying a
`shared_ptr<Fast::Texture>` refcount inc/dec pair (atomics, disproportionately expensive
under Azahar's JIT), preceded by a 512-entry overlap-invalidation walk of struct clears.

Now a load records ONE span: the word-0 view is stored eagerly at the span's base slot
(it doubles as the span record — its payload fields equal the original argument), plus
per-word bookkeeping bytes (`tmem_span_base` u16 map, `tmem_slot_stale` u8, one
`tmem_span_words` u16 at the base). Interior per-word views materialize lazily on first
read in `RDP::MaterializeTmemSlot` with the VERBATIM old loop math (host/logical byte
offset scaling, size reduction, the word!=0 line-size zeroing), so every consumer sees
byte-identical data through the new `RDP::LoadedTextureAt()` accessor (all ~88 read sites
converted, including the [tmem]/[fontmach]/effect-tile probes and the desktop
GfxDebuggerWindow). Operation count per 4 KiB load: 512 struct copies + 512-entry struct
walk + ~1024 atomic RMWs  ->  1 struct copy + ~512 u16/u8 bookkeeping writes + 0 atomics
beyond the single span-record copy.

Semantics preservation, argued invariantly:
- Overlap invalidation: the old walk cleared every per-word entry whose own recorded
  [tmem_start, +tmem_word_count) range overlapped the new load; all entries of one span
  share that range, so the set == "all words of every span with >=1 word in the new range",
  which is what the span-map walk removes (O(touched words), not 512). The one divergent
  entry class — an entry whose recorded word count is 0, which the old walk deliberately
  skipped — is still skipped via a per-word re-check of fresh slots' own records.
- In-place mutators: `GfxDpImageRectangle` and the port's VI present fallback write slot
  fields directly; they now go through `RDP::MutableLoadedTextureAt()`, which materializes
  every word of the containing span first, so no lazy derivation can later run against a
  mutated span record — equivalent to the old always-materialized array. (Known bounded
  divergence: after the VI fallback detaches word 0 (word_count=0), a later load
  overlapping the span's OTHER words clears word 0's map linkage where the old walk kept
  the detached entry; the fallback rebuilds its slot on every present, so unobservable.)
- Resource retention: any slot holding a `resource` shared_ptr is fresh for the live span
  covering it (only the eager base store and materialization write refs, and span removal
  clears fresh slots), so span removal releases exactly the refs the old eager clears
  released — the 24 MiB resource-cache evictor's use_count() pin test sees the same world.
- `tmem_start`/`tmem_word_count` are consumed ONLY by this invalidation logic (audited:
  no other reader in libultraship or the port), so their bookkeeping role is internal.

Portable C++ (no `__3DS__` gate — same win on desktop). The port-side counterpart (the
`MutableLoadedTextureAt` call in n64_gfx_bridge.cpp's VI fallback) is committed directly.
Applied after `lus-tmem-diag-race-latch.patch`.

## lus-3ds-livery-ident.patch (SHIP-LIVERY-2 — per-draw texture identity)

Two receipt hooks for the port's [livery] one-shot window (gfx_citro3d.cpp, INI
`[debug] diag_livery=1`): StoreLoadedTexture notes every TMEM store with the source's
resource path; ImportTexture notes every resolution with the texture id the cache
lookup BOUND for the consuming draw (hit or miss) plus the resource identity. The port
maintains an id→name table from the import notes, so its per-draw [livery] log can
name the texture every in-race draw actually samples — the instrument that hunts the
player-machine wrong-yellow-decals bug across ALL TMEM slots (the [interleave] window
only ever sampled 0x0/0x10/0x50). Both hooks early-return port-side while the window
is disarmed. Generated against the FULL stack above; applied last.

## lus-3ds-shade-alpha-ccmux.patch (CCMUX-11 — "Unsupported ccmux: 11" on ship explosion)

`GenerateCC`'s colour-mux switch had no case for `G_CCMUX_SHADE_ALPHA` (11 — only
reachable through the 5-bit C slot; A/B are masked to `G_CCMUX_0` at >= 8 and D at 7),
so the slot silently packed `SHADER_0` and spammed stderr twice per shader build.
F-Zero X hits it from the machine-debris display list `D_4007FD8` (racer.c draws it
right before the `gMachineDebris` loop of a 0-HP explosion): combine
`fc65fecb 44fe7d3e` = rgb `(1 - SHADE) * SHADE_ALPHA + SHADE`, alpha `1`, both
cycles — per-vertex machine-colour debris lerped toward white by the sine-flickered
per-vertex alpha (effects.c debris update). With the mux dropped to 0 the lerp
collapsed to plain SHADE: flat debris, no white-hot flicker.

The patch maps `G_CCMUX_SHADE_ALPHA` exactly like the other constant-ish muxes
(`ENV_ALPHA`/`PRIMITIVE_ALPHA` pattern): a generic shader input, with the per-vertex
fill broadcasting `v_arr[i]->color.a` into the input's rgb — identical to upstream
Fast3D/SoH's handling, exact on desktop backends. On the 3DS TexEnv side no new
mapping is needed (inputs are already generic); the debris combiner maps as a
constant-spill REPLACE stage + INTERPOLATE(1, PRIMARY_COLOR, PREVIOUS). Accepted
3DS-only approximation: SHADE rgb wins the single per-vertex colour attribute, so
the SHADE_ALPHA input rides the stage constant refreshed from vertex 0 of each
flush — the debris cloud still pulses white, but in unison rather than per-triangle.
Port counterpart committed directly in gfx_citro3d.cpp: `Cycle1ReadsCombined`
dead-cycle-0 skip in `MapCombiner`, without which the 2-cycle form of this combiner
(cycle 0 zeroed by GenerateCC, formula in cycle 1) would refuse the constant spill
("would clobber the cycle-1 result") and fall back to the unmapped-census path.
Generated against the FULL stack above; applied last.

## lus-currentdir-reset-churn.patch (LEAK — per-task deque<string> alloc/free churn)

The first real-hardware OOM ([fatal] operator new failed size=504, ra symbolized to
std::deque<std::string>::_M_create_nodes) died allocating a fresh backing deque for
`currentDir` — a write-only `std::stack<std::string>` (no reader anywhere in this fork;
torch never emits OTR_G_PUSHCD, so on the 3DS it is empty for the whole run). Run()'s
epilogue reset it by ASSIGNING a new stack, which constructs a new deque (node map + one
504-byte node buffer: 512/sizeof(std::string) = 21 slots on ILP32) and frees the old —
two allocations + two frees per task, every frame, for a container that never holds
anything. After a session-long run fragmented the heap, that 504-byte request was the
allocation that finally failed. The patch clears the stack in place (pop loop): zero
heap traffic per task on this port, identical semantics (empty stack either way).
One-hunk, context-independent; applied after lus-3ds-shade-alpha-ccmux.patch.

## lus-cc-key-uninit-shader-id.patch (LEAK — THE session heap climb: poisoned combiner key)

The actual driver of the first-hardware-OOM's monotonic 38.7->75.5 MB heapUsed climb,
found by live-allocation histogram + return-address sampling on an emulator race soak
([live-hist]/[live-ra], port/3ds/main_3ds.cpp): 199 of 266 sampled allocations in the
leaking 129..256-byte class were Rb-tree nodes of `std::map<ColorCombinerKey,
ColorCombiner>` (mColorCombinerPool) — ~14 new nodes per race frame, +11 MB live over
~5300 frames, while every census column stayed flat (the pool is census-invisible).

Root cause: upstream added a `shader_id` field to ColorCombinerKey (commit "fix windows
arm CI failure"), but the single construction site (GfxSpTri1's `ColorCombinerKey key;`)
never writes it and nothing ever reads it — an uninitialized stack local inside a
defaulted operator<=>. Whenever the garbage differed from every pooled entry, the lookup
missed: a leaked 148-byte node, a batch-breaking Flush() and a redundant GenerateCC per
draw. The pool has no eviction because unique combiners are finite BY DESIGN — the
poisoned key broke exactly that premise. Fix: value-initialize (`ColorCombinerKey
key{};`). Verified in a fixed-build soak: the ≤256B live class sits flat (~1.4k entries)
through a full race and heapUsed plateaus. One-hunk; applied last of the libultraship
series (after lus-currentdir-reset-churn.patch).

## lus-tmem-same-content-skip.patch (LOADBLOCK-OPT — steady-state re-load elimination)

Menus replay an IDENTICAL per-frame load sequence (the [interleave] work showed it): 82
LOADBLOCKs + 11 LOADTILEs of unchanged o2r-backed textures, every frame — after the span
rewrite that is still ~82 span stores + up to ~300 KB of TMEM-mirror memcpy per frame for
data TMEM already holds. This patch makes GfxDpLoadBlock/GfxDpLoadTile detect a provably
content-identical re-load and skip the store, the invalidation walk AND the memcpy,
leaving only the cheap derivation + equality compare (~1-2 us/load). textures_changed
still arms, so the draw-side import/rebind pipeline is exactly as after a real load.

Soundness (GdxTmemSameContentLoad):
- span live at the load's base, base slot fresh, un-detached, word count matching, and a
  per-span `tmem_span_load_kind` stamp armed for the SAME loader (block vs tile TMEM
  layouts differ for unaligned lines). Any overlapping later load either removed the span
  (overlap invalidation) or — when its mirror overwrote TMEM bytes BEYOND its own span's
  word range (unaligned LOADTILE row strides, scaled transfers) — revoked the stamp via
  GdxNoteTmemLoadKind's overhang sweep. So stamp-armed implies the TMEM mirror still
  holds this load's bytes.
- freshly derived payload equals the stored span field-for-field (addr, sizes, strides,
  tex_flags, masked/blended — derived from the LIVE mMaskedTextures, so replacement-pack
  changes defeat the skip — and the full RawTexMetadata incl. resource identity and
  path_hash). The mirror's copy extent is a pure function of (size_bytes, base) on this
  platform, and the tile row write is a pure function of the compared payload fields, so
  equal payloads imply a bit-identical re-run.
- source immutability: gated on `raw_tex_metadata.resource != nullptr`. The stored span's
  shared_ptr pins the Fast::Texture (and the ImageData its addr points into) alive, so
  pointer equality is object identity, and this port never mutates decoded o2r payloads
  in place. Raw RDRAM sources (which CAN mutate without notice) always take the full
  store path.
- tmem_generation is deliberately not bumped on a skip: its only consumer is the
  GDX_DIAG_TMEMCHK trace (audited; the texture-cache content key hashes TMEM bytes
  directly). The [tmem] store trace likewise logs only real stores.

The skip is compiled everywhere but enabled `__3DS__`-only (constexpr flag): desktop was
never the measured bottleneck and Windows' VirtualQuery-based TmemSourceReadableLimit can
legitimately vary between identical calls, which would break the "pure function of the
payload" premise. Expected on-device: steady menus do ~0 real loads/frame — [profop] F3
22.34 -> well under 1 ms (change frames pay the span-store cost, itself ~20x cheaper),
F4 3.15 -> ~0, [prof] dsp 27.6 -> single digits.
Applied after `lus-tmem-span-store.patch`, closing the libultraship series.

## lus-texrect-run-memo.patch (TEXRECT — per-rect full GfxSpTri1 state re-resolution)

The #1 remaining menu-wall cost after the LOADBLOCK fixes: [profop] E4(G_TEXRECT) =
9.71 ms/222 calls on 3D-model menu screens (6.0/93 on plain menus). Every texrect draws as
2 triangles THROUGH THE FULL GfxSpTri1 state-resolution path, and the S7 tri memo
deliberately excludes rect draws (GfxDrawRectangle's temporary othermode/viewport/geometry
scope would leave a stale TRI memo marked clean). So each rect paid 2× (combiner options
assembly + tile-extent derivation with integer divides + ApplyTileMaskExtent + sampler
guard walk + ShaderGetInfo + GetClipParameters — two virtual backend round trips per tri).
Batching was NOT the problem: consecutive same-state rects already accumulate in mBufVbo
(no per-rect Flush — flushes come only from real state changes and the per-glyph
textures_changed imports, which are semantically required texture switches).

The patch adds a SECOND memo instance for the rect path (sGfxS7RectMemo), banked and
consumed only by is_rect draws, under its own dirty bit with a wider opcode whitelist:
the tri whitelist minus the triangle-emitting ops (their GfxSpTri1 applies GAME
sampler/shader state to mRenderingState, which the rect fast path must be able to prove
untouched) plus the texrect opcodes themselves. The single bool dirty flag becomes a
2-bit mask whose per-opcode contribution is precomputed into the flat-dispatch entry
(gfx_step's whitelist branch becomes a branchless OR). Safety rails, mirroring the tri
memo's and honoring GfxDrawRectangle's mutate/restore pattern:

- the memo is keyed on ColorCombiner identity AND first_tile_index (the texrect handlers
  swap the tile per rect; a switch away from the game's tile re-arms textures_changed on
  exit — probed live — and the key covers the switch back), and re-probes
  textures_changed[] live, so per-glyph loads still import exactly as before;
- every rect runs under the SAME deterministic temporary scope (geometry_mode = 0, the
  fixed rect-bypass viewport, COPY-mode textfilt/combiner overrides that are pure
  functions of the surrounding — whitelist-frozen — othermode state), so a memo banked by
  one rect describes the next rect's resolved state exactly while the dirty bit is clear;
- G_FILLRECT / OTR_G_IMAGE_RECT are NOT whitelisted (fill mutates combine/fill state,
  image-rect rewrites tile descriptors + TMEM slot fields); their draws re-bank through
  the slow path, staying exact;
- GfxDrawRectangle's exit invalidation now kills the TRI memo only; SpReset, StartFrame,
  texture/shader cache clears kill BOTH memos (GfxS7InvalidateDrawMemos) — every
  mRenderingState.mTextures/sampler_valid reset path is one of those;
- state application (depth/decal/viewport/scissor/shader/alpha flushes) stays live and
  exact; only the derivation + the provably no-op sampler re-application is skipped.

Expected effect: tri 2 of every rect and both tris of every rect after the first in a
same-state run skip the extent block and both virtual calls — on the 222-rect 3D-model
menu screens most rects sit in such runs, so E4 should drop from ~9.7 toward the
per-vertex-repack floor (~3-5 ms; the per-glyph import+flush rects keep their required
texture-switch cost). (optmask bit3)

## lus-texrect-viewport-hoist.patch (TEXRECT — per-rect bypass-viewport re-derivation)

Companion to lus-texrect-run-memo.patch (generated against the stack WITH it applied; the
two share the kGfxS7Dirty* mask). GfxDrawRectangle re-derived the rect-bypass viewport for
EVERY rect: the default_viewport construction plus AdjustVIewportOrScissor — which makes a
virtual GetClipParameters round trip into the backend and a handful of ratio multiplies —
222×/frame on the 3D-model menu screens. The result is a pure function of frame/framebuffer
state: mFbActive/mActiveFrameBuffer geometry, native/current dimensions, the game window
viewport, mRendersToFb/mMsaaLevel and the backend clip parameters. Every mutation site of
those inputs is either a non-whitelisted opcode (the framebuffer set/reset handlers set the
new kGfxS7DirtyRectViewport bit via the same flat-dispatch dirty OR) or a task/frame
boundary (SpReset in Run's prologue — which precedes the mRendersToFb re-latch AND the
after-clear seed rect — and StartFrame both run GfxS7InvalidateDrawMemos, which arms the
full mask). While the bit is clear the cached adjusted viewport equals what the derivation
would return; the recompute branch is byte-identical to the legacy always-on code. Expected
effect: one virtual call + the adjust arithmetic per rect RUN instead of per rect
(~0.3-0.5 ms on 222-rect screens). Applied after lus-texrect-run-memo.patch, closing the
libultraship series.

## lus-traffic-pipesync-noop.patch (TRAFFIC-GRIND lever 2 — G_RDPPIPESYNC batch breaks)

Real-HW traffic profile: [profop] E7(G_RDPPIPESYNC) = 2.5 ms over 444-459 calls per worst-window
frame — and each drain that found a non-empty batch split it into a tiny extra draw (the hidden
downstream drw/dsp/submit cost behind the ~208 draws/frame). This renderer orders draws by
submission, so a pipesync only ever guarded state that is sampled AFTER append time; every such
register now flushes at its own mutation site ON A VALUE CHANGE:

- prim_depth — gfx_set_prim_depth_handler_rdp ([prim-depth-flush]);
- blend_color.a — gfx_set_blend_color_handler_rdp ([blend-alpha-flush]);
- prim/env colour — GfxDpSetPrimColor/GfxDpSetEnvColor (lus-3ds-primenv-flush; 3DS backend
  batch constants from vertex 0);
- fog line — the G_MW_FOG moveword handlers (lus-3ds-fog-exact-params; one fog LUT per draw);
- fog COLOUR — GfxDpSetFogColor, guard added BY THIS PATCH: the fog blend stage constant is
  also hoisted from vertex 0 (gfx_citro3d RefreshStageConstants fogBlendStage) and game DLs
  pair gDPSetFogColor with a preceding pipesync, so the drain was silently masking exactly this
  one remaining hazard. No-op re-sets do not split.

Everything else (combiner/shader, othermode-derived depth/decal/alpha, viewport/scissor,
samplers, imports) flushes inside GfxSpTri1's state application when it differs from
mRenderingState. So the 3DS pipesync handler is now a true no-op (GDX_PIPESYNC_FLUSH=1 restores
the drain for A/B); desktop keeps the legacy drain + GDX_NO_PIPESYNC_FLUSH switch. Emulator race
A/B: [profop] E7 3.50 ms/413 calls -> off the top-6 entirely; part of the cost re-surfaces where
the batch now legitimately splits (E4 +0.65, FA +~0.4 — those flushes now perform the draws the
pipesync used to), net ~-2.4 ms emu on the storm windows plus fewer tiny submits. Applied after
lus-3ds-livery-ident.patch.

## lus-traffic-tri-memo-whitelist.patch (TRAFFIC-GRIND lever 3a — memo dirty-mask churn)

Traffic frames interleave ~415 pipesyncs, ~60 prim-colour sets and per-part combine re-asserts
between the machine G_TRI2 runs; every one of those armed the S7 draw-state memo dirty mask and
forced the next tri into the FULL re-resolution (extent divides + ApplyTileMaskExtent + two
virtual backend calls). Whitelists for BOTH memos: the four RDP sync ops (their handlers mutate
no interpreter state — the non-pipe syncs are stubs, the pipesync is at most a batch split),
and SETPRIMCOLOR/SETENVCOLOR/SETCOMBINE (nothing the memo banks reads prim/env colour; the
batch-constant hazard flushes at the mutation site per lus-3ds-primenv-flush, and combiner
identity is a memo KEY — comb/prg compare — so a combine change to a different combiner misses
by construction; only a re-assert of the SAME combine keeps the memo, which is the dominant
30-machines-few-materials pattern). Note the residual traffic miss driver is F5/F2/F3
(~513 SETTILEs + ~250 LOADBLOCKs per frame arming textures_changed, probed live by the memo);
those genuinely change tile state between parts and stay conservative. Applied after
lus-traffic-pipesync-noop.patch.

## lus-traffic-vtx-clipmask.patch (TRAFFIC-GRIND lever 3b — per-tri 21-test clip pre-scan)

GfxSpTri1's clip pre-scan ran the 7-plane x 3-vertex switch-lambda on EVERY non-rect triangle,
fully-inside ones included (~860 race tris/frame x 21 short-circuit FP tests + lambda calls).
LoadedVertex gains a 7-bit clip_out OUTSIDE mask (fits the struct's tail padding), computed at
vertex-transform time with the clipper's EXACT plane arithmetic (x + w < 0 etc., NOT the
clip_rej x < -w spelling — the two can disagree by a rounding step); the pre-scan collapses to
a 3-byte OR. All LoadedVertex producers reaching the non-rect path keep the mask in sync:
GfxSpVertex (inline), the clip-fan vertices (UpdateLoadedVertexClipFlags), and GfxSpLine3DGdx's
screen-space quad (mask recomputed after its x/y rewrite); rect builders skip the pre-scan by
is_rect. requiresClipping is bit-for-bit the old value. Applied after
lus-traffic-tri-memo-whitelist.patch, closing the libultraship series.

`decomp-3ds-sky-overscan-deadend-note.patch` is a comment-only note in
`Background_Update` recording that the `verticalRange *= factor` skybox-overscan
approach for the distant-sky black wedge is PROVEN INEFFECTIVE (the +/-32000 s16
vertex-position clamp in `Background_UpdateSkyboxVtx` caps the quad; GDX_DIAG_SKY
`[sky]` topGap held ~0.31 at both 1.41x and 2.70x). See
`docs/research/night-verify-2026-08-20.md`.

Note: `lus-3ds-fog-exact-params.patch` must be applied AFTER
`lus-texcache-content-hash-span.patch` (it extends the same `#ifdef __3DS__` hook
block in src/fast/interpreter.cpp).

Note: `lus-3ds-texcache-resource-stable-key.patch` is generated against the FULLY
patched tree (its context includes the content-hash-span + speedtex hunks), so it is
applied LAST of the pre-SELECT-PERF libultraship stack.

Note: `lus-3ds-settimg-resolution-memo.patch` is generated against the tree with ALL
previous libultraship patches applied (its ImportTexture hunk rewraps the stable-key
block that `lus-3ds-texcache-resource-stable-key.patch` introduces), so it is applied
after it, last of the libultraship stack.

Note: `lus-3ds-hud-speedtex-hash-span.patch` must be applied AFTER
`lus-texcache-content-hash-span.patch` (it edits the RGBA16 content-hash span block that
patch introduces) and BEFORE `lus-3ds-fog-exact-params.patch` (its hunks sit at a lower
line number than the fog hook block, so this ordering keeps the offsets stable).

Note: `lus-3ds-hud-tall-atlas-extent.patch` edits `ImportTextureRgba16` at a lower line
number than the `content-hash-span`/`fog` hunks, so it is applied before them; the three
touch disjoint regions of src/fast/interpreter.cpp and do not conflict.

Note: the `lus-s7-*.patch` series (S7 interpreter CPU-cost reduction) is REGENERATED
against the FULL menuperf/selperf libultraship stack above (the original series predated
`lus-3ds-texcache-resource-stable-key.patch` / `lus-3ds-settimg-resolution-memo.patch`,
whose SETTIMG-handler rewrite invalidated the raw-dispatch hunk's context — strict
`git apply` of the old files fails on this base; Apple's `patch 2.0` applies them with
SILENT fuzz, which is why a fuzzy-built combined tree must always be hunk-audited).
Series order after the stack: `lus-s7-raw-instance-dispatch.patch` →
`lus-s7-geo-diag-gate.patch` → `lus-s7-tri-state-memo.patch`, each generated against its
predecessors.

## lus-flat-dispatch.patch (DISPATCH — per-command triple-table probing)

The [prof] dsp= column (Interpreter::Run self time: per-command routing + un-instrumented
small handlers) measured 26.28 ms of a ~33 ms menu build / 26.67 ms in the race storm — the
game's dominant CPU cost. gfx_step routed EVERY display-list command through a chain of up
to three 256-slot handler tables (`otrHandlers` -> `rdpHandlers` ->
`ucode_handlers[ucode_handler_index]`): each stage a `contains()` probe plus an `at()`
returning a `{name, fn}` pair BY VALUE, the ucode stage behind a bounds check and a
double indirection — so the common ops (VTX/TRI/MTX, third table) paid ~6 probes — plus a
per-command `GfxS7OpcodePreservesDrawState` switch, a 5-way OTR-filepath opcode scan and
the G_LOAD_UCODE compare.

The chain's precedence (otr overrides > rdp > selected ucode, G_LOAD_UCODE ahead of all) is
STATIC once the ucode is selected, so the patch merges it into a single 256-slot
`sFlatDispatch[]` of `{fn, flags}` (plain function pointers — the handlers already were,
post-S7) rebuilt at every `ucode_handler_index` write (static init, gfx_set_ucode_handler
on a real change, Interpreter ucode reset, gfx_set_target_ucode; F3D<->F3DEX2 switching is
a rare 256-iteration loop). gfx_step is now: one indexed load, the [perf-s7] dirty-flag
test off a precomputed flag bit, the OTR pointer guard off a flag bit, one indirect call.
Semantics preserved exactly: G_LOAD_UCODE keeps its pre-probe precedence as a flat handler
in its slot; unknown-op slots hold diagnostic handlers reproducing the legacy
SPDLOG_CRITICAL("Unhandled OP code ... loaded ucode"/"invalid ucode") + advance behaviour;
the entry is copied out before the call so a mid-command rebuild (G_LOAD_UCODE) cannot
invalidate it. Pure portable C++ — no `__3DS__` gate; it is the same win on desktop.
Expected effect: removes ~5 dependent table probes + pair copies + the whitelist switch per
command from the walk that dominates dsp= (1-3k+ commands/frame).

## lus-vtx-mtx-hoist.patch (DISPATCH — per-vertex MP_matrix reload)

Observation #2 follow-up to lus-flat-dispatch: the GfxSpVertex transform loop read all 16
`mRsp->MP_matrix` floats through the member-pointer chain on EVERY vertex — the stores
through `d`/`loaded_vertices` may alias `mRsp` for all the compiler knows, so it could not
hoist them. Nothing in the loop writes MP_matrix (matrix ops are separate DL commands), so
the patch copies the matrix to a local `mp[4][4]` once per G_VTX batch and transforms from
that. The verbose-gated mpFirstHash diagnostic keeps reading the live `mRsp->MP_matrix`
(same values; no semantic change). Applied after `lus-flat-dispatch.patch`.

## lus-s7-raw-instance-dispatch.patch (S7 — per-command weak_ptr traffic)

Every opcode handler in src/fast/interpreter.cpp opened with `mInstance.lock()` — a
`std::weak_ptr` promotion, i.e. an atomic refcount inc + dec on the shared control block —
once per display-list command (~1.2-3.2k commands per race frame). The patch mirrors the
instance into a file-static raw pointer (`sGfxInstanceRaw`, set in GfxSetInstance) and uses
it in the 97 handler-path sites, which only ever run inside the interpreter's own DL walk
where the instance is alive by construction. Cross-thread entry points that legitimately
race startup/shutdown (`gdx_get_widescreen_geometry_xscale`, the framebuffer API shims)
keep the checked weak_ptr. Also defines `gdx_s7_lus_optmask` (bit0), weak-read by the
bridge's one-shot `[perf-s7]` line so A/B logs self-identify. In the o2r-filepath SETTIMG
handler the substitution sits inside the `if (texture != nullptr)` block AFTER the
SELECT-PERF memo's `#endif` — that is the one hunk whose context the memo patch rewrote.
Expected effect: removes ~2 atomic RMW ops per command from the walk (~0.1-0.5 ms/frame on
the ARM11; more under Azahar, whose JIT makes atomics disproportionately expensive).

## lus-s7-geo-diag-gate.patch (S7 — per-vertex/per-tri diagnostics arithmetic)

The geometry diagnostics in GfxSpVertex/GfxSpTri1 ran unconditionally on every vertex of
every frame: 4 isfinite + 6 min/max + a 3-divide NDC reduction per vertex, a per-triangle
big-tri extent scan (a divide per vertex on Reject variants), per-triangle fog-factor and
UV min/max range scans. Every consumer of those fields ([geodiag]/[bigtri]/[phasegeom]/
[gpustate]) prints only under gdx_diag_verbose(), so the patch gates the arithmetic on a
new `gdx_s7_geo_diag_enabled` flag (bit1 in the optmask): env-seeded from GDX_DIAG_VERBOSE
for desktop parity, and mirrored from the live Dev-Tools gate once per task by
gdx_gfx_run (a cached `gGdxDevGateCache[]` int read — no getenv/config I/O per frame), so
the F1 toggle still works mid-run. Counters that feed always-on lines (verticesLoaded,
trianglesSubmitted/Emitted, fogTriangles) stay unconditional. No menuperf/selperf code
reads the gated min/max fields outside verbose paths (audited on this base: the [gpu]
md= tag reads gGameMode, not geometry diagnostics).
Expected effect: ~0.1-0.3 ms on a median 957-vertex frame; up to ~1-2 ms on the 11k-vertex
p95 spikes (VFPv2 divides are ~19 cycles, non-pipelined).

## lus-3ds-hud-tall-atlas-extent.patch (HUD garbled speedometer)

The RGBA16 TMEM-decode path (`Interpreter::ImportTextureRgba16`, the `!GDX_NO_TMEM`
branch) derived the decoded height as `(remaining TMEM) / lineBytes` and bounded it to the
`gDPSetTileSize` window (lrs/lrt) ONLY when the CLAMP wrap bit was set. F-Zero X's in-race
HUD sprite atlases are all `G_IM_FMT_RGBA/16b` loaded `G_TX_WRAP | G_TX_NOMASK`
(`src/overlays/ovl_i3/hud.c`), so on the WRAP axis the height inflated to fill TMEM:
the 12x160 speedometer digit strip (`aSpeedDigitsTex`) decoded as 12x170, the 20x16 km/h
glyph and the tall 8x224 / 8x72 / 8x132 symbol/lap/racer strips likewise overshot. Because
those atlases are indexed by the T (vertical) texture coordinate, the inflated height shifted
padH/vScale and every UV-selected glyph sampled the wrong row -> the garbled cyan/white
speed digits and green/magenta gauge noise. The wide, single-cell atlases that fill their
tile window (32x32 portraits, 24x16 TIME, 72x16 energy) were visually unaffected, which is
why the bug read as "format-specific" when it was actually extent-specific.

Fix: when an axis carries no mask (`G_TX_NOMASK`), the tile-size window is authoritative
(that is exactly what N64 hardware samples for a NOMASK tile), so bound the extent to it
regardless of the CLAMP bit. Wrapping track/vehicle materials use a real mask
(`masks/maskt != G_TX_NOMASK`) so their path is unchanged, and the bound only ever shrinks
(`tileH < height`), never grows, so it can never over-read. Regression guard:
`port/tests/gfx_rgba16_extent_tests.cpp` (host exe `gdx_gfx_rgba16_extent_tests`) reproduces
the 170-vs-160 delta on the speedometer strip and asserts short/CLAMP cases are untouched.

## lus-s7-tri-state-memo.patch (S7 — per-triangle draw-state re-derivation)

GfxSpTri1 re-derived draw-time state for EVERY triangle: the two-texture-unit extent block
(integer divides + tile-window math + sampler guards per unit) and two virtual calls into the
backend (ShaderGetInfo, GetClipParameters) — even though consecutive triangles overwhelmingly
share identical state (a course chunk is one material followed by a long VTX/TRI run, and the
race p95 spikes are exactly the heavy-tri frames). The patch adds a conservative dirty flag
driven from gfx_step: a whitelist names the pure geometry/flow opcodes that cannot touch draw
state (VTX/TRI/QUAD/MTX/DL/ENDDL/branches + their OTR/wide forms, F3DEX2 family only — the
check also requires ucode_handler_index == ucode_f3dex2, so colliding F3D/S2DEX encodings
never slip through); every other opcode marks the state dirty. While clean, GfxSpTri1 reuses
a memo of the tile extents/tm/effective tiles and the ShaderGetInfo/GetClipParameters results.

Safety rails, each a real hazard found in review: (1) the memo is keyed on the ColorCombiner
identity and re-probes textures_changed[] live (cache eviction inside an import can re-arm it
with no opcode in between); (2) rect draws neither hit nor bank the memo, and GfxDrawRectangle
invalidates on exit — it wraps its GfxSpTri1 calls in temporary othermode/viewport/geometry
mutations it restores afterwards; (3) SpReset (task boundary), StartFrame (frame boundary,
clip parameters), TextureCacheClear/Delete/DeletePalette and gfx_shader_cache_clear (freed
comb/prg pointers) all invalidate. The always-live prefix of GfxSpTri1 (clip/cull rejection,
depth/decal/viewport guards, blend-flag derivation, combiner lookup — already 1-entry cached)
is unchanged, so state application still happens exactly where it used to.

Menuperf/selperf-base coupling, audited: the SELECT-PERF SETTIMG memo still ends in
GfxDpSetTextureImage and its opcode is (correctly) NOT whitelisted, so every o2r SETTIMG
dirties this memo exactly like an unmemoized one; the sampler-parameter application the fast
path skips is a provable no-op while the flag is clean (no opcode has touched other_mode_h/
cms/cmt since the banking triangle, and every sampler_valid[] reset path also invalidates);
resource-cache-cap evictions land in TextureCacheDelete/Clear, which invalidate.

Expected effect: removes ~150-350 cycles/tri on clean-state triangles — ~0.1-0.4 ms on a
median 318-tri frame, several ms on the 3726-tri p95 spikes the campaign names as the tail
to attack. (optmask bit2)

## lus-newlib-portability.patch (stream A / Phase 0 spike)

newlib type quirks for devkitARM: on arm-none-eabi `int32_t`/`uint32_t` are
`long`-based, so exact template matches and virtual-override signatures that pass on
desktop fail. 8 files; see docs/research/spike-lus-carve-report.md finding 4.

## lus-device-path-archives.patch (M1 assets debug)

`ArchiveManager::GetArchiveListInPaths` ran every archive path through
`std::filesystem::absolute()`. Under std::filesystem's POSIX grammar a devkitPro/newlib
device path ("sdmc:/3ds/gdiffuser/fzerox.o2r") is RELATIVE — "sdmc:" is an ordinary
first component — so absolute() prepends the cwd ("/") and O2rArchive received
"/sdmc:/3ds/gdiffuser/fzerox.o2r", which no devoptab matches (fopen → ENOSYS, errno 88).
The patch adds `ResolveArchivePath`: device-prefixed paths (`<name>:/...`) pass through
untouched, everything else keeps absolute(). Root cause of the M1 "every o2r key
missing" boot (docs/research/m1-boot-debug.md); it was masked by the zipshim's former
ZIP_CREATE empty-handle fallback, now also removed (port/3ds/assets/zipshim/).

## decomp-port-segment-bzero.patch (M1 boot debug)

`#ifndef PORT` guards on the two raw `bzero(SEGMENT_BSS_START(x), SEGMENT_BSS_SIZE(x))`
calls (`src/sys/sys_main.c` leo, `src/sys/rom/disk_drive.c` ovl_i11). Under PORT the
overlay BSS markers are 1-byte stubs in `port/gen/LinkStubs.c` that the linker places
arbitrarily; on the 3DS link `leo_BSS_END` (0x009C4070) lands BELOW `leo_BSS_START`
(0x009D1160), so the size goes negative (≈4 GB as `size_t`) and the bzero zeroes the
tail of the host `.bss` before spinning on unmapped writes at the image end — the M1
black-screen boot hang (docs/research/m1-boot-debug.md). Every other segment-BSS
consumer (`Dma_LoadOverlay`, `DiskDrive_LoadOverlay*`, `Dma_ClearRomCopy`,
`func_800742FC`) was already a PORT no-op or guarded; these two were only compiled
with `EXPANSION_KIT` off, which desktop never builds.

## decomp-ilp32.patch (M1 integration; stream E's blockers #1 and #2)

Prerequisites for ANY 32-bit (3DS/devkitARM) build of the decomp — see
docs/research/32bit-sweep.md "Deferred / filed upstream" items 1-4:

- **include/libc/stdint.h** — proper ILP32 branch: `intptr_t`/`uintptr_t` spelled
  `int`/`unsigned int` exactly like newlib (`__INTPTR_TYPE__`), and 32-bit
  `INTPTR_MIN/MAX`, `UINTPTR_MAX`. Previously every non-LP64 host fell into the
  LLP64 branch and got a 64-bit `uintptr_t` (truncating casts everywhere + hard
  conflicting-typedef errors against newlib's `<stdint.h>`).
- **include/libc/stddef.h / stdlib.h** — `ptrdiff_t` (`int`) and `wchar_t`
  (`unsigned int`) spelled to match newlib/GCC on ILP32; `long`-based spellings are
  size-identical but type-distinct and hard-error when a TU sees both headers.
- **include/PR/os_time.h** — on `GDX_PLATFORM_3DS`, `osGetTime`/`osSetTime` are
  aliased to `gdx3ds_osGetTime`/`gdx3ds_osSetTime`: libctru's os.o also exports an
  `osGetTime` (ms since 2000, unrelated semantics) and is always pulled into the link,
  so the port shim (port/3ds/lus_glue/gdx3ds_libultra.cpp) cannot define the colliding
  name. Desktop builds see the original names.
- **include/PR/gbi.h** —
  1. `_GFXW1_PTR` tags host pointers with `GDX_GFXW1_HOST_TAG` ("GDX1") in the spare
     high32 of the u64 `GfxW1` on 32-bit builds: a 32-bit host pointer is otherwise
     value-indistinguishable from a segment token (3DS heap 0x08xxxxxx == segment 8;
     static data 0x00xxxxxx == segment 0). port/n64_gfx_bridge.cpp's ProcessList
     recognizes exactly this tag as its host-pointer fast path (kGfxW1HostTag32 —
     keep the constants in sync).
  2. Static `gs*` initializer forms rewritten onto `_GFX_STATIC_PTR_INIT`: a 32-bit
     toolchain has NO relocation that stores a symbol address into a 64-bit field
     (GCC: "initializer element is not constant"), so on ILP32 the macro initializes
     a word-split `GwordsStatic32` union arm — address relocated into `w1lo`, tag as
     a plain constant in `w1hi`. On 64-bit hosts and N64 builds the macro expands to
     the original `{{ w0, _GFXW1_PTR(x) }}` shape (semantically identical).

## lus-3ds-settimg-low-address.patch (M1 present debug)

`gfx_set_timg_handler_rdp` rejects any texture address `<= 0x0FFFFFFF` as an
unresolved N64 segmented address (non-Windows branch). On the 3DS the process image
maps from 0x00100000 and the malloc heap at 0x08000000, so EVERY valid host texture
pointer fell inside that range: SETTIMG silently dropped all game textures, no
ImportTexture/UploadTexture ever ran, and every textured draw sampled a stale
texture id — the M1 "top screen black while everything upstream is healthy" content
root cause (docs/research/m1-boot-debug.md, shift N+1). The patch adds a `__3DS__`
branch that only rejects pointers below the process image base (0x00100000).

## decomp-port-audio-specwait-yield.patch (M1 race freeze)

`Audio_SetSpec` waits for the audio-heap reset with a raw
`do {} while (!AudioThread_ResetComplete())` spin on the game fiber (host thread).
Under PORT the reset stages only advance on the dedicated audio thread
(`gdx_audio_produce_one_tick`), and on the 3DS kernel — and Azahar's faithful
emulation of it — a running thread is never preempted by an equal-priority thread
(the devkitARM thread shim inherits the creator's priority), so the spin starves
the audio thread forever: a permanent, svc-silent, single-core freeze. The PORT
branch (both `audio/rom` and `audio/disk` variants) yields 1 ms per iteration via
`port/n64_sched.c:gdx_audio_specwait_yield`, which also feeds the watchdog's
`spec=` counter. Desktop is unaffected in behavior (preemptive OS scheduling
already saved it) but gets the same polite wait.

## decomp-3ds-dma-low-address.patch (M1 race-entry freeze)

`Dma_PortRamPointer` (src/sys/dma.c) treats any address `< GDX_DMA_RDRAM_SIZE`
(16MB) as a raw RDRAM offset. On 64-bit hosts pointer width alone excludes real
pointers from that branch; on ILP32 it does not, and the 3DS process image maps
from 0x00100000 — EXE globals below 16MB (gCourseCtx at ~0x0038xxxx) had their
DMAs rerouted into the RDRAM shadow. Course data never arrived, the course
segment list never linked, and Course_SegmentLengthsInit spun forever on NULL
segment pointers — the mode+difficulty race-entry freeze and its ~18.5k/s
unmapped-access storm (which also tanked emulator fps). The patch makes the
branch prefer a registered host range (the EXE image is registered at boot) for
addresses >= 0x00100000, restoring the 64-bit semantics; unclaimed addresses
still resolve as RDRAM offsets, and addresses below 0x00100000 (unmapped on the
3DS) stay unambiguous.

## lus-texcache-content-hash-span.patch (T-TEXCACHE re-upload thrash)

The interpreter's `tmem_content_hash` (the staleness signal in the texture-cache key)
hashed `min(remaining TMEM, tile_line_bytes * 64)` — for common strides that reaches
the END of TMEM, so a stable texture's key folded in every OTHER texture streaming
through TMEM. Any scene with per-frame-varying TMEM traffic (attract, menus with
animations, a moving race) minted a fresh key per import: every import missed, LUS
re-decoded and the backend re-Morton-swizzled + re-uploaded the same textures every
frame ([c3d] texUp thrash, docs/research/m1-boot-debug.md "T-TEXCACHE"), while the
1024-entry LRU churned. The patch:

- bounds the hash span to exactly the bytes the decode reads (RGBA16 TMEM path:
  a mirror of ImportTextureRgba16's extent derivation incl. mask/CLAMP; font I/IA
  paths: the recorded load extent) — a strict content identity, tighter AND more
  correct than the old bound, which could also UNDERSHOOT decodes with strides
  < 64 bytes;
- folds the FNV-1a hash 4 bytes at a time (it runs on every import, ~100-200/frame);
- adds 3DS-only telemetry hooks (`gdx3ds_texcache_note_import/miss/delete`,
  implemented in port/3ds/gfx/gfx_citro3d.cpp) that extend the [c3d] line with
  texImp/texMiss/texDel and emit bounded race-gated `[texmiss]` key dumps.

## lus-3ds-hud-speedtex-hash-span.patch (HUD speedometer garbage — residual after the extent fix)

`lus-3ds-hud-tall-atlas-extent.patch` fixed the RGBA16 **decode** extent for NOMASK+WRAP HUD
atlases (`ImportTextureRgba16` now bounds the decoded height to the `gDPSetTileSize` window),
which stopped the row-shifted garble on the 8x224 / 8x72 / 8x132 symbol/lap/racer strips. But
the in-race speedometer km/h block (bottom-right green/magenta noise) and speed digits (garbled
dark rectangles under the ENERGY bar) STILL rendered as scrambled bytes — a different failure
mode (wrong bytes, not a row shift), so it was a second, independent bug.

Root cause: the `tmem_content_hash` SPAN in `Interpreter::ImportTexture` (added by
`lus-texcache-content-hash-span.patch`) mirrored the decode's `CLAMP` bound but NOT the
extent-fix's NOMASK→tile-window bound. So for a NOMASK+WRAP atlas the hash folded MORE TMEM rows
than the decode actually read: the 12x160 `aSpeedDigitsTex` hashed 170 rows (4080 B) while the
decode read 160 (3840 B), and the 20x16 `aKmhTex` hashed ~102 rows (4080 B) while the decode read
16 (640 B). Both strips load to TMEM word 0, so the over-read folded whatever texture streamed
through the shared low TMEM below them — which VARIES frame-to-frame in a live race. A STATIC
atlas therefore got a fresh content key every frame: every import missed the texcache, LUS
re-decoded and the 3DS backend re-Morton-swizzled + re-uploaded it every frame, and a re-upload
race served a rotated/half-written `texture_id` → the scrambled green/magenta pixels. (The wide
single-cell atlases — 32x32 portraits, 24x16 TIME, 72x16 energy — fill their tile window, so
their hash span already equalled the decode read and they were stable, matching the observation
that only the two tall speed atlases stayed broken.)

Fix: apply the same `boundW/boundH = CLAMP || mask == G_TX_NOMASK` bound to the hash span that the
decode uses, so the hash covers EXACTLY the bytes the decode reads and no more. Distinct decoded
content still hashes distinctly (the identity the key exists for); identical content now hashes
identically frame-to-frame, so the texcache hits and the per-frame re-upload/rotation stops.
The bound only ever SHRINKS the span (tile window < TMEM-fill), so it can never over-read.

Also adds a `GDX_DIAG_SPEEDTEX` env-gated `[speedtex]` dump (race-gated, 240-line budget) that
prints the addr / tmem word / line / tile WxH / span / hash for exactly these two atlases, so an
emulator pass can confirm the hash is now STABLE across frames for one address. Regression guard:
`port/tests/gfx_rgba16_extent_tests.cpp` gains span-vs-decode-read checks (host exe
`gdx_gfx_rgba16_extent_tests`, ALL PASS) proving old span=4080 ≠ decode read but new span == the
decode read (3840 for digits, 640 for km/h), with the CLAMP case unchanged.

## lus-3ds-texcache-resource-stable-key.patch (MENU-PERF / SHIP-TEXTURE — stable resource keys)

The interpreter's texture cache keys resource-backed textures (OTR/O2R filepath
SETTIMGs — menu text/UI, HUD atlases, portraits, machine decals) by their `ImageData`
heap address. Upstream LUS never unloads resources, so an address is a stable identity
there. The 3DS resource-cache byte budget (`lus-resource-cache-cap.patch`, 24 MiB)
breaks that invariant: an evicted `Fast::Texture` frees its `ImageData`, a later
resource load can recycle the SAME heap address for a DIFFERENT texture, and when the
descriptor geometry also matches (F-Zero X's machine decals are dozens of same-format
16x16 I4 blocks) the address-keyed lookup HITS the stale entry and binds the WRONG
texture's GPU upload. Content-hashing does not cover these entries
(`tmem_content_hash` is computed only for `metadata->resource == nullptr` TMEM
decodes), and menus draw nearly everything through the resource path.

Fix (replicated from feat/3ds-tilebind, WITH its Id-pitfall correction): fold an
FNV-1a hash of the resource's archive path into the otherwise-unused
`tmem_content_hash` field for resource-backed imports. Device receipts on tilebind
(`[ship-draw] khash=00000001` on every resource) proved `ResourceInitData::Id` is a
constant 1 in this loader, so the path is hashed ALWAYS — never trust Id. A recycled
address can then never satisfy another resource's key; reloads of the same resource
keep an identical hash, so the steady state costs no extra uploads.

## lus-3ds-settimg-resolution-memo.patch (SELECT-PERF — per-SETTIMG resource memo)

The bridge rewrites asset-segment SETTIMGs to OTR_G_SETTIMG_OTR_FILEPATH commands
whose w1 is an interned key string, and the interpreter's handler ran
`ResourceManager::LoadResourceProcess` for EVERY such command EVERY frame:
ResourceIdentifier construction (std::string copy), OTR-signature/alt-path checks,
the cache mutex, two string-keyed map lookups and the LRU tick. Menus draw nearly
everything through this path; the machine-select and machine-settings
(accel/max-speed) screens are the worst case (~93 resolutions/frame: the 30-machine
grid / multi-pass single machine with body + decal textures, stat bars, fonts) — a
real slice of the 23-34 ms menu build on the 268 MHz ARM11.

The patch memoizes the resolution in a 512-slot direct-mapped table keyed on the w1
key-string POINTER (bridge-interned, stable per texture). Three safety layers, none
trusting the pointer alone:

- payload is a `weak_ptr<Fast::Texture>`: eviction under the 24 MiB resource-cache
  budget (lus-resource-cache-cap.patch) drops the cache's shared_ptr and the memo
  entry expires BY CONSTRUCTION — no generation counter to desync, and weak refs do
  not count toward the evictor's `use_count() > 1` pin test, so the memo never pins
  memory;
- a hit strcmp-verifies the texture's `InitData->Path` against the key (with the
  `__OTR__` prefix skipped, matching LoadResourceProcess), so a recycled key pointer
  can never bind another resource's texture — the stable-key patch's identity
  discipline, at strcmp cost;
- every 256th hit falls through to the full LoadResourceProcess to refresh the
  resource-cache LRU tick, so the byte-budget evictor still sees the entries as hot.

The memo also precomputes the stable resource key fold (the per-import FNV-1a over
the archive path that lus-3ds-texcache-resource-stable-key.patch added) and ships it
via a new `RawTexMetadata::path_hash` field, so ImportTexture stops re-hashing the
path string on every one of those ~93 imports/frame. Value-identical fold by
construction: texture-cache keys are stable across memoized and non-memoized loads.
`path_hash == 0` (raw/hash SETTIMG paths, desktop) keeps the pre-existing derivation.

Telemetry: the handler calls `gdx3ds_settimg_note_load/note_memo`
(port/3ds/gfx/gfx_citro3d.cpp), surfaced as rl=/rm= on the [gpu] line and dRl=/dRm=
on the [c3d] line. Expected on a machine-select dwell: rl collapses from ~93/frame
to ~0.4/frame (the 256-hit lease) with rm carrying the volume; imp/dImp stays ~93
(ImportTexture still runs, now without LoadResourceProcess or the path FNV behind
it). Everything is `__3DS__`-gated; desktop builds compile the pre-existing path.

## lus-3ds-primenv-flush.patch (PRIM-COLOR — the yellow Blue Falcon body)

The player's Blue Falcon body rendered with a rival's YELLOW patterning in-race
(machine-select correct, real N64 blue). Every texture-layer suspect was exonerated
on-device (delivery CRC-perfect, texcache keys distinct, [interleave] LOAD→CONSUME
slot binding correct incl. the player's wingChecker): the body texture is I4 16x16 —
intensity only — so its on-screen colour comes ENTIRELY from the combiner's constant
colour. F-Zero X colours each machine body via `gDPSetEnvColor(racer->bodyR/G/B)`
between same-combiner draws (decomp `src/game/racer.c` Racer_Draw; body combiner
`LERP(TEXEL0, ENVIRONMENT, TEXEL0_ALPHA)` × SHADE, machine_draw.c shows the same
family), and `Interpreter::GfxDpSetEnvColor`/`GfxDpSetPrimColor` mutate RDP state
WITHOUT flushing pending tris — correct for desktop GL, which reads prim/env as
per-vertex attributes baked at tri-append time. The 3DS backend instead hoists those
draw-constant inputs onto per-stage TexEnv CONSTANT registers bound from **vertex 0
of the flushed batch** (RefreshStageConstants: "constant across a flush, so vertex 0
is authoritative"). That invariant is exactly what the missing flush breaks: any
batch that survives a prim/env change (untextured same-shader pieces, sampler-equal
same-shader runs — nothing else in the tri path splits on a colour change) hands
EVERY tri in it the FIRST draw's colour. With ~6 machines cycling through the same
combiner per race frame the player's body could inherit a rival's yellow; the
machine-select screen draws ONE machine per frame, so no batch ever spanned a
change — why it always looked correct. Same defect class as the G_MW_FOG moveword
flush inside lus-3ds-fog-exact-params.patch (per-vertex on desktop, draw-constant
on this backend ⇒ value changes must split the batch).

Fix (`__3DS__`-gated, desktop byte-identical): GfxDpSetPrimColor/GfxDpSetEnvColor
flush pending tris when the incoming VALUE differs from the current register
(prim includes prim_lod_fraction — it rides the same constant path). No-op re-sets
do not split, so DL patterns that re-assert an unchanged colour cost nothing.
Regression risk is extra draw splits only where colours genuinely change mid-batch —
draws that were WRONG before; the backend side (committed in
port/3ds/gfx/gfx_citro3d.cpp, no patch) compensates by value-dirty-tracking the
per-draw constant re-apply (mAppliedStageConst mirror), skipping the redundant
C3D_TexEnvColor + TexEnv GPU re-emit that previously ran on every
constant-consuming draw.

Diag: one-shot `[prim]` window (INI `[debug] diag_prim=1` or env GDX_DIAG_PRIM),
3 race frames after a 90-frame warmup — `[prim] set` logs every prim/env value
change with the pending-tri count it split off, `[prim] draw` logs each
constant-consuming draw's bound constant (c0, PICA packed, r low byte) + tex0 WxH +
which stage registers moved. Player body receipt: a 16x16 draw whose c0 reads the
Blue Falcon blue. Hook `gdx3ds_prim_note_set` follows the fog/texcache strong-link
pattern; the patch applies LAST of the libultraship stack (after
lus-3ds-settimg-resolution-memo.patch).

## lus-3ds-fog-exact-params.patch (C2/C3-COURSE-CULL — the "invisible road")

The 3DS backend drives the PICA fog unit with a 128-entry LUT and reconstructed the
fog line by fitting a secant through the batch's two depth-extreme vertices' baked fog
factors (gfx_citro3d.cpp UpdateFogState). Those factors are CLAMPED
(`clamp(r*fog_mul + fog_offset, 0, 255)` with r = z/w in [-1, 1]), so for any steep
fog curve the secant is wrong the moment an endpoint clamps — and worse, a per-draw
LUT built from GLOBAL fog state can bind the WRONG line entirely. F-Zero X races were
the worst case of both: the whole track painted bit-exact solid fog colour (Mute
City: the sky pink 0xfdc0fc), leaving only the chunk under the machine visible, no
rivals — the "invisible road" race bug. A `gdx-nofog.txt` A/B (below) proved the fog
unit was the painter: with the unit disabled the full track, rails and rivals render
perfectly. The game's CPU chunk cull and the whole decomp draw path were exonerated
by instrumentation (decomp-race-cull-diagnostics.patch).

The patch mirrors the interpreter's RSP fog state to the backend via
`gdx3ds_fog_note_params(mul, off, exact)` — the same `#ifdef __3DS__` strong-link
hook pattern as the texcache telemetry — so the LUT is built from the true clamped
line. Four details are load-bearing (each was a real bug in an earlier iteration):

- **Flush pending tris when a G_MW_FOG moveword CHANGES the line (C4).** The
  append-time latch alone is not enough: with no flush, one batch can contain tris
  baked under line A and (after a mid-DL gSPFogPosition retune with no other state
  change) tris baked under line B — and the whole batch binds the LAST latched line.
  Drive-time race frames hit exactly this (intro frames flush per draw and telemetry
  agreed there, which is why C3's first 48-draw [fogdraw] window looked clean while
  the captures stayed solid pink). The `__3DS__`-gated Flush() in both moveword
  handlers splits the batch at the line change; a no-op moveword does not split.

- **Latch at fog-attribute APPEND time, not at the `G_MW_FOG` moveword.** Movewords
  do not flush pending tris (per-vertex baking never needed a flush), so a
  moveword-time mirror hands an already-batched draw the NEXT draw's fog line.
  F-Zero X's race DL interleaves several gSPFogPosition settings per frame; the
  track batch got a later, shallower line that saturates by ~250 world units —
  that misbinding WAS the solid-pink track.
- **Coordinate mapping.** The backend's LUT input is the bufVbo z/w, and
  GetClipParameters() sets z_is_from_0_to_1, so the interpreter hands it
  d = (r + 1)/2 in [0, 1]. The exact LUT-space line is therefore
  `f(d) = (d*(2*mul) + (off - mul)) / 255`, not mul/255 & off/255.
- **`exact=0` for constant-factor blend-colour draws** (G_BL_CLR_BL shroud mode):
  their appended factor is the constant fog_color.a, not a depth line, so they keep
  the vertex-scan fit (whose a~0/b~const output is exactly their semantics).

The ucode-load resets also note (0, 0, 1). The vertex-scan secant remains as the
fallback. Desktop builds are untouched (`__3DS__`-gated; desktop fog interpolates the
per-vertex baked factors and never had the bug).

## decomp-3ds-machineselect-gradient-coalesce.patch (SELECT-PERF — background gradient)

`MachineSelect_BackgroundDraw` (shared by the machine-select AND machine-settings /
accel-max-speed screens — exactly the two screens reported extra slow) paints its
blue background gradient as one `gDPPipeSync + gDPSetFillColor + gDPFillRectangle`
triplet PER SCANLINE: ~224 (240 borderless) single-row fill rects, ~700 Gfx commands
per frame that the bridge re-translates and the interpreter rasterises as 2 tris each
(with the full per-tri state resolution). No other menu screen does this — main menu
and course select emit zero fill rects, the title screen one. It is a pure
differential CPU tax on the machine screens.

The `>>3` quantisation collapses the 224 rows to at most ~8 distinct 5551 colours
(r∈{0,1}, g=0, b∈0..7), so the patch (GDX_PLATFORM_3DS-gated; desktop DL content
stays byte-identical) coalesces runs of identical fill colour into one tall fill
rect: ~8-9 rects instead of 224/240. Pixel-identical by construction — a multi-row
FILL rect is the exact union of its single-row rects, the colour math per row is
unchanged (including the borderless index clamp), and a PipeSync between same-colour
fills has no visual effect. Expected: machine-screen draws= / tris= in [c3d] drop by
~215 rects / ~430 tris per frame, build= falls correspondingly.

## decomp-race-cull-diagnostics.patch (C2/C3-COURSE-CULL)

Bounded, read-only-except-kill-switch, PORT-gated diagnostics that settled where race
geometry was lost:

- `[cull]` in Course_Draw (C2): per-64th-frame census of the chunk depth/NDC cull
  with its matrix operands and 4 sample chunks; 64 bursts max.
- `[cull2]` (C3): full concatenated-matrix row (m30/m31, col0/col1, p00/p11), a
  reclassification of every depth-in-range rejected chunk by WHICH NDC test rejected
  it, by-index samples with both NDC coordinates, and a near-band (w < 3200) dump —
  this proved every road-ahead chunk passes the cull (ndcx ~0.02 at 200-4400 units)
  and the rejects are geometrically correct side/behind chunks.
- `[grp]` (C3): the chunk-group tessellator walk (both DrawChunkGroup functions),
  logging each group and each emitted span on armed frames — proved the painter's
  order walk emits contiguous spans covering the whole visible course.
- `[rvcull]` in Racer_Draw (C2): racer visibility census with operands for up to 3
  culled-but-active rivals — rivals vis up to 29/30 at race start, gate healthy.
- **`gdx-nocull.txt` kill-switch** (C3, via `gdx_nocull_test_enabled()` in
  port/n64_sched.c): force-passes every depth-in-range chunk, bypassing the NDC
  tests. The discriminating experiment: the race frame was pixel-identical with the
  cull forced open, exonerating the cull end-to-end and (with `gdx-nofog.txt`)
  pinning the invisible road on the fog LUT misbinding fixed by
  lus-3ds-fog-exact-params.patch.

Kept applied: the logs are the fastest regression tripwire for this class. The census
family is OFF by default behind the `GDX_DIAG_CULL` Dev Tools gate (C4: an armed frame
emits ~75 formatted-float lines and the human measured the bursts costing fps); enable
via the env var or F1 > Dev Tools > "Course/racer cull census". No game state is
written unless the nocull file exists; desktop behavior identical (PORT-gated, logging
only, switches off by default).

## decomp-3ds-rom-audio-port.patch (AUDIO-CONTENT — rom-driver host port; silence root cause)

The 3DS build compiles the ROM-side audio driver (`src/audio/rom/`, selected by
`GDX_EXPANSION_KIT=OFF` in port/3ds/game/CMakeLists.txt) — a driver the desktop port
NEVER builds (desktop is EXPANSION_KIT=1 → `src/audio/disk/`), so it had received
none of the disk driver's PORT work. Result on device: HLE ran an empty ~27-command
mix every tick and `[audio-out]` showed `nz=0` forever. Four fixes, all mirrors of
the disk driver's existing PORT blocks:

- **`external.c`**: `Audio_Update` was stubbed `return` under PORT (stale
  "Audio_Init is skipped" rationale from pre-EK desktop bring-up). That killed the
  game's entire per-frame audio pump — engine SE, SE stacks, BGM state machines.
  Un-stubbed (guarded by `gAudioContextInitialized`), plus `Audio_SetOutMode` and
  `Audio_GuitarSeqStart` un-stubbed. One-shot `[audio-rom]` receipt on first pump.
- **`lib/thread.c`**: the little-endian AudioCmd union fix (`QueueCmdS8/U16` stored
  their payload at byte 3 via the console's big-endian shift; every
  `AUDIOCMD_CHANNEL_SET_IO` — including `Audio_SEStart`'s sfxId — read back 0), plus
  the cross-thread wiring: QueueCmd under `gdx_audio_ctx_lock`,
  `ScheduleProcessCmds`/CreateTask drain over the lock-free cmd ring, taskStartQueue
  post to the host atomic. Periodic `[audio-seq]` receipt (seq players + notesOn).
- **`lib/load.c`**: the in-place font "relocation" read the big-endian font image as
  host-endian and patched garbage pointers into it. Replaced with the disk driver's
  `gdx_fontconv` host-native conversion, adapted to the ROM font layout (no SFX
  array; instruments at u32[1+i]; Sample flags `codec:4 medium:2`). `[fontconv]`
  receipt per converted font.
- **`lib/seqplayer.c`**: `AudioSeq_PortConvertSeqEnvelope` for the layer/channel ENV
  ops (seq blob stays big-endian; envelopes must be converted, pointer-keyed cache).

Port-side counterpart (committed normally, not a patch): `[audio-hle]` receipts in
port/n64_audio_hle.c — per-run command count, ADPCM/ENVMIXER voice-op counters, and
a one-shot "FIRST NONZERO output" line naming the first real PCM leaving the DSP.

Note: `decomp-3ds-rom-audio-port.patch` was REGENERATED 2026-08-21 (audio
forensics) from the feat/3ds-audiocontent worktree's final audio state: the
originally committed 736-line patch was an earlier iteration and is NOT
sufficient for audible output (empty ~27-cmd mix, adpcm=0). The regenerated
1015-line patch (external.c, lib/{audio.h,heap.c,load.c,seqplayer.c,thread.c})
is the audible state, roundtrip-verified against the sanctioned stack. See
docs/research/audio-regression-forensics.md.

## decomp-port-course-select-state-reset.patch (TIMETRIAL — post-GP course-select input freeze)
`CourseSelect_Init`'s `sCourseSelectState = COURSE_SELECT_CUP_SELECT` reset is
`#ifdef EXPANSION_KIT` in the decomp: the retail US ROM relied on the course_select
OVERLAY re-DMA restoring the `.data` initializer on every entry, and the EK
re-release added the explicit reset. The port links overlays statically
(`Dma_LoadOverlay` does not restore initialized data), so on the 3DS build — the
only non-EK port build (`port/3ds/game/CMakeLists.txt` sets `GDX_EXPANSION_KIT OFF`;
desktop is EK=ON, which is why desktop never showed this) — a SECOND course-select
visit inherits the previous visit's state (`COURSE_SELECT_CONTINUE` after advancing
to machine select, `COURSE_SELECT_NEXT_COURSE_*` after a GP cup's next-course
screens). `CourseSelect_Update`'s state switch has no case for those values, while
every draw switch does: the screen keeps rendering (identical DL every frame,
`[gpu] md=10 draws≈141 tris≈3760` — the frozen fingerprint in the hardware log)
but ignores ALL input forever. Hardware-verified as the "TIME TRIAL entry wedges
after a GP cup" freeze (watchdog `stage=7 fiber=-1` was a red herring — normal
between-frames snapshot; the `[tt]` park dump added alongside this patch shows all
fibers healthy in that state). Fix: perform the reset under `PORT` as well.
## lus-crowd2-tilestate-value-gate.patch (CROWD-GRIND-2 — SETTILE/LOADBLOCK memo-thrash)
Crowd frames (30 machines) re-issue ~513 SETTILEs + ~250 LOADBLOCKs, most of them
bit-identical re-assertions of the tile/TMEM state already in effect (machine parts
sharing one livery texture). Each one armed textures_changed and dirtied the draw
memos, forcing the next TRI into the full Flush + ImportTexture + extent
re-derivation path — the [profop] 06 (G_TRI2) 12.5 ms/frame crowd thrash.
Three coordinated pieces, in dependency order:
1. EVICTION HOOK (the prerequisite invariant): every site that nulls a
   mRenderingState.mTextures binding (bind-path LRU eviction, TextureCacheDelete,
   TextureCacheDeletePalette) now arms textures_changed itself. Previously the
   unconditional SETTILE re-arm was the rescue for these holes; the mutation sites
   must own their own re-arm before that rescue can be value-gated. GfxDpLoadTlut
   (palette content change) and GfxDpImageRectangle (in-place tile-descriptor +
   loaded-slot rewrite) get the same treatment for the same reason.
2. VALUE GATE: GfxDpSetTile / GfxDpSetTileSize compare every field they would write
   (post cms/cmt canonicalization) and return without arming or dirtying when the
   descriptor already holds those values — a bit-identical re-assertion leaves no
   observable state. The LOADTILE skip path's extent writes ride the same gate.
3. CONDITIONAL DIRTY: SETTILE/SETTILESIZE/LOADBLOCK(+WIDE)/LOADTILE/SETTIMG(+OTR
   hash/filepath) move onto the flat-dispatch preserve whitelist (their precomputed
   kGfxS7Dirty* contribution becomes zero); the handlers OR the legacy contribution
   into sGfxS7DirtyMask exactly when they change state. The LOADBLOCK/LOADTILE
   same-content skip no longer arms textures_changed (sound with piece 1 in place).
   SETTIMG is unconditionally preserve: it only stages texture_to_load, which the
   draw-state derivation never reads. OTR_G_SETTIMG_FB stays off the whitelist.
Applied after `lus-cc-key-uninit-shader-id.patch`, closing the libultraship series.
## decomp-port-rival-detail.patch (RIVAL-DETAIL — crowd-frame CPU via earlier rival LOD)
The 30-machine crowd frames pay per-rival triangles + draws + texture switches the
game already knows how to shed: Racer_Draw natively tiers every machine by view
depth (machineLod 1..6 at 230/290/380/470/1500, shadow gate unk_2B2 at 800, effect
tier unk_2B3 at 400/900) and D_800CDDB0 holds six per-machine display lists down to
a far impostor. This patch makes those stock thresholds engage earlier for RIVALS
only, driven by a port hook `gdx_rival_detail_level()` (0 NATIVE / 1 REDUCED /
2 MINIMAL; 3DS: `[perf] rival_detail`, live row on the menu DISP tab, implemented in
port/3ds/gdx3ds_menu.c — latched from the INI even when the menu UI is disabled).
Mechanism: a pre-pass after the sp580 projection row build marks biased racers in
`sGdxRivalBiased[]`; in the main tier loop a biased rival's `temp_fa0` is stretched
`near + (d - near) * bias` before the threshold chain (REDUCED near=250 bias=2.5,
MINIMAL near=120 bias=6). Piecewise-multiplicative: exact native inside the near
band, scale-free beyond it, and ALL downstream thresholds (LOD tiers, shadow,
effects) inherit the one rewrite — no per-threshold constants to keep in sync.
Effective REDUCED: lod5 by ~470 raw, shadows off past ~470 (stock 800), effects
2/1 tiers at ~310/~510 (stock 400/900). MINIMAL: lod5+ past ~180, shadows off past
~233, effects at ~167/~250.
Safety rails: human machines (index < gNumPlayers) are NEVER biased; in MINIMAL the
nearest GDX_RIVAL_PROTECT_COUNT(5) rivals by |view depth| keep native treatment (you
fight your neighbours — a rival right behind the camera still counts as near); the
TIME_ATTACK ghost loop (<=3 machines, not a crowd) and the GP_END_CS/COURSE_EDIT
all-lod-1 paths are untouched; NATIVE (0) leaves every comparison on the stock
constants; non-PORT builds compile the original code. `temp_fa0` is not read after
the tier assignments (verified), so overwriting it in place is safe. Applies
independently of the other decomp patches (racer.c is otherwise untouched by the
stack); listed last of the decomp series.
Applied after `lus-cc-key-uninit-shader-id.patch`.
## lus-3ds-triloop-packed-vbo.patch (TRILOOP — the tri path's double vertex copy)
Precedent: Wyatt-James/sm64-3ds-port's tri-loop optimization (~0.8 ms measured on
their tri path: fold the backend repack into the first write). Our shape of the same
tax: GfxSpTri1 appended a VARIABLE-stride vertex to mBufVbo (pos + uv + per-vertex
clamp limits + fog rgba + grayscale + EVERY combiner input, ~9-28 floats), then
DrawTriangles (port/3ds/gfx/gfx_citro3d.cpp) re-walked the whole batch repacking it
into the fixed 12-float PICA layout in the linearAlloc pool — every vertex copied
twice with per-field branches and fminf clamps, the per-vertex work for
constant-spill inputs (prim/env/key/convert) computed then discarded down to a
single vertex-0 read, plus a possible per-vertex fog re-scan in UpdateFogState.
Now (3DS only, `__3DS__`-gated; desktop keeps the two-stage path byte-identical):
the batch's FIRST tri append asks the backend for pack parameters + a write cursor
into the linear VBO pool (`gdx3ds_vbopack_begin`, contract in
port/3ds/gfx/gdx3ds_vbopack.h), and every vertex is then written ONCE, in final
PICA layout, straight into the pool: UV pow2-padding scale and BOTH clamp families
(shader-clamp limits and the sky-wedge content-edge clamp) applied at append time,
only the vtxColorInput evaluated per vertex, out[9] resolved per the repack's
fog-factor/vertex-alpha/opaque policy. Flush() submits repack-free
(`gdx3ds_vbopack_draw`) through the same DrawTriangles tail (binds, stage
constants, alpha test, fog, stereo loop). The values are the legacy
append-then-repack expressions in the same float op order, so the emitted VBO is
bit-identical.
What the fixed layout no longer carries rides a per-batch aux record written from
the first vertex's state: every combiner input's vertex-0 RGBA (consumed by the new
RefreshStageConstantsPacked instead of variable-layout vertex-0 reads), the
draw-constant fog rgb and the vertex-0 fog factor (UpdateFogState's exact=0
fallback becomes a=0, b=factor — the legacy secant through a constant series,
no scan). Soundness is the flush invariant this renderer already enforces
everywhere (s7 memo, prim/env constant hoist, fog latch): no state feeding pack
parameters or the aux record can change mid-batch without a Flush() splitting it,
so batch-begin latching equals the legacy draw-time derivation.
Fallback/killswitch: `gdx3ds_vbopack_begin` refuses (whole batch takes the legacy
path, which stays fully compiled) when [debug] triloop=0 / env GDX_TRILOOP_OFF, no
shader/frame, the pool cannot fit a full 256-tri batch, or
`gdx_s7_geo_diag_enabled` is armed (the verbose per-vertex UV/geometry diagnostics
live in the legacy loop and keep full fidelity). Receipt: `dPk=` on the [c3d] line
counts packed (repack-free) draws per 64-frame window; optmask bit4. Port-side
counterpart committed directly (gdx3ds_vbopack.h, gfx_citro3d.cpp/.h, the lus_carve
include-dir line in port/3ds/gfx/CMakeLists.txt).
Applied after `lus-crowd2-tilestate-value-gate.patch`, closing the libultraship series.

## lus-tmem2-tmemfast.patch (LOCKED-60 Task B — LOADBLOCK/TMEM bookkeeping)
Hardware profile: `F3 loadblock` = 1.9-2.4 ms per crowd frame at 144-199 calls (~12 us each)
although the same-content skip already avoids the copy for repeated content. Step 1 adds
`[tmem2]` per-phase timers inside GfxDpLoadBlock / GfxDpLoadTile / StoreLoadedTexture
(derive, path lookup, same-content compare, TMEM mirror memcpy, invalidation walk, span
record, note/arm) — tick accumulation only under the latched gputrace gate, counters always —
drained by the bridge on the `[race-dl]` cadence. The breakdown (docs/research/
tmem2-progress.md): ~50 % of the handler is StoreLoadedTexture's per-WORD overlap
invalidation walk, run for every load even though a crowd load replaces exactly one span whose
only fresh slot is its base, and the record step rewrites the same words right after. Step 2
(`[debug] tmemfast=1`, default on) adds `GdxTmem2FastTeardown`: the spans overlapping the new
range are found through a small live-span list (`RDP::tmem_span_list`, kept by both store
paths, rebuilt from the map on overflow) instead of the per-word map; when none has a fresh
interior view (new `RDP::tmem_span_fresh` counter, maintained by MaterializeTmemSlot / the
legacy walk) and every base record still describes its span, the teardown releases the fresh
bases, drops the base-indexed records and unmaps only the words outside the new range — no
TMEM word is visited; a same-range replacement also skips the per-word span_base fill and
stale stamp. Fresh interior views or a detached/divergent base fall through to the legacy
walk. Receipt `fast=hit/miss miss=int/div/list` on the `[tmem2]` line. Killswitch mirror `gdx_tmem2_fast_enabled` ([debug] tmemfast on 3DS,
latched per gfx task by the bridge; GDX_TMEMFAST env on desktop); off = the legacy walk.
Applied after `lus-3ds-triloop-packed-vbo.patch`, closing the libultraship series.
## lus-trect-census.patch (LOCKED-60 Task A — [trect] texrect-run census)

Diagnostic only, generated as a pure delta on the full stack above (applied last of the
libultraship series). Adds the `[trect]`/`[trect2]` census: every G_TEXRECT is classified
against the previous rect of its RUN (rects with no game triangle / task boundary between)
as identical-state, texture-only delta (atlas candidate; clamp/UV-inside sub-counts) or
other delta (first differing group: om/cc/pe/sc/tl), with batch-break attribution (Flush
with a non-empty VBO between rects `fBtw` vs inside the rect `fIn`), per-run distinct
texture histogram, and the per-frame ms split rect = imp + draw + rest. Active only under
gputrace (drained on the [prof] window by gdx3ds_gpu_prof.c) or the bridge-mirrored
verbose gate ([race-dl] cadence); the always-on residue is two counter increments.

## lus-trectbatch-atlas.patch (LOCKED-60 Task A — HUD atlas + same-page rect batch merge)

Pure delta on the stack WITH lus-trect-census.patch (closes the libultraship series). The
census showed same-state rects already share a batch and that EVERY texture switch inside a
HUD run costs a batch break (one DrawTriangles + one import lookup, ~38 us in Azahar): 18/frame
steady, 113-138/frame on the race-start crowd and rankings frames that own [profop] E4 = 3.9-8.9
ms on hardware. Port side (gfx_citro3d.cpp/.h, gdx3ds_vbopack.h): small clamp-addressed rect
textures (<= 320x64: HUD digits and the 304x3 / 160x6 gradient strips) are placed as VIEWS into
shared 512x256 RGBA8 atlas pages (shelf-packed, 1-texel
replicated gutter, per-view UV affine scale+offset, page refcount resets a page when its last
view id is recycled; up to 8 pages, fallback to standalone when full). This patch: (1) a rect
whose tile is clamp/mask-0 on both axes, unmirrored, with UVs inside the tile arms the atlas
around its ImportTexture; (2) rect draws import BEFORE the flush and keep the open PACKED
batch when the resolved texture is a view on the page already bound (pack params refreshed
for the new offset, sampler state inherited); any other outcome rebinds the previous id
around the Flush so the pending batch draws with its own texture; (3) the packed vertex write
adds the view offset AFTER the clamp (legacy expression kept verbatim when the batch has no
view). Receipts on the [trect2] line: tb=on/merged/switched/bypassed/armed vt=<non-rect
draws that resolved to a view, diagnostic> atlas=placed/full/pages/resets. Killswitch
[debug] trectbatch=0 (env GDX_TRECTBATCH_OFF): byte-identical legacy order, no arming.
