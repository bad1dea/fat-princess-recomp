#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <SDL2/SDL.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
#include "ios_touch.h"
#else
struct fp_touch_state { uint32_t buttons; int32_t lx, ly; };
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace {
std::atomic<double> g_pace_hz{60.0};          // rate the game is released and presented at
std::atomic<long long> g_last_tick_ns{0};     // when the emulated vblank last released the game
std::atomic<double> g_work_ms{0.0};           // time from that release to the game queueing its next flip

// FP_PACE_LOG=path: one line per event ("T tick_ns", "F flip_queued_ns", "P present_done_ns", CLOCK_MONOTONIC_RAW) for pacing analysis.
void pace_log(char kind) {
    static FILE* f=[]()->FILE* {const char* p=std::getenv("FP_PACE_LOG");return p&&*p?std::fopen(p,"w"):nullptr;}();
    if(!f) return;
#ifdef _WIN32
    const long long ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    timespec ts;clock_gettime(CLOCK_MONOTONIC_RAW,&ts);
    const long long ns=(long long)ts.tv_sec*1000000000ll+ts.tv_nsec;
#endif
    static std::atomic_flag lock=ATOMIC_FLAG_INIT;
    while(lock.test_and_set(std::memory_order_acquire)) {}
    std::fprintf(f,"%c %lld\n",kind,ns);
    lock.clear(std::memory_order_release);
}
}
#include "debug_hud.h"
#include "overlay_stats.h"
#include "settings.h"
#include "game_controls.h"
#include "osk_panel.h"
#include "trophy_ui.h"
#include <deque>
#include <mutex>
#include "self_test.h"
#include "room_password.h"
#include "settings_panel.h"
#include "../third_party/ps3recomp/libs/video/rsx_debug_stats.h"

extern "C" int rsx_vulkan_backend_init(uint32_t, uint32_t, const char*);
extern "C" void rsx_vulkan_backend_present(void);
extern "C" void rsx_vulkan_backend_dump_swapchain(const char* path);
extern "C" int rsx_vulkan_backend_display_hz(void);
extern "C" void fp_pad_inject(unsigned mask, unsigned hold_ms);
extern "C" void fp_console_set_source(int (*source)(char*, unsigned));
extern "C" void sceNpTrophySetUnlockHook(void (*hook)(int, const char*, int, const unsigned char*, unsigned));
extern "C" void rsx_engine_pass_dump_request(const char* dir, unsigned passes);
extern "C" void rsx_vulkan_backend_set_interp(int factor);
extern "C" void rsx_vulkan_backend_set_overlay_anchor(int anchor);
extern "C" void rsx_vulkan_backend_set_fullscreen(int on);
extern "C" void rsx_vulkan_backend_interp_flip(uint32_t flips);
extern "C" void rsx_vulkan_backend_interp_service(void);
extern "C" int rsx_vulkan_backend_cached_frame(const unsigned char** px, uint32_t* w, uint32_t* h);
extern "C" int rsx_vulkan_backend_pump_messages(void);
#ifdef _WIN32
extern "C" int fp_d3d12_init(uint32_t, uint32_t, const char*);
extern "C" void fp_d3d12_present(void);
extern "C" int fp_d3d12_pump_messages(void);
#endif
#ifdef _WIN32
extern "C" void fp_d3d12_set_debug_overlay(const uint8_t*,uint32_t,uint32_t,const char*);
extern "C" void fp_d3d12_set_overlay_anchor(int);
extern "C" void fp_d3d12_set_notify_overlay(const uint8_t*,uint32_t,uint32_t,uint32_t);
extern "C" void fp_d3d12_set_overlay_radius(uint32_t);
extern "C" void* fp_d3d12_get_hwnd(void);
extern "C" void fp_d3d12_set_fullscreen(int);
#endif
// D3D12 owns its host panel and OSK. Interpolation, frame capture and GPU statistics use Vulkan.
static bool g_d3d12 = false;
extern "C" int fp_gfx_is_d3d12(void) { return g_d3d12 ? 1 : 0; }
extern "C" void fp_backend_set_fullscreen(int on) {
#ifdef _WIN32
    if (g_d3d12) { fp_d3d12_set_fullscreen(on); return; }
#endif
    rsx_vulkan_backend_set_fullscreen(on);
}

extern "C" int rsx_vulkan_backend_set_output_size(uint32_t, uint32_t);
#ifdef _WIN32
extern "C" int fp_d3d12_set_output_size(uint32_t, uint32_t);
#endif
extern "C" int fp_backend_set_output_size(uint32_t width, uint32_t height) {
#ifdef _WIN32
    if (g_d3d12) return fp_d3d12_set_output_size(width, height);
#endif
    return rsx_vulkan_backend_set_output_size(width, height);
}

extern "C" void rsx_vulkan_backend_set_overlay_radius(uint32_t radius);
// Host panels (menu, stats, keyboard) get rounded corners and sit off the window corner, like the trophy card.
static void fp_set_overlay(const uint8_t* pixels,uint32_t w,uint32_t h,const char* title) {
    const uint32_t radius = fp::settings_get("resolution") == "1080P" ? 15 : 10;
#ifdef _WIN32
    if(g_d3d12) { fp_d3d12_set_overlay_radius(radius);fp_d3d12_set_debug_overlay(pixels,w,h,title);return; }
#endif
    rsx_vulkan_backend_set_overlay_radius(radius);
    rsx_vulkan_backend_set_debug_overlay(pixels,w,h,title);
}
extern "C" void rsx_vulkan_backend_set_notify_overlay(const uint8_t*,uint32_t,uint32_t,uint32_t);
static void fp_set_notify(const uint8_t* pixels,uint32_t w,uint32_t h,uint32_t radius) {
#ifdef _WIN32
    if(g_d3d12) { fp_d3d12_set_notify_overlay(pixels,w,h,radius);return; }
#endif
    rsx_vulkan_backend_set_notify_overlay(pixels,w,h,radius);
}

static void fp_dump_overlay(const std::vector<std::uint8_t>& rgba, unsigned w, unsigned h, const char* tag);
// Trophy cards: the trophy library reports each fresh unlock on the guest thread; the window thread shows them one at a
// time, top right, for five seconds each: a chime, then the card slides in from the right edge. F2 or the controller's
// Guide button opens the trophy's details while it is up (trophy_ui.cpp).
static std::mutex g_trophy_mutex;
static std::deque<std::pair<int, fp::TrophyCard>> g_trophy_queue;
static void fp_trophy_unlocked(int id, const char* name, int grade, const unsigned char* png, unsigned png_size) {
    fp::TrophyCard card{name ? name : "", grade, png ? std::vector<uint8_t>(png, png + png_size) : std::vector<uint8_t>{}, {}};
    std::lock_guard<std::mutex> lock(g_trophy_mutex);
    if (g_trophy_queue.size() < 32) g_trophy_queue.emplace_back(id, std::move(card));
}
extern "C" int sceNpTrophyPreviewUnlock(int trophyId);
static void fp_trophy_cards_tick() {
    using Clock = std::chrono::steady_clock;
    // FP_TROPHY_PREVIEW=0,12,... queues those cards once the trophy set is registered, without unlocking anything.
    static const char* preview = std::getenv("FP_TROPHY_PREVIEW");
    while (preview && *preview) {
        if (sceNpTrophyPreviewUnlock(std::atoi(preview)) != 0) break;
        preview = std::strchr(preview, ',');
        if (preview) ++preview;
    }
    static bool showing = false;
    static Clock::time_point shown, until;
    static std::vector<uint8_t> card_px;
    static unsigned card_w = 0, card_h = 0, card_r = 0;
    if (showing && Clock::now() < until) {
        // slide in over 0.4 s: the card's left part grows out of the right edge (ease-out)
        const float t = std::chrono::duration<float>(Clock::now() - shown).count() / 0.4f;
        if (t < 1.f) {
            const float e = 1.f - (1.f - t) * (1.f - t);
            const unsigned vw = std::max(8u, unsigned(e * float(card_w)));
            std::vector<uint8_t> part(size_t(vw) * card_h * 4);
            for (unsigned y = 0; y < card_h; ++y)
                std::memcpy(&part[size_t(y) * vw * 4], &card_px[size_t(y) * card_w * 4], size_t(vw) * 4);
            fp_set_notify(part.data(), vw, card_h, card_r);
        } else if (t < 1.5f) fp_set_notify(card_px.data(), card_w, card_h, card_r);
        return;
    }
    std::pair<int, fp::TrophyCard> next;
    {
        std::lock_guard<std::mutex> lock(g_trophy_mutex);
        if (g_trophy_queue.empty()) {
            if (showing) { fp_set_notify(nullptr, 0, 0, 0); showing = false; }
            return;
        }
        next = std::move(g_trophy_queue.front());
        g_trophy_queue.pop_front();
    }
    next.second.hint = fp::settings_controller_hints() ? "GUIDE: DETAILS" : "F2: DETAILS";
    const float scale = fp::settings_get("resolution") == "1080P" ? 1.5f : 1.f;
    card_px = fp::trophy_card_pixels(next.second, scale, card_w, card_h, card_r);
    fp_dump_overlay(card_px, card_w, card_h, "trophy");
    if (fp::settings_get("trophy_sound") != "OFF") fp::trophy_chime();
    fp::trophy_ui_note_toast(next.first);
    showing = true;
    shown = Clock::now();
    until = shown + std::chrono::seconds(6);
}
static void fp_set_overlay_anchor(int anchor) {
#ifdef _WIN32
    if(g_d3d12) { fp_d3d12_set_overlay_anchor(anchor);return; }
#endif
    rsx_vulkan_backend_set_overlay_anchor(anchor);
}
extern "C" const unsigned char* fp_menu_keyboard_state(int* focused) {
#ifdef _WIN32
    if(g_d3d12) {
        static unsigned char keys[SDL_NUM_SCANCODES];
        std::memset(keys,0,sizeof keys);
        *focused=GetForegroundWindow()==(HWND)fp_d3d12_get_hwnd();
        if(*focused) {
            for(int sc=SDL_SCANCODE_A;sc<=SDL_SCANCODE_Z;++sc) keys[sc]=(GetAsyncKeyState('A'+sc-SDL_SCANCODE_A)&0x8000)!=0;
            for(int sc=SDL_SCANCODE_1;sc<=SDL_SCANCODE_9;++sc) keys[sc]=(GetAsyncKeyState('1'+sc-SDL_SCANCODE_1)&0x8000)!=0;
            for(int sc=SDL_SCANCODE_F1;sc<=SDL_SCANCODE_F12;++sc) keys[sc]=(GetAsyncKeyState(VK_F1+sc-SDL_SCANCODE_F1)&0x8000)!=0;
            const int map[][2]={{SDL_SCANCODE_0,'0'},{SDL_SCANCODE_RETURN,VK_RETURN},{SDL_SCANCODE_ESCAPE,VK_ESCAPE},
                {SDL_SCANCODE_UP,VK_UP},{SDL_SCANCODE_DOWN,VK_DOWN},{SDL_SCANCODE_LEFT,VK_LEFT},{SDL_SCANCODE_RIGHT,VK_RIGHT},
                {SDL_SCANCODE_PAGEUP,VK_PRIOR},{SDL_SCANCODE_PAGEDOWN,VK_NEXT},{SDL_SCANCODE_SPACE,VK_SPACE},
                {SDL_SCANCODE_BACKSPACE,VK_BACK},{SDL_SCANCODE_LSHIFT,VK_LSHIFT},{SDL_SCANCODE_RSHIFT,VK_RSHIFT},
                {SDL_SCANCODE_LCTRL,VK_LCONTROL},{SDL_SCANCODE_RCTRL,VK_RCONTROL},{SDL_SCANCODE_TAB,VK_TAB},
                {SDL_SCANCODE_LALT,VK_LMENU},{SDL_SCANCODE_RALT,VK_RMENU},
                {SDL_SCANCODE_PERIOD,VK_OEM_PERIOD},{SDL_SCANCODE_COMMA,VK_OEM_COMMA},{SDL_SCANCODE_MINUS,VK_OEM_MINUS},
                {SDL_SCANCODE_EQUALS,VK_OEM_PLUS},{SDL_SCANCODE_SEMICOLON,VK_OEM_1},{SDL_SCANCODE_SLASH,VK_OEM_2}};
            for(const auto& item:map) keys[item[0]]=(GetAsyncKeyState(item[1])&0x8000)!=0;
        }
        return keys;
    }
#endif
    *focused=SDL_GetKeyboardFocus()!=nullptr;
    return SDL_GetKeyboardState(nullptr);
}

