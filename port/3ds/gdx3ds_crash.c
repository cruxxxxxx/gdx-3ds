/* port/3ds/gdx3ds_crash.c -- REL1CRASH: self-describing hardware crashes. Contract and why:
 * port/3ds/include/gdx3ds_crash.h.
 *
 * Everything on the fault path is static: the report buffer, the handler stacks, the FS
 * calls (raw FSUSER/FSFILE IPC, no newlib FILE, no heap). The fault may have happened with
 * the heap or a stdio lock in any state, so the crash file is written before anything that
 * could touch them, and only then are the lines offered to the regular log.
 */
#include "gdx3ds_crash.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx_dev_gates.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <3ds.h>

#ifndef GDX3DS_LOG_TAG
#define GDX3DS_LOG_TAG ""
#endif
/* hw bug n60-3d8, item 3: the real branch@hash now comes from this generated, build-time
 * header (port/3ds/cmake/gdx3ds_gen_build_id.cmake) instead of a configure-time -D; the
 * __has_include guard + #ifndef below is a safety net for a build that never generated or
 * wired up the include path for it. */
#if defined(__has_include)
#if __has_include("gdx3ds_build_id.h")
#include "gdx3ds_build_id.h"
#endif
#endif
#ifndef GDX3DS_BUILD_ID
#define GDX3DS_BUILD_ID "unknown"
#endif

#define GDX3DS_CRASH_PATH "/3ds/gdiffuser/crash" GDX3DS_LOG_TAG ".txt"
#define GDX3DS_CRASH_SLOTS 24
#define GDX3DS_CRASH_SLOT_BYTES 4096
#define GDX3DS_CRASH_REPORT_BYTES 24576
#define GDX3DS_CRASH_TAIL_BYTES 12288
#define GDX3DS_CRASH_STACK_WORDS 96

/* filelog tail accessor (gdx3ds_filelog.c): lock-free copy of the retained ring's tail. */
extern size_t gdx3ds_filelog_crash_tail(char* out, size_t cap, unsigned* pendingOut);

static int sEnabled = 0;
static int sArmedThreads = 0;
static volatile int sSlotNext = 0;
static volatile int sBusy = 0;
/* CRASHFIX: set once main() starts its teardown (gdx3ds_crash_note_exit). */
static volatile int sExiting = 0;
static u8 sSlotStacks[GDX3DS_CRASH_SLOTS][GDX3DS_CRASH_SLOT_BYTES] __attribute__((aligned(8)));
static char sReport[GDX3DS_CRASH_REPORT_BYTES];
static size_t sReportLen = 0;
static char sTail[GDX3DS_CRASH_TAIL_BYTES];
/* n60boot: the boot trail's sink (gdx3ds_boot_trail.c) while the boot window is open. */
static GdxCrashBootSink sBootSink = NULL;
static int sBootArmed = 0;

/* ---- tiny formatter: no snprintf on the fault path (it can allocate and take locks) ---- */

static void ReportPutStr(const char* s) {
    while (*s != '\0' && sReportLen + 1 < sizeof(sReport)) {
        sReport[sReportLen++] = *s++;
    }
}

static void ReportPutHex(u32 v) {
    static const char kHex[] = "0123456789abcdef";
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        buf[i] = kHex[v & 0xFu];
        v >>= 4;
    }
    buf[8] = '\0';
    ReportPutStr(buf);
}

static void ReportPutDec(u32 v) {
    char buf[11];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < 10);
    while (n > 0) {
        char c[2] = { buf[--n], '\0' };
        ReportPutStr(c);
    }
}

static void ReportPutReg(const char* name, u32 v) {
    ReportPutStr(name);
    ReportPutStr("=");
    ReportPutHex(v);
    ReportPutStr(" ");
}

