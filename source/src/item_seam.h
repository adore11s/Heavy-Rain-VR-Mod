#pragma once
// 0.32.0: where a first-person item's drawn place may reach the engine (playbook 19.8 point 11, Gate 3 rows 2 / 5).
//
// STATIC 2026-11-20 (TARGET_ADAPTER_MAP "Item seam"): NODE::UpdateWorld 0x140893860 STORES world (+0x90) = local (+0x50)
// x ParentMatrix 0x14017b750 and marks the node clean (+0xE8 bit 1). Every reader (290 callers: scripts, AI, physics,
// zone tests, PLAY_ANIM_RELATIVE_TO_ENTITY, the renderer) rebuilds a node only while it is dirty, and 0x140893960
// (node, 2, 0) dirties a node with everything below it (112 callers; a character's animation update runs it on the
// character's node every frame, 0x1409fd3f0, so every attached object is rebuilt by whoever reads it first).
// NODE::SetWorld 0x140893700 goes the other way: local = world x inverse(ParentMatrix) (SCRIPT_ENTITY_ABC::SetMatrix,
// NODE::SetPosition 0x140893620, the animation's world setter 0x1409fa410, AI, physics).
// LIVE (0.20.0 logs): Heavy Rain updates and then draws on ONE thread (Present, VIEWPORT_LIST and the item lines share
// a thread id; the engine's render thread is 0); the draw is DISPLAY_WINDOW::Display.
//
// So the mod's drawn answer must only exist while the draw runs:
//   SetWorld caller           -> always the game's matrix (a drawn one would change the object's local for good);
//   UpdateWorld, outside draw -> the game's matrix; a node that would be drawn moved is remembered (Remember);
//   Begin (draw starts)       -> remembered nodes are dirtied, so the draw rebuilds them through the hook;
//   UpdateWorld, inside draw  -> the drawn answer; a node that got one is noted (Answered);
//   End (draw ends)           -> each answered node is rebuilt with the game's matrix (its stored world is the game's
//                                again) and dirtied with everything below it, so the next draw rebuilds it from the
//                                drawn pose even when nothing moved (a still item while only the controller moves).
// Any other caller (0x1409fd4b0: an object's own skeleton root, stored in the skeleton instance +0x70 during the
// animation update - every skinned prop) gets the game's matrix too since 0.33.0; fpbody keeps the edit per skeleton
// and applies it to that skeleton's palette only (RememberRootEdit / RootEditOf). RouteOf says Draw for it: the hook
// computes the answer there and decides.
// A remembered node is touched only while its first qword (vtable) is still the one seen when it was remembered: a
// destroyed node has its base class's vtable or freed memory there.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hrvr::itemseam {

enum class Caller { Update, SetWorld, Other };
enum class Route { Game, Remember, Draw };

struct Engine {
    void (*invalidate)(void* node) = nullptr;  // 0x140893960(node, 2, 0): the node and everything below it dirty
    void (*rebuild)(void* node) = nullptr;     // NODE::UpdateWorld: world = local x ParentMatrix, clean
    uint64_t (*vtable)(void* node) = nullptr;  // the node's first qword, 0 when unreadable
};

struct Counters {
    std::atomic<uint64_t> setWorld{0}, kept{0}, refreshed{0}, gone{0}, drawn{0}, rebuilt{0}, other{0};
    // dirtied at the draw's start but never asked for in the draw: a reader that takes +0x90 without the clean-bit
    // test, or a node not drawn at all (hidden). Many while an item visibly stays at the animated hand = such a reader.
    std::atomic<uint64_t> unasked{0};
};

// The calling thread is rebuilding nodes for End: its own hook calls must not remember them again.
inline thread_local bool t_rebuilding = false;

class Seam {
public:
    void SetEngine(const Engine& e) { engine_ = e; }
    // false = the 0.17.0 .. 0.31.0 behaviour: every caller gets the drawn answer at any time (fp_items_seam=off, or an
    // engine function missing in this build).
    void SetOn(bool on) { on_.store(on && engine_.invalidate && engine_.rebuild && engine_.vtable); }
    bool On() const { return on_.load(std::memory_order_relaxed); }
    bool Drawing() const { return drawing_.load(std::memory_order_relaxed); }

    Route RouteOf(Caller who) const {
        if (!On()) return Route::Draw;
        if (who == Caller::SetWorld) return Route::Game;
        if (who == Caller::Update && !Drawing()) return t_rebuilding ? Route::Game : Route::Remember;
        return Route::Draw;  // UpdateWorld inside the draw, or another caller (kept as before)
    }

    // UpdateWorld outside the draw handed the game's matrix to a node the draw would move.
    void Remember(void* node) { Add(pending_, node); }
    // The draw's answer for this node differed from the game's.
    void Answered(void* node) { Add(answered_, node); }
    // Any UpdateWorld for this node inside the draw (whatever the answer): it was rebuilt there.
    void Asked(void* node) {
        if (!expect_.load(std::memory_order_relaxed)) return;
        std::lock_guard lock(mutex_);
        for (Entry& e : refreshed_)
            if (e.node == node) e.vtable = 0;  // asked
    }

    // No draw follows for the remembered nodes (first person is off, or another loop draws): drop them unseen.
    void Forget() {
        std::lock_guard lock(mutex_);
        pending_.clear();
    }

    void Begin() {
        if (!On()) return;
        std::vector<Entry> work;
        {
            std::lock_guard lock(mutex_);
            work.swap(pending_);
        }
        for (const Entry& e : work) {
            if (!Same(e)) {
                counters.gone.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            engine_.invalidate(e.node);
            counters.refreshed.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::lock_guard lock(mutex_);
            refreshed_.clear();
            for (const Entry& e : work)
                if (Same(e)) refreshed_.push_back(e);
            expect_.store(int(refreshed_.size()));
        }
        drawing_.store(true);
    }

    void End() {
        drawing_.store(false);
        if (!On()) {
            std::lock_guard lock(mutex_);
            pending_.clear();
            answered_.clear();
            refreshed_.clear();
            expect_.store(0);
            return;
        }
        std::vector<Entry> work;
        {
            std::lock_guard lock(mutex_);
            work.swap(answered_);
            for (const Entry& e : refreshed_)
                if (e.vtable) counters.unasked.fetch_add(1, std::memory_order_relaxed);
            refreshed_.clear();
            expect_.store(0);
        }
        t_rebuilding = true;
        for (const Entry& e : work) {
            if (!Same(e)) {
                counters.gone.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            engine_.rebuild(e.node);     // the game's own world is stored again
            engine_.invalidate(e.node);  // and left dirty, with everything below it
            counters.rebuilt.fetch_add(1, std::memory_order_relaxed);
        }
        t_rebuilding = false;
    }

    Counters counters;

private:
    struct Entry {
        void* node;
        uint64_t vtable;
    };
    static constexpr size_t kMax = 512;  // per frame; more than that stay as the game has them

    bool Same(const Entry& e) const { return e.vtable && engine_.vtable(e.node) == e.vtable; }
    void Add(std::vector<Entry>& list, void* node) {
        if (!node) return;
        const uint64_t vt = engine_.vtable ? engine_.vtable(node) : 0;
        if (!vt) return;
        std::lock_guard lock(mutex_);
        for (const Entry& e : list)
            if (e.node == node) return;
        if (list.size() < kMax) list.push_back({node, vt});
    }

    Engine engine_{};
    std::atomic<bool> on_{false}, drawing_{false};
    std::atomic<int> expect_{0};
    std::mutex mutex_;
    std::vector<Entry> pending_, answered_, refreshed_;
};

}  // namespace hrvr::itemseam
