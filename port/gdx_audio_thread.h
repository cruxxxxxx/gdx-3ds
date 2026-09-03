// Dedicated audio production thread. A real std::thread owns per-tick audio synthesis, so a long
// synchronous game-thread load (course/segment asset loads) no longer starves audio for its whole
// duration. See gdx_audio_thread.cpp for the cross-thread touchpoints, the mutex boundary, and
// the kill-switch semantics.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Resolves the kill switch (GDX_AUDIO_THREAD, default ON unless "0"; --audio-thread /
// --no-audio-thread override it) and, if enabled, starts the thread. Call once from
// port/main.cpp AFTER ctx->InitAudio(), so the AudioPlayer backend exists. Safe to call before
// bootproc()/Audio_Init(): the thread's loop waits for gAudioContextInitialized.
void gdx_audio_thread_start(int argc, char** argv);

// Signals the audio thread to stop and joins it. Call once, after the frame loop exits and
// before process teardown. No-op if the thread was never started (kill switch OFF).
void gdx_audio_thread_stop(void);

// Wakes the audio thread immediately instead of waiting for its 5ms self-pump timeout. Call
// once per rendered host frame (port/main.cpp's frame loop, alongside gdx_vi_tick()). No-op if
// the dedicated thread isn't active this run.
void gdx_audio_thread_notify_frame(void);

// True if the dedicated audio thread is this run's active producer (kill switch ON).
//   - sys_audio.c's Audio_ThreadEntry gates its own legacy production call on this, so exactly
//     one producer ever touches gAudioCtx's task-creation state.
//   - libultraship's os.cpp osAiGetLength() gates the old under-report cushion on it, which is
//     only needed on the legacy fiber path.
int gdx_audio_thread_active(void);

// Guards the threadCmdBuf handoff between the game thread (AudioThread_QueueCmd in
// decomp/src/audio/disk/lib/thread.c) and the audio thread (AudioThread_CreateTask's drain).
// RECURSIVE: a command handler inside the mutexed drain may re-enter QueueCmd, which a plain
// mutex would self-deadlock on. C linkage so thread.c can take it without pulling in <mutex>.
// See gdx_audio_thread.cpp's MUTEX BOUNDARY comment.
void gdx_audio_ctx_lock(void);
void gdx_audio_ctx_unlock(void);

// Lock-free ring carrying gAudioCtx.threadCmdProcQueue's (readPos<<8 | writePos) tokens between
// AudioThread_ScheduleProcessCmds and AudioThread_CreateTaskImpl's drain, replacing that queue's
// osSendMesg/osRecvMesg so the audio thread never touches libultra's run queue or waiter lists
// (see gdx_audio_thread.cpp's CmdRing comment). Both keep the OS_MESG_NOBLOCK return convention:
// 0 = success, -1 = full (push) / empty (pop).
int gdx_audio_cmdring_push(unsigned int token);
int gdx_audio_cmdring_pop(unsigned int* out);

// Replaces AudioThread_CreateTaskImpl's per-tick osSendMesg to gAudioCtx.taskStartQueue. That
// queue has NO consumer in this port (AudioThread_WaitForAudioTask is never called), and the send
// was the last decomp osSendMesg this thread still executed every tick. Now a plain atomic counter
// touching zero libultra scheduler state. The accessors are diagnostic only.
void gdx_audio_taskstart_post(unsigned int token);
unsigned int gdx_audio_taskstart_count(void);
unsigned int gdx_audio_taskstart_last_token(void);

#ifdef __cplusplus
}
#endif
