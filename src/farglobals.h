// FarNitroView for Far Manager: what the parts of the plugin share - Far's API, settings, messages.
#pragma once
#include <windows.h>
#include <string>

#include "plugin.hpp"

#ifndef NV_VERSION_MAJOR
#define NV_VERSION_MAJOR 1
#define NV_VERSION_MINOR 0
#define NV_VERSION_BUILD 0
#endif

extern PluginStartupInfo PsInfo;
extern FarStandardFunctions FSF;

// {66794197-D0FB-4FF5-A184-59A04BC3ACC1}
static const GUID MainGuid = {0x66794197, 0xd0fb, 0x4ff5, {0xa1, 0x84, 0x59, 0xa0, 0x4b, 0xc3, 0xac, 0xc1}};
// {242DCD8F-A8BE-40E1-BE3C-AD68C1957F5E}
static const GUID MenuGuid = {0x242dcd8f, 0xa8be, 0x40e1, {0xbe, 0x3c, 0xad, 0x68, 0xc1, 0x95, 0x7f, 0x5e}};
// {6141B682-177F-40AD-9F33-0B69EFCFCC00}
static const GUID ConfigMenuGuid = {0x6141b682, 0x177f, 0x40ad, {0x9f, 0x33, 0x0b, 0x69, 0xef, 0xcf, 0xcc, 0x00}};
// {70B67C20-725A-46A7-AD32-49AE152C3766}
static const GUID ConfigDialogGuid = {0x70b67c20, 0x725a, 0x46a7, {0xad, 0x32, 0x49, 0xae, 0x15, 0x2c, 0x37, 0x66}};
// {E348932A-F09B-45B5-BAD8-880D41DE3667}
static const GUID ErrorMessageGuid = {0xe348932a, 0xf09b, 0x45b5, {0xba, 0xd8, 0x88, 0x0d, 0x41, 0xde, 0x36, 0x67}};

enum LangId {
    MTitle,
    MOk,
    MCancel,
    MConfigTitle,
    MProcess,
    MEnterArchive,
    MQuickView,
    MView,
    MDrawing,
    MFullscreen,
    MSmooth,
    MShowInfo,
    MAutosize,
    MAutosizeOpen,
    MEnlarge,
    MJumps,
    MKeepPosition,
    MLoop,
    MBackground,
    MBgNormal,
    MBgSelected,
    MPrefix,
    MLoading,
    MCannotDecode,
    MSelected,
    MNoImages,
};

const wchar_t *GetMsg(int id);

struct Config {
    bool enterArchive = true;   // Enter / Ctrl-PgDn on an image
    bool quickView = true;      // images in the quick view panel
    bool view = true;           // F3
    bool fullscreen = false;    // start in full screen
    bool smooth = true;         // smooth scaling
    bool showInfo = true;       // the information line
    bool autosize = true;       // fit large images on opening
    bool enlarge = false;       // ... and small ones too
    bool keepPosition = false;  // keep the zoom and position when going to another image
    bool loop = false;          // after the last image the first one
    uint32_t bgNormal = 0x000000;
    uint32_t bgSelected = 0x202040;
    std::wstring prefix = L"nv";

    void Load();
    void Save() const;
    bool Dialog();   // the configuration dialog; true if changed
};

extern Config Cfg;
