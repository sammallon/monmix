#!/usr/bin/env python3
"""
Splash-logo builder. Reads ColorWithName.svg, recolors the bottom text path
to white (so it reads on the near-black splash background), rasterizes to PNG
via svglib + pycairo, then runs LVGLImage.py to emit a CF_RGB565A8 C array.

Run from project root after editing the SVG or the splash dimensions.
"""

import os
import sys
import subprocess
from pathlib import Path

# rlPyCairo prefers cairocffi but cairocffi can't find libcairo on this host.
# Block cairocffi at import time so rlPyCairo falls back to pycairo (which
# wheels bundle the DLL and works).
import builtins
_orig_import = builtins.__import__
def _import(name, *args, **kwargs):
    if name == "cairocffi":
        raise ImportError("blocked: prefer pycairo on this host")
    return _orig_import(name, *args, **kwargs)
builtins.__import__ = _import
sys.modules.pop("cairocffi", None)

import re
from svglib.svglib import svg2rlg
from reportlab.graphics import renderPM
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SRC_SVG = Path(r"C:\Users\samallon\OneDrive\church projects\MV Logo\SVG\ColorWithName.svg")
TMP_SVG = ROOT / "tools" / "_splash_white_text.svg"
TMP_PNG = ROOT / "tools" / "_splash_logo.png"
OUT_DIR = ROOT / "main" / "images"
# managed_components is fetched per-build into the worktree but isn't there
# pre-build. Fall back to the canonical copy at the parent project root.
LVGL_CONV_CANDIDATES = [
    ROOT / "managed_components" / "lvgl__lvgl" / "scripts" / "LVGLImage.py",
    Path(r"S:\playground\monmix\esp_ui\managed_components\lvgl__lvgl\scripts\LVGLImage.py"),
]
LVGL_CONV = next((p for p in LVGL_CONV_CANDIDATES if p.exists()), LVGL_CONV_CANDIDATES[0])

# Splash render width. 1024-wide panel, target ~66% of width for the logo
# = 675 px. SVG aspect ~1.79:1 -> ~378 px tall (63% of the 600 px height).
# Asset cost: 675 * 378 * 3 = 766 KB in flash for RGB565A8. Comfortable in
# the 4 MB app partition (about 19% of it). Rendered at near-native SVG
# size (773 px wide) so LANCZOS downscale is gentle and the result is clean.
WIDTH_PX = 675

def recolor_text():
    """Add fill=#ffffff to the text path (id=text2). Pure text substitution
    so the SVG namespacing stays exactly as Inkscape wrote it -- ElementTree
    re-serialisation rewrites prefixes (svg:svg) which svglib then mis-parses
    and drops every CSS-class fill, leaving us with black outlines only."""
    src = SRC_SVG.read_text(encoding="utf-8")
    # Match the style="..." attribute on the element with id="text2" and
    # prepend fill:#ffffff;. The text path is the only one with id="text2".
    pat = re.compile(r'(style=")([^"]*?)("\s+d="[^"]*"\s+id="text2")', re.DOTALL)
    m = pat.search(src)
    if not m:
        # Fall back: id may appear before style. Try the looser form.
        pat = re.compile(r'(<path\s+style=")([^"]*?)("[^>]*?id="text2")', re.DOTALL)
        m = pat.search(src)
    if not m:
        raise SystemExit("text2 path style attr not found; layout changed?")
    new = m.group(1) + "fill:#ffffff;" + m.group(2) + m.group(3)
    out = src[:m.start()] + new + src[m.end():]
    TMP_SVG.write_text(out, encoding="utf-8")


def render_png():
    """Render the recoloured SVG and post-process to transparent bg. The user
    asked for a transparent SVG canvas so the logo composites over whatever
    screen bg LVGL picks (no quantisation drift from a baked-in dark bg).
    renderPM's bg=None doesn't actually emit transparent pixels -- it falls
    back to black -- and cairosvg can't load libcairo on this host. So we
    render with bg=black and post-process: any pixel that's still pure black
    after rendering = canvas, set alpha=0. The SVG has no intentional black
    fills (cls-1 = white, cls-2 = teal, recoloured text = white) so pure-black
    pixels are unambiguously canvas.

    Anti-aliased edges between logo and canvas blend the logo colour with
    black; those pixels stay opaque with a slight dark fringe. Acceptable
    for a 1-pixel boundary against a dark screen bg.

    svglib's drawing.scale() corrupts visible content so we keep the geometry
    untouched and let PIL do the final downscale."""
    drawing = svg2rlg(str(TMP_SVG))
    full_png = TMP_PNG.with_name("_splash_full.png")
    # bg=0x000000 fills the canvas with black; we'll alpha-key it below.
    renderPM.drawToFile(drawing, str(full_png), fmt="PNG",
                        bg=0x000000, configPIL=None)
    img = Image.open(full_png).convert("RGB")
    ratio = WIDTH_PX / img.size[0]
    img = img.resize((WIDTH_PX, int(img.size[1] * ratio)), Image.LANCZOS)
    # Alpha-key pure black -> transparent.
    rgba = img.convert("RGBA")
    pixels = rgba.load()
    w, h = rgba.size
    for y in range(h):
        for x in range(w):
            r, g, b, _ = pixels[x, y]
            if r == 0 and g == 0 and b == 0:
                pixels[x, y] = (0, 0, 0, 0)
    rgba.save(TMP_PNG)


def run_lvgl_conv():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        str(LVGL_CONV),
        "--ofmt", "C",
        # RGB565A8 -- per-pixel alpha so LVGL composites the logo cleanly
        # over the screen bg. Avoids the colour mismatch you get when baking
        # the dark bg into the PNG (LANCZOS + RGB565 quantisation shifts
        # solid pixels off the screen-bg target by 1-2 LSB).
        "--cf", "RGB565A8",
        "--name", "splash_logo",
        "-o", str(OUT_DIR),
        str(TMP_PNG),
    ]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def patch_include():
    """LVGLImage.py emits a multi-branch include guard ending in
    "lvgl/lvgl.h", which the IDF managed-component layout doesn't expose.
    Replace with a plain "lvgl.h" so the file builds in this project."""
    out_c = OUT_DIR / "splash_logo.c"
    src = out_c.read_text(encoding="utf-8")
    needle = (
        '#if defined(LV_LVGL_H_INCLUDE_SIMPLE)\n'
        '#include "lvgl.h"\n'
        '#elif defined(LV_LVGL_H_INCLUDE_SYSTEM)\n'
        '#include <lvgl.h>\n'
        '#elif defined(LV_BUILD_TEST)\n'
        '#include "../lvgl.h"\n'
        '#else\n'
        '#include "lvgl/lvgl.h"\n'
        '#endif'
    )
    replacement = (
        '// IDF managed-component include path: plain "lvgl.h" resolves to the\n'
        '// LVGL package\'s top header. The fallback chain LVGLImage.py emits\n'
        '// points at the upstream-monorepo layout which IDF doesn\'t expose.\n'
        '#include "lvgl.h"'
    )
    if needle in src:
        out_c.write_text(src.replace(needle, replacement), encoding="utf-8")


def main():
    recolor_text()
    render_png()
    print(f"PNG: {TMP_PNG} ({TMP_PNG.stat().st_size} bytes)")
    run_lvgl_conv()
    patch_include()
    out_c = OUT_DIR / "splash_logo.c"
    print(f"C: {out_c} ({out_c.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
