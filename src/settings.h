#pragma once
// In-app settings: loaded from an ini file at startup (before the runtime reads its environment),
// edited through an overlay menu (F1, or Select+Start on a controller) and saved on close.
#include <cstdint>
#include <string>

namespace fp {
// Overlay is drawn as a bitmap into the backend's debug overlay slot. Returns true while the menu is open.
bool settings_menu_frame(std::string& panel_text);
bool settings_menu_open();
bool settings_controller_hints();   // the last menu input came from a controller (prompt wording)
std::string settings_prompt_text(std::string text);   // current touch/controller/keyboard labels
// The game's text entry (cellOskDialog) is on screen.
bool settings_osk_active();
void settings_menu_toggle();
bool settings_quit_requested();   // QUIT GAME was confirmed in the menu
// Keyboard scancode (SDL_Scancode value) bound to a pad button name ("cross", "start", ...).
int settings_key(const char* button);
// Live values read by the host loop.
bool settings_renderer_d3d12();   // Windows only: FP_GFX=d3d12 or the RENDERER setting
int settings_interp();            // 1 = off, 2 or 4 frames shown per guest frame
float settings_frame_lock();      // QrkGfx_FrameRateLock to run with, 0 = time based
double settings_fps_cap();      // frames per second, 0 = unlimited
bool settings_stats_visible();
std::string settings_title_suffix();   // " | FRIEND REQUEST FROM X ..." while one waits, else empty
void settings_stats_set(bool on);
bool settings_set(const char* id, const std::string& value);   // as if changed in the menu; false if unknown or unavailable
bool settings_action(const char* id);   // as if the DEBUG action row were activated; false if not an action
bool settings_text_entry(const char* id);   // open the keyboard for a text item of the ACCOUNT page; the result lands in the item
bool settings_text_active();
bool settings_text_set(const char* id, const std::string& value);
bool settings_command(const char* id);   // cmd_login, cmd_register, cmd_logout, as if chosen on the ACCOUNT page
std::string settings_account();          // signed-in account name, empty when signed out
std::string settings_notice();           // last result line of an account request
bool settings_busy();                    // an account or friends request is running
bool settings_open_url(const char* id, const std::string& query = std::string());  // open the https address stored in a text item (plus a query) in the browser
std::string settings_get(const char* id);   // current value text of an item, empty if unknown   // F3 toggles through this so the menu shows the same state
}
