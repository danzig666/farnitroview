// FarNitroView for Far Manager: PDF pages through PDFium.
#include "pdf.h"
#include "par.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ---------------------------------------------------------------------------
// the part of PDFium's C API we use (fpdfview.h, fpdf_progressive.h), from pdfium.dll

namespace {
typedef void *FPDF_DOCUMENT, *FPDF_PAGE, *FPDF_BITMAP;

struct FPDF_FILEACCESS {
    unsigned long m_FileLen;
    int (*m_GetBlock)(void *param, unsigned long position, unsigned char *buf, unsigned long size);
    void *m_Param;
};
struct IFSDK_PAUSE {
    int version;
    int (*NeedToPauseNow)(IFSDK_PAUSE *self);
    void *user;
};
enum { FPDF_ERR_FILE = 2, FPDF_ERR_FORMAT = 3, FPDF_ERR_PASSWORD = 4, FPDF_ERR_SECURITY = 5 };
enum { FPDF_ANNOT = 1, FPDFBitmap_BGRA = 4 };
enum { FPDF_RENDER_TOBECONTINUED = 1, FPDF_RENDER_DONE = 2 };

struct {
    void (*InitLibrary)();
    FPDF_DOCUMENT (*LoadCustomDocument)(FPDF_FILEACCESS *, const char *password);
    void (*CloseDocument)(FPDF_DOCUMENT);
    unsigned long (*GetLastError)();
    int (*GetPageCount)(FPDF_DOCUMENT);
    int (*GetPageSizeByIndex)(FPDF_DOCUMENT, int index, double *w, double *h);
    FPDF_PAGE (*LoadPage)(FPDF_DOCUMENT, int index);
    void (*ClosePage)(FPDF_PAGE);
    FPDF_BITMAP (*BitmapCreateEx)(int w, int h, int format, void *first, int stride);
    int (*BitmapFillRect)(FPDF_BITMAP, int left, int top, int w, int h, unsigned long color);
    void (*BitmapDestroy)(FPDF_BITMAP);
    int (*RenderStart)(FPDF_BITMAP, FPDF_PAGE, int x, int y, int w, int h, int rotate, int flags, IFSDK_PAUSE *);
    int (*RenderContinue)(FPDF_PAGE, IFSDK_PAUSE *);
    void (*RenderClose)(FPDF_PAGE);
} A;

bool LoadApi() {
    std::wstring dll(32768, L'\0');
    DWORD n = GetModuleFileNameW((HMODULE)&__ImageBase, &dll[0], (DWORD)dll.size());
    dll.resize(n);
    dll.resize(dll.find_last_of(L"\\/") + 1);
    dll += L"pdfium.dll";
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return false;
    const struct { void *fn; const char *name; } t[] = {
        {&A.InitLibrary, "FPDF_InitLibrary"},
        {&A.LoadCustomDocument, "FPDF_LoadCustomDocument"},
        {&A.CloseDocument, "FPDF_CloseDocument"},
        {&A.GetLastError, "FPDF_GetLastError"},
        {&A.GetPageCount, "FPDF_GetPageCount"},
        {&A.GetPageSizeByIndex, "FPDF_GetPageSizeByIndex"},
        {&A.LoadPage, "FPDF_LoadPage"},
        {&A.ClosePage, "FPDF_ClosePage"},
        {&A.BitmapCreateEx, "FPDFBitmap_CreateEx"},
        {&A.BitmapFillRect, "FPDFBitmap_FillRect"},
        {&A.BitmapDestroy, "FPDFBitmap_Destroy"},
        {&A.RenderStart, "FPDF_RenderPageBitmap_Start"},
        {&A.RenderContinue, "FPDF_RenderPage_Continue"},
        {&A.RenderClose, "FPDF_RenderPage_Close"},
    };
    for (auto &e : t) {
        FARPROC p = GetProcAddress(m, e.name);
        if (!p) return false;
        *(FARPROC *)e.fn = p;
    }
    A.InitLibrary();
    return true;
}

std::mutex Mu;   // around every PDFium call (and Docs)
std::map<std::wstring, std::weak_ptr<PdfDoc>> Docs;
std::atomic<bool> Enabled{true};

int GetBlock(void *file, unsigned long pos, unsigned char *buf, unsigned long size) {
    OVERLAPPED o = {};
    o.Offset = pos;
    DWORD n = 0;
    return ReadFile((HANDLE)file, buf, size, &n, &o) && n == size;
}
}   // namespace

