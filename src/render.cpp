// FarNitroView for Far Manager: drawing an image into a BGRA buffer at any zoom and orientation.
//
// Reduced (scale < 1): the image is area-averaged (separable box filter with exact fractional
// coverage) to the scale once, oriented, and kept; panning only copies from it. Enlarged (or a
// reduced image too large to keep): each screen pixel is sampled from the image directly,
// bilinear at non-integer scales below 4, pixel-exact otherwise.
#include "render.h"
#include "par.h"

#include <algorithm>
#include <cmath>
#include <cstring>

Orient Orient::FromExif(int o) {
    static const int t[9][4] = {{1, 0, 0, 1},  {1, 0, 0, 1},  {-1, 0, 0, 1}, {-1, 0, 0, -1}, {1, 0, 0, -1},
                                {0, 1, 1, 0},  {0, 1, -1, 0}, {0, -1, -1, 0}, {0, -1, 1, 0}};
    Orient r;
    if (o < 1 || o > 8) o = 1;
    memcpy(r.m, t[o], sizeof r.m);
    return r;
}

void Orient::Then(const int u[4]) {
    int r[4] = {m[0] * u[0] + m[1] * u[2], m[0] * u[1] + m[1] * u[3], m[2] * u[0] + m[3] * u[2],
                m[2] * u[1] + m[3] * u[3]};
    memcpy(m, r, sizeof m);
}
void Orient::RotateCW() { static const int u[4] = {0, 1, -1, 0}; Then(u); }
void Orient::RotateCCW() { static const int u[4] = {0, -1, 1, 0}; Then(u); }
void Orient::FlipH() { static const int u[4] = {-1, 0, 0, 1}; Then(u); }
void Orient::FlipV() { static const int u[4] = {1, 0, 0, -1}; Then(u); }

// ---------------------------------------------------------------------------
// area averaging

namespace {
struct Taps {
    std::vector<int> start, count, off;   // per output sample: first source sample, how many, where its weights are
    std::vector<int> w;                   // weights, each output's sum is 1 << 14
};

Taps MakeTaps(int src, int dst) {
    Taps t;
    t.start.resize(dst);
    t.count.resize(dst);
    t.off.resize(dst);
    const double ratio = (double)src / dst;
    for (int j = 0; j < dst; j++) {
        double a = j * ratio, b = std::min((double)src, (j + 1) * ratio);
        int i0 = (int)a, i1 = std::min(src, (int)std::ceil(b));
        if (i1 <= i0) i1 = i0 + 1;
        t.start[j] = i0;
        t.count[j] = i1 - i0;
        t.off[j] = (int)t.w.size();
        int sum = 0;
        for (int i = i0; i < i1; i++) {
            double cov = std::min(b, (double)i + 1) - std::max(a, (double)i);
            int wt = (int)std::lround(std::max(0.0, cov) / (b - a) * 16384);
            t.w.push_back(wt);
            sum += wt;
        }
        // rounding remainder onto the largest weight
        int best = t.off[j];
        for (int k = t.off[j]; k < (int)t.w.size(); k++)
            if (t.w[k] > t.w[best]) best = k;
        t.w[best] += 16384 - sum;
    }
    return t;
}
}   // namespace

