// FarNitroView for Far Manager: the image viewer (a modal loop on Far's console input) and the
// quick view.
#include "viewer.h"
#include "farglobals.h"
#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <shlwapi.h>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Loader: decodes on a background thread, the image shown first, then its neighbours. Keys are
// "path|page" (LoadKey).

static std::wstring LoadKey(const std::wstring &path, int page) { return path + L"|" + std::to_wstring(page); }

class Loader {
public:
    explicit Loader(HANDLE notify) : notify_(notify), worker_([this] { Run(); }) {}
    ~Loader() {
        {
            std::lock_guard<std::mutex> l(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
    // paths in priority order; decoded images not among them are dropped
    void Want(const std::vector<std::wstring> &paths) {
        std::lock_guard<std::mutex> l(mu_);
        for (auto it = done_.begin(); it != done_.end();)
            it = std::find(paths.begin(), paths.end(), it->first) == paths.end() ? done_.erase(it) : std::next(it);
        queue_.clear();
        for (auto &p : paths)
            if (!done_.count(p) && p != busy_ && std::find(queue_.begin(), queue_.end(), p) == queue_.end())
                queue_.push_back(p);
        cv_.notify_all();
    }
    bool Get(const std::wstring &path, ImagePtr *img, std::wstring *err) {
        std::lock_guard<std::mutex> l(mu_);
        auto it = done_.find(path);
        if (it == done_.end()) return false;
        *img = it->second.img;
        *err = it->second.err;
        return true;
    }
    void Forget(const std::wstring &path) {
        std::lock_guard<std::mutex> l(mu_);
        done_.erase(path);
    }
    void SetBackground(uint32_t bg) {
        std::lock_guard<std::mutex> l(mu_);
        bg_ = bg;
        done_.clear();
    }

private:
    struct Entry {
        ImagePtr img;
        std::wstring err;
    };
    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_lock<std::mutex> l(mu_);
        for (;;) {
            cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            busy_ = queue_.front();
            queue_.erase(queue_.begin());
            std::wstring key = busy_;
            uint32_t bg = bg_;
            l.unlock();
            size_t bar = key.rfind(L'|');
            Entry e;
            e.img = DecodeImage(key.substr(0, bar), bg, &e.err, _wtoi(key.c_str() + bar + 1));
            l.lock();
            busy_.clear();
            if (bg == bg_) done_[key] = e;
            SetEvent(notify_);
        }
        l.unlock();
        CoUninitialize();
    }
    HANDLE notify_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::wstring> queue_;
    std::map<std::wstring, Entry> done_;
    std::wstring busy_;
    uint32_t bg_ = Cfg.bgNormal;
    bool stop_ = false;
    std::thread worker_;
};

// ---------------------------------------------------------------------------
// the quick view

namespace {
struct QuickView {
    std::unique_ptr<Overlay> ov;
    intptr_t viewerId = -1;
    std::wstring path;
    RECT cells = {};
    RECT panelRect = {};
    bool suspended = false;
    // the decoding thread: the latest request only
    std::thread worker;
    std::mutex mu;
    std::condition_variable cv;
    std::wstring want;
    unsigned gen = 0;
    bool stop = false;
};
QuickView *QV;

void QuickViewWorker(QuickView *q) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_lock<std::mutex> l(q->mu);
    for (;;) {
        q->cv.wait(l, [q] { return q->stop || !q->want.empty(); });
        if (q->stop) break;
        std::wstring path = q->want;
        unsigned gen = q->gen;
        q->want.clear();
        l.unlock();
        std::wstring err;
        ImagePtr img = DecodeImage(path, Cfg.bgNormal, &err);
        l.lock();
        if (gen == q->gen && q->ov) {
            q->ov->Update([&](OverlayState &s) {
                s.image = img;
                s.view = ViewParams();
                if (img) s.view.orient = Orient::FromExif(img->orientation);
                s.view.smooth = Cfg.smooth;
                s.message = img ? L"" : err;
            });
        }
    }
    l.unlock();
    CoUninitialize();
}

bool GetPanel(HANDLE panel, PanelInfo *pi) {
    memset(pi, 0, sizeof *pi);
    pi->StructSize = sizeof *pi;
    return PsInfo.PanelControl(panel, FCTL_GETPANELINFO, 0, pi) != 0;
}
}   // namespace

static void QuickViewInit() {
    if (QV) return;
    QV = new QuickView;
    QV->ov.reset(new Overlay(false));
    QV->ov->SetTick([] {
        if (QV && QV->viewerId != -1) PsInfo.AdvControl(&MainGuid, ACTL_SYNCHRO, 0, (void *)(intptr_t)2);
    });
    QV->worker = std::thread(QuickViewWorker, QV);
}

void QuickViewShow(const std::wstring &path, intptr_t viewerId, int left, int top, int width, int height) {
    QuickViewInit();
    PanelInfo pp;
    GetPanel(PANEL_PASSIVE, &pp);
    QV->viewerId = viewerId;
    QV->path = path;
    QV->panelRect = pp.PanelRect;
    SetRect(&QV->cells, left, top, left + width, top + height);
    QV->ov->Place(FindConsoleHostWindow(), Placement::Cells, QV->cells);
    QV->ov->Update([&](OverlayState &s) {
        s.image.reset();
        s.autoFit = true;
        s.enlarge = Cfg.enlarge;
        s.bg = Cfg.bgNormal;
        s.info.clear();
        s.message = s.busyText = GetMsg(MLoading);
        s.visible = !QV->suspended;
    });
    {
        std::lock_guard<std::mutex> l(QV->mu);
        QV->want = path;
        QV->gen++;
    }
    QV->cv.notify_all();
}

void QuickViewClose(intptr_t viewerId) {
    if (!QV || (viewerId != -1 && viewerId != QV->viewerId)) return;
    QV->viewerId = -1;
    {
        std::lock_guard<std::mutex> l(QV->mu);
        QV->gen++;
        QV->want.clear();
    }
    QV->ov->Update([](OverlayState &s) {
        s.visible = false;
        s.image.reset();
    });
}

void QuickViewValidate() {
    if (!QV || QV->viewerId == -1) return;
    WindowType wt = {sizeof wt};
    bool ok = PsInfo.AdvControl(&MainGuid, ACTL_GETWINDOWTYPE, 0, &wt) && wt.Type == WTYPE_PANELS;
    PanelInfo pp;
    if (ok) ok = GetPanel(PANEL_PASSIVE, &pp) && pp.PanelType == PTYPE_QVIEWPANEL && (pp.Flags & PFLAGS_VISIBLE);
    if (ok && !EqualRect(&pp.PanelRect, &QV->panelRect)) ok = false;   // moved: a new VE_READ follows
    ok = ok && !QV->suspended;
    QV->ov->Update([ok](OverlayState &s) { s.visible = ok; });
}

void QuickViewSuspend(bool suspend) {
    if (!QV) return;
    QV->suspended = suspend;
    if (suspend) QV->ov->Hide();
}

void ShutdownViewers() {
    if (!QV) return;
    {
        std::lock_guard<std::mutex> l(QV->mu);
        QV->stop = true;
    }
    QV->cv.notify_all();
    QV->worker.join();
    QV->ov.reset();
    delete QV;
    QV = nullptr;
}

// ---------------------------------------------------------------------------
// the viewer

namespace {

struct Item {
    std::wstring path, name;
    intptr_t panelIndex = -1;
    bool sel = false, sel0 = false;
};

enum class Fit { None, Both, Width, Height };

bool SamePath(const std::wstring &a, const std::wstring &b) { return !_wcsicmp(a.c_str(), b.c_str()); }

std::wstring FullPath(const std::wstring &p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring r(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, &r[0], nullptr);
    r.resize(n);
    return r;
}

std::wstring NameOf(const std::wstring &p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

bool ListPanel(const std::wstring &path, std::vector<Item> *items, int *cur, PanelInfo *pi) {
    if (!GetPanel(PANEL_ACTIVE, pi) || pi->PanelType != PTYPE_FILEPANEL || !(pi->Flags & PFLAGS_REALNAMES))
        return false;
    std::wstring dir;
    size_t ds = (size_t)PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);
    if (ds) {
        std::vector<char> b(ds);
        FarPanelDirectory *fd = (FarPanelDirectory *)b.data();
        fd->StructSize = sizeof *fd;
        if (PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, ds, fd) && fd->Name) dir = fd->Name;
    }
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    std::vector<char> buf;
    *cur = -1;
    for (size_t i = 0; i < pi->ItemsNumber; i++) {
        size_t sz = (size_t)PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELITEM, (intptr_t)i, nullptr);
        if (!sz) continue;
        if (buf.size() < sz) buf.resize(sz);
        FarGetPluginPanelItem g = {sizeof g, sz, (PluginPanelItem *)buf.data()};
        if (!PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELITEM, (intptr_t)i, &g)) continue;
        const PluginPanelItem *it = g.Item;
        if (!it->FileName || (it->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsImageName(it->FileName)) continue;
        Item e;
        std::wstring n = it->FileName;
        e.path = n.find_first_of(L"\\:") != std::wstring::npos ? n : dir + n;
        e.name = NameOf(n);
        e.panelIndex = (intptr_t)i;
        e.sel = e.sel0 = (it->Flags & PPIF_SELECTED) != 0;
        if (*cur < 0 && SamePath(FullPath(e.path), path)) *cur = (int)items->size();
        items->push_back(e);
    }
    if (*cur < 0) {
        items->clear();
        return false;
    }
    return true;
}

