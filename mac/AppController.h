// The Mac window around the shared Workspace.
#pragma once
#import <Cocoa/Cocoa.h>

@interface AppController : NSObject <NSApplicationDelegate>
- (instancetype)initWithResourceDirectory:(NSString *)resources supportDirectory:(NSString *)support;
@end
