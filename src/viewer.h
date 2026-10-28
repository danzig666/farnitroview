// FarNitroView for Far Manager: the image viewer and the quick view.
#pragma once
#include <string>

// Shows path (and the other images of the active panel when it is on it, else of its folder)
// until the user leaves. intercepted: opened instead of Far's viewer (F3 leaves too).
void RunViewer(const std::wstring &path, bool intercepted);

// Quick view: an image shown over Far's quick view panel.
void QuickViewShow(const std::wstring &path, intptr_t viewerId, int left, int top, int width, int height);
void QuickViewClose(intptr_t viewerId);   // -1: whichever
void QuickViewValidate();                 // (Far's main thread) hides / shows it as Far's screen changes
void QuickViewSuspend(bool suspend);      // while the viewer or a dialog of ours is up
void ShutdownViewers();
