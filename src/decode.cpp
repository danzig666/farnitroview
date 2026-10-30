// FarNitroView for Far Manager: file -> Image, with jpeg-nitro's decoders and WIC as the fallback.
#include "image.h"
#include "par.h"
#include "pdf.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <shlwapi.h>
#include <wincodec.h>

extern "C" {
#include "nitro_convert.h"
#include "nitro_load.h"
#ifdef NV_SVG
#include "nitrosvg.h"
#endif
}

bool HaveSvg() {
#ifdef NV_SVG
    return true;
#else
    return false;
#endif
}
bool HaveAvif() {
#ifdef NV_DAV1D
    return true;
#else
    return false;
#endif
}

Image::~Image() {
    if (px) _aligned_free(px);
    for (uint8_t *f : frames) _aligned_free(f);
}

bool Image::Alloc(int width, int height) {
    w = width;
    h = height;
    pitch = (size_t)w * 4;
    px = (uint8_t *)_aligned_malloc(pitch * (size_t)h + 64, 64);
    return px != nullptr;
}

uint8_t *Image::NewFrame() {
    uint8_t *f = (uint8_t *)_aligned_malloc(pitch * (size_t)h + 64, 64);
    if (f) frames.push_back(f);
    return f;
}

// Extensions beyond nv_image_exts that WIC reads on a stock Windows (or with the usual codecs)
static const wchar_t *const kWicExts[] = {L"ico", L"cur", L"jxr", L"wdp", L"hdp", L"dds", L"dib", L"jxl", nullptr};

static bool HasExt(const wchar_t *path, const wchar_t *e) {
    const wchar_t *ext = PathFindExtensionW(path);
    return *ext && !_wcsicmp(ext + 1, e);
}

bool IsImageName(const wchar_t *path) {
    if (nv_is_image_name_w(path)) return true;
    if (HaveSvg() && HasExt(path, L"svg")) return true;
    if (HasExt(path, L"pdf") && HavePdf()) return true;
    for (const wchar_t *const *e = kWicExts; *e; e++)
        if (HasExt(path, *e)) return true;
    return false;
}

bool IsPdf(const wchar_t *path, const void *data, size_t len) {
    return HasExt(path, L"pdf") || (data && len >= 5 && !memcmp(data, "%PDF-", 5));
}

