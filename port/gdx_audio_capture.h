/* Streaming PCM capture for the bit-identical audio gate. The tap sits at
 * decomp/src/audio/disk/lib/thread.c:87-96, immediately before AudioThread_CreateTaskImpl's
 * osAiSetNextBuffer -- upstream of ALL host post-processing (low-pass, volume, underrun fade).
 * gdx_audio_lle.c's fixed 64000-frame RAM ring still runs alongside; this module is the streaming
 * replacement the PCM-parity harness uses, appending raw interleaved s16 stereo straight to
 * <prefix>.pcm and, on finalize, writing a SHA-256 sidecar the harness compares run to run.
 *
 * FORMAT of <prefix>.pcm: headerless, interleaved signed 16-bit little-endian stereo
 * (L,R,L,R,...). One "frame" is one L+R pair = 4 bytes. Sample rate is 32000 Hz on this title but
 * is taken from the tap per call. The bytes are host-native little-endian s16 exactly as the tap
 * delivers them (the AI output buffer), so no conversion happens anywhere.
 *
 * ENV CONTROL:
 *   GDX_PCM_CAPTURE          output path PREFIX. Unset -> every call is a no-op, so normal play is
 *                            unaffected. Set -> armed at gdx_pcm_capture_init().
 *   GDX_PCM_CAPTURE_FRAMES   optional frame cap. When >0 the window auto-finalizes after that many
 *                            frames, giving a capture length in frames rather than wall-clock.
 *                            Unset/0 -> unbounded until gdx_pcm_capture_shutdown().
 *
 * THREADING: feed()/active() run on the audio thread (the tap); init/arm/shutdown/finished run on
 * the host/main thread. The handful of cross-thread flags are plain aligned ints, read and written
 * atomically on this target -- the benign-race pattern used elsewhere in the port (see
 * port_log.h). No lock is taken on the audio-tick fast path.
 */
#ifndef GDX_AUDIO_CAPTURE_H
#define GDX_AUDIO_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse the env vars once and, when configured, arm the window (opens <prefix>.pcm). Call once
   at boot, BEFORE the audio thread produces its first tick: the RNG determinism pin below needs
   gdx_pcm_capture_active() to already be true on that tick. */
void gdx_pcm_capture_init(void);

/* Arm the capture window explicitly. Idempotent; a no-op when unconfigured or already
   finalized. gdx_pcm_capture_init() calls this for you when capture is configured. */
void gdx_pcm_capture_arm(void);

/* Append interleaved s16 stereo frames (frameCount = L+R pairs). A no-op unless configured AND
   already armed -- deliberately fail-closed, since auto-arming here would run off the main thread
   and hide a violation of the threading contract above. Finalizes on reaching the frame cap. */
void gdx_pcm_capture_feed(const int16_t* frames, unsigned int frameCount, unsigned int sampleRate);

/* 1 while a capture window is armed and not yet finalized. Gates the deterministic-RNG
   substitution at thread.c:205-226 (the osGetCount() pin in AudioThread_CreateTaskImpl); 0 in all
   normal play, so that site keeps its original hardware-entropy expression. */
int gdx_pcm_capture_active(void);

/* 1 once the capture window has finalized (frame cap reached, or shutdown). Polled by the
   host loop to trigger auto-exit. Always 0 when capture is unconfigured. */
int gdx_pcm_capture_finished(void);

/* Finalize an in-progress capture: close <prefix>.pcm and write <prefix>.pcm.sha256. Called
   at host shutdown so an unbounded capture still emits its digest. A no-op when unconfigured
   or already finalized. */
void gdx_pcm_capture_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_AUDIO_CAPTURE_H */
