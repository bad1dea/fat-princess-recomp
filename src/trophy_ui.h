#pragma once
#include <string>
#include <cstdint>
#include <vector>

namespace fp {
bool trophy_ui_active();
// detail_id >= 0 opens that trophy's details, -1 the list. False when no trophy set is registered yet.
bool trophy_ui_open(int detail_id);
// The details of the trophy whose toast is up (or was in the last 15 s), otherwise the list.
bool trophy_ui_open_recent();
void trophy_ui_close();
void trophy_ui_note_toast(int id);
std::string trophy_ui_detail(int id);   // the description of a trophy ("" if unknown)
// Menu input while a screen is open; returns true when the screen changed.
bool trophy_ui_nav(bool up, bool down, bool ok, bool back, bool left = false, bool right = false);
bool trophy_ui_take_dirty();
std::vector<uint8_t> trophy_ui_pixels(float scale, bool controller, unsigned& w, unsigned& h, unsigned& radius);
void trophy_chime();
}
