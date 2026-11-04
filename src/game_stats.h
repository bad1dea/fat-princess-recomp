#pragma once
// Progress toward the counting trophies, read from the game's own statistics. The game keeps every counter in its score
// manager (notes/multiplayer-ranking.md); each trophy's rule is "unlock <Stat> >= <N>" in data/gamescripts/trophies.txt.
// The counters are read by running the game's getters on the guest thread, triggered through the console (see
// fp_player_control_command), so values are as fresh as the last request.
#include <string>

namespace fp {
void game_stats_request();                  // ask for a refresh (any thread); the next frame of the game answers
bool game_stats_take_request();
unsigned game_stats_generation();           // changes whenever new counters arrive             // guest side: true once per request
struct TrophyProgress { int value = 0, target = 0; std::string unit; };
bool game_stats_progress(int trophy_id, TrophyProgress& out);   // false: no counter for this trophy, or not read yet
}
