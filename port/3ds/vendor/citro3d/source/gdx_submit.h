// GDX (c5submit): internals of the P3a submission diet shared between gdx_submit.c, uniforms.c
// and the draw entry points. The control surface is gdx/c3d_gdx.h.
#pragma once
#include "internal.h"
#include <c3d/uniforms.h>
#include "../gdx/c3d_gdx.h"
#include "../gdx/c3d_gdx_scan.h"
#include <stdio.h>

// n3dsfix (docs/research/native/hw-n60-3d7-analysis.md): a hardware A/B (native.track_resident=0
// on top of [perf] preset=n60) proved this exact bounds check CAN fire on a real New3DS, at the
// title attract-demo's one full-30-machine-GP frame (demo 3, course Big Blue) -- previously it was
// an undiagnosed freeze with no crash.txt; with track_resident off it is this svcBreak instead. The
// break itself was already correct (it is GPUCMD_AddInternal's own unchecked-overflow reaction this
// wrapper replaces), but it carried no record of WHERE or BY HOW MUCH, so every future hit of it
// needed an addr2line pass against that exact build to say anything. This durable, synchronously
// SD-flushed line (bypassing the async log ring, so it survives even a freeze one line later) makes
// that diagnosis immediate from the card alone. Weak + null-checked: zero-cost before gdx3ds_filelog
// is initialized (early boot) or on a non-3DS build of this vendored source.
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern void gdx3ds_filelog_flush(void) __attribute__((weak));
// o3ds-cpucuts: the svcBreak follows at once, so the line must not wait for the async writer
// ([debug] filelog_flush_defer turns a race's gdx3ds_filelog_flush into a writer wake).
extern void gdx3ds_filelog_flush_sync(void) __attribute__((weak));
static inline void GdxCmdbufOverflowLog(const char* where, u32 words, u32 off, u32 size)
{
	if (gdx3ds_filelog_write == NULL) {
		return;
	}
	char buf[128];
	int n = snprintf(buf, sizeof(buf), "[cmdbuf] OVERFLOW %s words=%lu off=%lu size=%lu (panicking)\n",
	                  where, (unsigned long)words, (unsigned long)off, (unsigned long)size);
	if (n > 0) {
		gdx3ds_filelog_write(buf, (size_t)n);
		if (gdx3ds_filelog_flush_sync != NULL) {
			gdx3ds_filelog_flush_sync();
		} else if (gdx3ds_filelog_flush != NULL) {
			gdx3ds_filelog_flush();
		}
	}
}

// Everything C3D_UpdateUniforms reads and may change, for the verify mode's save/restore.
typedef struct
{
	bool fvDirty[2][C3D_FVUNIF_COUNT];
	bool ivDirty[2][C3D_IVUNIF_COUNT];
	bool boolDirty[2];
	u16 boolUnifs[2];
	bool fvEverDirty[2][C3D_FVUNIF_COUNT];
	bool ivEverDirty[2][C3D_IVUNIF_COUNT];
	bool shaderFVecDirty[2];
	int shaderFVecCount[2];
	float24Uniform_s* shaderFVecData[2];
} C3Di_GdxUnifState;

// ELIDE's shadow: the VS float-uniform values this library last uploaded, per register.
typedef struct
{
	C3D_FVec value[C3D_FVUNIF_COUNT];
	bool valid[C3D_FVUNIF_COUNT];
} C3Di_GdxUnifShadow;

void C3Di_GdxUpdateUniforms(GPU_SHADER_TYPE type, u32 mode);
void C3Di_GdxUnifSave(C3Di_GdxUnifState* s);
void C3Di_GdxUnifLoad(const C3Di_GdxUnifState* s);
void C3Di_GdxShadowSave(C3Di_GdxUnifShadow* s);
void C3Di_GdxShadowLoad(const C3Di_GdxUnifShadow* s);
u32 C3Di_GdxTakeElided(void);
bool C3Di_GdxUniformsPending(GPU_SHADER_TYPE type); // GDX (n60core2)

void C3Di_DrawElementsStock(GPU_Primitive_t primitive, int count, int type, const void* indices);
void C3Di_DrawArraysStock(GPU_Primitive_t primitive, int first, int size);
void C3Dgdx_DrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices);
void C3Dgdx_DrawArrays(GPU_Primitive_t primitive, int first, int size);

// Words appended to gpuCmdBuf, exactly as libctru's GPUCMD_AddInternal lays them out: first
// parameter, header with (count-1) in bits 20-27, the other parameters, and a zero pad word
// when count-1 is odd. The bounds test is GPUCMD_AddInternal's, per packet.
static inline u32* C3Dgdx_Reserve(u32 words)
{
	if (!gpuCmdBuf || gpuCmdBufOffset + words > gpuCmdBufSize)
	{
		GdxCmdbufOverflowLog("C3Dgdx_Reserve", words, gpuCmdBufOffset, gpuCmdBufSize);
		svcBreak(USERBREAK_PANIC); // Shouldn't happen (GPUCMD_AddInternal's own reaction).
	}
	u32* p = &gpuCmdBuf[gpuCmdBufOffset];
	gpuCmdBufOffset += words;
	return p;
}

static inline void C3Dgdx_Put1(u32 header, u32 param)
{
	u32* p = C3Dgdx_Reserve(2);
	p[0] = param;
	p[1] = header;
}

// One GPUCMD_AddInternal packet of 1..256 parameters.
static inline void C3Dgdx_PutChunk(u32 header, const u32* param, u32 count)
{
	u32 extra = count - 1;
	u32 words = extra + 2 + (extra & 1);
	if (!gpuCmdBuf || gpuCmdBufOffset + count + 1 > gpuCmdBufSize)
	{
		GdxCmdbufOverflowLog("C3Dgdx_PutChunk", count + 1, gpuCmdBufOffset, gpuCmdBufSize);
		svcBreak(USERBREAK_PANIC);
	}
	u32* p = &gpuCmdBuf[gpuCmdBufOffset];
	u32 i;
	p[0] = param[0];
	p[1] = header | ((extra & 0xff) << 20);
	for (i = 1; i < count; i ++)
	{
		p[1 + i] = param[i];
	}
	if (extra & 1)
	{
		p[1 + count] = 0;
	}
	gpuCmdBufOffset += words;
}

// GPUCMD_Add: chunks of at most 256 parameters, the register advanced for incremental writes.
static inline void C3Dgdx_PutN(u32 header, const u32* param, u32 count)
{
	if (!count)
	{
		count = 1;
	}
	while (count)
	{
		u32 chunk = count > 0x100 ? 0x100 : count;
		C3Dgdx_PutChunk(header, param, chunk);
		param += chunk;
		count -= chunk;
		if (header & BIT(31))
		{
			header += chunk;
		}
	}
}

// Single-parameter write: inline when emitInline, else the stock libctru call chain.
static inline void C3Dgdx_Write1(u32 emitInline, u32 header, u32 param)
{
	if (emitInline)
	{
		C3Dgdx_Put1(header, param);
	}
	else
	{
		GPUCMD_AddSingleParam(header, param);
	}
}

static inline void C3Dgdx_WriteN(u32 emitInline, u32 header, const u32* param, u32 count)
{
	if (emitInline)
	{
		C3Dgdx_PutN(header, param, count);
	}
	else
	{
		GPUCMD_Add(header, param, count);
	}
}

#define C3DGDX_HDR(reg, mask) GPUCMD_HEADER(0, (mask), (reg))
#define C3DGDX_HDR_INC(reg) GPUCMD_HEADER(1, 0xF, (reg))
