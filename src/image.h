// FarNitroView for Far Manager: a decoded image and the decoder front end.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// 8-bit BGRA pixels, rows `pitch` bytes apart, as the file stores them (the EXIF orientation is
// applied when drawing). Transparency is already composited over the background colour.
struct Image {
    int w = 0, h = 0;
    size_t pitch = 0;
    uint8_t *px = nullptr;
    int orientation = 1;        // EXIF 1..8
    bool alpha = false;         // the file had transparency
    std::wstring decoder;       // "nitrojpeg (rst)", "WIC" ...
    double ms = 0;              // decoding time
    unsigned long long fileSize = 0;
    ~Image() { if (px) _aligned_free(px); }
    bool Alloc(int width, int height);
};

typedef std::shared_ptr<const Image> ImagePtr;

// Decodes a file: our decoders (nitrojpeg, nitropng, nitropsd, nitrodng, nitrowebp, nitrotiff,
// nitrohevc for HEIC, a camera raw's embedded JPEG), else Windows Imaging Component.
// NULL and *error set when nothing can decode it.
ImagePtr DecodeImage(const std::wstring &path, uint32_t background, std::wstring *error);

// Is this a file name we try to show? (our formats + what WIC usually reads)
bool IsImageName(const wchar_t *path);
// ... or do its first bytes look like an image (for files without a known extension)?
bool IsImageSignature(const void *data, size_t len);
