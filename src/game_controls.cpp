#include "game_controls.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <deque>
#include <array>
#include <algorithm>
#include <cmath>

extern "C" unsigned char* vm_base;
extern "C" unsigned char g_fp_patch[32];

namespace {
struct Controls {
    std::mutex mutex;
    fp::PlayerControlState state;
    uint32_t player = 0, requested_player = 0;
    unsigned stage = 0;
    std::chrono::steady_clock::time_point deadline;
    struct Command { uint32_t player; std::string text; };
    std::deque<Command> commands;
    std::string profile = "ORIGINAL";
    float recheck_seconds = 1.f;
    unsigned lock_on_percent = 90;
    uint32_t profile_player = 0, profile_brain = 0, aim_begin = 0, aim_end = 0, original_period = 0;
    std::array<uint32_t, 16> original_aim{};
};
Controls& controls() { static Controls c; return c; }

uint32_t read32(uint32_t a) {
    const auto* b = vm_base + a;
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}
bool readable(uint32_t address, uint32_t bytes) {
    return vm_base && address >= 0x10000 && address < 0x50000000 && bytes <= 0x50000000 - address;
}
void write32(uint32_t address, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) vm_base[address + i] = uint8_t(value >> ((3-i)*8));
}
void apply_profile(Controls& c) {
    c.state.ai_profile = c.profile;
    c.state.profile_applied = false;
    if (!c.state.available) {
        if (c.player!=c.profile_player || !readable(c.player,0x1600) || read32(c.player+0x14D8)!=c.profile_brain) c.profile_brain=0;
        return;
    }
    const uint32_t brain = c.state.available && c.state.ai ? read32(c.player + 0x14D8) : 0;
    if (!readable(brain, 0x4014)) { c.profile_brain = 0; return; }
    const uint32_t begin = read32(brain + 0x330), end = read32(brain + 0x334);
    if (end <= begin || (end - begin) % 4 || end - begin > c.original_aim.size()*4 || !readable(begin, end-begin)) return;
    if (brain != c.profile_brain || c.player != c.profile_player) {
        const uint32_t period = read32(brain + 0x90);
        float seconds; std::memcpy(&seconds, &period, 4);
        if (!std::isfinite(seconds) || seconds <= 0 || seconds > 60) return;
        for (unsigned i=0; i<(end-begin)/4; ++i) if (read32(begin+i*4)>100) return;
        c.original_period=period; c.aim_begin=begin; c.aim_end=end;
        for (unsigned i=0; i<(end-begin)/4; ++i) c.original_aim[i]=read32(begin+i*4);
        c.profile_brain=brain;
        c.profile_player=c.player;
    }
    // A rebuilt configuration owns different arrays. Never write through a retired allocation.
    if (begin != c.aim_begin || end != c.aim_end) return;
    float period; std::memcpy(&period, &c.original_period, 4);
    if (c.profile=="EASY") period*=2.f;
    if (c.profile=="HARD") period*=.5f;
    if (c.profile=="ADVANCED") period*=.25f;
    if (c.profile=="CUSTOM") period=c.recheck_seconds;
    period=std::clamp(period,.05f,60.f);
    uint32_t bits; std::memcpy(&bits, &period, 4);
    write32(brain+0x90, c.profile=="ORIGINAL" || c.profile=="MEDIUM" ? c.original_period : bits);
    for (unsigned i=0; i<(end-begin)/4; ++i) {
        unsigned chance=c.original_aim[i];
        if (chance) {
            if (c.profile=="EASY") chance=std::min(chance,25u);
            if (c.profile=="HARD") chance=std::max(chance,90u);
            if (c.profile=="ADVANCED") chance=100;
            if (c.profile=="CUSTOM") chance=c.lock_on_percent;
        }
        write32(begin+i*4,chance);
    }
    c.state.profile_applied=true;
}
}

