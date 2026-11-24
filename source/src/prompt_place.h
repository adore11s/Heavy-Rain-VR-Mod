#pragma once
// 1.6.6 (owner 2026-11-24: "the R2 prompt just keeps switching two positions while walking ... it looks all good when im
// standing still but when i start moving it starts to fight between two positions"). LIVE 13:33: ARI's wave prompt is
// a 3D MPAR prompt anchored about 1 m below the head (at the hand); prompts.cpp placed it "on the player" (anchor within
// 0.45 m of the head horizontally: kept round the front point) or "on an object" (outside: placed by its direction from
// the eye), decided again EVERY frame. Walking swings the animated anchor across 0.45 m, so the same prompt took both
// places by turns (log: "on the player (1.00 m from the head)" and "on an object (0.81 m from the eye)" in the same
// window, 975 / 472 frames). Now each prompt's place is decided per prompt, not per frame:
// - on the player once its anchor was in the body column; never back to "on an object" while the prompt lives;
// - its offset from the front point (across the view, height) is taken when it first shows on the player and then
//   kept, so it has ONE place that only follows the front point (the gaze, lazily) - not the swinging hand.
// A prompt not updated for kForgetMs is forgotten (the next one may reuse the element). Pure logic: tests/prompt_place_tests.cpp.
#include <cstdint>

namespace hrvr::promptplace {

struct Place {
    const void* key = nullptr;
    uint64_t ms = 0;
    bool onPlayer = false;
    bool haveOffset = false;
    float side = 0.f, up = 0.f;
};

class Memory {
public:
    static constexpr uint64_t kForgetMs = 1000;
    static constexpr int kSlots = 32;

    // The prompt `key` updated at `now` with its anchor in the body column or not. The returned place stays valid until
    // the next call.
    Place* Update(const void* key, uint64_t now, bool inColumn) {
        Place* p = nullptr;
        Place* oldest = &slots_[0];
        for (Place& s : slots_) {
            if (s.key == key && now - s.ms <= kForgetMs) p = &s;
            if (s.ms < oldest->ms) oldest = &s;
        }
        if (!p) {
            for (Place& s : slots_)
                if (!s.key || now - s.ms > kForgetMs) {
                    p = &s;
                    break;
                }
            if (!p) p = oldest;
            *p = Place{};
            p->key = key;
        }
        p->ms = now;
        if (inColumn && !p->onPlayer) {  // promoted at most once; the offset is taken by the caller at that moment
            p->onPlayer = true;
            p->haveOffset = false;
        }
        return p;
    }

private:
    Place slots_[kSlots];
};

}  // namespace hrvr::promptplace
