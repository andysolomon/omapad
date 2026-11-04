#import <Cocoa/Cocoa.h>
#import "AppController.h"

int main(int argc, const char *argv[]) {
	@autoreleasepool {
		NSApplication *app = NSApplication.sharedApplication;
		NSString *home = NSProcessInfo.processInfo.environment[@"OMAPAD_SUPPORT_DIR"];
		NSString *support = home ?: [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject
			stringByAppendingPathComponent:@"omapad"];
		AppController *controller = [[AppController alloc] initWithResourceDirectory:NSBundle.mainBundle.resourcePath
		                                                            supportDirectory:support];
		app.delegate = controller;
		[app run];
	}
	return 0;
}
