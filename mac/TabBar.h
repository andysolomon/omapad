// A compact in-window tab strip, drawn natively so it follows light and dark mode.
#pragma once
#import <Cocoa/Cocoa.h>

@interface OmaTabItem : NSObject
@property(nonatomic, copy) NSString *title;
@property(nonatomic, copy) NSString *toolTip;
@property(nonatomic) BOOL dirty;
@end

@class TabBar;
@protocol TabBarDelegate <NSObject>
- (void)tabBar:(TabBar *)bar didSelectIndex:(NSInteger)index;
- (void)tabBar:(TabBar *)bar didRequestCloseIndex:(NSInteger)index;
- (void)tabBar:(TabBar *)bar didMoveIndex:(NSInteger)from toIndex:(NSInteger)to;
- (void)tabBarDidRequestNewTab:(TabBar *)bar;
- (NSMenu *)tabBar:(TabBar *)bar menuForIndex:(NSInteger)index;
@end

@interface TabBar : NSView
@property(nonatomic, weak) id<TabBarDelegate> delegate;
- (void)setItems:(NSArray<OmaTabItem *> *)items activeIndex:(NSInteger)active focused:(BOOL)focused;
@end
