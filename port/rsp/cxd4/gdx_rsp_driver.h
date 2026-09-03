/* Public API of the G-Diffuser cxd4 RSP driver shim (LLE audio).
 * See gdx_rsp_driver.c for the memory/endianness contract. */
#ifndef GDX_RSP_DRIVER_H
#define GDX_RSP_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Wire the RSP once, pointing its flat RDRAM base at `rdram_base` (the game's
 * gdx_rdram). DMEM/IMEM are private 4KB buffers inside the driver. */
void gdx_rsp_lle_init(unsigned char* rdram_base);

/* Boot aspMain (text byte-swapped BE->host into IMEM; data section byte-swapped
 * into DMEM[0] as a fallback) and run one audio task described by a 64-byte OSTask
 * (host-native-little-endian fields) placed at DMEM[0xFC0]. The task's ucode_data /
 * data_ptr must be physical offsets into rdram_base; the referenced buffers and the
 * command list must already live in that RDRAM (host-native word order). Runs until
 * the task-complete BREAK. */
void gdx_rsp_lle_run_task(const void* ucodeText, unsigned textBytes,
                          const void* ucodeData, unsigned dataBytes,
                          const void* dmemTaskHeader);

/* Did the last gdx_rsp_lle_run_task reach BREAK (1) or trip the instruction-count
 * watchdog (0)? A 0 means the task was mis-formed/runaway -- the caller must discard
 * its output and fall back to the HLE for that tick. (Requires the driver compiled
 * with -DSP_EXECUTE_LOG; without it, always returns 1.) */
int gdx_rsp_lle_completed(void);

/* Post-run introspection (for tests/diagnostics). */
unsigned gdx_rsp_lle_status(void);
const unsigned char* gdx_rsp_lle_dmem(void);

#ifdef __cplusplus
}
#endif
#endif /* GDX_RSP_DRIVER_H */
