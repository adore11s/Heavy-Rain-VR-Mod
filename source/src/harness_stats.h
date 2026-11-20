#pragma once
// Flat-harness statistics (vr-modding-playbook A5): image metrics on downsampled luminance grids,
// robust median / MAD, and the preregistered three-capture gate verdicts (docs/STEREO_ROUTE.md).
// Pure code, desk-tested in tests/harness_tests.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hrvr::harness {

struct Grid {
    int w = 0, h = 0;
    std::vector<uint8_t> lum;  // w * h luminance 0..255
    bool Valid() const { return w > 0 && h > 0 && lum.size() == size_t(w) * size_t(h); }
};

// Mean absolute luminance difference (0..255 units); -1 when the grids are not comparable.
inline double MeanAbsDiff(const Grid& a, const Grid& b) {
    if (!a.Valid() || !b.Valid() || a.w != b.w || a.h != b.h) return -1.0;
    uint64_t sum = 0;
    for (size_t i = 0; i < a.lum.size(); ++i) sum += uint64_t(std::abs(int(a.lum[i]) - int(b.lum[i])));
    return double(sum) / double(a.lum.size());
}

// Fraction of cells brighter than 16 (A5.1: a capture that is mostly black is probably not the 3D scene).
inline double NonBlack(const Grid& a) {
    if (!a.Valid()) return 0.0;
    size_t n = 0;
    for (uint8_t v : a.lum) n += v > 16;
    return double(n) / double(a.lum.size());
}

// Mean horizontal shift (cells) that best aligns b to a, searched in [-maxShift, maxShift]:
// a camera moved sideways shows as a horizontal shift; a vertical component would be a defect.
inline int BestHorizontalShift(const Grid& a, const Grid& b, int maxShift) {
    if (!a.Valid() || !b.Valid() || a.w != b.w || a.h != b.h) return 0;
    int best = 0;
    double bestErr = 1e30;
    for (int s = -maxShift; s <= maxShift; ++s) {
        uint64_t sum = 0, n = 0;
        for (int y = 0; y < a.h; ++y)
            for (int x = std::max(0, -s); x < std::min(a.w, a.w - s); ++x) {
                sum += uint64_t(std::abs(int(a.lum[size_t(y) * a.w + x]) - int(b.lum[size_t(y) * a.w + x + s])));
                ++n;
            }
        const double err = n ? double(sum) / double(n) : 1e30;
        if (err < bestErr) {
            bestErr = err;
            best = s;
        }
    }
    return best;
}

inline double Median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
inline double Mad(const std::vector<double>& v) {
    const double m = Median(v);
    std::vector<double> d;
    d.reserve(v.size());
    for (double x : v) d.push_back(std::fabs(x - m));
    return Median(d) * 1.4826;  // consistent estimator of sigma for normal data
}

// ---- preregistered decision rules (docs/STEREO_ROUTE.md, written before the first run) ----
enum class Verdict { Pass, Minor, Confounded, Null, Invalid };
inline const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::Pass: return "PASS";
        case Verdict::Minor: return "MINOR";
        case Verdict::Confounded: return "CONFOUNDED";
        case Verdict::Null: return "NULL";
        default: return "INVALID";
    }
}
constexpr double kSanityNonBlack = 0.30;  // A5.1
constexpr double kCleanB = 0.10;          // re-entry with zero camera change: identical image
constexpr double kMinorB = 1.00;          // small, named side effects allowed for classification
constexpr double kSeparationC = 10.0;     // camera change must exceed the re-entry noise 10x
constexpr double kFloorC = 0.05;          // ... and never be judged against a floor below this

// B: the same frame rendered twice with no camera change (the control that can veto the verdict).
inline Verdict GateB(double medianB) {
    if (medianB < 0.0) return Verdict::Invalid;
    if (medianB <= kCleanB) return Verdict::Pass;
    if (medianB <= kMinorB) return Verdict::Minor;
    return Verdict::Confounded;
}
// C: the second render with the camera moved one IPD sideways. Read only when B is not confounded.
inline Verdict GateC(Verdict b, double medianB, double medianC) {
    if (b == Verdict::Invalid || medianC < 0.0) return Verdict::Invalid;
    if (b == Verdict::Confounded) return Verdict::Confounded;
    return medianC >= kSeparationC * std::max(medianB, kFloorC) ? Verdict::Pass : Verdict::Null;
}
// D: the second render with the mod's projection model (FOV scalars -> frustum) written over the
// engine's own: it must reproduce the engine image (no larger than twice the re-entry noise).
inline Verdict GateD(Verdict b, double medianB, double medianD) {
    if (b == Verdict::Invalid || medianD < 0.0) return Verdict::Invalid;
    if (b == Verdict::Confounded) return Verdict::Confounded;
    return medianD <= std::max(kCleanB, 2.0 * medianB) ? Verdict::Pass : Verdict::Null;
}

}  // namespace hrvr::harness