bool HavePdf() {
    static const bool loaded = LoadApi();   // (only when a PDF comes along)
    return Enabled && loaded;
}

void SetPdfEnabled(bool on) { Enabled = on; }

struct PdfDoc {
    std::wstring key;
    HANDLE file = INVALID_HANDLE_VALUE;
    FPDF_FILEACCESS access = {};
    FPDF_DOCUMENT doc = nullptr;
    int pages = 0;
    ~PdfDoc() {
        std::lock_guard<std::mutex> l(Mu);
        if (doc) A.CloseDocument(doc);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        auto it = Docs.find(key);
        if (it != Docs.end() && it->second.expired()) Docs.erase(it);
    }
};

PdfPage::~PdfPage() {
    std::lock_guard<std::mutex> l(Mu);
    if (handle) A.ClosePage(handle);
}

std::shared_ptr<PdfPage> PdfOpenPage(const std::wstring &path, int page, int *pages, double *width, double *height,
                                     std::wstring *error) {
    if (!HavePdf()) {
        *error = L"PDF needs pdfium.dll next to the plugin";
        return nullptr;
    }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        *error = L"Cannot read the file";
        return nullptr;
    }
    const unsigned long long size = (unsigned long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    const unsigned long long time = (unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
    if (size > 0xFFFFFFFFu) {   // (the limit of PDFium's FPDF_FILEACCESS on Windows)
        *error = L"PDF files over 4 GB are not supported";
        return nullptr;
    }
    std::wstring key = path + L"|" + std::to_wstring(size) + L"|" + std::to_wstring(time);

    std::shared_ptr<PdfDoc> d;   // (before the lock: released after it, its destructor takes the lock)
    std::lock_guard<std::mutex> l(Mu);
    auto it = Docs.find(key);
    if (it != Docs.end()) d = it->second.lock();
    if (!d) {
        d = std::make_shared<PdfDoc>();
        d->key = key;
        d->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (d->file == INVALID_HANDLE_VALUE) {
            *error = L"Cannot read the file";
            return nullptr;
        }
        d->access.m_FileLen = (unsigned long)size;
        d->access.m_GetBlock = GetBlock;
        d->access.m_Param = d->file;
        d->doc = A.LoadCustomDocument(&d->access, nullptr);
        if (!d->doc) {
            switch (A.GetLastError()) {
            case FPDF_ERR_PASSWORD: *error = L"The PDF is protected by a password"; break;
            case FPDF_ERR_SECURITY: *error = L"The PDF uses an unsupported security scheme"; break;
            case FPDF_ERR_FORMAT: *error = L"Not a PDF, or a damaged one"; break;
            case FPDF_ERR_FILE: *error = L"Cannot read the file"; break;
            default: *error = L"Cannot open the PDF"; break;
            }
            return nullptr;
        }
        d->pages = A.GetPageCount(d->doc);
        if (d->pages <= 0) {
            *error = L"The PDF has no pages";
            return nullptr;
        }
        Docs[key] = d;
    }
    if (page < 0 || page >= d->pages) page = 0;
    if (!A.GetPageSizeByIndex(d->doc, page, width, height) || *width <= 0 || *height <= 0) {
        *error = L"Damaged PDF page";
        return nullptr;
    }
    auto p = std::make_shared<PdfPage>();
    p->doc = d;
    p->index = page;
    *pages = d->pages;
    return p;
}

// ---------------------------------------------------------------------------
// rendering in slices

namespace {
enum { kSliceMs = 30, kQuickMs = 60, kFirstPartialMs = 600, kPartialMs = 300 };
const double kCoverPixels = 16e6;

struct Pause : IFSDK_PAUSE {
    ULONGLONG until;
    const std::function<bool()> *cancel;
};
int NeedToPause(IFSDK_PAUSE *self) {
    Pause *p = static_cast<Pause *>(self);
    return GetTickCount64() >= p->until || (*p->cancel)();
}

// Into buf (bw x bh, white first): the page at sizeX x sizeY pixels, with this pixel of it at the
// corner. False when cancelled. tick: called between the slices. A page PDFium fails on stays white.
bool RenderPage(PdfPage *p, uint32_t *buf, int bw, int bh, int startX, int startY, int sizeX, int sizeY,
                const std::function<bool()> &cancel, const std::function<void()> &tick) {
    std::fill(buf, buf + (size_t)bw * bh, 0xFFFFFFFFu);
    std::lock_guard<std::mutex> one(p->rendering);
    std::unique_lock<std::mutex> l(Mu);
    if (!p->handle) p->handle = A.LoadPage(p->doc->doc, p->index);   // (the long part of a heavy page, cannot be stopped)
    if (!p->handle) return true;
    FPDF_BITMAP bmp = A.BitmapCreateEx(bw, bh, FPDFBitmap_BGRA, buf, bw * 4);
    if (!bmp) return true;
    Pause pz;
    pz.version = 1;
    pz.NeedToPauseNow = NeedToPause;
    pz.user = nullptr;
    pz.cancel = &cancel;
    pz.until = GetTickCount64() + kSliceMs;
    int st = A.RenderStart(bmp, p->handle, -startX, -startY, sizeX, sizeY, 0, FPDF_ANNOT, &pz);
    bool cancelled = false;
    while (st == FPDF_RENDER_TOBECONTINUED) {
        l.unlock();
        cancelled = cancel();
        if (!cancelled) tick();
        l.lock();
        if (cancelled) break;
        pz.until = GetTickCount64() + kSliceMs;
        st = A.RenderContinue(p->handle, &pz);
    }
    A.RenderClose(p->handle);
    A.BitmapDestroy(bmp);
    return !cancelled;
}

// a rendering in the source frame -> the page as shown
void Turn(const Orient &o, const uint32_t *src, int SW, int SH, uint32_t *out, int RW, int RH) {
    const int *m = o.m;
    ParallelFor((size_t)RH, [&](size_t b) {
        const int db = 2 * (int)b + 1 - RH;
        for (int a = 0; a < RW; a++) {
            int da = 2 * a + 1 - RW;
            int px = (m[0] * da + m[1] * db + SW - 1) / 2, py = (m[2] * da + m[3] * db + SH - 1) / 2;
            out[b * RW + a] = src[(size_t)py * SW + px];
        }
    });
}

bool SamePage(const std::shared_ptr<PdfPage> &a, const std::shared_ptr<PdfPage> &b) {
    return a && b && a->doc == b->doc && a->index == b->index;
}

bool Inside(const RECT &in, const RECT &r) {
    return in.left >= r.left && in.top >= r.top && in.right <= r.right && in.bottom <= r.bottom;
}

struct Grid {
    int ox, oy;     // the page's corner on the screen
    int w, h;       // the page, as shown
    RECT visible;   // its visible part, in the grid
};

Grid MakeGrid(const Image &im, const Orient &o, double scale, int W, int H, double left, double top) {
    int ow, oh;
    OrientedSize(im, o, &ow, &oh);
    Grid g;
    g.ox = (int)std::lround(left);
    g.oy = (int)std::lround(top);
    g.w = std::max(1, (int)std::lround(ow * scale));
    g.h = std::max(1, (int)std::lround(oh * scale));
    g.visible = {std::max(0, -g.ox), std::max(0, -g.oy), std::min(g.w, W - g.ox), std::min(g.h, H - g.oy)};
    return g;
}

// the visible part with as much around it as is reasonable to keep
RECT CoverOf(const RECT &v, int gridW, int gridH, int W, int H) {
    for (double f : {1.0, 0.75, 0.5, 0.25}) {
        RECT c = {std::max(0L, v.left - (LONG)(W * f)), std::max(0L, v.top - (LONG)(H * f)),
                  std::min((LONG)gridW, v.right + (LONG)(W * f)), std::min((LONG)gridH, v.bottom + (LONG)(H * f))};
        if ((double)(c.right - c.left) * (c.bottom - c.top) <= kCoverPixels) return c;
    }
    return v;
}
}   // namespace

PdfView::PdfView(std::function<void()> notify) : notify_(std::move(notify)), worker_([this] { Run(); }) {}

PdfView::~PdfView() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
        gen_++;
    }
    wake_.notify_all();
    worker_.join();
}

