/* port/3ds/vendor/citro3d/gdx/c3d_gdx.h -- GDX (c5submit): the P3a submission diet's
 * control surface, for the port (docs/research/native/c5submit-progress.md).
 *
 * The vendored citro3d keeps its stock code paths verbatim. When C3Dgdx_mode is 0 (the
 * default), C3D_DrawElements / C3D_DrawArrays run exactly the stock code. When it is not,
 * the draw goes through source/gdx_submit.c, which can:
 *
 *   C3DGDX_DIET_UNIFSCAN  scan the float-uniform dirty arrays a word (4 flags) at a time
 *                         instead of a byte at a time, for the vertex AND the geometry
 *                         shader, so the never-bound GS costs a 24-word OR instead of a
 *                         96-step loop. Same runs, same order: byte-identical.
 *   C3DGDX_DIET_EMIT      the draw prologue/epilogue and every single-parameter register
 *                         write of the context update written straight into gpuCmdBuf
 *                         (one bounds check per packet group, no GPUCMD_Add call chain),
 *                         with GPUCMD_AddInternal's exact word layout: byte-identical.
 *   C3DGDX_DIET_FLAGSKIP  one test of the dirty flags against every flag the context update
 *                         handles, skipping the ~18 individual flag tests when none is set
 *                         (and only then): byte-identical.
 *   C3DGDX_DIET_ELIDE     NOT byte-identical, not in C3DGDX_DIET_ALL: drop vertex-shader float
 *                         uniform registers whose value the GPU already holds (a shadow of the
 *                         last value this library uploaded to each register). The GPU state at
 *                         every draw is the same; the command words are fewer. Needs UNIFSCAN.
 *                         Anything that writes VS float uniforms behind citro3d's back (a replayed
 *                         or patched command buffer, raw packets) must call
 *                         C3Dgdx_InvalidateUniformShadow() afterwards, or this is wrong.
 *
 * C3DGDX_VERIFY runs every draw twice from the same citro3d state -- the stock path, then the
 * diet -- and compares the command words each emitted AND the citro3d state each left behind
 * (context, dirty flags, the uniform module's statics). The stock result is what stays in the
 * command buffer. With ELIDE the words differ by design, so the compare is semantic: every packet
 * that is not a VS float-uniform write must match word for word and in order, and the VS float
 * uniform register file (modelled from the shadow as the state before the draw) must be the same
 * at the draw trigger and at the end. C3DGDX_VERIFY_SELFTEST corrupts one diet word every 997th
 * draw so the detector itself is tested.
 *
 * C3DGDX_PROF_SPLIT times the stock path per section (tick reads around each section of a
 * verbatim copy of C3Di_UpdateContext); C3DGDX_PROF_TOTAL times each draw call as a whole (one
 * read pair), stock or diet, so the two can be compared on one build.
 *
 * The command-buffer layout of uniform writes is unchanged by every bit except ELIDE: a
 * FLOATUNIFORM_CONFIG write (0x80000000|index) then one FLOATUNIFORM_DATA packet per dirty run,
 * split at 256 words exactly as GPUCMD_Add does. ELIDE keeps that packet shape but sends fewer
 * or shorter runs (equal registers at a run's ends, or 2+ equal registers inside it, are cut). */
#ifndef C3D_GDX_H
#define C3D_GDX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	C3DGDX_DIET_UNIFSCAN = 1u << 0,
	C3DGDX_DIET_EMIT     = 1u << 1,
	C3DGDX_DIET_FLAGSKIP = 1u << 2,
	C3DGDX_DIET_ALL      = 7u,   // the byte-identical diet
	C3DGDX_DIET_ELIDE    = 1u << 3,
	C3DGDX_DIET_MASK     = 0xFu,

	C3DGDX_VERIFY        = 1u << 8,
	C3DGDX_PROF_SPLIT    = 1u << 9,
	C3DGDX_PROF_TOTAL    = 1u << 10,
	C3DGDX_VERIFY_SELFTEST = 1u << 11,
};

/* Section ids of C3DGDX_PROF_SPLIT (C3Dgdx_Stats.secTicks / secFired / secWords). */
enum {
	C3DGDX_SEC_FB = 0,     /* framebuffer, viewport, scissor */
	C3DGDX_SEC_PROGRAM,    /* shaderProgramConfigure (program switch) */
	C3DGDX_SEC_ATTR,       /* attribute info */
	C3DGDX_SEC_BUF,        /* buffer info */
	C3DGDX_SEC_EFFECT,     /* depth/stencil/blend/alpha/cull */
	C3DGDX_SEC_TEX,        /* texture units + texture status */
	C3DGDX_SEC_TEXENVBUF,  /* procedural texture, combiner buffer, fog LUT, gas */
	C3DGDX_SEC_TEXENV,     /* the six combiner stages */
	C3DGDX_SEC_LIGHTFIX,   /* light environment + fixed attributes */
	C3DGDX_SEC_VSUNIF,     /* C3D_UpdateUniforms(GPU_VERTEX_SHADER) */
	C3DGDX_SEC_GSUNIF,     /* C3D_UpdateUniforms(GPU_GEOMETRY_SHADER) */
	C3DGDX_SEC_DRAW,       /* the draw call's own prologue/trigger/epilogue */
	C3DGDX_SEC_COUNT
};

