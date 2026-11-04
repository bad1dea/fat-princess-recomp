#pragma once
// The trophy book as a native game menu (Bragging Rights > Trophies): the menu scripts (tools/make_menu_patch.py) show
// symbols, this fills them. Called on the guest thread from `echo fp trophy ...` (menu_bridge.cpp).
#include <string>
#include <utility>
#include <vector>

namespace fp {
using SymbolReplies = std::vector<std::pair<std::string, std::string>>;
// args: {"open"}, {"page", "+1"|"-1"}, {"focus", "<row 0-7>"}. Appends the symbols the page needs.
void trophy_book_command(const std::vector<std::string>& args, SymbolReplies& out);
constexpr int kBookRows = 8;
}
