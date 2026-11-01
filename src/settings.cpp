#include "settings.h"
#include "game_controls.h"
#include "ai_route_policy.h"
#include "osk_panel.h"
#include "self_test.h"
#include "trophy_ui.h"
#include <SDL2/SDL.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
#include "ios_touch.h"
#endif
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <functional>
#include <future>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define mkdir(path, mode) _mkdir(path)
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

extern "C" int fp_gfx_is_d3d12(void);
extern "C" const unsigned char* fp_menu_keyboard_state(int*);
extern "C" void fp_backend_set_fullscreen(int on);
extern "C" int fp_backend_set_output_size(uint32_t width, uint32_t height);
extern "C" void rsx_vulkan_backend_set_render_scale(float scale, int fsr, float sharpness);
extern "C" void rsx_vulkan_backend_set_overlay_opacity(float opacity);
extern "C" void rsx_vulkan_backend_set_present_mode(const char* mode);
extern "C" void rsx_vulkan_backend_set_async(int on);
extern "C" void rsx_vulkan_backend_set_hitch_ms(double ms);
extern "C" void rsx_vulkan_backend_set_disk_cache(int on);
extern "C" void fp_pad_set_local_players(int n);
extern "C" {
#include "../third_party/ps3recomp/libs/network/np_psnr.h"
}
extern "C" float g_audio_gain;
extern "C" int g_audio_limiter;
extern "C" int g_voice_enabled, g_voice_volume, g_voice_mic_gain, g_voice_ptt;
extern "C" volatile int g_fp_pad_blocked;

namespace {
enum class Kind { Toggle, Choice, Text, Secret, Key, Action, Command, Submenu, Runtime };

struct Item {
    const char* tab;
    const char* id;
    const char* label;
    Kind kind;
    std::string def;
    std::vector<std::string> choices;
    bool restart;
    std::string value;
};

// FP_DEBUG_MENU (CMake option, off for release packages) adds a DEBUG tab for testing.
#ifdef FP_DEBUG_MENU
const char* const kTabs[] = {"GAME", "VIDEO", "AUDIO", "CONTROLS", "ONLINE", "TOOLS", "DEBUG"};
constexpr int kTabCount = 7;
extern "C" int sceNpTrophyPreviewUnlock(int trophyId);
extern "C" const char* sceNpTrophyPreviewName(int trophyId);
#else
const char* const kTabs[] = {"GAME", "VIDEO", "AUDIO", "CONTROLS", "ONLINE", "TOOLS"};
constexpr int kTabCount = 6;
#endif

std::vector<Item>& menu_items() { static std::vector<Item> values; return values; }
bool is_open = false;
bool dirty = true;
int tab = 0;
std::string subpage;
std::map<std::string, int> rows;
std::map<std::string, std::string>& startup_settings() { static std::map<std::string, std::string> values; return values; }
bool controller_hints = false;
std::string confirm_title;
std::function<void()> confirm_action;
bool confirm_yes = false;
bool editing = false;
std::string edit_buffer;
bool capturing = false;
std::string notice;
bool restart_needed = false;
std::string& config_file() { static std::string path; return path; }

std::string page_id() { return subpage.empty() ? kTabs[tab] : subpage; }
bool friends_page() { return page_id() == "ONLINE/FRIENDS"; }
int& selected_row() { return rows[page_id()]; }
void confirm(const std::string& title, std::function<void()> action) {
    confirm_title = title; confirm_action = std::move(action); confirm_yes = false; dirty = true;
}

Item* find(const char* id) {
    for (auto& i : menu_items()) if (!std::strcmp(i.id, id)) return &i;
    return nullptr;
}

void add(const char* tab_name, const char* id, const char* label, Kind kind, std::string def,
         std::vector<std::string> choices = {}, bool restart = false) {
    menu_items().push_back({tab_name, id, label, kind, def, std::move(choices), restart, def});
}

// Online names must be unique per player: the login name plus a short hash of the machine name.
std::string default_player_name() {
    const char* user = std::getenv("USERNAME");
    if (!user) user = std::getenv("USER");
    const char* host = std::getenv("COMPUTERNAME");
    if (!host) host = std::getenv("HOSTNAME");
    std::string base = user && *user ? user : "Player";
    unsigned h = 2166136261u;
    for (const char* p = host ? host : ""; *p; ++p) h = (h ^ static_cast<unsigned char>(*p)) * 16777619u;
    for (const char* p = user ? user : ""; *p; ++p) h = (h ^ static_cast<unsigned char>(*p)) * 16777619u;
    char tail[8];
    std::snprintf(tail, sizeof tail, "-%03x", h & 0xFFF);
    return base.substr(0, 11) + tail;
}

// A friend request, a new friendship or a removal arrived from the server. This runs on the guest thread that pumps
// psnr, so it only leaves a note for the menu thread.
std::atomic<int> unseen_requests{0};
std::atomic<bool> friends_stale{true};
std::mutex push_mutex;
std::string push_text;   // for the window title and the menu notice

void on_friend_push(uint8_t type, const char* name) {
    std::lock_guard<std::mutex> lock(push_mutex);
    if (type == PSNR_FRIEND_REQ_PUSH) { ++unseen_requests; push_text = std::string("FRIEND REQUEST FROM ") + name; }
    else if (type == PSNR_FRIEND_ADDED_PUSH) push_text = std::string(name) + " IS NOW YOUR FRIEND";
    else push_text = std::string(name) + " REMOVED YOU";
    friends_stale = true;
}

void init_items() {
    if (!menu_items().empty()) return;
    add("VIDEO", "fps_cap", "FRAME LIMIT", Kind::Choice, "AUTO", {"AUTO", "30", "45", "60", "90", "120", "UNLIMITED"});
    add("VIDEO", "resolution", "OUTPUT RESOLUTION", Kind::Choice, "720P", {"720P", "1080P"});
    add("VIDEO", "render_scale", "RENDER SCALE (VULKAN)", Kind::Choice, "1X", {"0.5X", "0.67X", "0.75X", "1X", "1.5X", "2X", "3X"});
    add("VIDEO", "upscaler", "UPSCALER (UNDER 1X)", Kind::Choice, "FSR 1.0", {"BILINEAR", "FSR 1.0"});
    add("VIDEO", "fsr_sharp", "FSR SHARPNESS", Kind::Choice, "MEDIUM", {"SOFT", "MEDIUM", "SHARP"});
    add("VIDEO", "fullscreen", "FULLSCREEN (ALT+ENTER)", Kind::Toggle, "OFF");
#ifdef _WIN32
    add("VIDEO", "renderer", "RENDERER", Kind::Choice, "VULKAN", {"VULKAN", "D3D12"}, true);
#endif
    add("VIDEO", "interp", "INTERPOLATION (TEST)", Kind::Choice, "OFF", {"OFF", "2X", "4X"});
    add("VIDEO", "vsync", "VSYNC", Kind::Choice, "ON", {"ON", "ADAPTIVE", "OFF"});
#if defined(__APPLE__) && TARGET_OS_IOS
    add("VIDEO", "stats", "FPS PANEL", Kind::Choice, "OFF", {"OFF", "COMPACT", "DETAILED"});   // too much for a phone screen
#else
    add("VIDEO", "stats", "FPS PANEL (F3)", Kind::Choice, "DETAILED", {"OFF", "COMPACT", "DETAILED"});
#endif
    add("VIDEO", "overlay_opacity", "PANEL OPACITY %", Kind::Choice, "70", {"100", "85", "70", "50", "35", "20"});
    add("VIDEO", "async", "ASYNC GPU SUBMIT", Kind::Toggle, "ON");
    add("AUDIO", "volume", "VOLUME", Kind::Choice, "100", {"0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100"});
    add("AUDIO", "mute", "MUTE", Kind::Toggle, "OFF");
    add("AUDIO", "limiter", "LOUD MIX LIMITER", Kind::Toggle, "ON");
    add("AUDIO", "voice", "VOICE CHAT", Kind::Toggle, "ON");
    add("AUDIO", "voice_volume", "VOICE CHAT VOLUME %", Kind::Choice, "100", {"0", "25", "50", "75", "100", "125", "150", "200", "300"});
    add("AUDIO", "mic_gain", "MICROPHONE GAIN %", Kind::Choice, "100", {"25", "50", "75", "100", "150", "200", "300", "400"});
    add("NETWORK", "online", "ONLINE (PSNR)", Kind::Toggle, "ON");
#ifndef FP_DEFAULT_SERVER
#define FP_DEFAULT_SERVER "127.0.0.1:36100"   // builds may set their own (-DFP_DEFAULT_SERVER=...)
#endif
    add("NETWORK", "server", "SERVER HOST:PORT", Kind::Text, FP_DEFAULT_SERVER);
    add("NETWORK", "name", "ONLINE NAME", Kind::Text, default_player_name(), {}, true);
    add("NETWORK", "p2p_port", "P2P PORT", Kind::Text, "3658");
    // The signed in account. Not on any tab: the menu shows its state on ACCOUNT, the ini keeps it.
    add("", "account_user", "", Kind::Text, "");
    add("", "account_token", "", Kind::Text, "");
    add("ACCOUNT", "acct_status", "STATUS", Kind::Command, "");
    add("ACCOUNT", "tmp_name", "ACCOUNT NAME", Kind::Text, "");
    add("ACCOUNT", "tmp_pw", "PASSWORD", Kind::Secret, "");
    add("ACCOUNT", "tmp_email", "EMAIL (OPTIONAL)", Kind::Text, "");
    add("ACCOUNT", "cmd_register", "REGISTER", Kind::Command, "");
    add("ACCOUNT", "cmd_login", "SIGN IN", Kind::Command, "");
    add("ACCOUNT", "cmd_logout", "SIGN OUT", Kind::Command, "");
    add("ACCOUNT", "tmp_newpw", "NEW PASSWORD", Kind::Secret, "");
    add("ACCOUNT", "cmd_changepw", "CHANGE PASSWORD", Kind::Command, "");
    np_psnr_on_friend_push(on_friend_push);
    static const struct { const char* id; const char* label; const char* key; } keys[] = {
        {"key_cross", "CROSS (CONFIRM)", "Z"}, {"key_circle", "CIRCLE (BACK)", "X"},
        {"key_square", "SQUARE", "A"}, {"key_triangle", "TRIANGLE", "S"},
        {"key_start", "START", "Return"}, {"key_select", "SELECT", "Tab"},
        {"key_l1", "L1", "Q"}, {"key_r1", "R1", "W"}, {"key_l2", "L2", "5"}, {"key_r2", "R2", "6"},
        {"key_l3", "L3", "E"}, {"key_r3", "R3", "R"},
        {"key_up", "UP", "Up"}, {"key_down", "DOWN", "Down"}, {"key_left", "LEFT", "Left"}, {"key_right", "RIGHT", "Right"},
    };
    for (auto& k : keys) add("CONTROLS", k.id, k.label, Kind::Key, k.key);
    add("GAME", "skip_intro", "SKIP LOGOS AND LICENSE SCREEN", Kind::Toggle, "ON", {}, true);
    add("GAME", "skip_howto", "SKIP THE NEW PLAYER PROMPT", Kind::Toggle, "ON", {}, true);
    add("GAME", "skip_title", "SKIP THE PRESS START SCREEN", Kind::Toggle, "OFF", {}, true);
    add("AUDIO", "mic_mode", "MICROPHONE", Kind::Choice, "PUSH TO TALK", {"PUSH TO TALK", "OPEN MIC"});
    add("GAME", "name_font", "NAME TAG FONT", Kind::Choice, "ORIGINAL", {"ORIGINAL", "FATFINGERS", "FATFINGERS OUTLINED", "BOOPEE", "TOON"});
    add("GAME", "speed", "SIMULATION SPEED %", Kind::Choice, "100", {"50", "75", "100", "125", "150", "175", "200"});
    add("DEBUG", "debug_bindings", "DEBUG CHEAT COMBOS", Kind::Toggle, "ON", {}, true);
    add("DEBUG", "hitch_log", "LOG SLOW ENGINE OPS (MS)", Kind::Choice, "OFF", {"OFF", "2", "5", "10", "20"});
    add("DEBUG", "no_shader_cache", "DISABLE SHADER CACHE", Kind::Toggle, "OFF");
    // Actions hold the controller combo of the game's own debug bindings (tools/make_cheat_patch.py); the value is the
    // CELL_PAD_CTRL mask. They need the cheat patch installed and a match running.
    static const struct { const char* id; const char* label; const char* mask; } actions[] = {
        {"act_god_on", "GOD MODE ON", "0x8C00"}, {"act_god_off", "GOD MODE OFF", "0x8006"},
        {"act_dmg10", "DAMAGE GIVEN X10", "0x1C00"}, {"act_dmg1", "DAMAGE GIVEN X1", "0x1006"},
        {"act_metal_add", "ADD METAL", "0x0480"}, {"act_metal_sub", "REMOVE METAL", "0x04A0"},
        {"act_wood_add", "ADD WOOD", "0x0880"}, {"act_wood_sub", "REMOVE WOOD", "0x08A0"},
        {"act_both_add", "ADD METAL AND WOOD", "0x0C80"}, {"act_both_sub", "REMOVE METAL AND WOOD", "0x0CA0"},
        {"act_warrior", "CLASS: WARRIOR", "0x0012"}, {"act_ranger", "CLASS: RANGER", "0x0082"},
        {"act_mage", "CLASS: MAGE", "0x0042"}, {"act_worker", "CLASS: WORKER", "0x0022"},
        {"act_death", "CLASS: DEATH", "0x0014"}, {"act_priest", "CLASS: PRIEST", "0x0024"},
        {"act_villager", "CLASS: VILLAGER", "0x0044"}, {"act_chicken", "CLASS: CHICKEN", "0x0084"},
        {"act_giant", "CLASS: GIANT", "0x4006"},
        {"act_freecam", "FREE CAMERA (NO PAD CONTROL)", "0x0C10"},
    };
    for (auto& a : actions) add("DEBUG", a.id, a.label, Kind::Action, a.mask);
    add("GAME", "timing", "GAME SPEED", Kind::Choice, "30 FPS LOCK", {"30 FPS LOCK", "60 FPS LOCK", "TIME BASED"});
    add("GAME", "local_players", "LOCAL PLAYERS", Kind::Choice, "4", {"1", "2", "3", "4"});
    add("GAME", "ui_scale", "MENU TEXT SIZE", Kind::Choice, "STANDARD", {"STANDARD", "LARGE"});
    add("GAME", "trophies", "TROPHIES", Kind::Command, "");
    add("AUDIO", "trophy_sound", "TROPHY SOUND", Kind::Toggle, "ON");
    add("CONTROLS", "menu_button", "OPEN SETTINGS", Kind::Choice, "SELECT+START", {"SELECT+START", "SELECT", "L3", "R3"});
    add("CONTROLS", "button_prompts", "BUTTON PROMPTS", Kind::Choice, "AUTO", {"AUTO", "PLAYSTATION", "XBOX", "NINTENDO"});
    add("TOOLS/SELFTEST", "test_scenario", "SCENARIO", Kind::Choice, "AI SOAK", {"AI SOAK", "CONTROL HANDOFF", "WATER SWEEP", "CLASS CHANGES"});
    add("TOOLS/SELFTEST", "test_duration", "DURATION (SECONDS)", Kind::Choice, "300", {"60", "120", "300", "900", "1800"});
    add("TOOLS/SELFTEST", "test_start", "START SELF-TEST", Kind::Command, "");
    add("TOOLS/SELFTEST", "test_report", "OPEN REPORT", Kind::Command, "");
    add("TOOLS/SELFTEST", "test_stop", "STOP SELF-TEST", Kind::Command, "");
    add("TOOLS", "ai_control", "PLAYER CONTROL", Kind::Runtime, "OFF");
    add("TOOLS/AI", "ai_profile", "AI PROFILE", Kind::Choice, "ORIGINAL", {"ORIGINAL", "EASY", "MEDIUM", "HARD", "ADVANCED", "CUSTOM"});
    add("TOOLS/AI", "ai_nav_guard", "HAZARD AVOIDANCE (TEST)", Kind::Toggle, "OFF");
    add("TOOLS/AI", "ai_recheck", "CUSTOM DECISION PERIOD (S)", Kind::Choice, "1.0", {"0.25", "0.5", "1.0", "1.5", "2.0", "3.0"});
    add("TOOLS/AI", "ai_lock_on", "CUSTOM LOCK-ON CHANCE %", Kind::Choice, "90", {"0", "25", "50", "75", "90", "100"});
    static const struct { const char* id; const char* label; const char* page; const char* parent; } links[] = {
        {"page_ai", "AI SETTINGS", "TOOLS/AI", "TOOLS"},
        {"page_selftest", "SELF-TEST", "TOOLS/SELFTEST", "TOOLS"},
        {"page_menu", "MENU AND ACCESSIBILITY", "CONTROLS/MENU", "CONTROLS"},
        {"page_startup", "STARTUP", "GAME/STARTUP", "GAME"},
        {"page_quality", "QUALITY", "VIDEO/QUALITY", "VIDEO"},
        {"page_video_advanced", "ADVANCED", "VIDEO/ADVANCED", "VIDEO"},
        {"page_voice", "VOICE CHAT", "AUDIO/VOICE", "AUDIO"},
        {"page_movement", "MOVEMENT KEYS", "CONTROLS/MOVEMENT", "CONTROLS"},
        {"page_shoulders", "SHOULDER AND STICK KEYS", "CONTROLS/SHOULDERS", "CONTROLS"},
        {"page_account", "ACCOUNT", "ONLINE/ACCOUNT", "ONLINE"},
        {"page_friends", "FRIENDS", "ONLINE/FRIENDS", "ONLINE"},
        {"page_connection", "CONNECTION", "ONLINE/CONNECTION", "ONLINE"},
        {"page_cheats", "PLAYER CHEATS", "TOOLS/CHEATS", "TOOLS"},
        {"page_classes", "CHANGE CLASS", "TOOLS/CLASSES", "TOOLS"},
        {"page_resources", "RESOURCES", "TOOLS/RESOURCES", "TOOLS"},
        {"page_simulation", "SIMULATION", "TOOLS/SIMULATION", "TOOLS"},
        {"page_diagnostics", "DIAGNOSTICS", "TOOLS/DIAGNOSTICS", "TOOLS"},
    };
    for (const auto& link : links) add(link.parent, link.id, link.label, Kind::Submenu, link.page);
    static const struct { const char* page; std::initializer_list<const char*> ids; } pages[] = {
        {"GAME", {"trophies", "local_players", "name_font", "ui_scale", "overlay_opacity", "page_startup"}},
        {"GAME/STARTUP", {"skip_intro", "skip_howto", "skip_title"}},
        {"VIDEO", {"fullscreen", "resolution", "vsync", "fps_cap", "page_quality", "page_video_advanced"}},
        {"VIDEO/QUALITY", {"render_scale", "upscaler", "fsr_sharp"}},
        {"VIDEO/ADVANCED", {"renderer", "interp", "async"}},
        {"AUDIO", {"volume", "mute", "limiter", "trophy_sound", "page_voice"}},
        {"AUDIO/VOICE", {"voice", "voice_volume", "mic_mode", "mic_gain"}},
        {"CONTROLS", {"key_cross", "key_circle", "key_square", "key_triangle", "key_start", "key_select", "page_movement", "page_shoulders", "page_menu"}},
        {"CONTROLS/MENU", {"menu_button", "button_prompts"}},
        {"CONTROLS/MOVEMENT", {"key_up", "key_down", "key_left", "key_right"}},
        {"CONTROLS/SHOULDERS", {"key_l1", "key_r1", "key_l2", "key_r2", "key_l3", "key_r3"}},
        {"ONLINE", {"online", "name", "page_account", "page_friends", "page_connection"}},
        {"ONLINE/CONNECTION", {"server", "p2p_port"}},
        {"ONLINE/ACCOUNT", {"acct_status", "tmp_name", "tmp_pw", "cmd_login", "tmp_email", "cmd_register", "cmd_logout", "tmp_newpw", "cmd_changepw"}},
        {"TOOLS/SELFTEST", {"test_scenario", "test_duration", "test_start", "test_stop", "test_report"}},
        {"TOOLS", {"ai_control", "page_ai", "page_selftest", "act_freecam", "page_cheats", "page_classes", "page_resources", "page_simulation", "page_diagnostics"}},
        {"TOOLS/AI", {"ai_profile", "ai_nav_guard", "ai_recheck", "ai_lock_on"}},
        {"TOOLS/CHEATS", {"debug_bindings", "act_god_on", "act_god_off", "act_dmg10", "act_dmg1"}},
        {"TOOLS/CLASSES", {"act_warrior", "act_ranger", "act_mage", "act_worker", "act_death", "act_priest", "act_villager", "act_chicken", "act_giant"}},
        {"TOOLS/RESOURCES", {"act_metal_add", "act_metal_sub", "act_wood_add", "act_wood_sub", "act_both_add", "act_both_sub"}},
        {"TOOLS/SIMULATION", {"timing", "speed"}},
        {"TOOLS/DIAGNOSTICS", {"stats", "hitch_log", "no_shader_cache"}},
    };
#ifdef FP_DEBUG_MENU
    {
        std::vector<std::string> ids;
        for (int i = 0; i < 27; ++i) ids.push_back(std::to_string(i));
        add("DEBUG", "dbg_trophy_id", "PREVIEW TROPHY NUMBER", Kind::Choice, "0", ids);
        add("DEBUG", "dbg_trophy_show", "SHOW TROPHY CARD (NO UNLOCK)", Kind::Command, "");
        add("DEBUG", "dbg_trophy_all", "SHOW EVERY TROPHY CARD (NO UNLOCK)", Kind::Command, "");
        static const struct { const char* id; const char* label; const char* page; } debug_links[] = {
            {"dbg_page_selftest", "SELF-TEST", "TOOLS/SELFTEST"}, {"dbg_page_cheats", "PLAYER CHEATS", "TOOLS/CHEATS"},
            {"dbg_page_classes", "CHANGE CLASS", "TOOLS/CLASSES"}, {"dbg_page_resources", "RESOURCES", "TOOLS/RESOURCES"},
            {"dbg_page_simulation", "SIMULATION", "TOOLS/SIMULATION"}, {"dbg_page_diagnostics", "DIAGNOSTICS", "TOOLS/DIAGNOSTICS"},
        };
        for (const auto& link : debug_links) add("DEBUG", link.id, link.label, Kind::Submenu, link.page);
    }
#endif
    std::map<std::string, int> order;
    int rank = 0;
    for (const auto& page : pages) for (const auto* id : page.ids)
        if (Item* it = find(id)) { it->tab = page.page; order[id] = rank++; }
    std::stable_sort(menu_items().begin(), menu_items().end(), [&](const Item& a, const Item& b) {
        return (order.count(a.id) ? order[a.id] : 10000) < (order.count(b.id) ? order[b.id] : 10000);
    });
    find("act_freecam")->label = "FREE CAMERA";
    find("online")->label = "ONLINE PLAY";
    find("server")->label = "SERVER ADDRESS";
    find("volume")->label = "GAME VOLUME";
}

std::string default_path() {
    if (const char* p = std::getenv("FP_CONFIG")) return p;
    std::string dir;
    if (const char* x = std::getenv("XDG_CONFIG_HOME")) dir = x;
    else if (const char* h = std::getenv("HOME")) dir = std::string(h) + "/.config";
    else dir = ".";
    dir += "/fat-princess-native";
    mkdir(dir.c_str(), 0755);
    return dir + "/settings.ini";
}

void save() {
    std::ofstream out(config_file(), std::ios::trunc);
    for (auto& i : menu_items())
        if (i.kind != Kind::Action && i.kind != Kind::Command && i.kind != Kind::Submenu && i.kind != Kind::Runtime && std::strncmp(i.id, "tmp_", 4)) out << i.id << '=' << i.value << '\n';
}

void load() {
    config_file() = default_path();
    std::ifstream in(config_file());
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (Item* i = find(line.substr(0, eq).c_str()))
            if (i->kind != Kind::Submenu && i->kind != Kind::Runtime) i->value = line.substr(eq + 1);
    }
    if (find("stats")->value=="ON") find("stats")->value="DETAILED";
    if (find("server")->value=="127.0.0.1:36100") find("server")->value=FP_DEFAULT_SERVER;   // the old loopback default
    if (Item* a = find("account_user")) if (!a->value.empty()) find("tmp_name")->value = a->value;
}