static const char* ExceptionTypeName(ERRF_ExceptionType type) {
    switch (type) {
        case ERRF_EXCEPTION_PREFETCH_ABORT: return "prefetch-abort";
        case ERRF_EXCEPTION_DATA_ABORT: return "data-abort";
        case ERRF_EXCEPTION_UNDEFINED: return "undefined-instruction";
        case ERRF_EXCEPTION_VFP: return "vfp";
        default: return "unknown";
    }
}

/* ARMv6 FSR status (bits [10,3:0]). Alignment is the one an emulator never raises. */
static const char* FaultStatusName(u32 fsr) {
    const u32 status = (fsr & 0xFu) | ((fsr >> 6) & 0x10u);
    switch (status) {
        case 0x01: return "ALIGNMENT";
        case 0x05: return "translation(section)";
        case 0x07: return "translation(page)";
        case 0x09: return "domain(section)";
        case 0x0B: return "domain(page)";
        case 0x0D: return "permission(section)";
        case 0x0F: return "permission(page)";
        case 0x08: return "precise-external";
        case 0x16: return "imprecise-external";
        case 0x02: return "debug";
        default: return "other";
    }
}

/* ---- memory queries -------------------------------------------------------------------- */

static bool AddressReadable(u32 addr) {
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) {
        return false;
    }
    return (mi.perm & MEMPERM_READ) != 0 && mi.state != MEMSTATE_FREE;
}

static bool AddressIsCode(u32 addr) {
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr & ~3u))) {
        return false;
    }
    return (mi.perm & MEMPERM_EXECUTE) != 0;
}

/* ---- report ------------------------------------------------------------------------------ */

static void ReportHeader(const ERRF_ExceptionInfo* excep, const CpuRegisters* regs) {
    u32 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    ReportPutStr("[crash] ARM11 ");
    ReportPutStr(ExceptionTypeName(excep->type));
    ReportPutStr(" build=" GDX3DS_BUILD_ID " tid=");
    ReportPutDec(tid);
    ReportPutStr(" pc=");
    ReportPutHex(regs->pc);
    ReportPutStr(" lr=");
    ReportPutHex(regs->lr);
    ReportPutStr(" far=");
    ReportPutHex(excep->far);
    ReportPutStr(" fsr=");
    ReportPutHex(excep->fsr);
    if (excep->type == ERRF_EXCEPTION_DATA_ABORT || excep->type == ERRF_EXCEPTION_PREFETCH_ABORT) {
        ReportPutStr(" (");
        ReportPutStr(FaultStatusName(excep->fsr));
        if (excep->type == ERRF_EXCEPTION_DATA_ABORT) {
            ReportPutStr((excep->fsr & (1u << 11)) != 0 ? ", write" : ", read");
        }
        ReportPutStr(")");
    }
    if (excep->type == ERRF_EXCEPTION_VFP) {
        ReportPutStr(" fpexc=");
        ReportPutHex(excep->fpexc);
        ReportPutStr(" fpinst=");
        ReportPutHex(excep->fpinst);
    }
    ReportPutStr("\n[crash] regs ");
    static const char* const kNames[13] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6",
                                            "r7", "r8", "r9", "r10", "r11", "r12" };
    for (int i = 0; i < 13; i++) {
        ReportPutReg(kNames[i], regs->r[i]);
    }
    ReportPutReg("sp", regs->sp);
    ReportPutReg("cpsr", regs->cpsr);
    ReportPutStr("\n[crash] addr2line -f -C -e G-Diffuser-3DS.elf ");
    ReportPutHex(regs->pc);
    ReportPutStr(" ");
    ReportPutHex(regs->lr);
    ReportPutStr("\n");
}

/* Every stack word that points into executable memory: the return-address candidates an
 * addr2line pass turns into a call chain (no frame pointers in this build). */
