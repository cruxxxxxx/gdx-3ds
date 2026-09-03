/* port/3ds/harness/link_stubs.cpp — link-time stubs for the DL test app.
 *
 * These close exactly the non-libzip families from the unresolved-symbol audit
 * in port/3ds/gfx/STATUS.md (libzip itself is closed by linking stream D's
 * gdx3ds_zipshim). Scope: THIS test executable only — the real 3DS app links
 * the port sources that own these symbols, and whether any of these stubs
 * graduate into the integration build is an orchestrator/stream-E decision.
 *
 *  1. Ship::Context — GetInstance / GetResourceManager / GetConsoleVariables.
 *     Per STATUS.md: a shim TU owning the singleton, NOT src/ship/Context.cpp
 *     in the carve (that would drag Window/Console/CrashHandler). The instance
 *     is never constructed (no carved constructor exists); it lives in static
 *     storage behind a no-op deleter, which is safe because the two accessors
 *     defined here are the only members ever called on it and neither touches
 *     object state.
 *  2. Ship::ConsoleVariable — GetFloat / GetInteger return the caller-provided
 *     default (the 3DS has no ImGui CVar editor; defaults are the contract).
 *     Same never-constructed-instance pattern, same justification.
 *  3. CVar C API — CVarGetFloat / CVarGetInteger, thin default-returning
 *     wrappers (same TU per the STATUS.md recommendation).
 *  4. G-Diffuser port helpers referenced by this fork's interpreter.cpp —
 *     gdx_dbg_logf (forward to stderr), gdx_workshop_texture_dump_enabled
 *     (off) and gdx_workshop_dump_texture (unreachable while disabled).
 *
 * No additions beyond the audited families were needed.
 */

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <memory>

#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "libultraship/bridge/consolevariablebridge.h"

/* ------------------------------------------------------------------------- */
/* 1. Ship::Context                                                           */
/* ------------------------------------------------------------------------- */

namespace Ship {

static std::shared_ptr<Context>& StubContext() {
    /* Raw storage, never constructed/destroyed: Context's own ctor lives in
     * the un-carved Context.cpp. Nothing here reads members. */
    alignas(Context) static unsigned char sStorage[sizeof(Context)];
    static std::shared_ptr<Context> sInstance(reinterpret_cast<Context*>(sStorage),
                                              [](Context*) {});
    return sInstance;
}

std::shared_ptr<Context> Context::GetInstance() {
    return StubContext();
}

std::shared_ptr<ResourceManager> Context::GetResourceManager() const {
    /* Empty: the harness feeds raw host pointers, never OTR resource paths,
     * so no interpreter path that dereferences the resource manager runs. */
    return nullptr;
}

std::shared_ptr<ConsoleVariable> Context::GetConsoleVariables() const {
    alignas(ConsoleVariable) static unsigned char sStorage[sizeof(ConsoleVariable)];
    static std::shared_ptr<ConsoleVariable> sVars(reinterpret_cast<ConsoleVariable*>(sStorage),
                                                  [](ConsoleVariable*) {});
    return sVars;
}

/* ------------------------------------------------------------------------- */
/* 2. Ship::ConsoleVariable                                                   */
/* ------------------------------------------------------------------------- */

int32_t ConsoleVariable::GetInteger(const char* name, int32_t defaultValue) {
    (void)name;
    return defaultValue;
}

float ConsoleVariable::GetFloat(const char* name, float defaultValue) {
    (void)name;
    return defaultValue;
}

} // namespace Ship

/* ------------------------------------------------------------------------- */
/* 3. CVar C API                                                              */
/* ------------------------------------------------------------------------- */

int32_t CVarGetInteger(const char* name, int32_t defaultValue) {
    (void)name;
    return defaultValue;
}

float CVarGetFloat(const char* name, float defaultValue) {
    (void)name;
    return defaultValue;
}

/* ------------------------------------------------------------------------- */
/* 4. G-Diffuser port helpers                                                 */
/* ------------------------------------------------------------------------- */

extern "C" void gdx_dbg_logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

extern "C" int gdx_workshop_texture_dump_enabled(void) {
    return 0;
}

extern "C" void gdx_workshop_dump_texture(const void* origSrcAddr, size_t origSrcLen,
                                          const char* resourcePathOrNull, const unsigned char* rgba32,
                                          int width, int height, int n64Fmt, int n64Siz) {
    (void)origSrcAddr;
    (void)origSrcLen;
    (void)resourcePathOrNull;
    (void)rgba32;
    (void)width;
    (void)height;
    (void)n64Fmt;
    (void)n64Siz;
}
