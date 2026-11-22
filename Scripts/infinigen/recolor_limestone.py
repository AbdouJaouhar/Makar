"""
Recolours Infinigen boulder albedo textures from their random rock palette (often a red sandstone) to the grey-white
coral limestone of Caribbean shores, keeping the texture's own light and dark detail.

Run inside the Infinigen environment in WSL (Pillow and numpy are installed there):
    python recolor_limestone.py <folder>    # every *_DIFFUSE.png below the folder, in place
"""

import pathlib
import sys

import numpy as np
from PIL import Image

# Weathered coral limestone: pale warm grey, darker grey-brown in hollows (linear-ish 0..1 sRGB values).
LIGHT = np.array([0.82, 0.80, 0.74])
DARK = np.array([0.36, 0.34, 0.30])


def recolor(path):
    image = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    luminance = image @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    # Stretch the luminance range to use the full light-dark span, then map it onto the limestone ramp.
    low, high = np.percentile(luminance, 2), np.percentile(luminance, 98)
    t = np.clip((luminance - low) / max(high - low, 1e-3), 0.0, 1.0)[..., None]
    t = t ** 0.8  # mostly pale rock, dark only in the deepest hollows
    out = DARK + (LIGHT - DARK) * t
    Image.fromarray((np.clip(out, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)).save(path)
    print(f"recolor_limestone: {path}")


if __name__ == "__main__":
    for texture in pathlib.Path(sys.argv[1]).rglob("*_DIFFUSE.png"):
        recolor(texture)
