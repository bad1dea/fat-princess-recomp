#pragma once
#include <string>

namespace fp {
// One measuring window of the presenter, filled by fp_backend_present in host.cpp.
struct PresentWindow {
    double secs;             // length of the window
    unsigned presents;       // frames presented to the window
    unsigned guest_flips;    // flips the game requested in the window
    double frame_avg_ms, p95_ms, max_ms;
    unsigned draws;          // guest draws in the last frame
    double submits;          // GPU submits per frame
    double pace_hz;          // rate the game is released at
};
// Text for the F3 overlay (GAME, HOST, AUDIO, NET). Fixed columns, at most 41 characters per line.
std::string stats_overlay_text(const PresentWindow& w);
}
