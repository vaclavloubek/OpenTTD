#!/usr/bin/env python3
"""
Prototype of the simple 3D cab view, used to make the mockup images for the design (DESIGN_3D.md).

It deliberately draws the same way the game would: a list of flat-coloured polygons per tile,
painted from the farthest tile to the nearest (painter's algorithm), every colour snapped to the
OpenTTD 8bpp palette, so the result is what GfxFillPolygon/GfxDrawLine can produce on any blitter.
Nothing here is game code; it only shows the look and checks the numbers (scale, field of view,
view distance, fog).

Usage: python3 mockup.py <openttd palettes.h> <kabina screenshot 2x> <output dir>
"""

import math
import random
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------------------------
# Units and scale (same as the game): a tile is 16 units, a height level 8 units.
# A wagon is 8 units long (~20 m), so 1 unit ~ 2.5 m.
TILE = 16.0
LEVEL = 8.0
TRACK_LEVEL = 2

MAP = 112                 # tiles in each direction of the prototype map
VIEW_TILES = 56           # view distance in tiles
FOG_START = 14 * TILE     # fog starts here ...
FOG_END = VIEW_TILES * TILE  # ... and is complete here, so the map edge melts into the sky
FOV_DEG = 64              # horizontal field of view
EYE = 8.0                 # driver's eye above the rail head (units), roof height of an OpenTTD train
PITCH_DEG = -6.0

SUN = np.array([-0.55, 0.35, 0.76])
SUN = SUN / np.linalg.norm(SUN)


def load_palette(path):
	s = open(path).read()
	s = s[s.index('static const Palette _palette'):]
	s = s[:s.index('};')]
	cols = re.findall(r'(?:M|Colour)\(\s*(\d+),\s*(\d+),\s*(\d+)', s)[:256]
	pal = np.array([[int(c) for c in col] for col in cols], dtype=float)
	# Skip transparent index 0 and the animated / special colours at the end of the palette.
	usable = np.array([i for i in range(1, 215)])
	return pal, usable


class Palette:
	def __init__(self, path):
		self.pal, self.usable = load_palette(path)
		self.cache = {}

	def snap(self, rgb):
		"""Nearest palette entry, like a 32 KiB RGB->index lookup table in the game."""
		key = tuple(int(c) >> 2 for c in rgb)
		if key not in self.cache:
			d = ((self.pal[self.usable] - np.array(rgb)) ** 2).sum(axis=1)
			idx = self.usable[int(d.argmin())]
			self.cache[key] = tuple(int(c) for c in self.pal[idx])
		return self.cache[key]


# ---------------------------------------------------------------------------------------------
# Prototype world.

def track_centreline():
	"""Double track: straight, a 45 degree curve to the right, a diagonal, back to straight."""
	pts = []
	x, y = 31.5 * TILE, -6 * TILE
	heading = 0.0  # 0 = +y, positive = towards +x
	plan = [(18 * TILE, 0.0), (2 * TILE, math.radians(45)), (3 * TILE, 0.0), (2 * TILE, -math.radians(45)),
			(80 * TILE, 0.0)]
	step = 1.0
	for length, turn in plan:
		n = int(length / step)
		for _ in range(n):
			pts.append((x, y, heading))
			heading += turn / n
			x += math.sin(heading) * step
			y += math.cos(heading) * step
	return np.array(pts)


def offset_line(line, d):
	"""Parallel track d units to the right."""
	out = line.copy()
	out[:, 0] += np.cos(line[:, 2]) * d
	out[:, 1] -= np.sin(line[:, 2]) * d
	return out


