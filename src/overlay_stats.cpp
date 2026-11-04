#include "overlay_stats.h"
#include "settings.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#endif

extern "C" {
void fp_audio_get_stats(unsigned long long* blocks, unsigned long long* holes, double* device_ms, double* queued_ms);
double rsx_vulkan_backend_interp_gpu_ms(void);
unsigned long long fp_audio_limited_blocks(void);
extern unsigned long long g_voice_tx_blocks, g_voice_rx_blocks;
extern int g_audio_jitter_blocks;
extern unsigned long long g_audio_late_blocks;
int np_psnr_enabled(void);
int np_psnr_connected(void);
int np_signaling_active_peers(void);
}

namespace {
using Clock = std::chrono::steady_clock;

struct Cpu {
    double process_pct = 0;      // of one core, so 200 means two cores busy
    double top_pct = 0;
    char top_name[20] = "-";
};

#ifdef _WIN32
double process_cpu_seconds() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const auto to_s = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
    return to_s(kernel) + to_s(user);
}
// Per thread times need a thread snapshot; the process total is enough here.
void busiest_thread(double, Cpu&) {}
#else
double process_cpu_seconds() {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// utime + stime in clock ticks and the thread name from /proc/self/task/<tid>/stat.
bool read_thread(const char* tid, double& ticks, char (&name)[20]) {
    char path[64], buf[1024];
    std::snprintf(path, sizeof path, "/proc/self/task/%s/stat", tid);
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    const char* open = std::strchr(buf, '(');
    const char* close = std::strrchr(buf, ')');
    if (!open || !close) return false;
    std::snprintf(name, sizeof name, "%.*s", (int)std::min<size_t>(close - open - 1, sizeof name - 1), open + 1);
    // after ") " the fields run from number 3 (state); utime is number 14 and stime number 15
    unsigned long ut = 0, st = 0;
    if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) return false;
    ticks = (double)(ut + st);
    return true;
}

struct ThreadTicks { std::string tid; double ticks; };

void busiest_thread(double wall_s, Cpu& out) {
    static ThreadTicks prev[512];
    static size_t prev_n = 0;
    const double hz = (double)sysconf(_SC_CLK_TCK);
    ThreadTicks now[512];
    size_t now_n = 0;
    double best = 0;
    char best_name[20] = "-";
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.' || now_n >= 512) continue;
            double ticks;
            char name[20];
            if (!read_thread(e->d_name, ticks, name)) continue;
            now[now_n++] = {e->d_name, ticks};
            for (size_t i = 0; i < prev_n; ++i) {
                if (prev[i].tid != e->d_name) continue;
                const double pct = 100.0 * (ticks - prev[i].ticks) / hz / wall_s;
                if (pct > best) { best = pct; std::memcpy(best_name, name, sizeof best_name); }
                break;
            }
        }
        closedir(d);
    }
    for (size_t i = 0; i < now_n; ++i) prev[i] = now[i];
    prev_n = now_n;
    out.top_pct = best;
    std::memcpy(out.top_name, best_name, sizeof out.top_name);
}
#endif

// Process and busiest thread CPU, refreshed about once a second.
const Cpu& cpu_now() {
    static Cpu cpu;
    static Clock::time_point last = Clock::now();
    static double last_cpu = process_cpu_seconds();
    const auto now = Clock::now();
    const double wall = std::chrono::duration<double>(now - last).count();
    if (wall >= 1.0) {
        const double used = process_cpu_seconds();
        cpu.process_pct = 100.0 * (used - last_cpu) / wall;
        busiest_thread(wall, cpu);
        last_cpu = used;
        last = now;
    }
    return cpu;
}

struct AudioRates {
    double blocks_per_s = 0;
    unsigned long long holes_10s = 0;
    double device_ms = 0, queued_ms = 0;
};

