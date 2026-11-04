#pragma once
// Pictures and fonts read at run time from the player's own siege.qrk (never shipped). Everything returns null / false
// when the archive or a member is missing, so callers fall back to their plain drawing.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fp {
struct ArtImage { int w = 0, h = 0; std::vector<uint8_t> rgba; };
struct ArtGlyph { int x = 0, y = 0, w = 0, h = 0, yoffset = 0; };
struct ArtFont {
    ArtImage atlas;
    std::map<unsigned, ArtGlyph> glyphs;   // by Unicode code point
    float delta = 0, space = 15, line = 0;
    const ArtGlyph* find_glyph(unsigned c) const;   // lower case falls back to upper, then '?'
    float width(const std::string& text, float scale) const;   // pen advance of `text` at `scale` (1 = atlas pixels)
};
// "data/textures/menus/princesstipred" (no extension) -> decoded GTF; null when unavailable.
const ArtImage* art_image(const std::string& name);
// "fatprincessbold", "boopee": the game's bitmap fonts; null when unavailable.
const ArtFont* art_font(const std::string& name);
bool art_available();
// A text member of siege.qrk ("data/gamescripts/trophies.txt"); false when missing.
bool art_text_member(const std::string& name, std::string& out);
// Decode the first level of a PS3 GTF texture (DXT1/3/5, BGRA, L8). False for formats it does not know.
bool decode_gtf(const uint8_t* data, size_t size, ArtImage& out);
}