bool IsImageSignature(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    if (HaveSvg() && len >= 5) {   // SVG: "<svg" near the start (after an XML declaration, comments ...)
        size_t n = len < 1024 ? len : 1024;
        for (size_t i = 0; i + 4 <= n; i++)
            if (!memcmp(p + i, "<svg", 4)) return true;
    }
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
// WIC: any page of multi-page files; animated GIFs composited frame by frame

namespace {
template <class T> struct Com {   // releases on scope exit
    T *p = nullptr;
    ~Com() { if (p) p->Release(); }
    T **operator&() { return &p; }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

int MetaInt(IWICMetadataQueryReader *r, const wchar_t *name, int def) {
    if (!r) return def;
    PROPVARIANT v;
    PropVariantInit(&v);
    int out = def;
    if (SUCCEEDED(r->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1) out = v.bVal;
        else if (v.vt == VT_UI2) out = v.uiVal;
        else if (v.vt == VT_UI4) out = (int)v.ulVal;
        else if (v.vt == VT_BOOL) out = v.boolVal ? 1 : 0;
    }
    PropVariantClear(&v);
    return out;
}

// a frame as premultiplied BGRA, w x h, rows of 4w bytes
bool FramePixels(IWICBitmapFrameDecode *frame, UINT w, UINT h, uint8_t *dst, size_t pitch) {
    Com<IWICBitmapSource> conv;
    return SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, frame, &conv)) &&
           SUCCEEDED(conv->CopyPixels(nullptr, (UINT)pitch, (UINT)(pitch * h), dst));
}

// premultiplied pixels over the background, opaque; returns whether any were transparent
bool Flatten(const uint8_t *src, uint8_t *dst, size_t pitch, int w, int h, uint32_t bg) {
    const int bR = bg >> 16 & 255, bG = bg >> 8 & 255, bB = bg & 255;
    std::atomic<bool> any{false};
    ParallelFor((size_t)h, [&](size_t y) {
        const uint8_t *s = src + y * pitch;
        uint8_t *d = dst + y * pitch;
        bool a = false;
        for (int x = 0; x < w; x++, s += 4, d += 4) {
            int A = s[3];
            if (A == 255) { memcpy(d, s, 4); continue; }
            a = true;
            d[0] = OverPremul(s[0], A, bB); d[1] = OverPremul(s[1], A, bG); d[2] = OverPremul(s[2], A, bR);
            d[3] = 255;
        }
        if (a) any = true;
    });
    return any;
}

// GIF: the frames composited on the logical screen (offsets, transparency, disposal)
bool DecodeGifAnimation(IWICBitmapDecoder *dec, UINT count, uint32_t bg, Image *im) {
    Com<IWICMetadataQueryReader> gm;
    dec->GetMetadataQueryReader(&gm);
    int W = MetaInt(gm.p, L"/logscrdesc/Width", 0), H = MetaInt(gm.p, L"/logscrdesc/Height", 0);
    if (W <= 0 || H <= 0) {
        Com<IWICBitmapFrameDecode> f0;
        UINT w0, h0;
        if (FAILED(dec->GetFrame(0, &f0)) || FAILED(f0->GetSize(&w0, &h0))) return false;
        W = (int)w0;
        H = (int)h0;
    }
    if (W > 16384 || H > 16384 || !im->Alloc(W, H)) return false;
    const size_t P = (size_t)W * 4, bytes = P * H, cap = (size_t)768 << 20;   // all frames: at most 768 MB
    std::vector<uint8_t> canvas(bytes, 0), saved, fr;
    bool anyAlpha = false;
    for (UINT i = 0; i < count; i++) {
        if (i && (size_t)(i + 1) * bytes > cap) break;
        Com<IWICBitmapFrameDecode> frame;
        Com<IWICMetadataQueryReader> m;
        UINT fw = 0, fh = 0;
        if (FAILED(dec->GetFrame(i, &frame)) || FAILED(frame->GetSize(&fw, &fh)) || !fw || !fh) break;
        frame->GetMetadataQueryReader(&m);
        const int left = MetaInt(m.p, L"/imgdesc/Left", 0), top = MetaInt(m.p, L"/imgdesc/Top", 0);
        const int disposal = MetaInt(m.p, L"/grctlext/Disposal", 0);
        int delay = MetaInt(m.p, L"/grctlext/Delay", 10) * 10;
        if (delay < 20) delay = 100;   // (as browsers do)
        if (disposal == 3) saved = canvas;
        fr.resize((size_t)fw * fh * 4);
        if (!FramePixels(frame.p, fw, fh, fr.data(), (size_t)fw * 4)) break;
        // the frame over the canvas (premultiplied "over"), clipped to it
        for (int y = 0; y < (int)fh; y++) {
            int cy = top + y;
            if (cy < 0 || cy >= H) continue;
            const uint8_t *s = &fr[(size_t)y * fw * 4];
            uint8_t *d = &canvas[(size_t)cy * P];
            for (int x = 0; x < (int)fw; x++, s += 4) {
                int cx = left + x;
                if (cx < 0 || cx >= W) continue;
                uint8_t *q = d + (size_t)cx * 4;
                int a = s[3];
                if (a == 255) memcpy(q, s, 4);
                else if (a)
                    for (int c = 0; c < 4; c++) q[c] = (uint8_t)(s[c] + (q[c] * (255 - a) + 127) / 255);
            }
        }
        uint8_t *out = i == 0 ? im->px : im->NewFrame();
        if (!out) break;
        anyAlpha |= Flatten(canvas.data(), out, P, W, H, bg);
        im->delays.push_back(delay);
        // what the next frame starts from
        if (disposal == 2) {
            for (int y = std::max(0, top); y < std::min(H, top + (int)fh); y++)
                memset(&canvas[(size_t)y * P + (size_t)std::max(0, left) * 4], 0,
                       (size_t)(std::min(W, left + (int)fw) - std::max(0, left)) * 4);
        } else if (disposal == 3 && !saved.empty()) {
            canvas.swap(saved);
        }
    }
    if (im->delays.empty()) return false;
    if (im->delays.size() == 1) im->delays.clear();   // a still image after all
    im->alpha = anyAlpha;
    return true;
}
}   // namespace

