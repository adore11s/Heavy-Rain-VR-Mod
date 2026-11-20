#pragma once
// Where the player is in the story, for crash reports and the log (0.28.0; owner 2026-11-17: "the crash report should
// say story vars and what level they crashed in"). STATIC, docs/TARGET_ADAPTER_MAP.md "Storyboard" / "Story variables".
//
// Chapter: the running storyboard's CURRENT element (+0xE0) and its name (own name, else its scene's "_NNNN_NAME"),
// read in the storyboard's own per-frame task STORYBOARD::Update 0x140250df0 - the hook this module owns in every
// build; other code can run in that task through SetTaskCallback.
// Story variables: the game's Lua globals table named by the script manager (g_ScriptManager +0x198; lua_State at
// +0x20), walked like the game's own QA variables screen (LUA_DEBUG_SCREEN 0x1402854e0: lua_next over string keys;
// booleans, numbers and strings, sub-tables recursed). The walk runs in the storyboard task (game thread, no script
// running), at a chapter change and every 30 s; a crash report prints the last snapshot and never calls Lua itself.
// Off with hrvr.ini crash_story_vars=off.
#include <cstddef>

namespace hrvr::story {

bool Install();
// Code to run in the storyboard task (called before the game's own update; storyboard in `sb`).
using TaskCallback = void (*)(void* sb);
void SetTaskCallback(TaskCallback cb);
// The storyboard task's hook is in place.
bool TaskHooked();
// The current chapter's name ("_0801_CRIME_SCENE"; "" unknown) and how long ago it started (ms, 0 unknown).
void Chapter(char* out, size_t cap, unsigned long long* sinceMs);
// 0.36.10: the current element is a story chapter ("_NNNN_NAME"), not the main menu, install / splash screens or
// "(none)" (LIVE names: "SCENE_INSTALL", "LOADING_SCENE_0201_SPLASHS" = the main menu, "MENU_REVIEW_SCENES", "(none)").
bool InStoryChapter();
// For a crash report, without locks or allocation: calls line(text, ctx) for the chapter line and the story variables.
void DescribeForCrash(void (*line)(const char* text, void* ctx), void* ctx);

}  // namespace hrvr::story
