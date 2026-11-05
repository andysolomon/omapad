#import "TabBar.h"

@implementation OmaTabItem
@end

static const CGFloat kMinTab = 90;
static const CGFloat kMaxTab = 220;
static const CGFloat kCloseSize = 14;
static const CGFloat kPadding = 10;

@implementation TabBar {
	NSStackView *_shortcuts;
	NSInteger _hoverTab;
	NSArray<OmaTabItem *> *_items;
	NSInteger _active;
	BOOL _focused;
	CGFloat _offset;
	NSInteger _hoverClose;
	NSInteger _dragIndex;
	BOOL _dragged;
	NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame {
	if ((self = [super initWithFrame:frame])) {
		_items = @[];
		_active = -1;
		_hoverClose = -1;
		_dragIndex = -1;
		_showClose = YES;
		_activeBar = YES;
		_inactiveColour = YES;
		_buttonsOnInactive = YES;
		_hoverTab = -1;
	}
	return self;
}

- (BOOL)isFlipped { return YES; }

- (CGFloat)rowHeight { return _reduced ? 22 : 30; }

- (NSDictionary *)textAttributes:(BOOL)active {
	return @{
		NSFontAttributeName: [NSFont systemFontOfSize:12 weight:active ? NSFontWeightMedium : NSFontWeightRegular],
		NSForegroundColorAttributeName: active ? NSColor.labelColor : NSColor.secondaryLabelColor,
	};
}

- (NSString *)displayTitle:(OmaTabItem *)item {
	return item.display ?: item.title;
}

- (BOOL)buttonsShownOn:(NSInteger)index { return _buttonsOnInactive || index == _active; }

- (CGFloat)shortcutsWidth { return _rightShortcuts && !_vertical ? 72 : 0; }

- (void)setRightShortcuts:(BOOL)on {
	_rightShortcuts = on;
	if (on && !_shortcuts) {
		NSButton *(^make)(NSString *, NSString *, SEL) = ^NSButton *(NSString *symbol, NSString *tip, SEL action) {
			NSButton *button = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:symbol accessibilityDescription:tip] target:self action:action];
			button.bordered = NO;
			button.toolTip = tip;
			button.refusesFirstResponder = YES;
			return button;
		};
		_shortcuts = [NSStackView stackViewWithViews:@[
			make(@"plus", @"New document", @selector(shortcutNew:)),
			make(@"chevron.down", @"List the open documents", @selector(shortcutList:)),
			make(@"xmark", @"Close this document", @selector(shortcutClose:)),
		]];
		_shortcuts.spacing = 2;
		_shortcuts.translatesAutoresizingMaskIntoConstraints = NO;
		[self addSubview:_shortcuts];
		[NSLayoutConstraint activateConstraints:@[
			[_shortcuts.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-4],
			[_shortcuts.topAnchor constraintEqualToAnchor:self.topAnchor constant:4],
		]];
	}
	_shortcuts.hidden = !on || _vertical;
	self.needsDisplay = YES;
}

- (void)shortcutNew:(id)sender { [self.delegate tabBarDidRequestNewTab:self]; }
- (void)shortcutList:(NSButton *)sender { [self.delegate tabBarDidRequestList:self sender:sender]; }
- (void)shortcutClose:(id)sender { [self.delegate tabBarDidRequestCloseActive:self]; }

- (NSRect)pinRectForTab:(NSRect)tab {
	const NSRect close = [self closeRectForTab:tab];
	return NSMakeRect(NSMinX(close) - kCloseSize - 2, NSMinY(close), kCloseSize, kCloseSize);
}

- (CGFloat)widthOfItem:(OmaTabItem *)item {
	const CGFloat text = [[self displayTitle:item] sizeWithAttributes:[self textAttributes:YES]].width;
	if (_vertical) return NSWidth(self.bounds);
	const CGFloat minimum = _reduced ? 60 : kMinTab;
	return MIN(kMaxTab, MAX(minimum, ceil(text) + kPadding * 2 + (_showClose && !item.pinned ? kCloseSize + 4 : 0) + (_showPin ? kCloseSize + 2 : 0)));
}

