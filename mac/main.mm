#import <Cocoa/Cocoa.h>
#import "AppController.h"
#include "Workspace.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, const char *argv[]) {
	@autoreleasepool {
		std::vector<std::string> incoming;
		NSArray *arguments = NSProcessInfo.processInfo.arguments;
		for (NSUInteger i = 1; i < arguments.count; i++) {
			NSString *argument = arguments[i];
			if ([argument hasPrefix:@"-Apple"] || [argument hasPrefix:@"-NS"] || [argument hasPrefix:@"-psn"]) {
				i++;
				continue;
			}
			incoming.push_back([argument UTF8String] ?: "");
		}
		const omapad::CommandLine line = omapad::parseCommandLine(incoming);
		if (line.help) {
			fputs(omapad::commandLineHelp(), stdout);
			return 0;
		}
		omapad::setLaunchOptions(line);
		NSApplication *app = NSApplication.sharedApplication;
		NSString *home = NSProcessInfo.processInfo.environment[@"OMAPAD_SUPPORT_DIR"];
		NSString *support = home ?: [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject
			stringByAppendingPathComponent:@"omapad"];
		std::string settingsDirectory = support.UTF8String, stateDirectory = settingsDirectory;
		omapad::resolveConfigDirectories(settingsDirectory, stateDirectory);
		support = [NSString stringWithUTF8String:settingsDirectory.c_str()];
		AppController *controller = [[AppController alloc] initWithResourceDirectory:NSBundle.mainBundle.resourcePath
		                                                            supportDirectory:support];
		app.delegate = controller;
		[app run];
	}
	return 0;
}
