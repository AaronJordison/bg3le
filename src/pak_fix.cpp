// Corrected copies of mod archives (see pak_fix.h).
//
// Empty files become a single newline: an empty Stats/Generated/Data/*.txt
// hangs LoadModule at ~95%, and a newline loads (tested with Expansion).
//
// Case: wherever a mod's own files refer to a file the archive holds, the
// spellings are made to agree, lowercase where they differ, by renaming the
// entry and rewriting the references. Spellings the engine derives itself
// win: GUI textures are always ".DDS" (every one in the base game is), and
// files the engine finds by a fixed name keep their spelling. Directories
// keep the archive's spelling, since the engine enumerates some of them.
//
// The copy is the original plus the rewritten files and a new file list
// appended, cloned where the filesystem allows it.

#include "pak_fix.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "lsf.h"
#include "pak.h"

#ifndef FICLONE
#define FICLONE 0x40049409
#endif

namespace bg3le {

namespace {

// Part of every cached copy's name: bump it when the fixes change.
constexpr char kFormat[] = "1";
constexpr std::size_t kMaxScanBytes = 32u << 20;
constexpr std::uint64_t kAlign = 64;

char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower(c == '\\' ? '/' : c);
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool iends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size()
           && iequals(s.substr(s.size() - suffix.size()), suffix);
}

std::string_view last_component(std::string_view name) {
    const std::size_t slash = name.rfind('/');
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

std::string extension(std::string_view name) {
    std::string_view const leaf = last_component(name);
    const std::size_t dot = leaf.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    return lowered(leaf.substr(dot + 1));
}

bool is_text(std::string const& ext) {
    static const std::set<std::string> kText = {
        "lsx", "lsj", "xml", "xaml", "txt", "khn", "lua",
        "json", "ini", "cfg", "yaml", "yml"};
    return kText.count(ext) != 0;
}

bool is_lsf(std::string const& ext) {
    return ext == "lsf" || ext == "lsfx" || ext == "lsbc" || ext == "lsbs";
}

// Files the engine or the extender finds by name rather than by reference.
bool fixed_name(std::string_view leaf) {
    static const std::set<std::string> kFixed = {
        "meta.lsx", "metadata.lsx", "metadata.lsf", "config.json",
        "bootstrapserver.lua", "bootstrapclient.lua", "story_header.div"};
    return kFixed.count(lowered(leaf)) != 0;
}

bool gui_metadata(std::string_view name) {
    return iends_with(name, "/GUI/metadata.lsx")
           || iends_with(name, "/GUI/metadata.lsf");
}

// Directories whose textures the engine names after a stats Icon.
bool icon_directory(std::string const& lowerName) {
    static const char* const kDirs[] = {
        "/tooltips/icons/", "/tooltips/itemicons/",
        "/controlleruiicons/skills_png/", "/controlleruiicons/items_png/"};
    if (lowerName.find("/gui/assets") == std::string::npos) return false;
    const std::size_t slash = lowerName.rfind('/');
    for (char const* dir : kDirs) {
        const std::size_t len = std::strlen(dir);
        if (slash + 1 >= len
            && lowerName.compare(slash + 1 - len, len, dir) == 0) {
            return true;
        }
    }
    return false;
}

std::uint32_t crc32(char const* data, std::size_t size) {
    static std::uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        built = true;
    }
    std::uint32_t c = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        c = table[(c ^ (unsigned char)data[i]) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xffffffffu;
}

struct Content {
    std::size_t File = 0;
    bool Lsf = false;
    std::vector<char> Text;
    LsfDocument Doc;
    bool Changed = false;

    char* bytes() { return Lsf ? Doc.Values.data() : Text.data(); }
};

// One spelling of a name: a range of an entry's name or of a file's text.
struct Occurrence {
    int Content = -1;  // -1: the entry name `Index`
    std::size_t Index = 0;
    std::size_t Pos = 0;
    std::size_t Len = 0;
};

struct Group {
    std::vector<Occurrence> Occurrences;
    bool Directory = false;
    std::string Pin;
    bool PinConflict = false;
};

// A component of an entry's name and the group that spells it.
struct Part {
    std::size_t Begin, End;
    int Group;
};

class Fixer {
public:
    Fixer(PakListing const& listing, std::vector<Content>& contents)
        : listing_(listing), contents_(contents) {
        for (PakFile const& f : listing_.Files) names_.push_back(f.Name);
        parts_.resize(names_.size());
        stems_.resize(names_.size());
        for (std::size_t i = 0; i < names_.size(); ++i) add_entry(i);
    }

    void scan() {
        for (std::size_t c = 0; c < contents_.size(); ++c) {
            Content& content = contents_[c];
            if (content.Lsf) {
                for (LsfString const& s : content.Doc.StringValues) {
                    token(c, s.Offset, s.Length, s.Attribute);
                }
            } else {
                scan_text(c);
            }
        }
    }

    // Applies every group's spelling. False, with nothing changed, if two
    // entries would end up with one name.
    bool unify(std::vector<std::string>* fixes, std::vector<std::string>* names) {
        std::vector<std::string> newNames = names_;
        std::map<std::string, int> edited;

        for (std::size_t g = 0; g < groups_.size(); ++g) {
            if (find(g) != (int)g) continue;
            Group const& group = groups_[g];
            if (group.Occurrences.size() < 2 && group.Pin.empty()) continue;

            std::vector<std::string> spellings;
            for (Occurrence const& o : group.Occurrences) {
                spellings.push_back(spelling(o));
            }
            std::string target;
            if (!choose(group, spellings, &target)) continue;

            for (std::size_t k = 0; k < group.Occurrences.size(); ++k) {
                Occurrence const& o = group.Occurrences[k];
                if (spellings[k] == target) continue;
                if (o.Content < 0) {
                    newNames[o.Index].replace(o.Pos, o.Len, target);
                } else {
                    Content& content = contents_[(std::size_t)o.Content];
                    std::memcpy(content.bytes() + o.Pos, target.data(), o.Len);
                    content.Changed = true;
                    const std::string what =
                        listing_.Files[content.File].Name + ": \""
                        + spellings[k] + "\" -> \"" + target + "\"";
                    ++edited[what];
                }
            }
        }

        std::set<std::string> seen;
        for (std::string const& n : newNames) {
            if (!seen.insert(n).second) {
                fixes->push_back("left case alone: two files would be named "
                                 + n);
                for (Content& c : contents_) c.Changed = false;
                return false;
            }
        }
        for (std::size_t i = 0; i < newNames.size(); ++i) {
            if (newNames[i] != names_[i]) {
                fixes->push_back("renamed " + names_[i] + " -> " + newNames[i]);
            }
        }
        for (auto const& [what, count] : edited) {
            fixes->push_back(count > 1 ? what + " (" + std::to_string(count)
                                             + " times)"
                                       : what);
        }
        *names = std::move(newNames);
        return true;
    }

private:
    PakListing const& listing_;
    std::vector<Content>& contents_;
    std::vector<std::string> names_;
    std::vector<std::vector<Part>> parts_;
    std::vector<int> stems_;
    std::vector<Group> groups_;
    std::vector<int> parent_;
    std::unordered_map<std::string, int> ids_;
    std::unordered_map<std::string, std::vector<std::size_t>> full_;
    std::unordered_map<std::string, std::vector<std::size_t>> suffixes_;
    std::unordered_map<std::string, std::vector<std::size_t>> icons_;

    int group(std::string const& id, bool directory) {
        auto [it, added] = ids_.emplace(id, (int)groups_.size());
        if (added) {
            groups_.emplace_back();
            groups_.back().Directory = directory;
            parent_.push_back(it->second);
        }
        return it->second;
    }

    int find(std::size_t g) {
        while (parent_[g] != (int)g) {
            parent_[g] = parent_[(std::size_t)parent_[g]];
            g = (std::size_t)parent_[g];
        }
        return (int)g;
    }

    void pin(int g, std::string const& spelling) {
        Group& group = groups_[(std::size_t)find((std::size_t)g)];
        if (group.Pin.empty()) group.Pin = spelling;
        else if (group.Pin != spelling) group.PinConflict = true;
    }

    void unite(int a, int b) {
        const int ra = find((std::size_t)a);
        const int rb = find((std::size_t)b);
        if (ra == rb) return;
        Group& to = groups_[(std::size_t)ra];
        Group& from = groups_[(std::size_t)rb];
        to.Occurrences.insert(to.Occurrences.end(), from.Occurrences.begin(),
                              from.Occurrences.end());
        to.Directory = to.Directory || from.Directory;
        to.PinConflict = to.PinConflict || from.PinConflict;
        if (to.Pin.empty()) to.Pin = from.Pin;
        else if (!from.Pin.empty() && from.Pin != to.Pin) to.PinConflict = true;
        from = Group{};
        parent_[(std::size_t)rb] = ra;
    }

    void add(int g, Occurrence o) {
        groups_[(std::size_t)find((std::size_t)g)].Occurrences.push_back(o);
    }

    void add_entry(std::size_t i) {
        std::string const& name = names_[i];
        std::string const low = lowered(name);
        full_[low].push_back(i);

        std::size_t begin = 0;
        while (true) {
            const std::size_t slash = name.find('/', begin);
            if (slash == std::string::npos) break;
            const int g = group("d:" + low.substr(0, slash), true);
            parts_[i].push_back({begin, slash, g});
            add(g, {-1, i, begin, slash - begin});
            suffixes_[low.substr(slash + 1)].push_back(i);
            begin = slash + 1;
        }

        const std::size_t dot = name.rfind('.');
        const bool hasExt = dot != std::string::npos && dot > begin;
        const std::size_t stemEnd = hasExt ? dot : name.size();
        const int stem = group("s:" + low.substr(0, stemEnd), false);
        stems_[i] = stem;
        parts_[i].push_back({begin, stemEnd, stem});
        add(stem, {-1, i, begin, stemEnd - begin});
        int ext = -1;
        if (hasExt) {
            ext = group("x:" + low, false);
            parts_[i].push_back({dot + 1, name.size(), ext});
            add(ext, {-1, i, dot + 1, name.size() - dot - 1});
        }

        std::string_view const leaf = last_component(name);
        if (fixed_name(leaf)) {
            pin(stem, name.substr(begin, stemEnd - begin));
            if (ext >= 0) pin(ext, name.substr(dot + 1));
        }
        if (hasExt && low.find("/gui/") != std::string::npos
            && extension(name) == "dds") {
            pin(ext, "DDS");
            if (icon_directory(low)) {
                icons_[low.substr(begin, stemEnd - begin)].push_back(i);
                pin_icon_directories(i);
            }
        }
    }

    // The engine builds these paths from an Icon name, so their directories
    // must be spelled as in the base game.
    void pin_icon_directories(std::size_t i) {
        static const std::map<std::string, std::string> kCanonical = {
            {"gui", "GUI"},
            {"assets", "Assets"},
            {"assetslowres", "AssetsLowRes"},
            {"tooltips", "Tooltips"},
            {"icons", "Icons"},
            {"itemicons", "ItemIcons"},
            {"controlleruiicons", "ControllerUIIcons"},
            {"skills_png", "skills_png"},
            {"items_png", "items_png"}};
        std::vector<Part> const& parts = parts_[i];
        std::size_t from = parts.size();
        for (std::size_t k = 0; k + 1 < parts.size(); ++k) {
            if (iequals(std::string_view(names_[i]).substr(
                            parts[k].Begin, parts[k].End - parts[k].Begin),
                        "GUI")) {
                from = k;
            }
        }
        for (std::size_t k = from; k < parts.size(); ++k) {
            if (groups_[(std::size_t)find((std::size_t)parts[k].Group)]
                    .Directory == false) {
                break;
            }
            auto it = kCanonical.find(lowered(std::string_view(names_[i]).substr(
                parts[k].Begin, parts[k].End - parts[k].Begin)));
            if (it != kCanonical.end()) pin(parts[k].Group, it->second);
        }
    }

    std::string spelling(Occurrence const& o) {
        if (o.Content < 0) return names_[o.Index].substr(o.Pos, o.Len);
        char const* at = contents_[(std::size_t)o.Content].bytes() + o.Pos;
        return std::string(at, o.Len);
    }

    bool choose(Group const& group, std::vector<std::string> const& spellings,
                std::string* target) {
        if (group.PinConflict) return false;
        if (!group.Pin.empty()) {
            *target = group.Pin;
            return target->size() == spellings.front().size();
        }
        bool same = true;
        for (std::string const& s : spellings) same = same && s == spellings[0];
        if (same) return false;

        if (group.Directory) {
            // The archive's own spelling, if it has only one.
            std::string archive;
            for (std::size_t k = 0; k < spellings.size(); ++k) {
                if (group.Occurrences[k].Content >= 0) continue;
                if (archive.empty()) archive = spellings[k];
                else if (archive != spellings[k]) return false;
            }
            if (archive.empty()) return false;
            *target = archive;
            return true;
        }

        *target = spellings[0];
        for (std::size_t c = 0; c < target->size(); ++c) {
            for (std::string const& s : spellings) {
                if (s[c] != (*target)[c]) {
                    (*target)[c] = lower((*target)[c]);
                    break;
                }
            }
        }
        return true;
    }

    // Token bytes [pos, pos + n) name entry `i`'s [begin, end).
    void link(std::size_t c, std::size_t pos, std::size_t i, std::size_t begin,
              std::size_t end) {
        for (Part const& p : parts_[i]) {
            if (p.Begin < begin || p.End > end) continue;
            add(p.Group, {(int)c, 0, pos + (p.Begin - begin), p.End - p.Begin});
        }
    }

    std::size_t unique(
        std::unordered_map<std::string, std::vector<std::size_t>> const& index,
        std::string const& key) {
        auto it = index.find(key);
        if (it == index.end() || it->second.size() != 1) return SIZE_MAX;
        return it->second.front();
    }

    void token(std::size_t c, std::size_t pos, std::size_t len,
               std::string_view attribute) {
        if (len < 3 || len > 255) return;
        char const* raw = contents_[c].bytes() + pos;
        for (std::size_t k = 0; k < len; ++k) {
            if ((unsigned char)raw[k] < 0x20) return;
        }
        std::string const key = lowered(std::string_view(raw, len));

        std::size_t i = unique(full_, key);
        if (i != SIZE_MAX) {
            link(c, pos, i, 0, names_[i].size());
            return;
        }
        const bool slash = key.find('/') != std::string::npos;
        const bool dot = key.find('.') != std::string::npos;
        if (slash || dot) {
            i = unique(suffixes_, key);
            if (i != SIZE_MAX) {
                link(c, pos, i, names_[i].size() - len, names_[i].size());
                return;
            }
        }

        // GUI metadata keys are ".png" paths under the metadata's directory;
        // the engine loads the ".DDS" beside them.
        std::string const& file = listing_.Files[contents_[c].File].Name;
        if (iends_with(key, ".png") && gui_metadata(file)) {
            std::string const dir = lowered(file.substr(0, file.rfind('/')));
            i = unique(full_, dir + "/" + key.substr(0, len - 4) + ".dds");
            if (i != SIZE_MAX) {
                link(c, pos, i, dir.size() + 1, names_[i].size() - 4);
            }
            return;
        }

        if (!slash && !dot
            && (iequals(attribute, "Icon") || iequals(attribute, "MapKey"))) {
            auto it = icons_.find(key);
            if (it == icons_.end()) return;
            // One name for every texture derived from it, so one group.
            for (std::size_t e : it->second) unite(stems_[it->second[0]], stems_[e]);
            add(stems_[it->second[0]], {(int)c, 0, pos, len});
        }
    }

    // Quoted strings, with the attribute or stats field they belong to.
    void scan_text(std::size_t c) {
        std::vector<char> const& text = contents_[c].Text;
        std::size_t lineStart = 0;
        std::size_t at = 0;
        while (at < text.size()) {
            const char q = text[at];
            if (q == '\n') {
                lineStart = ++at;
                continue;
            }
            if (q != '"' && q != '\'') {
                ++at;
                continue;
            }
            std::size_t end = at + 1;
            while (end < text.size() && text[end] != q && text[end] != '\n'
                   && end - at <= 256) {
                ++end;
            }
            if (end >= text.size() || text[end] != q) {
                ++at;
                continue;
            }
            const std::string_view line(text.data() + lineStart,
                                        at - lineStart);
            token(c, at + 1, end - at - 1, context(line));
            at = end + 1;
        }
    }

    // `data "Icon" "..."` in stats, `id="Icon" ... value="..."` in lsx.
    static std::string_view context(std::string_view line) {
        for (std::string_view const lead : {"data \"", "id=\""}) {
            const std::size_t p = line.find(lead);
            if (p == std::string_view::npos) continue;
            const std::size_t from = p + lead.size();
            const std::size_t to = line.find('"', from);
            if (to != std::string_view::npos) return line.substr(from, to - from);
        }
        return {};
    }
};

bool copy_file(char const* from, char const* to) {
    const int in = ::open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    const int out = ::open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        ::close(in);
        return false;
    }
    bool ok = ::ioctl(out, FICLONE, in) == 0;
    if (!ok) {
        ok = true;
        while (true) {
            const ssize_t n = ::copy_file_range(in, nullptr, out, nullptr,
                                                1u << 30, 0);
            if (n == 0) break;
            if (n > 0) continue;
            // Across filesystems on older kernels: copy by hand.
            if (::lseek(out, 0, SEEK_CUR) != 0) {
                ok = false;
                break;
            }
            std::vector<char> buf(1u << 20);
            ssize_t r;
            while ((r = ::read(in, buf.data(), buf.size())) > 0) {
                if (::write(out, buf.data(), (std::size_t)r) != r) {
                    ok = false;
                    break;
                }
            }
            ok = ok && r == 0;
            break;
        }
    }
    ::close(in);
    return ::close(out) == 0 && ok;
}

}  // namespace

