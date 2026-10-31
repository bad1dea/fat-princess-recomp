// First-run setup for test builds: when the game files are missing, ask for a key and download the game zip from
// FP_FILES_URL + key into Documents, where ios_launch.cpp unpacks it. Builds without FP_FILES_URL skip this and
// wait for the files to be copied in through the Files app as before.
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
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

// The setup page: a dark backdrop with one card (title, state, progress bar, numbers, a note).
@interface FPSetupController : UIViewController
@property(nonatomic) UILabel* state;
@property(nonatomic) UILabel* percent;
@property(nonatomic) UILabel* detail;
@property(nonatomic) UILabel* note;
@property(nonatomic) UIProgressView* bar;
@property(nonatomic) UIActivityIndicatorView* spinner;
@property(nonatomic) CAGradientLayer* backdrop;
@end

@implementation FPSetupController
- (UILabel*)label:(CGFloat)size weight:(UIFontWeight)weight color:(UIColor*)color {
    UILabel* l = [UILabel new];
    l.font = [UIFont systemFontOfSize:size weight:weight];
    l.textColor = color;
    l.textAlignment = NSTextAlignmentCenter;
    l.numberOfLines = 0;
    return l;
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.04 green:0.03 blue:0.03 alpha:1];
    self.view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    UIColor* gold = [UIColor colorWithRed:0.91 green:0.71 blue:0.29 alpha:1];
    UIColor* text = [UIColor colorWithWhite:0.93 alpha:1];
    UIColor* dim = [UIColor colorWithWhite:0.62 alpha:1];
    self.backdrop = [CAGradientLayer layer];
    self.backdrop.colors = @[(id)[UIColor colorWithRed:0.16 green:0.11 blue:0.08 alpha:1].CGColor,
                             (id)[UIColor colorWithRed:0.04 green:0.03 blue:0.03 alpha:1].CGColor];
    [self.view.layer addSublayer:self.backdrop];

    UIView* card = [UIView new];
    card.backgroundColor = [UIColor colorWithRed:0.10 green:0.08 blue:0.07 alpha:0.92];
    card.layer.cornerRadius = 18;
    card.layer.borderWidth = 1;
    card.layer.borderColor = [UIColor colorWithRed:0.91 green:0.71 blue:0.29 alpha:0.25].CGColor;
    card.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:card];

    UILabel* title = [self label:26 weight:UIFontWeightHeavy color:gold];
    title.text = @"Fat Princess";
    self.state = [self label:16 weight:UIFontWeightSemibold color:text];
    self.percent = [self label:44 weight:UIFontWeightBold color:text];
    self.percent.font = [UIFont monospacedDigitSystemFontOfSize:44 weight:UIFontWeightBold];
    self.bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    self.bar.progressTintColor = gold;
    self.bar.trackTintColor = [UIColor colorWithWhite:1 alpha:0.12];
    self.bar.layer.cornerRadius = 3;
    self.bar.clipsToBounds = YES;
    [self.bar.heightAnchor constraintEqualToConstant:6].active = YES;
    self.detail = [self label:13 weight:UIFontWeightRegular color:dim];
    self.detail.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightRegular];
    self.spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    self.spinner.color = gold;
    self.spinner.hidesWhenStopped = YES;
    self.note = [self label:13 weight:UIFontWeightMedium color:dim];
    self.note.text = @"Keep the app open until this finishes.";

    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, self.state, self.percent, self.bar,
                                                                         self.detail, self.spinner, self.note]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    [stack setCustomSpacing:4 afterView:title];
    [stack setCustomSpacing:18 afterView:self.detail];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:stack];

    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint* wide = [card.widthAnchor constraintEqualToConstant:380];
    wide.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [card.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [card.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        wide,
        [card.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor constant:-32],
        [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:26],
        [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-22],
        [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:26],
        [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-26],
    ]];
    [self show:@"Game files needed" percent:nil progress:0 detail:@"" busy:NO];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    self.backdrop.frame = self.view.bounds;
}
- (void)show:(NSString*)state percent:(NSString*)percent progress:(float)p detail:(NSString*)detail busy:(BOOL)busy {
    self.state.text = state;
    self.percent.text = percent;
    self.percent.hidden = percent == nil;
    self.bar.hidden = percent == nil;
    [self.bar setProgress:p animated:NO];
    self.detail.text = detail;
    self.detail.hidden = detail.length == 0;
    if (busy) [self.spinner startAnimating]; else [self.spinner stopAnimating];
}
@end

namespace {
UIWindow* g_window;
FPSetupController* g_page;

void pump(const std::atomic<bool>& done) {
    while (!done.load()) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
}

FPSetupController* page() {
    if (!g_window) {
        UIWindowScene* scene = nil;
        for (UIScene* s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene*)s; break; }
        g_window = scene ? [[UIWindow alloc] initWithWindowScene:scene] : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
        g_page = [FPSetupController new];
        g_window.frame = (scene ? scene.coordinateSpace.bounds : UIScreen.mainScreen.bounds);
        g_window.rootViewController = g_page;
        g_window.windowLevel = UIWindowLevelAlert;
        [g_window makeKeyAndVisible];
    }
    return g_page;
}

// Asks for the key over the setup page. Returns it, or nil when the player declined.
NSString* ask(NSString* message) {
    __block NSString* key = nil;
    std::atomic<bool> done{false};
    std::atomic<bool>* d = &done;
    FPSetupController* p = page();
    [p show:@"Game files needed" percent:nil progress:0 detail:@"" busy:NO];
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
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
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
                detail = [detail stringByAppendingFormat:@"  ·  %.1f MB/s  ·  %@", rate / 1e6,
                          left >= 60 ? [NSString stringWithFormat:@"%d min %d s left", left / 60, left % 60]
                                     : [NSString stringWithFormat:@"%d s left", left]];
            }
            [p show:@"Downloading game files" percent:[NSString stringWithFormat:@"%d%%", int(frac * 100)]
                progress:frac detail:detail busy:NO];
        }
        [session finishTasksAndInvalidate];
        UIApplication.sharedApplication.idleTimerDisabled = NO;
        std::fprintf(stderr, "[ios] game download: HTTP %ld, %lld bytes in %.0f s%s%s\n", long(dl.status),
                     (long long)dl.written, CACurrentMediaTime() - start, dl.error ? ", " : "", dl.error ? dl.error.UTF8String : "");
        if (dl.saved) {
            [p show:@"Installing" percent:nil progress:0 detail:@"Unpacking the game files. This takes a minute." busy:YES];
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
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