void ListFolder(const std::wstring &path, std::vector<Item> *items, int *cur) {
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/") + 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !IsImageName(fd.cFileName)) continue;
            Item e;
            e.path = dir + fd.cFileName;
            e.name = fd.cFileName;
            items->push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(items->begin(), items->end(),
              [](const Item &a, const Item &b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; });
    *cur = -1;
    for (size_t i = 0; i < items->size(); i++)
        if (SamePath((*items)[i].path, path)) *cur = (int)i;
    if (*cur < 0) {   // (not an image by its name: shown alone)
        items->clear();
        Item e;
        e.path = path;
        e.name = NameOf(path);
        items->push_back(e);
        *cur = 0;
    }
}

class Viewer {
public:
    Viewer(const std::wstring &path, bool intercepted);
    ~Viewer();
    void Run();

private:
    // state
    std::vector<Item> items_;
    int cur_ = 0, prev_ = -1;
    bool panel_ = false, intercepted_;
    PanelInfo panelInfo_ = {};
    HANDLE notify_, in_;
    Overlay ov_{true};
    Loader loader_;
    HWND host_;
    bool fullscreen_;
    ImagePtr img_, raw_;        // shown (post-processed), as decoded
    int rawItem_ = -1;          // which of items_ raw_ is
    std::wstring err_;
    int page_ = 0;              // of a multi-page file
    bool pagesAsImages_ = false;// P: paging goes through the pages too
    enum { LandNone, LandTop, LandBottom } pageLand_ = LandNone;   // a PDF's next page: where to show it from
    int frame_ = 0;             // of an animation
    ULONGLONG nextFrame_ = 0;
    bool paused_ = false;
    int pp_ = 0;                // post-processing (PP_*)
    std::wstring lastTitle_;
    bool loaded_ = false, keepNext_ = false, quit_ = false;
    Orient orient_;
    double scale_ = 1, cx_ = 0, cy_ = 0;
    Fit fit_ = Fit::Both;
    bool showInfo_;
    std::wstring title0_;
    bool resizeSeen_ = false;
    INPUT_RECORD resizeRec_ = {};
    // mouse
    bool lDown_ = false, dragging_ = false, wheelZoom_ = false;
    POINT dragFrom_ = {};
    double dragCx_ = 0, dragCy_ = 0;