bool on(const char* id) { Item* i = find(id); return i && i->value == "ON"; }
bool on_value(const char* id, const char* v) { Item* i = find(id); return i && i->value == v; }

extern "C" void fp_pad_inject(unsigned mask, unsigned hold_ms);

void apply_live() {
    if (find("ai_profile")) fp::player_ai_configure(find("ai_profile")->value,
        float(std::atof(find("ai_recheck")->value.c_str())), unsigned(std::atoi(find("ai_lock_on")->value.c_str())));
    if (Item* v = find("volume")) {
        const int pct = std::atoi(v->value.c_str());
        g_audio_gain = on("mute") ? 0.0f : static_cast<float>(pct) / 100.0f;
    }
    if (Item* rs = find("render_scale")) if (!fp_gfx_is_d3d12()) {
        const std::string sh = find("fsr_sharp") ? find("fsr_sharp")->value : "MEDIUM";
        // Guest video buffers keep their boot-time dimensions. Scale display-sized
        // render targets relative to them so 1X follows the selected output size.
        const char* native_res = std::getenv("PS3_VIDEO_RES");
        const float native_height = native_res && !std::strcmp(native_res, "1080") ? 1080.f : 720.f;
        const float output_height = on_value("resolution", "1080P") ? 1080.f : 720.f;
        rsx_vulkan_backend_set_render_scale((float)std::atof(rs->value.c_str()) * output_height / native_height, on_value("upscaler", "FSR 1.0"),
                                            sh == "SOFT" ? 1.0f : sh == "SHARP" ? 0.0f : 0.25f);
    }
    if (Item* o = find("overlay_opacity")) if (!fp_gfx_is_d3d12()) rsx_vulkan_backend_set_overlay_opacity((float)std::atoi(o->value.c_str()) / 100.0f);
    if (find("limiter")) g_audio_limiter = on("limiter") ? 1 : 0;
    if (find("mic_mode")) g_voice_ptt = find("mic_mode")->value == "OPEN MIC" ? 0 : 1;
    if (find("voice")) g_voice_enabled = on("voice") ? 1 : 0;
    if (Item* v = find("voice_volume")) g_voice_volume = std::atoi(v->value.c_str());
    if (Item* v = find("mic_gain")) g_voice_mic_gain = std::atoi(v->value.c_str());
}

