// Bridge between the game's own menu scripts and the host settings.
//
// The recomp pages added to Twiddly Knobs (tools/make_menu_patch.py) are ordinary Quark menu objects. Their
// rows run the console command `echo fp <verb> ...`. The game's `echo` handler (guest func_009413C0, a bare
// return in the retail build) is patched by tools/lift_patches.py ("menubridge") to call fp_menu_echo(), and then
// to run every command queued by fp_menu_reply() through the game's console (func_001DE460), which is how the
// page shows the new value: `sym set FpV_<id> <value>`.
//
//   echo fp sync <page>         queue the current value of every row of a page
//   echo fp cycle <id> <+1|-1>  step a setting through its choices (settings_set does the real work and saves)
//   echo fp act <id>            run an action row (opening the developer overlay)
//   echo anything else          untouched (the cheat patch and the retail scripts use echo as a log line)
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "ppu_recomp.h"
#include "settings.h"
using namespace fp;
extern "C" void fp_game_stats_collect(void);

namespace {

struct Row {
    const char* page;
    const char* id;                      // settings.cpp item id
    std::vector<const char*> choices;    // empty: an action row
};

const std::vector<const char*> kOnOff{"ON", "OFF"};

const std::vector<Row>& rows() {
    static const std::vector<Row> r{
        {"graphics", "vsync", {"ON", "ADAPTIVE", "OFF"}},
        {"graphics", "fps_cap", {"AUTO", "30", "45", "60", "90", "120", "UNLIMITED"}},
        {"graphics", "resolution", {"720P", "1080P"}},
        {"graphics", "render_scale", {"0.5X", "0.67X", "0.75X", "1X", "1.5X", "2X", "3X"}},
        {"graphics", "fullscreen", kOnOff},
        {"audio", "volume", {"0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100"}},
        {"audio", "mute", kOnOff},
        {"audio", "limiter", kOnOff},
        {"gameplay", "skip_intro", kOnOff},
        {"gameplay", "skip_howto", kOnOff},
        {"gameplay", "ai_nav_guard", kOnOff},
        {"advanced", "stats", {"OFF", "COMPACT", "DETAILED"}},
        {"advanced", "hitch_log", {"OFF", "2", "5", "10", "20"}},
        {"advanced", "overlay", {}},
    };
    return r;
}

const Row* find_row(const std::string& id) {
    for (const Row& row : rows()) if (id == row.id) return &row;
    return nullptr;
}

std::deque<std::string> g_replies;

std::string value_of(const Row& row) {
    std::string v = settings_get(row.id);
    for (char& c : v) c = (char)std::toupper((unsigned char)c);
    return v.empty() ? "-" : v;     // a '-' tells the page the setting is unavailable on this build
}

void queue_value(const Row& row) {
    if (row.choices.empty()) return;
    // The page draws the symbol with _DrawUText, which looks the value up as a language key (FpVal_<value>; see symbols() in
    // tools/make_menu_patch.py), so the symbol holds the key and not the text.
    std::string key = "FpVal_";
    for (char c : value_of(row)) key += std::isalnum((unsigned char)c) ? c : '_';
    g_replies.push_back(std::string("sym set FpV_") + row.id + " " + key);
}

std::vector<std::string> tokens(uint32_t args) {
    std::vector<std::string> out;
    if (args < 0x10000 || args > 0xdffff000) return out;
    const uint32_t count = vm_read32(args + 0x900);
    if (count > 16) return out;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t offset = vm_read32(args + 0x800 + 4 * i);
        if (offset >= 0x800) break;
        std::string t;
        for (uint32_t j = offset; j < 0x800 && t.size() < 64; ++j) {
            const char c = (char)vm_read8(args + j);
            if (!c) break;
            t += c;
        }
        out.push_back(t);
    }
    return out;
}

std::string clean(std::string v) {   // text for a quoted sym value: no quotes, no control characters
    for (char& c : v) if (c == '"' || (unsigned char)c < 32) c = ' ';
    if (v.size() > 56) v.resize(56);
    return v;
}

