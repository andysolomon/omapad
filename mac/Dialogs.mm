#import "Dialogs.h"

#include <algorithm>
#include <cctype>

using omapad::FormField;
using omapad::ShortcutEntry;
using omapad::Workspace;

static NSString *toNS(const std::string &s) {
	return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}
static std::string toStd(NSString *s) { return s.UTF8String ?: ""; }

#pragma mark - Shortcuts

namespace {

struct NamedKey {
	const char *name;
	unichar character;
};

const NamedKey kNamedKeys[] = {
	{"Up", NSUpArrowFunctionKey},       {"Down", NSDownArrowFunctionKey},
	{"Left", NSLeftArrowFunctionKey},   {"Right", NSRightArrowFunctionKey},
	{"PageUp", NSPageUpFunctionKey},    {"PageDown", NSPageDownFunctionKey},
	{"Home", NSHomeFunctionKey},        {"End", NSEndFunctionKey},
	{"Delete", NSDeleteFunctionKey},    {"BackSpace", 0x7F},
	{"Tab", '\t'},                      {"Return", '\r'},
	{"Space", ' '},
};

}  // namespace

std::string OmaShortcutFromEvent(NSEvent *event) {
	NSString *characters = [event charactersByApplyingModifiers:0];
	if (!characters.length) return {};
	const unichar c = [characters characterAtIndex:0];
	std::string key;
	for (const NamedKey &named : kNamedKeys)
		if (c == named.character) key = named.name;
	if (key.empty() && c >= NSF1FunctionKey && c <= NSF35FunctionKey) key = "F" + std::to_string(c - NSF1FunctionKey + 1);
	const bool isFunctionKey = key.size() >= 2 && key[0] == 'F' && std::isdigit(static_cast<unsigned char>(key[1]));
	if (key.empty()) {
		if (c >= 0xF700 || c < 0x20) return {};  // Other function keys and control characters.
		key = toStd(characters.uppercaseString);
	}
	const NSEventModifierFlags flags = event.modifierFlags;
	std::string out;
	if (flags & NSEventModifierFlagCommand) out += "Mod+";
	if (flags & NSEventModifierFlagControl) out += "Ctrl+";
	if (flags & NSEventModifierFlagOption) out += "Alt+";
	// Without Command, Control or Option, a key types text, unless it's a function key.
	if (out.empty() && !isFunctionKey) return {};
	if (flags & NSEventModifierFlagShift) out += "Shift+";
	return out + key;
}

void OmaApplyShortcut(const std::string &shortcut, NSMenuItem *item) {
	if (shortcut.empty()) return;
	NSEventModifierFlags mods = 0;
	NSString *key = nil;
	NSString *text = toNS(shortcut);
	// "Mod++" means the plus key, which splitting on + would lose.
	if ([text hasSuffix:@"++"]) {
		key = @"+";
		text = [text substringToIndex:text.length - 2];
	}
	for (NSString *part in [text componentsSeparatedByString:@"+"]) {
		if ([part isEqualToString:@"Mod"]) mods |= NSEventModifierFlagCommand;
		else if ([part isEqualToString:@"Ctrl"]) mods |= NSEventModifierFlagControl;
		else if ([part isEqualToString:@"Alt"]) mods |= NSEventModifierFlagOption;
		else if ([part isEqualToString:@"Shift"]) mods |= NSEventModifierFlagShift;
		else if (part.length) key = part;
	}
	if (!key.length) return;
	bool named = false;
	for (const NamedKey &entry : kNamedKeys) {
		if ([key isEqualToString:@(entry.name)]) {
			key = [NSString stringWithCharacters:&entry.character length:1];
			named = true;
			break;
		}
	}
	if (!named) {
		const NSInteger number = key.length >= 2 && [key hasPrefix:@"F"] ? [key substringFromIndex:1].integerValue : 0;
		if (number >= 1) {
			const unichar character = static_cast<unichar>(NSF1FunctionKey + number - 1);
			key = [NSString stringWithCharacters:&character length:1];
		} else {
			key = key.lowercaseString;
		}
	}
	item.keyEquivalent = key;
	item.keyEquivalentModifierMask = mods;
}

#pragma mark - Form

@interface OmaFormDelegate : NSObject <NSWindowDelegate>
@end
@implementation OmaFormDelegate
- (void)accept:(id)sender { [NSApp stopModalWithCode:NSModalResponseOK]; }
- (void)cancel:(id)sender { [NSApp stopModalWithCode:NSModalResponseCancel]; }
- (BOOL)windowShouldClose:(NSWindow *)window {
	[NSApp stopModalWithCode:NSModalResponseCancel];
	return NO;
}
- (void)stepperChanged:(NSStepper *)stepper {
	NSTextField *field = (NSTextField *)stepper.superview.subviews.firstObject;
	field.integerValue = stepper.integerValue;
}
@end

