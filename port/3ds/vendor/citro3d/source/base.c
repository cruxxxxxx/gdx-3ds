#include "internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <c3d/base.h>
#include <c3d/effect.h>
#include <c3d/uniforms.h>
#include "../gdx/c3d_gdx.h" /* GDX (o3ds3d): C3Dgdx_CmdBufWords */

C3D_Context __C3D_Context;

static aptHookCookie hookCookie;

// n60lead (cmdbuf hwm receipt): a cheap, once-per-submission read of how close a model's citro3d
// command buffer runs to the size C3D_Init gave it (native/n3dsfix sized it x4 on New 3DS instead
// of x2; Old 3DS's margin is still an open question -- this receipt is how both get measured on
// hardware without tripping the gdx_submit.h/nrr_direct.cpp overflow path). Hooked from
// C3Di_SplitFrame below, the one place every frame's command list is actually submitted
// (GPUCMD_Split) -- never from inside a draw, so it costs nothing in the NRR direct/race hot path
// tools/native/nrr_size.sh measures. Logs "[cmdbuf] hwm words=.. size=.. pct=.." on every new
// high-water mark and otherwise at most once every 5s. Weak + null-checked, same as the overflow
// log: zero cost before gdx3ds_filelog is up or on a non-3DS build of this vendored source.
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern void gdx3ds_filelog_flush(void) __attribute__((weak));
// o3ds-cpucuts ([debug] filelog_flush_defer, port/3ds/include/gdx3ds_filelog.h): the 5 s line below
// flushed the log synchronously every time it printed -- hardware o3ds-3d@0c2668a, every one of the
// 1P race's 13-24 KiB / 38-691 ms [hitch] sync flushes. With the lever on, only a NEW record (the
// line an overflow diagnosis needs) flushes, and that one names itself for [hitch] src=.
extern int gdx3ds_filelog_flush_lever(void) __attribute__((weak));
extern void gdx3ds_filelog_flush_src(const char* src) __attribute__((weak));

static void Gdx3dsCmdbufHwmFlush(bool newRecord)
{
	if (gdx3ds_filelog_flush_lever != NULL && gdx3ds_filelog_flush_lever()) {
		if (!newRecord) {
			return;
		}
		if (gdx3ds_filelog_flush_src != NULL) {
			gdx3ds_filelog_flush_src("cmdbuf_hwm");
			return;
		}
	}
	if (gdx3ds_filelog_flush != NULL) {
		gdx3ds_filelog_flush();
	}
}

static u32 s_cmdbufHwmWords = 0;
// The capacity the record above was measured against -- GPUCMD_Split (libctru) carves each
// C3D_FrameSplit's segment out of the SAME physical allocation by sliding gpuCmdBuf forward and
// shrinking gpuCmdBufSize by the segment just split off, so "remaining capacity" legitimately
// differs split to split within one logical frame (stereo's two eyes, HUD, ...) -- it is exactly
// the denominator gdx_submit.h's own overflow check uses at that moment. Pairing it with the words
// that set the record (instead of whatever gpuCmdBufSize happens to be live at LOG time) keeps the
// printed pct the one that was actually true when that peak was reached.
static u32 s_cmdbufHwmSize = 0;
static u64 s_cmdbufHwmLastLogMs = 0;

static void Gdx3dsCmdbufHwmNote(u32 words, u32 size) {
	bool newRecord = words > s_cmdbufHwmWords;
	if (newRecord) {
		s_cmdbufHwmWords = words;
		s_cmdbufHwmSize = size;
	}
	u64 nowMs = osGetTime();
	if (!newRecord && (nowMs - s_cmdbufHwmLastLogMs) < 5000) {
		return;
	}
	s_cmdbufHwmLastLogMs = nowMs;
	if (gdx3ds_filelog_write == NULL) {
		return;
	}
	char buf[96];
	double pct = s_cmdbufHwmSize ? (100.0 * (double)s_cmdbufHwmWords / (double)s_cmdbufHwmSize) : 0.0;
	int n = snprintf(buf, sizeof(buf), "[cmdbuf] hwm words=%lu size=%lu pct=%.1f%s\n",
	                 (unsigned long)s_cmdbufHwmWords, (unsigned long)s_cmdbufHwmSize, pct,
	                 newRecord ? " (new record)" : "");
	if (n > 0) {
		gdx3ds_filelog_write(buf, (size_t)n);
		Gdx3dsCmdbufHwmFlush(newRecord);
	}
}

__attribute__((weak)) void C3Di_LightEnvUpdate(C3D_LightEnv* env)
{
	(void)env;
}

__attribute__((weak)) void C3Di_LightEnvDirty(C3D_LightEnv* env)
{
	(void)env;
}

__attribute__((weak)) void C3Di_ProcTexUpdate(C3D_Context* ctx)
{
	(void)ctx;
}

