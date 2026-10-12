#!/usr/bin/env python3
"""
Third prototype of the 3D cab view (variant C): real 3D shapes, dressed in the game's graphics.

Differences to variant B (mockup_ttd.py):
- Rendered at full screen resolution (GUI scale 2x, like on Retina) with smooth (bilinear) texture
  sampling, so there are no big pixels. A fine "detail" pattern on the ground keeps it from looking
  blurry up close. Colours are still snapped to the 8bpp palette at the end.
- Houses and the station building are boxes in true 3D. Their walls and roofs are cut out of the
  game's isometric sprite: a point (x, y, z) of the building is at sprite pixel
  (c0 + 2 (y - x), r0 + (x + y) - z), which is the game's own projection (RemapCoords). So each wall
  gets the facade the game draws, transparent pixels above the eaves leave the gable shape.
- Trees and vehicles stay as cut-outs (as in every 3D game), but scaled smoothly.

Usage: python3 mockup_ttd3d.py <OpenGFX source dir> <kabina screenshot 2x> <output dir>
"""

import math
import sys

import numpy as np
from PIL import Image

import mockup
import mockup_ttd as base
from mockup_ttd import TILE, LEVEL, SUN, Gfx, World

W, H = 1268, 620          # full resolution of the viewport at GUI scale 2x
base.W, base.H = W, H
HOUSE_Z = 0.6             # sprite pixels -> world height (the game's projection squashes x/y, not z)
CUTOUT_UNITS = 0.42       # world units per sprite pixel for trees and vehicles


def bilinear(img_idx, pal, fx, fy, fallback=None):
	"""Smooth sample of a palette sprite at float pixel coordinates. Returns rgb and coverage (0..1)."""
	hgt, wid = img_idx.shape
	fx = fx - 0.5
	fy = fy - 0.5
	x0 = np.floor(fx).astype(int)
	y0 = np.floor(fy).astype(int)
	ax = fx - x0
	ay = fy - y0
	rgb = np.zeros(fx.shape + (3,))
	cov = np.zeros(fx.shape)
	for dx, dy, wgt in ((0, 0, (1 - ax) * (1 - ay)), (1, 0, ax * (1 - ay)), (0, 1, (1 - ax) * ay), (1, 1, ax * ay)):
		xi = np.clip(x0 + dx, 0, wid - 1)
		yi = np.clip(y0 + dy, 0, hgt - 1)
		idx = img_idx[yi, xi]
		if fallback is not None:
			idx = np.where(idx == 0, fallback[np.clip(yi, 0, fallback.shape[0] - 1), np.clip(xi, 0, fallback.shape[1] - 1)], idx)
		opaque = idx != 0
		w = wgt * opaque
		rgb += pal[idx] * w[..., None]
		cov += w
	rgb = rgb / np.maximum(cov, 1e-6)[..., None]
	return rgb, cov


