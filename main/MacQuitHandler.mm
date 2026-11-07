#include "MacQuitHandler.hpp"
#include <QCoreApplication>
#include <QEvent>
#import <AppKit/AppKit.h>

// Qt's Cocoa delegate deliberately returns NSTerminateCancel even on accepted
// quits so AppKit does not call exit(). Handle the quit Apple event directly:
// return its reply normally, then let Qt unwind its event loop back to main().
@interface NkrQuitHandler : NSObject
- (void)handleQuit:(NSAppleEventDescriptor *)event
        withReply:(NSAppleEventDescriptor *)reply;
- (void)install:(NSNotification *)notification;
@end

@implementation NkrQuitHandler
- (void)install:(NSNotification *)notification {
    Q_UNUSED(notification);
    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:self andSelector:@selector(handleQuit:withReply:)
        forEventClass:kCoreEventClass andEventID:kAEQuitApplication];
}
- (void)handleQuit:(NSAppleEventDescriptor *)event
        withReply:(NSAppleEventDescriptor *)reply {
    Q_UNUSED(event);
    QEvent quit(QEvent::Quit);
    QCoreApplication::sendEvent(QCoreApplication::instance(), &quit);
    if (!quit.isAccepted()) {
        [reply setParamDescriptor:[NSAppleEventDescriptor descriptorWithInt32:userCanceledErr]
                      forKeyword:keyErrorNumber];
    }
}
@end

void InstallMacQuitHandler() {
    auto *handler = [[NkrQuitHandler alloc] init];
    // AppKit installs its own handlers during launch, after main() setup.
    [[NSNotificationCenter defaultCenter]
        addObserver:handler selector:@selector(install:)
        name:NSApplicationDidFinishLaunchingNotification object:nil];
    [handler install:nil];
    QObject::connect(QCoreApplication::instance(), &QObject::destroyed, [handler] {
        [[NSNotificationCenter defaultCenter] removeObserver:handler];
        [[NSAppleEventManager sharedAppleEventManager]
            removeEventHandlerForEventClass:kCoreEventClass andEventID:kAEQuitApplication];
        [handler release];
    });
}
