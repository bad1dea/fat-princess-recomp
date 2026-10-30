// Opt-in routing policy. Mode 4 adds a guard after native combat movement gates.
// Native request flag 0 skips the raised line-of-sight shortcut in 0x00213860.
#include "ai_route_policy.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>

extern "C" unsigned char* vm_base;
namespace {
bool valid(uint32_t address,uint32_t size) {
    return vm_base && address>=0x10000 && uint64_t(address)+size<=0x50000000;
}
uint32_t word(uint32_t address) {
    const auto* p=vm_base+address;
    return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
}
std::atomic<unsigned>& route_mode() {
    static std::atomic<unsigned> value{[] {
        const char* text=std::getenv("FP_AI_NAV_GRAPH");return text?unsigned(std::strtoul(text,nullptr,0)):0u;
    }()};
    return value;
}
unsigned mode() { return route_mode().load(std::memory_order_relaxed); }
bool controlled(uint32_t player) {
    static const int team=[] {
        const char* value=std::getenv("FP_AI_NAV_TEAM");return value?std::atoi(value):-1;
    }();
    return (mode()>=1 && mode()<=4) && valid(player,0x1635) && word(player+0xA8)<32 &&
        vm_base[player+0x1549] && vm_base[player+0x154A] && !vm_base[player+0x154B] &&
        (team<0 || word(player+0xAC)==unsigned(team));
}
}

extern "C" void fp_ai_route_set_guard(int enabled) {
    route_mode().store(enabled?4u:0u,std::memory_order_relaxed);
}

extern "C" int fp_ai_route_guard_enabled() { return mode()==4; }

extern "C" unsigned fp_ai_route_request_flags(uint32_t player,unsigned original) {
    return mode()==1 && controlled(player)?0:original;
}

extern "C" int fp_ai_route_ground_enabled(uint32_t player) { return mode()>=2 && mode()<=4 && controlled(player); }

extern "C" int fp_ai_route_water_enabled(uint32_t player) {
    if ((mode()!=3 && mode()!=4) || !controlled(player)) return 0;
    const uint32_t character=word(player+0xB4);
    // Preserve native escape movement for a bot already in water or lava.
    return valid(character,0x626) && !vm_base[character+0x624] && !vm_base[character+0x625];
}

extern "C" int fp_ai_route_surface_enabled(uint32_t player) {
    return mode()==4 && fp_ai_route_water_enabled(player);
}

extern "C" int fp_ai_route_hold(uint32_t player) {
    if (!controlled(player)) return 0;
    const uint32_t request=word(player+0x155C);
    // A zero-waypoint path can be a valid direct result. Hold only a completed,
    // unsuccessful graph query, leaving the interpreter's retry timer running.
    return valid(request,0x2C) && vm_base[request+0x1C] && !word(request+0x18);
}
