// FarNitroView for Far Manager: updating the plugin from the GitHub releases.
#include "update.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#ifndef NV_VERSION_MAJOR
#define NV_VERSION_MAJOR 1
#define NV_VERSION_MINOR 0
#define NV_VERSION_BUILD 0
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;

static const wchar_t kLatestUrl[] = L"https://api.github.com/repos/danzig666/farnitroview/releases/latest";
static const char kDownloadPrefix[] = "https://github.com/danzig666/farnitroview/releases/download/";
static const wchar_t kTmpDir[] = L".update-tmp";

std::wstring UpdatePluginDir() {
    std::wstring p(32768, L'\0');
    p.resize(GetModuleFileNameW((HMODULE)&__ImageBase, &p[0], (DWORD)p.size()));
    p.resize(p.find_last_of(L"\\/") + 1);
    return p;
}

static std::wstring Widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static bool ReadVersion(const std::wstring &text, int v[3]) {
    const wchar_t *s = text.c_str();
    if (*s == L'v' || *s == L'V') s++;
    for (int i = 0; i < 3; i++) {
        if (*s < L'0' || *s > L'9') return false;
        v[i] = 0;
        while (*s >= L'0' && *s <= L'9' && v[i] < 100000) v[i] = v[i] * 10 + (*s++ - L'0');
        if (i < 2 && *s++ != L'.') return false;
    }
    return *s == 0;
}

bool UpdateIsNewer(const std::wstring &version, int major, int minor, int build) {
    int v[3];
    if (!ReadVersion(version, v)) return false;
    const int c[3] = {major, minor, build};
    for (int i = 0; i < 3; i++)
        if (v[i] != c[i]) return v[i] > c[i];
    return false;
}

// ---------------------------------------------------------------------------
// the release, from the API's JSON: only a few values, so no parser. An answer that does not look
// as expected is an error, never a guess.

// the string value of "key" in [from, to)
static bool FindValue(const std::string &j, const char *key, size_t from, size_t to, std::string *out) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos || p >= to) return false;
    p += k.size();
    while (p < j.size() && (j[p] == ' ' || j[p] == '\n' || j[p] == '\r' || j[p] == '\t' || j[p] == ':')) p++;
    if (p >= j.size() || j[p] != '"') return false;
    const size_t e = j.find('"', p + 1);
    if (e == std::string::npos || e >= to) return false;
    *out = j.substr(p + 1, e - p - 1);
    return true;
}

bool UpdateParseRelease(const std::string &j, bool noAvx2, UpdateInfo *info, std::wstring *error) {
    std::string tag, digest, url;
    if (!FindValue(j, "tag_name", 0, j.size(), &tag)) {
        *error = L"Unexpected answer from GitHub";
        return false;
    }
    info->version = Widen(tag[0] == 'v' ? tag.substr(1) : tag);
    int v[3];
    if (!ReadVersion(info->version, v)) {
        *error = L"Unexpected version on GitHub: " + Widen(tag);
        return false;
    }
    const std::string name = "\"FarNitroView-" + tag.substr(tag[0] == 'v' ? 1 : 0) + (noAvx2 ? "-x64-noavx2.zip\"" : "-x64.zip\"");
    const size_t at = j.find(name);
    if (at == std::string::npos) {
        *error = L"The release has no zip for this build";
        return false;
    }
    // an asset: its name, ... its size, digest ... and last its download address
    const size_t end = j.find("\"browser_download_url\"", at);
    if (end == std::string::npos || !FindValue(j, "browser_download_url", end, j.size(), &url) ||
        !FindValue(j, "digest", at, end, &digest) || digest.compare(0, 7, "sha256:") || digest.size() != 7 + 64) {
        *error = L"The release does not say how to check its zip";
        return false;
    }
    size_t sz = j.find("\"size\"", at);
    unsigned long long size = 0;
    if (sz != std::string::npos && sz < end) {
        sz += 6;
        while (sz < end && (j[sz] == ' ' || j[sz] == ':')) sz++;
        while (sz < end && j[sz] >= '0' && j[sz] <= '9') size = size * 10 + (j[sz++] - '0');
    }
    if (url.compare(0, sizeof kDownloadPrefix - 1, kDownloadPrefix) || !size || size > (1ull << 30)) {
        *error = L"Unexpected download on GitHub";
        return false;
    }
    info->url = Widen(url);
    info->size = size;
    info->sha256 = digest.substr(7);
    wchar_t pg[128];
    swprintf(pg, 128, L"https://github.com/danzig666/farnitroview/releases/tag/%ls", Widen(tag).c_str());
    info->page = pg;
    return true;
}

// ---------------------------------------------------------------------------
// HTTPS