bool apply_resolution() {
    const bool hd = on_value("resolution", "1080P");
    return fp_backend_set_output_size(hd ? 1920 : 1280, hd ? 1080 : 720) == 0;
}

bool pending_fullscreen = false;
void apply_fullscreen() { fp_backend_set_fullscreen(on("fullscreen") ? 1 : 0); }

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}
void clear_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

// Network values are read from the environment when the psnr connection is made, so a change drops the
// connection and the next ContextStart (entering the online menu) connects with the new values.
void apply_network() {
    if (on("online")) {
        set_env("PS3_NET_ONLINE", "1");
        set_env("PSNR_SERVER", find("server")->value);
        set_env("PS3_NET_P2P_PORT", find("p2p_port")->value);
        if (!std::getenv("PS3_NP_ONLINE_ID")) set_env("PS3_NP_ONLINE_ID", find("name")->value);
    } else {
        clear_env("PS3_NET_ONLINE");
        clear_env("PSNR_SERVER");
    }
    np_psnr_reset();
}


// ---- account and friends ------------------------------------------------------------------------

std::string error_text(int code) {
    switch (code) {
    case -1: return "CANNOT REACH THE SERVER";
    case 2: return "NO SUCH PLAYER";
    case 10: return "NAME: 3-16 LETTERS, DIGITS, - OR _";
    case 11: return "THAT NAME IS TAKEN";
    case 12: return "PASSWORD MUST BE 8-64 CHARACTERS";
    case 13: return "THE SERVER TAKES NO NEW ACCOUNTS";
    case 14: return "WRONG NAME OR PASSWORD";
    case 15: return "THIS BUILD IS NOT ALLOWED ON THE SERVER";
    case 16: return "THE SERVER WANTS AN ACCOUNT";
    case 17: return "THAT NAME BELONGS TO AN ACCOUNT";
    case 18: return "TOO MANY TRIES, WAIT A MINUTE";
    case 19: return "SIGN IN FIRST";
    default: return "SERVER ERROR " + std::to_string(code);
    }
}

std::string account_name() { Item* i = find("account_user"); return i ? i->value : std::string(); }

std::string status_text() {
    const std::string n = account_name();
    return n.empty() ? "NOT SIGNED IN" : "SIGNED IN AS " + n;
}

// The token replaces the password in the ini; the name becomes the online name from the next launch on.
void signed_in(const std::string& name, const std::string& token) {
    find("account_user")->value = name;
    find("account_token")->value = token;
    find("tmp_pw")->value.clear();
    find("tmp_newpw")->value.clear();
    set_env("PS3_NP_ACCOUNT_TOKEN", token);
    np_psnr_set_identity(name.c_str());
    friends_stale = true;
    np_psnr_reset();
    save();
}

void sign_out() {
    find("account_user")->value.clear();
    find("account_token")->value.clear();
#ifdef _WIN32
    _putenv_s("PS3_NP_ACCOUNT_TOKEN", "");
#else
    unsetenv("PS3_NP_ACCOUNT_TOKEN");
#endif
    np_psnr_reset();
    save();
}

struct NetworkWork {
    std::future<std::function<void()>> result;
    std::string label;
};
NetworkWork& network_work() { static NetworkWork work; return work; }
bool network_busy() { return network_work().result.valid(); }
void network_poll() {
    auto& work=network_work();
    if(work.result.valid() && work.result.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try { auto apply=work.result.get(); apply(); }
        catch(...) { notice="REQUEST FAILED. TRY AGAIN"; }
        work.label.clear(); dirty=true;
        std::fprintf(stderr,"[fp-account] %s\n",notice.c_str());
    }
}
bool network_start(const std::string& label, std::function<std::function<void()>()> task) {
    if(network_busy()) { notice="WAIT FOR THE CURRENT REQUEST"; dirty=true; return false; }
    try {
        network_work().label=label;
        network_work().result=std::async(std::launch::async,std::move(task));
    } catch(...) { notice="COULD NOT START REQUEST. TRY AGAIN"; dirty=true; return false; }
    notice=label; dirty=true; return true;
}
void account_command(const std::string& id) {
    if(network_busy()) { notice="WAIT FOR THE CURRENT REQUEST"; return; }
    if(id=="cmd_logout") { sign_out(); notice="SIGNED OUT"; return; }
    set_env("PSNR_SERVER",find("server")->value);
    const std::string name=find("tmp_name")->value, pw=find("tmp_pw")->value;
    const std::string email=find("tmp_email")->value, user=account_name(), neu=find("tmp_newpw")->value;
    if((id=="cmd_register" || id=="cmd_login") && (name.empty() || pw.empty())) { notice="ENTER A NAME AND A PASSWORD"; return; }
    if(id=="cmd_changepw" && (user.empty() || pw.empty() || neu.empty())) { notice="ENTER CURRENT AND NEW PASSWORDS"; return; }
    network_start("ACCOUNT REQUEST IN PROGRESS",[=]() -> std::function<void()> {
        char token[160]={}, canonical[17]={};
        if(id=="cmd_changepw") {
            int rc=np_account_change_password(user.c_str(),pw.c_str(),neu.c_str());
            if(rc) return [rc]{ notice=error_text(rc); };
            rc=np_account_request(PSNR_LOGIN,user.c_str(),neu.c_str(),nullptr,token,sizeof token,canonical);
            if(rc) return []{ sign_out();notice="PASSWORD CHANGED. SIGN IN AGAIN"; };
        } else {
            const bool reg=id=="cmd_register";
            const int rc=np_account_request(reg?PSNR_REGISTER:PSNR_LOGIN,name.c_str(),pw.c_str(),reg?email.c_str():nullptr,token,sizeof token,canonical);
            if(rc) return [rc]{ notice=error_text(rc); };
        }
        const std::string account=canonical, credential=token;
        return [=] {
            signed_in(account,credential);
            notice=id=="cmd_changepw"?"PASSWORD CHANGED":"SIGNED IN AS "+account+". RESTART TO PLAY AS THIS NAME";
        };
    });
}

// The FRIENDS tab. Rows: the add field, then requests to answer, friends, requests you sent, and blocked names.
struct FriendRow { int kind; std::string name; bool online; uint64_t room; };
enum { kAddRow, kIncoming, kFriend, kOutgoing, kBlocked };
std::vector<FriendRow> friend_rows;
int friend_status = -1;   // result of the last fetch: 0 ok, -1 not connected, else a server error
long long friends_next_ms = 0;

long long mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void refresh_friends() {
    const long long now=mono_ms();
    if(network_busy() || (!friends_stale && now<friends_next_ms)) return;
    friends_stale=false; friends_next_ms=now+3000;
    if(account_name().empty()) { friend_rows={{kAddRow,"",false,0}}; friend_status=19; return; }
    set_env("PSNR_SERVER",find("server")->value);
    network_start("REFRESHING FRIENDS",[]() -> std::function<void()> {
        np_friend_list list{};
        const int status=np_friend_fetch(&list);
        std::vector<FriendRow> rows{{kAddRow,"",false,0}};
        if(status==0) {
            for(int i=0;i<list.nincoming;++i) rows.push_back({kIncoming,list.incoming[i],false,0});
            for(int i=0;i<list.nfriends;++i) rows.push_back({kFriend,list.friends[i].name,list.friends[i].online!=0,list.friends[i].room});
            for(int i=0;i<list.noutgoing;++i) rows.push_back({kOutgoing,list.outgoing[i],false,0});
            for(int i=0;i<list.nblocked;++i) rows.push_back({kBlocked,list.blocked[i],false,0});
        }
        return [status,rows=std::move(rows)] { friend_status=status;friend_rows=rows;notice=status?error_text(status):"FRIENDS UPDATED"; };
    });
}

void friend_op(uint8_t type,const std::string& name,const char* done) {
    const std::string message=done;
    network_start("FRIEND REQUEST IN PROGRESS",[=]() -> std::function<void()> {
        const int rc=np_friend_do(type,name.c_str());
        return [=] { notice=rc==0?message:error_text(rc);friends_stale=true; };
    });
}

void apply_item(const char* id) {
    if (fp_gfx_is_d3d12() && (!std::strcmp(id, "vsync") || !std::strcmp(id, "async") || !std::strcmp(id, "hitch_log") ||
                              !std::strcmp(id, "no_shader_cache"))) return;   // Vulkan-only settings
    if (!std::strcmp(id, "vsync")) {
        const std::string& v = find("vsync")->value;
        rsx_vulkan_backend_set_present_mode(v == "ON" ? "fifo" : v == "ADAPTIVE" ? "mailbox" : "immediate");
    } else if (!std::strcmp(id, "async")) rsx_vulkan_backend_set_async(on("async"));
    else if (!std::strcmp(id, "hitch_log")) {
        const std::string& v = find("hitch_log")->value;
        rsx_vulkan_backend_set_hitch_ms(v == "OFF" ? -1.0 : std::atof(v.c_str()));
    } else if (!std::strcmp(id, "no_shader_cache")) rsx_vulkan_backend_set_disk_cache(!on("no_shader_cache"));
    else if (!std::strcmp(id, "local_players")) fp_pad_set_local_players(std::atoi(find("local_players")->value.c_str()));
    else if (!std::strcmp(id, "ai_nav_guard")) fp_ai_route_set_guard(on("ai_nav_guard"));
    else if (!std::strcmp(id, "online") || !std::strcmp(id, "server") || !std::strcmp(id, "p2p_port")) apply_network();
}

// Values the runtime reads from the environment at start-up. A variable already set by the user wins.
void export_environment() {
#ifdef _WIN32
    auto set = [](const char* name, const std::string& value) { if (!std::getenv(name)) _putenv_s(name, value.c_str()); };
#else
    auto set = [](const char* name, const std::string& value) { ::setenv(name, value.c_str(), 0); };
#endif
    // Host output size is live. Keep the guest in its default 720p mode;
    // an explicit PS3_VIDEO_RES override remains available for diagnostics.
    if (Item* u = find("upscaler")) if (u->value == "FSR 1.0") set("RSX_UPSCALER", "fsr");
    if (Item* f = find("fsr_sharp")) set("RSX_FSR_SHARPNESS", f->value == "SOFT" ? "1.0" : f->value == "SHARP" ? "0.0" : "0.25");
    if (Item* r = find("render_scale")) if (r->value != "1X") set("RSX_RES_SCALE", r->value.substr(0, r->value.size() - 1));
    // What run.sh and run.bat export, so the exe works when started by hand. A value already set (the launchers, the
    // headless test tools) wins. Without the window variable the backend renders offscreen only.
    set("PS3RECOMP_VK_WINDOW", "1");
    set("PS3RECOMP_VK_GUEST_PROGRAMS", "1");
    set("GCM_PRESENT_BARRIER", "1");
    set("GCM_SEMA_ACQUIRE", "0");
    set("SPU_MFC_LOOKUP_CACHE", "1");
    set("RSX_RENDERPASS_BATCH", "1");
    set("RSX_DRAW_BATCH", "1");
    set("RSX_DESCRIPTOR_CACHE", "1");
    set("RSX_DRAW_STATE_CACHE", "1");
    set("SPURS_SYNTHETIC_CHAIN_EVENTS", "0");
    set("PS3_NET_SOCKETS", "1");
    // The main thread's two per-frame poll loops (a worker hand-off waited with usleep(500) once a frame, and
    // func_002BDB20's flag wait with usleep(200) ~2.5 times a frame) re-check every 50 us instead.
    set("PS3_USLEEP_POLL", "0030DAF0:50,002C2FAC:50");
    if (Item* i = find("fps_cap")) if (i->value != "AUTO") set("FP_FPS_CAP", i->value == "UNLIMITED" ? "0" : i->value);
    set("FP_STATS", find("stats")->value!="OFF" ? "1" : "0");
    if (Item* i = find("vsync")) set("FP_PRESENT_MODE", i->value == "ON" ? "fifo" : i->value == "ADAPTIVE" ? "mailbox" : "immediate");
    set("RSX_ASYNC_SUBMIT", on("async") ? "1" : "0");
    // The build the server checks, and the signed in account, whose name replaces the online name below.
#ifdef FP_BUILD_ID
    set("PS3_NP_BUILD_ID", FP_BUILD_ID);
    set("PS3_NP_BUILD_SECRET", FP_BUILD_SECRET);
#endif
    if (on("online") && !account_name().empty() && !find("account_token")->value.empty()) {
        set("PS3_NP_ACCOUNT_TOKEN", find("account_token")->value);
        set("PS3_NP_ONLINE_ID", account_name());
    }
    if (on("online")) {
        set("PS3_NET_ONLINE", "1");
        set("PSNR_SERVER", find("server")->value);
        set("PS3_NP_ONLINE_ID", find("name")->value);
        set("PS3_NP_COMM_ID", "NPWR00737_00");   // the game asks for its friends before Matching2 starts
        set("PS3_NET_P2P_PORT", find("p2p_port")->value);
    }
    if (on("debug_bindings")) set("FP_CHEATS", "debugbindings");
    if (Item* i = find("hitch_log")) if (i->value != "OFF") set("FP_HITCH_MS", i->value);
    if (on("no_shader_cache")) set("FP_SHADER_CACHE_DIR", "");
    set("FP_LOCAL_PLAYERS", find("local_players")->value);
    // The audio SPU job calls DSP stubs it loads into local store at run time (LS 0x38B90, 0x38ED8). Without the
    // interpreter the job ends there, never sets event flag 0x00F9D500 and all game audio stops after a few minutes.
    // Every pipeline's shaders, once, so an F10 pass dump can name what each draw ran.
    if (!std::getenv("RSX_ENGINE_SHADER_DUMP")) { mkdir("shader-dump", 0755); set("RSX_ENGINE_SHADER_DUMP", "shader-dump"); }
    set("SPU_INTERP_UNLIFTED", "1");
    // The audio library's SPURS task (it loads the DSP module at LS 0x38B80) and its PPU thread; cellAudio raises their priority.
    set("SPU_BOOST_IMAGES", "532678573");
    set("FP_BOOST_THREADS", "MultiStream PU Thread");
    // The D3D12 renderer draws through the live NV4097 engine, which stays inert unless this is set (read once).
    if (fp::settings_renderer_d3d12()) set("RSX_LIVE_DRAW", "1");
}

