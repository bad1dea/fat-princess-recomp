// First-run setup for test builds: when the game files are missing, ask for a key and download the game zip from
// FP_FILES_URL + key into Documents, where ios_launch.cpp unpacks it. Builds without FP_FILES_URL skip this and
// wait for the files to be copied in through the Files app as before.
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>   // CACurrentMediaTime
#include <atomic>
#include <cstdio>

#if !__has_feature(objc_arc)
#error "ios_setup.mm needs ARC: objects here live across run-loop turns"
#endif

#ifdef FP_FILES_URL
@interface FPSetupDownload : NSObject <NSURLSessionDownloadDelegate>
@property(nonatomic) NSString* target;
@property(nonatomic) int64_t written;
@property(nonatomic) int64_t total;
@property(nonatomic) NSInteger status;
@property(nonatomic) BOOL finished;
@property(nonatomic) BOOL saved;
@property(nonatomic, copy) NSString* error;
@end

@implementation FPSetupDownload
- (void)URLSession:(NSURLSession*)s downloadTask:(NSURLSessionDownloadTask*)t didWriteData:(int64_t)n
    totalBytesWritten:(int64_t)written totalBytesExpectedToWrite:(int64_t)total {
    self.written = written;
    if (total > 0) self.total = total;
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

// The setup page, in the settings menu's palette (settings_panel.h): navy ground, a pink rule along the top and bottom
// edges and the title in pink, off-white text, blue-gray numbers, one thin pink progress bar, lower-left.
static UIColor* fp_rgb(int r, int g, int b) { return [UIColor colorWithRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:1]; }

@interface FPSetupController : UIViewController
@property(nonatomic) UILabel* state;
@property(nonatomic) UILabel* detail;
@property(nonatomic) UILabel* note;
@property(nonatomic) UIProgressView* bar;
@property(nonatomic) UIActivityIndicatorView* spinner;
@end

@implementation FPSetupController
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UILabel*)label:(UIFont*)font color:(UIColor*)color {
    UILabel* l = [UILabel new];
    l.font = font;
    l.textColor = color;
    l.numberOfLines = 0;
    return l;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    UIColor *ground = fp_rgb(24, 27, 36), *text = fp_rgb(242, 235, 220), *muted = fp_rgb(133, 149, 164);
    UIColor *accent = fp_rgb(238, 145, 174), *rule = fp_rgb(85, 71, 87);
    self.view.backgroundColor = ground;
    self.view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    for (int edge = 0; edge < 2; ++edge) {   // the menu panel's top and bottom pink rules, across the screen
        UIView* line = [UIView new];
        line.backgroundColor = accent;
        line.translatesAutoresizingMaskIntoConstraints = NO;
        [self.view addSubview:line];
        [NSLayoutConstraint activateConstraints:@[
            [line.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
            [line.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
            [line.heightAnchor constraintEqualToConstant:3],
            edge ? [line.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
                 : [line.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        ]];
    }

    self.state = [self label:[UIFont monospacedSystemFontOfSize:17 weight:UIFontWeightBold] color:accent];
    self.spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    self.spinner.color = accent;
    self.spinner.hidesWhenStopped = YES;
    UIStackView* head = [[UIStackView alloc] initWithArrangedSubviews:@[self.spinner, self.state]];
    head.spacing = 10;
    head.alignment = UIStackViewAlignmentCenter;
    UIView* divider = [UIView new];
    divider.backgroundColor = rule;
    [divider.heightAnchor constraintEqualToConstant:1].active = YES;

    self.bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleBar];
    self.bar.progressTintColor = accent;
    self.bar.trackTintColor = rule;
    self.detail = [self label:[UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightRegular] color:text];
    self.note = [self label:[UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular] color:muted];
    self.note.text = @"Keep the app open until this finishes.";

    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[head, divider, self.bar, self.detail, self.note]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentFill;
    stack.spacing = 8;
    [stack setCustomSpacing:10 afterView:head];
    [stack setCustomSpacing:14 afterView:divider];
    [stack setCustomSpacing:16 afterView:self.detail];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];

    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint* width = [stack.widthAnchor constraintEqualToAnchor:safe.widthAnchor multiplier:0.45];
    width.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:32],
        [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-32],
        width,
        [stack.widthAnchor constraintGreaterThanOrEqualToConstant:260],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:440],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-32],
    ]];
    [self show:@"Game data needed" percent:nil progress:0 detail:@"" busy:NO];
}
// percent nil hides the bar; the percentage itself is part of the detail line.
- (void)show:(NSString*)state percent:(NSString*)percent progress:(float)p detail:(NSString*)detail busy:(BOOL)busy {
    self.state.text = state;
    self.bar.hidden = percent == nil;
    [self.bar setProgress:p animated:NO];
    self.detail.text = percent ? [NSString stringWithFormat:@"%@   %@", percent, detail] : detail;
    self.detail.hidden = self.detail.text.length == 0;
    if (busy) [self.spinner startAnimating]; else [self.spinner stopAnimating];
}
@end

