#pragma once
#include <cstdint>

// Two short captures at most, separated so a prolonged fight cannot turn
// diagnostics into continuous work. Invalid CPU timings break the streak.
struct FPAudioProfileGate {
    static constexpr std::uint64_t budget_us = 10667;
    static constexpr std::uint64_t cooldown_us = 120000000;
    unsigned streak = 0, captures = 0;
    std::uint64_t last_capture = 0;
    bool note(std::uint64_t cpu_us, std::uint64_t now_us) {
        if (cpu_us <= budget_us) { streak = 0; return false; }
        if (streak < 3) ++streak;
        if (streak < 3 || captures >= 2 ||
            (captures && now_us - last_capture < cooldown_us)) return false;
        streak = 0;
        ++captures;
        last_capture = now_us;
        return true;
    }
};
