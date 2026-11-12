#ifndef RHY_LOG_H_
#define RHY_LOG_H_

// Append-only, timestamped log written next to the .asi. Flushed on every write.
void rhyLogInit();
void rhyLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// n09: the DLL_PROCESS_DETACH line, written WITHOUT the log mutex. rhyLog() takes a std::mutex, and
// at process detach another thread can have been terminated while holding it - which would leave
// the last line of the run unsent, exactly the "no shutdown line at all" of the 2026-11-12 exit
// hang. This variant formats into its own buffer and writes it through a FILE* it opens itself. It
// can interleave with a concurrent rhyLog(); at detach the only thing at stake is one line of a
// debug log, and a missing one is the bug being fixed.
void rhyLogDetach(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif  // RHY_LOG_H_
