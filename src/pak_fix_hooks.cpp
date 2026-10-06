// Hands the game a mod archive's corrected copy (pak_fix.h) wherever it
// opens or stats the original. These are the calls the game imports for it.

#undef _FORTIFY_SOURCE

#include <cstdarg>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "pak_fix.h"

namespace {

template <typename Fn>
Fn next(char const* name, char const* version = nullptr) {
    void* fn = version != nullptr ? ::dlvsym(RTLD_NEXT, name, version) : nullptr;
    if (fn == nullptr) fn = ::dlsym(RTLD_NEXT, name);
    return reinterpret_cast<Fn>(fn);
}

using OpenFn = int (*)(char const*, int, ...);
using FopenFn = FILE* (*)(char const*, char const*);

bool takes_mode(int flags) {
    return (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
}

int open_via(OpenFn real, char const* path, int flags, mode_t mode) {
    if ((flags & O_ACCMODE) == O_RDONLY) {
        std::string const copy = bg3le::pak_fix_redirect(path);
        if (!copy.empty()) return real(copy.c_str(), flags, mode);
    }
    return real(path, flags, mode);
}

FILE* fopen_via(FopenFn real, char const* path, char const* mode) {
    if (mode != nullptr && mode[0] == 'r' && std::strchr(mode, '+') == nullptr) {
        std::string const copy = bg3le::pak_fix_redirect(path);
        if (!copy.empty()) return real(copy.c_str(), mode);
    }
    return real(path, mode);
}

}  // namespace

extern "C" int open(char const* path, int flags, ...) {
    static const auto real = next<OpenFn>("open");
    mode_t mode = 0;
    if (takes_mode(flags)) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return open_via(real, path, flags, mode);
}

extern "C" int open64(char const* path, int flags, ...) {
    static const auto real = next<OpenFn>("open64");
    mode_t mode = 0;
    if (takes_mode(flags)) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return open_via(real, path, flags, mode);
}

extern "C" FILE* fopen(char const* path, char const* mode) {
    static const auto real = next<FopenFn>("fopen");
    return fopen_via(real, path, mode);
}

extern "C" FILE* fopen64(char const* path, char const* mode) {
    static const auto real = next<FopenFn>("fopen64");
    return fopen_via(real, path, mode);
}

// The game checks for files with this; part 1 of a split copy is not on disk
// under the name it asks for.
extern "C" int access(char const* path, int mode) __THROW {
    using Fn = int (*)(char const*, int);
    static const auto real = next<Fn>("access");
    if ((mode & W_OK) == 0) {
        std::string const copy = bg3le::pak_fix_redirect(path);
        if (!copy.empty()) return real(copy.c_str(), mode);
    }
    return real(path, mode);
}

// Only a compatibility symbol in glibc 2.33 and later, which the game, built
// against an older one, still calls.
extern "C" int __xstat64(int version, char const* path,
                         struct stat64* out) __THROW {
    using Fn = int (*)(int, char const*, struct stat64*);
    static const auto real = next<Fn>("__xstat64", "GLIBC_2.2.5");
    std::string const copy = bg3le::pak_fix_redirect(path);
    return real(version, copy.empty() ? path : copy.c_str(), out);
}