typedef struct {
	uint32_t draws;          /* draw calls that took the gdx path */
	uint32_t drawsStock;     /* ...of which ran the stock code (mode without any diet bit) */
	uint64_t totTicks;       /* PROF_TOTAL: ticks inside the draw calls, read pair included */
	uint32_t totDraws;
	uint64_t secTicks[C3DGDX_SEC_COUNT]; /* PROF_SPLIT: per section, one read included */
	uint32_t secFired[C3DGDX_SEC_COUNT]; /* draws in which the section emitted words */
	uint32_t secWords[C3DGDX_SEC_COUNT]; /* words the section emitted */
	uint32_t splitDraws;
	uint32_t words;          /* command words emitted by the draw calls (all modes) */
	uint32_t unifVec4;       /* float-uniform vec4s uploaded (VS) */
	uint32_t unifSame;       /* ...whose value equals the last upload of that register */
	uint32_t unifElided;     /* ELIDE: VS float-uniform vec4s not sent (the GPU held them) */
	uint32_t verified;       /* VERIFY: draws compared */
	uint32_t mismatchWords;  /* VERIFY: draws whose command words differ */
	uint32_t mismatchState;  /* VERIFY: draws whose post-draw citro3d state differs */
	uint32_t unverifiable;   /* VERIFY: draws skipped (light env / procedural texture / gas bound,
	                            or a stream too long for the compare buffer) */
	uint32_t firstBadDraw;   /* VERIFY: index (1-based, since arming) of the first mismatch */
	uint32_t firstBadWord;   /* ...its first differing word index */
	uint32_t firstBadStock;  /* ...the stock word there */
	uint32_t firstBadDiet;   /* ...the diet word there */
} C3Dgdx_Stats;

/* 0 = stock citro3d (the default). Otherwise a mask of the bits above. */
extern uint32_t C3Dgdx_mode;

/* Ticks one svcGetSystemTick read costs, measured by C3Dgdx_Calibrate (for PROF_*). */
uint32_t C3Dgdx_Calibrate(void);

/* ELIDE: forget every value the shadow believes the GPU holds (see C3DGDX_DIET_ELIDE). */
void C3Dgdx_InvalidateUniformShadow(void);

/* GDX (n60core2): the NRR direct emitter ([native] nrr_direct) writes command words without
 * citro3d's context update. Before its first word of a hand-off it asks whether the next draw
 * would emit anything first (dirty context groups, uniforms: StatePending) and, if so, emits it
 * now exactly as that draw's context update would (FlushState: the diet's update when
 * C3Dgdx_mode carries diet bits, else stock C3Di_UpdateContext). */
int C3Dgdx_StatePending(void);
int C3Dgdx_UniformsPending(int shaderType); /* 0 = vertex, 1 = geometry (diagnostics) */
/* Save / load every piece of citro3d state a context update changes (the NRR verify's shadow run). */
unsigned C3Dgdx_StateBytes(void);
void C3Dgdx_StateSave(void* buf);
void C3Dgdx_StateLoad(const void* buf);
void C3Dgdx_FlushState(void);

/* Copies the counters out; reset != 0 zeroes them (not the arming-relative draw index). */
void C3Dgdx_GetStats(C3Dgdx_Stats* out, int reset);

/* n60hwdiag: the GX queue watch. With C3Dgdx_QueueWatchMs > 0 an INFINITE wait on the frame's GX
 * queue (C3D_FrameBegin's WaitAndClearQueue, the [overlap] drain) waits in slices of that many ms;
 * after an empty slice (the first 5, then every 15th) C3Dgdx_QueueStuckHook(where, queue, waited ms,
 * 0) runs (a durable crumb with the queue counters). The wait itself is never abandoned: libctru's
 * gxCmdQueueWait waits on its static isRunning flag, gxCmdQueueStop only requests a stop while a
 * command runs and gxCmdQueueClear svcBreaks on a running bound queue, so a wedged queue cannot be
 * stopped safely. Off (0, the default) every wait is stock gxCmdQueueWait. */
struct tag_gxCmdQueue_s;
extern int C3Dgdx_QueueWatchMs;
extern void (*C3Dgdx_QueueStuckHook)(const char* where, const struct tag_gxCmdQueue_s* q, unsigned waitedMs,
                                     int forcing);
bool C3Dgdx_QueueWait(struct tag_gxCmdQueue_s* q, long long timeout, const char* where);
/* n60hwdiag: around every context save/restore window (C3Dgdx_StateSave/Load, the diet snapshot):
 * out[0] windows, out[1] windows in which the GPU completed queue entries (each one a completion
 * the pre-0340435 whole-context restore rolled back: a lost completion), out[2] windows where the
 * queue changed otherwise (entries added or cleared inside the window). Cumulative. */
