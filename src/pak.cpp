// Reads Larian's LSPK archives.
//
// bg3le needs this for one thing: which mod defines a given stat. That is
// not recorded anywhere in memory -- upstream learns it by hooking the
// engine as each Stats/Generated/*.txt is opened, and no engine function in
// the Linux build carries a symbol to hook. The files themselves do say,
// though, in their paths, so the archives are read directly.
//
// Single-part archives of version 15, 16 and 18 are handled -- 18 is what
// the game ships and what recent mod tools write, 15 and 16 are what older
// mods were packed with, and one of those is the user's own. Multi-part
// archives are textures and are skipped.
//
// Header, then a file list at the offset it names: uint32 count, uint32
// compressed size, then an LZ4 block holding count fixed-size entries. The
// entry shrank in 18, from three 64-bit sizes to a 48-bit offset and two
// 32-bit sizes, so both shapes are read into one struct.

#include "pak.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <mutex>
#include <string>
#include <vector>

#include "inflate.h"
#include "log.h"

extern "C" {
#include "lz4.h"
}
#include "zstd.h"

namespace bg3le {

namespace {

constexpr std::size_t kHeaderSize = 40;

// Guards against a corrupt header turning into a huge allocation.
constexpr std::uint32_t kMaxFiles = 4u << 20;
constexpr std::uint32_t kMaxListBytes = 1u << 28;
constexpr std::uint32_t kMaxFileBytes = 1u << 28;

#pragma pack(push, 1)
struct Entry18 {
    char Name[256];
    std::uint32_t OffsetLow;
    std::uint16_t OffsetHigh;
    std::uint8_t Part;
    std::uint8_t Flags;
    std::uint32_t SizeOnDisk;
    std::uint32_t UncompressedSize;
};

struct Entry15 {
    char Name[256];
    std::uint64_t Offset;
    std::uint64_t SizeOnDisk;
    std::uint64_t UncompressedSize;
    std::uint32_t Part;
    std::uint32_t Flags;
    std::uint32_t Crc;
    std::uint32_t Unused;
};
#pragma pack(pop)

static_assert(sizeof(Entry18) == 272, "LSPK v18 file entries are 272 bytes");
static_assert(sizeof(Entry15) == 296, "LSPK v15 file entries are 296 bytes");

// Both entry shapes, read into the form the rest of this file wants.
struct Entry {
    char Name[257];
    std::uint64_t Offset;
    std::uint64_t SizeOnDisk;
    std::uint64_t UncompressedSize;
    std::uint8_t Flags;
};

// The low nibble of Flags is the compression method; the rest is its level,
// which does not matter for decoding.
constexpr std::uint8_t kMethodMask = 0x0f;
constexpr std::uint8_t kMethodNone = 0;
constexpr std::uint8_t kMethodZlib = 1;
constexpr std::uint8_t kMethodLZ4 = 2;
constexpr std::uint8_t kMethodZstd = 3;

// A name that fills the field has no terminator of its own.
void copy_name(Entry* out, char const* name) {
    std::memcpy(out->Name, name, 256);
    out->Name[256] = '\0';
}

bool read_at(std::FILE* f, long offset, void* out, std::size_t size) {
    if (std::fseek(f, offset, SEEK_SET) != 0) return false;
    return std::fread(out, 1, size, f) == size;
}

// One file's bytes, decompressed if it was stored that way.
bool read_entry(std::FILE* f, Entry const& e, std::vector<char>* out) {
    if (e.SizeOnDisk == 0 || e.SizeOnDisk > kMaxFileBytes) return false;
    if (e.UncompressedSize > kMaxFileBytes) return false;

    std::vector<char> raw((std::size_t)e.SizeOnDisk);
    if (!read_at(f, (long)e.Offset, raw.data(), raw.size())) {
        return false;
    }

    switch (e.Flags & kMethodMask) {
    case kMethodNone:
        // A stored entry leaves UncompressedSize at zero.
        *out = std::move(raw);
        return true;

    case kMethodLZ4: {
        out->resize((std::size_t)e.UncompressedSize);
        const int got = LZ4_decompress_safe(raw.data(), out->data(),
                                            (int)raw.size(),
                                            (int)out->size());
        return got == (int)e.UncompressedSize;
    }

    case kMethodZlib:
        out->resize((std::size_t)e.UncompressedSize);
        return inflate(raw.data(), raw.size(), out->data(), out->size());

    case kMethodZstd: {
        // Newer LSLib builds pack with it (AutomaticMagicalSecretsExtender).
        out->resize((std::size_t)e.UncompressedSize);
        const std::size_t got = ZSTD_decompress(out->data(), out->size(),
                                                raw.data(), raw.size());
        return !ZSTD_isError(got) && got == (std::size_t)e.UncompressedSize;
    }

    default:
        return false;
    }
}

}  // namespace

namespace {

// One archive's file list, kept after the first read.
//
// Decoding it means decompressing an LZ4 block that is megabytes wide for
// a large archive, and mod loading asks the same archive for file after
// file: reading MCM's forty-odd Lua files re-decoded its list forty-odd
// times, which put seconds into the level load.
// Only small archives are kept. A mod pak holds a few hundred entries;
// the game's own hold hundreds of thousands, at 288 bytes each, and those
// are read once per process anyway.
constexpr std::size_t kCacheableEntries = 8192;

// Function-local statics rather than namespace globals: bg3le_init is a
// constructor too, and .init_array runs it before pak.cpp's own dynamic
// initialisation (preload.cpp is linked first) -- the stats mirror's
// constructor-time pak_list call used to find this map unconstructed and
// died in tree emplace. First use now builds it, whenever that is.
std::mutex& lists_lock() {
    static std::mutex lock;
    return lock;
}

std::map<std::string, std::vector<Entry>>& lists() {
    static std::map<std::string, std::vector<Entry>> lists;
    return lists;
}

bool read_list(char const* path, std::vector<Entry>* out) {
    {
        std::lock_guard<std::mutex> held(lists_lock());
        auto& cache = lists();
        auto cached = cache.find(path);
        if (cached != cache.end()) {
            *out = cached->second;
            return true;
        }
    }

    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;

    unsigned char header[kHeaderSize];
    if (std::fread(header, 1, sizeof(header), f) != sizeof(header)
        || std::memcmp(header, "LSPK", 4) != 0) {
        std::fclose(f);
        return false;
    }

    std::uint32_t version = 0;
    std::uint64_t listOffset = 0;
    std::memcpy(&version, header + 4, sizeof(version));
    std::memcpy(&listOffset, header + 8, sizeof(listOffset));

    std::size_t entrySize = 0;
    if (version == 18) {
        entrySize = sizeof(Entry18);
        // Only 18 records the part count in the header; 15 and 16 keep it
        // per entry, where a multi-part archive shows up as a part other
        // than zero and is skipped there.
        std::uint16_t parts = 0;
        std::memcpy(&parts, header + 38, sizeof(parts));
        if (parts != 1) {
            std::fclose(f);
            return false;
        }
    } else if (version == 15 || version == 16) {
        entrySize = sizeof(Entry15);
    } else {
        std::fclose(f);
        return false;
    }

    std::uint32_t count = 0;
    std::uint32_t compressed = 0;
    if (!read_at(f, (long)listOffset, &count, sizeof(count))
        || std::fread(&compressed, 1, sizeof(compressed), f)
               != sizeof(compressed)) {
        std::fclose(f);
        return false;
    }
    if (count == 0 || count > kMaxFiles || compressed == 0
        || compressed > kMaxListBytes) {
        std::fclose(f);
        return false;
    }

    std::vector<char> packed(compressed);
    if (std::fread(packed.data(), 1, packed.size(), f) != packed.size()) {
        std::fclose(f);
        return false;
    }

    std::vector<char> list(count * entrySize);
    const int want = (int)list.size();
    if (LZ4_decompress_safe(packed.data(), list.data(), (int)packed.size(),
                            want) != want) {
        std::fclose(f);
        return false;
    }
    packed.clear();
    packed.shrink_to_fit();

    std::vector<Entry> entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        char const* at = list.data() + (std::size_t)i * entrySize;
        Entry e{};
        if (version == 18) {
            Entry18 raw{};
            std::memcpy(&raw, at, sizeof(raw));
            if (raw.Part != 0) continue;
            copy_name(&e, raw.Name);
            e.Offset = (std::uint64_t)raw.OffsetLow
                       | ((std::uint64_t)raw.OffsetHigh << 32);
            e.SizeOnDisk = raw.SizeOnDisk;
            e.UncompressedSize = raw.UncompressedSize;
            e.Flags = raw.Flags;
        } else {
            Entry15 raw{};
            std::memcpy(&raw, at, sizeof(raw));
            if (raw.Part != 0) continue;
            copy_name(&e, raw.Name);
            e.Offset = raw.Offset;
            e.SizeOnDisk = raw.SizeOnDisk;
            e.UncompressedSize = raw.UncompressedSize;
            e.Flags = (std::uint8_t)raw.Flags;
        }
        entries.push_back(e);
    }