- (NSRect)rectOfIndex:(NSInteger)index {
	if (_vertical) return NSMakeRect(0, [self rowHeight] * index - _offset, NSWidth(self.bounds), [self rowHeight]);
	if (_multiLine) {
		CGFloat x = 0, y = 0;
		for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
			const CGFloat w = [self widthOfItem:_items[i]];
			if (x > 0 && x + w > NSWidth(self.bounds)) {
				x = 0;
				y += [self rowHeight];
			}
			if (i == index) return NSMakeRect(x, y, w, [self rowHeight]);
			x += w;
		}
		return NSZeroRect;
	}
	CGFloat x = -_offset;
	for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
		const CGFloat w = [self widthOfItem:_items[i]];
		if (i == index) return NSMakeRect(x, 0, w, NSHeight(self.bounds));
		x += w;
	}
	return NSZeroRect;
}

- (CGFloat)heightForWidth:(CGFloat)width {
	if (!_multiLine) return [self rowHeight];
	CGFloat x = 0, y = [self rowHeight];
	for (OmaTabItem *item in _items) {
		const CGFloat w = [self widthOfItem:item];
		if (x > 0 && x + w > width) {
			x = 0;
			y += [self rowHeight];
		}
		x += w;
	}
	return y;
}

- (CGFloat)totalWidth {
	CGFloat total = 0;
	for (OmaTabItem *item in _items) total += [self widthOfItem:item];
	return total;
}

- (NSRect)closeRectForTab:(NSRect)tab {
	return NSMakeRect(NSMaxX(tab) - kCloseSize - 6, NSMinY(tab) + floor((NSHeight(tab) - kCloseSize) / 2), kCloseSize, kCloseSize);
}

- (NSInteger)indexAtPoint:(NSPoint)point {
	for (NSInteger i = 0; i < (NSInteger)_items.count; i++)
		if (NSPointInRect(point, [self rectOfIndex:i])) return i;
	return -1;
}

- (void)clampOffset {
	const CGFloat maxOffset = _multiLine ? 0 : _vertical ? MAX(0, [self rowHeight] * _items.count - NSHeight(self.bounds)) : MAX(0, [self totalWidth] - (NSWidth(self.bounds) - [self shortcutsWidth]));
	_offset = MAX(0, MIN(_offset, maxOffset));
}

- (void)setItems:(NSArray<OmaTabItem *> *)items activeIndex:(NSInteger)active focused:(BOOL)focused {
	_focused = focused;
	const BOOL activeChanged = active != _active;
	_items = [items copy];
	_active = active;
	if (activeChanged && active >= 0 && active < (NSInteger)items.count) {
		NSRect r = [self rectOfIndex:active];
		if (_vertical) {
			if (NSMinY(r) < 0) _offset += NSMinY(r);
			else if (NSMaxY(r) > NSHeight(self.bounds)) _offset += NSMaxY(r) - NSHeight(self.bounds);
		} else if (!_multiLine) {
			if (NSMinX(r) < 0) _offset += NSMinX(r);
			else if (NSMaxX(r) > NSWidth(self.bounds) - [self shortcutsWidth]) _offset += NSMaxX(r) - (NSWidth(self.bounds) - [self shortcutsWidth]);
		}
	}
	[self clampOffset];
	[self rebuildToolTips];
	self.needsDisplay = YES;
}

- (void)rebuildToolTips {
	[self removeAllToolTips];
	for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
		if (_items[i].toolTip.length)
			[self addToolTipRect:[self rectOfIndex:i] owner:_items[i].toolTip userData:NULL];
	}
}

- (void)setFrameSize:(NSSize)size {
	[super setFrameSize:size];
	[self clampOffset];
	[self rebuildToolTips];
}