__attribute__((weak)) void C3Di_ProcTexDirty(C3D_Context* ctx)
{
	(void)ctx;
}

__attribute__((weak)) void C3Di_GasUpdate(C3D_Context* ctx)
{
	(void)ctx;
}

static void C3Di_AptEventHook(APT_HookType hookType, C3D_UNUSED void* param)
{
	C3D_Context* ctx = C3Di_GetContext();

	switch (hookType)
	{
		case APTHOOK_ONSUSPEND:
		{
			C3Di_RenderQueueWaitDone();
			C3Di_RenderQueueDisableVBlank();
			break;
		}
		case APTHOOK_ONRESTORE:
		{
			C3Di_RenderQueueEnableVBlank();
			ctx->flags |= C3DiF_AttrInfo | C3DiF_BufInfo | C3DiF_Effect | C3DiF_FrameBuf
				| C3DiF_Viewport | C3DiF_Scissor | C3DiF_Program | C3DiF_VshCode | C3DiF_GshCode
				| C3DiF_TexAll | C3DiF_TexEnvBuf | C3DiF_TexEnvAll | C3DiF_LightEnv | C3DiF_Gas;

			C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
			C3Di_DirtyUniforms(GPU_GEOMETRY_SHADER);

			ctx->fixedAttribDirty |= ctx->fixedAttribEverDirty;
			ctx->gasFlags |= C3DiG_BeginAcc | C3DiG_AccStage | C3DiG_RenderStage;

			C3D_LightEnv* env = ctx->lightEnv;
			if (ctx->fogLut)
				ctx->flags |= C3DiF_FogLut;
			if (ctx->gasLut)
				ctx->flags |= C3DiF_GasLut;
			if (env)
				C3Di_LightEnvDirty(env);
			C3Di_ProcTexDirty(ctx);
			break;
		}
		default:
			break;
	}
}

bool C3D_Init(size_t cmdBufSize)
{
	int i;
	C3D_Context* ctx = C3Di_GetContext();

	if (ctx->flags & C3DiF_Active)
		return false;

	cmdBufSize = (cmdBufSize + 0xF) &~ 0xF; // 0x10-byte align
	ctx->cmdBufSize = cmdBufSize/4;
	ctx->cmdBuf = (u32*)linearAlloc(cmdBufSize);
	ctx->cmdBufUsage = 0;
	if (!ctx->cmdBuf)
		return false;

	ctx->gxQueue.maxEntries = 32;
	ctx->gxQueue.entries = (gxCmdEntry_s*)malloc(ctx->gxQueue.maxEntries*sizeof(gxCmdEntry_s));
	if (!ctx->gxQueue.entries)
	{
		linearFree(ctx->cmdBuf);
		return false;
	}

	ctx->flags = C3DiF_Active | C3DiF_TexEnvBuf | C3DiF_TexEnvAll | C3DiF_Effect | C3DiF_TexStatus | C3DiF_TexAll;

	// TODO: replace with direct struct access
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_CullFace(GPU_CULL_BACK_CCW);
	C3D_StencilTest(false, GPU_ALWAYS, 0x00, 0xFF, 0x00);
	C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
	C3D_BlendingColor(0);
	C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GREATER, 0);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_AlphaTest(false, GPU_ALWAYS, 0x00);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_FragOpMode(GPU_FRAGOPMODE_GL);
	C3D_FragOpShadow(0.0, 1.0);

	ctx->texConfig = BIT(12);
	ctx->texShadow = BIT(0);
	ctx->texEnvBuf = 0;
	ctx->texEnvBufClr = 0xFFFFFFFF;
	ctx->fogClr = 0;
	ctx->fogLut = NULL;

	for (i = 0; i < 3; i ++)
		ctx->tex[i] = NULL;

	for (i = 0; i < 6; i ++)
		C3D_TexEnvInit(&ctx->texEnv[i]);

	ctx->fixedAttribDirty = 0;
	ctx->fixedAttribEverDirty = 0;

	C3Di_RenderQueueInit();
	aptHook(&hookCookie, C3Di_AptEventHook, NULL);

	return true;
}

void C3D_SetViewport(u32 x, u32 y, u32 w, u32 h)
{
	C3D_Context* ctx = C3Di_GetContext();
	ctx->flags |= C3DiF_Viewport | C3DiF_Scissor;
	ctx->viewport[0] = f32tof24(w / 2.0f);
	ctx->viewport[1] = f32tof31(2.0f / w) << 1;
	ctx->viewport[2] = f32tof24(h / 2.0f);
	ctx->viewport[3] = f32tof31(2.0f / h) << 1;
	ctx->viewport[4] = (y << 16) | (x & 0xFFFF);
	ctx->scissor[0] = GPU_SCISSOR_DISABLE;
}

