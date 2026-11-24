#pragma once
// v1.3 (owner 2026-11-24: the glasses copy must show for every player, but the copyrighted model must NEVER ship - not in
// the repo, the zip or the DLL): the ARI glasses copy's mesh is read at run time from the PLAYER'S OWN game files, the
// same way tools/ari_copy.py glasses() + write_inc() build the (local-only) src/ari_glasses_mesh.inc:
//   BigFile_WIN.dbg (name -> package id) + .idx (class 29 record: offset, size, tail, part .dat / .dNN)
//   -> `segs` blocks inflated (inflate.h) + the record's uncompressed tail      (rig_extract.read_objects, read_package)
//   -> DC_INFO object table + DC_DATA stream spans                              (rig_sockets.object_table, stream_span)
//   -> MESHDATA v0x1c of the three rigid meshes 30070 / 30071 / 30072           (rig_mesh.parse_meshdata, geometry)
//   -> NODE 62732..62734 +0x14 local 4x4 (64 byte-reversed bytes = big-endian floats, last element first), +0x94 parent;
//      the two temple arms placed by their own node's local at rest (OPEN) on the frame part's space
//   -> smooth normals (area-weighted face sums), lens flag per vertex (any triangle of material 21070).
// Every read is bounds-checked; any mismatch gives false and a reason (logged; no copy is drawn). Desk proof:
// tests/ari_mesh_load_tests.cpp reproduces the .inc exactly from the archives.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "inflate.h"

