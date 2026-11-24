// VR settings panel (panel.h), ported from the GoW VR mod's vr/panel.cpp (MIT) to the fixed nine-row profile
// GOW_STYLE_NINE_ROW_V6 (playbook chapter 11, revision 20 look; 0.23.0): the GoW design tokens, row pitch and hit
// geometry, two rows removed (finishers perspective, wall collision) and the canvas 256 px shorter; V6's patterned
// background (panel_pattern.h, in the owner's rain slate) and the built-in Montserrat (RCDATA 3101..3103, added for
// this process only). Rows bind to Heavy Rain capabilities; a row whose capability did not install is drawn disabled
// and does nothing, rather than a working-looking button.
#include "panel.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "fpcam.h"
#include "game.h"
#include "log.h"
#include "menus.h"
#include "panel_pattern.h"
#include "rendersize.h"
#include "stereo.h"
#include "vrinput.h"

namespace hrvr::panel {
namespace {

constexpr int kW = kWidthPx, kH = kHeightPx;
constexpr int kTop = 122, kRow = 128;  // the first row's label, the row pitch

const float kSpeeds[] = {0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
const int kSnaps[] = {15, 30, 45, 60, 90};
// 0.36.6: "Camera closer (third person)" (Beyond 1.0.3's presets): Off .. 2 m in 25 cm steps; 0.36.8 (owner): on to 5 m
// in 50 cm steps, 2 m the default
const float kPushes[] = {0.f, 0.25f, 0.5f, 0.75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f, 2.5f, 3.f, 3.5f, 4.f, 4.5f, 5.f};

enum Action {
    kNone, kF1On, kF1Off, kF3On, kF3Off, kMotionOn, kMotionOff, kTurnSmooth, kTurnSnap, kStepDown, kStepUp,
    kPadXbox, kPadPs, kSizeDown, kSizeUp, kNative, kAfr, kMono, kVrOn, kVrOff, kPushDown, kPushUp, kHeadOn, kHeadOff,
    kYellowOn, kYellowOff
};
struct Button {
    RECT r;
    Action a;
    const wchar_t* text;
    bool selected;
    bool enabled;
};

// GDI surface
HDC g_dc = nullptr;
HBITMAP g_bmp = nullptr;
uint32_t* g_bits = nullptr;
HFONT g_fTitle = nullptr, g_fLabel = nullptr, g_fButton = nullptr, g_fHint = nullptr;
uint32_t g_out[kW * kH];
uint32_t g_cursor[kCursorPx * kCursorPx];
bool g_cursorMade = false;

Button g_buttons[32];
int g_nButtons = 0;
Action g_hover = kNone;
bool g_needDraw = true;
char g_lastState[128] = "";

std::atomic<bool> g_capture[2] = {false, false};
TriggerClick g_click[2];

template <class T, size_t N>
int Nearest(const T (&list)[N], float v) {
    int best = 0;
    for (int i = 1; i < int(N); ++i)
        if (std::fabs(float(list[i]) - v) < std::fabs(float(list[best]) - v)) best = i;
    return best;
}

void Apply(Action a) {
    switch (a) {
        case kF1On: fpcam::SetGameplayFP(true); break;
        case kF1Off: fpcam::SetGameplayFP(false); break;
        case kF3On: fpcam::SetCutsceneFP(true); break;
        case kF3Off: fpcam::SetCutsceneFP(false); break;
        case kMotionOn: vrinput::SetMotionControls(true); break;
        case kMotionOff: vrinput::SetMotionControls(false); break;
        case kHeadOn: fpcam::SetHideHead(true); break;
        case kHeadOff: fpcam::SetHideHead(false); break;
        case kYellowOn: stereo::SetYellowFilter(true); break;
        case kYellowOff: stereo::SetYellowFilter(false); break;
        case kTurnSmooth: fpcam::SetSnapTurn(false); break;
        case kTurnSnap: fpcam::SetSnapTurn(true); break;
        case kStepDown:
        case kStepUp: {
            const int d = a == kStepUp ? 1 : -1;
            if (fpcam::SnapTurn()) {
                const int n = int(sizeof(kSnaps) / sizeof(kSnaps[0]));
                int i = Nearest(kSnaps, fpcam::SnapDegrees()) + d;
                i = i < 0 ? 0 : i >= n ? n - 1 : i;
                fpcam::SetSnapDegrees(float(kSnaps[i]));
            } else {
                const int n = int(sizeof(kSpeeds) / sizeof(kSpeeds[0]));
                int i = Nearest(kSpeeds, fpcam::TurnSpeed()) + d;
                i = i < 0 ? 0 : i >= n ? n - 1 : i;
                fpcam::SetTurnSpeed(kSpeeds[i]);
            }
            break;
        }
        case kPushDown:
        case kPushUp: {
            const int n = int(sizeof(kPushes) / sizeof(kPushes[0]));
            int i = Nearest(kPushes, stereo::CameraPush()) + (a == kPushUp ? 1 : -1);
            i = i < 0 ? 0 : i >= n ? n - 1 : i;
            stereo::SetCameraPush(kPushes[i]);
            break;
        }
        case kPadXbox: vrinput::SetPadType(2); break;
        case kPadPs: vrinput::SetPadType(5); break;
        case kSizeDown:
        case kSizeUp: rendersize::SetSetting(rendersize::Step(rendersize::Setting(), a == kSizeUp ? 1 : -1)); break;
        case kNative: stereo::SetMethod(stereo::kNative); break;
        case kAfr: stereo::SetMethod(stereo::kAfr); break;
        case kMono: stereo::SetMethod(stereo::kMono); break;
        case kVrOn: break;                              // VR is on while the panel shows
        case kVrOff: stereo::RequestVr(false); break;  // as F9; F9 turns it back on
        default: return;
    }
    g_needDraw = true;
    Log("PANEL action %d -> F1 %d, F3 %d, motion %d, turning %s %.0f %% / %.0f deg, camera closer %.2f m, pad %s, picture "
        "size %d, %s",
        int(a), int(fpcam::GameplayFP()), int(fpcam::CutsceneFP()), int(vrinput::MotionControls()),
        fpcam::SnapTurn() ? "snap" : "smooth", fpcam::TurnSpeed() * 100.f, fpcam::SnapDegrees(), stereo::CameraPush(),
        vrinput::PadType() == 5 ? "PlayStation" : "Xbox", rendersize::Setting(), stereo::MethodName(stereo::Method()));
}

// V6 background (playbook 11): computed once, copied under every redraw.
uint32_t g_bgPix[kW * kH];
void MakeBackground() {
    for (int py = 0; py < kH; ++py)
        for (int px = 0; px < kW; ++px) g_bgPix[py * kW + px] = pattern::PatternPixel(px, py);
}

// The built-in Montserrat (SIL OFL 1.1, source/assets/fonts; RCDATA 3101 Bold, 3102 SemiBold, 3103 Regular of this
// module) added for this process only: nothing is installed, the fonts are gone when the game closes.
bool LoadEmbeddedFonts() {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LoadEmbeddedFonts), &mod))
        return false;
    int ok = 0;
    for (int id = 3101; id <= 3103; ++id) {
        HRSRC r = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
        HGLOBAL g = r ? LoadResource(mod, r) : nullptr;
        void* p = g ? LockResource(g) : nullptr;
        DWORD n = 0;
        if (p && AddFontMemResourceEx(p, SizeofResource(mod, r), nullptr, &n) && n > 0) ++ok;
    }
    return ok == 3;
}

