#import "FindPanel.h"

@interface FindPanel () <NSWindowDelegate>
@end

@implementation FindPanel {
	NSPanel *_panel;
	NSSegmentedControl *_tabs;
	NSComboBox *_what;
	NSComboBox *_with;
	NSTextField *_filters;
	NSTextField *_directory;
	NSButton *_matchCase, *_wholeWord, *_wrap, *_backward, *_inSelection, *_recursive, *_hidden, *_dotNewline;
	NSPopUpButton *_mode;
	NSTextField *_status;
	NSView *_withRow, *_filesRow, *_directoryRow, *_filesOptionsRow, *_inSelectionRow;
	NSView *_findButtons, *_replaceButtons, *_filesButtons;
	NSButton *_findInFilesButton, *_stopButton;
	NSMutableArray<NSString *> *_whatHistory;
	NSMutableArray<NSString *> *_withHistory;
	omapad::Workspace *_workspace;
}

- (NSButton *)checkbox:(NSString *)title {
	NSButton *b = [NSButton checkboxWithTitle:title target:nil action:nil];
	b.font = [NSFont systemFontOfSize:12];
	return b;
}

- (NSButton *)button:(NSString *)title action:(SEL)action {
	NSButton *b = [NSButton buttonWithTitle:title target:self action:action];
	b.controlSize = NSControlSizeRegular;
	return b;
}

- (NSStackView *)row:(NSArray<NSView *> *)views {
	NSStackView *row = [NSStackView stackViewWithViews:views];
	row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
	row.alignment = NSLayoutAttributeCenterY;
	row.spacing = 8;
	return row;
}

