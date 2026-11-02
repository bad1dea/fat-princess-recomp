#include "trophy_book.h"
#include "game_art.h"
#include "game_stats.h"
#include "trophy_ui.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

extern "C" void fp_game_stats_collect(void);

namespace fp {
namespace {
int g_page = 0, g_row = 0;

std::string upper(std::string s) { for (char& c : s) c = char(std::toupper((unsigned char)c)); return s; }

// Width in book pixels of `text` in a game font at `scale`; a rough per-character guess when the font is not available.
float width_of(const ArtFont* font, const std::string& text, float scale) { return font ? font->width(text, scale) : float(text.size()) * 24.f * scale; }

std::string fit(const ArtFont* font, std::string text, float scale, float room) {
    while (text.size() > 3 && width_of(font, text, scale) > room) text = text.substr(0, text.size() - 4) + "...";
    return text;
}

std::vector<std::string> wrap(const ArtFont* font, const std::string& text, float scale, float room, size_t max_lines) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&] {
        if (word.empty()) return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && width_of(font, trial, scale) > room) { lines.push_back(line); line = word; } else line = trial;
        word.clear();
    };
    for (char c : text) { if (c == ' ') flush(); else word += c; }
    flush();
    if (!line.empty()) lines.push_back(line);
    if (lines.size() > max_lines) { lines.resize(max_lines); lines.back() = fit(font, lines.back() + "...", scale, room); }
    return lines;
}

const char* grade_word(int g) { return g == 1 ? "PLATINUM" : g == 2 ? "GOLD" : g == 3 ? "SILVER" : "BRONZE"; }

std::string progress_text(const TrophyRow& r) {
    if (r.target <= 0) return {};
    return std::to_string(std::min(r.progress, r.target)) + " OF " + std::to_string(r.target) + (r.unit.empty() ? "" : " " + upper(r.unit));
}

int page_count(size_t n) { return std::max(1, int((n + kBookRows - 1) / kBookRows)); }

void put(SymbolReplies& out, const std::string& sym, const std::string& value) { out.emplace_back(sym, value); }

void page_texts(const std::vector<TrophyRow>& rows, SymbolReplies& out) {
    const ArtFont* boo = art_font("boopee");
    const int pages = page_count(rows.size());
    g_page = (g_page % pages + pages) % pages;
    int earned = 0;
    for (const auto& r : rows) earned += r.unlocked;
    put(out, "FpT_count", std::to_string(earned) + " / " + std::to_string(rows.size()));
    put(out, "FpT_page", "PAGE " + std::to_string(g_page + 1) + " OF " + std::to_string(pages));
    for (int i = 0; i < kBookRows; ++i) {
        const size_t at = size_t(g_page) * kBookRows + size_t(i);
        std::string name, tag;
        if (at < rows.size()) {
            const TrophyRow& r = rows[at];
            tag = r.unlocked ? "DONE" : (r.target > 0 ? std::to_string(std::min(r.progress, r.target)) + "/" + std::to_string(r.target) : "");
            name = fit(boo, r.name, 0.62f, 340.f - width_of(boo, tag, 0.5f) - 30.f);
        }
        put(out, "FpT_n" + std::to_string(i), name);
        put(out, "FpT_s" + std::to_string(i), tag);
    }
}

void detail_texts(const std::vector<TrophyRow>& rows, SymbolReplies& out) {
    const ArtFont* boo = art_font("boopee");
    const ArtFont* bold = art_font("fatprincessbold");
    const size_t at = size_t(g_page) * kBookRows + size_t(g_row);
    for (const char* k : {"FpT_dn1", "FpT_dn2", "FpT_dg", "FpT_dd0", "FpT_dd1", "FpT_dd2", "FpT_dd3", "FpT_dp", "FpT_ds"}) put(out, k, "");
    if (at >= rows.size()) return;
    const TrophyRow& r = rows[at];
    const auto name = wrap(bold, upper(r.name), 0.5f, 340.f, 2);
    for (size_t i = 0; i < name.size(); ++i) put(out, "FpT_dn" + std::to_string(i + 1), name[i]);
    put(out, "FpT_dg", std::string(grade_word(r.grade)) + " TROPHY");
    put(out, "FpTIcon", std::string("FpTrophyIcon") + (r.unlocked ? "" : "G") + std::to_string(r.id));
    const auto desc = wrap(boo, r.detail, 0.62f, 340.f, 4);
    for (size_t i = 0; i < desc.size(); ++i) put(out, "FpT_dd" + std::to_string(i), desc[i]);
    put(out, "FpT_dp", progress_text(r));
    put(out, "FpT_ds", r.unlocked ? (r.when_text.empty() ? "EARNED" : upper(r.when_text)) : "NOT EARNED YET");
}
}  // namespace

void trophy_book_command(const std::vector<std::string>& a, SymbolReplies& out) {
    if (a.empty()) return;
    if (a[0] == "open") {
        fp_game_stats_collect();   // fresh counters now: this runs on the guest thread, inside the menu command
        g_page = 0; g_row = 0;
        put(out, "FpT_title", "TROPHIES");
        put(out, "FpT_hint", "LEFT / RIGHT: TURN PAGE");
    } else if (a[0] == "page" && a.size() >= 2) {
        g_page += std::atoi(a[1].c_str()) < 0 ? -1 : 1;
    } else if (a[0] == "focus" && a.size() >= 2) {
        g_row = std::clamp(std::atoi(a[1].c_str()), 0, kBookRows - 1);
    } else if (a[0] == "refresh") {
    } else return;
    const auto rows = trophy_ui_rows();
    if (a[0] != "focus") page_texts(rows, out);
    // a short last page: keep the cursor on a row that exists
    const int on_page = std::min<int>(kBookRows, std::max(0, int(rows.size()) - g_page * kBookRows));
    if (on_page > 0 && g_row >= on_page) g_row = on_page - 1;
    detail_texts(rows, out);
    put(out, "FpTFocus", "FpTrophyRow" + std::to_string(g_row));
}
}  // namespace fp