bool MakeSurface() {
    if (g_dc) return g_bits != nullptr;
    g_dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kW;
    bi.bmiHeader.biHeight = -kH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_bmp || !bits) {
        Log("PANEL DIB section failed");
        return false;
    }
    g_bits = static_cast<uint32_t*>(bits);
    SelectObject(g_dc, g_bmp);
    auto font = [](int px, int weight, const wchar_t* face) {
        return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    };
    // V6 sizes (GoW D-317b); GDI faces "Montserrat" (Bold / Regular) and "Montserrat SemiBold" (its own family).
    const bool mont = LoadEmbeddedFonts();
    if (mont) {
        g_fTitle = font(50, FW_BOLD, L"Montserrat");
        g_fLabel = font(32, FW_SEMIBOLD, L"Montserrat SemiBold");
        g_fButton = font(34, FW_BOLD, L"Montserrat");
        g_fHint = font(26, FW_NORMAL, L"Montserrat");
    } else {  // V5's fonts
        g_fTitle = font(54, FW_BOLD, L"Segoe UI");
        g_fLabel = font(36, FW_SEMIBOLD, L"Segoe UI");
        g_fButton = font(38, FW_SEMIBOLD, L"Segoe UI");
        g_fHint = font(28, FW_NORMAL, L"Segoe UI");
    }
    Log("PANEL font %s", mont ? "Montserrat (built in)" : "Segoe UI (the built-in Montserrat did not load)");
    MakeBackground();
    SetBkMode(g_dc, TRANSPARENT);
    return true;
}