__attribute__((constructor(101))) void settings_startup() {
    init_items();
    load();
    // A frame limit given in the environment stands in for the saved one until the menu changes it.
    if (const char* e = std::getenv("FP_SKIP_TITLE")) if (Item* i = find("skip_title")) i->value = *e == '1' ? "ON" : "OFF";   // test routes
    if (const char* e = std::getenv("FP_MIC_MODE")) if (Item* i = find("mic_mode")) {   // OPEN_MIC: no space, for shell quoting
        i->value = e;
        for (char& c : i->value) if (c == '_') c = ' ';
    }
    if (const char* e = std::getenv("FP_GAME_SPEED")) if (Item* i = find("speed")) i->value = e;
    if (const char* e = std::getenv("FP_FPS_CAP")) if (Item* i = find("fps_cap")) i->value = std::atof(e) > 0 ? e : "UNLIMITED";
    // Preserve diagnostic modes until this row changes. Later menu changes
    // override the launch choice, just like the frame-limit setting above.
    if (std::getenv("FP_AI_NAV_GRAPH")) find("ai_nav_guard")->value = fp_ai_route_guard_enabled() ? "ON" : "OFF";
    else fp_ai_route_set_guard(on("ai_nav_guard"));
    for (const auto& i : menu_items()) if (i.restart) startup_settings()[i.id] = i.value;
    export_environment();
    apply_live();
    pending_fullscreen = on("fullscreen");
}

// ---- input -----------------------------------------------------------------------------------------
enum Nav { UP, DOWN, LEFT, RIGHT, OK, BACK, TAB_PREV, TAB_NEXT, NAV_COUNT };

struct Edge {
    bool prev[SDL_NUM_SCANCODES] = {false};
    bool down(SDL_Scancode s, const Uint8* keys) {
        const bool now = keys[s] != 0;
        const bool pressed = now && !prev[s];
        prev[s] = now;
        return pressed;
    }
};
Edge keyedge;

bool pad_prev[32] = {false};
bool combo_prev = false;

extern "C" unsigned fp_pad_host_buttons(void);

// CELL_PAD_CTRL_* bits from libs/input/cellPad.h
constexpr unsigned CELL_PAD_CTRL_SELECT = 1u << 0, CELL_PAD_CTRL_START = 1u << 3, CELL_PAD_CTRL_UP = 1u << 4,
                   CELL_PAD_CTRL_RIGHT = 1u << 5, CELL_PAD_CTRL_DOWN = 1u << 6, CELL_PAD_CTRL_LEFT = 1u << 7,
                   CELL_PAD_CTRL_L1 = 1u << 10, CELL_PAD_CTRL_R1 = 1u << 11, CELL_PAD_CTRL_CIRCLE = 1u << 13,
                   CELL_PAD_CTRL_CROSS = 1u << 14;

// Held buttons of the physical pads, read from the pad code. Opening the controllers a second time
// from the presenter thread corrupted the heap.
bool pad_down(unsigned ctrl_bit) {
#if defined(__APPLE__) && TARGET_OS_IOS
    if (fp_touch_pad_bits() & ctrl_bit) return true;   // the on-screen controller navigates the menu too
#endif
    return (fp_pad_host_buttons() & ctrl_bit) != 0;
}

char typed_char(SDL_Scancode s, bool shift) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return static_cast<char>((shift ? 'A' : 'a') + (s - SDL_SCANCODE_A));
    static const char shifted_digits[] = "!@#$%^&*(";
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return shift ? shifted_digits[s - SDL_SCANCODE_1] : static_cast<char>('1' + (s - SDL_SCANCODE_1));
    if (s == SDL_SCANCODE_0) return shift ? ')' : '0';
    if (s == SDL_SCANCODE_SLASH) return shift ? '?' : '/';
    if (s == SDL_SCANCODE_EQUALS) return shift ? '+' : '=';
    if (s == SDL_SCANCODE_COMMA) return shift ? '<' : ',';
    if (s == SDL_SCANCODE_SPACE) return ' ';
    if (s == SDL_SCANCODE_PERIOD) return shift ? '>' : '.';
    if (s == SDL_SCANCODE_MINUS) return shift ? '_' : '-';
    if (s == SDL_SCANCODE_SEMICOLON) return shift ? ':' : 0;
    return 0;
}

std::vector<Item*> page_items() {
    std::vector<Item*> out;
    for (auto& i : menu_items()) if (i.tab == page_id()) {
#if defined(__APPLE__) && TARGET_OS_IOS
        if (!std::strcmp(i.id,"fullscreen")) continue;
        if (!fp_touch_keyboard_connected() && (i.kind==Kind::Key ||
            !std::strcmp(i.id,"page_movement") || !std::strcmp(i.id,"page_shoulders"))) continue;
#endif
        out.push_back(&i);
    }
    return out;
}

bool persistent(const Item& it) {
    return it.kind == Kind::Toggle || it.kind == Kind::Choice || it.kind == Kind::Key || it.kind == Kind::Text;
}
bool can_reset() {
    if (page_id() == "ONLINE/ACCOUNT" || friends_page() || (page_id()=="TOOLS/SELFTEST" && fp::self_test_status().running)) return false;
    for (const auto* it : page_items()) if (persistent(*it)) return true;
    return false;
}
void update_restart() {
    restart_needed = false;
    for (const auto& it : menu_items()) if (it.restart && startup_settings()[it.id] != it.value) restart_needed = true;
}

bool debug_bindings_loaded() {
    const auto entry = startup_settings().find("debug_bindings");
    return entry != startup_settings().end() && entry->second == "ON";
}
std::string debug_bindings_help() {
    return on("debug_bindings") ? "RESTART TO LOAD DEBUG CHEAT COMBOS" : "ENABLE DEBUG CHEAT COMBOS IN PLAYER CHEATS";
}

std::string disabled_reason(const Item& it) {
    const std::string id = it.id;
    if (network_busy() && (it.tab==std::string("ONLINE/ACCOUNT") || id=="online" || id=="server" || id=="p2p_port" || id=="name")) return "WAIT FOR THE CURRENT NETWORK REQUEST";
    if (id == "test_start" && fp_gfx_is_d3d12()) return "SELF-TEST CAPTURES REQUIRE THE VULKAN RENDERER";
    if (id == "test_start" && find("test_scenario")->value=="CLASS CHANGES" && !debug_bindings_loaded()) return debug_bindings_help();
    if (id == "test_start" && (!fp::player_control_state().available || fp::self_test_status().running)) return "START IN A MATCH WITH NO OTHER TEST RUNNING";
    if (id=="test_report" && fp::self_test_status().report.empty()) return "RUN A SELF-TEST TO CREATE A REPORT";
    if (id == "test_stop" && !fp::self_test_status().running) return "NO SELF-TEST IS RUNNING";
    if ((id == "test_scenario" || id == "test_duration") && fp::self_test_status().running) return "STOP THE CURRENT TEST BEFORE CHANGING THIS";
    if (it.kind == Kind::Runtime && !fp::player_control_state().available) return "START A MATCH TO CHANGE PLAYER CONTROL";
    if ((id=="ai_recheck" || id=="ai_lock_on") && find("ai_profile")->value!="CUSTOM") return "SELECT THE CUSTOM AI PROFILE TO EDIT THIS";
    if (it.kind == Kind::Action) {
        if (!fp::player_control_state().available) return "START A MATCH TO USE THIS TOOL";
        if (id!="act_freecam" && !debug_bindings_loaded()) return debug_bindings_help();
    }
    if (fp_gfx_is_d3d12() && (id == "render_scale" || id == "upscaler" ||
        id == "fsr_sharp" || id == "interp" || id == "vsync" || id == "async" || id == "hitch_log" ||
        id == "no_shader_cache" || id == "overlay_opacity")) return "AVAILABLE WITH THE VULKAN RENDERER";
    if ((id == "upscaler" || id == "fsr_sharp") && std::atof(find("render_scale")->value.c_str()) >= 1.f)
        return "USED WHEN RENDER SCALE IS BELOW 1X";
    if (id == "fsr_sharp" && !on_value("upscaler", "FSR 1.0")) return "SELECT FSR 1.0 TO ADJUST SHARPNESS";
    if ((id == "voice_volume" || id == "mic_gain" || id == "mic_mode") && !on("voice")) return "TURN ON VOICE CHAT TO ADJUST THIS";
    if (id == "acct_status") return "";
    if ((id == "cmd_logout" || id == "cmd_changepw" || id == "tmp_newpw") && account_name().empty()) return "SIGN IN FIRST";
    if ((id == "cmd_login" || id == "cmd_register") && !account_name().empty()) return "ALREADY SIGNED IN";
    return "";
}

std::string help_text(const Item* it) {
    if (!it) return "RESTORE ONLY THE SETTINGS ON THIS PAGE";
    if (const auto why = disabled_reason(*it); !why.empty()) return why;
    const std::string id = it->id;
    if (id == "resolution") return fp_gfx_is_d3d12() ?
        "LIVE OUTPUT SIZE. D3D12 SCALES THE GUEST FRAME" : "LIVE OUTPUT SIZE. RENDER SCALE CONTROLS DETAIL";
    if (id == "debug_bindings") return "GAME LOADS THESE CONTROLLER BINDINGS AT STARTUP";
    if (id == "skip_intro" || id == "skip_howto" || id == "skip_title") return "USED ON THE NEXT GAME START";
    if (id == "name") return "GAME CACHES YOUR NAME AT STARTUP. RESTART TO UPDATE";
    if (id == "fullscreen") return "BORDERLESS FULLSCREEN. ALT+ENTER TO TOGGLE";
    if (id=="test_scenario" && find("test_scenario")->value=="WATER SWEEP") return "GREAT GORGE ONLY. START WITH FREE CAMERA OFF";
    if (id=="test_scenario" && find("test_scenario")->value=="CLASS CHANGES") return "CYCLES WARRIOR/RANGER/MAGE/WORKER. CHEAT COMBOS ON";
    if (id.rfind("test_",0)==0) return "TESTS THIS MATCH. REPORTS ARE SAVED UNDER REPORTS";
    if (id=="stats" && fp_gfx_is_d3d12()) return "D3D12 SHOWS THE COMPACT FPS PANEL";
    if (id == "ui_scale") return "LARGER TEXT USES FEWER VISIBLE ROWS";
    if (id == "menu_button") return "CHOOSE A SHORTCUT THAT DOES NOT CONFLICT WITH PLAY";
    if (id == "ai_control") {
        const auto state = fp::player_control_state();
        if (!state.error.empty()) return state.error;
        return state.pending ? "WAITING FOR THE GAME TO CHANGE CONTROL" : "AI PLAYS YOUR CHARACTER. SELECT YOU TO TAKE CONTROL";
    }
    if (id=="act_freecam") return "CONTROLLER MOVES THE CAMERA. AI CAN KEEP PLAYING";
    if (id=="ai_profile") {
        if (it->value=="ADVANCED") return "EXPERIMENTAL: 4X DECISION RATE, 100% LOCK-ON CHANCE";
        if (it->value=="EASY") return "HALF DECISION RATE. LOCK-ON CHANCE CAPPED AT 25%";
        if (it->value=="HARD") return "2X DECISION RATE. LOCK-ON CHANCE AT LEAST 90%";
        if (it->value=="CUSTOM") return "TUNE YOUR BOT ONLY. HEALTH AND DAMAGE STAY THE SAME";
        return "ORIGINAL GAME BRAIN AND MAP SETTINGS. NO CLOUD MODEL";
    }
    if (id=="ai_recheck") return "SECONDS BETWEEN RECONSIDERING TASKS. LOWER IS FASTER";
    if (id=="ai_nav_guard") return "CHECKS WATER AND LAVA BEFORE MOVING. APPLIES TO LOCAL AI";
    if (id=="ai_lock_on") return "CHANCE TO LOCK ON WHEN ATTACKING. NOT HIT ACCURACY";
    if (it->kind == Kind::Submenu) return "OPEN " + std::string(it->label);
    if (id == "trophies") return "EVERY TROPHY AND YOUR PROGRESS. F2 OR GUIDE OPENS IT IN GAME";
    if (id == "fps_cap") return "SIMULATION LOCK OR INTERPOLATION CAN OVERRIDE THIS LIMIT";
    if (id == "timing" || id == "speed") return "TESTING OPTIONS. CHANGING THESE CAN CHANGE GAME SPEED";
    if (id == "renderer") return "D3D12 IS EXPERIMENTAL. RESTART REQUIRED";
    if (id == "interp") return "EXPERIMENTAL. SYNTHESIZES FRAMES BETWEEN GAME UPDATES";
    if (id == "p2p_port") return "USE A DIFFERENT PORT FOR EACH INSTANCE ON ONE MACHINE";
    if (id == "server") return "CHANGING THIS RECONNECTS ONLINE PLAY";
#ifdef FP_DEBUG_MENU
    if (id == "dbg_trophy_id" || id == "dbg_trophy_show") {
        const char* name = sceNpTrophyPreviewName(std::atoi(find("dbg_trophy_id")->value.c_str()));
        if (!name) return "TROPHIES LOAD WITH THE GAME. THE TROPHY STAYS LOCKED";
        std::string upper = name;
        for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
        return "TROPHY " + find("dbg_trophy_id")->value + ": " + upper + ". IT STAYS LOCKED";
    }
    if (id == "dbg_trophy_all") return "QUEUES ALL 27 CARDS, 5 SECONDS EACH. NOTHING IS UNLOCKED";
#endif
    if (it->kind == Kind::Action) return "RUN THIS ACTION NOW IN THE CURRENT MATCH";
    if (it->kind == Kind::Key) return "SELECT THEN PRESS A KEY. ESC CANCELS";
    return it->restart ? "TAKES EFFECT AFTER RESTARTING THE GAME" : "CHANGES TAKE EFFECT IMMEDIATELY";
}