namespace hrvr::aricopy {

struct Vertex {
    float x, y, z, nx, ny, nz, lens;
};
struct Mesh {
    std::vector<Vertex> verts;
    std::vector<uint16_t> indices;  // triangle list
};

namespace load {

constexpr uint32_t kPackageClass = 29, kMeshClass = 0x852, kNodeClass = 0x7d2, kMaterialClass = 0x855,
                   kExtraByteClass = 0xffb;
constexpr char kPackage[] = "HR_PROP_ARIGLASSES";
struct Part {
    uint32_t mesh, node;
    const char* name;  // MESHDATA export name
};
constexpr Part kParts[3] = {{30070, 62732, "MAIN_s"}, {30071, 62733, "ARI_R_s"}, {30072, 62734, "ARI_L_s"}};
constexpr uint32_t kLensMaterial = 21070;  // HR_GLASSES_ARI_GLASSES_MAT (plastic is 21069)

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline float BeF32(const uint8_t* p) {
    const uint32_t u = Be32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline float LeF32(const uint8_t* p) {
    float f;
    std::memcpy(&f, p, 4);
    return f;
}

inline bool Fail(std::string* why, const std::string& s) {
    if (why) *why = s;
    return false;
}

// A bounds-checked big-endian cursor over [0, n).
struct Cur {
    const uint8_t* d;
    size_t n, p;
    bool ok = true;
    bool Need(uint64_t k) {
        if (!ok || uint64_t(p) + k > n) ok = false;
        return ok;
    }
    uint32_t U32() {
        if (!Need(4)) return 0;
        const uint32_t v = Be32(d + p);
        p += 4;
        return v;
    }
    uint8_t U8() {
        if (!Need(1)) return 0;
        return d[p++];
    }
    void Skip(uint64_t k) {
        if (Need(k)) p += size_t(k);
    }
    std::string String() {
        const uint32_t len = U32();
        if (len > 4096) ok = false;
        if (!Need(len)) return {};
        std::string s(reinterpret_cast<const char*>(d + p), len);
        p += len;
        return s;
    }
};

inline bool ReadFileRange(const std::wstring& path, uint64_t off, uint64_t size, std::vector<uint8_t>* out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    bool ok = true;
    if (size == UINT64_MAX) {
        ok = _fseeki64(f, 0, SEEK_END) == 0;
        const long long end = ok ? _ftelli64(f) : -1;
        ok = ok && end >= 0 && _fseeki64(f, 0, SEEK_SET) == 0;
        size = ok ? uint64_t(end) : 0;
    } else {
        ok = _fseeki64(f, static_cast<long long>(off), SEEK_SET) == 0;
    }
    ok = ok && size < (uint64_t(1) << 31);
    if (ok) {
        out->resize(size_t(size));
        ok = size == 0 || std::fread(out->data(), 1, out->size(), f) == out->size();
    }
    std::fclose(f);
    return ok;
}

struct Record {
    uint32_t cls, one, id, off, size, tail, part;
};

// The package's .idx record, found by its name in the .dbg (same records, same order; matched by class + id).
inline bool FindPackage(const std::wstring& base, const char* name, Record* rec, std::string* why) {
    std::vector<uint8_t> idx, dbg;
    if (!ReadFileRange(base + L".idx", 0, UINT64_MAX, &idx)) return Fail(why, "cannot read BigFile_WIN.idx");
    if (idx.size() < 0x68 || std::memcmp(idx.data(), "QUANTICDREAMTABINDEX", 20) != 0)
        return Fail(why, "BigFile_WIN.idx: not a QUANTICDREAMTABINDEX");
    const uint32_t count = Be32(idx.data() + 0x64);
    if (0x68 + 28ull * count != idx.size()) return Fail(why, "BigFile_WIN.idx: record count does not match its size");
    if (!ReadFileRange(base + L".dbg", 0, UINT64_MAX, &dbg)) return Fail(why, "cannot read BigFile_WIN.dbg");
    if (dbg.size() < 0x1C) return Fail(why, "BigFile_WIN.dbg too short");
    const uint32_t n = Be32(dbg.data() + 0x18);
    const size_t want = std::strlen(name);
    size_t pos = 0x1C;
    int64_t at = -1;
    uint32_t id = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (pos + 16 > dbg.size()) break;
        const uint32_t cls = Be32(dbg.data() + pos), oid = Be32(dbg.data() + pos + 8), ln = Be32(dbg.data() + pos + 12);
        if (pos + 16ull + ln > dbg.size()) break;
        if (cls == kPackageClass && ln == want && std::memcmp(dbg.data() + pos + 16, name, want) == 0) {
            at = i, id = oid;
            break;
        }
        pos += 40ull + ln;
    }
    if (at < 0) return Fail(why, std::string("package ") + name + " not named in BigFile_WIN.dbg");
    auto recAt = [&](uint32_t i) {
        const uint8_t* p = idx.data() + 0x68 + 28ull * i;
        return Record{Be32(p), Be32(p + 4), Be32(p + 8), Be32(p + 12), Be32(p + 16), Be32(p + 20), Be32(p + 24)};
    };
    if (uint32_t(at) < count) {
        const Record r = recAt(uint32_t(at));
        if (r.cls == kPackageClass && r.id == id) {
            *rec = r;
            return true;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const Record r = recAt(i);
        if (r.cls == kPackageClass && r.id == id) {
            *rec = r;
            return true;
        }
    }
    return Fail(why, std::string("package ") + name + " has no BigFile_WIN.idx record");
}

// The package's object data: the `segs` blocks inflated + the record's uncompressed tail (or a QZIP body).
inline bool ReadPackage(const std::wstring& base, const Record& r, std::vector<uint8_t>* raw, std::string* why) {
    wchar_t ext[8];
    if (r.part == 0)
        std::swprintf(ext, 8, L".dat");
    else
        std::swprintf(ext, 8, L".d%02u", r.part % 100);
    std::vector<uint8_t> rec;
    if (!ReadFileRange(base + ext, r.off, r.size, &rec)) return Fail(why, "cannot read the package bytes");
    raw->clear();
    if (rec.size() >= 16 && std::memcmp(rec.data(), "segs", 4) == 0) {
        const uint32_t n = Be16(rec.data() + 6), rawSize = Be32(rec.data() + 8), packed = Be32(rec.data() + 12);
        const size_t hdr = (n * 8 + 0x1F) & ~size_t(0xF);
        const size_t avail = std::min<size_t>(rec.size(), hdr + size_t(packed));
        if (16 + 8ull * n > avail) return Fail(why, "segs block table past the record");
        raw->reserve(size_t(rawSize) + r.tail);
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* b = rec.data() + 16 + 8 * i;
            const size_t zs = Be16(b) ? Be16(b) : 0x10000, rs = Be16(b + 2) ? Be16(b + 2) : 0x10000;
            const uint32_t o = Be32(b + 4);
            const size_t start = hdr + (o & ~1u), before = raw->size();
            if (o & 1) {
                if (start + zs > avail) return Fail(why, "segs block past the record");
                if (!inflate::Block(rec.data() + start, zs, raw, rs) || raw->size() - before != rs)
                    return Fail(why, "segs block " + std::to_string(i) + " does not inflate");
            } else {
                if (start + rs > avail) return Fail(why, "stored segs block past the record");
                raw->insert(raw->end(), rec.data() + start, rec.data() + start + rs);
            }
        }
        if (raw->size() != rawSize) return Fail(why, "segs: inflated size differs from the header");
        if (r.tail) {
            if (r.tail > rec.size()) return Fail(why, "record tail larger than the record");
            raw->insert(raw->end(), rec.end() - r.tail, rec.end());
        }
        return true;
    }
    if (rec.size() >= 5 && std::memcmp(rec.data(), "QZIP", 4) == 0) {
        raw->assign(rec.begin() + 5, rec.end());
        return true;
    }
    return Fail(why, "package is neither segs nor QZIP");
}

struct Object {
    uint32_t cls, id;
    size_t begin, end;  // the object's stream body
};

// DC_DATA v7 stream header (reader 0x140ae77d0): u32 body size, u8 parts flag, u32 B, [parts], u8 bulk flag, [bulk].
inline bool StreamSpan(const std::vector<uint8_t>& raw, size_t p, size_t* begin, size_t* end) {
    if (p + 9 > raw.size()) return false;
    const uint64_t size = Be32(&raw[p]);
    const uint8_t parts = raw[p + 4];
    const uint64_t b = Be32(&raw[p + 5]);
    uint64_t q = p + 9, n = 0;
    if (parts == 1) {
        if (q + 4 > raw.size()) return false;
        n = Be32(&raw[size_t(q)]);
        q += 4 + 4 * n * (b < 2 ? 1 : b);
    }
    if (q >= raw.size()) return false;
    const uint8_t bulk = raw[size_t(q)];
    q += 1;
    if (bulk == 1) q += b < 2 ? 1 + 12 * n : 12 * n * b;
    if (q + size > raw.size()) return false;
    *begin = size_t(q), *end = size_t(q + size);
    return true;
}

inline bool ObjectTable(const std::vector<uint8_t>& raw, std::vector<Object>* objs, std::string* why) {
    if (raw.size() < 20 || std::memcmp(raw.data(), "DC_INFO ", 8) != 0) return Fail(why, "no DC_INFO");
    const uint32_t size = Be32(&raw[12]), count = Be32(&raw[16]);
    size_t p = 20;
    objs->clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 8 > raw.size()) return Fail(why, "DC_INFO past the data");
        const uint32_t c = Be32(&raw[p]), id = Be32(&raw[p + 4]);
        p += 8 + (c == kExtraByteClass);
        objs->push_back({c, id, 0, 0});
    }
    if (p != 16ull + size || p + 16 > raw.size() || std::memcmp(&raw[p], "DC_DATA ", 8) != 0)
        return Fail(why, "DC_INFO does not end at DC_DATA");
    const uint64_t end = p + 16ull + Be32(&raw[p + 12]);
    p += 16;
    for (Object& o : *objs) {
        if (!StreamSpan(raw, p, &o.begin, &o.end)) return Fail(why, "DC_DATA stream past the data");
        p = o.end;
    }
    if (p > end) return Fail(why, "DC_DATA streams run past its total");
    return true;
}

