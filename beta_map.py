#!/usr/bin/env python3
"""beta-map: an isometric web map of a Minecraft Beta world (the McRegion format, Beta 1.3 to 1.8).

    python beta_map.py WORLD OUT [--title NAME] [--places FILE] [--spawn X,Z] [--live URL] [--jobs N]

WORLD is the world folder (the one with level.dat and region/). OUT gets a complete static website: open
OUT/index.html through any web server, or push OUT to GitHub Pages. Run it again whenever you want a fresh map;
unchanged regions come from a cache next to OUT (OUT-cache), so later runs are much faster.

What it draws:
  iso0..iso3, nether-iso0..3   the world as little 4 px cubes, seen from the four directions (the main view)
  iso*-torches                 torch flames, in their own see-through tiles over the main view
  flat, nether                 top-down pictures, 1 px per block (the minimap)
  height, nether-height        each column's top block height (red) and whether there's a column (green), so the
                               page can put coordinates, places and players exactly on the 3D view
Only reads the world; safe to run on a live server's world, but a copy (or a backup) gives a cleaner picture.
Needs nothing but Python 3.8+ (it writes its own PNGs)."""
import argparse, datetime, glob, gzip, hashlib, io, json, multiprocessing, os, re, shutil, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, 'web')
VERSION = '1'                                       # bump when the drawing changes: the cache is redrawn
T, MIN_ZOOM = 512, -4
BG = (20, 22, 26)

# ---------------------------------------------------------------- colours (Beta 1.7.3 block ids)
COL = {1: (125, 125, 125), 2: (106, 160, 64), 3: (134, 96, 67), 4: (112, 112, 112), 5: (180, 144, 90), 7: (60, 60, 60),
       8: (48, 86, 200), 9: (48, 86, 200), 10: (214, 96, 20), 11: (214, 96, 20), 12: (219, 211, 160), 13: (136, 126, 126),
       14: (150, 140, 110), 15: (140, 128, 120), 16: (100, 100, 100), 17: (102, 81, 50), 18: (56, 108, 36),
       19: (195, 195, 80), 20: (205, 232, 240), 21: (100, 110, 150), 22: (30, 67, 140), 23: (110, 110, 110),
       24: (216, 203, 155), 25: (100, 67, 50), 26: (160, 35, 35), 27: (150, 120, 80), 28: (150, 120, 80),
       29: (110, 110, 90), 30: (220, 220, 220), 33: (110, 110, 90), 34: (110, 110, 90), 35: (222, 222, 222),
       41: (250, 215, 60), 42: (220, 220, 220), 43: (165, 165, 165), 44: (165, 165, 165), 45: (150, 80, 65),
       46: (200, 60, 40), 47: (120, 90, 50), 48: (80, 110, 80), 49: (24, 20, 36), 51: (240, 140, 30), 52: (30, 50, 70),
       53: (180, 144, 90), 54: (150, 105, 40), 56: (129, 140, 143), 57: (100, 220, 215), 58: (140, 95, 55),
       59: (150, 170, 50), 60: (110, 75, 45), 61: (95, 95, 95), 62: (95, 95, 95), 64: (150, 115, 70),
       65: (160, 125, 75), 67: (112, 112, 112), 71: (190, 190, 190), 73: (132, 107, 107), 74: (132, 107, 107),
       78: (240, 250, 250), 79: (165, 195, 245), 80: (240, 250, 250), 81: (20, 120, 30), 82: (160, 165, 180),
       83: (140, 190, 90), 84: (100, 67, 50), 85: (150, 115, 70), 86: (200, 120, 20), 87: (120, 52, 50),
       88: (85, 65, 50), 89: (245, 225, 140), 90: (130, 50, 210), 91: (220, 140, 30), 92: (230, 220, 220),
       93: (150, 150, 150), 94: (150, 150, 150), 95: (150, 105, 40), 96: (130, 100, 60)}
WOOL = [(222, 222, 222), (234, 128, 56), (190, 74, 201), (110, 140, 210), (196, 180, 30), (65, 174, 56),
        (208, 132, 153), (64, 64, 64), (155, 161, 161), (46, 110, 137), (126, 61, 181), (46, 56, 141), (79, 50, 31),
        (53, 70, 27), (150, 52, 48), (25, 22, 22)]
OTHER = (150, 150, 150)
# drawn as what's under them in the top-down pictures: too small to see from above
SEE_THROUGH = bytes(sorted({0, 6, 27, 28, 31, 32, 37, 38, 39, 40, 50, 51, 55, 63, 65, 66, 68, 69, 70, 72, 75, 76, 77}))
WATER = (8, 9)


