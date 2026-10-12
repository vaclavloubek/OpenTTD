#!/usr/bin/env python3
"""
Second prototype of the 3D cab view: the same perspective view, but dressed in the game's own
graphics (OpenGFX), so it keeps the look of Transport Tycoon.

- The ground of every tile is the tile's own sprite (grass, rails, road, field, water, garden of a
  house), "unfolded" from the isometric diamond back onto the flat tile and drawn in perspective.
- Houses, trees, signals, station buildings and trains stay as their isometric sprites, standing
  upright like cardboard cut-outs and scaled with distance. Trains pick one of their 8 sprites by
  the angle we see them from.
- Everything is rendered at GUI scale 1x and enlarged 2x without smoothing, i.e. the chunky
  pixels of the game, and every colour is snapped to the 8bpp palette.

Usage: python3 mockup_ttd.py <OpenGFX source dir> <kabina screenshot 2x> <output dir>
"""

import math
import random
import sys

import numpy as np
from PIL import Image

from ogfx import OpenGFX
import mockup

TILE = 16.0
LEVEL = 8.0
TRACK_LEVEL = 2
MAP = 96

W, H = 634, 310               # render size at GUI scale 1x (the window is 2x this on Retina)
FOV_DEG = 64
EYE = 8.0
PITCH_DEG = -6.0
VIEW_TILES = 48
FOG_START = 12 * TILE
FOG_END = VIEW_TILES * TILE
SPRITE_UNITS = 16 * math.sqrt(2) / 64 * 1.15  # world units per sprite pixel (tile diagonal = 64 px)

SUN = np.array([-0.55, 0.35, 0.76])
SUN = SUN / np.linalg.norm(SUN)

# Sprite numbers (src/table/sprites.h)
SPR_FLAT_GRASS = 3981
SPR_RAIL_Y, SPR_RAIL_X = 1011, 1012
SPR_RAIL_W_CORNER, SPR_RAIL_E_CORNER = 1016, 1015  # pieces around the corner (16, 0) and (0, 16)
SPR_ROAD_Y, SPR_ROAD_X = 1332, 1333
SPR_WATER = 4061
SPR_FIELD = 4126
SPR_STATION_BUILDING_Y = 1074
SPR_PLATFORM_Y_FRONT, SPR_PLATFORM_Y_REAR = 1069, 1071
TREES = [1576, 1583, 1590, 1604, 1618, 1646, 1653, 1660, 1674, 1681, 1688, 1695, 1702]
TREE_GROWN = 3                 # growth stage of a full grown tree
HOUSES_SMALL = [(1491, 1492), (1489, 1490), (1487, 1488), (1493, 1494), (1495, 1496), (1509, 1510),
	(1511, 1512), (1515, 1516), (1517, 1518), (1566, 1567), (1568, 1569), (1572, 1573), (1574, 1575)]
HOUSES_BIG = [(None, 1441), (None, 1449), (None, 1459), (None, 1462), (None, 1465), (None, 1468),
	(None, 1477), (None, 1478), (1530, 1531), (1534, 1535), (1536, 1537), (1538, 1539), (1548, 1549),
	(1550, 1551), (1542, 1543)]
SH125 = 2941    # 8 views
WAGON = 2733    # 4 views
SIGNAL_GREEN, SIGNAL_RED = 1280, 1279


