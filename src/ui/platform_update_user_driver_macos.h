// The macOS update driver's own user driver (ADR 0041, "The platform updaters"). Objective-C,
// for platform_update_driver_macos.mm and its test.
//
// What stands between Sparkle and the window it shows (Sparkle's own, or the end-to-end update
// test's silent stand-in). Everything is passed on but one question.
//
// When Sparkle has unpacked and checked an update it asks once more, "ready to install?", and
// waits. While it waits its installer already holds the update, and installs it the moment
// Bloom is gone for whatever reason, without asking Bloom anything: no copy of the installed
// version is kept and nothing watches the new one start. An artist who quits Bloom with that
// question still open would get exactly that. So the question is never left open: it is
// answered with "install" at once, which is what the artist asked for when the download was
// started, and Sparkle's course goes on to the driver's delegate, which keeps the copy and
// starts the watch before Sparkle asks Bloom to quit. Unsaved work is still Bloom's own
// question at that quit.
#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

@interface BloomUserDriver : NSObject
// `driver` is what shows Sparkle's course to the artist; `announce` writes the answer to the
// log, for the end-to-end update test.
- (instancetype)initWithDriver:(id<SPUUserDriver>)driver announce:(BOOL)announce;
@end