# ---------------------------------------------------------------- NBT (reading only)
def read_tag(s, t):
    if t == 1: return struct.unpack('>b', s.read(1))[0]
    if t == 2: return struct.unpack('>h', s.read(2))[0]
    if t == 3: return struct.unpack('>i', s.read(4))[0]
    if t == 4: return struct.unpack('>q', s.read(8))[0]
    if t == 5: return struct.unpack('>f', s.read(4))[0]
    if t == 6: return struct.unpack('>d', s.read(8))[0]
    if t == 7: return bytearray(s.read(struct.unpack('>i', s.read(4))[0]))
    if t == 8: return s.read(struct.unpack('>H', s.read(2))[0]).decode('utf-8', 'replace')
    if t == 9:
        et, n = s.read(1)[0], struct.unpack('>i', s.read(4))[0]
        return [read_tag(s, et) for _ in range(n)]
    if t == 10:
        out = []
        while True:
            ct = s.read(1)[0]
            if ct == 0: return out
            name = s.read(struct.unpack('>H', s.read(2))[0]).decode('utf-8', 'replace')
            out.append((name, read_tag(s, ct)))
    if t == 11:
        n = struct.unpack('>i', s.read(4))[0]
        return list(struct.unpack('>%di' % n, s.read(4 * n)))
    raise ValueError('unknown NBT tag %d' % t)


def parse(raw):
    s = io.BytesIO(raw)
    t = s.read(1)[0]; s.read(struct.unpack('>H', s.read(2))[0])
    return read_tag(s, t)


def field(compound, name):
    return next(v for n, v in compound if n == name)


