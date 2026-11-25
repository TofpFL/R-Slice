#!/usr/bin/env python3
"""Convert 'DATA_ERROR Phase 2.gif' into a Sparrow-atlas character for vslice-lime-build.

GIF frames (640x480, char at exact game position):
  IDLE  frames 0-10   (11)
  LEFT  frames 11-22  (12)
  DOWN  frames 23-33  (11)
  UP    frames 34-45  (12)
  RIGHT frames 46-56  (11)

Each sprite keeps its absolute position via frameX/frameY/frameWidth/frameHeight
(trim offset inside a virtual canvas whose bottom-center is the character anchor).
"""
import json, os
from PIL import Image
import numpy as np

GIF = r'F:\vslice-lime-build\export\release\windows\bin\DATA_ERROR Phase 2.gif'
OUT_PNG = r'F:\vslice-lime-build\assets\shared\images\characters\dataerror.png'
OUT_XML = r'F:\vslice-lime-build\assets\shared\images\characters\dataerror.xml'
OUT_JSON = r'F:\vslice-lime-build\assets\preload\data\characters\dataerror.json'

SEGMENTS = [
    ('IDLE',  0, 10),
    ('LEFT',  11, 22),
    ('DOWN',  23, 33),
    ('UP',    34, 45),
    ('RIGHT', 46, 56),
]

im = Image.open(GIF)
W, H = im.size  # 640x480
n = im.n_frames
assert n == 57, n

frames = []
for i in range(n):
    im.seek(i)
    frames.append(np.array(im.convert('RGBA')))

# alpha bbox per frame
boxes = []
for a in frames:
    ys, xs = np.where(a[:, :, 3] > 0)
    boxes.append((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))

# union bbox -> virtual canvas with anchor at bottom-center
ux0 = min(b[0] for b in boxes); uy0 = min(b[1] for b in boxes)
ux1 = max(b[2] for b in boxes); uy1 = max(b[3] for b in boxes)
print('union bbox', ux0, uy0, ux1, uy1)
anchor_x = (ux0 + ux1) / 2
anchor_y = uy1  # feet line

# canvas: cover union bbox + 20px margin, anchor at bottom-center
M = 24
cw = (ux1 - ux0) + 2 * M
ch = (uy1 - uy0) + 2 * M
canvas_x0 = anchor_x - cw / 2
canvas_y0 = anchor_y - ch  # anchor at bottom edge minus margin -> adjust
# put anchor M px above bottom: canvas bottom = anchor_y + M
canvas_y0 = anchor_y + M - ch
canvas_x0 = int(round(canvas_x0)); canvas_y0 = int(round(canvas_y0))
print('canvas', canvas_x0, canvas_y0, cw, ch, 'anchor gif', anchor_x, anchor_y)

# shelf-pack crops into atlas
sprites = []  # (name, crop PIL, pos_in_canvas x,y)
for prefix, f0, f1 in SEGMENTS:
    for i in range(f0, f1 + 1):
        x0, y0, x1, y1 = boxes[i]
        crop = frames[i][y0:y1, x0:x1]
        img = Image.fromarray(crop, 'RGBA')
        cx = x0 - canvas_x0
        cy = y0 - canvas_y0
        name = f'{prefix}{i - f0:04d}'
        sprites.append((name, img, cx, cy))
        print(name, 'crop', x1-x0, 'x', y1-y0, 'canvas pos', cx, cy)

# sort by height desc for packing, keep name mapping
order = sorted(range(len(sprites)), key=lambda k: -sprites[k][1].height)
ATLAS_W = 2048
atlas = Image.new('RGBA', (ATLAS_W, 1), (0, 0, 0, 0))
# compute needed height via shelves
x = y = shelf_h = 0
placements = {}
for k in order:
    name, img, cx, cy = sprites[k]
    w, h = img.size
    if x + w > ATLAS_W:
        x = 0; y += shelf_h; shelf_h = 0
    placements[name] = (x, y, w, h, cx, cy)
    x += w; shelf_h = max(shelf_h, h)
ATLAS_H = y + shelf_h
atlas = atlas.crop((0, 0, ATLAS_W, ATLAS_H))
for name, img, *_ in sprites:
    ax, ay, w, h, cx, cy = placements[name]
    atlas.paste(img, (ax, ay))
print('atlas', ATLAS_W, 'x', ATLAS_H)
atlas.save(OUT_PNG, optimize=True)

# XML: frameX/frameY = -(pos in canvas), frameWidth/Height = canvas size
lines = ['<?xml version="1.0" encoding="utf-8"?>',
         f'<TextureAtlas imagePath="dataerror.png">']
for prefix, f0, f1 in SEGMENTS:
    for i in range(f0, f1 + 1):
        name = f'{prefix}{i - f0:04d}'
        ax, ay, w, h, cx, cy = placements[name]
        lines.append(f'\t<SubTexture name="{name}" x="{ax}" y="{ay}" width="{w}" height="{h}" frameX="{-cx}" frameY="{-cy}" frameWidth="{cw}" frameHeight="{ch}" />')
lines.append('</TextureAtlas>')
with open(OUT_XML, 'w', encoding='utf-8-sig', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')

# character JSON (sparrow)
chars = {
    'version': '1.0.1',
    'name': 'DATA ERROR',
    'assetPath': 'characters/dataerror',
    'renderType': 'sparrow',
    'singTime': 8.0,
    'isPixel': True,
    'offsets': [0, 0],
    'cameraOffsets': [0, 0],
    'animations': [
        {'name': 'idle', 'prefix': 'IDLE', 'offsets': [0, 0]},
        {'name': 'singLEFT', 'prefix': 'LEFT', 'offsets': [0, 0]},
        {'name': 'singDOWN', 'prefix': 'DOWN', 'offsets': [0, 0]},
        {'name': 'singUP', 'prefix': 'UP', 'offsets': [0, 0]},
        {'name': 'singRIGHT', 'prefix': 'RIGHT', 'offsets': [0, 0]},
    ],
}
with open(OUT_JSON, 'w', encoding='utf-8') as f:
    json.dump(chars, f, indent=2)
print('wrote', OUT_PNG, OUT_XML, OUT_JSON)
