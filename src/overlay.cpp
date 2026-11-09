// SPDX-License-Identifier: MIT
#include "overlay.h"

#include <dxgi.h>

#include <climits>
#include <map>
#include <memory>
#include <thread>

#include "raster.h"

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

OverlayParams OverlayParams::From(const Settings& s) {
  OverlayParams p;
  p.spotRadius = s.spotRadius;
  p.dimPercent = s.dimPercent;
  p.softness = s.softness;
  p.squareWidth = s.squareWidth;
  p.zoomTenths = s.zoomTenths;
  p.lensRadius = s.lensRadius;
  p.laserSize = s.laserSize;
  p.penWidth = s.penWidth;
  p.laserColor = s.laserColor;
  p.penColor = s.penColor;
  return p;
}

namespace {

constexpr float kFadeInMs = 90.f;
constexpr float kFadeOutMs = 150.f;
constexpr wchar_t kLayerClass[] = L"LimelightLayer";

struct Mon {
  HMONITOR h = nullptr;
  RECT rc{};
  float scale = 1.f;
  int hz = 60;
};

struct Layer {
  HWND hwnd = nullptr;
  Image img;
  POINT pos{};
  SIZE size{};
  float alpha = 0.f, target = 0.f;
  BYTE committedAlpha = 0;
  bool visible = false;
  bool needFull = true;
  RECT dirty{};
  bool posDirty = false;
  double lastUse = 0;
};

struct DimLayer : Layer {
  RECT hole{};
  bool holeValid = false;
  uint64_t tileKey = 0;
  uint32_t dimPx = 0;
};

struct InkLayer : Layer {
  bool hasInk = false;
};

bool SamePt(POINT a, POINT b) { return a.x == b.x && a.y == b.y; }

// Wine bakes the constant alpha into the uploaded pixels instead of applying it at
// composition time, so fades there need full uploads. Only used for testing on Linux.
bool RunningOnWine() {
  static const bool wine =
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
  return wine;
}

RECT ClipRect(const RECT& r, int w, int h) {
  RECT c{std::max<LONG>(r.left, 0), std::max<LONG>(r.top, 0), std::min<LONG>(r.right, w),
         std::min<LONG>(r.bottom, h)};
  if (c.right < c.left) c.right = c.left;
  if (c.bottom < c.top) c.bottom = c.top;
  return c;
}

LRESULT CALLBACK LayerProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_DPICHANGED:
      return 0;  // we size ourselves in physical pixels
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProcW(h, msg, w, l);
}

HWND CreateLayerWindow() {
  static bool registered = false;
  HINSTANCE inst = GetModuleHandleW(nullptr);
  if (!registered) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = LayerProc;
    wc.hInstance = inst;
    wc.lpszClassName = kLayerClass;
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);
    registered = true;
  }
  HWND h = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      kLayerClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
  if (!h) Log("CreateLayerWindow failed: %lu", GetLastError());
  return h;
}

// Waits for the next vertical blank of a given display. DXGI's WaitForVBlank is
// tied to the hardware, so it keeps ticking even when nothing on screen changes.
// If it is unusable (returns immediately), fall back to a high-resolution timer.
class VBlank {
 public:
  void Init() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    Refresh();
  }
  void Shutdown() {
    Release();
    if (factory_) factory_->Release();
    factory_ = nullptr;
    if (timer_) CloseHandle(timer_);
    timer_ = nullptr;
  }
  void Refresh() {
    Release();
    if (factory_ && !factory_->IsCurrent()) {
      factory_->Release();
      factory_ = nullptr;
    }
    if (!factory_ &&
        FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory_))))
      factory_ = nullptr;
    if (!factory_) return;
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; factory_->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
      IDXGIOutput* out = nullptr;
      for (UINT j = 0; ad->EnumOutputs(j, &out) != DXGI_ERROR_NOT_FOUND; ++j) {
        DXGI_OUTPUT_DESC d;
        if (SUCCEEDED(out->GetDesc(&d)))
          outs_.push_back({d.Monitor, out});
        else
          out->Release();
      }
      ad->Release();
    }
  }
  void Wait(HMONITOR mon, int hz) {
    if (useDxgi_ && !outs_.empty()) {
      IDXGIOutput* o = outs_[0].second;
      for (auto& p : outs_)
        if (p.first == mon) {
          o = p.second;
          break;
        }
      const double t0 = NowMs();
      const HRESULT hr = o->WaitForVBlank();
      const double el = NowMs() - t0;
      if (SUCCEEDED(hr) && el >= 0.4) {
        fast_ = 0;
        return;
      }
      if (++fast_ > 24) {
        useDxgi_ = false;
        Log("vblank: DXGI wait unusable, switching to timer pacing");
      }
    }
    TimerWait(hz);
  }

 private:
  void Release() {
    for (auto& o : outs_) o.second->Release();
    outs_.clear();
  }
  void TimerWait(int hz) {
    const double period = 1000.0 / Clamp(hz, 30, 360);
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(period * 10000.0);
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(timer_, 100);
    else
      Sleep(DWORD(period));
  }
  IDXGIFactory1* factory_ = nullptr;
  std::vector<std::pair<HMONITOR, IDXGIOutput*>> outs_;
  HANDLE timer_ = nullptr;
  bool useDxgi_ = true;
  int fast_ = 0;
};