// Stack views grow from the bottom unless the document view is flipped.
@interface OmaFlippedView : NSView
@end
@implementation OmaFlippedView
- (BOOL)isFlipped { return YES; }
@end

BOOL OmaRunForm(NSString *title, std::vector<FormField> &fields) {
	OmaFormDelegate *delegate = [OmaFormDelegate new];
	NSPanel *window = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 520, 580)
	                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
	                                               backing:NSBackingStoreBuffered
	                                                 defer:NO];
	window.title = title;
	window.delegate = delegate;
	window.releasedWhenClosed = NO;
	window.minSize = NSMakeSize(420, 240);

	NSMutableArray<NSView *> *controls = [NSMutableArray array];  // One per field, or NSNull's stand-in.
	NSStackView *rows = [NSStackView new];
	rows.orientation = NSUserInterfaceLayoutOrientationVertical;
	rows.alignment = NSLayoutAttributeLeading;
	rows.spacing = 8;
	rows.edgeInsets = NSEdgeInsetsMake(14, 18, 14, 18);
	rows.translatesAutoresizingMaskIntoConstraints = NO;

	const auto labelled = [](NSString *text, NSView *control) {
		NSTextField *label = [NSTextField labelWithString:text];
		label.alignment = NSTextAlignmentLeft;
		[label setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
		NSStackView *row = [NSStackView stackViewWithViews:@[label, control]];
		row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
		row.alignment = NSLayoutAttributeCenterY;
		row.spacing = 8;
		return row;
	};

	for (size_t i = 0; i < fields.size(); i++) {
		const FormField &field = fields[i];
		NSView *control = [NSView new];
		switch (field.kind) {
		case FormField::Kind::Heading: {
			NSTextField *heading = [NSTextField labelWithString:toNS(field.label)];
			heading.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
			if (i > 0) [rows setCustomSpacing:18 afterView:rows.arrangedSubviews.lastObject];
			[rows addArrangedSubview:heading];
			break;
		}
		case FormField::Kind::Bool: {
			NSButton *box = [NSButton checkboxWithTitle:toNS(field.label) target:nil action:nil];
			box.state = field.value == "1" ? NSControlStateValueOn : NSControlStateValueOff;
			[rows addArrangedSubview:box];
			control = box;
			break;
		}
		case FormField::Kind::Int: {
			NSTextField *text = [NSTextField textFieldWithString:toNS(field.value)];
			[text.widthAnchor constraintEqualToConstant:90].active = YES;
			NSStepper *stepper = [NSStepper new];
			stepper.minValue = field.minimum;
			stepper.maxValue = field.maximum;
			stepper.integerValue = text.integerValue;
			stepper.valueWraps = NO;
			stepper.target = delegate;
			stepper.action = @selector(stepperChanged:);
			NSView *pair = [NSView new];
			text.translatesAutoresizingMaskIntoConstraints = NO;
			stepper.translatesAutoresizingMaskIntoConstraints = NO;
			[pair addSubview:text];
			[pair addSubview:stepper];
			[NSLayoutConstraint activateConstraints:@[
				[text.leadingAnchor constraintEqualToAnchor:pair.leadingAnchor],
				[text.topAnchor constraintEqualToAnchor:pair.topAnchor],
				[text.bottomAnchor constraintEqualToAnchor:pair.bottomAnchor],
				[stepper.leadingAnchor constraintEqualToAnchor:text.trailingAnchor constant:4],
				[stepper.centerYAnchor constraintEqualToAnchor:text.centerYAnchor],
				[stepper.trailingAnchor constraintEqualToAnchor:pair.trailingAnchor],
			]];
			[rows addArrangedSubview:labelled(toNS(field.label), pair)];
			control = text;
			break;
		}
		case FormField::Kind::Text: {
			NSTextField *text = [NSTextField textFieldWithString:toNS(field.value)];
			[text.widthAnchor constraintGreaterThanOrEqualToConstant:260].active = YES;
			[rows addArrangedSubview:labelled(toNS(field.label), text)];
			control = text;
			break;
		}
		case FormField::Kind::Choice: {
			NSPopUpButton *popup = [NSPopUpButton new];
			for (const auto &choice : field.choices) {
				[popup addItemWithTitle:toNS(choice.second)];
				if (choice.first == field.value) [popup selectItem:popup.lastItem];
			}
			[rows addArrangedSubview:labelled(toNS(field.label), popup)];
			control = popup;
			break;
		}
		}
		[controls addObject:control];
	}

	OmaFlippedView *document = [OmaFlippedView new];
	document.translatesAutoresizingMaskIntoConstraints = NO;
	[document addSubview:rows];
	NSScrollView *scroll = [NSScrollView new];
	scroll.documentView = document;
	scroll.hasVerticalScroller = YES;
	scroll.autohidesScrollers = YES;
	scroll.borderType = NSNoBorder;
	scroll.drawsBackground = NO;
	scroll.translatesAutoresizingMaskIntoConstraints = NO;

	NSButton *ok = [NSButton buttonWithTitle:@"OK" target:delegate action:@selector(accept:)];
	ok.keyEquivalent = @"\r";
	NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:delegate action:@selector(cancel:)];
	cancel.keyEquivalent = @"\033";
	NSStackView *buttons = [NSStackView stackViewWithViews:@[cancel, ok]];
	buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
	buttons.translatesAutoresizingMaskIntoConstraints = NO;

	NSView *content = window.contentView;
	[content addSubview:scroll];
	[content addSubview:buttons];
	[NSLayoutConstraint activateConstraints:@[
		[scroll.topAnchor constraintEqualToAnchor:content.topAnchor],
		[scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
		[scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
		[scroll.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-10],
		[buttons.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-14],
		[buttons.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-12],
		[document.topAnchor constraintEqualToAnchor:scroll.contentView.topAnchor],
		[document.leadingAnchor constraintEqualToAnchor:scroll.contentView.leadingAnchor],
		[document.trailingAnchor constraintEqualToAnchor:scroll.contentView.trailingAnchor],
		[rows.topAnchor constraintEqualToAnchor:document.topAnchor],
		[rows.leadingAnchor constraintEqualToAnchor:document.leadingAnchor],
		[rows.trailingAnchor constraintLessThanOrEqualToAnchor:document.trailingAnchor],
		[rows.bottomAnchor constraintEqualToAnchor:document.bottomAnchor],
	]];

	// Short forms don't need all the room a long one does.
	[content layoutSubtreeIfNeeded];
	const CGFloat wanted = std::min<CGFloat>(rows.fittingSize.height + 54, 620);
	[window setContentSize:NSMakeSize(520, wanted)];
	[window center];

	const NSInteger response = [NSApp runModalForWindow:window];
	// Finish any edit in progress, so what was just typed counts.
	[window makeFirstResponder:nil];
	[window orderOut:nil];
	if (response != NSModalResponseOK) return NO;

	for (size_t i = 0; i < fields.size(); i++) {
		FormField &field = fields[i];
		NSView *control = controls[i];
		switch (field.kind) {
		case FormField::Kind::Heading: break;
		case FormField::Kind::Bool: field.value = ((NSButton *)control).state == NSControlStateValueOn ? "1" : "0"; break;
		case FormField::Kind::Int: {
			const NSInteger value = std::min<NSInteger>(std::max<NSInteger>(((NSTextField *)control).integerValue, field.minimum), field.maximum);
			field.value = std::to_string(value);
			break;
		}
		case FormField::Kind::Text: field.value = toStd(((NSTextField *)control).stringValue); break;
		case FormField::Kind::Choice: {
			const NSInteger index = ((NSPopUpButton *)control).indexOfSelectedItem;
			if (index >= 0 && index < static_cast<NSInteger>(field.choices.size())) field.value = field.choices[index].first;
			break;
		}
		}
	}
	return YES;
}

#pragma mark - Shortcut Mapper

@interface OmaShortcutMapper : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate>
- (instancetype)initWithWorkspace:(Workspace *)workspace;
- (void)run;
@end

@implementation OmaShortcutMapper {
	Workspace *_workspace;
	std::vector<ShortcutEntry> _entries;
	std::vector<size_t> _visible;
	NSPanel *_window;
	NSTableView *_table;
	NSSearchField *_search;
	NSTextField *_hint;
	id _monitor;
}

- (instancetype)initWithWorkspace:(Workspace *)workspace {
	if ((self = [super init])) {
		_workspace = workspace;
		[self buildWindow];
		[self reload];
	}
	return self;
}

- (NSButton *)button:(NSString *)title action:(SEL)action {
	return [NSButton buttonWithTitle:title target:self action:action];
}

- (void)buildWindow {
	_window = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 640, 560)
	                                     styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
	                                       backing:NSBackingStoreBuffered
	                                         defer:NO];
	_window.title = @"Shortcut Mapper";
	_window.delegate = self;
	_window.releasedWhenClosed = NO;
	_window.minSize = NSMakeSize(480, 300);
	[_window center];

	_search = [NSSearchField new];
	_search.placeholderString = @"Filter commands or shortcuts";
	_search.delegate = self;
	_search.translatesAutoresizingMaskIntoConstraints = NO;

	_table = [NSTableView new];
	_table.dataSource = self;
	_table.delegate = self;
	_table.usesAlternatingRowBackgroundColors = YES;
	_table.allowsMultipleSelection = NO;
	_table.target = self;
	_table.doubleAction = @selector(setShortcut:);
	NSTableColumn *command = [[NSTableColumn alloc] initWithIdentifier:@"command"];
	command.title = @"Command";
	command.width = 420;
	NSTableColumn *shortcut = [[NSTableColumn alloc] initWithIdentifier:@"shortcut"];
	shortcut.title = @"Shortcut";
	shortcut.width = 170;
	[_table addTableColumn:command];
	[_table addTableColumn:shortcut];

	NSScrollView *scroll = [NSScrollView new];
	scroll.documentView = _table;
	scroll.hasVerticalScroller = YES;
	scroll.borderType = NSBezelBorder;
	scroll.translatesAutoresizingMaskIntoConstraints = NO;

	_hint = [NSTextField labelWithString:@"Double-click a command, then press its new shortcut."];
	_hint.font = [NSFont systemFontOfSize:11];
	_hint.textColor = NSColor.secondaryLabelColor;
	_hint.translatesAutoresizingMaskIntoConstraints = NO;

	NSButton *done = [self button:@"Done" action:@selector(done:)];
	done.keyEquivalent = @"\r";
	NSStackView *buttons = [NSStackView stackViewWithViews:@[
		[self button:@"Set…" action:@selector(setShortcut:)],
		[self button:@"Clear" action:@selector(clearShortcut:)],
		[self button:@"Reset" action:@selector(resetShortcut:)],
		[self button:@"Reset All…" action:@selector(resetAll:)],
		[NSView new],
		done,
	]];
	buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
	buttons.translatesAutoresizingMaskIntoConstraints = NO;

	NSView *content = _window.contentView;
	[content addSubview:_search];
	[content addSubview:scroll];
	[content addSubview:_hint];
	[content addSubview:buttons];
	[NSLayoutConstraint activateConstraints:@[
		[_search.topAnchor constraintEqualToAnchor:content.topAnchor constant:12],
		[_search.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:14],
		[_search.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-14],
		[scroll.topAnchor constraintEqualToAnchor:_search.bottomAnchor constant:10],
		[scroll.leadingAnchor constraintEqualToAnchor:_search.leadingAnchor],
		[scroll.trailingAnchor constraintEqualToAnchor:_search.trailingAnchor],
		[_hint.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:8],
		[_hint.leadingAnchor constraintEqualToAnchor:_search.leadingAnchor],
		[_hint.trailingAnchor constraintEqualToAnchor:_search.trailingAnchor],
		[buttons.topAnchor constraintEqualToAnchor:_hint.bottomAnchor constant:8],
		[buttons.leadingAnchor constraintEqualToAnchor:_search.leadingAnchor],
		[buttons.trailingAnchor constraintEqualToAnchor:_search.trailingAnchor],
		[buttons.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-12],
	]];
}

