// port/3ds/lus_glue/gdx3ds_cvar_stub.cpp — Ship::ConsoleVariable + the CVar C bridge
// for the 3DS build, replacing src/ship/config/ConsoleVariable.cpp +
// src/libultraship/bridge/consolevariablebridge.cpp at link.
//
// The class itself is a self-contained name->CVar map, so the in-memory behavior is
// implemented for real (setters/getters/registers work; the port bridge sets and reads
// CVars at runtime). Only persistence differs from desktop: Save()/Load() are no-ops —
// 3DS user configuration lives in stream B's INI (gdx3ds_config), not LUS's JSON
// Config, and defaults are what M1 boots with.

#include "ship/config/ConsoleVariable.h"
#include "libultraship/bridge/consolevariablebridge.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace Ship {

ConsoleVariable::ConsoleVariable() {
}

ConsoleVariable::~ConsoleVariable() {
}

std::shared_ptr<CVar> ConsoleVariable::Get(const char* name) {
    auto it = mVariables.find(std::string_view(name));
    return (it != mVariables.end()) ? it->second : nullptr;
}

int32_t ConsoleVariable::GetInteger(const char* name, int32_t defaultValue) {
    auto var = Get(name);
    if (var != nullptr && var->Type == ConsoleVariableType::Integer) {
        return var->Integer;
    }
    return defaultValue;
}

float ConsoleVariable::GetFloat(const char* name, float defaultValue) {
    auto var = Get(name);
    if (var != nullptr && var->Type == ConsoleVariableType::Float) {
        return var->Float;
    }
    return defaultValue;
}

const char* ConsoleVariable::GetString(const char* name, const char* defaultValue) {
    auto var = Get(name);
    if (var != nullptr && var->Type == ConsoleVariableType::String && var->String != nullptr) {
        return var->String;
    }
    return defaultValue;
}

Color_RGBA8 ConsoleVariable::GetColor(const char* name, Color_RGBA8 defaultValue) {
    auto var = Get(name);
    if (var != nullptr && var->Type == ConsoleVariableType::Color) {
        return var->Color;
    }
    return defaultValue;
}

Color_RGB8 ConsoleVariable::GetColor24(const char* name, Color_RGB8 defaultValue) {
    auto var = Get(name);
    if (var != nullptr && var->Type == ConsoleVariableType::Color24) {
        return var->Color24;
    }
    return defaultValue;
}

// Each setter allocates a fresh CVar so a type change never leaves a stale union member
// (matches the desktop implementation's semantics; the CVar dtor frees String).
static std::shared_ptr<CVar> MakeVar(ConsoleVariableType type) {
    auto var = std::make_shared<CVar>();
    var->Type = type;
    return var;
}

void ConsoleVariable::SetInteger(const char* name, int32_t value) {
    auto var = MakeVar(ConsoleVariableType::Integer);
    var->Integer = value;
    mVariables[name] = var;
}

void ConsoleVariable::SetFloat(const char* name, float value) {
    auto var = MakeVar(ConsoleVariableType::Float);
    var->Float = value;
    mVariables[name] = var;
}

void ConsoleVariable::SetString(const char* name, const char* value) {
    auto var = MakeVar(ConsoleVariableType::String);
    var->String = strdup(value != nullptr ? value : "");
    mVariables[name] = var;
}

void ConsoleVariable::SetColor(const char* name, Color_RGBA8 value) {
    auto var = MakeVar(ConsoleVariableType::Color);
    var->Color = value;
    mVariables[name] = var;
}

void ConsoleVariable::SetColor24(const char* name, Color_RGB8 value) {
    auto var = MakeVar(ConsoleVariableType::Color24);
    var->Color24 = value;
    mVariables[name] = var;
}

void ConsoleVariable::RegisterInteger(const char* name, int32_t defaultValue) {
    if (Get(name) == nullptr) {
        SetInteger(name, defaultValue);
    }
}

void ConsoleVariable::RegisterFloat(const char* name, float defaultValue) {
    if (Get(name) == nullptr) {
        SetFloat(name, defaultValue);
    }
}

void ConsoleVariable::RegisterString(const char* name, const char* defaultValue) {
    if (Get(name) == nullptr) {
        SetString(name, defaultValue);
    }
}

void ConsoleVariable::RegisterColor(const char* name, Color_RGBA8 defaultValue) {
    if (Get(name) == nullptr) {
        SetColor(name, defaultValue);
    }
}

void ConsoleVariable::RegisterColor24(const char* name, Color_RGB8 defaultValue) {
    if (Get(name) == nullptr) {
        SetColor24(name, defaultValue);
    }
}

void ConsoleVariable::ClearVariable(const char* name) {
    mVariables.erase(name);
}