void Fill(const RECT& r, COLORREF c, int round = 0) {
    HBRUSH b = CreateSolidBrush(c);
    if (round > 0) {
        HGDIOBJ ob = SelectObject(g_dc, b), op = SelectObject(g_dc, GetStockObject(NULL_PEN));
        RoundRect(g_dc, r.left, r.top, r.right + 1, r.bottom + 1, round, round);
        SelectObject(g_dc, ob);
        SelectObject(g_dc, op);
    } else {
        FillRect(g_dc, &r, b);
    }
    DeleteObject(b);
}

void Text(const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    SelectObject(g_dc, f);
    SetTextColor(g_dc, c);
    DrawTextW(g_dc, s, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX);
}

void AddButton(int x, int y, int w, int h, Action a, const wchar_t* text, bool selected, bool enabled) {
    if (g_nButtons < 32) g_buttons[g_nButtons++] = {{x, y, x + w, y + h}, a, text, selected, enabled};
}

// A row: a label, then two choices or "- value +". GOW_STYLE_NINE_ROW_V6 order (zero-based index -> y).
struct Row {
    const wchar_t* label;
    Action a, b;            // the two choices, or the stepper's down / up
    const wchar_t *ta, *tb; // their texts (nullptr: a stepper)
    bool selA;              // the first choice is the selected one (two choices only)
    int value;              // stepper: 0 = turning step, 1 = picture size, 2 = camera closer
    bool enabledA, enabledB;
    Action c = kNone;        // a third choice (Rendering method: Native | AFR | Mono)
    const wchar_t* tc = nullptr;
    int selected3 = 0;       // with a third choice: which of the three is selected (-1 none)
};
int Rows(Row* r) {
    const bool snap = fpcam::SnapTurn();
    int n = 0;
    r[n++] = {L"First person (walking)", kF1On, kF1Off, L"On", L"Off", fpcam::GameplayFP(), 0, true, true};
    r[n++] = {L"First person (cutscenes)", kF3On, kF3Off, L"On", L"Off", fpcam::CutsceneFP(), 0, true, true};
    r[n++] = {L"Motion controls (first person)", kMotionOn, kMotionOff, L"On", L"Off", vrinput::MotionControls(), 0, true, true};
    // 0.36.12 (owner): the head kept out of the first-person view (fp_hidehead), on by default, live
    r[n++] = {L"Hide head (first person)", kHeadOn, kHeadOff, L"On", L"Off", fpcam::HideHead(), 0, true, true};
    r[n++] = {L"Turning (first person)", kTurnSmooth, kTurnSnap, L"Smooth", L"Snap", !snap, 0, true, true};
    r[n++] = {snap ? L"Snap angle" : L"Turning speed", kStepDown, kStepUp, nullptr, nullptr, false, 0, true, true};
    // 0.36.6 (owner: imported from the Beyond mod's panel): the game's own camera moved toward the character, third
    // person only (first person has its own eye), Off .. 2 m. '-' at Off and '+' at 2 m do nothing.
    const float push = stereo::CameraPush();
    r[n++] = {L"Camera closer (third person)", kPushDown, kPushUp, nullptr, nullptr, false, 2, push > 0.001f,
              push < kPushes[sizeof(kPushes) / sizeof(kPushes[0]) - 1] - 0.001f};
    r[n++] = {L"Controller buttons", kPadXbox, kPadPs, L"Xbox", L"PlayStation", vrinput::PadType() != 5, 0, true, true};
    // Game resolution < 1440 .. 2816 < Auto (rendersize.h); '+' at Auto and '-' at the game's own do nothing.
    const int size = rendersize::Setting();
    r[n++] = {L"VR picture size (per eye)", kSizeDown, kSizeUp, nullptr, nullptr, false, 1,
              size != rendersize::kGame, size != rendersize::kAuto};
    // Native | AFR | Mono (playbook 07.7 / 11; stereo.h); Native is the default (owner 2026-11-20, was AFR). AER (hrvr.ini
    // rendering_method=aer only, not a template row) selects none of the three.
    const int m = stereo::Method();
    r[n++] = {L"Rendering method", kNative, kAfr, L"Native", L"AFR", false, 0, true, true, kMono, L"Mono",
              m == stereo::kNative ? 0 : m == stereo::kAfr ? 1 : m == stereo::kMono ? 2 : -1};
    // 1.5 (owner 2026-11-24): the game's yellow colour filter (its per-channel gamma; Off = the "No PP" mod's colours), live
    r[n++] = {L"Yellow filter", kYellowOn, kYellowOff, L"On", L"Off", stereo::YellowFilter(), 0, true, true};
    r[n++] = {L"VR on / off  (Crashing during cutscene)", kVrOn, kVrOff, L"On", L"Off", true, 0, true, true};
    return n;
}

