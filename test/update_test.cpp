// Test of the updater (src/update.cpp), without Far. Run by run_tests.sh in test/images, with the
// zips mkimages.py makes. A folder stands in for the plugin's; a loaded DLL in it stands in for
// the running plugin. "--live dir": also the real thing, the latest release from GitHub into dir.
#include "../src/update.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static int Fails;
static void Check(bool ok, const char *what) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) Fails++;
}

static void Put(const std::wstring &path, const char *text) {
    FILE *f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}
static std::string Get(const std::wstring &path) {
    std::string s;
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return s;
    char b[256];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
static bool Exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static void TestVersions() {
    Check(UpdateIsNewer(L"1.4.0", 1, 3, 0), "1.4.0 is newer than 1.3.0");
    Check(UpdateIsNewer(L"v1.3.1", 1, 3, 0), "v1.3.1 is newer than 1.3.0");
    Check(UpdateIsNewer(L"2.0.0", 1, 30, 9), "2.0.0 is newer than 1.30.9");
    Check(UpdateIsNewer(L"1.10.0", 1, 9, 0), "1.10.0 is newer than 1.9.0");
    Check(!UpdateIsNewer(L"1.3.0", 1, 3, 0), "1.3.0 is not newer than itself");
    Check(!UpdateIsNewer(L"1.2.9", 1, 3, 0), "1.2.9 is not newer than 1.3.0");
    Check(!UpdateIsNewer(L"1.4", 1, 3, 0) && !UpdateIsNewer(L"1.4.0-rc1", 1, 3, 0) && !UpdateIsNewer(L"", 1, 3, 0),
          "what is not a version is never newer");
}

static const char kAsset[] =
    "{\"name\":\"%s\",\"uploader\":{\"login\":\"x\",\"html_url\":\"https://github.com/x\"},\"size\":%d,"
    "\"digest\":\"sha256:%s\",\"browser_download_url\":\"%s\"}";

static std::string Release(const char *tag, const char *digest, const char *url) {
    char a[1024], b[1024];
    std::string v = tag + 1;
    snprintf(a, sizeof a, kAsset, ("FarNitroView-" + v + "-x64-noavx2.zip").c_str(), 111,
             "1111111111111111111111111111111111111111111111111111111111111111",
             ("https://github.com/danzig666/farnitroview/releases/download/" + std::string(tag) + "/noavx2.zip").c_str());
    snprintf(b, sizeof b, kAsset, ("FarNitroView-" + v + "-x64.zip").c_str(), 222, digest, url);
    return std::string("{\"html_url\": \"https://github.com/r\", \"tag_name\": \"") + tag + "\", \"assets\": [" + a + "," + b + "]}";
}

static void TestParse() {
    const char *d = "2222222222222222222222222222222222222222222222222222222222222222";
    const char *u = "https://github.com/danzig666/farnitroview/releases/download/v1.4.0/x64.zip";
    UpdateInfo i;
    std::wstring e;
    Check(UpdateParseRelease(Release("v1.4.0", d, u), false, &i, &e) && i.version == L"1.4.0" && i.size == 222 && i.sha256 == d,
          "a release: the AVX2 zip");
    Check(UpdateParseRelease(Release("v1.4.0", d, u), true, &i, &e) && i.size == 111 && i.sha256[0] == '1', "a release: the other zip");
    Check(!UpdateParseRelease(Release("v1.4.0", d, "https://evil.example/x.zip"), false, &i, &e), "a zip from anywhere else is refused");
    Check(!UpdateParseRelease(Release("v1.4.0", "abc", u), false, &i, &e), "a zip without a proper SHA-256 is refused");
    Check(!UpdateParseRelease("{\"message\":\"Not Found\"}", false, &i, &e), "an answer without a release is refused");
    Check(!UpdateParseRelease(Release("vnext", d, u), false, &i, &e), "a tag that is not a version is refused");
}

static void TestApply(const std::wstring &work) {
    const std::wstring dir = work + L"plugin\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring e;
    bool na = false;
    const std::wstring ok = work + L"update-ok.zip", bad = work + L"update-nodll.zip";
    const std::string sha = UpdateFileSha256(ok);
    Check(sha.size() == 64, "SHA-256 of a file");

    // the plugin as installed: a loaded DLL (a real one, so that Windows really locks it)
    CopyFileW(L"..\\..\\build\\pdfium.dll", (dir + L"FarNitroView.dll").c_str(), FALSE);
    Put(dir + L"FarNitroViewEng.lng", "old lng");
    Put(dir + L"keep-me.txt", "mine");
    HMODULE loaded = LoadLibraryW((dir + L"FarNitroView.dll").c_str());
    Check(loaded != nullptr, "the old plugin is loaded");

    // what must not touch anything
    Check(!UpdateApply(ok, "0000", dir, &e, &na) && !na, "a zip with the wrong SHA-256 is refused");
    Check(Get(dir + L"FarNitroViewEng.lng") == "old lng", "... and nothing changed");
    e.clear();
    Check(!UpdateApply(bad, UpdateFileSha256(bad), dir, &e, &na), "a zip without the plugin is refused");
    Check(Get(dir + L"FarNitroViewEng.lng") == "old lng" && !Exists(dir + L".update-tmp"), "... and nothing changed, nothing left");

    // a file that cannot be moved aside: all put back
    e.clear();
    HANDLE locked = CreateFileW((dir + L"FarNitroViewEng.lng").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(!UpdateApply(ok, sha, dir, &e, &na), "a file that cannot be replaced fails the update");
    CloseHandle(locked);
    Check(Get(dir + L"FarNitroViewEng.lng") == "old lng", "... that file is unchanged");
    Check(Get(dir + L"FarNitroView.dll").size() > 1000, "... and the plugin is the old one again");

    // no right to write there (a folder denying it: this also holds when running as administrator)
    e.clear();
    const std::wstring denied = work + L"plugin\\denied\\";
    CreateDirectoryW(denied.c_str(), nullptr);
    _wsystem((L"icacls \"" + denied + L".\" /deny *S-1-1-0:(WD,AD) >nul").c_str());
    Check(!UpdateApply(ok, sha, denied, &e, &na) && na, "a folder we may not write to is reported as such");
    _wsystem((L"icacls \"" + denied + L".\" /remove:d *S-1-1-0 >nul").c_str());
    RemoveDirectoryW(denied.c_str());

    // the update itself, with the old DLL still loaded
    e.clear();
    Check(UpdateApply(ok, sha, dir, &e, &na), "the update");
    if (!e.empty()) wprintf(L"    %ls\n", e.c_str());
    Check(Get(dir + L"FarNitroView.dll") == "new dll", "... the plugin is the new one");
    Check(Get(dir + L"FarNitroViewEng.lng") == "new lng", "... the language file too");
    Check(Get(dir + L"licenses-pdfium\\a.txt") == "new license", "... and a new subfolder");
    Check(Get(dir + L"keep-me.txt") == "mine", "... what is not in the update stays");
    Check(!Exists(dir + L".update-tmp"), "... nothing left of the unpacking");
    FreeLibrary(loaded);   // (Far restarted)
    UpdateCleanup(dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.old").c_str(), &fd);
    Check(h == INVALID_HANDLE_VALUE, "the old files are gone at the next start");
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && !wcscmp(argv[1], L"--live")) {
        UpdateInfo i;
        std::wstring e, dir = argv[2];
        bool na = false;
        if (dir.back() != L'\\') dir += L'\\';
        bool ok = UpdateFetchLatest(false, &i, &e);
        wprintf(L"latest: %ls %ls %llu bytes %hs\n", i.version.c_str(), i.url.c_str(), i.size, i.sha256.c_str());
        ok = ok && UpdateInstall(i, dir, &e, &na);
        wprintf(L"install: %ls %ls\n", ok ? L"ok" : L"FAILED", e.c_str());
        return ok ? 0 : 1;
    }
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    TestVersions();
    TestParse();
    TestApply(std::wstring(cwd) + L"\\");
    printf(Fails ? "update tests FAILED\n" : "all update tests passed\n");
    return Fails;
}
