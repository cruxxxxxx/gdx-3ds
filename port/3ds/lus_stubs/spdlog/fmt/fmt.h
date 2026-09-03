/* Spike stub: minimal fmt shims spdlog call sites use. */
#pragma once
#include <string>
#include <sstream>

namespace fmt {
template <typename... Args> inline std::string format(const std::string& f, Args&&...) {
    return f; /* spike: formatting fidelity irrelevant, only compilation matters */
}
template <typename T> inline const void* ptr(const T* p) {
    return static_cast<const void*>(p);
}
} // namespace fmt