void C3D_SetScissor(GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom)
{
	C3D_Context* ctx = C3Di_GetContext();
	ctx->flags |= C3DiF_Scissor;
	ctx->scissor[0] = mode;
	if (mode == GPU_SCISSOR_DISABLE) return;
	ctx->scissor[1] = (top << 16) | (left & 0xFFFF);
	ctx->scissor[2] = ((bottom-1) << 16) | ((right-1) & 0xFFFF);
}

void C3Di_UpdateContext(void)
{
	int i;
	C3D_Context* ctx = C3Di_GetContext();

	if (ctx->flags & C3DiF_FrameBuf)
	{
		ctx->flags &= ~C3DiF_FrameBuf;
		if (ctx->flags & C3DiF_DrawUsed)
		{
			ctx->flags &= ~C3DiF_DrawUsed;
			GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
			GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
		}
		C3Di_FrameBufBind(&ctx->fb);
	}

	if (ctx->flags & C3DiF_Viewport)
	{
		ctx->flags &= ~C3DiF_Viewport;
		GPUCMD_AddIncrementalWrites(GPUREG_VIEWPORT_WIDTH, ctx->viewport, 4);
		GPUCMD_AddWrite(GPUREG_VIEWPORT_XY, ctx->viewport[4]);
	}

	if (ctx->flags & C3DiF_Scissor)
	{
		ctx->flags &= ~C3DiF_Scissor;
		GPUCMD_AddIncrementalWrites(GPUREG_SCISSORTEST_MODE, ctx->scissor, 3);
	}

	if (ctx->flags & C3DiF_Program)
	{
		shaderProgramConfigure(ctx->program, (ctx->flags & C3DiF_VshCode) != 0, (ctx->flags & C3DiF_GshCode) != 0);
		ctx->flags &= ~(C3DiF_Program | C3DiF_VshCode | C3DiF_GshCode);
	}

	if (ctx->flags & C3DiF_AttrInfo)
	{
		ctx->flags &= ~C3DiF_AttrInfo;
		C3Di_AttrInfoBind(&ctx->attrInfo);
	}

	if (ctx->flags & C3DiF_BufInfo)
	{
		ctx->flags &= ~C3DiF_BufInfo;
		C3Di_BufInfoBind(&ctx->bufInfo);
	}

	if (ctx->flags & C3DiF_Effect)
	{
		ctx->flags &= ~C3DiF_Effect;
		C3Di_EffectBind(&ctx->effect);
	}

	if (ctx->flags & C3DiF_TexAll)
	{
		u32 units = 0;
		for (i = 0; i < 3; i ++)
		{
			if (ctx->tex[i])
			{
				units |= BIT(i);
				if (ctx->flags & C3DiF_Tex(i))
					C3Di_SetTex(i, ctx->tex[i]);
			}
		}

		// Enable texture units and clear texture cache
		ctx->texConfig &= ~7;
		ctx->texConfig |= units | BIT(16);
		ctx->flags &= ~C3DiF_TexAll;
		ctx->flags |= C3DiF_TexStatus;
	}

	if (ctx->flags & C3DiF_TexStatus)
	{
		ctx->flags &= ~C3DiF_TexStatus;
		GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0xB, ctx->texConfig);
		// Clear texture cache if requested *after* configuring texture units
		if (ctx->texConfig & BIT(16))
		{
			ctx->texConfig &= ~BIT(16);
			GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
		}
		GPUCMD_AddWrite(GPUREG_TEXUNIT0_SHADOW, ctx->texShadow);
	}

	if (ctx->flags & (C3DiF_ProcTex | C3DiF_ProcTexColorLut | C3DiF_ProcTexLutAll))
		C3Di_ProcTexUpdate(ctx);

	if (ctx->flags & C3DiF_TexEnvBuf)
	{
		ctx->flags &= ~C3DiF_TexEnvBuf;
		GPUCMD_AddMaskedWrite(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, ctx->texEnvBuf);
		GPUCMD_AddWrite(GPUREG_TEXENV_BUFFER_COLOR, ctx->texEnvBufClr);
		GPUCMD_AddWrite(GPUREG_FOG_COLOR, ctx->fogClr);
	}

	if ((ctx->flags & C3DiF_FogLut) && (ctx->texEnvBuf&7) != GPU_NO_FOG)
	{
		ctx->flags &= ~C3DiF_FogLut;
		if (ctx->fogLut)
		{
			GPUCMD_AddWrite(GPUREG_FOG_LUT_INDEX, 0);
			GPUCMD_AddWrites(GPUREG_FOG_LUT_DATA0, ctx->fogLut->data, 128);
		}
	}

	if ((ctx->texEnvBuf&7) == GPU_GAS)
		C3Di_GasUpdate(ctx);

	if (ctx->flags & C3DiF_TexEnvAll)
	{
		for (i = 0; i < 6; i ++)
		{
			if (!(ctx->flags & C3DiF_TexEnv(i))) continue;
			C3Di_TexEnvBind(i, &ctx->texEnv[i]);
		}
		ctx->flags &= ~C3DiF_TexEnvAll;
	}

	C3D_LightEnv* env = ctx->lightEnv;

	if (ctx->flags & C3DiF_LightEnv)
	{
		u32 enable = env != NULL;
		GPUCMD_AddWrite(GPUREG_LIGHTING_ENABLE0, enable);
		GPUCMD_AddWrite(GPUREG_LIGHTING_ENABLE1, !enable);
		ctx->flags &= ~C3DiF_LightEnv;
	}

	if (env)
		C3Di_LightEnvUpdate(env);

	if (ctx->fixedAttribDirty)
	{
		for (i = 0; i < 12; i ++)
		{
			if (!(ctx->fixedAttribDirty & BIT(i))) continue;
			C3D_FVec* v = &ctx->fixedAttribs[i];

			GPUCMD_AddWrite(GPUREG_FIXEDATTRIB_INDEX, i);
			C3D_ImmSendAttrib(v->x, v->y, v->z, v->w);
		}
		ctx->fixedAttribDirty = 0;
	}

	C3D_UpdateUniforms(GPU_VERTEX_SHADER);
	C3D_UpdateUniforms(GPU_GEOMETRY_SHADER);
}

