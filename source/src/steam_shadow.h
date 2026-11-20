#pragma once
#include <array>
#include <cstddef>

namespace hrvr::vrinput::detail {
// ISteamController (v007) slots Heavy Rain calls with a pad handle or while loading action
// handles: GetConnectedControllers 3, GetActionSetHandle 4, ActivateActionSet 5,
// DeactivateAllActionSetLayers 9, GetDigitalActionHandle 11, GetDigitalActionData 12,
// GetAnalogActionHandle 14, GetAnalogActionData 15, GetMotionData 20, TriggerVibration 23,
// GetInputTypeForHandle 26. The table is bounded; the remaining slots and the preceding RTTI
// pointer are preserved exactly as published by Steam.
constexpr size_t kOverrideSlots[] = {3, 4, 5, 9, 11, 12, 14, 15, 20, 23, 26};
struct ShadowTable {
    static constexpr size_t kSlots = 64;
    static constexpr size_t kOverrides = sizeof(kOverrideSlots) / sizeof(kOverrideSlots[0]);
    std::array<void*, kSlots + 1> words{};

    void Build(void* const* original, void* const* detours) {
        words[0] = original[-1];
        for (size_t i = 0; i < kSlots; ++i) words[i + 1] = original[i];
        for (size_t i = 0; i < kOverrides; ++i) words[kOverrideSlots[i] + 1] = detours[i];
    }
    void** Table() { return words.data() + 1; }
};
}  // namespace hrvr::vrinput::detail
