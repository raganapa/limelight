// SPDX-License-Identifier: MIT
#include "ui.h"

#include <map>
#include <memory>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "../res/resource.h"

using Gdiplus::Color;
using Gdiplus::Font;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::RectF;
using Gdiplus::SolidBrush;
using Gdiplus::StringAlignment;
using Gdiplus::StringAlignmentCenter;
using Gdiplus::StringAlignmentFar;
using Gdiplus::StringAlignmentNear;
using Gdiplus::StringFormat;

namespace {

constexpr wchar_t kClass[] = L"LimelightSettings";
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

// ---------------------------------------------------------------- theme
struct Theme {
  bool dark = false;
  Color bg, card, cardBorder, text, sub, track, accent, accentText, hover, pressed, ok, warn, idle;
};

Color Rgb(uint32_t rgb, BYTE a = 255) {
  return Color(a, BYTE(rgb >> 16), BYTE(rgb >> 8), BYTE(rgb));
}

uint32_t Mix(uint32_t a, uint32_t b, float t) {
  auto ch = [&](int s) {
    return uint32_t(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t + 0.5f) << s;
  };
  return ch(16) | ch(8) | ch(0);
}

bool SystemDark() {
  DWORD v = 1, sz = sizeof v;
  RegGetValueW(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
  return v == 0;
}

uint32_t SystemAccent() {
  DWORD c = 0;
  BOOL opaque = FALSE;
  if (SUCCEEDED(DwmGetColorizationColor(&c, &opaque))) return c & 0xFFFFFF;
  return 0x0067C0;
}

Theme MakeTheme() {
  Theme t;
  t.dark = SystemDark();
  uint32_t acc = SystemAccent();
  // Keep the accent readable: not too dark on dark, not too light on light.
  auto lum = [](uint32_t c) {
    return 0.2126f * ((c >> 16) & 255) + 0.7152f * ((c >> 8) & 255) + 0.0722f * (c & 255);
  };
  if (t.dark && lum(acc) < 110) acc = Mix(acc, 0xFFFFFF, 0.35f);
  if (!t.dark && lum(acc) > 170) acc = Mix(acc, 0x000000, 0.35f);
  if (t.dark) {
    t.bg = Rgb(0x202020);
    t.card = Rgb(0x2B2B2B);
    t.cardBorder = Rgb(0x363636);
    t.text = Rgb(0xFFFFFF);
    t.sub = Rgb(0xA8A8A8);
    t.track = Rgb(0x4E4E4E);
    t.hover = Rgb(0xFFFFFF, 14);
    t.pressed = Rgb(0xFFFFFF, 24);
  } else {
    t.bg = Rgb(0xF3F3F3);
    t.card = Rgb(0xFFFFFF);
    t.cardBorder = Rgb(0xE3E3E3);
    t.text = Rgb(0x1B1B1B);
    t.sub = Rgb(0x616161);
    t.track = Rgb(0xC9C9C9);
    t.hover = Rgb(0x000000, 10);
    t.pressed = Rgb(0x000000, 20);
  }
  t.accent = Rgb(acc);
  t.accentText = lum(acc) > 150 ? Rgb(0x000000) : Rgb(0xFFFFFF);
  t.ok = Rgb(0x3CC86A);
  t.warn = Rgb(0xF2B233);
  t.idle = Rgb(0x8A8A8A);
  return t;
}

// ---------------------------------------------------------------- widgets
enum class W { Header, Section, CardBegin, CardEnd, Segmented, Slider, Toggle, Swatches, Buttons, Text };

struct Widget {
  W type = W::Text;
  int id = 0;
  std::wstring label, sub;
  std::vector<std::wstring> options;  // segmented / buttons
  std::vector<uint32_t> colors;       // swatches
  int minV = 0, maxV = 100;
  std::function<int()> get;
  std::function<void(int)> set;
  std::function<std::wstring(int)> fmt;
  std::function<bool()> enabled;
  std::function<void(int)> click;     // buttons
  Effect preview = Effect::None;      // slider live preview
  // layout (DIP)
  float y = 0, h = 0;
  // derived control rects (DIP, content coordinates)
  RectF ctrl;
  std::vector<RectF> parts;
};

struct Ui {
  HWND hwnd = nullptr;
  HINSTANCE inst = nullptr;
  UiHost* host = nullptr;
  Theme th;
  std::vector<Widget> ws;
  float dpi = 96.f;
  float contentH = 0;
  float scroll = 0, scrollTarget = 0;
  int hover = -1, hoverPart = -1;
  int pressed = -1, pressedPart = -1;
  int dragging = -1;  // slider index
  bool draggingScroll = false;
  float scrollGrabY = 0, scrollGrabStart = 0;
  std::map<int, float> toggleAnim;  // widget index -> knob position 0..1
  bool animating = false;
  HBITMAP back = nullptr;
  HDC backDc = nullptr;
  int backW = 0, backH = 0;
  bool closedOnce = false;
};

Ui* g_ui = nullptr;

constexpr float kWidth = 700.f;   // DIP
constexpr float kMargin = 24.f;
constexpr float kRowH = 56.f;
constexpr float kCardPad = 18.f;

float K() { return g_ui->dpi / 96.f; }

const wchar_t* kFont = L"Segoe UI";
const wchar_t* kFontSemi = L"Segoe UI Semibold";

std::unique_ptr<Font> MakeFont(const wchar_t* fam, float sizeDip, int style = Gdiplus::FontStyleRegular) {
  const float px = sizeDip * K();
  const bool semi = wcscmp(fam, kFontSemi) == 0;
  const wchar_t* chain[] = {fam, L"Segoe UI", L"Tahoma", L"Arial", L"DejaVu Sans"};
  for (const wchar_t* c : chain) {
    // Without a Semibold face, fall back to bold of the regular family.
    const int st = (semi && c != fam) ? (style | Gdiplus::FontStyleBold) : style;
    Gdiplus::FontFamily ff(c);
    if (ff.GetLastStatus() == Gdiplus::Ok && ff.IsStyleAvailable(st))
      return std::make_unique<Font>(&ff, px, st, Gdiplus::UnitPixel);
  }
  return std::make_unique<Font>(Gdiplus::FontFamily::GenericSansSerif(), px, style,
                                Gdiplus::UnitPixel);
}

void RoundPath(GraphicsPath& p, RectF r, float rad) {
  rad = std::min({rad, r.Width / 2, r.Height / 2});
  if (rad <= 0.5f) {
    p.AddRectangle(r);
    return;
  }
  const float d = rad * 2;
  p.AddArc(r.X, r.Y, d, d, 180, 90);
  p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  p.CloseFigure();
}

void FillRound(Graphics& g, RectF r, float rad, const Color& c) {
  GraphicsPath p;
  RoundPath(p, r, rad);
  SolidBrush b(c);
  g.FillPath(&b, &p);
}

void StrokeRound(Graphics& g, RectF r, float rad, const Color& c, float w) {
  GraphicsPath p;
  RoundPath(p, r, rad);
  Pen pen(c, w);
  g.DrawPath(&pen, &p);
}

void Text(Graphics& g, const std::wstring& s, const Font& f, RectF r, const Color& c,
          StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter,
          bool wrap = false) {
  StringFormat sf;
  sf.SetAlignment(h);
  sf.SetLineAlignment(v);
  if (!wrap) {
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  }
  SolidBrush b(c);
  g.DrawString(s.c_str(), -1, &f, r, &sf, &b);
}

float MeasureW(Graphics& g, const std::wstring& s, const Font& f) {
  RectF out;
  g.MeasureString(s.c_str(), -1, &f, PointF(0, 0), &out);
  return out.Width;
}

// DIP rect -> pixel rect, applying scroll
RectF Px(RectF r) {
  const float k = K();
  return RectF(r.X * k, (r.Y - g_ui->scroll) * k, r.Width * k, r.Height * k);
}

// ---------------------------------------------------------------- content
std::wstring Pct(int v) { return std::to_wstring(v) + L"%"; }
std::wstring Pxs(int v) { return std::to_wstring(v) + L" px"; }

void Build() {
  Ui& u = *g_ui;
  UiHost* h = u.host;
  Settings& s = h->Prefs();
  std::vector<Widget> ws;
  auto add = [&](Widget w) { ws.push_back(std::move(w)); };

  Widget hdr;
  hdr.type = W::Header;
  add(hdr);

  auto section = [&](const wchar_t* t) {
    Widget w;
    w.type = W::Section;
    w.label = t;
    add(w);
  };
  auto cardBegin = [&] { Widget w; w.type = W::CardBegin; add(w); };
  auto cardEnd = [&] { Widget w; w.type = W::CardEnd; add(w); };

  const std::vector<std::wstring> effects = {L"Spotlight", L"Square", L"Zoom", L"Laser", L"Draw", L"Off"};
  auto effToIdx = [](Effect e) {
    switch (e) {
      case Effect::Spotlight: return 0;
      case Effect::Squarelight: return 1;
      case Effect::Magnifier: return 2;
      case Effect::Laser: return 3;
      case Effect::Draw: return 4;
      default: return 5;
    }
  };
  auto idxToEff = [](int i) {
    const Effect m[] = {Effect::Spotlight, Effect::Squarelight, Effect::Magnifier,
                        Effect::Laser, Effect::Draw, Effect::None};
    return m[Clamp(i, 0, 5)];
  };

  section(L"Highlight button");
  cardBegin();
  {
    Widget w;
    w.type = W::Segmented;
    w.label = L"Light press";
    w.sub = L"Hold the button lightly";
    w.options = effects;
    w.get = [&s, effToIdx] { return effToIdx(s.softPress); };
    w.set = [&s, h, idxToEff](int i) { s.softPress = idxToEff(i); h->PrefsChanged(); };
    add(w);
  }
  {
    Widget w;
    w.type = W::Segmented;
    w.label = L"Hard press";
    w.sub = L"Press it all the way";
    w.options = effects;
    w.get = [&s, effToIdx] { return effToIdx(s.hardPress); };
    w.set = [&s, h, idxToEff](int i) { s.hardPress = idxToEff(i); h->PrefsChanged(); };
    add(w);
  }
  {
    Widget w;
    w.type = W::Segmented;
    w.label = L"Action button";
    w.sub = L"The small button";
    w.options = {L"Default", L"Next effect", L"Black screen", L"Clear drawing"};
    w.get = [&s] { return int(s.actionButton); };
    w.set = [&s, h](int i) { s.actionButton = ActionMode(i); h->PrefsChanged(); };
    w.enabled = [h] { return h->HasActionButton(); };
    add(w);
  }
  {
    Widget w;
    w.type = W::Toggle;
    w.label = L"Real laser";
    w.sub = s.laserKnown ? L"Fires the laser diode at the tip"
                         : L"Run the Laser finder once to switch it on";
    w.get = [&s] { return s.physicalLaser ? 1 : 0; };
    w.set = [&s, h](int v) { s.physicalLaser = v != 0; h->PrefsChanged(); };
    w.enabled = [&s] { return s.laserKnown; };
    add(w);
  }
  cardEnd();

  section(L"Look");
  cardBegin();
  auto slider = [&](const wchar_t* label, int mn, int mx, int* field,
                    std::function<std::wstring(int)> fmt, Effect prev) {
    Widget w;
    w.type = W::Slider;
    w.label = label;
    w.minV = mn;
    w.maxV = mx;
    w.get = [field] { return *field; };
    w.set = [field, h](int v) { *field = v; h->PrefsChanged(); };
    w.fmt = std::move(fmt);
    w.preview = prev;
    add(w);
  };
  slider(L"Spotlight size", 80, 500, &s.spotRadius, Pxs, Effect::Spotlight);
  slider(L"Dim the screen", 20, 95, &s.dimPercent, Pct, Effect::Spotlight);
  slider(L"Soft edge", 0, 100, &s.softness, Pct, Effect::Spotlight);
  slider(L"Square width", 200, 1200, &s.squareWidth, Pxs, Effect::Squarelight);
  slider(L"Zoom", 15, 50, &s.zoomTenths,
         [](int v) {
           wchar_t b[16];
           swprintf(b, 16, L"%.1f×", v / 10.0);
           return std::wstring(b);
         },
         Effect::Magnifier);
  slider(L"Lens size", 80, 400, &s.lensRadius, Pxs, Effect::Magnifier);
  const std::vector<uint32_t> laserColors = {0xFF2E2E, 0x2EE65A, 0x2E9BFF, 0xFFD52E, 0xFF2ED5, 0xFFFFFF};
  const std::vector<uint32_t> penColors = {0xFF3B30, 0xFFCC00, 0x34C759, 0x0A84FF, 0xFFFFFF, 0x111111};
  {
    Widget w;
    w.type = W::Swatches;
    w.label = L"Laser colour";
    w.colors = laserColors;
    w.get = [&s] { return int(s.laserColor); };
    w.set = [&s, h](int v) { s.laserColor = uint32_t(v); h->PrefsChanged(); };
    add(w);
  }
  slider(L"Laser size", 6, 30, &s.laserSize, Pxs, Effect::Laser);
  {
    Widget w;
    w.type = W::Swatches;
    w.label = L"Pen colour";
    w.colors = penColors;
    w.get = [&s] { return int(s.penColor); };
    w.set = [&s, h](int v) { s.penColor = uint32_t(v); h->PrefsChanged(); };
    add(w);
  }
  slider(L"Pen thickness", 2, 24, &s.penWidth, Pxs, Effect::None);
  cardEnd();

  section(L"Behaviour");
  cardBegin();
  auto toggle = [&](const wchar_t* label, const wchar_t* sub, bool* field) {
    Widget w;
    w.type = W::Toggle;
    w.label = label;
    w.sub = sub ? sub : L"";
    w.get = [field] { return *field ? 1 : 0; };
    w.set = [field, h](int v) { *field = v != 0; h->PrefsChanged(); };
    add(w);
  };
  toggle(L"Hide the pointer during effects", L"The effect shows where you point", &s.hideCursor);
  toggle(L"Start in the middle of the screen", L"After a pause, effects begin at the centre",
         &s.recenter);
  toggle(L"Clear drawings on slide change", L"Next, Back or Esc wipes the ink", &s.clearInkOnSlide);
  {
    Widget w;
    w.type = W::Segmented;
    w.label = L"Vibration";
    w.sub = L"Haptic feedback in the remote";
    w.options = {L"Off", L"Hard press", L"Every press"};
    w.get = [&s] { return int(s.haptics); };
    w.set = [&s, h](int i) { s.haptics = HapticMode(i); h->PrefsChanged(); };
    add(w);
  }
  {
    Widget w;
    w.type = W::Slider;
    w.label = L"Vibration strength";
    w.minV = 0;
    w.maxV = 100;
    w.get = [&s] { return s.hapticStrength; };
    w.set = [&s, h](int v) { s.hapticStrength = v; h->PrefsChanged(); };
    w.fmt = Pct;
    w.enabled = [&s] { return s.haptics != HapticMode::Off; };
    add(w);
  }
  {
    Widget w;
    w.type = W::Toggle;
    w.label = L"Start with Windows";
    w.sub = L"Runs quietly in the tray";
    w.get = [h] { return h->Autostart() ? 1 : 0; };
    w.set = [h](int v) { h->SetAutostartPref(v != 0); };
    add(w);
  }
  cardEnd();

  section(L"How to use");
  cardBegin();
  {
    Widget w;
    w.type = W::Text;
    w.label =
        L"Hold the highlight button lightly for the light-press effect, press harder for the "
        L"hard-press effect, let go and it fades away. Double-tap the highlight button to click "
        L"a link or a video. Next and Back change slides as usual.";
    add(w);
  }
  {
    Widget w;
    w.type = W::Text;
    w.label =
        L"Without the remote: Ctrl+Alt+F1 Spotlight · F2 Square · F3 Zoom · "
        L"F4 Laser · F5 Draw · F6 Clear drawing · Esc stops.";
    add(w);
  }
  {
    Widget w;
    w.type = W::Buttons;
    w.options = {L"Try the spotlight", L"Laser finder & diagnostics"};
    w.click = [h](int i) {
      if (i == 0) h->TryEffect(Effect::Spotlight);
      else h->OpenDiagnostics();
    };
    add(w);
  }
  cardEnd();

  u.ws = std::move(ws);
}

// Layout in DIP. Uses a scratch Graphics for text measurement.
void Layout(Graphics& g) {
  Ui& u = *g_ui;
  float y = kMargin;
  const float left = kMargin, right = kWidth - kMargin;
  auto font = MakeFont(kFont, 13.f);
  for (auto& w : u.ws) {
    w.y = y;
    switch (w.type) {
      case W::Header:
        w.h = 72;
        break;
      case W::Section:
        w.h = 38;
        break;
      case W::CardBegin:
        w.h = 6;
        break;
      case W::CardEnd:
        w.h = 6 + 4;
        break;
      case W::Text: {
        const float tw = (right - left - 2 * kCardPad) * K();
        RectF out;
        g.MeasureString(w.label.c_str(), -1, font.get(), RectF(0, 0, tw, 2000), &out);
        w.h = out.Height / K() + 14;
        break;
      }
      case W::Buttons:
        w.h = 58;
        break;
      default:
        w.h = kRowH;
        break;
    }
    // control rects
    const float cy = y + w.h / 2;
    w.parts.clear();
    switch (w.type) {
      case W::Segmented: {
        const float segH = 32;
        // width from text
        float total = 0;
        std::vector<float> widths;
        for (auto& o : w.options) {
          const float ow = MeasureW(g, o, *font) / K() + 22;
          widths.push_back(ow);
          total += ow;
        }
        const float x1 = right - kCardPad;
        const float x0 = x1 - total;
        w.ctrl = RectF(x0, cy - segH / 2, total, segH);
        float x = x0;
        for (float ow : widths) {
          w.parts.push_back(RectF(x, cy - segH / 2, ow, segH));
          x += ow;
        }
        break;
      }
      case W::Slider: {
        const float x1 = right - kCardPad - 64;
        const float x0 = x1 - 250;
        w.ctrl = RectF(x0, cy - 12, x1 - x0, 24);
        break;
      }
      case W::Toggle:
        w.ctrl = RectF(right - kCardPad - 44, cy - 11, 44, 22);
        break;
      case W::Swatches: {
        const float d = 26, gap = 10;
        const float total = w.colors.size() * d + (w.colors.size() - 1) * gap;
        float x = right - kCardPad - 64 - total + 0;
        w.ctrl = RectF(x, cy - d / 2, total, d);
        for (size_t i = 0; i < w.colors.size(); ++i) {
          w.parts.push_back(RectF(x, cy - d / 2, d, d));
          x += d + gap;
        }
        break;
      }
      case W::Buttons: {
        float x = left + kCardPad;
        for (auto& o : w.options) {
          const float bw = MeasureW(g, o, *font) / K() + 36;
          w.parts.push_back(RectF(x, cy - 17, bw, 34));
          x += bw + 10;
        }
        break;
      }
      default:
        break;
    }
    y += w.h;
  }
  u.contentH = y + kMargin;
}

float ViewH() {
  RECT rc;
  GetClientRect(g_ui->hwnd, &rc);
  return (rc.bottom - rc.top) / K();
}

float MaxScroll() { return std::max(0.f, g_ui->contentH - ViewH()); }

bool Enabled(const Widget& w) { return !w.enabled || w.enabled(); }

void StartAnim() {
  if (!g_ui->animating) {
    g_ui->animating = true;
    SetTimer(g_ui->hwnd, 1, 15, nullptr);
  }
}

// ---------------------------------------------------------------- paint
void Paint(HDC dc) {
  Ui& u = *g_ui;
  RECT rc;
  GetClientRect(u.hwnd, &rc);
  const int W_ = rc.right, H_ = rc.bottom;
  if (W_ <= 0 || H_ <= 0) return;
  if (!u.back || u.backW != W_ || u.backH != H_) {
    if (u.back) {
      DeleteDC(u.backDc);
      DeleteObject(u.back);
    }
    u.back = CreateCompatibleBitmap(dc, W_, H_);
    u.backDc = CreateCompatibleDC(dc);
    SelectObject(u.backDc, u.back);
    u.backW = W_;
    u.backH = H_;
  }
  Graphics g(u.backDc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
  const Theme& t = u.th;
  g.Clear(t.bg);

  const float k = K();
  auto fBody = MakeFont(kFont, 14.f);
  auto fSmall = MakeFont(kFont, 12.f);
  auto fCtl = MakeFont(kFont, 13.f);
  auto fSection = MakeFont(kFontSemi, 14.f);
  auto fTitle = MakeFont(kFontSemi, 24.f);
  const float left = kMargin, right = kWidth - kMargin;

  // cards: find begin/end pairs and paint backgrounds first
  for (size_t i = 0; i < u.ws.size(); ++i) {
    if (u.ws[i].type != W::CardBegin) continue;
    size_t j = i;
    while (j < u.ws.size() && u.ws[j].type != W::CardEnd) ++j;
    if (j >= u.ws.size()) break;
    const float y0 = u.ws[i].y, y1 = u.ws[j].y + 6;
    RectF r = Px(RectF(left, y0, right - left, y1 - y0));
    FillRound(g, r, 8 * k, t.card);
    StrokeRound(g, r, 8 * k, t.cardBorder, 1.f);
    // separators between rows
    auto isRow = [](W t) {
      return t == W::Segmented || t == W::Slider || t == W::Toggle || t == W::Swatches;
    };
    for (size_t m = i + 1; m + 1 < j; ++m) {
      const float sy = u.ws[m].y + u.ws[m].h;
      if (!isRow(u.ws[m].type) || !isRow(u.ws[m + 1].type)) continue;
      Pen sep(t.cardBorder, 1.f);
      RectF a = Px(RectF(left + kCardPad, sy, right - left - 2 * kCardPad, 0));
      g.DrawLine(&sep, a.X, a.Y, a.X + a.Width, a.Y);
    }
  }

  for (size_t i = 0; i < u.ws.size(); ++i) {
    Widget& w = u.ws[i];
    const float top = (w.y - u.scroll) * k, bottom = (w.y + w.h - u.scroll) * k;
    if (bottom < 0 || top > H_) continue;
    const bool en = Enabled(w);
    const Color& textC = en ? t.text : t.sub;
    RectF labelR = Px(RectF(left + kCardPad, w.y, 260, w.h));
    switch (w.type) {
      case W::Header: {
        RectF ir = Px(RectF(left, w.y + 8, 44, 44));
        HICON ico = static_cast<HICON>(LoadImageW(u.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                   int(ir.Width), int(ir.Height), 0));
        if (ico) {
          HDC hdc = g.GetHDC();
          DrawIconEx(hdc, int(ir.X), int(ir.Y), ico, int(ir.Width), int(ir.Height), 0, nullptr,
                     DI_NORMAL);
          g.ReleaseHDC(hdc);
          DestroyIcon(ico);
        }
        Text(g, L"Limelight", *fTitle, Px(RectF(left + 56, w.y + 4, 300, 30)), t.text);
        // status pill
        const std::wstring st = u.host->StatusLine();
        const int lvl = u.host->StatusLevel();
        const Color dotC = lvl == 1 ? t.ok : (lvl == 2 ? t.warn : t.idle);
        RectF dot = Px(RectF(left + 58, w.y + 41, 9, 9));
        SolidBrush db(dotC);
        g.FillEllipse(&db, dot);
        Text(g, st, *fSmall, Px(RectF(left + 74, w.y + 34, right - left - 80, 22)), t.sub);
        break;
      }
      case W::Section:
        Text(g, w.label, *fSection, Px(RectF(left + 2, w.y + 12, 400, 24)), t.text);
        break;
      case W::Text: {
        RectF r = Px(RectF(left + kCardPad, w.y + 7, right - left - 2 * kCardPad, w.h - 7));
        Text(g, w.label, *fCtl, r, t.sub, StringAlignmentNear, StringAlignmentNear, true);
        break;
      }
      case W::Segmented:
      case W::Slider:
      case W::Toggle:
      case W::Swatches: {
        if (!w.sub.empty()) {
          Text(g, w.label, *fBody, Px(RectF(left + kCardPad, w.y + 8, 300, 22)), textC);
          Text(g, w.sub, *fSmall, Px(RectF(left + kCardPad, w.y + 29, 300, 18)), t.sub);
        } else {
          Text(g, w.label, *fBody, labelR, textC);
        }
        break;
      }
      default:
        break;
    }

    if (w.type == W::Segmented) {
      RectF r = Px(w.ctrl);
      FillRound(g, r, 6 * k, t.dark ? Rgb(0x1F1F1F) : Rgb(0xEDEDED));
      StrokeRound(g, r, 6 * k, t.cardBorder, 1.f);
      const int val = w.get ? w.get() : -1;
      for (size_t p = 0; p < w.parts.size(); ++p) {
        RectF pr = Px(w.parts[p]);
        const bool sel = int(p) == val;
        if (sel) {
          RectF in(pr.X + 2 * k, pr.Y + 2 * k, pr.Width - 4 * k, pr.Height - 4 * k);
          FillRound(g, in, 5 * k, en ? t.accent : t.track);
        } else if (en && u.hover == int(i) && u.hoverPart == int(p)) {
          RectF in(pr.X + 2 * k, pr.Y + 2 * k, pr.Width - 4 * k, pr.Height - 4 * k);
          FillRound(g, in, 5 * k, t.hover);
        }
        Text(g, w.options[p], *fCtl, pr, sel ? (en ? t.accentText : t.text) : textC,
             StringAlignmentCenter);
      }
    } else if (w.type == W::Slider) {
      const int v = w.get ? w.get() : 0;
      const float frac = float(v - w.minV) / float(std::max(1, w.maxV - w.minV));
      RectF r = Px(w.ctrl);
      const float ty = r.Y + r.Height / 2;
      const float th = 4 * k;
      FillRound(g, RectF(r.X, ty - th / 2, r.Width, th), th / 2, t.track);
      FillRound(g, RectF(r.X, ty - th / 2, r.Width * frac, th), th / 2, en ? t.accent : t.sub);
      const float kx = r.X + r.Width * frac;
      const bool hot = u.hover == int(i) || u.dragging == int(i);
      const float kr = (hot && en ? 10.f : 9.f) * k;
      SolidBrush outer(t.dark ? Rgb(0x454545) : Rgb(0xFFFFFF));
      g.FillEllipse(&outer, kx - kr, ty - kr, 2 * kr, 2 * kr);
      Pen ring(t.cardBorder, 1.f);
      g.DrawEllipse(&ring, kx - kr, ty - kr, 2 * kr, 2 * kr);
      const float ir = (u.dragging == int(i) ? 4.5f : (hot ? 6.f : 5.f)) * k;
      SolidBrush inner(en ? t.accent : t.sub);
      g.FillEllipse(&inner, kx - ir, ty - ir, 2 * ir, 2 * ir);
      const std::wstring val = w.fmt ? w.fmt(v) : std::to_wstring(v);
      Text(g, val, *fCtl, Px(RectF(w.ctrl.X + w.ctrl.Width + 10, w.y, 60, w.h)), t.sub,
           StringAlignmentFar);
    } else if (w.type == W::Toggle) {
      const int v = w.get ? w.get() : 0;
      float pos = float(v);
      auto it = u.toggleAnim.find(int(i));
      if (it != u.toggleAnim.end()) pos = it->second;
      RectF r = Px(w.ctrl);
      const Color on = en ? t.accent : t.track;
      if (pos > 0.01f) {
        FillRound(g, r, r.Height / 2, on);
      }
      if (pos < 0.99f) {
        const BYTE a = BYTE((1 - pos) * 255);
        FillRound(g, r, r.Height / 2, Color(a, t.card.GetR(), t.card.GetG(), t.card.GetB()));
        StrokeRound(g, r, r.Height / 2, Color(a, t.sub.GetR(), t.sub.GetG(), t.sub.GetB()),
                    1.2f * k);
      }
      const float kr = (u.hover == int(i) && en ? 6.5f : 6.f) * k;
      const float x0 = r.X + r.Height / 2, x1 = r.X + r.Width - r.Height / 2;
      const float kx = x0 + (x1 - x0) * pos;
      const float ky = r.Y + r.Height / 2;
      const Color knob = pos > 0.5f ? (en ? t.accentText : t.card) : t.sub;
      SolidBrush kb(knob);
      g.FillEllipse(&kb, kx - kr, ky - kr, 2 * kr, 2 * kr);
    } else if (w.type == W::Swatches) {
      const uint32_t cur = w.get ? uint32_t(w.get()) : 0;
      for (size_t p = 0; p < w.parts.size(); ++p) {
        RectF pr = Px(w.parts[p]);
        const bool sel = w.colors[p] == cur;
        if (sel) {
          Pen sp(t.accent, 2.f * k);
          g.DrawEllipse(&sp, pr.X - 3 * k, pr.Y - 3 * k, pr.Width + 6 * k, pr.Height + 6 * k);
        } else if (u.hover == int(i) && u.hoverPart == int(p)) {
          Pen sp(t.sub, 1.f * k);
          g.DrawEllipse(&sp, pr.X - 3 * k, pr.Y - 3 * k, pr.Width + 6 * k, pr.Height + 6 * k);
        }
        SolidBrush b(Rgb(w.colors[p]));
        g.FillEllipse(&b, pr);
        Pen edge(Color(60, 0, 0, 0), 1.f);
        g.DrawEllipse(&edge, pr);
      }
    } else if (w.type == W::Buttons) {
      for (size_t p = 0; p < w.parts.size(); ++p) {
        RectF pr = Px(w.parts[p]);
        const bool primary = p == 0;
        const bool hot = u.hover == int(i) && u.hoverPart == int(p);
        const bool down = u.pressed == int(i) && u.pressedPart == int(p);
        if (primary) {
          FillRound(g, pr, 6 * k, t.accent);
          if (hot) FillRound(g, pr, 6 * k, Color(down ? 50 : 28, 255, 255, 255));
          Text(g, w.options[p], *fCtl, pr, t.accentText, StringAlignmentCenter);
        } else {
          FillRound(g, pr, 6 * k, t.dark ? Rgb(0x373737) : Rgb(0xFBFBFB));
          StrokeRound(g, pr, 6 * k, t.cardBorder, 1.f);
          if (hot) FillRound(g, pr, 6 * k, down ? t.pressed : t.hover);
          Text(g, w.options[p], *fCtl, pr, t.text, StringAlignmentCenter);
        }
      }
    }
  }

  // footer
  {
    const float y = u.contentH - kMargin - 4;
    Text(g, L"Limelight " APP_VERSION L" · open source · changes are saved automatically",
         *fSmall, Px(RectF(left, y, right - left, 20)), t.sub, StringAlignmentCenter);
  }

  // overlay scrollbar
  const float maxS = MaxScroll();
  if (maxS > 0) {
    const float vh = ViewH();
    const float thumbH = std::max(40.f, vh * vh / u.contentH);
    const float ty = (vh - thumbH) * (u.scroll / maxS);
    const bool hot = u.draggingScroll || u.hover == -2;
    const float wdt = hot ? 8.f : 4.f;
    RectF r((kWidth - wdt - 3) * k, (ty + 2) * k, wdt * k, (thumbH - 4) * k);
    FillRound(g, r, wdt * k / 2, Color(hot ? 150 : 90, t.sub.GetR(), t.sub.GetG(), t.sub.GetB()));
  }

  BitBlt(dc, 0, 0, W_, H_, u.backDc, 0, 0, SRCCOPY);
}

// ---------------------------------------------------------------- input
struct Hit {
  int idx = -1, part = -1;
};

Hit HitTest(float xd, float yd) {  // DIP, content coordinates
  Hit h;
  for (size_t i = 0; i < g_ui->ws.size(); ++i) {
    const Widget& w = g_ui->ws[i];
    auto in = [&](const RectF& r, float grow = 0) {
      return xd >= r.X - grow && xd <= r.X + r.Width + grow && yd >= r.Y - grow &&
             yd <= r.Y + r.Height + grow;
    };
    switch (w.type) {
      case W::Segmented:
      case W::Swatches:
      case W::Buttons:
        for (size_t p = 0; p < w.parts.size(); ++p)
          if (in(w.parts[p], w.type == W::Swatches ? 4.f : 0.f)) return Hit{int(i), int(p)};
        break;
      case W::Slider:
        if (in(RectF(w.ctrl.X - 8, w.y + 6, w.ctrl.Width + 16, w.h - 12))) return Hit{int(i), 0};
        break;
      case W::Toggle:
        // the whole row toggles
        if (yd >= w.y && yd <= w.y + w.h && xd >= kMargin && xd <= kWidth - kMargin)
          return Hit{int(i), 0};
        break;
      default:
        break;
    }
  }
  return h;
}

void SetSliderFromX(int idx, float xd) {
  Widget& w = g_ui->ws[size_t(idx)];
  const float f = Clamp((xd - w.ctrl.X) / w.ctrl.Width, 0.f, 1.f);
  const int v = w.minV + int(std::lround(f * (w.maxV - w.minV)));
  if (w.get && w.get() != v && w.set) w.set(v);
}

void ScrollTo(float target) {
  g_ui->scrollTarget = Clamp(target, 0.f, MaxScroll());
  StartAnim();
}

void Relayout() {
  HDC dc = GetDC(g_ui->hwnd);
  {
    Graphics g(dc);
    Layout(g);
  }
  ReleaseDC(g_ui->hwnd, dc);
  g_ui->scroll = Clamp(g_ui->scroll, 0.f, MaxScroll());
  g_ui->scrollTarget = Clamp(g_ui->scrollTarget, 0.f, MaxScroll());
}

void ApplyTheme() {
  g_ui->th = MakeTheme();
  BOOL dark = g_ui->th.dark;
  DwmSetWindowAttribute(g_ui->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
  COLORREF cap = RGB(g_ui->th.bg.GetR(), g_ui->th.bg.GetG(), g_ui->th.bg.GetB());
  DwmSetWindowAttribute(g_ui->hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof cap);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Ui* u = g_ui;
  if (!u) return DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      Paint(dc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      Relayout();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_DPICHANGED: {
      u->dpi = float(HIWORD(wp));
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      Relayout();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      RECT r{0, 0, LONG(kWidth * u->dpi / 96), LONG(360 * u->dpi / 96)};
      AdjustWindowRectExForDpi(&r, GetWindowLongW(hwnd, GWL_STYLE), FALSE, 0, UINT(u->dpi));
      mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = r.right - r.left;
      mm->ptMinTrackSize.y = r.bottom - r.top;
      return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      ApplyTheme();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_MOUSEWHEEL: {
      const float delta = float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
      ScrollTo(u->scrollTarget - delta * 72.f);
      return 0;
    }
    case WM_KEYDOWN:
      switch (wp) {
        case VK_ESCAPE: DestroyWindow(hwnd); return 0;
        case VK_DOWN: ScrollTo(u->scrollTarget + 48); return 0;
        case VK_UP: ScrollTo(u->scrollTarget - 48); return 0;
        case VK_NEXT: ScrollTo(u->scrollTarget + ViewH() * 0.9f); return 0;
        case VK_PRIOR: ScrollTo(u->scrollTarget - ViewH() * 0.9f); return 0;
        case VK_HOME: ScrollTo(0); return 0;
        case VK_END: ScrollTo(MaxScroll()); return 0;
      }
      break;
    case WM_MOUSEMOVE: {
      const float k = K();
      const float xd = GET_X_LPARAM(lp) / k, yv = GET_Y_LPARAM(lp) / k, yd = yv + u->scroll;
      TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&tme);
      if (u->draggingScroll) {
        const float vh = ViewH();
        const float thumbH = std::max(40.f, vh * vh / u->contentH);
        const float dy = yv - u->scrollGrabY;
        u->scroll = u->scrollTarget =
            Clamp(u->scrollGrabStart + dy * MaxScroll() / std::max(1.f, vh - thumbH), 0.f, MaxScroll());
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (u->dragging >= 0) {
        SetSliderFromX(u->dragging, xd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      Hit h = HitTest(xd, yd);
      int hv = h.idx, hp = h.part;
      if (hv < 0 && MaxScroll() > 0 && xd > kWidth - 16) hv = -2;
      if (hv != u->hover || hp != u->hoverPart) {
        u->hover = hv;
        u->hoverPart = hp;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      SetCursor(LoadCursorW(nullptr, (hv >= 0 && Enabled(u->ws[size_t(hv)])) ? IDC_HAND : IDC_ARROW));
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
      break;
    case WM_MOUSELEAVE:
      if (u->hover != -1) {
        u->hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      const float k = K();
      const float xd = GET_X_LPARAM(lp) / k, yv = GET_Y_LPARAM(lp) / k, yd = yv + u->scroll;
      SetFocus(hwnd);
      if (MaxScroll() > 0 && xd > kWidth - 16) {
        u->draggingScroll = true;
        u->scrollGrabY = yv;
        u->scrollGrabStart = u->scroll;
        SetCapture(hwnd);
        return 0;
      }
      Hit h = HitTest(xd, yd);
      if (h.idx < 0) return 0;
      Widget& w = u->ws[size_t(h.idx)];
      if (!Enabled(w)) return 0;
      u->pressed = h.idx;
      u->pressedPart = h.part;
      SetCapture(hwnd);
      if (w.type == W::Slider) {
        u->dragging = h.idx;
        SetSliderFromX(h.idx, xd);
        if (w.preview != Effect::None) u->host->Preview(w.preview);
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: {
      const float k = K();
      const float xd = GET_X_LPARAM(lp) / k, yd = GET_Y_LPARAM(lp) / k + u->scroll;
      ReleaseCapture();
      if (u->draggingScroll) {
        u->draggingScroll = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      const int pi = u->pressed, pp = u->pressedPart;
      u->pressed = -1;
      if (u->dragging >= 0) {
        if (u->ws[size_t(u->dragging)].preview != Effect::None) u->host->Preview(Effect::None);
        u->dragging = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      Hit h = HitTest(xd, yd);
      if (pi >= 0 && h.idx == pi && h.part == pp) {
        Widget& w = u->ws[size_t(pi)];
        switch (w.type) {
          case W::Segmented:
            if (w.set) w.set(pp);
            break;
          case W::Swatches:
            if (w.set) w.set(int(w.colors[size_t(pp)]));
            break;
          case W::Toggle: {
            const int nv = w.get && w.get() ? 0 : 1;
            u->toggleAnim[pi] = nv ? 0.f : 1.f;
            if (w.set) w.set(nv);
            StartAnim();
            break;
          }
          case W::Buttons:
            if (w.click) w.click(pp);
            break;
          default:
            break;
        }
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (u->dragging >= 0) {
        if (u->ws[size_t(u->dragging)].preview != Effect::None) u->host->Preview(Effect::None);
        u->dragging = -1;
      }
      u->draggingScroll = false;
      return 0;
    case WM_TIMER: {
      bool more = false;
      // smooth scrolling
      const float d = u->scrollTarget - u->scroll;
      if (std::fabs(d) > 0.5f) {
        u->scroll += d * 0.28f;
        more = true;
      } else {
        u->scroll = u->scrollTarget;
      }
      for (auto it = u->toggleAnim.begin(); it != u->toggleAnim.end();) {
        const Widget& w = u->ws[size_t(it->first)];
        const float target = (w.get && w.get()) ? 1.f : 0.f;
        float& p = it->second;
        p += (target > p ? 1.f : -1.f) * 0.14f;
        if ((target >= 1.f && p >= 1.f) || (target <= 0.f && p <= 0.f)) {
          it = u->toggleAnim.erase(it);
        } else {
          more = true;
          ++it;
        }
      }
      if (!more) {
        KillTimer(hwnd, 1);
        u->animating = false;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY: {
      u->host->Preview(Effect::None);
      if (u->back) {
        DeleteDC(u->backDc);
        DeleteObject(u->back);
      }
      g_ui = nullptr;
      delete u;
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void ShowSettingsWindow(HINSTANCE inst, UiHost* host) {
  if (g_ui) {
    Build();
    Relayout();
    InvalidateRect(g_ui->hwnd, nullptr, FALSE);
    ShowWindow(g_ui->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_ui->hwnd);
    return;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);
    registered = true;
  }
  g_ui = new Ui();
  g_ui->inst = inst;
  g_ui->host = host;
  Build();

  // Place it on the monitor with the pointer, sized for its DPI.
  POINT pt;
  GetCursorPos(&pt);
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  UINT dx = 96, dy = 96;
  GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
  g_ui->dpi = float(dx);
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(mon, &mi);
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
  const int wantH = int(820 * dx / 96.f);
  const int workH = mi.rcWork.bottom - mi.rcWork.top;
  RECT r{0, 0, LONG(kWidth * dx / 96.f), std::min<LONG>(wantH, workH - 80)};
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, dx);
  const int ww = r.right - r.left, wh = r.bottom - r.top;
  const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
  const int y = mi.rcWork.top + (workH - wh) / 2;
  HWND h = CreateWindowExW(0, kClass, L"Limelight", style, x, y, ww, wh, nullptr, nullptr, inst,
                           nullptr);
  if (!h) {
    delete g_ui;
    g_ui = nullptr;
    return;
  }
  g_ui->hwnd = h;
  g_ui->dpi = float(GetDpiForWindow(h));
  ApplyTheme();
  Relayout();
  ShowWindow(h, SW_SHOWNORMAL);
  UpdateWindow(h);
  SetForegroundWindow(h);
}

void RefreshSettingsWindow() {
  if (!g_ui) return;
  if (g_ui->dragging >= 0 || g_ui->pressed >= 0 || g_ui->animating) {
    InvalidateRect(g_ui->hwnd, nullptr, FALSE);  // just repaint while the user is interacting
    return;
  }
  // Rebuild so texts that depend on the device (e.g. laser) are current.
  const float keep = g_ui->scroll;
  Build();
  Relayout();
  g_ui->scroll = g_ui->scrollTarget = Clamp(keep, 0.f, MaxScroll());
  InvalidateRect(g_ui->hwnd, nullptr, FALSE);
}

void CloseSettingsWindow() {
  if (g_ui) DestroyWindow(g_ui->hwnd);
}

bool IsSettingsDialogMessage(MSG*) { return false; }
