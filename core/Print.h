// A print job: works out the pages and draws them with Scintilla. Each platform supplies the
// drawing surface (a CGContext or a cairo context) and the dialog. All sizes are in points.
#pragma once
#include "Languages.h"

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace omapad {

struct PrintPageText {
	std::string left, middle, right;
	bool empty() const { return left.empty() && middle.empty() && right.empty(); }
};

struct PrintOptions {
	intptr_t start = 0, end = 0;
	int colourMode = 0;  // An SC_PRINT_ value.
	bool lineNumbers = true;
	PrintPageText header, footer;  // May hold variables like $(FILE_NAME).
	std::string title, path;
	int marginTop = 15, marginBottom = 15, marginLeft = 15, marginRight = 15;  // In millimetres.
};

// What to do with a job.
struct PrintOutput {
	bool showDialog = true;
	std::string pdfPath;  // Set to write a PDF instead of printing.
};

class PrintJob {
public:
	// Changes how the view prints, until the job is destroyed. The view must outlive the job.
	PrintJob(SciView &view, const PrintOptions &options);
	~PrintJob();
	PrintJob(const PrintJob &) = delete;
	PrintJob &operator=(const PrintJob &) = delete;

	// Width and height are the page without its margins. Call again if the paper changes.
	void paginate(void *surface, int width, int height);
	int pageCount() const { return static_cast<int>(pageStarts.size()); }
	intptr_t pageStart(int page) const { return pageStarts[page]; }
	void drawPage(void *surface, int page, int width, int height);

	// The band at the top and bottom of a page, in points. Zero if there's nothing to show in it.
	int headerHeight() const;
	int footerHeight() const;
	static constexpr int kCaptionPoints = 9;
	PrintPageText header(int page) const { return expand(options.header, page); }
	PrintPageText footer(int page) const { return expand(options.footer, page); }

	const PrintOptions &settings() const { return options; }

private:
	intptr_t format(void *surface, bool draw, intptr_t from, int width, int height);
	PrintPageText expand(const PrintPageText &text, int page) const;
	std::string expand(const std::string &text, int page) const;

	SciView &view;
	PrintOptions options;
	std::vector<intptr_t> pageStarts;
	std::time_t startedAt;
	intptr_t savedColourMode, savedMargin;
};

}  // namespace omapad
