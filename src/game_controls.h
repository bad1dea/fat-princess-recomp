#pragma once
#include <string>
#include <cstdint>

namespace fp {
struct PlayerControlState {
    bool available = false;
    bool ai = false;
    bool pending = false;
    bool requested_ai = false;
    bool free_camera = false;
    bool profile_applied = false;
    std::string ai_profile = "ORIGINAL";
    std::string error;
    uint32_t player = 0, character = 0;
    float x = 0, y = 0, z = 0;
    unsigned stick_x = 128, stick_y = 128;
};
PlayerControlState player_control_state();
bool player_control_request(bool ai);
bool player_ai_configure(const std::string& profile, float recheck_seconds, unsigned lock_on_percent);
// Called on the guest thread; publishes live state and expires requests when the player changes.
void player_control_poll();
bool player_command_request(const std::string& command);
}

extern "C" int fp_player_control_command(char* command, unsigned capacity);