void ConsoleVariable::CopyVariable(const char* from, const char* to) {
    auto var = Get(from);
    if (var == nullptr) {
        return;
    }
    switch (var->Type) {
        case ConsoleVariableType::Integer: SetInteger(to, var->Integer); break;
        case ConsoleVariableType::Float: SetFloat(to, var->Float); break;
        case ConsoleVariableType::String: SetString(to, var->String); break;
        case ConsoleVariableType::Color: SetColor(to, var->Color); break;
        case ConsoleVariableType::Color24: SetColor24(to, var->Color24); break;
    }
}

void ConsoleVariable::Save() {
    // No JSON Config on 3DS; user settings persist via stream B's INI instead.
}

void ConsoleVariable::Load() {
}

} // namespace Ship

// ── C bridge ────────────────────────────────────────────────────────────────────────

#include "ship/Context.h"

static std::shared_ptr<Ship::ConsoleVariable> GdxCVars() {
    auto ctx = Ship::Context::GetInstance();
    return (ctx != nullptr) ? ctx->GetConsoleVariables() : nullptr;
}

std::shared_ptr<Ship::CVar> CVarGet(const char* name) {
    auto cvars = GdxCVars();
    return (cvars != nullptr) ? cvars->Get(name) : nullptr;
}

extern "C" {

int32_t CVarGetInteger(const char* name, int32_t defaultValue) {
    auto cvars = GdxCVars();
    return (cvars != nullptr) ? cvars->GetInteger(name, defaultValue) : defaultValue;
}

float CVarGetFloat(const char* name, float defaultValue) {
    auto cvars = GdxCVars();
    return (cvars != nullptr) ? cvars->GetFloat(name, defaultValue) : defaultValue;
}

const char* CVarGetString(const char* name, const char* defaultValue) {
    auto cvars = GdxCVars();
    return (cvars != nullptr) ? cvars->GetString(name, defaultValue) : defaultValue;
}

Color_RGBA8 CVarGetColor(const char* name, Color_RGBA8 defaultValue) {
    auto cvars = GdxCVars();
    return (cvars != nullptr) ? cvars->GetColor(name, defaultValue) : defaultValue;
}

Color_RGB8 CVarGetColor24(const char* name, Color_RGB8 defaultValue) {
    auto cvars = GdxCVars();
    return (cvars != nullptr) ? cvars->GetColor24(name, defaultValue) : defaultValue;
}

void CVarSetInteger(const char* name, int32_t value) {
    if (auto cvars = GdxCVars()) {
        cvars->SetInteger(name, value);
    }
}

void CVarSetFloat(const char* name, float value) {
    if (auto cvars = GdxCVars()) {
        cvars->SetFloat(name, value);
    }
}

void CVarSetString(const char* name, const char* value) {
    if (auto cvars = GdxCVars()) {
        cvars->SetString(name, value);
    }
}

void CVarSetColor(const char* name, Color_RGBA8 value) {
    if (auto cvars = GdxCVars()) {
        cvars->SetColor(name, value);
    }
}

void CVarSetColor24(const char* name, Color_RGB8 value) {
    if (auto cvars = GdxCVars()) {
        cvars->SetColor24(name, value);
    }
}

void CVarRegisterInteger(const char* name, int32_t defaultValue) {
    if (auto cvars = GdxCVars()) {
        cvars->RegisterInteger(name, defaultValue);
    }
}

void CVarRegisterFloat(const char* name, float defaultValue) {
    if (auto cvars = GdxCVars()) {
        cvars->RegisterFloat(name, defaultValue);
    }
}

void CVarRegisterString(const char* name, const char* defaultValue) {
    if (auto cvars = GdxCVars()) {
        cvars->RegisterString(name, defaultValue);
    }
}

void CVarRegisterColor(const char* name, Color_RGBA8 defaultValue) {
    if (auto cvars = GdxCVars()) {
        cvars->RegisterColor(name, defaultValue);
    }
}

void CVarRegisterColor24(const char* name, Color_RGB8 defaultValue) {
    if (auto cvars = GdxCVars()) {
        cvars->RegisterColor24(name, defaultValue);
    }
}

void CVarClear(const char* name) {
    if (auto cvars = GdxCVars()) {
        cvars->ClearVariable(name);
    }
}

bool CVarExists(const char* name) {
    return CVarGet(name) != nullptr;
}

void CVarClearBlock(const char* name) {
    // Desktop clears every CVar under a prefix via the Config JSON tree; nothing
    // depends on this at M1 and there is no JSON tree here.
    (void)name;
}

void CVarCopy(const char* from, const char* to) {
    if (auto cvars = GdxCVars()) {
        cvars->CopyVariable(from, to);
    }
}

void CVarLoad() {
    if (auto cvars = GdxCVars()) {
        cvars->Load();
    }
}

void CVarSave() {
    if (auto cvars = GdxCVars()) {
        cvars->Save();
    }
}

} // extern "C"
