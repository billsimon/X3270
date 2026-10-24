#import <AppKit/AppKit.h>
#import "AppDelegate.h"
#include <csignal>

int main(int argc, const char *argv[]) {
    // Writes to a socket the host has closed must return EPIPE rather than
    // terminate the app (sockets also set SO_NOSIGPIPE; this covers the rest).
    signal(SIGPIPE, SIG_IGN);

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];

        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
