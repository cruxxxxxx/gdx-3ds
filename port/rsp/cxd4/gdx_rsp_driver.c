/* Thin driver for the vendored cxd4 RSP interpreter (LLE audio).
 *
 * cxd4's su.c is the scalar interpreter and the vu sources are the vector unit. cxd4's module.c
 * (its mupen64plus plugin framework) is NOT vendored, so this file supplies the module-level
 * symbols su.c references and module.c would otherwise define: RSP_INFO_NAME, GBI_phase, and the
 * message/update_conf/export/no_LLE stubs. It also provides a minimal "run one audio task" entry
 * point that boots the real aspMain microcode over the game's Acmd list -- the LLE replacement for
 * gdx_audio_hle_run.
 *
 * License: cxd4 is CC0; this driver is original G-Diffuser code.
 */
#include <string.h>
#include <stdint.h>
#include <setjmp.h>

#include "rsp.h"
#include "su.h"
#include "module.h"

/* Module-level globals su.c/su.h expect. MFC0_count, DRAM/DMEM/IMEM, CR[], conf[], SR[] and
 * MF_SP_STATUS_TIMEOUT are all DEFINED in su.c -- do not redefine them here. init_regs() lives in
 * the un-vendored module.c and only zeroes VU accumulator/flag globals, which are already
 * zero-initialized at load, so it is skipped. */
RSP_INFO RSP_INFO_NAME;                              /* the RCP register/memory info */
p_func GBI_phase;                                    /* GFX LLE hook (unused for audio) */

/* su.c calls these (cxd4 module.c debug/plumbing) -- stub them out. */
void no_LLE(void) { }
NOINLINE void message(const char* body) { (void)body; }
NOINLINE void update_conf(const char* source) { (void)source; }
NOINLINE void export_data_cache(void) { }
NOINLINE void export_instruction_cache(void) { }
void export_SP_memory(void) { }

/* ---- our RSP backing store ---- */
static unsigned char sDMEM[4096];
static unsigned char sIMEM[4096];
static u32 sSP_MEM_ADDR, sSP_DRAM_ADDR, sSP_RD_LEN, sSP_WR_LEN, sSP_STATUS;
static u32 sSP_DMA_FULL, sSP_DMA_BUSY, sSP_PC, sSP_SEMAPHORE;
static u32 sDPC[8];
static u32 sMI_INTR;

/* cxd4 externs (su.c) we drive */
extern pu8 DRAM, DMEM, IMEM;
extern pu32 CR[];
extern u8 conf[];
extern int MF_SP_STATUS_TIMEOUT;
extern unsigned long su_max_address;   /* cxd4 SP-DMA upper bound; default 0x7FFFFF (8MB) */
extern short MFC0_count[];              /* per-scalar-reg MFC0 spin counters (su.c) */
extern void run_task(void);

/* Watchdog. cxd4's run_task() loops until the ucode executes BREAK, so a mis-marshalled task
 * could spin the audio thread forever. Under -DSP_EXECUTE_LOG cxd4 calls step_SP_commands() per
 * executed instruction; this counts and longjmps out past a generous cap. A trip is reported as
 * "task did not complete", so the caller discards the output and falls back to the HLE. */
#define GDX_RSP_INSN_CAP 2000000L
static jmp_buf sWatchdog;
static long    sInsnCount;
static int     sCompleted;   /* 1 = last run_task reached BREAK; 0 = watchdog-tripped/no-BREAK */

/* Without SP_EXECUTE_LOG the per-instruction hook is dead and a runaway task hangs the audio
 * thread with no recovery, so fail the build rather than ship that. The unit-test target supplies
 * its own budgeted hook and marks itself exempt. */
#if !defined(SP_EXECUTE_LOG) && !defined(GDX_RSP_TEST_STEP_HOOK)
#error "gdx_rsp_driver.c requires SP_EXECUTE_LOG so the per-instruction watchdog is active."
#endif

/* The boot unit test defines its own step_SP_commands to capture the instruction stream, so the
 * driver's version is excluded there and the two do not collide at link time. */
#ifndef GDX_RSP_TEST_STEP_HOOK
void step_SP_commands(u32 inst) {
    (void)inst;
    if (++sInsnCount > GDX_RSP_INSN_CAP) {
        longjmp(sWatchdog, 1);
    }
}
#endif

/* Did the last gdx_rsp_lle_run_task complete (execute BREAK) rather than time out? */
int gdx_rsp_lle_completed(void) { return sCompleted; }

