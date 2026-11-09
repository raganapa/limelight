// SPDX-License-Identifier: MIT
#include "raster.h"

bool Image::Create(int width, int height) {
  if (width == w && height == h && px) return true;
  Destroy();
  if (width <= 0 || height <= 0) return false;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) {
    Log("CreateDIBSection %dx%d failed (%lu)", width, height, GetLastError());
    bmp = nullptr;
    return false;
  }
  dc = CreateCompatibleDC(nullptr);
  oldBmp = SelectObject(dc, bmp);
  px = static_cast<uint32_t*>(bits);
  w = width;
  h = height;
  memset(px, 0, size_t(w) * size_t(h) * 4);
  return true;
}

void Image::Destroy() {
  if (dc) {
    SelectObject(dc, oldBmp);
    DeleteDC(dc);
  }
  if (bmp) DeleteObject(bmp);
  dc = nullptr;
  bmp = nullptr;
  oldBmp = nullptr;
  px = nullptr;
  w = h = 0;
}

void Image::Clear(uint32_t v) {
  if (!px) return;
  std::fill_n(px, size_t(w) * size_t(h), v);
}

void Image::FillRect(int x0, int y0, int x1, int y1, uint32_t v) {
  if (!px) return;
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::min(x1, w);
  y1 = std::min(y1, h);
  if (x0 >= x1 || y0 >= y1) return;
  for (int y = y0; y < y1; ++y) std::fill_n(px + size_t(y) * w + x0, x1 - x0, v);
}

void Image::Blit(const Image& src, int dx, int dy) {
  if (!px || !src.px) return;
  int sx0 = 0, sy0 = 0, sx1 = src.w, sy1 = src.h;
  if (dx < 0) sx0 = -dx;
  if (dy < 0) sy0 = -dy;
  if (dx + sx1 > w) sx1 = w - dx;
  if (dy + sy1 > h) sy1 = h - dy;
  if (sx0 >= sx1 || sy0 >= sy1) return;
  const size_t bytes = size_t(sx1 - sx0) * 4;
  for (int y = sy0; y < sy1; ++y)
    memcpy(px + size_t(dy + y) * w + (dx + sx0), src.px + size_t(y) * src.w + sx0, bytes);
}

static inline float Smooth01(float t) {
  t = Clamp(t, 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

void MakeHoleTile(Image& tile, int halfW, int halfH, int corner, int feather, uint32_t dimPx) {
  const int pad = 2;
  const int tw = 2 * halfW + 2 * pad, th = 2 * halfH + 2 * pad;  // both even
  if (!tile.Create(tw, th)) return;
  const float cx = tw * 0.5f, cy = th * 0.5f;
  const float r = float(std::min({corner, halfW, halfH}));
  const float bx = float(halfW) - r, by = float(halfH) - r;
  const float f = float(std::max(feather, 0));
  // The shape is symmetric: compute the bottom-right quadrant and mirror it.
  const int qw = tw / 2, qh = th / 2;
  for (int y = qh; y < th; ++y) {
    uint32_t* row = tile.px + size_t(y) * tw;
    uint32_t* mrow = tile.px + size_t(th - 1 - y) * tw;
    const float qy = (y + 0.5f - cy) - by;
    for (int x = qw; x < tw; ++x) {
      const float qx = (x + 0.5f - cx) - bx;
      const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
      const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
      float c;  // how much dim covers this pixel
      if (f < 1.f)
        c = Clamp(d + 0.5f, 0.f, 1.f);
      else
        c = Smooth01((d + f) / f);
      const uint32_t v = ScalePx(dimPx, c);
      row[x] = v;
      row[tw - 1 - x] = v;
      mrow[x] = v;
      mrow[tw - 1 - x] = v;
    }
  }
}

void MakeDotSprite(Image& s, float radius, uint32_t rgb, bool glow, bool outline) {
  const float halo = glow ? radius * 3.2f : radius + (outline ? 2.5f : 1.0f);
  const int size = int(std::ceil(halo * 2.f)) + 4;
  if (!s.Create(size, size)) return;
  const float c = size * 0.5f;
  const float rr = ((rgb >> 16) & 255) / 255.f, gg = ((rgb >> 8) & 255) / 255.f,
              bb = (rgb & 255) / 255.f;
  // Bright core: mix towards white.
  const float cr = rr + (1 - rr) * 0.72f, cg = gg + (1 - gg) * 0.72f, cb = bb + (1 - bb) * 0.72f;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = x + 0.5f - c, dy = y + 0.5f - c;
      const float d = std::sqrt(dx * dx + dy * dy);
      // premultiplied accumulator
      float A = 0, R = 0, G = 0, B = 0;
      auto over = [&](float a, float r_, float g_, float b_) {
        if (a <= 0) return;
        R = r_ * a + R * (1 - a);
        G = g_ * a + G * (1 - a);
        B = b_ * a + B * (1 - a);
        A = a + A * (1 - a);
      };
      if (glow) {
        const float t = d / halo;
        float a = 0;
        if (t < 0.5f) a = 0.58f + (0.26f - 0.58f) * (t / 0.5f);
        else if (t < 1.f) a = 0.26f * (1.f - (t - 0.5f) / 0.5f);
        over(a, rr, gg, bb);
      }
      if (outline) over(Clamp(radius + 1.8f + 0.5f - d, 0.f, 1.f) * 0.92f, 1.f, 1.f, 1.f);
      over(Clamp(radius + 0.5f - d, 0.f, 1.f) * 0.96f, rr, gg, bb);
      if (glow) over(Clamp(radius * 0.38f + 0.5f - d, 0.f, 1.f) * 0.98f, cr, cg, cb);
      const int a8 = int(A * 255.f + 0.5f);
      s.px[size_t(y) * size + x] = a8 <= 0 ? 0
          : (uint32_t(a8) << 24) | (uint32_t(Clamp(R, 0.f, 1.f) * 255.f + 0.5f) << 16) |
            (uint32_t(Clamp(G, 0.f, 1.f) * 255.f + 0.5f) << 8) |
            uint32_t(Clamp(B, 0.f, 1.f) * 255.f + 0.5f);
    }
  }
}