// Feed keyboard input through SDL's public virtual-controller API, so the
// framework's existing cellPad implementation handles packets and pressure.
static SDL_Joystick* keyboard_pad;

extern "C" void fp_host_prepare(void) {
    fp_console_set_source(fp_player_control_command);
    sceNpTrophySetUnlockHook(fp_trophy_unlocked);
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[fp-input] SDL initialization failed: %s\n", SDL_GetError());
        return;
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            std::fprintf(stderr, "[fp-input] using physical controller: %s\n", SDL_GameControllerNameForIndex(i));
        }
    }
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    if (index < 0 || !(keyboard_pad = SDL_JoystickOpen(index))) {
        std::fprintf(stderr, "[fp-input] virtual controller failed: %s\n", SDL_GetError());
        return;
    }
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(keyboard_pad), guid, sizeof guid);
    char mapping[1024];
    std::snprintf(mapping, sizeof mapping,
        "%s,Fat Princess Keyboard,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
        "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
        "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,", guid);
    if (SDL_GameControllerAddMapping(mapping) < 0 || !SDL_IsGameController(index)) {
        std::fprintf(stderr, "[fp-input] keyboard mapping failed: %s\n", SDL_GetError());
        return;
    }
    SDL_JoystickSetVirtualAxis(keyboard_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(keyboard_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    SDL_JoystickUpdate();
    std::fprintf(stderr, "[fp-input] keyboard controller ready: F1 opens the settings menu\n");
}

extern "C" int fp_backend_init(uint32_t width, uint32_t height, const char* title) {
#ifdef _WIN32
    if (fp::settings_renderer_d3d12()) {
        std::fprintf(stderr, "[fp-gfx] renderer: D3D12 (live draw engine)\n");
        if (fp_d3d12_init(width, height, title) == 0) {
            g_d3d12 = true;
            SDL_InitSubSystem(SDL_INIT_VIDEO);
            return 0;
        }
        std::fprintf(stderr, "[fp-gfx] D3D12 init failed, falling back to Vulkan\n");
        _putenv_s("RSX_LIVE_DRAW", "0");
    }
#endif
    std::fprintf(stderr, "[fp-gfx] renderer: Vulkan\n");
    return rsx_vulkan_backend_init(width, height, title);
}

extern "C" uint32_t cellGcm_flip_request_count(void);
extern "C" uint32_t rsx_vulkan_backend_readback_center(void);
extern "C" uint32_t rsx_vulkan_backend_guest_draws(void);
// FP_OVERLAY_DUMP=path.ppm writes the overlay bitmap too, since a headless run has no swapchain to draw it on.
static void fp_dump_overlay(const std::vector<std::uint8_t>& rgba, unsigned w, unsigned h, const char* tag) {
    const char* path = std::getenv("FP_OVERLAY_DUMP");
    if (!path || !*path) return;
    char name[1100];
    static const bool seq = std::getenv("FP_OVERLAY_DUMP_SEQ") != nullptr;   // one file per change instead of overwriting
    static unsigned n = 0;
    if (seq) std::snprintf(name, sizeof name, "%s-%s-%03u.ppm", path, tag, n++);
    else std::snprintf(name, sizeof name, "%s-%s.ppm", path, tag);
    if (FILE* f = std::fopen(name, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < size_t(w) * h; ++i) std::fwrite(&rgba[i * 4], 1, 3, f);
        std::fclose(f);
    }
}
static bool fp_stats_allowed() {
    static bool enabled = !std::getenv("FP_STATS") || std::strcmp(std::getenv("FP_STATS"),"0") != 0;
    return enabled || fp::settings_stats_visible();
}
// Wall time on the host presenter thread; excludes parallel guest workers.
static double fp_fifo_wall_ms;
// FP_FRAME_RING=N keeps the last N presented guest frames (RGBA, ~3.7 MB each at 720p); F9 writes them to
// FP_FRAME_RING_DIR (default ./frame-ring-<time>) as PPMs named by flip number. For catching one-frame glitches.
static void fp_ring_store(uint32_t flip) {
    static const unsigned n = std::getenv("FP_FRAME_RING") ? unsigned(std::atoi(std::getenv("FP_FRAME_RING"))) : 0;
    if (!n) return;
    static std::vector<std::vector<unsigned char>> frames(n);
    static std::vector<uint32_t> flips(n, 0);
    static unsigned head = 0, w = 0, h = 0;
    static uint32_t last_flip = 0;
    static bool f9_down = false;
    if (flip && flip != last_flip) {
        const unsigned char* px; uint32_t fw, fh;
        if (rsx_vulkan_backend_cached_frame(&px, &fw, &fh)) {
            w = fw; h = fh;
            frames[head].assign(px, px + size_t(fw) * fh * 4);
            flips[head] = flip;
            head = (head + 1) % n;
        }
        last_flip = flip;
    }
    // FP_FRAME_RING_TRIGGER=path: the file appearing does what F9 does (headless runs have no keyboard).
    static const char* trigger = std::getenv("FP_FRAME_RING_TRIGGER");
    bool f9 = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F9] && SDL_GetKeyboardFocus();
    const bool triggered = trigger && *trigger && std::filesystem::exists(trigger);
    if (triggered) std::filesystem::remove(trigger);
    if (triggered || (f9 && !f9_down)) {
        const char* base = std::getenv("FP_FRAME_RING_DIR");
        char dir[1024];
        if (base && *base) std::snprintf(dir, sizeof dir, "%s", base);
        else std::snprintf(dir, sizeof dir, "frame-ring-%ld", long(std::time(nullptr)));
        std::filesystem::create_directories(dir);
        unsigned written = 0;
        std::vector<unsigned char> rgb(size_t(w) * h * 3);
        for (unsigned i = 0; i < n; ++i) {
            const unsigned k = (head + i) % n;
            if (frames[k].empty()) continue;
            char name[1100];
            std::snprintf(name, sizeof name, "%s/flip-%06u.ppm", dir, flips[k]);
            if (FILE* fp = std::fopen(name, "wb")) {
                std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
                for (size_t j = 0; j < size_t(w) * h; ++j)
                    std::memcpy(&rgb[j * 3], &frames[k][j * 4], 3);
                std::fwrite(rgb.data(), 1, rgb.size(), fp);
                std::fclose(fp);
                ++written;
            }
        }
        std::fprintf(stderr, "[fp-ring] wrote %u frames to %s\n", written, dir);
    }
    f9_down = f9;
}
static std::atomic<uint32_t> g_hang_presents{0};   // frames that reached fp_backend_present, for the hang watchdog
extern "C" void fp_backend_present(void) {
    g_hang_presents.fetch_add(1, std::memory_order_relaxed);
    using Clock = std::chrono::steady_clock;
    static const bool trace = std::getenv("FP_PRESENT_TIMING") && std::strcmp(std::getenv("FP_PRESENT_TIMING"),"0");
    static uint32_t last_flip=0, count=0, duplicates=0, drops=0, previous_luma=0;
    static std::array<double,120> intervals{};
    static unsigned filled=0, cursor=0, window_frames=0;
    static auto last=Clock::now(), epoch=last;
    static const bool epoch_logged=[&]{ std::fprintf(stderr,"[fp-stats] epoch_us=%llu\n",(unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(epoch.time_since_epoch()).count()); return true; }();
    (void)epoch_logged;
    static double window_ms=0, window_wait=0;
    static double previous_fifo_ms=0;
    static uint64_t window_submits=0;
    static uint32_t window_flip0=0;
    static rsx_debug_stats previous{};
    static FILE* csv=[]() -> FILE* {
        const char* path=std::getenv("FP_STATS_CSV");
        if(!path || !*path) return nullptr;
        FILE* f=std::fopen(path,"w");
        if(f) {std::fputs("elapsed_ms,frame,flip,frame_ms,draws,submissions,submit_wait_ms,surface,width,height,center_hex,mean_luma,dark_samples,duplicate,luma_drop,fifo_wall_ms,present_wall_ms,submit_total\n",f);std::setvbuf(f,nullptr,_IOLBF,0);}
        else std::fprintf(stderr,"[fp-stats] cannot open %s\n",path);
        return f;
    }();
    const uint32_t flip=cellGcm_flip_request_count();
    // Frame cap (FP_FPS_CAP, default 60, 0 = unlimited). Paces presents only; the guest's own
    // 30 Hz simulation step is untouched.
    // AUTO keeps a steady 60 while the machine holds it and falls back to an even 30, never an uneven mix; it goes
    // back to 60 only after a few seconds of clear headroom. The emulated vblank follows g_pace_hz too, so the game
    // itself is released at that rate.
    const auto ready=Clock::now();
    double cap_fps=fp::settings_fps_cap();
    if(cap_fps<0) {
        // Rates AUTO may pace at: the display refresh divided by 1, 2, 4..., from at most 240 down to 30 or more, so every
        // frame stays on screen a whole number of refreshes (55 fps on a 240 Hz screen alternates 4 and 5 refreshes and
        // looks like stutter). It starts at the highest tier at or under 60, drops a tier after two windows of late frames,
        // and tries the next tier up after a quiet spell. FP_AUTO_MAX_HZ caps the top tier.
        static std::vector<double> tiers=[]{
            std::vector<double> t;
            int refresh=g_d3d12?0:rsx_vulkan_backend_display_hz();
            if(refresh<=0) refresh=60;
            double top=240.;if(const char* e=std::getenv("FP_AUTO_MAX_HZ")) top=std::max(30.,std::atof(e));
            for(int k=1;k<=16;k*=2){const double hz=double(refresh)/k;if(hz<29.5) break;if(hz<=top+0.5) t.push_back(hz);}
            if(t.empty()) t={60.,30.};
            if(t.back()>31.) t.push_back(30.);
            std::fprintf(stderr,"[fp-pace] display %d Hz, AUTO tiers:",refresh);for(double h:t) std::fprintf(stderr," %.0f",h);std::fprintf(stderr,"\n");
            return t;}();
        static size_t tier=[]{size_t i=0;while(i+1<tiers.size() && tiers[i]>60.5) ++i;return i;}();
        static bool tier_set=false;if(!tier_set){tier_set=true;g_pace_hz=tiers[tier];}
        static auto last_ready=ready,window_start=ready,next_probe=ready+std::chrono::seconds(15);
        static unsigned frames=0,missed=0,bad_windows=0;
        static bool probing=false;static double probe_gap_s=15;
        if(flip) {
            const double interval=std::chrono::duration<double,std::milli>(ready-last_ready).count();
            const double hz=g_pace_hz.load(),period=1000./hz;
            // a loading stall (a quarter second or more) says nothing about the rate the game can hold
            if(interval<250.) {++frames;if(interval>period*1.25) ++missed;}
            const double elapsed=std::chrono::duration<double>(ready-window_start).count();
            if(elapsed>=2.0 && frames>=30) {
                const bool bad=missed>frames*0.15;
                if(!bad) {bad_windows=0;if(probing) {probing=false;probe_gap_s=15;}}
                else if(tier+1<tiers.size() && (probing || ++bad_windows>=2)) {
                    // a failed probe backs off further; two bad windows in a row drop a tier
                    if(probing) probe_gap_s=std::min(probe_gap_s*2,240.);
                    probing=false;bad_windows=0;++tier;g_pace_hz=tiers[tier];
                    next_probe=ready+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(probe_gap_s));
                    std::fprintf(stderr,"[fp-pace] %.0f -> %.0f (%u of %u frames late), next try in %.0f s\n",hz,tiers[tier],missed,frames,probe_gap_s);
                }
                frames=missed=0;window_start=ready;
            }
            if(tier>0 && !probing && ready>=next_probe) {
                --tier;g_pace_hz=tiers[tier];probing=true;frames=missed=0;window_start=ready;
                next_probe=ready+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(probe_gap_s));
                std::fprintf(stderr,"[fp-pace] %.0f -> %.0f (trying)\n",tiers[tier+1],tiers[tier]);
            }
        }
        last_ready=ready;
        cap_fps=g_pace_hz.load();
    } else g_pace_hz=cap_fps>0?std::max(30.,cap_fps):240.;
    const double cap_ms=cap_fps>0?1000./cap_fps:0.;
    if(cap_ms>0 && flip) {
        // The vblank tick that released this frame already runs at the cap rate, so the present only has to be steady
        // relative to it. Hold it to a fixed lead after that tick (the recent worst case of tick -> flip queued, plus a
        // margin) instead of a whole period later: same interval stability, one frame less input-to-display latency.
        // FP_PRESENT_LEAD_MS sets the lead. Off by default: the guest cannot start its next frame while its flip is held
        // here, so a lead near a period made it miss every other tick (a 60 cap ran at 45 fps, 22 ms frames, on testhost;
        // without the lead 59 fps, p95 18 ms).
        static const char* lead_env=std::getenv("FP_PRESENT_LEAD_MS");
        if(!lead_env) goto paced;
        static double lead_ms=8.0;
        static std::array<float,32> works{};static unsigned wi=0;
        const auto tick_t=Clock::time_point(std::chrono::nanoseconds(g_last_tick_ns.load()));
        const double work=std::chrono::duration<double,std::milli>(Clock::now()-tick_t).count();
        if(work>0 && work<cap_ms) {
            works[wi++%works.size()]=float(work);
            const double worst=*std::max_element(works.begin(),works.end())+2.0;
            lead_ms=std::max(worst,lead_ms-0.05);   // grows at once, shrinks slowly
        }
        const double lead=lead_env?std::atof(lead_env):lead_ms;
        const auto target=tick_t+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double,std::milli>(lead));
        auto t=Clock::now();
        if(t+std::chrono::microseconds(1500)<target) std::this_thread::sleep_until(target-std::chrono::microseconds(1000));
        while(Clock::now()<target) {}
    }
    paced:
    // FP_DUMP_MEM_AT_S=t1,t2,... with FP_DUMP_MEM_DIR=dir: write the guest heap (0x40000000 to 0x50000000) at those times.
    {
        static const char* when=std::getenv("FP_DUMP_MEM_AT_S");
        static const char* dir=std::getenv("FP_DUMP_MEM_DIR");
        static const auto t0=Clock::now();static unsigned next_dump=0;
        if(when && dir) {
            std::vector<double> times;{const char* q=when;char* e;while(*q){times.push_back(std::strtod(q,&e));q=*e==','?e+1:e;if(e==q-0&&!*e)break;if(*e=='\0')break;}}
            if(next_dump<times.size() && std::chrono::duration<double>(Clock::now()-t0).count()>=times[next_dump]) {
                extern uint8_t* vm_base;char path[1024];std::snprintf(path,sizeof path,"%s/mem-%u.bin",dir,next_dump);
                if(FILE* f=std::fopen(path,"wb")) {std::fwrite(vm_base+0x40000000ull,1,0x10000000,f);std::fclose(f);}
                std::fprintf(stderr,"[fp-dump] wrote %s at %.2f s\n",path,std::chrono::duration<double>(Clock::now()-t0).count());
                ++next_dump;
            }
        }
    }
    // QrkGfx_FrameRateLock and the PAL copy sit in the guest heap (found by poking a lock of 30). The game reads them
    // every frame, so GAME SPEED applies without a restart. Only touched while the pair holds a plausible lock value.
    {
        extern uint8_t* vm_base;
        const float want=fp::settings_frame_lock();
        float cur[2];for(int k=0;k<2;++k){uint32_t w;std::memcpy(&w,vm_base+0x4000952C+4*k,4);w=__builtin_bswap32(w);std::memcpy(&cur[k],&w,4);}
        static float written=-1.f;   // a lock derived from the speed setting counts as ours too
        auto lockish=[](float f){return f==0.f||f==30.f||f==60.f||f==written;};
        if(lockish(cur[0]) && cur[0]==cur[1] && cur[0]!=want) {
            written=want;
            uint32_t b;std::memcpy(&b,&want,4);b=__builtin_bswap32(b);
            std::memcpy(vm_base+0x4000952C,&b,4);std::memcpy(vm_base+0x40009530,&b,4);
            std::fprintf(stderr,"[fp-lock] frame lock %g -> %g\n",cur[0],want);
        }
    }
    // FP_SET_AT=id=value@seconds[,...]: change settings as if from the menu at the given times (live-apply tests).
    {
        static const char* spec=std::getenv("FP_SET_AT");
        static const auto t0=Clock::now();
        static std::string pending=spec?spec:"";
        const double now=std::chrono::duration<double>(Clock::now()-t0).count();
        while(!pending.empty()) {
            const size_t comma=pending.find(',');const std::string one=pending.substr(0,comma);
            const size_t eq=one.find('='),at=one.rfind('@');
            if(eq==std::string::npos||at==std::string::npos||std::atof(one.c_str()+at+1)>now) break;
            const bool ok=fp::settings_set(one.substr(0,eq).c_str(),one.substr(eq+1,at-eq-1));
            std::fprintf(stderr,"[fp-set] %s -> %s\n",one.substr(0,at).c_str(),ok?"applied":"unknown or unavailable item");
            pending=comma==std::string::npos?std::string():pending.substr(comma+1);
        }
    }
    // FP_SET_FILE=path: id=value lines applied as if from the menu when the file appears, then the file is removed
    // (tests that switch a setting once the match is up). Checked every 30 frames.
    {
        static const char* path=std::getenv("FP_SET_FILE");
        static unsigned tick=0;
        if(path && *path && ++tick%30==0) if(FILE* f=std::fopen(path,"r")) {
            char line[256];
            while(std::fgets(line,sizeof line,f)) {
                std::string one(line);while(!one.empty()&&(one.back()=='\n'||one.back()=='\r'))one.pop_back();
                const size_t eq=one.find('=');if(eq==std::string::npos)continue;
                const bool ok=fp::settings_set(one.substr(0,eq).c_str(),one.substr(eq+1));
                std::fprintf(stderr,"[fp-set] %s -> %s\n",one.c_str(),ok?"applied":"unknown or unavailable item");
            }
            std::fclose(f);std::remove(path);
        }
    }
    // FP_HOLD_AT=mask@seconds:ms[,...]: hold a pad mask from that time for that long (tests that need a held button).
    {
        static const char* spec=std::getenv("FP_HOLD_AT");
        static const auto t0=Clock::now();
        static std::string pending=spec?spec:"";
        const double now=std::chrono::duration<double>(Clock::now()-t0).count();
        while(!pending.empty()) {
            const size_t comma=pending.find(',');
            const std::string one=pending.substr(0,comma);
            const size_t at=one.find('@'),colon=one.find(':');
            if(at==std::string::npos||colon==std::string::npos||std::atof(one.c_str()+at+1)>now) break;
            const unsigned mask=unsigned(std::strtoul(one.c_str(),nullptr,0)),ms=unsigned(std::atoi(one.c_str()+colon+1));
            fp_pad_inject(mask,ms);
            std::fprintf(stderr,"[fp-hold] 0x%04X for %u ms (t=%.1f)\n",mask,ms,now);
            pending=comma==std::string::npos?std::string():pending.substr(comma+1);
        }
    }
    // FP_ACTION_AT=act_id@seconds[,...]: run DEBUG action rows at the given times (pad combo tests).
    {
        static const char* spec=std::getenv("FP_ACTION_AT");
        static const auto t0=Clock::now();
        static std::string pending=spec?spec:"";
        const double now=std::chrono::duration<double>(Clock::now()-t0).count();
        while(!pending.empty()) {
            const size_t comma=pending.find(',');
            const std::string one=pending.substr(0,comma);
            const size_t at1=one.rfind('@');
            if(at1==std::string::npos||std::atof(one.c_str()+at1+1)>now) break;
            const bool ok=fp::settings_action(one.substr(0,at1).c_str());
            std::fprintf(stderr,"[fp-action] %s -> %s (t=%.1f)\n",one.c_str(),ok?"sent":"not an action",now);
            pending=comma==std::string::npos?std::string():pending.substr(comma+1);
        }
    }
    // FP_FIND_STR=text with FP_FIND_AT_S=n: once, n seconds in, list where the guest holds that text (debugging aid).
    {
        static const char* find=std::getenv("FP_FIND_STR");
        static const double at=std::getenv("FP_FIND_AT_S") ? std::atof(std::getenv("FP_FIND_AT_S")) : 60.;
        static const auto t0=Clock::now();static bool done=false;
        if(find && !done && std::chrono::duration<double>(Clock::now()-t0).count()>=at) {
            done=true;extern uint8_t* vm_base;const size_t n=std::strlen(find);unsigned hits=0;
            for(uint64_t a=0;a<0x50000000ull && hits<40;a+=1) {
                if(std::memcmp(vm_base+a,find,n)==0 && (!std::getenv("FP_FIND_EXACT") || (a>0 && vm_base[a-1]==0 && vm_base[a+n]==0))) {
                    ++hits;std::fprintf(stderr,"[fp-find] \"%s\" at 0x%08llX  ctx: ",find,(unsigned long long)a);
                    for(int k=-8;k<24;++k){unsigned char c=vm_base[a+k];std::fputc(c>=32&&c<127?c:'.',stderr);}
                    std::fputc('\n',stderr);
                }
            }
            std::fprintf(stderr,"[fp-find] %u hits\n",hits);
        }
    }
    const auto present_start=Clock::now();
    if(g_d3d12) {
#ifdef _WIN32
        fp_d3d12_present();
#endif
    } else {
        rsx_vulkan_backend_set_interp(fp::settings_interp());
        rsx_vulkan_backend_interp_flip(flip);
        rsx_vulkan_backend_present();
        fp_ring_store(flip);
        static const auto test_epoch=Clock::now();
        static bool auto_test_started=false;
        if(!auto_test_started && std::getenv("FP_SELFTEST_AT") && std::chrono::duration<double>(Clock::now()-test_epoch).count()>=std::atof(std::getenv("FP_SELFTEST_AT")) && fp::player_control_state().available) {
            auto_test_started=true;
            fp::self_test_start(std::getenv("FP_SELFTEST_SCENARIO")?std::getenv("FP_SELFTEST_SCENARIO"):"AI SOAK",std::getenv("FP_SELFTEST_SECONDS")?std::atoi(std::getenv("FP_SELFTEST_SECONDS")):60);
        }
        const unsigned char* test_pixels=nullptr; uint32_t test_w=0,test_h=0;
        if(fp::self_test_status().running) {
            rsx_vulkan_backend_cached_frame(&test_pixels,&test_w,&test_h);
            fp::self_test_frame(flip,test_pixels,test_w,test_h);
        }
        static const bool test_exit=std::getenv("FP_SELFTEST_EXIT")!=nullptr;
        static Clock::time_point test_finished;
        const auto result=fp::self_test_status();
        if(test_exit && !result.running && result.result!="READY") {
            if(test_finished==Clock::time_point{}) test_finished=Clock::now();
            if(std::chrono::duration<double>(Clock::now()-test_finished).count()>=1 && !fp::player_control_state().pending) std::exit(0);
        }
    }
    if(flip) pace_log('P');
    const double present_ms=std::chrono::duration<double,std::milli>(Clock::now()-present_start).count();
    // F10: dump the next frame pass by pass (every draw, its pipeline, render targets and depth) into pass-dump-<time>,
    // for comparing an effect with a console capture. The pipelines' HLSL go to shader-dump (RSX_ENGINE_SHADER_DUMP).
    if(!g_d3d12) {
        static bool f10_down=false;
        static const double f10_at=std::getenv("FP_PASS_DUMP_AT")?std::atof(std::getenv("FP_PASS_DUMP_AT")):-1.;   // tests: as if F10 then
        static const auto f10_t0=Clock::now();
        static bool f10_fired=false;
        bool f10=SDL_GetKeyboardFocus() && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F10];
        // FP_PASS_DUMP_EVERY=s repeats it every s seconds after the first, FP_PASS_DUMP_MAX times (default 20).
        static const double f10_every=std::getenv("FP_PASS_DUMP_EVERY")?std::atof(std::getenv("FP_PASS_DUMP_EVERY")):0.;
        static const int f10_max=std::getenv("FP_PASS_DUMP_MAX")?std::atoi(std::getenv("FP_PASS_DUMP_MAX")):20;
        static double f10_next=f10_at;static int f10_count=0;
        if(f10_at>=0 && !f10_fired && std::chrono::duration<double>(Clock::now()-f10_t0).count()>=f10_next) {
            f10=true;f10_down=false;++f10_count;
            if(f10_every>0 && f10_count<f10_max) f10_next+=f10_every; else f10_fired=true;
        }
        if(f10 && !f10_down) {
            static int dump_seq=0;
            char dir[256];std::snprintf(dir,sizeof dir,"pass-dump-%ld-%02d",long(std::time(nullptr)),dump_seq++);
            std::filesystem::create_directories(dir);
            const std::string abs=std::filesystem::absolute(dir).string();
            rsx_engine_pass_dump_request(abs.c_str(),14);
            std::fprintf(stderr,"[fp-dump] capturing the next frame's passes into %s\n",abs.c_str());
        }
        f10_down=f10;
    }
    // FP_SHOT_DIR + FP_SHOT_EVERY_S: periodic frame dumps for watching a headless run.
    static const char* shot_dir=std::getenv("FP_SHOT_DIR");
    if(shot_dir && flip && !g_d3d12) {
        static const double every=std::getenv("FP_SHOT_EVERY_S") ? std::atof(std::getenv("FP_SHOT_EVERY_S")) : 10.;
        static auto last_shot=Clock::now();static unsigned shots=0;
        // FP_SHOT_AFTER_S delays the first dump; FP_SHOT_MAX stops after that many.
        static const double after=std::getenv("FP_SHOT_AFTER_S") ? std::atof(std::getenv("FP_SHOT_AFTER_S")) : 0.;
        static const unsigned maxshots=std::getenv("FP_SHOT_MAX") ? unsigned(std::atoi(std::getenv("FP_SHOT_MAX"))) : ~0u;
        static const auto started=Clock::now();
        if(shots<maxshots && std::chrono::duration<double>(Clock::now()-started).count()>=after &&
           std::chrono::duration<double>(Clock::now()-last_shot).count()>=every) {
            char path[1024];std::snprintf(path,sizeof path,"%s/shot-%04u.ppm",shot_dir,shots++);
            rsx_vulkan_backend_dump_cached_frame(path);last_shot=Clock::now();
            // the same frame as the window got it: if shot-N is picture and shot-N-window is black, presentation is at fault
            char window_path[1100];std::snprintf(window_path,sizeof window_path,"%s/shot-%04u-window.ppm",shot_dir,shots-1);
            rsx_vulkan_backend_dump_swapchain(window_path);
            std::fprintf(stderr,"[fp-shot] %u t=%.1f\n",shots-1,std::chrono::duration<double>(last_shot-started).count());
        }
    }
    const bool ai_control = fp::player_control_state().ai;
    static bool ai_badge_was_active = false;
    if (!ai_control && ai_badge_was_active && !fp::settings_stats_visible() &&
        !fp::settings_menu_open() && !fp::settings_osk_active() && !fp::trophy_ui_active())
        fp_set_overlay(nullptr,0,0,nullptr);
    ai_badge_was_active = ai_control;
    if(!flip || (!fp_stats_allowed() && !trace && !csv && !ai_control && !fp::self_test_status().running)) return;
    auto now=Clock::now();
    const double ms=std::chrono::duration<double,std::milli>(now-last).count();
    rsx_debug_stats stats{};if(!g_d3d12) rsx_vulkan_backend_get_debug_stats(&stats);
    const uint32_t draws=g_d3d12 ? 0 : rsx_vulkan_backend_guest_draws();
    const uint64_t submits=count ? stats.submissions-previous.submissions : 0;
    const double wait=count ? stats.submit_wait_ms-previous.submit_wait_ms : 0.;
    const bool duplicate=count && flip==last_flip;
    // A brightness drop is a clue, including legitimate transitions, not a flicker verdict.
    const bool drop=count && previous_luma>=24 && stats.mean_luma*100 < previous_luma*35;
    if(count) {
        intervals[cursor++ % intervals.size()]=ms;filled=std::min(filled+1,unsigned(intervals.size()));
        window_ms+=ms;window_wait+=wait;window_submits+=submits;++window_frames;
    }
    ++count;duplicates+=duplicate;drops+=drop;
    if(!window_flip0) window_flip0=flip;
    static unsigned captures=0,followup=0;
    const char* capture_dir=std::getenv("FP_FLICKER_CAPTURE_DIR");
    static const double capture_start=std::getenv("FP_FLICKER_START_MS") ?
        std::strtod(std::getenv("FP_FLICKER_START_MS"),nullptr) : 0.;
    if(capture_dir && count>3000 && captures<18 &&
       std::chrono::duration<double,std::milli>(now-epoch).count()>=capture_start) {
        if(drop && stats.mean_luma<48) followup=3;
        if(followup) {
            char path[1024];std::snprintf(path,sizeof path,"%s/frame-%06u-flip-%06u-luma-%u.ppm",
                                         capture_dir,count,flip,stats.mean_luma);
            if(!g_d3d12) rsx_vulkan_backend_dump_cached_frame(path);++captures;--followup;
            std::fprintf(stderr,"[fp-flicker] frame=%u surface=%u luma=%u draws=%u capture=%s\n",
                         count,stats.surface,stats.mean_luma,draws,path);
        }
    }
    if(csv) std::fprintf(csv,"%.3f,%u,%u,%.3f,%u,%llu,%.3f,%u,%u,%u,%06X,%u,%u,%u,%u,%.3f,%.3f,%llu\n",
        std::chrono::duration<double,std::milli>(now-epoch).count(),count,flip,count==1?0.:ms,draws,
        (unsigned long long)submits,wait,stats.surface,stats.width,stats.height,stats.center,
        stats.mean_luma,stats.dark_samples,unsigned(duplicate),unsigned(drop),
        count==1?0.:fp_fifo_wall_ms-previous_fifo_ms,present_ms,(unsigned long long)stats.submissions);
    previous_fifo_ms=fp_fifo_wall_ms;
    if(window_frames && window_ms>=500) {
        auto sorted=intervals;std::sort(sorted.begin(),sorted.begin()+filled);
        const double fps=1000.*window_frames/window_ms;
        const double p95=sorted[(filled-1)*95/100], maximum=sorted[filled-1];
        char title[240];
        std::snprintf(title,sizeof title,"Fat Princess - Native | %.1f FPS | %.1f ms%s",fps,window_ms/window_frames,fp::settings_title_suffix().c_str());
        std::string text;
        if (ai_control) text = "AI CONTROL ACTIVE\nF1 > TOOLS > PLAYER CONTROL\n";
        const auto test=fp::self_test_status();
        if(test.running) text+="SELF-TEST "+test.scenario+" "+std::to_string(test.elapsed)+"/"+std::to_string(test.duration)+" S\n";
        if (fp_stats_allowed() && fp::settings_stats_visible()) {
            if(fp::settings_get("stats")=="COMPACT" || g_d3d12) {
                char compact[96];std::snprintf(compact,sizeof compact,"%.1f FPS  %.1f MS\n",fps,window_ms/window_frames);text+=compact;
            } else text += fp::stats_overlay_text({window_ms/1000.,window_frames,flip-window_flip0,window_ms/window_frames,p95,maximum,
                draws,double(window_submits)/window_frames,g_pace_hz.load()});
        }
        if(fp::settings_menu_open() || fp::settings_osk_active() || fp::trophy_ui_active()) {
            // the settings menu, the on-screen keyboard and the trophy screens own the overlay while they are up (clearing it here every
            // half second made the keyboard vanish until the next button press redrew it)
        } else if(!text.empty()) {
            unsigned w,h;auto pixels=fp_hud_pixels(text,w,h);
            fp_dump_overlay(pixels,w,h,"stats");
            fp_set_overlay(pixels.data(),w,h,title);
        } else fp_set_overlay(nullptr,0,0,title);
        if(trace) std::fprintf(stderr,"[fp-stats] fps=%.2f frame=%.2f p95=%.2f max=%.2f draws=%u submits=%.1f wait=%.2f flip=%u dup=%u surface=%u luma=%u dark=%u/64 drops=%u\n",
            fps,window_ms/window_frames,p95,maximum,draws,double(window_submits)/window_frames,
            window_wait/window_frames,flip,duplicates,stats.surface,stats.mean_luma,stats.dark_samples,drops);
        window_ms=window_wait=0;window_submits=0;window_frames=0;window_flip0=flip;
    }
    last=now;last_flip=flip;previous_luma=stats.mean_luma;previous=stats;
}

