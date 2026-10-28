// FarNitroView for Far Manager: the window the image is drawn in, over Far's console window.
//
// A borderless, never-activated topmost popup on its own thread (its own message loop, per-monitor
// DPI aware), kept over the console's client area, the console's monitor (full screen) or a cell
// rectangle of the console (quick view). Keyboard input stays with the console; the window only
// sees the mouse (or nothing, when click-through).
#pragma once
#include "render.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>

enum class Placement { Console, FullScreen, Cells };

struct OverlayState {
    ImagePtr image;
    ViewParams view;
    bool autoFit = false;      // compute the view here: whole image, centred (quick view)
    bool enlarge = false;      // autoFit: small images too
    uint32_t bg = 0;
    std::wstring info;         // the information line (empty: none)
    std::wstring message;      // centred text (loading, error)
    bool visible = false;
};

struct OverlayEvent {
    enum Type { Down, Up, Move, Wheel, DblClick, Resize } type;
    int button = 0;   // 0 left, 1 right, 2 middle
    int x = 0, y = 0, delta = 0;
    WPARAM keys = 0;  // MK_*
};

class Overlay {
public:
    Overlay(bool interactive);
    ~Overlay();
    // where: the window to follow; cells: [left, top, right, bottom) in console cells (Placement::Cells)
    void Place(HWND target, Placement p, RECT cells = RECT{});
    void Update(const OverlayState &s);
    void Update(const std::function<void(OverlayState &)> &change);
    void Hide();
    SIZE Size();   // the drawing area, in pixels
    HANDLE EventHandle() const { return event_; }
    bool PopEvent(OverlayEvent *e);
    // called on the window's thread about 4 times a second
    void SetTick(std::function<void()> tick);

private:
    static DWORD WINAPI ThreadProc(void *self);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Reposition();
    void Paint(HDC dc);
    void Push(const OverlayEvent &e);
    bool ComputeRect(RECT *r);

    bool interactive_;
    HANDLE thread_ = nullptr, ready_ = nullptr, event_ = nullptr;
    HWND hwnd_ = nullptr;

    std::mutex mu_;
    OverlayState state_;
    HWND target_ = nullptr;
    Placement placement_ = Placement::Console;
    RECT cells_ = {};
    std::deque<OverlayEvent> events_;
    std::function<void()> tick_;
    SIZE size_ = {0, 0};

    // window thread only
    Renderer renderer_;
    HBITMAP dib_ = nullptr;
    void *bits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
    RECT shownRect_ = {};
    bool shown_ = false;
    int ticks_ = 0;
    HFONT font_ = nullptr;
    UINT fontDpi_ = 0;
    HANDLE conout_ = INVALID_HANDLE_VALUE;
    // Windows Terminal: where its terminal control is in the client area (looked up now and then)
    HWND termWnd_ = nullptr;
    SIZE termClient_ = {};
    RECT termOffset_ = {};
    int termTick_ = 0;
    bool termOk_ = false;
};

// The window Far's console is shown in: the console window itself (conhost), else the foreground
// window (Windows Terminal, ConEmu ...), else none.
HWND FindConsoleHostWindow();
