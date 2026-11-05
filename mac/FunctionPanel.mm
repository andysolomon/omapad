#import "FunctionPanel.h"

#include <algorithm>
#include <cctype>

using omapad::FunctionEntry;
using omapad::Workspace;

static NSString *toNS(const std::string &s) {
	return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

@interface OmaFunctionNode : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic) NSInteger index;  // Into the workspace's list. -1 for a class.
@property(nonatomic, strong) NSMutableArray<OmaFunctionNode *> *children;
@end
@implementation OmaFunctionNode
@end

@interface OmaFunctionOutline : NSOutlineView
@property(nonatomic, copy) void (^activate)(void);
@end
@implementation OmaFunctionOutline
- (void)keyDown:(NSEvent *)event {
	if (event.keyCode == 36 || event.keyCode == 76) {  // Return and Enter.
		if (self.activate) self.activate();
		return;
	}
	[super keyDown:event];
}
@end

@interface FunctionPanel () <NSOutlineViewDataSource, NSOutlineViewDelegate, NSSearchFieldDelegate>
@end

@implementation FunctionPanel {
	Workspace *_workspace;
	OmaFunctionOutline *_outline;
	NSSearchField *_filter;
	NSTextField *_empty;
	NSButton *_sort;
	NSMutableArray<OmaFunctionNode *> *_roots;
	NSMutableSet<NSString *> *_collapsed;
}