extern "C" int fp_backend_pump_messages(void) {
#ifdef _WIN32
    const int result = g_d3d12 ? fp_d3d12_pump_messages() : rsx_vulkan_backend_pump_messages();
#else
    const int result = rsx_vulkan_backend_pump_messages();
#endif
    // FP_MENU_AT=open_s,close_s opens and closes the settings menu on a timer (headless tests).
    {
        static const char* spec=std::getenv("FP_MENU_AT");
        static double t_open=-1,t_close=-1;
        static const auto t0=std::chrono::steady_clock::now();
        static bool parsed=false;
        if(spec && !parsed) {parsed=true;std::sscanf(spec,"%lf,%lf",&t_open,&t_close);}
        const double t=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        if(t_open>=0 && t>=t_open) {t_open=-1;if(!fp::settings_menu_open()) fp::settings_menu_toggle();}
        if(t_close>=0 && t>=t_close) {t_close=-1;if(fp::settings_menu_open()) fp::settings_menu_toggle();}
    }
    static bool stats_key_down=false;
    int keyboard_focused=0;const Uint8* menu_keys=fp_menu_keyboard_state(&keyboard_focused);
    const bool stats_key=menu_keys[SDL_SCANCODE_F3] && keyboard_focused;
    if(stats_key && !stats_key_down && !fp::settings_menu_open() && !fp::settings_osk_active() && !fp::trophy_ui_active()) {
        fp::settings_stats_set(!fp::settings_stats_visible());
        if(!fp::settings_stats_visible()) fp_set_overlay(nullptr,0,0,nullptr);
    }
    stats_key_down=stats_key;
    fp_trophy_cards_tick();
    {
        static bool menu_was_open=false;
        std::string panel;
        const bool menu=fp::settings_menu_frame(panel);
        if(menu && !panel.empty()) {
            // the on-screen keyboard: larger glyphs, bottom centre, so the game's own text field stays visible
            const bool osk=fp::settings_osk_active(), trophies=fp::trophy_ui_active();
            fp_set_overlay_anchor(osk?1:trophies?2:0);   // 2: centred (trophy screens)
            unsigned w,h,radius=0;
            auto pixels=osk ? fp::keyboard_pixels(fp::settings_keyboard_view(),w,h)
                : trophies ? fp::trophy_ui_pixels(fp::settings_get("resolution")=="1080P"?1.5f:1.f,fp::settings_controller_hints(),w,h,radius)
                : fp_settings_pixels(panel,w,h,fp::settings_get("ui_scale")=="LARGE"?3u:2u);
            if(w>2048||h>1536) pixels=fp_hud_pixels(panel,w,h);
            fp_dump_overlay(pixels,w,h,"menu");
            fp_set_overlay(pixels.data(),w,h,nullptr);
        } else if(!menu && menu_was_open) {
            { fp_set_overlay(nullptr,0,0,nullptr); fp_set_overlay_anchor(0); }
        }
        menu_was_open=menu;
    }
    // The on-screen controller presses the same virtual pad as the keyboard.
    fp_touch_state touch{};
#if defined(__APPLE__) && TARGET_OS_IOS
    fp_touch_tick();
    fp_touch_read(&touch);
#endif
    if (keyboard_pad) {
        int keyboard_focused=0;
        const Uint8* keys = fp_menu_keyboard_state(&keyboard_focused);
        const bool focused = keyboard_focused!=0;
        static bool fullscreen_enter = false;
        const bool enter = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];
        if (!enter) fullscreen_enter = false;
        if (enter && (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT])) fullscreen_enter = true;
        const auto down = [&](SDL_Scancode code) {
            return focused && keys[code] && !(fullscreen_enter &&
                (code == SDL_SCANCODE_RETURN || code == SDL_SCANCODE_KP_ENTER));
        };
        const bool menu_open = fp::settings_menu_open() || fp::settings_osk_active() || fp::trophy_ui_active();   // keys go to the host screen, not the game
        const auto bound = [&](const char* name) { return static_cast<SDL_Scancode>(fp::settings_key(name)); };
        const struct { const char* name; SDL_GameControllerButton button; } buttons[] = {
            {"cross", SDL_CONTROLLER_BUTTON_A}, {"circle", SDL_CONTROLLER_BUTTON_B},
            {"square", SDL_CONTROLLER_BUTTON_X}, {"triangle", SDL_CONTROLLER_BUTTON_Y},
            {"start", SDL_CONTROLLER_BUTTON_START}, {"select", SDL_CONTROLLER_BUTTON_BACK},
            {"l1", SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {"r1", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
            {"l3", SDL_CONTROLLER_BUTTON_LEFTSTICK}, {"r3", SDL_CONTROLLER_BUTTON_RIGHTSTICK},
            {"up", SDL_CONTROLLER_BUTTON_DPAD_UP}, {"down", SDL_CONTROLLER_BUTTON_DPAD_DOWN},
            {"left", SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {"right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
        };
        for (size_t i = 0; i < std::size(buttons); i++) {
            const bool touched = i < 10 && (touch.buttons >> i & 1);   // touch bits follow the table's first ten
            SDL_JoystickSetVirtualButton(keyboard_pad, buttons[i].button,
                !menu_open && (down(bound(buttons[i].name)) || touched) ? 1 : 0);
        }
        const int key_x = (down(bound("right")) - down(bound("left"))) * 32767;
        const int key_y = (down(bound("down")) - down(bound("up"))) * 32767;
        SDL_JoystickSetVirtualAxis(keyboard_pad, SDL_CONTROLLER_AXIS_LEFTX,
            static_cast<Sint16>(menu_open ? 0 : key_x ? key_x : touch.lx));
        SDL_JoystickSetVirtualAxis(keyboard_pad, SDL_CONTROLLER_AXIS_LEFTY,
            static_cast<Sint16>(menu_open ? 0 : key_y ? key_y : touch.ly));
        SDL_JoystickSetVirtualAxis(keyboard_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
            !menu_open && (down(bound("l2")) || (touch.buttons & (1u << 14))) ? 32767 : -32768);
        SDL_JoystickSetVirtualAxis(keyboard_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
            !menu_open && (down(bound("r2")) || (touch.buttons & (1u << 15))) ? 32767 : -32768);
        SDL_JoystickUpdate();
    }
    return result;
}

extern "C" void cellGcmTickVBlank(void);
extern "C" void cellGcmTickFlip(void);
extern "C" void cellGcm_rsx_process_fifo(void);
extern "C" int cellGcm_present_pending(void (*)(void));

// iOS refuses GPU work from a backgrounded app and loses the device. SDL reports the switch synchronously through an
// event watch, on the main thread between frames, so the frame clock stops ticking, walking and presenting until the
// app is back; the game waits at its next vblank.
static std::atomic<bool> g_backgrounded{false};
static int SDLCALL background_watch(void*, SDL_Event* e) {
    if (e->type == SDL_APP_WILLENTERBACKGROUND || e->type == SDL_APP_DIDENTERBACKGROUND) g_backgrounded = true;
    else if (e->type == SDL_APP_WILLENTERFOREGROUND || e->type == SDL_APP_DIDENTERFOREGROUND) g_backgrounded = false;
    return 1;
}

// The command walker holds at a flip until its completed image is presented.
// Keep simulation/vblank signals independent of whether a new image is ready.
extern "C" unsigned long fp_run_frame_clock(void (*present)(void)) {
    using Clock = std::chrono::steady_clock;
    const char* title = std::getenv("PS3_TITLE");
    // PS3_VIDEO_RES fixes guest buffer geometry at boot; host output can change live.
    const bool hd = fp::settings_get("resolution") == "1080P";
    bool ready = fp_backend_init(hd ? 1920 : 1280, hd ? 1080 : 720, title ? title : "Fat Princess - Native") == 0;
    std::fprintf(stderr,"[fp-host] frame-boundary presenter %s\n",ready ? "ready" : "failed");
    SDL_AddEventWatch(background_watch, nullptr);
    auto next = Clock::now();
    // The emulated vblank also completes flips, so it bounds the frame rate the game can reach.
    // Default: the frame limit, never below 60; 240 when the limit is off. FP_VBLANK_HZ overrides.
    static const double forced_hz = std::getenv("FP_VBLANK_HZ") ? std::atof(std::getenv("FP_VBLANK_HZ")) : 0.0;
    auto tick = std::chrono::nanoseconds(16666667);
    double tick_hz = 60.0;
    // Default polls every 500 us; FP_FRAME_POLL_US=0 yields instead.
    static const char* poll_env = std::getenv("FP_FRAME_POLL_US");
    const long poll_us = poll_env ? std::atol(poll_env) : 500;
    for (;;) {
        // Sleep in poll_us steps for the flip queue, but never past the next vblank: wake 150 us early and spin the rest,
        // so the tick lands within microseconds instead of up to a whole poll step late.
        {
            const auto until = next - std::chrono::microseconds(150);
            const auto t = Clock::now();
            if (poll_us > 0 && t < until) std::this_thread::sleep_for(std::min<Clock::duration>(std::chrono::microseconds(poll_us), until - t));
            else if (poll_us <= 0) std::this_thread::yield();
            if (Clock::now() < next && next - Clock::now() <= std::chrono::microseconds(150)) while (Clock::now() < next) {}
        }
        if (!g_d3d12) rsx_vulkan_backend_interp_service();
        auto now = Clock::now();
        const double want_hz = forced_hz > 1.0 ? forced_hz : g_pace_hz.load();
        if (want_hz != tick_hz) {
            tick_hz = want_hz;
            tick = std::chrono::nanoseconds(static_cast<long long>(1e9 / tick_hz));
            next = now + tick;
        }
        unsigned catchup = 0;
        while (now >= next && catchup++ < 8) {
            g_last_tick_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
            pace_log('T');
            cellGcmTickVBlank(); cellGcmTickFlip(); next += tick;
        }
        if (catchup >= 8) next = now + tick;
        if (!ready) continue;
        cellGcm_present_pending(present);
        const auto fifo_start=Clock::now();
        cellGcm_rsx_process_fifo();
        {
            // The game's frame work is the time from the vblank that released it to the moment its flip is queued;
            // after that it only waits for the next vblank. That wait is the headroom AUTO pacing looks at.
            static uint32_t seen_flips=0;
            const uint32_t flips=cellGcm_flip_request_count();
            if(flips!=seen_flips) {
                seen_flips=flips;
                const auto queued=Clock::now();
                g_work_ms.store(std::chrono::duration<double,std::milli>(
                    queued-Clock::time_point(std::chrono::nanoseconds(g_last_tick_ns.load()))).count());
            }
        }
        fp_fifo_wall_ms+=std::chrono::duration<double,std::milli>(Clock::now()-fifo_start).count();
        if (!g_d3d12) rsx_vulkan_backend_interp_service();
        cellGcm_present_pending(present);
        if (!cellGcm_flip_request_count()) fp_backend_present();
        if (fp_backend_pump_messages() != 0) std::exit(0);
        if (g_backgrounded) {
            std::fprintf(stderr, "[fp-host] in the background: rendering paused\n");
            while (g_backgrounded) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (fp_backend_pump_messages() != 0) std::exit(0);
            }
            std::fprintf(stderr, "[fp-host] back in the foreground\n");
            next = Clock::now() + tick;
        }
    }
}

#if defined(__linux__) || (defined(__APPLE__) && TARGET_OS_IOS && defined(__aarch64__))   // SIGPROF and ucontext
// FP_PROF=path: SIGPROF sampling of the instruction pointer per host thread, written to
// `path` every 15 s as (tid, rip) pairs. Resolve offline with tools/resolve_prof.py.
// On iOS `path.maps` holds the main image's load address and the thread names instead of /proc/self/maps.
#include <csignal>
#include <sys/time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <ucontext.h>
#else
#include <sys/ucontext.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unordered_set>
#include <mach-o/dyld.h>
#endif
namespace {
constexpr size_t prof_capacity = 1u << 22;
struct ProfSample { uint32_t tid; uint32_t us; uint64_t rip; };   // us: steady clock microseconds, low 32 bits
ProfSample* prof_samples;
#if defined(__APPLE__) && defined(__aarch64__)
ProfSample* prof_callers;
#endif
std::atomic<size_t> prof_count{0};
#if defined(__linux__)
uint32_t prof_tid() { return uint32_t(syscall(SYS_gettid)); }
uint64_t prof_pc(void* uc) { return uint64_t(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP]); }
#else
// Thread names are taken the first time a thread is sampled; the writer saves them next to the samples.
constexpr int prof_names_max = 256;
struct ProfName { std::atomic<uint32_t> tid; char name[32]; };
ProfName prof_names[prof_names_max];
uint32_t prof_tid() {
    uint64_t id = 0; pthread_threadid_np(nullptr, &id);
    const uint32_t tid = uint32_t(id);
    for (auto& n : prof_names) {
        const uint32_t have = n.tid.load(std::memory_order_acquire);
        if (have == tid) break;
        uint32_t empty = 0;
        if (have == 0 && n.tid.compare_exchange_strong(empty, tid)) { pthread_getname_np(pthread_self(), n.name, sizeof n.name); break; }
    }
    return tid;
}
uint64_t prof_pc(void* uc) { return uint64_t(static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__pc); }
#endif
void prof_handler(int, siginfo_t*, void* uc) {
    const size_t i = prof_count.fetch_add(1, std::memory_order_relaxed);
    if (i >= prof_capacity) return;
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    prof_samples[i] = {prof_tid(), uint32_t(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000), prof_pc(uc)};
#if defined(__APPLE__) && defined(__aarch64__)
    // LR names the interrupted function's return site without walking a live stack.
    // Remove pointer authentication bits from the saved ARM64 return address.
    if (prof_callers) prof_callers[i] = {prof_samples[i].tid, prof_samples[i].us,
        uint64_t(static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__lr) & 0x0000ffffffffffffull};
#endif
}
void prof_start() {
    const char* path = std::getenv("FP_PROF");
    if (!path || !*path || prof_samples) return;
    prof_samples = static_cast<ProfSample*>(calloc(prof_capacity, sizeof(ProfSample)));
#if defined(__APPLE__) && defined(__aarch64__)
    prof_callers = static_cast<ProfSample*>(calloc(prof_capacity, sizeof(ProfSample)));
#endif
    struct sigaction sa{};
    sa.sa_sigaction = prof_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    itimerval timer{{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &timer, nullptr);
    std::string out = path;
    std::thread([out] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            const size_t n = std::min(prof_count.load(), prof_capacity);
            std::string tmp = out + ".tmp";
            if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
                std::fwrite(prof_samples, sizeof(ProfSample), n, f);
                std::fclose(f);
                std::rename(tmp.c_str(), out.c_str());
#if defined(__APPLE__)
#if defined(__aarch64__)
                if (prof_callers) {
                    const std::string callers = out + ".callers";
                    if (FILE* c = std::fopen((callers + ".tmp").c_str(), "wb")) {
                        std::fwrite(prof_callers, sizeof(ProfSample), n, c);
                        std::fclose(c);
                        std::rename((callers + ".tmp").c_str(), callers.c_str());
                    }
                }
#endif
                if (FILE* m = std::fopen((out + ".maps").c_str(), "w")) {
                    std::fprintf(m, "base %llx slide %llx\n", (unsigned long long)(uintptr_t)_dyld_get_image_header(0),
                                 (unsigned long long)_dyld_get_image_vmaddr_slide(0));
                    for (auto& t : prof_names) if (uint32_t tid = t.tid.load()) std::fprintf(m, "thread %u %s\n", tid, t.name);
                    // Samples outside the main image (system libraries, the graphics driver) are named here: the
                    // symbols of the shared cache are only available on the device.
                    const uintptr_t main = (uintptr_t)_dyld_get_image_header(0);
                    std::unordered_set<uint64_t> seen;
                    for (size_t i = 0; i < n; ++i) {
                      for (int source = 0; source < 2; ++source) {
                        uint64_t pc = prof_samples[i].rip;
#if defined(__aarch64__)
                        if (source) { if (!prof_callers) continue; pc = prof_callers[i].rip; }
#else
                        if (source) continue;
#endif
                        Dl_info info{};
                        if (!seen.insert(pc).second || !dladdr((const void*)pc, &info) || (uintptr_t)info.dli_fbase == main) continue;
                        const char* image = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
                        std::fprintf(m, "pc %llx %s %s\n", (unsigned long long)pc, image ? image + 1 : "?",
                                     info.dli_sname ? info.dli_sname : "?");
                      }
                    }
                    std::fclose(m);
                }
#endif
                if (FILE* in = std::fopen("/proc/self/maps", "r")) {
                    if (FILE* m = std::fopen((out + ".maps").c_str(), "w")) {
                        char buf[4096]; size_t got;
                        while ((got = std::fread(buf, 1, sizeof buf, in)) > 0) std::fwrite(buf, 1, got, m);
                        std::fclose(m);
                    }
                    std::fclose(in);
                }
            }
        }
    }).detach();
}
#if defined(__linux__)
__attribute__((constructor)) void prof_start_at_load() { prof_start(); }
#endif
}
// iOS reads its environment from the app's documents after load, so the launcher starts the sampler.
extern "C" void fp_prof_start(void) { prof_start(); }
#else
extern "C" void fp_prof_start(void) {}
#endif