void PdfView::Render(const Job &j, const RECT &r, uint64_t gen, bool first, Result *out) {
    const int RW = r.right - r.left, RH = r.bottom - r.top;
    const bool tr = j.o.Transposed();
    const int SW = tr ? RH : RW, SH = tr ? RW : RH;           // the rendering in the source frame
    const int FW = tr ? j.gridH : j.gridW, FH = tr ? j.gridW : j.gridH;   // the whole page in it
    // the corner of the source rectangle
    const int *m = j.o.m;
    double sx0 = 1e300, sy0 = 1e300;
    for (int c = 0; c < 4; c++) {
        double du = (c & 1 ? r.right : r.left) - j.gridW / 2.0, dv = (c & 2 ? r.bottom : r.top) - j.gridH / 2.0;
        sx0 = std::min(sx0, FW / 2.0 + m[0] * du + m[1] * dv);
        sy0 = std::min(sy0, FH / 2.0 + m[2] * du + m[3] * dv);
    }
    std::vector<uint32_t> src((size_t)SW * SH);
    out->page = j.page;
    out->o = j.o;
    out->scale = j.scale;
    out->r = r;

    const std::function<bool()> cancel = [&] { return gen_ != gen; };
    ULONGLONG nextPartial = GetTickCount64() + kFirstPartialMs;
    // a slow page with nothing to show yet comes in as it is being drawn
    const std::function<void()> tick = [&] {
        if (!first || GetTickCount64() < nextPartial) return;
        nextPartial = GetTickCount64() + kPartialMs;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (done_ && !done_->partial && SamePage(done_->page, j.page) && done_->o == j.o) return;
        }
        auto part = std::make_shared<Result>();
        part->page = j.page;
        part->o = j.o;
        part->scale = j.scale;
        part->r = r;
        part->partial = true;
        part->px.resize((size_t)RW * RH);
        Turn(j.o, src.data(), SW, SH, part->px.data(), RW, RH);
        std::lock_guard<std::mutex> l(mu_);
        if (gen_ == gen) done_ = part;
    };
    if (!RenderPage(j.page.get(), src.data(), SW, SH, (int)std::lround(sx0), (int)std::lround(sy0), FW, FH, cancel, tick))
        return;
    out->px.resize((size_t)RW * RH);
    Turn(j.o, src.data(), SW, SH, out->px.data(), RW, RH);
}

