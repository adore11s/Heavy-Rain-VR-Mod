#pragma once
// Named bone access on Heavy Rain characters (STATIC 2026-10-27, two independent engine readers agree:
// the camera target alias resolver camerasystem_1404e98b0 and THOUGHTS_GUI's per-frame anchor 1404bcea0):
//   CHARACTER+0x160 handle -> entity; entity+0x68 handle -> scene node (camerasystem_1402defd0)
//   node+0x90 world matrix (refresh via NODE::UpdateWorld when +0xE8 bit1 is clear)
//   node+0x160 -> skeleton: +0x00 u32 bone count, *(+0x210) 8-byte records {u32 CRC32(name), ...},
//   *(+0x10) bone world matrices (stride 0x40, row-major, translation at +0x30),
//   *(+0x1F8) bone entries (stride 0x80; entry+0x58 -> that bone's world matrix).
//   *(+0x08) bone LOCAL matrices, *(+0x20) u32 parent index per bone (STATIC 2026-10-28, the skeleton's world update
//   0x1409fd270: world[0] = local[0] x root, then world[i] = local[i] x world[parent[i]] for i = 1.. in order, so a
//   parent always comes before its children).
// Bone names are HumanIK: Hips, Spine..Spine2, Neck, Head, MNoseUp, {Left,Right}{Shoulder,Arm,ForeArm,Hand}, ...
#include <cstdint>

#include "posemath.h"

namespace hrvr::bones {

// Standard CRC32 (poly 0xEDB88320), the engine's bone-name hash (viewport_140a07be0, table 0x1413d0480).
inline uint32_t Crc32(const char* s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; ++s) {
        c ^= static_cast<uint8_t>(*s);
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// Scene node of a SINT::CHARACTER (cached handles only), or nullptr.
void* CharacterNode(void* character);
// Skeleton instance of a scene node, or nullptr.
void* Skeleton(void* node);
// Index of the bone with this name hash, -1 when absent.
int BoneIndex(void* skeleton, uint32_t nameHash);
// World matrix of bone `index` (copied out), false when unreadable or not finite.
bool BoneWorld(void* skeleton, int index, pose::Mat4* out);
// Named bone world matrix on a character, in one call.
bool CharacterBone(void* character, uint32_t nameHash, pose::Mat4* out);
// Scene node world matrix (the character's root), refreshed as the engine does before reading it.
bool NodeWorld(void* node, pose::Mat4* out);
// The parent of every bone (out[0..count), -1 for the root); false when unreadable or not ordered (parent >= child).
bool Parents(void* skeleton, uint32_t count, int* out);
// How many of up to n bones satisfy local x world[parent] = world (the hierarchy really is what the game composes).
int CheckHierarchy(void* skeleton, uint32_t count, const int* parent, int n, int* checked);
// Once per skeleton: logs bone count and whether the entry table and the world array agree (diagnostic).
void Describe(void* skeleton);

}  // namespace hrvr::bones