    std::fclose(f);
    if (entries.size() <= kCacheableEntries) {
        std::lock_guard<std::mutex> held(lists_lock());
        lists().emplace(path, entries);
    }
    *out = std::move(entries);
    return true;
}

}  // namespace

bool pak_list(char const* path,
              std::function<void(char const* name)> const& sink) {
    std::vector<Entry> entries;
    if (!read_list(path, &entries)) return false;
    for (Entry const& e : entries) sink(e.Name);
    return true;
}

bool pak_priority(char const* path, unsigned* priority) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;

    unsigned char header[kHeaderSize];
    const bool read = std::fread(header, 1, sizeof(header), f)
                          == sizeof(header)
                      && std::memcmp(header, "LSPK", 4) == 0;
    std::fclose(f);
    if (!read) return false;

    std::uint32_t version = 0;
    std::memcpy(&version, header + 4, sizeof(version));
    // 15 and 16 have no priority field; they are what mod tools wrote, and
    // the engine gives those the base priority.
    if (version != 18 && version != 15 && version != 16) return false;

    if (priority != nullptr) {
        *priority = version == 18 ? header[21] : 0u;
    }
    return true;
}

bool pak_read(char const* path,
              std::function<bool(char const* name)> const& accept,
              std::function<void(char const* name, char const* data,
                                 std::size_t size)> const& sink) {
    std::vector<Entry> entries;
    if (!read_list(path, &entries)) return false;

    // Nothing wanted: no need to open the archive at all.
    bool any = false;
    for (Entry const& e : entries) {
        if (accept(e.Name)) {
            any = true;
            break;
        }
    }
    if (!any) return true;

    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;

    std::vector<char> contents;
    for (Entry const& e : entries) {
        if (!accept(e.Name)) continue;
        if (!read_entry(f, e, &contents)) {
            logf("pak: %s: could not read %s", path, e.Name);
            continue;
        }
        sink(e.Name, contents.data(), contents.size());
    }

    std::fclose(f);
    return true;
}