#if defined(__linux__)
// A crash goes to the log with the faulting address and the host stack, so a report does not depend on a core dump
// (the system here stores none). Resolve the frames with `addr2line -f -C -e fp_native <offsets>`.
#include <execinfo.h>
#include <dirent.h>
#include <sys/prctl.h>
namespace {
void crash_handler(int sig, siginfo_t* info, void*) {
    char name[24] = "?";
    prctl(PR_GET_NAME, name, 0, 0, 0);
    char head[160];
    const int n = std::snprintf(head, sizeof head, "\n[crash] signal %d at %p in thread \"%s\" (tid %ld)\n", sig,
                                info ? info->si_addr : nullptr, name, long(syscall(SYS_gettid)));
    if (n > 0) { ssize_t w = write(2, head, size_t(n)); (void)w; }
    void* frames[48];
    const int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}
// A hang leaves no trace in the log: the game stops presenting and the player kills it. When no flip arrives for
// 12 s after the first 300 (counted at fp_backend_present: the title's flip requests kept counting during a Deck
// hang), every thread writes its host stack to the log once (resolve as for a crash); the dump
// re-arms when frames come back. FP_HANG_DUMP=0 turns it off.
void hang_stack_handler(int) {
    char name[24] = "?";
    prctl(PR_GET_NAME, name, 0, 0, 0);
    char head[96];
    const int n = std::snprintf(head, sizeof head, "[hang] thread \"%s\" (tid %ld):\n", name, long(syscall(SYS_gettid)));
    if (n > 0) { ssize_t w = write(2, head, size_t(n)); (void)w; }
    void* frames[32];
    backtrace_symbols_fd(frames, backtrace(frames, 32), 2);
}
void hang_watchdog() {
    const char* e = std::getenv("FP_HANG_DUMP");
    if (e && e[0] == '0') return;
    signal(SIGUSR2, hang_stack_handler);
    std::thread([] {
        uint32_t last = 0; int still = 0; bool dumped = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint32_t now = g_hang_presents.load(std::memory_order_relaxed);
            if (now != last) { last = now; still = 0; dumped = false; continue; }
            if (now < 300 || dumped || ++still < 12) continue;
            dumped = true;
            std::fprintf(stderr, "\n[hang] no frame presented for 12 s (present %u); thread stacks follow\n", now);
            if (DIR* d = opendir("/proc/self/task")) {
                const long self = syscall(SYS_gettid);
                while (dirent* t = readdir(d)) {
                    const long tid = std::atol(t->d_name);
                    if (tid <= 0 || tid == self) continue;
                    syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                closedir(d);
            }
            std::fprintf(stderr, "[hang] end of thread stacks\n");
        }
    }).detach();
}
__attribute__((constructor)) void crash_log_init() {
    static char stack[1 << 16];   // handle a stack overflow too
    stack_t ss{}; ss.ss_sp = stack; ss.ss_size = sizeof stack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, nullptr); sigaction(SIGBUS, &sa, nullptr); sigaction(SIGABRT, &sa, nullptr);
    hang_watchdog();
}
}
#endif

