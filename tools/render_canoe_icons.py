#!/usr/bin/env python3
"""Render the compact firmware icon paths for visual review."""
import re
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader/CanoeIcons.h"
OUTPUT = ROOT / "artifacts/ui/icon-preview.png"

text = HEADER.read_text(encoding="ascii")
icons = []
for name, body in re.findall(r"mIcon(\w+)\[\]=\{(.*?)\};", text):
    paths, current = [], []
    for token in re.findall(r"P\((-?\d+),(-?\d+)\)|\bB\b", body):
        if token == ("", ""):
            if current:
                paths.append(current)
                current = []
        else:
            current.append(tuple(map(int, token)))
    if current:
        paths.append(current)
    icons.append((name, paths))

scale, tile = 4, 112
image = Image.new("RGB", (tile * 4, tile * 4), "#191919")
draw = ImageDraw.Draw(image)
for index, (name, paths) in enumerate(icons):
    left, top = (index % 4) * tile, (index // 4) * tile
    for path in paths:
        points = [(left + 20 + x * 3, top + 12 + y * 3) for x, y in path]
        if len(points) == 1:
            x, y = points[0]
            draw.ellipse((x - 3, y - 3, x + 3, y + 3), fill="#f3f3f3")
        else:
            draw.line(points, fill="#f3f3f3", width=6, joint="curve")
            for x, y in (points[0], points[-1]):
                draw.ellipse((x - 3, y - 3, x + 3, y + 3), fill="#f3f3f3")
    draw.text((left + 8, top + 92), name, fill="#a8a8a8")
OUTPUT.parent.mkdir(parents=True, exist_ok=True)
image.save(OUTPUT)
print(OUTPUT)
