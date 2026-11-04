#import <Cocoa/Cocoa.h>
#include "TestViews.h"
#import "ScintillaView.h"
#import "Scintilla.h"

@interface TestDelegate : NSObject <ScintillaNotificationProtocol>
@property(nonatomic) std::function<void(const SCNotification &)> handler;
@end
@implementation TestDelegate
- (void)notification:(SCNotification *)n {
	if (self.handler) self.handler(*n);
}
@end

namespace {
struct MacView : omapad::SciView {
	ScintillaView *view;
	TestDelegate *delegate;
	intptr_t send(unsigned m, uintptr_t w, intptr_t l) override { return [view message:m wParam:w lParam:l]; }
};
}

void initTestToolkit() { [NSApplication sharedApplication]; }

std::unique_ptr<omapad::SciView> makeHiddenSciView(std::function<void(const SCNotification &)> notify) {
	auto out = std::make_unique<MacView>();
	out->view = [[ScintillaView alloc] initWithFrame:NSMakeRect(0, 0, 800, 600)];
	out->delegate = [TestDelegate new];
	out->delegate.handler = notify;
	out->view.delegate = out->delegate;
	return out;
}
