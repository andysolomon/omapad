// A compact in-window tab strip, drawn natively so it follows light and dark mode.
#pragma once
#import <Cocoa/Cocoa.h>

@interface OmaTabItem : NSObject
@property(nonatomic, copy) NSString *title;
@property(nonatomic, copy) NSString *toolTip;
@property(nonatomic) BOOL dirty;
@property(nonatomic) BOOL pinned;
@property(nonatomic) NSInteger colour;  // 1 to 5, or 0 for none.
@property(nonatomic, copy) NSString *display;  // The title with its marks.
@end

@class TabBar;
@protocol TabBarDelegate <NSObject>
- (void)tabBar:(TabBar *)bar didSelectIndex:(NSInteger)index;
- (void)tabBar:(TabBar *)bar didRequestCloseIndex:(NSInteger)index;
- (void)tabBar:(TabBar *)bar didMoveIndex:(NSInteger)from toIndex:(NSInteger)to;
- (void)tabBarDidRequestNewTab:(TabBar *)bar;
- (NSMenu *)tabBar:(TabBar *)bar menuForIndex:(NSInteger)index;
- (void)tabBar:(TabBar *)bar didDoubleClickIndex:(NSInteger)index;
- (void)tabBar:(TabBar *)bar didTogglePinIndex:(NSInteger)index;
- (void)tabBarDidRequestList:(TabBar *)bar sender:(NSButton *)button;
- (void)tabBarDidRequestCloseActive:(TabBar *)bar;
- (void)tabBar:(TabBar *)bar didHoverIndex:(NSInteger)index;
@end

@interface TabBar : NSView
@property(nonatomic, weak) id<TabBarDelegate> delegate;
@property(nonatomic) BOOL vertical, multiLine, locked, reduced, showClose, activeBar, inactiveColour;
@property(nonatomic) BOOL showPin, buttonsOnInactive, rightShortcuts;
- (void)setItems:(NSArray<OmaTabItem *> *)items activeIndex:(NSInteger)active focused:(BOOL)focused;
// How tall the bar wants to be at this width, for multi-line tabs.
- (CGFloat)heightForWidth:(CGFloat)width;
@end
