// Script patches for the game's own archive. Builds addons/patch.qrk from the user's siege.qrk at start-up, so no
// script text is stored in the repository or the executable, only the edits. The game loads addons/patch.qrk over
// siege.qrk. Same edits as tools/make_cheat_patch.py.
#include "settings.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define mkdir(path, mode) _mkdir(path)
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <zlib.h>

namespace {
constexpr int kRulesVersion = 18;
constexpr uint32_t kMagic = 0x004b5251;

uint32_t rd32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
void wr32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char(v >> (8 * i))); }

std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

// One member of a QRK v1 archive: header 6 u32, names, 21-byte records, data. Reads only the index and the member.
bool read_member(const std::string& archive, const std::string& name, std::string& out) {
    FILE* f = std::fopen(archive.c_str(), "rb");
    if (!f) return false;
    unsigned char head[24];
    bool ok = std::fread(head, 1, 24, f) == 24 && rd32(head) == kMagic && rd32(head + 4) == 1;
    std::string names, records;
    uint32_t count = 0, names_size = 0;
    if (ok) {
        count = rd32(head + 8); names_size = rd32(head + 20);
        ok = count < 1000000 && names_size < (1u << 28);
    }
    if (ok) {
        names.resize(names_size); records.resize(size_t(count) * 21);
        ok = std::fread(&names[0], 1, names_size, f) == names_size && std::fread(&records[0], 1, records.size(), f) == records.size();
    }
    for (uint32_t i = 0; ok && i < count; ++i) {
        const unsigned char* r = reinterpret_cast<const unsigned char*>(records.data()) + size_t(i) * 21;
        const uint32_t stored = rd32(r), offset = rd32(r + 4), size = rd32(r + 8), nameoff = rd32(r + 16);
        if (nameoff >= names.size() || names.compare(nameoff, name.size() + 1, name + '\0') != 0) continue;
        std::string blob(stored, '\0');
        ok = std::fseek(f, long(offset), SEEK_SET) == 0 && std::fread(&blob[0], 1, stored, f) == stored;
        if (ok && r[20]) {
            std::string raw(size, '\0');
            uLongf len = size;
            ok = uncompress(reinterpret_cast<Bytef*>(&raw[0]), &len, reinterpret_cast<const Bytef*>(blob.data()), stored) == Z_OK && len == size;
            blob.swap(raw);
        }
        if (ok) out = blob;
        std::fclose(f);
        return ok;
    }
    std::fclose(f);
    return false;
}

std::string build_archive(const std::vector<std::pair<std::string, std::string>>& items) {
    std::string names, records, data;
    std::vector<uint32_t> name_offsets;
    for (auto& it : items) { name_offsets.push_back(uint32_t(names.size())); names += it.first; names.push_back('\0'); }
    const uint32_t data_offset = 24 + uint32_t(names.size()) + 21 * uint32_t(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        wr32(records, uint32_t(items[i].second.size())); wr32(records, data_offset + uint32_t(data.size()));
        wr32(records, uint32_t(items[i].second.size())); wr32(records, 0); wr32(records, name_offsets[i]);
        records.push_back('\0');
        data += items[i].second;
    }
    std::string out;
    wr32(out, kMagic); wr32(out, 1); wr32(out, uint32_t(items.size())); wr32(out, data_offset); wr32(out, 1); wr32(out, uint32_t(names.size()));
    return out + names + records + data;
}

bool replace_once(std::string& s, const std::string& from, const std::string& to) {
    const size_t at = s.find(from);
    if (at == std::string::npos) return false;
    s.replace(at, from.size(), to);
    return true;
}

// "Key = value" at the start of a line.
bool set_value(std::string& s, const std::string& key, const std::string& value) {
    const std::string needle = "\n" + key + " = ";
    size_t at = s.find(needle);
    if (at == std::string::npos) return false;
    at += needle.size();
    const size_t end = s.find_first_of("\r\n", at);
    s.replace(at, end == std::string::npos ? std::string::npos : end - at, value);
    return true;
}

const char kExtraBindings[] = R"(
LShoulder RShoulder X MultiPress {
echo God mode on
sym set LocalPlayerDamageTakenMod 0.0
}
LStick RStick X MultiPress {
echo God mode off
sym set LocalPlayerDamageTakenMod 1.0
}
LShoulder RShoulder Y MultiPress {
echo Damage given x10
sym set LocalPlayerDamageGivenMod 10.0
}
LStick RStick Y MultiPress {
echo Damage given x1
sym set LocalPlayerDamageGivenMod 1.0
}
)";

