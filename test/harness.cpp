// Test harness: decodes files as the plugin does, compares with a reference decoding (.rgb from
// mkimages.py: width, height, RGB), and writes the fitted rendering as a BMP for a look.
// Build (after build.sh), from the project folder:
//   ext/llvm-mingw-*/bin/x86_64-w64-mingw32-clang++ -O2 -std=c++20 -march=x86-64-v3 -municode
//     -Ithird_party/jpeg-nitro/src test/harness.cpp build/obj-x86-64-v3/{decode,render,pdf,par}.o build/obj-x86-64-v3/nitro*.o -static
//     -lwindowscodecs -lole32 -loleaut32 -lshlwapi -luuid -lpthread -o build/harness.exe
// Run in test/images: ../../build/harness.exe *.jpg *.png ...
#include "../src/image.h"
#include "../src/pdf.h"
#include "../src/render.h"

#include <cmath>
#include <cstdio>
#include <vector>

static void WriteBmp(const wchar_t *path, const uint8_t *bgra, int w, int h) {
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    BITMAPFILEHEADER fh = {0x4D42};
    BITMAPINFOHEADER ih = {sizeof ih, w, -h, 1, 32};
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + w * h * 4;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(bgra, 4, (size_t)w * h, f);
    fclose(f);
}

// (a PDF page is rendered in the background: Draw again when it is there)
static void DrawDone(Renderer &r, const ImagePtr &im, const ViewParams &v, uint32_t bg, uint8_t *dst, size_t pitch, int W, int H) {
    r.Draw(im, v, bg, dst, pitch, W, H);
    r.Finish();
    r.Draw(im, v, bg, dst, pitch, W, H);
}

static bool Near(const uint8_t *bgra, int rgb) {
    return abs(bgra[2] - (rgb >> 16 & 255)) < 8 && abs(bgra[1] - (rgb >> 8 & 255)) < 8 && abs(bgra[0] - (rgb & 255)) < 8;
}

// A PDF's other views: rotated (must be the 100% rendering, turned), zoomed, panned, its other pages.
static int CheckPdf(const ImagePtr &im, const std::wstring &path, const std::vector<uint8_t> &full) {
    int fails = 0;
    auto check = [&](bool ok, const wchar_t *what) {
        if (ok) return;
        wprintf(L"  PDF %ls: FAILED\n", what);
        fails++;
    };
    Renderer r;
    const int w = im->w, h = im->h;
    auto at = [](const std::vector<uint8_t> &b, int W, int x, int y) { return &b[((size_t)y * W + x) * 4]; };
    // the page turned clockwise: a source pixel (x, y) is at (h - 1 - y, x)
    ViewParams v;
    v.orient.RotateCW();
    v.scale = 1;
    v.cx = h / 2.0;
    v.cy = w / 2.0;
    std::vector<uint8_t> turned((size_t)h * w * 4);
    DrawDone(r, im, v, 0, turned.data(), (size_t)h * 4, h, w);
    int bad = 0;
    for (int y = 2; y < h - 2; y++)
        for (int x = 2; x < w - 2; x++) {
            const uint8_t *a = at(full, w, x, y), *b = at(turned, h, h - 1 - y, x);
            if (abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > 48) bad++;
        }
    check(bad == 0, L"rotated");
    // 400%, the bottom left corner in the middle of the window: the red square is just up and to its right
    ViewParams z;
    z.scale = 4;
    z.cx = 0;
    z.cy = h;
    const int W = 640, H = 480;
    std::vector<uint8_t> win((size_t)W * H * 4);
    DrawDone(r, im, z, 0x102030, win.data(), (size_t)W * 4, W, H);
    check(Near(at(win, W, W / 2 + 80, H / 2 - 80), 0xd73a49), L"zoomed: the red square");
    check(Near(at(win, W, W / 2 - 80, H / 2 - 80), 0x102030), L"zoomed: the background left of the page");
    check(Near(at(win, W, W / 2 + 80, H / 2 + 80), 0x102030), L"zoomed: the background under the page");
    // panned a little: from what was rendered around the view, at once
    z.cx += 10;
    r.Draw(im, z, 0x102030, win.data(), (size_t)W * 4, W, H);
    check(Near(at(win, W, W / 2 + 40, H / 2 - 80), 0xd73a49), L"panned");
    // a change of zoom shows something (the last rendering stretched) before the new one is there
    z.scale = 2;
    z.cx = 0;
    r.Draw(im, z, 0x102030, win.data(), (size_t)W * 4, W, H);
    r.Finish();
    r.Draw(im, z, 0x102030, win.data(), (size_t)W * 4, W, H);
    check(Near(at(win, W, W / 2 + 40, H / 2 - 40), 0xd73a49), L"zoomed out");
    // the second page has /Rotate 90: wider than tall turns into taller than wide, the yellow still in the middle
    std::wstring e;
    ImagePtr p2 = DecodeImage(path, 0, &e, 1);
    check(p2 && p2->w == h && p2->h == w && p2->page == 1 && p2->pages == 3, L"/Rotate");
    if (p2) {
        ViewParams c;
        c.cx = p2->w / 2.0;
        c.cy = p2->h / 2.0;
        std::vector<uint8_t> b((size_t)p2->w * p2->h * 4);
        DrawDone(r, p2, c, 0, b.data(), (size_t)p2->w * 4, p2->w, p2->h);
        check(Near(at(b, p2->w, p2->w / 2, p2->h / 2), 0xffd33d), L"/Rotate: the middle");
    }
    return fails;
}