// Decodes page `page` (an animated GIF: all its frames). *pages: how many the file has.
static bool DecodeWic(const uint8_t *data, size_t len, uint32_t bg, Image *im, int *orientation, std::wstring *error,
                      int page) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        Com<IWICImagingFactory> f;
        Com<IWICStream> st;
        Com<IWICBitmapDecoder> dec;
        Com<IWICBitmapFrameDecode> frame;
        Com<IWICMetadataQueryReader> meta;
        UINT w = 0, h = 0, count = 1;
        GUID container = {};
        if (len > 0xFFFFFFFFu ||
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f.p))) ||
            FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE *)data, (DWORD)len)) ||
            FAILED(f->CreateDecoderFromStream(st.p, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
            if (error->empty()) *error = L"Unknown or damaged image";
            goto done;
        }
        dec->GetContainerFormat(&container);
        if (FAILED(dec->GetFrameCount(&count)) || !count) count = 1;
        const bool gif = IsEqualGUID(container, GUID_ContainerFormatGif);
        if (gif && count > 1) {   // an animation, not pages
            if (!DecodeGifAnimation(dec.p, count, bg, im)) {
                *error = L"Cannot decode image";
                goto done;
            }
        } else {
            if (page < 0 || page >= (int)count) page = 0;
            if (FAILED(dec->GetFrame((UINT)page, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 65535 ||
                h > 65535) {
                if (error->empty()) *error = L"Unknown or damaged image";
                goto done;
            }
            if (SUCCEEDED(frame->GetMetadataQueryReader(&meta))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(meta->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2 &&
                    v.uiVal >= 1 && v.uiVal <= 8)
                    *orientation = v.uiVal;
                PropVariantClear(&v);
            }
            if (!im->Alloc((int)w, (int)h) || !FramePixels(frame.p, w, h, im->px, im->pitch)) {
                *error = L"Cannot decode image";
                goto done;
            }
            im->alpha = Flatten(im->px, im->px, im->pitch, (int)w, (int)h, bg);
            im->page = page;
            im->pages = (int)count;
        }
        im->decoder = L"WIC";
        static const struct { const GUID *g; const wchar_t *n; } names[] = {
            {&GUID_ContainerFormatJpeg, L"WIC jpeg"}, {&GUID_ContainerFormatPng, L"WIC png"},
            {&GUID_ContainerFormatGif, L"WIC gif"},   {&GUID_ContainerFormatBmp, L"WIC bmp"},
            {&GUID_ContainerFormatTiff, L"WIC tiff"}, {&GUID_ContainerFormatIco, L"WIC ico"},
            {&GUID_ContainerFormatWmp, L"WIC jxr"},   {&GUID_ContainerFormatDds, L"WIC dds"}};
        for (auto &n : names)
            if (IsEqualGUID(container, *n.g)) im->decoder = n.n;
        ok = true;
    }
done:
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

// How many pages WIC sees in a file (a TIFF nitrotiff decoded: its first page only)
static int WicPageCount(const uint8_t *data, size_t len) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    UINT count = 1;
    {
        Com<IWICImagingFactory> f;
        Com<IWICStream> st;
        Com<IWICBitmapDecoder> dec;
        if (len <= 0xFFFFFFFFu &&
            SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f.p))) &&
            SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE *)data, (DWORD)len)) &&
            SUCCEEDED(f->CreateDecoderFromStream(st.p, nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
            if (FAILED(dec->GetFrameCount(&count)) || !count) count = 1;
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return (int)count;
}

// ---------------------------------------------------------------------------
// SVG

#ifdef NV_SVG
static bool DecodeSvg(const uint8_t *data, size_t len, Image *im, std::wstring *error) {
    nitrosvg *s = nitrosvg_load(data, len);
    if (!s) {
        *error = L"Not an SVG lunasvg can draw";
        return false;
    }
    im->svg.reset(s, nitrosvg_free);
    nitrosvg_size(s, &im->svgW, &im->svgH);
    im->w = std::max(1, (int)std::ceil(im->svgW));
    im->h = std::max(1, (int)std::ceil(im->svgH));
    im->decoder = L"lunasvg";
    return true;
}
#endif

// ---------------------------------------------------------------------------
// PDF: only the page's size here, it is rendered when it is shown (and PDFium reads the file itself)

static bool DecodePdf(const std::wstring &path, int page, Image *im, std::wstring *error) {
    double pw = 0, ph = 0;
    int pages = 1;
    im->pdf = PdfOpenPage(path, page, &pages, &pw, &ph, error);
    if (!im->pdf) return false;
    im->w = std::max(1, (int)std::lround(pw * 96 / 72));
    im->h = std::max(1, (int)std::lround(ph * 96 / 72));
    im->page = im->pdf->index;
    im->pages = pages;
    im->decoder = L"PDFium";
    return true;
}

// ---------------------------------------------------------------------------

// The file read into memory and given to the decoders. 0: decoded.
static int DecodeFile(const std::wstring &path, uint32_t background, int page, Image *im, int *orientationOut,
                      std::wstring *errOut) {
    std::wstring &err = *errOut;
    int &orientation = *orientationOut;
    std::string u8 = Utf8(path);
    size_t len = 0;
    int rawOrientation = 0, rawRead = 0;
    uint8_t *data = nv_read_image(u8.c_str(), &len, INT_MAX, &rawOrientation, &rawRead);
    if (!data) {
        err = L"Cannot read the file";
        return -1;
    }
    if (IsPdf(path.c_str(), data, len)) {   // (under another name)
        free(data);
        return DecodePdf(path, page, im, &err) ? 0 : -1;
    }
    int rc = 1;
#ifdef NV_SVG
    if (HasExt(path.c_str(), L"svg") || (!IsImageName(path.c_str()) && IsImageSignature(data, len) &&
                                         len >= 5 && memchr(data, '<', len < 64 ? len : 64))) {
        rc = DecodeSvg(data, len, im, &err) ? 0 : -1;
        if (rc && !HasExt(path.c_str(), L"svg")) rc = 1;   // (not SVG after all: the others' turn)
    }
#endif
    if (rc == 1 && page == 0) {
        size_t n = len;
        rc = DecodeNitro(data, n, rawOrientation, rawRead, background, im, &orientation, &err);
        if (rc == 0 && !rawRead && (HasExt(path.c_str(), L"tif") || HasExt(path.c_str(), L"tiff")))
            im->pages = WicPageCount(data, len);   // (nitrotiff: the first page)
    }
    if (rc == 1 || (rc != 0 && page > 0)) {
        // the platform's turn (a camera raw: its embedded JPEG, which nv_decode moved to the start)
        int o = orientation;
        if (DecodeWic(data, len, background, im, &o, &err, page)) {
            rc = 0;
            if (orientation == 1) orientation = o;
        }
    }
    free(data);
    return rc;
}

ImagePtr DecodeImage(const std::wstring &path, uint32_t background, std::wstring *error, int page) {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    auto im = std::make_shared<Image>();
    int orientation = 1;
    std::wstring err;
    const int rc = HasExt(path.c_str(), L"pdf") ? (DecodePdf(path, page, im.get(), &err) ? 0 : -1)
                                                : DecodeFile(path, background, page, im.get(), &orientation, &err);
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