#if defined(__linux__)
// FP_HUGE_TEXT=1: move the executable's code onto transparent huge pages at start-up. The lifted game is ~120 MB of
// code and the main thread's frame runs through a large part of it, so 4 KB pages cost instruction TLB misses. The
// code is copied into a 2 MB aligned anonymous mapping (madvise HUGEPAGE), made read+execute, and mremap'd over the
// original range in one step; the bytes are identical, so code running in the range (this function) is unaffected.
#include <sys/mman.h>
extern "C" char __executable_start[], etext[];
namespace {
__attribute__((constructor)) void huge_text() {
    const char* e = std::getenv("FP_HUGE_TEXT");
    if (!e || !*e || *e == '0') return;
    constexpr uintptr_t huge = uintptr_t(2) << 20;
    const uintptr_t lo = (reinterpret_cast<uintptr_t>(__executable_start) + huge - 1) & ~(huge - 1);
    const uintptr_t hi = reinterpret_cast<uintptr_t>(etext) & ~(huge - 1);
    if (hi <= lo) return;
    const size_t len = hi - lo;
    void* raw = mmap(nullptr, len + huge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return;
    const uintptr_t r = reinterpret_cast<uintptr_t>(raw), dst = (r + huge - 1) & ~(huge - 1);
    if (dst > r) munmap(raw, dst - r);
    if (r + len + huge > dst + len) munmap(reinterpret_cast<void*>(dst + len), r + len + huge - (dst + len));
    void* d = reinterpret_cast<void*>(dst);
    madvise(d, len, MADV_HUGEPAGE);
    std::memcpy(d, reinterpret_cast<const void*>(lo), len);
    if (mprotect(d, len, PROT_READ | PROT_EXEC) != 0 ||
        mremap(d, len, len, MREMAP_MAYMOVE | MREMAP_FIXED, reinterpret_cast<void*>(lo)) == MAP_FAILED) {
        std::fprintf(stderr, "[fp-hugetext] not moved: %s\n", std::strerror(errno));
        munmap(d, len);
        return;
    }
    // How much of it the kernel actually backed with huge pages.
    unsigned long huge_kb = 0;
    if (FILE* f = std::fopen("/proc/self/smaps", "r")) {
        char line[256]; bool in = false;
        while (std::fgets(line, sizeof line, f)) {
            unsigned long a, b;
            if (std::sscanf(line, "%lx-%lx ", &a, &b) == 2) in = a == lo;
            else if (in && std::sscanf(line, "AnonHugePages: %lu kB", &a) == 1) { huge_kb = a; break; }
        }
        std::fclose(f);
    }
    std::fprintf(stderr, "[fp-hugetext] %zu MB of code moved, %lu MB on huge pages\n", len >> 20, huge_kb >> 10);
}
}
#endif

// PGO training builds (-DFP_PGO_GENERATE) write their profile on SIGTERM; the route is stopped by a timeout.
#ifdef FP_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
namespace {
__attribute__((constructor)) void pgo_term_hook() {
    std::signal(SIGTERM, [](int) { __gcov_dump(); _exit(0); });
    // Hand-played sessions can end in a crash or a quit path that skips atexit: write what was counted every
    // 2 minutes and start the counters again, so each dump adds only its own slice to the .gcda files.
    std::thread([] { for (;;) { std::this_thread::sleep_for(std::chrono::minutes(2)); __gcov_dump(); __gcov_reset(); } }).detach();
}
}
#endif

// FP_CHEATS=name[,name...]: switches the runtime patches in the lifted code (tools/lift_patches.py).
// debugbindings: the controller bindings the scripts tag !Retail (class cheats, resource cheats, free camera, debug menu).
// guestai: the AI can drive the local player on an online guest too.
extern "C" unsigned char g_fp_patch[32] = {0};
namespace {
__attribute__((constructor)) void fp_cheats_init() {
    const char* list = std::getenv("FP_CHEATS");
    if (!list) return;
    std::string all = list;
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t end = all.find(',', pos);
        if (end == std::string::npos) end = all.size();
        const std::string name = all.substr(pos, end - pos);
        if (name == "debugbindings") g_fp_patch[0] = 1;
        if (name == "guestai") g_fp_patch[29] = 1;   // player toggleai on an online guest (notes/ai-toggle-re.md)
        pos = end + 1;
    }
    std::fprintf(stderr, "[fp-cheats] %s\n", list);
}
}