    void Go(int index, bool keep);
    void GoPage(int page, bool keep, bool fromBottom = false);
    bool PdfWheel(int dir);
    bool PdfPaged();
    void Zoom100(double ax, double ay);
    void Animate();
    DWORD AnimationWait();
    void SetPostProcess(int flags);
    void Step(int delta, bool keep, bool wholeFile = false);
    void CheckLoaded();
    void Refresh();
    void Prefetch();
    SIZE View() { return ov_.Size(); }
    void OrientedDims(int *w, int *h) {
        if (img_) OrientedSize(*img_, orient_, w, h);
        else *w = *h = 1;
    }
    double FitScale(Fit f);
    void ApplyFit(Fit f);
    void Clamp();
    void ZoomTo(double s, double ax, double ay);
    void ZoomStep(int dir, bool fine, bool finer, double ax, double ay);
    void Pan(double dx, double dy);
    void ToggleFit100(double ax, double ay);
    void SetFullscreen(bool on);
    void HandleKey(const KEY_EVENT_RECORD &k);
    void HandleMouse(const OverlayEvent &e);
    void Dialog(int which);
    std::wstring Title();
};

Viewer::Viewer(const std::wstring &path, bool intercepted)
    : intercepted_(intercepted), notify_(CreateEventW(nullptr, FALSE, FALSE, nullptr)), loader_(notify_) {
    std::wstring full = FullPath(path);
    panel_ = ListPanel(full, &items_, &cur_, &panelInfo_);
    if (!panel_) ListFolder(full, &items_, &cur_);
    in_ = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (!GetConsoleMode(in_, &mode))
        in_ = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    host_ = FindConsoleHostWindow();
    fullscreen_ = Cfg.fullscreen;
    showInfo_ = Cfg.showInfo;
    pp_ = (Cfg.ppChroma ? PP_CHROMA : 0) | (Cfg.ppAutoluma ? PP_AUTOLUMA : 0) | (Cfg.ppAutolevels ? PP_AUTOLEVELS : 0);
    wchar_t t[1024] = {};
    GetConsoleTitleW(t, 1024);
    title0_ = t;
    ov_.Place(host_, fullscreen_ ? Placement::FullScreen : Placement::Console);
}

Viewer::~Viewer() {
    ov_.Hide();
    SetConsoleTitleW(title0_.c_str());
    if (panel_) {
        bool changed = false;
        for (auto &e : items_) changed |= e.sel != e.sel0;
        if (changed) {
            PsInfo.PanelControl(PANEL_ACTIVE, FCTL_BEGINSELECTION, 0, nullptr);
            for (auto &e : items_)
                if (e.sel != e.sel0) PsInfo.PanelControl(PANEL_ACTIVE, FCTL_SETSELECTION, e.panelIndex, (void *)(intptr_t)e.sel);
            PsInfo.PanelControl(PANEL_ACTIVE, FCTL_ENDSELECTION, 0, nullptr);
        }
        PanelRedrawInfo ri = {sizeof ri, (size_t)items_[cur_].panelIndex, panelInfo_.TopPanelItem};
        PsInfo.PanelControl(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, &ri);
    }
    if (resizeSeen_) {   // Far did not see the console change size: hand it on
        DWORD n;
        WriteConsoleInputW(in_, &resizeRec_, 1, &n);
    }
    CloseHandle(notify_);
}

