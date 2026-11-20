#include "bones.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "engineobj.h"
#include "game.h"
#include "log.h"

namespace hrvr::bones {
namespace {

constexpr size_t kCharEntity = 0x160;    // CHARACTER: entity handle
constexpr size_t kEntityNode = 0x68;     // entity: scene node handle
constexpr size_t kNodeSkeleton = 0x160;  // scene node: skeleton instance
constexpr size_t kSkelWorlds = 0x10;     // skeleton: bone world matrices, stride 0x40
constexpr size_t kSkelEntries = 0x1F8;   // skeleton: bone entries, stride 0x80 (+0x58 -> world)
constexpr size_t kSkelHashes = 0x210;    // skeleton: 8-byte {CRC32, ...} records
constexpr uint32_t kMaxBones = 512;

bool Finite(const pose::Mat4& m) {
    for (float v : m.m)
        if (!std::isfinite(v)) return false;
    return true;
}

}  // namespace

void* CharacterNode(void* character) {
    void* entity = obj::Cached(obj::Ptr(character, kCharEntity));
    return entity ? obj::Cached(obj::Ptr(entity, kEntityNode)) : nullptr;
}

void* Skeleton(void* node) { return obj::Ptr(node, kNodeSkeleton); }

int BoneIndex(void* skeleton, uint32_t nameHash) {
    uint32_t count = 0;
    if (!obj::Get(skeleton, 0, count) || count == 0 || count > kMaxBones) return -1;
    const void* hashes = obj::Ptr(skeleton, kSkelHashes);
    if (!hashes) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t h = 0;
        if (!obj::Get(hashes, size_t(i) * 8, h)) return -1;
        if (h == nameHash) return int(i);
    }
    return -1;
}

bool BoneWorld(void* skeleton, int index, pose::Mat4* out) {
    uint32_t count = 0;
    if (index < 0 || !obj::Get(skeleton, 0, count) || uint32_t(index) >= count || count > kMaxBones) return false;
    const void* worlds = obj::Ptr(skeleton, kSkelWorlds);
    return worlds && obj::Bytes(worlds, size_t(index) * 0x40, out->m, sizeof(out->m)) && Finite(*out);
}

bool CharacterBone(void* character, uint32_t nameHash, pose::Mat4* out) {
    void* skel = Skeleton(CharacterNode(character));
    return skel && BoneWorld(skel, BoneIndex(skel, nameHash), out);
}

bool NodeWorld(void* node, pose::Mat4* out) {
    uint32_t flags = 0;
    if (!obj::Get(node, game::off::NodeFlags, flags)) return false;
    if (((flags >> 1) & 1) == 0) {
        auto update = reinterpret_cast<void (*)(void*)>(game::Addr(game::Anchor::NodeUpdateWorld));
        if (!update) return false;
        __try {
            update(node);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    return obj::Bytes(node, game::off::NodeWorld, out->m, sizeof(out->m)) && Finite(*out);
}

bool Parents(void* skeleton, uint32_t count, int* out) {
    const void* parents = obj::Ptr(skeleton, 0x20);
    if (!parents || !count) return false;
    out[0] = -1;
    for (uint32_t i = 1; i < count; ++i) {
        uint32_t p = 0;
        if (!obj::Get(parents, size_t(i) * 4, p) || p >= i) return false;
        out[i] = int(p);
    }
    return true;
}

int CheckHierarchy(void* skeleton, uint32_t count, const int* parent, int n, int* checked) {
    const uint8_t* locals = static_cast<const uint8_t*>(obj::Ptr(skeleton, 0x08));
    const uint8_t* worlds = static_cast<const uint8_t*>(obj::Ptr(skeleton, kSkelWorlds));
    int good = 0;
    *checked = 0;
    if (!locals || !worlds) return 0;
    const uint32_t step = count > uint32_t(n) ? count / uint32_t(n) : 1;
    for (uint32_t i = 1; i < count && *checked < n; i += step) {
        pose::Mat4 l, pw, w;
        if (!obj::Bytes(locals, size_t(i) * 0x40, l.m, 64) || !obj::Bytes(worlds, size_t(parent[i]) * 0x40, pw.m, 64) ||
            !obj::Bytes(worlds, size_t(i) * 0x40, w.m, 64))
            continue;
        ++*checked;
        const pose::Mat4 c = pose::Mul(l, pw);
        float err = 0.f;
        for (int k = 0; k < 16; ++k) err = std::fmax(err, std::fabs(c.m[k] - w.m[k]));
        good += err < 0.01f;
    }
    return good;
}

void Describe(void* skeleton) {
    static void* last = nullptr;
    if (!skeleton || skeleton == last) return;
    last = skeleton;
    uint32_t count = 0;
    obj::Get(skeleton, 0, count);
    const uint8_t* worlds = static_cast<const uint8_t*>(obj::Ptr(skeleton, kSkelWorlds));
    const void* entries = obj::Ptr(skeleton, kSkelEntries);
    int same = 0, checked = 0;
    for (uint32_t i = 0; entries && worlds && i < count && i < 8; ++i, ++checked) {
        const void* m = obj::Ptr(entries, size_t(i) * 0x80 + 0x58);
        same += m == worlds + size_t(i) * 0x40;
    }
    const int head = BoneIndex(skeleton, Crc32("Head")), nose = BoneIndex(skeleton, Crc32("MNoseUp")),
              neck = BoneIndex(skeleton, Crc32("Neck")), hips = BoneIndex(skeleton, Crc32("Hips")),
              lh = BoneIndex(skeleton, Crc32("LeftHand")), rh = BoneIndex(skeleton, Crc32("RightHand"));
    Log("BONES skeleton %p: %u bones, entry->world matches the world array for %d/%d; Head=%d MNoseUp=%d Neck=%d "
        "Hips=%d LeftHand=%d RightHand=%d",
        skeleton, count, same, checked, head, nose, neck, hips, lh, rh);
}

}  // namespace hrvr::bones