int pak_fix_build(char const* in, char const* out,
                  std::vector<std::string>* fixes) {
    // An archive this cannot read is one it cannot fix either.
    PakListing listing;
    if (!pak_listing(in, &listing)) return 0;
    if (listing.Parts != 1 || (listing.Flags & 0x04) != 0) return 0;
    for (PakFile const& f : listing.Files) {
        if (f.Part != 0) return 0;
    }

    std::FILE* f = std::fopen(in, "rb");
    if (f == nullptr) return -1;
    std::vector<Content> contents;
    for (std::size_t i = 0; i < listing.Files.size(); ++i) {
        PakFile const& file = listing.Files[i];
        std::string const ext = extension(file.Name);
        const bool lsf = is_lsf(ext);
        if (!lsf && !is_text(ext)) continue;
        if (pak_file_empty(file)) continue;
        const std::uint64_t size = file.UncompressedSize != 0
                                       ? file.UncompressedSize
                                       : file.SizeOnDisk;
        if (size > kMaxScanBytes) continue;

        Content c;
        c.File = i;
        c.Lsf = lsf;
        if (!pak_file_read(f, file, &c.Text)) continue;
        if (lsf) {
            if (!lsf_read(c.Text.data(), c.Text.size(), &c.Doc)) continue;
            c.Text.clear();
        }
        contents.push_back(std::move(c));
    }
    std::fclose(f);

    std::vector<std::string> names;
    Fixer fixer(listing, contents);
    fixer.scan();
    const std::size_t before = fixes->size();
    bool renamed = false;
    if (fixer.unify(fixes, &names)) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            renamed = renamed || names[i] != listing.Files[i].Name;
        }
    }

    std::vector<std::pair<std::size_t, std::string>> replaced;
    for (Content& c : contents) {
        if (!c.Changed) continue;
        replaced.emplace_back(c.File, c.Lsf ? lsf_write(c.Doc)
                                            : std::string(c.Text.begin(),
                                                          c.Text.end()));
    }
    for (std::size_t i = 0; i < listing.Files.size(); ++i) {
        if (!pak_file_empty(listing.Files[i])) continue;
        replaced.emplace_back(i, "\n");
        fixes->push_back(listing.Files[i].Name + ": was empty; now a newline");
    }
    const int count = (int)(fixes->size() - before);
    if (replaced.empty() && !renamed) return count;

    std::string const tmp = std::string(out) + ".tmp";
    if (!copy_file(in, tmp.c_str())) return -1;
    std::FILE* w = std::fopen(tmp.c_str(), "r+b");
    if (w == nullptr) return -1;
    bool ok = std::fseek(w, 0, SEEK_END) == 0;
    std::uint64_t at = (std::uint64_t)std::ftell(w);
    for (auto const& [i, data] : replaced) {
        const std::uint64_t pad = (kAlign - at % kAlign) % kAlign;
        static const char zeros[kAlign] = {};
        ok = ok && std::fwrite(zeros, 1, pad, w) == pad;
        at += pad;
        ok = ok && std::fwrite(data.data(), 1, data.size(), w) == data.size();
        PakFile& file = listing.Files[i];
        file.Offset = at;
        file.SizeOnDisk = data.size();
        file.UncompressedSize = 0;
        file.Flags = 0;
        file.Crc = crc32(data.data(), data.size());
        at += data.size();
    }
    if (renamed) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            listing.Files[i].Name = names[i];
        }
    }
    ok = ok && pak_write_listing(w, at, listing);
    ok = std::fclose(w) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), out) != 0) {
        std::remove(tmp.c_str());
        return -1;
    }
    return count;
}