- (void)drawRect:(NSRect)dirtyRect {
	[NSColor.windowBackgroundColor setFill];
	NSRectFill(self.bounds);
	for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
		const NSRect tab = [self rectOfIndex:i];
		if (NSMaxX(tab) < 0 || NSMinX(tab) > NSWidth(self.bounds) || NSMaxY(tab) < 0 || NSMinY(tab) > NSHeight(self.bounds)) continue;
		OmaTabItem *item = _items[i];
		const BOOL active = i == _active;
		if (active) {
			[NSColor.textBackgroundColor setFill];
			NSRectFill(tab);
			if (_activeBar) {
				[(_focused ? NSColor.controlAccentColor : NSColor.tertiaryLabelColor) setFill];
				NSRectFill(NSMakeRect(NSMinX(tab), NSMinY(tab), NSWidth(tab), 2));
			}
		} else if (_inactiveColour) {
			[[NSColor.labelColor colorWithAlphaComponent:0.04] setFill];
			NSRectFillUsingOperation(tab, NSCompositingOperationSourceOver);
		}
		if (item.colour >= 1 && item.colour <= 5) {
			NSArray<NSColor *> *colours = @[NSColor.systemYellowColor, NSColor.systemGreenColor, NSColor.systemBlueColor, NSColor.systemOrangeColor, NSColor.systemPurpleColor];
			[[colours[item.colour - 1] colorWithAlphaComponent:0.35] setFill];
			NSRectFillUsingOperation(tab, NSCompositingOperationSourceOver);
		}
		const BOOL closable = _showClose && !item.pinned && [self buttonsShownOn:i];
		const BOOL pinnable = _showPin && [self buttonsShownOn:i];
		[NSColor.separatorColor setFill];
		NSRectFillUsingOperation(NSMakeRect(NSMaxX(tab) - 1, NSMinY(tab) + 4, 1, NSHeight(tab) - 8), NSCompositingOperationSourceOver);

		const NSRect close = [self closeRectForTab:tab];
		NSMutableParagraphStyle *paragraph = [NSMutableParagraphStyle new];
		paragraph.lineBreakMode = NSLineBreakByTruncatingMiddle;
		NSMutableDictionary *attributes = [[self textAttributes:active] mutableCopy];
		attributes[NSParagraphStyleAttributeName] = paragraph;
		NSString *title = [self displayTitle:item];
		const CGFloat textHeight = [title sizeWithAttributes:attributes].height;
		[title drawInRect:NSMakeRect(NSMinX(tab) + kPadding, NSMinY(tab) + floor((NSHeight(tab) - textHeight) / 2),
		                             (pinnable ? NSMinX([self pinRectForTab:tab]) : closable ? NSMinX(close) : NSMaxX(tab) - 4) - NSMinX(tab) - kPadding - 2, textHeight)
		   withAttributes:attributes];
		if (pinnable) {
			NSDictionary *pinAttributes = @{NSFontAttributeName: [NSFont systemFontOfSize:11], NSForegroundColorAttributeName: item.pinned ? NSColor.labelColor : NSColor.tertiaryLabelColor};
			[@"⚲" drawInRect:[self pinRectForTab:tab] withAttributes:pinAttributes];
		}
		if (!closable) continue;

		if (i == _hoverClose) {
			[NSColor.quaternaryLabelColor setFill];
			[[NSBezierPath bezierPathWithRoundedRect:close xRadius:3 yRadius:3] fill];
		}
		[(i == _hoverClose ? NSColor.labelColor : NSColor.tertiaryLabelColor) setStroke];
		NSBezierPath *x = [NSBezierPath bezierPath];
		x.lineWidth = 1.2;
		const NSRect inner = NSInsetRect(close, 4, 4);
		[x moveToPoint:inner.origin];
		[x lineToPoint:NSMakePoint(NSMaxX(inner), NSMaxY(inner))];
		[x moveToPoint:NSMakePoint(NSMaxX(inner), NSMinY(inner))];
		[x lineToPoint:NSMakePoint(NSMinX(inner), NSMaxY(inner))];
		[x stroke];
	}
	if ([self shortcutsWidth] > 0) {
		[NSColor.windowBackgroundColor setFill];
		NSRectFill(NSMakeRect(NSWidth(self.bounds) - [self shortcutsWidth], 0, [self shortcutsWidth], NSHeight(self.bounds)));
	}
	[NSColor.separatorColor setFill];
	NSRectFillUsingOperation(NSMakeRect(0, NSHeight(self.bounds) - 1, NSWidth(self.bounds), 1), NSCompositingOperationSourceOver);
}