static std::mutex NetMu;
static HINTERNET NetSession;
static std::atomic<bool> NetCancelled{false};

void UpdateCancel() {
    NetCancelled = true;
    std::lock_guard<std::mutex> l(NetMu);
    if (NetSession) {   // (closing it makes what is waiting on it fail at once)
        WinHttpCloseHandle(NetSession);
        NetSession = nullptr;
    }
}

// sink: gets the body in pieces, false stops. Redirects are followed, only to https (WinHTTP's default).
template <class Sink> static bool HttpGet(const std::wstring &url, Sink sink, std::wstring *error) {
    URL_COMPONENTS u = {sizeof u};
    wchar_t host[256], path[2048];
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS) {
        *error = L"Bad address: " + url;
        return false;
    }
    wchar_t agent[64];
    swprintf(agent, 64, L"FarNitroView/%d.%d.%d", NV_VERSION_MAJOR, NV_VERSION_MINOR, NV_VERSION_BUILD);
    HINTERNET ses = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        *error = L"Cannot use the network";
        return false;
    }
    WinHttpSetTimeouts(ses, 10000, 10000, 10000, 30000);
    {
        std::lock_guard<std::mutex> l(NetMu);
        if (NetCancelled) {
            WinHttpCloseHandle(ses);
            *error = L"Cancelled";
            return false;
        }
        NetSession = ses;
    }
    bool ok = false;
    DWORD status = 0, statusSize = sizeof status;
    HINTERNET con = WinHttpConnect(ses, host, u.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr) &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        if (status == 200) {
            std::vector<char> buf(1 << 16);
            for (;;) {
                DWORD n = 0;
                if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &n)) break;
                if (!n) {
                    ok = true;
                    break;
                }
                if (!sink(buf.data(), n)) break;
            }
        } else {
            *error = L"GitHub answered with status " + std::to_wstring(status);
        }
    }
    if (!ok && error->empty()) *error = L"Cannot reach GitHub (" + std::to_wstring(GetLastError()) + L")";
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    {
        std::lock_guard<std::mutex> l(NetMu);
        if (NetSession == ses) {
            WinHttpCloseHandle(ses);
            NetSession = nullptr;
        }
    }
    return ok;
}

bool UpdateFetchLatest(bool noAvx2, UpdateInfo *info, std::wstring *error) {
    std::string body;
    auto sink = [&](const char *p, DWORD n) {
        body.append(p, n);
        return body.size() < (8u << 20);
    };
    return HttpGet(kLatestUrl, sink, error) && UpdateParseRelease(body, noAvx2, info, error);
}

// ---------------------------------------------------------------------------
// files

std::string UpdateFileSha256(const std::wstring &path) {
    std::string hex;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0))) {
        std::vector<unsigned char> buf(1 << 16);
        DWORD n = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n)
            ok = BCRYPT_SUCCESS(BCryptHashData(h, buf.data(), n, 0));
        unsigned char d[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(h, d, sizeof d, 0)))
            for (unsigned char c : d) {
                char t[3];
                snprintf(t, 3, "%02x", c);
                hex += t;
            }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

// every file under dir, relative to it; folders first-come
static void ListTree(const std::wstring &dir, const std::wstring &rel, std::vector<std::wstring> *files, std::vector<std::wstring> *dirs) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring r = rel + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // (never through a link to somewhere else)
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dirs->push_back(r);
            ListTree(dir, r + L"\\", files, dirs);
        } else
            files->push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void DeleteTree(const std::wstring &dir) {
    std::vector<std::wstring> files, dirs;
    ListTree(dir, L"", &files, &dirs);
    for (auto &f : files) {
        SetFileAttributesW((dir + f).c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW((dir + f).c_str());
    }
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) RemoveDirectoryW((dir + *it).c_str());
    RemoveDirectoryW(dir.c_str());
}

// name.<digits>.old, as Swap names what it moves aside
static bool IsRenamedOld(const std::wstring &f) {
    if (f.size() < 6 || _wcsicmp(f.c_str() + f.size() - 4, L".old")) return false;
    size_t i = f.size() - 5;
    if (f[i] < L'0' || f[i] > L'9') return false;
    while (i > 0 && f[i] >= L'0' && f[i] <= L'9') i--;
    return f[i] == L'.';
}

void UpdateCleanup(const std::wstring &dir) {
    if (dir.empty()) return;
    DeleteTree(dir + kTmpDir + L"\\");
    std::vector<std::wstring> files, dirs;
    ListTree(dir, L"", &files, &dirs);
    for (auto &f : files)
        if (IsRenamedOld(f)) DeleteFileW((dir + f).c_str());   // (in use by another Far: next time)
}