void C3Dgdx_QueueWindowStats(unsigned out[3]);
/* n60hwdiag [debug] gxq_selftest: the next watched wait reports two synthetic stuck slices after it
 * really finished (exercises the reporting path; test only). */
void C3Dgdx_QueueSelftestArm(void);

/* n60gp: the swap stamp of the pace swap gate ([debug] pace_swapgate, port/3ds/gfx/gdx3ds_pace.c).
 * onQueueFinish, right after it requests a top-screen swap (gfxScreenSwapBuffers: the GSP latches
 * it at the next VBlank0), records the VBlank0 counter of that moment and bumps a sequence number.
 * Written on the GSP event thread; C3Dgdx_SwapStampRead returns a consistent pair (seqlock). */
void C3Dgdx_StampTopSwap(unsigned vblank0);
/* out[0] = top swaps requested so far, out[1] = C3D_FrameCounter(0) at the latest one. */
void C3Dgdx_SwapStampRead(unsigned out[2]);
/* GDX (n60stereo): svcGetSystemTick() at the latest top swap request (the [pace] GPU-tail receipt). */
unsigned long long C3Dgdx_SwapStampTick(void);

/* GDX (n60tail, [native] sky_late, port/3ds/gfx/gdx3ds_skylate.cpp): the stencil words of every
 * effect bind while a mode is set. With C3Dgdx_stencilMode == 0 (the default) every bind writes the
 * effect's own stencil words (stock). Otherwise C3Di_EffectBind, the diet's DietEffectBind and the
 * NRR direct emitter write C3Dgdx_stencilWords[0..1] (GPUREG_STENCIL_TEST, GPUREG_STENCIL_OP) in
 * their place, and C3Dgdx_StencilCheck ORs into C3Dgdx_stencilBad every reason the bound effect
 * would make a late sky inexact (C3DGDX_STB_*). Mode values: */
enum {
	C3DGDX_STENCIL_SKY = 1,    /* the sky: test EQUAL 0, writes nothing */
	C3DGDX_STENCIL_MARK = 2,   /* the opaque region the sky moves behind: test ALWAYS, zpass REPLACE 1 */
};
enum {
	C3DGDX_STB_BLEND = 1u << 0,   /* MARK: blending that reads the colour buffer (not ADD ONE ZERO) */
	C3DGDX_STB_LOGIC = 1u << 1,   /* MARK: a logic op other than COPY */
	C3DGDX_STB_MASK = 1u << 2,    /* MARK: a colour write mask other than RGBA */
	C3DGDX_STB_FRAGOP = 1u << 3,  /* MARK: a fragment operation mode other than the default */
	C3DGDX_STB_STENCIL = 1u << 4, /* either: the effect itself enables the stencil test */
	C3DGDX_STB_SKYZ = 1u << 5,    /* SKY: the effect may write depth */
};
extern uint32_t C3Dgdx_stencilMode;
extern uint32_t C3Dgdx_stencilWords[2];
extern uint32_t C3Dgdx_stencilCur;   /* C3DGDX_STB_* of the effect the GPU holds now (set at every bind) */
extern uint32_t C3Dgdx_stencilBad;   /* OR of stencilCur at every DRAW since the mode was set */
extern uint32_t C3Dgdx_stencilFirst[4]; /* the first effect that drew bad: fragOpMode, alphaBlend, logicOp, depth */
/* fx4 = the effect's four words from GPUREG_FRAGOP_ALPHA_TEST on (alpha test, stencil test, stencil
 * op, depth/colour mask); called only with a mode set: checks (into stencilCur), then overwrites
 * fx4[1..2]. A bound effect no draw uses (a state flush rebinding the previous one) costs nothing. */
void C3Dgdx_StencilCheck(uint32_t fragOpMode, uint32_t alphaBlend, uint32_t logicOp, uint32_t* fx4);
/* Every draw while a mode is set (C3D_DrawArrays / C3D_DrawElements, the NRR direct draws). */
void C3Dgdx_StencilDraw(void);

/* GDX (o3ds3d, port/3ds/gfx/gdx3ds_prewrite.c): read-only views for the [prewrite] / [gpumem]
 * receipts and the [debug] ceiling_probe. C3Dgdx_CmdBufWords: the size C3D_Init gave the command
 * buffer, in words (C3D_GetCmdBufUsage x this = the words of the frame so far). C3Dgdx_QueueEntries:
 * the entries queued on the frame's GX queue. C3Dgdx_FrameQueueIdle: outside a frame only, wait up to
 * `timeoutNs` for the previous frame's GX queue to run out WITHOUT clearing it (the wait
 * C3D_FrameBegin makes, moved earlier); false on a timeout or inside a frame. */
unsigned C3Dgdx_CmdBufWords(void);
unsigned C3Dgdx_QueueEntries(void);
bool C3Dgdx_FrameQueueIdle(long long timeoutNs);

#ifdef __cplusplus
}
#endif

#endif /* C3D_GDX_H */