namespace fp {
PlayerControlState player_control_state() {
    auto& c = controls();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.state;
}

bool player_control_request(bool ai) {
    auto& c = controls();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (!c.state.available) return false;
    const bool command_in_flight = c.state.pending && c.stage != 0;
    c.state.error.clear();
    c.state.requested_ai = ai;
    c.state.pending = c.state.ai != ai || command_in_flight;
    c.requested_player = c.player;
    c.stage = 0;
    c.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    return true;
}

bool player_ai_configure(const std::string& profile, float seconds, unsigned chance) {
    if (profile!="ORIGINAL" && profile!="EASY" && profile!="MEDIUM" && profile!="HARD" && profile!="ADVANCED" && profile!="CUSTOM") return false;
    if (!std::isfinite(seconds) || seconds<.05f || seconds>60.f || chance>100) return false;
    auto& c=controls(); std::lock_guard<std::mutex> lock(c.mutex);
    c.profile=profile; c.recheck_seconds=seconds; c.lock_on_percent=chance;
    return true;
}

void player_control_poll() {
    uint32_t player = 0;
    bool available = false, ai = false;
    if (vm_base) {
        const uint32_t begin = read32(0xD1ACB8 + 8), end = read32(0xD1ACB8 + 12);
        const int32_t index = int32_t(read32(0xF01804 + 0x24));
        if (end >= begin && (end - begin) % 4 == 0 && (end - begin) / 4 <= 128 &&
            index >= 0 && uint32_t(index) < (end - begin) / 4 && readable(begin, end - begin)) {
            player = read32(begin + uint32_t(index) * 4);
            if (readable(player, 0x1600)) {
                available = read32(player + 0xB4) != 0 && vm_base[player + 0x1548] &&
                            int32_t(read32(player + 0x15FC)) >= 0 && !vm_base[0xF01804 + 0x31];
                ai = available && vm_base[player + 0x1549];
            }
        }
    }
    auto& c = controls();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.player = player;
    c.state.available = available;
    c.state.ai = ai;
    c.state.free_camera = vm_base && vm_base[0xD675E8];
    c.state.player = player;
    c.state.character = readable(player, 0x1600) ? read32(player + 0xB4) : 0;
    if (readable(c.state.character, 0x158)) {
        const uint32_t character = c.state.character;
        uint32_t bits = read32(character + 0x14C); std::memcpy(&c.state.x, &bits, 4);
        bits = read32(character + 0x150); std::memcpy(&c.state.y, &bits, 4);
        bits = read32(character + 0x154); std::memcpy(&c.state.z, &bits, 4);
    }
    if (vm_base) {
        const uint32_t slots = read32(0xC9C414);
        const uint32_t index = read32(0xF01804 + 0x24);
        if (index < 128 && readable(slots, (index + 1) * 0x1A8)) {
            c.state.stick_x = unsigned(vm_base[slots + index * 0x1A8 + 0x1C]) * 256 + vm_base[slots + index * 0x1A8 + 0x1D];
            c.state.stick_y = unsigned(vm_base[slots + index * 0x1A8 + 0x1E]) * 256 + vm_base[slots + index * 0x1A8 + 0x1F];
        }
    }
    if (c.state.pending) {
        if (!available || player != c.requested_player) {
            c.state.pending = false;
            c.state.error = "PLAYER CHANGED. CONTROL REQUEST CANCELLED";
        } else if (c.stage >= (c.state.requested_ai ? 2u : 1u) && ai == c.state.requested_ai) {
            c.state.pending = false;
        } else if (std::chrono::steady_clock::now() >= c.deadline) {
            c.state.pending = false;
            c.state.error = "CONTROL CHANGE DID NOT COMPLETE. TRY AGAIN";
        }
    }
    apply_profile(c);
}
bool player_command_request(const std::string& command) {
    if (command.empty() || command.size() >= 125 || command.find_first_of("\r\n") != std::string::npos) return false;
    auto& c = controls();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (!c.state.available || c.commands.size() >= 8) return false;
    c.commands.push_back({c.player, command});
    return true;
}
}

extern "C" int fp_player_control_command(char* command, unsigned capacity) {
    fp::player_control_poll();
    auto& c = controls();
    std::lock_guard<std::mutex> lock(c.mutex);
    while (!c.commands.empty() && (!c.state.available || c.commands.front().player != c.player)) c.commands.pop_front();
    if (!c.state.pending || c.stage >= (c.state.requested_ai ? 2u : 1u)) {
        if (c.commands.empty() || c.commands.front().text.size() + 1 > capacity || !command) return 0;
        std::memcpy(command, c.commands.front().text.c_str(), c.commands.front().text.size() + 1);
        c.commands.pop_front();
        return 1;
    }
    const char* text = !c.state.requested_ai ? "player toggleai off" :
                       c.stage == 0 ? "net set aiEnabled 1" : "player toggleai on";
    if (!command || std::strlen(text) + 1 > capacity) return 0;
    g_fp_patch[29] = 1;
    std::memcpy(command, text, std::strlen(text) + 1);
    ++c.stage;
    return 1;
}
