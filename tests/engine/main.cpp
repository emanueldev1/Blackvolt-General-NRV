#include "check.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#include <crtdbg.h>
#include <stdlib.h>
#include <windows.h>
#endif

// A test that hangs aborts the run with its name, instead of stalling CI until the job timeout.
namespace {
std::atomic<const char*> g_current{nullptr};
std::atomic<long long> g_startedMs{0};

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    // CI has no desktop: send CRT asserts and abort() to stderr instead of modal dialogs that hang.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const char* name = g_current.load();
            if (name && nowMs() - g_startedMs.load() > 120'000) {
                std::printf("[HANG] %s lleva más de 120 s, se aborta\n", name);
                std::fflush(stdout);
                std::_Exit(3);
            }
        }
    }).detach();

    for (auto& c : check::registry()) {
        const int before = check::failures();
        g_startedMs = nowMs();
        g_current = c.name;
        c.fn();
        g_current = nullptr;
        std::printf("%s %s (%lld ms)\n", check::failures() == before ? "[ ok ]" : "[FAIL]", c.name, nowMs() - g_startedMs.load());
    }
    std::printf("%zu tests, %d failed checks\n", check::registry().size(), check::failures());
    return check::failures() == 0 ? 0 : 1;
}
