// FarNitroView for Far Manager: settings and the configuration dialog.
#include "farglobals.h"

#include "DlgBuilder.hpp"
#include "PluginSettings.hpp"

#include <cwchar>

Config Cfg;

const wchar_t *GetMsg(int id) { return PsInfo.GetMsg(&MainGuid, id); }

void Config::Load() {
    PluginSettings s(MainGuid, PsInfo.SettingsControl);
    enterArchive = s.Get(0, L"EnterArchive", enterArchive);
    quickView = s.Get(0, L"QuickView", quickView);
    view = s.Get(0, L"View", view);
    fullscreen = s.Get(0, L"Fullscreen", fullscreen);
    smooth = s.Get(0, L"Smooth", smooth);
    showInfo = s.Get(0, L"ShowInfoLine", showInfo);
    autosize = s.Get(0, L"Autosize", autosize);
    enlarge = s.Get(0, L"Enlarge", enlarge);
    keepPosition = s.Get(0, L"KeepPosition", keepPosition);
    loop = s.Get(0, L"Loop", loop);
    bgNormal = (uint32_t)s.Get(0, L"BackgroundNormal", (unsigned __int64)bgNormal) & 0xFFFFFF;
    bgSelected = (uint32_t)s.Get(0, L"BackgroundSelected", (unsigned __int64)bgSelected) & 0xFFFFFF;
    prefix = s.Get(0, L"Prefix", prefix.c_str());
}

void Config::Save() const {
    PluginSettings s(MainGuid, PsInfo.SettingsControl);
    s.Set(0, L"EnterArchive", enterArchive);
    s.Set(0, L"QuickView", quickView);
    s.Set(0, L"View", view);
    s.Set(0, L"Fullscreen", fullscreen);
    s.Set(0, L"Smooth", smooth);
    s.Set(0, L"ShowInfoLine", showInfo);
    s.Set(0, L"Autosize", autosize);
    s.Set(0, L"Enlarge", enlarge);
    s.Set(0, L"KeepPosition", keepPosition);
    s.Set(0, L"Loop", loop);
    s.Set(0, L"BackgroundNormal", (unsigned __int64)bgNormal);
    s.Set(0, L"BackgroundSelected", (unsigned __int64)bgSelected);
    s.Set(0, L"Prefix", prefix.c_str());
}

static bool ParseColor(const wchar_t *s, uint32_t *c) {
    while (*s == L' ' || *s == L'#') s++;
    wchar_t *end;
    unsigned long v = wcstoul(s, &end, 16);
    if (end == s || v > 0xFFFFFF) return false;
    *c = (uint32_t)v;
    return true;
}

bool Config::Dialog() {
    bool b[11] = {enterArchive, quickView, view, fullscreen, smooth, showInfo, autosize, enlarge, keepPosition, loop};
    wchar_t bgN[16], bgS[16], pfx[32];
    swprintf(bgN, 16, L"%06X", bgNormal);
    swprintf(bgS, 16, L"%06X", bgSelected);
    wcsncpy(pfx, prefix.c_str(), 31);
    pfx[31] = 0;

    PluginDialogBuilder d(PsInfo, MainGuid, ConfigDialogGuid, MConfigTitle, L"Config");
    d.AddText(MProcess);
    d.AddCheckbox(MEnterArchive, &b[0]);
    d.AddCheckbox(MQuickView, &b[1]);
    d.AddCheckbox(MView, &b[2]);
    d.AddSeparator(MDrawing);
    d.AddCheckbox(MFullscreen, &b[3]);
    d.AddCheckbox(MSmooth, &b[4]);
    d.AddCheckbox(MShowInfo, &b[5]);
    d.AddSeparator(MAutosize);
    d.AddCheckbox(MAutosizeOpen, &b[6]);
    d.AddCheckbox(MEnlarge, &b[7]);
    d.AddSeparator(MJumps);
    d.AddCheckbox(MKeepPosition, &b[8]);
    d.AddCheckbox(MLoop, &b[9]);
    d.AddSeparator(MBackground);
    d.AddTextBefore(d.AddEditField(bgN, 16, 8), MBgNormal);
    d.AddTextBefore(d.AddEditField(bgS, 16, 8), MBgSelected);
    d.AddSeparator();
    d.AddTextBefore(d.AddEditField(pfx, 32, 12), MPrefix);
    d.AddOKCancel(MOk, MCancel);
    if (!d.ShowDialog()) return false;

    enterArchive = b[0];
    quickView = b[1];
    view = b[2];
    fullscreen = b[3];
    smooth = b[4];
    showInfo = b[5];
    autosize = b[6];
    enlarge = b[7];
    keepPosition = b[8];
    loop = b[9];
    ParseColor(bgN, &bgNormal);
    ParseColor(bgS, &bgSelected);
    if (*pfx) prefix = pfx;
    Save();
    return true;
}
