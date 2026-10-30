// First-run setup for test builds: when the game files are missing, ask for a key and download the game zip from
// FP_FILES_URL + key into Documents, where ios_launch.cpp unpacks it. Builds without FP_FILES_URL skip this and
// wait for the files to be copied in through the Files app as before.
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#include <atomic>
#include <cstdio>
#include <string>

#ifdef FP_FILES_URL
@interface FPSetupDownload : NSObject <NSURLSessionDownloadDelegate>
@property(nonatomic) NSString* target;
@property(nonatomic) double progress;
@property(nonatomic) NSInteger status;
@property(nonatomic) BOOL finished;
@property(nonatomic) BOOL saved;
@property(nonatomic, copy) NSString* error;
@end

@implementation FPSetupDownload
- (void)URLSession:(NSURLSession*)s downloadTask:(NSURLSessionDownloadTask*)t didWriteData:(int64_t)n
    totalBytesWritten:(int64_t)written totalBytesExpectedToWrite:(int64_t)total {
    if (total > 0) self.progress = double(written) / double(total);
}
- (void)URLSession:(NSURLSession*)s downloadTask:(NSURLSessionDownloadTask*)t didFinishDownloadingToURL:(NSURL*)url {
    self.status = [(NSHTTPURLResponse*)t.response statusCode];
    if (self.status != 200) return;
    NSFileManager* fm = NSFileManager.defaultManager;
    [fm removeItemAtPath:self.target error:nil];
    NSError* e = nil;
    self.saved = [fm moveItemAtURL:url toURL:[NSURL fileURLWithPath:self.target] error:&e];
    if (!self.saved) self.error = e.localizedDescription;
}
- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t didCompleteWithError:(NSError*)e {
    if (e) self.error = e.localizedDescription;
    self.finished = YES;
}
@end

namespace {
UIWindow* g_window;

void pump(const std::atomic<bool>& done) {
    while (!done.load()) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
}

UIViewController* host() {
    if (!g_window) {
        UIWindowScene* scene = nil;
        for (UIScene* s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene*)s; break; }
        g_window = scene ? [[UIWindow alloc] initWithWindowScene:scene] : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
        g_window.rootViewController = [UIViewController new];
        g_window.rootViewController.view.backgroundColor = UIColor.blackColor;
        g_window.windowLevel = UIWindowLevelAlert;
        [g_window makeKeyAndVisible];
    }
    return g_window.rootViewController;
}

// Shows the alert and waits for a button. Returns the text field's contents, or nil when the player declined.
NSString* ask(NSString* message) {
    __block NSString* key = nil;
    std::atomic<bool> done{false};
    std::atomic<bool>* d = &done;
    UIAlertController* a = [UIAlertController alertControllerWithTitle:@"Game files needed" message:message
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addTextFieldWithConfigurationHandler:^(UITextField* f) {
        f.placeholder = @"Key";
        f.autocapitalizationType = UITextAutocapitalizationTypeNone;
        f.autocorrectionType = UITextAutocorrectionTypeNo;
        f.secureTextEntry = NO;
    }];
    [a addAction:[UIAlertAction actionWithTitle:@"Not now" style:UIAlertActionStyleCancel
                                        handler:^(UIAlertAction*) { d->store(true); }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Download" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
        key = [a.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        d->store(true);
    }]];
    [host() presentViewController:a animated:YES completion:nil];
    pump(done);
    return key.length ? key : nil;
}

UIAlertController* g_status;
void status(NSString* title, NSString* message) {
    if (g_status) { g_status.title = title; g_status.message = message; return; }
    g_status = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [host() presentViewController:g_status animated:YES completion:nil];
}
void status_close() {
    if (!g_status) return;
    std::atomic<bool> done{false};
    std::atomic<bool>* d = &done;
    [g_status dismissViewControllerAnimated:NO completion:^{ d->store(true); }];
    pump(done);
    g_status = nil;
}
}

// Returns true when the zip is in Documents. Leaves "Unpacking..." on screen; fp_ios_setup_close() removes it.
extern "C" bool fp_ios_fetch_game(const char* docs) {
    NSString* message = @"Enter your key to download them (about 330 MB, Wi-Fi recommended).";
    for (;;) {
        NSString* key = ask(message);
        if (!key) return false;
        NSString* base = @FP_FILES_URL;
        NSString* path = [key stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet];
        NSURL* url = [NSURL URLWithString:[base stringByAppendingString:path]];
        FPSetupDownload* dl = [FPSetupDownload new];
        dl.target = [NSString stringWithFormat:@"%s/FatPrincess-game.zip", docs];
        NSURLSession* session = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.defaultSessionConfiguration
                                                              delegate:dl delegateQueue:NSOperationQueue.mainQueue];
        UIApplication.sharedApplication.idleTimerDisabled = YES;
        status(@"Downloading", @"Starting...");
        [[session downloadTaskWithURL:url] resume];
        int shown = -1;
        while (!dl.finished) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
            const int pct = int(dl.progress * 100.0);
            if (pct != shown) { shown = pct; status(@"Downloading", [NSString stringWithFormat:@"%d%%\nKeep the app open.", pct]); }
        }
        [session finishTasksAndInvalidate];
        UIApplication.sharedApplication.idleTimerDisabled = NO;
        std::fprintf(stderr, "[ios] game download: HTTP %ld%s%s\n", long(dl.status), dl.error ? ", " : "",
                     dl.error ? dl.error.UTF8String : "");
        if (dl.saved) { status(@"Installing", @"Unpacking the game files..."); CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false); return true; }
        status_close();
        message = dl.status == 404 ? @"That key did not work. Try again."
                                   : [NSString stringWithFormat:@"The download failed (%@). Try again.",
                                      dl.error ?: [NSString stringWithFormat:@"HTTP %ld", long(dl.status)]];
    }
}

extern "C" void fp_ios_setup_close(void) {
    status_close();
    if (g_window) { g_window.hidden = YES; g_window = nil; }
}
#else
extern "C" bool fp_ios_fetch_game(const char*) { return false; }
extern "C" void fp_ios_setup_close(void) {}
#endif
#endif
