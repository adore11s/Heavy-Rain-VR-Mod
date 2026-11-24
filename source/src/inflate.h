#pragma once
// v1.3: inflate (RFC 1951 deflate, RFC 1950 zlib wrapper) for the game's package blocks, so the DLL can read game data
// from the PLAYER'S OWN archives at run time (ari_mesh_load.h) instead of shipping it. The game's `segs` blocks are zlib
// or raw deflate (rig_extract.inflate: zlib first, raw when that fails - HR_PROP_ARIGLASSES: 14 raw blocks).
// Small canonical-Huffman decoder (bit by bit, after the RFC's own description); every read is bounds-checked and any
// malformed stream returns false - never trusts the file. Tests: tests/ari_mesh_load_tests.cpp (stored / fixed /
// dynamic vectors made by Python's zlib, and every block of the real package).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hrvr::inflate {
namespace detail {

struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    uint64_t buf = 0;
    int cnt = 0;
    bool bad = false;
    uint32_t Get(int need) {
        while (cnt < need) {
            if (pos >= n) {
                bad = true;
                return 0;
            }
            buf |= uint64_t(p[pos++]) << cnt;
            cnt += 8;
        }
        const uint32_t v = uint32_t(buf & ((uint64_t(1) << need) - 1));
        buf >>= need;
        cnt -= need;
        return v;
    }
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[320];
};

// Canonical code from code lengths. < 0 over-subscribed, 0 complete, > 0 incomplete.
inline int Build(Huffman& h, const uint16_t* length, int n) {
    for (uint16_t& c : h.count) c = 0;
    for (int s = 0; s < n; ++s) ++h.count[length[s]];
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s)
        if (length[s]) h.symbol[offs[length[s]]++] = uint16_t(s);
    return left;
}

// One symbol (codes are sent most significant bit first). < 0 on error.
inline int Decode(Bits& b, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= int(b.Get(1));
        if (b.bad) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

inline bool Codes(Bits& b, std::vector<uint8_t>& out, size_t start, const Huffman& lit, const Huffman& dist, size_t cap) {
    static const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = Decode(b, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() - start >= cap) return false;
            out.push_back(uint8_t(sym));
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        const size_t len = kLenBase[sym] + b.Get(kLenExtra[sym]);
        const int ds = Decode(b, dist);
        if (ds < 0 || ds >= 30 || b.bad) return false;
        const size_t d = kDistBase[ds] + b.Get(kDistExtra[ds]);
        if (b.bad || d > out.size() - start || out.size() - start + len > cap) return false;
        const size_t from = out.size() - d;
        for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
    }
}

}  // namespace detail

