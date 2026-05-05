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

# Splash render width. SVG aspect is ~1.79:1; 192*107*2 = 41 KB in flash for
# RGB565, comfortably under the 50 KB target. Logo still reads cleanly on the
# 1024-wide panel.
WIDTH_PX = 192

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
    """Render the recoloured SVG with a transparent background and downscale
    via PIL. Transparent bg (rather than a baked-in dark) means LVGL composites
    the logo over the screen bg directly -- so the logo bg always matches the
    screen bg exactly, with no RGB565 round-trip drift from the PNG decode +
    LANCZOS downsample shifting the solid pixels by 1-2 LSB.

    Cost: alpha channel doubles the asset (RGB565A8 instead of RGB565), but
    the splash is still ~60 KB which is acceptable.

    svglib's drawing.scale() corrupts visible content (paths fall outside the
    new clip rect) so we keep the geometry untouched and let PIL do the final
    downscale."""
    drawing = svg2rlg(str(TMP_SVG))
    full_png = TMP_PNG.with_name("_splash_full.png")
    # bg=None keeps the alpha channel; configPIL=None keeps PIL out of the
    # path so renderPM uses pycairo end-to-end.
    renderPM.drawToFile(drawing, str(full_png), fmt="PNG",
                        bg=None, configPIL=None)
    img = Image.open(full_png).convert("RGBA")
    ratio = WIDTH_PX / img.size[0]
    out = img.resize((WIDTH_PX, int(img.size[1] * ratio)), Image.LANCZOS)
    out.save(TMP_PNG)


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