static void gdx_noop_checkintr(void) { }

/* Wire RSP_INFO once, pointing RDRAM at the game's flat gdx_rdram arena; dmem/imem are private
 * 4KB buffers. MemorySwapped==1 means memory is stored host-endian, which matches gdx_rdram, so
 * the ucode blobs loaded into IMEM/DMEM must be byte-swapped to match (see the loader below). */
void gdx_rsp_lle_init(unsigned char* rdram_base) {
    memset(&RSP_INFO_NAME, 0, sizeof(RSP_INFO_NAME));
    RSP_INFO_NAME.MemorySwapped   = 1;
    RSP_INFO_NAME.RDRAM           = (pu8)rdram_base;
    RSP_INFO_NAME.DMEM            = sDMEM;
    RSP_INFO_NAME.IMEM            = sIMEM;
    RSP_INFO_NAME.MI_INTR_REG     = &sMI_INTR;
    RSP_INFO_NAME.SP_MEM_ADDR_REG = &sSP_MEM_ADDR;
    RSP_INFO_NAME.SP_DRAM_ADDR_REG= &sSP_DRAM_ADDR;
    RSP_INFO_NAME.SP_RD_LEN_REG   = &sSP_RD_LEN;
    RSP_INFO_NAME.SP_WR_LEN_REG   = &sSP_WR_LEN;
    RSP_INFO_NAME.SP_STATUS_REG   = &sSP_STATUS;
    RSP_INFO_NAME.SP_DMA_FULL_REG = &sSP_DMA_FULL;
    RSP_INFO_NAME.SP_DMA_BUSY_REG = &sSP_DMA_BUSY;
    RSP_INFO_NAME.SP_PC_REG       = &sSP_PC;
    RSP_INFO_NAME.SP_SEMAPHORE_REG= &sSP_SEMAPHORE;
    RSP_INFO_NAME.DPC_START_REG   = &sDPC[0];
    RSP_INFO_NAME.DPC_END_REG     = &sDPC[1];
    RSP_INFO_NAME.DPC_CURRENT_REG = &sDPC[2];
    RSP_INFO_NAME.DPC_STATUS_REG  = &sDPC[3];
    RSP_INFO_NAME.DPC_CLOCK_REG   = &sDPC[4];
    RSP_INFO_NAME.DPC_BUFBUSY_REG = &sDPC[5];
    RSP_INFO_NAME.DPC_PIPEBUSY_REG= &sDPC[6];
    RSP_INFO_NAME.DPC_TMEM_REG    = &sDPC[7];
    RSP_INFO_NAME.CheckInterrupts = gdx_noop_checkintr;

    DRAM = RSP_INFO_NAME.RDRAM;
    DMEM = RSP_INFO_NAME.DMEM;
    IMEM = RSP_INFO_NAME.IMEM;

    CR[0x0] = RSP_INFO_NAME.SP_MEM_ADDR_REG;
    CR[0x1] = RSP_INFO_NAME.SP_DRAM_ADDR_REG;
    CR[0x2] = RSP_INFO_NAME.SP_RD_LEN_REG;
    CR[0x3] = RSP_INFO_NAME.SP_WR_LEN_REG;
    CR[0x4] = RSP_INFO_NAME.SP_STATUS_REG;
    CR[0x5] = RSP_INFO_NAME.SP_DMA_FULL_REG;
    CR[0x6] = RSP_INFO_NAME.SP_DMA_BUSY_REG;
    CR[0x7] = RSP_INFO_NAME.SP_SEMAPHORE_REG;
    CR[0x8] = RSP_INFO_NAME.DPC_START_REG;
    CR[0x9] = RSP_INFO_NAME.DPC_END_REG;
    CR[0xA] = RSP_INFO_NAME.DPC_CURRENT_REG;
    CR[0xB] = RSP_INFO_NAME.DPC_STATUS_REG;
    CR[0xC] = RSP_INFO_NAME.DPC_CLOCK_REG;
    CR[0xD] = RSP_INFO_NAME.DPC_BUFBUSY_REG;
    CR[0xE] = RSP_INFO_NAME.DPC_PIPEBUSY_REG;
    CR[0xF] = RSP_INFO_NAME.DPC_TMEM_REG;
    MF_SP_STATUS_TIMEOUT = 32767;
    /* Raise the SP-DMA ceiling from cxd4's 8MB default to the full 16MB gdx_rdram. Without it,
     * any buffer the bridge stages above 8MB -- including the scratch carved from the TOP of
     * RDRAM -- DMAs as ZEROS (su.c SP_DMA_READ memsets when offD > su_max_address), feeding the
     * ucode a garbage command stream it never terminates on. */
    su_max_address = 0x00FFFFFFul;   /* 16MB - 1 == cxd4's 24-bit DMA max == gdx_rdram size */
    GBI_phase = no_LLE;
    conf[0x00] = 1; /* CFG_HLE_GFX: bounce any gfx task (we never feed one) */
    conf[0x01] = 0; /* CFG_HLE_AUD: LLE audio -- run the real ucode */
}

