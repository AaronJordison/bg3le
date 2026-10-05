#pragma once

// Materialise mods' stats files loose under the game's Data tree.
//
// The engine reads COMPRESSED mod-pak stats entries raw (ledger
// 2026-10-05): it hands the stats parser the stored bytes, so every data
// line in a compressed entry is lost while the entry names survive — the
// mod's statuses become generic. The base game's own compressed stats
// parse fine; only mod archives hit this. A loose file under Data wins
// the engine's own preference, and loose files parse.
//
// Runs once from bg3le_init, before the engine builds its file caches.

namespace bg3le {

// For every mod in the player's modsettings.lsx, write each of its own
// Public/<folder>/Stats/Generated/**.txt entries (decompressed) to the
// same relative path under Data. Refreshes every launch so a mod update
// is followed; prunes files whose mod went away (manifest next to the
// Data paks). BG3LE_STATS_MIRROR=0 disables.
void mirror_mod_stats();

}  // namespace bg3le