void PdfView::Run() {
    std::unique_lock<std::mutex> l(mu_);
    for (;;) {
        wake_.wait(l, [this] { return stop_ || queued_; });
        if (stop_) break;
        queued_ = false;
        const Job j = job_;
        const uint64_t gen = gen_;
        const bool both = !EqualRect(&j.visible, &j.cover);
        for (int phase = 0; phase < (both ? 2 : 1); phase++) {
            l.unlock();
            auto res = std::make_shared<Result>();
            Render(j, phase ? j.cover : j.visible, gen, phase == 0, res.get());
            l.lock();
            if (gen_ != gen || res->px.empty()) break;
            done_ = res;
            finished_.notify_all();
            if (notify_) notify_();
        }
        if (gen_ == gen) {
            active_ = false;
            finished_.notify_all();
        }
    }
}

PdfView::State PdfView::Prepare(const Image &im, const Orient &o, double scale, int W, int H, double left, double top) {
    const Grid g = MakeGrid(im, o, scale, W, H, left, top);
    if (g.visible.left >= g.visible.right || g.visible.top >= g.visible.bottom) return Sharp;
    std::unique_lock<std::mutex> l(mu_);
    if (!SamePage(askedPage_, im.pdf) || !(askedOrient_ == o)) {
        askedPage_ = im.pdf;
        askedOrient_ = o;
        askedTick_ = GetTickCount64();
    }
    auto sharp = [&] {
        return done_ && !done_->partial && SamePage(done_->page, im.pdf) && done_->o == o && done_->scale == scale &&
               Inside(g.visible, done_->r);
    };
    if (!sharp()) {
        const bool asked = active_ && SamePage(job_.page, im.pdf) && job_.o == o && job_.scale == scale &&
                           Inside(g.visible, job_.cover);
        if (!asked) {
            job_.page = im.pdf;
            job_.o = o;
            job_.scale = scale;
            job_.gridW = g.w;
            job_.gridH = g.h;
            job_.visible = g.visible;
            job_.cover = CoverOf(g.visible, g.w, g.h, W, H);
            queued_ = active_ = true;
            gen_++;
            wake_.notify_all();
            finished_.wait_for(l, std::chrono::milliseconds(kQuickMs), sharp);
        }
    }
    if (sharp()) return Sharp;
    return done_ && SamePage(done_->page, im.pdf) && done_->o == o ? Preview : Nothing;
}

