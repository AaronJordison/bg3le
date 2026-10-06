#pragma once

// Corrected copies of mod archives, for two things the native Linux game
// trips over and Windows does not: empty files (an empty stats .txt hangs
// the load) and file names whose case differs from what refers to them.

#include <string>
#include <vector>

namespace bg3le {

// The archive the game should read in place of `path`: a corrected copy,
// built on first ask and cached, or empty to read the original.
std::string pak_fix_redirect(char const* path);

// Analyses the archive at `in` and, if anything needs fixing, writes the
// corrected copy to `out`. Each fix is appended to `fixes`. Returns the
// number of fixes, or -1 if `in` could not be read or `out` written.
int pak_fix_build(char const* in, char const* out,
                  std::vector<std::string>* fixes);

}  // namespace bg3le
