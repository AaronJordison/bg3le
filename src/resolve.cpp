#include "resolve.h"

#include <elf.h>
#include <link.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "elf_symbols.h"
#include "hook.h"
#include "log.h"

namespace bg3le {
namespace {

// Hotfixes move code by bytes to kilobytes; searching near the recorded
// offset first keeps a common pattern from matching somewhere unrelated.
constexpr std::uintptr_t kWindow = 1u << 20;

const SymbolTable* g_symbols = nullptr;
std::mutex g_lock;

struct Pattern {
    std::vector<unsigned char> bytes;
    std::vector<bool> fixed;
    std::size_t anchor = 0;  // first fixed byte, for the memchr skip
};

bool parse(const char* text, Pattern* out) {
    const char* p = text;
    while (*p != '\0') {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?' && p[1] == '?') {
            out->bytes.push_back(0);
            out->fixed.push_back(false);
            p += 2;
            continue;
        }
        char hex[3] = {p[0], p[1], '\0'};
        char* end = nullptr;
        const long v = std::strtol(hex, &end, 16);
        if (end != hex + 2) return false;
        out->bytes.push_back(static_cast<unsigned char>(v));
        out->fixed.push_back(true);
        p += 2;
    }
    for (std::size_t i = 0; i < out->fixed.size(); ++i) {
        if (out->fixed[i]) { out->anchor = i; return true; }
    }
    return false;  // all wildcards
}

bool match_at(const unsigned char* code, const Pattern& pat) {
    for (std::size_t i = 0; i < pat.bytes.size(); ++i) {
        if (pat.fixed[i] && code[i] != pat.bytes[i]) return false;
    }
    return true;
}

// Matches of pat in [lo, hi) (link-time offsets), stopping at the second.
std::size_t scan(const Pattern& pat, std::uintptr_t lo, std::uintptr_t hi, std::uintptr_t* found) {
    const auto* base = reinterpret_cast<const unsigned char*>(load_bias());
    const std::size_t len = pat.bytes.size();
    if (hi < lo + len) return 0;
    std::size_t hits = 0;
    const unsigned char first = pat.bytes[pat.anchor];
    const unsigned char* p = base + lo + pat.anchor;
    const unsigned char* last = base + hi - len + pat.anchor;
    while (p <= last) {
        p = static_cast<const unsigned char*>(std::memchr(p, first, static_cast<std::size_t>(last - p) + 1));
        if (p == nullptr) break;
        const unsigned char* at = p - pat.anchor;
        if (match_at(at, pat)) {
            if (++hits > 1) return hits;
            *found = static_cast<std::uintptr_t>(at - base);
        }
        ++p;
    }
    return hits;
}

// --- per-build cache: "name offset" lines in ~/.cache/bg3le/resolve-<build id>.txt

std::map<std::string, std::uintptr_t>* g_cache = nullptr;
std::string g_cache_path;

void load_cache() {
    if (g_cache != nullptr) return;
    g_cache = new std::map<std::string, std::uintptr_t>();
    const char* id = build_id();
    if (id[0] == '\0') return;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    std::string dir = (xdg != nullptr && xdg[0] != '\0') ? xdg : (home != nullptr ? std::string(home) + "/.cache" : "");
    if (dir.empty()) return;
    dir += "/bg3le";
    ::mkdir(dir.c_str(), 0755);
    g_cache_path = dir + "/resolve-" + id + ".txt";
    if (FILE* f = std::fopen(g_cache_path.c_str(), "r")) {
        char name[256];
        unsigned long off = 0;
        while (std::fscanf(f, "%255s %lx", name, &off) == 2) (*g_cache)[name] = off;
        std::fclose(f);
    }
}

void store(const char* name, std::uintptr_t off) {
    (*g_cache)[name] = off;
    if (g_cache_path.empty()) return;
    if (FILE* f = std::fopen(g_cache_path.c_str(), "a")) {
        std::fprintf(f, "%s %lx\n", name, static_cast<unsigned long>(off));
        std::fclose(f);
    }
}

}  // namespace

void resolve_set_symbols(const SymbolTable* symbols) { g_symbols = symbols; }