inline const Object* FindObject(const std::vector<Object>& objs, uint32_t cls, uint32_t id) {
    for (const Object& o : objs)
        if (o.cls == cls && o.id == id) return &o;
    return nullptr;
}

struct Stream {
    uint32_t elements = 0, count = 0, stride = 0, stride2 = 0;
    size_t decl = 0, data = 0;
};
struct Sub {
    uint32_t prim = 0, stream = 0, first = 0, count = 0, ib = 0, firstIndex = 0, indices = 0;
    int64_t material = -1;
};
struct MeshData {
    uint32_t mode = 0;
    std::vector<Stream> streams;
    std::vector<std::pair<size_t, uint32_t>> ibs;  // offset, u16 count
    std::vector<Sub> subs;
    std::string name;
};

// MESHDATA chunk (version 0x1c, reader 0x140950d80) at p, inside [.., end) - rig_mesh.parse_meshdata.
inline bool ParseMeshdata(const std::vector<uint8_t>& raw, size_t p, size_t end, MeshData* m, std::string* why) {
    if (p + 12 > end || Be32(&raw[p + 8]) != 0x1C) return Fail(why, "MESHDATA is not version 0x1c");
    static const uint32_t kTypeSize[7] = {0, 0, 12, 16, 4, 4, 8};
    Cur c{raw.data(), end, p + 12};
    m->mode = c.U32();
    if (m->mode != 1) {
        const uint32_t ns = c.U32();
        if (ns > 64) return Fail(why, "MESHDATA stream count");
        for (uint32_t i = 0; i < ns && c.ok; ++i) {
            Stream s;
            const uint8_t second = c.U8();
            s.elements = c.U32(), s.count = c.U32(), s.stride = c.U32();
            s.stride2 = second ? c.U32() : 0;
            m->streams.push_back(s);
        }
        const uint32_t nib = c.U32();
        if (nib > 4096) return Fail(why, "MESHDATA index buffer count");
        std::vector<uint32_t> counts;
        for (uint32_t i = 0; i < nib && c.ok; ++i) counts.push_back(c.U32());
        for (uint32_t n : counts) {
            m->ibs.push_back({c.p, n});
            c.Skip(2ull * n);
        }
    }
    const uint32_t nsub = c.U32();
    if (nsub > 4096) return Fail(why, "MESHDATA sub-mesh count");
    for (uint32_t i = 0; i < nsub && c.ok; ++i) {
        Sub s;
        if (m->mode != 1) {
            s.prim = c.U32();
            c.U8(), c.U8();
            s.stream = c.U32(), s.first = c.U32(), s.count = c.U32(), s.ib = c.U32(), s.firstIndex = c.U32(),
            s.indices = c.U32();
            c.Skip(4ull * c.U32());  // palette
            const uint32_t cls = c.U32(), mid = c.U32();
            s.material = cls == kMaterialClass ? int64_t(mid) : -1;
            c.Skip(9);
        }
        c.Skip(4 + 60 + 16);
        m->subs.push_back(s);
    }
    if (m->mode != 1) {
        const uint32_t morphs = c.U32();
        for (uint32_t i = 0; i < morphs && c.ok; ++i) {
            const uint64_t n = c.U32();
            const uint8_t full = c.U8();
            c.Skip(n * 18 + (full == 1 ? n * 20 : 0));
        }
    }
    const uint32_t bindings = c.U32();
    for (uint32_t i = 0; i < bindings && c.ok; ++i) {
        c.String();
        c.Skip(15);
    }
    c.Skip(8ull * c.U32());
    c.Skip(60 + 16 + 4);
    const uint32_t extra = c.U32();
    if (extra) {
        for (uint32_t i = 0; i < extra && c.ok; ++i) {
            c.String();
            c.Skip(4);
        }
        for (size_t s = 0; s < m->streams.size() && c.ok; ++s) {
            const uint32_t groups = c.U32();
            c.Skip(4);
            for (uint32_t g = 0; g < groups && c.ok; ++g) c.Skip(26ull * c.U32());
        }
    }
    for (Stream& st : m->streams) {
        st.decl = c.p;
        c.Skip(4ull * st.elements);
        if (!c.ok) break;
        uint32_t tiled = 0;
        for (uint32_t e = 0; e < st.elements; ++e) {
            const uint8_t* el = raw.data() + st.decl + 4 * e;
            if (el[0] == 0) tiled += el[2] < 7 ? kTypeSize[el[2]] : 0;
        }
        if (st.stride && tiled != st.stride) return Fail(why, "MESHDATA elements do not tile the stride");
        st.data = c.p;
        c.Skip(uint64_t(st.count) * (uint64_t(st.stride) + st.stride2));
    }
    m->name = c.String();
    if (!c.ok) return Fail(why, "MESHDATA runs past its object");
    return true;
}

