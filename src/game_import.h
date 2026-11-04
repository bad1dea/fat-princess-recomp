// First-start setup from the player's own package and licence files (see game_import.cpp).
#pragma once
#include <filesystem>
#include <string>

bool fp_import_possible(const std::filesystem::path& dir);               // an import folder with a package is there
bool fp_import_game(const std::filesystem::path& dir, std::string& error);   // unpack, decrypt, write key files; false and a plain message on failure
