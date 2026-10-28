// FarNitroView for Far Manager: post-processing, as PictureView's F6 (chroma), F7 (autoluma) and
// F8 (autolevels). The levels come from the first frame and apply to every frame alike.
#include "image.h"
#include "par.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// the value below which `frac` of the histogram lies
int Percentile(const uint64_t *hist, uint64_t total, double frac) {
    uint64_t want = (uint64_t)(total * frac), sum = 0;
    for (int i = 0; i < 256; i++) {
        sum += hist[i];
        if (sum > want) return i;
    }
    return 255;
}

// a linear stretch lo..hi -> 0..255
void Stretch(uint8_t *lut, int lo, int hi) {
    if (hi - lo < 8) {   // (a flat image: leave it)
        for (int i = 0; i < 256; i++) lut[i] = (uint8_t)i;
        return;
    }
    for (int i = 0; i < 256; i++) lut[i] = (uint8_t)std::clamp((i - lo) * 255 / (hi - lo), 0, 255);
}

}   // namespace

ImagePtr PostProcess(const ImagePtr &src, int flags, double chroma) {
    if (!src || !flags || src->svg || !src->px) return src;
    const Image &s = *src;
    auto out = std::make_shared<Image>();
    if (!out->Alloc(s.w, s.h)) return src;
    out->delays = s.delays;
    out->page = s.page;
    out->pages = s.pages;
    out->orientation = s.orientation;
    out->alpha = s.alpha;
    out->decoder = s.decoder;
    out->ms = s.ms;
    out->fileSize = s.fileSize;
    for (size_t i = 0; i < s.frames.size(); i++)
        if (!out->NewFrame()) return src;

    // histograms of the first frame (every 4th row: plenty for percentiles)
    uint64_t hist[4][256] = {};   // B G R, luma
    for (int y = 0; y < s.h; y += 4) {
        const uint8_t *p = s.px + (size_t)y * s.pitch;
        for (int x = 0; x < s.w; x++, p += 4) {
            hist[0][p[0]]++;
            hist[1][p[1]]++;
            hist[2][p[2]]++;
            hist[3][(p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8]++;
        }
    }
    uint64_t total = 0;
    for (int i = 0; i < 256; i++) total += hist[3][i];
    uint8_t lut[3][256];
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) lut[c][i] = (uint8_t)i;
    if (flags & PP_AUTOLEVELS)   // each channel stretched on its own (also corrects a colour cast)
        for (int c = 0; c < 3; c++) Stretch(lut[c], Percentile(hist[c], total, 0.005), Percentile(hist[c], total, 0.995));
    if (flags & PP_AUTOLUMA) {   // the brightness stretched, the same for all channels (keeps the hues),
        uint8_t l[256];          // then a gamma that brings the median towards the middle
        Stretch(l, Percentile(hist[3], total, 0.005), Percentile(hist[3], total, 0.995));
        int med = l[Percentile(hist[3], total, 0.5)];
        double g = med > 8 && med < 247 ? std::clamp(std::log(0.5) / std::log(med / 255.0), 0.6, 1.6) : 1.0;
        uint8_t gl[256];
        for (int i = 0; i < 256; i++) gl[i] = (uint8_t)std::lround(255.0 * std::pow(l[i] / 255.0, g));
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < 256; i++) lut[c][i] = gl[lut[c][i]];
    }
    const bool doChroma = (flags & PP_CHROMA) && chroma > 0 && chroma != 1;
    const int k = (int)std::lround(chroma * 256);

    for (int f = 0; f < s.FrameCount(); f++) {
        const uint8_t *in = s.Frame(f);
        uint8_t *o = f == 0 ? out->px : out->frames[f - 1];
        ParallelFor((size_t)s.h, [&](size_t y) {
            const uint8_t *p = in + y * s.pitch;
            uint8_t *d = o + y * s.pitch;
            for (int x = 0; x < s.w; x++, p += 4, d += 4) {
                int b = lut[0][p[0]], g = lut[1][p[1]], r = lut[2][p[2]];
                if (doChroma) {   // saturation: away from (or towards) the pixel's own gray
                    int Y = (r * 77 + g * 150 + b * 29) >> 8;
                    b = std::clamp(Y + ((b - Y) * k >> 8), 0, 255);
                    g = std::clamp(Y + ((g - Y) * k >> 8), 0, 255);
                    r = std::clamp(Y + ((r - Y) * k >> 8), 0, 255);
                }
                d[0] = (uint8_t)b;
                d[1] = (uint8_t)g;
                d[2] = (uint8_t)r;
                d[3] = 255;
            }
        });
    }
    return out;
}
