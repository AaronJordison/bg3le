// LSF layout follows Norbyte's LSLib (LSFReader.cs, LSFWriter.cs); thanks to
// him for documenting the format. Sections follow the metadata in the order
// strings, nodes, attributes, values, keys; all but strings are LZ4 frames
// from version 2 on when LZ4 is the method.

#include "lsf.h"

#include <cstring>

#include "inflate.h"

extern "C" {
#include "lz4.h"
}
#include "zstd.h"

namespace bg3le {

namespace {

constexpr std::uint32_t kMaxSection = 1u << 28;

template <typename T>
bool get(std::vector<char> const& buf, std::size_t at, T* out) {
    if (at + sizeof(T) > buf.size()) return false;
    std::memcpy(out, buf.data() + at, sizeof(T));
    return true;
}

std::uint32_t u32(char const* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// An LZ4 frame, as LSLib writes it for "chunked" sections.
bool lz4_frame(char const* in, std::size_t size, std::vector<char>* out) {
    if (size < 7 || u32(in) != 0x184D2204u) return false;
    const unsigned char flg = (unsigned char)in[4];
    const bool blockChecksum = flg & 0x10;
    const bool contentSize = flg & 0x08;
    const bool contentChecksum = flg & 0x04;
    const bool dictId = flg & 0x01;
    std::size_t at = 6 + (contentSize ? 8 : 0) + (dictId ? 4 : 0) + 1;

    std::size_t written = 0;
    while (true) {
        if (at + 4 > size) return false;
        const std::uint32_t word = u32(in + at);
        at += 4;
        if (word == 0) break;
        const std::uint32_t length = word & 0x7fffffffu;
        if (at + length > size) return false;
        if (word & 0x80000000u) {
            if (written + length > out->size()) return false;
            std::memcpy(out->data() + written, in + at, length);
            written += length;
        } else {
            // Linked blocks reach back into what is already decoded.
            const std::size_t dict = written < 65536 ? written : 65536;
            const int got = LZ4_decompress_safe_usingDict(
                in + at, out->data() + written, (int)length,
                (int)(out->size() - written), out->data() + written - dict,
                (int)dict);
            if (got < 0) return false;
            written += (std::size_t)got;
        }
        at += length + (blockChecksum ? 4 : 0);
    }
    (void)contentChecksum;
    return written == out->size();
}

struct Reader {
    char const* data;
    std::size_t size;
    std::size_t at;
    unsigned method;
    std::uint32_t version;

    bool section(std::uint32_t uncompressed, std::uint32_t onDisk,
                 bool chunkable, std::vector<char>* out) {
        if (uncompressed > kMaxSection || onDisk > kMaxSection) return false;
        out->assign(uncompressed, 0);
        if (uncompressed == 0) return onDisk == 0;
        if (onDisk == 0 || method == 0) {
            if (at + uncompressed > size) return false;
            std::memcpy(out->data(), data + at, uncompressed);
            at += uncompressed;
            return true;
        }
        if (at + onDisk > size) return false;
        char const* in = data + at;
        at += onDisk;
        switch (method) {
        case 1:
            return inflate(in, onDisk, out->data(), out->size());
        case 2:
            if (chunkable && version >= 2) {
                return lz4_frame(in, onDisk, out);
            }
            return LZ4_decompress_safe(in, out->data(), (int)onDisk,
                                       (int)uncompressed)
                   == (int)uncompressed;
        case 3: {
            const std::size_t got =
                ZSTD_decompress(out->data(), out->size(), in, onDisk);
            return !ZSTD_isError(got) && got == uncompressed;
        }
        default:
            return false;
        }
    }
};

bool is_string_type(std::uint32_t type) {
    // String, Path, FixedString, LSString, WString, LSWString
    return type == 20 || type == 21 || type == 22 || type == 23 || type == 29
           || type == 30;
}

bool read_names(std::vector<char> const& buf,
                std::vector<std::vector<std::string>>* out) {
    std::uint32_t chains = 0;
    if (!get(buf, 0, &chains)) return false;
    std::size_t at = 4;
    out->resize(chains);
    for (std::uint32_t i = 0; i < chains; ++i) {
        std::uint16_t count = 0;
        if (!get(buf, at, &count)) return false;
        at += 2;
        for (std::uint16_t j = 0; j < count; ++j) {
            std::uint16_t length = 0;
            if (!get(buf, at, &length)) return false;
            at += 2;
            if (at + length > buf.size()) return false;
            (*out)[i].emplace_back(buf.data() + at, length);
            at += length;
        }
    }
    return true;
}

}  // namespace

bool lsf_read(char const* data, std::size_t size, LsfDocument* out) {
    if (size < 12 || std::memcmp(data, "LSOF", 4) != 0) return false;
    LsfDocument doc;
    doc.Version = u32(data + 4);
    if (doc.Version < 1 || doc.Version > 7) return false;

    doc.MetadataAt = 8 + (doc.Version >= 5 ? 8 : 4);
    const bool v6 = doc.Version >= 6;
    const std::size_t metaSize = v6 ? 48 : 40;
    if (size < doc.MetadataAt + metaSize) return false;
    char const* m = data + doc.MetadataAt;

    std::uint32_t su, sd, ku = 0, kd = 0, nu, nd, au, ad, vu, vd, format;
    unsigned char flags;
    if (v6) {
        su = u32(m), sd = u32(m + 4), ku = u32(m + 8), kd = u32(m + 12);
        nu = u32(m + 16), nd = u32(m + 20), au = u32(m + 24);
        ad = u32(m + 28), vu = u32(m + 32), vd = u32(m + 36);
        flags = (unsigned char)m[40];
        format = u32(m + 44);
    } else {
        su = u32(m), sd = u32(m + 4), nu = u32(m + 8), nd = u32(m + 12);
        au = u32(m + 16), ad = u32(m + 20), vu = u32(m + 24);
        vd = u32(m + 28);
        flags = (unsigned char)m[32];
        format = u32(m + 36);
    }
    doc.Prefix.assign(data, data + doc.MetadataAt + metaSize);

    Reader r{data, size, doc.MetadataAt + metaSize, flags & 0x0fu,
             doc.Version};
    if (!r.section(su, sd, false, &doc.Strings)
        || !r.section(nu, nd, true, &doc.Nodes)
        || !r.section(au, ad, true, &doc.Attributes)
        || !r.section(vu, vd, true, &doc.Values)) {
        return false;
    }
    doc.HasKeys = v6 && format == 1;
    if (doc.HasKeys && !r.section(ku, kd, true, &doc.Keys)) return false;

    std::vector<std::vector<std::string>> names;
    if (!read_names(doc.Strings, &names)) return false;

    const bool v3 = doc.Version >= 3 && format == 1;
    const std::size_t step = v3 ? 16 : 12;
    std::uint32_t offset = 0;
    for (std::size_t at = 0; at + step <= doc.Attributes.size(); at += step) {
        char const* a = doc.Attributes.data() + at;
        const std::uint32_t name = u32(a);
        const std::uint32_t typeAndLength = u32(a + 4);
        if (v3) offset = u32(a + 12);
        const std::uint32_t type = typeAndLength & 0x3f;
        const std::uint32_t length = typeAndLength >> 6;
        if (is_string_type(type) && length > 1
            && (std::size_t)offset + length <= doc.Values.size()) {
            LsfString s;
            s.Offset = offset;
            s.Length = length - 1;
            const std::size_t chain = name >> 16;
            const std::size_t link = name & 0xffff;
            if (chain < names.size() && link < names[chain].size()) {
                s.Attribute = names[chain][link];
            }
            doc.StringValues.push_back(std::move(s));
        }
        if (!v3) offset += length;
    }

    *out = std::move(doc);
    return true;
}

std::string lsf_write(LsfDocument const& doc) {
    std::string out(doc.Prefix.begin(), doc.Prefix.end());
    char* m = out.data() + doc.MetadataAt;
    auto put = [&](std::size_t at, std::size_t value) {
        const std::uint32_t v = (std::uint32_t)value;
        std::memcpy(m + at, &v, sizeof(v));
    };
    // Sizes on disk of zero with no method is how LSLib writes it.
    if (doc.Version >= 6) {
        put(0, doc.Strings.size()), put(4, 0);
        put(8, doc.Keys.size()), put(12, 0);
        put(16, doc.Nodes.size()), put(20, 0);
        put(24, doc.Attributes.size()), put(28, 0);
        put(32, doc.Values.size()), put(36, 0);
        m[40] = 0;
    } else {
        put(0, doc.Strings.size()), put(4, 0);
        put(8, doc.Nodes.size()), put(12, 0);
        put(16, doc.Attributes.size()), put(20, 0);
        put(24, doc.Values.size()), put(28, 0);
        m[32] = 0;
    }
    out.append(doc.Strings.begin(), doc.Strings.end());
    out.append(doc.Nodes.begin(), doc.Nodes.end());
    out.append(doc.Attributes.begin(), doc.Attributes.end());
    out.append(doc.Values.begin(), doc.Values.end());
    if (doc.HasKeys) out.append(doc.Keys.begin(), doc.Keys.end());
    return out;
}

}  // namespace bg3le
