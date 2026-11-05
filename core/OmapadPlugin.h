/* The omapad plugin interface. A plugin is a shared library at
   <settings>/plugins/<name>/<name>.dylib on a Mac or .so on Linux, exporting omapadPluginInit.
   Notepad++ plugins are Windows DLLs and can't load here. */
#ifndef OMAPAD_PLUGIN_H
#define OMAPAD_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OMAPAD_PLUGIN_API_VERSION 1

typedef struct OmapadHostApi {
	int apiVersion;
	/* Sends a Scintilla message to the active editor. */
	intptr_t (*send)(unsigned message, uintptr_t wParam, intptr_t lParam);
	int (*openFile)(const char *path);
	void (*showMessage)(const char *text);
	/* Writes the active file's path, returning its full length. */
	size_t (*activePath)(char *out, size_t capacity);
} OmapadHostApi;

typedef struct OmapadCommand {
	const char *title;
	const char *shortcut; /* Like "Mod+Alt+J", or NULL. */
	void (*run)(void);
} OmapadCommand;

typedef struct OmapadPluginInfo {
	int apiVersion;
	const char *name;
	const char *description;
	const OmapadCommand *commands;
	int commandCount;
} OmapadPluginInfo;

/* Called once at startup. The returned data must stay valid. */
typedef const OmapadPluginInfo *(*OmapadPluginInit)(const OmapadHostApi *host);
/* Optional. Called with the event's name: "fileOpened", "fileSaved", "tabChanged". */
typedef void (*OmapadPluginNotify)(const char *event, const char *path);

#ifdef __cplusplus
}
#endif
#endif
