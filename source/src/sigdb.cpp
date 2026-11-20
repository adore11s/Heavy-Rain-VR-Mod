#include "sigdb.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace hrvr::sigdb {
namespace {

constexpr uint64_t kPreferredBase = 0x140000000ull;
const void* g_module = nullptr;  // the desk test's mapped exe (SetModuleForTest); null = the game process
HMODULE ModuleBase() { return g_module ? static_cast<HMODULE>(const_cast<void*>(g_module)) : GetModuleHandleW(nullptr); }

struct SigEntry {
    SigKind kind;
    const char* name;
    uint64_t va;
    const char* pattern;
    int dispOff;  // Data: offset of the RIP displacement inside the pattern
    int ipOff;    // Data: offset of the end of that instruction (the RIP base)
    int delta;    // Derived: recorded distance from the base entry; Slot: vtable slot index
    uint64_t baseVa;
    const char* rtti;
};

const SigEntry kEntries[] = {
#include "sigdb.inc"
};
constexpr size_t kCount = sizeof(kEntries) / sizeof(kEntries[0]);

struct Parsed {
    std::vector<uint8_t> bytes, mask;
    int anchor = -1;  // first offset of 4 concrete bytes
};
Parsed g_parsed[kCount];
uintptr_t g_runtime[kCount] = {};
Status g_status[kCount] = {};
bool g_parsedOnce = false;

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void ParseAll() {
    if (g_parsedOnce) return;
    g_parsedOnce = true;
    for (size_t i = 0; i < kCount; ++i) {
        Parsed& p = g_parsed[i];
        for (const char* s = kEntries[i].pattern; s && *s;) {
            while (*s == ' ') ++s;
            if (!*s) break;
            if (s[0] == '?') {
                p.bytes.push_back(0);
                p.mask.push_back(0);
                s += 2;
            } else {
                p.bytes.push_back(uint8_t(HexNibble(s[0]) * 16 + HexNibble(s[1])));
                p.mask.push_back(1);
                s += 2;
            }
        }
        for (size_t k = 0; k + 3 < p.mask.size(); ++k)
            if (p.mask[k] && p.mask[k + 1] && p.mask[k + 2] && p.mask[k + 3]) {
                p.anchor = int(k);
                break;
            }
    }
}

struct Section {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

// 0.34.0 (playbook 10.1 / 06.8, other store builds): the sections to search on a build this mod was not made on. A
// protected build shuffles section names and moves code out of ".text" (the 2019 Epic exe: a 360 MB executable section
// next to the original layout - reverse_engineering/legacy_epic_2019/binary_info.json), so every EXECUTABLE section is
// searched for code patterns and every readable, non-executable one for the RTTI records.
std::vector<Section> Sections(bool executable) {
    std::vector<Section> out;
    auto* base = reinterpret_cast<const uint8_t*>(ModuleBase());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const DWORD c = sec->Characteristics;
        if (!(c & IMAGE_SCN_MEM_READ) || !sec->Misc.VirtualSize) continue;
        if (executable ? !(c & IMAGE_SCN_MEM_EXECUTE) : (c & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
        out.push_back({base + sec->VirtualAddress, size_t(sec->Misc.VirtualSize)});
    }
    return out;
}
bool InAny(const std::vector<Section>& secs, uintptr_t p) {
    for (const Section& s : secs)
        if (p >= reinterpret_cast<uintptr_t>(s.begin) && p < reinterpret_cast<uintptr_t>(s.begin) + s.size) return true;
    return false;
}

bool MatchAt(const uint8_t* p, const Parsed& pat) {
    for (size_t k = 0; k < pat.bytes.size(); ++k)
        if (pat.mask[k] && p[k] != pat.bytes[k]) return false;
    return true;
}

// SEH: a section of another build may hold unreadable pages.
bool SafeMatchAt(const uint8_t* p, const Parsed& pat) {
    __try {
        return MatchAt(p, pat);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// First pattern byte that differs at p (-1: matches, -2: unreadable).
int FirstMismatch(const uint8_t* p, const Parsed& pat) {
    __try {
        for (size_t k = 0; k < pat.bytes.size(); ++k)
            if (pat.mask[k] && p[k] != pat.bytes[k]) return int(k);
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

using Index = std::unordered_multimap<uint32_t, size_t>;

// SEH-guarded sweep (no C++ objects with destructors in this frame). `first2`: a bit per possible first two key bytes,
// so the hash lookup runs only where a pattern can start (a 360 MB section is swept in well under a second).
bool ScanLoop(const Section& text, const Index& index, const uint8_t* first2, std::vector<const uint8_t*>* found) {
    __try {
        for (size_t off = 0; off + 4 <= text.size; ++off) {
            uint16_t k2;
            std::memcpy(&k2, text.begin + off, 2);
            if (!(first2[k2 >> 3] & (1u << (k2 & 7)))) continue;
            uint32_t key;
            std::memcpy(&key, text.begin + off, 4);
            auto range = index.equal_range(key);
            for (auto it = range.first; it != range.second; ++it) {
                const size_t i = it->second;
                const Parsed& p = g_parsed[i];
                if (off < size_t(p.anchor)) continue;
                const uint8_t* start = text.begin + off - p.anchor;
                if (start + p.bytes.size() > text.begin + text.size) continue;
                if (found[i].size() < 2 && MatchAt(start, p)) found[i].push_back(start);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// One pass over .text: every pattern is indexed by its first 4 concrete bytes. Records up to two
// match positions per pattern (two = ambiguous).
void ScanText(const std::vector<Section>& code, std::vector<const uint8_t*> found[]) {
    Index index;
    std::vector<uint8_t> first2(65536 / 8, 0);
    for (size_t i = 0; i < kCount; ++i) {
        const Parsed& p = g_parsed[i];
        if ((kEntries[i].kind == SigKind::Code || kEntries[i].kind == SigKind::Data) && p.anchor >= 0) {
            uint32_t key;
            std::memcpy(&key, &p.bytes[p.anchor], 4);
            index.emplace(key, i);
            const uint16_t k2 = uint16_t(key & 0xFFFF);
            first2[k2 >> 3] |= uint8_t(1u << (k2 & 7));
        }
    }
    for (const Section& s : code)
        if (!ScanLoop(s, index, first2.data(), found))
            Log("SIGDB code scan stopped at an unreadable page in a section at %p (%zu MB)", s.begin, s.size >> 20);
}

// RTTI, one pass over .rdata: a qword pointing at a x64 CompleteObjectLocator (signature 1, offset 0,
// pSelf = its own RVA) is slot -1 of that class's primary vtable; the COL's TypeDescriptor
// (pTypeDescriptor RVA) holds the mangled name at +0x10. Records (name, vtable) for every class.
struct VtableRecord {
    const char* name;
    uintptr_t vtable;
};

// The COL may sit in another data section than the vtable on a re-laid-out build: any address inside the image.
bool CollectVtables(const Section& rdata, std::vector<VtableRecord>* out) {
    const auto base = reinterpret_cast<uintptr_t>(ModuleBase());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
    const uintptr_t lo = base + 0x1000, hi = base + imageSize;
    __try {
        for (size_t v = 0; v + 16 <= rdata.size; v += 8) {
            const uintptr_t colAddr = *reinterpret_cast<const uintptr_t*>(rdata.begin + v);
            if (colAddr < lo || colAddr + 24 > hi || (colAddr & 3)) continue;
            const auto* col = reinterpret_cast<const uint32_t*>(colAddr);
            if (col[0] != 1 || col[1] != 0 || col[5] != uint32_t(colAddr - base)) continue;
            if (col[3] < 0x1000 || col[3] + 0x110 > imageSize) continue;
            const char* name = reinterpret_cast<const char*>(base + col[3] + 0x10);
            if (name[0] != '.' || name[1] != '?') continue;
            out->push_back({name, reinterpret_cast<uintptr_t>(rdata.begin + v + 8)});
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The vtable of the class with exactly this mangled name; 0 when absent or when two different
// primary vtables carry it (ambiguous).
uintptr_t FindVtable(const std::vector<VtableRecord>& vts, const char* name) {
    uintptr_t found = 0;
    for (const VtableRecord& r : vts)
        if (std::strcmp(r.name, name) == 0) {
            if (found && found != r.vtable) return 0;
            found = r.vtable;
        }
    return found;
}

// Slot `slot` of a vtable, accepted only when it points into an executable section.
uintptr_t ReadSlotRaw(uintptr_t vt, int slot) {
    __try {
        return reinterpret_cast<const uintptr_t*>(vt)[slot];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
uintptr_t ReadSlot(uintptr_t vt, int slot, const std::vector<Section>& code) {
    const uintptr_t f = ReadSlotRaw(vt, slot);
    return f && InAny(code, f) ? f : 0;
}

int32_t ReadDisp(const uint8_t* p) {
    int32_t d;
    std::memcpy(&d, p, 4);
    return d;
}

const char* StatusName(Status s) {
    switch (s) {
        case Status::Verified: return "verified";
        case Status::Moved: return "MOVED";
        case Status::Missing: return "MISSING";
        case Status::Ambiguous: return "AMBIGUOUS";
        default: return "unresolved";
    }
}

}  // namespace

void SetModuleForTest(const void* base) { g_module = base; }

bool EntryForTest(int i, const char** name, Status* status, uint64_t* recordedRva, uint64_t* foundRva) {
    if (i < 0 || size_t(i) >= kCount) return false;
    const auto base = reinterpret_cast<uintptr_t>(ModuleBase());
    *name = kEntries[i].name;
    *status = g_status[i];
    *recordedRva = kEntries[i].va - kPreferredBase;
    *foundRva = g_runtime[i] ? g_runtime[i] - base : 0;
    return true;
}

Summary Resolve(bool knownBuild, bool log) {
    ParseAll();
    const auto base = reinterpret_cast<uintptr_t>(ModuleBase());
    std::vector<const uint8_t*> found[kCount];
    std::vector<VtableRecord> vtables;
    std::vector<Section> code;
    if (!knownBuild) {
        // 0.34.0: every executable section (code) and every readable data section (RTTI), not only .text / .rdata.
        code = Sections(true);
        const std::vector<Section> data = Sections(false);
        size_t codeBytes = 0, dataBytes = 0;
        for (const Section& s : code) codeBytes += s.size;
        for (const Section& s : data) dataBytes += s.size;
        const ULONGLONG t0 = GetTickCount64();
        ScanText(code, found);
        for (const Section& s : data)
            if (!CollectVtables(s, &vtables)) Log("SIGDB RTTI pass stopped at an unreadable page in a section at %p", s.begin);
        if (log)
            Log("SIGDB another build: searched %zu code section(s), %zu MB, and %zu data section(s), %zu MB (%zu RTTI "
                "classes) in %llu ms", code.size(), codeBytes >> 20, data.size(), dataBytes >> 20, vtables.size(),
                static_cast<unsigned long long>(GetTickCount64() - t0));
    }
    Summary sum;
    for (size_t i = 0; i < kCount; ++i) {
        const SigEntry& e = kEntries[i];
        const uintptr_t recorded = base + uintptr_t(e.va - kPreferredBase);
        uintptr_t at = 0;
        Status st = Status::Missing;
        if (e.kind == SigKind::Slot) {
            if (knownBuild) {
                at = recorded;
                st = Status::Verified;
            } else if (const uintptr_t vt = FindVtable(vtables, e.rtti)) {
                at = ReadSlot(vt, e.delta, code);
                st = !at ? Status::Missing : at == recorded ? Status::Verified : Status::Moved;
            }
        } else if (e.kind == SigKind::Vtable) {
            if (knownBuild) {
                at = recorded;
                st = Status::Verified;
            } else {
                at = FindVtable(vtables, e.rtti);
                st = !at ? Status::Missing : at == recorded ? Status::Verified : Status::Moved;
            }
        } else if (e.kind == SigKind::Code || e.kind == SigKind::Data) {
            if (knownBuild) {
                if (e.kind == SigKind::Code) {
                    at = recorded;
                    st = SafeMatchAt(reinterpret_cast<const uint8_t*>(recorded), g_parsed[i]) ? Status::Verified : Status::Missing;
                    if (st == Status::Missing) {
                        if (log)
                            Log("SIGDB %s: pattern differs in place at byte %d", e.name,
                                FirstMismatch(reinterpret_cast<const uint8_t*>(recorded), g_parsed[i]));
                        at = 0;
                    }
                } else {
                    at = recorded;  // a global: the analysed layout
                    st = Status::Verified;
                }
            } else if (found[i].size() == 1) {
                const uint8_t* site = found[i][0];
                at = e.kind == SigKind::Code ? reinterpret_cast<uintptr_t>(site)
                                             : reinterpret_cast<uintptr_t>(site + e.ipOff) + ReadDisp(site + e.dispOff);
                st = at == recorded ? Status::Verified : Status::Moved;
            } else {
                st = found[i].empty() ? Status::Missing : Status::Ambiguous;
            }
        }
        g_runtime[i] = at;
        g_status[i] = st;
    }
    // Derived entries last: their base must be resolved.
    for (size_t i = 0; i < kCount; ++i) {
        const SigEntry& e = kEntries[i];
        if (e.kind != SigKind::Derived) continue;
        const uintptr_t baseAt = Va(e.baseVa);
        const uintptr_t recorded = base + uintptr_t(e.va - kPreferredBase);
        g_runtime[i] = baseAt ? baseAt + e.delta : 0;
        g_status[i] = !baseAt ? Status::Missing : g_runtime[i] == recorded ? Status::Verified : Status::Moved;
    }
    for (size_t i = 0; i < kCount; ++i) {
        ++sum.total;
        switch (g_status[i]) {
            case Status::Verified: ++sum.verified; break;
            case Status::Moved: ++sum.moved; break;
            case Status::Ambiguous: ++sum.ambiguous; break;
            default: ++sum.missing; break;
        }
        if (log && g_status[i] != Status::Verified)
            Log("SIGDB %-34s %s (recorded rva 0x%llx -> %s0x%llx)", kEntries[i].name, StatusName(g_status[i]),
                static_cast<unsigned long long>(kEntries[i].va - kPreferredBase), g_runtime[i] ? "rva " : "",
                static_cast<unsigned long long>(g_runtime[i] ? g_runtime[i] - base : 0));
    }
    if (log)
        Log("SIGDB game addresses: %d recorded, %d verified, %d moved, %d missing, %d ambiguous", sum.total, sum.verified,
            sum.moved, sum.missing, sum.ambiguous);
    return sum;
}

uintptr_t Va(uint64_t recordedVa) {
    for (size_t i = 0; i < kCount; ++i)
        if (kEntries[i].va == recordedVa) return g_runtime[i];
    return 0;
}

Status StatusOf(uint64_t recordedVa) {
    for (size_t i = 0; i < kCount; ++i)
        if (kEntries[i].va == recordedVa) return g_status[i];
    return Status::Unresolved;
}

}  // namespace hrvr::sigdb
