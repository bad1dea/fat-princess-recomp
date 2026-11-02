// Trophy screens: the details panel opened from the toast (F2 or the controller's Guide button while the toast is up
// or for ten seconds after) and the full trophy list (from the details panel or the settings menu). The settings
// menu's frame routes input here while a screen is open; the host draws trophy_ui_pixels() as the overlay.
#include "trophy_ui.h"
#include "settings.h"
#include "osk_panel.h"
#include "game_stats.h"
#include "../third_party/ps3recomp/libs/codec/stb_image.h"
#include <SDL2/SDL.h>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <vector>

extern "C" {
typedef struct sceNpTrophyHostEntry {
    int id, grade, hidden, unlocked;
    unsigned long long timestamp;
    const char* name;
    const char* detail;
} sceNpTrophyHostEntry;
int sceNpTrophyHostList(sceNpTrophyHostEntry* out, int max, const char** title);
unsigned char* sceNpTrophyHostIcon(int trophyId, unsigned* size);
}

namespace {
using Clock = std::chrono::steady_clock;
enum class Screen { None, Detail, List };
struct State {
    Screen screen = Screen::None;
    bool list_from_detail = false;
    int detail_id = -1, selected = 0;
    int toast_id = -1;
    Clock::time_point toast_at{};
    std::map<int, fp::TrophyIcon> icons;
    bool dirty = true;
    Clock::time_point flip_at{};
    int flip_dir = 0;
};
State& st() { static State s; return s; }

std::vector<sceNpTrophyHostEntry> entries(const char** title) {
    std::vector<sceNpTrophyHostEntry> out(128);
    const int n = sceNpTrophyHostList(out.data(), int(out.size()), title);
    out.resize(size_t(n > 0 ? n : 0));
    return out;
}
const fp::TrophyIcon* icon(int id) {
    auto& s = st();
    auto it = s.icons.find(id);
    if (it != s.icons.end()) return &it->second;
    fp::TrophyIcon decoded;
    unsigned size = 0;
    if (unsigned char* png = sceNpTrophyHostIcon(id, &size)) {
        int w = 0, h = 0, n = 0;
        if (unsigned char* px = stbi_load_from_memory(png, int(size), &w, &h, &n, 4)) {
            decoded.w = w; decoded.h = h; decoded.rgba.assign(px, px + size_t(w) * h * 4);
            stbi_image_free(px);
        }
        free(png);
    }
    return &s.icons.emplace(id, std::move(decoded)).first->second;
}
std::string when_text(unsigned long long t) {
    if (!t) return "Earned";
    const std::time_t tt = std::time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[48];
    std::strftime(buf, sizeof buf, "Earned %b %d, %Y", &tm);
    return buf;
}
fp::TrophyRow row_of(const sceNpTrophyHostEntry& e) {
    fp::TrophyRow r;
    r.id = e.id; r.grade = e.grade; r.hidden = e.hidden != 0; r.unlocked = e.unlocked != 0;
    r.name = e.name ? e.name : ""; r.detail = e.detail ? e.detail : ""; r.when_text = r.unlocked ? when_text(e.timestamp) : "";
    fp::TrophyProgress p;
    if (fp::game_stats_progress(e.id, p)) { r.progress = p.value; r.target = p.target; r.unit = p.unit; }
    return r;
}
}

