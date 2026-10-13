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
 *
 * Things standing on stations, like platforms and station buildings, are drawn as the boxes
 * the viewport sorts them by, with the sprite mapped back onto the sides the same way. The
 * sides the isometric view does not show get the picture of the opposite side, and where the
 * sprite is transparent, the box is too.
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
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "train.h"
#include "roadveh.h"

#include "table/sprites.h"

#include "thread.h"

#include <chrono>
#include <functional>
#include <map>
#include <tuple>

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
constexpr float TRACK_SMOOTH_LENGTH = 2.0f; ///< Length along straight track over which its ground is smoothed, see #SmoothAlong.
constexpr float BOX_DIST = 32.0f * TILE; ///< Buildings, trees, signals and vehicles are drawn up to this distance.
constexpr float BUILDING_HEIGHT = 0.6f; ///< Heights of buildings and vehicles in the 3D view, compared to their heights in the sprites; the isometric view makes them look tall.
constexpr float CUTOUT_UNITS = 0.28f; ///< Size of a sprite pixel of a cut-out, like a tree, in world units.
constexpr float ISO_UNITS = 0.3536f; ///< Width of a sprite pixel across the view, in world units; the isometric view draws a unit as 2 * sqrt(2) pixels.
constexpr float MIN_ROOF_HEIGHT = 3.0f; ///< A building is given a roof when its sprite reaches at least this much above its walls.
constexpr size_t MAX_TEXTURES = 4096; ///< Clear the texture cache when it grows beyond this.
constexpr size_t MAX_NEW_TEXTURES_TIMED = 32; ///< Pictures that make more new textures don't count for the average time of a picture.
constexpr int MAX_THREADS = 8; ///< At most this many threads draw the picture.
constexpr int PICTURES_PER_SCALE = 10; ///< Pictures to draw at a size before changing it again, so it does not keep changing.
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
	std::vector<RGB> pixels = {{0, 0, 0}}; ///< The pixels, row by row.
	std::vector<float> alpha; ///< Opacity of the pixels, row by row; empty when all are opaque.

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
	 * Sample the opacity of the picture smoothly; outside the picture it is transparent.
	 * @param x Horizontal position in pixels.
	 * @param y Vertical position in pixels.
	 * @return The opacity, from 0 to 1.
	 */
	float SampleAlpha(float x, float y) const
	{
		if (this->alpha.empty()) return 1.0f;
		float fx = x - 0.5f;
		float fy = y - 0.5f;
		int x0 = static_cast<int>(std::floor(fx));
		int y0 = static_cast<int>(std::floor(fy));
		fx -= x0;
		fy -= y0;
		auto at = [this](int px, int py) {
			if (px < 0 || py < 0 || px >= this->width || py >= this->height) return 0.0f;
			return this->alpha[py * this->width + px];
		};
		return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
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
		if (!this->alpha.empty()) half.alpha.assign(half.pixels.size(), 0.0f);
		for (int y = 0; y < half.height; y++) {
			for (int x = 0; x < half.width; x++) {
				RGB sum = {0, 0, 0};
				float alpha_sum = 0;
				for (int dy = 0; dy < 2; dy++) {
					for (int dx = 0; dx < 2; dx++) {
						size_t idx = static_cast<size_t>(std::min(2 * y + dy, this->height - 1)) * this->width + std::min(2 * x + dx, this->width - 1);
						sum = sum + this->pixels[idx];
						if (!this->alpha.empty()) alpha_sum += this->alpha[idx];
					}
				}
				half.pixels[y * half.width + x] = sum * 0.25f;
				if (!half.alpha.empty()) half.alpha[y * half.width + x] = alpha_sum * 0.25f;
			}
		}
		return half;
	}
};

/** Number of smaller versions of a ground texture, used further away so the ground does not glitter. */
constexpr int TEXTURE_LEVELS = 4;

/**
 * The ground sprites of a tile put together, in normal zoom screen pixels.
 * Also used for a single sprite standing in a box; then it is relative to where the sprite is drawn.
 */
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

	/**
	 * Sample the opacity of the texture smoothly.
	 * @param col Screen column relative to the north corner of the tile.
	 * @param row Screen row relative to the north corner of the tile at its height.
	 * @param level Which version of the picture to use.
	 * @return The opacity, from 0 to 1.
	 */
	float SampleAlpha(float col, float row, int level) const
	{
		float scale = 1.0f / (1 << level);
		return this->levels[level].SampleAlpha((col - this->left) * scale, (row - this->top) * scale);
	}
};

/** Hash of the key of a ground texture. */
struct TextureKeyHash {
	size_t operator()(const std::vector<int32_t> &key) const
	{
		uint64_t h = 14695981039346656037ULL;
		for (int32_t v : key) h = (h ^ static_cast<uint32_t>(v)) * 1099511628211ULL;
		return static_cast<size_t>(h ^ (h >> 32));
	}
};

/**
 * Where a thing dressed in a sprite stands in the world. Points of the thing are given in the coordinates of its
 * sprite: the point (x, y, z) is at sprite pixel (2 (y - x) - sx, x + y - z - sy), see #RemapCoords. They are
 * turned around the vertical axis and moved to their place, and their heights are scaled.
 */
struct Placement {
	Vec3 origin = {0, 0, 0}; ///< Where the point (0, 0, 0) of the thing is in the world.
	float cos = 1; ///< Cosine of the angle the thing is turned by.
	float sin = 0; ///< Sine of the angle the thing is turned by.
	float base = 0; ///< Height the heights of the thing are scaled from.
	float height_scale = 1; ///< How much the heights above #base are scaled.
	float sx = 0; ///< Screen column where the viewport draws the sprite, in normal zoom pixels.
	float sy = 0; ///< Screen row where the viewport draws the sprite, in normal zoom pixels.

	/** Get the world position of a point of the thing. */
	Vec3 ToWorld(const Vec3 &p) const
	{
		return {this->origin.x + p.x * this->cos - p.y * this->sin, this->origin.y + p.x * this->sin + p.y * this->cos,
			this->origin.z + this->base + (p.z - this->base) * this->height_scale};
	}

	/** Get the world direction of a horizontal or vertical direction of the thing. */
	Vec3 Turn(const Vec3 &d) const
	{
		return {d.x * this->cos - d.y * this->sin, d.x * this->sin + d.y * this->cos, d.z};
	}
};

/** Roof of a building in the 3D view. */
enum class RoofKind : uint8_t {
	Flat, ///< A flat roof, or one sloping one way when the walls are not as high.
	Hip, ///< A hip roof with its ridge along the longer side; a pyramid on a square building.
	GableX, ///< A gable roof with its ridge along the x axis.
	GableY, ///< A gable roof with its ridge along the y axis.
};

/** How a building looks in the 3D view, worked out from its sprite, see Renderer::GetBuildingShape. */
struct BuildingShape {
	bool cutout = false; ///< Whether it is a cut-out facing the camera, as it is no box (silos, a forest).
	bool plants = false; ///< Whether it is mostly plants, like a forest; its cut-out is then the size of trees.
	float x0 = 0; ///< Start of the footprint along the x axis, relative to the north corner of the box.
	float y0 = 0; ///< Start of the footprint along the y axis, relative to the north corner of the box.
	float x1 = 0; ///< End of the footprint along the x axis, relative to the north corner of the box.
	float y1 = 0; ///< End of the footprint along the y axis, relative to the north corner of the box.
	std::array<float, 4> walls = {}; ///< Heights of the walls at the corners (x0, y0), (x1, y0), (x1, y1) and (x0, y1), above the bottom of the box.
	float ridge = 0; ///< Height of the ridge of the roof above the bottom of the box; no such roof when it is not above the walls.
	RoofKind roof = RoofKind::Flat; ///< Kind of roof.
	float band = 0; ///< Height of the lowest band of the walls, see #Renderer::AddSpriteWalls.
	float foot_col = 0; ///< Column of the sprite at the foot of the cut-out, relative to where the sprite is drawn.
	float foot_row = 0; ///< Row of the sprite at the foot of the cut-out, relative to where the sprite is drawn.
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
	bool cutout = false; ///< Whether pixels where the texture is transparent are left out.
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
	void Draw(const Rect &r, const CabViewCamera &camera, uint32_t frame_id, float budget_ms);

	/**
	 * Get how long making a picture takes, on average.
	 * @param scale Size of the picture compared to the view.
	 * @return The time in milliseconds; 0 before the first picture.
	 */
	float GetFrameTime(float scale) const { return this->fixed_ms + this->fill_ms * scale * scale; }

	void Reset()
	{
		this->sprites.clear();
		this->textures.clear();
		this->box_textures.clear();
		this->building_shapes.clear();
		this->frame = {};
		this->depth = {};
		this->frame_valid = false;
	}

private:
	int width = 0; ///< Width of the picture.
	int height = 0; ///< Height of the picture.
	int view_width = 0; ///< Width of the view on the screen; the picture is scaled to it.
	int view_height = 0; ///< Height of the view on the screen; the picture is scaled to it.
	float scale = 1.0f; ///< Size of the picture compared to the view, smaller when drawing takes too long.
	int pictures_at_scale = 0; ///< Pictures drawn since #scale last changed.
	std::vector<uint32_t> frame; ///< Colour buffer, see #Pack.
	std::vector<float> depth; ///< Inverse depth buffer; 0 is infinitely far.
	Camera cam; ///< The current camera.
	bool frame_valid = false; ///< Whether #frame holds a finished picture.
	CabViewCamera frame_camera; ///< Camera of the picture in #frame.
	uint32_t frame_id = 0; ///< Identification of the picture in #frame, see #DrawCabView3D.
	float fixed_ms = 0; ///< Average time (in milliseconds) of the parts of making a picture that don't depend on its size.
	float fill_ms = 0; ///< Average time (in milliseconds) filling a picture of the size of the view takes.

