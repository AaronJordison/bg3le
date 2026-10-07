// Writes the corrected copy bg3le would read in place of a mod archive, and
// lists the fixes: pakfix IN.pak OUT.pak. OUT is not written if nothing
// needs fixing.

#include <cstdio>
#include <string>
#include <vector>

#include "pak_fix.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: pakfix IN.pak OUT.pak\n");
        return 2;
    }
    std::vector<std::string> fixes;
    const int count = bg3le::pak_fix_build(argv[1], argv[2], &fixes);
    for (std::string const& line : fixes) std::printf("%s\n", line.c_str());
    if (count < 0) {
        std::fprintf(stderr, "pakfix: could not read %s or write %s\n",
                     argv[1], argv[2]);
        return 1;
    }
    std::printf("%d fixes\n", count);
    return 0;
}
