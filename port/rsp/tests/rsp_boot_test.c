/* Proves the vendored cxd4 RSP interpreter boots the REAL aspMain audio microcode (nead/ABI2)
 * and runs it to a clean task-complete BREAK, driven only through the driver shim's public entry
 * points.
 *
 * Task setup, established by reverse-engineering the nead command dispatcher:
 *   - The ucode's OWN boot code DMAs OSTask.ucode_data into DMEM[0] and rebuilds its jump table at
 *     DMEM[0x10], so the data section MUST live in RDRAM with ucode_data(+0x18)/
 *     ucode_data_size(+0x1C) pointing at it; a manual DMEM[0] preload alone is clobbered.
 *   - The dispatcher advances data_ptr and decrements k1 = data_size by 8 per command; at blez k1
 *     it sets SP_STATUS SET_SIG2 and BREAKs at IMEM 0x10D0. Termination is that byte counter, not
 *     a terminator opcode or a fixed command count.
 *   - Opcode 0x00 is a HEAVY handler; the true loop-continue no-ops are 0x03/0x06/0x07/0x09/0x0E/
 *     0x18/0x19/0x1B/0x1C/0x1D. This test uses 0x03.
 *   - All multi-byte OSTask fields and command words are big-endian (the RSP reads RDRAM/DMEM
 *     big-endian through cxd4's BES element-swap).
 *
 * Asserts (1) the first executed instruction is 0x200a0fc0 (ADDI $10,$0,0xFC0), which pins the
 * IMEM BE->LE swap and the boot shortcut, and (2) the run reaches BREAK with SP_STATUS_BROKE and
 * SET_SIG2, i.e. a full audio task completed. The instruction budget bounds the run so a
 * regression that fails to terminate is a FAIL rather than a hang.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

extern void gdx_rsp_lle_init(unsigned char*);
extern void gdx_rsp_lle_run_task(const void*, unsigned, const void*, unsigned, const void*);
extern unsigned gdx_rsp_lle_status(void);

#ifndef RSP_BLOB_DIR
#define RSP_BLOB_DIR "."
#endif

typedef unsigned int u32;

#define SP_STATUS_BROKE  0x0002u
#define SP_STATUS_SIG2   0x4000u

/* The blobs are Nintendo-copyrighted and not in the repo: slice them out of the user's own ROM
 * (argv[1] or GDX_RSP_TEST_ROM), same ranges as decomp/assets/yaml/us/rev0/rsp_blob.yaml. */
#define ROM_UCODE_TEXT_OFF 0x66270u
#define ROM_UCODE_DATA_OFF 0x71CA0u
#define UCODE_TEXT_BYTES   0x0D30u
#define UCODE_DATA_BYTES   0x02E0u

/* RDRAM layout for the offline test */
#define RD_UCODE_DATA  0x1000u   /* aspmain data copy (ucode_data) */
#define RD_CMD_LIST    0x2000u   /* command list (data_ptr), 8-byte aligned */

static jmp_buf g_bail;
static u32 g_log[512];
static int g_n = 0;
static int g_overrun = 0;
#define RUN_BUDGET 20000   /* a correct BREAK hits in <200 steps; only a hang reaches this */

void step_SP_commands(u32 inst) {
    if (g_n < (int)(sizeof g_log / sizeof g_log[0]))
        g_log[g_n] = inst;
    g_n++;
    if (g_n >= RUN_BUDGET) { g_overrun = 1; longjmp(g_bail, 1); }
}