- (void)reload {
	_entries = _workspace->shortcutEntries();
	[self applyFilter];
}

- (void)applyFilter {
	const std::string selected = [self selectedEntry] ? [self selectedEntry]->id : std::string();
	std::string filter = toStd(_search.stringValue);
	const auto lower = [](std::string value) {
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value;
	};
	filter = lower(filter);
	_visible.clear();
	for (size_t i = 0; i < _entries.size(); i++) {
		if (filter.empty() || lower(_entries[i].id).find(filter) != std::string::npos || lower(_entries[i].shortcut).find(filter) != std::string::npos)
			_visible.push_back(i);
	}
	[_table reloadData];
	for (size_t row = 0; row < _visible.size(); row++) {
		if (_entries[_visible[row]].id == selected) {
			[_table selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
			[_table scrollRowToVisible:row];
			break;
		}
	}
}

- (const ShortcutEntry *)selectedEntry {
	const NSInteger row = _table.selectedRow;
	if (row < 0 || row >= static_cast<NSInteger>(_visible.size()) || _visible[row] >= _entries.size()) return nullptr;
	return &_entries[_visible[row]];
}

- (void)controlTextDidChange:(NSNotification *)note { [self applyFilter]; }

- (NSInteger)numberOfRowsInTableView:(NSTableView *)view { return static_cast<NSInteger>(_visible.size()); }

- (NSView *)tableView:(NSTableView *)view viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
	NSTextField *cell = [view makeViewWithIdentifier:column.identifier owner:self];
	if (!cell) {
		cell = [NSTextField labelWithString:@""];
		cell.identifier = column.identifier;
		cell.lineBreakMode = NSLineBreakByTruncatingMiddle;
	}
	const ShortcutEntry &entry = _entries[_visible[row]];
	const bool isCommand = [column.identifier isEqualToString:@"command"];
	if (isCommand) {
		NSString *path = [toNS(entry.id) stringByReplacingOccurrencesOfString:@"/" withString:@" › "];
		cell.stringValue = path;
	} else {
		cell.stringValue = [self displayShortcut:entry.shortcut];
		// Changed ones stand out by weight, since a colour would vanish on a selected row.
		cell.font = [NSFont systemFontOfSize:NSFont.systemFontSize weight:entry.shortcut != entry.defaultShortcut ? NSFontWeightBold : NSFontWeightRegular];
	}
	return cell;
}

