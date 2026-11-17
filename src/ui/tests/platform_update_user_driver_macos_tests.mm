// The one question Bloom answers for Sparkle (platform_update_user_driver_macos.h): "ready to
// install" is answered with "install" at once and never reaches the window, so no update is
// left with Sparkle's installer for the moment Bloom quits; every other call is passed on.

#import "platform_update_user_driver_macos.h"

#include <cstdio>

// A window that leaves the question open, as Sparkle's own does until its button is pressed.
@interface FakeShownDriver : NSObject
@property(nonatomic) int extracting;
@property(nonatomic) int ready;
- (void)showDownloadDidStartExtractingUpdate;
- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply;
@end

@implementation FakeShownDriver

- (void)showDownloadDidStartExtractingUpdate {
    self.extracting += 1;
}

- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply {
    self.ready += 1;
}

@end

int main() {
    @autoreleasepool {
        int failures = 0;
        const auto expect = [&failures](const bool passed, const char* what) {
            if (!passed) {
                std::fprintf(stderr, "FAILED: %s\n", what);
                failures += 1;
            }
        };

        FakeShownDriver* shown = [[FakeShownDriver alloc] init];
        id window = shown;
        id between = [[BloomUserDriver alloc] initWithDriver:window announce:NO];
        id<SPUUserDriver> driver = between;

        [driver showDownloadDidStartExtractingUpdate];
        expect(shown.extracting == 1, "a call Bloom has no answer for is passed to the window");

        __block int answers = 0;
        __block SPUUserUpdateChoice answer = SPUUserUpdateChoiceDismiss;
        [driver showReadyToInstallAndRelaunch:^(SPUUserUpdateChoice choice) {
          answers += 1;
          answer = choice;
        }];
        expect(answers == 1 && answer == SPUUserUpdateChoiceInstall,
               "ready to install is answered with install, before the call returns");
        expect(shown.ready == 0, "the window is never asked, so the question is never left open");

        expect([between respondsToSelector:@selector(showDownloadDidStartExtractingUpdate)],
               "what the window can be told, Bloom's driver can be told");
        expect(![between respondsToSelector:@selector(showUpdateInFocus)],
               "what the window cannot be told is not claimed");
        return failures == 0 ? 0 : 1;
    }
}
