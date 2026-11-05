#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "AppController.h"
#import "Dialogs.h"
#import "FindPanel.h"
#import "FolderPanel.h"
#import "FunctionPanel.h"
#import "NotesPanel.h"
#import "TabBar.h"
#import "ScintillaView.h"
#import "Scintilla.h"

#include "Workspace.h"

using namespace omapad;

static NSString *toNS(const std::string &s) {
	return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

static std::string toStd(NSString *s) { return s.UTF8String ?: ""; }

#pragma mark - Small helpers

@interface OmaSciDelegate : NSObject <ScintillaNotificationProtocol>
@property(nonatomic, copy) void (^handler)(SCNotification *);
@end
@implementation OmaSciDelegate
- (void)notification:(SCNotification *)notification {
	if (self.handler) self.handler(notification);
}
@end

// Dropping files on the editor opens them. Dropping text still inserts it.
@interface OmaContentView : SCIContentView
@end
@implementation OmaContentView
static void (^sFileDropHandler)(NSArray<NSString *> *);
- (NSArray<NSString *> *)filesIn:(id<NSDraggingInfo>)info {
	NSArray *urls = [info.draggingPasteboard readObjectsForClasses:@[NSURL.class]
	                                                       options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
	NSMutableArray *paths = [NSMutableArray array];
	for (NSURL *url in urls) if (url.path) [paths addObject:url.path];
	return paths;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
	return [self filesIn:sender].count ? NSDragOperationCopy : [super draggingEntered:sender];
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
	return [self filesIn:sender].count ? NSDragOperationCopy : [super draggingUpdated:sender];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
	NSArray *files = [self filesIn:sender];
	if (files.count) {
		if (sFileDropHandler) sFileDropHandler(files);
		return YES;
	}
	return [super performDragOperation:sender];
}
@end

@interface OmaEditorView : ScintillaView
@property(nonatomic, copy) NSMenu *(^contextMenu)(NSEvent *event);
@end
@implementation OmaEditorView
+ (Class)contentViewClass { return OmaContentView.class; }
- (NSMenu *)menuForEvent:(NSEvent *)event { return self.contextMenu ? self.contextMenu(event) : nil; }
@end

// Reports system light and dark changes, so an automatic theme can follow them.
@interface OmaRootView : NSView
@property(nonatomic, copy) void (^appearanceChanged)(void);
@end
@implementation OmaRootView
- (void)viewDidChangeEffectiveAppearance {
	[super viewDidChangeEffectiveAppearance];
	if (self.appearanceChanged) self.appearanceChanged();
}
@end

// Sits over the Document Map and takes its mouse input, so the map itself never gets focus
// or a selection. Presses jump the editor there, and scrolling scrolls the editor.
@interface OmaMapOverlay : NSView
@property(nonatomic, copy) void (^pressed)(NSPoint point);
@property(nonatomic, copy) void (^scrolled)(CGFloat lines);
@end
@implementation OmaMapOverlay {
	CGFloat _pendingLines;
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (void)report:(NSEvent *)event {
	if (self.pressed) self.pressed([self convertPoint:event.locationInWindow fromView:nil]);
}
- (void)mouseDown:(NSEvent *)event { [self report:event]; }
- (void)mouseDragged:(NSEvent *)event { [self report:event]; }
- (void)scrollWheel:(NSEvent *)event {
	// Trackpads send fractions of a line, so they're saved up until there's a whole one.
	_pendingLines -= event.hasPreciseScrollingDeltas ? event.scrollingDeltaY / 16 : event.scrollingDeltaY;
	const CGFloat whole = trunc(_pendingLines);
	_pendingLines -= whole;
	if (whole != 0 && self.scrolled) self.scrolled(whole);
}
@end

// What a menu item does, carried on the item itself.
@interface OmaMenuAction : NSObject {
@public
	MenuItem item;
}
@end
@implementation OmaMenuAction
@end

struct MacSciView : SciView {
	__unsafe_unretained ScintillaView *view = nil;
	intptr_t send(unsigned message, uintptr_t w, intptr_t l) override { return [view message:message wParam:w lParam:l]; }
};

@interface AppController () <TabBarDelegate, NSWindowDelegate, NSMenuDelegate>
- (void)hostTabsChanged;
- (void)hostNotesChanged:(const NotesState &)state;
- (void)hostSplitChanged:(BOOL)visible stacked:(BOOL)stacked;
- (void)hostResultsVisible:(BOOL)visible;
- (void)hostFolderVisible:(BOOL)visible;
- (void)hostFunctionListVisible:(BOOL)visible;
- (void)hostFunctionListChanged;
- (void)hostDocumentMapVisible:(BOOL)visible;
- (void)hostPanel:(int)panel title:(NSString *)title visible:(BOOL)visible;
- (void)hostPanelChanged:(int)panel;
- (void)hostIncrementalBar:(BOOL)visible;
- (void)hostChrome:(const ChromeState &)chrome;
- (void)hostTabStyle:(const TabStyle &)style;
- (void)hostFocusPanel:(int)panel;
- (void)hostSelectFolderPath:(NSString *)path;
- (void)hostSetTray:(BOOL)visible;
- (void)hostSetWindowX:(long)x y:(long)y;
- (void)buildMenus;
- (Workspace *)workspacePointer;
- (void)hostFoldersChanged;
- (void)hostFocusPane:(int)pane;
- (void)hostThemeChanged;
- (void)hostStatusLeft:(NSString *)left right:(NSString *)right;
- (void)hostFlash:(NSString *)message;
- (void)hostFullScreen;
- (void)hostAlwaysOnTop:(BOOL)on;
- (BOOL)hostSystemIsDark;
- (void)hostChooseFont;
@property(nonatomic, readonly) FindPanel *findPanel;
@end

#pragma mark - Host

static NSColor *colourFromHexString(const std::string &hex) {
	if (hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return nil;
	const unsigned long rgb = std::strtoul(hex.c_str(), nullptr, 16);
	return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0 green:((rgb >> 8) & 0xFF) / 255.0 blue:(rgb & 0xFF) / 255.0 alpha:1];
}

struct MacHost : Host {
	__unsafe_unretained AppController *controller = nil;
	MacSciView panes[2], results, background, map, printer;

	SciView &paneView(int pane) override { return panes[pane]; }
	SciView &resultsView() override { return results; }
	SciView &backgroundView() override { return background; }
	SciView &printView() override { return printer; }
	SciView &mapView() override { return map; }

	int ask(const std::string &title, const std::string &detail, const std::vector<std::string> &buttons) override {
		NSAlert *alert = [NSAlert new];
		alert.messageText = toNS(title);
		alert.informativeText = toNS(detail);
		for (const std::string &button : buttons) [alert addButtonWithTitle:toNS(button)];
		return static_cast<int>([alert runModal] - NSAlertFirstButtonReturn);
	}
	bool chooseFilesToOpen(const std::string &start, std::vector<std::string> &out) override {
		NSOpenPanel *panel = [NSOpenPanel openPanel];
		panel.allowsMultipleSelection = YES;
		panel.canChooseDirectories = NO;
		if (!start.empty()) panel.directoryURL = [NSURL fileURLWithPath:toNS(start)];
		if ([panel runModal] != NSModalResponseOK) return false;
		for (NSURL *url in panel.URLs) out.push_back(toStd(url.path));
		return true;
	}
	bool chooseSavePath(const std::string &name, const std::string &start, std::string &out) override {
		NSSavePanel *panel = [NSSavePanel savePanel];
		panel.nameFieldStringValue = toNS(name);
		if (!start.empty()) panel.directoryURL = [NSURL fileURLWithPath:toNS(start)];
		panel.allowsOtherFileTypes = YES;
		panel.extensionHidden = NO;
		if ([panel runModal] != NSModalResponseOK) return false;
		out = toStd(panel.URL.path);
		return true;
	}
	bool chooseFolder(const std::string &start, std::string &out) override {
		NSOpenPanel *panel = [NSOpenPanel openPanel];
		panel.canChooseDirectories = YES;
		panel.canChooseFiles = NO;
		panel.prompt = @"Open Folder";
		if (!start.empty()) panel.directoryURL = [NSURL fileURLWithPath:toNS(start)];
		if ([panel runModal] != NSModalResponseOK) return false;
		out = toStd(panel.URL.path);
		return true;
	}
	bool chooseFont(std::string &, double &) override {
		// The Mac font panel stays open and reports changes as they happen.
		[controller hostChooseFont];
		return false;
	}
	bool promptText(const std::string &title, const std::string &label, const std::string &initial, std::string &out) override {
		NSAlert *alert = [NSAlert new];
		alert.messageText = toNS(title);
		alert.informativeText = toNS(label);
		[alert addButtonWithTitle:@"OK"];
		[alert addButtonWithTitle:@"Cancel"];
		NSTextField *field = [NSTextField textFieldWithString:toNS(initial)];
		field.frame = NSMakeRect(0, 0, 280, 24);
		alert.accessoryView = field;
		alert.window.initialFirstResponder = field;
		if ([alert runModal] != NSAlertFirstButtonReturn) return false;
		out = toStd(field.stringValue);
		return true;
	}
	bool clipboardData(const std::string &format, std::string &out) override {
		NSPasteboard *board = NSPasteboard.generalPasteboard;
		if (format == "text") {
			NSString *text = [board stringForType:NSPasteboardTypeString];
			if (!text) return false;
			out = toStd(text);
			return true;
		}
		NSData *data = [board dataForType:format == "text/html" ? NSPasteboardTypeHTML : NSPasteboardTypeRTF];
		if (!data) return false;
		out.assign(static_cast<const char *>(data.bytes), data.length);
		return true;
	}
	void setClipboardData(const std::string &format, const std::string &data) override {
		NSPasteboard *board = NSPasteboard.generalPasteboard;
		[board clearContents];
		const auto put = [&](const std::string &value, NSPasteboardType type) {
			[board setData:[NSData dataWithBytes:value.data() length:value.size()] forType:type];
		};
		if (format == "text/rtf") {
			put(data, NSPasteboardTypeRTF);
		} else if (format == "text/html") {
			put(data, NSPasteboardTypeHTML);
		} else {
			const size_t split = data.find('\0');
			put(data.substr(0, split), NSPasteboardTypeHTML);
			if (split != std::string::npos) put(data.substr(split + 1), NSPasteboardTypeRTF);
		}
	}
	void launch(const std::vector<std::string> &arguments, const std::string &directory) override {
		std::string error;
		if (!spawnDetached(arguments, directory, error)) flash(error);
	}
	int runList(const ListDialog &dialog, std::vector<int> &selection) override { return OmaRunList(dialog, selection); }
	void setPanelVisible(int panel, const std::string &title, bool visible) override { [controller hostPanel:panel title:toNS(title) visible:visible]; }
	void panelChanged(int panel) override { [controller hostPanelChanged:panel]; }
	void setIncrementalBar(bool visible) override { [controller hostIncrementalBar:visible]; }
	void setChrome(const ChromeState &chrome) override { [controller hostChrome:chrome]; }
	void setTabStyle(const TabStyle &style) override { [controller hostTabStyle:style]; }
	void closeFind() override { [controller.findPanel close]; }
	void setFindInSelection(bool on) override { [controller.findPanel setInSelection:on]; }
	void beep() override { NSBeep(); }
	void focusPanel(int panel) override { [controller hostFocusPanel:panel]; }
	void selectFolderPath(const std::string &path) override { [controller hostSelectFolderPath:toNS(path)]; }
	void setTray(bool visible) override { [controller hostSetTray:visible]; }
	void setWindowPosition(long x, long y) override { [controller hostSetWindowX:x y:y]; }
	bool registerFileTypes(const std::vector<std::string> &extensions, std::string &error) override {
		NSString *bundle = NSBundle.mainBundle.bundleIdentifier;
		if (!bundle) {
			error = "This copy isn't running from omapad.app.";
			return false;
		}
		bool ok = true;
		for (const std::string &extension : extensions) {
			UTType *type = [UTType typeWithFilenameExtension:toNS(extension)];
			if (!type) continue;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			const OSStatus status = LSSetDefaultRoleHandlerForContentType((__bridge CFStringRef)type.identifier, kLSRolesAll, (__bridge CFStringRef)bundle);
#pragma clang diagnostic pop
			if (status != noErr) {
				ok = false;
				error = "macOS refused ." + extension + " (error " + std::to_string(status) + ").";
			}
		}
		return ok;
	}
	void setClipboard(const std::string &text) override {
		[NSPasteboard.generalPasteboard clearContents];
		[NSPasteboard.generalPasteboard setString:toNS(text) forType:NSPasteboardTypeString];
	}
	void reveal(const std::string &path) override {
		[NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:toNS(path)]]];
	}
	bool moveToTrash(const std::string &path, std::string &error) override {
		NSError *failure = nil;
		if ([NSFileManager.defaultManager trashItemAtURL:[NSURL fileURLWithPath:toNS(path)] resultingItemURL:nil error:&failure]) return true;
		error = toStd(failure.localizedDescription);
		return false;
	}
	void tabsChanged() override { [controller hostTabsChanged]; }
	void notesChanged(const NotesState &state) override { [controller hostNotesChanged:state]; }
	void statusChanged(const std::string &left, const std::string &right) override { [controller hostStatusLeft:toNS(left) right:toNS(right)]; }
	void flash(const std::string &message) override { [controller hostFlash:toNS(message)]; }
	void splitChanged(bool visible, bool stacked) override { [controller hostSplitChanged:visible stacked:stacked]; }
	void setResultsVisible(bool visible) override { [controller hostResultsVisible:visible]; }
	void setFolderPanelVisible(bool visible) override { [controller hostFolderVisible:visible]; }
	void foldersChanged() override { [controller hostFoldersChanged]; }
	void focusPane(int pane) override { [controller hostFocusPane:pane]; }
	void themeChanged() override { [controller hostThemeChanged]; }
	void menusChanged() override { [controller buildMenus]; }
	bool runForm(const std::string &title, std::vector<FormField> &fields) override { return OmaRunForm(toNS(title), fields); }
	void runShortcutMapper() override { OmaRunShortcutMapper(*[controller workspacePointer]); }
	bool print(PrintJob &job, const PrintOutput &output, std::string &error) override { return OmaRunPrint(job, output, error); }
	void setDocumentMapVisible(bool visible) override { [controller hostDocumentMapVisible:visible]; }
	void setFunctionListVisible(bool visible) override { [controller hostFunctionListVisible:visible]; }
	void functionListChanged() override { [controller hostFunctionListChanged]; }
	void showFind(FindTab tab, const std::string &prefill, const std::string &directory) override {
		if (!directory.empty()) [controller.findPanel setDirectory:toNS(directory)];
		[controller.findPanel showTab:tab prefill:toNS(prefill)];
	}
	void setFindText(const std::string &text) override { [controller.findPanel setFindText:toNS(text)]; }
	FindOptions currentFindOptions() override { return [controller.findPanel currentOptions]; }
	void setFindStatus(const std::string &status, bool isError) override { [controller.findPanel setStatus:toNS(status) isError:isError]; }
	void setFindInFilesRunning(bool running) override { [controller.findPanel setFindInFilesRunning:running]; }
	void setFullScreen(bool) override { [controller hostFullScreen]; }
	void setAlwaysOnTop(bool on) override { [controller hostAlwaysOnTop:on]; }
	bool systemIsDark() override { return [controller hostSystemIsDark]; }
	void post(std::function<void()> work) override {
		auto *heap = new std::function<void()>(std::move(work));
		dispatch_async(dispatch_get_main_queue(), ^{
			(*heap)();
			delete heap;
		});
	}
	void quit() override { [NSApp terminate:nil]; }
};

#pragma mark - Controller

@implementation AppController {
	std::unique_ptr<MacHost> _host;
	std::unique_ptr<Workspace> _workspace;
	NSWindow *_window;
	OmaEditorView *_editors[2];
	NSView *_paneBoxes[2];
	NotesPanel *_notesPanel;
	BOOL _notesEnabled, _notesActive, _normalToolbarVisible, _normalStatusVisible, _normalIncrementalVisible;
	int _notesView;
	std::string _notesSelectedPath;
	CGFloat _notesSavedDivider;
	BOOL _notesSavedStacked;
	TabBar *_tabBars[2];
	ScintillaView *_results;
	ScintillaView *_background;
	ScintillaView *_printer;
	OmaSciDelegate *_delegates[3];
	NSSplitView *_outer;       // Folder panel on the left, everything else on the right.
	NSSplitView *_rightSplit;  // Editors above results.
	NSSplitView *_editorSplit; // The two views.
	FolderPanel *_folderPanel;
	FunctionPanel *_functionPanel;
	ScintillaView *_map;
	NSView *_mapBox;
	NSTextField *_statusLeft, *_statusMessage, *_statusRight;
	FindPanel *_findPanel;
	NSTimer *_backupTimer;
	NSMutableArray<NSString *> *_pendingOpens;
	BOOL _launched;
	BOOL _settingFocus;
	NSMenu *_builtMenus;
	OmaSidePanel *_panels[kPanelCount];
	NSStackView *_panelsBox;
	NSStackView *_toolbar;
	NSLayoutConstraint *_toolbarHeight, *_statusHeight, *_incrementalHeight;
	NSView *_incrementalBar, *_statusBar;
	NSSearchField *_incrementalField;
	NSButton *_incrementalCase, *_incrementalAll;
	NSTextField *_incrementalStatus;
	NSLayoutConstraint *_barHeight[2], *_barWidth[2];
	NSArray<NSLayoutConstraint *> *_barTop[2], *_barSide[2];
	TabStyle _tabStyle;
	NSStatusItem *_statusItem;
	NSLayoutConstraint *_paneInsets[2][6];  // Four that grow inward from the top left, and two from the bottom right.
	BOOL _tabsShown;  // Set from the first ChromeState.
	BOOL _sidePanelsAllowed;
	BOOL _postIt;
}

@synthesize findPanel = _findPanel;

- (instancetype)initWithResourceDirectory:(NSString *)resources supportDirectory:(NSString *)support {
	if ((self = [super init])) {
		_pendingOpens = [NSMutableArray array];
		_host = std::make_unique<MacHost>();
		_host->controller = self;
		const std::string dir = toStd(support);
		_workspace = std::make_unique<Workspace>(*_host, toStd(resources), dir, dir);
		[self buildWindow];
		_host->panes[0].view = _editors[0];
		_host->panes[1].view = _editors[1];
		_host->results.view = _results;
		_host->background.view = _background;
		_host->printer.view = _printer;
		_host->map.view = _map;
		_findPanel = [[FindPanel alloc] initWithWorkspace:_workspace.get()];
		__weak AppController *weakSelf = self;
		sFileDropHandler = ^(NSArray<NSString *> *paths) { [weakSelf openPaths:paths]; };
		_workspace->start();
		[self buildMenus];
		[self hostSplitChanged:NO stacked:NO];
	}
	return self;
}

- (void)openPaths:(NSArray<NSString *> *)paths {
	for (NSString *path in paths) {
		BOOL isDirectory = NO;
		if ([NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&isDirectory] && isDirectory)
			_workspace->addFolder(toStd(path));
		else
			_workspace->openPath(toStd(path));
	}
}

- (ScintillaView *)makeEditor:(int)index {
	OmaEditorView *editor = [[OmaEditorView alloc] initWithFrame:NSMakeRect(0, 0, 500, 600)];
	__weak AppController *weakSelf = self;
	OmaSciDelegate *delegate = [OmaSciDelegate new];
	delegate.handler = ^(SCNotification *n) {
		AppController *strong = weakSelf;
		if (strong) strong->_workspace->editorNotification(index, *n);
	};
	_delegates[index] = delegate;
	editor.delegate = delegate;
	_editors[index] = editor;
	return editor;
}

- (NSMenu *)contextMenuForEditor:(int)pane event:(NSEvent *)event {
	OmaEditorView *editor = _editors[pane];
	const NSPoint point = [editor.content convertPoint:event.locationInWindow fromView:nil];
	const long position = [editor message:SCI_POSITIONFROMPOINT wParam:static_cast<uintptr_t>(point.x) lParam:static_cast<intptr_t>(point.y)];
	_workspace->editorRightClicked(pane, position);
	return [self menuFromItems:_workspace->editorContextMenu() title:@""];
}

- (NSView *)makePaneBox:(int)index {
	NSView *box = [NSView new];
	TabBar *bar = [[TabBar alloc] initWithFrame:NSZeroRect];
	bar.delegate = self;
	bar.translatesAutoresizingMaskIntoConstraints = NO;
	bar.identifier = index == 0 ? @"0" : @"1";
	_tabBars[index] = bar;
	ScintillaView *editor = [self makeEditor:index];
	editor.translatesAutoresizingMaskIntoConstraints = NO;
	[box addSubview:bar];
	[box addSubview:editor];
	__weak AppController *weakSelf = self;
	((OmaEditorView *)editor).contextMenu = ^NSMenu *(NSEvent *event) {
		AppController *strong = weakSelf;
		return strong ? [strong contextMenuForEditor:index event:event] : nil;
	};
	_barHeight[index] = [bar.heightAnchor constraintEqualToConstant:30];
	_barWidth[index] = [bar.widthAnchor constraintEqualToConstant:170];
	_paneInsets[index][0] = [editor.topAnchor constraintEqualToAnchor:bar.bottomAnchor];
	_paneInsets[index][1] = [editor.leadingAnchor constraintEqualToAnchor:box.leadingAnchor];
	_paneInsets[index][2] = [editor.topAnchor constraintEqualToAnchor:box.topAnchor];
	_paneInsets[index][3] = [editor.leadingAnchor constraintEqualToAnchor:bar.trailingAnchor];
	_paneInsets[index][4] = [editor.trailingAnchor constraintEqualToAnchor:box.trailingAnchor];
	_paneInsets[index][5] = [editor.bottomAnchor constraintEqualToAnchor:box.bottomAnchor];
	_barTop[index] = @[
		[bar.topAnchor constraintEqualToAnchor:box.topAnchor],
		[bar.leadingAnchor constraintEqualToAnchor:box.leadingAnchor],
		[bar.trailingAnchor constraintEqualToAnchor:box.trailingAnchor],
		_barHeight[index],
		_paneInsets[index][0],
		_paneInsets[index][1],
	];
	_barSide[index] = @[
		[bar.topAnchor constraintEqualToAnchor:box.topAnchor],
		[bar.leadingAnchor constraintEqualToAnchor:box.leadingAnchor],
		[bar.bottomAnchor constraintEqualToAnchor:box.bottomAnchor],
		_barWidth[index],
		_paneInsets[index][2],
		_paneInsets[index][3],
	];
	[NSLayoutConstraint activateConstraints:_barTop[index]];
	[NSLayoutConstraint activateConstraints:@[_paneInsets[index][4], _paneInsets[index][5]]];
	return box;
}

- (void)buildWindow {
	_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1100, 720)
	                                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
	                                        backing:NSBackingStoreBuffered
	                                          defer:NO];
	_window.releasedWhenClosed = NO;
	_window.delegate = self;
	_window.minSize = NSMakeSize(480, 300);
	_window.tabbingMode = NSWindowTabbingModeDisallowed;
	_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
	[_window center];
	_window.frameAutosaveName = @"OmapadMainWindow";

	OmaRootView *root = [[OmaRootView alloc] initWithFrame:_window.contentView.bounds];
	__weak AppController *weakSelf = self;
	root.appearanceChanged = ^{
		AppController *strong = weakSelf;
		if (strong && strong->_workspace) strong->_workspace->systemAppearanceChanged();
	};
	_window.contentView = root;

	_paneBoxes[0] = [self makePaneBox:0];
	_paneBoxes[1] = [self makePaneBox:1];
	_editorSplit = [NSSplitView new];
	_editorSplit.vertical = YES;
	_editorSplit.dividerStyle = NSSplitViewDividerStyleThin;
	[_editorSplit addArrangedSubview:_paneBoxes[0]];

	OmaEditorView *resultsView = [[OmaEditorView alloc] initWithFrame:NSMakeRect(0, 0, 1000, 180)];
	resultsView.contextMenu = ^NSMenu *(NSEvent *) {
		AppController *strong = weakSelf;
		return strong ? [strong menuFromItems:strong->_workspace->resultsMenu() title:@""] : nil;
	};
	_results = resultsView;
	OmaSciDelegate *resultsDelegate = [OmaSciDelegate new];
	resultsDelegate.handler = ^(SCNotification *n) {
		AppController *strong = weakSelf;
		if (strong) strong->_workspace->resultsNotification(*n);
	};
	_delegates[2] = resultsDelegate;
	_results.delegate = resultsDelegate;
	_background = [[ScintillaView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
	_printer = [[ScintillaView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];

	_rightSplit = [NSSplitView new];
	_rightSplit.vertical = NO;
	_rightSplit.dividerStyle = NSSplitViewDividerStyleThin;
	[_rightSplit addArrangedSubview:_editorSplit];

	_folderPanel = [[FolderPanel alloc] initWithWorkspace:_workspace.get()];
	_folderPanel.menuBuilder = ^NSMenu *(std::vector<MenuItem> items) {
		AppController *strong = weakSelf;
		return strong ? [strong menuFromItems:items title:@""] : nil;
	};

	_functionPanel = [[FunctionPanel alloc] initWithWorkspace:_workspace.get()];

	_map = [[ScintillaView alloc] initWithFrame:NSMakeRect(0, 0, 120, 600)];
	_map.translatesAutoresizingMaskIntoConstraints = NO;
	OmaMapOverlay *overlay = [OmaMapOverlay new];
	overlay.translatesAutoresizingMaskIntoConstraints = NO;
	overlay.pressed = ^(NSPoint point) {
		AppController *strong = weakSelf;
		if (strong) strong->_workspace->mapPressed(static_cast<long>(point.x), static_cast<long>(point.y));
	};
	overlay.scrolled = ^(CGFloat lines) {
		AppController *strong = weakSelf;
		if (strong) strong->_workspace->mapScrolled(static_cast<long>(lines));
	};
	_mapBox = [NSView new];
	[_mapBox addSubview:_map];
	[_mapBox addSubview:overlay positioned:NSWindowAbove relativeTo:_map];
	[NSLayoutConstraint activateConstraints:@[
		[_map.topAnchor constraintEqualToAnchor:_mapBox.topAnchor],
		[_map.leadingAnchor constraintEqualToAnchor:_mapBox.leadingAnchor],
		[_map.trailingAnchor constraintEqualToAnchor:_mapBox.trailingAnchor],
		[_map.bottomAnchor constraintEqualToAnchor:_mapBox.bottomAnchor],
		[overlay.topAnchor constraintEqualToAnchor:_mapBox.topAnchor],
		[overlay.leadingAnchor constraintEqualToAnchor:_mapBox.leadingAnchor],
		[overlay.trailingAnchor constraintEqualToAnchor:_mapBox.trailingAnchor],
		[overlay.bottomAnchor constraintEqualToAnchor:_mapBox.bottomAnchor],
	]];

	_outer = [NSSplitView new];
	_outer.vertical = YES;
	_outer.dividerStyle = NSSplitViewDividerStyleThin;
	_outer.translatesAutoresizingMaskIntoConstraints = NO;
	[_outer addArrangedSubview:_rightSplit];

	_statusLeft = [NSTextField labelWithString:@""];
	_statusRight = [NSTextField labelWithString:@""];
	_statusMessage = [NSTextField labelWithString:@""];
	for (NSTextField *label in @[_statusLeft, _statusMessage, _statusRight]) {
		label.font = [NSFont systemFontOfSize:11];
		label.textColor = NSColor.secondaryLabelColor;
		label.lineBreakMode = NSLineBreakByTruncatingTail;
		label.translatesAutoresizingMaskIntoConstraints = NO;
		[label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
	}
	_statusMessage.textColor = NSColor.controlAccentColor;
	_statusRight.alignment = NSTextAlignmentRight;
	NSView *status = [NSView new];
	status.translatesAutoresizingMaskIntoConstraints = NO;
	[status addSubview:_statusLeft];
	[status addSubview:_statusMessage];
	[status addSubview:_statusRight];
	_statusBar = status;
	// Clicking either end of the status bar offers what it shows.
	for (NSTextField *label in @[_statusLeft, _statusRight]) {
		NSClickGestureRecognizer *click = [[NSClickGestureRecognizer alloc] initWithTarget:self action:@selector(statusClicked:)];
		[label addGestureRecognizer:click];
	}

	_sidePanelsAllowed = YES;
	_panelsBox = [NSStackView new];
	_panelsBox.orientation = NSUserInterfaceLayoutOrientationVertical;
	_panelsBox.distribution = NSStackViewDistributionFillEqually;
	_panelsBox.spacing = 1;

	[self buildToolbar];
	[self buildIncrementalBar];
	[root addSubview:_toolbar];
	[root addSubview:_incrementalBar];
	[root addSubview:_outer];
	[root addSubview:status];
	_toolbarHeight = [_toolbar.heightAnchor constraintEqualToConstant:30];
	_statusHeight = [status.heightAnchor constraintEqualToConstant:22];
	_incrementalHeight = [_incrementalBar.heightAnchor constraintEqualToConstant:0];
	[NSLayoutConstraint activateConstraints:@[
		[_toolbar.topAnchor constraintEqualToAnchor:root.topAnchor],
		[_toolbar.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
		[_toolbar.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
		_toolbarHeight,
		[_outer.topAnchor constraintEqualToAnchor:_toolbar.bottomAnchor],
		[_outer.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
		[_outer.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
		[_outer.bottomAnchor constraintEqualToAnchor:_incrementalBar.topAnchor],
		[_incrementalBar.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
		[_incrementalBar.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
		[_incrementalBar.bottomAnchor constraintEqualToAnchor:status.topAnchor],
		_incrementalHeight,
		[status.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
		[status.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
		[status.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
		_statusHeight,
		[_statusLeft.leadingAnchor constraintEqualToAnchor:status.leadingAnchor constant:10],
		[_statusLeft.centerYAnchor constraintEqualToAnchor:status.centerYAnchor],
		[_statusMessage.leadingAnchor constraintEqualToAnchor:_statusLeft.trailingAnchor constant:16],
		[_statusMessage.centerYAnchor constraintEqualToAnchor:status.centerYAnchor],
		[_statusRight.leadingAnchor constraintGreaterThanOrEqualToAnchor:_statusMessage.trailingAnchor constant:16],
		[_statusRight.trailingAnchor constraintEqualToAnchor:status.trailingAnchor constant:-10],
		[_statusRight.centerYAnchor constraintEqualToAnchor:status.centerYAnchor],
	]];
}

#pragma mark Toolbar and search bar

- (void)buildToolbar {
	NSMutableArray<NSView *> *views = [NSMutableArray array];
	for (const ToolItem &item : _workspace->toolbarItems()) {
		if (item.separator) {
			NSBox *gap = [NSBox new];
			gap.boxType = NSBoxSeparator;
			[gap.widthAnchor constraintEqualToConstant:1].active = YES;
			[gap.heightAnchor constraintEqualToConstant:18].active = YES;
			[views addObject:gap];
			continue;
		}
		NSImage *image = [NSImage imageWithSystemSymbolName:toNS(item.symbol) accessibilityDescription:toNS(item.title)];
		NSButton *button = image ? [NSButton buttonWithImage:image target:self action:@selector(toolbarPressed:)]
		                         : [NSButton buttonWithTitle:toNS(item.title) target:self action:@selector(toolbarPressed:)];
		button.bordered = NO;
		button.toolTip = toNS(item.title);
		button.tag = static_cast<NSInteger>(views.count);
		button.refusesFirstResponder = YES;
		if (!item.customIcon.empty()) {
			NSImage *custom = [[NSImage alloc] initWithContentsOfFile:toNS(item.customIcon)];
			if (custom) {
				custom.size = NSMakeSize(16, 16);
				button.image = custom;
				button.title = @"";
			}
		} else {
			button.identifier = toNS(item.symbol);
		}
		OmaMenuAction *action = [OmaMenuAction new];
		action->item.cmd = item.cmd;
		action->item.arg = item.arg;
		button.cell.representedObject = action;
		[button.widthAnchor constraintEqualToConstant:24].active = YES;
		[views addObject:button];
	}
	_toolbar = [NSStackView stackViewWithViews:views];
	_toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
	_toolbar.alignment = NSLayoutAttributeCenterY;
	_toolbar.spacing = 2;
	_toolbar.edgeInsets = NSEdgeInsetsMake(0, 8, 0, 8);
	_toolbar.translatesAutoresizingMaskIntoConstraints = NO;
	[_toolbar setHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
}

- (void)toolbarPressed:(NSButton *)button {
	OmaMenuAction *action = button.cell.representedObject;
	if (action) _workspace->run(action->item.cmd, action->item.arg, action->item.text);
}

- (void)statusClicked:(NSClickGestureRecognizer *)click {
	NSMenu *menu = [self menuFromItems:_workspace->statusMenu(click.view == _statusRight) title:@""];
	[menu popUpMenuPositioningItem:nil atLocation:[click locationInView:click.view] inView:click.view];
}

- (void)buildIncrementalBar {
	_incrementalField = [NSSearchField new];
	_incrementalField.target = self;
	_incrementalField.action = @selector(incrementalChanged:);
	[_incrementalField.widthAnchor constraintEqualToConstant:260].active = YES;
	NSButton *close = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"xmark" accessibilityDescription:@"Close"] target:self action:@selector(incrementalClose:)];
	close.bordered = NO;
	NSButton *previous = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"chevron.up" accessibilityDescription:@"Previous"] target:self action:@selector(incrementalPrevious:)];
	NSButton *next = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"chevron.down" accessibilityDescription:@"Next"] target:self action:@selector(incrementalNext:)];
	_incrementalAll = [NSButton checkboxWithTitle:@"Highlight all" target:self action:@selector(incrementalChanged:)];
	_incrementalAll.state = NSControlStateValueOn;
	_incrementalCase = [NSButton checkboxWithTitle:@"Match case" target:self action:@selector(incrementalChanged:)];
	_incrementalStatus = [NSTextField labelWithString:@""];
	NSStackView *row = [NSStackView stackViewWithViews:@[close, [NSTextField labelWithString:@"Find:"], _incrementalField, previous, next, _incrementalAll, _incrementalCase, _incrementalStatus]];
	row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
	row.spacing = 8;
	row.edgeInsets = NSEdgeInsetsMake(0, 8, 0, 8);
	row.hidden = YES;
	row.translatesAutoresizingMaskIntoConstraints = NO;
	_incrementalBar = row;
}

- (void)runIncremental:(BOOL)backward next:(BOOL)next {
	const std::string status = _workspace->incrementalSearch(toStd(_incrementalField.stringValue), backward, next, _incrementalCase.state == NSControlStateValueOn, _incrementalAll.state == NSControlStateValueOn);
	_incrementalStatus.stringValue = toNS(status);
}
- (void)incrementalChanged:(id)sender { [self runIncremental:NO next:NO]; }
- (void)incrementalNext:(id)sender { [self runIncremental:NO next:YES]; }
- (void)incrementalPrevious:(id)sender { [self runIncremental:YES next:YES]; }
- (void)incrementalClose:(id)sender { _workspace->closeIncrementalSearch(); }

- (void)hostIncrementalBar:(BOOL)visible {
	_normalIncrementalVisible = visible;
	_incrementalBar.hidden = !visible || _notesEnabled;
	_incrementalHeight.constant = visible ? 34 : 0;
	if (visible && !_notesEnabled) [_window makeFirstResponder:_incrementalField];
}

#pragma mark Panels and modes

- (void)hostPanel:(int)panel title:(NSString *)title visible:(BOOL)visible {
	if (!_panels[panel]) {
		_panels[panel] = [[OmaSidePanel alloc] initWithWorkspace:_workspace.get() panel:panel title:title];
		__weak AppController *weakSelf = self;
		_panels[panel].menuBuilder = ^NSMenu *(std::vector<MenuItem> items) {
			AppController *strong = weakSelf;
			return strong ? [strong menuFromItems:items title:@""] : nil;
		};
	}
	if (visible) [_panelsBox addArrangedSubview:_panels[panel]];
	else [_panels[panel] removeFromSuperview];
	[self hostPanelChanged:panel];
	[self updatePanelsBox];
}

- (void)updatePanelsBox {
	const BOOL needed = _panelsBox.arrangedSubviews.count > 0 && _sidePanelsAllowed;
	if (needed && !_panelsBox.superview) [_outer insertArrangedSubview:_panelsBox atIndex:[_outer.arrangedSubviews indexOfObject:_rightSplit] + 1];
	else if (!needed && _panelsBox.superview) [_panelsBox removeFromSuperview];
	[self arrangeSidePanels];
}

- (void)hostPanelChanged:(int)panel {
	if (_panels[panel].superview) [_panels[panel] reload];
}

- (void)hostChrome:(const ChromeState &)chrome {
	_normalToolbarVisible = chrome.toolbar;
	_normalStatusVisible = chrome.status;
	_toolbar.hidden = !chrome.toolbar || _notesEnabled;
	_toolbarHeight.constant = chrome.toolbar ? (chrome.largeToolbar ? 38 : 30) : 0;
	for (NSView *view in _toolbar.arrangedSubviews) {
		if (![view isKindOfClass:NSButton.class]) continue;
		NSButton *button = (NSButton *)view;
		if (button.identifier.length) {
			// Filled icons are the same symbols with ".fill", where the symbol has that form.
			NSImage *plain = [NSImage imageWithSystemSymbolName:button.identifier accessibilityDescription:button.toolTip];
			NSImage *filled = chrome.filledToolbar ? [NSImage imageWithSystemSymbolName:[button.identifier stringByAppendingString:@".fill"] accessibilityDescription:button.toolTip] : nil;
			if (filled ?: plain) button.image = filled ?: plain;
		}
		button.contentTintColor = chrome.toolbarColour.size() == 6 ? colourFromHexString(chrome.toolbarColour) : nil;
		if (button.image) button.symbolConfiguration = [NSImageSymbolConfiguration configurationWithScale:chrome.largeToolbar ? NSImageSymbolScaleLarge : NSImageSymbolScaleMedium];
	}
	for (int pane = 0; pane < 2; pane++) {
		for (int side = 0; side < 6; side++) {
			if (_paneInsets[pane][side]) _paneInsets[pane][side].constant = side < 4 ? chrome.borderWidth : -chrome.borderWidth;
		}
	}
	_statusBar.hidden = !chrome.status || _notesEnabled;
	_statusHeight.constant = chrome.status ? 22 : 0;
	_tabsShown = chrome.tabs;
	[self hostTabStyle:_tabStyle];
	_sidePanelsAllowed = chrome.panels;
	[self updatePanelsBox];
	[self hostFolderVisible:chrome.panels && _workspace->folderPanelVisible()];
	[self hostFunctionListVisible:chrome.panels && _workspace->functionListVisible()];
	[self hostDocumentMapVisible:chrome.panels && _workspace->documentMapVisible()];
	if (chrome.frameless != _postIt) {
		_postIt = chrome.frameless;
		_window.titleVisibility = _postIt ? NSWindowTitleHidden : NSWindowTitleVisible;
		_window.titlebarAppearsTransparent = _postIt;
		for (NSWindowButton which : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) [_window standardWindowButton:which].hidden = _postIt;
		_window.level = _postIt ? NSFloatingWindowLevel : NSNormalWindowLevel;
	}
	if (chrome.appearance == "light") NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
	else if (chrome.appearance == "dark") NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
	else NSApp.appearance = nil;
}

- (void)hostTabStyle:(const TabStyle &)style {
	_tabStyle = style;
	const BOOL showTabs = _tabsShown && !_notesEnabled;
	for (int pane = 0; pane < 2; pane++) {
		TabBar *bar = _tabBars[pane];
		bar.vertical = style.vertical;
		bar.multiLine = style.multiLine && !style.vertical;
		bar.locked = style.locked;
		bar.reduced = style.reduced;
		bar.showClose = style.closeButton;
		bar.activeBar = style.activeBar;
		bar.inactiveColour = style.inactiveColour;
		bar.showPin = style.pinButton;
		bar.buttonsOnInactive = style.buttonsOnInactive;
		bar.rightShortcuts = style.rightShortcuts;
		[NSLayoutConstraint deactivateConstraints:_barTop[pane]];
		[NSLayoutConstraint deactivateConstraints:_barSide[pane]];
		[NSLayoutConstraint activateConstraints:style.vertical ? _barSide[pane] : _barTop[pane]];
		bar.hidden = !showTabs;
		_barWidth[pane].constant = showTabs ? 170 : 0;
		_barHeight[pane].constant = !showTabs ? 0 : [bar heightForWidth:MAX(NSWidth(_paneBoxes[pane].bounds), 400)];
		bar.needsDisplay = YES;
	}
}

#pragma mark Host callbacks

- (void)hostTabsChanged {
	for (int pane = 0; pane < 2; pane++) {
		NSMutableArray *items = [NSMutableArray array];
		for (const TabInfo &info : _workspace->tabs(pane)) {
			OmaTabItem *item = [OmaTabItem new];
			item.title = toNS(info.title);
			item.toolTip = toNS(info.toolTip);
			item.dirty = info.dirty;
			item.pinned = info.pinned;
			item.colour = info.colour;
			item.display = toNS(tabDisplayTitle(info, _tabStyle));
			[items addObject:item];
		}
		if (_tabStyle.multiLine && _tabsShown && !_notesEnabled) _barHeight[pane].constant = [_tabBars[pane] heightForWidth:MAX(NSWidth(_paneBoxes[pane].bounds), 400)];
		[_tabBars[pane] setItems:items activeIndex:_workspace->activeTab(pane) focused:_workspace->focusedPane() == pane];
	}
	const std::string path = _workspace->windowPath();
	const std::string title = _workspace->windowTitle();
	_window.title = title.empty() ? @"omapad" : toNS(title);
	_window.representedURL = path.empty() ? nil : [NSURL fileURLWithPath:toNS(path)];
	_window.documentEdited = _workspace->windowDirty();
}

- (void)hostNotesChanged:(const NotesState &)state {
	const BOOL entering = state.enabled && !_notesEnabled;
	const BOOL leaving = !state.enabled && _notesEnabled;
	const BOOL viewChanged = state.viewMode != _notesView;
	const BOOL selectionChanged = state.selectedPath != _notesSelectedPath || (state.activeNote && !_notesActive);
	_notesSelectedPath = state.selectedPath;
	_notesActive = state.activeNote;
	_notesView = state.viewMode;
	if (entering) {
		_notesSavedStacked = !_editorSplit.vertical;
		_notesSavedDivider = _editorSplit.arrangedSubviews.count > 1
			? (_editorSplit.vertical ? NSWidth(_paneBoxes[0].frame) : NSHeight(_paneBoxes[0].frame)) : -1;
		_notesEnabled = YES;
		if (!_notesPanel) {
			_notesPanel = [[NotesPanel alloc] initWithWorkspace:_workspace.get()];
			NSView *root = _window.contentView;
			[root addSubview:_notesPanel];
			[NSLayoutConstraint activateConstraints:@[
				[_notesPanel.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
				[_notesPanel.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
				[_notesPanel.topAnchor constraintEqualToAnchor:root.topAnchor],
				[_notesPanel.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
			]];
		}
		[_paneBoxes[0] removeFromSuperview];
		[_notesPanel setSourceView:_paneBoxes[0]];
		_outer.hidden = YES;
		_toolbar.hidden = YES;
		_statusBar.hidden = YES;
		_incrementalBar.hidden = YES;
		[self hostTabStyle:_tabStyle];
	}
	[_notesPanel updateState:state];
	if (leaving) {
		[_notesPanel detachSourceView];
		_notesEnabled = NO;
		[_editorSplit insertArrangedSubview:_paneBoxes[0] atIndex:0];
		_outer.hidden = NO;
		_toolbar.hidden = !_normalToolbarVisible;
		_statusBar.hidden = !_normalStatusVisible;
		_incrementalBar.hidden = !_normalIncrementalVisible;
		[self hostTabStyle:_tabStyle];
		[self hostSplitChanged:_workspace->splitVisible() stacked:_workspace->splitStacked()];
		if (_notesSavedDivider >= 0 && _workspace->splitVisible() && _notesSavedStacked == _workspace->splitStacked()) {
			[_editorSplit setPosition:_notesSavedDivider ofDividerAtIndex:0];
		}
		[self hostFocusPane:_workspace->focusedPane()];
	} else if (state.enabled && state.activeNote && (viewChanged || selectionChanged || entering)) {
		if (state.viewMode == 2) [_notesPanel focusPreview];
		else [self hostFocusPane:0];
	}
}

- (void)hostStatusLeft:(NSString *)left right:(NSString *)right {
	_statusLeft.stringValue = left;
	_statusRight.stringValue = right;
}

- (void)hostFlash:(NSString *)message {
	_statusMessage.stringValue = message;
	[NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(clearFlash) object:nil];
	[self performSelector:@selector(clearFlash) withObject:nil afterDelay:6];
}

- (void)clearFlash { _statusMessage.stringValue = @""; }

- (void)hostSplitChanged:(BOOL)visible stacked:(BOOL)stacked {
	if (_notesEnabled) return;
	const BOOL shown = _paneBoxes[1].superview != nil;
	_editorSplit.vertical = !stacked;
	if (visible && !shown) [_editorSplit addArrangedSubview:_paneBoxes[1]];
	if (!visible && shown) [_paneBoxes[1] removeFromSuperview];
	[_editorSplit layoutSubtreeIfNeeded];
	if (visible) {
		const CGFloat size = stacked ? NSHeight(_editorSplit.bounds) : NSWidth(_editorSplit.bounds);
		[_editorSplit setPosition:floor(size / 2) ofDividerAtIndex:0];
	}
	[_editorSplit adjustSubviews];
}

- (void)hostResultsVisible:(BOOL)visible {
	if (visible && !_results.superview) {
		[_rightSplit addArrangedSubview:_results];
		[_rightSplit layoutSubtreeIfNeeded];
		[_rightSplit setPosition:NSHeight(_rightSplit.bounds) - 200 ofDividerAtIndex:0];
	} else if (!visible && _results.superview) {
		[_results removeFromSuperview];
	}
}

- (void)hostFolderVisible:(BOOL)visible {
	if (visible && !_folderPanel.superview) {
		[_outer insertArrangedSubview:_folderPanel atIndex:0];
		[_outer layoutSubtreeIfNeeded];
		[_outer setPosition:240 ofDividerAtIndex:0];
		[_folderPanel reload];
	} else if (!visible && _folderPanel.superview) {
		[_folderPanel removeFromSuperview];
	}
	[self arrangeSidePanels];
}

- (Workspace *)workspacePointer { return _workspace.get(); }

// Keeps the side panels' widths when the window resizes, and gives a newly shown one its room.
- (void)arrangeSidePanels {
	[_outer layoutSubtreeIfNeeded];
	NSArray<NSView *> *views = _outer.arrangedSubviews;
	const NSUInteger editors = [views indexOfObject:_rightSplit];
	for (NSUInteger i = 0; i < views.count; i++) [_outer setHoldingPriority:views[i] == _rightSplit ? 240 : 260 forSubviewAtIndex:i];
	const CGFloat total = NSWidth(_outer.bounds);
	const BOOL hasMap = _mapBox.superview != nil, hasFunctions = _functionPanel.superview != nil, hasPanels = _panelsBox.superview != nil;
	const CGFloat mapWidth = hasMap ? 120 : 0;
	const CGFloat functionWidth = hasFunctions ? 250 : 0;
	if (hasPanels) [_outer setPosition:total - mapWidth - functionWidth - 250 ofDividerAtIndex:editors];
	if (hasFunctions) [_outer setPosition:total - mapWidth - 250 ofDividerAtIndex:hasPanels ? editors + 1 : editors];
	if (hasMap) [_outer setPosition:total - mapWidth ofDividerAtIndex:views.count - 2];
}

- (void)hostFunctionListVisible:(BOOL)visible {
	if (visible && !_functionPanel.superview) {
		[_outer insertArrangedSubview:_functionPanel atIndex:[_outer.arrangedSubviews indexOfObject:_rightSplit] + 1];
		[_functionPanel reload];
	} else if (!visible && _functionPanel.superview) {
		[_functionPanel removeFromSuperview];
	}
	[self arrangeSidePanels];
}

- (void)hostFunctionListChanged {
	if (_functionPanel.superview) [_functionPanel reload];
}

- (void)hostDocumentMapVisible:(BOOL)visible {
	if (visible && !_mapBox.superview) [_outer addArrangedSubview:_mapBox];
	else if (!visible && _mapBox.superview) [_mapBox removeFromSuperview];
	[self arrangeSidePanels];
}

- (void)hostFoldersChanged {
	if (_folderPanel.superview) [_folderPanel reload];
}

- (void)hostFocusPane:(int)pane {
	if (_notesEnabled && (!_notesActive || _notesView == 2 || pane != 0)) return;
	if (_window.isVisible) [_window makeFirstResponder:_editors[pane].content];
}

- (void)hostThemeChanged {
	if (_workspace->usesAutomaticTheme() && _workspace->effectiveTheme() != "Omarchy") {
		_window.appearance = nil;
	} else {
		// An explicit theme decides the chrome too, so a dark theme never sits in a light window.
		_window.appearance = [NSAppearance appearanceNamed:_workspace->themeIsDark() ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
	}
}

- (void)hostFullScreen { [_window toggleFullScreen:nil]; }

- (void)hostAlwaysOnTop:(BOOL)on { _window.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel; }

- (BOOL)hostSystemIsDark {
	NSAppearance *appearance = _window.contentView.effectiveAppearance ?: NSApp.effectiveAppearance;
	return [[appearance bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]] isEqualToString:NSAppearanceNameDarkAqua];
}

- (NSFont *)currentFont {
	return [NSFont fontWithName:toNS(_workspace->settings.fontName) size:_workspace->settings.fontSize] ?: [NSFont userFixedPitchFontOfSize:13];
}

- (void)hostChooseFont {
	NSFontManager *manager = NSFontManager.sharedFontManager;
	manager.target = self;
	manager.action = @selector(changeFont:);
	[manager setSelectedFont:[self currentFont] isMultiple:NO];
	[manager orderFrontFontPanel:self];
}

- (void)changeFont:(NSFontManager *)sender {
	NSFont *font = [sender convertFont:[self currentFont]];
	_workspace->settings.fontName = toStd(font.familyName);
	_workspace->settings.fontSize = font.pointSize;
	_workspace->run(Cmd::SetTheme, 0, _workspace->settings.theme);  // Saves and reapplies.
}

#pragma mark Tab bar

- (int)paneOf:(TabBar *)bar { return bar == _tabBars[1] ? 1 : 0; }
- (void)tabBar:(TabBar *)bar didSelectIndex:(NSInteger)index { _workspace->clickTab([self paneOf:bar], static_cast<int>(index)); }
- (void)tabBar:(TabBar *)bar didRequestCloseIndex:(NSInteger)index { _workspace->closeTab([self paneOf:bar], static_cast<int>(index)); }
- (void)tabBar:(TabBar *)bar didMoveIndex:(NSInteger)from toIndex:(NSInteger)to { _workspace->moveTab([self paneOf:bar], static_cast<int>(from), static_cast<int>(to)); }
- (void)tabBar:(TabBar *)bar didHoverIndex:(NSInteger)index { _workspace->tabHovered([self paneOf:bar], static_cast<int>(index)); }

- (void)tabBar:(TabBar *)bar didTogglePinIndex:(NSInteger)index {
	_workspace->run(Cmd::TabPin, [self paneOf:bar] * 10000 + static_cast<int>(index));
}

- (void)tabBarDidRequestList:(TabBar *)bar sender:(NSButton *)button {
	NSMenu *menu = [NSMenu new];
	for (const MenuItem &item : _workspace->dynamicMenu(MenuItem::Dynamic::Documents)) [menu addItem:[self nativeItem:item]];
	[menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, NSHeight(button.bounds)) inView:button];
}

- (void)tabBarDidRequestCloseActive:(TabBar *)bar {
	_workspace->paneFocused([self paneOf:bar]);
	_workspace->run(Cmd::Close);
}

- (void)hostFocusPanel:(int)panel {
	NSView *target = panel == omapad::kPanelFolders ? _folderPanel.focusTarget : panel == omapad::kPanelFunctions ? _functionPanel.focusTarget : panel >= 0 && panel < omapad::kPanelCount ? _panels[panel].focusTarget : nil;
	if (target) [_window makeFirstResponder:target];
}

- (void)hostSelectFolderPath:(NSString *)path { [_folderPanel selectPath:path]; }

- (void)hostSetWindowX:(long)x y:(long)y {
	const NSRect frame = _window.frame;
	const NSRect screen = (_window.screen ?: NSScreen.mainScreen).frame;
	// Notepad++ counts from the top left, and macOS from the bottom left.
	[_window setFrameTopLeftPoint:NSMakePoint(x >= 0 ? NSMinX(screen) + x : NSMinX(frame), y >= 0 ? NSMaxY(screen) - y : NSMaxY(frame))];
}

#pragma mark System tray

- (void)hostSetTray:(BOOL)visible {
	if (!visible) {
		if (_statusItem) [NSStatusBar.systemStatusBar removeStatusItem:_statusItem];
		_statusItem = nil;
		return;
	}
	if (_statusItem) return;
	_statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
	_statusItem.button.image = [NSImage imageWithSystemSymbolName:@"doc.text" accessibilityDescription:@"omapad"];
	_statusItem.button.toolTip = @"omapad";
	NSMenu *menu = [NSMenu new];
	const auto add = [&](NSString *title, SEL action) {
		NSMenuItem *item = [menu addItemWithTitle:title action:action keyEquivalent:@""];
		item.target = self;
	};
	add(@"Activate omapad", @selector(trayActivate:));
	[menu addItem:NSMenuItem.separatorItem];
	add(@"New", @selector(trayNew:));
	add(@"New and Paste", @selector(trayNewPaste:));
	add(@"Open…", @selector(trayOpen:));
	add(@"Find in Files…", @selector(trayFindInFiles:));
	[menu addItem:NSMenuItem.separatorItem];
	add(@"Close Tray Icon", @selector(trayClose:));
	_statusItem.menu = menu;
}

- (void)showFromTray {
	[_window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
}
- (void)trayActivate:(id)sender { [self showFromTray]; }
- (void)trayNew:(id)sender { [self showFromTray]; _workspace->run(Cmd::New); }
- (void)trayNewPaste:(id)sender { [self showFromTray]; _workspace->run(Cmd::New); _workspace->run(Cmd::Paste); }
- (void)trayOpen:(id)sender { [self showFromTray]; _workspace->run(Cmd::Open); }
- (void)trayFindInFiles:(id)sender { [self showFromTray]; _workspace->run(Cmd::ShowFindInFiles); }
- (void)trayClose:(id)sender { [self showFromTray]; [self hostSetTray:NO]; }

- (void)windowDidMiniaturize:(NSNotification *)note {
	if (_workspace->settings.pref("systemTray") != "minimize") return;
	[self hostSetTray:YES];
	[_window deminiaturize:nil];
	[_window orderOut:nil];
}

#pragma mark Edit with omapad

// The Services menu entry, from the Finder and other programs.
- (void)editWithOmapad:(NSPasteboard *)pasteboard userData:(NSString *)userData error:(NSString **)error {
	NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
	NSMutableArray<NSString *> *paths = [NSMutableArray array];
	for (NSURL *url in urls) [paths addObject:url.path];
	[self openPaths:paths];
	[self showFromTray];
}

- (void)tabBarDidRequestNewTab:(TabBar *)bar {
	_workspace->paneFocused([self paneOf:bar]);
	_workspace->createUntitledDocument();
}
- (NSMenu *)tabBar:(TabBar *)bar menuForIndex:(NSInteger)index {
	return [self menuFromItems:_workspace->tabMenu([self paneOf:bar], static_cast<int>(index)) title:@""];
}

- (void)tabBar:(TabBar *)bar didDoubleClickIndex:(NSInteger)index {
	_workspace->tabDoubleClicked([self paneOf:bar], static_cast<int>(index));
}

#pragma mark Application

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
	std::vector<std::string> incoming;
	NSArray *arguments = NSProcessInfo.processInfo.arguments;
	for (NSUInteger i = 1; i < arguments.count; i++) {
		NSString *argument = arguments[i];
		// AppKit's own "-Key value" pairs aren't for us, but omapad's switches are.
		if ([argument hasPrefix:@"-Apple"] || [argument hasPrefix:@"-NS"] || [argument hasPrefix:@"-psn"]) {
			i++;
			continue;
		}
		incoming.push_back(toStd(argument));
	}
	const CommandLine line = parseCommandLine(incoming);
	NSApp.servicesProvider = self;
	_workspace->prepareCommandLine(line);
	_workspace->restoreSession();
	_workspace->afterStartup();
	[self hostSplitChanged:_workspace->splitVisible() stacked:_workspace->splitStacked()];
	_launched = YES;
	[self openPaths:_pendingOpens];
	[_pendingOpens removeAllObjects];
	_workspace->applyCommandLine(line);
	if (line.systemTray) {
		[_window orderOut:nil];
	} else {
		[_window makeKeyAndOrderFront:nil];
		[self hostFocusPane:_workspace->focusedPane()];
		[NSApp activateIgnoringOtherApps:YES];
	}
	_backupTimer = [NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(backupTick:) userInfo:nil repeats:YES];
}

- (void)backupTick:(NSTimer *)timer { _workspace->tick(); }

- (void)application:(NSApplication *)sender openFiles:(NSArray<NSString *> *)filenames {
	if (_launched) {
		for (NSString *path in filenames) {
			BOOL isDirectory = NO;
			[NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&isDirectory];
			if (isDirectory) _workspace->addFolder(toStd(path));
			else _workspace->openPath(toStd(path));
		}
	} else {
		[_pendingOpens addObjectsFromArray:filenames];
	}
	[sender replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

- (void)applicationDidBecomeActive:(NSNotification *)notification {
	if (_launched) _workspace->applicationActivated();
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
	return _workspace->prepareToQuit() ? NSTerminateNow : NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { return YES; }

- (BOOL)windowShouldClose:(NSWindow *)window {
	if (_workspace->settings.pref("systemTray") == "close") {
		[self hostSetTray:YES];
		[_window orderOut:nil];
		return NO;
	}
	[NSApp terminate:nil];
	return NO;  // Only reached if quitting was cancelled.
}

#pragma mark Menus

- (void)menuAction:(NSMenuItem *)sender {
	OmaMenuAction *action = sender.representedObject;
	const MenuItem item = action->item;
	// With the Find panel in front, Command-W closes that rather than a tab.
	if (item.cmd == Cmd::Close && NSApp.keyWindow && NSApp.keyWindow != _window) {
		[NSApp.keyWindow performClose:nil];
		return;
	}
	_workspace->run(item.cmd, item.arg, item.text);
}

- (BOOL)validateMenuItem:(NSMenuItem *)menuItem {
	if (menuItem.action != @selector(menuAction:)) return YES;
	OmaMenuAction *action = menuItem.representedObject;
	const MenuState state = _workspace->stateOf(action->item);
	menuItem.state = state.checked ? NSControlStateValueOn : NSControlStateValueOff;
	return state.enabled && action->item.cmd != Cmd::None;
}

- (NSMenuItem *)nativeItem:(const MenuItem &)source {
	if (source.separator) return NSMenuItem.separatorItem;
	NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:toNS(source.title) action:nil keyEquivalent:@""];
	if (source.dynamic != MenuItem::Dynamic::None) {
		NSMenu *menu = [[NSMenu alloc] initWithTitle:item.title];
		menu.delegate = self;
		menu.identifier = source.dynamic == MenuItem::Dynamic::Recent ? @"recent" : @"documents";
		[menu addItemWithTitle:@"" action:nil keyEquivalent:@""];
		item.submenu = menu;
		if (source.dynamic == MenuItem::Dynamic::Documents) NSApp.windowsMenu = nil;
		return item;
	}
	if (!source.children.empty()) {
		item.submenu = [self menuFromItems:source.children title:item.title];
		return item;
	}
	OmaApplyShortcut(source.shortcut, item);
	if (!source.macAction.empty()) {
		// Standard actions go to whichever view has focus, so they also work in the Find panel.
		item.action = NSSelectorFromString(toNS(source.macAction));
		return item;
	}
	OmaMenuAction *action = [OmaMenuAction new];
	action->item = source;
	item.representedObject = action;
	item.action = @selector(menuAction:);
	item.target = self;
	return item;
}

- (NSMenu *)menuFromItems:(const std::vector<MenuItem> &)items title:(NSString *)title {
	NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
	for (const MenuItem &source : items) [menu addItem:[self nativeItem:source]];
	return menu;
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
	[menu removeAllItems];
	if ([menu.identifier isEqualToString:@"recent"]) {
		for (const MenuItem &item : _workspace->dynamicMenu(MenuItem::Dynamic::Recent)) {
			NSMenuItem *native = [self nativeItem:item];
			if (item.cmd == Cmd::OpenRecent) native.toolTip = toNS(item.text);
			[menu addItem:native];
		}
	} else {
		NSMenuItem *minimize = [menu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
		minimize.keyEquivalentModifierMask = NSEventModifierFlagCommand;
		[menu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
		[menu addItem:NSMenuItem.separatorItem];
		for (const MenuItem &item : _workspace->dynamicMenu(MenuItem::Dynamic::Documents)) [menu addItem:[self nativeItem:item]];
	}
}

- (void)buildMenus {
	NSMenu *bar = [NSMenu new];
	NSMenuItem *appItem = [bar addItemWithTitle:@"omapad" action:nil keyEquivalent:@""];
	NSMenu *app = [[NSMenu alloc] initWithTitle:@"omapad"];
	appItem.submenu = app;
	MenuItem about;
	about.title = "About omapad";
	about.cmd = Cmd::About;
	[app addItem:[self nativeItem:about]];
	[app addItem:NSMenuItem.separatorItem];
	NSMenuItem *servicesItem = [app addItemWithTitle:@"Services" action:nil keyEquivalent:@""];
	NSMenu *services = [[NSMenu alloc] initWithTitle:@"Services"];
	servicesItem.submenu = services;
	NSApp.servicesMenu = services;
	[app addItem:NSMenuItem.separatorItem];
	[app addItemWithTitle:@"Hide omapad" action:@selector(hide:) keyEquivalent:@"h"];
	NSMenuItem *others = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
	others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
	[app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
	[app addItem:NSMenuItem.separatorItem];
	[app addItemWithTitle:@"Quit omapad" action:@selector(terminate:) keyEquivalent:@"q"];
	for (const MenuItem &top : _workspace->mainMenu()) [bar addItem:[self nativeItem:top]];
	NSApp.mainMenu = bar;
}

@end