- (NSString *)displayShortcut:(const std::string &)shortcut {
	NSString *text = toNS(shortcut);
	text = [text stringByReplacingOccurrencesOfString:@"Mod+" withString:@"⌘"];
	text = [text stringByReplacingOccurrencesOfString:@"Ctrl+" withString:@"⌃"];
	text = [text stringByReplacingOccurrencesOfString:@"Alt+" withString:@"⌥"];
	text = [text stringByReplacingOccurrencesOfString:@"Shift+" withString:@"⇧"];
	return text;
}

- (void)setShortcut:(id)sender {
	const ShortcutEntry *entry = [self selectedEntry];
	if (!entry) return;
	[self stopCapture];
	const std::string identifier = entry->id;
	_hint.stringValue = @"Press the new shortcut, or Esc to cancel.";
	_hint.textColor = NSColor.controlAccentColor;
	__weak OmaShortcutMapper *weakSelf = self;
	_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
		OmaShortcutMapper *strong = weakSelf;
		if (!strong) return event;
		if (event.keyCode == 53) {  // Escape.
			[strong stopCapture];
			return nil;
		}
		const std::string shortcut = OmaShortcutFromEvent(event);
		if (shortcut.empty()) {
			strong->_hint.stringValue = @"That key types text. Add Command, Control or Option, or press Esc to cancel.";
			return nil;
		}
		[strong stopCapture];
		strong->_workspace->assignShortcut(identifier, shortcut);
		[strong reload];
		return nil;
	}];
}

