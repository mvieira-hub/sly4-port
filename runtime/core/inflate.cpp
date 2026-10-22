#include "core/inflate.h"

#include <cstring>

namespace sly4 {

namespace {

// LSB-first bit reader. Past the end it feeds zero bytes and counts them, so a stream
// that needs them is reported as truncated.
struct Bits {
    const uint8_t* p;
    size_t n, pos = 0, pad = 0;
    uint64_t buf = 0;
    int cnt = 0;

    void refill() {
        while (cnt <= 56) {
            if (pos < n)
                buf |= uint64_t(p[pos++]) << cnt;
            else
                ++pad;
            cnt += 8;
        }
    }
    uint32_t peek(int k) {
        if (cnt < k) refill();
        return uint32_t(buf & ((uint64_t(1) << k) - 1));
    }
    void drop(int k) {
        buf >>= k;
        cnt -= k;
    }
    uint32_t get(int k) {
        const uint32_t v = peek(k);
        drop(k);
        return v;
    }
    size_t consumed_bits() const { return (pos + pad) * 8 - size_t(cnt); }
    bool overrun() const { return consumed_bits() > n * 8; }
};

// Canonical Huffman code: a direct table for codes up to kFast bits, then the
// count/symbol walk of RFC 1951 for the longer ones.
struct Huffman {
    static constexpr int kFast = 10;
    uint16_t fast[1 << kFast];  // (length << 9) | symbol; 0 = longer code
    int16_t count[16];
    int16_t symbol[288];

    bool build(const uint8_t* lengths, int n) {
        std::memset(count, 0, sizeof(count));
        std::memset(fast, 0, sizeof(fast));
        for (int s = 0; s < n; ++s) ++count[lengths[s]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left = (left << 1) - count[len];
            if (left < 0) return false;  // over-subscribed
        }
        int offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + count[len];
        for (int s = 0; s < n; ++s)
            if (lengths[s]) symbol[offs[lengths[s]]++] = int16_t(s);
        // Codes in canonical order, bit-reversed for the LSB-first reader.
        int code = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            for (int k = 0; k < count[len]; ++k, ++code, ++index) {
                if (len > kFast) continue;
                uint32_t rev = 0;
                for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1u) << (len - 1 - b);
                for (uint32_t i = rev; i < (1u << kFast); i += 1u << len)
                    fast[i] = uint16_t(len << 9 | symbol[index]);
            }
            code <<= 1;
        }
        return true;
    }

    int decode(Bits& b) const {
        if (b.cnt < 16) b.refill();
        if (const uint16_t e = fast[b.peek(kFast)]) {
            b.drop(e >> 9);
            return e & 511;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= int(b.get(1));
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kCodeLenOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Fixed {
    Huffman lit, dist;
    Fixed() {
        uint8_t l[288];
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        lit.build(l, 288);
        uint8_t d[30];
        std::memset(d, 5, sizeof(d));
        dist.build(d, 30);
    }
};

InflateResult codes(Bits& b, const Huffman& lit, const Huffman& dist, uint8_t* out, size_t cap, size_t& n) {
    for (;;) {
        int sym = lit.decode(b);
        if (sym < 0) return b.overrun() ? InflateResult::InputTruncated : InflateResult::BadData;
        if (sym < 256) {
            if (n >= cap) return InflateResult::OutputTooSmall;
            out[n++] = uint8_t(sym);
            continue;
        }
        if (sym == 256) return b.overrun() ? InflateResult::InputTruncated : InflateResult::Ok;
        sym -= 257;
        if (sym >= 29) return InflateResult::BadData;
        const size_t len = kLenBase[sym] + b.get(kLenExtra[sym]);
        const int ds = dist.decode(b);
        if (ds < 0 || ds >= 30) return b.overrun() ? InflateResult::InputTruncated : InflateResult::BadData;
        const size_t d = kDistBase[ds] + b.get(kDistExtra[ds]);
        if (d > n) return InflateResult::BadData;
        if (len > cap - n) return InflateResult::OutputTooSmall;
        const uint8_t* from = out + n - d;
        for (size_t i = 0; i < len; ++i) out[n + i] = from[i];  // may overlap: byte by byte
        n += len;
        if (b.pad > 8) return InflateResult::InputTruncated;
    }
}

}  // namespace

InflateResult inflate_raw(const uint8_t* src, size_t src_len, uint8_t* out, size_t out_cap, size_t& out_len,
                          size_t& in_used) {
    static const Fixed fixed;
    Bits b{src, src_len};
    out_len = 0;
    in_used = 0;
    InflateResult r = InflateResult::Ok;
    bool last = false;
    while (!last && r == InflateResult::Ok) {
        last = b.get(1);
        const uint32_t type = b.get(2);
        if (type == 0) {  // stored
            b.drop(b.cnt % 8);
            const uint32_t len = b.get(16), nlen = b.get(16);
            if (len != (~nlen & 0xFFFF)) return InflateResult::BadData;
            if (len > out_cap - out_len) return InflateResult::OutputTooSmall;
            for (uint32_t i = 0; i < len; ++i) out[out_len++] = uint8_t(b.get(8));
            if (b.overrun()) return InflateResult::InputTruncated;
        } else if (type == 1) {
            r = codes(b, fixed.lit, fixed.dist, out, out_cap, out_len);
        } else if (type == 2) {
            const int nlit = int(b.get(5)) + 257, ndist = int(b.get(5)) + 1, ncode = int(b.get(4)) + 4;
            if (nlit > 286 || ndist > 30) return InflateResult::BadData;
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[kCodeLenOrder[i]] = uint8_t(b.get(3));
            Huffman lencode;
            if (!lencode.build(lengths, 19)) return InflateResult::BadData;
            int i = 0;
            while (i < nlit + ndist) {
                const int sym = lencode.decode(b);
                if (sym < 0) return InflateResult::BadData;
                if (sym < 16) {
                    lengths[i++] = uint8_t(sym);
                    continue;
                }
                uint8_t value = 0;
                int repeat;
                if (sym == 16) {
                    if (i == 0) return InflateResult::BadData;
                    value = lengths[i - 1];
                    repeat = 3 + int(b.get(2));
                } else if (sym == 17) {
                    repeat = 3 + int(b.get(3));
                } else {
                    repeat = 11 + int(b.get(7));
                }
                if (i + repeat > nlit + ndist) return InflateResult::BadData;
                while (repeat--) lengths[i++] = value;
                if (b.overrun()) return InflateResult::InputTruncated;
            }
            if (lengths[256] == 0) return InflateResult::BadData;  // no end-of-block code
            Huffman lit, dist;
            if (!lit.build(lengths, nlit) || !dist.build(lengths + nlit, ndist)) return InflateResult::BadData;
            r = codes(b, lit, dist, out, out_cap, out_len);
        } else {
            return InflateResult::BadData;
        }
    }
    in_used = (b.consumed_bits() + 7) / 8;
    return r;
}

const char* inflate_result_name(InflateResult r) {
    switch (r) {
        case InflateResult::Ok: return "ok";
        case InflateResult::InputTruncated: return "entrada truncada";
        case InflateResult::OutputTooSmall: return "saída maior que o espaço";
        case InflateResult::BadData: return "dados inválidos";
    }
    return "?";
}

}  // namespace sly4