# ---------------------------------------------------------------- PNG (8 bit RGB or RGBA, no filter) in and out
def png_write(path, w, h, rgb, alpha=False):        # alpha: rgb is RGBA
    n = 4 if alpha else 3
    raw = b''.join(b'\x00' + bytes(rgb[y * w * n:(y + 1) * w * n]) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6 if alpha else 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    os.replace(tmp, path)


def png_read(path):
    """Only our own PNGs: RGB, 8 bit, filter 0 on every row."""
    d = open(path, 'rb').read(); pos, idat, w, h = 8, b'', 0, 0
    while pos < len(d):
        n = struct.unpack('>I', d[pos:pos + 4])[0]; t = d[pos + 4:pos + 8]; body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR': w, h = struct.unpack('>II', body[:8])
        elif t == b'IDAT': idat += body
        pos += 12 + n
    raw = zlib.decompress(idat); row = w * 3 + 1
    return bytearray(b''.join(raw[y * row + 1:(y + 1) * row] for y in range(h)))


# ---------------------------------------------------------------- reading region files
def chunks(path):
    """(chunk index 0..1023, Blocks, Data) for each readable chunk of a .mcr file."""
    data = open(path, 'rb').read()
    for i in range(1024):
        loc = struct.unpack('>I', data[4 * i:4 * i + 4])[0]
        if not loc: continue
        try:
            off = (loc >> 8) * 4096; ln = struct.unpack('>I', data[off:off + 4])[0]
            raw = data[off + 5:off + 4 + ln]
            raw = zlib.decompress(raw) if data[off + 4] == 2 else gzip.decompress(raw)
            lv = field(parse(raw), 'Level')
            yield i, field(lv, 'Blocks'), field(lv, 'Data')
        except Exception:
            continue                                   # a broken chunk stays blank


def top_overworld(col):
    y = len(col.rstrip(b'\x00')) - 1
    while y > 0 and col[y] in SEE_THROUGH: y -= 1
    return y


def top_nether(col):
    """The floor under the bedrock roof: down through the roof to air, then down through the air."""
    y = min(len(col.rstrip(b'\x00')) - 1, 126)
    while y > 0 and col[y] not in SEE_THROUGH: y -= 1
    while y > 0 and col[y] in SEE_THROUGH: y -= 1
    return y


def block_colour(col, data, base, y):
    """Colour of the block at height y of one column (col: its 128 block ids). Water: blended with what's under it."""
    b = col[y]
    if b in WATER:
        d = y
        while d > 0 and col[d] in WATER: d -= 1
        under = COL.get(col[d], OTHER)
        f = min(y - d, 8) / 8 * 0.3 + 0.62            # deeper water: less of the floor shows through
        w = COL[9]
        return tuple(int(w[i] * f + under[i] * (1 - f)) for i in range(3))
    if b == 35:
        k = base + y
        return WOOL[(data[k >> 1] >> (4 * (k & 1))) & 15]
    return COL.get(b, OTHER)


# ---------------------------------------------------------------- flat layers (top-down, the minimap)
def draw_flat(path, nether=False):
    """rgb and heights (both 512x512 bytearrays) for one .mcr file."""
    rgb = bytearray(bytes(BG) * (T * T)); height = bytearray(T * T); water = bytearray(T * T); seen = bytearray(T * T)
    top = top_nether if nether else top_overworld
    for i, blocks, meta in chunks(path):
        ox, oz = (i % 32) * 16, (i // 32) * 16
        for lx in range(16):
            for lz in range(16):
                base = lx * 2048 + lz * 128
                col = blocks[base:base + 128]
                y = top(col)
                if y < 0: continue
                p = (oz + lz) * T + ox + lx
                height[p] = y; water[p] = col[y] in WATER; seen[p] = 255
                rgb[3 * p:3 * p + 3] = bytes(block_colour(col, meta, base, y))
    # shading: a block higher than the one north of it is lit, lower is in shadow (water stays flat)
    bg = bytes(BG)
    for z in range(1, T):
        for x in range(T):
            p = z * T + x
            if water[p]: continue
            dh = height[p] - height[p - T]
            if dh == 0 or rgb[3 * p:3 * p + 3] == bg: continue
            f = 1.14 if dh > 0 else 0.84
            if abs(dh) > 2: f = 1.24 if dh > 0 else 0.72
            for k in range(3): rgb[3 * p + k] = min(255, int(rgb[3 * p + k] * f))
    hp = bytearray(3 * T * T)                          # red: height of the top block; green: 255 where there's a column
    hp[0::3] = height
    hp[1::3] = seen
    return rgb, hp


def flat_job(job):
    src, out, hout, nether = job
    rgb, hp = draw_flat(src, nether)
    png_write(out, T, T, rgb)
    png_write(hout, T, T, hp)
    return out


# ---------------------------------------------------------------- the isometric pictures (the main view)
# A region's picture: width 2048, height 1280; block (lx, y, lz) of the region at
# X = 2 (lx - lz) + 1022, Y = (lx + lz) - 2 y + 254. Each block is a 4x4 cube: a top of 2 + 4 px, then two rows of
# left (+z) and right (+x) faces. Drawn back to front, so nearer cubes cover the ones behind.
# Four directions: view k is the world turned k quarter turns first, block (x, z) -> (-z - 1, x) each turn, so the
# same drawing code always looks from the south-east of the turned world. Region (rx, rz) -> (-rz - 1, rx), and
# inside a region (lx, lz) -> (511 - lz, lx). Blocks are drawn down to 2 under the lowest ground within NEAR blocks,
# so cliffs and the ground under overhangs show and caves don't.
IW, IH = 2048, 1280
NEAR = 12
TREE = bytes((17, 18))
# blocks that don't hide what's behind or under them: the small ones, plus fences, glass and water
CLEAR = bytes(sorted(set(SEE_THROUGH) | {85, 20, 8, 9}))
SMALL = {66, 27, 28, 50, 75, 76, 85, 63, 68}       # drawn as little shapes: rails, torches, fences, signs
FLOWERS = {37: b'\xff\xe2\x28', 38: b'\xd6\x22\x22'}   # one pixel each, sticking up (dandelion, rose)
WATER_RGB, GLASS_RGB = (34, 82, 214), (205, 232, 240)
TORCH = {1: (255, 214, 80, 255), 2: (200, 40, 30, 255), 3: (100, 75, 45, 255)}   # flame, redstone flame, stick


def ground_floor(cols):
    """Per column (index lz * 512 + lx): how far down to draw, 2 under the lowest ground within NEAR blocks.
    Ground: the top block, not counting trees and see-through blocks."""
    from collections import deque
    INF = 255
    gr = [INF] * (T * T)
    for (lx, lz), c in cols.items():
        col, y = c[0], c[3]
        while y > 0 and (col[y] in CLEAR or col[y] in TREE): y -= 1
        gr[lz * T + lx] = y
    def slide(line):                                   # minimum over a window of 2 NEAR + 1, in one pass
        out, dq, n = [0] * len(line), deque(), len(line)
        for i in range(n + NEAR):
            if i < n:
                while dq and line[dq[-1]] >= line[i]: dq.pop()
                dq.append(i)
            j = i - NEAR
            if j >= 0:
                while dq[0] < j - NEAR: dq.popleft()
                out[j] = line[dq[0]]
        return out
    rows = [slide(gr[z * T:(z + 1) * T]) for z in range(T)]
    colsmin = [slide([rows[z][x] for z in range(T)]) for x in range(T)]
    return [max(0, colsmin[x][z] - 2) if colsmin[x][z] != INF else 0 for z in range(T) for x in range(T)]


def turn_local(lx, lz, k):
    for _ in range(k): lx, lz = 511 - lz, lx
    return lx, lz


def turn_region(rx, rz, k):
    for _ in range(k): rx, rz = -rz - 1, rx
    return rx, rz


def shade(c, f): return bytes(min(255, int(v * f)) for v in c)


def draw_iso(path, nether=False, k=0):
    """(rgb, alpha, torches) of one region's picture in view k. alpha: 0 nothing drawn, 255 solid, between: see-
    through (water or glass over nothing of this region; iso_compose blends those over the regions behind), with rgb
    premultiplied by it. Nether: cut under the bedrock roof."""
    rgb = bytearray(IW * IH * 3); mask = bytearray(IW * IH)
    wet = bytearray(IW * IH)                           # water pixels, and what was there before the water:
    under = bytearray(IW * IH * 3); under_a = bytearray(IW * IH)
    top = top_nether if nether else top_overworld
    cols = {}                                          # turned (lx, lz) -> (column bytes, data, base, top, highest)
    for i, blocks, meta in chunks(path):
        ox, oz = (i % 32) * 16, (i // 32) * 16
        for lx in range(16):
            for lz in range(16):
                base = lx * 2048 + lz * 128
                col = blocks[base:base + 128]
                y = top(col)
                if y >= 0:
                    up = y if nether else len(col.rstrip(b'\x00')) - 1   # torches, rails... above the top
                    cols[turn_local(ox + lx, oz + lz, k)] = (col, meta, base, y, up)
    looks = {}                                         # colour -> (2 px top, 4 px top, 4 px sides)
    def look(c):
        v = looks.get(c)
        if v is None:
            t = bytes(c); l, r = shade(c, 0.70), shade(c, 0.86)
            v = looks[c] = (t * 2, t * 4, l * 2 + r * 2)
        return v
    one2, one4, zero2, zero4 = b'\xff' * 2, b'\xff' * 4, b'\x00' * 2, b'\x00' * 4

    tor = bytearray(IW * IH)                           # torch pixels, kept apart: TORCH code where a torch is the
                                                       # nearest thing, 0 elsewhere
    def put(q, c):                                     # one opaque pixel
        rgb[3 * q:3 * q + 3] = c; mask[q] = 255; wet[q] = 0; tor[q] = 0

    def blend(q, c, a, water=False):                   # one see-through pixel over what's drawn so far ("over",
        if water:                                      # premultiplied); water over water: always over what was
            if not wet[q]:                             # under the first, so overlapping water tops don't stack up
                under[3 * q:3 * q + 3] = rgb[3 * q:3 * q + 3]; under_a[q] = mask[q]; wet[q] = 1
            p0, a0 = under[3 * q:3 * q + 3], under_a[q]
        else:
            p0, a0 = rgb[3 * q:3 * q + 3], mask[q]; wet[q] = 0
        rgb[3 * q:3 * q + 3] = bytes((int(c[0] * a + p0[0] * (1 - a)), int(c[1] * a + p0[1] * (1 - a)),
                                      int(c[2] * a + p0[2] * (1 - a))))
        mask[q] = min(255, int(round(255 * a + a0 * (1 - a)))); tor[q] = 0

    def sprite(b, p):
        if b in (66, 27, 28):                          # rails: on the top of the block under them
            c = bytes((170, 140, 60)) if b == 27 else bytes((118, 98, 78))
            put(p + 2 * IW + 1, c); put(p + 2 * IW + 2, c)
            for x in range(4): put(p + 3 * IW + x, c)
        elif b in (50, 75, 76):                        # torches: a flame and a stick, in their own layer
            tor[p + IW + 1] = 1 if b == 50 else 2
            tor[p + 2 * IW + 1] = 3
        elif b in FLOWERS:                             # flowers: one pixel, just above the block under them
            put(p + IW + 1, FLOWERS[b])
        elif b == 85:                                  # fence post
            c = COL[85]
            put(p + 1, bytes(c)); put(p + 2, bytes(c))
            for r in (1, 2, 3): put(p + r * IW + 1, shade(c, 0.70)); put(p + r * IW + 2, shade(c, 0.86))
        elif b in (63, 68):                            # signs
            c = (180, 144, 90)
            put(p + IW + 1, bytes(c)); put(p + IW + 2, bytes(c))
            put(p + 2 * IW + 1, shade(c, 0.8)); put(p + 2 * IW + 2, shade(c, 0.86))
            if b == 63: put(p + 3 * IW + 1, bytes((110, 85, 55)))

    floor = ground_floor(cols)
    ST = SEE_THROUGH
    for v in range(0, 1023):                           # back to front: x + z grows towards the viewer
        for lx in range(max(0, v - 511), min(511, v) + 1):
            lz = v - lx
            c = cols.get((lx, lz))
            if c is None: continue
            col, meta, base, h, up = c
            f1, f2 = cols.get((lx + 1, lz)), cols.get((lx, lz + 1))
            c1 = f1[0] if f1 else None; h1 = f1[3] if f1 else -1
            c2 = f2[0] if f2 else None; h2 = f2[3] if f2 else -1
            X = 2 * (lx - lz) + 1022
            # every block from just under the lowest ground nearby up to the top that has a face open to the air
            # (or to see-through blocks): its top, or the side facing the viewer (+x or +z). Above a column's top
            # counts as open (the Nether's cut roof), and so does a missing neighbour (the edge of the region or of
            # the generated world).
            for y in range(min(h, floor[lz * T + lx]), up + 1):
                b = col[y]
                if b == 0: continue
                p = ((lx + lz) - 2 * y + 254) * IW + X
                if b in SMALL or b in FLOWERS:
                    sprite(b, p); continue
                if b in ST: continue
                open1 = y > h1 or c1[y] in CLEAR       # the +x side
                open2 = y > h2 or c2[y] in CLEAR       # the +z side
                if b in WATER:
                    surface = not (y < 127 and col[y + 1] in WATER)
                    if surface:                        # the deeper, the less of the floor shows
                        d = y
                        while d > 0 and col[d] in WATER: d -= 1
                        a = 0.52 + min(y - d, 10) * 0.038
                        for q in (p + 1, p + 2, p + IW, p + IW + 1, p + IW + 2, p + IW + 3): blend(q, WATER_RGB, a, True)
                    # its sides only where a known neighbour is open and not water (at the region's edge the
                    # neighbour is in the next region: no side, or it shows as a seam)
                    if c1 is not None and open1 and not (y <= h1 and c1[y] in WATER):
                        for q in (p + 2 * IW + 2, p + 2 * IW + 3, p + 3 * IW + 2, p + 3 * IW + 3): blend(q, (34, 68, 170), 0.7, True)
                    if c2 is not None and open2 and not (y <= h2 and c2[y] in WATER):
                        for q in (p + 2 * IW, p + 2 * IW + 1, p + 3 * IW, p + 3 * IW + 1): blend(q, (28, 56, 140), 0.7, True)
                    continue
                if not (y == 127 or col[y + 1] in CLEAR or open1 or open2): continue
                if b == 20:                            # glass: tinted, see-through
                    for q, f in ((p + 1, 1), (p + 2, 1), (p + IW, 1), (p + IW + 1, 1), (p + IW + 2, 1), (p + IW + 3, 1),
                                 (p + 2 * IW, .8), (p + 2 * IW + 1, .8), (p + 3 * IW, .8), (p + 3 * IW + 1, .8),
                                 (p + 2 * IW + 2, .9), (p + 2 * IW + 3, .9), (p + 3 * IW + 2, .9), (p + 3 * IW + 3, .9)):
                        blend(q, tuple(int(v * f) for v in GLASS_RGB), 0.38)
                    continue
                t2, t4, s4 = look(block_colour(col, meta, base, y))
                rgb[3 * (p + 1):3 * (p + 3)] = t2; mask[p + 1:p + 3] = one2; wet[p + 1:p + 3] = zero2; tor[p + 1:p + 3] = zero2
                p += IW; rgb[3 * p:3 * (p + 4)] = t4; mask[p:p + 4] = one4; wet[p:p + 4] = zero4; tor[p:p + 4] = zero4
                p += IW; rgb[3 * p:3 * (p + 4)] = s4; mask[p:p + 4] = one4; wet[p:p + 4] = zero4; tor[p:p + 4] = zero4
                p += IW; rgb[3 * p:3 * (p + 4)] = s4; mask[p:p + 4] = one4; wet[p:p + 4] = zero4; tor[p:p + 4] = zero4
    return rgb, mask, tor


def iso_job(job):
    src, out, nether, k = job
    if not os.path.exists(out):
        rgb, mask, tor = draw_iso(src, nether, k)
        tmp = out + '.tmp'
        with open(tmp, 'wb') as f:
            for part in (mask, rgb, tor): f.write(zlib.compress(bytes(part), 1))
        os.replace(tmp, out)
    return out


def iso_load(path):
    data = open(path, 'rb').read(); parts = []
    for _ in range(3):
        d = zlib.decompressobj(); parts.append(d.decompress(data)); data = d.unused_data
    mask, rgb, tor = parts
    return rgb, mask, tor


NONZERO = bytes([0] + [1] * 255)                       # alpha -> 0/1, to find the drawn runs with bytes.find


def iso_compose(regs, cache_of, out_dir, torch_dir):
    """Paste the regions' pictures into 512 px tiles, back to front: solid pixels copied, see-through ones (alpha
    1..254, rgb premultiplied) blended over what the regions behind left there. Torches go into their own see-through
    tiles in torch_dir (only those with a torch), where anything a region nearer the viewer draws covers them.
    Returns the tiles written, the extent and the torch tiles."""
    tiles, ttiles = {}, {}
    bg = bytes(BG)
    for rx, rz in sorted(regs, key=lambda r: (r[0] + r[1], r[0])):
        rgb, alpha, tor = iso_load(cache_of[(rx, rz)])
        X0 = (rx - rz) * 1024 - 1022; Y0 = (rx + rz) * 512 - 254
        for row in range(IH):
            r0 = row * IW
            m = alpha[r0:r0 + IW].translate(NONZERO)
            a = m.find(1)
            Y = Y0 + row; ty = Y // T; ly = Y - ty * T
            while a != -1:
                b = m.find(0, a)
                if b == -1: b = IW
                X = X0 + a; end = X0 + b
                while X < end:                         # split the run at tile edges
                    tx = X // T; stop = min(end, (tx + 1) * T)
                    buf = tiles.get((tx, ty))
                    if buf is None: buf = tiles[(tx, ty)] = bytearray(bg * (T * T))
                    i0 = r0 + (X - X0); n = stop - X
                    src = 3 * i0; dst = 3 * (ly * T + (X - tx * T))
                    if alpha.count(255, i0, i0 + n) == n:
                        buf[dst:dst + 3 * n] = rgb[src:src + 3 * n]
                    else:
                        for j in range(n):
                            al = alpha[i0 + j]; s = src + 3 * j; t = dst + 3 * j
                            if al == 255: buf[t:t + 3] = rgb[s:s + 3]
                            else:
                                f = 1 - al / 255
                                buf[t:t + 3] = bytes((min(255, int(rgb[s] + buf[t] * f)), min(255, int(rgb[s + 1] + buf[t + 1] * f)),
                                                      min(255, int(rgb[s + 2] + buf[t + 2] * f))))
                    tb = ttiles.get((tx, ty))
                    if tb is not None:                 # this region covers torches of the ones behind
                        o = ly * T + (X - tx * T); tb[o:o + n] = bytes(n)
                    X = stop
                a = m.find(1, b)
            tm = tor[r0:r0 + IW].translate(NONZERO)    # then its own torches on top
            a = tm.find(1)
            while a != -1:
                b = tm.find(0, a)
                if b == -1: b = IW
                for j in range(a, b):
                    X = X0 + j; tx = X // T
                    tb = ttiles.get((tx, ty))
                    if tb is None: tb = ttiles[(tx, ty)] = bytearray(T * T)
                    tb[ly * T + X - tx * T] = tor[r0 + j]
                a = tm.find(1, b)
    for (tx, ty), buf in tiles.items():
        png_write(os.path.join(out_dir, '%d_%d.png' % (tx, ty)), T, T, buf)
    torches = []
    os.makedirs(torch_dir, exist_ok=True)
    for (tx, ty), tb in ttiles.items():
        if tb.count(0) == T * T: continue              # every torch here was covered later
        rgba = bytearray(T * T * 4)
        nz = tb.translate(NONZERO); i = nz.find(1)
        while i != -1:
            rgba[4 * i:4 * i + 4] = bytes(TORCH[tb[i]]); i = nz.find(1, i + 1)
        png_write(os.path.join(torch_dir, '%d_%d.png' % (tx, ty)), T, T, rgba, alpha=True)
        torches.append('%d,%d' % (tx, ty))
    xs = [k[0] for k in tiles]; ys = [k[1] for k in tiles]
    return set(tiles), [min(xs) * T, min(ys) * T, (max(xs) + 1) * T, (max(ys) + 1) * T] if tiles else None, sorted(torches)


# ---------------------------------------------------------------- zooming out
def shrink(children):
    """Four 512 px tiles (or None) as a 2x2 block -> one 512 px tile at half the scale."""
    out = bytearray(bytes(BG) * (T * T)); h = T // 2
    for (qx, qz), px in children.items():
        if px is None: continue
        for y in range(h):
            r1 = (2 * y) * T * 3; r2 = r1 + T * 3; o = ((qz * h + y) * T + qx * h) * 3
            for x in range(h):
                a, b = r1 + 6 * x, r2 + 6 * x
                for k in range(3):
                    out[o + 3 * x + k] = (px[a + k] + px[a + 3 + k] + px[b + k] + px[b + 3 + k]) >> 2
    return out


def zoom_job(job):
    out, z, px, pz, kids = job
    data = {q: (png_read(os.path.join(out, str(z + 1), '%d_%d.png' % k)) if k else None) for q, k in kids.items()}
    png_write(os.path.join(out, str(z), '%d_%d.png' % (px, pz)), T, T, shrink(data))


def zoom_out(pool, out, tiles):
    """Zoom levels -1 .. MIN_ZOOM under out/<z>/ from the tiles at zoom 0."""
    for z in range(-1, MIN_ZOOM - 1, -1):
        os.makedirs(os.path.join(out, str(z)), exist_ok=True)
        parents = {(x >> 1, y >> 1) for x, y in tiles}
        jobs = []
        for px, pz in parents:
            kids = {}
            for qx in (0, 1):
                for qz in (0, 1):
                    k = (px * 2 + qx, pz * 2 + qz)
                    kids[(qx, qz)] = k if k in tiles else None
            jobs.append((out, z, px, pz, kids))
        for _ in pool.imap_unordered(zoom_job, jobs): pass
        tiles = parents


# ---------------------------------------------------------------- the world
def find_regions(world):
    """{'overworld': {(rx, rz): path}, 'nether': {...}}"""
    regs = {'overworld': {}, 'nether': {}}
    for name, sub in (('overworld', 'region'), ('nether', os.path.join('DIM-1', 'region'))):
        for p in glob.glob(os.path.join(world, sub, 'r.*.*.mcr')):
            m = re.search(r'r\.(-?\d+)\.(-?\d+)\.mcr$', p)
            if m and os.path.getsize(p) > 8192: regs[name][(int(m[1]), int(m[2]))] = p
    return regs


def level_info(world):
    """(spawn [x, z] or None, when the world was last saved as a UTC datetime: the later of level.dat's LastPlayed
    and the newest region file's time, since some servers never update LastPlayed)"""
    times = [os.path.getmtime(p) for r in find_regions(world).values() for p in r.values()]
    try:
        data = field(parse(gzip.decompress(open(os.path.join(world, 'level.dat'), 'rb').read())), 'Data')
        spawn = [field(data, 'SpawnX'), field(data, 'SpawnZ')]
        try: times.append(field(data, 'LastPlayed') / 1000)
        except Exception: pass
    except Exception:
        spawn = None
    return spawn, datetime.datetime.fromtimestamp(max(times), datetime.timezone.utc) if times else datetime.datetime.now(datetime.timezone.utc)


def players_ever(world):
    """How many players have ever joined: the world keeps players/NAME.dat for each (Bedrock players through a proxy
    often have names starting with "."), counted once per name whatever the case. None without a players folder."""
    try: return len({f[:-4].lower() for f in os.listdir(os.path.join(world, 'players')) if f.endswith('.dat')})
    except OSError: return None


def sha(p):
    h = hashlib.sha1()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def read_places(path):
    """"overworld|nether X Z Name" per line, # for comments."""
    out = []
    for l in open(path, encoding='utf-8'):
        p = l.strip().split(None, 3)
        if len(p) == 4 and not p[0].startswith('#') and p[0] in ('overworld', 'nether'):
            try: out.append({'world': p[0], 'x': int(p[1]), 'z': int(p[2]), 'name': p[3][:60]})
            except ValueError: print('places: skipped', l.strip())
    return out


def build(a):
    world, out = os.path.abspath(a.world), os.path.abspath(a.out)
    regs = find_regions(world)
    if not regs['overworld']:
        if glob.glob(os.path.join(world, 'region', '*.mca')):
            sys.exit('%s is an Anvil world (Minecraft 1.2 or newer); beta-map only reads Beta worlds (.mcr files).' % world)
        sys.exit('No Beta region files (region/r.X.Z.mcr) in %s. Point it at the world folder, the one with level.dat.' % world)
    spawn, when = level_info(world)
    if a.spawn:
        try: spawn = [int(v) for v in a.spawn.split(',')]
        except ValueError: sys.exit('--spawn: give it as X,Z')
    day = when.strftime('%Y-%m-%d_%H%M%S')
    cache = os.path.abspath(a.cache or out.rstrip('/\\') + '-cache')
    tiles_root = os.path.join(out, 'tiles'); day_dir = os.path.join(tiles_root, day)
    os.makedirs(cache, exist_ok=True); shutil.rmtree(day_dir, ignore_errors=True); os.makedirs(day_dir)
    print('%s: %d Overworld and %d Nether region(s), as of %s UTC'
          % (world, len(regs['overworld']), len(regs['nether']), when.strftime('%Y-%m-%d %H:%M')), flush=True)
    layers, keep = {}, set()
    pool = multiprocessing.Pool(a.jobs or max(1, (os.cpu_count() or 2) - 1))
    try:
        hashes = {w: {k: sha(p) for k, p in r.items()} for w, r in regs.items()}
        # top-down pictures and heights
        for layer, w, hlayer in (('flat', 'overworld', 'height'), ('nether', 'nether', 'nether-height')):
            rs = regs[w]
            if not rs: continue
            o = os.path.join(day_dir, layer); os.makedirs(os.path.join(o, '0'))
            ho = os.path.join(day_dir, hlayer, '0'); os.makedirs(ho)
            jobs = [(p, os.path.join(o, '0', '%d_%d.png' % k), os.path.join(ho, '%d_%d.png' % k), w == 'nether') for k, p in rs.items()]
            for _ in pool.imap_unordered(flat_job, jobs): pass
            zoom_out(pool, o, set(rs))
            layers[layer] = {'bounds': [min(r[0] for r in rs) * T, min(r[1] for r in rs) * T,
                                        (max(r[0] for r in rs) + 1) * T, (max(r[1] for r in rs) + 1) * T]}
            print('  %s: %d region(s)' % (layer, len(rs)), flush=True)
        # the isometric pictures: each region's picture per direction cached by its file's hash
        jobs = {}
        for base_name, w in (('iso', 'overworld'), ('nether-iso', 'nether')):
            rs = regs[w]
            if not rs: continue
            for k in range(4):
                layer = '%s%d' % (base_name, k)
                cache_of = {turn_region(*key, k): os.path.join(cache, '%s-v%s-%s.bin' % (layer, VERSION, hashes[w][key])) for key in rs}
                todo = [(rs[key], cache_of[turn_region(*key, k)], w == 'nether', k) for key in rs
                        if not os.path.exists(cache_of[turn_region(*key, k)])]
                jobs[layer] = (cache_of, todo)
        alljobs = [j for _, todo in jobs.values() for j in todo]
        print('  drawing %d region picture(s) (%d from the cache)...' % (len(alljobs), sum(len(c) for c, _ in jobs.values()) - len(alljobs)), flush=True)
        for n, _ in enumerate(pool.imap_unordered(iso_job, alljobs), 1):
            if n % 10 == 0 or n == len(alljobs): print('    %d/%d' % (n, len(alljobs)), flush=True)
        for layer, (cache_of, todo) in jobs.items():
            o = os.path.join(day_dir, layer); os.makedirs(os.path.join(o, '0'))
            tiles, bounds, torches = iso_compose(set(cache_of), cache_of, os.path.join(o, '0'),
                                                 os.path.join(day_dir, layer + '-torches', '0'))
            zoom_out(pool, o, tiles)
            print('  %s: %d tiles, %d with torches' % (layer, len(tiles), len(torches)), flush=True)
            layers[layer] = {'bounds': bounds}
            layers[layer + '-torches'] = {'list': torches}   # zoom 0 only: too small to see further out
            keep |= set(cache_of.values())
        for f in glob.glob(os.path.join(cache, '*.bin')):  # the cache keeps only this world's current regions
            if f not in keep: os.remove(f)
    finally:
        pool.close(); pool.join()

    # map.json: what the page shows
    hcache = {}
    def height(w, x, z):                               # the top block's height there; None if unknown
        name = os.path.join(day_dir, 'height' if w == 'overworld' else 'nether-height', '0', '%d_%d.png' % (x >> 9, z >> 9))
        if name not in hcache: hcache[name] = png_read(name) if os.path.exists(name) else None
        hp = hcache[name]; i = 3 * ((z & 511) * T + (x & 511))
        return hp[i] if hp and hp[i + 1] else None
    places = read_places(a.places) if a.places else []
    for p in places: p['y'] = height(p['world'], p['x'], p['z'])
    info = {'id': day, 'title': a.title, 'taken': when.strftime('%Y-%m-%d %H:%M'), 'spawn': spawn,
            'spawnY': height('overworld', *spawn) if spawn else None, 'live': a.live,
            'players': None if a.no_player_count else players_ever(world),
            'tile': T, 'minZoom': MIN_ZOOM, 'layers': layers, 'places': places}
    for f in os.listdir(WEB):
        shutil.copy(os.path.join(WEB, f), os.path.join(out, f))
    open(os.path.join(out, '.nojekyll'), 'w').close()  # GitHub Pages: serve the files as they are
    tmp = os.path.join(out, 'map.json.tmp')
    with open(tmp, 'w') as f: json.dump(info, f)
    os.replace(tmp, os.path.join(out, 'map.json'))
    for d in glob.glob(os.path.join(tiles_root, '*')):   # older maps go once the new one is in place
        if os.path.basename(d) != day: shutil.rmtree(d, ignore_errors=True)
    print('Done: open %s through a web server (see the README).' % os.path.join(out, 'index.html'))


def main():
    ap = argparse.ArgumentParser(description='Draw an isometric web map of a Minecraft Beta (McRegion) world.')
    ap.add_argument('world', help='the world folder (with level.dat and region/)')
    ap.add_argument('out', help='where the website goes (made if missing; its tiles folder is replaced)')
    ap.add_argument('--title', default='Minecraft Beta map', help='shown at the top left of the page')
    ap.add_argument('--places', help='a text file of named places: "overworld X Z Name" per line')
    ap.add_argument('--spawn', help='X,Z to mark as spawn and start at (default: the world\'s spawn point)')
    ap.add_argument('--live', help='URL of a players.json for live player dots (see the README)')
    ap.add_argument('--no-player-count', action='store_true', help='hide the "Visitors" counter (players who ever joined)')
    ap.add_argument('--cache', help='where finished region pictures are kept between runs (default: OUT-cache)')
    ap.add_argument('--jobs', type=int, help='how many processes draw at once (default: CPU cores - 1)')
    build(ap.parse_args())


if __name__ == '__main__':
    main()