namespace {

thread_local bool t_bypass = false;
std::mutex g_lock;
std::map<std::string, std::string> g_redirects;

// "<profile>/Mods/<name>.pak", where the profile is "Baldur's Gate 3".
bool mod_archive(std::string_view path) {
    if (!iends_with(path, ".pak")) return false;
    const std::size_t leaf = path.rfind('/');
    if (leaf == std::string_view::npos || leaf < 5) return false;
    std::string_view const dir = path.substr(0, leaf);
    return iends_with(dir, "/Baldur's Gate 3/Mods");
}

std::string cache_directory() {
    std::string base;
    if (char const* data = std::getenv("XDG_DATA_HOME"); data && *data) {
        base = data;
    } else if (char const* home = std::getenv("HOME")) {
        base = std::string(home) + "/.local/share";
    } else {
        return {};
    }
    return base + "/bg3le/pakfix";
}

void make_directories(std::string const& path) {
    for (std::size_t at = 1; at != std::string::npos;) {
        at = path.find('/', at + 1);
        ::mkdir(path.substr(0, at).c_str(), 0755);
    }
}

// Every file in `dir` not starting with `keep`.
void prune(std::string const& dir, std::string const& keep) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return;
    while (dirent* e = ::readdir(d)) {
        std::string const n = e->d_name;
        if (n == "." || n == ".." || n.compare(0, keep.size(), keep) == 0) {
            continue;
        }
        ::unlink((dir + "/" + n).c_str());
    }
    ::closedir(d);
}