bool Renderer::BuildCache(const Image &im, const ViewParams &v) {
    // reduced size in the source frame (the scale is the same along both axes)
    const int nw = std::max(1, (int)std::lround(im.w * v.scale)), nh = std::max(1, (int)std::lround(im.h * v.scale));
    if ((double)nw * nh > 120e6) return false;
    std::vector<uint8_t> tmp((size_t)nw * im.h * 4), red((size_t)nw * nh * 4);
    Taps tx = MakeTaps(im.w, nw), ty = MakeTaps(im.h, nh);
    const Image *I = &im;
    // horizontal: every source row -> nw pixels
    ParallelFor((size_t)(im.h + 15) / 16, [&](size_t b) {
        for (int y = (int)b * 16; y < I->h && y < (int)b * 16 + 16; y++) {
            const uint8_t *s = I->px + (size_t)y * I->pitch;
            uint8_t *d = &tmp[(size_t)y * nw * 4];
            for (int j = 0; j < nw; j++, d += 4) {
                const uint8_t *p = s + (size_t)tx.start[j] * 4;
                const int *w = &tx.w[tx.off[j]];
                int b0 = 8192, g0 = 8192, r0 = 8192;
                for (int k = 0; k < tx.count[j]; k++, p += 4) {
                    b0 += p[0] * w[k];
                    g0 += p[1] * w[k];
                    r0 += p[2] * w[k];
                }
                d[0] = (uint8_t)(b0 >> 14);
                d[1] = (uint8_t)(g0 >> 14);
                d[2] = (uint8_t)(r0 >> 14);
                d[3] = 255;
            }
        }
    });
    // vertical: nh rows from the horizontally reduced ones
    ParallelFor((size_t)nh, [&](size_t r) {
        uint8_t *d = &red[r * nw * 4];
        const int *w = &ty.w[ty.off[r]];
        const int n = ty.count[r], y0 = ty.start[r];
        for (int x = 0; x < nw; x++) {
            int b0 = 8192, g0 = 8192, r0 = 8192;
            const uint8_t *p = &tmp[((size_t)y0 * nw + x) * 4];
            for (int k = 0; k < n; k++, p += (size_t)nw * 4) {
                b0 += p[0] * w[k];
                g0 += p[1] * w[k];
                r0 += p[2] * w[k];
            }
            d[4 * x] = (uint8_t)(b0 >> 14);
            d[4 * x + 1] = (uint8_t)(g0 >> 14);
            d[4 * x + 2] = (uint8_t)(r0 >> 14);
            d[4 * x + 3] = 255;
        }
    });
    tmp.clear();
    tmp.shrink_to_fit();
    // oriented
    const Orient o = v.orient;
    const int ow = o.Transposed() ? nh : nw, oh = o.Transposed() ? nw : nh;
    cache_.resize((size_t)ow * oh * 4);
    uint32_t *dst = (uint32_t *)cache_.data();
    const uint32_t *src = (const uint32_t *)red.data();
    ParallelFor((size_t)oh, [&](size_t y) {
        // centred coordinates of pixel centres, doubled to stay integral
        const int dy2 = 2 * (int)y + 1 - oh;
        uint32_t *d = dst + y * ow;
        for (int x = 0; x < ow; x++) {
            int dx2 = 2 * x + 1 - ow;
            int sx = (o.m[0] * dx2 + o.m[1] * dy2 + nw - 1) / 2, sy = (o.m[2] * dx2 + o.m[3] * dy2 + nh - 1) / 2;
            d[x] = src[(size_t)sy * nw + sx];
        }
    });
    cacheW_ = ow;
    cacheH_ = oh;
    return true;
}

// ---------------------------------------------------------------------------