// the title (and the information line) from the title template: {name} {path} {dir} {index}
// {count} {width} {height} {zoom} {decoder} {ms} {size} {page} {pages} {pageinfo} {pp} {selected}
std::wstring Viewer::Title() {
    const Item &it = items_[cur_];
    auto num = [](double v, const wchar_t *fmt) {
        wchar_t b[64];
        swprintf(b, 64, fmt, v);
        return std::wstring(b);
    };
    auto value = [&](const std::wstring &k) -> std::wstring {
        if (k == L"name") return it.name;
        if (k == L"path") return it.path;
        if (k == L"dir") return it.path.substr(0, it.path.find_last_of(L"\\/") + 1);
        if (k == L"index") return std::to_wstring(cur_ + 1);
        if (k == L"count") return std::to_wstring(items_.size());
        if (k == L"selected") return it.sel ? L"  [" + std::wstring(GetMsg(MSelected)) + L"]" : L"";
        if (!img_) return L"";
        if (k == L"width") return std::to_wstring(img_->w);
        if (k == L"height") return std::to_wstring(img_->h);
        if (k == L"zoom") return num(scale_ * 100, scale_ < 0.1 ? L"%.1f%%" : L"%.0f%%");
        if (k == L"decoder") return img_->decoder;
        if (k == L"ms") return num(img_->ms, L"%.0f");
        if (k == L"size")
            return img_->fileSize >= 1 << 20 ? num(img_->fileSize / 1048576.0, L"%.1f MB")
                                             : num(img_->fileSize / 1024.0, L"%.0f KB");
        if (k == L"page") return std::to_wstring(page_ + 1);
        if (k == L"pages") return std::to_wstring(img_->pages);
        if (k == L"pageinfo") {
            std::wstring r;
            if (img_->pages > 1)
                r = L"  " + std::wstring(GetMsg(MPage)) + L" " + std::to_wstring(page_ + 1) + L"/" +
                    std::to_wstring(img_->pages);
            if (img_->FrameCount() > 1)
                r += L"  " + std::to_wstring(img_->FrameCount()) + L" " + GetMsg(MFrames) + (paused_ ? L" (II)" : L"");
            if (pagesAsImages_) r += L"  [P]";
            return r;
        }
        if (k == L"pp") {
            std::wstring r;
            if (pp_ & PP_AUTOLEVELS) r += L" levels";
            if (pp_ & PP_AUTOLUMA) r += L" luma";
            if (pp_ & PP_CHROMA) r += L" chroma";
            return r.empty() ? r : L"  [" + r.substr(1) + L"]";
        }
        return L"{" + k + L"}";
    };
    std::wstring t;
    const std::wstring &tpl = Cfg.titleTemplate;
    for (size_t i = 0; i < tpl.size(); i++) {
        size_t e;
        if (tpl[i] == L'{' && (e = tpl.find(L'}', i)) != std::wstring::npos) {
            t += value(tpl.substr(i + 1, e - i - 1));
            i = e;
        } else
            t += tpl[i];
    }
    return t;
}

void Viewer::Refresh() {
    std::wstring title = Title();
    if (title != lastTitle_) SetConsoleTitleW(title.c_str());
    lastTitle_ = title;
    OverlayState s;
    s.image = img_;
    s.view.orient = orient_;
    s.view.scale = scale_;
    s.view.cx = cx_;
    s.view.cy = cy_;
    s.view.smooth = Cfg.smooth;
    s.view.frame = frame_;
    s.bg = items_[cur_].sel ? Cfg.bgSelected : Cfg.bgNormal;
    s.busyText = GetMsg(MLoading);
    if (showInfo_) s.info = title;
    if (!loaded_ && !img_) s.message = GetMsg(MLoading);
    else if (loaded_ && !img_) s.message = items_[cur_].name + L"\n" + GetMsg(MCannotDecode) + L"\n" + err_;
    s.visible = true;
    ov_.Update(s);
}

void Viewer::Prefetch() {
    std::vector<std::wstring> want;
    const int n = (int)items_.size();
    auto add = [&](int i, int page) {
        if (Cfg.loop) i = (i % n + n) % n;
        if (i < 0 || i >= n) return;
        std::wstring k = LoadKey(items_[i].path, page);
        if (std::find(want.begin(), want.end(), k) == want.end()) want.push_back(k);
    };
    add(cur_, page_);
    if (raw_ && page_ + 1 < raw_->pages) add(cur_, page_ + 1);
    add(cur_ + 1, 0);
    add(cur_ - 1, 0);
    add(cur_ + 2, 0);
    if (prev_ >= 0) add(prev_, 0);
    loader_.Want(want);
}

void Viewer::Go(int index, bool keep) {
    if (index < 0 || index >= (int)items_.size()) return;
    if (index != cur_) prev_ = cur_;
    cur_ = index;
    page_ = 0;
    loaded_ = false;
    keepNext_ = keep;
    Prefetch();
    CheckLoaded();
    if (!loaded_) Refresh();   // (the previous image stays until this one is there; the title says which)
}

void Viewer::GoPage(int page, bool keep, bool fromBottom) {
    if (!raw_ || raw_->pages <= 1) return;
    page = std::clamp(page, 0, raw_->pages - 1);
    if (page == page_) return;
    page_ = page;
    loaded_ = false;
    keepNext_ = keep;
    if (raw_->pdf) {   // reading on: the zoom and the rotation stay, the next page from its top
        keepNext_ = true;
        pageLand_ = fromBottom ? LandBottom : LandTop;
    }
    Prefetch();
    CheckLoaded();
    if (!loaded_) Refresh();
}

