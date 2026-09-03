// port/gdx_console_log.cpp — routes the port's log output into libultraship's Console window.
//
// libultraship registers the Console (Gui.cpp) but nothing outside ConsoleWindow itself calls
// ConsoleWindow::Append, so it only ever showed echoes of typed commands. This supplies the feed
// from both of the port's streams: gdx_port_logf (port_log.h, on by default) and spdlog
// (libultraship's own, silent unless GDX_LOG makes main.cpp raise the level).
//
// Producers are any thread or fiber, the consumer is the ImGui thread, so lines queue here and are
// handed over during the frame.

#include "gdx_console_log.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/ConsoleWindow.h"
#include "ship/window/gui/Gui.h"

#include "gdx_dev_gates.h" // gdx_port_log_tap

namespace {

struct PendingLine {
    std::string text;
    spdlog::level::level_enum level;
};

struct Queue {
    std::mutex mutex;
    std::deque<PendingLine> pending;
};

// Deliberately never destroyed: libultraship still logs from its own static destructors, and the
// relative teardown order of those and a file-scope object here is unspecified.
Queue& TheQueue() {
    static Queue* queue = new Queue();
    return *queue;
}

size_t sWindowLines = 0;

constexpr size_t kPendingCap = 1024; // lines held while no Console window exists yet
constexpr size_t kWindowCap = 2000;  // lines held by the window itself

void Push(const char* text, size_t length, spdlog::level::level_enum level) {
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        --length;
    }
    if (length == 0) {
        return;
    }

    // try_lock, not lock: the crash handler logs too, and it can run on the very thread that is
    // mid-drain. A dropped console line beats a deadlocked crash report — the line still reaches
    // stderr and the run log either way.
    Queue& queue = TheQueue();
    std::unique_lock<std::mutex> lock(queue.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    if (queue.pending.size() >= kPendingCap) {
        queue.pending.pop_front();
    }
    queue.pending.push_back({ std::string(text, length), level });
}

class ConsoleSink final : public spdlog::sinks::base_sink<std::mutex> {
  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        Push(formatted.data(), formatted.size(), msg.level);
    }

    void flush_() override {
    }
};

} // namespace

extern "C" void GdxConsoleLogPortTap(const char* message) {
    if (message != nullptr) {
        Push(message, std::char_traits<char>::length(message), spdlog::level::info);
    }
}

void GdxConsoleLogInstall() {
    gdx_port_log_tap = &GdxConsoleLogPortTap;

    // Records only arrive if the logger's level admits them, and main.cpp leaves spdlog off
    // unless GDX_LOG is set, so on a normal run this sink is inert. Attaching is safe only
    // before the async worker starts consuming, i.e. straight after InitLogging.
    auto logger = spdlog::default_logger();
    if (logger != nullptr) {
        logger->sinks().push_back(std::make_shared<ConsoleSink>());
    }
}

void GdxConsoleLogDrain() {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr || ctx->GetWindow() == nullptr || ctx->GetWindow()->GetGui() == nullptr) {
        return;
    }
    auto console =
        std::static_pointer_cast<Ship::ConsoleWindow>(ctx->GetWindow()->GetGui()->GetGuiWindow("Console"));
    if (console == nullptr) {
        return;
    }

    std::deque<PendingLine> batch;
    {
        Queue& queue = TheQueue();
        std::lock_guard<std::mutex> lock(queue.mutex);
        batch.swap(queue.pending);
    }
    if (batch.empty()) {
        return;
    }

    // ConsoleWindow keeps every line for the session and re-walks the whole channel each frame it
    // is open, so drop the history wholesale once it gets long. gdiffuser-run.log keeps all of it.
    if (sWindowLines + batch.size() > kWindowCap) {
        console->ClearLogs("Console");
        sWindowLines = 0;
    }

    for (const PendingLine& line : batch) {
        console->Append("Console", line.level, "%s", line.text.c_str()); // Append is printf-style
        ++sWindowLines;
    }
}