class Gfx:
	def __init__(self, root):
		self.g = OpenGFX(root)
		pal = None
		self.cache = {}
		# Palette of the base set (the DOS palette, same as src/table/palettes.h).
		path = self.g.sprites[SPR_FLAT_GRASS][0]
		self.g.get(SPR_FLAT_GRASS)
		im = self.g.images[f'{root}/{path}']
		self.pal = np.array(im.getpalette()[:768], dtype=float).reshape(-1, 3)
		# RGB -> palette index lookup, 32 levels per channel (the game would build this once).
		usable = np.arange(1, 215)
		lv = (np.arange(32) * 255 / 31)
		r, g, b = np.meshgrid(lv, lv, lv, indexing='ij')
		cube = np.stack([r, g, b], axis=-1).reshape(-1, 3)
		best = np.zeros(len(cube), dtype=np.uint8)
		bestd = np.full(len(cube), 1e9)
		for i in usable:
			d = ((cube - self.pal[i]) ** 2).sum(axis=1)
			m = d < bestd
			bestd[m] = d[m]
			best[m] = i
		self.lut = best.reshape(32, 32, 32)
		self.grass = self.sprite(SPR_FLAT_GRASS)[0]

	def sprite(self, sid):
		if sid not in self.cache:
			self.cache[sid] = self.g.get(sid)
		return self.cache[sid]

	def snap(self, rgb):
		q = np.clip(np.rint(rgb * 31 / 255), 0, 31).astype(int)
		return self.lut[q[..., 0], q[..., 1], q[..., 2]]

	def ground_texture(self, sids):
		"""64x31 tile texture: the first sprite, with the non-grass pixels of the others on top."""
		base = self.sprite(sids[0])[0].copy()
		for s in sids[1:]:
			t = self.sprite(s)[0]
			c = self.pal[t]
			m = ~((c[..., 1] > c[..., 0] + 8) & (c[..., 1] > c[..., 2])) & (t != 0)
			base[m] = t[m]
		return base


# ---------------------------------------------------------------------------------------------

def build_track(i0, j0, plan):
	"""TTD-style track: plan is a list of ('Y', n) and ('D', n) (diagonal pieces to the right)."""
	tiles = {}
	pts = []
	i, j, entry = i0, j0, 'bottom'
	for kind, n in plan:
		for _ in range(n):
			if kind == 'Y' and entry == 'bottom':
				tiles.setdefault((i, j), []).append('Y')
				pts += [(i * TILE + 8, j * TILE), (i * TILE + 8, j * TILE + 16)]
				j += 1
			elif entry == 'bottom':   # diagonal piece around the west corner
				tiles.setdefault((i, j), []).append('L')
				pts += [(i * TILE + 8, j * TILE), (i * TILE + 16, j * TILE + 8)]
				i, entry = i + 1, 'left'
			else:                     # diagonal piece around the east corner
				tiles.setdefault((i, j), []).append('R')
				pts += [(i * TILE, j * TILE + 8), (i * TILE + 8, j * TILE + 16)]
				j, entry = j + 1, 'bottom'
	# Dense centreline with heading, 1 unit steps.
	line = []
	for a, b in zip(pts[::2], pts[1::2]):
		d = math.hypot(b[0] - a[0], b[1] - a[1])
		hd = math.atan2(b[0] - a[0], b[1] - a[1])
		for s in np.arange(0, d, 1.0):
			line.append((a[0] + (b[0] - a[0]) * s / d, a[1] + (b[1] - a[1]) * s / d, hd))
	line = np.array(line)
	# The rails themselves get a small radius in the 45 degree bends (the ballast stays as in the game).
	k = 9
	sm = line.copy()
	for c in (0, 1):
		pad = np.pad(line[:, c], k, mode='edge')
		sm[:, c] = np.convolve(pad, np.ones(2 * k + 1) / (2 * k + 1), mode='valid')
	d = np.diff(sm[:, :2], axis=0)
	sm[1:, 2] = np.arctan2(d[:, 0], d[:, 1])
	return tiles, sm


