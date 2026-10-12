"""Minimal reader of OpenGFX sources (.pnml + .png): returns base sprites by their SpriteID."""

import os
import re

import numpy as np
from PIL import Image

ENTRY = re.compile(r'\[([^\[\]]*)\]')


def _strip(s):
	s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
	return re.sub(r'//[^\n]*', '', s)


class OpenGFX:
	def __init__(self, root):
		self.root = root
		self.templates = {}
		self.sprites = {}
		self.images = {}
		files = []
		for d, _, fs in os.walk(os.path.join(root, 'sprites')):
			files += [os.path.join(d, f) for f in fs if f.endswith(('.pnml', '.nml'))]
		for f in files:
			self._read_templates(_strip(open(f, encoding='utf-8', errors='replace').read()))
		for f in sorted(files):
			if '/base/' in f:
				self._read_sprites(_strip(open(f, encoding='utf-8', errors='replace').read()))

	def _block(self, s, start):
		depth = 0
		for k in range(start, len(s)):
			if s[k] == '{':
				depth += 1
			elif s[k] == '}':
				depth -= 1
				if depth == 0:
					return s[start + 1:k], k
		return s[start + 1:], len(s)

	def _read_templates(self, s):
		for m in re.finditer(r'template\s+(\w+)\s*\(([^)]*)\)\s*\{', s):
			body, _ = self._block(s, m.end() - 1)
			params = [p.strip() for p in m.group(2).split(',') if p.strip()]
			self.templates[m.group(1)] = (params, body)

	def _expand(self, body, env):
		out = []
		k = 0
		while k < len(body):
			m_e = ENTRY.search(body, k)
			m_t = re.search(r'(\w+)\s*\(([^()]*)\)', body[k:])
			t_pos = k + m_t.start() if m_t and m_t.group(1) in self.templates else None
			if m_e is None and t_pos is None:
				break
			if t_pos is not None and (m_e is None or t_pos < m_e.start()):
				name = m_t.group(1)
				args = [self._eval(a, env) for a in m_t.group(2).split(',') if a.strip()]
				params, tb = self.templates[name]
				out += self._expand(tb, dict(zip(params, args)))
				k = k + m_t.end()
			else:
				parts = [p.strip() for p in m_e.group(1).split(',')]
				try:
					vals = [self._eval(p, env) for p in parts[:6]]
					out.append(tuple(vals))
				except Exception:
					pass
				k = m_e.end()
		return out

	@staticmethod
	def _eval(expr, env):
		return int(eval(expr, {'__builtins__': {}}, dict(env)))

	def _read_sprites(self, s):
		for m in re.finditer(r'base_graphics\s+\w*\s*\(\s*(\d+)\s*,\s*"([^"]+)"\s*\)\s*\{', s):
			body, _ = self._block(s, m.end() - 1)
			first, path = int(m.group(1)), m.group(2)
			for n, e in enumerate(self._expand(body, {})):
				self.sprites.setdefault(first + n, (path, e))

	def get(self, sid):
		"""(RGBA image with index 0 transparent, palette indices array, xofs, yofs)."""
		path, (x, y, w, h, xo, yo) = self.sprites[sid]
		full = os.path.join(self.root, path)
		if full not in self.images:
			self.images[full] = Image.open(full)
		im = self.images[full]
		idx = np.array(im.crop((x, y, x + w, y + h)))
		return idx, xo, yo
