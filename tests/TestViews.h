// A real Scintilla view with no window, supplied by each platform.
#pragma once
#include "Languages.h"
#include <functional>
#include <memory>

void initTestToolkit();
std::unique_ptr<omapad::SciView> makeHiddenSciView(std::function<void(const struct SCNotification &)> notify);
