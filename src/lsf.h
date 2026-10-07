#pragma once

// Reads Larian's binary LSF resources far enough to find and edit their
// string values, and writes them back out uncompressed.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bg3le {

// A string value: its bytes in LsfDocument::Values, without the terminator.
struct LsfString {
    std::size_t Offset = 0;
    std::size_t Length = 0;
    std::string Attribute;
};

struct LsfDocument {
    std::vector<char> Prefix;  // magic, header and metadata, as read
    std::size_t MetadataAt = 0;
    std::uint32_t Version = 0;
    bool HasKeys = false;
    std::vector<char> Strings, Nodes, Attributes, Values, Keys;
    std::vector<LsfString> StringValues;
};

// False if `data` is not an LSF file this understands.
bool lsf_read(char const* data, std::size_t size, LsfDocument* out);

// The document as an uncompressed LSF file, the form most mods ship.
std::string lsf_write(LsfDocument const& doc);

}  // namespace bg3le
