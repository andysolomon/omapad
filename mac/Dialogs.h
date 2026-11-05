// The generated settings form and the Shortcut Mapper.
#pragma once
#import <Cocoa/Cocoa.h>
#include "Workspace.h"

// Shows `fields` in a modal window and writes the answers back. NO means it was cancelled.
BOOL OmaRunForm(NSString *title, std::vector<omapad::FormField> &fields);

// Lets the person pick a command and press a new shortcut for it.
void OmaRunShortcutMapper(omapad::Workspace &workspace);

// The event as a shortcut like "Mod+Shift+F", or empty if it isn't usable as one.
std::string OmaShortcutFromEvent(NSEvent *event);

// Sets the key equivalent for a shortcut like "Mod+Shift+F".
void OmaApplyShortcut(const std::string &shortcut, NSMenuItem *item);

// Runs a print job, or writes it to a PDF. Cancelling returns YES.
BOOL OmaRunPrint(omapad::PrintJob &job, const omapad::PrintOutput &output, std::string &error);