namespace fp {
bool trophy_ui_active() { return st().screen != Screen::None; }

std::string trophy_ui_detail(int id) {
    for (const auto& e : entries(nullptr)) if (e.id == id) return e.detail ? e.detail : "";
    return {};
}

void trophy_ui_note_toast(int id) { st().toast_id = id; st().toast_at = Clock::now(); }

bool trophy_ui_open(int detail_id) {
    auto& s = st();
    if (entries(nullptr).empty()) return false;   // no trophy set registered yet
    game_stats_request();                         // fresh counters for the progress bars
    if (detail_id >= 0) { s.screen = Screen::Detail; s.detail_id = detail_id; s.list_from_detail = false; }
    else { s.screen = Screen::List; s.list_from_detail = false; s.selected = 0; }
    s.dirty = true;
    return true;
}
bool trophy_ui_open_recent() {
    auto& s = st();
    const bool recent = s.toast_id >= 0 && Clock::now() - s.toast_at < std::chrono::seconds(15);
    return trophy_ui_open(recent ? s.toast_id : -1);
}
void trophy_ui_close() { st().screen = Screen::None; st().dirty = true; }

bool trophy_ui_nav(bool up, bool down, bool ok, bool back, bool left, bool right) {
    auto& s = st();
    if (s.screen == Screen::Detail) {
        if (back) { trophy_ui_close(); return true; }
        if (ok) { s.screen = Screen::List; s.list_from_detail = true; s.selected = s.detail_id; s.dirty = true; return true; }
        return false;
    }
    if (s.screen == Screen::List) {
        const int n = int(entries(nullptr).size());
        if (back) { if (s.list_from_detail) s.screen = Screen::Detail; else trophy_ui_close(); s.dirty = true; return true; }
        const int before = s.selected / kTrophyPageSize;
        if (up && n) { s.selected = (s.selected + n - 1) % n; s.dirty = true; }
        if (down && n) { s.selected = (s.selected + 1) % n; s.dirty = true; }
        if ((left || right) && n) {   // a whole page at a time, same row on the new page
            const int pages = (n + kTrophyPageSize - 1) / kTrophyPageSize, row = s.selected % kTrophyPageSize;
            const int to = (before + (right ? 1 : pages - 1)) % pages;
            s.selected = std::min(n - 1, to * kTrophyPageSize + row);
            s.dirty = true;
        }
        const int after = s.selected / kTrophyPageSize;
        if (after != before) { s.flip_at = Clock::now(); s.flip_dir = (down || right) ? 1 : -1; }
        if (ok && n) { s.screen = Screen::Detail; s.detail_id = s.selected; s.list_from_detail = false; s.dirty = true; }
        return s.dirty;
    }
    return false;
}

bool trophy_ui_take_dirty() {
    if (st().flip_dir != 0) {
        if (Clock::now() - st().flip_at < std::chrono::milliseconds(400)) st().dirty = true;
        else { st().flip_dir = 0; st().dirty = true; }
    }
    static unsigned seen = 0;
    const unsigned gen = game_stats_generation();
    if (gen != seen) { seen = gen; st().dirty = true; }
    const bool d = st().dirty; st().dirty = false; return d;
}

std::vector<uint8_t> trophy_ui_pixels(float scale, bool controller, unsigned& w, unsigned& h, unsigned& radius) {
    auto& s = st();
    const char* title = "";
    const auto list = entries(&title);
    int earned = 0;
    for (const auto& e : list) earned += e.unlocked != 0;
    if (s.screen == Screen::Detail) {
        TrophyDetailView v;
        const int id = s.detail_id >= 0 && s.detail_id < int(list.size()) ? s.detail_id : 0;
        if (!list.empty()) v.row = row_of(list[size_t(id)]);
        v.icon = icon(id);
        const bool fresh = v.row.unlocked && s.toast_id == id && Clock::now() - s.toast_at < std::chrono::minutes(2);
        v.status = !v.row.unlocked ? "Not earned yet" : fresh ? "Earned just now" : v.row.when_text;
        v.title = title; v.earned = earned; v.total = int(list.size());
        v.controls = settings_prompt_text(controller ? "CROSS VIEW TROPHIES     CIRCLE CLOSE" : "ENTER VIEW TROPHIES     ESC CLOSE");
        return trophy_detail_pixels(v, scale, w, h, radius);
    }
    TrophyListView v;
    v.title = title; v.earned = earned; v.total = int(list.size()); v.selected = s.selected;
    if (s.flip_dir != 0) { v.flip = std::chrono::duration<float>(Clock::now() - s.flip_at).count() / 0.4f; v.flip_dir = s.flip_dir; if (v.flip >= 1) v.flip = -1; }
    for (const auto& e : list) { v.rows.push_back(row_of(e)); v.icons.push_back(icon(e.id)); }
    v.controls = settings_prompt_text(controller ? "UP/DOWN SELECT     LEFT/RIGHT PAGE     CROSS DETAILS     CIRCLE BACK" : "UP/DOWN SELECT     LEFT/RIGHT PAGE     ENTER DETAILS     ESC BACK");
    return trophy_list_pixels(v, scale, w, h, radius);
}

// Three rising sine notes (E5, A5, D6) with short exponential envelopes, on their own SDL audio device so the game's
// mix is untouched. The device opens on first use and stays open.
void trophy_chime() {
    static SDL_AudioDeviceID dev = 0;
    static bool tried = false;
    constexpr int rate = 48000;
    if (!tried) {
        tried = true;
        if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec want{}, have{};
        want.freq = rate; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (dev) SDL_PauseAudioDevice(dev, 0);
    }
    if (!dev) return;
    const struct { float freq, at, dur; } notes[] = {{659.25f, .04f, .18f}, {880.f, .15f, .22f}, {1174.66f, .28f, .32f}};
    const int total = int(rate * 0.7f);
    std::vector<float> pcm(size_t(total) * 2, 0.f);
    for (const auto& n : notes)
        for (int i = int(n.at * rate); i < int((n.at + n.dur) * rate) && i < total; ++i) {
            const float t = float(i) / rate - n.at;
            const float attack = std::min(1.f, t / .012f), env = attack * std::exp(-t * 9.f);
            const float v = 0.22f * env * std::sin(6.2831853f * n.freq * t);
            pcm[size_t(i) * 2] += v; pcm[size_t(i) * 2 + 1] += v;
        }
    SDL_QueueAudio(dev, pcm.data(), Uint32(pcm.size() * sizeof(float)));
}
}