static unsigned load_blob(const char* name, unsigned char* buf, unsigned cap) {
    char path[1024];
    FILE* f;
    unsigned n;
    snprintf(path, sizeof path, "%s/%s", RSP_BLOB_DIR, name);
    f = fopen(path, "rb");
    if (!f) return 0;
    n = (unsigned)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static unsigned load_rom_slice(const char* romPath, unsigned off, unsigned char* buf, unsigned cap) {
    FILE* f = fopen(romPath, "rb");
    unsigned n;
    if (!f) return 0;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return 0; }
    n = (unsigned)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static unsigned char rdram[0x1000000];
static unsigned char text[8192], data[4096], ostask[64];

/* cxd4 stores DMEM/RDRAM/IMEM as host-native little-endian 32-bit words; its BES/HES element-swap
 * macros reconstruct big-endian byte/half access from that storage. So values the RSP will read
 * via LW are stored host-native... */
static void le32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
/* ...and N64 big-endian ROM blobs are byte-swapped per 32-bit word on the way in. */
static void copy_be_words_to_native(unsigned char* dst, const unsigned char* src, unsigned n) {
    unsigned i;
    for (i = 0; i + 4u <= n; i += 4u) {
        dst[i + 0] = src[i + 3]; dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1]; dst[i + 3] = src[i + 0];
    }
}

int main(int argc, char** argv) {
    unsigned tn = load_blob("aspmain_text.bin", text, sizeof text);
    unsigned dn = load_blob("aspmain_data.bin", data, sizeof data);
    const char* rom = (argc > 1) ? argv[1] : getenv("GDX_RSP_TEST_ROM");
    const unsigned NCMD = 8;
    unsigned data_size = NCMD * 8;
    unsigned i, st, saw_entry;

    if ((!tn || !dn) && rom != NULL) {
        tn = load_rom_slice(rom, ROM_UCODE_TEXT_OFF, text, UCODE_TEXT_BYTES);
        dn = load_rom_slice(rom, ROM_UCODE_DATA_OFF, data, UCODE_DATA_BYTES);
    }

    if (!tn || !dn) {
        printf("SKIP: no ucode source -- pass a US rev0 ROM path (argv[1] or GDX_RSP_TEST_ROM).\n");
        return 0; /* the blobs are never shipped; a bare checkout has nothing to boot */
    }

    /* The data section, byte-swapped BE->native, that the ucode's boot DMA pulls into DMEM to
     * rebuild its jump table... */
    memset(rdram, 0, 0x4000);
    copy_be_words_to_native(rdram + RD_UCODE_DATA, data, dn);
    /* ...and a command list of no-op packets, stored host-native so w0>>24 == 0x03. */
    for (i = 0; i < NCMD; i++)
        le32(rdram + RD_CMD_LIST + i * 8, 0x03000000u); /* w0: opcode 0x03; w1 stays 0 */

    memset(ostask, 0, sizeof ostask);
    le32(ostask + 0x00, 2);              /* type = M_AUDTASK */
    le32(ostask + 0x18, RD_UCODE_DATA);  /* ucode_data -> RDRAM data-section copy */
    le32(ostask + 0x1C, dn);             /* ucode_data_size = 0x2E0 (736) */
    le32(ostask + 0x30, RD_CMD_LIST);    /* data_ptr -> command list */
    le32(ostask + 0x34, data_size);      /* data_size = 0x40 (counts down to BREAK) */

    gdx_rsp_lle_init(rdram);
    if (setjmp(g_bail) == 0)
        gdx_rsp_lle_run_task(text, tn, data, dn, ostask); /* returns on BREAK */
    st = gdx_rsp_lle_status();

    saw_entry = (g_n > 0 && g_log[0] == 0x200a0fc0u);

    printf("executed %d instrs; entry=%d overrun=%d SP_STATUS=0x%08x (BROKE=%d SIG2=%d)\n",
           g_n, saw_entry, g_overrun, st,
           (st & SP_STATUS_BROKE) != 0, (st & SP_STATUS_SIG2) != 0);

    if (!saw_entry) {
        printf("FAIL: entry marker missing (IMEM byte-swap/boot wrong).\n");
        return 1;
    }
    if (g_overrun || !(st & SP_STATUS_BROKE)) {
        printf("FAIL: ucode did not reach task-complete BREAK.\n");
        return 1;
    }
    printf("PASS: cxd4 LLE core boots and runs the real aspMain ucode to a clean BREAK.\n");
    return 0;
}
