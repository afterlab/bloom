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
//
// Sparkle's installer asks the system's launcher whether Bloom runs, asks it to quit through
// the launcher, and replaces the bundle once the launcher says it is gone. A Bloom on another
// window platform than the system's own is not an application the launcher knows, so Sparkle
// would take it for closed and replace the bundle under it, without the copy that is kept for
// going back. Such a run has no driver.

#include <bloom/ui/platform_update_driver.hpp>

#include <bloom/ui/update_service.hpp>
#include <bloom/update/staging.hpp>

#import "platform_update_user_driver_macos.h"

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <QDebug>
#include <QGuiApplication>
#include <QString>
#include <QtGlobal>

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

// The end-to-end update test's stand-in for Sparkle's own window, used only by a test build that
// its environment tells to (kUpdateTestInstallNowVariable): the offer is answered with
// "install", nothing is shown, and what Sparkle reports goes to the log. Everything else is
// Sparkle's own course: it asks Bloom to quit the way the system does, replaces the bundle and
// starts the new version through the system's launcher.
@interface BloomUnattendedUserDriver : NSObject <SPUUserDriver>
@end

@implementation BloomUnattendedUserDriver

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

// The protocol asks for it; BloomUserDriver answers this question before it gets here.
- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply {
    reply(SPUUserUpdateChoiceInstall);
}

- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)applicationTerminated
                          retryTerminatingApplication:(void (^)(void))retryTerminatingApplication {
    qWarning() << "Unattended update: the platform updater installs;"
               << (applicationTerminated ? "it takes Bloom for closed"
                                         : "it has asked Bloom to quit");
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
    // The updater does not keep these alive itself. The user driver is only held.
    [[maybe_unused]] id<SPUUserDriver> userDriver_ = nil;
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
    // Only an application the system's launcher knows (the note at the top of this file).
    if (QGuiApplication::platformName() != QLatin1String("cocoa")) {
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
        id<SPUUserDriver> shown = nil;
        const bool unattended = environment.configuration.testBuild &&
                                qEnvironmentVariableIsSet(kUpdateTestInstallNowVariable);
        if (unattended) {
            shown = [[BloomUnattendedUserDriver alloc] init];
        } else {
            // The classes are found by name, so each is given its type before it is made.
            SPUStandardUserDriver* standard = [userDriverClass alloc];
            shown = [standard initWithHostBundle:NSBundle.mainBundle delegate:nil];
        }
        if (shown == nil) {
            return nullptr;
        }
        // It passes every call on, so it stands for the protocol without declaring each one.
        id between = [[BloomUserDriver alloc] initWithDriver:shown announce:unattended ? YES : NO];
        id<SPUUserDriver> userDriver = between;
        SPUUpdater* made = [updaterClass alloc];
        SPUUpdater* updater = [made initWithHostBundle:NSBundle.mainBundle
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