void Renderer::DrawDirect(const Image &im, const ViewParams &v, uint8_t *dst, size_t pitch, int W, int H, int x0,
                          int y0, int x1, int y1) {
    int ow, oh;
    OrientedSize(im, v.orient, &ow, &oh);
    const double s = v.scale, inv = 1.0 / s;
    const int *m = v.orient.m;
    const bool integral = std::fabs(s - std::round(s)) < 1e-9;
    const bool bilinear = v.smooth && !integral && s < 4;
    const Image *I = &im;
    ParallelFor((size_t)(y1 - y0), [&](size_t row) {
        const int Y = y0 + (int)row;
        // centred display coordinates of the first pixel of the row, one step per screen pixel
        const double dy = v.cy + (Y + 0.5 - H / 2.0) * inv - oh / 2.0;
        const double dx0 = v.cx + (x0 + 0.5 - W / 2.0) * inv - ow / 2.0;
        double sx = I->w / 2.0 + m[0] * dx0 + m[1] * dy, sy = I->h / 2.0 + m[2] * dx0 + m[3] * dy;
        const double stx = m[0] * inv, sty = m[2] * inv;
        uint32_t *d = (uint32_t *)(dst + (size_t)Y * pitch) + x0;
        const uint8_t *px = I->px;
        const size_t P = I->pitch;
        const int w = I->w, h = I->h;
        if (!bilinear) {
            for (int X = x0; X < x1; X++, sx += stx, sy += sty) {
                int ix = std::clamp((int)std::floor(sx), 0, w - 1), iy = std::clamp((int)std::floor(sy), 0, h - 1);
                *d++ = *(const uint32_t *)(px + (size_t)iy * P + (size_t)ix * 4);
            }
            return;
        }
        for (int X = x0; X < x1; X++, sx += stx, sy += sty) {
            double fx = sx - 0.5, fy = sy - 0.5;
            int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
            int ax = (int)((fx - ix) * 256), ay = (int)((fy - iy) * 256);
            int xa = std::clamp(ix, 0, w - 1), xb = std::clamp(ix + 1, 0, w - 1);
            int ya = std::clamp(iy, 0, h - 1), yb = std::clamp(iy + 1, 0, h - 1);
            const uint8_t *p00 = px + (size_t)ya * P + (size_t)xa * 4, *p01 = px + (size_t)ya * P + (size_t)xb * 4;
            const uint8_t *p10 = px + (size_t)yb * P + (size_t)xa * 4, *p11 = px + (size_t)yb * P + (size_t)xb * 4;
            uint8_t *o = (uint8_t *)d++;
            for (int c = 0; c < 3; c++) {
                int top = p00[c] * (256 - ax) + p01[c] * ax, bot = p10[c] * (256 - ax) + p11[c] * ax;
                o[c] = (uint8_t)((top * (256 - ay) + bot * ay + 32768) >> 16);
            }
            o[3] = 255;
        }
    });
}

void Renderer::Draw(const ImagePtr &im, const ViewParams &v, uint32_t bg, uint8_t *dst, size_t pitch, int W, int H,
                    RECT *drawn) {
    const uint32_t bgpx = 0xFF000000u | bg;
    ParallelFor((size_t)H, [&](size_t y) {
        uint32_t *d = (uint32_t *)(dst + y * pitch);
        std::fill(d, d + W, bgpx);
    });
    if (drawn) SetRectEmpty(drawn);
    if (!im || im->w <= 0 || v.scale <= 0) return;
    int ow, oh;
    OrientedSize(*im, v.orient, &ow, &oh);
    // the image's rectangle on screen
    const double left = W / 2.0 - v.cx * v.scale, top = H / 2.0 - v.cy * v.scale;
    int x0 = std::max(0, (int)std::floor(left)), y0 = std::max(0, (int)std::floor(top));
    int x1 = std::min(W, (int)std::ceil(left + ow * v.scale)), y1 = std::min(H, (int)std::ceil(top + oh * v.scale));
    if (x0 >= x1 || y0 >= y1) return;

    if (v.scale < 1) {
        if (cacheImage_.lock() != im || !(cacheOrient_ == v.orient) || cacheScale_ != v.scale || cache_.empty()) {
            cache_.clear();
            cacheImage_ = im;
            cacheOrient_ = v.orient;
            cacheScale_ = v.scale;
            if (!BuildCache(*im, v)) cache_.clear();
        }
        if (!cache_.empty()) {
            // whole cache pixels: the reduced image's top-left at a whole screen pixel
            const int cl = (int)std::lround(W / 2.0 - v.cx * cacheW_ / (double)ow),
                      ct = (int)std::lround(H / 2.0 - v.cy * cacheH_ / (double)oh);
            x0 = std::max(0, cl);
            y0 = std::max(0, ct);
            x1 = std::min(W, cl + cacheW_);
            y1 = std::min(H, ct + cacheH_);
            if (x0 >= x1 || y0 >= y1) return;
            const uint8_t *c = cache_.data();
            const int cw = cacheW_;
            ParallelFor((size_t)(y1 - y0), [&](size_t r) {
                int Y = y0 + (int)r;
                memcpy(dst + (size_t)Y * pitch + (size_t)x0 * 4, c + ((size_t)(Y - ct) * cw + (x0 - cl)) * 4,
                       (size_t)(x1 - x0) * 4);
            });
            if (drawn) SetRect(drawn, x0, y0, x1, y1);
            return;
        }
    }
    DrawDirect(*im, v, dst, pitch, W, H, x0, y0, x1, y1);
    if (drawn) SetRect(drawn, x0, y0, x1, y1);
}