// The wheel over a PDF: scrolls the page and at its edge goes on to the next (dir 1) or previous
// (-1) page. False: there is nothing more of this file that way.
bool Viewer::PdfWheel(int dir) {
    if (!img_ || !img_->pdf) return false;
    SIZE v = View();
    int ow, oh;
    OrientedDims(&ow, &oh);
    const double hh = v.cy / (2 * scale_);
    const bool atEdge = oh <= 2 * hh || (dir > 0 ? cy_ >= oh - hh - 1e-6 : cy_ <= hh + 1e-6);
    if (!atEdge) {
        Pan(0, dir * std::max(48.0, v.cy / 6.0));
        return true;
    }
    if (img_->pages <= 1) return false;
    if (page_ + dir >= 0 && page_ + dir < img_->pages) GoPage(page_ + dir, true, dir < 0);
    return true;
}

// the file shown has several PDF pages: paging goes through them, not to another file
bool Viewer::PdfPaged() { return raw_ && raw_->pdf && raw_->pages > 1 && rawItem_ == cur_; }

// * and 9: a PDF's 100% (96 dpi) is of no use, it is fitted into the window instead
void Viewer::Zoom100(double ax, double ay) {
    if (!img_) return;
    if (img_->pdf) {
        ApplyFit(Fit::Both);
        Refresh();
    } else
        ZoomTo(1, ax, ay);
}

void Viewer::Step(int delta, bool keep, bool wholeFile) {
    // P: the pages of a multi-page file first, as if they were files of their own
    if (!wholeFile && pagesAsImages_ && loaded_ && raw_ && raw_->pages > 1 && page_ + delta >= 0 && page_ + delta < raw_->pages) {
        GoPage(page_ + delta, keep);
        return;
    }
    const int n = (int)items_.size();
    int i = cur_ + delta;
    if (Cfg.loop) i = (i % n + n) % n;
    else i = std::clamp(i, 0, n - 1);
    if (i != cur_) Go(i, keep);
}

void Viewer::SetPostProcess(int flags) {
    pp_ = flags;
    img_ = pp_ ? PostProcess(raw_, pp_, Cfg.chroma) : raw_;
    Refresh();
}

// the time until the next animation frame (INFINITE: none)
DWORD Viewer::AnimationWait() {
    if (!img_ || img_->FrameCount() < 2 || paused_) return INFINITE;
    ULONGLONG now = GetTickCount64();
    return now >= nextFrame_ ? 0 : (DWORD)(nextFrame_ - now);
}

void Viewer::Animate() {
    if (AnimationWait() != 0) return;
    const int n = img_->FrameCount();
    frame_ = (frame_ + 1) % n;
    ULONGLONG now = GetTickCount64();
    nextFrame_ += (ULONGLONG)img_->delays[frame_];
    if (nextFrame_ < now) nextFrame_ = now + img_->delays[frame_];   // (fell behind: no catching up)
    Refresh();
}

void Viewer::CheckLoaded() {
    if (loaded_) return;
    ImagePtr img;
    std::wstring err;
    if (!loader_.Get(LoadKey(items_[cur_].path, page_), &img, &err)) return;
    loaded_ = true;
    const bool keep = (keepNext_ || Cfg.keepPosition) && img_ && img;
    const auto land = pageLand_;
    keepNext_ = false;
    pageLand_ = LandNone;
    raw_ = img;
    rawItem_ = cur_;
    img_ = pp_ && img ? PostProcess(img, pp_, Cfg.chroma) : img;
    err_ = err;
    frame_ = 0;
    if (img_ && img_->FrameCount() > 1) nextFrame_ = GetTickCount64() + img_->delays[0];
    if (!img_) {
        Refresh();
        return;
    }
    const bool nextPage = land != LandNone && img_->pdf;
    if (!nextPage) orient_ = Orient::FromExif(img_->orientation);
    if (keep) {
        if (fit_ != Fit::None) ApplyFit(fit_);
        else Clamp();
        if (nextPage) {
            int ow, oh;
            OrientedDims(&ow, &oh);
            cy_ = land == LandTop ? 0 : oh;
            Clamp();
        }
    } else if (img_->pdf && Cfg.pdfFitWidth) {
        ApplyFit(Fit::Width);
    } else if (Cfg.autosize) {
        ApplyFit(Fit::Both);
    } else {
        int ow, oh;
        OrientedDims(&ow, &oh);
        fit_ = Fit::None;
        scale_ = 1;
        cx_ = ow / 2.0;
        cy_ = oh / 2.0;
        Clamp();
    }
    Refresh();
}

double Viewer::FitScale(Fit f) {
    SIZE v = View();
    int ow, oh;
    OrientedDims(&ow, &oh);
    double sw = (double)std::max(1L, v.cx) / ow, sh = (double)std::max(1L, v.cy) / oh;
    switch (f) {
    case Fit::Width: return sw;
    case Fit::Height: return sh;
    default: {
        double s = std::min(sw, sh);
        return Cfg.enlarge || (img_ && img_->pdf) ? s : std::min(s, 1.0);
    }
    }
}

