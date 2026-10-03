#!/bin/bash
# Checks every member of the captured upstream surface (reference/ext-api-surface.txt)
# against the running game, functions and everything else: tables such as
# Ext.System, values such as Ext.Config's. tools/check-api.sh covers functions
# from upstream's declarations, in both contexts; this catches what has no
# declaration to check. The capture is the server's, so run it there (the
# default); --client also lists the server-only modules, which is expected.
#
# Needs the game running with bg3le attached, past the level load.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
CLI="$HERE/../client/bg3lua"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

python3 - "$HERE/../reference/ext-api-surface.txt" > "$work/surface.lua" <<'PY'
import re, sys
rows = []
for line in open(sys.argv[1]):
    line = line.strip()
    m = re.match(r'^Ext\.([\w.]+) = \{(.*)\}$', line)
    if m:
        rows.append((m.group(1), None, 'table'))
        rows += [(m.group(1), k, t) for k, t in re.findall(r'(\w+)\((\w+)\)', m.group(2))]
        continue
    m = re.match(r'^Ext\.([\w.]+) : (\w+)$', line)
    if m:
        rows.append((m.group(1), None, m.group(2)))
print('local rows = {')
for mod, key, kind in rows:
    print('  {%r, %s, %r},' % (mod, repr(key) if key else 'nil', kind))
print('}')
print('''
local function get(t, k)
  if t == nil then return nil end
  local ok, v = pcall(function() return t[k] end)
  if ok then return v end
  return nil
end
local missing, wrong = {}, {}
for _, r in ipairs(rows) do
  local v = Ext
  for part in r[1]:gmatch("[^.]+") do v = get(v, part) end
  if r[2] ~= nil then v = get(v, r[2]) end
  local name = "Ext." .. r[1] .. (r[2] and ("." .. r[2]) or "")
  local callable = type(v) == "function" or (getmetatable(v) ~= nil and getmetatable(v).__call ~= nil)
  if v == nil then
    missing[#missing + 1] = name .. " (" .. r[3] .. ")"
  elseif r[3] == "function" and not callable then
    wrong[#wrong + 1] = name .. ": " .. type(v) .. ", not a function"
  elseif (r[3] == "boolean" or r[3] == "number") and type(v) ~= r[3] then
    wrong[#wrong + 1] = name .. ": " .. type(v) .. ", not a " .. r[3]
  end
end
print(string.format("SURFACECHECK %d members, %d missing, %d of the wrong type", #rows, #missing, #wrong))
for _, m in ipairs(missing) do print("SURFACECHECK missing " .. m) end
for _, m in ipairs(wrong) do print("SURFACECHECK wrong " .. m) end
''')
PY

"$CLI" "$@" -f "$work/surface.lua" 2>&1 | grep -a "SURFACECHECK" | sed 's/^SURFACECHECK //' | grep -v "Ext\._Internal\."
