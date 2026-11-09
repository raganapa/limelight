// SPDX-License-Identifier: MIT
// Tiny CPU rasterizer for premultiplied BGRA surfaces backed by DIB sections,
// so they can be handed straight to UpdateLayeredWindow.
#pragma once
#include "common.h"

struct Image {
  int w = 0, h = 0;
  uint32_t* px = nullptr;  // premultiplied BGRA (0xAARRGGBB), top-down, stride = w
  HBITMAP bmp = nullptr;
  HDC dc = nullptr;
  HGDIOBJ oldBmp = nullptr;

  Image() = default;
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  ~Image() { Destroy(); }

  bool Create(int width, int height);
  void Destroy();
  bool Valid() const { return px != nullptr; }
  void Clear(uint32_t v = 0);
  // Fill [x0,x1) x [y0,y1), clipped.
  void FillRect(int x0, int y0, int x1, int y1, uint32_t v);
  // Copy src to (dx, dy), clipped.
  void Blit(const Image& src, int dx, int dy);
};

inline uint32_t Premul(uint32_t rgb, int a) {
  if (a <= 0) return 0;
  if (a > 255) a = 255;
  uint32_t r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  r = (r * a + 127) / 255;
  g = (g * a + 127) / 255;
  b = (b * a + 127) / 255;
  return (uint32_t(a) << 24) | (r << 16) | (g << 8) | b;
}

// Scale every channel of a premultiplied pixel by c (0..1).
inline uint32_t ScalePx(uint32_t p, float c) {
  if (c <= 0.f) return 0;
  if (c >= 1.f) return p;
  uint32_t k = uint32_t(c * 256.f + 0.5f);
  uint32_t a = ((p >> 24) * k) >> 8, r = (((p >> 16) & 255) * k) >> 8;
  uint32_t g = (((p >> 8) & 255) * k) >> 8, b = ((p & 255) * k) >> 8;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// Premultiplied "over": top over bottom.
inline uint32_t Over(uint32_t top, uint32_t bottom) {
  uint32_t ta = top >> 24;
  if (ta == 255) return top;
  if (ta == 0) return bottom;
  uint32_t inv = 255 - ta;
  auto ch = [&](int sh) {
    uint32_t t = (top >> sh) & 255, b = (bottom >> sh) & 255;
    uint32_t v = t + (b * inv + 127) / 255;
    return (v > 255 ? 255u : v) << sh;
  };
  return ch(24) | ch(16) | ch(8) | ch(0);
}

// A tile that is fully "dim" outside a rounded rectangle and clear inside,
// with a soft edge of `feather` pixels. A circle is halfW == halfH == corner.
void MakeHoleTile(Image& tile, int halfW, int halfH, int corner, int feather, uint32_t dimPx);

// Glowing laser dot (or a crisp pen tip when glow == false).
void MakeDotSprite(Image& s, float radius, uint32_t rgb, bool glow, bool outline);

// Magnifier lens. Static parts (shadow, ring, coverage mask) are cached in `lens`
// by PrepareLens; RenderLens then only resamples the content disc.
struct LensCache {
  Image frame;                 // shadow + ring
  std::vector<uint8_t> mask;   // content coverage 0..255, size L*L
  int L = 0, R = 0;
  uint32_t ring = 0;
};
void PrepareLens(LensCache& c, int radius, int ringW, int shadow, uint32_t ringRgb);
void RenderLens(Image& out, const LensCache& c, const Image& snap, int snapX, int snapY, int cx,
                int cy, float zoom);

// Anti-aliased round-capped segment, "max alpha" blending so overlapping
// segments of one stroke don't darken. Grows `dirty` (in image coordinates).
void InkSegment(Image& img, float x0, float y0, float x1, float y1, float halfW, uint32_t rgb,
                RECT& dirty);

// Copy a screen rectangle into img (img is (re)created to the right size).
bool CaptureScreen(Image& img, const RECT& r);

inline void UnionRectInPlace(RECT& a, const RECT& b) {
  if (b.right <= b.left || b.bottom <= b.top) return;
  if (a.right <= a.left || a.bottom <= a.top) { a = b; return; }
  a.left = std::min(a.left, b.left);
  a.top = std::min(a.top, b.top);
  a.right = std::max(a.right, b.right);
  a.bottom = std::max(a.bottom, b.bottom);
}
