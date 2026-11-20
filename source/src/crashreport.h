#pragma once
#include <cstddef>
#include <cstdint>

struct ID3D11Device;

// Crash reports for players to send: when the game really crashes (an unhandled exception), a
// text file hrvr_logs\crash_reports\crash_<date>.txt is written before the game's own crash
// handler runs (it stays chained behind ours). It holds the PC / GPU / headset, the mod and game
// build, the mod's state, the crash address, registers, stack and the last log lines, with the
// Windows user name removed. First-chance faults inside the game or the mod are also logged
// (never handled) so the log shows where a crash started.
namespace hrvr::crash {

// Init thread, right after LogOpen: VEH logger + top-level filter.
void Install(const char* modVersion);
// After the anchors resolve (the exe's import table is final): chain the game's own filter
// behind ours and start the 1 Hz thread that keeps ours in front.
void InstallLate(const char* textSha256, bool supportedBuild);
// The GPU and its driver, for the report (called once the game's device exists).
void NoteDevice(ID3D11Device* dev);
// For the log's summary block: the game build line, and the PC (Windows, CPU, RAM, GPU) as the report has them.
void BuildText(char* out, size_t cap);
void PcText(char os[96], char cpu[96], char ram[32], char gpu[192]);
// Dev command "crashreport": writes a sample report without crashing.
void WriteTestReport();
// After MH_Initialize: name the game function when the game ends itself through abort().
void InstallAbortHook();

}  // namespace hrvr::crash
