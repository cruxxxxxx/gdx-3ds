# 3DS Memory Budget — Static Table (Stream F)

**Scope**: static budget of the planned 3DS build vs the 3DS application-region reality.
Numbers are code-verified where a citation is given; estimates are marked EST. The M2
heap-high-water measurement under Citra (plan §6 tripwire) is the runtime follow-up.

---

## 1. The 3DS side: what an app actually gets (verified)

FCRAM is 128 MB (old3DS) / 256 MB (New3DS). The APPLICATION memregion an app can commit
is set by APPMEMTYPE (3dbrew, *Memory layout* — fetched 2026-08-12,
https://www.3dbrew.org/wiki/Memory_layout):

| Console | APPMEMTYPE | APPLICATION region |
|---|---|---|
| old3DS | 0 (default) | **64 MB** (0x0400_0000) |
| old3DS | 4 / 3 / 2 (extended) | 72 / 80 / **96 MB** |
| old3DS | 5 | 32 MB |
| New3DS | 6 (default) | **124 MB** (0x07C0_0000) |
| New3DS | 7 (extended) | **178 MB** (0x0B20_0000) |

BASE region is a constant 20 MB; SYSTEM gets the remainder. A `.3dsx` under the Homebrew
Launcher inherits the *host* title's memory mode, so plan against **64 MB (old3DS) /
124 MB (New3DS)** and treat 96/178 MB as opt-in packaging decisions (`.cia` exheader),
not defaults.

**Both the malloc heap and `linearAlloc` come out of the same commit budget**: libctru's
`allocateHeaps.c` queries `svcGetResourceLimit*`(`RESLIMIT_COMMIT`) and splits the
remainder between `__ctru_heap_size` and `__ctru_linear_heap_size` (linear heap capped
at 32 MB by default; both globals overridable at build time)
(https://github.com/devkitPro/libctru — `libctru/source/system/allocateHeaps.c`,
fetched 2026-08-12). GPU-visible allocations (VBOs, textures, ndsp wave buffers) must be
`linearAlloc`; VRAM is a separate 6 MB and is best reserved for color/depth targets.

## 2. The G-Diffuser side: static budget table

| Item | Size | Basis |
|---|---|---|
| `gdx_rdram` arena | **16 MB** fixed | `port/n64_rdram.h:11` (`GDX_RDRAM_SIZE = 0x1000000`); includes the 1 MB O2R staging carve (`n64_rdram.h:19`, `port/decomp_port.c:99-114`) and the GfxPool alias — overlay textures already reach ~12-13 MB physical inside it (research doc §2) |
| Code + rodata + data (.3dsx image) | **~12-16 MB (EST)** | Spike compiled 38 LUS-core `.o` files = 13 MB total under devkitARM GCC 16.1 (`spike-lus-carve/obj/`; biggest: `Archive.o` 2.1 MB, `ResourceLoader.o` 2.1 MB, `interpreter.o` 1.6 MB, `ResourceManager.o` 1.5 MB, `JsonFactory.o` 1.4 MB — `docs/research/spike-lus-carve-report.md:15-16`). Object size ≫ linked size (ELF/section overhead, COMDAT dedup, `--gc-sections`), but decomp game + ~10k-line bridge + zlib/libzip aren't in that 13 MB. Range until first real link: 8-16 MB; **budget 16 MB**. Measure at Phase-2 first link — this is the softest number here |
| LUS resource-manager cache | **UNBOUNDED — must be capped. Budget 24 MB** | `libultraship/include/ship/resource/ResourceManager.h:421-423`: cache is a plain `std::unordered_map<ResourceIdentifier, …shared_ptr<IResource>>` with **no size cap, no LRU, no eviction policy** — only explicit `UnloadResource()`/dirty-flag paths. On desktop it grows for the session. 3DS action item: add a byte-accounted cap (~24 MB) or flush at mode transitions (the game's own segment-carve swap points, `docs/ARCHITECTURE.md:282-288`, are natural flush boundaries) |
| O2R archive read path | **~2 MB transient (EST)** | libzip keeps the archive handle open for the archive's lifetime; each file read heap-allocates a `File::Buffer` (`shared_ptr<vector<uint8_t>>`) sized to the entry (`port/AssetLoader.cpp:26-133` → LUS archive backends). Worst single entry is the segment-8 `course_track_gfx` blob (133.95 ms PC decode, research doc §2). Decompression staging into RDRAM uses the fixed 1 MB carve, already counted above |
| linearAlloc pools (stream A plan) | **~20 MB: 1 MB VBO + 16 MB texture pool + fog LUTs + misc (EST)** | Plan §3.A. Texture ceiling anchored by the ~12-13 MB physical overlay-texture population in RDRAM (RGBA16 → native RGB5A1 is byte-for-byte comparable; some RGBA8 promotions). Exhaustion-logging fixed pools per sm64-3ds precedent |
| ndsp wave buffers + audio rings | **< 1 MB** | 32 kHz stereo s16; existing buffer CVar default 4096 frames (`port/main.cpp:871`) = 16 KB per buffer; a ring of a few + ndsp channel state |
| Thread/fiber stacks | **~1 MB after fixing defaults — currently ~7 MB** | `port/n64_sched.c:44` allows 32 fibers; **`n64_sched.c:223` creates every fiber with the default 1 MB stack**. The game actually creates 7 fibers (Idle 0x200, Main 0x400, Game 0x1000, Audio 0x800, Sys6 0x1000, Reset 0x1200 — `decomp/src/sys/sys_main.c:20-27,295-473`; Fault 0x800 `sys_fault.c:315`) = 7 MB at current defaults for stacks the N64 sized in KB. **Action item (stream B): pass explicit 64-128 KB stacks on 3DS.** Plus the audio producer `std::thread` (`port/gdx_audio_thread.cpp`, OS-default stack) and ndsp's own thread |
| 64DD `.ndd` (~64.45 MB) | **0 MB resident — must stream** | Plan §3.D already mandates SD streaming; full-buffering (`port/disk_buffer.cpp` desktop behavior) alone exceeds the entire old3DS region. EK is post-MVP |
| OS/CRT/misc slack | **8 MB (EST)** | malloc fragmentation, spdlog-stub, libctru services, safety margin |

**Static total: ~71-87 MB (center ≈ 80 MB), of which ~21 MB must be linearAlloc.**
VRAM (separate 6 MB): top-screen color ×2 (400×240×4 ≈ 768 KB) + depth + bottom screen
≈ 2-2.5 MB — fits; stereo doubles color targets and still fits.

## 3. Verdict per console

| Console | App region | Verdict |
|---|---|---|
| **New3DS (primary target)** | 124 MB default | **FITS with ~40-50 MB headroom** at the ~80 MB static center — *provided* the resource cache is capped (unbounded today) and fiber stacks are right-sized (7 MB → 1 MB). Extended 178 MB mode is available as `.cia`-time insurance, not needed on paper |
| **old3DS, default mode** | 64 MB | **DOES NOT FIT** as planned: RDRAM 16 + code ~16 + linear ~20 + cache 24 + slack ≈ 84 MB > 64. Fixed floor (RDRAM + code + minimum textures) alone is ~45-50 MB, leaving < 15 MB for cache + everything else |
| **old3DS, 80/96 MB extended** | 80-96 MB | **MARGINAL** — reachable only with deep cuts (cache ≤ 8 MB, texture pool ≤ 10 MB, code trimming), and the CPU verdict (268 MHz, separate stream-F report) is expected to kill old3DS regardless. Recommendation: **formally drop old3DS after M3**, per plan §7 |

## 4. Tripwire recommendation (M2)

Land the heap high-water instrumentation **with the M2 milestone build**, not after:

1. A per-second (or per-mode-transition) log line under Citra and hardware:
   `mallinfo()` arena high-water, `linearSpaceFree()`, resource-cache entry count ×
   accounted bytes (requires adding byte accounting to `ResourceManager` — it has none),
   and texture-pool occupancy from stream A's exhaustion counters.
2. Red line: total committed > **100 MB** on the New3DS profile at M2 (1 track,
   1 machine) ⇒ stop and shrink before M3 (30 machines) multiplies texture/resource
   pressure. Amber line: resource cache alone > 24 MB ⇒ implement the cap immediately.
3. Cheapest lever if tripped, in order: cap/flush resource cache at segment-carve
   transitions → shrink texture pool + evict-on-miss → drop the boot prewarm of
   segment 8 → shrink RDRAM staging carve last (it's load-bearing for texture decode).

---
*Stream F static budget, 2026-08-12. Citations: local tree as noted; 3dbrew Memory
layout (https://www.3dbrew.org/wiki/Memory_layout); devkitPro libctru
`source/system/allocateHeaps.c` (https://github.com/devkitPro/libctru).*