	std::unordered_map<SpriteID, RawSprite> sprites; ///< Sprites loaded so far.
	std::unordered_map<std::vector<int32_t>, GroundTexture, TextureKeyHash> textures; ///< Ground textures put together so far.
	std::unordered_map<uint64_t, GroundTexture> box_textures; ///< Textures of sprites standing in boxes, by sprite and palette.
	std::map<std::tuple<SpriteID, PaletteID, int, int, int, int, int, bool>, BuildingShape> building_shapes; ///< Shapes of buildings worked out so far, see #GetBuildingShape.
	std::vector<GroundSpriteToDraw> ground_sprites; ///< Scratch buffer for collecting ground sprites.
	std::vector<BoxSpriteToDraw> box_sprites; ///< Scratch buffer for collecting sprites standing in boxes.
	std::vector<int32_t> key; ///< Scratch buffer for texture keys.

	const RawSprite &GetRawSprite(SpriteID sprite);
	const GroundTexture &GetGroundTexture(TileIndex tile, int tile_z, bool boxes, std::pair<float, float> smooth);
	const GroundTexture &GetBoxTexture(SpriteID image, PaletteID pal, bool widen = true);
	RGB Fog(const RGB &colour, float dist) const;

	std::vector<ScreenTriangle> triangles; ///< Triangles of the current picture, nearest first.

	void AddTriangle(const std::array<ClipVertex, 3> &tri, const Paint &paint);
	void SetupTriangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c, const Paint &paint);
	void AddFlatQuad(const std::array<Vec3, 4> &quad, const RGB &colour, float bias);
	void Rasterise(int stripe, int stripes);

	void DrawSky();
	void AddTile(TileIndex tile, float dist);
	void AddTrack(TileIndex tile, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist);
	void AddBox(const BoxSpriteToDraw &box, int ground_z, float height_scale);
	const BuildingShape &GetBuildingShape(const BoxSpriteToDraw &box, bool station);
	void AddBuilding(const BoxSpriteToDraw &box, float height_scale, bool station);
	void AddCutout(const GroundTexture &tex, const Vec3 &foot, float foot_col, float foot_row, float scale_h, float scale_v);
	void AddBillboard(const BoxSpriteToDraw &box);
	void AddVehicles();
	void AddSpriteFace(const GroundTexture &tex, const Placement &pl, std::span<const Vec3> pts, const Vec3 &normal, const std::function<Vec3(const Vec3 &)> &shown, bool cutout);
	void AddSpriteWalls(const GroundTexture &tex, const Placement &pl, const Vec3 &lo, const Vec3 &hi, const std::array<float, 4> &tops, float band, bool cutout);
	void AddSpriteBox(const GroundTexture &tex, const Placement &pl, const Vec3 &lo, const Vec3 &hi, bool bottom, bool top, bool cutout = true);
	void ChooseScale(float budget_ms);
	void Output(const Rect &r);
};

/**
 * Run a job on several threads at once.
 * @param job The job; it gets the number of its part and the number of parts.
 * @return The number of parts.
 */
static int RunOnThreads(const std::function<void(int, int)> &job)
{
	int parts = std::clamp<int>(std::thread::hardware_concurrency(), 1, MAX_THREADS);
	std::vector<std::thread> threads;
	for (int i = 1; i < parts; i++) {
		std::thread &thread = threads.emplace_back();
		if (!StartNewThread(&thread, "ottd:cab3d", [&job, i, parts]() { job(i, parts); })) {
			threads.pop_back();
			job(i, parts);
		}
	}
	job(0, parts);
	for (std::thread &thread : threads) thread.join();
	return parts;
}

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
 * Smooth a picture along a direction: every pixel becomes the average of the picture along a line through it.
 * Lines along straight track are steps of pixels in the isometric sprites. Mapped onto the ground in 3D
 * they become teeth along the edges of the ballast, which seem to shake from side to side when the train
 * drives over them. Averaged along the track over whole steps, the edges are straight.
 * @param pic The picture; it has no opacity.
 * @param dx Columns the line spans.
 * @param dy Rows the line spans.
 * @return The smoothed picture.
 */
static Picture SmoothAlong(const Picture &pic, float dx, float dy)
{
	int samples = std::max(2, static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)) * 2)));
	Picture smooth = pic;
	for (int y = 0; y < pic.height; y++) {
		for (int x = 0; x < pic.width; x++) {
			RGB sum = {0, 0, 0};
			for (int i = 0; i < samples; i++) {
				float t = (i + 0.5f) / samples - 0.5f;
				sum = sum + pic.Sample(x + 0.5f + dx * t, y + 0.5f + dy * t);
			}
			smooth.pixels[static_cast<size_t>(y) * pic.width + x] = sum * (1.0f / samples);
		}
	}
	return smooth;
}

/**
 * Finish a texture whose picture has been painted: undo the premultiplied alpha, grow the picture
 * into the transparent pixels around it, so smooth sampling at the edges does not mix in black,
 * and make the smaller versions of it.
 * @param tex The texture, with its picture painted in its first level.
 * @param alpha Opacity of the pixels of the picture.
 * @param passes How many pixels at most the picture grows.
 * @param keep_alpha Whether to keep the opacity, for leaving out the transparent pixels when drawing.
 * @param smooth How far to smooth the picture, in pixels, see #SmoothAlong; nothing when zero.
 * @param widen Whether to widen the opaque area by a pixel, against teeth along the sides of boxes; not for cut-outs.
 */
static void FinishTexture(GroundTexture &tex, std::vector<float> alpha, int passes, bool keep_alpha, std::pair<float, float> smooth = {0, 0}, bool widen = true)
{
	Picture &pic = tex.levels[0];
	if (keep_alpha && !widen) {
		pic.alpha = alpha;
	} else if (keep_alpha) {
		/* Widen the opaque area by a pixel: the edges of the sprite are steps of pixels along the edges of
		 * its box, which would otherwise cut teeth into the sides of the box. */
		pic.alpha = alpha;
		for (int y = 0; y < pic.height; y++) {
			for (int x = 0; x < pic.width; x++) {
				float &a = pic.alpha[static_cast<size_t>(y) * pic.width + x];
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						int nx = x + dx, ny = y + dy;
						if (nx < 0 || ny < 0 || nx >= pic.width || ny >= pic.height) continue;
						a = std::max(a, alpha[static_cast<size_t>(ny) * pic.width + nx]);
					}
				}
			}
		}
	}

	RGB sum = {0, 0, 0};
	float count = 0;
	for (size_t i = 0; i < pic.pixels.size(); i++) {
		if (alpha[i] <= 0) continue;
		pic.pixels[i] = pic.pixels[i] * (1.0f / alpha[i]);
		sum = sum + pic.pixels[i];
		count++;
	}
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
	if (smooth.first != 0 || smooth.second != 0) pic = SmoothAlong(pic, smooth.first, smooth.second);

	for (int level = 1; level < TEXTURE_LEVELS; level++) tex.levels[level] = tex.levels[level - 1].Halve();
}

/**
 * Paint a sprite into a picture, on top of what is there.
 * @param pic The picture.
 * @param alpha Opacity of the pixels of the picture.
 * @param raw The sprite.
 * @param pal Its palette.
 * @param ox Column of the picture for the top left pixel of the sprite.
 * @param oy Row of the picture for the top left pixel of the sprite.
 * @param skip_under_building Leave out the pixels under buildings, see #IsUnderBuilding.
 */