bool changed(Item& it, const std::string& previous = {}) {
    update_restart();
    if (!std::strcmp(it.id, "fullscreen")) apply_fullscreen();
    if (!std::strcmp(it.id, "resolution") && !apply_resolution()) {
        // The backends retain the old output if replacement resources cannot be created.
        it.value = previous;
        notice = "COULD NOT CHANGE OUTPUT SIZE. PREVIOUS SIZE KEPT";
        dirty = true;
        return false;
    }
    apply_live();
    apply_item(it.id);
    if (it.restart) notice = "CHANGE APPLIES AFTER RESTART";
    else notice.clear();
    save();
    dirty = true;
    return true;
}

void cycle(Item& it, int dir) {
    if (const auto why = disabled_reason(it); !why.empty()) { notice = why; dirty = true; return; }
    if (it.kind == Kind::Runtime) {
        if(fp::self_test_status().running) fp::self_test_stop();
        fp::player_control_request(dir > 0); notice.clear(); dirty = true; return;
    }
    const std::string previous = it.value;
    if (it.kind == Kind::Toggle) {
        it.value = it.value == "ON" ? "OFF" : "ON";
    } else if (it.kind == Kind::Choice) {
        size_t at = 0;
        for (size_t k = 0; k < it.choices.size(); ++k) if (it.choices[k] == it.value) at = k;
        at = (at + it.choices.size() + dir) % it.choices.size();
        it.value = it.choices[at];
    } else {
        return;
    }
    changed(it, previous);
}

void reset_tab() {
    const std::string previous_resolution = find("resolution")->value;
    for (Item* it : page_items()) {
        if (!persistent(*it)) continue;
        it->value = it->def;
    }
    for (Item* it : page_items()) if (persistent(*it)) apply_item(it->id);
    bool output_ok = true;
    if (find("resolution")->value != previous_resolution && !apply_resolution()) {
        find("resolution")->value = previous_resolution;
        output_ok = false;
    }
    update_restart();
    notice = output_ok ? "PAGE RESTORED TO DEFAULTS" : "OUTPUT SIZE KEPT. OTHER PAGE SETTINGS RESTORED";
    apply_live();
    apply_fullscreen();
    save();
    dirty = true;
}

void activate(Item* it) {
    if (!it) { if (can_reset()) confirm("RESTORE THIS PAGE TO DEFAULTS?", reset_tab); return; }
    if (const auto why = disabled_reason(*it); !why.empty()) { notice = why; dirty = true; return; }
    if (!std::strcmp(it->id,"test_start")) {
        char* end=nullptr;
        const auto seconds=std::strtoul(find("test_duration")->value.c_str(),&end,10);
        const bool started=end && !*end && seconds>=60 && seconds<=7200 && fp::self_test_start(find("test_scenario")->value,unsigned(seconds));
        notice=started?"TEST STARTED. CLOSE SETTINGS TO WATCH":"TEST COULD NOT START";
        if(started) { const auto list=page_items();for(size_t i=0;i<list.size();++i) if(!std::strcmp(list[i]->id,"test_stop")) selected_row()=int(i); }
        dirty=true; return;
    }
    if (!std::strcmp(it->id,"test_report")) {
        const auto path=(std::filesystem::absolute(fp::self_test_status().report).parent_path()/"report.html").generic_string();
        std::string url="file://";if(path.empty() || path[0]!='/') url+='/';
        for(unsigned char c:path) {
            if(std::isalnum(c) || c=='/' || c==':' || c=='.' || c=='_' || c=='-' || c=='~') url+=c;
            else { char encoded[4];std::snprintf(encoded,sizeof encoded,"%%%02X",c);url+=encoded; }
        }
        notice=SDL_OpenURL(url.c_str())==0?"REPORT OPENED IN YOUR BROWSER":"OPEN REPORT.HTML IN THE REPORT FOLDER";
        dirty=true;return;
    }
    if (!std::strcmp(it->id,"test_stop")) { fp::self_test_stop(); notice="TEST STOPPED. REPORT SAVED"; dirty=true; return; }
    if (!std::strcmp(it->id,"trophies")) {
        if (fp::trophy_ui_open(-1)) { if (is_open) fp::settings_menu_toggle(); g_fp_pad_blocked = 1; }
        else { notice = "TROPHIES LOAD WITH THE GAME"; dirty = true; }
        return;
    }
#ifdef FP_DEBUG_MENU
    if (!std::strcmp(it->id,"dbg_trophy_show") || !std::strcmp(it->id,"dbg_trophy_all")) {
        const bool all=!std::strcmp(it->id,"dbg_trophy_all");
        const int first=all?0:std::atoi(find("dbg_trophy_id")->value.c_str()), last=all?26:first;
        int shown=0;
        for(int id=first;id<=last;++id) shown+=sceNpTrophyPreviewUnlock(id)==0;
        notice=!shown?"TROPHIES ARE NOT LOADED YET":all?std::to_string(shown)+" CARDS QUEUED. NOTHING WAS UNLOCKED":"CARD SHOWN. NOTHING WAS UNLOCKED";
        dirty=true;return;
    }
#endif
    if (it->kind == Kind::Submenu) { subpage = it->def; notice.clear(); dirty = true; return; }
    if (it->kind == Kind::Runtime) {
        const auto state = fp::player_control_state();
        if(fp::self_test_status().running) fp::self_test_stop();
        fp::player_control_request(!(state.pending ? state.requested_ai : state.ai)); notice.clear(); dirty = true; return;
    }
    if (!std::strcmp(it->id,"act_freecam")) {
        notice=fp::player_command_request("freecamera toggle") ? "FREE CAMERA CHANGE REQUESTED" : "CAMERA REQUEST COULD NOT BE QUEUED";
        dirty=true;return;
    }
    if (it->kind == Kind::Action) {
        fp_pad_inject(static_cast<unsigned>(std::strtoul(it->def.c_str(), nullptr, 16)), 450);
        notice = "SENT: " + std::string(it->label);
        dirty = true;
    }
    else if (it->kind == Kind::Toggle || it->kind == Kind::Choice) cycle(*it, 1);
    else if (it->kind == Kind::Text || it->kind == Kind::Secret) { editing = true; edit_buffer = it->value; SDL_StartTextInput(); dirty = true; }
    else if (it->kind == Kind::Command) {
        if (!std::strcmp(it->id, "cmd_logout")) confirm("SIGN OUT OF THIS ACCOUNT?", [] { account_command("cmd_logout"); });
        else if (std::strcmp(it->id, "acct_status")) account_command(it->id);
        dirty = true;
    }
    else if (it->kind == Kind::Key) { capturing = true; notice = "PRESS A KEY (ESC CANCELS)"; dirty = true; }
}

std::mutex text_mutex;
std::string pending_text;
int text_event(void*, SDL_Event* event) {
    if(event->type==SDL_TEXTINPUT) {
        std::lock_guard<std::mutex> lock(text_mutex);
        if(pending_text.size()<512) pending_text+=event->text.text;
    }
    return 1;
}
size_t text_limit(const Item& it) {
    if(it.kind==Kind::Secret) return 64;
    if(!std::strcmp(it.id,"server")) return 253;
    if(!std::strcmp(it.id,"tmp_email")) return 128;
    return 28;
}
void append_text(std::string& destination,const std::string& text,size_t limit) {
    const size_t previous=destination.size();
    for(unsigned char c:text) if(c>=32 && c<127 && destination.size()<limit) destination+=c;
    if(destination.size()!=previous) dirty=true;
}
bool paste_text(const Uint8* keys,std::string& destination,size_t limit) {
    if(!(keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL])) return false;
    if(keyedge.down(SDL_SCANCODE_V,keys)) {
        if(char* text=SDL_GetClipboardText()) { append_text(destination,text,limit);SDL_free(text); }
    }
    return true;
}

void handle_text(const Uint8* keys, Item& it) {
    if(paste_text(keys,edit_buffer,text_limit(it))) return;
    const bool shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    for (int s = 0; s < SDL_NUM_SCANCODES; ++s) {
        if (!keyedge.down(static_cast<SDL_Scancode>(s), keys)) continue;
        if (s == SDL_SCANCODE_RETURN || s == SDL_SCANCODE_KP_ENTER) { it.value = edit_buffer; editing = false; changed(it); return; }
        if (s == SDL_SCANCODE_ESCAPE) { editing = false; dirty = true; return; }
        if (s == SDL_SCANCODE_BACKSPACE) { if (!edit_buffer.empty()) edit_buffer.pop_back(); dirty = true; continue; }
        const char c = typed_char(static_cast<SDL_Scancode>(s), shift);
        if (fp_gfx_is_d3d12() && c && edit_buffer.size() < text_limit(it)) { edit_buffer += c; dirty = true; }
    }
}

void handle_capture(const Uint8* keys, Item& it) {
    for (int s = 0; s < SDL_NUM_SCANCODES; ++s) {
        if (!keyedge.down(static_cast<SDL_Scancode>(s), keys)) continue;
        capturing = false;
        if (s == SDL_SCANCODE_ESCAPE || s == SDL_SCANCODE_F1 || s == SDL_SCANCODE_F3) { notice.clear(); dirty = true; return; }
        it.value = SDL_GetScancodeName(static_cast<SDL_Scancode>(s));
        changed(it);
        notice = "BOUND";
        return;
    }
}


void friend_text(const Uint8* keys) {
    if(paste_text(keys,edit_buffer,16)) return;
    const bool shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc) {
        if (!keyedge.down(static_cast<SDL_Scancode>(sc), keys)) continue;
        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            editing = false;
            if (!edit_buffer.empty()) friend_op(PSNR_FRIEND_REQUEST, edit_buffer, "REQUEST SENT");
            edit_buffer.clear();
            return;
        }
        if (sc == SDL_SCANCODE_ESCAPE) { editing = false; dirty = true; return; }
        if (sc == SDL_SCANCODE_BACKSPACE) { if (!edit_buffer.empty()) edit_buffer.pop_back(); dirty = true; continue; }
        const char c = typed_char(static_cast<SDL_Scancode>(sc), shift);
        if (fp_gfx_is_d3d12() && c && edit_buffer.size() < 16) { edit_buffer += c; dirty = true; }
    }
}

constexpr unsigned CELL_PAD_CTRL_TRIANGLE = 1u << 12, CELL_PAD_CTRL_SQUARE = 1u << 15;
bool square_prev = false, triangle_prev = false;

void friends_nav(const bool* nav, const Uint8* keys, bool focused) {
    if(network_busy()) return;
    const bool sq = pad_down(CELL_PAD_CTRL_SQUARE), tr = pad_down(CELL_PAD_CTRL_TRIANGLE);
    bool decline = sq && !square_prev, block = tr && !triangle_prev;
    square_prev = sq;
    triangle_prev = tr;
    if (focused) {
        decline |= keyedge.down(SDL_SCANCODE_D, keys);
        block |= keyedge.down(SDL_SCANCODE_B, keys);
    }
    const int n = static_cast<int>(friend_rows.size());
    int& r = rows["ONLINE/FRIENDS"];
    if (n == 0) { r = 0; return; }     // the list is rebuilt on the next refresh
    if (r >= n) r = n - 1;
    if (r < 0) r = 0;
    if (nav[UP]) { r = (r + n - 1) % n; dirty = true; notice.clear(); }
    if (nav[DOWN]) { r = (r + 1) % n; dirty = true; notice.clear(); }
    const FriendRow fr = friend_rows[r];
    if (nav[OK]) {
        if (fr.kind == kAddRow) { editing = true; edit_buffer.clear(); SDL_StartTextInput(); dirty = true; }
        else if (fr.kind == kIncoming) friend_op(PSNR_FRIEND_ACCEPT, fr.name, "FRIEND ADDED");
        else if (fr.kind == kBlocked) friend_op(PSNR_UNBLOCK, fr.name, "UNBLOCKED");
        else { notice = "SQUARE/D REMOVE OR CANCEL. TRIANGLE/B BLOCK"; dirty = true; }
    }
    if (decline) {
        if (fr.kind == kIncoming) confirm("DECLINE REQUEST FROM " + fr.name + "?", [fr] { friend_op(PSNR_FRIEND_DECLINE, fr.name, "DECLINED"); });
        else if (fr.kind == kFriend) confirm("REMOVE " + fr.name + " FROM FRIENDS?", [fr] { friend_op(PSNR_FRIEND_REMOVE, fr.name, "REMOVED"); });
        else if (fr.kind == kOutgoing) confirm("CANCEL REQUEST TO " + fr.name + "?", [fr] { friend_op(PSNR_FRIEND_DECLINE, fr.name, "REQUEST CANCELLED"); });
        else if (fr.kind == kBlocked) friend_op(PSNR_UNBLOCK, fr.name, "UNBLOCKED");
    }
    if (block && (fr.kind == kIncoming || fr.kind == kFriend || fr.kind == kOutgoing))
        confirm("BLOCK " + fr.name + "?", [fr] { friend_op(PSNR_BLOCK, fr.name, "BLOCKED"); });
}