// The game reads the player name from the NP profile only while it brings up its NP client (func_003F6F28), which
// it normally does when the online menu is first entered. Run that step once, a few seconds after boot, so the
// offline roster and the lobby already have the name. Called from cellSysutilCheckCallback on the guest main thread.
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
// The game's native text code names its default font with the literal "clean" (at 0x00A983D8 in the retail executable),
// which resolves to a plain sans. The name tags above players are drawn with it, where the console shows the hand-drawn
// "toon" face (see the console captures). The literal is overwritten with "toon" at start-up; FP_NAME_FONT=clean (or
// off) keeps the executable's own.
extern "C" uint8_t* vm_base;
// Name tags: the overhead text (0x000CFF48) is drawn with the font named by the literal "clean" at guest 0x00A983D8, and
// the font manager (0x00280BB8) loads data/textures/fonts/<name>.txt, which for "clean" is the Calibri-like atlas in every
// dump we have. The NAME TAG FONT setting (or FP_NAME_FONT) writes another name over the literal; it has room for five
// characters, so the longer fonts go by the short aliases patches.cpp adds. Applies live: the name is read every frame.
static void fp_apply_name_font() {
    constexpr uint32_t kCleanLiteral = 0x00A983D8;
    static bool seen = false;
    static std::string written;
    if (!vm_base) return;
    uint8_t* text = vm_base + kCleanLiteral;
    if (!seen) { if (std::memcmp(text, "clean\0", 6) != 0) return; seen = true; written = "clean"; }   // not loaded yet
    std::string want;
    if (const char* e = std::getenv("FP_NAME_FONT")) want = e;
    else {
        const std::string v = fp::settings_get("name_font");
        want = v == "ORIGINAL" ? "clean" : v == "FATFINGERS OUTLINED" ? "ffo" : v == "BOOPEE" ? "bp" : v == "TOON" ? "toon" : "ff";
    }
    if (want == "off" || want.empty()) want = "clean";
    if (want == written) return;
    if (want.size() > 5) { std::fprintf(stderr, "[fp-font] FP_NAME_FONT must be 5 characters or fewer\n"); written = want; return; }
    for (size_t i = 0; i < 6; ++i) text[i] = uint8_t(i < want.size() ? want[i] : 0);
    written = want;
    std::fprintf(stderr, "[fp-font] name tag font is now \"%s\"\n", want.c_str());
}

