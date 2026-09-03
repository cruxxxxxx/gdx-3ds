# LUS carve stubs (Phase 0 output, orchestrator-owned)

Shadow headers that satisfy libultraship's desktop header leaks when compiling the
carve for 3DS (see docs/research/spike-lus-carve-report.md). Include this directory
BEFORE libultraship/include. Real third-party headers (nlohmann/json.hpp,
BS_thread_pool.hpp, tinyxml2.h) are NOT vendored here — fetch them at configure time
or vendor under your stream's directory.

port/3ds/patches/lus-newlib-portability.patch must be applied to the libultraship/
submodule working tree (git apply) before compiling the carve; it lands in the LUS
fork properly during integration.