// the unpacking place, next to the plugin (renames there stay on the same disk)
static bool MakeTmp(const std::wstring &dir, std::wstring *error, bool *noAccess) {
    const std::wstring tmp = dir + kTmpDir;
    DeleteTree(tmp + L"\\");
    if (CreateDirectoryW(tmp.c_str(), nullptr)) return true;
    const DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED) *noAccess = true;
    *error = L"Cannot write to " + dir;
    return false;
}

static bool Unzip(const std::wstring &tmp, const std::wstring &zipName) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    // (tar.exe is part of Windows 10 and 11; it refuses paths leading out of the folder)
    std::wstring tar = std::wstring(sys) + L"\\tar.exe", cmd = L"\"" + tar + L"\" -xf \"" + zipName + L"\" -C x";
    if (!CreateDirectoryW((tmp + L"x").c_str(), nullptr)) return false;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(tar.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, tmp.c_str(), &si, &pi)) return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

// The new files of `from` over dir; on a failure all as it was.
static bool Swap(const std::wstring &from, const std::wstring &dir, std::wstring *error) {
    std::vector<std::wstring> files, dirs;
    ListTree(from, L"", &files, &dirs);
    for (auto &d : dirs) CreateDirectoryW((dir + d).c_str(), nullptr);
    struct Done { std::wstring target, old; };
    std::vector<Done> done;
    const std::wstring stamp = L"." + std::to_wstring(GetTickCount64()) + L".old";
    bool ok = true;
    for (auto &f : files) {
        Done d;
        d.target = dir + f;
        if (GetFileAttributesW(d.target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            d.old = d.target + stamp;
            if (!MoveFileW(d.target.c_str(), d.old.c_str())) {
                *error = L"Cannot replace " + d.target + L" (" + std::to_wstring(GetLastError()) + L")";
                ok = false;
                break;
            }
        }
        done.push_back(d);
        if (!MoveFileW((from + f).c_str(), d.target.c_str())) {
            *error = L"Cannot write " + d.target + L" (" + std::to_wstring(GetLastError()) + L")";
            ok = false;
            break;
        }
    }
    if (ok) return true;
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
        DeleteFileW(it->target.c_str());
        if (!it->old.empty()) MoveFileW(it->old.c_str(), it->target.c_str());
    }
    return false;
}

static bool ApplyInTmp(const std::wstring &zipInTmp, const std::string &sha256, const std::wstring &dir, std::wstring *error) {
    const std::wstring tmp = dir + kTmpDir + L"\\";
    if (UpdateFileSha256(tmp + zipInTmp) != sha256) {
        *error = L"The downloaded file is not what GitHub says it should be";
        return false;
    }
    if (!Unzip(tmp, zipInTmp)) {
        *error = L"Cannot unpack the update";
        return false;
    }
    const std::wstring from = tmp + L"x\\FarNitroView\\";
    if (GetFileAttributesW((from + L"FarNitroView.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        *error = L"The update does not contain the plugin";
        return false;
    }
    return Swap(from, dir, error);
}

bool UpdateApply(const std::wstring &zip, const std::string &sha256, const std::wstring &dir, std::wstring *error, bool *noAccess) {
    *noAccess = false;
    if (!MakeTmp(dir, error, noAccess)) return false;
    bool ok = CopyFileW(zip.c_str(), (dir + kTmpDir + L"\\update.zip").c_str(), FALSE) && ApplyInTmp(L"update.zip", sha256, dir, error);
    if (!ok && error->empty()) *error = L"Cannot read the update";
    if (ok) UpdateCleanup(dir);
    else DeleteTree(dir + kTmpDir + L"\\");
    return ok;
}

bool UpdateInstall(const UpdateInfo &info, const std::wstring &dir, std::wstring *error, bool *noAccess) {
    *noAccess = false;
    if (!MakeTmp(dir, error, noAccess)) return false;
    const std::wstring zip = dir + kTmpDir + L"\\update.zip";
    HANDLE f = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = f != INVALID_HANDLE_VALUE;
    if (ok) {
        unsigned long long got = 0;
        auto sink = [&](const char *p, DWORD n) {
            DWORD w = 0;
            got += n;
            return got <= info.size && WriteFile(f, p, n, &w, nullptr) && w == n;
        };
        ok = HttpGet(info.url, sink, error);
        CloseHandle(f);
        if (ok && got != info.size) {
            *error = L"The download is incomplete";
            ok = false;
        }
    } else
        *error = L"Cannot write to " + dir;
    ok = ok && ApplyInTmp(L"update.zip", info.sha256, dir, error);
    if (ok) UpdateCleanup(dir);
    else DeleteTree(dir + kTmpDir + L"\\");
    return ok;
}