// FEMainOnline starts a bandwidth probe against Sony's servers, which are gone. The probe never completes and the
// game falls back to the title screen. Skip it and take the focus step of UIBWProbeDone.
const char kProbeLine[] = "cmd \"if sym OnlineIsRestricted == 0 net CheckBWProbe\"";
const char kProbeSkip[] = "cmd \"if sym OnlineIsRestricted == 0 pcmd 0.5 gameUI focusName $MenuMemoryOnline\"";

// All occurrences of from -> to.
void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}

// The offline player label is DefaultPlayerName in each language's data/local/<lang>/symbols.txt (UTF-16BE). With
// Online on the game takes the name from NP instead (fp_sysutil_tick in host.cpp starts the NP client at boot), so
// this rewrite is only the fallback for Online off.
std::string utf16be(const std::string& s) {
    std::string out;
    for (char c : s) { out.push_back('\0'); out.push_back(c); }
    return out;
}

const char* const kLanguages[] = {"chineses", "chineset", "danish", "dutch", "english", "finnish", "french", "german", "italian",
                                  "japanese", "korean", "norwegian", "polish", "portuguese", "russian", "spanish", "swedish", "uk"};

std::string game_dir() {
    if (const char* d = std::getenv("FP_GAME_DIR")) return d;
    if (const char* h = std::getenv("PS3_HDD0_ROOT")) return std::string(h) + "/game/NPUA80139/USRDIR";
    return "";
}

