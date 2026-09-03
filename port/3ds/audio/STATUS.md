# Stream C — audio (ndsp, HLE-primary)

- Done:
  - `gdx3ds_audio_ndsp.c` — full frozen-contract backend: ndsp channel 0, 32000 Hz stereo
    PCM16, LightLock-guarded 4096-frame default ring (drop-oldest, non-blocking push),
    3 × 512-frame linearAlloc `ndspWaveBuf` triple-buffer with `DSP_FlushDataCache`,
    `gdx3ds_audio_buffered()` = ring + in-flight frames.
  - Drain thread + core ladder: New3DS core 2 → core 1 (`APT_SetAppCpuTimeLimit(30)`,
    Luma >= 10.1.1) → core 0; `osSetSpeedupEnable(true)` on New3DS. Diagnostic exports
    (`gdx3ds_audio_output_active/drain_core/dropped_frames/underrun_chunks` — additive,
    not in the frozen header).
  - `ndspInit` failure (no `sdmc:/3ds/dspfirm.cdc` on real HW) handled: logs and degrades
    to a real-time-paced null sink; pacing semantics preserved, game runs silent.
  - CMake `if(NINTENDO_3DS)` switch keeps `gdx3ds_audio_stub.c` on host. Both builds
    verified green (devkitARM `.3dsx` + host).
  - `AUDIO_NOTES.md`: HLE-primary rationale, A/B plan vs `port/gdx_audio_capture.h`
    captures, measured-budget TODO list.
- Blocked on:
  - Stream E landing the hook requests below (nothing plays until then).
  - Streams B/D scripted boot for the on-device leg of the A/B plan (PC legs unblocked).
  - Real-hardware / Citra runtime measurement (budget TODOs in AUDIO_NOTES.md §4).
- Next: PC-side A/B reference captures (AUDIO_NOTES.md §3 steps 1-2); chunk/ring-size
  tuning once integration boots; surface `gdx3ds_audio_drain_core()` in the boot log.

## Hook requests for stream E — `port/gdx_audio_thread.cpp` (exact sites)

Where the 3DS backend replaces the SDL/LUS AudioPlayer path. All guarded
`#ifdef GDX_PLATFORM_3DS` (or the equivalent macro E standardizes); desktop behavior
byte-identical when the guard is off.

- **R1 — line 53** `#include "libultraship/bridge/audiobridge.h"`:
  on 3DS include `"gdx3ds_audio.h"` instead (LUS AudioPlayer/audiobridge does not exist
  in the carved build).
- **R2 — lines 217-218** (startup log arg `AudioPlayerGetDesiredBuffered()`) and
  **line 232** `const int32_t desired = AudioPlayerGetDesiredBuffered();`:
  on 3DS use a constant, suggested
  `constexpr int32_t kGdx3dsDesiredBufferedFrames = 2048;` — matches the desktop os.cpp
  2048-frame `osAiGetLength` cushion and leaves half the backend's 4096-frame default
  ring as headroom.
- **R3 — line 234** `while (AudioPlayerBuffered() < desired && ...)`:
  on 3DS call `(int32_t)gdx3ds_audio_buffered()`.
- **R4 (informational, no edit requested)** — lines 55-63 / 73-77 / 268 / 289-291
  (`<thread>`/`<mutex>`/`<condition_variable>` includes, `std::recursive_mutex`,
  `std::thread` spawn/join): verified to COMPILE under devkitARM (gcc threads=dkp,
  gthreads backed by libctru). Keep std::thread for the production thread at MVP — it
  lands on the default core/priority, which is acceptable because the spare-core
  placement is handled by this stream's drain thread inside `port/3ds/audio/`. If Phase 2
  profiling shows the production thread needs explicit core/priority placement, C will
  file a follow-up for a `threadCreate` seam here.

## Hook request for stream E — outside `gdx_audio_thread.cpp`

- **R5 — `osAiSetNextBuffer` routing**: the produced PCM leaves the decomp at
  `decomp/src/audio/disk/lib/thread.c:87-96` (the capture-tap site) via
  `osAiSetNextBuffer`, currently satisfied by libultraship's audiobridge → SDL
  AudioPlayer. On 3DS route it to
  `gdx3ds_audio_push((const int16_t*)buf, sizeBytes / 4)` (4 bytes per stereo frame);
  the return value can be ignored — overrun policy is drop-oldest inside the backend.
  Where the ifdef lives (LUS carve shim vs `port/shims.c` vs thread.c) is E's/the
  orchestrator's call; the backend only needs the call to arrive.
- **R6 (integration note, owner: B/Phase 0, not E)** — `port/3ds/main_3ds.cpp` already
  maps "audio device init" to `gdx3ds_audio_init(0)`; call it before
  `gdx_audio_thread_start()` and `gdx3ds_audio_shutdown()` after
  `gdx_audio_thread_stop()`.
