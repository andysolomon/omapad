#import "FolderPanel.h"

using omapad::FolderEntry;
using omapad::Workspace;

static NSString *toNS(const std::string &s) {
	return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

@interface OmaFolderNode : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSString *path;
@property(nonatomic) BOOL isDirectory;
@property(nonatomic) BOOL isRoot;
@property(nonatomic, strong) NSArray<OmaFolderNode *> *children;  // Nil until first needed.
@end
@implementation OmaFolderNode
@end

@class FolderOutline;
@protocol FolderOutlineEvents <NSObject>
- (NSMenu *)outline:(FolderOutline *)outline menuForRow:(NSInteger)row;
- (void)outlineActivateSelection:(FolderOutline *)outline;
@end

@interface FolderOutline : NSOutlineView
@property(nonatomic, weak) id<FolderOutlineEvents> events;
@end
@implementation FolderOutline
- (NSMenu *)menuForEvent:(NSEvent *)event {
	const NSInteger row = [self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
	if (row >= 0) [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
	return row >= 0 ? [self.events outline:self menuForRow:row] : nil;
}
- (void)keyDown:(NSEvent *)event {
	if (event.keyCode == 36 || event.keyCode == 76) {  // Return and Enter.
		[self.events outlineActivateSelection:self];
		return;
	}
	[super keyDown:event];
}
@end

@interface FolderPanel () <NSOutlineViewDataSource, NSOutlineViewDelegate, FolderOutlineEvents>
@end

@implementation FolderPanel {
	Workspace *_workspace;
	FolderOutline *_outline;
	NSMutableArray<OmaFolderNode *> *_roots;
	BOOL _reloading;
}

- (instancetype)initWithWorkspace:(Workspace *)workspace {
	if ((self = [super initWithFrame:NSMakeRect(0, 0, 240, 600)])) {
		_workspace = workspace;
		_roots = [NSMutableArray array];

		NSTextField *title = [NSTextField labelWithString:@"WORKSPACE"];
		title.font = [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold];
		title.textColor = NSColor.secondaryLabelColor;
		NSButton *add = [NSButton buttonWithImage:[NSImage imageNamed:NSImageNameAddTemplate] target:self action:@selector(addFolder:)];
		add.bordered = NO;
		add.toolTip = @"Add a folder";
		NSButton *refresh = [NSButton buttonWithImage:[NSImage imageNamed:NSImageNameRefreshTemplate] target:self action:@selector(refresh:)];
		refresh.bordered = NO;
		refresh.toolTip = @"Refresh";
		NSStackView *header = [NSStackView stackViewWithViews:@[title, [NSView new], refresh, add]];
		header.orientation = NSUserInterfaceLayoutOrientationHorizontal;
		header.edgeInsets = NSEdgeInsetsMake(0, 10, 0, 8);
		header.translatesAutoresizingMaskIntoConstraints = NO;

		_outline = [FolderOutline new];
		_outline.events = self;
		_outline.headerView = nil;
		_outline.dataSource = self;
		_outline.delegate = self;
		_outline.rowSizeStyle = NSTableViewRowSizeStyleSmall;
		_outline.floatsGroupRows = NO;
		_outline.autoresizesOutlineColumn = NO;
		_outline.target = self;
		_outline.doubleAction = @selector(doubleClicked:);
		_outline.style = NSTableViewStyleSourceList;
		NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"name"];
		column.resizingMask = NSTableColumnAutoresizingMask;
		[_outline addTableColumn:column];
		_outline.outlineTableColumn = column;
		_outline.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;

		NSScrollView *scroll = [NSScrollView new];
		scroll.documentView = _outline;
		scroll.hasVerticalScroller = YES;
		scroll.autohidesScrollers = YES;
		scroll.drawsBackground = NO;
		scroll.translatesAutoresizingMaskIntoConstraints = NO;

		[self addSubview:header];
		[self addSubview:scroll];
		[NSLayoutConstraint activateConstraints:@[
			[header.topAnchor constraintEqualToAnchor:self.topAnchor],
			[header.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
			[header.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
			[header.heightAnchor constraintEqualToConstant:30],
			[scroll.topAnchor constraintEqualToAnchor:header.bottomAnchor],
			[scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
			[scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
			[scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
		]];
	}
	return self;
}

- (NSArray<OmaFolderNode *> *)childrenOf:(OmaFolderNode *)node {
	if (!node.children) {
		NSMutableArray *children = [NSMutableArray array];
		for (const FolderEntry &entry : _workspace->listFolder(node.path.UTF8String)) {
			OmaFolderNode *child = [OmaFolderNode new];
			child.name = toNS(entry.name);
			child.path = toNS(entry.path);
			child.isDirectory = entry.isDirectory;
			[children addObject:child];
		}
		node.children = children;
	}
	return node.children;
}

- (void)reload {
	NSString *selected = nil;
	const NSInteger row = _outline.selectedRow;
	if (row >= 0) selected = [[_outline itemAtRow:row] path];
	_reloading = YES;
	[_roots removeAllObjects];
	for (const std::string &root : _workspace->folders()) {
		OmaFolderNode *node = [OmaFolderNode new];
		node.path = toNS(root);
		node.name = node.path.lastPathComponent;
		node.isDirectory = YES;
		node.isRoot = YES;
		[_roots addObject:node];
	}
	[_outline reloadData];
	for (OmaFolderNode *node in _roots) [self restoreExpansion:node];
	_reloading = NO;
	if (selected) {
		for (NSInteger i = 0; i < _outline.numberOfRows; i++) {
			if ([[[_outline itemAtRow:i] path] isEqualToString:selected]) {
				[_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
				break;
			}
		}
	}
}

- (void)restoreExpansion:(OmaFolderNode *)node {
	if (!node.isDirectory || !_workspace->isFolderExpanded(node.path.UTF8String)) return;
	[_outline expandItem:node];
	for (OmaFolderNode *child in [self childrenOf:node]) [self restoreExpansion:child];
}

- (NSInteger)outlineView:(NSOutlineView *)view numberOfChildrenOfItem:(id)item {
	return item ? [self childrenOf:item].count : _roots.count;
}
- (id)outlineView:(NSOutlineView *)view child:(NSInteger)index ofItem:(id)item {
	return item ? [self childrenOf:item][index] : _roots[index];
}
- (BOOL)outlineView:(NSOutlineView *)view isItemExpandable:(id)item { return [item isDirectory]; }

- (NSView *)outlineView:(NSOutlineView *)view viewForTableColumn:(NSTableColumn *)column item:(id)item {
	OmaFolderNode *node = item;
	NSTableCellView *cell = [view makeViewWithIdentifier:@"cell" owner:self];
	if (!cell) {
		cell = [NSTableCellView new];
		cell.identifier = @"cell";
		NSImageView *image = [NSImageView new];
		NSTextField *text = [NSTextField labelWithString:@""];
		text.lineBreakMode = NSLineBreakByTruncatingMiddle;
		text.font = [NSFont systemFontOfSize:12];
		image.translatesAutoresizingMaskIntoConstraints = NO;
		text.translatesAutoresizingMaskIntoConstraints = NO;
		[cell addSubview:image];
		[cell addSubview:text];
		cell.imageView = image;
		cell.textField = text;
		[NSLayoutConstraint activateConstraints:@[
			[image.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor],
			[image.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
			[image.widthAnchor constraintEqualToConstant:16],
			[image.heightAnchor constraintEqualToConstant:16],
			[text.leadingAnchor constraintEqualToAnchor:image.trailingAnchor constant:4],
			[text.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor],
			[text.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
		]];
	}
	cell.textField.stringValue = node.name;
	cell.textField.font = node.isRoot ? [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold] : [NSFont systemFontOfSize:12];
	cell.imageView.image = [NSWorkspace.sharedWorkspace iconForFile:node.path];
	cell.toolTip = node.path;
	return cell;
}

- (void)outlineViewItemDidExpand:(NSNotification *)note {
	if (_reloading) return;
	_workspace->setFolderExpanded([note.userInfo[@"NSObject"] path].UTF8String, true);
}

- (void)outlineViewItemDidCollapse:(NSNotification *)note {
	if (_reloading) return;
	_workspace->setFolderExpanded([note.userInfo[@"NSObject"] path].UTF8String, false);
}

- (void)activate:(OmaFolderNode *)node {
	if (!node) return;
	if (node.isDirectory) {
		if ([_outline isItemExpanded:node]) [_outline collapseItem:node];
		else [_outline expandItem:node];
	} else {
		_workspace->openPath(node.path.UTF8String);
	}
}

- (void)doubleClicked:(id)sender {
	if (_outline.clickedRow >= 0) [self activate:[_outline itemAtRow:_outline.clickedRow]];
}

- (void)outlineActivateSelection:(FolderOutline *)outline {
	if (outline.selectedRow >= 0) [self activate:[outline itemAtRow:outline.selectedRow]];
}

- (NSMenu *)outline:(FolderOutline *)outline menuForRow:(NSInteger)row {
	OmaFolderNode *node = [outline itemAtRow:row];
	if (!self.menuBuilder) return nil;
	return self.menuBuilder(_workspace->folderMenu(node.path.UTF8String, node.isDirectory, node.isRoot));
}

- (void)addFolder:(id)sender { _workspace->run(omapad::Cmd::OpenFolder); }

- (NSView *)focusTarget { return _outline; }

- (void)selectPath:(NSString *)path {
	for (NSInteger row = 0; row < _outline.numberOfRows; row++) {
		OmaFolderNode *node = [_outline itemAtRow:row];
		if (![node.path isEqualToString:path]) continue;
		[_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
		[_outline scrollRowToVisible:row];
		[self.window makeFirstResponder:_outline];
		return;
	}
}
- (void)refresh:(id)sender { [self reload]; }

@end