/* Load aspMain into IMEM and execute one audio task. cxd4 stores IMEM/DMEM/RDRAM as HOST-NATIVE
 * little-endian 32-bit words (BES/HES element-swap, ENDIAN_M=~0), so:
 *   ucodeText      big-endian aspMain text blob in raw ROM order, byte-swapped per 32-bit word
 *                  into IMEM.
 *   ucodeData      written to DMEM[0] as a convenience, but aspMain's boot re-DMAs its data
 *                  section from OSTask.ucode_data over DMEM[0] anyway, so the CALLER must place a
 *                  byte-swapped copy in RDRAM and set ucode_data/ucode_data_size. Loading it here
 *                  too just keeps DMEM[0] sane for a task with ucode_data==0.
 *   dmemTaskHeader 64-byte OSTask for DMEM[0xFC0], fields host-native little-endian; addresses are
 *                  physical RDRAM offsets.
 * Runs until the task-complete BREAK sets SP_STATUS_BROKE. */
void gdx_rsp_lle_run_task(const void* ucodeText, unsigned textBytes,
                          const void* ucodeData, unsigned dataBytes,
                          const void* dmemTaskHeader) {
    const unsigned char* t = (const unsigned char*)ucodeText;
    unsigned i;
    if (textBytes > 4096u) textBytes = 4096u;
    if (dataBytes > 0xFC0u) dataBytes = 0xFC0u;
    memset(sIMEM, 0, sizeof(sIMEM));
    for (i = 0; i + 4u <= textBytes; i += 4u) {   /* BE -> host-native 32-bit swap */
        sIMEM[i + 0] = t[i + 3];
        sIMEM[i + 1] = t[i + 2];
        sIMEM[i + 2] = t[i + 1];
        sIMEM[i + 3] = t[i + 0];
    }
    {   /* DMEM data section: same BE -> host-native 32-bit swap (see header note) */
        const unsigned char* d = (const unsigned char*)ucodeData;
        for (i = 0; i + 4u <= dataBytes; i += 4u) {
            sDMEM[i + 0] = d[i + 3];
            sDMEM[i + 1] = d[i + 2];
            sDMEM[i + 2] = d[i + 1];
            sDMEM[i + 3] = d[i + 0];
        }
    }
    memcpy(sDMEM + 0xFC0, dmemTaskHeader, 64);
    sSP_STATUS = 0;
    sSP_PC = 0;
    *RSP_INFO_NAME.SP_PC_REG = 0;
    sInsnCount = 0;
    /* Stock cxd4 resets the MFC0 spin counters per task inside the un-vendored module.c
     * DoRspCycles. Without that, MFC0_count[] accumulates for the life of the process and, once it
     * reaches MF_SP_STATUS_TIMEOUT, spuriously force-HALTs a later task with no BREAK. */
    memset(MFC0_count, 0, sizeof(short) * (size_t)NUMBER_OF_SCALAR_REGISTERS);
    if (setjmp(sWatchdog) == 0) {
        run_task();
        /* "Completed" means the ucode executed the task-complete BREAK, not merely that
         * run_task() returned: cxd4 also returns on an early HALT-without-BROKE (MFC0, semaphore,
         * interrupt paths), and trusting those would copy partial DMEM to the speakers. */
        sCompleted = (sSP_STATUS & SP_STATUS_BROKE) ? 1 : 0;
    } else {
        sCompleted = 0;        /* watchdog tripped: runaway/mis-marshalled task */
    }
}

/* Post-run introspection for tests. */
unsigned gdx_rsp_lle_status(void) { return sSP_STATUS; }
const unsigned char* gdx_rsp_lle_dmem(void) { return sDMEM; }
