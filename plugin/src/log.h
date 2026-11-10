#ifndef RHY_LOG_H_
#define RHY_LOG_H_

// Append-only, timestamped log written next to the .asi. Flushed on every write.
void rhyLogInit();
void rhyLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif  // RHY_LOG_H_
