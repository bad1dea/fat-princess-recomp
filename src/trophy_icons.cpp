#include "trophy_icons.h"
#include "../third_party/ps3recomp/libs/codec/stb_image.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fp {
namespace {
constexpr int kSize = 128;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A member of a TROPHY.TRP (big-endian header, 32-byte names, u64 offset and size per entry).
bool trp_member(const std::vector<uint8_t>& trp, const char* name, std::vector<uint8_t>& out) {
    if (trp.size() < 0x40 || be32(trp.data()) != 0xDCA24D00u) return false;
    const uint32_t count = be32(&trp[0x10]), esz = be32(&trp[0x14]);
    if (esz < 0x30 || count > 256) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 0x40 + size_t(i) * esz;
        if (at + 0x30 > trp.size()) return false;
        if (std::strncmp(reinterpret_cast<const char*>(&trp[at]), name, 32) != 0) continue;
        const uint64_t off = (uint64_t(be32(&trp[at + 32])) << 32) | be32(&trp[at + 36]);
        const uint64_t size = (uint64_t(be32(&trp[at + 40])) << 32) | be32(&trp[at + 44]);
        if (off + size > trp.size() || size == 0) return false;
        out.assign(trp.begin() + long(off), trp.begin() + long(off + size));
        return true;
    }
    return false;
}

// Area-average a decoded RGBA image into kSize x kSize (premultiplied, so the transparent edge stays clean).
std::vector<uint8_t> shrink(const uint8_t* px, int w, int h) {
    std::vector<uint8_t> out(size_t(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y) for (int x = 0; x < kSize; ++x) {
        const int x0 = x * w / kSize, x1 = std::max(x0 + 1, (x + 1) * w / kSize), y0 = y * h / kSize, y1 = std::max(y0 + 1, (y + 1) * h / kSize);
        unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
        for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) {
            const uint8_t* p = px + (size_t(yy) * w + xx) * 4;
            r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; ++n;
        }
        uint8_t* o = &out[(size_t(y) * kSize + x) * 4];
        if (a) { o[0] = uint8_t(r / a); o[1] = uint8_t(g / a); o[2] = uint8_t(b / a); }
        o[3] = uint8_t(a / n);
    }
    return out;
}

uint16_t pack565(const int c[3]) { return uint16_t((c[0] * 31 / 255) << 11 | (c[1] * 63 / 255) << 5 | (c[2] * 31 / 255)); }
void unpack565(uint16_t v, int c[3]) { c[0] = ((v >> 11) & 31) * 255 / 31; c[1] = ((v >> 5) & 63) * 255 / 63; c[2] = (v & 31) * 255 / 31; }

// DXT5: 8 bytes of alpha (two end points, 3-bit indices) and 8 bytes of colour (565 end points, 2-bit indices).
void encode_block(const uint8_t* px, int stride, uint8_t* out) {
    int amin = 255, amax = 0, lo = 1 << 30, hi = -1, clo[3] = {0, 0, 0}, chi[3] = {0, 0, 0};
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        const uint8_t* p = px + y * stride + x * 4;
        amin = std::min<int>(amin, p[3]); amax = std::max<int>(amax, p[3]);
        if (p[3] < 8) continue;   // transparent pixels do not pull the colours
        const int l = p[0] * 3 + p[1] * 6 + p[2];
        if (l < lo) { lo = l; for (int k = 0; k < 3; ++k) clo[k] = p[k]; }
        if (l > hi) { hi = l; for (int k = 0; k < 3; ++k) chi[k] = p[k]; }
    }
    if (hi < 0) { std::memset(out, 0, 16); out[0] = 0; out[1] = 0; return; }
    // alpha: a0 > a1 selects the 8-level mode
    uint8_t a0 = uint8_t(amax), a1 = uint8_t(amin);
    if (a0 == a1) { if (a0 == 255) a1 = 254; else a0 = uint8_t(a1 + 1); }
    int pal[8] = {a0, a1};
    for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        const int a = px[(i >> 2) * stride + (i & 3) * 4 + 3];
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; ++k) { const int d = std::abs(a - pal[k]); if (d < bd) { bd = d; best = k; } }
        bits |= uint64_t(best) << (3 * i);
    }
    out[0] = a0; out[1] = a1;
    for (int k = 0; k < 6; ++k) out[2 + k] = uint8_t(bits >> (8 * k));
    // colour
    uint16_t c0 = pack565(chi), c1 = pack565(clo);
    if (c0 < c1) std::swap(c0, c1);   // DXT5 always uses the four-colour mode; c0 > c1 keeps one convention
    int e0[3], e1[3];
    unpack565(c0, e0); unpack565(c1, e1);
    int cp[4][3];
    for (int k = 0; k < 3; ++k) { cp[0][k] = e0[k]; cp[1][k] = e1[k]; cp[2][k] = (2 * e0[k] + e1[k]) / 3; cp[3][k] = (e0[k] + 2 * e1[k]) / 3; }
    uint32_t idx = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t* p = px + (i >> 2) * stride + (i & 3) * 4;
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; ++k) {
            const int d = (p[0] - cp[k][0]) * (p[0] - cp[k][0]) + (p[1] - cp[k][1]) * (p[1] - cp[k][1]) + (p[2] - cp[k][2]) * (p[2] - cp[k][2]);
            if (d < bd) { bd = d; best = k; }
        }
        idx |= uint32_t(best) << (2 * i);
    }
    out[8] = uint8_t(c0); out[9] = uint8_t(c0 >> 8); out[10] = uint8_t(c1); out[11] = uint8_t(c1 >> 8);
    for (int k = 0; k < 4; ++k) out[12 + k] = uint8_t(idx >> (8 * k));
}

