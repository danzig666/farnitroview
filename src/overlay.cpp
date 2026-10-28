// FarNitroView for Far Manager: the window the image is drawn in, over Far's console window.
#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <uiautomation.h>
#include <windowsx.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

static const wchar_t kClass[] = L"NitroViewFarOverlay";
enum { WM_NV_UPDATE = WM_APP + 1 };

static bool ClassIs(HWND w, const wchar_t *name) {
    wchar_t c[64] = {};
    GetClassNameW(w, c, 64);
    return !wcscmp(c, name);
}

HWND FindConsoleHostWindow() {
    HWND con = GetConsoleWindow();
    if (con && IsWindowVisible(con) && ClassIs(con, L"ConsoleWindowClass")) return con;
    // a pseudo console (Windows Terminal ...) is owned by the terminal's window on recent Windows
    if (con) {
        HWND owner = GetAncestor(con, GA_ROOTOWNER);
        if (owner && owner != con && IsWindowVisible(owner)) return owner;
    }
    HWND fg = GetForegroundWindow();
    return fg ? GetAncestor(fg, GA_ROOT) : nullptr;
}

static void SetThreadDpiAware() {
    typedef DPI_AWARENESS_CONTEXT(WINAPI * Fn)(DPI_AWARENESS_CONTEXT);
    Fn f = (Fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    if (f && !f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
}

static UINT WindowDpi(HWND w) {
    typedef UINT(WINAPI * Fn)(HWND);
    static Fn f = (Fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT d = f ? f(w) : 0;
    return d ? d : 96;
}

// Windows Terminal: the screen rectangle of the terminal control (UI Automation)
static bool TerminalControlRect(HWND w, RECT *r) {
    IUIAutomation *ua = nullptr;
    IUIAutomationElement *root = nullptr, *el = nullptr;
    IUIAutomationCondition *cond = nullptr;
    bool ok = false;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                   (void **)&ua)) &&
        SUCCEEDED(ua->ElementFromHandle(w, &root)) && root) {
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"TermControl");
        if (SUCCEEDED(ua->CreatePropertyCondition(UIA_ClassNamePropertyId, v, &cond)) &&
            SUCCEEDED(root->FindFirst(TreeScope_Descendants, cond, &el)) && el &&
            SUCCEEDED(el->get_CurrentBoundingRectangle(r)))
            ok = r->right > r->left && r->bottom > r->top;
    }
    VariantClear(&v);
    if (el) el->Release();
    if (cond) cond->Release();
    if (root) root->Release();
    if (ua) ua->Release();
    return ok;
}

Overlay::Overlay(bool interactive) : interactive_(interactive) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (thread_) WaitForSingleObject(ready_, 5000);
}

Overlay::~Overlay() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    if (thread_) {
        WaitForSingleObject(thread_, 5000);
        CloseHandle(thread_);
    }
    if (ready_) CloseHandle(ready_);
    if (event_) CloseHandle(event_);
}

