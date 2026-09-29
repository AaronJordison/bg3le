// Finding engine code and data in whatever build is running.
//
// Addresses recorded against one build go stale with the next, even a hotfix:
// v4.76.31.656 moved nearly every function by 64-128 bytes without changing
// it. So a target is described by what it is -- its bytes, its symbol, the
// instruction that loads it -- and the recorded address is only where the
// search starts. Results are cached per build ID.
//
// Everything returns a link-time offset (add load_bias() for an address), or
// 0 when the target cannot be found unambiguously. Callers treat 0 as "turn
// the feature off", never as a guess.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bg3le {

class SymbolTable;

// A target's bytes as hex pairs, "??" for a byte that varies between builds
// (a rip-relative displacement, a call target). `start` is where the target
// begins relative to the first byte of the pattern.
struct Sig {
    const char* name;           // for the log and the cache
    std::uintptr_t recorded;    // link-time offset in the build it was taken from
    const char* pattern;
    std::ptrdiff_t start = 0;
};

// Needed by resolve_symbol and resolve_vtable; preload sets it once parsed.
void resolve_set_symbols(const SymbolTable* symbols);

// The unique match of sig in .text: within a window around sig.recorded
// first, then anywhere. 0 if there is none, or more than one.
std::uintptr_t resolve_code(const Sig& sig);

// A named symbol's offset (functions from bundled libraries keep theirs).
std::uintptr_t resolve_symbol(const char* mangled);

// Every symbol whose mangled name contains both substrings (b may be null),
// as offsets: for a family of overloads rather than one exact name.
std::vector<std::uintptr_t> resolve_symbols_with(const char* a, const char* b = nullptr);

// A vtable's first virtual slot, from its _ZTV symbol.
std::uintptr_t resolve_vtable(const char* ztv_mangled);

// The target of a rip-relative operand: sig finds the instruction, whose
// disp32 sits disp_at bytes in and which is insn_len bytes long.
std::uintptr_t resolve_rip(const Sig& sig, int disp_at, int insn_len);

// Same, for an E8 call or E9 jump: the function it goes to.
std::uintptr_t resolve_call(const Sig& sig);

// The build's GNU build ID as hex, or "" if the executable has none.
const char* build_id();

// Where `bytes` first occur within `window` bytes of `from` (a function's
// start), or 0. For checks that a function still holds the instructions bg3le
// relies on -- the field offsets they encode, say -- without pinning how far
// into the function they sit, which any change earlier in it would move.
std::uintptr_t code_near_n(std::uintptr_t from, const unsigned char* bytes, std::size_t len,
                           std::size_t window = 0x200);

template <std::size_t N>
std::uintptr_t code_near(std::uintptr_t from, const unsigned char (&bytes)[N],
                         std::size_t window = 0x200) {
    return code_near_n(from, bytes, N, window);
}

}  // namespace bg3le