std::string render_friends() {
    std::string out;
    if (friend_status == 19) out += "  SIGN IN UNDER ONLINE > ACCOUNT FIRST\n";
    else if (friend_status != 0) out += "  " + error_text(friend_status) + "\n";
    const int total = static_cast<int>(friend_rows.size());
    int& r = rows["ONLINE/FRIENDS"];
    if (r >= total) r = total - 1;
    if (r < 0) r = 0;
    const int kRows = on_value("ui_scale","LARGE")?5:9;
    int first = 0, last = total;
    if (total > kRows) { first = std::max(0, std::min(r - kRows / 2, total - kRows)); last = first + kRows; }
    if (first > 0) out += "  ^ MORE ABOVE\n";
    for (int i = first; i < last; ++i) {
        const FriendRow& fr = friend_rows[i];
        std::string label, value;
        switch (fr.kind) {
        case kAddRow: label = "ADD A FRIEND BY NAME"; value = (editing && i == r) ? edit_buffer + "_" : "ENTER TO TYPE"; break;
        case kIncoming: label = "REQUEST FROM " + fr.name; value = "ENTER ACCEPT"; break;
        case kFriend: label = fr.name; value = !fr.online ? "OFFLINE" : fr.room ? "IN A GAME" : "ONLINE"; break;
        case kOutgoing: label = "ASKED " + fr.name; value = "WAITING"; break;
        default: label = "BLOCKED " + fr.name; value = "ENTER UNBLOCK"; break;
        }
        std::string line = (i == r ? "> " : "  ") + label;
        const size_t width = 54;
        if (line.size() + value.size() + 1 < width) line += std::string(width - line.size() - value.size(), ' ');
        else line += ' ';
        out += line + value + "\n";
    }
    if (last < total) out += "  v MORE BELOW\n";
    return out;
}

// ---- on-screen keyboard (cellOskDialog) ---------------------------------------------------------
// The game asks for text (the clan tag) through cellOskDialogLoadAsync. The framework calls fp_osk_open from the guest
// thread; the menu thread shows this panel and hands the result back with cellOskDialog_host_result.
extern "C" void cellOskDialog_host_result(const char* text, int canceled);

struct OskState {
    std::mutex mutex;
    bool requested = false;     // set by the guest thread, taken by the menu thread
    bool closed_by_guest = false;
    bool active = false;        // menu thread only
    std::string prompt, text;
    unsigned limit = 16;
    int cx = 0, cy = 0;
    bool upper = true;
    bool symbols = false;
} osk;

constexpr int kOskLetterRows = 4, kOskActionCount = 6;
bool osk_pad_prev[8] = {false};
bool osk_triangle_prev = false, osk_just_opened = false;
bool osk_page_prev = false;
std::array<bool,4> osk_repeating{};
std::array<long long,4> osk_repeat_at{};

char osk_char(int row, int col) {
    char c = fp::keyboard_rows(osk.symbols)[row][col];
    return osk.upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
}

void osk_finish(bool canceled) {
    osk.active = false;
    g_fp_pad_blocked = is_open ? 1 : 0;
    dirty = true;
    cellOskDialog_host_result(osk.text.c_str(), canceled ? 1 : 0);
}

void osk_type(char c) {
    if (osk.text.size() < osk.limit) { osk.text += c; dirty = true; }
}

bool osk_backspace() {
    if (osk.text.empty()) return false;
    osk.text.pop_back(); dirty = true;
    return true;
}

void osk_press() {
    if (osk.cy < kOskLetterRows) { osk_type(osk_char(osk.cy, osk.cx)); return; }
    switch (osk.cx) {
    case 0: osk.upper = !osk.upper; dirty = true; break;
    case 1: osk.symbols = !osk.symbols; dirty = true; break;
    case 2: osk_type(' '); break;
    case 3: osk_backspace(); break;
    case 4: osk_finish(true); break;
    default: osk_finish(false); break;
    }
}

// Menu thread, once per frame. Returns true while the keyboard is up and fills panel when it changed.
std::string format_prompts(std::string out,bool pad);

// The trophy screens (trophy_ui.cpp): F2 or the controller's Guide button opens the details of the trophy just
// earned (or the list), and while a screen is up it takes the menu input and the game gets none.
bool trophy_frame(std::string& panel, const Uint8* keys, bool focused) {
    static bool guide_prev = false;
    static bool prev[4] = {};
    const unsigned bits[4] = {CELL_PAD_CTRL_UP, CELL_PAD_CTRL_DOWN, CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_CIRCLE};
    const bool guide = (fp_pad_host_buttons() & 0x10000u) != 0;
    const bool open_key = (focused && keyedge.down(SDL_SCANCODE_F2, keys)) || (guide && !guide_prev);
    guide_prev = guide;
    bool nav[4] = {};
    for (int i = 0; i < 4; ++i) { const bool now = pad_down(bits[i]); nav[i] = now && !prev[i]; prev[i] = now; }
    if (!fp::trophy_ui_active()) {
        if (!open_key || is_open || osk.active || !fp::trophy_ui_open_recent()) return false;
        for (bool& n : nav) n = false;   // the press that opened it is not also a selection
    } else if (open_key) fp::trophy_ui_close();
    if (fp::trophy_ui_active()) {
        for (bool n : nav) if (n) controller_hints = true;
        if (focused) {
            nav[0] |= keyedge.down(SDL_SCANCODE_UP, keys);
            nav[1] |= keyedge.down(SDL_SCANCODE_DOWN, keys);
            nav[2] |= keyedge.down(SDL_SCANCODE_RETURN, keys) | keyedge.down(SDL_SCANCODE_Z, keys);
            nav[3] |= keyedge.down(SDL_SCANCODE_ESCAPE, keys) | keyedge.down(SDL_SCANCODE_X, keys);
            for (int sc : {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE}) if (keys[sc]) controller_hints = false;
        }
        fp::trophy_ui_nav(nav[0], nav[1], nav[2], nav[3]);
    }
    if (!fp::trophy_ui_active()) { g_fp_pad_blocked = is_open ? 1 : 0; panel.clear(); return false; }
    g_fp_pad_blocked = 1;
    panel = fp::trophy_ui_take_dirty() ? "TROPHY" : "";
    return true;
}
bool osk_frame(std::string& panel, const Uint8* keys, bool focused) {
    {
        std::lock_guard<std::mutex> lock(osk.mutex);
        if (osk.closed_by_guest) { osk.closed_by_guest = false; osk.active = false; osk.requested = false; g_fp_pad_blocked = is_open ? 1 : 0; dirty = true; return false; }
        if (osk.requested) {
            osk.requested = false;
            osk.active = true;
            SDL_StartTextInput();
            osk.cx = 0; osk.cy = 1;
            osk.upper = true;
            osk.symbols = false;
            osk_repeating.fill(false);
            g_fp_pad_blocked = 1;
            dirty = true;
            osk_just_opened = true;
        }
    }
    if (!osk.active) return false;
    if (osk_just_opened) {
        // The press that opened the keyboard may still be held; it must not type a letter.
        osk_just_opened = false;
        const unsigned held[8] = {CELL_PAD_CTRL_UP, CELL_PAD_CTRL_DOWN, CELL_PAD_CTRL_LEFT, CELL_PAD_CTRL_RIGHT,
                                  CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_START, CELL_PAD_CTRL_SQUARE};
        for (int i = 0; i < 8; ++i) osk_pad_prev[i] = pad_down(held[i]);
        osk_triangle_prev = pad_down(CELL_PAD_CTRL_TRIANGLE);
        osk_page_prev = pad_down(CELL_PAD_CTRL_L1)||pad_down(CELL_PAD_CTRL_R1);
        for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc) keyedge.down(static_cast<SDL_Scancode>(sc), keys);
    }

    // Headless tests: FP_OSK_AUTO=text enters that text a moment after the keyboard opens (the overlay dump shows it first).
    {
        static int wait = -1;
        static const char* autotext = std::getenv("FP_OSK_AUTO");
        if (autotext) {
            if (wait < 0) wait = 120;
            if (--wait == 0) { osk.text = std::string(autotext).substr(0,osk.limit); wait = -1; osk_finish(false); return true; }
        }
    }

    const unsigned buttons[8] = {CELL_PAD_CTRL_UP, CELL_PAD_CTRL_DOWN, CELL_PAD_CTRL_LEFT, CELL_PAD_CTRL_RIGHT,
                                 CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_START, CELL_PAD_CTRL_SQUARE};
    bool edge[8] = {false};
    for (int i = 0; i < 8; ++i) {
        const bool now = pad_down(buttons[i]);
        edge[i] = now && !osk_pad_prev[i];
        osk_pad_prev[i] = now;
    }
    const bool tri = pad_down(CELL_PAD_CTRL_TRIANGLE);
    const bool triangle_edge = tri && !osk_triangle_prev;
    osk_triangle_prev = tri;
    if (focused) {
        paste_text(keys,osk.text,osk.limit);
        edge[0] |= keyedge.down(SDL_SCANCODE_UP, keys);
        edge[1] |= keyedge.down(SDL_SCANCODE_DOWN, keys);
        edge[2] |= keyedge.down(SDL_SCANCODE_LEFT, keys);
        edge[3] |= keyedge.down(SDL_SCANCODE_RIGHT, keys);
        const bool shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
        for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc) {
            if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT) continue;
            if (!keyedge.down(static_cast<SDL_Scancode>(sc), keys)) continue;
            if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) { osk_finish(false); return true; }
            if (sc == SDL_SCANCODE_ESCAPE) { osk_finish(true); return true; }
            if (sc == SDL_SCANCODE_BACKSPACE) { if (!osk.text.empty()) osk.text.pop_back(); dirty = true; continue; }
            const char c = typed_char(static_cast<SDL_Scancode>(sc), shift);
            if (fp_gfx_is_d3d12() && c) osk_type(c);
        }
    }
    const SDL_Scancode arrow_keys[]={SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT};
    for(unsigned i=0;i<4;++i) {
        const bool held=pad_down(buttons[i]) || (focused && keys[arrow_keys[i]]);
        const auto now=mono_ms();
        if(!held) osk_repeating[i]=false;
        else if(!osk_repeating[i]) { osk_repeating[i]=true;osk_repeat_at[i]=now+350; }
        else if(now>=osk_repeat_at[i]) { edge[i]=true;osk_repeat_at[i]=now+85; }
    }
    const bool page=pad_down(CELL_PAD_CTRL_L1)||pad_down(CELL_PAD_CTRL_R1);
    if(page && !osk_page_prev) { osk.symbols=!osk.symbols;dirty=true; }
    osk_page_prev=page;
    if (edge[0]) { fp::keyboard_vertical(osk.cy,osk.cx,-1,osk.symbols); dirty = true; }
    if (edge[1]) { fp::keyboard_vertical(osk.cy,osk.cx,1,osk.symbols); dirty = true; }
    const int now_cols = fp::keyboard_columns(osk.cy,osk.symbols);
    if (osk.cx >= now_cols) osk.cx = now_cols - 1;
    if (edge[2]) { osk.cx = (osk.cx + now_cols - 1) % now_cols; dirty = true; }
    if (edge[3]) { osk.cx = (osk.cx + 1) % now_cols; dirty = true; }
    if (edge[4]) osk_press();
    if (edge[7]) osk_backspace();
    if (triangle_edge) { osk.upper = !osk.upper; dirty = true; }
    if (edge[6]) { osk_finish(false); return true; }
    // Circle deletes, the way a phone keyboard's back key does; it closes the keyboard only once the text is empty
    // (on a Steam Deck the first B press after typing a letter closed it and threw the letter away).
    if (edge[5] && !osk_backspace()) { osk_finish(true); return true; }
    if (!osk.active) return true;

    if (!dirty) { panel.clear(); return true; }
    dirty = false;
    panel = "KEYBOARD\n";
    return true;
}

std::string wrapped(const std::string& text, unsigned max_lines = 2) {
    std::string rest = text, out;
    for (unsigned line = 0; !rest.empty() && line < max_lines; ++line) {
        size_t n = std::min<size_t>(54, rest.size());
        if (n < rest.size()) {
            const auto space = rest.rfind(' ', n);
            if (space != std::string::npos && space > 20) n = space;
        }
        std::string part = rest.substr(0, n);
        rest.erase(0, n);
        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        if (!rest.empty() && line + 1 == max_lines) part = part.substr(0, 51) + "...";
        out += part + "\n";
    }
    return out;
}