__attribute__((constructor(102))) void apply_script_patches() {
    const std::string dir = game_dir();
    if (dir.empty()) return;
    const std::string archive = dir + "/siege.qrk", target = dir + "/addons/patch.qrk", keyfile = target + ".key";
    const std::string timing = fp::settings_get("timing");
    const bool bindings = fp::settings_get("debug_bindings") == "ON";
    const std::string raw_name = fp::settings_get("name"), online = fp::settings_get("online");
    const bool skip_intro = fp::settings_get("skip_intro") != "OFF", skip_title = fp::settings_get("skip_title") != "OFF",
                since_howto = fp::settings_get("skip_howto") != "OFF";
    const std::string key = "rules=" + std::to_string(kRulesVersion) + " timing=" + timing + " bindings=" + (bindings ? "1" : "0") + " name=" + raw_name + " online=" + online + " skipintro=" + (skip_intro ? "1" : "0") + " skiptitle=" + (skip_title ? "1" : "0") + " skiphowto=" + (since_howto ? "1" : "0") + "\n";
    struct stat st;
    if (stat(archive.c_str(), &st) != 0) return;
    if (stat(target.c_str(), &st) == 0 && read_file(keyfile) == key) return;

    std::vector<std::pair<std::string, std::string>> items;
    std::string text;
    if (read_member(archive, "data/gamescripts/config.txt", text)) {
        text.insert(0, "\n");   // so the first line matches like the others
        const std::string lock = timing == "30 FPS LOCK" ? "30" : timing == "60 FPS LOCK" ? "60" : "0";
        set_value(text, "QrkGfx_FrameRateLock", lock);
        set_value(text, "QrkGfx_FrameRateLockPAL", lock);
        text.erase(0, 1);
        items.emplace_back("data/gamescripts/config.txt", text);
    }
    if (bindings && read_member(archive, "data/gamescripts/keybindings/controllerbindings.txt", text)) {
        for (size_t at; (at = text.find(" !Retail")) != std::string::npos;) text.erase(at, 8);
        text += kExtraBindings;
        items.emplace_back("data/gamescripts/keybindings/controllerbindings.txt", text);
    }
    // The save warning is the first screen and loads the game data, so it stays. It then chains through the Sony, Atomic,
    // PhysX and ESRB logos (about 35 s) and ends in the license screen unless the game thinks the license was accepted.
    // Mark it accepted and go to the menu loader that the last logo's onunload calls (FEEula), skipping all of it.
    if (skip_intro && read_member(archive, "data/menus/introsavewarning.txt", text)) {
        const std::string chain = "if BuildRegionJapan runNow loadmenu IntroGeneral-J.txt\r\nif !BuildRegionJapan runNow loadmenu IntroEpilepsy.txt";
        const std::string direct = "sym set EULAAcceptedUnsaved 1\r\nsym set EULAAccept FEEula\r\nsym set EULACancel FEEula\r\nmenuCmd FEEula";
        if (replace_once(text, chain, direct)) items.emplace_back("data/menus/introsavewarning.txt", text);
    }
    // Press Start on the title screen once its animation has played: MenuPressStart is what the button runs.
    if (skip_title && read_member(archive, "data/menus/menumaster.txt", text) &&
        replace_once(text, "}\r\nonunload {\r\nmenu3d remove MenuTitle", "pcmd 2.0 menuCmd MenuPressStart\r\n}\r\nonunload {\r\nmenu3d remove MenuTitle"))
        items.emplace_back("data/menus/menumaster.txt", text);
    // The "read how to play?" prompt shows while HowToPlayComplete is 0, and the game only sets it after all seven pages were
    // opened. It is also part of the saved local settings, so it comes back as 0 after each load. Set it when the main menu
    // loads, which is after the load and before the checks.
    if (since_howto && read_member(archive, "data/menus/menumaster.txt", text)) {
        // menumaster.txt may already be in items from the title patch: edit that copy.
        for (auto& it : items) if (it.first == "data/menus/menumaster.txt") { text = it.second; break; }
        const std::string check = "if sym HowToPlayComplete == 0 menuCmd HowToPlayEvaulate\r\n";
        if (replace_once(text, check, "sym set HowToPlayComplete 1\r\n" + check)) {
            bool found = false;
            for (auto& it : items) if (it.first == "data/menus/menumaster.txt") { it.second = text; found = true; }
            if (!found) items.emplace_back("data/menus/menumaster.txt", text);
        }
    }
    // The host overwrites the default font name "clean" in guest memory (fp_apply_name_font in host.cpp) and the literal
    // has room for five characters, so the longer font files get short aliases.
    for (const auto& alias : {std::pair<const char*, const char*>{"ff", "fatfingers"}, {"bp", "boopee"}, {"ffo", "fatfingersoutlined"}})
        if (read_member(archive, std::string("data/textures/fonts/") + alias.second + ".txt", text))
            items.emplace_back(std::string("data/textures/fonts/") + alias.first + ".txt", text);
    if (read_member(archive, "data/gamescripts/menucmds.txt", text) && replace_once(text, kProbeLine, kProbeSkip))
        items.emplace_back("data/gamescripts/menucmds.txt", text);
    // The player-count limit comes from the bandwidth probe's result, and with the probe gone every game above four
    // players is refused ("your current bandwidth will only allow..."). Lift the failed-probe limit to the game's
    // maximum, make the per-player cost tiny, and drop the validation on the custom game screen.
    if (read_member(archive, "data/gamescripts/console/autoexec.txt", text)) {
        replace_once(text, "sym init BwFailedNumPlayers 4", "sym init BwFailedNumPlayers 32");
        replace_once(text, "sym init UpBitsPerSecPerUser 40000", "sym init UpBitsPerSecPerUser 1");
        replace_once(text, "sym init UpBitsPerSecPerUserSquared 500", "sym init UpBitsPerSecPerUserSquared 1");
        items.emplace_back("data/gamescripts/console/autoexec.txt", text);
    }
    if (read_member(archive, "data/gamescripts/menufinal/menucustomonline.txt", text)) {
        replace_all(text, "net validateNumPlayers max_players_online", "echo");
        items.emplace_back("data/gamescripts/menufinal/menucustomonline.txt", text);
    }
    {
        std::string label;
        for (char c : raw_name) label.push_back(c >= 32 && c < 127 ? c : '?');
        const std::string from = utf16be("DefaultPlayerName=Player"), to = utf16be("DefaultPlayerName=" + label);
        for (const char* lang : kLanguages) {
            const std::string member = std::string("data/local/") + lang + "/symbols.txt";
            if (online != "ON" && !label.empty() && read_member(archive, member, text) && replace_once(text, from, to)) items.emplace_back(member, text);
        }
    }
    if (items.empty()) return;

    mkdir((dir + "/addons").c_str(), 0755);
    const std::string tmp = target + ".tmp" + std::to_string(long(getpid()));
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        const std::string blob = build_archive(items);
        const bool ok = std::fwrite(blob.data(), 1, blob.size(), f) == blob.size();
        std::fclose(f);
        if (ok && std::rename(tmp.c_str(), target.c_str()) == 0) {
            if (FILE* k = std::fopen(keyfile.c_str(), "wb")) { std::fwrite(key.data(), 1, key.size(), k); std::fclose(k); }
            std::fprintf(stderr, "[fp-patch] wrote %s (%zu scripts, timing %s, debug combos %s)\n", target.c_str(), items.size(),
                         timing.c_str(), bindings ? "on" : "off");
        } else std::remove(tmp.c_str());
    }
}
}  // namespace