void queue_text(const char* sym, const std::string& v) { g_replies.push_back(std::string("sym set ") + sym + " \"" + clean(v) + "\""); }

int g_poll_signed = -1;   // account state last reported to the page: -1 unknown, 0 signed out, 1 signed in
bool g_was_busy = false;

// What the Account page shows: status, the typed name, the masked password, the last result, and (for the sign-in gate)
// FpAcctGo=1 when a sign-in has just succeeded so the page can carry on to the online menu.
void queue_account() {
    const std::string who = settings_account();
    queue_text("FpV_acct_name", who.empty() ? (settings_get("tmp_name").empty() ? "TAP TO ENTER" : settings_get("tmp_name")) : who);
    queue_text("FpV_acct_pw", settings_get("tmp_pw").empty() ? "TAP TO ENTER" : "ENTERED");
    const bool busy = settings_busy();
    std::string note = busy ? "WORKING..." : settings_notice();
    for (char& c : note) c = (char)std::toupper((unsigned char)c);
    if (!busy && note.rfind("SIGNED IN", 0) == 0) note.clear();   // the page itself says it
    if (note.empty()) note = who.empty() ? "ENTER NAME AND PASSWORD" : "YOU CAN PLAY ONLINE";
    if (note.size() > 34) note.resize(34);
    queue_text("FpV_acct_note", note);
    const int now = who.empty() ? 0 : 1;
    g_replies.push_back(std::string("sym set FpAcctIn ") + (now ? "1" : "0"));
    if (g_poll_signed == 0 && now == 1) g_replies.push_back("sym set FpAcctGo 1");
    g_poll_signed = now;
    g_was_busy = busy;
}

}  // namespace

// Called from the patched echo handler with the handler's context (r4 = parsed command).
void fp_menu_echo(ppu_context* ctx) {
    const auto t = tokens((uint32_t)ctx->gpr[4]);
    if (t.size() < 3 || t[1] != "fp") return;
    const std::string& verb = t[2];
    if (verb == "sync" && t.size() >= 4) {
        for (const Row& row : rows()) if (t[3] == row.page) queue_value(row);
        std::printf("[fp-menu] sync %s (%zu replies)\n", t[3].c_str(), g_replies.size());
    } else if (verb == "cycle" && t.size() >= 5) {
        const Row* row = find_row(t[3]);
        if (!row || row->choices.empty()) { std::printf("[fp-menu] cycle: unknown row %s\n", t[3].c_str()); return; }
        const int dir = std::atoi(t[4].c_str()) < 0 ? -1 : 1;
        const std::string now = value_of(*row);
        int at = -1;
        for (size_t i = 0; i < row->choices.size(); ++i) if (now == row->choices[i]) at = (int)i;
        const int n = (int)row->choices.size();
        // A value outside the list (for example a config-file value) starts from the first or last choice.
        const int next = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
        const bool ok = settings_set(row->id, row->choices[next]);
        std::printf("[fp-menu] cycle %s %s -> %s (%s)\n", row->id, now.c_str(), row->choices[next], ok ? "applied" : "refused");
        queue_value(*row);
    } else if (verb == "text" && t.size() >= 4) {   // open the keyboard for tmp_name / tmp_pw / tmp_email
        const std::string id = t[3];
        if (id != "tmp_name" && id != "tmp_pw" && id != "tmp_email") return;
        const std::string env = "FP_MENU_TEST_" + id;   // tests: take the text from the environment instead of the keyboard
        if (const char* v = std::getenv(env.c_str())) settings_text_set(id.c_str(), v);
        else settings_text_entry(id.c_str());
        std::printf("[fp-menu] text entry %s\n", id.c_str());
    } else if (verb == "cmd" && t.size() >= 4) {      // sign in / sign out
        const std::string id = "cmd_" + t[3];
        const bool ok = settings_command(id.c_str());
        std::printf("[fp-menu] account %s (%s)\n", t[3].c_str(), ok ? "started" : "refused");
        g_poll_signed = settings_account().empty() ? 0 : 1;
    } else if (verb == "url" && t.size() >= 4) {
        // The typed name (letters, digits - and _ only) goes along as ?name= so the page starts with it filled in; never the password.
        std::string query, typed = settings_get("tmp_name");
        bool clean_name = !typed.empty() && typed.size() <= 16;
        for (char c : typed) if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') clean_name = false;
        if (clean_name) query = "?name=" + typed;
        const bool ok = t[3] == "register" && settings_open_url("join_url", query);
        std::printf("[fp-menu] open registration page: %s\n", ok ? "browser started" : "failed");
        queue_text("FpV_acct_note", ok ? "BROWSER OPENED. SIGN IN HERE" : "OPEN JOIN.SRSLY.COOL IN A BROWSER");
    } else if (verb == "stats") {                     // trophy progress counters (src/game_stats.cpp)
        fp_game_stats_collect();
    } else if (verb == "poll" && t.size() >= 4 && t[3] == "account") {
        queue_account();
    } else if (verb == "gate") {
        // The Play With Others script asks before connecting: needs an account unless online play is off or anonymous play
        // is allowed on purpose (FP_ALLOW_ANON=1, for a LAN test server).
        const bool anon = std::getenv("FP_ALLOW_ANON") && std::getenv("FP_ALLOW_ANON")[0] == '1';
        const bool online = settings_get("online") == "ON";
        const bool need = online && !anon && settings_account().empty();
        g_replies.push_back(std::string("sym set FpNeedLogin ") + (need ? "1" : "0"));
        std::printf("[fp-menu] gate: online=%d account=%s -> %s\n", online, settings_account().empty() ? "none" : "yes", need ? "sign-in page" : "connect");
    } else if (verb == "act" && t.size() >= 4) {
        if (t[3] == "overlay") { settings_menu_toggle(); std::printf("[fp-menu] developer overlay toggled\n"); }
        else if (t[3] == "ai_on" || t[3] == "ai_off") {   // the AI plays the local player (background videos, tests)
            const bool ok = settings_set("ai_control", t[3] == "ai_on" ? "ON" : "OFF");
            std::printf("[fp-menu] player control %s (%s)\n", t[3] == "ai_on" ? "AI" : "you", ok ? "accepted" : "refused");
        }
    }
    std::fflush(stdout);
}