// Positions, triangles (absolute in the returned positions) and material per triangle - rig_mesh.geometry (rigid).
inline bool Geometry(const std::vector<uint8_t>& raw, const MeshData& m, std::vector<float>* pos, std::vector<uint32_t>* tri,
                     std::vector<int64_t>* triMat, std::string* why) {
    uint32_t base = 0;
    for (const Sub& s : m.subs) {
        if (s.prim != 3 || s.indices < 3) continue;
        if (s.stream >= m.streams.size()) return Fail(why, "sub-mesh stream out of range");
        const Stream& st = m.streams[s.stream];
        const uint8_t *use0 = nullptr, *sec0 = nullptr;
        for (uint32_t e = 0; e < st.elements; ++e) {  // later entries win, as the tool's dict
            const uint8_t* el = raw.data() + st.decl + 4 * e;
            if (el[3] == 0 && el[0] == 0) use0 = el;
            if (el[3] == 0 && el[0] == 1) sec0 = el;
        }
        const bool cloth = !use0 && sec0 && st.stride2 > 0;
        const uint8_t* pe = cloth ? sec0 : use0;
        if (!pe || pe[2] != 2) continue;
        const uint64_t n = s.count, rowStride = cloth ? st.stride2 : st.stride;
        const uint64_t first = cloth ? st.data + uint64_t(st.count) * st.stride + uint64_t(s.first) * st.stride2
                                     : st.data + uint64_t(s.first) * st.stride;
        if (uint64_t(s.first) + n > st.count || first + (n ? (n - 1) * rowStride : 0) + pe[1] + 12 > raw.size())
            return Fail(why, "sub-mesh vertices past the stream");
        for (uint64_t v = 0; v < n; ++v) {
            const uint8_t* q = raw.data() + first + v * rowStride + pe[1];
            pos->push_back(LeF32(q)), pos->push_back(LeF32(q + 4)), pos->push_back(LeF32(q + 8));
        }
        if (s.ib >= m.ibs.size()) return Fail(why, "sub-mesh index buffer out of range");
        const auto& ib = m.ibs[s.ib];
        const uint32_t k = s.indices / 3 * 3;
        if (uint64_t(s.firstIndex) + k > ib.second) return Fail(why, "sub-mesh indices past the index buffer");
        std::vector<uint32_t> idx(k);
        uint32_t lo = UINT32_MAX, hi = 0;
        for (uint32_t i = 0; i < k; ++i) {
            idx[i] = Be16(raw.data() + ib.first + 2ull * (s.firstIndex + i));
            lo = std::min(lo, idx[i]), hi = std::max(hi, idx[i]);
        }
        if (k && hi >= n && lo >= s.first) {
            for (uint32_t& i : idx) i -= s.first;
            hi -= s.first;
        }
        if (k && hi >= n) return Fail(why, "index outside the sub-mesh's vertices");
        for (uint32_t i : idx) tri->push_back(i + base);
        triMat->insert(triMat->end(), k / 3, s.material);
        base += uint32_t(n);
    }
    if (tri->empty()) return Fail(why, "mesh without triangles");
    return true;
}