- (void)updateTrackingAreas {
	[super updateTrackingAreas];
	if (_tracking) [self removeTrackingArea:_tracking];
	_tracking = [[NSTrackingArea alloc] initWithRect:self.bounds
	                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow
	                                           owner:self
	                                        userInfo:nil];
	[self addTrackingArea:_tracking];
}

- (void)mouseMoved:(NSEvent *)event {
	const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
	const NSInteger index = [self indexAtPoint:p];
	if (index != _hoverTab) {
		_hoverTab = index;
		[self.delegate tabBar:self didHoverIndex:index];
	}
	const NSInteger hover = (index >= 0 && NSPointInRect(p, [self closeRectForTab:[self rectOfIndex:index]])) ? index : -1;
	if (hover != _hoverClose) {
		_hoverClose = hover;
		self.needsDisplay = YES;
	}
}

- (void)mouseExited:(NSEvent *)event {
	if (_hoverTab != -1) {
		_hoverTab = -1;
		[self.delegate tabBar:self didHoverIndex:-1];
	}
	if (_hoverClose != -1) {
		_hoverClose = -1;
		self.needsDisplay = YES;
	}
}

- (void)mouseDown:(NSEvent *)event {
	const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
	const NSInteger index = [self indexAtPoint:p];
	_dragged = NO;
	_dragIndex = -1;
	if (index < 0) {
		if (event.clickCount == 2) [self.delegate tabBarDidRequestNewTab:self];
		return;
	}
	if (_showPin && [self buttonsShownOn:index] && NSPointInRect(p, [self pinRectForTab:[self rectOfIndex:index]])) {
		[self.delegate tabBar:self didTogglePinIndex:index];
		return;
	}
	if (_showClose && !_items[index].pinned && [self buttonsShownOn:index] && NSPointInRect(p, [self closeRectForTab:[self rectOfIndex:index]])) {
		[self.delegate tabBar:self didRequestCloseIndex:index];
		return;
	}
	if (event.clickCount == 2) {
		[self.delegate tabBar:self didDoubleClickIndex:index];
		return;
	}
	_dragIndex = _locked ? -1 : index;
	[self.delegate tabBar:self didSelectIndex:index];
}

- (void)mouseDragged:(NSEvent *)event {
	if (_dragIndex < 0) return;
	const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
	const NSInteger over = [self indexAtPoint:p];
	if (over >= 0 && over != _dragIndex) {
		_dragged = YES;
		const NSInteger from = _dragIndex;
		_dragIndex = over;
		[self.delegate tabBar:self didMoveIndex:from toIndex:over];
	}
}

- (void)mouseUp:(NSEvent *)event { _dragIndex = -1; }

- (void)otherMouseDown:(NSEvent *)event {
	const NSInteger index = [self indexAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
	if (index >= 0) [self.delegate tabBar:self didRequestCloseIndex:index];
}

- (void)scrollWheel:(NSEvent *)event {
	_offset -= _vertical ? event.scrollingDeltaY : (event.scrollingDeltaX != 0 ? event.scrollingDeltaX : event.scrollingDeltaY);
	[self clampOffset];
	[self rebuildToolTips];
	self.needsDisplay = YES;
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
	const NSInteger index = [self indexAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
	return index >= 0 ? [self.delegate tabBar:self menuForIndex:index] : nil;
}

@end