static void PaintSprite(Picture &pic, std::vector<float> &alpha, const RawSprite &raw, PaletteID pal, int ox, int oy, bool skip_under_building)
{
	const uint8_t *remap = (pal != PAL_NONE) ? GetNonSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1 : nullptr;
	for (int y = 0; y < raw.height; y++) {
		for (int x = 0; x < raw.width; x++) {
			const SpriteLoader::CommonPixel &p = raw.pixels[y * raw.width + x];
			if (p.a == 0) continue;
			if (skip_under_building && IsUnderBuilding(p)) continue;
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

/**
 * Get the ground texture of a tile, putting it together from the tile's ground sprites when needed.
 * Tiles that look the same share the texture.
 * @param tile The tile.
 * @param tile_z Height of the tile as the viewport uses it, see #GetTilePixelSlope.
 * @param boxes Whether to also collect the sprites standing in boxes on the tile, into #box_sprites.
 * @param smooth How far to smooth the texture, in pixels, see #SmoothAlong.
 * @return The texture.
 */
const GroundTexture &Renderer::GetGroundTexture(TileIndex tile, int tile_z, bool boxes, std::pair<float, float> smooth)
{
	this->ground_sprites.clear();
	this->box_sprites.clear();
	CollectTileGroundSprites(tile, this->ground_sprites, boxes ? &this->box_sprites : nullptr);

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
	size_t sprite_keys = this->key.size();
	if (sprite_keys != 0 && (smooth.first != 0 || smooth.second != 0)) {
		this->key.push_back(static_cast<int32_t>(std::lround(smooth.first * 64)));
		this->key.push_back(static_cast<int32_t>(std::lround(smooth.second * 64)));
	}

	auto it = this->textures.find(this->key);
	if (it != this->textures.end()) return it->second;
	GroundTexture &tex = this->textures[this->key];
	if (this->key.empty()) return tex;

	/* Bounding box of all sprites. */
	int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
	for (size_t i = 0; i < sprite_keys; i += 4) {
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
	for (size_t i = 0; i < sprite_keys; i += 4) {
		const RawSprite &raw = this->GetRawSprite(this->key[i]);
		PaintSprite(pic, alpha, raw, this->key[i + 1], this->key[i + 2] + raw.x_offs - left, this->key[i + 3] + raw.y_offs - top, building);
	}

	FinishTexture(tex, std::move(alpha), building ? MAX_FILL_PASSES : 4, false, smooth);
	return tex;
}

/**
 * Get the texture of a sprite standing in a box, making it when needed.
 * Its pixels are relative to where the viewport would draw the sprite, before its offsets.
 * @param image The sprite.
 * @param pal Its palette.
 * @param widen Whether to widen the opaque area by a pixel, see #FinishTexture.
 * @return The texture; without any pixels when the sprite has none.
 */
const GroundTexture &Renderer::GetBoxTexture(SpriteID image, PaletteID pal, bool widen)
{
	uint64_t key = (static_cast<uint64_t>(image) << 33) | (static_cast<uint64_t>(pal) << 1) | (widen ? 1 : 0);
	auto it = this->box_textures.find(key);
	if (it != this->box_textures.end()) return it->second;
	GroundTexture &tex = this->box_textures[key];

	const RawSprite &raw = this->GetRawSprite(image & SPRITE_MASK);
	if (raw.pixels.empty()) return tex;
	tex.left = raw.x_offs;
	tex.top = raw.y_offs;
	Picture &pic = tex.levels[0];
	pic.width = raw.width;
	pic.height = raw.height;
	pic.pixels.assign(static_cast<size_t>(pic.width) * pic.height, {0, 0, 0});
	std::vector<float> alpha(pic.pixels.size(), 0.0f);
	PaintSprite(pic, alpha, raw, pal, 0, 0, false);
	FinishTexture(tex, std::move(alpha), 4, true, {0, 0}, widen);
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
				float dist = paint.bias / inv;

				RGB colour = paint.colour;
				if (paint.texture != nullptr) {
					for (int k = 0; k < ATTRIBUTES; k++) attr[k] = (w0 * t.aw[0][k] + w1 * t.aw[1][k] + w2 * t.aw[2][k]) * dist;
					if (paint.cutout && paint.texture->SampleAlpha(attr[0], attr[1], paint.level) < 0.5f) continue;
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
				this->depth[idx] = inv;
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

	/* Straight track: smooth its ground along the track, over whole steps of the pixels of its edges.
	 * Going a unit along it moves two columns in the texture, and a row less how much it climbs. */
	std::pair<float, float> smooth = {0, 0};
	TrackBits tracks = GetTileRailTracks(tile);
	if (tracks == TRACK_BIT_X) {
		float climb = (corner_z[CORNER_W] + corner_z[CORNER_S] - corner_z[CORNER_N] - corner_z[CORNER_E]) / (2 * TILE);
		smooth = {-2 * TRACK_SMOOTH_LENGTH, (1 - climb) * TRACK_SMOOTH_LENGTH};
	} else if (tracks == TRACK_BIT_Y) {
		float climb = (corner_z[CORNER_E] + corner_z[CORNER_S] - corner_z[CORNER_N] - corner_z[CORNER_W]) / (2 * TILE);
		smooth = {2 * TRACK_SMOOTH_LENGTH, (1 - climb) * TRACK_SMOOTH_LENGTH};
	}

	bool boxes = dist <= BOX_DIST;
	const GroundTexture &tex = this->GetGroundTexture(tile, raw_z, boxes, smooth);
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
	if (boxes) {
		for (const BoxSpriteToDraw &box : this->box_sprites) {
			switch (GetTileType(tile)) {
				case MP_TREES:
					this->AddBillboard(box);
					break;

				case MP_CLEAR:
					/* Fences around fields; their boxes are the whole tile. */
					break;

				case MP_HOUSE:
				case MP_INDUSTRY:
				case MP_OBJECT:
					if (!box.child) this->AddBuilding(box, BUILDING_HEIGHT, false);
					break;

				case MP_STATION:
					/* Station buildings, standing on the ground, get their real shape; platforms and roofs stay boxes. */
					if (!box.child && box.zmax - box.zmin >= 10 && box.zmin <= z + 1) {
						this->AddBuilding(box, BUILDING_HEIGHT, true);
						break;
					}
					[[fallthrough]];

				default:
					if (box.child) break;
					/* Fences along the edges of the tile don't map well onto their thin boxes; leave them out. */
					if (std::min(box.xmax - box.xmin, box.ymax - box.ymin) == 0 && std::max(box.xmax - box.xmin, box.ymax - box.ymin) >= 8) break;
					/* Thin things like signals and lamps stand as cut-outs. */
					if (box.xmax - box.xmin < 2 && box.ymax - box.ymin < 2) {
						this->AddBillboard(box);
					} else {
						this->AddBox(box, z, 1.0f);
					}
					break;
			}
		}
	}
}

/**
 * Add a flat side of a thing dressed in its sprite, see #Placement.
 * @param tex The texture of the sprite.
 * @param pl Where the thing is.
 * @param pts The corners of the side, in the coordinates of the sprite, in order around it.
 * @param normal Direction the side faces, in the coordinates of the sprite; sides facing away from the camera are left out.
 * @param shown Where on the thing the picture of a point of the side is: the point itself for sides the isometric
 *              view shows, the matching point of the opposite side for the others.
 * @param cutout Whether pixels where the sprite is transparent are left out.
 */
void Renderer::AddSpriteFace(const GroundTexture &tex, const Placement &pl, std::span<const Vec3> pts, const Vec3 &normal, const std::function<Vec3(const Vec3 &)> &shown, bool cutout)
{
	std::array<Vec3, 4> world;
	for (size_t i = 0; i < pts.size(); i++) world[i] = pl.ToWorld(pts[i]);
	/* Scaling the heights tilts slanted sides; take the normal from the corners, facing the way it should. */
	Vec3 n = (world[1] - world[0]).Cross(world[2] - world[0]);
	Vec3 want = pl.Turn(normal);
	if (n.Dot(want) < 0) n = n * -1;
	if (n.Dot(n) == 0) return;
	if ((this->cam.pos - world[0]).Dot(n) <= 0) return;
	n = n.Normalised();

	std::array<ClipVertex, 4> v;
	for (size_t i = 0; i < pts.size(); i++) {
		Vec3 m = shown(pts[i]);
		v[i].c = this->cam.ToCamera(world[i]);
		v[i].a[0] = 2.0f * (m.y - m.x) - pl.sx;
		v[i].a[1] = (m.x + m.y - m.z) - pl.sy;
		/* The fine pattern runs along the side. */
		if (std::abs(n.z) > 0.7f) {
			v[i].a[2] = world[i].x;
			v[i].a[3] = world[i].y;
		} else {
			v[i].a[2] = world[i].y * n.x - world[i].x * n.y;
			v[i].a[3] = world[i].z;
		}
	}
	Paint paint = {&tex, {}, Shade(n), 0, 1.0f, cutout};
	for (size_t i = 1; i + 1 < pts.size(); i++) this->AddTriangle({v[0], v[i], v[i + 1]}, paint);
}

/**
 * Add a box dressed in a sprite, see #Placement. The sides the isometric view does not show get the picture
 * of the opposite side; where the sprite is transparent, the box is too.
 * @param tex The texture of the sprite.
 * @param pl Where the box is.
 * @param lo Lowest corner of the box, in the coordinates of the sprite.
 * @param hi Highest corner of the box, in the coordinates of the sprite.
 * @param bottom Whether to add the bottom of the box.
 * @param top Whether to add the top of the box.
 * @param cutout Whether pixels where the sprite is transparent are left out.
 */
void Renderer::AddSpriteBox(const GroundTexture &tex, const Placement &pl, const Vec3 &lo, const Vec3 &hi, bool bottom, bool top, bool cutout)
{
	const float x0 = lo.x, y0 = lo.y, z0 = lo.z, x1 = hi.x, y1 = hi.y, z1 = hi.z;
	auto same = [](const Vec3 &p) { return p; };
	if (top) {
		std::array<Vec3, 4> q = {Vec3{x0, y0, z1}, Vec3{x1, y0, z1}, Vec3{x1, y1, z1}, Vec3{x0, y1, z1}};
		this->AddSpriteFace(tex, pl, q, {0, 0, 1}, same, cutout);
	}
	if (bottom) {
		std::array<Vec3, 4> q = {Vec3{x0, y0, z0}, Vec3{x1, y0, z0}, Vec3{x1, y1, z0}, Vec3{x0, y1, z0}};
		this->AddSpriteFace(tex, pl, q, {0, 0, -1}, [z1](Vec3 p) { p.z = z1; return p; }, cutout);
	}

	this->AddSpriteWalls(tex, pl, lo, hi, {z1, z1, z1, z1}, std::min(2.0f, (z1 - z0) / 3), cutout);
}

/**
 * Add the four walls of a thing dressed in its sprite, see #AddSpriteFace. Walls the isometric view
 * doesn't show get the picture of the opposite wall.
 * @param tex The texture of the sprite.
 * @param pl Where the thing is.
 * @param lo The lower corner of the footprint, at the bottom of the walls.
 * @param hi The upper corner of the footprint.
 * @param tops Heights of the tops of the walls at the corners (lo.x, lo.y), (hi.x, lo.y), (hi.x, hi.y) and (lo.x, hi.y).
 * @param band Height of the lowest band of the walls, which gets the picture just above it, stretched down. The bottom
 *             edge of a wall is a line of steps of pixels in the sprite, which would cut teeth into it, and often the
 *             ground sprite draws the lowest part of a building.
 * @param cutout Whether pixels where the sprite is transparent are left out.
 */
void Renderer::AddSpriteWalls(const GroundTexture &tex, const Placement &pl, const Vec3 &lo, const Vec3 &hi, const std::array<float, 4> &tops, float band, bool cutout)
{
	const float z0 = lo.z;
	const std::array<std::pair<float, float>, 4> corners = {{{lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}}};
	auto wall = [&](int a, int b, const Vec3 &normal, const std::function<Vec3(const Vec3 &)> &mirror) {
		const auto [ax, ay] = corners[a];
		const auto [bx, by] = corners[b];
		const float zb = z0 + std::clamp(band, 0.0f, 0.6f * (std::min(tops[a], tops[b]) - z0));
		std::array<Vec3, 4> upper = {Vec3{ax, ay, zb}, Vec3{bx, by, zb}, Vec3{bx, by, tops[b]}, Vec3{ax, ay, tops[a]}};
		this->AddSpriteFace(tex, pl, upper, normal, mirror, cutout);
		if (zb <= z0) return;
		std::array<Vec3, 4> lower = {Vec3{ax, ay, z0}, Vec3{bx, by, z0}, Vec3{bx, by, zb}, Vec3{ax, ay, zb}};
		this->AddSpriteFace(tex, pl, lower, normal, [&](Vec3 p) { p.z = zb; return mirror(p); }, cutout);
	};
	wall(1, 2, {1, 0, 0}, [](const Vec3 &p) { return p; });
	wall(0, 3, {-1, 0, 0}, [&](Vec3 p) { p.x = hi.x; return p; });
	wall(3, 2, {0, 1, 0}, [](const Vec3 &p) { return p; });
	wall(0, 1, {0, -1, 0}, [&](Vec3 p) { p.y = hi.y; return p; });
}

/**
 * Get the topmost opaque pixel of a texture.
 * @param tex The texture.
 * @return The row of the top of that pixel, relative to where the sprite is drawn; nothing when there is none.
 */
static std::optional<int> TopRow(const GroundTexture &tex)
{
	const Picture &pic = tex.levels[0];
	for (size_t i = 0; i < pic.alpha.size(); i++) {
		if (pic.alpha[i] >= 0.5f) return static_cast<int>(i / pic.width) + tex.top;
	}
	return std::nullopt;
}

/**
 * Add a thing standing on a tile as the box the viewport sorts it by, with its sprite on the sides.
 * @param box The sprite and its box.
 * @param ground_z Height of the ground of the tile it stands on.
 * @param height_scale How much the heights of the box are scaled, see #BUILDING_HEIGHT.
 */
void Renderer::AddBox(const BoxSpriteToDraw &box, int ground_z, float height_scale)
{
	const GroundTexture &tex = this->GetBoxTexture(box.image, box.pal);
	if (tex.levels[0].alpha.empty()) return;

	Placement pl;
	pl.base = static_cast<float>(box.zmin);
	pl.height_scale = height_scale;
	pl.sx = static_cast<float>(box.x) / ZOOM_BASE;
	pl.sy = static_cast<float>(box.y) / ZOOM_BASE;
	Vec3 lo = {static_cast<float>(box.xmin), static_cast<float>(box.ymin), static_cast<float>(box.zmin)};
	Vec3 hi = {static_cast<float>(box.xmax + 1), static_cast<float>(box.ymax + 1), static_cast<float>(box.zmax + 1)};
	this->AddSpriteBox(tex, pl, lo, hi, box.zmin > ground_z, true);
}

/** A shape of a building tried on its sprite, relative to the north corner of the bottom of its box. */
struct ShapeTry {
	float x0, y0, x1, y1; ///< The footprint.
	std::array<float, 4> walls; ///< Heights of the walls at the corners (x0, y0), (x1, y0), (x1, y1) and (x0, y1).
	float ridge; ///< Height of the ridge of the roof; no roof when it is not above the walls.
	RoofKind roof; ///< The kind of roof.
};

/** How well shapes of a building fit the outline of its sprite. */
class SpriteFit {
public:
	/** How a shape fits. */
	struct Result {
		int score = 0; ///< Opaque pixels in the outline of the shape, less the transparent ones.
		int inside = 0; ///< Opaque pixels in the outline.
		int area = 0; ///< Pixels in the outline.
		int above = 0; ///< Opaque pixels above the outline, like a tree behind a house.

		/**
		 * How much the outline and the sprite overlap.
		 * @param opaque Opaque pixels of the sprite.
		 * @return The pixels in both, compared to those in either.
		 */
		float Overlap(int opaque) const { return static_cast<float>(this->inside) / std::max(this->area + opaque - this->inside, 1); }
	};

	int opaque = 0; ///< Opaque pixels of the sprite.

	/**
	 * Prepare fitting shapes to a sprite.
	 * @param tex The texture of the sprite, without widening.
	 * @param ax Column where the north corner of the box is drawn, relative to where the sprite is drawn.
	 * @param ay Row where the north corner of the box is drawn, relative to where the sprite is drawn.
	 */
	SpriteFit(const GroundTexture &tex, float ax, float ay) : pic(tex.levels[0]), left(tex.left), top(tex.top), ax(ax), ay(ay)
	{
		const size_t width = this->pic.width;
		this->prefix.assign((this->pic.height + 1) * width, 0);
		for (int y = 0; y < this->pic.height; y++) {
			for (size_t x = 0; x < width; x++) {
				const size_t i = y * width + x;
				this->prefix[i + width] = this->prefix[i] + (this->pic.alpha[i] >= 0.5f ? 1 : 0);
			}
		}
		for (size_t x = 0; x < width; x++) this->opaque += this->prefix[this->pic.height * width + x];
	}

	/**
	 * Measure how well a shape fits the sprite: how many pixels of the outline of the shape are opaque.
	 * @param s The shape.
	 * @return How it fits.
	 */
	Result Measure(const ShapeTry &s) const
	{
		using Point = std::pair<float, float>;
		std::array<Point, 10> points;
		size_t n = 0;
		auto add = [&](float x, float y, float z) { points[n++] = {2 * (y - x) - this->ax, x + y - z - this->ay}; };
		const std::array<Point, 4> corners = {{{s.x0, s.y0}, {s.x1, s.y0}, {s.x1, s.y1}, {s.x0, s.y1}}};
		for (int i = 0; i < 4; i++) {
			add(corners[i].first, corners[i].second, 0);
			add(corners[i].first, corners[i].second, s.walls[i]);
		}
		const float xc = (s.x0 + s.x1) / 2, yc = (s.y0 + s.y1) / 2;
		if (s.ridge > *std::max_element(s.walls.begin(), s.walls.end())) {
			switch (s.roof) {
				case RoofKind::Hip:
					if (s.x1 - s.x0 >= s.y1 - s.y0) {
						const float half = (s.y1 - s.y0) / 2;
						add(s.x0 + half, yc, s.ridge);
						add(s.x1 - half, yc, s.ridge);
					} else {
						const float half = (s.x1 - s.x0) / 2;
						add(xc, s.y0 + half, s.ridge);
						add(xc, s.y1 - half, s.ridge);
					}
					break;
				case RoofKind::GableX:
					add(s.x0, yc, s.ridge);
					add(s.x1, yc, s.ridge);
					break;
				case RoofKind::GableY:
					add(xc, s.y0, s.ridge);
					add(xc, s.y1, s.ridge);
					break;
				default:
					break;
			}
		}
		std::sort(points.begin(), points.begin() + n);

		/* The top and the bottom edge of the outline, which is the convex hull of the points. */
		auto turn = [](const Point &o, const Point &a, const Point &b) { return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first); };
		std::array<Point, 10> upper, lower;
		size_t n_upper = 0, n_lower = 0;
		for (size_t i = 0; i < n; i++) {
			while (n_upper >= 2 && turn(upper[n_upper - 2], upper[n_upper - 1], points[i]) <= 0) n_upper--;
			upper[n_upper++] = points[i];
			while (n_lower >= 2 && turn(lower[n_lower - 2], lower[n_lower - 1], points[i]) >= 0) n_lower--;
			lower[n_lower++] = points[i];
		}
		auto row_at = [](const std::array<Point, 10> &edge, size_t len, float col) {
			for (size_t i = 0; i + 1 < len; i++) {
				if (col > edge[i + 1].first) continue;
				const float span = edge[i + 1].first - edge[i].first;
				if (span <= 0) return edge[i + 1].second;
				return edge[i].second + (edge[i + 1].second - edge[i].second) * (col - edge[i].first) / span;
			}
			return edge[len - 1].second;
		};

		Result r;
		const int first = static_cast<int>(std::ceil(points[0].first - this->left - 0.5f));
		const int last = static_cast<int>(std::floor(points[n - 1].first - this->left - 0.5f));
		for (int x = first; x <= last; x++) {
			const float col = x + this->left + 0.5f;
			const int from = static_cast<int>(std::ceil(row_at(upper, n_upper, col) - this->top - 0.5f));
			const int to = static_cast<int>(std::floor(row_at(lower, n_lower, col) - this->top - 0.5f));
			if (to < from) continue;
			r.area += to - from + 1;
			if (x < 0 || x >= this->pic.width) continue;
			r.inside += this->Opaque(x, std::clamp(from, 0, this->pic.height), std::clamp(to + 1, 0, this->pic.height));
			r.above += this->Opaque(x, 0, std::clamp(from - 1, 0, this->pic.height));
		}
		r.score = 2 * r.inside - r.area;
		return r;
	}

private:
	const Picture &pic; ///< The sprite.
	int left; ///< Column of the left of the sprite, relative to where it is drawn.
	int top; ///< Row of the top of the sprite, relative to where it is drawn.
	float ax; ///< Column where the north corner of the box is drawn, relative to where the sprite is drawn.
	float ay; ///< Row where the north corner of the box is drawn, relative to where the sprite is drawn.
	std::vector<int> prefix; ///< Opaque pixels of each column above each row.

	/** Count the opaque pixels of column x from row y0 up to row y1. */
	int Opaque(int x, int y0, int y1) const
	{
		if (y1 <= y0) return 0;
		return this->prefix[static_cast<size_t>(y1) * this->pic.width + x] - this->prefix[static_cast<size_t>(y0) * this->pic.width + x];
	}
};

/** How the walls of a building tried on its sprite go. */
enum class WallSlope : uint8_t {
	Level, ///< All walls are as high, under a hip roof.
	AlongX, ///< The roof slopes down along the x axis.
	AlongY, ///< The roof slopes down along the y axis.
};

/**
 * Make a shape out of the numbers changed while fitting it.
 * @param v The footprint x0, y0, x1, y1, the two heights of the walls, and the height of the ridge.
 * @param slope How the walls go.
 * @return The shape.
 */
static ShapeTry MakeShapeTry(const std::array<float, 7> &v, WallSlope slope)
{
	ShapeTry s{v[0], v[1], v[2], v[3], {v[4], v[4], v[4], v[4]}, 0, RoofKind::Hip};
	switch (slope) {
		case WallSlope::Level: s.ridge = std::max(v[6], v[4]); break;
		case WallSlope::AlongX: s.walls = {v[4], v[4], v[5], v[5]}; break;
		case WallSlope::AlongY: s.walls = {v[5], v[4], v[4], v[5]}; break;
	}
	return s;
}

/**
 * Work out the shape of a building from its sprite, as the box the viewport sorts it by is often much larger
 * than the building. First a box is fitted to the outline of the sprite, so that as many of its pixels as
 * possible are opaque and as many opaque pixels as possible are in it. Then the heights of the walls come from
 * the vertical edges at the west and east corners, and the roof is the one that fits the top of the sprite
 * best. Things no shape fits well that have something standing above them, like a farm house under a big tree,
 * and plants, like a forest, become cut-outs.
 * @param box The sprite and its box.
 * @param station Whether it is a station building: those can be green in company colours, and often have a
 *                roof sloping one way.
 * @return The shape, relative to the north corner of the bottom of the box.
 */
const BuildingShape &Renderer::GetBuildingShape(const BoxSpriteToDraw &box, bool station)
{
	/* Where the viewport draws the sprite, relative to the north corner of the box, in ZOOM_BASE pixels. */
	const int ax_base = box.x - 2 * (box.ymin - box.xmin) * ZOOM_BASE;
	const int ay_base = box.y - (box.xmin + box.ymin - box.zmin) * ZOOM_BASE;
	const float bx = static_cast<float>(box.xmax + 1 - box.xmin);
	const float by = static_cast<float>(box.ymax + 1 - box.ymin);
	const float bz = static_cast<float>(box.zmax + 1 - box.zmin);
	auto key = std::make_tuple(box.image, box.pal, box.xmax - box.xmin, box.ymax - box.ymin, box.zmax - box.zmin, ax_base, ay_base, station);
	auto it = this->building_shapes.find(key);
	if (it != this->building_shapes.end()) return it->second;
	BuildingShape &shape = this->building_shapes[key];
	shape.x1 = bx;
	shape.y1 = by;
	shape.walls = {bz, bz, bz, bz};
	shape.band = std::min(2.0f, bz / 3);

	const GroundTexture &tex = this->GetBoxTexture(box.image, box.pal, false);
	const Picture &pic = tex.levels[0];
	if (pic.alpha.empty()) return shape;
	const float ax = static_cast<float>(ax_base) / ZOOM_BASE;
	const float ay = static_cast<float>(ay_base) / ZOOM_BASE;

	/* Bottom edge of each column, and the outer columns of each row; positions relative to where the sprite is drawn. */
	constexpr float NONE = -1e9f;
	std::vector<float> bottom(pic.width, NONE);
	std::vector<int> leftmost(pic.height, -1), rightmost(pic.height, -1);
	int opaque = 0, green = 0;
	for (int y = 0; y < pic.height; y++) {
		for (int x = 0; x < pic.width; x++) {
			if (pic.alpha[static_cast<size_t>(y) * pic.width + x] < 0.5f) continue;
			opaque++;
			const RGB &c = pic.pixels[static_cast<size_t>(y) * pic.width + x];
			if (c.g > c.r + 8 && c.g > c.b + 8) green++;
			bottom[x] = static_cast<float>(y + 1 + tex.top);
			if (leftmost[y] < 0) leftmost[y] = x;
			rightmost[y] = x;
		}
	}
	if (opaque == 0) return shape;
	auto column = [&](int x) { return x + tex.left + 0.5f; };
	shape.plants = !station && green * 2 > opaque;

	/* Footprints to start fitting from. Column 2 * (y - x) - ax and row x + y - z - ay are where the point (x, y, z) of the box is drawn. */
	std::vector<std::array<float, 4>> starts;
	auto add_start = [&](float c1, float c2, float w_col, float e_col) {
		/* The bottom edges of the walls are on row = col / 2 + c1 west of the south corner, and row = c2 - col / 2 east of it. */
		const float d = (c2 - c1 + ax) / 2;
		const float s = (c1 + c2) / 2 + ay;
		const float x1 = std::min((s - d) / 2, bx), y1 = std::min((s + d) / 2, by);
		const float x0 = std::max(y1 - (e_col + ax) / 2, 0.0f), y0 = std::max(x1 + (w_col + ax) / 2, 0.0f);
		if (x1 - x0 >= 1 && y1 - y0 >= 1) starts.push_back({x0, y0, x1, y1});
	};
	{
		/* One building, whose south corner is the lowest point of the sprite; follow the bottom edges from there. */
		const float s_row = *std::max_element(bottom.begin(), bottom.end());
		float sum = 0;
		int count = 0;
		for (int x = 0; x < pic.width; x++) {
			if (bottom[x] < s_row - 0.75f) continue;
			sum += column(x);
			count++;
		}
		const float s_col = sum / count;
		const float c1 = s_row - s_col / 2, c2 = s_row + s_col / 2;
		auto follow = [&](int dir) {
			float end = s_col;
			int misses = 0;
			for (int x = static_cast<int>(std::floor(s_col)) - tex.left; x >= 0 && x < pic.width; x += dir) {
				const float line = column(x) < s_col ? column(x) / 2 + c1 : c2 - column(x) / 2;
				if (bottom[x] != NONE && std::abs(bottom[x] - line) <= 1.5f) {
					end = static_cast<float>(x + tex.left + (dir > 0 ? 1 : 0));
					misses = 0;
				} else if (++misses > 3) {
					break;
				}
			}
			return end;
		};
		add_start(c1, c2, follow(-1), follow(1));
	}
	{
		/* The lowest lines that no two neighbouring columns reach below, for a sprite whose bottom is cut off as the
		 * ground sprite draws it; the corners are the outermost columns that reach down to the lines. */
		auto lowest_line = [&](float slope) {
			float lowest = NONE;
			for (int x = 0; x + 1 < pic.width; x++) {
				if (bottom[x] == NONE || bottom[x + 1] == NONE) continue;
				lowest = std::max(lowest, std::min(bottom[x] - slope * column(x), bottom[x + 1] - slope * column(x + 1)));
			}
			return lowest;
		};
		const float c1 = lowest_line(0.5f), c2 = lowest_line(-0.5f);
		if (c1 != NONE && c2 != NONE) {
			auto near_line = [&](int x) {
				if (x < 0 || x >= pic.width || bottom[x] == NONE) return false;
				const float col = column(x);
				return bottom[x] >= (col < c2 - c1 ? col / 2 + c1 : c2 - col / 2) - 3;
			};
			float w_col = c2 - c1, e_col = c2 - c1;
			for (int x = 0; x < pic.width; x++) {
				if (!near_line(x) || !near_line(x + 1)) continue;
				w_col = static_cast<float>(x + tex.left);
				break;
			}
			for (int x = pic.width - 1; x >= 0; x--) {
				if (!near_line(x) || !near_line(x - 1)) continue;
				e_col = static_cast<float>(x + tex.left + 1);
				break;
			}
			add_start(c1, c2, w_col, e_col);
		}
	}
	starts.push_back({0, 0, bx, by});

	/* Fit a box, with a hip roof or a roof sloping one way, by changing one number at a time while that helps. */
	SpriteFit fit(tex, ax, ay);
	auto valid = [&](const std::array<float, 7> &v) {
		return v[0] >= 0 && v[1] >= 0 && v[2] <= bx && v[3] <= by && v[2] - v[0] >= 1 && v[3] - v[1] >= 1 && v[4] >= 1 && v[5] >= 1 &&
				v[4] <= 1.5f * bz + 4 && v[6] <= v[4] + 1.5f * std::max(v[2] - v[0], v[3] - v[1]);
	};
	auto descend = [&](std::array<float, 7> v, WallSlope slope) {
		int best = fit.Measure(MakeShapeTry(v, slope)).score;
		auto attempt = [&](const std::array<float, 7> &w) {
			if (!valid(w)) return false;
			const int score = fit.Measure(MakeShapeTry(w, slope)).score;
			if (score <= best) return false;
			best = score;
			v = w;
			return true;
		};
		for (float step = 2; step >= 0.25f;) {
			bool improved = false;
			for (size_t i = 0; i < v.size(); i++) {
				if (i == (slope == WallSlope::Level ? 5 : 6)) continue;
				for (float d : {step, -step}) {
					std::array<float, 7> w = v;
					w[i] += d;
					if (i == 4 && slope == WallSlope::Level) w[6] = std::max(w[6], w[4]);
					if (attempt(w)) improved = true;
				}
			}
			/* Move the whole footprint. */
			for (const std::pair<float, float> &move : {std::pair{step, 0.0f}, std::pair{-step, 0.0f}, std::pair{0.0f, step}, std::pair{0.0f, -step}}) {
				std::array<float, 7> w = v;
				w[0] += move.first;
				w[2] += move.first;
				w[1] += move.second;
				w[3] += move.second;
				if (attempt(w)) improved = true;
			}
			if (!improved) step /= 2;
		}
		return std::make_pair(best, v);
	};
	int best_score = std::numeric_limits<int>::min();
	std::array<float, 7> best_v{};
	WallSlope best_slope = WallSlope::Level;
	for (const auto &start : starts) {
		for (float h : {bz / 3, bz * 2 / 3}) {
			for (WallSlope slope : {WallSlope::Level, WallSlope::AlongX, WallSlope::AlongY}) {
				if (slope != WallSlope::Level && !station) continue;
				auto found = descend({start[0], start[1], start[2], start[3], h, h, h}, slope);
				if (found.first <= best_score) continue;
				best_score = found.first;
				best_v = found.second;
				best_slope = slope;
			}
		}
	}
	const ShapeTry fitted = MakeShapeTry(best_v, best_slope);
	const float x0 = fitted.x0, y0 = fitted.y0, x1 = fitted.x1, y1 = fitted.y1;
	const float box_overlap = fit.Measure(fitted).Overlap(opaque);

	/* The top of a wall at a corner is the top of the vertical edge there: the longest run of rows whose outer
	 * pixel is in the same column, at the corner or a bit inside. That skips a platform the building stands on, and its roof. */
	auto corner_top = [&](float col, bool west, float cx, float cy) -> std::optional<float> {
		int best = 0, best_row = 0, run = 0, run_x = -1;
		for (int y = pic.height - 1; y >= 0; y--) {
			int x = west ? leftmost[y] : rightmost[y];
			float edge = static_cast<float>(x + tex.left + (west ? 0 : 1));
			if (x < 0 || (west ? edge < col - 0.5f || edge > col + 4.5f : edge > col + 0.5f || edge < col - 4.5f)) {
				run = 0;
				run_x = -1;
				continue;
			}
			run = (x == run_x) ? run + 1 : 1;
			run_x = x;
			if (run >= best) {
				best = run;
				best_row = y + tex.top;
			}
		}
		if (best < 2) return std::nullopt;
		return std::max(cx + cy - best_row - ay, 1.0f);
	};
	std::optional<float> west = corner_top(2 * (y0 - x1) - ax, true, x1, y0);
	std::optional<float> east = corner_top(2 * (y1 - x0) - ax, false, x0, y1);
	ShapeTry result = fitted;
	result.ridge = 0;
	result.roof = RoofKind::Flat;
	const bool differ = west.has_value() && east.has_value() && std::abs(*west - *east) > 2 + std::min(*west, *east) / 3;
	if (best_slope == WallSlope::Level || differ) {
		if (west.has_value() && east.has_value()) {
			const float low = std::min(*west, *east), high = std::max(*west, *east);
			if (!differ) {
				result.walls = {low, low, low, low};
			} else if (best_slope == WallSlope::AlongX) {
				result.walls = {*west, *west, *east, *east};
			} else if (best_slope == WallSlope::AlongY) {
				result.walls = {*east, *west, *west, *east};
			} else {
				/* Not a simple house; the walls reach up to the top of both corners and the sprite shows the rest. */
				result.walls = {high, high, high, high};
			}
		} else if (west.has_value() || east.has_value()) {
			const float h = west.value_or(east.value_or(bz));
			result.walls = {h, h, h, h};
		}
	}

	/* The roof that fits the top of the sprite best, on level walls; not steeper than roofs are, and only when
	 * it fits clearly better than a flat one, as that is no roof but a parapet or something on top. */
	const float eave = result.walls[0];
	if (std::all_of(result.walls.begin(), result.walls.end(), [eave](float h) { return h == eave; })) {
		const int flat = fit.Measure(result).score;
		int best_roof = flat + opaque / 50;
		ShapeTry roofed = result;
		for (RoofKind kind : {RoofKind::Hip, RoofKind::GableX, RoofKind::GableY}) {
			roofed.roof = kind;
			const float half_span = (kind == RoofKind::GableX ? y1 - y0 : kind == RoofKind::GableY ? x1 - x0 : std::min(x1 - x0, y1 - y0)) / 2;
			for (roofed.ridge = eave + MIN_ROOF_HEIGHT / 2; roofed.ridge <= eave + 2.5f * half_span; roofed.ridge += 0.5f) {
				const int score = fit.Measure(roofed).score;
				if (score <= best_roof) continue;
				best_roof = score;
				result.roof = kind;
				result.ridge = roofed.ridge;
			}
		}
	}
	SpriteFit::Result measured = fit.Measure(result);
	if (measured.Overlap(opaque) < box_overlap - 0.1f) {
		/* The walls and the roof do not fit; keep the box. */
		result = fitted;
		result.ridge = 0;
		result.roof = RoofKind::Flat;
		measured = fit.Measure(result);
	}

	/* Silos and chimneys are no buildings, and look like towers as boxes. */
	const float thin = std::min(x1 - x0, y1 - y0);
	const bool slender = !station && thin < 6 && result.walls[0] > 7 * thin;
	shape.cutout = shape.plants || slender || (measured.Overlap(opaque) < 0.6f && measured.above > opaque / 5);
	if (shape.cutout) {
		/* A cut-out stands on the middle of the box. Plants have the bottom of the sprite on the ground, as trees
		 * stand all over the tile; other things the ground in the middle of the box, so the back does not float. */
		shape.foot_col = by - bx - ax;
		shape.foot_row = shape.plants ? *std::max_element(bottom.begin(), bottom.end()) : (bx + by) / 2 - ay;
		return shape;
	}

	shape.x0 = x0;
	shape.y0 = y0;
	shape.x1 = x1;
	shape.y1 = y1;
	shape.walls = result.walls;
	shape.ridge = result.ridge;
	shape.roof = result.roof;

	/* The ground sprite often draws the lowest part of the walls; then the sprite stops above the bottom edges. */
	std::vector<float> gaps;
	const float s_col = 2 * (y1 - x1) - ax, s_row = x1 + y1 - ay;
	for (int x = 0; x < pic.width; x++) {
		const float col = column(x);
		if (bottom[x] == NONE || col < 2 * (y0 - x1) - ax || col > 2 * (y1 - x0) - ax) continue;
		gaps.push_back(s_row - std::abs(col - s_col) / 2 - bottom[x]);
	}
	float gap = 0;
	if (!gaps.empty()) {
		std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
		gap = std::clamp(gaps[gaps.size() / 2], 0.0f, *std::min_element(shape.walls.begin(), shape.walls.end()) / 3);
	}
	shape.band = std::max(std::min(2.0f, shape.walls[0] / 3), gap + 0.5f);
	return shape;
}

/**
 * Add a building as a real 3D shape: walls up to the eaves, and a roof up to the top of its sprite, see
 * #GetBuildingShape. Every point of the walls and the roof gets the pixel the game draws there, so the
 * painted roof lands on the roof. Things that are no box become cut-outs.
 * @param box The sprite and its box.
 * @param height_scale How much the heights are scaled, see #BUILDING_HEIGHT.
 * @param station Whether it is a station building, see #GetBuildingShape.
 */
void Renderer::AddBuilding(const BoxSpriteToDraw &box, float height_scale, bool station)
{
	const BuildingShape &shape = this->GetBuildingShape(box, station);
	const float bx = static_cast<float>(box.xmin), by = static_cast<float>(box.ymin);
	const float z0 = static_cast<float>(box.zmin);
	Placement pl;
	pl.base = z0;
	pl.height_scale = height_scale;
	pl.sx = static_cast<float>(box.x) / ZOOM_BASE;
	pl.sy = static_cast<float>(box.y) / ZOOM_BASE;

	if (shape.cutout) {
		const GroundTexture &tex = this->GetBoxTexture(box.image, box.pal, false);
		Vec3 foot = {(bx + box.xmax + 1) / 2, (by + box.ymax + 1) / 2, z0};
		if (shape.plants) {
			this->AddCutout(tex, foot, shape.foot_col, shape.foot_row, CUTOUT_UNITS, CUTOUT_UNITS);
		} else {
			this->AddCutout(tex, foot, shape.foot_col, shape.foot_row, ISO_UNITS, height_scale);
		}
		return;
	}

	const GroundTexture &tex = this->GetBoxTexture(box.image, box.pal);
	if (tex.levels[0].alpha.empty()) return;
	const float x0 = bx + shape.x0, x1 = bx + shape.x1;
	const float y0 = by + shape.y0, y1 = by + shape.y1;
	std::array<float, 4> walls;
	for (int i = 0; i < 4; i++) walls[i] = z0 + shape.walls[i];
	const float eave = walls[0], ridge = z0 + shape.ridge;

	this->AddSpriteWalls(tex, pl, {x0, y0, z0}, {x1, y1, z0}, walls, shape.band, true);
	if (ridge <= eave) {
		/* A flat roof, or one sloping across the building. */
		std::array<Vec3, 4> q = {Vec3{x0, y0, walls[0]}, Vec3{x1, y0, walls[1]}, Vec3{x1, y1, walls[2]}, Vec3{x0, y1, walls[3]}};
		bool flat = walls[1] == eave && walls[2] == eave && walls[3] == eave;
		this->AddSpriteFace(tex, pl, q, {0, 0, 1}, [](const Vec3 &p) { return p; }, flat);
		return;
	}

	const float xc = (x0 + x1) / 2, yc = (y0 + y1) / 2;
	auto same = [](const Vec3 &p) { return p; };
	auto mirror_x = [xc](Vec3 p) { p.x = 2 * xc - p.x; return p; };
	auto mirror_y = [yc](Vec3 p) { p.y = 2 * yc - p.y; return p; };
	if (shape.roof == RoofKind::GableX) {
		/* Gable roof with its ridge along the x axis; the gables are walls. */
		Vec3 r0 = {x0, yc, ridge}, r1 = {x1, yc, ridge};
		std::array<Vec3, 4> south = {Vec3{x1, y1, eave}, Vec3{x0, y1, eave}, r0, r1};
		std::array<Vec3, 4> north = {Vec3{x0, y0, eave}, Vec3{x1, y0, eave}, r1, r0};
		std::array<Vec3, 3> west_end = {Vec3{x1, y0, eave}, Vec3{x1, y1, eave}, r1};
		std::array<Vec3, 3> east_end = {Vec3{x0, y1, eave}, Vec3{x0, y0, eave}, r0};
		this->AddSpriteFace(tex, pl, south, {0, 1, 1}, same, false);
		this->AddSpriteFace(tex, pl, north, {0, -1, 1}, mirror_y, false);
		this->AddSpriteFace(tex, pl, west_end, {1, 0, 0}, same, true);
		this->AddSpriteFace(tex, pl, east_end, {-1, 0, 0}, mirror_x, true);
	} else if (shape.roof == RoofKind::GableY) {
		/* Gable roof with its ridge along the y axis. */
		Vec3 r0 = {xc, y0, ridge}, r1 = {xc, y1, ridge};
		std::array<Vec3, 4> south = {Vec3{x1, y0, eave}, Vec3{x1, y1, eave}, r1, r0};
		std::array<Vec3, 4> north = {Vec3{x0, y1, eave}, Vec3{x0, y0, eave}, r0, r1};
		std::array<Vec3, 3> east_end = {Vec3{x1, y1, eave}, Vec3{x0, y1, eave}, r1};
		std::array<Vec3, 3> west_end = {Vec3{x0, y0, eave}, Vec3{x1, y0, eave}, r0};
		this->AddSpriteFace(tex, pl, south, {1, 0, 1}, same, false);
		this->AddSpriteFace(tex, pl, north, {-1, 0, 1}, mirror_x, false);
		this->AddSpriteFace(tex, pl, east_end, {0, 1, 0}, same, true);
		this->AddSpriteFace(tex, pl, west_end, {0, -1, 0}, mirror_y, true);
	} else if (x1 - x0 >= y1 - y0) {
		/* Hip roof, with its ridge along the longer side of the building; a pyramid on a square one. */
		float half = (y1 - y0) / 2;
		Vec3 r0 = {x0 + half, yc, ridge}, r1 = {x1 - half, yc, ridge};
		std::array<Vec3, 4> south = {Vec3{x1, y1, eave}, Vec3{x0, y1, eave}, r0, r1};
		std::array<Vec3, 4> north = {Vec3{x0, y0, eave}, Vec3{x1, y0, eave}, r1, r0};
		std::array<Vec3, 3> west_end = {Vec3{x1, y0, eave}, Vec3{x1, y1, eave}, r1};
		std::array<Vec3, 3> east_end = {Vec3{x0, y1, eave}, Vec3{x0, y0, eave}, r0};
		this->AddSpriteFace(tex, pl, south, {0, 1, 1}, same, false);
		this->AddSpriteFace(tex, pl, north, {0, -1, 1}, mirror_y, false);
		this->AddSpriteFace(tex, pl, west_end, {1, 0, 1}, same, false);
		this->AddSpriteFace(tex, pl, east_end, {-1, 0, 1}, mirror_x, false);
	} else {
		float half = (x1 - x0) / 2;
		Vec3 r0 = {xc, y0 + half, ridge}, r1 = {xc, y1 - half, ridge};
		std::array<Vec3, 4> south = {Vec3{x1, y0, eave}, Vec3{x1, y1, eave}, r1, r0};
		std::array<Vec3, 4> north = {Vec3{x0, y1, eave}, Vec3{x0, y0, eave}, r0, r1};
		std::array<Vec3, 3> east_end = {Vec3{x1, y1, eave}, Vec3{x0, y1, eave}, r1};
		std::array<Vec3, 3> west_end = {Vec3{x0, y0, eave}, Vec3{x1, y0, eave}, r0};
		this->AddSpriteFace(tex, pl, south, {1, 0, 1}, same, false);
		this->AddSpriteFace(tex, pl, north, {-1, 0, 1}, mirror_x, false);
		this->AddSpriteFace(tex, pl, east_end, {0, 1, 1}, same, false);
		this->AddSpriteFace(tex, pl, west_end, {0, -1, 1}, mirror_y, false);
	}
}

/**
 * Add a cut-out of a sprite facing the camera, like a tree.
 * @param tex The texture of the sprite, without widening, see #GetBoxTexture.
 * @param foot The point on the ground the cut-out stands on.
 * @param foot_col Column of the sprite at that point, relative to where the sprite is drawn.
 * @param foot_row Row of the sprite at that point, relative to where the sprite is drawn.
 * @param scale_h Width of a pixel of the sprite, in world units.
 * @param scale_v Height of a pixel of the sprite, in world units.
 */
void Renderer::AddCutout(const GroundTexture &tex, const Vec3 &foot, float foot_col, float foot_row, float scale_h, float scale_v)
{
	const Picture &pic = tex.levels[0];
	if (pic.alpha.empty()) return;

	/* Texture coordinates are pixels relative to where the sprite is drawn. */
	float left = static_cast<float>(tex.left), right = static_cast<float>(tex.left + pic.width);
	float top = static_cast<float>(tex.top), bottom = static_cast<float>(tex.top + pic.height);
	auto corner = [&](float col, float row) {
		ClipVertex v;
		Vec3 p = foot + this->cam.right * ((col - foot_col) * scale_h) + Vec3{0, 0, (foot_row - row) * scale_v};
		v.c = this->cam.ToCamera(p);
		v.a = {col, row, col * scale_h, row * scale_v};
		return v;
	};
	std::array<ClipVertex, 4> v = {corner(left, top), corner(right, top), corner(right, bottom), corner(left, bottom)};
	Paint paint = {&tex, {}, 1.0f, 0, 1.0f, true};
	this->AddTriangle({v[0], v[1], v[2]}, paint);
	this->AddTriangle({v[0], v[2], v[3]}, paint);
}

/**
 * Add a thing standing on a tile as a cut-out facing the camera, like trees and signals.
 * The viewport draws the sprite where the thing stands.
 * @param box The sprite and its box.
 */
void Renderer::AddBillboard(const BoxSpriteToDraw &box)
{
	float sx = static_cast<float>(box.x) / ZOOM_BASE;
	float sy = static_cast<float>(box.y) / ZOOM_BASE;
	float z = static_cast<float>(box.zmin);
	Vec3 foot = {(sy + z - sx / 2) / 2, (sy + z + sx / 2) / 2, z};
	this->AddCutout(this->GetBoxTexture(box.image, box.pal, false), foot, 0, 0, CUTOUT_UNITS, CUTOUT_UNITS);
}

/** Add the trains and road vehicles around the camera, as boxes dressed in their sprites. */
void Renderer::AddVehicles()
{
	/* Direction each way a vehicle can face goes to, in world coordinates. */
	static const std::array<std::pair<float, float>, DIR_END> headings = {{
		{-1, -1}, {-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1},
	}};

	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VEH_TRAIN && v->type != VEH_ROAD) continue;
		if (v->vehstatus.Test(VehState::Hidden)) continue;
		float dx = v->x_pos - this->cam.pos.x;
		float dy = v->y_pos - this->cam.pos.y;
		if (std::hypot(dx, dy) > BOX_DIST) continue;
		if (dx * this->cam.fwd.x + dy * this->cam.fwd.y < -2 * TILE) continue;

		/* The sprite of the vehicle going to the south west, along the x axis. */
		VehicleSpriteSeq seq;
		v->GetImage(DIR_SW, EIT_ON_MAP, &seq);
		if (!seq.IsValid()) continue;
		PaletteID pal = seq.seq[0].pal != 0 ? seq.seq[0].pal : GetVehiclePalette(v);
		const GroundTexture &tex = this->GetBoxTexture(seq.seq[0].sprite, pal);
		const Picture &pic = tex.levels[0];
		if (pic.alpha.empty()) continue;
		std::optional<int> top = TopRow(tex);
		if (!top.has_value()) continue;

		/* The box: its length from the game, its width and height from the sprite, which is 2 (length + width) wide. */
		uint8_t veh_length = (v->type == VEH_TRAIN) ? Train::From(v)->gcache.cached_veh_length : RoadVehicle::From(v)->gcache.cached_veh_length;
		float length = static_cast<float>(veh_length);
		float width = std::clamp(pic.width / 2.0f - length, 1.5f, std::max(length, 1.5f));
		float height = std::clamp(-*top - (length + width) / 2, 2.0f, 40.0f);

		auto [hx, hy] = headings[v->direction];
		float angle = std::atan2(hy, hx);
		Placement pl;
		pl.origin = {static_cast<float>(v->x_pos), static_cast<float>(v->y_pos), static_cast<float>(v->z_pos)};
		pl.cos = std::cos(angle);
		pl.sin = std::sin(angle);
		pl.height_scale = BUILDING_HEIGHT;

		/* Leave out the vehicle the camera is on. */
		Vec3 d = this->cam.pos - pl.origin;
		float along = d.x * pl.cos + d.y * pl.sin;
		float across = -d.x * pl.sin + d.y * pl.cos;
		if (std::abs(along) <= length / 2 + 1 && std::abs(across) <= width / 2 + 1) continue;

		this->AddSpriteBox(tex, pl, {-length / 2, -width / 2, 0}, {length / 2, width / 2, height}, false, true);
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
 * Put the finished picture on the screen, scaled to the view when it is smaller.
 * @param r Where on the screen.
 */
void Renderer::Output(const Rect &r)
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	const DrawPixelInfo *dpi = _cur_dpi;

	/* Where the pixels of the view are in the picture, which may be smaller: the first of the two columns
	 * (rows) of the picture to mix, and how much of the second one, out of 256. Mixed smoothly, so it does
	 * not look blocky. */
	auto map_axis = [](int view_size, int pic_size, std::vector<int> &first, std::vector<uint32_t> &weight) {
		first.resize(view_size);
		weight.resize(view_size);
		float step = static_cast<float>(pic_size) / view_size;
		for (int i = 0; i < view_size; i++) {
			float p = std::clamp((i + 0.5f) * step - 0.5f, 0.0f, static_cast<float>(pic_size - 1));
			first[i] = std::min(static_cast<int>(p), std::max(pic_size - 2, 0));
			weight[i] = pic_size > 1 ? static_cast<uint32_t>((p - first[i]) * 256 + 0.5f) : 0;
		}
	};
	std::vector<int> col, row;
	std::vector<uint32_t> col_weight, row_weight;
	map_axis(this->view_width, this->width, col, col_weight);
	map_axis(this->view_height, this->height, row, row_weight);
	bool scaled = this->width != this->view_width || this->height != this->view_height;
	int next_col = this->width > 1 ? 1 : 0;
	int next_row = this->height > 1 ? this->width : 0;

	/* Mix two colours; red and blue are mixed at once, as they are 16 bits apart. */
	auto mix = [](uint32_t a, uint32_t b, uint32_t w) -> uint32_t {
		uint32_t rb = ((a & 0xFF00FF) * (256 - w) + (b & 0xFF00FF) * w) >> 8;
		uint32_t g = ((a & 0x00FF00) * (256 - w) + (b & 0x00FF00) * w) >> 8;
		return (rb & 0xFF00FF) | (g & 0x00FF00);
	};
	/* Get the colours of columns [x0, x1] of a row of the view. */
	std::vector<std::vector<uint32_t>> rows(MAX_THREADS);
	auto get_row = [&](int y, int x0, int x1, std::vector<uint32_t> &out) -> const uint32_t * {
		if (!scaled) return &this->frame[static_cast<size_t>(y) * this->width + x0];
		out.resize(x1 - x0 + 1);
		const uint32_t *top = &this->frame[static_cast<size_t>(row[y]) * this->width];
		const uint32_t *bottom = top + next_row;
		uint32_t fy = row_weight[y];
		for (int x = x0; x <= x1; x++) {
			int c = col[x];
			uint32_t fx = col_weight[x];
			out[x - x0] = mix(mix(top[c], top[c + next_col], fx), mix(bottom[c], bottom[c + next_col], fx), fy);
		}
		return out.data();
	};

	if (blitter->GetScreenDepth() == 32) {
		/* All 32bpp blitters keep one Colour per pixel on the screen. Clear the area first,
		 * so the blitter's animation buffer (if any) leaves these pixels alone, then copy
		 * the true colour picture in. */
		GfxFillRect(r, PixelColour{0});
		int left = std::max(r.left, dpi->left);
		int right = std::min(r.right, dpi->left + dpi->width - 1);
		int top = std::max(r.top, dpi->top);
		int bottom = std::min(r.bottom, dpi->top + dpi->height - 1);
		if (left > right || top > bottom) return;
		auto copy_rows = [&](int part, int parts) {
			for (int y = top + part; y <= bottom; y += parts) {
				Colour *dst = static_cast<Colour *>(blitter->MoveTo(dpi->dst_ptr, left - dpi->left, y - dpi->top));
				const uint32_t *src = get_row(y - r.top, left - r.left, right - r.left, rows[part]);
				for (int x = left; x <= right; x++, dst++, src++) {
					*dst = Colour(GB(*src, 16, 8), GB(*src, 8, 8), GB(*src, 0, 8));
				}
			}
		};
		/* Small parts, e.g. under a tooltip, are not worth starting threads for. */
		if ((right - left + 1) * (bottom - top + 1) < 65536) {
			copy_rows(0, 1);
		} else {
			RunOnThreads(copy_rows);
		}
		return;
	}

	/* Other blitters: let the blitter encode the picture as a sprite of palette colours. */
	SpriteLoader::SpriteCollection collection;
	SpriteLoader::Sprite &sprite = collection[ZoomLevel::Min];
	sprite.AllocateData(ZoomLevel::Min, static_cast<size_t>(this->view_width) * this->view_height);
	sprite.width = this->view_width;
	sprite.height = this->view_height;
	sprite.x_offs = 0;
	sprite.y_offs = 0;
	sprite.colours = SpriteComponent::Palette;

	/* Ordered dither, so smooth gradients don't band where colours are rounded to the palette. */
	static constexpr std::array<std::array<int, 4>, 4> BAYER = {{{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}}};
	for (int y = 0; y < this->view_height; y++) {
		const uint32_t *src = get_row(y, 0, this->view_width - 1, rows[0]);
		for (int x = 0; x < this->view_width; x++) {
			int d = (BAYER[y & 3][x & 3] - 8) * 3 / 4;
			uint32_t c = src[x];
			auto channel = [&](uint8_t start) { return static_cast<uint8_t>(std::clamp<int>(GB(c, start, 8) + d, 0, 255)); };
			SpriteLoader::CommonPixel &p = sprite.data[static_cast<size_t>(y) * this->view_width + x];
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
 * @param budget_ms Time making a picture may take, see #DrawCabView3D.
 */
void Renderer::Draw(const Rect &r, const CabViewCamera &camera, uint32_t frame_id, float budget_ms)
{
	if (camera.dark) {
		GfxFillRect(r, PC_BLACK);
		return;
	}
	if (r.Width() <= 0 || r.Height() <= 0) return;

	/* The window is often drawn in several parts; draw the picture only once. */
	if (this->frame_valid && this->frame_id == frame_id && this->frame_camera == camera && this->view_width == r.Width() && this->view_height == r.Height()) {
		this->Output(r);
		return;
	}

	auto start = std::chrono::steady_clock::now();
	if (this->textures.size() >= MAX_TEXTURES) this->textures.clear();
	if (this->box_textures.size() >= MAX_TEXTURES) this->box_textures.clear();
	size_t old_textures = this->textures.size() + this->box_textures.size();

	this->view_width = r.Width();
	this->view_height = r.Height();
	this->width = std::max(1, static_cast<int>(std::lround(r.Width() * this->scale)));
	this->height = std::max(1, static_cast<int>(std::lround(r.Height() * this->scale)));
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
	this->AddVehicles();

	/* Fill the triangles in stripes of rows, on several threads. */
	auto prepared = std::chrono::steady_clock::now();
	int bands = RunOnThreads([this](int stripe, int stripes) { this->Rasterise(stripe, stripes); });

	this->frame_valid = true;
	this->frame_id = frame_id;
	this->frame_camera = camera;

	auto rendered = std::chrono::steady_clock::now();
	this->Output(r);
	auto done = std::chrono::steady_clock::now();

	/* Pictures that had to put together many new textures, like the first one, take much longer than usual; leave them out. */
	if (this->textures.size() + this->box_textures.size() < old_textures + MAX_NEW_TEXTURES_TIMED) {
		float fixed = std::chrono::duration<float, std::milli>((prepared - start) + (done - rendered)).count();
		float area = static_cast<float>(this->width * this->height) / (this->view_width * this->view_height);
		float fill = std::chrono::duration<float, std::milli>(rendered - prepared).count() / area;
		if (this->fill_ms <= 0) {
			this->fixed_ms = fixed;
			this->fill_ms = fill;
		} else {
			this->fixed_ms += (std::min(fixed, 3 * this->fixed_ms) - this->fixed_ms) * 0.1f;
			this->fill_ms += (std::min(fill, 3 * this->fill_ms) - this->fill_ms) * 0.1f;
		}
	}
	this->ChooseScale(budget_ms);

	Debug(misc, 3, "[CabView3D] camera {:.1f},{:.1f},{:.1f} yaw {:.2f}", camera.x, camera.y, camera.z, camera.yaw);
	Debug(misc, 3, "[CabView3D] {}x{} for {}x{}, {} tiles, {} triangles, prepare {} us, fill {} us ({} threads), output {} us, {} textures",
		this->width, this->height, this->view_width, this->view_height, visible.size(), this->triangles.size(),
		std::chrono::duration_cast<std::chrono::microseconds>(prepared - start).count(),
		std::chrono::duration_cast<std::chrono::microseconds>(rendered - prepared).count(), bands,
		std::chrono::duration_cast<std::chrono::microseconds>(done - rendered).count(), this->textures.size());
}

/**
 * Make the picture smaller when making it takes longer than there is time for, so the view stays smooth,
 * and larger again when there is time. The picture is scaled to the view smoothly, and the graphics of
 * the game are smooth too, so a somewhat smaller picture hardly shows.
 * @param budget_ms Time there is for making a picture, in milliseconds.
 */
void Renderer::ChooseScale(float budget_ms)
{
	if (++this->pictures_at_scale < PICTURES_PER_SCALE || budget_ms <= 0 || this->fill_ms <= 0) return;

	/* The time to fill the picture goes with its area. Steps of 5%, so small changes in time don't change the size. */
	float fit = std::sqrt(std::max(budget_ms * 0.9f - this->fixed_ms, 0.0f) / this->fill_ms);
	float scale = std::clamp(std::floor(fit * 20) / 20, CAB_VIEW_3D_MIN_SCALE, 1.0f);
	/* Only grow when there is clearly time for it, so the size does not go back and forth. */
	if (scale > this->scale && this->GetFrameTime(scale) > budget_ms * 0.8f) scale = this->scale;
	if (scale == this->scale) return;

	Debug(misc, 3, "[CabView3D] picture at {:.0f}% of the view: there is {:.1f} ms, drawing at full size takes {:.1f} ms", scale * 100, budget_ms, this->GetFrameTime(1.0f));
	this->scale = scale;
	this->pictures_at_scale = 0;
}

/** The one renderer; all cab views draw one after the other. */
Renderer _renderer;

} // namespace

/**
 * Draw the 3D view from the cab.
 * @param r Where on the screen.
 * @param camera Where the camera is.
 * @param frame_id Number of the picture; the picture is only drawn again when it or the camera changed.
 * @param budget_ms Time (in milliseconds) making a picture may take; when it takes longer, the picture is made smaller.
 */
void DrawCabView3D(const Rect &r, const CabViewCamera &camera, uint32_t frame_id, float budget_ms)
{
	_renderer.Draw(r, camera, frame_id, budget_ms);
}

/**
 * Get how long drawing a new picture of the 3D view from the cab takes, on average.
 * @param scale Size of the picture compared to the view; the picture is made smaller when there is not enough time.
 * @return The time in milliseconds; 0 before the first picture.
 */
float GetCabView3DFrameTime(float scale)
{
	return _renderer.GetFrameTime(scale);
}

/** Forget all loaded graphics and buffers, e.g. because the graphics may have changed. */
void ResetCabView3D()
{
	_renderer.Reset();
}