class World:
	def __init__(self, with_details):
		self.details = with_details
		rnd = random.Random(15_3)
		self.rnd = rnd
		self.track1 = track_centreline()
		self.track2 = offset_line(self.track1, TILE)

		n = MAP + 1
		cx, cy = np.meshgrid(np.arange(n) * TILE, np.arange(n) * TILE, indexing='ij')
		h = 2.0 \
			+ 1.4 * np.sin(cx / 190.0 + 1.0) * np.cos(cy / 230.0) \
			+ 4.8 * np.exp(-(((cx - 54 * TILE) / (10 * TILE)) ** 2 + ((cy - 26 * TILE) / (9 * TILE)) ** 2)) \
			+ 3.5 * np.exp(-(((cx - 16 * TILE) / (9 * TILE)) ** 2 + ((cy - 40 * TILE) / (8 * TILE)) ** 2)) \
			+ 3.0 * np.exp(-(((cx - 40 * TILE) / (8 * TILE)) ** 2 + ((cy - 52 * TILE) / (8 * TILE)) ** 2))
		lake = (((cx - 17 * TILE) / (6 * TILE)) ** 2 + ((cy - 15 * TILE) / (4 * TILE)) ** 2) < 1
		h = np.round(h)
		h[lake] = 0
		forced = np.zeros_like(h, dtype=bool)
		forced[lake] = True

		# Flat land under the tracks.
		pts = np.concatenate([self.track1[:, :2], self.track2[:, :2]])
		corners = np.stack([cx.ravel(), cy.ravel()], axis=1)
		dmin = np.full(len(corners), 1e9)
		for chunk in np.array_split(pts[::2], 20):
			d = np.sqrt(((corners[:, None, :] - chunk[None, :, :]) ** 2).sum(axis=2)).min(axis=1)
			dmin = np.minimum(dmin, d)
		near_track = (dmin < 13).reshape(h.shape)
		h[near_track] = TRACK_LEVEL
		forced |= near_track

		# Town on the left of the station.
		self.station_y = (20 * TILE, 24 * TILE)
		town = np.zeros_like(forced)
		town[25:34, 18:29] = True
		h[town] = TRACK_LEVEL
		forced |= town

		# OpenTTD rule: neighbouring corners differ by at most one level.
		for _ in range(60):
			pad = np.pad(h, 1, mode='edge')
			nb = np.stack([pad[:-2, 1:-1], pad[2:, 1:-1], pad[1:-1, :-2], pad[1:-1, 2:]])
			new = np.clip(h, nb.max(axis=0) - 1, nb.min(axis=0) + 1)
			h = np.where(forced, h, new)
		self.h = np.clip(h, 0, 15)

		self.track_tiles = {}
		for line in (self.track1, self.track2):
			for x, y, _ in line[::4]:
				self.track_tiles[(int(x // TILE), int(y // TILE))] = True

		self.objects = {}   # tile -> list of drawable objects
		self.roads = set()
		self.fields = set()
		if with_details:
			self.build_details()

	# Bilinear height inside a tile (the game splits tiles into two planar triangles; close enough).
	def z_at(self, x, y):
		i, j = int(x // TILE), int(y // TILE)
		if not (0 <= i < MAP and 0 <= j < MAP):
			return 0.0
		fx, fy = x / TILE - i, y / TILE - j
		h = self.h
		z = (h[i, j] * (1 - fx) * (1 - fy) + h[i + 1, j] * fx * (1 - fy) + h[i, j + 1] * (1 - fx) * fy
			+ h[i + 1, j + 1] * fx * fy)
		return z * LEVEL

	def is_water(self, i, j):
		return self.h[i:i + 2, j:j + 2].max() == 0

	def add(self, tile, obj):
		self.objects.setdefault(tile, []).append(obj)

	def build_details(self):
		rnd = self.rnd
		tz = TRACK_LEVEL * LEVEL

		# Station: platforms on the left of track 1 and between the tracks, canopies, a building.
		sy0, sy1 = self.station_y
		seg = self.track1[(self.track1[:, 1] > sy0) & (self.track1[:, 1] < sy1)]
		x1 = seg[0, 0]
		x2 = x1 + TILE
		for y in np.arange(sy0, sy1, TILE):
			t = (int(x1 // TILE), int(y // TILE))
			self.add(t, ('box', (x1 - 9.0, y, x1 - 4.2, y + TILE, tz, tz + 1.5), (196, 192, 184), None))
			self.add(t, ('box', (x1 - 7.6, y + 7, x1 - 7.0, y + 7.6, tz + 1.5, tz + 9.0), (90, 90, 96), None))
			self.add(t, ('box', (x1 - 9.4, y, x1 - 4.6, y + TILE, tz + 9.0, tz + 9.8), (150, 60, 50), None))
			t2 = (int((x1 + 8) // TILE), int(y // TILE))
			self.add(t2, ('box', (x1 + 4.2, y, x2 - 4.2, y + TILE, tz, tz + 1.5), (196, 192, 184), None))
			self.add(t2, ('box', (x1 + 7.7, y + 7, x1 + 8.3, y + 7.6, tz + 1.5, tz + 9.0), (90, 90, 96), None))
			self.add(t2, ('box', (x1 + 4.6, y, x2 - 4.6, y + TILE, tz + 9.0, tz + 9.8), (150, 60, 50), None))
		tb = (int((x1 - 16) // TILE), int((sy0 + 24) // TILE))
		self.add(tb, ('house', (x1 - 22, sy0 + 14, x1 - 11, sy0 + 46, tz, tz + 11), (200, 170, 120), (130, 50, 40)))

		# Signals: one ahead of us (green) and at the station exit of the other track (red).
		for line, y, colour in ((self.track1, 13 * TILE, (40, 220, 60)), (self.track2, sy0 - 2 * TILE, (240, 40, 30))):
			idx = int(np.abs(line[:, 1] - y).argmin())
			x, yy, hd = line[idx]
			x -= math.cos(hd) * 5.0
			yy += math.sin(hd) * 5.0
			self.add((int(x // TILE), int(yy // TILE)), ('signal', (x, yy, tz), colour))

		# Town: two streets and houses left of the station.
		for i in range(25, 34):
			self.roads.add((i, 22, 'x'))
		for j in range(18, 29):
			self.roads.add((29, j, 'y'))
		for i in range(25, 34):
			for j in range(18, 29):
				if j == 22 or i == 29:
					continue
				if (i, j) in self.track_tiles or i >= int(x1 // TILE) - 1 or (i, j) == tb:
					continue
				if rnd.random() < 0.12:
					continue
				cxt, cyt = i * TILE + 8, j * TILE + 8
				w = rnd.uniform(5.0, 6.8)
				d = rnd.uniform(5.0, 6.8)
				centre = abs(i - 29) + abs(j - 22) < 4
				height = rnd.uniform(18, 34) if centre and rnd.random() < 0.6 else rnd.uniform(7, 11)
				wall = rnd.choice([(170, 70, 50), (200, 180, 140), (180, 180, 172), (150, 110, 80), (210, 200, 170)])
				roof = rnd.choice([(120, 40, 30), (80, 80, 90), (100, 60, 40)])
				kind = 'box' if height > 14 else 'house'
				self.add((i, j), (kind, (cxt - w, cyt - d, cxt + w, cyt + d, tz, tz + height), wall, roof))

		# Fields on the right, near the line.
		for i in range(36, 44):
			for j in range(0, 12):
				if (i, j) not in self.track_tiles:
					self.fields.add((i, j))

		# Trees: woods on the hills and scattered elsewhere.
		for i in range(MAP):
			for j in range(MAP):
				if (i, j) in self.track_tiles or (i, j) in self.fields or self.is_water(i, j):
					continue
				if 25 <= i < 34 and 18 <= j < 29:
					continue
				dens = 0.08 + 0.55 * max(0.0, (self.h[i, j] - 3) / 3)
				if 30 <= i <= 37 and j < 30:
					dens *= 0.4
				for _ in range(3):
					if rnd.random() < dens:
						x = i * TILE + rnd.uniform(2, 14)
						y = j * TILE + rnd.uniform(2, 14)
						self.add((i, j), ('tree', (x, y, self.z_at(x, y)), rnd.uniform(9, 15)))

		# Another train on track 2, standing in the station with its front towards us.
		self.place_train(self.track2, sy0 + 6, 4, (40, 80, 170), (230, 200, 40))

	def place_train(self, line, y_front, cars, body, nose):
		tz = TRACK_LEVEL * LEVEL
		for k in range(cars):
			yc = y_front + k * 8.2 + 4.0
			idx = int(np.abs(line[:, 1] - yc).argmin())
			x, y, hd = line[idx]
			self.add((int(x // TILE), int(y // TILE)),
				('obox', (x, y, tz + 0.6, hd, 7.8, 2.2, 6.2), body, nose if k == 0 else None))


# ---------------------------------------------------------------------------------------------
# Camera and drawing.

class Renderer:
	def __init__(self, world, palette, width, height, cam_s):
		self.w, self.pal = world, palette
		self.W, self.H = width, height
		x, y, hd = world.track1[int(cam_s)]
		self.cam = np.array([x, y, TRACK_LEVEL * LEVEL + EYE])
		self.yaw = hd
		p = math.radians(PITCH_DEG)
		f = np.array([math.sin(hd), math.cos(hd), 0.0])
		up = np.array([0.0, 0.0, 1.0])
		self.right = np.array([math.cos(hd), -math.sin(hd), 0.0])
		self.fwd = f * math.cos(p) + up * math.sin(p)
		self.up = -f * math.sin(p) + up * math.cos(p)
		self.focal = (width / 2) / math.tan(math.radians(FOV_DEG) / 2)
		self.img = Image.new('RGB', (width, height))
		self.dr = ImageDraw.Draw(self.img)
		self.polys = 0

	def to_cam(self, p):
		d = np.asarray(p, dtype=float) - self.cam
		return np.array([d @ self.right, d @ self.fwd, d @ self.up])

	def project(self, c):
		return (self.W / 2 + self.focal * c[0] / c[1], self.H / 2 - self.focal * c[2] / c[1])

	def shade(self, rgb, normal, dist, light=True):
		col = np.array(rgb, dtype=float)
		if light:
			n = np.asarray(normal, dtype=float)
			n = n / (np.linalg.norm(n) + 1e-9)
			col = col * (0.62 + 0.48 * max(0.0, float(n @ SUN)))
		f = min(1.0, max(0.0, (dist - FOG_START) / (FOG_END - FOG_START))) ** 0.8
		col = col * (1 - f) + np.array(self.fog) * f
		return self.pal.snap(np.clip(col, 0, 255))

	def poly(self, pts3, rgb, normal=None, light=True, cull=False):
		cs = [self.to_cam(p) for p in pts3]
		if cull and normal is not None:
			centre = np.mean(pts3, axis=0)
			if (centre - self.cam) @ normal >= 0:
				return
		# Clip against the near plane.
		near = 0.3
		out = []
		for k in range(len(cs)):
			a, b = cs[k], cs[(k + 1) % len(cs)]
			if a[1] >= near:
				out.append(a)
			if (a[1] >= near) != (b[1] >= near):
				t = (near - a[1]) / (b[1] - a[1])
				out.append(a + (b - a) * t)
		if len(out) < 3:
			return
		dist = float(np.linalg.norm(np.mean(pts3, axis=0) - self.cam))
		if normal is None:
			normal = np.cross(np.subtract(pts3[1], pts3[0]), np.subtract(pts3[2], pts3[0]))
		self.dr.polygon([self.project(c) for c in out], fill=self.shade(rgb, normal, dist, light))
		self.polys += 1

	def line3(self, a, b, rgb, width_units):
		ca, cb = self.to_cam(a), self.to_cam(b)
		if ca[1] < 0.3 or cb[1] < 0.3:
			return
		depth = (ca[1] + cb[1]) / 2
		w = max(1, int(round(self.focal * width_units / depth)))
		dist = float(np.linalg.norm((np.asarray(a) + np.asarray(b)) / 2 - self.cam))
		self.dr.line([self.project(ca), self.project(cb)], fill=self.shade(rgb, (0, 0, 1), dist, False), width=w)

	# --- sky -----------------------------------------------------------------------------------
	def sky(self):
		horizon = self.H / 2 - self.focal * math.tan(math.radians(-PITCH_DEG))
		# Fog colour = an exact palette colour (light sky blue), so fog and sky meet without a seam.
		self.fog = (156, 204, 220)
		# A few palette blues from the zenith down to the horizon; the bands are part of the look.
		bands = [(56, 120, 188), (72, 152, 216), (100, 172, 224), (132, 188, 216), (156, 204, 220)]
		weights = [0.30, 0.25, 0.20, 0.15, 0.10]
		y = 0.0
		for col, wgt in zip(bands, weights):
			y1 = y + horizon * wgt
			self.dr.rectangle([0, y, self.W, y1 + 1], fill=self.pal.snap(col))
			y = y1
		self.dr.rectangle([0, horizon, self.W, self.H], fill=self.pal.snap(self.fog))

	# --- tiles ---------------------------------------------------------------------------------
	def visible_tiles(self):
		tiles = []
		half = math.radians(FOV_DEG) / 2 + 0.25
		for i in range(MAP):
			for j in range(MAP):
				c = np.array([(i + 0.5) * TILE, (j + 0.5) * TILE, 0])
				d = c[:2] - self.cam[:2]
				dist = math.hypot(*d)
				if dist > FOG_END + TILE:
					continue
				ang = math.atan2(d[0], d[1]) - self.yaw
				ang = (ang + math.pi) % (2 * math.pi) - math.pi
				if dist > 2 * TILE and abs(ang) > half:
					continue
				fwd = d[0] * math.sin(self.yaw) + d[1] * math.cos(self.yaw)
				if fwd < -TILE:
					continue
				tiles.append((dist, i, j))
		tiles.sort(reverse=True)
		return tiles

	def ground(self, i, j):
		w = self.w
		x0, y0 = i * TILE, j * TILE
		h = w.h
		p = [np.array([x0, y0, h[i, j] * LEVEL]), np.array([x0 + TILE, y0, h[i + 1, j] * LEVEL]),
			np.array([x0 + TILE, y0 + TILE, h[i + 1, j + 1] * LEVEL]), np.array([x0, y0 + TILE, h[i, j + 1] * LEVEL])]
		if w.is_water(i, j):
			col = (40, 84, 160) if (i * 7 + j * 3) % 5 else (52, 96, 172)
			self.poly(p, col, (0, 0, 1))
			return
		hash_ = (i * 73856093 ^ j * 19349663) & 0xff
		grass = [(76, 128, 32), (68, 120, 28), (84, 136, 36)][hash_ % 3]
		if (i, j) in w.fields:
			grass = (172, 148, 72) if (i + j // 4) % 2 else (140, 150, 60)
		if h[i:i + 2, j:j + 2].min() == 0:
			grass = (180, 168, 120)  # beach
		# Close to the train, split grass into 2x2 patches of slightly different green: without
		# textures this is what makes speed visible on flat land.
		dist = math.hypot(x0 + 8 - self.cam[0], y0 + 8 - self.cam[1])
		if dist < 12 * TILE and (i, j) not in w.fields and h[i:i + 2, j:j + 2].min() > 0:
			greens = [(64, 112, 12), (84, 132, 20), (72, 120, 16), (76, 128, 32)]
			for a in range(2):
				for b in range(2):
					xa, ya = x0 + a * 8, y0 + b * 8
					q = [np.array([x, y, w.z_at(x, y)]) for x, y in ((xa, ya), (xa + 8, ya), (xa + 8, ya + 8), (xa, ya + 8))]
					col = greens[(hash_ >> (2 * (a * 2 + b))) % 4]
					self.poly(q, col, np.cross(q[1] - q[0], q[3] - q[0]))
		# Split along the diagonal like the game when the tile is not planar.
		elif p[0][2] + p[2][2] == p[1][2] + p[3][2]:
			n = np.cross(p[1] - p[0], p[3] - p[0])
			self.poly(p, grass, n)
		else:
			self.poly([p[0], p[1], p[2]], grass, np.cross(p[1] - p[0], p[2] - p[0]))
			self.poly([p[0], p[2], p[3]], grass, np.cross(p[2] - p[0], p[3] - p[0]))
		if (i, j) in w.fields:
			for k in range(1, 4):
				xa = x0 + k * 4
				self.poly([(xa - 0.5, y0 + 1, w.z_at(xa, y0 + 1) + 0.05), (xa + 0.5, y0 + 1, w.z_at(xa, y0 + 1) + 0.05),
					(xa + 0.5, y0 + 15, w.z_at(xa, y0 + 15) + 0.05), (xa - 0.5, y0 + 15, w.z_at(xa, y0 + 15) + 0.05)],
					(110, 90, 40), (0, 0, 1))
		for (ri, rj, axis) in [r for r in w.roads if r[0] == i and r[1] == j]:
			z = TRACK_LEVEL * LEVEL + 0.05
			if axis == 'x':
				q = [(x0, y0 + 4, z), (x0 + TILE, y0 + 4, z), (x0 + TILE, y0 + 12, z), (x0, y0 + 12, z)]
			else:
				q = [(x0 + 4, y0, z), (x0 + 12, y0, z), (x0 + 12, y0 + TILE, z), (x0 + 4, y0 + TILE, z)]
			self.poly(q, (110, 110, 112), (0, 0, 1))

	def track(self, i, j):
		w = self.w
		z = TRACK_LEVEL * LEVEL
		for line in (w.track1, w.track2):
			sel = (line[:, 0] // TILE == i) & (line[:, 1] // TILE == j)
			idx = np.nonzero(sel)[0]
			if len(idx) == 0:
				continue
			a, b = idx[0], min(idx[-1] + 1, len(line) - 1)
			step = 2
			for k in range(a, b, step):
				k2 = min(k + step, b)
				p, q = line[k], line[k2]
				for half, col, zz in ((3.2, (96, 80, 64), 0.05),):
					r1 = np.array([math.cos(p[2]), -math.sin(p[2])]) * half
					r2 = np.array([math.cos(q[2]), -math.sin(q[2])]) * half
					self.poly([(p[0] - r1[0], p[1] - r1[1], z + zz), (p[0] + r1[0], p[1] + r1[1], z + zz),
						(q[0] + r2[0], q[1] + r2[1], z + zz), (q[0] - r2[0], q[1] - r2[1], z + zz)], col, (0, 0, 1))
			dist = math.hypot(line[a, 0] - self.cam[0], line[a, 1] - self.cam[1])
			if dist < 6 * TILE:  # sleepers only close by
				for k in range(a, b, 2):
					p = line[k]
					r = np.array([math.cos(p[2]), -math.sin(p[2])])
					f = np.array([math.sin(p[2]), math.cos(p[2])]) * 0.45
					c = p[:2]
					self.poly([(*(c - r * 2.4 - f), z + 0.08), (*(c + r * 2.4 - f), z + 0.08),
						(*(c + r * 2.4 + f), z + 0.08), (*(c - r * 2.4 + f), z + 0.08)], (60, 44, 32), (0, 0, 1))
			for side in (-1.25, 1.25):
				for k in range(a, b, 4):
					k2 = min(k + 4, b)
					p, q = line[k], line[k2]
					ra = np.array([math.cos(p[2]), -math.sin(p[2])]) * side
					rb = np.array([math.cos(q[2]), -math.sin(q[2])]) * side
					self.line3((p[0] + ra[0], p[1] + ra[1], z + 0.15), (q[0] + rb[0], q[1] + rb[1], z + 0.15),
						(200, 200, 205), 0.3)

	# --- objects -------------------------------------------------------------------------------
	def box(self, x0, y0, x1, y1, z0, z1, wall, top=None):
		top = top or tuple(int(c * 0.8) for c in wall)
		faces = [
			([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], (0, -1, 0)),
			([(x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1)], (0, 1, 0)),
			([(x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1)], (-1, 0, 0)),
			([(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], (1, 0, 0)),
		]
		for pts, n in faces:
			self.poly(pts, wall, np.array(n, float), cull=True)
		self.poly([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], top, np.array([0, 0, 1.0]), cull=True)

	def house(self, x0, y0, x1, y1, z0, z1, wall, roof):
		self.box(x0, y0, x1, y1, z0, z1, wall)
		rh = 0.45 * min(x1 - x0, y1 - y0)
		if x1 - x0 >= y1 - y0:  # ridge along x
			ym = (y0 + y1) / 2
			self.poly([(x0, y0, z1), (x1, y0, z1), (x1, ym, z1 + rh), (x0, ym, z1 + rh)], roof, np.array([0, -rh, (ym - y0)]), cull=True)
			self.poly([(x1, y1, z1), (x0, y1, z1), (x0, ym, z1 + rh), (x1, ym, z1 + rh)], roof, np.array([0, rh, (y1 - ym)]), cull=True)
			self.poly([(x0, y1, z1), (x0, y0, z1), (x0, ym, z1 + rh)], wall, np.array([-1.0, 0, 0]), cull=True)
			self.poly([(x1, y0, z1), (x1, y1, z1), (x1, ym, z1 + rh)], wall, np.array([1.0, 0, 0]), cull=True)
		else:
			xm = (x0 + x1) / 2
			self.poly([(x0, y1, z1), (x0, y0, z1), (xm, y0, z1 + rh), (xm, y1, z1 + rh)], roof, np.array([-rh, 0, (xm - x0)]), cull=True)
			self.poly([(x1, y0, z1), (x1, y1, z1), (xm, y1, z1 + rh), (xm, y0, z1 + rh)], roof, np.array([rh, 0, (x1 - xm)]), cull=True)
			self.poly([(x0, y0, z1), (x1, y0, z1), (xm, y0, z1 + rh)], wall, np.array([0, -1.0, 0]), cull=True)
			self.poly([(x1, y1, z1), (x0, y1, z1), (xm, y1, z1 + rh)], wall, np.array([0, 1.0, 0]), cull=True)

	def obox(self, x, y, z, hd, length, half_w, height, body, nose):
		"""Vehicle: box rotated to the track heading, with windows and a coloured front."""
		f = np.array([math.sin(hd), math.cos(hd), 0.0])
		r = np.array([math.cos(hd), -math.sin(hd), 0.0])
		c = np.array([x, y, z])
		L = length / 2
		corners = lambda zz: [c + f * L - r * half_w + [0, 0, zz], c + f * L + r * half_w + [0, 0, zz],
			c - f * L + r * half_w + [0, 0, zz], c - f * L - r * half_w + [0, 0, zz]]
		b, t = corners(0), corners(height)
		sides = [((b[0], b[1], t[1], t[0]), f), ((b[1], b[2], t[2], t[1]), r), ((b[2], b[3], t[3], t[2]), -f),
			((b[3], b[0], t[0], t[3]), -r)]
		for k, (pts, n) in enumerate(sides):
			col = nose if (nose is not None and k == 2) else body
			self.poly(list(pts), col, n, cull=True)
		self.poly(t, (150, 150, 160), np.array([0, 0, 1.0]), cull=True)
		# Window band on the sides.
		for n_side, s in ((r, 1), (-r, -1)):
			if (c - self.cam) @ n_side >= 0:
				continue
			off = r * (half_w + 0.02) * s
			wz0, wz1 = height * 0.55, height * 0.8
			self.poly([c + f * (L - 0.6) + off + [0, 0, wz0], c - f * (L - 0.6) + off + [0, 0, wz0],
				c - f * (L - 0.6) + off + [0, 0, wz1], c + f * (L - 0.6) + off + [0, 0, wz1]], (40, 44, 60), n_side)

	def tree(self, x, y, z, size):
		cb = self.to_cam((x, y, z))
		ct = self.to_cam((x, y, z + size))
		if cb[1] < 0.5:
			return
		dist = math.hypot(x - self.cam[0], y - self.cam[1])
		bx, by = self.project(cb)
		tx, ty = self.project(ct)
		rad = self.focal * size * 0.36 / cb[1]
		self.dr.line([(bx, by), (bx, by - (by - ty) * 0.35)], fill=self.shade((90, 60, 30), (0, 0, 1), dist, False),
			width=max(1, int(rad * 0.18)))
		cy = by - (by - ty) * 0.62
		oct_ = lambda cx, cy, rx, ry: [(cx + rx * math.cos(a), cy + ry * math.sin(a)) for a in np.arange(8) * math.pi / 4 + math.pi / 8]
		self.dr.polygon(oct_(bx, cy, rad, (by - ty) * 0.42), fill=self.shade((36, 84, 20), (0, 0, 1), dist, False))
		self.dr.polygon(oct_(bx - rad * 0.22, cy - (by - ty) * 0.1, rad * 0.62, (by - ty) * 0.26),
			fill=self.shade((64, 120, 28), (0, 0, 1), dist, False))

	def signal(self, x, y, z, colour):
		self.line3((x, y, z), (x, y, z + 9.0), (70, 70, 74), 0.35)
		c = self.to_cam((x, y, z + 9.0))
		if c[1] < 0.5:
			return
		px, py = self.project(c)
		s = max(2.0, self.focal * 0.9 / c[1])
		self.dr.rectangle([px - s, py - 2.4 * s, px + s, py], fill=self.pal.snap((24, 24, 24)))
		self.dr.ellipse([px - s * 0.6, py - 1.9 * s, px + s * 0.6, py - 0.7 * s], fill=self.pal.snap(colour))

	def draw_objects(self, i, j):
		objs = self.w.objects.get((i, j), [])

		def key(o):
			if o[0] in ('box', 'house'):
				x0, y0, x1, y1 = o[1][:4]
				p = ((x0 + x1) / 2, (y0 + y1) / 2)
			else:
				p = o[1][:2]
			return -math.hypot(p[0] - self.cam[0], p[1] - self.cam[1])
		for o in sorted(objs, key=key):
			if o[0] == 'box':
				self.box(*o[1], o[2], o[3])
			elif o[0] == 'house':
				self.house(*o[1], o[2], o[3])
			elif o[0] == 'tree':
				self.tree(*o[1], o[2])
			elif o[0] == 'signal':
				self.signal(*o[1], o[2])
			elif o[0] == 'obox':
				self.obox(*o[1], o[2], o[3])

	def render(self):
		self.sky()
		for _, i, j in self.visible_tiles():
			self.ground(i, j)
			if (i, j) in self.w.track_tiles:
				self.track(i, j)
			self.draw_objects(i, j)
		return self.img


# ---------------------------------------------------------------------------------------------
# Put the 3D view into the real Kabina window (screenshot at GUI scale 2x).

VIEWPORT_BOX = (166, 118, 166 + 1268, 118 + 620)  # inside of the viewport inset in kabina_2x.png


def compose(shot_path, view, out_path, toggle_label):
	shot = Image.open(shot_path).convert('RGB')
	win = shot.copy()
	win.paste(view, VIEWPORT_BOX[:2])
	dr = ImageDraw.Draw(win)
	# 2D/3D toggle button between "Výhled vpřed" and "Zastavit" (font is only an approximation).
	x0, y0, x1, y1 = 394, 878, 470, 916
	dr.rectangle([x0, y0, x1, y1], fill=(168, 168, 168))
	dr.rectangle([x0, y0, x1, y0 + 1], fill=(216, 216, 216))
	dr.rectangle([x0, y0, x0 + 1, y1], fill=(216, 216, 216))
	dr.rectangle([x0, y1 - 1, x1, y1], fill=(98, 101, 98))
	dr.rectangle([x1 - 1, y0, x1, y1], fill=(98, 101, 98))
	try:
		font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 22)
	except OSError:
		font = ImageFont.load_default()
	tw = dr.textlength(toggle_label, font=font)
	dr.text(((x0 + x1 - tw) / 2 + 1, y0 + 8), toggle_label, font=font, fill=(16, 16, 16))
	win = win.crop((160, 80, 1440, 920))
	win.save(out_path)


def main():
	pal_path, shot_path, out_dir = sys.argv[1:4]
	pal = Palette(pal_path)
	for name, details in (('krok3_teren_a_trat', False), ('krok4_cely_svet', True)):
		world = World(details)
		r = Renderer(world, pal, 1268, 620, cam_s=13 * TILE)
		img = r.render()
		img.save(f'{out_dir}/{name}_pohled.png')
		compose(shot_path, img, f'{out_dir}/{name}_kabina.png', '3D')
		print(name, 'polygons:', r.polys)


if __name__ == '__main__':
	main()