static void ReportStack(const CpuRegisters* regs) {
    u32 sp = regs->sp & ~3u;
    ReportPutStr("[crash] stack code-words (sp+off=value):");
    int shown = 0;
    for (int i = 0; i < GDX3DS_CRASH_STACK_WORDS; i++) {
        const u32 addr = sp + (u32)i * 4u;
        if ((i == 0 || (addr & 0xFFFu) == 0) && !AddressReadable(addr)) {
            ReportPutStr(" <unreadable at ");
            ReportPutHex(addr);
            ReportPutStr(">");
            break;
        }
        const u32 v = *(const volatile u32*)addr;
        if (v >= 0x00100000u && v < 0x04000000u && AddressIsCode(v)) {
            ReportPutStr(shown % 6 == 0 ? "\n[crash]   " : " ");
            ReportPutStr("+");
            ReportPutHex((u32)i * 4u);
            ReportPutStr("=");
            ReportPutHex(v);
            shown++;
        }
    }
    ReportPutStr("\n");
}

/* ---- sinks ------------------------------------------------------------------------------- */

static void CrashFileWrite(void) {
    Handle file = 0;
    const Result rc = FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                                              fsMakePath(PATH_ASCII, GDX3DS_CRASH_PATH),
                                              FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_FAILED(rc)) {
        return;
    }
    /* APPEND: several crashing runs of one title between two card pulls (a bisect session)
     * must all survive; each report starts with its own "[crash] ARM11" header line. */
    u32 written = 0;
    u64 offset = 0;
    if (R_FAILED(FSFILE_GetSize(file, &offset))) {
        offset = 0;
    }
    FSFILE_Write(file, &written, offset, sReport, (u32)sReportLen, FS_WRITE_FLUSH);
    offset += written;
    static const char kTailHead[] = "[crash] ---- file-log tail (retained ring, newest last) ----\n";
    FSFILE_Write(file, &written, offset, kTailHead, sizeof(kTailHead) - 1, FS_WRITE_FLUSH);
    offset += written;
    unsigned pending = 0;
    const size_t tailLen = gdx3ds_filelog_crash_tail(sTail, sizeof(sTail), &pending);
    FSFILE_Write(file, &written, offset, sTail, (u32)tailLen, FS_WRITE_FLUSH);
    offset += written;
    char note[64];
    size_t n = 0;
    const char* head = "\n[crash] ---- end; unflushed-at-fault bytes=";
    while (*head != '\0' && n + 1 < sizeof(note)) {
        note[n++] = *head++;
    }
    char dec[11];
    int d = 0;
    do {
        dec[d++] = (char)('0' + (pending % 10u));
        pending /= 10u;
    } while (pending != 0u && d < 10);
    while (d > 0 && n + 2 < sizeof(note)) {
        note[n++] = dec[--d];
    }
    note[n++] = '\n';
    FSFILE_Write(file, &written, offset, note, (u32)n, FS_WRITE_FLUSH);
    FSFILE_Close(file);
}

/* The report's lines into the regular log (one write per line, then a flush). After the
 * crash file, because this path takes the sink's lock and newlib's FILE lock. */
static void CrashLogWrite(void) {
    size_t start = 0;
    for (size_t i = 0; i < sReportLen; i++) {
        if (sReport[i] == '\n') {
            gdx3ds_filelog_write(sReport + start, i - start);
            start = i + 1;
        }
    }
    gdx3ds_filelog_flush_sync();
}

static void ParkForever(void) {
    for (;;) {
        svcSleepThread(1000000000LL);
    }
}

static void CrashHandler(ERRF_ExceptionInfo* excep, CpuRegisters* regs) {
    /* A fault inside this handler must go to the default path, not back here. */
    threadOnException(NULL, NULL, NULL);
    if (__atomic_exchange_n(&sBusy, 1, __ATOMIC_ACQ_REL) != 0) {
        ParkForever(); /* another thread's report is being written; it ends the process */
    }
    sReportLen = 0;
    ReportHeader(excep, regs);
    ReportStack(regs);
    if (sBootSink != NULL) {
        sBootSink(sReport, sReportLen); /* boot.txt first: one flushed write */
    }
    CrashFileWrite();
    if (sExiting) {
        /* CRASHFIX: a fault during the teardown (rel1crash B/D: a title that armed this handler
         * and ran cleanly reported "the system will reboot" on close from HOME). The report is
         * on the card; end the process quietly, as an unarmed title's teardown fault does,
         * instead of raising ErrDisp over a close the user asked for. No log write: the file
         * log may already be closed. */
        svcExitProcess();
    }
    CrashLogWrite();
    ERRF_ExceptionHandler(excep, regs); /* ErrDisp, exactly as without this handler */
}