- (void)stopCapture {
	if (_monitor) [NSEvent removeMonitor:_monitor];
	_monitor = nil;
	_hint.stringValue = @"Double-click a command, then press its new shortcut.";
	_hint.textColor = NSColor.secondaryLabelColor;
}

- (void)clearShortcut:(id)sender {
	const ShortcutEntry *entry = [self selectedEntry];
	if (!entry) return;
	[self stopCapture];
	const std::string identifier = entry->id;
	_workspace->assignShortcut(identifier, "");
	[self reload];
}

- (void)resetShortcut:(id)sender {
	const ShortcutEntry *entry = [self selectedEntry];
	if (!entry) return;
	[self stopCapture];
	const std::string identifier = entry->id, shortcut = entry->defaultShortcut;
	_workspace->assignShortcut(identifier, shortcut);
	[self reload];
}

- (void)resetAll:(id)sender {
	[self stopCapture];
	NSAlert *alert = [NSAlert new];
	alert.messageText = @"Reset every shortcut to its default?";
	[alert addButtonWithTitle:@"Cancel"];
	[alert addButtonWithTitle:@"Reset All"];
	if ([alert runModal] != NSAlertSecondButtonReturn) return;
	_workspace->resetShortcuts();
	[self reload];
}

- (void)done:(id)sender {
	[self stopCapture];
	[NSApp stopModal];
}

- (BOOL)windowShouldClose:(NSWindow *)window {
	[self stopCapture];
	[NSApp stopModal];
	return NO;
}

- (void)run {
	[NSApp runModalForWindow:_window];
	[_window orderOut:nil];
}

@end

void OmaRunShortcutMapper(Workspace &workspace) {
	OmaShortcutMapper *mapper = [[OmaShortcutMapper alloc] initWithWorkspace:&workspace];
	[mapper run];
}