void PrepareLens(LensCache& c, int radius, int ringW, int shadow, uint32_t ringRgb) {
  const int L = 2 * (radius + ringW + shadow) + 4;
  if (c.L == L && c.R == radius && c.ring == ringRgb && c.frame.Valid()) return;
  c.L = L;
  c.R = radius;
  c.ring = ringRgb;
  c.frame.Create(L, L);
  c.mask.assign(size_t(L) * L, 0);
  const float ctr = L * 0.5f;
  const float outer = float(radius + ringW);
  const float shadowOff = shadow * 0.18f;  // light from above
  for (int y = 0; y < L; ++y) {
    for (int x = 0; x < L; ++x) {
      const float dx = x + 0.5f - ctr, dy = y + 0.5f - ctr;
      const float d = std::sqrt(dx * dx + dy * dy);
      const float sdy = dy - shadowOff;
      const float sd = std::sqrt(dx * dx + sdy * sdy);
      uint32_t p = 0;
      if (shadow > 0) {
        float t = 1.f - (sd - outer) / float(shadow);
        t = Clamp(t, 0.f, 1.f);
        p = Premul(0x000000, int(0.34f * t * t * 255.f));
      }
      const float ringCov = Clamp(outer + 0.5f - d, 0.f, 1.f);
      if (ringCov > 0) p = Over(Premul(ringRgb, int(ringCov * 255.f + 0.5f)), p);
      c.frame.px[size_t(y) * L + x] = p;
      c.mask[size_t(y) * L + x] = uint8_t(Clamp(radius + 0.5f - d, 0.f, 1.f) * 255.f + 0.5f);
    }
  }
}

