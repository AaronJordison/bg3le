// The loose mirror of mods' stats files. See stats_mirror.h for why.
//
// Everything here is best-effort: a mirror that cannot run must never take
// the game down with it. Every failure path logs and returns.

#include "stats_mirror.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"
#include "pak.h"

namespace bg3le {

namespace {

std::string profile_root() {
    char const* home = std::getenv("HOME");
    if (home == nullptr) return {};
    return std::string(home)
           + "/.local/share/Larian Studios/Baldur's Gate 3";
}

std::string data_root() {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return {};

    std::string path(exe);
    const std::size_t bin = path.rfind("/bin/");
    if (bin == std::string::npos) return {};
    return path.substr(0, bin) + "/Data";
}

std::string slurp(char const* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return {};
    std::string text;
    char block[65536];
    while (const std::size_t got = std::fread(block, 1, sizeof(block), f)) {
        text.append(block, got);
    }
    std::fclose(f);
    return text;
}

bool make_parents(std::string const& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return true;

    std::string dir = path.substr(0, slash);
    for (std::size_t at = 1; at <= dir.size(); ++at) {
        if (at != dir.size() && dir[at] != '/') continue;
        const std::string step = dir.substr(0, at);
        if (::mkdir(step.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool write_file(char const* path, char const* data, std::size_t size) {
    const std::string tmp = std::string(path) + ".bg3le-mirror-tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = size == 0
                    || std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    if (!ok) {
        ::remove(tmp.c_str());
        return false;
    }
    return ::rename(tmp.c_str(), path) == 0;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), ::tolower);
    return text;
}

// The module UUIDs the player enabled, in modsettings.lsx order.
std::vector<std::string> active_uuids(std::string const& profile) {
    std::vector<std::string> uuids;
    const std::string text =
        slurp((profile + "/PlayerProfiles/Public/modsettings.lsx").c_str());
    std::size_t at = 0;
    for (;;) {
        const std::size_t entry = text.find("<node id=\"ModuleShortDesc\"", at);
        if (entry == std::string::npos) break;
        const std::size_t end = text.find("</node>", entry);
        const std::size_t uuid = text.find("id=\"UUID\"", entry);
        if (uuid == std::string::npos
            || (end != std::string::npos && uuid > end)) {
            at = entry + 1;
            continue;
        }
        const std::size_t value = text.find("value=\"", uuid);
        if (value == std::string::npos) break;
        const std::size_t from = value + 7;
        const std::size_t to = text.find('"', from);
        if (to == std::string::npos) break;
        uuids.emplace_back(text.data() + from, to - from);
        at = end == std::string::npos ? to : end;
    }
    return uuids;
}

// The value of one attribute of a meta.lsx, or empty.
std::string meta_attribute(std::string const& meta, char const* id) {
    const std::string needle = std::string("id=\"") + id + "\"";
    const std::size_t at = meta.find(needle);
    if (at == std::string::npos) return {};
    const std::size_t value = meta.find("value=\"", at);
    if (value == std::string::npos) return {};
    const std::size_t from = value + 7;
    const std::size_t to = meta.find('"', from);
    if (to == std::string::npos) return {};
    return meta.substr(from, to - from);
}

// The module folder of a "Mods/<folder>/meta.lsx" entry name, or empty.
std::string meta_folder(char const* entry) {
    const std::size_t len = std::strlen(entry);
    if (len < 15  // "Mods/x/meta.lsx"
        || std::strncmp(entry, "Mods/", 5) != 0
        || std::strcmp(entry + len - 8, "meta.lsx") != 0) {
        return {};
    }
    const char* rest = entry + 5;
    const char* slash = std::strchr(rest, '/');
    if (slash == nullptr) return {};
    return std::string(rest, slash - rest);
}

// The mod's own stats tree only: a compat patch shipped into another mod's
// Public folder is not this module's to mirror, and the engine mounts each
// module's own directory. The trailing slash in the prefix keeps a folder
// that merely prefixes another ("Foo", "FooBar") from matching.
bool own_stats_entry(std::string const& nameLower,
                     std::string const& folderLower) {
    const std::string prefix = "public/" + folderLower + "/stats/generated/";
    if (nameLower.size() <= prefix.size()) return false;
    if (nameLower.compare(0, prefix.size(), prefix) != 0) return false;
    return nameLower.compare(nameLower.size() - 4, 4, ".txt") == 0;
}

std::vector<std::string> paks_in(char const* dir) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir);
    if (d == nullptr) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() < 5
            || name.compare(name.size() - 4, 4, ".pak") != 0) {
            continue;
        }
        out.push_back(std::string(dir) + "/" + name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

void mirror_mod_stats() {
    char const* disable = std::getenv("BG3LE_STATS_MIRROR");
    if (disable != nullptr && disable[0] == '0') {
        logf("stats mirror: disabled (BG3LE_STATS_MIRROR=0)");
        return;
    }

    const std::string profile = profile_root();
    const std::string data = data_root();
    if (profile.empty() || data.empty()) {
        logf("stats mirror: no profile or data root");
        return;
    }

    const std::vector<std::string> wanted = active_uuids(profile);
    if (wanted.empty()) {
        logf("stats mirror: no mods enabled in modsettings.lsx");
        return;
    }

    const std::string manifestPath = data + "/bg3le.stats-mirror.manifest";
    std::vector<std::string> previous;
    {
        const std::string manifest = slurp(manifestPath.c_str());
        std::size_t at = 0;
        while (at < manifest.size()) {
            std::size_t nl = manifest.find('\n', at);
            if (nl == std::string::npos) nl = manifest.size();
            std::string line = manifest.substr(at, nl - at);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) previous.push_back(line);
            at = nl + 1;
        }
    }
    std::sort(previous.begin(), previous.end());

    std::vector<std::string> mirrored;  // Data-relative paths
    std::size_t written = 0;
    std::size_t bytes = 0;
    std::size_t activePaks = 0;

    const std::vector<std::string> roots{profile + "/Mods", data + "/Mods"};
    for (const std::string& root : roots) {
        for (const std::string& pak : paks_in(root.c_str())) {
            // Which module lives here: its meta.lsx carries the UUID the
            // modsettings list is written in.
            std::string folder;
            pak_list(pak.c_str(), [&](char const* name) {
                const std::string found = meta_folder(name);
                if (!found.empty()) folder = found;
            });
            if (folder.empty()) continue;

            const std::string metaName = "Mods/" + folder + "/meta.lsx";
            std::string uuid;
            pak_read(
                pak.c_str(),
                [&](char const* name) {
                    return std::strcmp(name, metaName.c_str()) == 0;
                },
                [&](char const*, char const* data_, std::size_t size) {
                    uuid = meta_attribute(std::string(data_, size), "UUID");
                });
            if (uuid.empty()
                || std::find(wanted.begin(), wanted.end(), uuid)
                       == wanted.end()) {
                continue;
            }
            ++activePaks;

            const std::string folderLower = lower(folder);
            std::vector<std::string> entries;
            pak_list(pak.c_str(), [&](char const* name) {
                if (own_stats_entry(lower(name), folderLower)) {
                    entries.push_back(name);
                }
            });

            for (const std::string& entry : entries) {
                std::string contents;
                bool found = false;
                pak_read(
                    pak.c_str(),
                    [&](char const* name) { return name == entry; },
                    [&](char const*, char const* data_, std::size_t size) {
                        contents.assign(data_, size);
                        found = true;
                    });
                if (!found) {
                    logf("stats mirror: %s: could not read", entry.c_str());
                    continue;
                }

                const std::string target = data + "/" + entry;
                if (!make_parents(target)
                    || !write_file(target.c_str(), contents.data(),
                                   contents.size())) {
                    logf("stats mirror: %s: could not write", target.c_str());
                    continue;
                }
                mirrored.push_back(entry);
                bytes += contents.size();
                ++written;
            }
        }
    }

    // A mod that is gone must not keep speaking through its loose files.
    std::sort(mirrored.begin(), mirrored.end());
    std::size_t pruned = 0;
    for (const std::string& stale : previous) {
        if (std::binary_search(mirrored.begin(), mirrored.end(), stale)) {
            continue;
        }
        if (::remove((data + "/" + stale).c_str()) == 0) ++pruned;
    }

    if (mirrored != previous) {
        std::FILE* m = std::fopen(manifestPath.c_str(), "wb");
        if (m != nullptr) {
            for (const std::string& line : mirrored) {
                std::fprintf(m, "%s\n", line.c_str());
            }
            std::fclose(m);
        } else {
            logf("stats mirror: could not write the manifest");
        }
    }

    logf("stats mirror: %zu active paks, %zu files (%zu bytes) loose, "
         "%zu pruned",
         activePaks, written, bytes, pruned);
}

}  // namespace bg3le