// The console keeps the text pointer and runs the command later (not at once), so every reply needs memory of its own that
// stays valid: a ring of 64 slots of 256 bytes in a block taken from the game's own allocator (sys_memory_allocate) on first use.
extern "C" int64_t sys_memory_allocate(ppu_context* ctx);
namespace { uint32_t g_slots; unsigned g_next; }

static void ensure_slots(ppu_context* ctx) {
    if (g_slots) return;
    const uint64_t r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    const uint32_t scratch = (uint32_t)ctx->gpr[4] + 0x3F0;     // room for the u32 result inside the parsed command text
    ctx->gpr[3] = 0x10000; ctx->gpr[4] = 0; ctx->gpr[5] = scratch;
    const int64_t rc = sys_memory_allocate(ctx);
    g_slots = rc == 0 ? vm_read32(scratch) : 0;
    ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5;
    std::printf("[fp-menu] reply memory %s 0x%08X\n", g_slots ? "at" : "unavailable, rc", g_slots ? g_slots : (unsigned)rc);
}

// Next command for the game console, copied to guest memory; its address goes to *text. 0 when the queue is empty.
int fp_menu_reply(ppu_context* ctx, uint32_t* text) {
    if (g_replies.empty()) return 0;
    ensure_slots(ctx);
    if (!g_slots) { g_replies.clear(); return 0; }
    const std::string cmd = g_replies.front();
    g_replies.pop_front();
    if (cmd.size() + 1 > 256) return 0;
    const uint32_t at = g_slots + 256 * (g_next++ % 64);
    for (size_t i = 0; i <= cmd.size(); ++i) vm_write8(at + (uint32_t)i, (uint8_t)cmd[i]);
    *text = at;
    std::printf("[fp-menu] reply \"%s\"\n", cmd.c_str());
    return 1;
}