DWORD WINAPI Overlay::ThreadProc(void *p) {
    Overlay *self = (Overlay *)p;
    SetThreadDpiAware();
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // (UI Automation)
    HINSTANCE inst = (HINSTANCE)&__ImageBase;
    WNDCLASSEXW wc = {sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);   // (fails harmlessly when another overlay registered it)
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (!self->interactive_) ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
    self->conout_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    HWND w = CreateWindowExW(ex, kClass, L"FarNitroView", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, self);
    if (w && !self->interactive_) SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
    self->hwnd_ = w;
    if (w) SetTimer(w, 1, 50, nullptr);
    SetEvent(self->ready_);
    MSG m;
    while (w && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (self->dib_) DeleteObject(self->dib_);
    if (self->font_) DeleteObject(self->font_);
    if (self->conout_ != INVALID_HANDLE_VALUE) CloseHandle(self->conout_);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

LRESULT CALLBACK Overlay::WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
    Overlay *self = (Overlay *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (!self || !self->hwnd_) return DefWindowProcW(w, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

// ---------------------------------------------------------------------------

void Overlay::Place(HWND target, Placement p, RECT cells) {
    {
        std::lock_guard<std::mutex> l(mu_);
        target_ = target;
        placement_ = p;
        cells_ = cells;
    }
    if (hwnd_) SendMessageW(hwnd_, WM_NV_UPDATE, 0, 0);   // synchronously: Size() is right afterwards
}

void Overlay::Update(const OverlayState &s) {
    {
        std::lock_guard<std::mutex> l(mu_);
        state_ = s;
    }
    if (hwnd_) PostMessageW(hwnd_, WM_NV_UPDATE, 0, 0);
}

void Overlay::Update(const std::function<void(OverlayState &)> &change) {
    {
        std::lock_guard<std::mutex> l(mu_);
        change(state_);
    }
    if (hwnd_) PostMessageW(hwnd_, WM_NV_UPDATE, 0, 0);
}

void Overlay::Hide() {
    {
        std::lock_guard<std::mutex> l(mu_);
        state_.visible = false;
    }
    if (hwnd_) SendMessageW(hwnd_, WM_NV_UPDATE, 0, 0);
}

SIZE Overlay::Size() {
    std::lock_guard<std::mutex> l(mu_);
    return size_;
}

bool Overlay::PopEvent(OverlayEvent *e) {
    std::lock_guard<std::mutex> l(mu_);
    if (events_.empty()) return false;
    *e = events_.front();
    events_.pop_front();
    return true;
}

void Overlay::SetTick(std::function<void()> tick) {
    std::lock_guard<std::mutex> l(mu_);
    tick_ = std::move(tick);
}

void Overlay::Push(const OverlayEvent &e) {
    {
        std::lock_guard<std::mutex> l(mu_);
        if (e.type == OverlayEvent::Move && !events_.empty() && events_.back().type == OverlayEvent::Move)
            events_.back() = e;
        else if (events_.size() < 256)
            events_.push_back(e);
    }
    SetEvent(event_);
}

// ---------------------------------------------------------------------------

bool Overlay::ComputeRect(RECT *r) {
    HWND t;
    Placement p;
    RECT cells;
    {
        std::lock_guard<std::mutex> l(mu_);
        t = target_;
        p = placement_;
        cells = cells_;
    }
    if (p == Placement::FullScreen) {
        MONITORINFO mi = {sizeof mi};
        HMONITOR mon = MonitorFromWindow(t ? t : GetConsoleWindow(), MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(mon, &mi)) return false;
        *r = mi.rcMonitor;
        return true;
    }
    if (!t || !IsWindow(t) || IsIconic(t)) return false;
    RECT c;
    POINT o = {0, 0};
    if (!GetClientRect(t, &c) || !ClientToScreen(t, &o) || c.right <= 0 || c.bottom <= 0) return false;
    // the text area: the client area (conhost ...), Windows Terminal's terminal control (below the tabs)
    RECT area = {o.x, o.y, o.x + c.right, o.y + c.bottom};
    int pad = 0;
    if (ClassIs(t, L"CASCADIA_HOSTING_WINDOW_CLASS")) {
        if (termWnd_ != t || termClient_.cx != c.right || termClient_.cy != c.bottom || ticks_ - termTick_ > 40) {
            termWnd_ = t;
            termClient_ = {c.right, c.bottom};
            termTick_ = ticks_;
            RECT tc;
            termOk_ = TerminalControlRect(t, &tc);
            if (termOk_) SetRect(&termOffset_, tc.left - o.x, tc.top - o.y, tc.right - o.x, tc.bottom - o.y);
        }
        if (termOk_) SetRect(&area, o.x + termOffset_.left, o.y + termOffset_.top, o.x + termOffset_.right, o.y + termOffset_.bottom);
        pad = MulDiv(8, WindowDpi(t), 96);   // Windows Terminal's default padding
    }
    if (p == Placement::Console) {
        *r = area;
        return true;
    }
    // console cells -> pixels
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (conout_ == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(conout_, &bi)) return false;
    const int cols = bi.srWindow.Right - bi.srWindow.Left + 1, rows = bi.srWindow.Bottom - bi.srWindow.Top + 1;
    if (cols <= 0 || rows <= 0) return false;
    // (the console's own font size is in unscaled units, this thread is DPI aware: the cells are
    // the text area divided by the grid, a partial cell left over at the right / bottom)
    const int aw = area.right - area.left - 2 * pad, ah = area.bottom - area.top - 2 * pad;
    double cw = std::floor((double)aw / cols), ch = std::floor((double)ah / rows);
    if (cw < 1 || ch < 1) {
        cw = (double)aw / cols;
        ch = (double)ah / rows;
    }
    const int x0 = area.left + pad, y0 = area.top + pad;
    SetRect(r, x0 + (int)std::lround(cells.left * cw), y0 + (int)std::lround(cells.top * ch),
            x0 + (int)std::lround(cells.right * cw), y0 + (int)std::lround(cells.bottom * ch));
    return r->right > r->left && r->bottom > r->top;
}

void Overlay::Reposition() {
    RECT r;
    bool want;
    HWND t;
    {
        std::lock_guard<std::mutex> l(mu_);
        want = state_.visible;
        t = target_;
    }
    if (want) want = ComputeRect(&r);
    if (want) {
        HWND fg = GetForegroundWindow();
        fg = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
        want = fg == t || fg == hwnd_ || !t;
    }
    if (!want) {
        if (shown_) {
            ShowWindow(hwnd_, SW_HIDE);
            shown_ = false;
        }
        return;
    }
    bool resized = r.right - r.left != shownRect_.right - shownRect_.left || r.bottom - r.top != shownRect_.bottom - shownRect_.top;
    if (!shown_ || !EqualRect(&r, &shownRect_)) {
        SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | (shown_ ? 0 : SWP_SHOWWINDOW));
        shownRect_ = r;
        shown_ = true;
    }
    if (resized) {
        {
            std::lock_guard<std::mutex> l(mu_);
            size_.cx = r.right - r.left;
            size_.cy = r.bottom - r.top;
        }
        OverlayEvent e = {OverlayEvent::Resize};
        Push(e);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// darkens a band of the buffer for text on it
static void Shade(uint8_t *bits, int W, int H, RECT b) {
    b.left = std::max(0L, b.left);
    b.top = std::max(0L, b.top);
    b.right = std::min((LONG)W, b.right);
    b.bottom = std::min((LONG)H, b.bottom);
    for (int y = b.top; y < b.bottom; y++) {
        uint8_t *p = bits + ((size_t)y * W + b.left) * 4;
        for (int x = b.left; x < b.right; x++, p += 4) {
            p[0] = (uint8_t)(p[0] * 3 / 10);
            p[1] = (uint8_t)(p[1] * 3 / 10);
            p[2] = (uint8_t)(p[2] * 3 / 10);
        }
    }
}

void Overlay::Paint(HDC dc) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    const int W = cr.right, H = cr.bottom;
    if (W <= 0 || H <= 0) return;
    if (!dib_ || dibW_ != W || dibH_ != H) {
        if (dib_) DeleteObject(dib_);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        dib_ = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        dibW_ = W;
        dibH_ = H;
        if (!dib_) return;
    }
    OverlayState s;
    {
        std::lock_guard<std::mutex> l(mu_);
        s = state_;
    }
    if (s.autoFit && s.image) {
        int ow, oh;
        OrientedSize(*s.image, s.view.orient, &ow, &oh);
        double sc = std::min((double)W / ow, (double)H / oh);
        if (!s.enlarge) sc = std::min(sc, 1.0);
        s.view.scale = sc;
        s.view.cx = ow / 2.0;
        s.view.cy = oh / 2.0;
    }
    GdiFlush();
    renderer_.Draw(s.image, s.view, s.bg, (uint8_t *)bits_, (size_t)W * 4, W, H);

    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, dib_);
    if (!s.info.empty() || !s.message.empty()) {
        UINT dpi = WindowDpi(hwnd_);
        if (!font_ || fontDpi_ != dpi) {
            if (font_) DeleteObject(font_);
            font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                CLEARTYPE_QUALITY, 0, L"Segoe UI");
            fontDpi_ = dpi;
        }
        HGDIOBJ oldFont = SelectObject(mem, font_);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(240, 240, 240));
        const int pad = MulDiv(6, dpi, 96);
        if (!s.info.empty()) {
            RECT t = {0, 0, 0, 0};
            DrawTextW(mem, s.info.c_str(), -1, &t, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            RECT band = {0, 0, std::min((LONG)W, t.right + 2 * pad), t.bottom + pad};
            GdiFlush();
            Shade((uint8_t *)bits_, W, H, band);
            RECT tr = {pad, pad / 2, W - pad, band.bottom};
            DrawTextW(mem, s.info.c_str(), -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        if (!s.message.empty()) {
            RECT t = {0, 0, W - 4 * pad, 0};
            DrawTextW(mem, s.message.c_str(), -1, &t, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            int bw = t.right + 2 * pad, bh = t.bottom + 2 * pad;
            RECT box = {(W - bw) / 2, (H - bh) / 2, (W - bw) / 2 + bw, (H - bh) / 2 + bh};
            GdiFlush();
            Shade((uint8_t *)bits_, W, H, box);
            RECT tr = {box.left + pad, box.top + pad, box.right - pad, box.bottom - pad};
            DrawTextW(mem, s.message.c_str(), -1, &tr, DT_WORDBREAK | DT_NOPREFIX | DT_CENTER);
        }
        SelectObject(mem, oldFont);
    }
    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

LRESULT Overlay::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NV_UPDATE:
        Reposition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_TIMER: {
        Reposition();
        if (++ticks_ % 5 == 0) {
            std::function<void()> t;
            {
                std::lock_guard<std::mutex> l(mu_);
                t = tick_;
            }
            if (t) t();
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        Paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return interactive_ ? HTCLIENT : HTTRANSPARENT;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        OverlayEvent e = {down ? OverlayEvent::Down : OverlayEvent::Up};
        e.button = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ? 1 : 2;
        e.x = GET_X_LPARAM(lp);
        e.y = GET_Y_LPARAM(lp);
        e.keys = wp;
        if (down) SetCapture(hwnd_);
        else if (!(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
        Push(e);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        OverlayEvent e = {OverlayEvent::DblClick};
        e.x = GET_X_LPARAM(lp);
        e.y = GET_Y_LPARAM(lp);
        e.keys = wp;
        Push(e);
        return 0;
    }
    case WM_MOUSEMOVE: {
        OverlayEvent e = {OverlayEvent::Move};
        e.x = GET_X_LPARAM(lp);
        e.y = GET_Y_LPARAM(lp);
        e.keys = wp;
        Push(e);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &p);
        OverlayEvent e = {OverlayEvent::Wheel};
        e.x = p.x;
        e.y = p.y;
        e.delta = GET_WHEEL_DELTA_WPARAM(wp);
        e.keys = GET_KEYSTATE_WPARAM(wp);
        Push(e);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
