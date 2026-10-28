// FarNitroView for Far Manager: drawing an image into a BGRA buffer at any zoom and orientation.
#pragma once
#include "image.h"
#include <vector>

// Display -> source mapping of an orientation, on coordinates relative to the image centre:
// source = M * display. M[0] M[1] / M[2] M[3], entries -1, 0, 1.
struct Orient {
    int m[4] = {1, 0, 0, 1};
    static Orient FromExif(int o);
    bool Transposed() const { return m[0] == 0; }
    void RotateCW();    // the shown image turns 90 degrees clockwise
    void RotateCCW();
    void FlipH();       // mirrored left - right
    void FlipV();       // upside down
    bool operator==(const Orient &o) const { return m[0] == o.m[0] && m[1] == o.m[1] && m[2] == o.m[2] && m[3] == o.m[3]; }
    void Then(const int u[4]);   // M = M * U
};

// The image as shown: its size after the orientation.
static inline void OrientedSize(const Image &im, const Orient &o, int *w, int *h) {
    *w = o.Transposed() ? im.h : im.w;
    *h = o.Transposed() ? im.w : im.h;
}

struct ViewParams {
    Orient orient;
    double scale = 1;     // screen pixels per image pixel
    double cx = 0, cy = 0;// the (oriented) image point at the middle of the viewport
    bool smooth = true;
    int frame = 0;        // of an animation
};

class Renderer {
public:
    // Fills dst (W x H BGRA, rows pitch bytes apart) with bg and draws the image. The image's
    // rectangle on screen is returned in *drawn (empty if it is outside).
    void Draw(const ImagePtr &im, const ViewParams &v, uint32_t bg, uint8_t *dst, size_t pitch, int W, int H,
              RECT *drawn = nullptr);
    void Clear() { cache_.clear(); cacheImage_.reset(); svgPx_.clear(); svgImage_.reset(); }

private:
    // the whole image, oriented and reduced to the current scale (scale < 1)
    std::vector<uint8_t> cache_;
    int cacheW_ = 0, cacheH_ = 0;
    std::weak_ptr<const Image> cacheImage_;
    Orient cacheOrient_;
    double cacheScale_ = 0;
    int cacheFrame_ = 0;
    // a vector image's last rendering
    std::vector<uint32_t> svgPx_;
    std::weak_ptr<const Image> svgImage_;
    Orient svgOrient_;
    double svgScale_ = 0, svgLeft_ = 0, svgTop_ = 0;
    RECT svgRect_ = {};

    bool BuildCache(const Image &im, const ViewParams &v);
    void DrawSvg(const ImagePtr &im, const ViewParams &v, uint8_t *dst, size_t pitch, int x0, int y0, int x1, int y1,
                 double left, double top);
    void DrawDirect(const Image &im, const ViewParams &v, uint8_t *dst, size_t pitch, int W, int H, int x0, int y0,
                    int x1, int y1);
};
