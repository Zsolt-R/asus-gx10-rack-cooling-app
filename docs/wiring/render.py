#!/usr/bin/env python3
"""Render the wiring drawings (.excalidraw) to clean PNGs.

The drawings only use rectangles, ellipses, lines and free text, so this draws
them straight to SVG -- crisp lines, plain sans-serif text, no hand-drawn
wobble -- and has headless Chrome turn the SVG into a PNG.

    python3 render.py                       # every .excalidraw under this folder
    python3 render.py mini-fan-wiring.excalidraw

Needs google-chrome (or chromium) on PATH.
"""
import glob
import html
import json
import os
import shutil
import subprocess
import sys
import tempfile

PAD = 40
FONT = "Helvetica, Arial, 'Liberation Sans', sans-serif"
MONO = "'Cascadia Code', 'DejaVu Sans Mono', monospace"


def chrome():
    for name in ("google-chrome", "chromium", "chromium-browser"):
        path = shutil.which(name)
        if path:
            return path
    sys.exit("render.py: need google-chrome or chromium on PATH")


def bounds(elements):
    xs, ys = [], []
    for e in elements:
        if e["type"] == "line":
            for px, py in e["points"]:
                xs.append(e["x"] + px)
                ys.append(e["y"] + py)
        else:
            xs += [e["x"], e["x"] + e["width"]]
            ys += [e["y"], e["y"] + e["height"]]
    return min(xs) - PAD, min(ys) - PAD, max(xs) + PAD, max(ys) + PAD


def fill(e):
    bg = e.get("backgroundColor", "transparent")
    return "none" if bg == "transparent" else bg


def svg_element(e):
    stroke = e.get("strokeColor", "#1e1e1e")
    sw = e.get("strokeWidth", 1)
    op = e.get("opacity", 100) / 100
    common = f'stroke="{stroke}" stroke-width="{sw}" opacity="{op}"'
    if e.get("strokeStyle") == "dashed":
        common += ' stroke-dasharray="8 6"'
    t = e["type"]
    if t == "rectangle":
        r = min(10, min(e["width"], e["height"]) * 0.25) if e.get("roundness") else 0
        return (f'<rect x="{e["x"]}" y="{e["y"]}" width="{e["width"]}" '
                f'height="{e["height"]}" rx="{r}" fill="{fill(e)}" {common}/>')
    if t == "ellipse":
        return (f'<ellipse cx="{e["x"] + e["width"] / 2}" cy="{e["y"] + e["height"] / 2}" '
                f'rx="{e["width"] / 2}" ry="{e["height"] / 2}" fill="{fill(e)}" {common}/>')
    if t == "line":
        pts = " ".join(f'{e["x"] + px},{e["y"] + py}' for px, py in e["points"])
        return (f'<polyline points="{pts}" fill="none" stroke-linejoin="round" '
                f'stroke-linecap="round" {common}/>')
    if t == "text":
        size = e["fontSize"]
        lh = size * e.get("lineHeight", 1.25)
        family = MONO if e.get("fontFamily") == 3 else FONT
        align = e.get("textAlign", "left")
        anchor, x = {"left": ("start", e["x"]),
                     "center": ("middle", e["x"] + e["width"] / 2),
                     "right": ("end", e["x"] + e["width"])}[align]
        out = []
        for i, line in enumerate(e["text"].split("\n")):
            # Baseline sits about 0.8 em below the top of each line box.
            y = e["y"] + i * lh + (lh - size) / 2 + size * 0.8
            out.append(f'<text x="{x}" y="{y}" font-size="{size}" font-family="{family}" '
                       f'fill="{stroke}" opacity="{op}" text-anchor="{anchor}" '
                       f'xml:space="preserve">{html.escape(line)}</text>')
        return "\n".join(out)
    return ""


def render(src):
    d = json.load(open(src))
    els = [e for e in d["elements"] if not e.get("isDeleted")]
    x0, y0, x1, y1 = bounds(els)
    w, h = int(x1 - x0 + 0.5), int(y1 - y0 + 0.5)
    body = "\n".join(svg_element(e) for e in els)
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
           f'viewBox="{x0} {y0} {w} {h}"><rect x="{x0}" y="{y0}" width="{w}" '
           f'height="{h}" fill="#ffffff"/>{body}</svg>')
    out = os.path.splitext(src)[0] + ".png"
    with tempfile.TemporaryDirectory() as tmp:
        page = os.path.join(tmp, "page.html")
        with open(page, "w") as f:
            f.write(f'<!doctype html><html><body style="margin:0">{svg}</body></html>')
        subprocess.run([chrome(), "--headless=new", "--disable-gpu", "--no-sandbox",
                        "--hide-scrollbars", f"--window-size={w},{h}",
                        f"--screenshot={os.path.abspath(out)}", "file://" + page],
                       check=True, capture_output=True, timeout=120)
    print(f"{src} -> {out} ({w}x{h})")


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(here, "**", "*.excalidraw"),
                                             recursive=True))
    for f in files:
        render(f)