void Layout(wchar_t* stepText, wchar_t* sizeText, wchar_t* pushText) {
    g_nButtons = 0;
    Row rows[16];
    const int n = Rows(rows);
    for (int i = 0; i < n; ++i) {
        const Row& r = rows[i];
        const int y = kTop + i * kRow + 48;
        if (r.ta && r.tc) {  // three choices across the same width (V6: x 40 / 360 / 680, 280 wide, gaps 40)
            AddButton(kThreeX[0], y, kThreeW, 68, r.a, r.ta, r.selected3 == 0, r.enabledA);
            AddButton(kThreeX[1], y, kThreeW, 68, r.b, r.tb, r.selected3 == 1, r.enabledB);
            AddButton(kThreeX[2], y, kThreeW, 68, r.c, r.tc, r.selected3 == 2, true);
        } else if (r.ta) {
            AddButton(40, y, 440, 68, r.a, r.ta, r.selA, r.enabledA);
            AddButton(520, y, 440, 68, r.b, r.tb, !r.selA, r.enabledB);
        } else {
            AddButton(40, y, 140, 68, r.a, L"\x2212", false, r.enabledA);
            AddButton(820, y, 140, 68, r.b, L"+", false, r.enabledB);
        }
    }
    if (fpcam::SnapTurn()) swprintf_s(stepText, 32, L"%.0f\x00b0", fpcam::SnapDegrees());
    else swprintf_s(stepText, 32, L"%.0f %%", fpcam::TurnSpeed() * 100.f);
    const float push = stereo::CameraPush();
    if (push < 0.001f) swprintf_s(pushText, 32, L"Off");
    else swprintf_s(pushText, 32, L"%.2f m", push);
    // GoW formatting: "2048 x 2048", "Auto (N x N)".
    const int size = rendersize::Setting();
    const uint32_t autoN = rendersize::AutoSize();
    if (size == rendersize::kAuto) {
        if (autoN) swprintf_s(sizeText, 32, L"Auto (%u x %u)", autoN, autoN);
        else swprintf_s(sizeText, 32, L"Auto");
    } else if (size == rendersize::kGame) {
        swprintf_s(sizeText, 32, L"Game resolution");
    } else {
        swprintf_s(sizeText, 32, L"%d x %d", size, size);
    }
}

void Draw() {
    if (!MakeSurface()) return;
    wchar_t stepText[32], sizeText[32], pushText[32];
    Layout(stepText, sizeText, pushText);
    // Heavy Rain palette (owner 2026-10-27: "swap the red for the colours of the Heavy Rain cover"): the GoW
    // layout and tokens with the red accent replaced by origami-paper tan, and the greys turned rain-cold slate.
    // V6: the patterned slate under everything, V6's brighter label / hint text.
    const COLORREF title = RGB(150, 118, 76), button = RGB(64, 68, 74), hover = RGB(94, 99, 107),
                   selected = RGB(150, 118, 76), selectedHover = RGB(178, 144, 98), white = RGB(245, 245, 245),
                   label = RGB(225, 225, 230), hint = RGB(165, 165, 170);
    GdiFlush();
    std::memcpy(g_bits, g_bgPix, sizeof(g_bgPix));
    Fill({0, 0, kW, 100}, title);
    Text(L"VR SETTINGS", {40, 0, kW - 40, 100}, g_fTitle, white, DT_LEFT | DT_VCENTER);
    Row rows[16];
    const int n = Rows(rows);
    for (int i = 0; i < n; ++i) {
        const int y = kTop + i * kRow;
        Text(rows[i].label, {40, y, kW - 40, y + 44}, g_fLabel, label, DT_LEFT | DT_VCENTER);
        if (!rows[i].ta) {  // the stepper's value box
            const int by = y + 48;
            Fill({200, by, 800, by + 68}, RGB(28, 31, 35), 14);
            Text(rows[i].value == 1 ? sizeText : rows[i].value == 2 ? pushText : stepText, {200, by, 800, by + 68}, g_fButton, white,
                 DT_CENTER | DT_VCENTER);
        }
    }
    for (int i = 0; i < g_nButtons; ++i) {
        const Button& b = g_buttons[i];
        const bool hv = b.enabled && b.a == g_hover;
        Fill(b.r, b.selected ? (hv ? selectedHover : selected) : (hv ? hover : button), 14);
        Text(b.text, b.r, g_fButton, b.enabled ? white : hint, DT_CENTER | DT_VCENTER);
    }
    Text(L"Point a controller at an option and pull its trigger.", {40, kH - 150, kW - 40, kH - 110}, g_fHint, hint,
         DT_LEFT | DT_VCENTER);
    Text(L"VR off: press F9 on the keyboard to turn it back on.", {40, kH - 106, kW - 40, kH - 66}, g_fHint, hint,
         DT_LEFT | DT_VCENTER);
    // Playbook 11.1: GoW's exact third hint (its V6 panel draws it in the same Montserrat 26 px box). 0.23.0 had an own
    // wording; parity restored in 0.30.1 (the chord is on the welcome window VR CONTROLS and in the README).
    Text(L"Quick first-person switch: press \x25CB (PS VR2) / B near your head.", {40, kH - 62, kW - 40, kH - 22},
         g_fHint, hint, DT_LEFT | DT_VCENTER);
    GdiFlush();
    for (int i = 0; i < kW * kH; ++i) g_out[i] = g_bits[i] | 0xff000000u;  // GDI leaves alpha 0: opaque
}

