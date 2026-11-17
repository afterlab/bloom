// Bloom's user driver between Sparkle and its window (platform_update_user_driver_macos.h).
// Compiled only on macOS, with automatic reference counting.

#import "platform_update_user_driver_macos.h"

#include <QDebug>

@implementation BloomUserDriver {
    id<SPUUserDriver> _driver;
    BOOL _announce;
}

- (instancetype)initWithDriver:(id<SPUUserDriver>)driver announce:(BOOL)announce {
    self = [super init];
    if (self != nil) {
        _driver = driver;
        _announce = announce;
    }
    return self;
}

- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector] || [_driver respondsToSelector:selector];
}

- (id)forwardingTargetForSelector:(SEL)selector {
    return _driver;
}

- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply {
    if (_announce) {
        qWarning() << "Unattended update: the platform updater is ready to install";
    }
    reply(SPUUserUpdateChoiceInstall);
}

@end
