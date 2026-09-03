// port/3ds/lus_glue/gdx3ds_context_stub.cpp — link-level replacement for
// libultraship's src/ship/Context.cpp on 3DS.
//
// The carve (port/3ds/gfx/CMakeLists.txt) deliberately excludes Context.cpp: compiling
// it would drag Window-backend selection, Console, CrashHandler and the spdlog logger
// into the 3DS build (see the unresolved-symbol audit in port/3ds/gfx/STATUS.md, which
// recommends exactly this shim). This TU defines the Ship::Context members the carved
// interpreter/resource layer and the port bridge actually reference:
//   - the singleton (GetInstance / CreateUninitializedInstance / ctor / dtor)
//   - GetResourceManager / GetConsoleVariables / GetWindow / GetConfig accessors
//   - InitResourceManager (REAL: constructs the carved Ship::ResourceManager over the
//     SD archive paths), InitConsoleVariables (stub ConsoleVariable, see
//     gdx3ds_cvar_stub.cpp), InitWindow (stores + Init()s the provided window)
// Everything else from Context.cpp (logging, console, crash handler, audio, control
// deck, file drop, event system) intentionally has NO definition here — if a new
// reference appears at link time it must be a conscious decision, not silent creep.

#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/resource/ResourceManager.h"
#include "ship/window/Window.h"

#include <cstdio>

namespace Ship {

std::weak_ptr<Context> Context::mContext;

Context::Context(std::string name, std::string shortName, std::string configFilePath)
    : mConfigFilePath(std::move(configFilePath)), mName(std::move(name)), mShortName(std::move(shortName)) {
}

Context::~Context() {
    // Desktop Context.cpp saves the window config and tears subsystems down in order;
    // the 3DS process lifetime is the application lifetime, so member destructors are
    // enough here.
}

std::shared_ptr<Context> Context::GetInstance() {
    return mContext.lock();
}

std::shared_ptr<Context> Context::CreateUninitializedInstance(const std::string& name, const std::string& shortName,
                                                              const std::string& configFilePath) {
    if (!mContext.expired()) {
        std::fprintf(stderr, "[lus_glue] CreateUninitializedInstance called with a live Context\n");
        return nullptr;
    }
    auto ctx = std::make_shared<Context>(name, shortName, configFilePath);
    mContext = ctx;
    return ctx;
}

std::string Context::GetName() const {
    return mName;
}

std::string Context::GetShortName() const {
    return mShortName;
}

std::shared_ptr<Config> Context::GetConfig() const {
    return mConfig; // always null on 3DS: config comes from stream B's INI, not LUS Config
}

std::shared_ptr<ConsoleVariable> Context::GetConsoleVariables() const {
    return mConsoleVariables;
}

std::shared_ptr<ResourceManager> Context::GetResourceManager() const {
    return mResourceManager;
}

std::shared_ptr<Window> Context::GetWindow() const {
    return mWindow;
}

bool Context::InitConsoleVariables() {
    if (GetConsoleVariables() != nullptr) {
        return true;
    }
    mConsoleVariables = std::make_shared<ConsoleVariable>(); // stubbed: gdx3ds_cvar_stub.cpp
    return true;
}

bool Context::InitResourceManager(const std::vector<std::string>& archivePaths,
                                  const std::unordered_set<uint32_t>& validHashes, uint32_t reservedThreadCount,
                                  const bool allowEmptyPaths) {
    if (GetResourceManager() != nullptr) {
        return true;
    }
    mResourceManager = std::make_shared<ResourceManager>();
    mResourceManager->Init(archivePaths, validHashes, reservedThreadCount);
    if (!allowEmptyPaths && !mResourceManager->IsLoaded()) {
        std::fprintf(stderr, "[lus_glue] ResourceManager: no archive mounted (paths=%zu)\n", archivePaths.size());
        return false;
    }
    return true;
}

bool Context::InitWindow(std::shared_ptr<Window> window) {
    if (GetWindow() != nullptr) {
        return true;
    }
    if (window == nullptr) {
        return false; // no default backend selection on 3DS; main_3ds.cpp constructs it
    }
    mWindow = window;
    mWindow->Init();
    return true;
}

} // namespace Ship
