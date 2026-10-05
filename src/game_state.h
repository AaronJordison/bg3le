#pragma once

namespace bg3le {

// Hooks the client's game state queue; installed before the game starts.
void install_game_state_hook();
// Wraps the stats parser's call sites; each parse completes one owed
// StatsLoaded fire, in every context (see stats_parse_hook in game_state.cpp).
void install_stats_parse_hook();

// The client state most recently queued, or null before the first.
char const* client_game_state();

}  // namespace bg3le
