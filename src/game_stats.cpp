#include "game_stats.h"
#include "game_art.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>
#include "ppu_recomp.h"

extern "C" uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" int64_t sys_memory_allocate(ppu_context* ctx);

namespace {
constexpr uint32_t kScoreManagerPtr = 0xF54AA0;   // pointer to the score manager (US 1.00 program)
std::atomic<bool> g_requested{false};
std::mutex g_lock;
std::map<int, fp::TrophyProgress> g_progress;
uint32_t g_scratch;   // guest memory for the getter's text and value output

// The game's name hash: lower case, two CRC-CCITT registers fed alternately (0x001EA5F0).
uint32_t stat_hash(const std::string& s) {
    static uint16_t table[256];
    if (!table[1]) for (int i = 0; i < 256; ++i) { unsigned c = unsigned(i) << 8; for (int k = 0; k < 8; ++k) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) : (c << 1); table[i] = uint16_t(c); }
    uint16_t reg[2] = {0xFFFF, 0xFFFF};
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned ch = (unsigned char)s[i];
        if (ch < 128) ch = unsigned(std::tolower(int(ch)));
        const int k = int(i & 1);
        reg[k] = uint16_t((reg[k] << 8) ^ table[(reg[k] >> 8) ^ ch]);
    }
    return uint32_t(reg[0]) << 16 | reg[1];
}
const char* unit_of(int trophy) {
    switch (trophy) {
        case 0: case 5: case 6: case 13: case 16: case 17: case 21: case 22: case 23: case 24: case 25: case 26: return "kills";
        case 1: return "resources";   case 2: return "calories";   case 3: return "builds";   case 4: return "blocks";
        case 7: return "hearts";      case 8: return "slaps";      case 12: return "escorts"; case 15: return "meters";
        case 20: return "wins";       default: return "";
    }
}
struct Rule { int trophy; std::string stat; int target; };
std::vector<Rule> rules() {
    std::vector<Rule> out;
    std::string text;
    if (!fp::art_text_member("data/gamescripts/trophies.txt", text)) return out;
    static const std::regex re(R"(trophy\s+(\d+)\s*\{\s*unlock\s+(\w+)\s*>=\s*(\d+))");
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        out.push_back({std::atoi((*it)[1].str().c_str()), (*it)[2].str(), std::atoi((*it)[3].str().c_str())});
    return out;
}
// The getter registered for a statistic id: a node of the std::map at manager+0x480 holds a pointer-to-member.
uint32_t getter_of(uint32_t manager, uint32_t id) {
    const uint32_t header = vm_read32(manager + 0x480);
    uint32_t node = vm_read32(header + 4);
    for (int depth = 0; node && node != header && depth < 64; ++depth) {
        if (vm_read8(node + 0x19)) return 0;
        const uint32_t key = vm_read32(node + 0xC);
        if (key == id) {
            const uint32_t f = vm_read32(node + 0x10), delta = vm_read32(node + 0x14);
            return (f & 1) ? vm_read32(vm_read32(manager + delta) + f - 1) : f;   // an OPD
        }
        node = vm_read32(node + (id < key ? 0 : 8));
    }
    return 0;
}
}

namespace fp {
std::atomic<unsigned> g_generation{0};
unsigned game_stats_generation() { return g_generation; }
void game_stats_request() { g_requested = true; }
bool game_stats_take_request() { return g_requested.exchange(false); }
bool game_stats_progress(int trophy_id, TrophyProgress& out) {
    std::lock_guard<std::mutex> lock(g_lock);
    const auto it = g_progress.find(trophy_id);
    if (it == g_progress.end()) return false;
    out = it->second;
    return true;
}
}

// Guest thread (the console's `echo fp stats`): run the getter of every counting trophy's statistic.
extern "C" void fp_game_stats_collect(void) {
    ppu_context* ctx = g_active_ctx;
    if (!ctx) return;
    static const std::vector<Rule> list = rules();
    const uint32_t manager = vm_read32(kScoreManagerPtr);
    if (!manager || list.empty()) return;
    const uint32_t begin = vm_read32(manager + 0x4BC + 4), end = vm_read32(manager + 0x4BC + 8);
    if (!begin || end < begin || (end - begin) / 8 > 512) return;
    if (!g_scratch) {
        const uint64_t r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
        const uint32_t cell = uint32_t(ctx->gpr[1]) - 0x200;   // free stack below sp
        ctx->gpr[3] = 0x10000; ctx->gpr[4] = 0; ctx->gpr[5] = cell;
        if (sys_memory_allocate(ctx) == 0) g_scratch = vm_read32(cell);
        ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5;
        if (!g_scratch) return;
    }
    const ppu_context saved = *ctx;
    std::map<int, fp::TrophyProgress> fresh;
    for (const Rule& r : list) {
        if (r.target <= 1) continue;
        uint32_t id = 0xFFFFFFFF;
        const uint32_t h = stat_hash(r.stat);
        for (uint32_t a = begin; a < end; a += 8) if (vm_read32(a) == h) { id = vm_read32(a + 4); break; }
        if (id > 255) continue;
        const uint32_t opd = getter_of(manager, id);
        if (!opd || !vm_read32(opd)) continue;
        vm_write32(g_scratch + 0x80, 0);
        ppu_guest_call_ct(vm_read32(opd), vm_read32(opd + 4), manager, g_scratch, g_scratch + 0x80, 0, 0, 0, 0, 0);
        const uint32_t bits = vm_read32(g_scratch + 0x80);
        float v;
        std::memcpy(&v, &bits, 4);
        if (!(v >= 0.f) || v > 1e9f) continue;
        fresh[r.trophy] = {int(std::lround(v)), r.target, unit_of(r.trophy)};
    }
    *ctx = saved;
    std::lock_guard<std::mutex> lock(g_lock);
    g_progress = std::move(fresh);
    ++fp::g_generation;
    std::printf("[fp-stats] read %zu counters, trophy 0 = %d\n", g_progress.size(), g_progress.count(0) ? g_progress[0].value : -1);
}
