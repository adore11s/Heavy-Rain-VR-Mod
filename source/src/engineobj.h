#pragma once
// SEH-guarded reads of engine objects that can be freed between frames (B2S guide rule e).
// KCOM idiom in Heavy Rain (STATIC, every resolve site, e.g. camerasystem_1402defd0):
//   handle+0x30 & 0x202000 -> the object is cached at handle+0x50; an object's own handle is at +0x20.
// Only cached handles are resolved here: the slow resolver has side effects and may not run off its thread.
#include <cstddef>
#include <cstdint>

namespace hrvr::obj {

// Object behind a cached KCOM handle, or nullptr (also for an uncached or unreadable handle).
void* Cached(void* handle);
// Pointer / value fields, nullptr / false when unreadable.
void* Ptr(const void* obj, size_t off);
bool Bytes(const void* obj, size_t off, void* out, size_t n);
template <typename T>
bool Get(const void* obj, size_t off, T& out) {
    return Bytes(obj, off, &out, sizeof(T));
}
// The object's vtable equals the class's (recorded VA, mapped through sigdb).
bool HasVtable(const void* obj, uint64_t vtRecordedVa);
// A live KCOM object of that class: the vtable matches and its own handle (+0x20) resolves back to it.
// A freed copy that still holds the vtable fails once the handle is reused.
bool IsLive(const void* obj, uint64_t vtRecordedVa);
// Recorded VA (0x14xxxxxxx) of the object's vtable, 0 when unreadable (logs).
uint64_t VtableVa(const void* obj);
// Engine string {char* ptr; int len; int cap} (kernelnet_140803f00): copies at most cap-1 chars.
bool String(const void* str, char* out, size_t cap);

}  // namespace hrvr::obj
