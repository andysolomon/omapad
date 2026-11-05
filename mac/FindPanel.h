// The Find / Replace / Find in Files window.
#pragma once
#import <Cocoa/Cocoa.h>
#include "Workspace.h"

@interface FindPanel : NSObject
- (instancetype)initWithWorkspace:(omapad::Workspace *)workspace;
- (void)showTab:(omapad::FindTab)tab prefill:(NSString *)text;
- (omapad::FindOptions)currentOptions;
- (void)setStatus:(NSString *)status isError:(BOOL)isError;
- (void)setDirectory:(NSString *)directory;
- (void)setFindText:(NSString *)text;
- (void)setInSelection:(BOOL)on;
- (void)setFindInFilesRunning:(BOOL)running;
- (void)close;
@end
