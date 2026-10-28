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
// Loader: decodes on a background thread, the image shown first, then its neighbours

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
            std::wstring path = busy_;
            uint32_t bg = bg_;
            l.unlock();
            Entry e;
            e.img = DecodeImage(path, bg, &e.err);
            l.lock();
            busy_.clear();
            if (bg == bg_) done_[path] = e;
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
        s.message = GetMsg(MLoading);
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
    ImagePtr img_;
    std::wstring err_;
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
    void Step(int delta, bool keep);
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

std::wstring Viewer::Title() {
    const Item &it = items_[cur_];
    wchar_t b[512];
    std::wstring t;
    swprintf(b, 512, L"[%d/%d]  ", cur_ + 1, (int)items_.size());
    t = b + it.name;
    if (img_) {
        swprintf(b, 512, L"   %d × %d   %.0f%%   %ls %.0f ms", img_->w, img_->h, scale_ * 100,
                 img_->decoder.c_str(), img_->ms);
        t += b;
    }
    if (it.sel) t += L"   [" + std::wstring(GetMsg(MSelected)) + L"]";
    return t;
}

void Viewer::Refresh() {
    SetConsoleTitleW((L"FarNitroView: " + Title()).c_str());
    OverlayState s;
    s.image = img_;
    s.view.orient = orient_;
    s.view.scale = scale_;
    s.view.cx = cx_;
    s.view.cy = cy_;
    s.view.smooth = Cfg.smooth;
    s.bg = items_[cur_].sel ? Cfg.bgSelected : Cfg.bgNormal;
    if (showInfo_) s.info = Title();
    if (!loaded_ && !img_) s.message = GetMsg(MLoading);
    else if (loaded_ && !img_) s.message = items_[cur_].name + L"\n" + GetMsg(MCannotDecode) + L"\n" + err_;
    s.visible = true;
    ov_.Update(s);
}

void Viewer::Prefetch() {
    std::vector<std::wstring> want;
    const int n = (int)items_.size();
    auto add = [&](int i) {
        if (Cfg.loop) i = (i % n + n) % n;
        if (i >= 0 && i < n && std::find(want.begin(), want.end(), items_[i].path) == want.end())
            want.push_back(items_[i].path);
    };
    add(cur_);
    add(cur_ + 1);
    add(cur_ - 1);
    add(cur_ + 2);
    if (prev_ >= 0) add(prev_);
    loader_.Want(want);
}

void Viewer::Go(int index, bool keep) {
    if (index < 0 || index >= (int)items_.size()) return;
    if (index != cur_) prev_ = cur_;
    cur_ = index;
    loaded_ = false;
    keepNext_ = keep;
    Prefetch();
    CheckLoaded();
    if (!loaded_) Refresh();   // (the previous image stays until this one is there; the title says which)
}

void Viewer::Step(int delta, bool keep) {
    const int n = (int)items_.size();
    int i = cur_ + delta;
    if (Cfg.loop) i = (i % n + n) % n;
    else i = std::clamp(i, 0, n - 1);
    if (i != cur_) Go(i, keep);
}

void Viewer::CheckLoaded() {
    if (loaded_) return;
    ImagePtr img;
    std::wstring err;
    if (!loader_.Get(items_[cur_].path, &img, &err)) return;
    loaded_ = true;
    const bool keep = (keepNext_ || Cfg.keepPosition) && img_ && img;
    keepNext_ = false;
    img_ = img;
    err_ = err;
    if (!img_) {
        Refresh();
        return;
    }
    orient_ = Orient::FromExif(img_->orientation);
    if (keep) {
        if (fit_ != Fit::None) ApplyFit(fit_);
        else Clamp();
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
        return Cfg.enlarge ? s : std::min(s, 1.0);
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
        case L'*': ZoomTo(1, mx, my); return;
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
    case VK_F12:
        if (img_) orient_ = Orient::FromExif(img_->orientation);
        if (fit_ != Fit::None) ApplyFit(fit_);
        else Clamp();
        Refresh();
        return;
    case VK_F5:
        loader_.Forget(it.path);
        loaded_ = false;
        keepNext_ = true;
        Prefetch();
        return;
    case VK_F1: Dialog(1); return;
    case VK_F2: Dialog(2); return;

    case VK_SPACE: case VK_NEXT: case 'N': case '2':
        if (alt) return;   // (pages of multi-page images: not supported)
        jump(1);
        return;
    case VK_BACK: case VK_PRIOR: case 'B': case '1':
        if (alt) return;
        jump(-1);
        return;
    case VK_HOME: case 'R': Go(0, ctrl); return;
    case VK_END: case 'T': Go((int)items_.size() - 1, ctrl); return;

    case 'F': case VK_CLEAR: SetFullscreen(!fullscreen_); return;
    case VK_NUMPAD5: SetFullscreen(!fullscreen_); return;
    case 'I': showInfo_ = !showInfo_; Refresh(); return;

    case VK_MULTIPLY: case '9':
        if (ctrl || shift || alt) fitKey();
        else ZoomTo(1, mx, my);
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
        WaitForMultipleObjects(3, h, FALSE, 250);
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