void Viewer::ApplyFit(Fit f) {
    int ow, oh;
    OrientedDims(&ow, &oh);
    fit_ = f;
    scale_ = FitScale(f);
    cx_ = ow / 2.0;
    cy_ = oh / 2.0;
    if (f == Fit::Width) cy_ = 0;   // the top of a tall image
    if (f == Fit::Height) cx_ = 0;  // the left of a wide one
    Clamp();
}

void Viewer::Clamp() {
    SIZE v = View();
    int ow, oh;
    OrientedDims(&ow, &oh);
    const double hw = v.cx / (2 * scale_), hh = v.cy / (2 * scale_);
    cx_ = ow <= 2 * hw ? ow / 2.0 : std::clamp(cx_, hw, ow - hw);
    cy_ = oh <= 2 * hh ? oh / 2.0 : std::clamp(cy_, hh, oh - hh);
}

void Viewer::ZoomTo(double s, double ax, double ay) {
    if (!img_) return;
    const double lo = std::min(FitScale(Fit::Both), 1.0) / 8, hi = 64;
    s = std::clamp(s, lo, hi);
    SIZE v = View();
    const double px = cx_ + (ax - v.cx / 2.0) / scale_, py = cy_ + (ay - v.cy / 2.0) / scale_;
    scale_ = s;
    cx_ = px - (ax - v.cx / 2.0) / s;
    cy_ = py - (ay - v.cy / 2.0) / s;
    fit_ = Fit::None;
    Clamp();
    Refresh();
}

void Viewer::ZoomStep(int dir, bool fine, bool finer, double ax, double ay) {
    if (!img_) return;
    if (fine) {
        ZoomTo(scale_ * std::pow(finer ? 1.02 : 1.1, dir), ax, ay);
        return;
    }
    double next = scale_ * std::pow(std::sqrt(2.0), dir);
    // stop exactly at 100% and at fit on the way
    for (double stop : {1.0, FitScale(Fit::Both)}) {
        if (dir > 0 && stop > scale_ * 1.0001 && stop < next) next = stop;
        if (dir < 0 && stop < scale_ * 0.9999 && stop > next) next = stop;
    }
    ZoomTo(next, ax, ay);
}

void Viewer::Pan(double dx, double dy) {
    if (!img_) return;
    cx_ += dx / scale_;
    cy_ += dy / scale_;
    Clamp();
    Refresh();
}

void Viewer::ToggleFit100(double ax, double ay) {
    if (!img_) return;
    if (img_->pdf) {   // (no 100% here, see Zoom100: the whole page, or as wide as the window)
        ApplyFit(fit_ == Fit::Both || std::fabs(scale_ - FitScale(Fit::Both)) < 1e-6 ? Fit::Width : Fit::Both);
        Refresh();
        return;
    }
    if (fit_ == Fit::Both || std::fabs(scale_ - FitScale(Fit::Both)) < 1e-6) {
        if (std::fabs(scale_ - 1) < 1e-6) return;
        ZoomTo(1, ax, ay);
    } else {
        ApplyFit(Fit::Both);
        Refresh();
    }
}

void Viewer::SetFullscreen(bool on) {
    fullscreen_ = on;
    ov_.Place(host_, on ? Placement::FullScreen : Placement::Console);
    if (fit_ != Fit::None) ApplyFit(fit_);
    else Clamp();
    Refresh();
}

void Viewer::Dialog(int which) {
    ov_.Hide();
    if (which == 1) PsInfo.ShowHelp(PsInfo.ModuleName, L"Keys", FHELP_SELFHELP);
    else {
        uint32_t bg = Cfg.bgNormal;
        if (Cfg.Dialog()) {
            showInfo_ = Cfg.showInfo;
            if (pp_) img_ = PostProcess(raw_, pp_, Cfg.chroma);   // (the chroma multiplier may have changed)
            if (bg != Cfg.bgNormal) {
                loader_.SetBackground(Cfg.bgNormal);
                loaded_ = false;
                keepNext_ = true;
                Prefetch();
            }
        }
    }
    host_ = FindConsoleHostWindow();
    ov_.Place(host_, fullscreen_ ? Placement::FullScreen : Placement::Console);
    Refresh();
}

static bool CopyToClipboard(const std::wstring &s) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
    return true;
}