AudioRates audio_now() {
    static unsigned long long last_blocks = 0;
    static Clock::time_point last = Clock::now();
    static std::deque<std::pair<Clock::time_point, unsigned long long>> holes;
    static AudioRates r;
    unsigned long long blocks, hole_total;
    double device, queued;
    fp_audio_get_stats(&blocks, &hole_total, &device, &queued);
    const auto now = Clock::now();
    const double wall = std::chrono::duration<double>(now - last).count();
    if (wall >= 1.0) {
        r.blocks_per_s = (blocks - last_blocks) / wall;
        last_blocks = blocks;
        last = now;
    }
    holes.emplace_back(now, hole_total);
    while (holes.size() > 1 && now - holes[1].first >= std::chrono::seconds(10)) holes.pop_front();
    r.holes_10s = hole_total - holes.front().second;
    r.device_ms = device;
    r.queued_ms = queued;
    return r;
}

}  // namespace

namespace fp {
std::string stats_overlay_text(const PresentWindow& w) {
    char line[160];
    std::string out;
    const auto add = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof line, fmt, args...);
        out += line;
    };
    const double secs = w.secs > 0 ? w.secs : 1;
    const double game_fps = w.guest_flips / secs;
    const double present_fps = w.presents / secs;

    add("GAME\n");
    add("  FPS        %6.1f   TARGET %.0f HZ\n", game_fps, w.pace_hz);
    // The game steps once per flip with dt = 1 / lock, so its simulated seconds per wall second are fps / lock.
    // Time based mode takes dt from the measured frame time, which makes that 1.00 by construction.
    const float lock = settings_frame_lock();
    if (lock > 0) add("  SPEED      %5.2fX   LOCK %.0f\n", game_fps / lock, lock);
    else add("  SPEED      TIME BASED\n");

    {
        const std::string res = settings_get("resolution"), scale = settings_get("render_scale");
        add("  OUTPUT     %s   SCALE %s\n", res.c_str(), scale.empty() ? "1X" : scale.c_str());
        if (const char* ring = std::getenv("FP_FRAME_RING")) if (std::atoi(ring) > 0) add("  RING       %s FRAMES   F9 SAVES\n", ring);
    }
    add("HOST\n");
    add("  PRESENT    %6.1f FPS  %5.1f MS\n", present_fps, w.frame_avg_ms);
    add("  P95 / MAX  %6.1f / %.1f MS\n", w.p95_ms, w.max_ms);
    const Cpu& cpu = cpu_now();
    add("  CPU        %5.0f%%   %s %.0f%%\n", cpu.process_pct, cpu.top_name, cpu.top_pct);
    add("  DRAWS      %6u   SUBMITS %.1f\n", w.draws, w.submits);
    const int interp = settings_interp();
    if (interp > 1) add("  INTERP     %5dX   GPU %.1f MS\n", interp, rsx_vulkan_backend_interp_gpu_ms());
    else add("  INTERP        OFF\n");

    add("AUDIO\n");
    const AudioRates a = audio_now();
    add("  MIX        %6.1f BLOCKS/S\n", a.blocks_per_s);
    add("  HOLES      %6llu IN 10 S\n", a.holes_10s);
    add("  DELAY      %6.1f MS   BUF %.1f + Q %.1f + J %.1f\n", a.device_ms + a.queued_ms + g_audio_jitter_blocks * 5.33, a.device_ms, a.queued_ms, g_audio_jitter_blocks * 5.33);
    add("  LATE       %6llu BLOCKS   LIMITED %llu\n", g_audio_late_blocks, fp_audio_limited_blocks());
    if (np_psnr_enabled()) {
        static unsigned long long tx0 = 0, rx0 = 0, t0 = 0;
        static double txs = 0, rxs = 0;
        const auto now = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        if (!t0) { t0 = now; tx0 = g_voice_tx_blocks; rx0 = g_voice_rx_blocks; }
        else if (now - t0 >= 2000) {
            txs = (g_voice_tx_blocks - tx0) * 1000.0 / (now - t0); rxs = (g_voice_rx_blocks - rx0) * 1000.0 / (now - t0);
            t0 = now; tx0 = g_voice_tx_blocks; rx0 = g_voice_rx_blocks;
        }
        add("  VOICE      TX %.0f  RX %.0f BLOCKS/S\n", txs, rxs);
    }

    if (np_psnr_enabled()) {
        add("NET\n");
        add("  PSNR       %s   PEERS %d\n", np_psnr_connected() ? "CONNECTED" : "OFFLINE", np_signaling_active_peers());
        add("  NAME       %s\n", settings_get("name").c_str());
    }
    add("F3 HIDE   F1 MENU");
    return out;
}
}  // namespace fp