void gdx3ds_crash_note_exit(void) {
    sExiting = 1;
}

/* ---- arming ------------------------------------------------------------------------------ */

void gdx3ds_crash_arm_current_thread(void) {
    if (!sEnabled) {
        return;
    }
    const int slot = __atomic_fetch_add(&sSlotNext, 1, __ATOMIC_ACQ_REL);
    if (slot < GDX3DS_CRASH_SLOTS) {
        threadOnException(CrashHandler, sSlotStacks[slot] + GDX3DS_CRASH_SLOT_BYTES,
                          WRITE_DATA_TO_HANDLER_STACK);
    } else {
        threadOnException(CrashHandler, RUN_HANDLER_ON_FAULTING_STACK, WRITE_DATA_TO_FAULTING_STACK);
    }
    __atomic_fetch_add(&sArmedThreads, 1, __ATOMIC_RELAXED);
}

/* n60boot: the boot window's handler. Armed on the main thread from main() entry -- before
 * config, gfxInit and the file log -- whatever [debug] crashdump says, so an early data abort
 * leaves its report in boot.txt (the sink) and crash.txt. Disarmed when the boot trail closes
 * unless crashdump=1 armed the same handler for the whole run. */
void gdx3ds_crash_arm_boot(GdxCrashBootSink sink) {
    sBootSink = sink;
    const int slot = __atomic_fetch_add(&sSlotNext, 1, __ATOMIC_ACQ_REL);
    if (slot >= GDX3DS_CRASH_SLOTS) {
        return;
    }
    threadOnException(CrashHandler, sSlotStacks[slot] + GDX3DS_CRASH_SLOT_BYTES, WRITE_DATA_TO_HANDLER_STACK);
    sBootArmed = 1;
}

void gdx3ds_crash_disarm_boot(void) {
    sBootSink = NULL;
    if (sBootArmed && !sEnabled) {
        threadOnException(NULL, NULL, NULL);
    }
    sBootArmed = 0;
}

int gdx3ds_crash_boot_armed(void) {
    return sBootArmed;
}

/* Default OFF on every model (2026-09-29). On the Old 3DS (Luma 6.6) the handler never produced a
 * crash file for the real boot crash, and both titles that armed it and ran cleanly (rel1crash B/D)
 * reported "the system will reboot" on close from HOME. Opt in with [debug] crashdump=1 until
 * crashdump_selftest proves the handler on the console. */
static int DefaultEnabled(void) {
    return 0;
}

static void SelfTestFault(void) {
    volatile u32* target = (volatile u32*)0x00000010u;
    *target = 0xDEADC0DEu; /* NULL-page store: a translation-fault data abort */
}

void gdx3ds_crash_init(void) {
    const int modelDefault = DefaultEnabled();
    sEnabled = gdx3ds_config_get_bool("debug", "crashdump", modelDefault) ? 1 : 0;
    const int selftest = gdx3ds_config_get_bool("debug", "crashdump_selftest", 0) ? 1 : 0;
    gdx3ds_crash_arm_current_thread();
    char line[200];
    size_t n = 0;
    const char* parts[] = { "[crash] armed crashdump=", sEnabled ? "1" : "0",
                            " (model default ", modelDefault ? "1" : "0",
                            ") file=sdmc:" GDX3DS_CRASH_PATH " selftest=", selftest ? "1" : "0",
                            " svcBreak-trace=1" };
    for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); p++) {
        for (const char* s = parts[p]; *s != '\0' && n + 1 < sizeof(line); s++) {
            line[n++] = *s;
        }
    }
    gdx3ds_filelog_write(line, n);
    gdx3ds_filelog_flush_sync();
    if (sEnabled && selftest) {
        static const char kSelf[] = "[crash] selftest: NULL store now; expect crash" GDX3DS_LOG_TAG
                                    ".txt + ErrDisp";
        gdx3ds_filelog_write(kSelf, sizeof(kSelf) - 1);
        gdx3ds_filelog_flush_sync();
        SelfTestFault();
    }
}

