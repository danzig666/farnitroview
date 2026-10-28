// FarNitroView for Far Manager: file -> Image, with jpeg-nitro's decoders and WIC as the fallback.
#include "image.h"
#include "par.h"

#include <climits>
#include <cstring>
#include <shlwapi.h>
#include <wincodec.h>

extern "C" {
#include "nitro_convert.h"
#include "nitro_load.h"
}

bool Image::Alloc(int width, int height) {
    w = width;
    h = height;
    pitch = (size_t)w * 4;
    px = (uint8_t *)_aligned_malloc(pitch * (size_t)h + 64, 64);
    return px != nullptr;
}

// Extensions beyond nv_image_exts that WIC reads on a stock Windows (or with the usual codecs)
static const wchar_t *const kWicExts[] = {L"ico", L"cur", L"jxr", L"wdp", L"hdp", L"dds", L"dib", L"jxl", nullptr};

bool IsImageName(const wchar_t *path) {
    if (nv_is_image_name_w(path)) return true;
    const wchar_t *ext = PathFindExtensionW(path);
    if (!*ext) return false;
    for (const wchar_t *const *e = kWicExts; *e; e++)
        if (!_wcsicmp(ext + 1, *e)) return true;
    return false;
}

bool IsImageSignature(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    if (len >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return true;                  // JPEG
    if (len >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return true;                             // PNG
    if (len >= 6 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) return true;          // GIF
    if (len >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) return true;           // WebP
    if (len >= 4 && !memcmp(p, "8BPS", 4)) return true;                                          // PSD
    if (len >= 12 && !memcmp(p + 4, "ftyp", 4) &&
        (!memcmp(p + 8, "heic", 4) || !memcmp(p + 8, "heix", 4) || !memcmp(p + 8, "mif1", 4) ||
         !memcmp(p + 8, "avif", 4)))
        return true;                                                                             // HEIF / AVIF
    return false;
}

// ---------------------------------------------------------------------------
// conversions to BGRA, transparency over the background

static inline uint8_t Over(int c, int a, int bg) { return (uint8_t)((c * a + bg * (255 - a) + 127) / 255); }
static inline uint8_t OverPremul(int c, int a, int bg) { return (uint8_t)(c + (bg * (255 - a) + 127) / 255); }

// RGB written into the left 3/4 of each BGRA row -> BGRA in place (right to left: no overlap)
static void ExpandRgbRows(Image *im) {
    ParallelFor((size_t)im->h, [im](size_t y) {
        uint8_t *row = im->px + y * im->pitch;
        for (int x = im->w - 1; x >= 0; x--) {
            uint8_t r = row[3 * x], g = row[3 * x + 1], b = row[3 * x + 2];
            row[4 * x] = b;
            row[4 * x + 1] = g;
            row[4 * x + 2] = r;
            row[4 * x + 3] = 255;
        }
    });
}

static uint8_t *GetBuf(size_t len, size_t *cap, void *) {
    *cap = len;
    return (uint8_t *)malloc(len);
}
static void PutBuf(uint8_t *p, size_t, void *) { free(p); }

static std::wstring Widen(const char *s) {
    std::wstring r;
    while (s && *s) r += (wchar_t)(unsigned char)*s++;
    return r;
}

static std::string Utf8(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// Our decoders. 0: decoded; 1: not ours (*orientation may be known); -1: failed for good.
static int DecodeNitro(uint8_t *data, size_t len, int rawOrientation, int rawRead, uint32_t bg, Image *im,
                       int *orientation, std::wstring *error) {
    NvDecoder dec = {GetBuf, PutBuf, nullptr, 1, INT_MAX, 0, 0};
    NvImage ni;
    memset(&ni, 0, sizeof ni);
    uint8_t *buf = nullptr;
    size_t cap = 0;
    int rc = nv_decode(data, &len, rawOrientation, rawRead, &dec, &ni, &buf, &cap);
    if (ni.orientation >= 1 && ni.orientation <= 8) *orientation = ni.orientation;
    if (rc) {
        free(buf);
        if (rc < 0) { *error = L"Out of memory"; return -1; }
        if (ni.error) *error = Widen(ni.error);
        return 1;
    }
    if (!im->Alloc(ni.width, ni.height)) {
        free(buf);
        *error = L"Out of memory";
        return -1;
    }
    const int bR = bg >> 16 & 255, bG = bg >> 8 & 255, bB = bg & 255;
    bool alpha = false;
    switch (ni.kind) {
    case NV_YUV:
        nc_yuv_to_rgb(&ni, buf, im->px, im->pitch);
        ExpandRgbRows(im);
        break;
    case NV_PLANAR:
        nc_psd_to_rgb(&ni, buf, im->px, im->pitch);
        ExpandRgbRows(im);
        break;
    case NV_BGRA: {   // nitropng (palette, 16-bit): straight alpha; nitrotiff / nitrodng: premultiplied
        const bool premul = ni.mode != NV_MODE_NITROPNG;
        std::atomic<bool> any{false};
        ParallelFor((size_t)ni.height, [&](size_t y) {
            const uint8_t *s = buf + y * ni.pitch[0];
            uint8_t *d = im->px + y * im->pitch;
            bool a = false;
            for (int x = 0; x < ni.width; x++, s += 4, d += 4) {
                int A = s[3];
                if (A == 255) { memcpy(d, s, 4); continue; }
                a = true;
                if (premul) {
                    d[0] = OverPremul(s[0], A, bB); d[1] = OverPremul(s[1], A, bG); d[2] = OverPremul(s[2], A, bR);
                } else {
                    d[0] = Over(s[0], A, bB); d[1] = Over(s[1], A, bG); d[2] = Over(s[2], A, bR);
                }
                d[3] = 255;
            }
            if (a) any = true;
        });
        alpha = any;
        break;
    }
    case NV_PNG: {   // 8-bit rows of gray / gray+alpha / RGB / RGBA after a filter-type byte
        const int nc = ni.ncomp;
        const bool hasA = nc == 2 || nc == 4;
        alpha = hasA;
        ParallelFor((size_t)ni.height, [&](size_t y) {
            const uint8_t *s = buf + y * ni.pitch[0] + 1;
            uint8_t *d = im->px + y * im->pitch;
            for (int x = 0; x < ni.width; x++, s += nc, d += 4) {
                int r = s[0], g = nc >= 3 ? s[1] : r, b = nc >= 3 ? s[2] : r, A = hasA ? s[nc - 1] : 255;
                if (A == 255) { d[0] = (uint8_t)b; d[1] = (uint8_t)g; d[2] = (uint8_t)r; }
                else { d[0] = Over(b, A, bB); d[1] = Over(g, A, bG); d[2] = Over(r, A, bR); }
                d[3] = 255;
            }
        });
        break;
    }
    }
    free(buf);
    im->alpha = alpha;
    const char *mode = nv_mode_name(ni.mode);
    if (ni.mode >= 0) im->decoder = L"nitrojpeg (" + Widen(mode) + L")";
    else im->decoder = Widen(mode ? mode : "nitro");
    return 0;
}

// ---------------------------------------------------------------------------
// WIC

static bool DecodeWic(const uint8_t *data, size_t len, uint32_t bg, Image *im, int *orientation, std::wstring *error) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory *f = nullptr;
    IWICStream *st = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICBitmapSource *conv = nullptr;
    IWICMetadataQueryReader *meta = nullptr;
    UINT w = 0, h = 0;
    GUID container = {};
    if (len > 0xFFFFFFFFu ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) ||
        FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE *)data, (DWORD)len)) ||
        FAILED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 65535 || h > 65535) {
        if (error->empty()) *error = L"Unknown or damaged image";
        goto done;
    }
    if (SUCCEEDED(frame->GetMetadataQueryReader(&meta))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(meta->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2 && v.uiVal >= 1 &&
            v.uiVal <= 8)
            *orientation = v.uiVal;
        PropVariantClear(&v);
    }
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, frame, &conv)) || !im->Alloc((int)w, (int)h) ||
        FAILED(conv->CopyPixels(nullptr, (UINT)im->pitch, (UINT)(im->pitch * h), im->px))) {
        *error = L"Cannot decode image";
        goto done;
    }
    {
        const int bR = bg >> 16 & 255, bG = bg >> 8 & 255, bB = bg & 255;
        std::atomic<bool> any{false};
        ParallelFor((size_t)h, [&](size_t y) {
            uint8_t *d = im->px + y * im->pitch;
            bool a = false;
            for (UINT x = 0; x < w; x++, d += 4) {
                int A = d[3];
                if (A == 255) continue;
                a = true;
                d[0] = OverPremul(d[0], A, bB); d[1] = OverPremul(d[1], A, bG); d[2] = OverPremul(d[2], A, bR);
                d[3] = 255;
            }
            if (a) any = true;
        });
        im->alpha = any;
    }
    im->decoder = L"WIC";
    if (SUCCEEDED(dec->GetContainerFormat(&container))) {
        static const struct { const GUID *g; const wchar_t *n; } names[] = {
            {&GUID_ContainerFormatJpeg, L"WIC jpeg"}, {&GUID_ContainerFormatPng, L"WIC png"},
            {&GUID_ContainerFormatGif, L"WIC gif"},   {&GUID_ContainerFormatBmp, L"WIC bmp"},
            {&GUID_ContainerFormatTiff, L"WIC tiff"}, {&GUID_ContainerFormatIco, L"WIC ico"},
            {&GUID_ContainerFormatWmp, L"WIC jxr"},   {&GUID_ContainerFormatDds, L"WIC dds"}};
        for (auto &n : names)
            if (IsEqualGUID(container, *n.g)) im->decoder = n.n;
    }
    ok = true;
done:
    if (meta) meta->Release();
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    if (f) f->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

// ---------------------------------------------------------------------------

ImagePtr DecodeImage(const std::wstring &path, uint32_t background, std::wstring *error) {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    std::string u8 = Utf8(path);
    size_t len = 0;
    int rawOrientation = 0, rawRead = 0;
    uint8_t *data = nv_read_image(u8.c_str(), &len, INT_MAX, &rawOrientation, &rawRead);
    if (!data) {
        *error = L"Cannot read the file";
        return nullptr;
    }
    auto im = std::make_shared<Image>();
    int orientation = 1;
    std::wstring err;
    int rc = DecodeNitro(data, len, rawOrientation, rawRead, background, im.get(), &orientation, &err);
    if (rc == 1) {
        // the platform's turn (a camera raw: its embedded JPEG, which nv_decode moved to the start)
        int o = orientation;
        if (DecodeWic(data, len, background, im.get(), &o, &err)) {
            rc = 0;
            if (orientation == 1) orientation = o;
        }
    }
    free(data);
    if (rc) {
        *error = err.empty() ? L"Cannot decode image" : err;
        return nullptr;
    }
    im->orientation = orientation;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        im->fileSize = (unsigned long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    im->ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    return im;
}