std::string prompt_style() {
#if defined(__APPLE__) && TARGET_OS_IOS
    if (fp_touch_controls_visible()) return "PLAYSTATION";
#endif
    auto style=find("button_prompts")->value;
#if defined(__APPLE__) && TARGET_OS_IOS
    if(style=="AUTO") {
        const int family=fp_touch_controller_family();
        if(family==1) return "PLAYSTATION";
        if(family==2) return "XBOX";
        if(family==3) return "NINTENDO";
    }
#endif
#if SDL_VERSION_ATLEAST(2,0,14)
    if(style=="AUTO") for(int i=0;i<SDL_NumJoysticks();++i) {
        if(!SDL_IsGameController(i) || SDL_JoystickIsVirtual(i)) continue;
        const auto type=SDL_GameControllerTypeForIndex(i);
        if(type==SDL_CONTROLLER_TYPE_XBOX360 || type==SDL_CONTROLLER_TYPE_XBOXONE) return "XBOX";
        if(type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO) return "NINTENDO";
#if SDL_VERSION_ATLEAST(2,24,0)
        if(type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) return "NINTENDO";
#endif
        return "PLAYSTATION";
    }
#endif
    return style=="AUTO"?"PLAYSTATION":style;
}
std::string format_prompts(std::string out,bool pad) {
    auto replace=[&](const std::string& from,const std::string& to) {
        size_t pos=0;while((pos=out.find(from,pos))!=std::string::npos) {out.replace(pos,from.size(),to);pos+=to.size();}
    };
    if(pad) {
        replace("ENTER/CROSS", "CROSS"); replace("ESC/CIRCLE", "CIRCLE");
        replace("ENTER TO TYPE", "CROSS TO TYPE"); replace("ENTER ACCEPT", "CROSS ACCEPT");
        replace("ENTER UNBLOCK", "CROSS UNBLOCK");
        replace("F3 HIDE   F1 MENU", "MENU > DIAGNOSTICS TO HIDE");
        replace("F2 OR GUIDE", "GUIDE");
        replace("F1 CLOSE", "SELECT+START CLOSE");
        replace("F1 >", "SELECT+START >");
#if defined(__APPLE__) && TARGET_OS_IOS
        if(fp_touch_controls_visible()) {
            replace("SELECT+START CLOSE", "MENU CLOSE"); replace("SELECT+START >", "MENU >");
            replace("GUIDE OPENS IT IN GAME", "OPEN FROM GAME > TROPHIES");
            replace("GUIDE: DETAILS", "MENU > GAME > TROPHIES");
        }
#endif
    }
    if (pad && find("menu_button")->value!="SELECT+START") {
        replace("SELECT+START",find("menu_button")->value);
    }
    if(pad && prompt_style()!="PLAYSTATION") {
        const bool xbox=prompt_style()=="XBOX";
        bool swap=false;
#ifdef SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS
        swap=!xbox && !SDL_GetHintBoolean(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS,SDL_TRUE);
#endif
        for(auto replacement:{std::pair<std::string,std::string>{"CROSS",swap?"B":"A"},{"CIRCLE",swap?"A":"B"},
            {"SQUARE",swap?"Y":"X"},{"TRIANGLE",swap?"X":"Y"},{"L1/R1",xbox?"LB/RB":"L/R"},
            {"SELECT+START CLOSE",xbox?"VIEW+MENU CLOSE":"MINUS+PLUS CLOSE"},{"SELECT+START >",xbox?"VIEW+MENU >":"MINUS+PLUS >"},
            {"SELECT CLOSE",xbox?"VIEW CLOSE":"MINUS CLOSE"},{"SELECT >",xbox?"VIEW >":"MINUS >"},
            {"START DONE",xbox?"MENU DONE":"PLUS DONE"}}) {
            size_t pos=0;while((pos=out.find(replacement.first,pos))!=std::string::npos) { out.replace(pos,replacement.first.size(),replacement.second);pos+=replacement.second.size(); }
        }
    }
    return out;
}

std::string render() {
    const std::string heading = fp::player_control_state().ai ? "SETTINGS  AI ACTIVE" : "SETTINGS";
    const std::string close = format_prompts("F1 CLOSE",controller_hints);
    std::string out = heading + std::string(54 - heading.size() - close.size(), ' ') + close + "\n";
    for (int t = 0; t < kTabCount; ++t) out += t == tab ? std::string("[") + kTabs[t] + "] " : std::string(" ") + kTabs[t] + "  ";
    out += "\n";
    if (page_id()=="ONLINE") {
        const std::string connection=!on("online")?"OFFLINE":network_busy()?"REQUEST PENDING":np_psnr_connected()?"CONNECTED":"NOT CONNECTED";
        out+=wrapped(connection+"  "+status_text(),1);
    }
    if (!subpage.empty()) {
        std::string breadcrumb = subpage;
        breadcrumb.replace(breadcrumb.find('/'), 1, " > ");
        out += breadcrumb + "\n";
    }
    out += "------------------------------------------------------\n";
    if (confirm_action) {
        out += wrapped(confirm_title, 3) + "\n";
        out += confirm_yes ? "  CANCEL                 > CONFIRM\n" : "> CANCEL                   CONFIRM\n";
        out += format_prompts("\nLEFT/RIGHT CHOOSE   ENTER/CROSS SELECT\nESC/CIRCLE CANCEL\n",controller_hints);
        return out;
    }
    if (page_id()=="TOOLS/SELFTEST") {
        const auto test=fp::self_test_status();
        out += wrapped(test.result + (test.running ? "  " + std::to_string(test.elapsed) + "/" + std::to_string(test.duration) + " SECONDS" : ""),1);
    }
    if (friends_page()) out += render_friends();
    else {
        auto list = page_items();
        const int total = static_cast<int>(list.size()) + (can_reset() ? 1 : 0);
        int& r = selected_row();
        r = std::max(0, std::min(r, total - 1));
        if (!list.empty() && r == 0 && !std::strcmp(list[0]->id, "acct_status")) r = 1;
        const int kRows = on_value("ui_scale", "LARGE") ? 6 : 10;
        int first = 0, last = total;
        if (total > kRows) { first = std::max(0, std::min(r - kRows / 2, total - kRows)); last = first + kRows; }
        if (first > 0) out += "  ^ MORE ABOVE\n";
        for (int i = first; i < last; ++i) {
            Item* it = i < static_cast<int>(list.size()) ? list[i] : nullptr;
            const bool disabled = it && !disabled_reason(*it).empty();
            std::string line = i == r ? "> " : disabled ? "- " : "  ";
            std::string label = it ? it->label : "RESTORE PAGE DEFAULTS";
            std::string value;
            if (it) {
                value = (editing && i == r) ? edit_buffer + "_" : (capturing && i == r) ? "PRESS A KEY" : it->value;
                if (it->kind == Kind::Secret) value = std::string((editing && i == r) ? edit_buffer.size() : it->value.size(), '*') + ((editing && i == r) ? "_" : "");
                if (!std::strcmp(it->id, "acct_status")) value = status_text();
                if (it->kind == Kind::Choice || it->kind == Kind::Toggle) value = "< " + value + " >";
                if (it->kind == Kind::Submenu) value = ">";
                if (it->kind == Kind::Action || (it->kind == Kind::Command && std::strcmp(it->id, "acct_status"))) value.clear();
                if (!std::strcmp(it->id,"act_freecam")) value=fp::player_control_state().free_camera?"ON":"OFF";
                if (it->kind == Kind::Runtime) {
                    const auto state = fp::player_control_state();
                    value = !state.available ? "MATCH ONLY" : state.pending ? (state.requested_ai ? "TO AI..." : "TO YOU...") : state.ai ? "< AI >" : "< YOU >";
                }
                if(value.size()>26) value="..."+value.substr(value.size()-23);
                if (it->restart) label += " *";
            }
            line += label;
            if (line.size() + value.size() + 1 < 54) line += std::string(54 - line.size() - value.size(), ' ');
            else line += ' ';
            out += (line + value).substr(0, 54) + "\n";
        }
        if (last < total) out += "  v MORE BELOW\n";
    }
    out += "------------------------------------------------------\n";
    auto list = page_items();
    if (!list.empty() && selected_row() == 0 && !std::strcmp(list[0]->id, "acct_status")) selected_row() = 1;
    Item* current = selected_row() >= 0 && selected_row() < int(list.size()) ? list[selected_row()] : nullptr;
    // An action result replaces the contextual hint until selection changes.
    // Disabled actions often return that same hint, so they need only one slot.
    out += wrapped(!notice.empty() ? notice : friends_page() ?
        "SELECT ACCEPTS REQUESTS OR OPENS ADD FRIEND" : help_text(current));
    if (friends_page()) out += format_prompts(controller_hints ? "CROSS SELECT  SQUARE REMOVE  TRIANGLE BLOCK\n" : "ENTER SELECT  D REMOVE/DECLINE  B BLOCK\n",controller_hints);
    else out += format_prompts(controller_hints ? "D-PAD/STICK MOVE/CHANGE  CROSS SELECT\n" : "ARROWS MOVE/CHANGE  ENTER SELECT\n",controller_hints);
    out += format_prompts(controller_hints ? "CIRCLE BACK  L1/R1 TABS  SELECT+START CLOSE\n" : "ESC BACK  Q/E TABS  F1 CLOSE\n",controller_hints);
    if (restart_needed) out += "* PENDING CHANGES APPLY AFTER RESTART\n";
    return out;
}
}  // namespace

namespace fp {
bool settings_menu_open() { return is_open; }
bool settings_controller_hints() { return controller_hints; }
std::string settings_prompt_text(std::string text) { return format_prompts(std::move(text),controller_hints); }

void settings_menu_toggle() {
    is_open = !is_open;
    editing = capturing = false;
    notice.clear();
    confirm_action = {}; confirm_title.clear();
    dirty = true;
    g_fp_pad_blocked = is_open ? 1 : 0;
    if (!is_open) save();
}

int settings_key(const char* button) {
    std::string id = std::string("key_") + button;
    Item* i = find(id.c_str());
    const SDL_Scancode s = i ? SDL_GetScancodeFromName(i->value.c_str()) : SDL_SCANCODE_UNKNOWN;
    return s == SDL_SCANCODE_UNKNOWN ? 0 : static_cast<int>(s);
}

// Frames shown per guest frame: 1 off, 2 or 4. The guest then runs at its original 30 Hz and the
// backend synthesizes the frames in between (rsx_vulkan_backend_set_interp).
int settings_interp() {
    const std::string v = settings_get("interp");
    return v == "2X" ? 2 : v == "4X" ? 4 : 1;
}

// D3D12 is Windows only. FP_GFX=d3d12|vulkan beats the saved setting.
bool settings_renderer_d3d12() {
#ifdef _WIN32
    if (const char* e = std::getenv("FP_GFX")) return !_stricmp(e, "d3d12");
    Item* i = find("renderer");
    return i && i->value == "D3D12";
#else
    return false;
#endif
}

// Frames per second the host paces to: > 0 a fixed limit, 0 unlimited, < 0 the host's own 60 or 30 choice.
// In a lock mode the game steps dt = 1 / lock per frame, so speed is fps / lock and any cap different from the lock changes
// it (lock 60 at 50 fps ran at 0.8x). Time based mode takes dt from the frame time, so it holds 1.00x at any rate: AUTO
// can then follow what the machine manages. A lock mode keeps AUTO at its lock.
double settings_fps_cap() {
    if (settings_interp() > 1) return 30.0;
    // A lock mode only runs at the right speed when the frame rate is the lock: the limit follows it, whatever FRAME LIMIT says.
    { const std::string t = settings_get("timing"); if (t == "30 FPS LOCK") return 30.0; if (t == "60 FPS LOCK") return 60.0; }
    Item* i = find("fps_cap");
    if (i && i->value == "AUTO") {
        const std::string t = settings_get("timing");
        const int pct = std::atoi(settings_get("speed").c_str());
        if (t == "30 FPS LOCK") return 30.0;
        if (t == "60 FPS LOCK") return 60.0;
        return pct == 100 || pct <= 0 ? -1.0 : 30.0;
    }
    if (!i || i->value == "UNLIMITED") return 0.0;
    return std::atof(i->value.c_str());
}

// QrkGfx_FrameRateLock the game should run with: 0 is its time-based mode, otherwise dt = 1/lock per frame.
// The game steps dt = 1 / lock per frame, so it runs at fps / lock times its normal speed. A speed above 100% is a
// lower lock: 30 at 200% becomes 15. Time based mode (lock 0) is wall clock and cannot be sped up, so it takes the
// frame limit as its base while a speed is set. The guest reads the lock every frame, no restart needed.
float settings_frame_lock() {
    const std::string t = settings_get("timing");
    const int pct = std::atoi(settings_get("speed").c_str());
    const bool sped = pct > 0 && pct != 100;
    float lock = settings_interp() > 1 ? 30.f : t == "30 FPS LOCK" ? 30.f : t == "60 FPS LOCK" ? 60.f : 0.f;
    if (lock == 0.f && sped) { const double cap = settings_fps_cap(); lock = cap > 0 ? (float)cap : 60.f; }
    if (lock > 0.f && sped) lock = std::max(1.f, std::floor(lock * 100.f / pct + 0.5f));
    return lock;
}

bool settings_stats_visible() { return find("stats")->value!="OFF"; }

// Appended to the window title: a friend request waits until the FRIENDS tab is opened.
std::string settings_title_suffix() {
    if (!unseen_requests) return std::string();
    std::lock_guard<std::mutex> lock(push_mutex);
    return " | " + push_text + " (F1, FRIENDS)";
}

bool settings_set(const char* id, const std::string& value) {
    init_items();
    Item* i = find(id);
    if (!i) return false;
    if (i->kind == Kind::Submenu || !disabled_reason(*i).empty()) return false;
    const std::string normalized=std::string(id)=="stats" && value=="ON"?"DETAILED":value;
    if(i->kind==Kind::Choice && std::find(i->choices.begin(),i->choices.end(),normalized)==i->choices.end()) return false;
    if (i->kind == Kind::Runtime) {
        if (value != "ON" && value != "OFF" && value != "AI" && value != "YOU") return false;
        const bool accepted = player_control_request(value == "ON" || value == "AI");
        dirty = true;
        return accepted;
    }
    if (i->value == normalized && !std::strcmp(id, "resolution")) return true;
    const std::string previous = i->value;
    i->value = normalized;
    return changed(*i, previous);
}

bool settings_action(const char* id) {
    init_items();
    Item* i = find(id);
    if (!i || i->kind != Kind::Action) return false;
    if (!disabled_reason(*i).empty()) return false;
    activate(i);
    return true;
}

std::string settings_get(const char* id) {
    init_items();
    Item* i = find(id);
    if (i && i->kind == Kind::Runtime) return player_control_state().ai ? "ON" : "OFF";
    return i ? i->value : std::string();
}

void settings_stats_set(bool value) {
    if (Item* i = find("stats")) { i->value = value ? "COMPACT" : "OFF"; save(); dirty = true; }
}

bool settings_menu_frame(std::string& panel) {
    network_poll();
    static bool open_from_env = std::getenv("FP_SETTINGS_OPEN") != nullptr;
    if (open_from_env) { open_from_env = false; if (!is_open) settings_menu_toggle(); }
    // Headless tests: FP_MENU_TAB=ACCOUNT shows that tab when the menu opens; FP_ACCOUNT_AUTO=register|login,name,password
    // runs that menu command once at start (the same code as the menu rows, with its result in the notice).
    {
        static bool done = false;
        if (!done) {
            done = true;
            if (const char* t = std::getenv("FP_MENU_TAB")) {
                for (int i = 0; i < kTabCount; ++i) if (!std::strcmp(kTabs[i], t)) tab = i;
#ifndef FP_DEBUG_MENU
                if (!std::strcmp(t, "DEBUG")) tab = 5;
#endif
                if (!std::strcmp(t, "NETWORK")) tab = 4;
                if (!std::strcmp(t, "ACCOUNT")) { tab = 4; subpage = "ONLINE/ACCOUNT"; }
                if (!std::strcmp(t, "FRIENDS")) { tab = 4; subpage = "ONLINE/FRIENDS"; }
            }
            if (const char* page = std::getenv("FP_MENU_PAGE")) {
                for (const auto& it : menu_items()) if (it.kind == Kind::Submenu && it.def == page) {
                    subpage = page;
                    for (int i = 0; i < kTabCount; ++i) if (subpage.find(std::string(kTabs[i]) + '/') == 0) tab = i;
                }
            }
            if (const char* a = std::getenv("FP_ACCOUNT_AUTO")) {
                std::string spec = a;
                const size_t c1 = spec.find(','), c2 = spec.find(',', c1 + 1);
                if (c1 != std::string::npos && c2 != std::string::npos) {
                    find("tmp_name")->value = spec.substr(c1 + 1, c2 - c1 - 1);
                    find("tmp_pw")->value = spec.substr(c2 + 1);
                    account_command("cmd_" + spec.substr(0, c1));
                    std::fprintf(stderr, "[fp-account] %s\n", notice.c_str());
                }
            }
        }
    }
    static bool text_watcher=[] { SDL_AddEventWatch(text_event,nullptr);return true; }();
    (void)text_watcher;
    {
        std::string text;
        { std::lock_guard<std::mutex> lock(text_mutex); text.swap(pending_text); }
        if(osk.active) append_text(osk.text,text,osk.limit);
        else if(editing) {
            const auto list=page_items();const int row=selected_row();
            const size_t limit=friends_page()?16:row>=0 && row<int(list.size())?text_limit(*list[row]):28;
            append_text(edit_buffer,text,limit);
        } else SDL_StopTextInput();
    }
    if (pending_fullscreen) { pending_fullscreen = false; apply_fullscreen(); }
    int has_focus=0;
    const Uint8* keys = fp_menu_keyboard_state(&has_focus);
    const bool focused = has_focus!=0;
    static bool fullscreen_enter = false;
    const bool enter = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];
    if (!enter) fullscreen_enter = false;
    if (focused && enter && (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]) && !fullscreen_enter) {
        fullscreen_enter = true;
        settings_set("fullscreen", on("fullscreen") ? "OFF" : "ON");
    }
    if (fullscreen_enter) {
        keyedge.down(SDL_SCANCODE_RETURN, keys);
        keyedge.down(SDL_SCANCODE_KP_ENTER, keys);
        panel.clear();
        return is_open || osk.active;
    }
