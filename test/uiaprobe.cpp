// probe: the TermControl rectangle of a Windows Terminal window (by title)
#include <windows.h>
#include <uiautomation.h>
#include <cstdio>

int wmain(int argc, wchar_t **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HWND w = FindWindowW(L"CASCADIA_HOSTING_WINDOW_CLASS", argc > 1 ? argv[1] : L"NVTEST");
    RECT wr, cr;
    GetWindowRect(w, &wr);
    GetClientRect(w, &cr);
    POINT o = {0, 0};
    ClientToScreen(w, &o);
    printf("hwnd %p window %ld,%ld-%ld,%ld client origin %ld,%ld size %ldx%ld\n", (void *)w, wr.left, wr.top, wr.right,
           wr.bottom, o.x, o.y, cr.right, cr.bottom);
    IUIAutomation *ua = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void **)&ua);
    printf("uia %lx\n", hr);
    IUIAutomationElement *root = nullptr, *el = nullptr;
    ua->ElementFromHandle(w, &root);
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"TermControl");
    IUIAutomationCondition *c = nullptr;
    ua->CreatePropertyCondition(UIA_ClassNamePropertyId, v, &c);
    hr = root->FindFirst(TreeScope_Descendants, c, &el);
    printf("find %lx el %p\n", hr, (void *)el);
    if (el) {
        RECT r;
        el->get_CurrentBoundingRectangle(&r);
        printf("TermControl %ld,%ld-%ld,%ld (%ldx%ld)\n", r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
    }
    return 0;
}
