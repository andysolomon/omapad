#include <gtk/gtk.h>
#include "TestViews.h"
#include "Scintilla.h"
#include "ScintillaWidget.h"

namespace {
struct GtkSciView : omapad::SciView {
	GtkWidget *widget;
	std::function<void(const SCNotification &)> notify;
	intptr_t send(unsigned m, uintptr_t w, intptr_t l) override { return scintilla_send_message(SCINTILLA(widget), m, w, l); }
};
void onNotify(GtkWidget *, gint, SCNotification *n, gpointer data) {
	auto *self = static_cast<GtkSciView *>(data);
	if (self->notify) self->notify(*n);
}
}

void initTestToolkit() {
	int argc = 0;
	char **argv = nullptr;
	if (!gtk_init_check(&argc, &argv)) {
		fprintf(stderr, "No display available for the tests\n");
		exit(2);
	}
}

std::unique_ptr<omapad::SciView> makeHiddenSciView(std::function<void(const SCNotification &)> notify) {
	auto out = std::make_unique<GtkSciView>();
	out->widget = scintilla_new();
	g_object_ref_sink(out->widget);
	out->notify = std::move(notify);
	g_signal_connect(out->widget, SCINTILLA_NOTIFY, G_CALLBACK(onNotify), out.get());
	return out;
}

void *makeTestSurface() {
	cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 600, 800);
	cairo_t *context = cairo_create(image);
	cairo_surface_destroy(image);
	return context;
}

void freeTestSurface(void *surface) { cairo_destroy(static_cast<cairo_t *>(surface)); }

// GtkPrintOperation needs the application's window, so the Linux PDF is checked by driving the app.
bool writeTestPdf(omapad::PrintJob &, const omapad::PrintOutput &, std::string &) { return false; }
