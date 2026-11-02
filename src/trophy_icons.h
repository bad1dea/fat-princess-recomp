#pragma once
// The trophy icons as game textures, so the native trophy book can draw them: every TROPnnn.PNG of the trophy set
// (TROPDIR/<set>/TROPHY.TRP next to USRDIR) becomes data/textures/menus/fptrophyNN.gtf (colour) and fptrophygNN.gtf
// (dimmed, for trophies not earned yet), 128x128 DXT5, added to the start-up patch archive.
#include <string>
#include <utility>
#include <vector>

namespace fp {
// game_dir is the USRDIR folder. Empty when the trophy file is missing.
std::vector<std::pair<std::string, std::string>> trophy_icon_textures(const std::string& game_dir);
}
