#include "game_art.h"
#include <zlib.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fp {
namespace {
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
struct Member { uint32_t stored, offset, size; bool compressed; };

struct Qrk {
    std::string path;
    std::map<std::string, Member> members;
    bool ok = false;
    explicit Qrk(const std::string& p) : path(p) {
        std::ifstream f(p, std::ios::binary);
        uint8_t h[24];
        if (!f.read(reinterpret_cast<char*>(h), 24) || le32(h) != 0x004b5251 || le32(h + 4) != 1) return;
        const uint32_t count = le32(h + 8), names_size = le32(h + 20);
        if (count > 1000000 || names_size > 64u * 1024 * 1024) return;
        std::vector<uint8_t> names(names_size), rows(size_t(count) * 21);
        if (!f.read(reinterpret_cast<char*>(names.data()), names_size) || !f.read(reinterpret_cast<char*>(rows.data()), std::streamsize(rows.size()))) return;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* r = &rows[size_t(i) * 21];
            const uint32_t no = le32(r + 16);
            if (no >= names.size()) continue;
            size_t end = no;
            while (end < names.size() && names[end]) ++end;
            members[std::string(reinterpret_cast<const char*>(&names[no]), end - no)] = {le32(r), le32(r + 4), le32(r + 8), r[20] == 1};
        }
        ok = true;
    }
    bool read(const std::string& name, std::vector<uint8_t>& out) const {
        const auto it = members.find(name);
        if (it == members.end() || it->second.size > 64u * 1024 * 1024) return false;
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> raw(it->second.stored);
        f.seekg(it->second.offset);
        if (!f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()))) return false;
        if (!it->second.compressed) { out = std::move(raw); return out.size() == it->second.size; }
        out.resize(it->second.size);
        uLongf len = it->second.size;
        return uncompress(out.data(), &len, raw.data(), uLong(raw.size())) == Z_OK && len == it->second.size;
    }
};

std::string game_dir() {
    if (const char* d = std::getenv("FP_GAME_DIR")) return d;
    if (const char* h = std::getenv("PS3_HDD0_ROOT")) return std::string(h) + "/game/NPUA80139/USRDIR";
    return "";
}

const Qrk* qrk() {
    static const Qrk* q = [] () -> const Qrk* {
        const std::string dir = game_dir();
        if (dir.empty()) return nullptr;
        auto* a = new Qrk(dir + "/siege.qrk");
        if (!a->ok) { delete a; return nullptr; }
        return a;
    }();
    return q;
}

void rgb565(uint16_t c, uint8_t* o) {
    o[0] = uint8_t(((c >> 11) & 31) * 255 / 31); o[1] = uint8_t(((c >> 5) & 63) * 255 / 63); o[2] = uint8_t((c & 31) * 255 / 31);
}
// One 4x4 colour block (DXT1 style; `four` forces the 4-colour mode that DXT3/5 always use).
void color_block(const uint8_t* b, bool four, uint8_t out[16][4]) {
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]); pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || four) {
        for (int k = 0; k < 3; ++k) { pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3); }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
        pal[2][3] = 255;
    }
    const uint32_t idx = le32(b + 4);
    for (int i = 0; i < 16; ++i) std::memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
}
}

bool decode_gtf(const uint8_t* d, size_t size, ArtImage& img) {
    if (size < 128) return false;
    const uint8_t fmt = d[24];
    const int w = d[32] << 8 | d[33], h = d[34] << 8 | d[35];
    const int f = fmt & ~0x60;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    img.w = w; img.h = h; img.rgba.assign(size_t(w) * h * 4, 0);
    const uint8_t* p = d + 128;
    const size_t avail = size - 128;
    if (f == 0x86 || f == 0x87 || f == 0x88) {
        const int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = f == 0x86 ? 8 : 16;
        if (avail < size_t(bw) * bh * bs) return false;
        for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* b = p + (size_t(by) * bw + bx) * bs;
            uint8_t px[16][4];
            color_block(f == 0x86 ? b : b + 8, f != 0x86, px);
            if (f == 0x87) for (int i = 0; i < 16; ++i) { const int v = (b[i / 2] >> (4 * (i & 1))) & 15; px[i][3] = uint8_t(v * 17); }
            if (f == 0x88) {
                uint8_t a[8] = {b[0], b[1]};
                if (a[0] > a[1]) for (int k = 1; k < 7; ++k) a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1]) / 7);
                else { for (int k = 1; k < 5; ++k) a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1]) / 5); a[6] = 0; a[7] = 255; }
                uint64_t bits = 0;
                for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
                for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
            }
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) std::memcpy(&img.rgba[(size_t(y) * w + x) * 4], px[i], 4);
            }
        }
        return true;
    }
    if (f == 0x85 && (fmt & 0x20)) {
        if (avail < size_t(w) * h * 4) return false;
        for (size_t i = 0; i < size_t(w) * h; ++i) { img.rgba[i * 4] = p[i * 4 + 2]; img.rgba[i * 4 + 1] = p[i * 4 + 1]; img.rgba[i * 4 + 2] = p[i * 4]; img.rgba[i * 4 + 3] = p[i * 4 + 3]; }
        return true;
    }
    if (f == 0x81) {
        if (avail < size_t(w) * h) return false;
        for (size_t i = 0; i < size_t(w) * h; ++i) { img.rgba[i * 4] = img.rgba[i * 4 + 1] = img.rgba[i * 4 + 2] = p[i]; img.rgba[i * 4 + 3] = 255; }
        return true;
    }
    return false;
}