std::string read_text(std::string const& path) {
    std::string out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

void log_fixes(std::string const& pak, std::string const& text) {
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        logf("pakfix: %s: %.*s", pak.c_str(), (int)(end - at), text.data() + at);
        at = end + 1;
    }
}

std::string prepare(std::string const& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return {};
    std::string const root = cache_directory();
    if (root.empty()) return {};

    std::string const leaf(last_component(path));
    std::string const dir = root + "/" + leaf;
    std::string const key = std::to_string((long long)st.st_size) + "-"
                            + std::to_string((long long)st.st_mtim.tv_sec)
                            + "." + std::to_string(st.st_mtim.tv_nsec) + "-f"
                            + kFormat;
    std::string const copy = dir + "/" + key + ".pak";
    std::string const notes = dir + "/" + key + ".txt";
    std::string const clean = dir + "/" + key + ".clean";

    if (::access(clean.c_str(), F_OK) == 0) return {};
    if (::access(copy.c_str(), F_OK) == 0) {
        log_fixes(leaf, read_text(notes));
        logf("pakfix: %s: reading the corrected copy %s", leaf.c_str(),
             copy.c_str());
        return copy;
    }

    make_directories(dir);
    prune(dir, key);
    std::vector<std::string> fixes;
    const int count = pak_fix_build(path.c_str(), copy.c_str(), &fixes);
    std::string text;
    for (std::string const& line : fixes) text += line + "\n";
    if (count < 0) {
        logf("pakfix: %s: could not build a corrected copy; reading the "
             "original", leaf.c_str());
        return {};
    }
    if (::access(copy.c_str(), F_OK) != 0) {
        if (!text.empty()) log_fixes(leaf, text);
        if (std::FILE* f = std::fopen(clean.c_str(), "wb")) std::fclose(f);
        return {};
    }
    if (std::FILE* f = std::fopen(notes.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    log_fixes(leaf, text);
    logf("pakfix: %s: %d fixes, reading the corrected copy %s", leaf.c_str(),
         count, copy.c_str());
    return copy;
}

bool enabled() {
    static const bool on = [] {
        char const* v = std::getenv("BG3LE_PAKFIX");
        return v == nullptr || v[0] != '0';
    }();
    return on;
}

}  // namespace

std::string pak_fix_redirect(char const* path) {
    if (t_bypass || path == nullptr || !mod_archive(path) || !enabled()) {
        return {};
    }
    std::lock_guard<std::mutex> held(g_lock);
    auto it = g_redirects.find(path);
    if (it != g_redirects.end()) return it->second;

    t_bypass = true;
    std::string copy = prepare(path);
    t_bypass = false;
    g_redirects.emplace(path, copy);
    return copy;
}

}  // namespace bg3le