// One raw deflate stream from data[0, size), appended to *out (at most `cap` bytes). False on any error. *used = the
// bytes the stream took (it ends on a byte boundary there).
inline bool Raw(const uint8_t* data, size_t size, std::vector<uint8_t>* out, size_t cap = size_t(1) << 30,
                size_t* used = nullptr) {
    using namespace detail;
    if (!data || !out) return false;
    const size_t start = out->size();
    Bits b{data, size};
    Huffman lit, dist;
    int last = 0;
    do {
        last = int(b.Get(1));
        const uint32_t type = b.Get(2);
        if (b.bad) break;
        if (type == 0) {  // stored
            b.buf = 0, b.cnt = 0;
            if (b.pos + 4 > size) {
                b.bad = true;
                break;
            }
            const uint32_t len = data[b.pos] | uint32_t(data[b.pos + 1]) << 8;
            const uint32_t nlen = data[b.pos + 2] | uint32_t(data[b.pos + 3]) << 8;
            b.pos += 4;
            if (len != (~nlen & 0xffff) || b.pos + len > size || out->size() - start + len > cap) {
                b.bad = true;
                break;
            }
            out->insert(out->end(), data + b.pos, data + b.pos + len);
            b.pos += len;
        } else if (type == 1) {  // fixed codes
            uint16_t l[288];
            int s = 0;
            for (; s < 144; ++s) l[s] = 8;
            for (; s < 256; ++s) l[s] = 9;
            for (; s < 280; ++s) l[s] = 7;
            for (; s < 288; ++s) l[s] = 8;
            Build(lit, l, 288);
            for (s = 0; s < 30; ++s) l[s] = 5;
            Build(dist, l, 30);
            if (!Codes(b, *out, start, lit, dist, cap)) b.bad = true;
        } else if (type == 2) {  // dynamic codes
            static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            const int nlen = int(b.Get(5)) + 257, ndist = int(b.Get(5)) + 1, ncode = int(b.Get(4)) + 4;
            if (b.bad || nlen > 286 || ndist > 30) {
                b.bad = true;
                break;
            }
            uint16_t l[320] = {};
            for (int k = 0; k < ncode; ++k) l[kOrder[k]] = uint16_t(b.Get(3));
            Huffman lens;
            if (b.bad || Build(lens, l, 19) != 0) {
                b.bad = true;
                break;
            }
            int k = 0;
            while (k < nlen + ndist && !b.bad) {
                int sym = Decode(b, lens);
                if (sym < 0) {
                    b.bad = true;
                    break;
                }
                if (sym < 16) {
                    l[k++] = uint16_t(sym);
                    continue;
                }
                uint16_t v = 0;
                int rep = 0;
                if (sym == 16) {
                    if (k == 0) {
                        b.bad = true;
                        break;
                    }
                    v = l[k - 1];
                    rep = 3 + int(b.Get(2));
                } else if (sym == 17) {
                    rep = 3 + int(b.Get(3));
                } else {
                    rep = 11 + int(b.Get(7));
                }
                if (k + rep > nlen + ndist) {
                    b.bad = true;
                    break;
                }
                while (rep--) l[k++] = v;
            }
            if (b.bad || l[256] == 0) {
                b.bad = true;
                break;
            }
            int e = Build(lit, l, nlen);
            if (e < 0 || (e > 0 && nlen - lit.count[0] != 1)) {
                b.bad = true;
                break;
            }
            e = Build(dist, l + nlen, ndist);
            if (e < 0 || (e > 0 && ndist - dist.count[0] != 1)) {
                b.bad = true;
                break;
            }
            if (!Codes(b, *out, start, lit, dist, cap)) b.bad = true;
        } else {
            b.bad = true;
        }
    } while (!last && !b.bad);
    if (b.bad) out->resize(start);
    if (used) *used = b.pos;
    return !b.bad;
}

inline uint32_t Adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

// A zlib stream (2-byte header, deflate, big-endian Adler-32), checked like zlib.decompress.
inline bool Zlib(const uint8_t* data, size_t size, std::vector<uint8_t>* out, size_t cap = size_t(1) << 30) {
    if (!data || !out || size < 6) return false;
    const uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || (uint32_t(cmf) << 8 | flg) % 31 != 0 || (flg & 0x20)) return false;
    const size_t start = out->size();
    size_t used = 0;
    if (!Raw(data + 2, size - 2, out, cap, &used) || 2 + used + 4 > size) {
        out->resize(start);
        return false;
    }
    const uint8_t* t = data + 2 + used;  // the big-endian Adler-32 right after the deflate data
    if ((uint32_t(t[0]) << 24 | uint32_t(t[1]) << 16 | uint32_t(t[2]) << 8 | t[3]) ==
        Adler32(out->data() + start, out->size() - start))
        return true;
    out->resize(start);
    return false;
}

// A package block as the game's tools read it (rig_extract.inflate): zlib, else raw deflate.
inline bool Block(const uint8_t* data, size_t size, std::vector<uint8_t>* out, size_t cap = size_t(1) << 30) {
    return Zlib(data, size, out, cap) || Raw(data, size, out, cap);
}

}  // namespace hrvr::inflate