#if defined(__APPLE__) && TARGET_OS_IOS
    // Touch is the default on a phone. A keyboard only gets keyboard prompts
    // when one is actually attached; real pad input still overrides it below.
    static int previous_inputs=-1;
    int pads=0;
    for(int i=0;i<SDL_NumJoysticks();++i) if(SDL_IsGameController(i) && !SDL_JoystickIsVirtual(i)) ++pads;
    const int inputs=(fp_touch_keyboard_connected()?1:0) | (pads?2:0) | (fp_touch_controls_visible()?4:0) |
                     (fp_touch_controller_family()<<4);
    if(inputs!=previous_inputs) {controller_hints=!(inputs&1) || (inputs&2);previous_inputs=inputs;dirty=true;}
    if(!fp_touch_keyboard_connected()) controller_hints=true;
#endif
    if (osk_frame(panel, keys, focused)) return true;   // the game's text entry owns the overlay while it is up
    if (trophy_frame(panel, keys, focused)) return true;   // so does a trophy screen
    const auto shortcut=find("menu_button")->value;
    const bool combo = shortcut=="SELECT" ? pad_down(CELL_PAD_CTRL_SELECT) : shortcut=="L3" ? pad_down(1u<<1) : shortcut=="R3" ? pad_down(1u<<2) : pad_down(CELL_PAD_CTRL_SELECT) && pad_down(CELL_PAD_CTRL_START);
    bool toggle = (focused && keyedge.down(SDL_SCANCODE_F1, keys)) || (combo && !combo_prev);
#if defined(__APPLE__) && TARGET_OS_IOS
    const bool touch_menu=fp_touch_take_menu();
    if(touch_menu) {controller_hints=true;dirty=true;}
    toggle = touch_menu || toggle;
#endif
    combo_prev = combo;
    if (toggle) settings_menu_toggle();
    if (!is_open) return false;
    {
        const auto state = player_control_state();
        const auto test=self_test_status();
        const std::string signature = test.result + std::to_string(test.elapsed) + std::to_string(state.available) + std::to_string(state.ai) +
            std::to_string(state.pending) + std::to_string(state.requested_ai) + state.error;
        static std::string previous;
        if (previous != signature) { previous = signature; dirty = true; }
    }

    static long long online_second=-1;
    if(page_id().rfind("ONLINE",0)==0 && mono_ms()/1000!=online_second) { online_second=mono_ms()/1000;dirty=true; }
    if (friends_page()) { refresh_friends(); if (unseen_requests) { unseen_requests = 0; dirty = true; } }
    auto list = page_items();
    if (!list.empty() && selected_row() == 0 && !std::strcmp(list[0]->id, "acct_status")) selected_row() = 1;
    Item* current = selected_row() >= 0 && selected_row() < static_cast<int>(list.size()) ? list[selected_row()] : nullptr;
    if (friends_page() && editing) {
        friend_text(keys);
    } else if (editing && current) {
        handle_text(keys, *current);
    } else if (capturing && current) {
        handle_capture(keys, *current);
    } else {
        bool nav[NAV_COUNT] = {false};
        const unsigned pad_buttons[NAV_COUNT] = {
            CELL_PAD_CTRL_UP, CELL_PAD_CTRL_DOWN, CELL_PAD_CTRL_LEFT, CELL_PAD_CTRL_RIGHT,
            CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_L1, CELL_PAD_CTRL_R1};
        for (int n = 0; n < NAV_COUNT; ++n) {
            const bool now = pad_down(pad_buttons[n]);
            if (now && !pad_prev[n]) nav[n] = true;
            if (nav[n]) { controller_hints = true; dirty = true; }
            pad_prev[n] = now;
        }
        if (focused) {
            nav[UP] |= keyedge.down(SDL_SCANCODE_UP, keys);
            nav[DOWN] |= keyedge.down(SDL_SCANCODE_DOWN, keys);
            nav[LEFT] |= keyedge.down(SDL_SCANCODE_LEFT, keys);
            nav[RIGHT] |= keyedge.down(SDL_SCANCODE_RIGHT, keys);
            nav[OK] |= keyedge.down(SDL_SCANCODE_RETURN, keys) | keyedge.down(SDL_SCANCODE_Z, keys);
            nav[BACK] |= keyedge.down(SDL_SCANCODE_ESCAPE, keys) | keyedge.down(SDL_SCANCODE_X, keys);
            nav[TAB_PREV] |= keyedge.down(SDL_SCANCODE_Q, keys) | keyedge.down(SDL_SCANCODE_PAGEUP, keys);
            nav[TAB_NEXT] |= keyedge.down(SDL_SCANCODE_E, keys) | keyedge.down(SDL_SCANCODE_PAGEDOWN, keys);
            for (int sc : {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_Q, SDL_SCANCODE_E})
                if (keys[sc]) { controller_hints = false; dirty = true; }
        }
        static bool repeating[4] = {};
        static long long repeat_at[4] = {};
        const SDL_Scancode directions[4] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
        const auto now = mono_ms();
        for (int n=0;n<4;++n) {
            const bool held=pad_down(pad_buttons[n]) || (focused && keys[directions[n]]);
            const bool allowed=n<2 || (current && current->kind==Kind::Choice && !confirm_action);
            if (!held) repeating[n]=false;
            else if (!repeating[n]) { repeating[n]=true; repeat_at[n]=now+450; }
            else if (allowed && now>=repeat_at[n]) { nav[n]=true; dirty=true; repeat_at[n]=now+110; }
        }
        if (confirm_action) {
            if (nav[LEFT] || nav[RIGHT] || nav[UP] || nav[DOWN]) { confirm_yes = !confirm_yes; dirty = true; }
            if (nav[BACK]) { confirm_action = {}; dirty = true; }
            else if (nav[OK]) {
                auto action = confirm_yes ? std::move(confirm_action) : std::function<void()>{};
                confirm_action = {}; dirty = true;
                if (action) action();
            }
            panel = format_prompts(render(),controller_hints); dirty = false; return true;
        }
        if (friends_page()) friends_nav(nav, keys, focused);
        else {
            const int total = static_cast<int>(list.size()) + (can_reset() ? 1 : 0);
            int& r = selected_row();
            if (nav[UP] || nav[DOWN]) {
                const int direction = nav[UP] ? -1 : 1;
                do { r = (r + total + direction) % total; }
                while (r < int(list.size()) && !std::strcmp(list[r]->id, "acct_status"));
                dirty = true; notice.clear();
            }
            current = r >= 0 && r < int(list.size()) ? list[r] : nullptr;
            if (nav[LEFT] && current) cycle(*current, -1);
            if (nav[RIGHT] && current) cycle(*current, 1);
            if (nav[OK]) activate(current);
        }
        if (nav[TAB_PREV]) { tab = (tab + kTabCount - 1) % kTabCount; subpage.clear(); dirty = true; notice.clear(); }
        if (nav[TAB_NEXT]) { tab = (tab + 1) % kTabCount; subpage.clear(); dirty = true; notice.clear(); }
        if (nav[BACK]) {
            if (!subpage.empty()) { subpage.clear(); notice.clear(); dirty = true; }
            else { settings_menu_toggle(); return false; }
        }
    }
    if (!dirty) { panel.clear(); return true; }
    dirty = false;
    panel = format_prompts(render(),controller_hints);
    return true;
}
}  // namespace fp

// Called by the cellOskDialog module on the guest thread; the menu thread picks the request up.
namespace fp { bool settings_osk_active() { return osk.active; } }
namespace fp {
KeyboardView settings_keyboard_view() {
    KeyboardView view;view.title=osk.prompt=="Tag"?"Player tag":osk.prompt;view.text=osk.text;view.limit=osk.limit;
    view.row=osk.cy;view.column=osk.cx;view.upper=osk.upper;view.symbols=osk.symbols;
    auto prompt=osk.prompt;for(char& c:prompt)c=char(std::toupper(static_cast<unsigned char>(c)));
    view.secret=prompt.find("PASSWORD")!=std::string::npos;
    view.controls=controller_hints ? format_prompts("CROSS SELECT   CIRCLE DELETE   TRIANGLE SHIFT   START DONE",true)
                                 : "ARROWS MOVE   ENTER SELECT   BACKSPACE DELETE";
    view.hint=controller_hints ? format_prompts("L1/R1 SYMBOLS   CIRCLE DELETE/BACK",true)
                              : "ESC CANCEL   TYPE OR CTRL+V TO PASTE";
    return view;
}
}

extern "C" void fp_osk_open(const char* prompt, const char* init, unsigned limit) {
    std::lock_guard<std::mutex> lock(osk.mutex);
    osk.prompt = prompt ? prompt : "";
    osk.text = init ? init : "";
    osk.limit = limit ? limit : 16;
    if(osk.text.size()>osk.limit)osk.text.resize(osk.limit);
    osk.requested = true;
}

extern "C" void fp_osk_close(void) {
    std::lock_guard<std::mutex> lock(osk.mutex);
    if (osk.active || osk.requested) osk.closed_by_guest = true;
}
