#pragma once
#include "debug_hud.h"
#include <array>
#include <string>

inline std::vector<std::uint8_t> fp_settings_pixels(std::string_view text, unsigned& w, unsigned& h, unsigned scale=2) {
    auto pixels = fp_hud_pixels(text, w, h, scale);
    using Color = std::array<std::uint8_t, 3>;
    const Color background{24, 27, 36}, foreground{242, 235, 220}, muted{133, 149, 164};
    const Color accent{238, 145, 174}, focus{78, 39, 59}, border{85, 71, 87};
    std::vector<std::string> lines(1);
    for (char c : text) { if (c == '\n') lines.emplace_back(); else lines.back() += c; }
    const std::string empty;
    for (unsigned y = 0; y < h; ++y) {
        const unsigned line = y >= 8 ? (y - 8) / (9 * scale) : unsigned(lines.size());
        const std::string& row = line < lines.size() ? lines[line] : empty;
        const bool selected = row.rfind("> ", 0) == 0;
        const bool disabled = row.rfind("- ", 0) == 0;
        const bool separator = row.size() > 20 && row.find_first_not_of('-') == std::string::npos;
        const auto first = row.find('['), last = row.find(']');
        for (unsigned x = 0; x < w; ++x) {
        const auto at = (size_t(y) * w + x) * 4;
        const bool ink = pixels[at] == 220;
        Color color = ink ? (disabled ? muted : foreground) : background;
        if (selected && x >= 4 && x + 4 < w && !ink) color = focus;
        if (selected && x >= 4 && x < 7) color = accent;
        if (line == 0 && ink) color = accent;
        if (line == 1) {
            if (first != std::string::npos && last != std::string::npos && x >= 8 + first * 6 * scale && x < 8 + (last + 1) * 6 * scale)
                color = ink ? foreground : focus;
            else if (ink) color = muted;
        }
        if (separator) color = ((y - 8) % (9 * scale) == 4 * scale && x >= 8 && x + 8 < w) ? border : background;
        if (y < 3 || y + 2 >= h) color = accent;
        for (unsigned channel = 0; channel < 3; ++channel) pixels[at + channel] = color[channel];
        }
    }
    return pixels;
}
