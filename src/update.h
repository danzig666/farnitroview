// FarNitroView for Far Manager: updating the plugin from the GitHub releases. Nothing of Far here
// (the prompts are in plugin.cpp).
//
// The release's zip is checked against the SHA-256 GitHub gives for it and unpacked next to the
// plugin. The files in use are renamed out of the way (a loaded DLL can be renamed, not replaced),
// the new ones moved in, everything put back if a step fails. The new version runs when Far is
// restarted, which also removes the renamed old files.
#pragma once
#include <string>

struct UpdateInfo {
    std::wstring version;   // "1.4.0"
    std::wstring url;       // of the zip for this build
    std::wstring page;      // of the release
    unsigned long long size = 0;
    std::string sha256;     // lower case hex
};

// The folder this module is in, with a backslash at the end
std::wstring UpdatePluginDir();
// Is this version ("1.4.0", "v1.4.0") newer than major.minor.build? False if it cannot be read.
bool UpdateIsNewer(const std::wstring &version, int major, int minor, int build);
// The latest release in the JSON GitHub's API returns, and the zip for this CPU variant in it.
bool UpdateParseRelease(const std::string &json, bool noAvx2, UpdateInfo *info, std::wstring *error);
// Asks GitHub (blocks, at most some 30 seconds).
bool UpdateFetchLatest(bool noAvx2, UpdateInfo *info, std::wstring *error);
// Downloads and installs into dir. *noAccess: dir is not writable, nothing was done.
bool UpdateInstall(const UpdateInfo &info, const std::wstring &dir, std::wstring *error, bool *noAccess);
// The part after the download: a zip already on the disk (its folder FarNitroView/ goes into dir).
bool UpdateApply(const std::wstring &zip, const std::string &sha256, const std::wstring &dir, std::wstring *error,
                 bool *noAccess);
std::string UpdateFileSha256(const std::wstring &path);
// Makes a running UpdateFetchLatest / UpdateInstall give up at once.
void UpdateCancel();
// Removes what an earlier update left in dir: renamed old files, an unfinished unpacking.
void UpdateCleanup(const std::wstring &dir);