- (instancetype)initWithWorkspace:(Workspace *)workspace {
	if ((self = [super initWithFrame:NSMakeRect(0, 0, 240, 600)])) {
		_workspace = workspace;
		_roots = [NSMutableArray array];
		_collapsed = [NSMutableSet set];

		NSTextField *title = [NSTextField labelWithString:@"FUNCTIONS"];
		title.font = [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold];
		title.textColor = NSColor.secondaryLabelColor;
		_sort = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"textformat.abc" accessibilityDescription:@"Sort by name"]
		                           target:self
		                           action:@selector(reload)];
		_sort.buttonType = NSButtonTypePushOnPushOff;
		_sort.bordered = NO;
		_sort.toolTip = @"Sort by name";
		NSButton *refresh = [NSButton buttonWithImage:[NSImage imageNamed:NSImageNameRefreshTemplate] target:self action:@selector(refresh:)];
		refresh.bordered = NO;
		refresh.toolTip = @"Refresh";
		NSStackView *header = [NSStackView stackViewWithViews:@[title, [NSView new], _sort, refresh]];
		header.orientation = NSUserInterfaceLayoutOrientationHorizontal;
		header.edgeInsets = NSEdgeInsetsMake(0, 10, 0, 8);
		header.translatesAutoresizingMaskIntoConstraints = NO;

		_filter = [NSSearchField new];
		_filter.placeholderString = @"Filter";
		_filter.controlSize = NSControlSizeSmall;
		_filter.font = [NSFont systemFontOfSize:11];
		_filter.delegate = self;
		_filter.translatesAutoresizingMaskIntoConstraints = NO;

		_outline = [OmaFunctionOutline new];
		_outline.headerView = nil;
		_outline.dataSource = self;
		_outline.delegate = self;
		_outline.rowSizeStyle = NSTableViewRowSizeStyleSmall;
		_outline.autoresizesOutlineColumn = NO;
		_outline.target = self;
		_outline.doubleAction = @selector(doubleClicked:);
		_outline.style = NSTableViewStyleSourceList;
		__weak FunctionPanel *weakSelf = self;
		_outline.activate = ^{
			FunctionPanel *strong = weakSelf;
			if (strong) [strong goToRow:strong->_outline.selectedRow];
		};
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

		_empty = [NSTextField wrappingLabelWithString:@""];
		_empty.font = [NSFont systemFontOfSize:11];
		_empty.textColor = NSColor.secondaryLabelColor;
		_empty.alignment = NSTextAlignmentCenter;
		_empty.translatesAutoresizingMaskIntoConstraints = NO;

		[self addSubview:header];
		[self addSubview:_filter];
		[self addSubview:scroll];
		[self addSubview:_empty];
		[NSLayoutConstraint activateConstraints:@[
			[header.topAnchor constraintEqualToAnchor:self.topAnchor],
			[header.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
			[header.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
			[header.heightAnchor constraintEqualToConstant:30],
			[_filter.topAnchor constraintEqualToAnchor:header.bottomAnchor],
			[_filter.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:8],
			[_filter.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-8],
			[scroll.topAnchor constraintEqualToAnchor:_filter.bottomAnchor constant:6],
			[scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
			[scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
			[scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
			[_empty.topAnchor constraintEqualToAnchor:_filter.bottomAnchor constant:18],
			[_empty.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14],
			[_empty.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-14],
		]];
	}
	return self;
}

- (void)reload {
	const std::vector<FunctionEntry> &entries = _workspace->functionEntries();
	const auto lower = [](std::string value) {
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value;
	};
	const std::string filter = lower(_filter.stringValue.UTF8String ?: "");
	NSMutableArray<OmaFunctionNode *> *roots = [NSMutableArray array];
	NSMutableDictionary<NSString *, OmaFunctionNode *> *classes = [NSMutableDictionary dictionary];
	for (size_t i = 0; i < entries.size(); i++) {
		const FunctionEntry &entry = entries[i];
		if (!filter.empty() && lower(entry.name).find(filter) == std::string::npos && lower(entry.className).find(filter) == std::string::npos) continue;
		OmaFunctionNode *node = [OmaFunctionNode new];
		node.name = toNS(entry.name);
		node.index = static_cast<NSInteger>(i);
		if (entry.className.empty()) {
			[roots addObject:node];
			continue;
		}
		NSString *className = toNS(entry.className);
		OmaFunctionNode *parent = classes[className];
		if (!parent) {
			parent = [OmaFunctionNode new];
			parent.name = className;
			parent.index = -1;
			parent.children = [NSMutableArray array];
			classes[className] = parent;
			[roots addObject:parent];
		}
		[parent.children addObject:node];
	}
	if (_sort.state == NSControlStateValueOn) {
		NSSortDescriptor *byName = [NSSortDescriptor sortDescriptorWithKey:@"name" ascending:YES selector:@selector(localizedCaseInsensitiveCompare:)];
		[roots sortUsingDescriptors:@[byName]];
		for (OmaFunctionNode *node in roots) [node.children sortUsingDescriptors:@[byName]];
	}
	_roots = roots;
	[_outline reloadData];
	for (OmaFunctionNode *node in roots)
		if (node.children && ![_collapsed containsObject:node.name]) [_outline expandItem:node];

	const bool nothing = roots.count == 0;
	_empty.hidden = !nothing;
	if (nothing) {
		if (!filter.empty()) _empty.stringValue = @"Nothing matches the filter.";
		else if (!_workspace->activeLanguageHasFunctionList()) _empty.stringValue = @"This language has no function list.";
		else _empty.stringValue = @"No functions found.";
	}
}

- (void)controlTextDidChange:(NSNotification *)note { [self reload]; }

- (void)refresh:(id)sender { _workspace->refreshFunctionList(); }

- (void)goToRow:(NSInteger)row {
	if (row < 0) return;
	OmaFunctionNode *node = [_outline itemAtRow:row];
	if (node.index >= 0) {
		_workspace->goToFunction(static_cast<size_t>(node.index));
	} else if ([_outline isItemExpanded:node]) {
		[_outline collapseItem:node];
	} else {
		[_outline expandItem:node];
	}
}

- (void)doubleClicked:(id)sender { [self goToRow:_outline.clickedRow]; }

- (NSInteger)outlineView:(NSOutlineView *)view numberOfChildrenOfItem:(id)item {
	return item ? [item children].count : _roots.count;
}
- (id)outlineView:(NSOutlineView *)view child:(NSInteger)index ofItem:(id)item {
	return item ? [item children][index] : _roots[index];
}
- (BOOL)outlineView:(NSOutlineView *)view isItemExpandable:(id)item { return [item children] != nil; }

- (NSView *)outlineView:(NSOutlineView *)view viewForTableColumn:(NSTableColumn *)column item:(id)item {
	OmaFunctionNode *node = item;
	NSTextField *cell = [view makeViewWithIdentifier:@"cell" owner:self];
	if (!cell) {
		cell = [NSTextField labelWithString:@""];
		cell.identifier = @"cell";
		cell.lineBreakMode = NSLineBreakByTruncatingTail;
	}
	cell.stringValue = node.name;
	cell.toolTip = node.name;
	cell.font = node.children ? [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold] : [NSFont systemFontOfSize:12];
	return cell;
}

- (void)outlineViewItemDidCollapse:(NSNotification *)note { [_collapsed addObject:[note.userInfo[@"NSObject"] name]]; }
- (void)outlineViewItemDidExpand:(NSNotification *)note { [_collapsed removeObject:[note.userInfo[@"NSObject"] name]]; }

@end