// A PS3 .gtf: 128-byte big-endian header (copied from the game's own DXT5 textures) and the blocks.
std::string make_gtf(const std::vector<uint8_t>& rgba) {
    const uint32_t bytes = kSize * kSize;   // DXT5: one byte per pixel
    std::string out(128 + bytes, '\0');
    uint8_t* h = reinterpret_cast<uint8_t*>(&out[0]);
    const uint8_t head[] = {0x02, 0x01, 0x01, 0xFF};
    std::memcpy(h, head, 4);
    h[4] = uint8_t(bytes >> 24); h[5] = uint8_t(bytes >> 16); h[6] = uint8_t(bytes >> 8); h[7] = uint8_t(bytes);
    h[11] = 1; h[16] = 0; h[19] = 0x80;
    h[20] = h[4]; h[21] = h[5]; h[22] = h[6]; h[23] = h[7];
    h[24] = 0x88; h[25] = 0x01; h[26] = 0x02; h[27] = 0x00; h[28] = 0x00; h[29] = 0x00; h[30] = 0xAA; h[31] = 0xE4;
    h[32] = 0; h[33] = kSize; h[34] = 0; h[35] = kSize; h[36] = 0; h[37] = 1;
    for (int by = 0; by < kSize / 4; ++by) for (int bx = 0; bx < kSize / 4; ++bx)
        encode_block(&rgba[(size_t(by) * 4 * kSize + size_t(bx) * 4) * 4], kSize * 4, h + 128 + (size_t(by) * (kSize / 4) + bx) * 16);
    return out;
}

void dim(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const unsigned l = (rgba[i] * 3 + rgba[i + 1] * 6 + rgba[i + 2]) / 10 / 2 + 20;
        rgba[i] = rgba[i + 1] = rgba[i + 2] = uint8_t(l);
    }
}
}  // namespace

std::vector<std::pair<std::string, std::string>> trophy_icon_textures(const std::string& game_dir) {
    std::vector<std::pair<std::string, std::string>> out;
    const std::string path = game_dir + "/../TROPDIR/NPWR00737_00/TROPHY.TRP";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::vector<uint8_t> trp;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) trp.insert(trp.end(), buf, buf + n);
    std::fclose(f);
    for (int id = 0; id < 64; ++id) {
        char name[40], tex[96];
        std::snprintf(name, sizeof name, "TROP%03d.PNG", id);
        std::vector<uint8_t> png;
        if (!trp_member(trp, name, png)) break;
        int w = 0, h = 0, n = 0;
        uint8_t* px = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
        if (!px) break;
        std::vector<uint8_t> small = shrink(px, w, h);
        stbi_image_free(px);
        std::snprintf(tex, sizeof tex, "data/textures/menus/fptrophy%02d.gtf", id);
        out.emplace_back(tex, make_gtf(small));
        dim(small);
        std::snprintf(tex, sizeof tex, "data/textures/menus/fptrophyg%02d.gtf", id);
        out.emplace_back(tex, make_gtf(small));
    }
    return out;
}
}  // namespace fp