// Voice chat's push to talk: the title keeps the state of its own talk button (the layout setting decides which one) in
// the voice object's byte at +0x30 and raises it only while held. Found by watching it against injected R2 presses.
extern "C" int fp_voice_game_talking(void) {
    return vm_base ? vm_base[0x00F2B1E8 + 0x30] != 0 : 0;
}

extern "C" int fp_psnr_room_password(uint32_t key, char* out, uint32_t capacity) {
    // Keyboard output at [TOC-0x3B54]+0x64C (0xD1C98C), consumed at
    // 0x1542F8 / 0x154754 / 0x15502C before storing the room key at 0xF01838.
    // Verify the key so a later, unrelated keyboard entry is never advertised.
    if (!vm_base || vm_read32(0xF01838) != key) return 0;
    return fp::room_password_text(vm_base + 0xD1C98C, key, out, capacity);
}

// Who hears us: the voice object keeps its own channel at +0x34 (the team in a match) and a circular member list
// with its sentinel at +0x0C (node +0 next, +8 member; member +0 channel, +4 u16 Matching2 room member id). The game
// sends its own voice only to members on its channel (0x0041CB70); cellVoice's FPV2 datagrams follow the same rule.
// Returns the psnr user ids, or -1 when there is no channel yet (send to everyone, as before).
extern "C" uint32_t np_m2_member_user(uint16_t member_id);
extern "C" int fp_voice_game_recipients(uint32_t* users, int max) {
    constexpr uint32_t obj = 0x00F2B1E8;
    if (!vm_base) return -1;
    const uint32_t mine = vm_read32(obj + 0x34);
    if (mine == 0xFFFFFFFFu) return -1;
    const uint32_t sentinel = vm_read32(obj + 0x0C);
    if (!sentinel) return -1;
    static const bool trace = std::getenv("FP_VOICE_TRACE") != nullptr;
    static std::string last_desc;   // only the cellVoice reader thread calls this
    std::string desc;
    int n = 0;
    uint32_t node = vm_read32(sentinel);
    for (int guard = 0; node && node != sentinel && guard < 64 && n < max; ++guard, node = vm_read32(node)) {
        const uint32_t m = vm_read32(node + 8);
        if (!m) continue;
        const uint16_t id = uint16_t(vm_read32(m + 4) >> 16);
        if (trace) desc += " member " + std::to_string(id) + " ch " + std::to_string(int32_t(vm_read32(m))) +
                           " user " + std::to_string(np_m2_member_user(id)) + ";";
        if (vm_read32(m) != mine) continue;
        if (const uint32_t u = np_m2_member_user(id)) users[n++] = u;
    }
    if (trace && desc != last_desc) { std::fprintf(stderr, "[voice] my channel %d:%s\n", int32_t(mine), desc.c_str()); last_desc = desc; }
    return n;
}

