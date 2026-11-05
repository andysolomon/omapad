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

// A table whose right-click menu depends on the row.
@interface OmaMenuTable : NSTableView
@property(nonatomic, copy) NSMenu *(^menuForRow)(NSInteger row);
@end
@implementation OmaMenuTable
- (NSMenu *)menuForEvent:(NSEvent *)event {
	const NSInteger row = [self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
	if (row >= 0) [self selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];
	return self.menuForRow ? self.menuForRow(row) : nil;
}
@end

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

static NSColor *colourFromHex(const std::string &hex) {
	const unsigned long value = std::strtoul(hex.c_str(), nullptr, 16);
	return [NSColor colorWithSRGBRed:((value >> 16) & 0xFF) / 255.0 green:((value >> 8) & 0xFF) / 255.0 blue:(value & 0xFF) / 255.0 alpha:1];
}

static std::string hexFromColour(NSColor *colour) {
	NSColor *rgb = [colour colorUsingColorSpace:NSColorSpace.sRGBColorSpace] ?: NSColor.blackColor;
	char out[8];
	snprintf(out, sizeof out, "%02X%02X%02X", static_cast<int>(rgb.redComponent * 255 + 0.5), static_cast<int>(rgb.greenComponent * 255 + 0.5), static_cast<int>(rgb.blueComponent * 255 + 0.5));
	return out;
}

// A style's colours and font style on one row.
@interface OmaStyleRow : NSStackView
@property(nonatomic, strong) NSButton *fgOn, *bgOn, *bold, *italic, *underline;
@property(nonatomic, strong) NSColorWell *fg, *bg;
@property(nonatomic, strong) NSTextField *fontName;
@property(nonatomic, strong) NSTextField *fontSize;
@end
@implementation OmaStyleRow
- (instancetype)initWithLabel:(NSString *)label value:(const std::string &)value {
	if ((self = [super initWithFrame:NSZeroRect])) {
		const size_t first = value.find(',');
		const size_t second = first == std::string::npos ? first : value.find(',', first + 1);
		const std::string fgHex = second == std::string::npos ? "" : value.substr(0, first);
		const std::string bgHex = second == std::string::npos ? "" : value.substr(first + 1, second - first - 1);
		const int fontStyle = second == std::string::npos ? 0 : std::atoi(value.c_str() + second + 1);

		NSTextField *name = [NSTextField labelWithString:label];
		[name.widthAnchor constraintEqualToConstant:130].active = YES;
		_fgOn = [NSButton checkboxWithTitle:@"Text" target:nil action:nil];
		_fgOn.state = fgHex.empty() ? NSControlStateValueOff : NSControlStateValueOn;
		_fg = [self wellWithHex:fgHex];
		_bgOn = [NSButton checkboxWithTitle:@"Back" target:nil action:nil];
		_bgOn.state = bgHex.empty() ? NSControlStateValueOff : NSControlStateValueOn;
		_bg = [self wellWithHex:bgHex];
		const auto box = [&](NSString *title, int bit) {
			NSButton *button = [NSButton checkboxWithTitle:title target:nil action:nil];
			button.state = (fontStyle & bit) ? NSControlStateValueOn : NSControlStateValueOff;
			return button;
		};
		_bold = box(@"Bold", 1);
		_italic = box(@"Italic", 2);
		_underline = box(@"Underline", 4);
		self.orientation = NSUserInterfaceLayoutOrientationHorizontal;
		self.alignment = NSLayoutAttributeCenterY;
		self.spacing = 8;
		for (NSView *view in @[name, _fgOn, _fg, _bgOn, _bg, _bold, _italic, _underline]) [self addArrangedSubview:view];
		// The font's name and size come after the third comma, for rows that have them.
		const size_t third = second == std::string::npos ? second : value.find(',', second + 1);
		const size_t fourth = third == std::string::npos ? third : value.find(',', third + 1);
		if (fourth != std::string::npos) {
			_fontName = [NSTextField textFieldWithString:toNS(value.substr(third + 1, fourth - third - 1))];
			_fontName.placeholderString = @"Font";
			[_fontName.widthAnchor constraintEqualToConstant:110].active = YES;
			_fontSize = [NSTextField textFieldWithString:toNS(value.substr(fourth + 1))];
			_fontSize.placeholderString = @"Size";
			[_fontSize.widthAnchor constraintEqualToConstant:40].active = YES;
			[self addArrangedSubview:_fontName];
			[self addArrangedSubview:_fontSize];
		}
	}
	return self;
}
- (NSColorWell *)wellWithHex:(const std::string &)hex {
	NSColorWell *well = [NSColorWell new];
	well.color = hex.empty() ? NSColor.grayColor : colourFromHex(hex);
	well.target = self;
	well.action = @selector(picked:);
	[well.widthAnchor constraintEqualToConstant:38].active = YES;
	[well.heightAnchor constraintEqualToConstant:20].active = YES;
	return well;
}
// Picking a colour means using it.
- (void)picked:(NSColorWell *)well { (well == _fg ? _fgOn : _bgOn).state = NSControlStateValueOn; }
- (std::string)value {
	const int fontStyle = (_bold.state == NSControlStateValueOn ? 1 : 0) | (_italic.state == NSControlStateValueOn ? 2 : 0) | (_underline.state == NSControlStateValueOn ? 4 : 0);
	std::string out = (_fgOn.state == NSControlStateValueOn ? hexFromColour(_fg.color) : std::string()) + "," + (_bgOn.state == NSControlStateValueOn ? hexFromColour(_bg.color) : std::string()) + "," + std::to_string(fontStyle);
	if (_fontName) {
		const int size = _fontSize.intValue;
		out += "," + toStd(_fontName.stringValue) + "," + (size > 0 ? std::to_string(size) : std::string());
	}
	return out;
}
@end

// A heading over a grid of checkboxes, one per choice.
@interface OmaFlagsGrid : NSStackView
@property(nonatomic, strong) NSMutableArray<NSButton *> *boxes;
@end
@implementation OmaFlagsGrid
- (instancetype)initWithField:(const FormField &)field {
	if ((self = [super initWithFrame:NSZeroRect])) {
		self.orientation = NSUserInterfaceLayoutOrientationVertical;
		self.alignment = NSLayoutAttributeLeading;
		self.spacing = 4;
		NSTextField *title = [NSTextField labelWithString:toNS(field.label)];
		title.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
		[self addArrangedSubview:title];
		_boxes = [NSMutableArray array];
		const long value = std::strtol(field.value.c_str(), nullptr, 10);
		const NSUInteger columns = 5;
		NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
		NSMutableArray<NSView *> *row = nil;
		for (const auto &choice : field.choices) {
			NSButton *box = [NSButton checkboxWithTitle:toNS(choice.second) target:nil action:nil];
			box.state = (value & std::strtol(choice.first.c_str(), nullptr, 10)) ? NSControlStateValueOn : NSControlStateValueOff;
			[_boxes addObject:box];
			if (!row || row.count == columns) {
				row = [NSMutableArray array];
				[rows addObject:row];
			}
			[row addObject:box];
		}
		while (row && row.count < columns) [row addObject:[NSGridCell emptyContentView]];
		[self addArrangedSubview:[NSGridView gridViewWithViews:rows]];
	}
	return self;
}
- (std::string)valueForField:(const FormField &)field {
	long value = 0;
	for (size_t i = 0; i < field.choices.size(); i++)
		if (_boxes[i].state == NSControlStateValueOn) value |= std::strtol(field.choices[i].first.c_str(), nullptr, 10);
	return std::to_string(value);
}
@end

// Puts a column of rows in a scrolling view that fills whatever holds it.
static NSScrollView *OmaScrollingRows(NSStackView *rows) {
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
	[NSLayoutConstraint activateConstraints:@[
		[document.topAnchor constraintEqualToAnchor:scroll.contentView.topAnchor],
		[document.leadingAnchor constraintEqualToAnchor:scroll.contentView.leadingAnchor],
		[document.trailingAnchor constraintEqualToAnchor:scroll.contentView.trailingAnchor],
		[rows.topAnchor constraintEqualToAnchor:document.topAnchor],
		[rows.leadingAnchor constraintEqualToAnchor:document.leadingAnchor],
		[rows.trailingAnchor constraintLessThanOrEqualToAnchor:document.trailingAnchor],
		[rows.bottomAnchor constraintEqualToAnchor:document.bottomAnchor],
	]];
	return scroll;
}

static NSStackView *OmaNewFormRows() {
	NSStackView *rows = [NSStackView new];
	rows.orientation = NSUserInterfaceLayoutOrientationVertical;
	rows.alignment = NSLayoutAttributeLeading;
	rows.spacing = 8;
	rows.edgeInsets = NSEdgeInsetsMake(14, 18, 14, 18);
	rows.translatesAutoresizingMaskIntoConstraints = NO;
	return rows;
}

BOOL OmaRunForm(NSString *title, std::vector<FormField> &fields) {
	bool hasPages = false;
	for (const FormField &field : fields) hasPages = hasPages || field.kind == FormField::Kind::Page;
	const CGFloat formWidth = hasPages ? 780 : 520;
	OmaFormDelegate *delegate = [OmaFormDelegate new];
	NSPanel *window = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, formWidth, 580)
	                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
	                                               backing:NSBackingStoreBuffered
	                                                 defer:NO];
	window.title = title;
	window.delegate = delegate;
	window.releasedWhenClosed = NO;
	window.minSize = NSMakeSize(420, 240);

	NSMutableArray<NSView *> *controls = [NSMutableArray array];  // One per field, or NSNull's stand-in.
	NSStackView *rows = OmaNewFormRows();
	NSTabView *tabs = hasPages ? [NSTabView new] : nil;
	const auto addPage = [&](NSString *label, NSStackView *pageRows) {
		NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:label];
		item.label = label;
		NSView *holder = [NSView new];
		NSScrollView *pageScroll = OmaScrollingRows(pageRows);
		[holder addSubview:pageScroll];
		[NSLayoutConstraint activateConstraints:@[
			[pageScroll.topAnchor constraintEqualToAnchor:holder.topAnchor],
			[pageScroll.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor],
			[pageScroll.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor],
			[pageScroll.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor],
		]];
		item.view = holder;
		[tabs addTabViewItem:item];
	};
	NSString *pageTitle = nil;

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
			if (rows.arrangedSubviews.count > 0) [rows setCustomSpacing:18 afterView:rows.arrangedSubviews.lastObject];
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
		case FormField::Kind::Page: {
			// The rows so far belong to the page before.
			if (pageTitle) {
				addPage(pageTitle, rows);
				rows = OmaNewFormRows();
			}
			pageTitle = toNS(field.label);
			break;
		}
		case FormField::Kind::LongText: {
			NSTextField *label = [NSTextField labelWithString:toNS(field.label)];
			NSTextView *text = [NSTextView new];
			text.string = toNS(field.value);
			text.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
			text.richText = NO;
			text.automaticQuoteSubstitutionEnabled = NO;
			text.automaticDashSubstitutionEnabled = NO;
			text.automaticTextReplacementEnabled = NO;
			text.automaticSpellingCorrectionEnabled = NO;
			text.verticallyResizable = YES;
			text.textContainer.widthTracksTextView = YES;
			text.autoresizingMask = NSViewWidthSizable;
			NSScrollView *holder = [NSScrollView new];
			holder.documentView = text;
			holder.hasVerticalScroller = YES;
			holder.autohidesScrollers = YES;
			holder.borderType = NSBezelBorder;
			[holder.widthAnchor constraintEqualToConstant:formWidth - 60].active = YES;
			[holder.heightAnchor constraintEqualToConstant:58].active = YES;
			NSStackView *column = [NSStackView stackViewWithViews:@[label, holder]];
			column.orientation = NSUserInterfaceLayoutOrientationVertical;
			column.alignment = NSLayoutAttributeLeading;
			column.spacing = 3;
			[rows addArrangedSubview:column];
			control = text;
			break;
		}
		case FormField::Kind::StyleRow: {
			OmaStyleRow *row = [[OmaStyleRow alloc] initWithLabel:toNS(field.label) value:field.value];
			[rows addArrangedSubview:row];
			control = row;
			break;
		}
		case FormField::Kind::Flags: {
			OmaFlagsGrid *grid = [[OmaFlagsGrid alloc] initWithField:field];
			[rows addArrangedSubview:grid];
			control = grid;
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

	NSView *main;
	if (hasPages) {
		if (pageTitle) addPage(pageTitle, rows);
		tabs.translatesAutoresizingMaskIntoConstraints = NO;
		main = tabs;
	} else {
		main = OmaScrollingRows(rows);
	}

	NSButton *ok = [NSButton buttonWithTitle:@"OK" target:delegate action:@selector(accept:)];
	ok.keyEquivalent = @"\r";
	NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:delegate action:@selector(cancel:)];
	cancel.keyEquivalent = @"\033";
	NSStackView *buttons = [NSStackView stackViewWithViews:@[cancel, ok]];
	buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
	buttons.translatesAutoresizingMaskIntoConstraints = NO;

	NSView *content = window.contentView;
	[content addSubview:main];
	[content addSubview:buttons];
	const CGFloat inset = hasPages ? 12 : 0;
	[NSLayoutConstraint activateConstraints:@[
		[main.topAnchor constraintEqualToAnchor:content.topAnchor constant:inset],
		[main.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:inset],
		[main.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-inset],
		[main.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-10],
		[buttons.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-14],
		[buttons.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-12],
	]];

	// Short forms don't need all the room a long one does.
	[content layoutSubtreeIfNeeded];
	const CGFloat wanted = hasPages ? 640 : std::min<CGFloat>(rows.fittingSize.height + 54, 620);
	[window setContentSize:NSMakeSize(formWidth, wanted)];
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
		case FormField::Kind::Heading:
		case FormField::Kind::Page: break;
		case FormField::Kind::LongText: field.value = toStd(((NSTextView *)control).string); break;
		case FormField::Kind::StyleRow: field.value = [(OmaStyleRow *)control value]; break;
		case FormField::Kind::Flags: field.value = [(OmaFlagsGrid *)control valueForField:field]; break;
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
	NSPopUpButton *_category;
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
	_category = [NSPopUpButton new];
	[_category addItemsWithTitles:@[@"Main menu", @"Macros", @"Run commands", @"Plugin commands", @"Scintilla commands"]];
	_category.target = self;
	_category.action = @selector(categoryChanged:);
	_category.translatesAutoresizingMaskIntoConstraints = NO;

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
		[self button:@"Delete…" action:@selector(deleteCommand:)],
		[self button:@"Reset All…" action:@selector(resetAll:)],
		[NSView new],
		done,
	]];
	buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
	buttons.translatesAutoresizingMaskIntoConstraints = NO;

	NSView *content = _window.contentView;
	[content addSubview:_category];
	[content addSubview:_search];
	[content addSubview:scroll];
	[content addSubview:_hint];
	[content addSubview:buttons];
	[NSLayoutConstraint activateConstraints:@[
		[_category.topAnchor constraintEqualToAnchor:content.topAnchor constant:12],
		[_category.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:14],
		[_search.topAnchor constraintEqualToAnchor:content.topAnchor constant:12],
		[_search.leadingAnchor constraintEqualToAnchor:_category.trailingAnchor constant:8],
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

- (void)categoryChanged:(id)sender { [self applyFilter]; }

- (void)deleteCommand:(id)sender {
	const ShortcutEntry *entry = [self selectedEntry];
	if (!entry) return;
	const std::string id = entry->id;
	NSAlert *alert = [NSAlert new];
	if (id.rfind("Macro/", 0) != 0 && id.rfind("Run/", 0) != 0) {
		alert.messageText = @"Only saved macros and run commands can be deleted.";
		[alert runModal];
		return;
	}
	alert.messageText = toNS("Delete “" + id + "”?");
	[alert addButtonWithTitle:@"Delete"];
	[alert addButtonWithTitle:@"Cancel"];
	if ([alert runModal] != NSAlertFirstButtonReturn) return;
	_workspace->deleteShortcutCommand(id);
	[self reload];
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
	const std::string category = toStd(_category.titleOfSelectedItem ?: @"Main menu");
	for (size_t i = 0; i < _entries.size(); i++) {
		if (_entries[i].category != category) continue;
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

#pragma mark - Printing

// One tall view with each page below the last, as AppKit's printing expects.
@interface OmaPrintView : NSView
- (instancetype)initWithJob:(omapad::PrintJob *)job;
@end

@implementation OmaPrintView {
	omapad::PrintJob *_job;
	NSSize _paper;
}

- (instancetype)initWithJob:(omapad::PrintJob *)job {
	if ((self = [super initWithFrame:NSMakeRect(0, 0, 612, 792)])) _job = job;
	return self;
}

- (BOOL)isFlipped { return YES; }

static const CGFloat kPointsPerMillimetre = 72.0 / 25.4;

- (int)bodyWidth {
	const omapad::PrintOptions &o = _job->settings();
	return std::max(36, static_cast<int>(_paper.width - (o.marginLeft + o.marginRight) * kPointsPerMillimetre));
}

- (int)bodyHeight {
	const omapad::PrintOptions &o = _job->settings();
	return std::max(36, static_cast<int>(_paper.height - (o.marginTop + o.marginBottom) * kPointsPerMillimetre));
}

- (BOOL)knowsPageRange:(NSRangePointer)range {
	_paper = NSPrintOperation.currentOperation.printInfo.paperSize;
	// Pages are worked out on a throwaway surface, because there's nothing to draw on yet.
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef measuring = CGBitmapContextCreate(nullptr, 16, 16, 8, 0, space, kCGImageAlphaPremultipliedLast);
	CGColorSpaceRelease(space);
	_job->paginate(measuring, [self bodyWidth], [self bodyHeight]);
	CGContextRelease(measuring);
	range->location = 1;
	range->length = _job->pageCount();
	[self setFrameSize:NSMakeSize(_paper.width, _paper.height * _job->pageCount())];
	return YES;
}

- (NSRect)rectForPage:(NSInteger)page { return NSMakeRect(0, _paper.height * (page - 1), _paper.width, _paper.height); }

- (void)drawCaption:(const omapad::PrintPageText &)text atY:(CGFloat)y width:(CGFloat)width bold:(BOOL)bold italic:(BOOL)italic {
	NSFont *font = bold ? [NSFont boldSystemFontOfSize:omapad::PrintJob::kCaptionPoints] : [NSFont systemFontOfSize:omapad::PrintJob::kCaptionPoints];
	if (italic) font = [NSFontManager.sharedFontManager convertFont:font toHaveTrait:NSItalicFontMask];
	NSDictionary *attributes = @{
		NSFontAttributeName: font,
		NSForegroundColorAttributeName: NSColor.blackColor,
	};
	NSString *left = toNS(text.left), *middle = toNS(text.middle), *right = toNS(text.right);
	[left drawAtPoint:NSMakePoint(0, y) withAttributes:attributes];
	[middle drawAtPoint:NSMakePoint((width - [middle sizeWithAttributes:attributes].width) / 2, y) withAttributes:attributes];
	[right drawAtPoint:NSMakePoint(width - [right sizeWithAttributes:attributes].width, y) withAttributes:attributes];
}

- (void)drawRect:(NSRect)dirty {
	const NSInteger page = NSPrintOperation.currentOperation.currentPage - 1;
	if (page < 0 || page >= _job->pageCount()) return;
	const omapad::PrintOptions &o = _job->settings();
	const int width = [self bodyWidth], height = [self bodyHeight];
	CGContextRef context = NSGraphicsContext.currentContext.CGContext;
	CGContextSaveGState(context);
	CGContextTranslateCTM(context, o.marginLeft * kPointsPerMillimetre, _paper.height * page + o.marginTop * kPointsPerMillimetre);
	_job->drawPage(context, static_cast<int>(page), width, height);
	if (_job->headerHeight() > 0) [self drawCaption:_job->header(static_cast<int>(page)) atY:0 width:width bold:o.headerBold italic:o.headerItalic];
	if (_job->footerHeight() > 0) [self drawCaption:_job->footer(static_cast<int>(page)) atY:height - _job->footerHeight() + 6 width:width bold:o.footerBold italic:o.footerItalic];
	CGContextRestoreGState(context);
}

@end

BOOL OmaRunPrint(omapad::PrintJob &job, const omapad::PrintOutput &output, std::string &error) {
	NSPrintInfo *info = [NSPrintInfo.sharedPrintInfo copy];
	// The view lays out its own margins, since they come from the preferences.
	info.leftMargin = info.rightMargin = info.topMargin = info.bottomMargin = 0;
	info.horizontalPagination = NSPrintingPaginationModeClip;
	info.verticalPagination = NSPrintingPaginationModeClip;
	info.horizontallyCentered = NO;
	info.verticallyCentered = NO;
	const bool toFile = !output.pdfPath.empty();
	if (toFile) {
		info.jobDisposition = NSPrintSaveJob;
		info.dictionary[NSPrintJobSavingURL] = [NSURL fileURLWithPath:toNS(output.pdfPath)];
	}
	OmaPrintView *view = [[OmaPrintView alloc] initWithJob:&job];
	NSPrintOperation *operation = [NSPrintOperation printOperationWithView:view printInfo:info];
	operation.jobTitle = toNS(job.settings().title);
	operation.showsPrintPanel = output.showDialog;
	operation.showsProgressPanel = !toFile;
	operation.printPanel.options |= NSPrintPanelShowsPaperSize | NSPrintPanelShowsOrientation | NSPrintPanelShowsScaling;
	const BOOL done = [operation runOperation];
	// The only way to tell a failure from cancelling is that a file was asked for and isn't there.
	if (toFile && (!done || ![NSFileManager.defaultManager fileExistsAtPath:toNS(output.pdfPath)])) {
		error = "The PDF wasn't written.";
		return NO;
	}
	return YES;
}

#pragma mark - List dialog

@interface OmaListDialog : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic) NSInteger pressed;
- (int)runList:(const omapad::ListDialog &)list selection:(std::vector<int> &)selection;
@end

@implementation OmaListDialog {
	std::vector<std::vector<std::string>> _rows;
	NSTableView *_table;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)view { return static_cast<NSInteger>(_rows.size()); }

- (NSView *)tableView:(NSTableView *)view viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
	NSTextField *cell = [view makeViewWithIdentifier:@"cell" owner:self];
	if (!cell) {
		cell = [NSTextField labelWithString:@""];
		cell.identifier = @"cell";
		cell.lineBreakMode = NSLineBreakByTruncatingMiddle;
	}
	const NSUInteger index = [view.tableColumns indexOfObject:column];
	const auto &values = _rows[static_cast<size_t>(row)];
	cell.stringValue = index < values.size() ? toNS(values[index]) : @"";
	return cell;
}

- (void)press:(NSButton *)button {
	self.pressed = button.tag;
	[NSApp stopModalWithCode:NSModalResponseOK];
}
- (void)doubleClicked:(id)sender {
	if (_table.clickedRow < 0) return;
	self.pressed = 0;
	[NSApp stopModalWithCode:NSModalResponseOK];
}
- (BOOL)windowShouldClose:(NSWindow *)window {
	self.pressed = -1;
	[NSApp stopModalWithCode:NSModalResponseCancel];
	return NO;
}

- (int)runList:(const omapad::ListDialog &)list selection:(std::vector<int> &)selection {
	_rows = list.rows;
	self.pressed = -1;
	NSPanel *window = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 720, 460)
	                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
	                                               backing:NSBackingStoreBuffered
	                                                 defer:NO];
	window.title = toNS(list.title);
	window.delegate = self;
	window.releasedWhenClosed = NO;
	_table = [NSTableView new];
	_table.dataSource = self;
	_table.delegate = self;
	_table.allowsMultipleSelection = list.multiSelect;
	_table.target = self;
	_table.doubleAction = @selector(doubleClicked:);
	for (const std::string &title : list.columns) {
		NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:toNS(title)];
		column.title = toNS(title);
		column.width = 680 / MAX(list.columns.size(), static_cast<size_t>(1));
		[_table addTableColumn:column];
	}
	NSScrollView *scroll = [NSScrollView new];
	scroll.documentView = _table;
	scroll.hasVerticalScroller = YES;
	scroll.borderType = NSBezelBorder;
	scroll.translatesAutoresizingMaskIntoConstraints = NO;
	NSMutableArray<NSView *> *buttons = [NSMutableArray array];
	for (size_t i = 0; i < list.buttons.size(); i++) {
		NSButton *button = [NSButton buttonWithTitle:toNS(list.buttons[i]) target:self action:@selector(press:)];
		button.tag = static_cast<NSInteger>(i);
		[buttons addObject:button];
	}
	NSStackView *row = [NSStackView stackViewWithViews:buttons];
	row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
	row.translatesAutoresizingMaskIntoConstraints = NO;
	NSView *content = window.contentView;
	[content addSubview:scroll];
	[content addSubview:row];
	[NSLayoutConstraint activateConstraints:@[
		[scroll.topAnchor constraintEqualToAnchor:content.topAnchor constant:12],
		[scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:12],
		[scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
		[scroll.bottomAnchor constraintEqualToAnchor:row.topAnchor constant:-10],
		[row.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
		[row.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-12],
	]];
	[_table reloadData];
	if (!_rows.empty()) [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
	[window center];
	[NSApp runModalForWindow:window];
	[window orderOut:nil];
	selection.clear();
	[_table.selectedRowIndexes enumerateIndexesUsingBlock:^(NSUInteger index, BOOL *) { selection.push_back(static_cast<int>(index)); }];
	return static_cast<int>(self.pressed);
}

@end

int OmaRunList(const omapad::ListDialog &list, std::vector<int> &selection) {
	OmaListDialog *dialog = [OmaListDialog new];
	return [dialog runList:list selection:selection];
}

#pragma mark - Side panel

@interface OmaSidePanel () <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation OmaSidePanel {
	Workspace *_workspace;
	int _panel;
	std::vector<omapad::PanelRow> _rows;
	std::vector<std::string> _headings;
	NSTableView *_table;
	NSScrollView *_scroll;
}

- (instancetype)initWithWorkspace:(Workspace *)workspace panel:(int)panel title:(NSString *)title {
	if ((self = [super initWithFrame:NSMakeRect(0, 0, 250, 300)])) {
		_workspace = workspace;
		_panel = panel;
		NSTextField *label = [NSTextField labelWithString:title.uppercaseString];
		label.font = [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold];
		label.textColor = NSColor.secondaryLabelColor;
		label.translatesAutoresizingMaskIntoConstraints = NO;
		_table = [OmaMenuTable new];
		_table.headerView = nil;
		_table.dataSource = self;
		_table.delegate = self;
		_table.target = self;
		_table.doubleAction = @selector(activated:);
		_table.rowSizeStyle = NSTableViewRowSizeStyleSmall;
		_table.style = NSTableViewStyleSourceList;
		NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"title"];
		column.resizingMask = NSTableColumnAutoresizingMask;
		[_table addTableColumn:column];
		_table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
		__weak OmaSidePanel *weakSelf = self;
		((OmaMenuTable *)_table).menuForRow = ^NSMenu *(NSInteger row) {
			OmaSidePanel *strong = weakSelf;
			if (!strong || !strong.menuBuilder) return nil;
			return strong.menuBuilder(strong->_workspace->panelMenu(strong->_panel, static_cast<int>(MAX(row, 0))));
		};
		NSScrollView *scroll = [NSScrollView new];
		scroll.documentView = _table;
		scroll.hasVerticalScroller = YES;
		scroll.autohidesScrollers = YES;
		scroll.drawsBackground = NO;
		scroll.translatesAutoresizingMaskIntoConstraints = NO;
		_scroll = scroll;
		[self addSubview:label];
		[self addSubview:scroll];
		[NSLayoutConstraint activateConstraints:@[
			[label.topAnchor constraintEqualToAnchor:self.topAnchor constant:8],
			[label.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:10],
			[scroll.topAnchor constraintEqualToAnchor:label.bottomAnchor constant:6],
			[scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
			[scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
			[scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
		]];
	}
	return self;
}

- (NSView *)focusTarget { return _table; }

- (void)reload {
	std::vector<omapad::PanelRow> rows = _workspace->panelRows(_panel);
	const std::vector<std::string> headings = _workspace->panelColumns(_panel);
	const bool sameColumns = headings == _headings && (!headings.empty() || _table.tableColumns.count == 1);
	if (!sameColumns) {
		_headings = headings;
		while (_table.tableColumns.count) [_table removeTableColumn:_table.tableColumns.firstObject];
		if (headings.empty()) {
			NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"title"];
			column.resizingMask = NSTableColumnAutoresizingMask;
			[_table addTableColumn:column];
			_table.headerView = nil;
		} else {
			_table.headerView = [NSTableHeaderView new];
			_table.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
			for (size_t i = 0; i < headings.size(); i++) {
				NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:[NSString stringWithFormat:@"%zu", i]];
				column.title = toNS(headings[i]);
				column.width = _panel == omapad::kPanelCharacters ? (i >= 3 ? 90 : 56) : (i == 0 ? 150 : i == headings.size() - 1 && headings.size() > 1 ? 220 : 50);
				column.resizingMask = NSTableColumnUserResizingMask;
				[_table addTableColumn:column];
			}
			_scroll.hasHorizontalScroller = YES;
		}
		// The scroll view has to lay itself out again around the header.
		_scroll.documentView = _table;
		[_scroll tile];
	}
	bool same = sameColumns && rows.size() == _rows.size();
	for (size_t i = 0; same && i < rows.size(); i++) same = rows[i].title == _rows[i].title && rows[i].depth == _rows[i].depth && rows[i].columns == _rows[i].columns;
	if (same) return;
	_rows = std::move(rows);
	[_table reloadData];
}

- (void)activated:(id)sender {
	if (_table.clickedRow >= 0) _workspace->panelActivate(_panel, static_cast<int>(_table.clickedRow), static_cast<int>(MAX(_table.clickedColumn, 0)));
}

- (void)tableView:(NSTableView *)view didClickTableColumn:(NSTableColumn *)column {
	_workspace->panelColumnClicked(_panel, column.identifier.intValue);
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)view { return static_cast<NSInteger>(_rows.size()); }

- (NSView *)tableView:(NSTableView *)view viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
	NSTextField *cell = [view makeViewWithIdentifier:@"cell" owner:self];
	if (!cell) {
		cell = [NSTextField labelWithString:@""];
		cell.identifier = @"cell";
		cell.lineBreakMode = NSLineBreakByTruncatingTail;
		cell.font = _panel == omapad::kPanelCharacters ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular] : [NSFont systemFontOfSize:12];
	}
	const omapad::PanelRow &entry = _rows[static_cast<size_t>(row)];
	const size_t at = static_cast<size_t>(column.identifier.intValue);
	const bool cellText = !_headings.empty() && at < entry.columns.size();
	cell.stringValue = toNS(cellText ? entry.columns[at] : std::string(2 * static_cast<size_t>(entry.depth), ' ') + entry.title);
	cell.toolTip = toNS(entry.toolTip);
	return cell;
}

@end
