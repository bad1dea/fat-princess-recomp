#pragma once
#include <cstdint>
#include <string>
namespace fp {
// Presentation-thread API. Guest state arrives through game_controls snapshots.
struct SelfTestStatus {
    bool running = false;
    unsigned elapsed = 0, duration = 0;
    std::string scenario, result = "READY", report;
};
SelfTestStatus self_test_status();
bool self_test_start(const std::string& scenario, unsigned seconds);
void self_test_stop();
void self_test_frame(uint32_t flip, const unsigned char* rgba, unsigned width, unsigned height);
}
