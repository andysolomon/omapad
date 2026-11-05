// The Function List side panel.
#pragma once
#import <Cocoa/Cocoa.h>
#include "Workspace.h"

@interface FunctionPanel : NSView
- (instancetype)initWithWorkspace:(omapad::Workspace *)workspace;
- (NSView *)focusTarget;
- (void)reload;  // Shows the workspace's current list, keeping the filter and what's collapsed.
@end
