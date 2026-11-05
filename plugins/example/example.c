/* A small omapad plugin. Build it on a Mac with:
     cc -dynamiclib -I../../core example.c -o example.dylib
   or on Linux with:
     cc -shared -fPIC -I../../core example.c -o example.so
   and put it in <settings>/plugins/example/. */
#include "OmapadPlugin.h"

#define SCI_INSERTTEXT 2003

static const OmapadHostApi *host;

static void insertGreeting(void) {
	host->send(SCI_INSERTTEXT, (uintptr_t)-1, (intptr_t)"Hello from a plugin\n");
	host->showMessage("The example plugin ran");
}

static const OmapadCommand commands[] = {
	{"Insert Greeting", "Mod+Alt+J", insertGreeting},
};

static const OmapadPluginInfo info = {OMAPAD_PLUGIN_API_VERSION, "Example Plugin", "Shows how plugins work", commands, 1};

const OmapadPluginInfo *omapadPluginInit(const OmapadHostApi *api) {
	host = api;
	return &info;
}
