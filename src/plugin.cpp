// FarNitroView for Far Manager 3 (x64): an image viewer in the spirit of PictureView, on the
// decoders of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro).
//
// It opens images on Enter / Ctrl-PgDn (Analyse), instead of Far's viewer (F3), in the quick
// view panel, from the plugin menu and with the "nv:" command prefix.
#include "farglobals.h"
#include "image.h"
#include "viewer.h"

#include <vector>

PluginStartupInfo PsInfo;
FarStandardFunctions FSF;

enum { SYNC_OPEN = 1, SYNC_QVIEW = 2 };

static std::wstring PendingOpen;   // F3: the image to open once Far is back in the panels
static bool PendingIntercepted;
static intptr_t SkipViewerId = -1;

static std::wstring ViewerFileName(intptr_t id) {
    size_t n = (size_t)PsInfo.ViewerControl(id, VCTL_GETFILENAME, 0, nullptr);
    if (!n) return L"";
    std::wstring s(n, L'\0');
    PsInfo.ViewerControl(id, VCTL_GETFILENAME, n, &s[0]);
    s.resize(wcslen(s.c_str()));
    return s;
}

// the active panel's current item as a path, if it is a real file
static std::wstring CurrentPanelFile() {
    PanelInfo pi = {sizeof pi};
    if (!PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi) || pi.PanelType != PTYPE_FILEPANEL ||
        !(pi.Flags & PFLAGS_REALNAMES) || !pi.ItemsNumber)
        return L"";
    size_t sz = (size_t)PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELITEM, (intptr_t)pi.CurrentItem, nullptr);
    if (!sz) return L"";
    std::vector<char> buf(sz);
    FarGetPluginPanelItem g = {sizeof g, sz, (PluginPanelItem *)buf.data()};
    if (!PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELITEM, (intptr_t)pi.CurrentItem, &g) || !g.Item->FileName ||
        (g.Item->FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return L"";
    std::wstring name = g.Item->FileName;
    if (name.find_first_of(L"\\:") != std::wstring::npos) return name;
    size_t ds = (size_t)PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);
    if (!ds) return L"";
    std::vector<char> db(ds);
    FarPanelDirectory *fd = (FarPanelDirectory *)db.data();
    fd->StructSize = sizeof *fd;
    if (!PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, ds, fd) || !fd->Name) return L"";
    std::wstring dir = fd->Name;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    return dir + name;
}