- (NSView *)labelled:(NSString *)label control:(NSView *)control {
	NSTextField *text = [NSTextField labelWithString:label];
	text.alignment = NSTextAlignmentRight;
	[text.widthAnchor constraintEqualToConstant:72].active = YES;
	[control setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
	return [self row:@[text, control]];
}

- (NSComboBox *)comboBox {
	NSComboBox *box = [NSComboBox new];
	box.completes = NO;
	box.numberOfVisibleItems = 10;
	box.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
	[box.widthAnchor constraintGreaterThanOrEqualToConstant:380].active = YES;
	return box;
}

- (instancetype)initWithWorkspace:(omapad::Workspace *)workspace {
	if ((self = [super init])) {
		_workspace = workspace;
		_whatHistory = [NSMutableArray array];
		_withHistory = [NSMutableArray array];
		[self buildPanel];
	}
	return self;
}

- (void)buildPanel {
	_panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 560, 300)
	                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow | NSWindowStyleMaskResizable
	                                      backing:NSBackingStoreBuffered
	                                        defer:YES];
	_panel.title = @"Find";
	_panel.floatingPanel = YES;
	_panel.becomesKeyOnlyIfNeeded = NO;
	_panel.hidesOnDeactivate = YES;
	_panel.releasedWhenClosed = NO;
	_panel.delegate = self;
	_panel.frameAutosaveName = @"OmapadFindPanel";

	_tabs = [NSSegmentedControl segmentedControlWithLabels:@[@"Find", @"Replace", @"Find in Files"]
	                                          trackingMode:NSSegmentSwitchTrackingSelectOne
	                                                target:self
	                                                action:@selector(tabChanged:)];
	_tabs.selectedSegment = 0;

	_what = [self comboBox];
	_what.target = self;
	_what.action = @selector(whatEntered:);
	_with = [self comboBox];
	_filters = [NSTextField textFieldWithString:@"*"];
	_filters.placeholderString = @"*.txt *.py (space separated, * for all)";
	_directory = [NSTextField textFieldWithString:NSHomeDirectory()];
	NSButton *browse = [self button:@"Choose…" action:@selector(browse:)];

	_matchCase = [self checkbox:@"Match case"];
	_wholeWord = [self checkbox:@"Whole word"];
	_wrap = [self checkbox:@"Wrap around"];
	_wrap.state = NSControlStateValueOn;
	_backward = [self checkbox:@"Backward"];
	_inSelection = [self checkbox:@"In selection"];
	_recursive = [self checkbox:@"In subfolders"];
	_recursive.state = NSControlStateValueOn;
	_hidden = [self checkbox:@"In hidden folders"];
	_mode = [NSPopUpButton new];
	[_mode addItemsWithTitles:@[@"Normal", @"Extended (\\n, \\r, \\t, \\x…)", @"Regular expression"]];
	_mode.target = self;
	_mode.action = @selector(modeChanged:);
	_dotNewline = [self checkbox:@". matches newline"];
	_dotNewline.enabled = NO;

	NSView *whatRow = [self labelled:@"Find:" control:_what];
	_withRow = [self labelled:@"Replace:" control:_with];
	_filesRow = [self labelled:@"Filters:" control:_filters];
	_directoryRow = [self row:@[[self labelled:@"Folder:" control:_directory], browse]];
	NSView *modeRow = [self row:@[[self labelled:@"Mode:" control:_mode], _dotNewline]];
	NSView *optionsRow = [self row:@[_matchCase, _wholeWord, _wrap, _backward]];
	_inSelectionRow = [self row:@[_inSelection]];
	_filesOptionsRow = [self row:@[_recursive, _hidden]];

	_findButtons = [self row:@[
		[self button:@"Find Next" action:@selector(findNext:)],
		[self button:@"Count" action:@selector(count:)],
		[self button:@"Find All" action:@selector(findAll:)],
		[self button:@"Mark All" action:@selector(markAll:)],
		[self button:@"Clear Marks" action:@selector(clearMarks:)],
	]];
	_replaceButtons = [self row:@[
		[self button:@"Find Next" action:@selector(findNext:)],
		[self button:@"Replace" action:@selector(replace:)],
		[self button:@"Replace All" action:@selector(replaceAll:)],
		[self button:@"In All Open Documents" action:@selector(replaceAllOpen:)],
	]];
	_findInFilesButton = [self button:@"Find All" action:@selector(findInFiles:)];
	_stopButton = [self button:@"Stop" action:@selector(stop:)];
	_stopButton.enabled = NO;
	_filesButtons = [self row:@[_findInFilesButton, _stopButton]];

	_status = [NSTextField wrappingLabelWithString:@""];
	_status.font = [NSFont systemFontOfSize:11];
	_status.textColor = NSColor.secondaryLabelColor;

	NSStackView *stack = [NSStackView stackViewWithViews:@[
		_tabs, whatRow, _withRow, _filesRow, _directoryRow, modeRow, optionsRow, _inSelectionRow, _filesOptionsRow,
		_findButtons, _replaceButtons, _filesButtons, _status,
	]];
	stack.orientation = NSUserInterfaceLayoutOrientationVertical;
	stack.alignment = NSLayoutAttributeLeading;
	stack.spacing = 8;
	stack.edgeInsets = NSEdgeInsetsMake(12, 14, 12, 14);
	stack.detachesHiddenViews = YES;
	stack.translatesAutoresizingMaskIntoConstraints = NO;
	[_panel.contentView addSubview:stack];
	NSView *content = _panel.contentView;
	[NSLayoutConstraint activateConstraints:@[
		[stack.topAnchor constraintEqualToAnchor:content.topAnchor],
		[stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
		[stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
		[stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
		[whatRow.trailingAnchor constraintEqualToAnchor:stack.trailingAnchor constant:-14],
		[_withRow.trailingAnchor constraintEqualToAnchor:stack.trailingAnchor constant:-14],
		[_filesRow.trailingAnchor constraintEqualToAnchor:stack.trailingAnchor constant:-14],
		[_directoryRow.trailingAnchor constraintEqualToAnchor:stack.trailingAnchor constant:-14],
		[_status.trailingAnchor constraintEqualToAnchor:stack.trailingAnchor constant:-14],
	]];
	_panel.initialFirstResponder = _what;
	[self applyTab];
}

- (void)applyTab {
	const NSInteger tab = _tabs.selectedSegment;
	_withRow.hidden = tab != static_cast<NSInteger>(omapad::FindTab::Replace);
	_filesRow.hidden = tab != static_cast<NSInteger>(omapad::FindTab::Files);
	_directoryRow.hidden = tab != static_cast<NSInteger>(omapad::FindTab::Files);
	_filesOptionsRow.hidden = tab != static_cast<NSInteger>(omapad::FindTab::Files);
	_inSelectionRow.hidden = tab == static_cast<NSInteger>(omapad::FindTab::Files);
	_findButtons.hidden = tab != static_cast<NSInteger>(omapad::FindTab::Find);
	_replaceButtons.hidden = tab != static_cast<NSInteger>(omapad::FindTab::Replace);
	_filesButtons.hidden = tab != static_cast<NSInteger>(omapad::FindTab::Files);
	_backward.hidden = tab == static_cast<NSInteger>(omapad::FindTab::Files);
	_wrap.hidden = tab == static_cast<NSInteger>(omapad::FindTab::Files);
	_panel.title = @[@"Find", @"Replace", @"Find in Files"][tab];
	[_panel.contentView layoutSubtreeIfNeeded];
	NSSize fitting = _panel.contentView.fittingSize;
	NSRect frame = [_panel frameRectForContentRect:NSMakeRect(0, 0, MAX(NSWidth(_panel.contentView.bounds), fitting.width), fitting.height)];
	frame.origin = NSMakePoint(NSMinX(_panel.frame), NSMaxY(_panel.frame) - NSHeight(frame));
	[_panel setFrame:frame display:YES];
}

- (void)tabChanged:(id)sender {
	[self applyTab];
	[self setStatus:@"" isError:NO];
}

- (void)showTab:(omapad::FindTab)tab prefill:(NSString *)text {
	_tabs.selectedSegment = static_cast<NSInteger>(tab);
	[self applyTab];
	if (text.length && [text rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location == NSNotFound)
		_what.stringValue = text;
	if (!_panel.isVisible) [_panel center];
	[_panel makeKeyAndOrderFront:nil];
	[_panel makeFirstResponder:_what];
	[self setStatus:@"" isError:NO];
}

- (void)setFindText:(NSString *)text {
	_what.stringValue = text ?: @"";
}

// Return in the Find field runs whatever the current tab is for.
- (void)whatEntered:(id)sender {
	if (_tabs.selectedSegment == static_cast<NSInteger>(omapad::FindTab::Files)) [self findInFiles:sender];
	else [self findNext:sender];
}

- (void)setDirectory:(NSString *)directory {
	if (directory.length) _directory.stringValue = directory;
}

- (void)modeChanged:(id)sender { _dotNewline.enabled = _mode.indexOfSelectedItem == 2; }

- (omapad::FindOptions)currentOptions {
	omapad::FindOptions o;
	o.what = _what.stringValue.UTF8String ?: "";
	o.with = _with.stringValue.UTF8String ?: "";
	o.matchCase = _matchCase.state == NSControlStateValueOn;
	o.wholeWord = _wholeWord.state == NSControlStateValueOn;
	o.wrap = _wrap.state == NSControlStateValueOn;
	o.backward = _backward.state == NSControlStateValueOn;
	o.inSelection = _inSelection.state == NSControlStateValueOn;
	o.mode = static_cast<omapad::SearchMode>(_mode.indexOfSelectedItem);
	o.dotMatchesNewline = _dotNewline.state == NSControlStateValueOn;
	o.directory = _directory.stringValue.stringByExpandingTildeInPath.UTF8String ?: "";
	o.filters = _filters.stringValue.UTF8String ?: "*";
	o.recursive = _recursive.state == NSControlStateValueOn;
	o.includeHidden = _hidden.state == NSControlStateValueOn;
	return o;
}

- (void)remember:(NSString *)value in:(NSMutableArray<NSString *> *)history box:(NSComboBox *)box {
	if (!value.length) return;
	[history removeObject:value];
	[history insertObject:value atIndex:0];
	while (history.count > 20) [history removeLastObject];
	[box removeAllItems];
	[box addItemsWithObjectValues:history];
}

- (void)rememberHistory {
	[self remember:_what.stringValue in:_whatHistory box:_what];
	[self remember:_with.stringValue in:_withHistory box:_with];
}

- (void)setStatus:(NSString *)status isError:(BOOL)isError {
	_status.stringValue = status ?: @"";
	_status.textColor = isError ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
}

- (void)setFindInFilesRunning:(BOOL)running {
	_findInFilesButton.enabled = !running;
	_stopButton.enabled = running;
}

- (void)findNext:(id)sender { [self rememberHistory]; _workspace->findPanelFindNext(); }
- (void)count:(id)sender { [self rememberHistory]; _workspace->findPanelCount(); }
- (void)findAll:(id)sender { [self rememberHistory]; _workspace->findPanelFindAll(); }
- (void)replace:(id)sender { [self rememberHistory]; _workspace->findPanelReplace(); }
- (void)replaceAll:(id)sender { [self rememberHistory]; _workspace->findPanelReplaceAll(); }
- (void)replaceAllOpen:(id)sender { [self rememberHistory]; _workspace->findPanelReplaceAllOpen(); }
- (void)markAll:(id)sender { [self rememberHistory]; _workspace->findPanelMarkAll(); }
- (void)clearMarks:(id)sender { _workspace->findPanelClearMarks(); }
- (void)findInFiles:(id)sender { [self rememberHistory]; _workspace->findPanelFindInFiles(); }
- (void)stop:(id)sender { _workspace->stopFindInFiles(); }

- (void)browse:(id)sender {
	NSOpenPanel *open = [NSOpenPanel openPanel];
	open.canChooseDirectories = YES;
	open.canChooseFiles = NO;
	open.directoryURL = [NSURL fileURLWithPath:_directory.stringValue];
	if ([open runModal] == NSModalResponseOK) _directory.stringValue = open.URL.path;
}

@end
