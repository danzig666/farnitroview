// FarNitroView for Far Manager: PDF pages through PDFium (pdfium.dll next to the plugin, loaded on
// first use: without it there is no PDF, everything else works).
//
// PDFium is not thread safe: every call is made under one lock, and a rendering gives the lock up
// between short slices. A document stays open (by path, size and time) as long as a page of it is
// in use, so going through the pages of a large file does not read it again and again.
#pragma once
#include "render.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

struct PdfDoc;

struct PdfPage {
    std::shared_ptr<PdfDoc> doc;
    int index = 0;
    void *handle = nullptr;   // FPDF_PAGE: parsed on the first rendering, then kept
    std::mutex rendering;     // (PDFium: a page renders once at a time)
    ~PdfPage();
};

// Page `page` of a file (not parsed yet, so this is quick), its size in points, how many the file has.
std::shared_ptr<PdfPage> PdfOpenPage(const std::wstring &path, int page, int *pages, double *width, double *height,
                                     std::wstring *error);

// A window's renderings of PDF pages, on a thread of its own: the latest request only, what is
// being rendered is abandoned when something else is asked for. The grid below is the page as
// shown, whole pixels at the scale, its corner on a whole screen pixel.
class PdfView {
public:
    enum State {
        Nothing,   // nothing of this page is there yet
        Preview,   // it can be shown from another scale, or from a rendering that is not finished
        Sharp      // it is there as it should be
    };
    explicit PdfView(std::function<void()> notify);
    ~PdfView();
    // Asks for what is missing and gives a quick rendering a moment to finish.
    State Prepare(const Image &im, const Orient &o, double scale, int W, int H, double left, double top);
    // The page's visible part into dst (W x H), from what is there.
    void Draw(const Image &im, const Orient &o, double scale, int W, int H, double left, double top, uint8_t *dst,
              size_t pitch);
    bool Busy();
    void Wait();                   // until nothing is being rendered
    ULONGLONG NothingSince();      // when the page with nothing to show was first asked for

private:
    struct Result {
        std::shared_ptr<PdfPage> page;
        Orient o;
        double scale = 0;
        RECT r = {};               // what it holds, in the grid
        bool partial = false;      // a rendering not finished yet
        std::vector<uint32_t> px;  // rows of r's width
    };
    struct Job {
        std::shared_ptr<PdfPage> page;
        Orient o;
        double scale = 0;
        int gridW = 0, gridH = 0;
        RECT visible = {}, cover = {};   // rendered in this order; cover: more around it, for panning
    };
    void Run();
    void Render(const Job &j, const RECT &r, uint64_t gen, bool first, Result *out);

    std::function<void()> notify_;
    std::mutex mu_;
    std::condition_variable wake_, finished_;
    Job job_;
    bool queued_ = false, active_ = false, stop_ = false;
    std::atomic<uint64_t> gen_{0};
    std::shared_ptr<const Result> done_;
    std::shared_ptr<PdfPage> askedPage_;
    Orient askedOrient_;
    ULONGLONG askedTick_ = 0;
    std::thread worker_;
};