class World:
	def __init__(self, gfx):
		self.gfx = gfx
		rnd = random.Random(15_3)
		plan = [('Y', 22), ('D', 6), ('Y', 60)]
		self.t1, self.track1 = build_track(31, -4, plan)
		self.t2, self.track2 = build_track(32, -4, plan)
		self.rails = {}
		for t in (self.t1, self.t2):
			for k, v in t.items():
				self.rails.setdefault(k, []).extend(v)

		n = MAP + 1
		cx, cy = np.meshgrid(np.arange(n) * TILE, np.arange(n) * TILE, indexing='ij')
		h = 2.0 \
			+ 1.3 * np.sin(cx / 170.0 + 1.0) * np.cos(cy / 210.0) \
			+ 5.0 * np.exp(-(((cx - 52 * TILE) / (9 * TILE)) ** 2 + ((cy - 22 * TILE) / (8 * TILE)) ** 2)) \
			+ 4.0 * np.exp(-(((cx - 14 * TILE) / (8 * TILE)) ** 2 + ((cy - 36 * TILE) / (8 * TILE)) ** 2)) \
			+ 3.0 * np.exp(-(((cx - 46 * TILE) / (8 * TILE)) ** 2 + ((cy - 56 * TILE) / (8 * TILE)) ** 2))
		lake = (((cx - 19 * TILE) / (6 * TILE)) ** 2 + ((cy - 14 * TILE) / (4 * TILE)) ** 2) < 1
		h = np.round(h)
		h[lake] = 0
		forced = lake.copy()
		flat = np.zeros_like(forced)
		for (i, j) in self.rails:
			if 0 <= i < MAP and 0 <= j < MAP:
				flat[max(i - 1, 0):i + 3, max(j - 1, 0):j + 3] = True
		self.town = (24, 34, 22, 34)   # i0, i1, j0, j1
		ti0, ti1, tj0, tj1 = self.town
		flat[ti0:ti1 + 1, tj0:tj1 + 1] = True
		h[flat] = TRACK_LEVEL
		forced |= flat
		for _ in range(60):
			pad = np.pad(h, 1, mode='edge')
			nb = np.stack([pad[:-2, 1:-1], pad[2:, 1:-1], pad[1:-1, :-2], pad[1:-1, 2:]])
			h = np.where(forced, h, np.clip(h, nb.max(axis=0) - 1, nb.min(axis=0) + 1))
		self.h = np.clip(h, 0, 15)

		self.ground = {}     # tile -> list of sprite ids for the ground
		self.objects = {}    # tile -> [(kind, ...)]
		self.flat_boxes = {}  # tile -> platform slabs

		# Station on the straight after the curve: platforms left of track 1 and between the tracks.
		self.station = (27, 32)
		x1 = self.track1[-1, 0]
		for j in range(*self.station):
			i1 = int(x1 // TILE)
			self.add(('platform', (x1 - 8.0, j * TILE, x1 - 3.4, j * TILE + 16)), (i1, j))
			self.add(('platform', (x1 + 3.4, j * TILE, x1 + 12.6, j * TILE + 16)), (i1 + 1, j))
		self.add(('sprite', SPR_STATION_BUILDING_Y, (x1 - 15, 29.5 * TILE), 'house'), (int((x1 - 15) // TILE), 29))

		# Signals on the right of the tracks.
		for line, y, sid in ((self.track1, 20.5 * TILE, SIGNAL_GREEN), (self.track2, 26 * TILE, SIGNAL_RED)):
			k = int(np.abs(line[:, 1] - y).argmin())
			x, yy, hd = line[k]
			x += math.cos(hd) * 5
			yy -= math.sin(hd) * 5
			self.add(('sprite', sid, (x, yy), 'signal'), (int(x // TILE), int(yy // TILE)))

		# Town: streets and houses left of the station.
		for i in range(ti0, ti1 + 1):
			self.ground[(i, 28)] = [SPR_ROAD_X]
		for j in range(tj0, tj1 + 1):
			self.ground[(29, j)] = [SPR_ROAD_Y]
		for i in range(ti0, ti1 + 1):
			for j in range(tj0, tj1 + 1):
				if (i, j) in self.ground or (i, j) in self.rails or i >= int(x1 // TILE) - 1:
					continue
				if (i, j) in self.objects or rnd.random() < 0.1:
					continue
				centre = abs(i - 29) + abs(j - 28) < 4
				gnd, bld = rnd.choice(HOUSES_BIG if centre and rnd.random() < 0.7 else HOUSES_SMALL)
				if gnd:
					self.ground[(i, j)] = [gnd]
				self.add(('sprite', bld, (i * TILE + 8, j * TILE + 8), 'house'), (i, j))

		# Fields on the right near the line.
		for i in range(35, 42):
			for j in range(0, 12):
				if (i, j) not in self.rails:
					self.ground[(i, j)] = [SPR_FIELD + (i // 3 + j // 4) % 3]

		# Trees.
		for i in range(MAP):
			for j in range(MAP):
				if (i, j) in self.rails or (i, j) in self.ground or self.is_water(i, j) or (i, j) in self.objects:
					continue
				if ti0 <= i <= ti1 and tj0 <= j <= tj1:
					continue
				dens = 0.12 + 0.6 * max(0.0, (self.h[i, j] - 3) / 3)
				if 29 <= i <= 35 and j < 40:
					dens *= 0.3
				if 26 <= i <= 37 and 10 <= j <= 17:
					dens = 0.0
				kind = rnd.choice(TREES)
				for _ in range(3):
					if rnd.random() < dens:
						x = i * TILE + rnd.uniform(2, 14)
						y = j * TILE + rnd.uniform(2, 14)
						self.add(('sprite', kind + TREE_GROWN, (x, y), 'tree'), (i, j))

		# Another train standing in the station on track 2, front towards us.
		for k in range(5):
			yc = 18.5 * TILE + k * 7.6
			idx = int(np.abs(self.track2[:, 1] - yc).argmin())
			x, y, hd = self.track2[idx]
			self.add(('train', SH125 if k == 0 else WAGON, (x, y), hd + math.pi, k > 0), (int(x // TILE), int(y // TILE)))

	def add(self, obj, tile):
		self.objects.setdefault(tile, []).append(obj)

	def is_water(self, i, j):
		return self.h[i:i + 2, j:j + 2].max() == 0

	def z_at(self, x, y):
		i, j = int(x // TILE), int(y // TILE)
		if not (0 <= i < MAP and 0 <= j < MAP):
			return 0.0
		fx, fy = x / TILE - i, y / TILE - j
		h = self.h
		return LEVEL * (h[i, j] * (1 - fx) * (1 - fy) + h[i + 1, j] * fx * (1 - fy) + h[i, j + 1] * (1 - fx) * fy
			+ h[i + 1, j + 1] * fx * fy)

	def ground_sprites(self, i, j):
		if (i, j) in self.rails:
			kinds = self.rails[(i, j)]
			m = {'Y': SPR_RAIL_Y, 'L': SPR_RAIL_W_CORNER, 'R': SPR_RAIL_E_CORNER}
			return [m[k] for k in kinds]
		if (i, j) in self.ground:
			return self.ground[(i, j)]
		if self.is_water(i, j):
			return [SPR_WATER]
		return [SPR_FLAT_GRASS]


# ---------------------------------------------------------------------------------------------

class Renderer:
	def __init__(self, world, gfx, cam_line_index):
		self.w, self.gfx = world, gfx
		x, y, hd = world.track1[cam_line_index]
		self.cam = np.array([x, y, TRACK_LEVEL * LEVEL + EYE])
		self.yaw = hd
		p = math.radians(PITCH_DEG)
		f = np.array([math.sin(hd), math.cos(hd), 0.0])
		up = np.array([0.0, 0.0, 1.0])
		self.right = np.array([math.cos(hd), -math.sin(hd), 0.0])
		self.fwd = f * math.cos(p) + up * math.sin(p)
		self.up = -f * math.sin(p) + up * math.cos(p)
		self.focal = (W / 2) / math.tan(math.radians(FOV_DEG) / 2)
		self.frame = np.zeros((H, W, 3))
		self.fog = np.array([156.0, 204.0, 220.0])
		self.texcache = {}

	def to_cam(self, p):
		d = np.asarray(p, dtype=float) - self.cam
		return np.array([d @ self.right, d @ self.fwd, d @ self.up])

	def fogged(self, rgb, depth):
		f = np.clip((depth - FOG_START) / (FOG_END - FOG_START), 0, 1) ** 0.8
		return rgb * (1 - f[..., None]) + self.fog * f[..., None]

	# --- sky -----------------------------------------------------------------------------------
	def sky(self):
		horizon = H / 2 - self.focal * math.tan(math.radians(-PITCH_DEG))
		bands = np.array([(56, 120, 188), (72, 152, 216), (100, 172, 224), (132, 188, 216), (156, 204, 220)], float)
		ys = np.arange(H)[:, None]
		xs = np.arange(W)[None, :]
		t = np.clip(ys / max(horizon, 1), 0, 0.999) * (len(bands) - 1)
		k = t.astype(int)
		frac = t - k
		# Two-colour checker dithering between the palette bands, like FILLRECT_CHECKER.
		use_next = (frac > 0.66) | ((frac > 0.33) & ((xs + ys) % 2 == 0))
		k2 = np.minimum(k + use_next, len(bands) - 1)
		self.frame[:] = bands[np.broadcast_to(k2, (H, W))]
		self.frame[ys[:, 0] >= horizon] = self.fog

	# --- textured triangles ----------------------------------------------------------------------
	def tri(self, P, L, tex, shade):
		"""P: 3 world points, L: 3 tile-local (lx, ly) coordinates, tex: 64x31 palette index sprite."""
		C = [self.to_cam(p) for p in P]
		poly = list(zip(C, L))
		near = 0.5
		out = []
		for k in range(len(poly)):
			(a, la), (b, lb) = poly[k], poly[(k + 1) % len(poly)]
			if a[1] >= near:
				out.append((a, np.asarray(la, float)))
			if (a[1] >= near) != (b[1] >= near):
				t = (near - a[1]) / (b[1] - a[1])
				out.append((a + (b - a) * t, np.asarray(la, float) + (np.asarray(lb, float) - np.asarray(la, float)) * t))
		for k in range(1, len(out) - 1):
			self.raster(out[0], out[k], out[k + 1], tex, shade)

	def raster(self, A, B, Cc, tex, shade):
		verts = [A, B, Cc]
		sx = np.array([W / 2 + self.focal * v[0][0] / v[0][1] for v in verts])
		sy = np.array([H / 2 - self.focal * v[0][2] / v[0][1] for v in verts])
		iw = np.array([1 / v[0][1] for v in verts])
		lx = np.array([v[1][0] for v in verts])
		ly = np.array([v[1][1] for v in verts])
		x0, x1 = max(int(math.floor(sx.min())), 0), min(int(math.ceil(sx.max())), W - 1)
		y0, y1 = max(int(math.floor(sy.min())), 0), min(int(math.ceil(sy.max())), H - 1)
		if x0 > x1 or y0 > y1:
			return
		area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])
		if abs(area) < 1e-9:
			return
		X, Y = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
		b0 = ((sx[1] - X) * (sy[2] - Y) - (sx[2] - X) * (sy[1] - Y)) / area
		b1 = ((sx[2] - X) * (sy[0] - Y) - (sx[0] - X) * (sy[2] - Y)) / area
		b2 = 1 - b0 - b1
		inside = (b0 >= -1e-6) & (b1 >= -1e-6) & (b2 >= -1e-6)
		if not inside.any():
			return
		den = b0 * iw[0] + b1 * iw[1] + b2 * iw[2]
		u = (b0 * iw[0] * lx[0] + b1 * iw[1] * lx[1] + b2 * iw[2] * lx[2]) / den
		v = (b0 * iw[0] * ly[0] + b1 * iw[1] * ly[1] + b2 * iw[2] * ly[2]) / den
		depth = 1 / den
		u = np.clip(u, 0, 15.999)
		v = np.clip(v, 0, 15.999)
		# Unfold the isometric diamond: tile corner (x, y) is at sprite pixel (32 + 2(y - x), (x + y) * 31/32).
		px = np.clip((31.5 + 2 * (v - u)).astype(int), 0, 63)
		py = np.clip(((u + v) * 30.99 / 32).astype(int), 0, 30)
		idx = tex[py, px]
		idx = np.where(idx == 0, self.gfx.grass[py, px], idx)
		idx = np.where(idx == 0, 0x52, idx)
		rgb = self.gfx.pal[idx] * shade
		rgb = self.fogged(rgb, depth)
		region = self.frame[y0:y1 + 1, x0:x1 + 1]
		region[inside] = rgb[inside]

	def ground(self, i, j):
		w = self.w
		key = tuple(w.ground_sprites(i, j))
		if key not in self.texcache:
			self.texcache[key] = self.gfx.ground_texture(list(key))
		tex = self.texcache[key]
		x0, y0 = i * TILE, j * TILE
		h = w.h
		P = [np.array([x0, y0, h[i, j] * LEVEL]), np.array([x0 + TILE, y0, h[i + 1, j] * LEVEL]),
			np.array([x0 + TILE, y0 + TILE, h[i + 1, j + 1] * LEVEL]), np.array([x0, y0 + TILE, h[i, j + 1] * LEVEL])]
		L = [(0, 0), (16, 0), (16, 16), (0, 16)]
		for a, b, c in ((0, 1, 2), (0, 2, 3)):
			n = np.cross(P[b] - P[a], P[c] - P[a])
			n = n / np.linalg.norm(n)
			shade = 0.78 + 0.32 * max(0.0, float(n @ SUN)) - 0.32 * float(np.array([0, 0, 1.0]) @ SUN)
			self.tri([P[a], P[b], P[c]], [L[a], L[b], L[c]], tex, shade + 0.12)

	# --- cut-out sprites -----------------------------------------------------------------------
	def billboard(self, sid, wx, wy, kind):
		idx, xo, yo = self.gfx.sprite(sid)
		z = self.w.z_at(wx, wy)
		c = self.to_cam((wx, wy, z))
		if c[1] < 1.0:
			return
		s = self.focal * SPRITE_UNITS / c[1]      # screen pixels per sprite pixel
		ax = W / 2 + self.focal * c[0] / c[1]
		ay = H / 2 - self.focal * c[2] / c[1]
		# Sprite origin: houses/stations = north corner of the tile, the centre of the tile is 16 px lower.
		oy = 16 if kind == 'house' else 0
		left = ax + (xo) * s
		top = ay + (yo - oy) * s
		hgt, wid = idx.shape
		X0, X1 = int(math.floor(left)), int(math.ceil(left + wid * s))
		Y0, Y1 = int(math.floor(top)), int(math.ceil(top + hgt * s))
		cx0, cx1, cy0, cy1 = max(X0, 0), min(X1, W), max(Y0, 0), min(Y1, H)
		if cx0 >= cx1 or cy0 >= cy1:
			return
		xs = ((np.arange(cx0, cx1) + 0.5 - left) / s).astype(int)
		ys = ((np.arange(cy0, cy1) + 0.5 - top) / s).astype(int)
		ok_x = (xs >= 0) & (xs < wid)
		ok_y = (ys >= 0) & (ys < hgt)
		sub = idx[np.clip(ys, 0, hgt - 1)][:, np.clip(xs, 0, wid - 1)]
		mask = (sub != 0) & ok_y[:, None] & ok_x[None, :]
		rgb = self.fogged(self.gfx.pal[sub], np.full(sub.shape, c[1]))
		region = self.frame[cy0:cy1, cx0:cx1]
		region[mask] = rgb[mask]

	def train(self, base, wx, wy, heading, is_wagon):
		# Pick the sprite showing the vehicle as we see it: 8 directions, 0 = moving away from us (N).
		rel = (heading - self.yaw) % (2 * math.pi)
		d = int(round(rel / (math.pi / 4))) % 8
		self.billboard(base + (d % 4 if is_wagon else d), wx, wy, 'vehicle')

	def platform(self, x0, y0, x1, y1):
		"""Platforms as low slabs in the platform colours of the sprites (light grey, yellow edge)."""
		z0, z1 = TRACK_LEVEL * LEVEL, TRACK_LEVEL * LEVEL + 1.6
		top = self.gfx.pal[[0x0D]][0] if False else np.array([200.0, 200.0, 200.0])
		edge = np.array([252.0, 212.0, 0.0])
		side = np.array([148.0, 148.0, 148.0])
		quads = [
			([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], top),
			([(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)], side),
			([(x1, y1, z0), (x1, y0, z0), (x1, y0, z1), (x1, y1, z1)], side),
			([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], side),
		]
		for pts, col in quads:
			n_vis = self.flat_poly(pts, col)
		for xe in (x0, x1):
			self.flat_poly([(xe - 0.5, y0, z1 + 0.01), (xe + 0.5, y0, z1 + 0.01), (xe + 0.5, y1, z1 + 0.01),
				(xe - 0.5, y1, z1 + 0.01)], edge)

	def flat_poly(self, pts, col):
		tex = np.zeros((31, 64), dtype=np.uint8)
		rgbcol = np.asarray(col, float)
		# Draw untextured: reuse the rasteriser with a one-colour texture from the nearest palette entry.
		pi = int(self.gfx.snap(rgbcol))
		tex[:] = pi
		self.tri([np.array(p) for p in pts[:3]], [(0, 0)] * 3, tex, 1.0)
		self.tri([np.array(p) for p in (pts[0], pts[2], pts[3])], [(0, 0)] * 3, tex, 1.0)

	def rails(self, i, j):
		"""Sleepers and rails on top of the ballast of the rail sprite, so they stay sharp up close."""
		z = TRACK_LEVEL * LEVEL
		sleeper = np.array([72.0, 44.0, 4.0])
		rail = np.array([200.0, 200.0, 200.0])
		for line in (self.w.track1, self.w.track2):
			sel = np.nonzero((line[:, 0] // TILE == i) & (line[:, 1] // TILE == j))[0]
			if len(sel) == 0:
				continue
			near = math.hypot(line[sel[0], 0] - self.cam[0], line[sel[0], 1] - self.cam[1]) < 10 * TILE
			for k in sel[::3] if near else []:
				x, y, hd = line[k]
				r = np.array([math.cos(hd), -math.sin(hd)])
				f = np.array([math.sin(hd), math.cos(hd)]) * 0.45
				c = np.array([x, y])
				q = [c - r * 2.2 - f, c + r * 2.2 - f, c + r * 2.2 + f, c - r * 2.2 + f]
				self.flat_poly([(*p, z + 0.05) for p in q], sleeper)
			a, b = sel[0], min(sel[-1] + 1, len(line) - 1)
			for side in (-1.15, 1.15):
				for k in range(a, b, 2):
					k2 = min(k + 2, b)
					p0, p1 = line[k], line[k2]
					r0 = np.array([math.cos(p0[2]), -math.sin(p0[2])])
					r1 = np.array([math.cos(p1[2]), -math.sin(p1[2])])
					w = 0.22
					q = [p0[:2] + r0 * (side - w), p0[:2] + r0 * (side + w), p1[:2] + r1 * (side + w), p1[:2] + r1 * (side - w)]
					self.flat_poly([(*p, z + 0.1) for p in q], rail)

	def objects(self, i, j):
		objs = self.w.objects.get((i, j), [])

		def dist(o):
			if o[0] == 'platform':
				x0, y0, x1, y1 = o[1]
				p = ((x0 + x1) / 2, (y0 + y1) / 2)
			else:
				p = o[2]
			return -math.hypot(p[0] - self.cam[0], p[1] - self.cam[1])
		for o in sorted(objs, key=dist):
			if o[0] == 'platform':
				self.platform(*o[1])
			elif o[0] == 'sprite':
				self.billboard(o[1], o[2][0], o[2][1], o[3])
			elif o[0] == 'train':
				self.train(o[1], o[2][0], o[2][1], o[3], o[4])

	def visible_tiles(self):
		tiles = []
		half = math.radians(FOV_DEG) / 2 + 0.3
		for i in range(MAP):
			for j in range(MAP):
				d = np.array([(i + 0.5) * TILE, (j + 0.5) * TILE]) - self.cam[:2]
				dist = math.hypot(*d)
				if dist > FOG_END + TILE:
					continue
				ang = (math.atan2(d[0], d[1]) - self.yaw + math.pi) % (2 * math.pi) - math.pi
				if dist > 2 * TILE and abs(ang) > half:
					continue
				if d[0] * math.sin(self.yaw) + d[1] * math.cos(self.yaw) < -TILE:
					continue
				tiles.append((dist, i, j))
		tiles.sort(reverse=True)
		return tiles

	def render(self):
		self.sky()
		for _, i, j in self.visible_tiles():
			self.ground(i, j)
			if (i, j) in self.w.rails:
				self.rails(i, j)
			self.objects(i, j)
		idx = self.gfx.snap(self.frame)
		rgb = self.gfx.pal[idx].astype(np.uint8)
		return Image.fromarray(rgb)


def main():
	root, shot_path, out_dir = sys.argv[1:4]
	gfx = Gfx(root)
	world = World(gfx)
	r = Renderer(world, gfx, cam_line_index=int(np.abs(world.track1[:, 1] - 14 * TILE).argmin()))
	img1x = r.render()
	img1x.save(f'{out_dir}/ttd_pohled_1x.png')
	img2x = img1x.resize((W * 2, H * 2), Image.NEAREST)
	img2x.save(f'{out_dir}/ttd_pohled_2x.png')
	view = img2x.crop((0, 0, 1268, 620))
	mockup.compose(shot_path, view, f'{out_dir}/ttd_kabina.png', '3D')


if __name__ == '__main__':
	main()
