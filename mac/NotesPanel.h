#import <Cocoa/Cocoa.h>
#include "Notes.h"
#include "Workspace.h"

// Native writing workspace. Its source view is one of AppController's existing panes.
@interface NotesPanel : NSView
- (instancetype)initWithWorkspace:(omapad::Workspace *)workspace;
- (void)updateState:(const omapad::NotesState &)state;
- (void)setSourceView:(NSView *)view;
- (NSView *)detachSourceView;
- (void)focusPreview;
@end