void Viewer::HandleKey(const KEY_EVENT_RECORD &k) {
    if (!k.bKeyDown) return;
    const DWORD st = k.dwControlKeyState;
    const bool ctrl = st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED), alt = st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED),
               shift = st & SHIFT_PRESSED;
    const WORD vk = k.wVirtualKeyCode;
    const wchar_t ch = k.uChar.UnicodeChar;
    SIZE v = View();
    const double mx = v.cx / 2.0, my = v.cy / 2.0;
    Item &it = items_[cur_];

    // moving to another image: Shift inverts the selection of the current one first (panel only),
    // Ctrl keeps the zoom and the position
    auto jump = [&](int delta) {
        // a PDF: Alt goes to another file, and a file of several pages is paged without it
        if (alt && !ctrl && !shift && (IsPdf(it.path.c_str(), nullptr, 0) || (raw_ && raw_->pdf && rawItem_ == cur_))) {
            Step(delta, false, true);
            return;
        }
        if (!alt && !ctrl && !shift && PdfPaged()) {
            GoPage(page_ + delta, false);
            return;
        }
        if (alt && !ctrl) {   // pages of a multi-page file; Alt-Shift: 20 at a time
            GoPage(page_ + delta * (shift ? 20 : 1), false);
            return;
        }
        if (alt) return;
        if (shift && panel_) it.sel = !it.sel;
        Step(delta, ctrl);
        if (shift && panel_ && loaded_) Refresh();
    };
    auto fitKey = [&]() {   // '/', '0', Tab, '*', '9' with a modifier
        if (ctrl) ApplyFit(Fit::Both);
        else if (shift) ApplyFit(Fit::Width);
        else if (alt) ApplyFit(Fit::Height);
        Refresh();
    };

    // characters first (layout independent: '*' may be Shift+8, ']' AltGr+G = Ctrl+Alt)
    const bool altGr = ctrl && alt && ch >= 32;
    if ((!ctrl && !alt) || altGr) {
        switch (ch) {
        case L'*': Zoom100(mx, my); return;
        case L'/': ToggleFit100(mx, my); return;
        case L'+': case L'=': ZoomStep(1, false, false, mx, my); return;
        case L'-': case L'_': ZoomStep(-1, false, false, mx, my); return;
        case L'[': orient_.RotateCCW(); if (fit_ != Fit::None) ApplyFit(fit_); else Clamp(); Refresh(); return;
        case L']': orient_.RotateCW(); if (fit_ != Fit::None) ApplyFit(fit_); else Clamp(); Refresh(); return;
        case L'~': case L'`':
            if (prev_ >= 0 && prev_ < (int)items_.size()) Go(prev_, true);
            return;
        }
    }

    switch (vk) {
    case VK_ESCAPE: case VK_F10: quit_ = true; return;
    case VK_RETURN:
        if (alt) SetFullscreen(!fullscreen_);
        else quit_ = true;
        return;
    case VK_F4:
        if (alt) { quit_ = true; return; }
        [[fallthrough]];
    case VK_F3: {
        if (vk == VK_F3 && intercepted_ && !ctrl && !alt && !shift) { quit_ = true; return; }
        bool cw = (vk == VK_F4) != shift;
        if (cw) orient_.RotateCW();
        else orient_.RotateCCW();
        if (fit_ != Fit::None) ApplyFit(fit_);
        else Clamp();
        Refresh();
        return;
    }
    case VK_F9: orient_.FlipH(); Refresh(); return;
    case VK_F11: orient_.FlipV(); Refresh(); return;
    case VK_F12:   // no post-processing, rotations, mirrors
        if (pp_) SetPostProcess(0);
        if (img_) orient_ = Orient::FromExif(img_->orientation);
        if (fit_ != Fit::None) ApplyFit(fit_);
        else Clamp();
        Refresh();
        return;
    case VK_F5:
        loader_.Forget(LoadKey(it.path, page_));
        loaded_ = false;
        keepNext_ = true;
        Prefetch();
        return;
    case VK_F1: Dialog(1); return;
    case VK_F2: Dialog(2); return;

    case VK_SPACE: case VK_NEXT: case 'N': case '2':
        jump(1);
        return;
    case VK_BACK: case VK_PRIOR: case 'B': case '1':
        jump(-1);
        return;
    case 'P':
        pagesAsImages_ = !pagesAsImages_;
        Refresh();
        return;
    case VK_PAUSE:
        paused_ = !paused_;
        if (!paused_) nextFrame_ = GetTickCount64();
        Refresh();
        return;
    case VK_F6: SetPostProcess(pp_ ^ PP_CHROMA); return;
    case VK_F7: SetPostProcess(pp_ ^ PP_AUTOLUMA); return;
    case VK_F8: SetPostProcess(pp_ ^ PP_AUTOLEVELS); return;
    case VK_HOME: case 'R': Go(0, ctrl); return;
    case VK_END: case 'T': Go((int)items_.size() - 1, ctrl); return;

    case 'F': case VK_CLEAR: SetFullscreen(!fullscreen_); return;
    case VK_NUMPAD5: SetFullscreen(!fullscreen_); return;
    case 'I': showInfo_ = !showInfo_; Refresh(); return;

    case VK_MULTIPLY: case '9':
        if (ctrl || shift || alt) fitKey();
        else Zoom100(mx, my);
        return;
    case VK_DIVIDE: case VK_OEM_2: case '0': case VK_TAB:
        if (ctrl || shift || alt) fitKey();
        else ToggleFit100(mx, my);
        return;
    case VK_ADD: case VK_OEM_PLUS: case 'E': ZoomStep(1, alt, alt && ctrl, mx, my); return;
    case VK_SUBTRACT: case VK_OEM_MINUS: case 'Q': ZoomStep(-1, alt, alt && ctrl, mx, my); return;

    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case 'A': case 'D': case 'W': case 'S': {
        // a quarter of the view; Alt: smooth steps, slower with Ctrl
        double step = alt ? (ctrl ? 4 : 24) : (ctrl ? 8 : std::max(32.0, std::min(v.cx, v.cy) / 4.0));
        double dx = vk == VK_LEFT || vk == 'A' ? -step : vk == VK_RIGHT || vk == 'D' ? step : 0;
        double dy = vk == VK_UP || vk == 'W' ? -step : vk == VK_DOWN || vk == 'S' ? step : 0;
        Pan(dx, dy);
        return;
    }
    case VK_INSERT:
        if (ctrl) { CopyToClipboard(L"FarNitroView: " + Title()); return; }
        if (panel_) { it.sel = true; Refresh(); }
        return;
    case VK_DELETE:
        if (panel_) { it.sel = false; Refresh(); }
        return;
    }
}

