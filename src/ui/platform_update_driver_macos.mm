// The macOS driver of the platform updater (ADR 0041): Sparkle 2. Compiled only on macOS, with
// the framework's headers from the dependency prefix and with automatic reference counting.
//
// The framework is not linked. It is loaded from Bloom.app/Contents/Frameworks when the driver is
// made, and its classes are found by name, so only a packaged Bloom.app ever loads it; a test
// program and a build folder never need it.
//
// Sparkle shows its own window, downloads in its own background with its own progress and
// cancel, checks the archive's Ed25519 signature against the key in the bundle's Info.plist
// (SUPublicEDKey), installs and starts Bloom again. Bloom gives it the address of the update
// list, taken from the verified feed, and never lets it check by itself.

#include <bloom/ui/platform_update_driver.hpp>

#include <bloom/ui/update_service.hpp>
#include <bloom/update/staging.hpp>

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <QDebug>
#include <QString>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {

[[nodiscard]] NSString* toNSString(const std::string& text) {
    NSString* converted = [[NSString alloc] initWithBytes:text.data()
                                                   length:text.size()
                                                 encoding:NSUTF8StringEncoding];
    return converted != nil ? converted : @"";
}

[[nodiscard]] NSURL* toFileUrl(const std::filesystem::path& path) {
    return [NSURL fileURLWithPath:toNSString(path.string())];
}

[[nodiscard]] QString describe(NSError* error) {
    return error != nil ? QString::fromNSString(error.localizedDescription) : QString();
}

} // namespace

// What Sparkle asks Bloom while it works.
@interface BloomSparkleDelegate : NSObject <SPUUpdaterDelegate>
// The address of the update list, from the verified feed.
@property(nonatomic, copy) NSString* feed;
// The label of the version that list offers, from the verified feed.
@property(nonatomic, copy) NSString* version;
- (instancetype)initWithEnvironment:
    (std::shared_ptr<const bloom::ui::UpdateEnvironment>)environment;
@end

@implementation BloomSparkleDelegate {
    std::shared_ptr<const bloom::ui::UpdateEnvironment> _environment;
}

- (instancetype)initWithEnvironment:
    (std::shared_ptr<const bloom::ui::UpdateEnvironment>)environment {
    self = [super init];
    if (self != nil) {
        _environment = std::move(environment);
        _feed = @"";
        _version = @"";
    }
    return self;
}

- (nullable NSString*)feedURLStringForUpdater:(SPUUpdater*)updater {
    return self.feed.length > 0 ? self.feed : nil;
}

// Bloom has its own switch for the automatic check (Preferences); Sparkle never asks.
- (BOOL)updaterShouldPromptForPermissionToCheckForUpdates:(SPUUpdater*)updater {
    return NO;
}

// Before Sparkle replaces the bundle and starts Bloom again: the installed bundle is copied to
// the update folder and the helper is started to watch the new version's start, so the copy can
// be put back when the new version does not start. The copy is made off the interface thread
// (on the system's own volume format it is a clone and takes neither time nor room). When
// anything of this fails the update goes on without the helper.
- (BOOL)updater:(SPUUpdater*)updater
    shouldPostponeRelaunchForUpdate:(SUAppcastItem*)item
                 untilInvokingBlock:(void (^)(void))installHandler {
    const auto environment = _environment;
    const char* label = self.version.UTF8String;
    const std::string version = label != nullptr ? label : "";
    const auto previous =
        bloom::update::watchedPreviousVersion(environment->install, environment->folders);
    if (previous.empty() || version.empty()) {
        return NO;
    }
    NSURL* installed = toFileUrl(environment->install.root);
    NSURL* kept = toFileUrl(previous);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSFileManager* files = [[NSFileManager alloc] init];
      [files removeItemAtURL:kept error:nil];
      [files createDirectoryAtURL:[kept URLByDeletingLastPathComponent]
          withIntermediateDirectories:YES
                           attributes:nil
                                error:nil];
      NSError* error = nil;
      const BOOL copied = [files copyItemAtURL:installed toURL:kept error:&error];
      dispatch_async(dispatch_get_main_queue(), ^{
        if (!copied) {
            qWarning().noquote() << "The installed version could not be kept for going back:"
                                 << describe(error);
        } else if (!bloom::ui::startUpdateWatch(*environment, version)) {
            qWarning() << "The update helper could not be started; the update goes on "
                          "without it.";
        }
        installHandler();
      });
    });
    return YES;
}