void RenderLens(Image& out, const LensCache& c, const Image& snap, int snapX, int snapY, int cx,
                int cy, float zoom) {
  if (!out.Create(c.L, c.L) || !snap.Valid() || !c.frame.Valid()) return;
  memcpy(out.px, c.frame.px, size_t(c.L) * c.L * 4);
  const int L = c.L;
  const float ctr = L * 0.5f;
  const float inv = 1.f / std::max(zoom, 1.f);
  const int sw = snap.w, sh = snap.h;
  const int32_t du = int32_t(inv * 65536.f);
  for (int y = 0; y < L; ++y) {
    const uint8_t* m = &c.mask[size_t(y) * L];
    // find the span of this row that touches the disc
    int x0 = 0, x1 = L - 1;
    while (x0 < L && !m[x0]) ++x0;
    if (x0 >= L) continue;
    while (x1 > x0 && !m[x1]) --x1;
    const float v = float(cy - snapY) + (y + 0.5f - ctr) * inv - 0.5f;
    int vy = int(std::floor(v));
    const uint32_t fy = uint32_t((v - vy) * 256.f);
    const int y0 = Clamp(vy, 0, sh - 1), y1 = Clamp(vy + 1, 0, sh - 1);
    const uint32_t* r0 = snap.px + size_t(y0) * sw;
    const uint32_t* r1 = snap.px + size_t(y1) * sw;
    const float u0 = float(cx - snapX) + (x0 + 0.5f - ctr) * inv - 0.5f;
    int32_t u = int32_t(std::floor(u0 * 65536.f));
    uint32_t* o = out.px + size_t(y) * L;
    for (int x = x0; x <= x1; ++x, u += du) {
      const uint8_t cov = m[x];
      if (!cov) continue;
      const int ux = u >> 16;
      const uint32_t fx = uint32_t(u & 0xFFFF) >> 8;
      const int xa = Clamp(ux, 0, sw - 1), xb = Clamp(ux + 1, 0, sw - 1);
      const uint32_t p00 = r0[xa], p01 = r0[xb], p10 = r1[xa], p11 = r1[xb];
      uint32_t rgb = 0;
      for (int sh8 = 0; sh8 < 24; sh8 += 8) {
        const uint32_t a = (p00 >> sh8) & 255, b = (p01 >> sh8) & 255;
        const uint32_t cc = (p10 >> sh8) & 255, d = (p11 >> sh8) & 255;
        const uint32_t top = a * (256 - fx) + b * fx;
        const uint32_t bot = cc * (256 - fx) + d * fx;
        const uint32_t val = (top * (256 - fy) + bot * fy) >> 16;
        rgb |= (val > 255 ? 255u : val) << sh8;
      }
      const uint32_t content = 0xFF000000u | rgb;
      o[x] = cov == 255 ? content : Over(ScalePx(content, cov / 255.f), o[x]);
    }
  }
}

void InkSegment(Image& img, float x0, float y0, float x1, float y1, float halfW, uint32_t rgb,
                RECT& dirty) {
  if (!img.Valid()) return;
  const float pad = halfW + 1.5f;
  int bx0 = int(std::floor(std::min(x0, x1) - pad)), by0 = int(std::floor(std::min(y0, y1) - pad));
  int bx1 = int(std::ceil(std::max(x0, x1) + pad)), by1 = int(std::ceil(std::max(y0, y1) + pad));
  bx0 = std::max(bx0, 0);
  by0 = std::max(by0, 0);
  bx1 = std::min(bx1, img.w);
  by1 = std::min(by1, img.h);
  if (bx0 >= bx1 || by0 >= by1) return;
  const float vx = x1 - x0, vy = y1 - y0;
  const float len2 = vx * vx + vy * vy;
  for (int y = by0; y < by1; ++y) {
    uint32_t* row = img.px + size_t(y) * img.w;
    const float py = y + 0.5f;
    for (int x = bx0; x < bx1; ++x) {
      const float px_ = x + 0.5f;
      float t = len2 > 1e-6f ? ((px_ - x0) * vx + (py - y0) * vy) / len2 : 0.f;
      t = Clamp(t, 0.f, 1.f);
      const float qx = px_ - (x0 + t * vx), qy = py - (y0 + t * vy);
      const float d = std::sqrt(qx * qx + qy * qy);
      const float cov = Clamp(halfW + 0.5f - d, 0.f, 1.f);
      if (cov <= 0.f) continue;
      const int a8 = int(cov * 255.f + 0.5f);
      if (uint32_t(a8) > (row[x] >> 24)) row[x] = Premul(rgb, a8);
    }
  }
  UnionRectInPlace(dirty, RECT{bx0, by0, bx1, by1});
}

bool CaptureScreen(Image& img, const RECT& r) {
  const int w = r.right - r.left, h = r.bottom - r.top;
  if (!img.Create(w, h)) return false;
  HDC screen = GetDC(nullptr);
  BOOL ok = BitBlt(img.dc, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
  ReleaseDC(nullptr, screen);
  GdiFlush();
  return ok != FALSE;
}
