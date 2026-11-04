#import "TabBar.h"

@implementation OmaTabItem
@end

static const CGFloat kMinTab = 90;
static const CGFloat kMaxTab = 220;
static const CGFloat kCloseSize = 14;
static const CGFloat kPadding = 10;

@implementation TabBar {
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
	}
	return self;
}

- (BOOL)isFlipped { return YES; }

- (NSDictionary *)textAttributes:(BOOL)active {
	return @{
		NSFontAttributeName: [NSFont systemFontOfSize:12 weight:active ? NSFontWeightMedium : NSFontWeightRegular],
		NSForegroundColorAttributeName: active ? NSColor.labelColor : NSColor.secondaryLabelColor,
	};
}

- (NSString *)displayTitle:(OmaTabItem *)item {
	return item.dirty ? [@"● " stringByAppendingString:item.title] : item.title;
}

- (CGFloat)widthOfItem:(OmaTabItem *)item {
	const CGFloat text = [[self displayTitle:item] sizeWithAttributes:[self textAttributes:YES]].width;
	return MIN(kMaxTab, MAX(kMinTab, ceil(text) + kPadding * 2 + kCloseSize + 4));
}

- (NSRect)rectOfIndex:(NSInteger)index {
	CGFloat x = -_offset;
	for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
		const CGFloat w = [self widthOfItem:_items[i]];
		if (i == index) return NSMakeRect(x, 0, w, NSHeight(self.bounds));
		x += w;
	}
	return NSZeroRect;
}

- (CGFloat)totalWidth {
	CGFloat total = 0;
	for (OmaTabItem *item in _items) total += [self widthOfItem:item];
	return total;
}

- (NSRect)closeRectForTab:(NSRect)tab {
	return NSMakeRect(NSMaxX(tab) - kCloseSize - 6, floor((NSHeight(tab) - kCloseSize) / 2), kCloseSize, kCloseSize);
}

- (NSInteger)indexAtPoint:(NSPoint)point {
	for (NSInteger i = 0; i < (NSInteger)_items.count; i++)
		if (NSPointInRect(point, [self rectOfIndex:i])) return i;
	return -1;
}

- (void)clampOffset {
	const CGFloat maxOffset = MAX(0, [self totalWidth] - NSWidth(self.bounds));
	_offset = MAX(0, MIN(_offset, maxOffset));
}

- (void)setItems:(NSArray<OmaTabItem *> *)items activeIndex:(NSInteger)active focused:(BOOL)focused {
	_focused = focused;
	const BOOL activeChanged = active != _active;
	_items = [items copy];
	_active = active;
	if (activeChanged && active >= 0 && active < (NSInteger)items.count) {
		NSRect r = [self rectOfIndex:active];
		if (NSMinX(r) < 0) _offset += NSMinX(r);
		else if (NSMaxX(r) > NSWidth(self.bounds)) _offset += NSMaxX(r) - NSWidth(self.bounds);
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
		if (NSMaxX(tab) < 0 || NSMinX(tab) > NSWidth(self.bounds)) continue;
		OmaTabItem *item = _items[i];
		const BOOL active = i == _active;
		if (active) {
			[NSColor.textBackgroundColor setFill];
			NSRectFill(tab);
			[(_focused ? NSColor.controlAccentColor : NSColor.tertiaryLabelColor) setFill];
			NSRectFill(NSMakeRect(NSMinX(tab), 0, NSWidth(tab), 2));
		}
		[NSColor.separatorColor setFill];
		NSRectFillUsingOperation(NSMakeRect(NSMaxX(tab) - 1, 4, 1, NSHeight(tab) - 8), NSCompositingOperationSourceOver);

		const NSRect close = [self closeRectForTab:tab];
		NSMutableParagraphStyle *paragraph = [NSMutableParagraphStyle new];
		paragraph.lineBreakMode = NSLineBreakByTruncatingMiddle;
		NSMutableDictionary *attributes = [[self textAttributes:active] mutableCopy];
		attributes[NSParagraphStyleAttributeName] = paragraph;
		NSString *title = [self displayTitle:item];
		const CGFloat textHeight = [title sizeWithAttributes:attributes].height;
		[title drawInRect:NSMakeRect(NSMinX(tab) + kPadding, floor((NSHeight(tab) - textHeight) / 2),
		                             NSMinX(close) - NSMinX(tab) - kPadding - 2, textHeight)
		   withAttributes:attributes];

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
	const NSInteger hover = (index >= 0 && NSPointInRect(p, [self closeRectForTab:[self rectOfIndex:index]])) ? index : -1;
	if (hover != _hoverClose) {
		_hoverClose = hover;
		self.needsDisplay = YES;
	}
}

- (void)mouseExited:(NSEvent *)event {
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
	if (NSPointInRect(p, [self closeRectForTab:[self rectOfIndex:index]])) {
		[self.delegate tabBar:self didRequestCloseIndex:index];
		return;
	}
	_dragIndex = index;
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
	_offset -= event.scrollingDeltaX != 0 ? event.scrollingDeltaX : event.scrollingDeltaY;
	[self clampOffset];
	[self rebuildToolTips];
	self.needsDisplay = YES;
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
	const NSInteger index = [self indexAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
	return index >= 0 ? [self.delegate tabBar:self menuForIndex:index] : nil;
}

@end
