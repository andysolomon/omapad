#include "Print.h"

#include "Scintilla.h"

#include <filesystem>

namespace omapad {

namespace {

const int kNumberMargin = 0;
const int kCaptionGap = 8;

std::string formatTime(std::time_t when, const char *format) {
	char buffer[128];
	std::tm local{};
	localtime_r(&when, &local);
	return std::strftime(buffer, sizeof buffer, format, &local) ? buffer : "";
}

void replaceAll(std::string &text, const std::string &what, const std::string &with) {
	for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size())) text.replace(at, what.size(), with);
}

}  // namespace

PrintJob::PrintJob(SciView &v, const PrintOptions &o) : view(v), options(o), startedAt(std::time(nullptr)) {
	savedColourMode = view.send(SCI_GETPRINTCOLOURMODE);
	savedMargin = view.send(SCI_GETMARGINWIDTHN, kNumberMargin);
	view.send(SCI_SETPRINTCOLOURMODE, options.colourMode);
	view.send(SCI_SETPRINTWRAPMODE, SC_WRAP_WORD);
	view.send(SCI_SETPRINTMAGNIFICATION, 0);
	// Scintilla sizes the printed margin itself. It only needs to be on or off.
	view.send(SCI_SETMARGINWIDTHN, kNumberMargin, options.lineNumbers ? (savedMargin > 0 ? savedMargin : 1) : 0);
}

PrintJob::~PrintJob() {
	view.send(SCI_SETPRINTCOLOURMODE, savedColourMode);
	view.send(SCI_SETMARGINWIDTHN, kNumberMargin, savedMargin);
}

int PrintJob::headerHeight() const { return options.header.empty() ? 0 : kCaptionPoints + kCaptionGap; }
int PrintJob::footerHeight() const { return options.footer.empty() ? 0 : kCaptionPoints + kCaptionGap; }

intptr_t PrintJob::format(void *surface, bool draw, intptr_t from, int width, int height) {
	Sci_RangeToFormatFull range{};
	range.hdc = surface;
	range.hdcTarget = surface;
	range.rc.left = 0;
	range.rc.top = headerHeight();
	range.rc.right = width;
	range.rc.bottom = height - footerHeight();
	range.rcPage.left = 0;
	range.rcPage.top = 0;
	range.rcPage.right = width;
	range.rcPage.bottom = height;
	range.chrg.cpMin = from;
	range.chrg.cpMax = options.end;
	return view.send(SCI_FORMATRANGEFULL, draw, reinterpret_cast<intptr_t>(&range));
}

void PrintJob::paginate(void *surface, int width, int height) {
	pageStarts.clear();
	for (intptr_t at = options.start; at < options.end;) {
		const intptr_t next = format(surface, false, at, width, height);
		pageStarts.push_back(at);
		if (next <= at) break;  // The page is too small to fit anything. Don't loop for ever.
		at = next;
	}
	if (pageStarts.empty()) pageStarts.push_back(options.start);  // An empty document is one blank page.
}

void PrintJob::drawPage(void *surface, int page, int width, int height) {
	if (page >= 0 && page < pageCount()) format(surface, true, pageStarts[page], width, height);
}

std::string PrintJob::expand(const std::string &text, int page) const {
	std::string out = text;
	replaceAll(out, "$(FULL_CURRENT_PATH)", options.path.empty() ? options.title : options.path);
	replaceAll(out, "$(FILE_NAME)", options.title);
	replaceAll(out, "$(SHORT_DATE)", formatTime(startedAt, "%x"));
	replaceAll(out, "$(LONG_DATE)", formatTime(startedAt, "%A, %B %e, %Y"));
	replaceAll(out, "$(TIME)", formatTime(startedAt, "%X"));
	replaceAll(out, "$(CURRENT_PRINTING_PAGE)", std::to_string(page + 1));
	replaceAll(out, "$(TOTAL_PAGES)", std::to_string(pageCount()));
	return out;
}

PrintPageText PrintJob::expand(const PrintPageText &text, int page) const {
	return {expand(text.left, page), expand(text.middle, page), expand(text.right, page)};
}

}  // namespace omapad