bool pak_write(char const* path,
               std::vector<std::pair<std::string, std::string>> const& files) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;

    // Contents first, then the list, then the header: the header names the
    // list's offset, so it is written last with a seek back.
    unsigned char header[kHeaderSize] = {};
    if (std::fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        std::fclose(f);
        return false;
    }

    std::vector<Entry18> entries;
    entries.reserve(files.size());
    std::uint64_t at = kHeaderSize;
    for (auto const& file : files) {
        if (file.first.size() >= sizeof(Entry18::Name)) {
            std::fclose(f);
            return false;
        }
        if (std::fwrite(file.second.data(), 1, file.second.size(), f)
            != file.second.size()) {
            std::fclose(f);
            return false;
        }

        Entry18 e{};
        std::memcpy(e.Name, file.first.c_str(), file.first.size() + 1);
        e.OffsetLow = (std::uint32_t)(at & 0xffffffffu);
        e.OffsetHigh = (std::uint16_t)(at >> 32);
        e.Part = 0;
        e.Flags = kMethodNone;
        e.SizeOnDisk = (std::uint32_t)file.second.size();
        // A stored entry leaves the uncompressed size at zero, the way the
        // reader above expects.
        e.UncompressedSize = 0;
        entries.push_back(e);
        at += file.second.size();
    }

    const std::uint64_t listOffset = at;
    const std::uint32_t count = (std::uint32_t)entries.size();
    const int raw = (int)(entries.size() * sizeof(Entry18));
    std::vector<char> packed((std::size_t)LZ4_compressBound(raw));
    const int compressed = LZ4_compress_default(
        (char const*)entries.data(), packed.data(), raw, (int)packed.size());
    if (compressed <= 0) {
        std::fclose(f);
        return false;
    }

    const std::uint32_t compressedSize = (std::uint32_t)compressed;
    if (std::fwrite(&count, 1, sizeof(count), f) != sizeof(count)
        || std::fwrite(&compressedSize, 1, sizeof(compressedSize), f)
               != sizeof(compressedSize)
        || std::fwrite(packed.data(), 1, compressedSize, f)
               != compressedSize) {
        std::fclose(f);
        return false;
    }

    const std::uint32_t version = 18;
    const std::uint16_t parts = 1;
    std::memcpy(header, "LSPK", 4);
    std::memcpy(header + 4, &version, sizeof(version));
    std::memcpy(header + 8, &listOffset, sizeof(listOffset));
    // The field counts the whole list block, the two counts included --
    // writing just the compressed size made an archive the engine
    // refused, and refusing one archive stopped it loading any mod at all.
    const std::uint32_t listSize = compressedSize + 8;
    std::memcpy(header + 16, &listSize, sizeof(listSize));
    std::memcpy(header + 38, &parts, sizeof(parts));

    if (std::fseek(f, 0, SEEK_SET) != 0
        || std::fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    return true;
}

}  // namespace bg3le
