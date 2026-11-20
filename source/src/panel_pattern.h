#pragma once
// The V6 panel background (playbook 11, GoW panel.cpp / welcome_draw.cpp PatternPixel, D-317 / D-317b): faint light
// shapes in staggered streams rising up-right at 30 degrees - open chevrons, double chevrons, long diamond outlines,
// dashes ending in a dot, three loose dots - one shape per 200 x 92 px cell (one cell in six empty), anti-aliased by
// signed distance. Ported unchanged; only the two colours are parameters, so the owner's background colour can be
// used (playbook 11: "tint the pattern's grey and shape colours toward it with the same small contrast").
// Pure CPU code: the panel (panel.cpp), the welcome windows and tests/panel_tests.cpp share it.
#include <cmath>
#include <cstdint>

namespace hrvr::panel::pattern {

inline uint32_t Hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline float Rnd(uint32_t h) { return float(h & 0xffffu) / 65535.f; }
inline float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const float t = std::fmin(std::fmax((wx * vx + wy * vy) / std::fmax(vx * vx + vy * vy, 1e-6f), 0.f), 1.f);
    const float dx = wx - vx * t, dy = wy - vy * t;
    return std::sqrt(dx * dx + dy * dy);
}
// Distance (pixels) to the shape `kind` of size s, centred at the origin of the stream frame (x along the stream).
inline float ShapeDist(int kind, float x, float y, float s) {
    const float h = 0.5f * s;
    switch (kind) {
        case 0:  // open chevron pointing along the stream
            return std::fmin(SegDist(x, y, -0.35f * s, -h, 0.35f * s, 0.f), SegDist(x, y, -0.35f * s, h, 0.35f * s, 0.f));
        case 1: {  // double chevron
            const float a = std::fmin(SegDist(x, y, -0.6f * s, -h, 0.1f * s, 0.f), SegDist(x, y, -0.6f * s, h, 0.1f * s, 0.f));
            const float b = std::fmin(SegDist(x, y, -0.1f * s, -h, 0.6f * s, 0.f), SegDist(x, y, -0.1f * s, h, 0.6f * s, 0.f));
            return std::fmin(a, b);
        }
        case 2: {  // diamond outline, long along the stream
            const float rx = 0.9f * s, ry = 0.45f * s;
            const float k = std::fabs(x) / rx + std::fabs(y) / ry - 1.f;
            return std::fabs(k) * (rx * ry) / std::sqrt(rx * rx + ry * ry);
        }
        case 3: {  // a long dash ending in a dot
            const float dash = SegDist(x, y, -1.6f * s, 0.f, 0.9f * s, 0.f);
            const float dot = std::sqrt((x - 1.45f * s) * (x - 1.45f * s) + y * y) - 0.12f * s;
            return std::fmin(dash, std::fmax(dot, 0.f));
        }
        default: {  // three loose dots
            const float d1 = std::sqrt((x + 0.5f * s) * (x + 0.5f * s) + y * y);
            const float d2 = std::sqrt(x * x + (y - 0.25f * s) * (y - 0.25f * s));
            const float d3 = std::sqrt((x - 0.5f * s) * (x - 0.5f * s) + y * y);
            return std::fmax(std::fmin(std::fmin(d1, d2), d3) - 0.09f * s, 0.f);
        }
    }
}
// How much of pixel (px, py) a shape covers, 0..1.
inline float Coverage(int px, int py) {
    const float ca = 0.8660254f, sa = 0.5f;  // 30 deg, rising to the right
    const float cellU = 200.f, cellV = 92.f, stroke = 6.f;
    const float u = px * ca - py * sa, v = px * sa + py * ca;  // along / across the streams
    const int row = int(std::floor(v / cellV));
    const float uo = u + Rnd(Hash(uint32_t(row) * 7919u + 13u)) * cellU;  // rows staggered
    const int col = int(std::floor(uo / cellU));
    const uint32_t h = Hash(uint32_t(row) * 73856093u ^ uint32_t(col) * 19349663u);
    const int kind = int(h % 6u);  // 5 = an empty cell
    if (kind >= 5) return 0.f;
    const float s = (kind == 3 ? 34.f : 44.f) + Rnd(h >> 8) * 16.f;
    const float cx = (col + 0.5f) * cellU + (Rnd(h >> 4) - 0.5f) * 30.f;
    const float cy = (row + 0.5f) * cellV + (Rnd(h >> 12) - 0.5f) * 16.f;
    const float d = ShapeDist(kind, uo - cx, v - cy, s);
    const float w = kind == 4 ? 0.f : 0.5f * stroke;
    return std::fmin(std::fmax(w + 0.75f - d, 0.f), 1.5f) / 1.5f;  // anti-aliased edge
}

struct Rgb {
    float r, g, b;
};
// GoW's colours: grey (42,45,48), shapes (53,56,60).
constexpr Rgb kGowGrey{42.f, 45.f, 48.f}, kGowInk{53.f, 56.f, 60.f};
// Heavy Rain (owner 2026-10-27: origami tan on rain slate): the panel's slate (36,39,44) and shapes the same
// 11 / 11 / 12 levels lighter as GoW's.
constexpr Rgb kHrGrey{36.f, 39.f, 44.f}, kHrInk{47.f, 50.f, 56.f};

// One pixel of the pattern, 0x00RRGGBB.
inline uint32_t PatternPixel(int px, int py, Rgb bg = kHrGrey, Rgb ink = kHrInk) {
    const float cov = Coverage(px, py);
    const float r = bg.r + (ink.r - bg.r) * cov, g = bg.g + (ink.g - bg.g) * cov, b = bg.b + (ink.b - bg.b) * cov;
    return uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
}

}  // namespace hrvr::panel::pattern
