// Test harness: decodes files as the plugin does, compares with a reference decoding (.rgb from
// mkimages.py: width, height, RGB), and writes the fitted rendering as a BMP for a look.
// Build (after build.sh), from the project folder:
//   ext/llvm-mingw-*/bin/x86_64-w64-mingw32-clang++ -O2 -std=c++20 -march=x86-64-v3 -municode
//     -Ithird_party/jpeg-nitro/src test/harness.cpp build/obj-x86-64-v3/{decode,render,par}.o build/obj-x86-64-v3/nitro*.o -static
//     -lwindowscodecs -lole32 -loleaut32 -lshlwapi -luuid -lpthread -o build/harness.exe
// Run in test/images: ../../build/harness.exe *.jpg *.png ...
#include "../src/image.h"
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
        r.Draw(im, v, 0, full.data(), (size_t)ow * 4, ow, oh);
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
        r.Draw(im, v, 0x303030, fit.data(), (size_t)W * 4, W, H);
        QueryPerformanceCounter(&t1);
        double rms = (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
        std::wstring out = std::wstring(argv[a]) + L".fit.bmp";
        WriteBmp(out.c_str(), fit.data(), W, H);
        // 300% zoom, bilinear, of the middle
        v.scale = 3;
        r.Draw(im, v, 0x303030, fit.data(), (size_t)W * 4, W, H);
        WriteBmp((std::wstring(argv[a]) + L".zoom.bmp").c_str(), fit.data(), W, H);
        wprintf(L"%-40ls %-22ls %5dx%-5d o%d %hs %6.1f ms  draw-fit %5.1f ms  PSNR %5.1f max %d\n", argv[a],
                im->decoder.c_str(), im->w, im->h, im->orientation, im->alpha ? "A" : "-", im->ms, rms, psnr, maxd);
    }
    return fails;
}