int wmain(int argc, wchar_t **argv) {
    int fails = 0;
    for (int a = 1; a < argc; a++) {
        std::wstring err;
        ImagePtr im = DecodeImage(argv[a], 0, &err);
        if (!im) {
            wprintf(L"%-40ls FAILED: %ls\n", argv[a], err.c_str());
            fails++;
            continue;
        }
        Renderer r;
        ViewParams v;
        v.orient = Orient::FromExif(im->orientation);
        int ow, oh;
        OrientedSize(*im, v.orient, &ow, &oh);
        // 100%: the oriented image exactly
        v.scale = 1;
        v.cx = ow / 2.0;
        v.cy = oh / 2.0;
        std::vector<uint8_t> full((size_t)ow * oh * 4);
        DrawDone(r, im, v, 0, full.data(), (size_t)ow * 4, ow, oh);
        // compare with the reference
        std::wstring ref = std::wstring(argv[a]) + L".rgb";
        FILE *f = _wfopen(ref.c_str(), L"rb");
        double psnr = -1;
        int maxd = -1;
        if (f) {
            int rw = 0, rh = 0;
            fread(&rw, 4, 1, f);
            fread(&rh, 4, 1, f);
            if (rw == ow && rh == oh) {
                std::vector<uint8_t> rgb((size_t)rw * rh * 3);
                fread(rgb.data(), 1, rgb.size(), f);
                double se = 0;
                maxd = 0;
                for (size_t i = 0; i < (size_t)rw * rh; i++)
                    for (int c = 0; c < 3; c++) {
                        int d = (int)full[i * 4 + 2 - c] - rgb[i * 3 + c];
                        se += d * d;
                        if (abs(d) > maxd) maxd = abs(d);
                    }
                double mse = se / ((double)rw * rh * 3);
                psnr = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99;
            } else {
                wprintf(L"  size mismatch: ref %dx%d, ours (oriented) %dx%d\n", rw, rh, ow, oh);
                fails++;
            }
            fclose(f);
        }
        // fitted into 800 x 600
        const int W = 800, H = 600;
        v.scale = std::min(std::min((double)W / ow, (double)H / oh), 1.0);
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceFrequency(&fq);
        std::vector<uint8_t> fit((size_t)W * H * 4);
        QueryPerformanceCounter(&t0);
        DrawDone(r, im, v, 0x303030, fit.data(), (size_t)W * 4, W, H);
        QueryPerformanceCounter(&t1);
        double rms = (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
        std::wstring out = std::wstring(argv[a]) + L".fit.bmp";
        WriteBmp(out.c_str(), fit.data(), W, H);
        // 300% zoom, bilinear, of the middle
        v.scale = 3;
        DrawDone(r, im, v, 0x303030, fit.data(), (size_t)W * 4, W, H);
        WriteBmp((std::wstring(argv[a]) + L".zoom.bmp").c_str(), fit.data(), W, H);
        // a vector image: a pixel at 100% above the middle (the test SVG has its yellow disc there)
        if (im->svg || im->pdf) {
            bool ok = Near(&full[((size_t)(oh * 3 / 10) * ow + ow / 2) * 4], 0xffd33d);
            psnr = ok ? 99 : 0;
            maxd = ok ? 0 : 255;
        }
        if (im->pdf) fails += CheckPdf(im, argv[a], full);
        // another page / the last frame, and post-processing, must work too
        int extra = 0;
        if (im->pages > 1) {
            std::wstring e2;
            ImagePtr p2 = DecodeImage(argv[a], 0, &e2, im->pages - 1);
            if (!p2 || p2->page != im->pages - 1) {
                wprintf(L"  page %d: FAILED %ls\n", im->pages, e2.c_str());
                fails++;
            } else
                extra = p2->w;
        }
        if (im->FrameCount() > 1) {
            v.frame = im->FrameCount() - 1;
            v.scale = 1;
            r.Draw(im, v, 0, full.data(), (size_t)ow * 4, ow, oh);
        }
        ImagePtr pp = PostProcess(im, PP_CHROMA | PP_AUTOLUMA | PP_AUTOLEVELS, 1.5);
        if (!pp || pp->w != im->w || pp->FrameCount() != im->FrameCount()) {
            wprintf(L"  post-processing FAILED\n");
            fails++;
        }
        wprintf(L"%-40ls %-22ls %5dx%-5d o%d %hs %6.1f ms  draw-fit %5.1f ms  frames %d pages %d(%d)  PSNR %5.1f max %d\n",
                argv[a], im->decoder.c_str(), im->w, im->h, im->orientation, im->alpha ? "A" : "-", im->ms, rms,
                im->FrameCount(), im->pages, extra, psnr, maxd);
    }
    return fails;
}