void Viewer::HandleMouse(const OverlayEvent &e) {
    switch (e.type) {
    case OverlayEvent::Resize:
        if (fit_ != Fit::None) ApplyFit(fit_);
        else Clamp();
        Refresh();
        return;
    case OverlayEvent::Down:
        if (e.button == 0) {
            lDown_ = true;
            dragging_ = false;
            wheelZoom_ = false;
            dragFrom_ = {e.x, e.y};
            dragCx_ = cx_;
            dragCy_ = cy_;
        }
        return;
    case OverlayEvent::Up:
        if (e.button == 0) lDown_ = dragging_ = false;
        else if (e.button == 1) quit_ = true;
        else if (e.button == 2) SetFullscreen(!fullscreen_);
        return;
    case OverlayEvent::Move:
        if (lDown_ && (e.keys & MK_LBUTTON) && img_) {
            int dx = e.x - dragFrom_.x, dy = e.y - dragFrom_.y;
            if (!dragging_ && std::abs(dx) + std::abs(dy) < 3) return;
            dragging_ = true;
            cx_ = dragCx_ - dx / scale_;
            cy_ = dragCy_ - dy / scale_;
            Clamp();
            Refresh();
        } else if (!(e.keys & MK_LBUTTON)) {
            lDown_ = dragging_ = false;
        }
        return;
    case OverlayEvent::DblClick: {
        if (!img_) return;
        SIZE v = View();
        int ow, oh;
        OrientedDims(&ow, &oh);
        double left = v.cx / 2.0 - cx_ * scale_, top = v.cy / 2.0 - cy_ * scale_;
        bool onImage = e.x >= left && e.x < left + ow * scale_ && e.y >= top && e.y < top + oh * scale_;
        if (onImage) ToggleFit100(e.x, e.y);
        else {   // the background: fill the side that has bars
            ApplyFit((double)ow / oh < (double)v.cx / v.cy ? Fit::Width : Fit::Height);
            Refresh();
        }
        return;
    }
    case OverlayEvent::Wheel: {
        int dir = e.delta > 0 ? 1 : -1;
        if ((e.keys & MK_LBUTTON) || (e.keys & MK_CONTROL)) {
            wheelZoom_ = true;
            ZoomStep(dir, false, false, e.x, e.y);
        } else {
            if (!(e.keys & MK_SHIFT) && PdfWheel(-dir)) return;
            if ((e.keys & MK_SHIFT) && panel_) items_[cur_].sel = !items_[cur_].sel;
            Step(-dir, false);
            Refresh();
        }
        return;
    }
    }
}

void Viewer::Run() {
    Prefetch();
    Refresh();
    HANDLE h[3] = {in_, ov_.EventHandle(), notify_};
    INPUT_RECORD recs[64];
    while (!quit_) {
        CheckLoaded();
        Animate();
        WaitForMultipleObjects(3, h, FALSE, std::min<DWORD>(250, AnimationWait()));
        DWORD n = 0;
        if (GetNumberOfConsoleInputEvents(in_, &n) && n) {
            if (!ReadConsoleInputW(in_, recs, 64, &n)) n = 0;
            for (DWORD i = 0; i < n && !quit_; i++) {
                const INPUT_RECORD &r = recs[i];
                if (r.EventType == KEY_EVENT) HandleKey(r.Event.KeyEvent);
                else if (r.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                    resizeSeen_ = true;
                    resizeRec_ = r;
                } else if (r.EventType == MOUSE_EVENT && r.Event.MouseEvent.dwEventFlags == MOUSE_WHEELED) {
                    SIZE v = View();
                    OverlayEvent e = {OverlayEvent::Wheel};
                    e.delta = (short)HIWORD(r.Event.MouseEvent.dwButtonState);
                    e.x = v.cx / 2;
                    e.y = v.cy / 2;
                    DWORD ks = r.Event.MouseEvent.dwControlKeyState;
                    if (ks & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) e.keys |= MK_CONTROL;
                    if (ks & SHIFT_PRESSED) e.keys |= MK_SHIFT;
                    HandleMouse(e);
                }
            }
        }
        OverlayEvent e;
        while (!quit_ && ov_.PopEvent(&e)) HandleMouse(e);
    }
}

}   // namespace

void RunViewer(const std::wstring &path, bool intercepted) {
    QuickViewSuspend(true);
    {
        Viewer v(path, intercepted);
        v.Run();
    }
    QuickViewSuspend(false);
}
