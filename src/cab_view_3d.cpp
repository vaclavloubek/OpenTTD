/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cab_view_3d.cpp Software rendered 3D view from the cab of a train.
 *
 * The view is drawn by the game itself, without OpenGL, into its own buffer at the full
 * resolution of the screen, and then put on the screen as one sprite of the current blitter.
 *
 * The ground of every tile is dressed in the sprites the game draws for it: the draw procedure
 * of the tile runs like for a viewport (see #CollectTileGroundSprites), its ground sprites are
 * put together and the isometric picture is mapped back onto the tile surface in 3D. A point
 * (x, y, z) of the tile is at screen pixel (2 (y - x), x + y - z), which is #RemapCoords.
 * Textures are sampled smoothly, so there are no big pixels even close by. Rails and sleepers
 * are drawn on top as geometry, so they stay sharp up close.
 */

#include "stdafx.h"
#include "cab_view_3d.h"
#include "viewport_func.h"
#include "landscape.h"
#include "map_func.h"
#include "tile_map.h"
#include "rail_map.h"
#include "road_map.h"
#include "station_map.h"
#include "track_func.h"
#include "spritecache.h"
#include "spriteloader/spriteloader.hpp"
#include "blitter/factory.hpp"
#include "gfx_func.h"
#include "palette_func.h"
#include "debug.h"

#include "table/sprites.h"

#include "thread.h"

#include <chrono>

#include "safeguards.h"

namespace {

constexpr float TILE = static_cast<float>(TILE_SIZE); ///< Size of a tile; as float, so negating it works.
constexpr float VIEW_TILES = 56.0f; ///< How far the view reaches, in tiles.
constexpr float FOG_START = 14.0f * TILE; ///< Distance where the haze starts.
constexpr float FOG_END = VIEW_TILES * TILE; ///< Distance where everything has melted into the haze.
constexpr float FOV_DEG = 64.0f; ///< Horizontal field of view.
constexpr float PITCH_DEG = -6.0f; ///< How much the camera looks down.
constexpr float NEAR = 0.5f; ///< Near clipping distance.
constexpr float DETAIL_DIST = 6.0f * TILE; ///< Distance where the fine ground pattern has faded out.
constexpr float RAIL_DIST = 24.0f * TILE; ///< Rails are drawn as geometry up to this distance.
constexpr float RAIL_FADE_START = 6.0f * TILE; ///< From this distance on, rails blend into the ballast, as they get thinner than a pixel.
constexpr float SLEEPER_DIST = 10.0f * TILE; ///< Sleepers are drawn as geometry up to this distance.
constexpr float RAIL_GAUGE = 2.3f; ///< Distance between the rails.
constexpr float RAIL_WIDTH = 0.44f; ///< Width of the head of a rail.
constexpr float SLEEPER_LENGTH = 4.4f; ///< Length of a sleeper, across the track.
constexpr float SLEEPER_WIDTH = 0.9f; ///< Width of a sleeper, along the track.
constexpr float SLEEPER_SPACING = 2.0f; ///< Distance between sleepers.
constexpr size_t MAX_TEXTURES = 4096; ///< Clear the texture cache when it grows beyond this.
constexpr int MAX_THREADS = 8; ///< At most this many threads draw the picture.
constexpr int STRIPE_HEIGHT = 4; ///< Height of the stripes of rows the threads draw.

/** A colour with floating point channels in the range 0..255. */
struct RGB {
	float r, g, b;

	RGB operator*(float f) const { return {r * f, g * f, b * f}; }
	RGB operator+(const RGB &o) const { return {r + o.r, g + o.g, b + o.b}; }
};

/** Pack a colour into 0x00RRGGBB. */
inline uint32_t Pack(const RGB &c)
{
	uint32_t r = static_cast<uint32_t>(std::clamp(c.r, 0.0f, 255.0f));
	uint32_t g = static_cast<uint32_t>(std::clamp(c.g, 0.0f, 255.0f));
	uint32_t b = static_cast<uint32_t>(std::clamp(c.b, 0.0f, 255.0f));
	return (r << 16) | (g << 8) | b;
}

constexpr RGB SKY_TOP = {64, 128, 196}; ///< Colour of the sky at the top of the view.
constexpr RGB HAZE = {156, 204, 220}; ///< Colour of the haze, and of the sky at the horizon.
constexpr RGB RAIL_COLOUR = {200, 200, 200}; ///< Colour of the rails.
constexpr RGB RAIL_FAR_COLOUR = {104, 92, 80}; ///< Colour of far away rails, about that of the ballast under them.
constexpr RGB SLEEPER_COLOUR = {72, 44, 4}; ///< Colour of the sleepers.
constexpr RGB FOUNDATION_COLOUR = {132, 112, 88}; ///< Colour of the sides of foundations.

/** A vector in world coordinates. */
struct Vec3 {
	float x, y, z;

	Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(float f) const { return {x * f, y * f, z * f}; }
	float Dot(const Vec3 &o) const { return x * o.x + y * o.y + z * o.z; }
	Vec3 Cross(const Vec3 &o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
	Vec3 Normalised() const { float l = std::sqrt(this->Dot(*this)); return l > 0 ? *this * (1.0f / l) : *this; }
};

/** All corners of a tile. */
constexpr std::array<Corner, CORNER_END> ALL_CORNERS = {CORNER_W, CORNER_S, CORNER_E, CORNER_N};

/** Division rounding down, also for negative numbers. */
constexpr int32_t FloorDiv(int32_t a, int32_t b)
{
	return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

/** Direction towards the sun, for shading slopes. */
static const Vec3 SUN = Vec3{-0.55f, 0.35f, 0.76f}.Normalised();

/** How bright a surface with the given normal is. */
float Shade(const Vec3 &normal)
{
	return 0.9f + 0.32f * std::max(0.0f, normal.Normalised().Dot(SUN)) - 0.32f * SUN.z;
}

/** A sprite as loaded from the graphics files, before it is encoded for the blitter. */
struct RawSprite {
	int width = 0; ///< Width in pixels.
	int height = 0; ///< Height in pixels.
	int x_offs = 0; ///< Horizontal offset of the top left pixel.
	int y_offs = 0; ///< Vertical offset of the top left pixel.
	std::vector<SpriteLoader::CommonPixel> pixels; ///< The pixels, row by row.
};

/** Sprite "encoder" that keeps the sprite as it was loaded, at the normal zoom level. */
class RawSpriteEncoder : public SpriteEncoder {
public:
	RawSprite *target = nullptr; ///< Where to store the sprite.

	bool Is32BppSupported() override { return true; }

	Sprite *Encode(SpriteType, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator) override
	{
		/* The normal zoom level has the real pixels of the base graphics; the closer
		 * zoom levels are mostly blown up copies of it, which smooth sampling does better. */
		const SpriteLoader::Sprite &src = sprite[ZoomLevel::Normal];
		this->target->width = src.width;
		this->target->height = src.height;
		this->target->x_offs = src.x_offs;
		this->target->y_offs = src.y_offs;
		if (src.data != nullptr) this->target->pixels.assign(src.data, src.data + static_cast<size_t>(src.width) * src.height);

		Sprite *s = allocator.Allocate<Sprite>(sizeof(Sprite));
		s->width = src.width;
		s->height = src.height;
		s->x_offs = src.x_offs;
		s->y_offs = src.y_offs;
		return s;
	}
};

/** A picture of colours, for sampling smoothly. */
struct Picture {
	int width = 1; ///< Width in pixels.
	int height = 1; ///< Height in pixels.
	std::vector<RGB> pixels = {{0, 0, 0}}; ///< The pixels, row by row; all opaque.

	/**
	 * Sample the picture smoothly.
	 * @param x Horizontal position in pixels.
	 * @param y Vertical position in pixels.
	 * @return The colour.
	 */
	RGB Sample(float x, float y) const
	{
		float fx = std::clamp(x - 0.5f, 0.0f, this->width - 1.001f);
		float fy = std::clamp(y - 0.5f, 0.0f, this->height - 1.001f);
		int x0 = static_cast<int>(fx);
		int y0 = static_cast<int>(fy);
		fx -= x0;
		fy -= y0;
		int x1 = std::min(x0 + 1, this->width - 1);
		int y1 = std::min(y0 + 1, this->height - 1);
		const RGB &a = this->pixels[y0 * this->width + x0];
		const RGB &b = this->pixels[y0 * this->width + x1];
		const RGB &c = this->pixels[y1 * this->width + x0];
		const RGB &d = this->pixels[y1 * this->width + x1];
		return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
	}

	/**
	 * Make a picture of half the size, each pixel the average of four.
	 * @return The smaller picture.
	 */
	Picture Halve() const
	{
		Picture half;
		half.width = std::max(1, (this->width + 1) / 2);
		half.height = std::max(1, (this->height + 1) / 2);
		half.pixels.assign(static_cast<size_t>(half.width) * half.height, {0, 0, 0});
		for (int y = 0; y < half.height; y++) {
			for (int x = 0; x < half.width; x++) {
				RGB sum = {0, 0, 0};
				for (int dy = 0; dy < 2; dy++) {
					for (int dx = 0; dx < 2; dx++) {
						sum = sum + this->pixels[std::min(2 * y + dy, this->height - 1) * this->width + std::min(2 * x + dx, this->width - 1)];
					}
				}
				half.pixels[y * half.width + x] = sum * 0.25f;
			}
		}
		return half;
	}
};

/** Number of smaller versions of a ground texture, used further away so the ground does not glitter. */
constexpr int TEXTURE_LEVELS = 4;

/** The ground sprites of a tile put together, in normal zoom screen pixels. */
struct GroundTexture {
	int left = 0; ///< Screen column of the first pixel, relative to the north corner of the tile.
	int top = 0; ///< Screen row of the first pixel, relative to the north corner of the tile at its height.
	std::array<Picture, TEXTURE_LEVELS> levels; ///< The picture, and versions of half, a quarter... the size.

	/**
	 * Sample the texture smoothly.
	 * @param col Screen column relative to the north corner of the tile.
	 * @param row Screen row relative to the north corner of the tile at its height.
	 * @param level Which version of the picture to use.
	 * @return The colour.
	 */
	RGB Sample(float col, float row, int level) const
	{
		float scale = 1.0f / (1 << level);
		return this->levels[level].Sample((col - this->left) * scale, (row - this->top) * scale);
	}
};

/** Hash of the key of a ground texture. */
struct TextureKeyHash {
	size_t operator()(const std::vector<int32_t> &key) const
	{
		size_t h = 14695981039346656037ULL;
		for (int32_t v : key) h = (h ^ static_cast<uint32_t>(v)) * 1099511628211ULL;
		return h;
	}
};

/** Camera with its projection. */
struct Camera {
	Vec3 pos; ///< Position of the eye.
	Vec3 right; ///< Unit vector to the right.
	Vec3 fwd; ///< Unit vector of the direction of view.
	Vec3 up; ///< Unit vector upwards on the screen.
	float focal; ///< Distance of the projection plane, in pixels.
	float cx; ///< Horizontal centre of the view.
	float cy; ///< Vertical centre of the view.

	/** Transform a point from world to camera coordinates: x to the right, y ahead, z up. */
	Vec3 ToCamera(const Vec3 &p) const
	{
		Vec3 d = p - this->pos;
		return {d.Dot(this->right), d.Dot(this->fwd), d.Dot(this->up)};
	}
};

/** Number of interpolated attributes per vertex. */
constexpr int ATTRIBUTES = 4;

/** A vertex in camera coordinates, with its attributes. */
struct ClipVertex {
	Vec3 c; ///< Position in camera coordinates.
	std::array<float, ATTRIBUTES> a; ///< Attributes, interpolated perspective correctly.
};

/** How to paint the pixels of a triangle. */
struct Paint {
	const GroundTexture *texture; ///< Texture of the ground, or nullptr for a flat colour.
	RGB colour; ///< The flat colour, already shaded.
	float shade; ///< Brightness of the textured ground.
	int level; ///< Which version of the texture to use, see #GroundTexture::levels.
	float bias; ///< Depth bias, so things lying on the ground win from the ground.
};

/** A triangle on the screen, ready to fill. */
struct ScreenTriangle {
	int x0, x1, y0, y1; ///< Bounding box, in pixels.
	std::array<float, 3> ex, ey, ec; ///< Edge functions: the weight of each corner is ex * x + ey * y + ec.
	std::array<float, 3> iw; ///< Inverse depth of each corner.
	std::array<std::array<float, ATTRIBUTES>, 3> aw; ///< Attributes of each corner, divided by its depth.
	Paint paint; ///< How to paint it.
};

/** The renderer, with its buffers and caches. */
class Renderer {
public:
	void Draw(const Rect &r, const CabViewCamera &camera, uint32_t frame_id);

	void Reset()
	{
		this->sprites.clear();
		this->textures.clear();
		this->frame = {};
		this->depth = {};
		this->frame_valid = false;
	}

private:
	int width = 0; ///< Width of the view.
	int height = 0; ///< Height of the view.
	std::vector<uint32_t> frame; ///< Colour buffer, see #Pack.
	std::vector<float> depth; ///< Inverse depth buffer; 0 is infinitely far.
	Camera cam; ///< The current camera.
	bool frame_valid = false; ///< Whether #frame holds a finished picture.
	CabViewCamera frame_camera; ///< Camera of the picture in #frame.
	uint32_t frame_id = 0; ///< Identification of the picture in #frame, see #DrawCabView3D.

	std::unordered_map<SpriteID, RawSprite> sprites; ///< Sprites loaded so far.
	std::unordered_map<std::vector<int32_t>, GroundTexture, TextureKeyHash> textures; ///< Ground textures put together so far.
	std::vector<GroundSpriteToDraw> ground_sprites; ///< Scratch buffer for collecting ground sprites.
	std::vector<int32_t> key; ///< Scratch buffer for texture keys.

	const RawSprite &GetRawSprite(SpriteID sprite);
	const GroundTexture &GetGroundTexture(TileIndex tile, int tile_z);
	RGB Fog(const RGB &colour, float dist) const;

	std::vector<ScreenTriangle> triangles; ///< Triangles of the current picture, nearest first.

	void AddTriangle(const std::array<ClipVertex, 3> &tri, const Paint &paint);
	void SetupTriangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c, const Paint &paint);
	void AddFlatQuad(const std::array<Vec3, 4> &quad, const RGB &colour, float bias);
	void Rasterise(int stripe, int stripes);

	void DrawSky();
	void AddTile(TileIndex tile, float dist);
	void AddTrack(TileIndex tile, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist);
	void Output(const Rect &r);
};

/**
 * Get a sprite as loaded from the graphics files.
 * @param sprite The sprite.
 * @return The sprite.
 */
const RawSprite &Renderer::GetRawSprite(SpriteID sprite)
{
	auto it = this->sprites.find(sprite);
	if (it != this->sprites.end()) return it->second;

	RawSprite &raw = this->sprites[sprite];
	RawSpriteEncoder encoder;
	encoder.target = &raw;
	UniquePtrSpriteAllocator allocator;
	::GetRawSprite(sprite, SpriteType::Normal, &allocator, &encoder);
	return raw;
}

/** How many times at most the ground around a building is grown into the area under it. */
constexpr int MAX_FILL_PASSES = 32;

/**
 * Whether a pixel of the ground of a house, industry or object lies under its building.
 * The original graphics paint that area in the darkest grey of the palette.
 * @param p The pixel.
 * @return True when the pixel is hidden by the building in the game.
 */
static bool IsUnderBuilding(const SpriteLoader::CommonPixel &p)
{
	if (p.m != 0) return p.m == 1;
	return p.r <= 20 && p.g <= 20 && p.b <= 20;
}

/**
 * Get the ground texture of a tile, putting it together from the tile's ground sprites when needed.
 * Tiles that look the same share the texture.
 * @param tile The tile.
 * @param tile_z Height of the tile as the viewport uses it, see #GetTilePixelSlope.
 * @return The texture.
 */
const GroundTexture &Renderer::GetGroundTexture(TileIndex tile, int tile_z)
{
	this->ground_sprites.clear();
	CollectTileGroundSprites(tile, this->ground_sprites);

	/* Positions relative to the north corner of the tile, in normal zoom pixels. */
	Point origin = RemapCoords(TileX(tile) * TILE_SIZE, TileY(tile) * TILE_SIZE, tile_z);
	this->key.clear();
	for (const GroundSpriteToDraw &gs : this->ground_sprites) {
		if (HasBit(gs.image, PALETTE_MODIFIER_TRANSPARENT)) continue;
		this->key.push_back(gs.image & SPRITE_MASK);
		this->key.push_back(gs.pal);
		this->key.push_back(FloorDiv(gs.x - origin.x, ZOOM_BASE));
		this->key.push_back(FloorDiv(gs.y - origin.y, ZOOM_BASE));
	}

	auto it = this->textures.find(this->key);
	if (it != this->textures.end()) return it->second;
	GroundTexture &tex = this->textures[this->key];
	if (this->key.empty()) return tex;

	/* Bounding box of all sprites. */
	int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
	for (size_t i = 0; i < this->key.size(); i += 4) {
		const RawSprite &raw = this->GetRawSprite(this->key[i]);
		if (raw.pixels.empty()) continue;
		left = std::min(left, this->key[i + 2] + raw.x_offs);
		top = std::min(top, this->key[i + 3] + raw.y_offs);
		right = std::max(right, this->key[i + 2] + raw.x_offs + raw.width);
		bottom = std::max(bottom, this->key[i + 3] + raw.y_offs + raw.height);
	}
	if (left >= right || top >= bottom) return tex;

	tex.left = left;
	tex.top = top;
	Picture &pic = tex.levels[0];
	pic.width = right - left;
	pic.height = bottom - top;
	pic.pixels.assign(static_cast<size_t>(pic.width) * pic.height, {0, 0, 0});
	std::vector<float> alpha(pic.pixels.size(), 0.0f);

	/* The ground of houses, industries and objects is black where their buildings stand,
	 * as the buildings hide it. Leave that out, and fill it from the ground around it. */
	bool building = IsTileType(tile, MP_HOUSE) || IsTileType(tile, MP_INDUSTRY) || IsTileType(tile, MP_OBJECT);

	/* Put the sprites on top of each other, like the viewport draws them. */
	for (size_t i = 0; i < this->key.size(); i += 4) {
		const RawSprite &raw = this->GetRawSprite(this->key[i]);
		PaletteID pal = this->key[i + 1];
		const uint8_t *remap = (pal != PAL_NONE) ? GetNonSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1 : nullptr;
		int ox = this->key[i + 2] + raw.x_offs - left;
		int oy = this->key[i + 3] + raw.y_offs - top;
		for (int y = 0; y < raw.height; y++) {
			for (int x = 0; x < raw.width; x++) {
				const SpriteLoader::CommonPixel &p = raw.pixels[y * raw.width + x];
				if (p.a == 0) continue;
				if (building && IsUnderBuilding(p)) continue;
				RGB c;
				if (p.m != 0) {
					const Colour &pc = _cur_palette.palette[remap != nullptr ? remap[p.m] : p.m];
					c = {static_cast<float>(pc.r), static_cast<float>(pc.g), static_cast<float>(pc.b)};
				} else {
					c = {static_cast<float>(p.r), static_cast<float>(p.g), static_cast<float>(p.b)};
				}
				size_t idx = static_cast<size_t>(oy + y) * pic.width + ox + x;
				float a = p.a / 255.0f;
				pic.pixels[idx] = pic.pixels[idx] * (1 - a) + c * a;
				alpha[idx] = a + alpha[idx] * (1 - a);
			}
		}
	}

	/* Undo the premultiplied alpha, and grow the picture into the transparent pixels around
	 * it, so smooth sampling at the edges of the tile does not mix in black. */
	RGB sum = {0, 0, 0};
	float count = 0;
	for (size_t i = 0; i < pic.pixels.size(); i++) {
		if (alpha[i] <= 0) continue;
		pic.pixels[i] = pic.pixels[i] * (1.0f / alpha[i]);
		sum = sum + pic.pixels[i];
		count++;
	}
	int passes = building ? MAX_FILL_PASSES : 4;
	for (int pass = 0; pass < passes; pass++) {
		std::vector<float> grown = alpha;
		bool changed = false;
		for (int y = 0; y < pic.height; y++) {
			for (int x = 0; x < pic.width; x++) {
				size_t idx = static_cast<size_t>(y) * pic.width + x;
				if (alpha[idx] > 0) continue;
				RGB acc = {0, 0, 0};
				int n = 0;
				auto take = [&](int nx, int ny) {
					if (nx < 0 || ny < 0 || nx >= pic.width || ny >= pic.height) return;
					size_t nidx = static_cast<size_t>(ny) * pic.width + nx;
					if (alpha[nidx] <= 0) return;
					acc = acc + pic.pixels[nidx];
					n++;
				};
				take(x - 1, y);
				take(x + 1, y);
				take(x, y - 1);
				take(x, y + 1);
				if (n == 0) continue;
				pic.pixels[idx] = acc * (1.0f / n);
				grown[idx] = 1;
				changed = true;
			}
		}
		alpha = std::move(grown);
		if (!changed) break;
	}
	RGB mean = count > 0 ? sum * (1.0f / count) : RGB{0, 0, 0};
	for (size_t i = 0; i < pic.pixels.size(); i++) {
		if (alpha[i] <= 0) pic.pixels[i] = mean;
	}

	for (int level = 1; level < TEXTURE_LEVELS; level++) tex.levels[level] = tex.levels[level - 1].Halve();

	return tex;
}

/**
 * Let a colour melt into the haze with distance.
 * @param colour The colour.
 * @param dist Distance from the camera.
 * @return The hazy colour.
 */
RGB Renderer::Fog(const RGB &colour, float dist) const
{
	float f = std::clamp((dist - FOG_START) / (FOG_END - FOG_START), 0.0f, 1.0f);
	return colour * (1 - f) + HAZE * f;
}

/**
 * Add a triangle to the picture, clipped at the near plane.
 * @param tri The corners, in camera coordinates.
 * @param paint How to paint it.
 */
void Renderer::AddTriangle(const std::array<ClipVertex, 3> &tri, const Paint &paint)
{
	if (tri[0].c.y >= NEAR && tri[1].c.y >= NEAR && tri[2].c.y >= NEAR) {
		this->SetupTriangle(tri[0], tri[1], tri[2], paint);
		return;
	}
	if (tri[0].c.y < NEAR && tri[1].c.y < NEAR && tri[2].c.y < NEAR) return;

	/* Cut off the part behind the near plane. */
	std::array<ClipVertex, 4> poly;
	int n = 0;
	for (int i = 0; i < 3; i++) {
		const ClipVertex &a = tri[i];
		const ClipVertex &b = tri[(i + 1) % 3];
		if (a.c.y >= NEAR) poly[n++] = a;
		if ((a.c.y >= NEAR) != (b.c.y >= NEAR)) {
			float t = (NEAR - a.c.y) / (b.c.y - a.c.y);
			ClipVertex &v = poly[n++];
			v.c = a.c + (b.c - a.c) * t;
			for (int k = 0; k < ATTRIBUTES; k++) v.a[k] = a.a[k] + (b.a[k] - a.a[k]) * t;
		}
	}
	for (int i = 1; i + 1 < n; i++) this->SetupTriangle(poly[0], poly[i], poly[i + 1], paint);
}

/**
 * Project a triangle in front of the camera onto the screen, and add it to the triangles to fill.
 * @param a First corner.
 * @param b Second corner.
 * @param c Third corner.
 * @param paint How to paint it.
 */
void Renderer::SetupTriangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c, const Paint &paint)
{
	const ClipVertex *v[3] = {&a, &b, &c};
	float sx[3], sy[3];
	ScreenTriangle t;
	for (int i = 0; i < 3; i++) {
		t.iw[i] = 1.0f / v[i]->c.y;
		sx[i] = this->cam.cx + this->cam.focal * v[i]->c.x * t.iw[i];
		sy[i] = this->cam.cy - this->cam.focal * v[i]->c.z * t.iw[i];
		for (int k = 0; k < ATTRIBUTES; k++) t.aw[i][k] = v[i]->a[k] * t.iw[i];
	}

	float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
	if (std::abs(area) < 1e-6f) return;

	t.x0 = std::max(0, static_cast<int>(std::floor(std::min({sx[0], sx[1], sx[2]}))));
	t.x1 = std::min(this->width - 1, static_cast<int>(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
	t.y0 = std::max(0, static_cast<int>(std::floor(std::min({sy[0], sy[1], sy[2]}))));
	t.y1 = std::min(this->height - 1, static_cast<int>(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
	if (t.x0 > t.x1 || t.y0 > t.y1) return;

	/* Edge functions: the weight of corner i, one in the corner and zero on the opposite edge. */
	float inv_area = 1.0f / area;
	for (int i = 0; i < 3; i++) {
		int j = (i + 1) % 3;
		int k = (i + 2) % 3;
		t.ex[i] = (sy[j] - sy[k]) * inv_area;
		t.ey[i] = (sx[k] - sx[j]) * inv_area;
		t.ec[i] = (sx[j] * sy[k] - sx[k] * sy[j]) * inv_area;
	}
	t.paint = paint;
	if (paint.texture != nullptr) {
		/* Use a smaller version of the texture when many texture pixels fall on one screen pixel. */
		float tex_area = std::abs((v[1]->a[0] - v[0]->a[0]) * (v[2]->a[1] - v[0]->a[1]) - (v[2]->a[0] - v[0]->a[0]) * (v[1]->a[1] - v[0]->a[1]));
		float ratio = std::sqrt(tex_area / std::abs(area));
		t.paint.level = ratio > 1 ? std::min(TEXTURE_LEVELS - 1, static_cast<int>(std::log2(ratio) + 0.5f)) : 0;
	}
	this->triangles.push_back(t);
}

/**
 * Add a flat coloured quad, shaded by its orientation.
 * @param quad The corners in world coordinates, in order around the quad.
 * @param colour The colour.
 * @param bias Depth bias.
 */
void Renderer::AddFlatQuad(const std::array<Vec3, 4> &quad, const RGB &colour, float bias)
{
	Vec3 normal = (quad[1] - quad[0]).Cross(quad[2] - quad[0]);
	if (normal.z < 0) normal = normal * -1;
	Paint paint = {nullptr, colour * Shade(normal), 1.0f, 0, bias};

	std::array<ClipVertex, 4> v;
	for (int i = 0; i < 4; i++) v[i] = {this->cam.ToCamera(quad[i]), {}};
	this->AddTriangle({v[0], v[1], v[2]}, paint);
	this->AddTriangle({v[0], v[2], v[3]}, paint);
}

/**
 * Fill the pixels of all triangles in every n-th stripe of rows.
 * Stripes do not share any pixels, so they can be drawn at the same time. Thin stripes
 * spread the work evenly, as the sky is cheap and the ground close by is expensive.
 * @param stripe Which stripe, from 0 to \a stripes - 1.
 * @param stripes Number of stripes.
 */
void Renderer::Rasterise(int stripe, int stripes)
{
	std::array<float, ATTRIBUTES> attr;
	for (const ScreenTriangle &t : this->triangles) {
		const Paint &paint = t.paint;
		for (int y = t.y0; y <= t.y1; y++) {
			if ((y / STRIPE_HEIGHT) % stripes != stripe) {
				/* Skip to the next stripe of ours. */
				y += STRIPE_HEIGHT - 1 - y % STRIPE_HEIGHT;
				continue;
			}
			/* Find the pixels of this row inside the triangle, instead of testing them all. */
			float py = y + 0.5f;
			float lo = t.x0 + 0.5f;
			float hi = t.x1 + 0.5f;
			bool empty = false;
			for (int i = 0; i < 3; i++) {
				float c = t.ey[i] * py + t.ec[i];
				if (t.ex[i] > 0) {
					lo = std::max(lo, -c / t.ex[i]);
				} else if (t.ex[i] < 0) {
					hi = std::min(hi, -c / t.ex[i]);
				} else if (c < 0) {
					empty = true;
				}
			}
			if (empty || lo > hi) continue;
			int xs = std::max(t.x0, static_cast<int>(std::ceil(lo - 0.5f)));
			int xe = std::min(t.x1, static_cast<int>(std::floor(hi - 0.5f)));

			float px = xs + 0.5f;
			float w0 = t.ex[0] * px + t.ey[0] * py + t.ec[0];
			float w1 = t.ex[1] * px + t.ey[1] * py + t.ec[1];
			float w2 = t.ex[2] * px + t.ey[2] * py + t.ec[2];
			size_t idx = static_cast<size_t>(y) * this->width + xs;
			for (int x = xs; x <= xe; x++, idx++, w0 += t.ex[0], w1 += t.ex[1], w2 += t.ex[2]) {
				float inv = (w0 * t.iw[0] + w1 * t.iw[1] + w2 * t.iw[2]) * paint.bias;
				if (inv <= this->depth[idx]) continue;
				this->depth[idx] = inv;
				float dist = paint.bias / inv;

				RGB colour = paint.colour;
				if (paint.texture != nullptr) {
					for (int k = 0; k < ATTRIBUTES; k++) attr[k] = (w0 * t.aw[0][k] + w1 * t.aw[1][k] + w2 * t.aw[2][k]) * dist;
					colour = paint.texture->Sample(attr[0], attr[1], paint.level);
					float light = paint.shade;
					if (dist < DETAIL_DIST) {
						/* Fine pattern fixed to the world, so the ground does not look blurry up close. */
						uint32_t gx = static_cast<uint32_t>(static_cast<int>(std::floor(attr[2] * 4)));
						uint32_t gy = static_cast<uint32_t>(static_cast<int>(std::floor(attr[3] * 4)));
						uint32_t h = ((gx * 73856093U) ^ (gy * 19349663U)) & 0xFF;
						light += (h / 255.0f - 0.5f) * 0.16f * (1 - dist / DETAIL_DIST);
					}
					colour = colour * light;
				}
				this->frame[idx] = Pack(this->Fog(colour, dist));
			}
		}
	}
}

/** Fill the view with the sky, and the haze below the horizon. */
void Renderer::DrawSky()
{
	float horizon = this->cam.cy + this->cam.focal * std::tan(PITCH_DEG * static_cast<float>(M_PI) / 180.0f);
	for (int y = 0; y < this->height; y++) {
		RGB colour = HAZE;
		if (y < horizon) {
			float t = std::pow(1.0f - std::clamp(y / std::max(horizon, 1.0f), 0.0f, 1.0f), 1.2f);
			colour = SKY_TOP * t + HAZE * (1 - t);
		}
		std::fill_n(this->frame.begin() + static_cast<size_t>(y) * this->width, this->width, Pack(colour));
	}
}

/**
 * Get the corner of a tile at the given local coordinates.
 * @param corner The corner.
 * @return Its position relative to the north corner of the tile.
 */
static Point CornerOffset(Corner corner)
{
	switch (corner) {
		case CORNER_W: return {TILE_SIZE, 0};
		case CORNER_S: return {TILE_SIZE, TILE_SIZE};
		case CORNER_E: return {0, TILE_SIZE};
		default: return {0, 0};
	}
}

/**
 * Height of the surface of a tile at a point, the way the game splits a slope into two triangles.
 * @param z Height of the corners.
 * @param split_we Whether the tile is split along the line between its west and east corner.
 * @param x X coordinate relative to the north corner of the tile.
 * @param y Y coordinate relative to the north corner of the tile.
 * @return The height.
 */
static float SurfaceZ(const std::array<float, CORNER_END> &z, bool split_we, float x, float y)
{
	float fx = x / TILE;
	float fy = y / TILE;
	float n = z[CORNER_N], w = z[CORNER_W], s = z[CORNER_S], e = z[CORNER_E];
	if (split_we) {
		if (fx + fy <= 1) return n + (w - n) * fx + (e - n) * fy;
		return s + (w - s) * (1 - fy) + (e - s) * (1 - fx);
	}
	if (fx >= fy) return n + (w - n) * (fx - fy) + (s - n) * fy;
	return n + (e - n) * (fy - fx) + (s - n) * fx;
}

/**
 * Add the ground of a tile to the picture, and what lies on it.
 * @param tile The tile.
 * @param dist Distance of the tile from the camera.
 */
void Renderer::AddTile(TileIndex tile, float dist)
{
	Slope raw_slope, slope;
	int raw_z, z;
	std::tie(raw_slope, raw_z) = GetTilePixelSlope(tile);
	std::tie(slope, z) = GetFoundationPixelSlope(tile);
	slope = RemoveHalftileSlope(slope);

	std::array<float, CORNER_END> corner_z;
	for (Corner c : ALL_CORNERS) corner_z[c] = static_cast<float>(z + GetSlopePixelZInCorner(slope, c));
	/* The game splits a slope along the line between the two corners at the same height. */
	bool split_we = corner_z[CORNER_W] == corner_z[CORNER_E] || corner_z[CORNER_N] != corner_z[CORNER_S];

	const GroundTexture &tex = this->GetGroundTexture(tile, raw_z);
	float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);

	/* Attributes: screen column and row in the texture, and world x and y for the fine pattern. */
	auto vertex = [&](Corner c) {
		Point o = CornerOffset(c);
		float h = corner_z[c];
		ClipVertex v;
		v.c = this->cam.ToCamera({x0 + o.x, y0 + o.y, h});
		v.a = {2.0f * (o.y - o.x), static_cast<float>(o.x + o.y) - (h - raw_z), x0 + o.x, y0 + o.y};
		return v;
	};
	std::array<ClipVertex, CORNER_END> v;
	std::array<Vec3, CORNER_END> p;
	for (Corner c : ALL_CORNERS) {
		v[c] = vertex(c);
		Point o = CornerOffset(c);
		p[c] = {x0 + o.x, y0 + o.y, corner_z[c]};
	}

	auto ground = [&](Corner a, Corner b, Corner c) {
		Vec3 normal = (p[b] - p[a]).Cross(p[c] - p[a]);
		if (normal.z < 0) normal = normal * -1;
		this->AddTriangle({v[a], v[b], v[c]}, {&tex, {}, Shade(normal), 0, 1.0f});
	};
	if (split_we) {
		ground(CORNER_N, CORNER_W, CORNER_E);
		ground(CORNER_S, CORNER_E, CORNER_W);
	} else {
		ground(CORNER_N, CORNER_W, CORNER_S);
		ground(CORNER_N, CORNER_S, CORNER_E);
	}

	/* Sides of foundations, down to the ground around them. */
	if (z != raw_z || slope != raw_slope) {
		static const std::array<std::pair<Corner, Corner>, 4> edges = {{{CORNER_N, CORNER_W}, {CORNER_W, CORNER_S}, {CORNER_S, CORNER_E}, {CORNER_E, CORNER_N}}};
		for (auto [a, b] : edges) {
			float ga = static_cast<float>(raw_z + GetSlopePixelZInCorner(RemoveHalftileSlope(raw_slope), a));
			float gb = static_cast<float>(raw_z + GetSlopePixelZInCorner(RemoveHalftileSlope(raw_slope), b));
			if (corner_z[a] <= ga && corner_z[b] <= gb) continue;
			this->AddFlatQuad({p[a], p[b], Vec3{p[b].x, p[b].y, gb}, Vec3{p[a].x, p[a].y, ga}}, FOUNDATION_COLOUR, 1.0f);
		}
	}

	this->AddTrack(tile, corner_z, split_we, dist);
}

/**
 * Get the rail track on a tile.
 * @param tile The tile.
 * @return The track pieces.
 */
static TrackBits GetTileRailTracks(TileIndex tile)
{
	switch (GetTileType(tile)) {
		case MP_RAILWAY: return IsPlainRail(tile) ? GetTrackBits(tile) : TRACK_BIT_NONE;
		case MP_STATION: return HasStationRail(tile) ? GetRailStationTrackBits(tile) : TRACK_BIT_NONE;
		case MP_ROAD: return IsLevelCrossing(tile) ? GetCrossingRailBits(tile) : TRACK_BIT_NONE;
		default: return TRACK_BIT_NONE;
	}
}

/**
 * Add rails and sleepers on top of the ground, so they stay sharp up close.
 * @param tile The tile.
 * @param corner_z Height of the corners of the tile.
 * @param split_we How the tile is split into triangles, see #SurfaceZ.
 * @param dist Distance of the tile from the camera.
 */
void Renderer::AddTrack(TileIndex tile, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist)
{
	if (dist > RAIL_DIST) return;
	TrackBits tracks = GetTileRailTracks(tile);
	if (tracks == TRACK_BIT_NONE) return;

	/* Ends of the track pieces, relative to the north corner: middles of the tile edges. */
	static const std::array<std::array<float, 4>, TRACK_END> ends = {{
		{0, 8, 16, 8}, // TRACK_X
		{8, 0, 8, 16}, // TRACK_Y
		{0, 8, 8, 0},  // TRACK_UPPER
		{8, 16, 16, 8}, // TRACK_LOWER
		{8, 0, 16, 8}, // TRACK_LEFT
		{0, 8, 8, 16}, // TRACK_RIGHT
	}};

	float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	auto at = [&](float lx, float ly, float lift) {
		return Vec3{x0 + lx, y0 + ly, SurfaceZ(corner_z, split_we, std::clamp(lx, 0.0f, 16.0f), std::clamp(ly, 0.0f, 16.0f)) + lift};
	};

	for (Track track : SetTrackBitIterator(tracks)) {
		const auto &e = ends[track];
		float dx = e[2] - e[0];
		float dy = e[3] - e[1];
		float len = std::hypot(dx, dy);
		float ux = dx / len, uy = dy / len; // along the track
		float px = -uy, py = ux; // across the track

		if (dist < SLEEPER_DIST) {
			int count = std::max(1, static_cast<int>(len / SLEEPER_SPACING));
			for (int i = 0; i < count; i++) {
				float t = (i + 0.5f) * len / count;
				float cx = e[0] + ux * t, cy = e[1] + uy * t;
				float hl = SLEEPER_LENGTH / 2, hw = SLEEPER_WIDTH / 2;
				this->AddFlatQuad({
					at(cx - px * hl - ux * hw, cy - py * hl - uy * hw, 0.08f),
					at(cx + px * hl - ux * hw, cy + py * hl - uy * hw, 0.08f),
					at(cx + px * hl + ux * hw, cy + py * hl + uy * hw, 0.08f),
					at(cx - px * hl + ux * hw, cy - py * hl + uy * hw, 0.08f),
				}, SLEEPER_COLOUR, 1.0005f);
			}
		}

		/* The rails reach a bit beyond the tile edge, so there are no gaps at bends. */
		float fade = std::clamp((dist - RAIL_FADE_START) / (RAIL_DIST - RAIL_FADE_START), 0.0f, 1.0f);
		RGB rail_colour = RAIL_COLOUR * (1 - fade) + RAIL_FAR_COLOUR * fade;
		float ext = RAIL_WIDTH / 2;
		for (float side : {-RAIL_GAUGE / 2, RAIL_GAUGE / 2}) {
			float ax = e[0] - ux * ext + px * side, ay = e[1] - uy * ext + py * side;
			float bx = e[2] + ux * ext + px * side, by = e[3] + uy * ext + py * side;
			float hw = RAIL_WIDTH / 2;
			this->AddFlatQuad({
				at(ax - px * hw, ay - py * hw, 0.18f),
				at(bx - px * hw, by - py * hw, 0.18f),
				at(bx + px * hw, by + py * hw, 0.18f),
				at(ax + px * hw, ay + py * hw, 0.18f),
			}, rail_colour, 1.001f);
		}
	}
}

/**
 * Put the finished picture on the screen, as a sprite of the current blitter.
 * @param r Where on the screen.
 */
void Renderer::Output(const Rect &r)
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	const DrawPixelInfo *dpi = _cur_dpi;

	if (blitter->GetScreenDepth() == 32) {
		/* All 32bpp blitters keep one Colour per pixel on the screen. Clear the area first,
		 * so the blitter's animation buffer (if any) leaves these pixels alone, then copy
		 * the true colour picture in. */
		GfxFillRect(r, PixelColour{0});
		int left = std::max(r.left, dpi->left);
		int right = std::min(r.right, dpi->left + dpi->width - 1);
		int top = std::max(r.top, dpi->top);
		int bottom = std::min(r.bottom, dpi->top + dpi->height - 1);
		for (int y = top; y <= bottom; y++) {
			Colour *dst = static_cast<Colour *>(blitter->MoveTo(dpi->dst_ptr, left - dpi->left, y - dpi->top));
			const uint32_t *src = &this->frame[static_cast<size_t>(y - r.top) * this->width + (left - r.left)];
			for (int x = left; x <= right; x++, dst++, src++) {
				*dst = Colour(GB(*src, 16, 8), GB(*src, 8, 8), GB(*src, 0, 8));
			}
		}
		return;
	}

	/* Other blitters: let the blitter encode the picture as a sprite of palette colours. */
	SpriteLoader::SpriteCollection collection;
	SpriteLoader::Sprite &sprite = collection[ZoomLevel::Min];
	sprite.AllocateData(ZoomLevel::Min, static_cast<size_t>(this->width) * this->height);
	sprite.width = this->width;
	sprite.height = this->height;
	sprite.x_offs = 0;
	sprite.y_offs = 0;
	sprite.colours = SpriteComponent::Palette;

	/* Ordered dither, so smooth gradients don't band where colours are rounded to the palette. */
	static constexpr std::array<std::array<int, 4>, 4> BAYER = {{{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}}};
	for (int y = 0; y < this->height; y++) {
		for (int x = 0; x < this->width; x++) {
			size_t idx = static_cast<size_t>(y) * this->width + x;
			int d = (BAYER[y & 3][x & 3] - 8) * 3 / 4;
			uint32_t c = this->frame[idx];
			auto channel = [&](uint8_t start) { return static_cast<uint8_t>(std::clamp<int>(GB(c, start, 8) + d, 0, 255)); };
			SpriteLoader::CommonPixel &p = sprite.data[idx];
			p.a = 0xFF;
			p.m = GetNearestColourIndex(channel(16), channel(8), channel(0));
		}
	}

	UniquePtrSpriteAllocator allocator;
	const Sprite *encoded = blitter->Encode(SpriteType::Font, collection, allocator);
	GfxBlitEncodedSprite(encoded, r.left, r.top);
}

/**
 * Draw the view.
 * @param r Where on the screen.
 * @param camera Where the camera is.
 * @param frame_id Number of the picture, see #DrawCabView3D.
 */
void Renderer::Draw(const Rect &r, const CabViewCamera &camera, uint32_t frame_id)
{
	if (camera.dark) {
		GfxFillRect(r, PC_BLACK);
		return;
	}
	if (r.Width() <= 0 || r.Height() <= 0) return;

	/* The window is often drawn in several parts; draw the picture only once. */
	if (this->frame_valid && this->frame_id == frame_id && this->frame_camera == camera && this->width == r.Width() && this->height == r.Height()) {
		this->Output(r);
		return;
	}

	auto start = std::chrono::steady_clock::now();
	if (this->textures.size() >= MAX_TEXTURES) this->textures.clear();

	this->width = r.Width();
	this->height = r.Height();
	size_t pixels = static_cast<size_t>(this->width) * this->height;
	this->frame.resize(pixels);
	this->depth.assign(pixels, 0.0f);

	float pitch = PITCH_DEG * static_cast<float>(M_PI) / 180.0f;
	Vec3 flat_fwd = {std::cos(camera.yaw), std::sin(camera.yaw), 0};
	Vec3 world_up = {0, 0, 1};
	this->cam.pos = {camera.x, camera.y, camera.z};
	this->cam.right = flat_fwd.Cross(world_up);
	this->cam.fwd = flat_fwd * std::cos(pitch) + world_up * std::sin(pitch);
	this->cam.up = world_up * std::cos(pitch) - flat_fwd * std::sin(pitch);
	this->cam.focal = (this->width / 2.0f) / std::tan(FOV_DEG / 2 * static_cast<float>(M_PI) / 180.0f);
	this->cam.cx = this->width / 2.0f;
	this->cam.cy = this->height / 2.0f;

	this->DrawSky();

	/* Tiles in the field of view, nearest first, so hidden pixels are not textured. */
	std::vector<std::pair<float, TileIndex>> visible;
	float half_fov = std::atan(this->cam.cx / this->cam.focal);
	float spread = std::tan(std::min(half_fov + 0.1f, 1.4f));
	int cx = static_cast<int>(camera.x / TILE);
	int cy = static_cast<int>(camera.y / TILE);
	/* The haze depends on the distance straight ahead, so at the sides the view reaches further. */
	int reach = static_cast<int>(VIEW_TILES * std::hypot(1.0f, spread)) + 2;
	for (int ty = std::max(0, cy - reach); ty <= std::min<int>(Map::MaxY(), cy + reach); ty++) {
		for (int tx = std::max(0, cx - reach); tx <= std::min<int>(Map::MaxX(), cx + reach); tx++) {
			TileIndex tile = TileXY(tx, ty);
			if (IsTileType(tile, MP_VOID)) continue;
			float dx = (tx + 0.5f) * TILE - camera.x;
			float dy = (ty + 0.5f) * TILE - camera.y;
			float ahead = dx * flat_fwd.x + dy * flat_fwd.y;
			float side = dx * this->cam.right.x + dy * this->cam.right.y;
			if (ahead < -TILE * 1.5f) continue;
			if (std::abs(side) > std::max(ahead, 0.0f) * spread + TILE * 1.5f) continue;
			if (ahead > FOG_END + TILE) continue;
			visible.emplace_back(std::hypot(dx, dy), tile);
		}
	}
	std::sort(visible.begin(), visible.end());
	this->triangles.clear();
	for (const auto &[dist, tile] : visible) this->AddTile(tile, dist);

	/* Fill the triangles in stripes of rows, on several threads. */
	auto prepared = std::chrono::steady_clock::now();
	int bands = std::clamp<int>(std::thread::hardware_concurrency(), 1, MAX_THREADS);
	std::vector<std::thread> threads;
	for (int i = 1; i < bands; i++) {
		std::thread &thread = threads.emplace_back();
		if (!StartNewThread(&thread, "ottd:cab3d", [this, i, bands]() { this->Rasterise(i, bands); })) {
			threads.pop_back();
			this->Rasterise(i, bands);
		}
	}
	this->Rasterise(0, bands);
	for (std::thread &thread : threads) thread.join();

	this->frame_valid = true;
	this->frame_id = frame_id;
	this->frame_camera = camera;

	auto rendered = std::chrono::steady_clock::now();
	this->Output(r);
	auto done = std::chrono::steady_clock::now();

	Debug(misc, 3, "[CabView3D] camera {:.1f},{:.1f},{:.1f} yaw {:.2f}", camera.x, camera.y, camera.z, camera.yaw);
	Debug(misc, 3, "[CabView3D] {}x{}, {} tiles, {} triangles, prepare {} us, fill {} us ({} threads), output {} us, {} textures",
		this->width, this->height, visible.size(), this->triangles.size(),
		std::chrono::duration_cast<std::chrono::microseconds>(prepared - start).count(),
		std::chrono::duration_cast<std::chrono::microseconds>(rendered - prepared).count(), bands,
		std::chrono::duration_cast<std::chrono::microseconds>(done - rendered).count(), this->textures.size());
}

/** The one renderer; all cab views draw one after the other. */
Renderer _renderer;

} // namespace

/**
 * Draw the 3D view from the cab.
 * @param r Where on the screen.
 * @param camera Where the camera is.
 * @param frame_id Number of the picture; the picture is only drawn again when it or the camera changed.
 */
void DrawCabView3D(const Rect &r, const CabViewCamera &camera, uint32_t frame_id)
{
	_renderer.Draw(r, camera, frame_id);
}

/** Forget all loaded graphics and buffers, e.g. because the graphics may have changed. */
void ResetCabView3D()
{
	_renderer.Reset();
}