const ArtGlyph* ArtFont::find_glyph(unsigned c) const {
    auto it = glyphs.find(c);
    if (it == glyphs.end() && c >= 'a' && c <= 'z') it = glyphs.find(c - 32);
    if (it == glyphs.end()) it = glyphs.find('?');
    return it == glyphs.end() ? nullptr : &it->second;
}

float ArtFont::width(const std::string& text, float scale) const {
    float pen = 0;
    for (unsigned char c : text) {
        if (c == ' ') { pen += space * scale; continue; }
        const auto it = find_glyph(c);
        if (it) pen += (it->w + delta) * scale;
    }
    return pen;
}

static std::mutex g_art_lock;
bool art_available() { return qrk() != nullptr; }
bool art_text_member(const std::string& name, std::string& out) {
    std::vector<uint8_t> raw;
    const Qrk* q = qrk();
    if (!q || !q->read(name, raw)) return false;
    out.assign(raw.begin(), raw.end());
    return true;
}

const ArtImage* art_image(const std::string& name) {
    static std::map<std::string, ArtImage*> cache;
    std::lock_guard<std::mutex> lock(g_art_lock);
    const auto it = cache.find(name);
    if (it != cache.end()) return it->second && !it->second->rgba.empty() ? it->second : nullptr;
    ArtImage* img = nullptr;
    std::vector<uint8_t> raw;
    if (const Qrk* q = qrk()) {
        if (q->read(name + ".gtf", raw)) {
            img = new ArtImage;
            if (!decode_gtf(raw.data(), raw.size(), *img)) { delete img; img = nullptr; }
        }
    }
    cache[name] = img;
    return img;
}

const ArtFont* art_font(const std::string& name) {
    static std::map<std::string, ArtFont*> cache;
    {
        std::lock_guard<std::mutex> lock(g_art_lock);
        const auto it = cache.find(name);
        if (it != cache.end()) return it->second;
    }
    ArtFont* font = nullptr;
    std::vector<uint8_t> raw;
    const std::string base = "data/textures/fonts/" + name;
    if (const Qrk* q = qrk()) {
        if (q->read(base + ".txt", raw)) {
            font = new ArtFont;
            std::istringstream in(std::string(raw.begin(), raw.end()));
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                float v;
                if (std::sscanf(line.c_str(), "charDeltaSpaceDefault = %f", &v) == 1) font->delta = v;
                else if (std::sscanf(line.c_str(), "spaceSize = %f", &v) == 1) font->space = v;
                else if (std::sscanf(line.c_str(), "lineHeight = %f", &v) == 1) font->line = v;
                else if (line.compare(0, 5, "char ") == 0) {
                    char nm[32]; int x, y, w, h, yo = 0, used = 0;
                    if (std::sscanf(line.c_str() + 5, "%31s = %d %d %d %d%n", nm, &x, &y, &w, &h, &used) == 5) {
                        const char* yp = std::strstr(line.c_str(), "yoffset");
                        if (yp) std::sscanf(yp, "yoffset %d", &yo);
                        const std::string n = nm;
                        unsigned code = 0;
                        if (n.size() == 1) code = (unsigned char)n[0];
                        else if (n == "comma") code = ',';
                        else if (n == "singleQuote") code = '\'';
                        else if (n == "doubleQuote") code = '"';
                        else if (n == "equals") code = '=';
                        else if (n.compare(0, 2, "U+") == 0) code = unsigned(std::strtoul(n.c_str() + 2, nullptr, 16));
                        if (code) font->glyphs[code] = {x, y, w, h, yo};
                    }
                }
            }
            std::vector<uint8_t> tex;
            if (!q->read(base + ".gtf", tex) || !decode_gtf(tex.data(), tex.size(), font->atlas) || font->glyphs.empty()) { delete font; font = nullptr; }
        }
    }
    std::lock_guard<std::mutex> lock(g_art_lock);
    cache[name] = font;
    return font;
}
}