void MakeCursor() {
    if (g_cursorMade) return;
    g_cursorMade = true;
    const float c = (kCursorPx - 1) * 0.5f;
    for (int y = 0; y < kCursorPx; ++y)
        for (int x = 0; x < kCursorPx; ++x) {
            const float d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c)) / c;  // 0 centre .. 1 edge
            float a = 0.f, r = 0.f, g = 0.f, b = 0.f;
            if (d < 0.55f) {
                a = 1.f;
                r = g = b = 1.f;  // white dot
            } else if (d < 0.85f) {
                a = 1.f;
                r = 0.59f;
                g = 0.46f;
                b = 0.30f;  // origami-tan ring
            } else if (d < 1.f) {
                a = (1.f - d) / 0.15f;  // soft edge (premultiplied)
                r = 0.59f * a;
                g = 0.46f * a;
                b = 0.30f * a;
            }
            auto u8 = [](float v) { return uint32_t(std::lrint(std::fmin(std::fmax(v, 0.f), 1.f) * 255.f)); };
            g_cursor[y * kCursorPx + x] = u8(a) << 24 | u8(r) << 16 | u8(g) << 8 | u8(b);
        }
}

// The panel shows while the game is paused, or while the game's own Options screen is open - also from the main menu,
// where nothing is paused (playbook 10.6, menus.h).
bool Paused() {
    const auto* p = game::Global<uint8_t>(game::g::GamePaused);
    return (p && *p != 0) || menus::OptionsOpen();
}

}  // namespace

// The pointer ray through a One Euro filter (GoW D-263: at ~2 m a trembling hand is a jumping dot): still
// at rest, a fast sweep passes (min cutoff 3 Hz, beta 6). Raw controller samples still drive the game.
void SmoothAim(int hand, pose::Vec3& p, pose::Vec3& d) {
    static bool have[2] = {false, false};
    static pose::Vec3 sp[2], sd[2];
    static LARGE_INTEGER last[2] = {}, qf{};
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = last[hand].QuadPart ? float(now.QuadPart - last[hand].QuadPart) / float(qf.QuadPart) : 0.f;
    last[hand] = now;
    if (!have[hand] || dt <= 0.f || dt > 0.1f) {
        sp[hand] = p;
        sd[hand] = d;
        have[hand] = true;
        return;
    }
    const float dp = std::sqrt((p.x - sp[hand].x) * (p.x - sp[hand].x) + (p.y - sp[hand].y) * (p.y - sp[hand].y) +
                               (p.z - sp[hand].z) * (p.z - sp[hand].z));
    const float dd = std::sqrt((d.x - sd[hand].x) * (d.x - sd[hand].x) + (d.y - sd[hand].y) * (d.y - sd[hand].y) +
                               (d.z - sd[hand].z) * (d.z - sd[hand].z));
    const float cutoff = 3.f + 6.f * (dp + 2.f * dd) / dt;
    const float a = 1.f / (1.f + 1.f / (6.2831853f * cutoff * dt));
    sp[hand] = {sp[hand].x + (p.x - sp[hand].x) * a, sp[hand].y + (p.y - sp[hand].y) * a, sp[hand].z + (p.z - sp[hand].z) * a};
    sd[hand] = {sd[hand].x + (d.x - sd[hand].x) * a, sd[hand].y + (d.y - sd[hand].y) * a, sd[hand].z + (d.z - sd[hand].z) * a};
    const float n = std::sqrt(sd[hand].x * sd[hand].x + sd[hand].y * sd[hand].y + sd[hand].z * sd[hand].z);
    if (n > 1e-6f) sd[hand] = {sd[hand].x / n, sd[hand].y / n, sd[hand].z / n};
    p = sp[hand];
    d = sd[hand];
}

