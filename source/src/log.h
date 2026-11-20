#pragma once
#include <cstdarg>
#include <cstddef>

namespace hrvr {

// Opens <gameDir>\hrvr_logs\hrvr.log, rotating the previous run to hrvr.prev.log.
void LogOpen(const wchar_t* gameDir);
void Log(const char* fmt, ...);
void LogFlush();
// Seconds since LogOpen: the clock of every log line's timestamp.
double LogSeconds();
// <gameDir>\hrvr_logs (empty before LogOpen).
const wchar_t* LogDir();
// Crash reports: calls fn(line, ctx) for the most recent log lines, oldest first. Never blocks
// (a crash inside Log may hold the lock) and never allocates.
void LogRecent(void (*fn)(const char* line, void* ctx), void* ctx);
// Replaces what could identify the player (game folder and user profile paths, the Windows user name,
// the computer name) in a UTF-8 line, in place. Every log line goes through it; crash reports too.
void LogScrub(char* line, size_t cap);
// Playbook 28.1 (GoW core/log.cpp): one stored line, "[<ms since the DLL loaded, %10.3f>][t<thread>] <subsystem>: <text>".
// The mod's messages start with their subsystem in capitals ("STEREO pair 3: ...", "FP state: ..."): that word becomes
// the lower-case subsystem with a colon ("stereo: pair 3: ...", "fp: state: ..."); a message already starting with a
// lower-case "word:" keeps it; the build line ("hrvr <version> linked ...") stays as it is; anything else gets "hrvr: ".
void FormatLogLine(char* out, size_t cap, double ms, unsigned long thread, const char* msg);

}  // namespace hrvr
