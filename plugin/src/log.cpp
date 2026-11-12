#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;
static bool g_logOpen = false;

// "<folder of our module>/rhyremix.log", falling back to the current directory.
static std::string logPath() {
    char module[MAX_PATH];
    module[0] = '\0';
    HMODULE self = NULL;  // our own .asi, not the game exe
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&logPath), &self);
    DWORD n = GetModuleFileNameA(self, module, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return "rhyremix.log";
    }
    std::string path(module, n);
    std::string::size_type sep = path.find_last_of("\\/");
    if (sep == std::string::npos) {
        return "rhyremix.log";
    }
    return path.substr(0, sep + 1) + "rhyremix.log";
}

// The wall-clock stamp both writers use. `lt` comes from std::localtime(), i.e. a shared static:
// rhyLog() is safe because it calls this under g_logMutex, rhyLogDetach() accepts the race (see
// its comment).
static bool stampNow(char* ts, size_t cap) {
    auto now = std::chrono::system_clock::now();
    std::time_t epoch = std::chrono::system_clock::to_time_t(now);
    unsigned ms = static_cast<unsigned>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm* lt = std::localtime(&epoch);
    if (!lt) {
        return false;
    }
    std::snprintf(ts, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03u",
                  lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                  lt->tm_hour, lt->tm_min, lt->tm_sec, ms);
    return true;
}

void rhyLogInit() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logOpen) {
        return;
    }
    g_logOpen = true;
    g_logFile = std::fopen(logPath().c_str(), "a");
    if (!g_logFile) {
        return;
    }
    std::fprintf(g_logFile, "---- log starts ----\n");
    std::fflush(g_logFile);
}

void rhyLog(const char* fmt, ...) {
    rhyLogInit();

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }

    char ts[32];
    if (!stampNow(ts, sizeof ts)) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile) {
        return;
    }
    std::fprintf(g_logFile, "[%s] %s\n", ts, msg);
    std::fflush(g_logFile);
}

// n09: see log.h. No mutex, its own FILE*, one line, flushed and closed. std::localtime()'s static
// buffer is still shared with a concurrent rhyLog(), which at worst garbles the timestamp of the
// last line of the run - and "the last line of the run is missing entirely" is what actually
// happened on 2026-11-12, so this trade is the right way round.
void rhyLogDetach(const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    char ts[32];
    if (!stampNow(ts, sizeof ts)) {
        return;
    }
    FILE* f = std::fopen(logPath().c_str(), "a");
    if (!f) {
        return;
    }
    std::fprintf(f, "[%s] %s\n", ts, msg);
    std::fflush(f);
    std::fclose(f);
}