// NODE record (class 0x7d2): +0x14 the local 4x4 stored last element first, big-endian (row-major, row vectors);
// +0x94 parent handle (class, id; -1 when not a node).
inline bool NodeLocal(const std::vector<uint8_t>& raw, const Object& o, double m[16], int64_t* parent) {
    if (o.begin + 0x9C > o.end) return false;
    for (int k = 0; k < 16; ++k) m[k] = BeF32(&raw[o.begin + 0x50 - 4 * k]);
    *parent = Be32(&raw[o.begin + 0x94]) == kNodeClass ? int64_t(Be32(&raw[o.begin + 0x98])) : -1;
    return true;
}

}  // namespace load

// The open glasses (frame part space, metres) from <resources>\BigFile_WIN.*; false + reason on any mismatch.
inline bool LoadGlasses(const std::wstring& resources, Mesh* out, std::string* why) {
    using namespace load;
    const std::wstring base = resources + L"\\BigFile_WIN";
    Record rec{};
    std::vector<uint8_t> raw;
    std::vector<Object> objs;
    if (!FindPackage(base, kPackage, &rec, why) || !ReadPackage(base, rec, &raw, why) || !ObjectTable(raw, &objs, why))
        return false;
    // every node of the package is a rigid transform (as the tool asserts)
    for (const Object& o : objs) {
        if (o.cls != kNodeClass) continue;
        double m[16];
        int64_t parent = 0;
        if (!NodeLocal(raw, o, m, &parent)) return Fail(why, "NODE record too short");
        double err = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double d = 0;
                for (int k = 0; k < 3; ++k) d += m[i * 4 + k] * m[j * 4 + k];
                err = std::fmax(err, std::fabs(d - (i == j ? 1.0 : 0.0)));
            }
        const double det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
                           m[2] * (m[4] * m[9] - m[5] * m[8]);
        if (err > 1e-5 || std::fabs(det - 1) > 1e-4) return Fail(why, "NODE " + std::to_string(o.id) + " is not a rotation");
    }
    std::vector<double> pos;
    std::vector<uint32_t> tri;
    std::vector<int64_t> mat;
    for (int part = 0; part < 3; ++part) {
        const Part& pt = kParts[part];
        const Object* mo = FindObject(objs, kMeshClass, pt.mesh);
        const Object* no = FindObject(objs, kNodeClass, pt.node);
        if (!mo || !no) return Fail(why, std::string("part ") + pt.name + " not in the package");
        size_t at = mo->end;
        for (size_t q = mo->begin; q + 8 <= mo->end; ++q)
            if (std::memcmp(&raw[q], "MESHDATA", 8) == 0) {
                at = q;
                break;
            }
        if (at == mo->end) return Fail(why, std::string("part ") + pt.name + " has no MESHDATA");
        MeshData md;
        if (!ParseMeshdata(raw, at, mo->end, &md, why)) return false;
        if (md.name.substr(0, md.name.find('#')) != pt.name) return Fail(why, "mesh " + md.name + " is not " + pt.name);
        std::vector<float> p;
        std::vector<uint32_t> t;
        if (!Geometry(raw, md, &p, &t, &mat, why)) return false;
        double local[16];
        int64_t parent = 0;
        NodeLocal(raw, *no, local, &parent);
        const uint32_t first = uint32_t(pos.size() / 3);
        for (size_t v = 0; v < p.size(); v += 3) {
            const double x = p[v], y = p[v + 1], z = p[v + 2];
            if (part == 0) {
                pos.push_back(x), pos.push_back(y), pos.push_back(z);
            } else {  // the arm at its rest (open) place on the frame: [p, 1] x local
                if (parent != int64_t(kParts[0].node)) return Fail(why, std::string(pt.name) + " is not under the frame");
                for (int j = 0; j < 3; ++j)
                    pos.push_back(x * local[j] + y * local[4 + j] + z * local[8 + j] + local[12 + j]);
            }
        }
        for (uint32_t i : t) tri.push_back(i + first);
    }
    const size_t nv = pos.size() / 3, nt = tri.size() / 3;
    if (nv > 65535) return Fail(why, "too many vertices for 16-bit indices");
    std::vector<double> nrm(nv * 3, 0.0);
    std::vector<uint8_t> lens(nv, 0);
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t a = tri[3 * t], b = tri[3 * t + 1], c = tri[3 * t + 2];
        const double* A = &pos[3 * a];
        const double* B = &pos[3 * b];
        const double* C = &pos[3 * c];
        const double u[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, w[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
        const double f[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        for (uint32_t v : {a, b, c}) {
            for (int k = 0; k < 3; ++k) nrm[3 * v + k] += f[k];
            if (mat[t] == int64_t(kLensMaterial)) lens[v] = 1;
        }
    }
    out->verts.resize(nv);
    for (size_t v = 0; v < nv; ++v) {
        double l = std::sqrt(nrm[3 * v] * nrm[3 * v] + nrm[3 * v + 1] * nrm[3 * v + 1] + nrm[3 * v + 2] * nrm[3 * v + 2]);
        if (l == 0) l = 1;
        out->verts[v] = {float(pos[3 * v]),      float(pos[3 * v + 1]),     float(pos[3 * v + 2]), float(nrm[3 * v] / l),
                         float(nrm[3 * v + 1] / l), float(nrm[3 * v + 2] / l), lens[v] ? 1.f : 0.f};
    }
    out->indices.resize(tri.size());
    for (size_t i = 0; i < tri.size(); ++i) out->indices[i] = uint16_t(tri[i]);  // < nv <= 65535
    return true;
}

}  // namespace hrvr::aricopy
