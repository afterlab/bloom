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
#include <QGuiApplication>
#include <QString>
#include <QTimer>
#include <QtGlobal>

#include <filesystem>
#include <functional>
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
// The end-to-end update test's run (BloomUnattendedUserDriver below).
@property(nonatomic) BOOL unattended;
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

// Sparkle starts Bloom again through the system's launcher, as the person at the computer. The
// end-to-end update test runs Bloom under an identity of its own, which the launcher would not
// pass on, so in its run the test starts the new version itself.
- (BOOL)updaterShouldRelaunchApplication:(SPUUpdater*)updater {
    return !self.unattended;
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

// The end-to-end update test's stand-in for Sparkle's own window, used only by a test build that
// its environment tells to (kUpdateTestInstallNowVariable): every question is answered with
// "install", nothing is shown, and what Sparkle reports goes to the log. Such a run may have no
// window system, and then nothing delivers the system's Quit: Bloom is closed here when Sparkle
// says it is waiting for that.
@interface BloomUnattendedUserDriver : NSObject <SPUUserDriver>
- (instancetype)initWithClose:(std::function<void()>)close;
@end

@implementation BloomUnattendedUserDriver {
    std::function<void()> _close;
}

- (instancetype)initWithClose:(std::function<void()>)close {
    self = [super init];
    if (self != nil) {
        _close = std::move(close);
    }
    return self;
}

- (void)closeBloom:(const char*)reason {
    qWarning().noquote() << "Unattended update:" << reason;
    if (_close) {
        _close();
    }
}

- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest*)request
                              reply:(void (^)(SUUpdatePermissionResponse*))reply {
    // Never asked (the delegate says so); answered all the same, with everything off.
    Class responseClass = NSClassFromString(@"SUUpdatePermissionResponse");
    if (responseClass != Nil) {
        SUUpdatePermissionResponse* response = [responseClass alloc];
        reply([response initWithAutomaticUpdateChecks:NO sendSystemProfile:NO]);
    }
}

- (void)showUserInitiatedUpdateCheckWithCancellation:(void (^)(void))cancellation {
}

- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem*)appcastItem
                                 state:(SPUUserUpdateState*)state
                                 reply:(void (^)(SPUUserUpdateChoice))reply {
    qWarning() << "Unattended update: the platform updater found the update";
    reply(SPUUserUpdateChoiceInstall);
}

- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData*)downloadData {
}

- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError*)error {
}

- (void)showUpdateNotFoundWithError:(NSError*)error
                    acknowledgement:(void (^)(void))acknowledgement {
    qWarning().noquote() << "The platform updater found no update:" << describe(error);
    acknowledgement();
}

- (void)showUpdaterError:(NSError*)error acknowledgement:(void (^)(void))acknowledgement {
    qWarning().noquote() << "The platform updater reported:" << describe(error);
    acknowledgement();
}

- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation {
}

- (void)showDownloadDidReceiveExpectedContentLength:(uint64_t)expectedContentLength {
}

- (void)showDownloadDidReceiveDataOfLength:(uint64_t)length {
}

- (void)showDownloadDidStartExtractingUpdate {
    qWarning() << "Unattended update: the platform updater downloaded the update";
}

- (void)showExtractionReceivedProgress:(double)progress {
}

- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply {
    qWarning() << "Unattended update: the platform updater is ready to install";
    reply(SPUUserUpdateChoiceInstall);
}

- (void)showSendingTerminationSignal {
    [self closeBloom:"the platform updater asks Bloom to close"];
}

- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)applicationTerminated
                          retryTerminatingApplication:(void (^)(void))retryTerminatingApplication {
    if (!applicationTerminated) {
        [self closeBloom:"the platform updater waits for Bloom to close"];
    }
}

- (void)showUpdateInstalledAndRelaunched:(BOOL)relaunched
                         acknowledgement:(void (^)(void))acknowledgement {
    acknowledgement();
}

- (void)dismissUpdateInstallation {
}

@end

namespace bloom::ui {

namespace {

class SparkleDriver final : public PlatformUpdateDriver {
  public:
    SparkleDriver(SPUUpdater* updater, id<SPUUserDriver> userDriver,
                  BloomSparkleDelegate* delegate, const bool turnMainRunLoop)
        : updater_(updater), userDriver_(userDriver), delegate_(delegate) {
        if (turnMainRunLoop) {
            // Sparkle works through the main thread's run loop and the main queue. Qt turns
            // that loop itself on the system's own window platform; on another platform (the
            // end-to-end update test runs without a window) nothing does, so it is turned here.
            QObject::connect(&runLoop_, &QTimer::timeout, &runLoop_, [] {
                (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
            });
            runLoop_.start(20);
        }
    }

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
    // The updater does not keep these alive itself. The user driver is only held.
    [[maybe_unused]] id<SPUUserDriver> userDriver_ = nil;
    BloomSparkleDelegate* delegate_ = nil;
    bool started_ = false;
    QTimer runLoop_;
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
makePlatformUpdateDriver(const UpdateEnvironment& environment, PlatformUpdateHooks hooks) {
    // Sparkle closes Bloom the way the system's Quit does, so it needs no hook of Bloom's; only
    // the end-to-end update test's run, which may have no window system, uses one.
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
        // A test build that the end-to-end update test started installs without a question.
        id<SPUUserDriver> userDriver = nil;
        const bool unattended = environment.configuration.testBuild &&
                                qEnvironmentVariableIsSet(kUpdateTestInstallNowVariable);
        if (unattended) {
            userDriver = [[BloomUnattendedUserDriver alloc] initWithClose:hooks.requestClose];
            delegate.unattended = YES;
        } else {
            // The classes are found by name, so each is given its type before it is made.
            SPUStandardUserDriver* standard = [userDriverClass alloc];
            userDriver = [standard initWithHostBundle:NSBundle.mainBundle delegate:nil];
        }
        SPUUpdater* made = [updaterClass alloc];
        SPUUpdater* updater = [made initWithHostBundle:NSBundle.mainBundle
                                     applicationBundle:NSBundle.mainBundle
                                            userDriver:userDriver
                                              delegate:delegate];
        if (delegate == nil || userDriver == nil || updater == nil) {
            return nullptr;
        }
        const bool turnMainRunLoop =
            unattended && QGuiApplication::platformName() != QLatin1String("cocoa");
        return std::make_unique<SparkleDriver>(updater, userDriver, delegate, turnMainRunLoop);
    }
}

} // namespace bloom::ui