class Renderer3D(base.Renderer):
	def __init__(self, world, gfx, cam_line_index):
		super().__init__(world, gfx, cam_line_index)
		self.frame = np.zeros((H, W, 3))

	# Generic perspective-correct triangle with a sampler(u, v, depth) -> (rgb, mask).
	def tri_s(self, P, L, sampler):
		C = [self.to_cam(p) for p in P]
		poly = list(zip(C, L))
		near = 0.5
		out = []
		for k in range(len(poly)):
			(a, la), (b, lb) = poly[k], poly[(k + 1) % len(poly)]
			la, lb = np.asarray(la, float), np.asarray(lb, float)
			if a[1] >= near:
				out.append((a, la))
			if (a[1] >= near) != (b[1] >= near):
				t = (near - a[1]) / (b[1] - a[1])
				out.append((a + (b - a) * t, la + (lb - la) * t))
		for k in range(1, len(out) - 1):
			self.raster_s(out[0], out[k], out[k + 1], sampler)

	def raster_s(self, A, B, Cc, sampler):
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
		rgb, mask = sampler(u, v, depth)
		rgb = self.fogged(rgb, depth)
		m = inside & mask
		region = self.frame[y0:y1 + 1, x0:x1 + 1]
		region[m] = rgb[m]

	# --- ground --------------------------------------------------------------------------------
	def ground(self, i, j):
		w = self.w
		key = tuple(w.ground_sprites(i, j))
		if key not in self.texcache:
			self.texcache[key] = self.gfx.ground_texture(list(key))
		tex = self.texcache[key]
		x0, y0 = i * TILE, j * TILE
		hh = w.h
		P = [np.array([x0, y0, hh[i, j] * LEVEL]), np.array([x0 + TILE, y0, hh[i + 1, j] * LEVEL]),
			np.array([x0 + TILE, y0 + TILE, hh[i + 1, j + 1] * LEVEL]), np.array([x0, y0 + TILE, hh[i, j + 1] * LEVEL])]
		L = [(0, 0), (16, 0), (16, 16), (0, 16)]
		gfx = self.gfx
		for a, b, c in ((0, 1, 2), (0, 2, 3)):
			n = np.cross(P[b] - P[a], P[c] - P[a])
			n = n / np.linalg.norm(n)
			shade = 0.9 + 0.32 * max(0.0, float(n @ SUN)) - 0.32 * SUN[2]

			def sampler(u, v, depth, shade=shade):
				u = np.clip(u, 0, 15.999)
				v = np.clip(v, 0, 15.999)
				# Unfold the isometric diamond (tile corner (x, y) -> sprite pixel (32 + 2 (y - x), (x + y) * 31/32)).
				fx = 32 + 2 * (v - u)
				fy = (u + v) * 31 / 32
				rgb, _ = bilinear(tex, gfx.pal, fx, fy, fallback=gfx.grass)
				# Fine detail pattern (world fixed, 1/4 unit) that fades out with distance.
				gx = np.floor((x0 + u) * 4).astype(np.int64)
				gy = np.floor((y0 + v) * 4).astype(np.int64)
				hsh = ((gx * 73856093) ^ (gy * 19349663)) & 0xff
				detail = (hsh / 255.0 - 0.5) * 0.16 * np.clip(1 - depth / (6 * TILE), 0, 1)
				return rgb * (shade + detail)[..., None], np.ones(u.shape, bool)
			self.tri_s([P[a], P[b], P[c]], [L[a], L[b], L[c]], sampler)

	def flat_poly(self, pts, col):
		col = np.asarray(col, float)

		def sampler(u, v, depth):
			return np.broadcast_to(col, u.shape + (3,)).copy(), np.ones(u.shape, bool)
		pts = [np.array(p, float) for p in pts]
		self.tri_s(pts[:3], [(0, 0)] * 3, sampler)
		self.tri_s([pts[0], pts[2], pts[3]], [(0, 0)] * 3, sampler)

	# --- buildings as textured boxes -----------------------------------------------------------
	def house_box(self, sid, cx, cy):
		idx, _, _ = self.gfx.sprite(sid)
		hgt, wid = idx.shape
		a = min(16.0, wid / 4.0)                   # footprint side (64 px wide sprite = whole tile)
		c0 = wid / 2.0                             # column of the north corner of the footprint
		cols = np.nonzero(idx[:, int(c0) - 1:int(c0) + 1].any(axis=1))[0]
		r_bottom = hgt                             # south corner = bottom of the sprite
		r0 = r_bottom - 2 * a                      # north corner row at z = 0
		top = cols.min() if len(cols) else 0
		hpx = max(4.0, r0 - top)                   # building height in sprite pixels (top of the roof)
		# Eaves: top of the outermost columns (west and east corner of the footprint).
		edge = []
		for cols_ in (idx[:, :2], idx[:, -2:]):
			rows = np.nonzero(cols_.any(axis=1))[0]
			if len(rows):
				edge.append((r0 + a) - rows.min())
		eave = min(edge) if edge else hpx
		if wid < 40:                               # small houses: the edge columns are already roof
			eave = min(eave, hpx * 0.5)
		roof_px = hpx - eave if hpx - eave > 3 else 0.0
		top_px = hpx
		hpx = hpx - roof_px                        # wall height in sprite pixels
		hz = hpx * HOUSE_Z
		z0 = self.w.z_at(cx, cy)
		X0, Y0 = cx - a / 2, cy - a / 2
		gfx = self.gfx

		def face_sampler(kind):
			def sampler(s, t, depth):
				# s: position along the wall (0..a), t: height in sprite pixels.
				if kind == 'x':      # wall facing +x / -x: points (a, s, t)
					col = c0 + 2 * (s - a)
					row = r0 + (a + s) - t
				elif kind == 'y':    # wall facing +y / -y: points (s, a, t)
					col = c0 + 2 * (a - s)
					row = r0 + (s + a) - t
				else:                # roof: points (s, t, hpx)
					col = c0 + 2 * (t - s)
					row = r0 + (s + t) - hpx
				rgb, cov = bilinear(idx, gfx.pal, col, row)
				return rgb * shade_of[kind], cov > 0.5
			return sampler

		shade_of = {}
		faces = []
		cam = self.cam
		# Front/back walls (normal -y / +y) use the sprite's SE facade, side walls its SW facade.
		if cam[1] < Y0:
			faces.append(('y', [(X0, Y0, z0), (X0 + a, Y0, z0), (X0 + a, Y0, z0 + hz), (X0, Y0, z0 + hz)], (0, -1, 0)))
		if cam[1] > Y0 + a:
			faces.append(('y', [(X0 + a, Y0 + a, z0), (X0, Y0 + a, z0), (X0, Y0 + a, z0 + hz), (X0 + a, Y0 + a, z0 + hz)], (0, 1, 0)))
		if cam[0] > X0 + a:
			faces.append(('x', [(X0 + a, Y0, z0), (X0 + a, Y0 + a, z0), (X0 + a, Y0 + a, z0 + hz), (X0 + a, Y0, z0 + hz)], (1, 0, 0)))
		if cam[0] < X0:
			faces.append(('x', [(X0, Y0 + a, z0), (X0, Y0, z0), (X0, Y0, z0 + hz), (X0, Y0 + a, z0 + hz)], (-1, 0, 0)))
		if not roof_px:
			faces.append(('r', [(X0, Y0, z0 + hz), (X0 + a, Y0, z0 + hz), (X0 + a, Y0 + a, z0 + hz), (X0, Y0 + a, z0 + hz)], (0, 0, 1)))
		for kind, pts, n in faces:
			shade_of[kind] = 0.85 + 0.35 * max(0.0, float(np.array(n, float) @ SUN))
			P = [np.array(p, float) for p in pts]
			if kind == 'r':
				L = [(0, 0), (a, 0), (a, a), (0, a)]
			elif n[0] + n[1] > 0:
				L = [(0, 0), (a, 0), (a, hpx), (0, hpx)]
			else:
				L = [(a, 0), (0, 0), (0, hpx), (a, hpx)]
			self.tri_s([P[0], P[1], P[2]], [L[0], L[1], L[2]], face_sampler(kind))
			self.tri_s([P[0], P[2], P[3]], [L[0], L[2], L[3]], face_sampler(kind))
		if roof_px:
			# Hip roof up to the top of the sprite; every roof point takes the sprite pixel the game
			# would draw there, so the painted roof lands on the roof.
			apex = (a / 2, a / 2, top_px)
			ring = [(0, 0, hpx), (a, 0, hpx), (a, a, hpx), (0, a, hpx)]
			for k in range(4):
				p0, p1 = ring[k], ring[(k + 1) % 4]
				loc = np.array([p0, p1, apex], float)
				Wpts = [np.array([X0 + q[0], Y0 + q[1], z0 + q[2] * HOUSE_Z]) for q in loc]
				n = np.cross(Wpts[1] - Wpts[0], Wpts[2] - Wpts[0])
				if n[2] < 0:
					n = -n
				if (np.mean(Wpts, axis=0) - self.cam) @ n >= 0:
					continue
				A, B, Cz = np.linalg.solve(np.c_[loc[:, :2], np.ones(3)], loc[:, 2])
				shade = 0.85 + 0.35 * max(0.0, float(n / np.linalg.norm(n) @ SUN))

				def sampler(s_, t_, depth, A=A, B=B, Cz=Cz, shade=shade):
					zp = A * s_ + B * t_ + Cz
					rgb, cov = bilinear(idx, gfx.pal, c0 + 2 * (t_ - s_), r0 + s_ + t_ - zp)
					return rgb * shade, cov > 0.5
				self.tri_s(Wpts, [tuple(q[:2]) for q in loc], sampler)

	# --- vehicles as textured boxes ------------------------------------------------------------
	def train(self, base_sid, wx, wy, heading, is_wagon):
		"""A box along the track, textured from the vehicle's sprite seen along the x axis (direction NE)."""
		idx, _, _ = self.gfx.sprite(base_sid + 1)
		hgt, wid = idx.shape
		L = 8.0                                  # vehicle length in units (a full length wagon)
		wv = max(2.0, wid / 2 - L)               # width in units from the sprite width 2 (L + w)
		c0 = 2 * L                               # north corner column of the bounding box
		r0 = hgt - (L + wv)                      # north corner row at z = 0
		hpx = max(4.0, r0 - np.nonzero(idx.any(axis=1))[0].min())
		hz = hpx * HOUSE_Z
		z0 = self.w.z_at(wx, wy) + 0.4
		f = np.array([math.sin(heading), math.cos(heading), 0.0])   # front direction
		r = np.array([math.cos(heading), -math.sin(heading), 0.0])
		c = np.array([wx, wy, z0])
		gfx = self.gfx
		shade_of = {}

		def sampler_for(kind):
			def sampler(s, t, depth):
				if kind == 'side':   # face y = w of the sprite box: points (s, w, t), s from the front
					col = c0 + 2 * (wv - s)
					row = r0 + s + wv - t
				elif kind == 'end':  # face x = 0 (front): points (0, s, t)
					col = c0 + 2 * s
					row = r0 + s - t
				else:                # roof: points (s, t, hpx)
					col = c0 + 2 * (t - s)
					row = r0 + s + t - hpx
				rgb, cov = bilinear(idx, gfx.pal, col, row)
				return rgb * shade_of[kind], cov > 0.5
			return sampler

		def corner(sf, sr, zz):
			return c + f * sf + r * sr + np.array([0, 0, zz])
		Lh, wh = L / 2, wv / 2
		faces = [
			('end', [corner(Lh, -wh, 0), corner(Lh, wh, 0), corner(Lh, wh, hz), corner(Lh, -wh, hz)], f, [(0, 0), (wv, 0), (wv, hpx), (0, hpx)]),
			('end', [corner(-Lh, wh, 0), corner(-Lh, -wh, 0), corner(-Lh, -wh, hz), corner(-Lh, wh, hz)], -f, [(0, 0), (wv, 0), (wv, hpx), (0, hpx)]),
			('side', [corner(Lh, wh, 0), corner(-Lh, wh, 0), corner(-Lh, wh, hz), corner(Lh, wh, hz)], r, [(0, 0), (L, 0), (L, hpx), (0, hpx)]),
			('side', [corner(-Lh, -wh, 0), corner(Lh, -wh, 0), corner(Lh, -wh, hz), corner(-Lh, -wh, hz)], -r, [(L, 0), (0, 0), (0, hpx), (L, hpx)]),
			('roof', [corner(Lh, -wh, hz), corner(Lh, wh, hz), corner(-Lh, wh, hz), corner(-Lh, -wh, hz)], np.array([0, 0, 1.0]), [(0, 0), (0, wv), (L, wv), (L, 0)]),
		]
		for kind, P, n, Lc in faces:
			if (np.mean(P, axis=0) - self.cam) @ n >= 0:
				continue
			shade_of[kind] = 0.85 + 0.35 * max(0.0, float(n @ SUN))
			self.tri_s([P[0], P[1], P[2]], [Lc[0], Lc[1], Lc[2]], sampler_for(kind))
			self.tri_s([P[0], P[2], P[3]], [Lc[0], Lc[2], Lc[3]], sampler_for(kind))

	# --- smooth cut-outs -----------------------------------------------------------------------
	def billboard(self, sid, wx, wy, kind):
		if kind == 'house':
			self.house_box(sid, wx, wy)
			return
		idx, xo, yo = self.gfx.sprite(sid)
		z = self.w.z_at(wx, wy)
		c = self.to_cam((wx, wy, z))
		if c[1] < 1.0:
			return
		s = self.focal * CUTOUT_UNITS / c[1]
		ax = W / 2 + self.focal * c[0] / c[1]
		ay = H / 2 - self.focal * c[2] / c[1]
		left, top = ax + xo * s, ay + yo * s
		hgt, wid = idx.shape
		cx0, cx1 = max(int(left), 0), min(int(math.ceil(left + wid * s)), W)
		cy0, cy1 = max(int(top), 0), min(int(math.ceil(top + hgt * s)), H)
		if cx0 >= cx1 or cy0 >= cy1:
			return
		X, Y = np.meshgrid(np.arange(cx0, cx1) + 0.5, np.arange(cy0, cy1) + 0.5)
		rgb, cov = bilinear(idx, self.gfx.pal, (X - left) / s, (Y - top) / s)
		inside = ((X - left) / s >= 0) & ((X - left) / s < wid) & ((Y - top) / s >= 0) & ((Y - top) / s < hgt)
		m = (cov > 0.5) & inside
		rgb = self.fogged(rgb, np.full(cov.shape, c[1]))
		region = self.frame[cy0:cy1, cx0:cx1]
		region[m] = rgb[m]

	def sky(self):
		# Smooth gradient; the final palette snap turns it into soft bands.
		horizon = H / 2 - self.focal * math.tan(math.radians(-base.PITCH_DEG))
		top, low = np.array([64.0, 128.0, 196.0]), self.fog
		t = np.clip(np.arange(H) / max(horizon, 1), 0, 1)[:, None, None]
		self.frame[:] = top * (1 - t) ** 1.2 + low * (1 - (1 - t) ** 1.2)
		self.frame[int(horizon):] = self.fog

	def render(self):
		self.sky()
		for _, i, j in self.visible_tiles():
			self.ground(i, j)
			if (i, j) in self.w.rails:
				self.rails(i, j)
			self.objects(i, j)
		# Ordered dither before snapping to the palette, so gradients do not band.
		bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0 - 0.5
		d = np.tile(bayer, (H // 4 + 1, W // 4 + 1))[:H, :W, None] * 6.0
		idx = self.gfx.snap(np.clip(self.frame + d, 0, 255))
		return Image.fromarray(self.gfx.pal[idx].astype(np.uint8))


# Finished buildings with plain facades (no scaffolding or open frames).
base.HOUSES_BIG = [(None, 1449), (None, 1459), (None, 1462), (None, 1465), (None, 1468), (None, 1469),
	(None, 1477), (None, 1478), (1548, 1549), (1550, 1551), (1542, 1543), (1536, 1537)]


def main():
	root, shot_path, out_dir = sys.argv[1:4]
	gfx = Gfx(root)
	world = World(gfx)
	r = Renderer3D(world, gfx, cam_line_index=int(np.abs(world.track1[:, 1] - 14 * TILE).argmin()))
	img = r.render()
	img.save(f'{out_dir}/ttd3d_pohled.png')
	mockup.compose(shot_path, img, f'{out_dir}/ttd3d_kabina.png', '3D')


if __name__ == '__main__':
	main()
