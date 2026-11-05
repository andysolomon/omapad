// A real Scintilla view with no window, supplied by each platform.
#pragma once
#include "Languages.h"
#include "Print.h"
#include <functional>
#include <memory>

void initTestToolkit();
std::unique_ptr<omapad::SciView> makeHiddenSciView(std::function<void(const struct SCNotification &)> notify);

// A surface for Scintilla to measure and draw on: a bitmap, since nothing needs to see it.
void *makeTestSurface();
void freeTestSurface(void *surface);

// Writes a real PDF the way the app does, if the platform can without a display server. False if it can't.
bool writeTestPdf(omapad::PrintJob &job, const omapad::PrintOutput &output, std::string &error);
