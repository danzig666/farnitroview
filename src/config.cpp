// FarNitroView for Far Manager: settings and the configuration dialog.
#include "farglobals.h"

#include "DlgBuilder.hpp"
#include "PluginSettings.hpp"

#include <cmath>
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
    ppAutolevels = s.Get(0, L"Autolevels", ppAutolevels);
    ppAutoluma = s.Get(0, L"Autoluma", ppAutoluma);
    ppChroma = s.Get(0, L"Chroma", ppChroma);
    chroma = s.Get(0, L"ChromaPercent", (int)(chroma * 100)) / 100.0;
    titleTemplate = s.Get(0, L"TitleTemplate", titleTemplate.c_str());
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
    s.Set(0, L"Autolevels", ppAutolevels);
    s.Set(0, L"Autoluma", ppAutoluma);
    s.Set(0, L"Chroma", ppChroma);
    s.Set(0, L"ChromaPercent", (int)std::lround(chroma * 100));
    s.Set(0, L"TitleTemplate", titleTemplate.c_str());
}

static bool ParseColor(const wchar_t *s, uint32_t *c) {
    while (*s == L' ' || *s == L'#') s++;
    wchar_t *end;
    unsigned long v = wcstoul(s, &end, 16);
    if (end == s || v > 0xFFFFFF) return false;
    *c = (uint32_t)v;
    return true;
}

// DlgBuilder's EndColumns() puts every item of the second column at the start of the column, even the
// edit fields AddTextBefore had moved after their labels (and makes them one wider); where the column
// starts is known only when the builder sizes the dialog, so the fields are moved back here.
struct LabeledField {
    intptr_t label, field;
};

static intptr_t WINAPI ConfigDlgProc(HANDLE dlg, intptr_t msg, intptr_t p1, void *p2) {
    if (msg == DN_INITDIALOG) {
        for (auto *f = (const LabeledField *)p2; f->label; f++) {
            SMALL_RECT label, field;
            PsInfo.SendDlgMessage(dlg, DM_GETITEMPOSITION, f->label, &label);
            PsInfo.SendDlgMessage(dlg, DM_GETITEMPOSITION, f->field, &field);
            const auto width = field.Right - field.Left;
            field.Left = (SHORT)(label.Left + PsInfo.SendDlgMessage(dlg, DM_GETTEXT, f->label, nullptr) + 1);
            field.Right = (SHORT)(field.Left + width - 1);
            PsInfo.SendDlgMessage(dlg, DM_SETITEMPOSITION, f->field, &field);
        }
    }
    return PsInfo.DefDlgProc(dlg, msg, p1, p2);
}

bool Config::Dialog() {
    bool b[13] = {enterArchive, quickView, view,  fullscreen, smooth,       showInfo, autosize,
                  enlarge,      keepPosition, loop, ppAutolevels, ppAutoluma, ppChroma};
    wchar_t bgN[16], bgS[16], pfx[32], chr[16];
    swprintf(bgN, 16, L"%06X", bgNormal);
    swprintf(bgS, 16, L"%06X", bgSelected);
    swprintf(chr, 16, L"%.2f", chroma);
    wcsncpy(pfx, prefix.c_str(), 31);
    pfx[31] = 0;
    std::wstring tpl = titleTemplate;

    LabeledField moved[4] = {};
    PluginDialogBuilder d(PsInfo, MainGuid, ConfigDialogGuid, MConfigTitle, L"Config", ConfigDlgProc, moved);
    d.StartColumns();
    const FarDialogItem *first = d.AddText(MProcess);  // item 1: item 0 is the border
    int nMoved = 0;
    const auto addField = [&](wchar_t *value, int width, int labelId) {
        const FarDialogItem *field = d.AddEditField(value, 16, width);
        const FarDialogItem *label = d.AddTextBefore(const_cast<FarDialogItem *>(field), labelId);
        moved[nMoved++] = {label - first + 1, field - first + 1};
    };
    d.AddCheckbox(MEnterArchive, &b[0]);
    d.AddCheckbox(MQuickView, &b[1]);
    d.AddCheckbox(MView, &b[2]);
    d.AddText(MDrawing);
    d.AddCheckbox(MFullscreen, &b[3]);
    d.AddCheckbox(MSmooth, &b[4]);
    d.AddCheckbox(MShowInfo, &b[5]);
    d.AddText(MAutosize);
    d.AddCheckbox(MAutosizeOpen, &b[6]);
    d.AddCheckbox(MEnlarge, &b[7]);
    d.ColumnBreak();
    d.AddText(MJumps);
    d.AddCheckbox(MKeepPosition, &b[8]);
    d.AddCheckbox(MLoop, &b[9]);
    d.AddText(MPostProcessing);
    d.AddCheckbox(MAutolevels, &b[10]);
    d.AddCheckbox(MAutoluma, &b[11]);
    d.AddCheckbox(MChroma, &b[12]);
    addField(chr, 6, MChromaMul);
    d.AddText(MBackground);
    addField(bgN, 8, MBgNormal);
    addField(bgS, 8, MBgSelected);
    d.EndColumns();
    d.AddSeparator();
    d.AddText(MTitleTemplate);
    d.AddEditField(tpl, 72);
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
    ppAutolevels = b[10];
    ppAutoluma = b[11];
    ppChroma = b[12];
    double c = _wtof(chr);
    if (c > 0 && c <= 10) chroma = c;
    ParseColor(bgN, &bgNormal);
    ParseColor(bgS, &bgSelected);
    if (*pfx) prefix = pfx;
    if (!tpl.empty()) titleTemplate = tpl;
    Save();
    return true;
}
