# Achievements with mods active

Goal: port bg3se's `EnableAchievements` option (Windows) to bg3le, so mods
do not silently block Steam achievements. **Status: done, validated live on
2026-09-23.** Address-level detail for every function named below is in
[PREDICATE-ANALYSIS.md](PREDICATE-ANALYSIS.md).

Every claim is tagged: `[LIVE]` confirmed in a running process, `[DISASM]`
read from objdump output, `[DECOMP]` from a decompiler, `[HYP]` hypothesis.

## How bg3se does it on Windows

bg3se patches two functions directly: `ls::ModuleSettings::IsModded` (forced
to always report "not modded") and `esv::SavegameManager::ThrowError` (its
warning branch NOP'd out). Both are same-size single-branch byte patches
found by AOB pattern scanning, since neither function is exported even on
Windows.

## What bg3le does

The Linux binary exports none of these names, but the same logic exists as
`HasCustomMods` at raw VMA `0x3767580`, the loop "does any module fail the
official-module predicate". `ensure_achievement_gate_patch()` in
`src/preload.cpp` overwrites its prologue (`55 41 57 41 56 41 55 41 54 53
50`, verified before writing) with `31 c0 c3` (`xor eax,eax; ret`): "not
modded", the direct equivalent of bg3se's `IsModded` patch.

Until 2026-09-28 the patch went one level lower, forcing the per-module
predicate (`0x37675f0`) to return 1. That predicate has 13 callers, and a new
game builds its module list with it: with every module "official", a new
game started with the 13 base modules and no mods at all, so no modded class,
race or item existed in character creation. Save loads were unaffected, as
they take the save's own module list. Patching the loop leaves the predicate
truthful for its other callers.

- Applied from the `bg3le_init` constructor, before `main` and before the
  game's `fork()`, so both processes and every session-load cache see it.
  Re-checked after `RegisterDIVFunctions` and once per server tick; the
  re-check is a `memcmp`, the patch is only written when missing.
- Only when `/proc/self/exe` is `bg3` and the target lies inside the main
  object's `.text` (`in_text()` in `src/hook.cpp`). `LD_PRELOAD` also lands
  in `steam-launch-wrapper` and `reaper`, and the first build crashed both
  by writing into whatever happened to sit at that offset.
- Off with `BG3LE_ACHIEVEMENTS=0` (unset or any other value means on), or
  with `"EnableAchievements": false` in `ScriptExtenderSettings.json` next
  to the game binary (bg3se parity, default true; `settings_flag()` in
  `src/console.cpp`).
- Log lines: `EnableAchievements: patched IsModded at 0x3767580`
  on success, `WARNING: EnableAchievements IsModded patch refused` when the
  prologue bytes do not match (a new game build; see "Updating for a new
  binary" below). `Modded achievements enabled` is reported at story load,
  at the same point bg3se reports it.

## Why the mod gate is not one branch

The check is one small predicate, "is this module official", called from a
loop "does any module fail the predicate", and that loop is consulted by
several independent consumers, two of which cache the answer in a byte.
Patching a single consumer therefore changes nothing observable.

### Verified facts

| Raw VMA | Label | What it is | Tag |
|---|---|---|---|
| `0x37675f0` | `FUN_038675f0` | Per-module predicate. Formats the module GUID at `entry+8` as `%08x-%04hx-...`, builds a FixedString from it, then compares that FixedString against a packed table of 19 official-module constants with SSE. Returns 1 for an official module. | `[DISASM]` `[DECOMP]` |
| `0x3767580` | `FUN_03867580` | `HasCustomMods(list)`. `list+0x14` = count, `list+8` = entries, stride `0x60`. Returns 0 if the list is empty or every entry passes the predicate, 1 if any entry fails it. Prologue `55 41 57 41 56 41 55 41 54 53 50`. | `[DISASM]` |
| `0x3767780` | `FUN_03867780` | Registered Osiris native for `UnlockAchievement` (id `0x80001669`). `if (!HasCustomMods(gEocServer+0x268)) ...` (server-side check #1). Hit 8 times in the child process during one achievement event, always with the check returning 1 while mods were active. | `[LIVE]` `[DECOMP]` |
| `0x3767800` | `FUN_03867800` | Called two steps later by the above; runs `HasCustomMods(gEocServer+0x268)` **again** (server-side check #2), then resolves the achievement through the manager at `gEocServer+0xb0`. | `[DECOMP]` |
| `0x3767890` | `FUN_03867890` | Sends `eocnet::AchievementMessage` (net id `0xb3`) to the character's user peer. | `[DECOMP]` |
| `0x3767230` | `FUN_03867230` | Sibling Osiris native (achievement progress), same `HasCustomMods` guard, sends net id `0xb9`. | `[DECOMP]` |
| `0x4019c40` | `FUN_04119c40` | Save/session load. `modded = (*(obj+0xf8) != 0) ? 1 : HasCustomMods(obj+0x20)`, stored into `param_1+0x87`: a cached per-session "IsModded" byte. | `[DECOMP]` |
| `0x2c2cb20` | `FUN_02d2cb20` | Load-status reporter behind the Load Game badges. Reads the `+0x87` cache and also has an inlined copy of the loop over `gEocServer+0x108`. The Linux shape of bg3se's `ThrowError` AOB. | `[DECOMP]` |
| `0x62be2ca` | `FUN_063be100` | Client mod-manager state: `HasCustomMods([r13+0x8b8]+0x198)` compared with the cached byte `[r13+0x828]`, stored on change. A second cached "IsModded" byte. | `[DISASM]` |
| `0x388e8e0` | `FUN_0398e8e0` | Called by the Osiris handler once check #1 passes. Resolves a GUID string to an entity handle; 337 callers. A generic helper, not "the unlock". | `[DECOMP]` |
| `0x34b2440` | client `ProcessMsg` | Branch `0x34b25fa..0x34b268b` handles net id `0xb3`: maps the user id, gets the achievements manager, calls `mgr->vtable[0xb8]` per id. | `[DISASM]` |
| `0x5078630` | manager `UnlockAchievement` | Returns if `mgr+0x84 == 0` (Steam stats not received yet), returns if already unlocked in its own cache, else calls the Steam wrapper. | `[DISASM]` |
| `0x6aa1170` | Steam wrapper | `ISteamUserStats::SetAchievement(def->name)` (vtable `+0x38`) and `StoreStats` (`+0x50`). | `[DISASM]` `[LIVE]` |
| process | topology | Two `bg3` PIDs, a real `fork()`. Game logic that touches `gEocServer` runs in the child. The child writes to the parent-named `bg3le.log.<parentpid>` through the inherited `FILE*`, so "the parent's log" is the shared log. Attach gdb to the child. | `[LIVE]` |

Address convention: `FUN_0xxxxxxx` labels are raw VMA + `0x100000` (the
convention the analysis was recorded in). Patching code uses raw VMA plus
the load bias, as `src/hook.cpp` does.

### The full chain, server to Steam

1. Server, Osiris native `UnlockAchievement` (`0x3767780`): check #1, then
   the GUID-to-entity helper (`0x388e8e0`), then `0x3767800`.
2. `0x3767800`: check #2, then the achievement lookup, then `0x3767890`.
3. `0x3767890`: sends `eocnet::AchievementMessage`, net id `0xb3`, to the
   character's user peer.
4. Client `ProcessMsg` (`0x34b2440`) dispatches it to the achievements
   manager.
5. Manager `UnlockAchievement` (`0x5078630`) calls the Steam wrapper
   (`0x6aa1170`), which calls `SetAchievement` and `StoreStats`.

The client side never calls the predicate, the loop, or reads `+0x87` or
`+0x828`. There is no client-side mod check; the only client gate is
`mgr+0x84` (Steam stats loaded). The Steam call is asynchronous to the
Osiris handler and crosses from the server process to the client, which is
why forward-searching from the handler for "the branch" kept failing.

### Coverage of the IsModded patch

Opens checks #1 and #2 and every loop-based consumer, including both cached
"modded" bytes. Not covered: the load-status reporter's inlined loop in
`0x2c2cb20`, which calls the predicate directly (at `0x2c2cc44`), so the Load
Game "modded" badge may show again; and two inlined copies of the
official-module compare, `0x2c27ae0` (by-value variant) and `0x2e06590`
(module-settings validation), which affect load validation, not the unlock.
Re-run the badge test from "Live validation" before patching any of them,
and never patch the predicate itself.

## History: why a bypass came first

Before the predicate was found, an earlier version hooked Osiris' DIV
dispatch for `UnlockAchievement` (id `0x80001669`), walked the argument
list for the achievement name and called `ISteamUserStats::SetAchievement`
and `StoreStats` directly, with `BG3LE_ACHIEVEMENTS=0` turning it off. It
worked `[LIVE]` but was a bypass of the check rather than the check itself:
it did nothing for the Load Game badges, it depended on the Steam vtable
hook being installed before the first unlock, and it re-implemented the
client manager's own already-unlocked bookkeeping. Once the predicate was
mapped, the bypass and its forced Steam calls were removed.

An intermediate attempt NOP'd only check #1 (the `jne` at `0x37677a3`) and
saw no change, because check #2 in `0x3767800` sits two calls later. A
second contributor to that false negative: the client manager caches
"already unlocked" at startup, so clearing an achievement with the game
running makes the engine path skip silently while a direct Steam call still
shows a toast. Always clear achievements with the game closed.

## Live validation (2026-09-23)

With mods active and the predicate patched, killing the Adamantine Golem
unlocked `BG3_Quest33` with a real Steam toast, and the "no achievements"
and "modded" badges on the Load Game list disappeared. The `SetAchievement`
backtrace matched the mapped chain exactly: Steam wrapper `0x6aa119c`,
manager `0x50786ef`, client `ProcessMsg` `0x34b267f`. The control run with
the patch off showed the badges and produced no `SetAchievement` line.

## Updating for a new binary

When a game update moves the code, the patch refuses itself (the
`WARNING: ... refused` log line) and achievements simply stay mod-blocked.
To re-target: find the function that formats a module GUID with
`%08x-%04hx-%04hx-...` and compares the resulting FixedString index against
a run of 19 `.bss` globals; PREDICATE-ANALYSIS.md lists the globals, the
FixedString constructor and all callers of the current build to cross-check
against. The loop that calls it over `list+8` (count at `list+0x14`, stride
`0x60`) is `HasCustomMods`. Update `kIsModded` and `kIsModdedBytes` in
`src/preload.cpp`, run the A/B protocol in the runbook, and check that a new
game's character creation still lists modded classes.

## Known limitations

- A pre-existing, unrelated bg3le bug crashes when `Osi.UnlockAchievement`
  is called on an **already-earned** achievement (a `COsiArgumentDesc`
  marshalling bug in `src/osi.cpp`, root cause still unknown).
- The two inlined copies of the official-module compare are left unpatched
  (see "Coverage" above).

## Lessons

- Never label a function until it is decompiled with clean noreturn flags;
  bogus `noreturn` on `strlen` and the FixedString constructor made the
  predicate look like a void function for several sessions.
- Every conclusion needs an A/B: mods off and mods on, same PID, same
  trigger.
- Attach to the child PID, verified with `PPid` in `/proc/<pid>/status`;
  log lines are shared between both processes.
- `LD_PRELOAD` reaches every process in the launch chain. Check the host
  binary and the target range before writing to code.
