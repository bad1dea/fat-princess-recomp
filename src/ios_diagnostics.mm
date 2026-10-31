// Diagnostic snapshots for iOS test builds. Network work stays off the game thread.
#import <UIKit/UIKit.h>
#include <cstdio>
#include <cstdlib>
#include <sys/utsname.h>
#include <map>
#include <algorithm>
#include <vector>
#include <cstdint>

#ifdef FP_REPORT_URL
static NSData* tailFile(NSString* path, NSUInteger limit) {
    NSFileHandle* file = [NSFileHandle fileHandleForReadingAtPath:path];
    if (!file) return [NSData data];
    @try {
        unsigned long long size = [file seekToEndOfFile];
        [file seekToFileOffset:size > limit ? size - limit : 0];
        NSData* data = [file readDataOfLength:limit];
        [file closeFile];
        return data;
    } @catch (NSException*) { [file closeFile]; return [NSData data]; }
}

@interface FPDiagnostics : NSObject
@property(nonatomic) NSString* docs;
@property(nonatomic) NSURLSession* session;
@property(nonatomic) dispatch_source_t timer;
@property(nonatomic) BOOL busy;
@property(nonatomic) dispatch_queue_t queue;
@end
@implementation FPDiagnostics
- (void)send {
    if (self.busy) return;
    NSString* pending = [self.docs stringByAppendingPathComponent:@"diagnostic-pending.txt"];
    NSData* body = [NSData dataWithContentsOfFile:pending];
    if (!body) {
        struct utsname hardware; uname(&hardware);
        NSString* header = [NSString stringWithFormat:@"device: %s\nos: %@\nbuild: %@\ntime: %@\n\n", hardware.machine,
            NSProcessInfo.processInfo.operatingSystemVersionString,
            [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"unknown", NSDate.date];
        NSMutableData* snapshot = [[header dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
        [snapshot appendData:tailFile([self.docs stringByAppendingPathComponent:@"play.log"], 512 * 1024)];
        [snapshot appendData:[@"\n\nPROFILE MAPS\n" dataUsingEncoding:NSUTF8StringEncoding]];
        const char* prof = getenv("FP_PROF");
        if (prof) {
            struct Sample { uint32_t tid, us; uint64_t pc; };
            std::map<std::pair<uint32_t, uint64_t>, size_t> counts;
            if (FILE* file = fopen(prof, "rb")) {
                Sample sample;
                while (fread(&sample, sizeof sample, 1, file) == 1) ++counts[{sample.tid, sample.pc}];
                fclose(file);
            }
            std::vector<std::pair<size_t, std::pair<uint32_t, uint64_t>>> ranked;
            for (const auto& entry : counts) ranked.push_back({entry.second, entry.first});
            std::sort(ranked.rbegin(), ranked.rend());
            NSMutableString* summary = [NSMutableString stringWithString:@"\nPROFILE COUNTS (count tid pc)\n"];
            for (size_t i = 0; i < std::min(size_t(2048), ranked.size()); ++i)
                [summary appendFormat:@"%zu %u %llx\n", ranked[i].first, ranked[i].second.first, (unsigned long long)ranked[i].second.second];
            [snapshot appendData:[summary dataUsingEncoding:NSUTF8StringEncoding]];
        }
        if (prof) [snapshot appendData:tailFile([NSString stringWithFormat:@"%s.maps", prof], 128 * 1024)];
        body = snapshot;
        if (![body writeToFile:pending atomically:YES]) return;
    }
    NSURLComponents* url = [NSURLComponents componentsWithString:@FP_REPORT_URL];
    if (![url.scheme isEqualToString:@"https"]) return;
    url.queryItems = @[[NSURLQueryItem queryItemWithName:@"name" value:@"ios"],
        [NSURLQueryItem queryItemWithName:@"kind" value:@"diag"],
        [NSURLQueryItem queryItemWithName:@"build" value:[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"unknown"]];
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url.URL];
    request.HTTPMethod = @"POST";
    [request setValue:@"text/plain; charset=utf-8" forHTTPHeaderField:@"Content-Type"];
    self.busy = YES;
    [[self.session uploadTaskWithRequest:request fromData:body completionHandler:^(NSData*, NSURLResponse* response, NSError* error) {
        NSInteger status = [response isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse*)response).statusCode : 0;
        dispatch_async(self.queue, ^{
            if (!error && status >= 200 && status < 300) [[NSFileManager defaultManager] removeItemAtPath:pending error:nil];
            self.busy = NO;
            fprintf(stderr, "[ios-diag] upload status=%ld error=%ld\n", (long)status, (long)error.code);
        });
    }] resume];
}
@end
#endif

extern "C" void fp_ios_diagnostics_start(const char* docs) {
#ifdef FP_REPORT_URL
    static FPDiagnostics* diagnostic;
    if (diagnostic) return;
    diagnostic = [FPDiagnostics new];
    diagnostic.docs = [NSString stringWithUTF8String:docs];
    NSURLSessionConfiguration* config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForRequest = 20;
    config.timeoutIntervalForResource = 30;
    diagnostic.session = [NSURLSession sessionWithConfiguration:config];
    diagnostic.queue = dispatch_queue_create("diagnostic-upload", DISPATCH_QUEUE_SERIAL);
    diagnostic.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, diagnostic.queue);
    dispatch_source_set_timer(diagnostic.timer, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), 120 * NSEC_PER_SEC, 5 * NSEC_PER_SEC);
    dispatch_source_set_event_handler(diagnostic.timer, ^{ [diagnostic send]; });
    dispatch_resume(diagnostic.timer);
#else
    (void)docs;
#endif
}