/* ---- --wrap=threadCreate: every libctru thread (fibers, audio, ndsp, apt, ...) gets armed -- */

typedef struct CrashThunk {
    ThreadFunc entry;
    void* arg;
} CrashThunk;

#define GDX3DS_CRASH_THUNKS 64
static CrashThunk sThunks[GDX3DS_CRASH_THUNKS];
static volatile int sThunkNext = 0;

Thread __real_threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int core_id,
                           bool detached);

static void CrashThunkEntry(void* raw) {
    CrashThunk* thunk = (CrashThunk*)raw;
    gdx3ds_crash_arm_current_thread();
    thunk->entry(thunk->arg);
}

/* ---- N60CORE1: the one-shot core redirect (contract in gdx3ds_crash.h) ---- */

static volatile int sRedirectCore = -1;    /* armed by begin(), consumed by the first create */
static volatile int sRedirectCreated = -1; /* core the redirected thread was created on */
static volatile int sRedirectFellBack = 0;
static volatile int sRedirectRanOn = -1;   /* svcGetProcessorID() from the thread itself */
static CrashThunk sRedirectThunk;

static void RedirectThunkEntry(void* raw) {
    CrashThunk* thunk = (CrashThunk*)raw;
    const s32 core = svcGetProcessorID();
    sRedirectRanOn = (core >= 0 && core <= 3) ? (int)core : -2; /* -2 = not a core id (Azahar returns junk) */
    __dmb();
    if (sEnabled) {
        gdx3ds_crash_arm_current_thread();
    }
    thunk->entry(thunk->arg);
}

static Thread CreateRedirected(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int core_id,
                               bool detached, int toCore) {
    sRedirectThunk.entry = entrypoint;
    sRedirectThunk.arg = arg;
    __dmb();
    Thread t = __real_threadCreate(RedirectThunkEntry, &sRedirectThunk, stack_size, prio, toCore, detached);
    if (t != NULL) {
        sRedirectCreated = toCore;
        return t;
    }
    sRedirectFellBack = 1;
    t = __real_threadCreate(RedirectThunkEntry, &sRedirectThunk, stack_size, prio, core_id, detached);
    if (t != NULL) {
        sRedirectCreated = core_id;
    }
    return t;
}

void gdx3ds_thread_redirect_begin(int core) {
    sRedirectCreated = -1;
    sRedirectFellBack = 0;
    sRedirectRanOn = -1;
    __dmb();
    sRedirectCore = core;
    __dmb();
}

int gdx3ds_thread_redirect_end(int* fellBack) {
    sRedirectCore = -1;
    __dmb();
    if (fellBack != NULL) {
        *fellBack = sRedirectFellBack;
    }
    return sRedirectCreated;
}

int gdx3ds_thread_redirect_ran_on(void) {
    __dmb();
    return sRedirectRanOn;
}

Thread __wrap_threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int core_id,
                           bool detached) {
    if (sRedirectCore >= 0) {
        const int redirect = __atomic_exchange_n(&sRedirectCore, -1, __ATOMIC_ACQ_REL);
        if (redirect >= 0) {
            return CreateRedirected(entrypoint, arg, stack_size, prio, core_id, detached, redirect);
        }
    }
    if (!sEnabled) {
        return __real_threadCreate(entrypoint, arg, stack_size, prio, core_id, detached);
    }
    const int idx = __atomic_fetch_add(&sThunkNext, 1, __ATOMIC_ACQ_REL);
    if (idx >= GDX3DS_CRASH_THUNKS) {
        return __real_threadCreate(entrypoint, arg, stack_size, prio, core_id, detached);
    }
    sThunks[idx].entry = entrypoint;
    sThunks[idx].arg = arg;
    return __real_threadCreate(CrashThunkEntry, &sThunks[idx], stack_size, prio, core_id, detached);
}