static bool IsFile(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------------------

extern "C" {

void WINAPI GetGlobalInfoW(GlobalInfo *Info) {
    Info->StructSize = sizeof *Info;
    Info->MinFarVersion = MAKEFARVERSION(3, 0, 0, 6699, VS_RELEASE);
    Info->Version = MAKEFARVERSION(NV_VERSION_MAJOR, NV_VERSION_MINOR, 0, NV_VERSION_BUILD, VS_RELEASE);
    Info->Guid = MainGuid;
    Info->Title = L"FarNitroView";
    Info->Description = L"Fast image viewer on the jpeg-nitro decoders (PictureView style)";
    Info->Author = L"danzig666; decoders: A'rpi (jpeg-nitro)";
}

void WINAPI SetStartupInfoW(const PluginStartupInfo *Info) {
    PsInfo = *Info;
    FSF = *Info->FSF;
    PsInfo.FSF = &FSF;
    Cfg.Load();
}

void WINAPI GetPluginInfoW(PluginInfo *Info) {
    Info->StructSize = sizeof *Info;
    Info->Flags = PF_VIEWER | PF_EDITOR;
    static const wchar_t *menu[1];
    menu[0] = GetMsg(MTitle);
    Info->PluginMenu.Guids = &MenuGuid;
    Info->PluginMenu.Strings = menu;
    Info->PluginMenu.Count = 1;
    Info->PluginConfig.Guids = &ConfigMenuGuid;
    Info->PluginConfig.Strings = menu;
    Info->PluginConfig.Count = 1;
    Info->CommandPrefix = Cfg.prefix.c_str();
}

intptr_t WINAPI ConfigureW(const ConfigureInfo *) {
    QuickViewSuspend(true);
    Cfg.Dialog();
    QuickViewSuspend(false);
    return TRUE;
}

HANDLE WINAPI AnalyseW(const AnalyseInfo *Info) {
    if (!Cfg.enterArchive || !Info->FileName || (Info->OpMode & (OPM_FIND | OPM_SILENT | OPM_QUICKVIEW | OPM_VIEW | OPM_EDIT | OPM_DESCR | OPM_COMMANDS)))
        return nullptr;
    if (!IsImageName(Info->FileName) && !IsImageSignature(Info->Buffer, Info->BufferSize)) return nullptr;
    return (HANDLE)1;
}

void WINAPI CloseAnalyseW(const CloseAnalyseInfo *) {}

HANDLE WINAPI OpenW(const OpenInfo *Info) {
    std::wstring path;
    switch (Info->OpenFrom) {
    case OPEN_ANALYSE: {
        const OpenAnalyseInfo *oa = (const OpenAnalyseInfo *)Info->Data;
        if (oa && oa->Info && oa->Info->FileName) RunViewer(oa->Info->FileName, false);
        return PANEL_STOP;
    }
    case OPEN_PLUGINSMENU:
        path = CurrentPanelFile();
        break;
    case OPEN_VIEWER:
        path = ViewerFileName(-1);
        break;
    case OPEN_EDITOR: {
        size_t n = (size_t)PsInfo.EditorControl(-1, ECTL_GETFILENAME, 0, nullptr);
        if (n) {
            path.resize(n);
            PsInfo.EditorControl(-1, ECTL_GETFILENAME, n, &path[0]);
            path.resize(wcslen(path.c_str()));
        }
        break;
    }
    case OPEN_COMMANDLINE: {
        const OpenCommandLineInfo *cl = (const OpenCommandLineInfo *)Info->Data;
        path = cl && cl->CommandLine ? cl->CommandLine : L"";
        while (!path.empty() && path.front() == L' ') path.erase(0, 1);
        while (!path.empty() && path.back() == L' ') path.pop_back();
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') path = path.substr(1, path.size() - 2);
        if (path.empty()) path = CurrentPanelFile();
        else {
            size_t n = (size_t)FSF.ConvertPath(CPM_FULL, path.c_str(), nullptr, 0);
            if (n) {
                std::wstring full(n, L'\0');
                FSF.ConvertPath(CPM_FULL, path.c_str(), &full[0], n);
                full.resize(wcslen(full.c_str()));
                path = full;
            }
        }
        break;
    }
    default:
        return nullptr;
    }
    if (!path.empty() && IsFile(path)) RunViewer(path, false);
    else if (Info->OpenFrom != OPEN_PLUGINSMENU || !path.empty()) {
        const wchar_t *msg[] = {GetMsg(MTitle), GetMsg(MNoImages), path.c_str()};
        PsInfo.Message(&MainGuid, &ErrorMessageGuid, FMSG_WARNING | FMSG_MB_OK, nullptr, msg, 3, 0);
    }
    return nullptr;
}

// F3 on an image in the panels: our viewer instead of Far's, without Far's flashing first
intptr_t WINAPI ProcessConsoleInputW(ProcessConsoleInputInfo *Info) {
    if (!Cfg.view || Info->Rec.EventType != KEY_EVENT) return 0;
    const KEY_EVENT_RECORD &k = Info->Rec.Event.KeyEvent;
    if (!k.bKeyDown || k.wVirtualKeyCode != VK_F3 ||
        (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED | SHIFT_PRESSED)))
        return 0;
    WindowType wt = {sizeof wt};
    if (!PsInfo.AdvControl(&MainGuid, ACTL_GETWINDOWTYPE, 0, &wt) || wt.Type != WTYPE_PANELS) return 0;
    std::wstring path = CurrentPanelFile();
    if (path.empty() || !IsImageName(path.c_str())) return 0;
    PendingOpen = path;
    PendingIntercepted = true;
    PsInfo.AdvControl(&MainGuid, ACTL_SYNCHRO, 0, (void *)(intptr_t)SYNC_OPEN);
    return 1;
}

intptr_t WINAPI ProcessViewerEventW(const ProcessViewerEventInfo *Info) {
    if (Info->Event == VE_CLOSE) {
        QuickViewClose(Info->ViewerID);
        return 0;
    }
    if (Info->Event != VE_READ || Info->ViewerID == SkipViewerId) return 0;
    ViewerInfo vi = {sizeof vi};
    if (!PsInfo.ViewerControl(Info->ViewerID, VCTL_GETINFO, 0, &vi)) return 0;
    std::wstring path = ViewerFileName(Info->ViewerID);
    if (path.empty()) return 0;
    // the quick view panel's viewer: as wide as the passive quick view panel's inside
    PanelInfo pp = {sizeof pp};
    bool qview = PsInfo.PanelControl(PANEL_PASSIVE, FCTL_GETPANELINFO, 0, &pp) && pp.PanelType == PTYPE_QVIEWPANEL &&
                 (pp.Flags & PFLAGS_VISIBLE) && vi.WindowSizeX == pp.PanelRect.right - pp.PanelRect.left - 1;
    const bool image = IsImageName(path.c_str());
    if (qview) {
        if (Cfg.quickView && image)
            QuickViewShow(path, Info->ViewerID, (int)pp.PanelRect.left + 1, (int)pp.PanelRect.top + 1,
                          (int)vi.WindowSizeX, (int)vi.WindowSizeY);
        else
            QuickViewClose(-1);
        return 0;
    }
    if (!Cfg.view || !image) return 0;
    // Far's viewer on an image (from an archive, Find file, a macro ...): ours instead, right now
    // (an archive's extracted file is deleted when Far's viewer closes), then Far's is closed
    SkipViewerId = Info->ViewerID;
    RunViewer(path, true);
    PsInfo.ViewerControl(Info->ViewerID, VCTL_QUIT, 0, nullptr);
    return 0;
}

intptr_t WINAPI ProcessSynchroEventW(const ProcessSynchroEventInfo *Info) {
    if (Info->Event != SE_COMMONSYNCHRO) return 0;
    switch ((intptr_t)Info->Param) {
    case SYNC_OPEN:
        if (!PendingOpen.empty()) {
            std::wstring p = PendingOpen;
            bool icp = PendingIntercepted;
            PendingOpen.clear();
            if (IsFile(p)) RunViewer(p, icp);
        }
        break;
    case SYNC_QVIEW:
        QuickViewValidate();
        break;
    }
    return 0;
}

void WINAPI ExitFARW(const ExitInfo *) { ShutdownViewers(); }

}   // extern "C"
