#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <mach/mach.h>
#include "ios_lifecycle.h"

extern "C" void fp_audio_set_interrupted(int paused);
extern "C" void fp_audio_reset_output(void);

namespace {
enum { Inactive = 1, Background = 2, AudioInterrupted = 4 };
std::atomic<unsigned> reasons{Inactive};
std::atomic<unsigned> resume_generation{0};

void update(unsigned set, unsigned clear, const char* event) {
    const unsigned previous = reasons.load();
    const unsigned state = (previous | set) & ~clear;
    reasons.store(state);
    if ((previous & (Inactive | Background)) && !(state & (Inactive | Background))) resume_generation.fetch_add(1);
    fp_audio_set_interrupted(state != 0);
    std::fprintf(stderr, "[ios-state] %s active=%d audio-paused=%d reasons=%u\n",
                 event, !(state & (Inactive | Background)), state != 0, state);
}
}

extern "C" int fp_ios_is_active(void) {
    return !(reasons.load() & (Inactive | Background));
}
extern "C" unsigned fp_ios_resume_generation(void) { return resume_generation.load(); }

extern "C" int fp_ios_audio_output_info(unsigned*rate,unsigned*frames) {
    AVAudioSession*session=AVAudioSession.sharedInstance;
    const double hz=session.sampleRate,seconds=session.IOBufferDuration;
    if(!std::isfinite(hz) || !std::isfinite(seconds) || hz<8000 || hz>192000 || seconds<=0 || seconds>0.25)return 0;
    *rate=(unsigned)std::lround(hz);*frames=(unsigned)std::lround(hz*seconds);
    return *frames!=0;
}

extern "C" void fp_ios_lifecycle_start(void) {
    static NSMutableArray* observers;
    if (observers) return;
    observers = [NSMutableArray array];
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    auto observe = [&](NSNotificationName name, void (^block)(NSNotification*)) {
        [observers addObject:[center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:block]];
    };
    observe(UIApplicationWillResignActiveNotification, ^(NSNotification*) { update(Inactive, 0, "will-resign-active"); });
    observe(UIApplicationDidEnterBackgroundNotification, ^(NSNotification*) { update(Inactive | Background, 0, "did-enter-background"); });
    observe(UIApplicationWillEnterForegroundNotification, ^(NSNotification*) { update(Inactive, Background, "will-enter-foreground"); });
    observe(UIApplicationDidBecomeActiveNotification, ^(NSNotification*) {
        // SDL also retries activation here: an interruption-end notification can be absent.
        update(0, Inactive | Background | AudioInterrupted, "did-become-active");
    });
    observe(AVAudioSessionInterruptionNotification, ^(NSNotification* notification) {
        const bool began = [notification.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] == AVAudioSessionInterruptionTypeBegan;
        update(began ? AudioInterrupted : 0, began ? 0 : AudioInterrupted, began ? "audio-interruption-began" : "audio-interruption-ended");
    });
    __block unsigned route_rate=0,route_frames=0;
    fp_ios_audio_output_info(&route_rate,&route_frames);
    observe(AVAudioSessionRouteChangeNotification, ^(NSNotification*note) {
        unsigned rate=0,frames=0;
        if(!fp_ios_audio_output_info(&rate,&frames))return;
        const bool changed=route_rate && (route_rate!=rate || route_frames!=frames);
        route_rate=rate;route_frames=frames;
        // Reopening SDL must not recursively reset for an unchanged category/format.
        if(changed)fp_audio_reset_output();
        std::fprintf(stderr,"[ios-state] audio-route-clock rate=%u io_frames=%u reason=%lu reset=%d\n",
            rate,frames,(unsigned long)[note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue],changed);
    });
    observe(AVAudioSessionMediaServicesWereResetNotification, ^(NSNotification*) {
        fp_audio_reset_output();
        std::fprintf(stderr, "[ios-state] audio-media-services-reset\n");
    });
    observe(UIApplicationUserDidTakeScreenshotNotification, ^(NSNotification*) {
        resume_generation.fetch_add(1);   // capture/UI work can block an entire event-pump call
        std::fprintf(stderr, "[ios-state] screenshot active=%d\n", fp_ios_is_active());
    });
    const UIApplicationState state = UIApplication.sharedApplication.applicationState;
    update(state == UIApplicationStateActive ? 0 : Inactive, state == UIApplicationStateActive ? Inactive : 0, "observer-start");
}

// Called after the host iteration's pool drains. One task query per five
// seconds, rather than per poll, tracks physical use separately from the large
// deliberately reserved guest arena and worker stacks.
extern "C" void fp_ios_memory_sample(void) {
    using Clock=std::chrono::steady_clock;
    static Clock::time_point next{};
    const auto now=Clock::now();
    if(now<next)return;
    next=now+std::chrono::seconds(5);
    task_vm_info_data_t info{};
    mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&info),&count)!=KERN_SUCCESS)return;
    static uint64_t peak=0;
    if(info.phys_footprint>peak)peak=info.phys_footprint;
    std::fprintf(stderr,"[ios-memory] ts_us=%llu resident_mb=%.2f footprint_mb=%.2f peak_mb=%.2f virtual_mb=%.2f pool=per-host-iteration\n",
        (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count(),
        info.resident_size/1048576.,info.phys_footprint/1048576.,peak/1048576.,info.virtual_size/1048576.);
}