bool Update(const xr::FrameInput& in, const pose::Vec3& screen, float screenW, Frame* out) {
    if (!Paused()) {
        Hidden();
        return false;
    }
    const pose::Vec3 head{in.headRel.m[12], in.headRel.m[13], in.headRel.m[14]};
    const Placement pl = Place(screen, screenW, head);
    Action hover = kNone;
    bool cursor = false;
    pose::Vec3 hitP{};
    for (int hand = 1; hand >= 0; --hand) {  // the right hand first
        const xr::HandInput& hs = in.hand[hand];
        bool hit = false;
        if (hs.active && hs.aimValid) {
            pose::Vec3 o{hs.aimRel.m[12], hs.aimRel.m[13], hs.aimRel.m[14]};
            pose::Vec3 d{-hs.aimRel.m[8], -hs.aimRel.m[9], -hs.aimRel.m[10]};  // aim -Z
            SmoothAim(hand, o, d);
            float px = 0.f, py = 0.f;
            pose::Vec3 q{};
            if (Hit(pl, o, d, &px, &py, &q)) {
                hit = true;
                if (!cursor) {
                    cursor = true;
                    hitP = q;
                    for (int i = 0; i < g_nButtons; ++i) {
                        const RECT& r = g_buttons[i].r;
                        if (g_buttons[i].enabled && px >= r.left && px < r.right && py >= r.top && py < r.bottom)
                            hover = g_buttons[i].a;
                    }
                    if (g_click[hand].Feed(hs.trigger) && hover != kNone) Apply(hover);
                }
            }
        }
        g_capture[hand] = hit;
        // Off the panel the trigger only re-arms (below 0.35) or disarms (a pull): a pull begun elsewhere
        // never clicks when the ray reaches the panel.
        if (!hit) g_click[hand].Feed(hs.active ? hs.trigger : 0.f);
    }
    // Redraw when a value or the hovered button changed (values also change with F1 / F3 / F10 / F11).
    uint32_t ew = 0, eh = 0;
    stereo::EyeImageSize(&ew, &eh);
    char state[128];
    sprintf_s(state, "%d %d %d %d %.2f %.0f %d %u %u %d %u %d %.2f %d %d", int(fpcam::GameplayFP()), int(fpcam::CutsceneFP()),
              int(vrinput::MotionControls()), int(fpcam::SnapTurn()), fpcam::TurnSpeed(), fpcam::SnapDegrees(),
              vrinput::PadType(), ew, eh, rendersize::Setting(), rendersize::AutoSize(), stereo::Method(),
              stereo::CameraPush(), int(fpcam::HideHead()), int(stereo::YellowFilter()));
    if (hover != g_hover || std::strcmp(state, g_lastState) != 0 || g_needDraw) {
        g_hover = hover;
        strcpy_s(g_lastState, state);
        g_needDraw = false;
        Draw();
        out->dirty = true;
        static int shown = 0;
        if (!shown++) Log("PANEL VR settings panel shown (%dx%d px, %.2f x %.4f m, right of the screen)", kW, kH, kWidthM, kHeightM);
    } else {
        out->dirty = false;
    }
    out->placement = pl;
    out->pixels = g_out;
    out->cursor = cursor;
    if (cursor) {
        const float nx = std::sin(pl.yaw), nz = std::cos(pl.yaw);
        out->cursorAt = {hitP.x + nx * 0.01f, hitP.y, hitP.z + nz * 0.01f};  // just in front of the panel
    }
    return true;
}

void Hidden() {
    g_capture[0] = g_capture[1] = false;
    g_hover = kNone;
    g_needDraw = true;
}

const uint32_t* CursorPixels() {
    MakeCursor();
    return g_cursor;
}

bool CapturesTrigger(int hand) { return g_capture[hand & 1].load(std::memory_order_relaxed); }

}  // namespace hrvr::panel
