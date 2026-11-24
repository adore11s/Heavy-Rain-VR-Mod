#pragma once
// 1.6 (owner 2026-11-24): Jayden's ARI glasses by hand in first person (F1) with motion controls - grip at the chest and
// bring the hand up to the eyes (put on), grip at the face and lower the hand (take off); 1.6.3 owner: back to the grip
// (1.6.2 used the trigger) - a grip used by the gesture is kept from the game's L1 / R1; the game's semicircle prompt is
// then neither shown nor answered by the stick. Pure logic (no engine, no Windows): the movie table, the gesture, the
// arc fed to the game's semicircle check and the first-time hint stage. Evidence: docs/ARI_GESTURE.md.
#include <cmath>
#include <cstring>

#include "posemath.h"

namespace hrvr::ari {

enum class Kind { None, On, Off };

// DATA 2026-11-24 (tools/ari_scan.py + ari_classify.py over all 11,695 packages): the semicircle prompts that start a
// movie playing HR_0020_ARI_*_ARIGLASSES_PUTON (On) or _TAKEOFF (Off). The user action carries the started movie's
// name (UA +0x3C8). 1101's ARI ball game (its own cutscene clips) and 4201's "Door" are NOT glasses prompts.
struct MovieEntry {
    const char* name;
    Kind kind;
};
inline constexpr MovieEntry kMovies[] = {
    {"_0801_JAYDEN_ON_THE_CRIME_SCENE_SARIFilters_NewARIon", Kind::On},
    {"_0801_JAYDEN_ON_THE_CRIME_SCENE_SARIFilters_NewARIoff", Kind::Off},
    {"_3801_MADJACK_SeqExplorePutAri_PutAri", Kind::On},
    {"_3801_MADJACK_SeqExploreRemoveAri_MRemoveAri", Kind::Off},
    {"_4201_JAYDEN_AT_BLACK_LAGOON_SNJAARI_PUTARI", Kind::On},
    {"_4201_JAYDEN_AT_BLACK_LAGOON_SNJAEnterOfficeBis_ari", Kind::On},
    {"_4201_JAYDEN_AT_BLACK_LAGOON_SNJAAriRemove_RemoveAri", Kind::Off},
    {"JAYDENOFFICE_SNJAOfficeStartARI_MStartARI", Kind::On},
};
// 1.6.5 (owner 2026-11-24: "make the r1 switch to r2, and even do xbox (like even button prompt and art)" - "ONLY for
// these Jayden's first person stuff"): ARI's R1 prompt is its WAVE (the scan); DATA (tools/ua_scan.py, all 28,986
// user actions): a PARELSTD with texture Button_R1 and input id 0x62 whose condition starts one of these movies.
// In F1 + motion controls it becomes R2 (input 0x63, texture Button_R2 - the game picks the PlayStation / Xbox art
// from that name), so the grip is free for the glasses. Other R1 prompts (Mad Jack's shot, the 4201 fight, the 1101
// shakes) are not touched.
inline constexpr const char* kWaveMovies[] = {
    "_0801_JAYDEN_ON_THE_CRIME_SCENE_SARIwave_ARIWave",
    "_0801_JAYDEN_ON_THE_CRIME_SCENE_SBodyKneelStart_Wave",
    "_3801_MADJACK_SeqExamineARIWave_WAVE",
    "_4201_JAYDEN_AT_BLACK_LAGOON_SNJAAriWave_Wave",
};
inline bool IsWaveMovie(const char* name) {
    if (!name || !*name) return false;
    for (const char* m : kWaveMovies)
        if (std::strcmp(m, name) == 0) return true;
    return false;
}
constexpr int kInputR1 = 0x62, kInputR2 = 0x63;  // DATA: PARINPUT ids (L1 0x60, L2 0x61, R1 0x62, R2 0x63)

inline Kind KindOfMovie(const char* name) {
    if (!name || !*name) return Kind::None;
    for (const MovieEntry& m : kMovies)
        if (std::strcmp(m.name, name) == 0) return m.kind;
    return Kind::None;
}

// ---- the gesture (seated reference frame: +Y up, metres; positions from the XR frame input) ----
struct V {
    float x = 0.f, y = 0.f, z = 0.f;
};
inline V Add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V Scale(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dist(V a, V b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Params {
    float chestDown = 0.30f;    // chest point below the headset
    float chestFwd = 0.12f;     // ... and ahead of it (the shirt / jacket front)
    float chestRadius = 0.22f;  // grab within this of the chest point
    float eyeFwd = 0.08f;       // glasses point ahead of the headset centre
    float eyeRadius = 0.17f;    // the hand counts as "at the eyes" within this
    float pressOn = 0.60f;      // the grip counts as pressed (edge) ...
    float pressOff = 0.35f;     // ... and released below this (hysteresis)
    float maxTime = 2.0f;       // seconds from the grab to the end of the movement
    float offDrop = 0.25f;      // take off: the hand ends this far below the headset
    float offTravel = 0.15f;    // ... after moving down at least this far from the grab
};

struct HandIn {
    bool active = false;
    V pos{};
    float press = 0.f;  // the grip (squeeze), 0 .. 1
};

class Gesture {
public:
    enum class Event { None, Grabbed, Released, Done };

    // One XR frame. kind = the glasses prompt on offer (None: nothing to do, any hold is dropped). head = headset
    // position, fwd = its horizontal forward (unit, y = 0).
    Event Feed(Kind kind, double t, V head, V fwd, const HandIn hands[2]) {
        Event ev = Event::None;
        if (kind == Kind::None) {
            if (hand_ >= 0) ev = Event::Released;
            hand_ = -1;
            kind_ = Kind::None;
            for (int h = 0; h < 2; ++h) wasDown_[h] = hands[h].active && hands[h].press >= p.pressOn;
            return ev;
        }
        const V eyes = Add(head, Scale(fwd, p.eyeFwd));
        if (hand_ >= 0) {
            const HandIn& h = hands[hand_];
            if (kind != kind_ || !h.active || h.press < p.pressOff || t - t0_ > p.maxTime) {
                hand_ = -1;
                ev = Event::Released;
            } else if (kind_ == Kind::On ? Dist(h.pos, eyes) <= p.eyeRadius
                                         : h.pos.y <= head.y - p.offDrop && grabY_ - h.pos.y >= p.offTravel) {
                hand_ = -1;
                ev = Event::Done;
            }
        }
        for (int i = 0; i < 2; ++i) {
            const HandIn& h = hands[i];
            const bool down = h.active && h.press >= (wasDown_[i] ? p.pressOff : p.pressOn);
            const bool edge = down && !wasDown_[i];
            wasDown_[i] = down;
            if (!edge || hand_ >= 0 || ev == Event::Done) continue;
            if (InStartZone(kind, head, fwd, h.pos)) {
                hand_ = i;
                kind_ = kind;
                t0_ = t;
                grabY_ = h.pos.y;
                ev = Event::Grabbed;
            }
        }
        return ev;
    }
    // Where the gesture starts: the chest (put on) or the face (take off). Also used to keep that grip from the game.
    bool InStartZone(Kind kind, V head, V fwd, V pos) const {
        if (kind == Kind::On) return Dist(pos, Add(Add(head, V{0.f, -p.chestDown, 0.f}), Scale(fwd, p.chestFwd))) <= p.chestRadius;
        if (kind == Kind::Off) return Dist(pos, Add(head, Scale(fwd, p.eyeFwd))) <= p.eyeRadius + 0.03f;
        return false;
    }
    bool Holding() const { return hand_ >= 0; }
    int HoldingHand() const { return hand_; }
    Params p;

private:
    int hand_ = -1;
    Kind kind_ = Kind::None;
    double t0_ = 0.0;
    float grabY_ = 0.f;
    bool wasDown_[2] = {false, false};
};

// ---- the arc handed to the game's semicircle check (MPAR_ELEMENT_AXIS_ROTATION, vfunc 0x14045df90) ----
// STATIC: the check only starts tracking after the stick rested near the centre (radius < tol / 2) OR sat outside the
// start window, then enters at the start point (within -0.43 .. 0.87 rad along the direction); progress begins at
// 0.5 and grows by (angle turned / span) (half that when element +0x210 is null); success at 0.85; the stick must
// stay out at radius >= 1 - tol. So: rest, start point, sweep past the span, hold the end - x = sin a, y = cos a.
struct ArcFeed {
    // 1.6.3 (LIVE: a 0.3 s sweep + 0.3 s hold never answered - the element has MparInertia 0.15): a 0.5 s sweep to
    // 1.15 x span, then the end point held until the game answers (ari.cpp ends the feed on the check's result),
    // at most 2 s.
    // owner 2026-11-24: "make the swipe longer": the start held 0.3 s (a smoothed stick reaches full push first), a
    // 0.6 s sweep to 1.3 x span, the end held until the game answers.
    static constexpr double kRest = 0.10, kStart = 0.30, kSweep = 0.60, kHold = 2.0;
    static constexpr double kOver = 1.30;
    static constexpr double kTotal = kRest + kStart + kSweep + kHold;
    // 1.6.6 (LIVE 2026-11-24 13:33, owner: "it does the first half of prompt but doesnt do the spin ... gets stuck"; 14 of
    // 14 feeds): the game engages at the start point (progress 0.5, the movie's first half) and gives the player to a
    // cutscene state for 0.6 .. 8 s in 12 of them; the feed ran on the WALL clock and was no longer written there (gated
    // by the F1 view), so it ended unseen and the held end point can never enter the reset prompt. Now the feed's own
    // clock moves only when the game's check runs (at most kMaxStep a check), the feed is written whatever the view, and
    // a prompt the game reset (not engaged in the hold) is fed again from the rest - at most kMaxRestarts times.
    static constexpr double kMaxStep = 0.1, kRestartAfter = 0.3, kWallMax = 20.0;
    static constexpr int kMaxRestarts = 4;
    float start = 0.f, span = 0.f;
    int dir = 0;  // 0: angle increases, 1: decreases (element +0x224)
    double t0 = -1.0;          // wall time the gesture was done (< 0: no feed)
    double clock = 0.0;        // feed time, advanced by the game's checks only
    double lastCheck = -1.0;   // wall time of the previous check
    int restarts = 0;
    void Begin(double now) {
        t0 = now;
        clock = 0.0;
        lastCheck = -1.0;
        restarts = 0;
    }
    bool Active(double now) const { return t0 >= 0.0 && clock < kTotal && now - t0 < kWallMax; }
    // One check of the element at wall time now: moves the feed clock on, gives the stick point to write.
    void Step(double now, float* x, float* y) {
        if (lastCheck >= 0.0) clock += std::fmin(kMaxStep, std::fmax(0.0, now - lastCheck));
        lastCheck = now;
        At(clock, x, y);
    }
    // After the game's check: the element not engaged once the hold has lasted kRestartAfter = the game reset the
    // prompt -> rest and start again. True when restarted.
    bool AfterCheck(bool engaged) {
        if (engaged || clock < kRest + kStart + kSweep + kRestartAfter || restarts >= kMaxRestarts) return false;
        clock = 0.0;
        ++restarts;
        return true;
    }
    // The stick point at feed time u (centre while resting / after the end).
    void At(double u, float* x, float* y) const {
        *x = *y = 0.f;
        if (u >= kTotal) return;
        u -= kRest;
        if (u < 0.0) return;  // resting at the centre
        u -= kStart;
        const double f = u <= 0.0 ? 0.0 : u >= kSweep ? 1.0 : u / kSweep;
        const double a = double(start) + (dir == 0 ? 1.0 : -1.0) * kOver * double(span) * f;
        *x = float(std::sin(a));
        *y = float(std::cos(a));
    }
    // Wall-clock form for the desk tests (every check seen): the point at time t after t0.
    void Point(double t, float* x, float* y) const {
        *x = *y = 0.f;
        if (t0 < 0.0) return;
        At(t - t0, x, y);
    }
};

// 1.7.1 (owner: "make the hints appear in every level where jayden has the glasses motion but ... only once per level"):
// the hints are kept per LEVEL (hrvr.ini [hrvr_ari_hints] <level>=bits, 1 put on done, 2 take off done). The level is
// the story chapter's number ("_0801" of "_0801_JAYDEN_ON_THE_CRIME_SCENE"); when the chapter is not known, the prompt
// movie's own prefix ("_3801" of "_3801_MADJACK_...", "JAYDENOFFICE" of the office movies - those play in two levels,
// 1101 and 3201, so the chapter is preferred). Five levels have the gesture: 0801, 1101, 3201, 3801, 4201.
inline bool ChapterNumbered(const char* s) {
    if (!s || s[0] != '_') return false;
    for (int i = 1; i <= 4; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return s[5] == '_' || s[5] == 0;
}
inline void LevelKey(const char* chapter, const char* movie, char* out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    const char* src = ChapterNumbered(chapter) ? chapter : ChapterNumbered(movie) ? movie : nullptr;
    size_t n = 0;
    if (src) {
        for (; n < 5 && n + 1 < cap; ++n) out[n] = src[n];
    } else if (movie) {
        for (; movie[n] && movie[n] != '_' && n + 1 < cap; ++n) out[n] = movie[n];
    }
    out[n] = 0;
}

// ---- the first-time hints: each shown until that gesture was done by hand once (hrvr.ini ari_hint_seen /
// ari_hint_off_seen; 1.7.1: per level, above); 1.6.2 (owner: "maybe in this level it should show first take off glasses") also for taking off ----
enum class Hint { None, GrabOn, PutOn, GrabOff, TakeOff };
inline Hint HintStage(bool seenOn, bool seenOff, Kind offered, bool holding) {
    if (offered == Kind::On && !seenOn) return holding ? Hint::PutOn : Hint::GrabOn;
    if (offered == Kind::Off && !seenOff) return holding ? Hint::TakeOff : Hint::GrabOff;
    return Hint::None;
}

}  // namespace hrvr::ari
