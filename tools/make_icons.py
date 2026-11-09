#!/usr/bin/env python3
"""Generates res/limelight.ico (app) and res/tray.ico (notification area).

Run from the repository root:  python3 tools/make_icons.py
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "res")

LIME = (205, 255, 64)
LIME_DEEP = (120, 200, 20)


def app_icon(size: int) -> Image.Image:
    s = size * 4  # supersample
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    # rounded-square background with a vertical gradient
    bg = Image.new("RGBA", (s, s))
    top, bottom = (40, 44, 56), (16, 18, 24)
    px = bg.load()
    for y in range(s):
        t = y / (s - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(s):
            px[x, y] = c + (255,)
    mask = Image.new("L", (s, s), 0)
    r = int(s * 0.22)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=255)
    img.paste(bg, (0, 0), mask)

    # soft glow
    glow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    cx, cy, rad = s * 0.5, s * 0.47, s * 0.27
    gd = ImageDraw.Draw(glow)
    gd.ellipse([cx - rad * 1.45, cy - rad * 1.45, cx + rad * 1.45, cy + rad * 1.45],
               fill=LIME + (110,))
    glow = glow.filter(ImageFilter.GaussianBlur(s * 0.07))
    img = Image.alpha_composite(img, Image.composite(glow, Image.new("RGBA", (s, s)), mask))

    # the spotlight disc
    d = ImageDraw.Draw(img)
    d.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], fill=LIME + (255,))
    # bright core
    core = rad * 0.42
    d.ellipse([cx - core - rad * 0.18, cy - core - rad * 0.2, cx + core - rad * 0.18,
               cy + core - rad * 0.2], fill=(245, 255, 210, 255))
    # little base line (stage)
    lw = s * 0.36
    ly = s * 0.83
    d.rounded_rectangle([cx - lw / 2, ly - s * 0.025, cx + lw / 2, ly + s * 0.025],
                        radius=s * 0.025, fill=(255, 255, 255, 70))
    return img.resize((size, size), Image.LANCZOS)


def tray_icon(size: int) -> Image.Image:
    s = size * 8
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = s / 2
    outer = s * 0.46
    ring = max(s * 0.07, 8)
    # dark outline keeps it visible on light taskbars
    d.ellipse([c - outer, c - outer, c + outer, c + outer], fill=(20, 22, 28, 235))
    inner = outer - ring
    d.ellipse([c - inner, c - inner, c + inner, c + inner], fill=LIME + (255,))
    core = inner * 0.42
    d.ellipse([c - core - inner * 0.2, c - core - inner * 0.22, c + core - inner * 0.2,
               c + core - inner * 0.22], fill=(245, 255, 215, 255))
    return img.resize((size, size), Image.LANCZOS)


def save_ico(path, frames):
    frames = sorted(frames, key=lambda im: im.size[0], reverse=True)
    frames[0].save(path, format="ICO", sizes=[im.size for im in frames],
                   append_images=frames[1:])


def main():
    os.makedirs(RES, exist_ok=True)
    save_ico(os.path.join(RES, "limelight.ico"),
             [app_icon(n) for n in (256, 128, 64, 48, 40, 32, 24, 20, 16)])
    save_ico(os.path.join(RES, "tray.ico"), [tray_icon(n) for n in (64, 48, 40, 32, 24, 20, 16)])
    app_icon(512).save(os.path.join(RES, "limelight-512.png"))
    print("icons written to", RES)


if __name__ == "__main__":
    main()