namespace {
UIWindow* g_window;
FPSetupController* g_page;

// The page must cover the whole screen whatever the scene reports at creation (an iPad scene can still be
// settling its size and orientation then), so it is resized to the current screen on every run-loop turn.
void fit() {
    if (!g_window) return;
    const CGRect screen = g_window.windowScene ? g_window.windowScene.coordinateSpace.bounds : UIScreen.mainScreen.bounds;
    if (!CGRectEqualToRect(g_window.frame, screen)) g_window.frame = screen;
    UIView* v = g_window.rootViewController.view;
    if (!CGRectEqualToRect(v.frame, g_window.bounds)) v.frame = g_window.bounds;
}

void run(double seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    fit();
}

void pump(const std::atomic<bool>& done) {
    while (!done.load()) run(0.05);
}

FPSetupController* page() {
    if (!g_window) {
        UIWindowScene* scene = nil;
        for (UIScene* s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene*)s; break; }
        g_window = scene ? [[UIWindow alloc] initWithWindowScene:scene] : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
        g_page = [FPSetupController new];
        g_window.rootViewController = g_page;
        g_window.windowLevel = UIWindowLevelAlert;
        g_window.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;   // the key prompt matches the page
        [g_window makeKeyAndVisible];
        fit();
    }
    return g_page;
}

// Asks for the key over the setup page. Returns it, or nil when the player declined.
NSString* ask(NSString* message) {
    __block NSString* key = nil;
    std::atomic<bool> done{false};
    std::atomic<bool>* d = &done;
    FPSetupController* p = page();
    [p show:@"Game data needed" percent:nil progress:0 detail:@"" busy:NO];
    UIAlertController* a = [UIAlertController alertControllerWithTitle:@"Enter your key" message:message
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addTextFieldWithConfigurationHandler:^(UITextField* f) {
        f.placeholder = @"Key";
        f.secureTextEntry = YES;
        f.autocapitalizationType = UITextAutocapitalizationTypeNone;
        f.autocorrectionType = UITextAutocorrectionTypeNo;
        f.textContentType = UITextContentTypeOneTimeCode;   // keeps the password manager from offering to save it
    }];
    [a addAction:[UIAlertAction actionWithTitle:@"Not now" style:UIAlertActionStyleCancel
                                        handler:^(UIAlertAction*) { d->store(true); }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Download" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
        key = [a.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        d->store(true);
    }]];
    [p presentViewController:a animated:YES completion:nil];
    pump(done);
    return key.length ? key : nil;
}

NSString* megabytes(int64_t bytes) { return [NSString stringWithFormat:@"%.0f MB", double(bytes) / 1e6]; }
}

// Returns true when the zip is in Documents. Leaves "Installing" on screen; fp_ios_setup_close() removes it.
extern "C" bool fp_ios_fetch_game(const char* docs) {
    NSString* message = @"The game files are about 330 MB. Wi-Fi recommended.";
    for (;;) {
        NSString* key = ask(message);
        if (!key) return false;
        NSString* path = [key stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet];
        NSURL* url = [NSURL URLWithString:[@FP_FILES_URL stringByAppendingString:path]];
        FPSetupDownload* dl = [FPSetupDownload new];
        dl.target = [NSString stringWithFormat:@"%s/FatPrincess-game.zip", docs];
        NSURLSession* session = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.defaultSessionConfiguration
                                                              delegate:dl delegateQueue:NSOperationQueue.mainQueue];
        UIApplication.sharedApplication.idleTimerDisabled = YES;
        FPSetupController* p = page();
        [p show:@"Connecting" percent:nil progress:0 detail:@"" busy:YES];
        [[session downloadTaskWithURL:url] resume];
        const CFTimeInterval start = CACurrentMediaTime();
        CFTimeInterval last = 0, rate_t = start;
        int64_t rate_bytes = 0;
        double rate = 0;
        while (!dl.finished) {
            run(0.1);
            const CFTimeInterval now = CACurrentMediaTime();
            if (now - last < 0.25 || dl.total <= 0) continue;
            last = now;
            if (now - rate_t >= 1.0) {   // speed over the last second, smoothed
                const double r = double(dl.written - rate_bytes) / (now - rate_t);
                rate = rate > 0 ? rate * 0.6 + r * 0.4 : r;
                rate_t = now; rate_bytes = dl.written;
            }
            const float frac = float(double(dl.written) / double(dl.total));
            NSString* detail = [NSString stringWithFormat:@"%@ of %@", megabytes(dl.written), megabytes(dl.total)];
            if (rate > 0) {
                const int left = int(double(dl.total - dl.written) / rate);
                detail = [detail stringByAppendingFormat:@"   %.1f MB/s   %@", rate / 1e6,
                          left >= 60 ? [NSString stringWithFormat:@"%d min %d s left", left / 60, left % 60]
                                     : [NSString stringWithFormat:@"%d s left", left]];
            }
            [p show:@"Downloading game data" percent:[NSString stringWithFormat:@"%d%%", int(frac * 100)]
                progress:frac detail:detail busy:NO];
        }
        [session finishTasksAndInvalidate];
        UIApplication.sharedApplication.idleTimerDisabled = NO;
        std::fprintf(stderr, "[ios] game download: HTTP %ld, %lld bytes in %.0f s%s%s\n", long(dl.status),
                     (long long)dl.written, CACurrentMediaTime() - start, dl.error ? ", " : "", dl.error ? dl.error.UTF8String : "");
        if (dl.saved) {
            [p show:@"Installing game data" percent:nil progress:0 detail:@"Unpacking. This takes about a minute." busy:YES];
            run(0.3);
            return true;
        }
        message = dl.status == 404 ? @"That key did not work. Try again."
                                   : [NSString stringWithFormat:@"The download failed (%@). Try again.",
                                      dl.error ?: [NSString stringWithFormat:@"HTTP %ld", long(dl.status)]];
    }
}

extern "C" void fp_ios_setup_close(void) {
    if (g_window) { g_window.hidden = YES; g_window = nil; g_page = nil; }
}
#else
extern "C" bool fp_ios_fetch_game(const char*) { return false; }
extern "C" void fp_ios_setup_close(void) {}
#endif
#endif
