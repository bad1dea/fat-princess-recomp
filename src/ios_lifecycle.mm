#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#include <atomic>
#include <cstdio>
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
