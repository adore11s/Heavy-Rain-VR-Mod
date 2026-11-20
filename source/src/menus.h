#pragma once
// The game's own menus (playbook 10.6; STATIC 2026-11-17, docs/TARGET_ADAPTER_MAP.md "Menus"): the VR settings panel
// stands beside the game's Options screen wherever it is opened - from the pause menu AND from the main menu, where
// the game is not paused.
//
// Heavy Rain's menus are MenusMaster menus (MmDk::CMenu; data objects of class 0x32c9: MainMenu_V2 at the main menu,
// InGame_Pause in play). MenusMaster runs CMenu::Handle 0x14059b3f0 for every running menu each frame; the menu's
// current page is +0x290 (+0x298 while its state +0x378 is 1 or 4 - the game's own GetCurPage, 0x140159db0), and a
// page's first field is its name. Both menus open Options as the page "Option_Main" (their scripts:
// ChangePage(1,"Option_Main")), with the sub-pages SOUND, VIDEO, Grahpics_Opt(A), CONTROLER* / CONTROLLER_*.
// The hook only reads (exception-guarded) and logs every page change, the probe the playbook asks for.
#include <cstring>
#include <string>
#include <vector>

namespace hrvr::menus {

// A page is one of `prefixes` (each an exact name or the start of one: "CONTROLER" covers CONTROLER_MC).
inline bool PageMatches(const char* page, const std::vector<std::string>& prefixes) {
    if (!page || !page[0]) return false;
    for (const auto& p : prefixes)
        if (!p.empty() && std::strncmp(page, p.c_str(), p.size()) == 0) return true;
    return false;
}
// The settings value "a, b ,c" as a list (spaces trimmed, empty items dropped).
inline std::vector<std::string> ParseList(const std::string& list) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) end = list.size();
        std::string item = list.substr(start, end - start);
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) out.push_back(item);
        start = end + 1;
    }
    return out;
}
constexpr const char* kDefaultPages = "Option_Main,SOUND,VIDEO,Grahpics_Opt,CONTROLER,CONTROLLER_";

// Hooks CMenu::Handle (sigdb anchor MENU::Handle); hrvr.ini panel_pages = comma-separated page-name prefixes.
bool Install();
// A running menu (handled within the last 300 ms) shows one of the Options pages.
bool OptionsOpen();
// Command file verb "menus": logs every running menu and its page.
bool Command(const char* verb, const char* arg);

}  // namespace hrvr::menus
