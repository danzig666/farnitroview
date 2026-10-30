// FarNitroView for Far Manager: a decoded image and the decoder front end.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct nitrosvg;
struct PdfPage;

// 8-bit BGRA pixels, rows `pitch` bytes apart, as the file stores them (the EXIF orientation is
// applied when drawing). Transparency is already composited over the background colour.
// An animation has more frames of the same size (frame 0 is px); a vector image (SVG) or a PDF
// page has no pixels: it is rendered at whatever scale it is shown.
struct Image {
    int w = 0, h = 0;
    size_t pitch = 0;
    uint8_t *px = nullptr;
    std::vector<uint8_t *> frames;   // animation: frames 1 .. n-1 (each w x h, pitch)
    std::vector<int> delays;         // animation: every frame's delay in ms (empty: a still image)
    int page = 0, pages = 1;         // multi-page files: which page this is, how many there are
    std::shared_ptr<nitrosvg> svg;   // a vector image (w, h: its size rounded up)
    double svgW = 0, svgH = 0;       // ... its exact size
    std::shared_ptr<PdfPage> pdf;    // a PDF page (w, h: its size at 96 dpi, which is what 100% shows)
    int orientation = 1;             // EXIF 1..8
    bool alpha = false;              // the file had transparency
    std::wstring decoder;            // "nitrojpeg (rst)", "WIC" ...
    double ms = 0;                   // decoding time
    unsigned long long fileSize = 0;
    Image() = default;
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;
    ~Image();
    bool Alloc(int width, int height);
    uint8_t *NewFrame();                       // one more animation frame (uninitialised)
    int FrameCount() const { return delays.empty() ? 1 : (int)delays.size(); }
    const uint8_t *Frame(int i) const { return i <= 0 || i > (int)frames.size() ? px : frames[i - 1]; }
};

typedef std::shared_ptr<const Image> ImagePtr;

// Decodes a file: our decoders (nitrojpeg, nitropng, nitropsd, nitrodng, nitrowebp, nitrotiff,
// nitrohevc for HEIC, nitroavif for AVIF, a camera raw's embedded JPEG; lunasvg for SVG; PDFium for
// PDF), else Windows Imaging Component (also the pages of multi-page files and animated GIFs).
// page: of a multi-page file. NULL and *error set when nothing can decode it.
ImagePtr DecodeImage(const std::wstring &path, uint32_t background, std::wstring *error, int page = 0);

// Post-processing, as PictureView's F6 / F7 / F8: a new image (all frames), or the same when off.
enum { PP_CHROMA = 1, PP_AUTOLUMA = 2, PP_AUTOLEVELS = 4 };
ImagePtr PostProcess(const ImagePtr &im, int flags, double chroma);

// Is this a file name we try to show? (our formats + what WIC usually reads)
bool IsImageName(const wchar_t *path);
// ... or do its first bytes look like an image (for files without a known extension)?
bool IsImageSignature(const void *data, size_t len);
// Is this a PDF, by its name or its first bytes (whether or not it can be shown)?
bool IsPdf(const wchar_t *path, const void *data, size_t len);
// Which optional decoders are built in ("SVG", "AVIF"); PDF: is pdfium.dll there (and not switched off)
bool HaveSvg();
bool HaveAvif();
bool HavePdf();
void SetPdfEnabled(bool on);
