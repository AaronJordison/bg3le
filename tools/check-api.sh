#!/bin/bash
# Checks every function in reference/upstream-api.txt against the running
# game, in both contexts: the server's for Both and Server, the client's for
# Both and Client. Lists what is missing; exits 1 if anything is.
#
# Needs the game running with bg3le attached and a save loaded.
# Regenerate the list with tools/upstream-api.py when bg3se adds functions.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
CLI="$HERE/../client/bg3lua"
LIST="$HERE/../reference/upstream-api.txt"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

status=0
for context in Server Client; do
    {
        echo "local api = {"
        grep -v '^#' "$LIST" | awk -v c="$context" '$1 == "Both" || $1 == c {
            printf "  {\"%s\", \"%s\"},\n", $2, $3 }'
        echo "}"
        cat <<'EOF'
local missing, present = {}, 0
for _, row in ipairs(api) do
  local t = Ext
  for part in row[1]:gmatch("[^.]+") do t = type(t) == "table" and t[part] or nil end
  local f = type(t) == "table" and t[row[2]] or nil
  local mt = type(f) == "table" and getmetatable(f) or nil
  if type(f) == "function" or (mt ~= nil and mt.__call ~= nil) then
    present = present + 1
  else
    missing[#missing + 1] = "Ext." .. row[1] .. "." .. row[2]
  end
end
print(string.format("APICHECK %d of %d present", present, #api))
for _, m in ipairs(missing) do print("APICHECK missing " .. m) end
EOF
    } > "$work/check.lua"

    flag=""
    [ "$context" = "Client" ] && flag="--client"
    out="$(timeout 60 "$CLI" $flag -f "$work/check.lua" 2>&1 | grep '^APICHECK' | sed 's/^APICHECK //')"
    if [ -z "$out" ]; then
        echo "$context: no answer from the game" >&2
        status=1
        continue
    fi
    echo "$context: $(echo "$out" | head -1)"
    echo "$out" | tail -n +2 | sed 's/^/  /'
    echo "$out" | grep -q '^missing ' && status=1
done
exit $status