bool C3Di_SplitFrame(u32** pBuf, u32* pSize)
{
	C3D_Context* ctx = C3Di_GetContext();

	if (!gpuCmdBufOffset)
		return false; // Nothing was drawn

	if (ctx->flags & C3DiF_DrawUsed)
	{
		ctx->flags &= ~C3DiF_DrawUsed;
		GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
		GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
		GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
	}

	Gdx3dsCmdbufHwmNote(gpuCmdBufOffset, gpuCmdBufSize);

	GPUCMD_Split(pBuf, pSize);
	u32 totalCmdBufSize = *pBuf + *pSize - ctx->cmdBuf;
	ctx->cmdBufUsage = (float)totalCmdBufSize / ctx->cmdBufSize;
	return true;
}

float C3D_GetCmdBufUsage(void)
{
	return C3Di_GetContext()->cmdBufUsage;
}

/* GDX (o3ds3d [gpumem]): see gdx/c3d_gdx.h. */
unsigned C3Dgdx_CmdBufWords(void) {
	return (unsigned)C3Di_GetContext()->cmdBufSize;
}

void C3D_Fini(void)
{
	C3D_Context* ctx = C3Di_GetContext();

	if (!(ctx->flags & C3DiF_Active))
		return;

	aptUnhook(&hookCookie);
	C3Di_RenderQueueExit();
	free(ctx->gxQueue.entries);
	linearFree(ctx->cmdBuf);
	ctx->flags = 0;
}

void C3D_BindProgram(shaderProgram_s* program)
{
	C3D_Context* ctx = C3Di_GetContext();

	if (!(ctx->flags & C3DiF_Active))
		return;

	shaderProgram_s* oldProg = ctx->program;
	shaderInstance_s* newGsh = program->geometryShader;
	if (oldProg != program)
	{
		ctx->program = program;
		ctx->flags |= C3DiF_Program | C3DiF_AttrInfo;

		if (!oldProg)
			ctx->flags |= C3DiF_VshCode | C3DiF_GshCode;
		else
		{
			shaderInstance_s* oldGsh = oldProg->geometryShader;

			DVLP_s* oldProgV = oldProg->vertexShader->dvle->dvlp;
			DVLP_s* oldProgG = oldGsh ? oldGsh->dvle->dvlp : oldProgV;

			DVLP_s* newProgV = program->vertexShader->dvle->dvlp;
			DVLP_s* newProgG = newGsh ? newGsh->dvle->dvlp : newProgV;

			if (oldProgV != newProgV || (!newGsh && oldProgG != newProgG))
				ctx->flags |= C3DiF_VshCode;
			if (oldProgG != newProgG || (newProgG==oldProgV && newProgG->codeSize >= 512))
				ctx->flags |= C3DiF_GshCode;
		}
	}

	C3Di_LoadShaderUniforms(program->vertexShader);
	if (newGsh)
		C3Di_LoadShaderUniforms(newGsh);
	else
		C3Di_ClearShaderUniforms(GPU_GEOMETRY_SHADER);
}

C3D_FVec* C3D_FixedAttribGetWritePtr(int id)
{
	if (id < 0 || id >= 12)
		return NULL;

	C3D_Context* ctx = C3Di_GetContext();

	if (!(ctx->flags & C3DiF_Active))
		return NULL;

	ctx->fixedAttribDirty     |= BIT(id);
	ctx->fixedAttribEverDirty |= BIT(id);
	return &ctx->fixedAttribs[id];
}