// Test only: FP_VOICE_FORCE_READ=1 keeps the voice state at 0x00D12F40 on 0 ("someone shares my channel"), so a
// headless pair on opposite teams still reads its voice port. The game rewrites it on every team change.
static void fp_voice_force_read() {
    static const bool on = std::getenv("FP_VOICE_FORCE_READ") && std::getenv("FP_VOICE_FORCE_READ")[0] == '1';
    if (on && vm_base && vm_read32(0x00D12F40) == 1) std::memset(vm_base + 0x00D12F40, 0, 4);
}

// FP_AI_TRACE=1: every ~5 s, the local player's AI state (notes/ai-toggle-re.md): players vector at 0xD1ACB8, local
// index at 0xF01804+0x24 (the settings struct), aiEnabled at +0x30, net running 0xD6F65A, net role 0 byte 0xD6F660.
static void fp_ai_trace() {
    static const bool on = std::getenv("FP_AI_TRACE") && std::getenv("FP_AI_TRACE")[0] == '1';
    static unsigned n = 0;
    if (!on || !vm_base || ++n % 150) return;
    const uint32_t settings = 0xF01804, b = vm_read32(0xD1ACB8 + 8), e = vm_read32(0xD1ACB8 + 0xC);
    const int32_t i = settings ? int32_t(vm_read32(settings + 0x24)) : -1;
    const uint32_t count = b && e >= b && (e - b) % 4 == 0 ? (e - b) / 4 : 0;
    const uint32_t p = (count <= 128 && i >= 0 && uint32_t(i) < count) ? vm_read32(b + uint32_t(i) * 4) : 0;
    std::fprintf(stderr, "[fp-ai] settings 0x%08X local %d aiEnabled %u net %u role0 %u players %u player 0x%08X",
                 settings, i, settings ? vm_base[settings + 0x30] : 0, vm_base[0xD6F65A], vm_base[0xD6F660],
                 count, p);
    if (p) std::fprintf(stderr, " ai %u %u %u brain 0x%08X behaviour 0x%08X", vm_base[p + 0x1549], vm_base[p + 0x154A],
                        vm_base[p + 0x154B], vm_read32(p + 0x14D8), vm_read32(p + 0x157C));
    if (p) {
        const uint32_t ch = vm_read32(p + 0xB4), behaviour = vm_read32(p + 0x157C);
        const uint32_t slots = vm_read32(0x00C97F88 + 0x448C), index = vm_read32(p + 0xA8);
        std::fprintf(stderr, " pregame %u human %u controller %d character 0x%08X", vm_base[settings + 0x31],
                     vm_base[p + 0x1548], int32_t(vm_read32(p + 0x15FC)), ch);
        if (ch) {
            float pos[3];
            for (unsigned k = 0; k < 3; ++k) {
                const uint32_t bits = vm_read32(ch + 0x14C + k * 4);
                std::memcpy(&pos[k], &bits, sizeof bits);
            }
            std::fprintf(stderr, " pos %.3f %.3f %.3f", pos[0], pos[1], pos[2]);
        }
        if (behaviour) std::fprintf(stderr, " thinkDelay %d", int32_t(vm_read32(behaviour + 0x1C)));
        if (slots && index < 128) {
            const uint32_t slot = slots + index * 0x1A8;
            const auto read16 = [](uint32_t a) { return unsigned(vm_base[a]) * 256 + vm_base[a + 1]; };
            std::fprintf(stderr, " slot 0x%08X virtual %u rawXY %u %u inputXY %u %u", slot, vm_base[slot + 9],
                         read16(slot + 0x124), read16(slot + 0x126), read16(slot + 0x1C), read16(slot + 0x1E));
        }
    }
    std::fprintf(stderr, "\n");
}

extern "C" void fp_sysutil_tick(void) {
    fp::player_control_poll();
    fp_apply_name_font();
    fp_voice_force_read();
    fp_ai_trace();
    constexpr uint32_t kEntryOpd = 0x00C55418, kNpStart = 0x003F6F28, kNetMgrTocSlot = 0x47A4;
    static bool done = false;
    static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    if (done) return;
    static const double delay = std::getenv("FP_NPINIT_DELAY_S") ? std::atof(std::getenv("FP_NPINIT_DELAY_S")) : 6.0;
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < delay) return;
    if (fp::settings_get("online") != "ON") { done = true; return; }
    const uint32_t toc = vm_read32(kEntryOpd + 4);
    const uint32_t cell = toc ? vm_read32(toc + kNetMgrTocSlot) : 0;
    const uint32_t netmgr = cell ? vm_read32(cell) : 0;
    if (!netmgr) return;
    done = true;
    // netmgr+0x10 is set once the menu has already started NP.
    if (vm_read32(netmgr + 0x10)) return;
    std::fprintf(stderr, "[fp-np] starting the NP client at boot for the profile name (netmgr 0x%08X)\n", netmgr);
    ppu_guest_call_ct(kNpStart, toc, netmgr, 1, 0, 0, 0, 0, 0, 0);
}