void PdfView::Draw(const Image &im, const Orient &o, double scale, int W, int H, double left, double top, uint8_t *dst,
                   size_t pitch) {
    const Grid g = MakeGrid(im, o, scale, W, H, left, top);
    const RECT v = g.visible;
    if (v.left >= v.right || v.top >= v.bottom) return;
    std::shared_ptr<const Result> res;
    {
        std::lock_guard<std::mutex> l(mu_);
        res = done_;
    }
    if (res && (!SamePage(res->page, im.pdf) || !(res->o == o))) res.reset();
    const Result *R = res.get();
    const int rw = R ? R->r.right - R->r.left : 0, rh = R ? R->r.bottom - R->r.top : 0;
    const bool same = R && R->scale == scale;
    const double k = R ? R->scale / scale : 1;   // the result's pixels per screen pixel
    ParallelFor((size_t)(v.bottom - v.top), [&](size_t row) {
        const int gy = v.top + (int)row;
        uint32_t *d = (uint32_t *)(dst + (size_t)(gy + g.oy) * pitch) + (v.left + g.ox);
        std::fill(d, d + (v.right - v.left), 0xFFFFFFFFu);
        if (!R) return;
        if (same) {
            if (gy < R->r.top || gy >= R->r.bottom) return;
            const int x0 = std::max(v.left, R->r.left), x1 = std::min(v.right, R->r.right);
            if (x0 < x1)
                memcpy(d + (x0 - v.left), &R->px[(size_t)(gy - R->r.top) * rw + (x0 - R->r.left)], (size_t)(x1 - x0) * 4);
            return;
        }
        // another scale: stretched until the rendering arrives (bilinear when enlarging)
        const double fy = (gy + 0.5) * k - R->r.top - 0.5;
        if (fy < -0.5 || fy >= rh - 0.5) return;
        const int iy = (int)std::floor(fy), ay = (int)((fy - iy) * 256);
        const int ya = std::clamp(iy, 0, rh - 1), yb = std::clamp(iy + 1, 0, rh - 1);
        for (int gx = v.left; gx < v.right; gx++) {
            const double fx = (gx + 0.5) * k - R->r.left - 0.5;
            if (fx < -0.5 || fx >= rw - 0.5) continue;
            const int ix = (int)std::floor(fx), ax = (int)((fx - ix) * 256);
            const int xa = std::clamp(ix, 0, rw - 1), xb = std::clamp(ix + 1, 0, rw - 1);
            uint32_t *out = d + (gx - v.left);
            if (k >= 1) {
                *out = R->px[(size_t)(ay < 128 ? ya : yb) * rw + (ax < 128 ? xa : xb)];
                continue;
            }
            const uint8_t *p00 = (const uint8_t *)&R->px[(size_t)ya * rw + xa], *p01 = (const uint8_t *)&R->px[(size_t)ya * rw + xb];
            const uint8_t *p10 = (const uint8_t *)&R->px[(size_t)yb * rw + xa], *p11 = (const uint8_t *)&R->px[(size_t)yb * rw + xb];
            uint8_t *q = (uint8_t *)out;
            for (int c = 0; c < 3; c++) {
                int top0 = p00[c] * (256 - ax) + p01[c] * ax, bot = p10[c] * (256 - ax) + p11[c] * ax;
                q[c] = (uint8_t)((top0 * (256 - ay) + bot * ay + 32768) >> 16);
            }
            q[3] = 255;
        }
    });
}

bool PdfView::Busy() {
    std::lock_guard<std::mutex> l(mu_);
    return active_;
}

void PdfView::Wait() {
    std::unique_lock<std::mutex> l(mu_);
    finished_.wait(l, [this] { return !active_; });
}

ULONGLONG PdfView::NothingSince() {
    std::lock_guard<std::mutex> l(mu_);
    return askedTick_;
}
