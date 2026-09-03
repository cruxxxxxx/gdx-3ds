// port/3ds/lus_glue/gdx3ds_libultra.cpp — the libultra surface libultraship's
// src/libultraship/libultra/*.cpp + bridge TUs provide on desktop, for the carved 3DS
// build. Each block names its desktop source; behavior is a straight adaptation with
// the SDL/ControlDeck/EventSystem dependencies replaced by the 3DS contracts.

#include "libultraship/libultra/os.h"
#include "libultraship/libultra/controller.h"
#include "libultraship/libultra/pfs.h"
#include "libultraship/libultra/motor.h"
#include "libultraship/bridge/eventsbridge.h"

#include "gdx3ds_os.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ratio>

// N64 46.875 MHz counter rate, exactly as libultraship/src/libultraship/libultra/os.cpp.
typedef std::ratio<3000, 64> n64ClockRatio;
typedef std::ratio_divide<std::micro, n64ClockRatio> n64CycleRate;
typedef std::chrono::duration<long long, n64CycleRate> n64CycleRateDuration;

extern "C" {

// ── os_cache.cpp: cache ops are no-ops on a coherent host ─────────────────────────────
void osWritebackDCacheAll() {
}

void osInvalICache(void* p, int32_t x) {
    (void)p;
    (void)x;
}

void osWritebackDCache(void* p, int32_t x) {
    (void)p;
    (void)x;
}

void osInvalDCache(void* p, int32_t l) {
    (void)p;
    (void)l;
}

// ── os.cpp: time ──────────────────────────────────────────────────────────────────────
uint8_t __osMaxControllers = MAXCONTROLLERS;
uint64_t __osCurrentTime = 0;

// NOT named osGetTime/osSetTime: libctru's os.o exports an osGetTime (ms since 2000)
// and is always in the link (osSetSpeedupEnable et al.), so those names would be a
// multiple-definition error. The decomp's calls are routed here by the
// GDX_PLATFORM_3DS alias in decomp/include/PR/os_time.h (decomp-ilp32.patch).
void gdx3ds_osSetTime(OSTime time) {
    __osCurrentTime =
        std::chrono::duration_cast<n64CycleRateDuration>(std::chrono::steady_clock::now().time_since_epoch()).count() +
        time;
}

uint64_t gdx3ds_osGetTime() {
    return std::chrono::duration_cast<n64CycleRateDuration>(std::chrono::steady_clock::now().time_since_epoch())
               .count() -
           __osCurrentTime;
}

uint32_t osGetCount() {
    return std::chrono::duration_cast<n64CycleRateDuration>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int osSetTimer(OSTimer* t, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    (void)t;
    (void)countdown;
    (void)interval;
    (void)mq;
    (void)msg;
    return 0;
}

// ── os.cpp / os_pi.cpp: cart + PI manager ─────────────────────────────────────────────
// Same decomp-vs-LUS OSIoMesg shim as desktop os.cpp: the decomp's OSIoMesg layout is
// read through this struct, and completion posts to the retQueue.
struct GdxDecompOSIoMesg {
    OSIoMesgHdr hdr;
    void* dramAddr;
    uint32_t devAddr;
    uint32_t size;
    OSPiHandle* piHandle;
};

// port/gdx_segment_source.c — archive-first cart reads with raw-ROM fallback.
int GdxSegmentSourceRead(uint32_t romBase, uint32_t size, void* dst);

OSPiHandle* osCartRomInit() {
    static OSPiHandle sCartRomHandle = {};
    sCartRomHandle.type = DEVICE_TYPE_CART;
    sCartRomHandle.domain = PI_DOMAIN1;
    return &sCartRomHandle;
}

void osCreatePiManager(OSPri pri, OSMesgQueue* cmdQ, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    // Desktop os_pi.cpp is a no-op too: osEPiStartDma completes synchronously.
    (void)pri;
    (void)cmdQ;
    (void)cmdBuf;
    (void)cmdMsgCnt;
}

s32 osEPiStartDma(OSPiHandle* pihandle, OSIoMesg* mb, s32 direction) {
    auto* decompMesg = reinterpret_cast<GdxDecompOSIoMesg*>(mb);
    if (decompMesg != nullptr) {
        decompMesg->piHandle = pihandle;

        if (direction == OS_READ && decompMesg->dramAddr != nullptr && decompMesg->size != 0) {
            const uint32_t romOffset = decompMesg->devAddr & 0x0FFFFFFFu;
            if (!GdxSegmentSourceRead(romOffset, decompMesg->size, decompMesg->dramAddr)) {
                std::memset(decompMesg->dramAddr, 0, decompMesg->size);
            }
        }

        if (decompMesg->hdr.retQueue != nullptr) {
            osSendMesg(decompMesg->hdr.retQueue, OS_MESG_PTR(mb), OS_MESG_NOBLOCK);
        }
    }
    return 0;
}

// ── os.cpp: controllers (HID via stream B instead of the LUS ControlDeck) ─────────────
int32_t osContInit(OSMesgQueue* mq, uint8_t* controllerBits, OSContStatus* status) {
    (void)mq;
    *controllerBits = 1; // built-in HID on port 0; no hotplug on 3DS
    status->status |= 1;
    return 0;
}

int32_t osContStartReadData(OSMesgQueue* mesg) {
    (void)mesg;
    return 0;
}

void osContGetReadData(OSContPad* pad) {
    // The CALLER is the decomp (src/sys/controller.c), whose OSContPad is the original
    // 6-byte { u16 button; s8 stick_x; s8 stick_y; u8 errno; } — NOT libultraship's
    // 0x24-byte gyro-extended struct this TU's prototype names. Write the caller's
    // layout, or pads 1..3 and the adjacent globals get scribbled at the wrong stride
    // (desktop LUS os.cpp writes its own shape here; on 3DS the decomp is the only
    // caller, so the decomp layout is the correct ABI).
    struct GdxDecompContPad {
        uint16_t button;
        int8_t stick_x;
        int8_t stick_y;
        uint8_t err;
    };
    auto* out = reinterpret_cast<GdxDecompContPad*>(pad);
    std::memset(out, 0, sizeof(GdxDecompContPad) * __osMaxControllers);

    Gdx3dsPadState pads[MAXCONTROLLERS] = {};
    gdx3ds_os_poll_input(pads, MAXCONTROLLERS);
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        out[i].button = pads[i].buttons;
        out[i].stick_x = pads[i].stickX;
        out[i].stick_y = pads[i].stickY;
        out[i].err = pads[i].connected ? 0 : 0x8; // CONT_NO_RESPONSE_ERROR
    }
}

// ── os.cpp: rumble — no Rumble Pak on 3DS ─────────────────────────────────────────────
s32 __osMotorAccess(OSPfs* pfs, u32 vibrate) {
    (void)pfs;
    (void)vibrate;
    return 0;
}

s32 osMotorInit(OSMesgQueue* ctrlrqueue, OSPfs* pfs, s32 channel) {
    (void)ctrlrqueue;
    (void)channel;
    if (pfs != nullptr) {
        pfs->status = 0;
    }
    return 0; // report success; access calls are no-ops
}

// ── bridge/windowbridge.cpp ───────────────────────────────────────────────────────────
float WindowGetAspectRatio() {
    return 400.0f / 240.0f; // fixed top screen
}

// ── bridge/eventsbridge.cpp: no EventSystem in the carve; IDs stay -1 and calls no-op.
// port/enhancements/events/GameEvents.cpp tolerates that (its listeners simply never
// fire — the enhancement hooks are desktop features).
EventID EventSystemRegisterEvent(const char* name) {
    (void)name;
    return -1;
}

void EventSystemCallEvent(EventID id, void* event, const char* file, int line, const char* key) {
    (void)id;
    (void)event;
    (void)file;
    (void)line;
    (void)key;
}

} // extern "C"