BOOL CALLBACK EnumMonProc(HMONITOR h, HDC, LPRECT, LPARAM lp) {
  auto* v = reinterpret_cast<std::vector<Mon>*>(lp);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof mi;
  if (!GetMonitorInfoW(h, &mi)) return TRUE;
  UINT dx = 96, dy = 96;
  if (FAILED(GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  int hz = 60;
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    hz = int(dm.dmDisplayFrequency);
  v->push_back(Mon{h, mi.rcMonitor, dx / 96.f, hz});
  return TRUE;
}

}  // namespace

struct Overlay::Impl {
  Overlay* owner = nullptr;
  std::thread th;
  HANDLE wake = nullptr;

  // ---- shared with the UI thread (guarded by m)
  std::mutex m;
  Effect sEffect = Effect::None;
  OverlayParams sParams;
  bool sParamsDirty = true;
  bool sClearInk = false;
  std::wstring sToast;
  bool sToastDirty = false;
  bool sDisplayChanged = true;
  bool sQuit = false;

  // ---- render thread only
  bool quit = false;
  Effect effect = Effect::None;
  OverlayParams P;
  std::vector<Mon> mons;
  std::vector<std::unique_ptr<DimLayer>> dims;
  std::vector<std::unique_ptr<InkLayer>> inks;
  Layer lens, dot, toast;
  std::map<uint64_t, std::unique_ptr<Image>> tiles;
  bool dimRect = false;  // remembered shape while fading out
  int dotKind = 0;       // 0 = laser dot, 1 = pen tip
  int dotSpriteKind = -1;
  Image laserSprite, penSprite;
  float spriteScale = 0.f;
  bool spritesDirty = true;
  LensCache lensCache;
  Image snap;
  RECT snapRc{};
  bool snapValid = false;
  HMONITOR snapMon = nullptr;
  POINT lastLensCur{INT_MIN, INT_MIN};
  bool lensDirty = true;
  bool penWasDown = false;
  std::vector<std::pair<float, float>> stroke;
  bool inkClearing = false;
  double toastUntil = 0;
  VBlank vb;
  double lastFrame = 0, lastRestack = 0;
  bool restackNeeded = false;
  POINT cur{0, 0};
  int curMon = 0;
  double lastBusy = 0;

  // ------------------------------------------------------------ plumbing
  void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  void Consume() {
    std::lock_guard<std::mutex> lk(m);
    quit = sQuit;
    effect = sEffect;
    if (sParamsDirty) {
      sParamsDirty = false;
      if (!(P == sParams)) {
        P = sParams;
        tiles.clear();
        spritesDirty = true;
        lensDirty = true;
        for (auto& d : dims)
          if (d) d->holeValid = false, d->tileKey = 0;
      }
    }
    if (sClearInk) {
      sClearInk = false;
      bool any = false;
      for (auto& k : inks)
        if (k && k->hasInk) any = true;
      if (any) inkClearing = true;
    }
    if (sToastDirty) {
      sToastDirty = false;
      pendingToast = sToast;
      hasPendingToast = true;
    }
    if (sDisplayChanged) {
      sDisplayChanged = false;
      displayChanged = true;
    }
  }
  std::wstring pendingToast;
  bool hasPendingToast = false;
  bool displayChanged = false;

  void RefreshMonitors() {
    std::vector<Mon> nm;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&nm));
    if (nm.empty()) {
      Mon m0;
      m0.h = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
      m0.rc = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
      nm.push_back(m0);
    }
    // Drop per-monitor layers; they are recreated lazily for the new layout.
    for (auto& d : dims)
      if (d) DestroyLayer(*d);
    for (auto& k : inks)
      if (k) DestroyLayer(*k);
    dims.clear();
    inks.clear();
    mons = nm;
    dims.resize(mons.size());
    inks.resize(mons.size());
    tiles.clear();
    spritesDirty = true;
    lensDirty = true;
    owner->hasInk_ = false;
    vb.Refresh();
    Log("overlay: %zu monitor(s)", mons.size());
    for (auto& mo : mons)
      Log("  monitor %ld,%ld %ldx%ld scale %.2f %d Hz", mo.rc.left, mo.rc.top,
          mo.rc.right - mo.rc.left, mo.rc.bottom - mo.rc.top, mo.scale, mo.hz);
  }

  int MonitorIndexAt(POINT p) {
    for (size_t i = 0; i < mons.size(); ++i)
      if (PtInRect(&mons[i].rc, p)) return int(i);
    HMONITOR h = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    for (size_t i = 0; i < mons.size(); ++i)
      if (mons[i].h == h) return int(i);
    return 0;
  }

  void DestroyLayer(Layer& L) {
    if (L.hwnd) DestroyWindow(L.hwnd);
    L.hwnd = nullptr;
    L.img.Destroy();
    L.visible = false;
    L.alpha = L.target = 0;
  }

  static BYTE AlphaByte(float a) {
    a = Clamp(a, 0.f, 1.f);
    const float e = a * a * (3.f - 2.f * a);
    return BYTE(e * 255.f + 0.5f);
  }

  void Commit(Layer& L) {
    if (!L.hwnd || !L.img.Valid()) return;
    const BYTE a = AlphaByte(L.alpha);
    if (a != L.committedAlpha && RunningOnWine()) L.needFull = true;
    const bool content = L.needFull || (L.dirty.right > L.dirty.left && L.dirty.bottom > L.dirty.top);
    if (!content && a == L.committedAlpha && !L.posDirty) return;
    BLENDFUNCTION bf{AC_SRC_OVER, 0, a, AC_SRC_ALPHA};
    POINT src{0, 0};
    UPDATELAYEREDWINDOWINFO u{};
    u.cbSize = sizeof u;
    u.pptDst = &L.pos;
    u.pblend = &bf;
    u.dwFlags = ULW_ALPHA;
    if (content) {
      u.hdcSrc = L.img.dc;
      u.pptSrc = &src;
      u.psize = &L.size;
      u.prcDirty = L.needFull ? nullptr : &L.dirty;
    }
    if (!UpdateLayeredWindowIndirect(L.hwnd, &u)) {
      static int failures = 0;
      if (failures++ < 8) Log("UpdateLayeredWindowIndirect failed: %lu", GetLastError());
      // Retry once with a full upload; some drivers dislike dirty rects at the edge.
      if (!L.needFull && content) {
        u.prcDirty = nullptr;
        UpdateLayeredWindowIndirect(L.hwnd, &u);
      }
    }
    L.needFull = false;
    L.dirty = RECT{};
    L.posDirty = false;
    L.committedAlpha = a;
  }

  void Show(Layer& L) {
    if (!L.hwnd || L.visible) return;
    SetWindowPos(L.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    L.visible = true;
    restackNeeded = true;
  }

  void Hide(Layer& L) {
    if (!L.hwnd || !L.visible) return;
    ShowWindow(L.hwnd, SW_HIDE);
    L.visible = false;
  }

  void Restack() {
    auto top = [](Layer& L) {
      if (L.visible && L.hwnd)
        SetWindowPos(L.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    };
    for (auto& d : dims)
      if (d) top(*d);
    for (auto& k : inks)
      if (k) top(*k);
    top(lens);
    top(dot);
    top(toast);
    restackNeeded = false;
    lastRestack = NowMs();
  }

  // Fade a layer towards its target and push the result to the screen.
  void Step(Layer& L, float dt) {
    if (L.target > L.alpha)
      L.alpha = std::min(L.target, L.alpha + dt / kFadeInMs);
    else if (L.target < L.alpha)
      L.alpha = std::max(L.target, L.alpha - dt / kFadeOutMs);
    if (L.alpha > 0.f || L.target > 0.f) {
      if (!L.hwnd) L.hwnd = CreateLayerWindow();
      if (!L.visible) {
        // Show first, then upload everything: some systems drop content that was
        // pushed while the window was hidden. It was hidden at alpha 0, so the
        // old content cannot flash.
        Show(L);
        L.needFull = true;
      }
      Commit(L);
      L.lastUse = NowMs();
    } else if (L.visible) {
      Commit(L);
      Hide(L);
    }
  }

  // ------------------------------------------------------------ dim (spotlight / squarelight)
  uint32_t DimPx() const { return Premul(0x000000, P.dimPercent * 255 / 100); }

  Image& Tile(bool rect, float scale, uint64_t& keyOut) {
    int halfW, halfH, corner, feather;
    if (!rect) {
      halfW = halfH = corner = Clamp(int(P.spotRadius * scale + 0.5f), 20, 1100);
      feather = int(halfW * (P.softness / 100.f) * 0.45f);
    } else {
      halfW = Clamp(int(P.squareWidth * scale * 0.5f + 0.5f), 40, 1400);
      halfH = Clamp(int(halfW * 0.5625f + 0.5f), 24, 1000);
      corner = int(18 * scale + 0.5f);
      feather = int(std::min(halfW, halfH) * (P.softness / 100.f) * 0.35f);
    }
    // The cache is flushed whenever the look changes, so geometry is enough for the key.
    keyOut = uint64_t(rect ? 1 : 0) | (uint64_t(halfW & 0xFFFF) << 1) |
             (uint64_t(halfH & 0xFFFF) << 17) | (uint64_t(feather & 0xFFFF) << 33) |
             (uint64_t(corner & 0xFFF) << 49);
    auto& slot = tiles[keyOut];
    if (!slot) {
      slot = std::make_unique<Image>();
      MakeHoleTile(*slot, halfW, halfH, corner, feather, DimPx());
    }
    return *slot;
  }

  DimLayer& Dim(size_t i) {
    if (!dims[i]) dims[i] = std::make_unique<DimLayer>();
    return *dims[i];
  }

  void UpdateDim(size_t i, bool cursorHere) {
    DimLayer& D = Dim(i);
    const Mon& M = mons[i];
    const int w = M.rc.right - M.rc.left, h = M.rc.bottom - M.rc.top;
    const uint32_t dp = DimPx();
    if (!D.img.Valid() || D.img.w != w || D.img.h != h) {
      if (!D.img.Create(w, h)) return;
      D.img.Clear(dp);
      D.dimPx = dp;
      D.needFull = true;
      D.holeValid = false;
      D.pos = POINT{M.rc.left, M.rc.top};
      D.size = SIZE{w, h};
    } else if (D.dimPx != dp) {
      D.img.Clear(dp);
      D.dimPx = dp;
      D.needFull = true;
      D.holeValid = false;
    }
    if (!cursorHere && D.holeValid) return;
    uint64_t key = 0;
    Image& t = Tile(dimRect, M.scale, key);
    if (!t.Valid()) return;
    const int cx = cur.x - M.rc.left, cy = cur.y - M.rc.top;
    const RECT nb{cx - t.w / 2, cy - t.h / 2, cx - t.w / 2 + t.w, cy - t.h / 2 + t.h};
    if (D.holeValid && D.tileKey == key && EqualRect(&nb, &D.hole)) return;
    RECT dirty{};
    if (D.holeValid) {
      D.img.FillRect(D.hole.left, D.hole.top, D.hole.right, D.hole.bottom, dp);
      UnionRectInPlace(dirty, ClipRect(D.hole, w, h));
    }
    D.img.Blit(t, nb.left, nb.top);
    UnionRectInPlace(dirty, ClipRect(nb, w, h));
    D.hole = nb;
    D.holeValid = true;
    D.tileKey = key;
    if (!D.needFull) UnionRectInPlace(D.dirty, dirty);
  }

  // ------------------------------------------------------------ magnifier
  void CaptureForLens(int mi) {
    bool hid = false;
    for (auto& d : dims)
      if (d && d->visible) {
        d->alpha = d->target = 0;
        Commit(*d);
        Hide(*d);
        hid = true;
      }
    if (dot.visible) {
      dot.alpha = dot.target = 0;
      Commit(dot);
      Hide(dot);
      hid = true;
    }
    if (toast.visible) {
      toast.alpha = toast.target = 0;
      toastUntil = 0;
      Commit(toast);
      Hide(toast);
      hid = true;
    }
    if (lens.visible) {
      Hide(lens);
      hid = true;
    }
    if (hid) DwmFlush();  // make sure the frame we copy no longer contains them
    snapRc = mons[mi].rc;
    snapMon = mons[mi].h;
    snapValid = CaptureScreen(snap, snapRc);
    if (!snapValid) Log("magnifier: screen capture failed (%lu)", GetLastError());
    lastLensCur = POINT{INT_MIN, INT_MIN};
    lensDirty = true;
  }

  void UpdateLens(bool want) {
    const Mon& M = mons[curMon];
    const int R = Clamp(int(P.lensRadius * M.scale + 0.5f), 40, 900);
    const int ring = std::max(2, int(2.5f * M.scale + 0.5f));
    const int shadow = int(16 * M.scale + 0.5f);
    PrepareLens(lensCache, R, ring, shadow, 0xFFFFFF);
    if (want && snapValid) {
      if (lensDirty || !SamePt(cur, lastLensCur) || lens.img.w != lensCache.L) {
        RenderLens(lens.img, lensCache, snap, snapRc.left, snapRc.top, cur.x, cur.y,
                   P.zoomTenths / 10.f);
        lens.size = SIZE{lensCache.L, lensCache.L};
        lens.needFull = true;
        lastLensCur = cur;
        lensDirty = false;
      }
      const POINT np{cur.x - lensCache.L / 2, cur.y - lensCache.L / 2};
      if (!SamePt(np, lens.pos)) {
        lens.pos = np;
        lens.posDirty = true;
      }
    }
  }

  // ------------------------------------------------------------ laser dot / pen tip
  void UpdateDot(bool want) {
    const float sc = mons[curMon].scale;
    if (spritesDirty || spriteScale != sc) {
      MakeDotSprite(laserSprite, P.laserSize * sc, P.laserColor, true, false);
      MakeDotSprite(penSprite, std::max(2.f, P.penWidth * sc * 0.5f + 1.f), P.penColor, false, true);
      spriteScale = sc;
      spritesDirty = false;
      dotSpriteKind = -1;
    }
    if (!want) return;
    if (dotSpriteKind != dotKind) {
      Image& s = dotKind ? penSprite : laserSprite;
      if (dot.img.Create(s.w, s.h)) {
        dot.img.Blit(s, 0, 0);
        dot.size = SIZE{s.w, s.h};
        dot.needFull = true;
        dotSpriteKind = dotKind;
      }
    }
    const POINT np{cur.x - dot.size.cx / 2, cur.y - dot.size.cy / 2};
    if (!SamePt(np, dot.pos)) {
      dot.pos = np;
      dot.posDirty = true;
    }
  }

  // ------------------------------------------------------------ ink
  InkLayer& Ink(size_t i) {
    if (!inks[i]) inks[i] = std::make_unique<InkLayer>();
    InkLayer& K = *inks[i];
    const Mon& M = mons[i];
    const int w = M.rc.right - M.rc.left, h = M.rc.bottom - M.rc.top;
    if (!K.img.Valid() || K.img.w != w || K.img.h != h) {
      if (K.img.Create(w, h)) {
        K.img.Clear(0);
        K.pos = POINT{M.rc.left, M.rc.top};
        K.size = SIZE{w, h};
        K.needFull = true;
      }
    }
    return K;
  }

  void InkLine(std::pair<float, float> a, std::pair<float, float> b) {
    const float halfW = std::max(1.f, P.penWidth * mons[curMon].scale * 0.5f);
    const float pad = halfW + 2.f;
    RECT bb{LONG(std::floor(std::min(a.first, b.first) - pad)),
            LONG(std::floor(std::min(a.second, b.second) - pad)),
            LONG(std::ceil(std::max(a.first, b.first) + pad)),
            LONG(std::ceil(std::max(a.second, b.second) + pad))};
    for (size_t i = 0; i < mons.size(); ++i) {
      RECT is;
      if (!IntersectRect(&is, &bb, &mons[i].rc)) continue;
      InkLayer& K = Ink(i);
      if (!K.img.Valid()) continue;
      const float ox = float(mons[i].rc.left), oy = float(mons[i].rc.top);
      RECT dirty{};
      InkSegment(K.img, a.first - ox, a.second - oy, b.first - ox, b.second - oy, halfW,
                 P.penColor, dirty);
      if (!K.needFull) UnionRectInPlace(K.dirty, dirty);
      K.hasInk = true;
      K.alpha = K.target = 1.f;  // ink appears instantly, no fade-in
      owner->hasInk_ = true;
    }
  }

  static std::pair<float, float> Mid(std::pair<float, float> a, std::pair<float, float> b) {
    return {(a.first + b.first) * 0.5f, (a.second + b.second) * 0.5f};
  }

  void InkQuad(std::pair<float, float> a, std::pair<float, float> c, std::pair<float, float> b) {
    const float len = std::hypot(c.first - a.first, c.second - a.second) +
                      std::hypot(b.first - c.first, b.second - c.second);
    const int steps = Clamp(int(len / 2.5f), 1, 64);
    auto prev = a;
    for (int i = 1; i <= steps; ++i) {
      const float t = float(i) / steps, u = 1.f - t;
      const std::pair<float, float> p{u * u * a.first + 2 * u * t * c.first + t * t * b.first,
                                      u * u * a.second + 2 * u * t * c.second + t * t * b.second};
      InkLine(prev, p);
      prev = p;
    }
  }

  // Pointer positions between two frames (GetMouseMovePointsEx keeps a short history),
  // so fast pen strokes stay round instead of turning into polygons.
  bool historyOk = true;
  DWORD lastHistTime = 0;
  POINT lastHistPt{INT_MIN, INT_MIN};

  int FetchHistory(POINT now, MOUSEMOVEPOINT* pts) {
    if (!historyOk) return 0;
    MOUSEMOVEPOINT in{};
    in.x = now.x & 0xFFFF;
    in.y = now.y & 0xFFFF;
    const int n = GetMouseMovePointsEx(sizeof in, &in, pts, 64, GMMP_USE_DISPLAY_POINTS);
    if (n <= 0) return 0;
    for (int i = 0; i < n; ++i) {
      if (pts[i].x > 32767) pts[i].x -= 65536;
      if (pts[i].y > 32767) pts[i].y -= 65536;
    }
    if (pts[0].x != now.x || pts[0].y != now.y) {  // different coordinate space: give up
      historyOk = false;
      return 0;
    }
    return n;
  }

  void SeedHistory(POINT now) {
    MOUSEMOVEPOINT pts[64];
    if (FetchHistory(now, pts) > 0) {
      lastHistTime = pts[0].time;
      lastHistPt = POINT{pts[0].x, pts[0].y};
    } else {
      lastHistTime = 0;
    }
  }

  std::vector<POINT> MoveHistory(POINT now) {
    std::vector<POINT> out;
    MOUSEMOVEPOINT pts[64];
    const int n = FetchHistory(now, pts);
    if (n <= 0 || lastHistTime == 0) {
      if (n > 0) {
        lastHistTime = pts[0].time;
        lastHistPt = POINT{pts[0].x, pts[0].y};
      }
      return out;
    }
    int stop = n;
    for (int i = 0; i < n; ++i) {
      if (pts[i].time < lastHistTime ||
          (pts[i].time == lastHistTime && pts[i].x == lastHistPt.x && pts[i].y == lastHistPt.y)) {
        stop = i;
        break;
      }
    }
    for (int i = stop - 1; i >= 1; --i) out.push_back(POINT{pts[i].x, pts[i].y});  // oldest first
    lastHistTime = pts[0].time;
    lastHistPt = POINT{pts[0].x, pts[0].y};
    return out;
  }

  void AddInkPoint(POINT p) {
    const std::pair<float, float> q{float(p.x), float(p.y)};
    if (!stroke.empty()) {
      const auto& last = stroke.back();
      if (std::fabs(last.first - q.first) < 0.5f && std::fabs(last.second - q.second) < 0.5f) return;
    }
    stroke.push_back(q);
    const size_t n = stroke.size();
    if (n == 1) {
      InkLine(q, q);
    } else if (n == 2) {
      InkLine(stroke[0], Mid(stroke[0], stroke[1]));
    } else {
      InkQuad(Mid(stroke[n - 3], stroke[n - 2]), stroke[n - 2], Mid(stroke[n - 2], stroke[n - 1]));
      stroke.erase(stroke.begin());
    }
  }

  void EndStroke() {
    const size_t n = stroke.size();
    if (n >= 2) InkLine(Mid(stroke[n - 2], stroke[n - 1]), stroke[n - 1]);
    stroke.clear();
  }

  void FinishInkClear() {
    for (auto& k : inks) {
      if (!k) continue;
      k->alpha = k->target = 0;
      Commit(*k);
      Hide(*k);
      k->img.Destroy();
      k->hasInk = false;
      k->needFull = true;
    }
    inkClearing = false;
    owner->hasInk_ = false;
  }

  // ------------------------------------------------------------ toast
  void RenderToast(const std::wstring& text) {
    const Mon& M = mons[curMon];
    const float sc = M.scale;
    using namespace Gdiplus;
    std::unique_ptr<Font> fontPtr;
    for (const wchar_t* c : {L"Segoe UI Semibold", L"Segoe UI", L"Tahoma", L"Arial"}) {
      FontFamily fam(c);
      if (fam.GetLastStatus() == Ok) {
        fontPtr = std::make_unique<Font>(&fam, 16.f * sc,
                                         wcscmp(c, L"Segoe UI Semibold") ? FontStyleBold : FontStyleRegular,
                                         UnitPixel);
        break;
      }
    }
    if (!fontPtr)
      fontPtr = std::make_unique<Font>(FontFamily::GenericSansSerif(), 16.f * sc, FontStyleBold,
                                       UnitPixel);
    Font& font = *fontPtr;
    RectF box;
    {
      Bitmap probe(1, 1, PixelFormat32bppPARGB);
      Graphics g(&probe);
      g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &box);
    }
    const int padX = int(22 * sc), padY = int(11 * sc);
    const int w = int(std::ceil(box.Width)) + 2 * padX;
    const int h = int(std::ceil(box.Height)) + 2 * padY;
    if (!toast.img.Create(w, h)) return;
    toast.img.Clear(0);
    {
      Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(toast.img.px));
      Graphics g(&bmp);
      g.SetSmoothingMode(SmoothingModeAntiAlias);
      g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
      GraphicsPath path;
      const REAL r = REAL(h) * 0.5f;
      path.AddArc(REAL(0), REAL(0), 2 * r, 2 * r - 1, 90.f, 180.f);
      path.AddArc(REAL(w) - 2 * r - 1, REAL(0), 2 * r, 2 * r - 1, 270.f, 180.f);
      path.CloseFigure();
      SolidBrush bg(Color(230, 24, 24, 28));
      g.FillPath(&bg, &path);
      SolidBrush fg(Color(255, 255, 255, 255));
      g.DrawString(text.c_str(), -1, &font, PointF(REAL(padX), REAL(padY)), &fg);
    }
    toast.size = SIZE{w, h};
    const int mw = M.rc.right - M.rc.left;
    toast.pos = POINT{M.rc.left + (mw - w) / 2, M.rc.bottom - h - int(110 * sc)};
    toast.needFull = true;
    toastUntil = NowMs() + 1100;
  }

  // ------------------------------------------------------------ frame
  bool Busy(double now) const {
    if (effect != Effect::None || hasPendingToast || displayChanged) return true;
    if (toastUntil > now || inkClearing) return true;
    auto moving = [](const Layer& L) { return L.alpha != L.target; };
    for (auto& d : dims)
      if (d && moving(*d)) return true;
    for (auto& k : inks)
      if (k && moving(*k)) return true;
    return moving(lens) || moving(dot) || moving(toast);
  }

  // statistics for the log (one line per activation)
  int statFrames = 0;
  double statWork = 0, statMaxGap = 0, statFirst = 0, statLast = 0;

  void Frame() {
    const double now = NowMs();
    if (statFrames == 0) statFirst = now;
    else statMaxGap = std::max(statMaxGap, now - statLast);
    statLast = now;
    float dt = float(now - lastFrame);
    if (lastFrame == 0 || dt > 50.f) dt = 16.f;
    lastFrame = now;

    if (displayChanged) {
      displayChanged = false;
      RefreshMonitors();
    }
    if (mons.empty()) RefreshMonitors();

    POINT p;
    if (GetCursorPos(&p)) cur = p;
    curMon = MonitorIndexAt(cur);

    const bool wantDim = effect == Effect::Spotlight || effect == Effect::Squarelight;
    const bool wantLens = effect == Effect::Magnifier;
    const bool wantDot = effect == Effect::Laser || effect == Effect::Draw;
    const bool penDown = effect == Effect::Draw;
    if (wantDim) dimRect = effect == Effect::Squarelight;
    if (wantDot) dotKind = penDown ? 1 : 0;

    if (hasPendingToast) {
      hasPendingToast = false;
      RenderToast(pendingToast);
    }

    // Magnifier needs a clean screenshot before it shows.
    if (wantLens && (!snapValid || snapMon != mons[curMon].h)) CaptureForLens(curMon);

    // Spotlight / squarelight: only the monitor under the pointer is dimmed.
    for (size_t i = 0; i < mons.size(); ++i) {
      const bool here = int(i) == curMon;
      const float target = (wantDim && here) ? 1.f : 0.f;
      if (target > 0.f || (dims[i] && dims[i]->alpha > 0.f)) {
        UpdateDim(i, here);
        dims[i]->target = target;
      }
    }

    lens.target = (wantLens && snapValid) ? 1.f : 0.f;
    if (lens.target > 0.f || lens.alpha > 0.f) UpdateLens(wantLens);

    dot.target = wantDot ? 1.f : 0.f;
    if (dot.target > 0.f || dot.alpha > 0.f) UpdateDot(wantDot);

    // Ink: draw while the pen is down; strokes stay until cleared.
    if (penDown && inkClearing) FinishInkClear();
    if (penDown && !penWasDown) {
      stroke.clear();
      AddInkPoint(cur);
      SeedHistory(cur);
    } else if (penDown) {
      for (POINT q : MoveHistory(cur)) AddInkPoint(q);
      AddInkPoint(cur);
    } else if (penWasDown) {
      EndStroke();
    }
    penWasDown = penDown;
    for (auto& k : inks)
      if (k) k->target = (k->hasInk && !inkClearing) ? 1.f : 0.f;

    toast.target = (toastUntil > now) ? 1.f : 0.f;

    // Fade + present, bottom to top.
    for (auto& d : dims)
      if (d) Step(*d, dt);
    for (auto& k : inks)
      if (k) Step(*k, dt);
    Step(lens, dt);
    Step(dot, dt);
    Step(toast, dt);

    if (inkClearing) {
      bool done = true;
      for (auto& k : inks)
        if (k && k->alpha > 0.f) done = false;
      if (done) FinishInkClear();
    }
    if (!wantLens && lens.alpha <= 0.f && snapValid) {
      snap.Destroy();
      snapValid = false;
      snapMon = nullptr;
    }
    if (restackNeeded || now - lastRestack > 750) Restack();
    ++statFrames;
    statWork += NowMs() - now;
  }

  void LogStats() {
    if (statFrames < 2) {
      statFrames = 0;
      return;
    }
    Log("overlay: %d frames in %.0f ms (avg %.1f ms/frame, worst gap %.1f ms, work %.2f ms/frame)",
        statFrames, statLast - statFirst, (statLast - statFirst) / (statFrames - 1), statMaxGap,
        statWork / statFrames);
    statFrames = 0;
    statWork = statMaxGap = 0;
  }

  void IdleMaintenance() {
    const double now = NowMs();
    // Give memory back when the effects have not been used for a while.
    for (auto& d : dims)
      if (d && !d->visible && d->img.Valid() && now - d->lastUse > 60000) {
        d->img.Destroy();
        d->holeValid = false;
      }
    if (now - lastBusy > 60000) {
      tiles.clear();
      lens.img.Destroy();
      lensCache.frame.Destroy();
      lensCache.mask.clear();
      lensCache.L = lensCache.R = 0;
      lensDirty = true;
    }
    bool inkVisible = false;
    for (auto& k : inks)
      if (k && k->visible) inkVisible = true;
    if (inkVisible && now - lastRestack > 750) Restack();
  }

  void Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    vb.Init();
    bool wasBusy = false;
    while (true) {
      Consume();
      if (quit) break;
      const double now = NowMs();
      if (Busy(now)) {
        if (wasBusy) vb.Wait(mons.empty() ? nullptr : mons[curMon].h,
                             mons.empty() ? 60 : mons[curMon].hz);
        Pump();
        Consume();
        if (quit) break;
        Frame();
        wasBusy = true;
        lastBusy = NowMs();
      } else {
        if (wasBusy) LogStats();
        wasBusy = false;
        bool inkVisible = false;
        for (auto& k : inks)
          if (k && k->visible) inkVisible = true;
        const DWORD timeout = inkVisible ? 750 : 20000;
        const DWORD r = MsgWaitForMultipleObjects(1, &wake, FALSE, timeout, QS_ALLINPUT);
        Pump();
        if (r == WAIT_TIMEOUT) IdleMaintenance();
      }
    }
    for (auto& d : dims)
      if (d) DestroyLayer(*d);
    for (auto& k : inks)
      if (k) DestroyLayer(*k);
    DestroyLayer(lens);
    DestroyLayer(dot);
    DestroyLayer(toast);
    vb.Shutdown();
  }
};

bool Overlay::Start() {
  if (impl_) return true;
  impl_ = new Impl();
  impl_->owner = this;
  impl_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  impl_->th = std::thread([this] { impl_->Run(); });
  return true;
}

void Overlay::Stop() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->sQuit = true;
  }
  SetEvent(impl_->wake);
  if (impl_->th.joinable()) impl_->th.join();
  CloseHandle(impl_->wake);
  delete impl_;
  impl_ = nullptr;
}

void Overlay::SetParams(const OverlayParams& p) {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->sParams = p;
    impl_->sParamsDirty = true;
  }
  SetEvent(impl_->wake);
}

void Overlay::SetEffect(Effect e) {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (impl_->sEffect == e) return;
    impl_->sEffect = e;
  }
  SetEvent(impl_->wake);
}

void Overlay::ClearInk() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->sClearInk = true;
  }
  SetEvent(impl_->wake);
}

void Overlay::Toast(const std::wstring& text) {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->sToast = text;
    impl_->sToastDirty = true;
  }
  SetEvent(impl_->wake);
}

void Overlay::DisplayChanged() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->sDisplayChanged = true;
  }
  SetEvent(impl_->wake);
}
