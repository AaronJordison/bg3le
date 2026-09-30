#!/bin/bash
#
# Packages a release: dist/bg3le-<version>-linux-x86_64.tar.gz, with the portable libbg3le.so from
# tools/build-sniper.sh, install.py and everything it installs, and the licenses of what the library bundles.
#
# The library goes to build/libbg3le.so inside the package, where install.py looks by default, so unpacked, a plain
# ./install.py installs it. The version is git describe's, or BG3LE_VERSION if set.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/external/third_party"
LLVM="$ROOT/build-sniper/llvm-project"

# Portable build first: it is incremental, and it fails if the library asks for a glibc newer than the runtime's.
"$ROOT/tools/build-sniper.sh"

VERSION="${BG3LE_VERSION:-$(git -C "$ROOT" describe --tags --always --dirty)}"
NAME="bg3le-$VERSION-linux-x86_64"
WORK="$ROOT/build-sniper/package"
STAGE="$WORK/$NAME"
rm -rf "$WORK"
mkdir -p "$STAGE/build" "$STAGE/installer" "$STAGE/client" "$STAGE/licenses" "$ROOT/dist"

echo "== $NAME =="
install -m 0755 "$ROOT/build-sniper/bg3le/libbg3le.so" "$STAGE/build/libbg3le.so"
install -m 0755 "$ROOT/install.py" "$STAGE/install.py"
install -m 0755 "$ROOT/installer/bg3le-launch" "$STAGE/installer/bg3le-launch"
install -m 0755 "$ROOT/client/bg3lua" "$STAGE/client/bg3lua"
install -m 0644 "$ROOT/README.md" "$STAGE/README.md"
install -m 0644 "$ROOT/LICENSE" "$STAGE/LICENSE"

# What libbg3le.so is built from, beyond bg3le itself: statically linked code and compiled-in headers.
lic() {  # lic <name> <file>
    if [ -f "$2" ]; then install -m 0644 "$2" "$STAGE/licenses/$1"; else echo "  missing license for $1: $2" >&2; exit 1; fi
}
lic bg3se.txt         "$ROOT/vendor/bg3se/LICENSE"
lic libcxx.txt        "$LLVM/libcxx/LICENSE.TXT"
lic libcxxabi.txt     "$LLVM/libcxxabi/LICENSE.TXT"
lic abseil.txt        "$EXT/abseil/LICENSE"
lic protobuf.txt      "$EXT/protobuf/LICENSE"
lic imgui.txt         "$EXT/imgui/LICENSE.txt"
lic optick.txt        "$EXT/optick/LICENSE"
lic glm.txt           "$EXT/glm/copying.txt"
lic rapidjson.txt     "$EXT/rapidjson/license.txt"
lic tinycrypt.txt     "$EXT/tinycrypt/LICENSE"
lic vulkan-headers.md "$EXT/Vulkan/LICENSE.md"
lic lz4.txt           "$ROOT/external/lz4/LICENSE"
# Lua keeps its license at the end of lua.h.
sed -n '/^\* Copyright (C) .* Lua.org, PUC-Rio\./,/^\*\*\*\*/p' "$ROOT/external/lua/lua.h" \
    | sed -e '/^\*\*\*\*/d' -e 's/^\* \{0,1\}//' > "$STAGE/licenses/lua.txt"
[ -s "$STAGE/licenses/lua.txt" ] || { echo "  missing license for Lua" >&2; exit 1; }

cat > "$STAGE/INSTALL.txt" <<EOF
bg3le $VERSION, for Baldur's Gate 3's native Linux build on Steam.

Needs Steam (not the Flatpak) and python3. The library is built against the Steam Runtime 3 (sniper) SDK, so it
loads on any machine that runs the game.

  ./install.py              install, or update an existing install
  ./install.py --dry-run    show what would change
  ./install.py --uninstall  remove it

install.py copies the library, the launch wrapper and the bg3lua console client into ~/.local/share/bg3le, and adds
the wrapper to the game's Steam launch options. Steam has to be closed for that; it asks before stopping it.
EOF

# The package installs as it is: every file install.py copies is where it looks for it.
python3 "$STAGE/install.py" --help > /dev/null

ARCHIVE="$ROOT/dist/$NAME.tar.gz"
tar -C "$WORK" --owner=0 --group=0 --numeric-owner --sort=name -czf "$ARCHIVE" "$NAME"
echo "  $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
echo "  sha256 $(sha256sum "$ARCHIVE" | cut -d' ' -f1)"
case "$VERSION" in
    *-dirty) echo "  note: built from uncommitted changes (version $VERSION)" ;;
esac
