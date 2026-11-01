// Bounded sampling of the actual overloaded audio task, with no signal timer
// and no sampling of unrelated threads. Mach's retained thread send right
// remains safe to query if the task exits during a capture.
#include "audio_profile_gate.h"
#include "ios_lifecycle.h"
#include "../third_party/ps3recomp/runtime/spu/spu_workload.h"
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned sample_limit = 800;
std::atomic<mach_port_t> request{MACH_PORT_NULL};
std::atomic<bool> busy{false}, audio_running{false}, finished{false};
std::atomic<uint64_t> trigger_cpu{0}, target_id{0};
std::string output;
uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}
void note_round(uint32_t taskset, uint32_t task, int running, uint64_t, uint64_t cpu) {
    // MultiStream's taskset in this title. Other SPURS tasks have different
    // deadlines and must not trigger an audio capture.
    if (taskset != 0x00F9BB80 || task != 0) return;
    audio_running.store(running != 0, std::memory_order_release);
    if (running || finished.load(std::memory_order_acquire) || busy.load(std::memory_order_acquire)) return;
    static thread_local FPAudioProfileGate gate;
    if (!gate.note(cpu, now_us())) return;
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    target_id.store(id, std::memory_order_relaxed);
    trigger_cpu.store(cpu, std::memory_order_relaxed);
    busy.store(true, std::memory_order_release);
    request.store(mach_thread_self(), std::memory_order_release);
}
struct Sample { uint64_t pc, lr; };
void capture(mach_port_t target, unsigned number) {
    Sample samples[sample_limit];
    unsigned count = 0, suspended = 0;
    uint64_t overhead = 0, max_overhead = 0;
    const uint64_t begin = now_us();
    const uint64_t tid = target_id.load(), cpu = trigger_cpu.load();
    kern_return_t error = KERN_SUCCESS;
    for (unsigned tick = 0; tick < sample_limit; ++tick) {
        std::this_thread::sleep_until(Clock::time_point(std::chrono::microseconds(begin + (tick + 1) * 10000ull)));
        if (!fp_ios_is_active()) break;
        // Exclude WAIT_SIGNAL parking from the active-work histogram.
        if (!audio_running.load(std::memory_order_acquire)) continue;
        arm_thread_state64_t state{};
        mach_msg_type_number_t size = ARM_THREAD_STATE64_COUNT;
        const uint64_t start = now_us();
        error = thread_suspend(target);
        if (error != KERN_SUCCESS) break;
        ++suspended;
        const kern_return_t got = thread_get_state(target, ARM_THREAD_STATE64,
            reinterpret_cast<thread_state_t>(&state), &size);
        const bool active = audio_running.load(std::memory_order_acquire);
        // Nothing that allocates, resolves symbols, logs or takes an app lock
        // may run while the target is suspended. Always resume after get_state.
        const kern_return_t resumed = thread_resume(target);
        const uint64_t elapsed = now_us() - start;
        overhead += elapsed;
        if (elapsed > max_overhead) max_overhead = elapsed;
        if (resumed != KERN_SUCCESS) { error = resumed; break; }
        if (got != KERN_SUCCESS) { error = got; break; }
        if (active && size == ARM_THREAD_STATE64_COUNT) {
            samples[count++] = {arm_thread_state64_get_pc(state), arm_thread_state64_get_lr(state)};
        }
        // Stop if sampling itself becomes expensive. Never sustain more than
        // 80 ms total suspension/query cost or one query above 2 ms.
        if (elapsed > 2000 || overhead > 80000) break;
    }
    mach_port_deallocate(mach_task_self(), target);
    const uint64_t duration = now_us() - begin;
    std::map<std::pair<uint64_t,uint64_t>, unsigned> callers;
    std::map<uint64_t,unsigned> pcs;
    std::set<uint64_t> addresses;
    for (unsigned i = 0; i < count; ++i) {
        ++callers[{samples[i].pc,samples[i].lr}]; ++pcs[samples[i].pc];
        addresses.insert(samples[i].pc); addresses.insert(samples[i].lr);
    }
    const std::string temp = output + ".tmp";
    if (FILE* f = std::fopen(temp.c_str(), "w")) {
        std::fprintf(f, "\nAUDIO OVERLOAD PROFILE\nbase %llx\nthread %llu MultiStream audio SPU\n"
            "capture: %u\ntrigger-cpu-us: %llu\nsamples: %u\nperiod-us: 10000\n"
            "duration-us: %llu\nquery-total-us: %llu\nquery-max-us: %llu\nsuspended: %u\nmach-error: %d\n"
            "Samples cover active work, including any waits inside that work; percentages are not CPU percentages.\n"
            "CALLER COUNTS (count tid pc lr)\n",
            (unsigned long long)(uintptr_t)_dyld_get_image_header(0), (unsigned long long)tid,
            number,(unsigned long long)cpu,count,(unsigned long long)duration,
            (unsigned long long)overhead,(unsigned long long)max_overhead,suspended,error);
        for (const auto& [key,n] : callers)
            std::fprintf(f,"%u %llu %llx %llx\n",n,(unsigned long long)tid,
                (unsigned long long)key.first,(unsigned long long)key.second);
        std::fprintf(f,"PROFILE COUNTS (count tid pc)\n");
        for (const auto& [pc,n] : pcs)
            std::fprintf(f,"%u %llu %llx\n",n,(unsigned long long)tid,(unsigned long long)pc);
        unsigned named = 0;
        for (uint64_t pc : addresses) {
            if (named == 256) break;
            Dl_info info{};
            if (dladdr(reinterpret_cast<const void*>(pc), &info) &&
                info.dli_fbase != reinterpret_cast<const void*>(_dyld_get_image_header(0))) {
                const char* name = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
                std::fprintf(f,"pc %llx %.48s %.128s\n",(unsigned long long)pc,
                    name ? name + 1 : "?",info.dli_sname ? info.dli_sname : "?");
                ++named;
            }
        }
        std::fclose(f);
        std::rename(temp.c_str(),output.c_str());
    }
    std::fprintf(stderr,"[audio-profile] capture=%u samples=%u duration_ms=%.1f query_total_ms=%.3f query_max_ms=%.3f error=%d\n",
        number,count,duration/1000.0,overhead/1000.0,max_overhead/1000.0,error);
}
}
extern "C" void fp_ios_audio_profile_start(const char* docs) {
    // Continuous profiling is separately opt-in; avoid mixing its samples.
    if (!output.empty()) return;
    output = std::string(docs) + "/audio-profile.txt";
    unlink(output.c_str()); // discard a preceding process/build's addresses
    const char* option = std::getenv("FP_AUDIO_PROFILE");
    if (std::getenv("FP_PROF") || !option || option[0] != '1') {
        std::fprintf(stderr,"[audio-profile] instruction sampling off; CPU timing and diagnostic uploads remain enabled\n");
        return;
    }
    g_spu_task_run_observer = note_round;
    std::thread([] {
        pthread_setname_np("audio-profile");
        unsigned number = 0;
        for (;;) {
            const mach_port_t target = request.exchange(MACH_PORT_NULL,std::memory_order_acquire);
            if (target != MACH_PORT_NULL) {
                capture(target,++number);
                if (number == 2) finished.store(true,std::memory_order_release);
                busy.store(false,std::memory_order_release);
                if (number == 2) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
    std::fprintf(stderr,"[audio-profile] armed: 3 over-budget rounds, 8 seconds at 100 Hz, maximum 2 captures\n");
}
