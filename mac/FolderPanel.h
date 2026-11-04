// The Folder as Workspace side panel.
#pragma once
#import <Cocoa/Cocoa.h>
#include "Workspace.h"

@interface FolderPanel : NSView
- (instancetype)initWithWorkspace:(omapad::Workspace *)workspace;
@property(nonatomic, copy) NSMenu * (^menuBuilder)(std::vector<omapad::MenuItem> items);
- (void)reload;  // Reads the folders again, keeping what's expanded and selected.
@end
