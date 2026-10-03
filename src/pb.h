// The sliver of protobuf the LuaDebug and extender protocols need.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace bg3le::pb {

inline void varint(std::string* out, std::uint64_t v) {
    while (v >= 0x80) {
        out->push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out->push_back(static_cast<char>(v));
}

inline void key(std::string* out, unsigned field, unsigned wire) {
    varint(out, (static_cast<std::uint64_t>(field) << 3) | wire);
}

// proto3 omits zero-valued scalars.
inline void uint_field(std::string* out, unsigned field, std::uint64_t v) {
    if (v == 0) return;
    key(out, field, 0);
    varint(out, v);
}

inline void bytes_field(std::string* out, unsigned field, const std::string& v) {
    key(out, field, 2);
    varint(out, v.size());
    out->append(v);
}

// Decoding: walks fields, reporting each to the caller.
class Reader {
public:
    Reader(const char* data, std::size_t size) : p_(data), end_(data + size) {}

    bool next(unsigned* field, unsigned* wire) {
        if (p_ >= end_) return false;
        std::uint64_t k = 0;
        if (!read_varint(&k)) return false;
        *field = static_cast<unsigned>(k >> 3);
        *wire = static_cast<unsigned>(k & 7);
        return true;
    }

    bool read_varint(std::uint64_t* out) {
        std::uint64_t v = 0;
        unsigned shift = 0;
        while (p_ < end_) {
            const unsigned char b = static_cast<unsigned char>(*p_++);
            v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) {
                *out = v;
                return true;
            }
            shift += 7;
            if (shift > 63) return false;
        }
        return false;
    }

    bool read_bytes(std::string* out) {
        std::uint64_t len = 0;
        if (!read_varint(&len)) return false;
        if (static_cast<std::uint64_t>(end_ - p_) < len) return false;
        out->assign(p_, static_cast<std::size_t>(len));
        p_ += len;
        return true;
    }

    // A fixed32 or fixed64 field's raw bytes.
    bool read_fixed(std::string* out, std::size_t n) {
        if (static_cast<std::size_t>(end_ - p_) < n) return false;
        out->assign(p_, n);
        p_ += n;
        return true;
    }

    // Skips a field whose contents we do not care about.
    bool skip(unsigned wire) {
        std::uint64_t v = 0;
        switch (wire) {
            case 0: return read_varint(&v);
            case 1: return advance(8);
            case 2: {
                std::string ignored;
                return read_bytes(&ignored);
            }
            case 5: return advance(4);
            default: return false;
        }
    }

private:
    bool advance(std::size_t n) {
        if (static_cast<std::size_t>(end_ - p_) < n) return false;
        p_ += n;
        return true;
    }

    const char* p_;
    const char* end_;
};

}  // namespace bg3le::pb
