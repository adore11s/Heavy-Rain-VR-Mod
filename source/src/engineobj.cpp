#include "engineobj.h"

#include <windows.h>

#include <cstring>

#include "game.h"

namespace hrvr::obj {

void* Cached(void* handle) {
    __try {
        if (!handle || reinterpret_cast<uintptr_t>(handle) < 0x10000) return nullptr;
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(handle) + 0x30);
        if ((flags & 0x202000) == 0) return nullptr;
        return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(handle) + 0x50);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool Bytes(const void* obj, size_t off, void* out, size_t n) {
    __try {
        if (!obj || reinterpret_cast<uintptr_t>(obj) < 0x10000) return false;
        std::memcpy(out, static_cast<const uint8_t*>(obj) + off, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* Ptr(const void* obj, size_t off) {
    void* p = nullptr;
    return Get(obj, off, p) ? p : nullptr;
}

bool HasVtable(const void* obj, uint64_t vtRecordedVa) {
    const uintptr_t want = game::Va(vtRecordedVa);
    uintptr_t vt = 0;
    return want && Get(obj, 0, vt) && vt == want;
}

bool IsLive(const void* obj, uint64_t vtRecordedVa) {
    return HasVtable(obj, vtRecordedVa) && Cached(Ptr(obj, 0x20)) == obj;
}

uint64_t VtableVa(const void* obj) {
    uintptr_t vt = 0;
    if (!Get(obj, 0, vt) || !vt) return 0;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return vt - base + 0x140000000ull;
}

bool String(const void* str, char* out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    const char* p = static_cast<const char*>(Ptr(str, 0));
    int len = 0;
    if (!p || !Get(str, 8, len) || len < 0) return false;
    const size_t n = size_t(len) < cap - 1 ? size_t(len) : cap - 1;
    if (!Bytes(p, 0, out, n)) return false;
    out[n] = 0;
    return true;
}

}  // namespace hrvr::obj
