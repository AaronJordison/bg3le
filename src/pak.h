#pragma once

// Reads Larian's LSPK archives, enough to pull text files out of them.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bg3le {

// Calls `sink` with the decompressed contents of every file in `path` whose
// name `accept` returns true for. Names use forward slashes and are relative
// to the archive root, e.g. "Public/Shared/Stats/Generated/Data/Weapon.txt".
//
// Returns false if the archive could not be read at all; a file that fails
// to decompress is skipped, not fatal.
// Every file name in `path`, without reading any of their contents. The
// list is decoded once per archive and kept, so this is cheap to repeat.
bool pak_list(char const* path,
              std::function<void(char const* name)> const& sink);

// The archive's load priority, as the engine reads it: where two archives
// hold the same path, the higher priority wins. The game ships most of its
// own at zero and its patch archives above them.
//
// Returns false if `path` is not an archive this reader understands.
bool pak_priority(char const* path, unsigned* priority);

bool pak_read(char const* path,
              std::function<bool(char const* name)> const& accept,
              std::function<void(char const* name, char const* data,
                                 std::size_t size)> const& sink);

// Writes an LSPK v18 archive with the given files, stored uncompressed.
// For rebuilding an archive whose contents have been edited; the engine
// reads stored entries as readily as compressed ones.
bool pak_write(char const* path,
               std::vector<std::pair<std::string, std::string>> const& files);

// One archive's file list in full, for rewriting it (src/pak_fix.cpp).
struct PakFile {
    std::string Name;
    std::uint64_t Offset = 0;
    std::uint64_t SizeOnDisk = 0;
    std::uint64_t UncompressedSize = 0;
    std::uint8_t Flags = 0;
    std::uint32_t Part = 0;
    std::uint32_t Crc = 0;  // 15 and 16 only
};

struct PakListing {
    std::uint32_t Version = 0;
    std::uint8_t Flags = 0;  // the header's; 0x04 is a solid archive
    std::uint32_t Parts = 1;
    std::uint64_t ListOffset = 0;
    std::uint32_t ListSize = 0;  // as the header records it
    std::uint32_t ListCompressed = 0;
    std::vector<PakFile> Files;
};

// Every entry, multi-part ones included, uncached.
bool pak_listing(char const* path, PakListing* out);

// One file's contents, decompressed. False for an empty file.
bool pak_file_read(std::FILE* f, PakFile const& file, std::vector<char>* out);

// Whether a file has no contents at all.
bool pak_file_empty(PakFile const& file);

// Writes `listing` as the file list at offset `at` of the archive open in
// `f`, and points the header at it. The data the entries name stays put.
bool pak_write_listing(std::FILE* f, std::uint64_t at,
                       PakListing const& listing);

}  // namespace bg3le