/* ---- --wrap=svcBreak: name every library panic before it happens ------------------------- */

Result __real_svcBreak(UserBreakType breakReason);

static volatile int sBreakDepth = 0;

Result __wrap_svcBreak(UserBreakType breakReason) {
    const u32 ra = (u32)__builtin_return_address(0);
    if (__atomic_fetch_add(&sBreakDepth, 1, __ATOMIC_ACQ_REL) != 0) {
        /* The log path itself broke (e.g. __syscall_getreent on a thread without libctru
         * thread vars): do not recurse, just break. */
        return __real_svcBreak(breakReason);
    }
    char line[96];
    size_t n = 0;
    const char* head = "[fatal] svcBreak reason=";
    while (*head != '\0') {
        line[n++] = *head++;
    }
    line[n++] = (char)('0' + ((u32)breakReason % 10u));
    const char* mid = " ra=0x";
    while (*mid != '\0') {
        line[n++] = *mid++;
    }
    static const char kHex[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        line[n++] = kHex[(ra >> (i * 4)) & 0xFu];
    }
    const char* tail = " (addr2line -f -C -e G-Diffuser-3DS.elf <ra>)";
    while (*tail != '\0' && n + 1 < sizeof(line)) {
        line[n++] = *tail++;
    }
    gdx3ds_filelog_write(line, n);
    gdx3ds_filelog_flush_sync();
    if (sExiting || aptShouldClose()) {
        /* HEAPFIX: a break during the teardown, or with a HOME close order pending, hard-locked an
         * Old 3DS (card-5: std::terminate in the exit chain's disk-save flush -> svcBreak with the
         * HOME menu holding the screens; no ErrDisp, no dump, power button only). The line above
         * is on the card; end the process the way CrashHandler already does for a teardown fault,
         * so the close the user asked for completes. */
        static const char kExitLine[] = "[fatal] svcBreak during teardown/close: svcExitProcess instead";
        gdx3ds_filelog_write(kExitLine, sizeof(kExitLine) - 1);
        gdx3ds_filelog_flush_sync();
        svcExitProcess();
    }
    return __real_svcBreak(breakReason);
}

/* ---- [debug] boot_breadcrumbs = N: the decomp's own boot checkpoints for N host frames ---- */

static unsigned long sBreadcrumbFrames = 0;
static int sBreadcrumbPrev = 0;
static int sBreadcrumbOn = 0;

void gdx3ds_crash_breadcrumbs_arm(void) {
    const int frames = gdx3ds_config_get_int("debug", "boot_breadcrumbs", 0);
    if (frames <= 0) {
        return;
    }
    sBreadcrumbFrames = (unsigned long)frames;
    sBreadcrumbPrev = gGdxDevGateCache[GDX_GATE_DIAG_VERBOSE];
    gGdxDevGateCache[GDX_GATE_DIAG_VERBOSE] = 1;
    sBreadcrumbOn = 1;
    static const char kOn[] = "[crash] boot breadcrumbs ON (GDX_DIAG_VERBOSE gate: sys_gfx/sys_main "
                              "[game] ck lines) until host frame debug.boot_breadcrumbs";
    gdx3ds_filelog_write(kOn, sizeof(kOn) - 1);
}

void gdx3ds_crash_breadcrumbs_frame(unsigned long frame) {
    if (!sBreadcrumbOn || frame < sBreadcrumbFrames) {
        return;
    }
    gGdxDevGateCache[GDX_GATE_DIAG_VERBOSE] = sBreadcrumbPrev;
    sBreadcrumbOn = 0;
    static const char kOff[] = "[crash] boot breadcrumbs OFF (gate restored)";
    gdx3ds_filelog_write(kOff, sizeof(kOff) - 1);
}
