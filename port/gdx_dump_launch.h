// port/gdx_dump_launch.h — per-class offline "Dump All" launcher.
//
// The in-game Workshop "Asset Dump" section (port/gdx_menu.cpp) runs one child process PER SELECTED
// CLASS so a single broken class can never abort the rest of the batch. This header is the seam
// between the ImGui UI (which only READS the shared snapshot) and the detached worker thread that
// spawns the children.
//
// Backends, in preference order: the native `gdx-extract.exe` next to the game exe drives everything
// (the --list-classes probe and the per-class runs) with no Python; a Python interpreter +
// tools/gen_dump_all.py covers source checkouts where that binary has not been built/copied yet. With
// neither available the section renders disabled with one plain line, never an error popup. Nothing
// here writes outside the caller-supplied dumpDir; AppData/XDG are never touched.

#pragma once

#include <string>
#include <vector>

namespace gdx {

// Which backend the launcher discovered. Native is preferred; Python is the dev fallback.
enum class DumpBackend { None, Native, Python };

// ── Environment discovery ─────────────────────────────────────────────────────────────────────────
// Located once at first open. Pure filesystem + PATH inspection — no subprocess is spawned here, so
// it is safe to call from the render thread.
struct DumpEnvironment {
    bool available = false;              // true when a usable backend (native or python) was found
    DumpBackend backend = DumpBackend::None;

    // ── Native backend (preferred) ──
    std::string extractBin;              // absolute path to gdx-extract.exe next to the game exe
    // Explicitly resolved source paths passed to `gdx-extract dump` (empty == omit the flag; the tool
    // has graceful per-class behavior for absent optional sources). archivePath is resolved to the
    // REAL cart archive (fzerox.o2r in the data dir, else the dev tree's assets/extracted/generic.o2r)
    // and NEVER to the gdiffuser.o2r port-assets placeholder.
    std::string archivePath;             // --archive
    std::string diskArchivePath;         // --disk-archive (optional)
    std::string iplArchivePath;          // --ipl-archive  (optional)
    std::string recipesDir;              // --recipes      (optional)
    std::string manifestPath;            // --manifest     (optional)
    std::string romPath;                 // --rom          (optional; the tool is archive-only capable)

    // ── Python backend (dev fallback) ──
    std::string pythonExe;               // "python"/"py"/"python3", or an absolute path
    std::string toolPath;                // absolute path to tools/gen_dump_all.py

    std::string reason;                  // one-line explanation shown in the UI when !available
};

// Discover the dump backend. First looks for gdx-extract.exe next to the game exe (same convention as
// port/gdx_extract_launch.cpp); if found, resolves the explicit source paths and selects the native
// backend. Otherwise falls back to a Python interpreter (PATH) + tools/gen_dump_all.py (next to the
// exe, then walking up to 6 parents to cover the dev tree where the exe is build/x64/port/Release/).
DumpEnvironment GdxDumpDiscover();

// ── Class list ────────────────────────────────────────────────────────────────────────────────────
// Hardcoded fallback used when the `--list-classes` probe is unavailable or fails. Kept in sync with
// the native registry (dump_all.cpp) and tools/gen_dump_all.py's CLASS_REGISTRY (order is display
// order).
std::vector<std::string> GdxDumpFallbackClasses();

// Pretty display name for a raw class token (e.g. "audio" -> "Audio Samples (WAV)"). Unknown tokens
// are returned title-cased so extra classes from --list-classes still read cleanly.
std::string GdxDumpPrettyName(const std::string& rawClass);

// Kick a one-shot background probe of the discovered backend's `--list-classes`. The current class
// list starts as the fallback and is atomically replaced if the probe returns a non-empty list. Safe
// to call repeatedly; only the first call per process does anything. No-op when env is unavailable.
void GdxDumpBeginClassListProbe(const DumpEnvironment& env);

// The current class list (fallback until/unless the probe upgrades it). Render-thread safe.
std::vector<std::string> GdxDumpCurrentClasses();

// ── Per-class progress (UI reads snapshots of this) ─────────────────────────────────────────────────
enum class DumpPhase { Idle, Queued, Running, Done, Failed };

struct DumpClassProgress {
    std::string name;             // raw class token (the --classes argument)
    DumpPhase phase = DumpPhase::Idle;
    int exitCode = 0;             // child exit code (valid once phase is Done/Failed)
    int itemsDumped = -1;         // parsed from stdout when possible, else -1
    double elapsedSeconds = 0.0;  // wall-clock for Running/Done/Failed
    std::string lastLine;         // last stdout/stderr line, truncated (diagnostic)
};

struct DumpBatchSnapshot {
    bool running = false;
    bool cancelRequested = false;
    std::vector<DumpClassProgress> classes;
    std::string summary;          // filled when the batch ends
};

// ── Batch runner ────────────────────────────────────────────────────────────────────────────────────
// Spawns a DETACHED worker that runs one dump child PER CLASS, sequentially. Never blocks the caller.
// No-op if a batch is already running. Per-class failure is isolated: a non-zero child exit is recorded
// and the worker moves to the next class.
void GdxDumpStartBatch(const DumpEnvironment& env, const std::vector<std::string>& classes,
                       const std::string& dumpDir);

bool GdxDumpBatchRunning();

// Cooperative cancel: the worker stops AFTER the current class finishes, and no child is killed, so
// cancel latency equals the current class's remaining runtime.
void GdxDumpRequestCancel();

// Latest snapshot for the UI. Render-thread safe (guarded internally).
DumpBatchSnapshot GdxDumpSnapshot();

// ── Core primitive (also used by the env-gated self-test) ───────────────────────────────────────────
// Run ONE class synchronously: spawn the child, capture its exit code and last output line, and best-
// effort parse an item count. Returns true iff the child exited 0. Blocks the calling thread until the
// child exits — call from a worker, never from the render thread.
bool GdxDumpRunOneClass(const DumpEnvironment& env, const std::string& className,
                        const std::string& dumpDir, DumpClassProgress& out);

} // namespace gdx