@end

namespace bloom::ui {

namespace {

class SparkleDriver final : public PlatformUpdateDriver {
  public:
    SparkleDriver(SPUUpdater* updater, SPUStandardUserDriver* userDriver,
                  BloomSparkleDelegate* delegate)
        : updater_(updater), userDriver_(userDriver), delegate_(delegate) {}

    void showUpdate(const std::string& appcastUrl, const std::string& version) override {
        @autoreleasepool {
            delegate_.feed = toNSString(appcastUrl);
            delegate_.version = toNSString(version);
            if (!started_) {
                NSError* error = nil;
                if (![updater_ startUpdater:&error]) {
                    qWarning().noquote()
                        << "The platform updater could not be started:" << describe(error);
                    return;
                }
                started_ = true;
                // Bloom checks, at most once a day and only when its own switch is on; nothing
                // about the computer is sent, and nothing is downloaded before the artist asks.
                updater_.automaticallyChecksForUpdates = NO;
                updater_.automaticallyDownloadsUpdates = NO;
                updater_.sendsSystemProfile = NO;
            }
            [updater_ checkForUpdates];
        }
    }

  private:
    SPUUpdater* updater_ = nil;
    // The updater and the user driver do not keep these alive themselves.
    SPUStandardUserDriver* userDriver_ = nil;
    BloomSparkleDelegate* delegate_ = nil;
    bool started_ = false;
};

// Loads the bundle's own copy of the framework, and nothing from anywhere else.
[[nodiscard]] bool loadSparkle() {
    NSURL* frameworks = NSBundle.mainBundle.privateFrameworksURL;
    if (frameworks == nil) {
        return false;
    }
    NSBundle* framework =
        [NSBundle bundleWithURL:[frameworks URLByAppendingPathComponent:@"Sparkle.framework"]];
    NSError* error = nil;
    return framework != nil && [framework loadAndReturnError:&error];
}

} // namespace

std::unique_ptr<PlatformUpdateDriver>
makePlatformUpdateDriver(const UpdateEnvironment& environment, PlatformUpdateHooks /*hooks*/) {
    // Sparkle closes Bloom the way the system's Quit does, so it needs no hook of Bloom's.
    if (environment.install.kind != update::InstallKind::MacBundle ||
        !environment.configuration.configured()) {
        return nullptr;
    }
    @autoreleasepool {
        // Without the key in the bundle Sparkle would verify nothing; such a build has no driver.
        if ([NSBundle.mainBundle objectForInfoDictionaryKey:@"SUPublicEDKey"] == nil ||
            !loadSparkle()) {
            return nullptr;
        }
        Class userDriverClass = NSClassFromString(@"SPUStandardUserDriver");
        Class updaterClass = NSClassFromString(@"SPUUpdater");
        if (userDriverClass == Nil || updaterClass == Nil) {
            return nullptr;
        }
        BloomSparkleDelegate* delegate = [[BloomSparkleDelegate alloc]
            initWithEnvironment:std::make_shared<const UpdateEnvironment>(environment)];
        SPUStandardUserDriver* userDriver =
            [(SPUStandardUserDriver*)[userDriverClass alloc]
                initWithHostBundle:NSBundle.mainBundle
                          delegate:nil];
        SPUUpdater* updater = [(SPUUpdater*)[updaterClass alloc]
            initWithHostBundle:NSBundle.mainBundle
             applicationBundle:NSBundle.mainBundle
                    userDriver:userDriver
                      delegate:delegate];
        if (delegate == nil || userDriver == nil || updater == nil) {
            return nullptr;
        }
        return std::make_unique<SparkleDriver>(updater, userDriver, delegate);
    }
}

} // namespace bloom::ui