const char* build_id() {
    static std::string id;
    static bool done = false;
    if (done) return id.c_str();
    done = true;
    ::dl_iterate_phdr(
        [](struct dl_phdr_info* info, std::size_t, void*) {
            if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') return 0;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE) continue;
                auto* p = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
                const unsigned char* end = p + ph.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= end) {
                    const auto* n = reinterpret_cast<const ElfW(Nhdr)*>(p);
                    const unsigned char* name = p + sizeof(*n);
                    const unsigned char* desc = name + ((n->n_namesz + 3) & ~3u);
                    if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0) {
                        char hex[3];
                        for (unsigned k = 0; k < n->n_descsz; ++k) {
                            std::snprintf(hex, sizeof(hex), "%02x", desc[k]);
                            id += hex;
                        }
                        return 1;
                    }
                    p = desc + ((n->n_descsz + 3) & ~3u);
                }
            }
            return 1;
        },
        nullptr);
    return id.c_str();
}

std::uintptr_t resolve_code(const Sig& sig) {
    std::lock_guard<std::mutex> guard(g_lock);
    Pattern pat;
    if (!parse(sig.pattern, &pat)) {
        logf("resolve: %s has a malformed pattern", sig.name);
        return 0;
    }
    std::uintptr_t text = 0;
    std::size_t size = 0;
    if (!text_range(&text, &size)) return 0;
    const std::uintptr_t text_end = text + size;
    auto fits = [&](std::uintptr_t at) { return at >= text && at + pat.bytes.size() <= text_end; };
    const auto* base = reinterpret_cast<const unsigned char*>(load_bias());

    load_cache();
    auto cached = g_cache->find(sig.name);
    if (cached != g_cache->end()) {
        const std::uintptr_t at = cached->second - sig.start;
        if (fits(at) && match_at(base + at, pat)) return cached->second;
    }

    const std::uintptr_t rec = sig.recorded - sig.start;
    std::uintptr_t found = 0;
    std::size_t hits = 0;
    if (fits(rec) && match_at(base + rec, pat)) {
        // Still where it was recorded; only take it if it is also unique nearby.
        hits = scan(pat, rec > text + kWindow ? rec - kWindow : text,
                    rec + kWindow < text_end ? rec + kWindow : text_end, &found);
    } else {
        hits = scan(pat, rec > text + kWindow ? rec - kWindow : text,
                    rec + kWindow < text_end ? rec + kWindow : text_end, &found);
        if (hits == 0) hits = scan(pat, text, text_end, &found);
    }
    if (hits != 1) {
        logf("resolve: %s: %s -- leaving it off", sig.name, hits == 0 ? "no match" : "ambiguous");
        return 0;
    }
    const std::uintptr_t off = found + sig.start;
    if (off != sig.recorded) {
        logf("resolve: %s at %#lx (recorded %#lx, %+ld)", sig.name, (unsigned long)off,
             (unsigned long)sig.recorded, (long)(off - sig.recorded));
    }
    store(sig.name, off);
    return off;
}

std::uintptr_t resolve_symbol(const char* mangled) {
    if (g_symbols == nullptr) return 0;
    void* p = g_symbols->find(mangled);
    if (p == nullptr) {
        logf("resolve: no symbol %s", mangled);
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(p) - load_bias();
}

std::uintptr_t resolve_vtable(const char* ztv_mangled) {
    const std::uintptr_t vt = resolve_symbol(ztv_mangled);
    return vt == 0 ? 0 : vt + 16;  // past offset-to-top and the typeinfo pointer
}

std::uintptr_t resolve_rip(const Sig& sig, int disp_at, int insn_len) {
    const std::uintptr_t insn = resolve_code(sig);
    if (insn == 0) return 0;
    std::int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(load_bias() + insn + disp_at), 4);
    return insn + insn_len + disp;
}

std::uintptr_t resolve_call(const Sig& sig) {
    const std::uintptr_t insn = resolve_code(sig);
    if (insn == 0) return 0;
    const auto op = *reinterpret_cast<const unsigned char*>(load_bias() + insn);
    if (op != 0xE8 && op != 0xE9) {
        logf("resolve: %s is not a call or jump", sig.name);
        return 0;
    }
    return resolve_rip(sig, 1, 5);
}

}  // namespace bg3le
