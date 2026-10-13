/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cab_view_3d.cpp Software rendered 3D view from the cab of a train, a ship or an aircraft.
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
#include "bridge.h"
#include "bridge_map.h"
#include "tunnel_map.h"
#include "tunnelbridge_map.h"
#include "clear_map.h"
#include "water_map.h"
#include "rail.h"
#include "newgrf_railtype.h"
#include "road.h"
#include "newgrf_roadstop.h"
#include "company_func.h"
#include "settings_type.h"
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
#include "ship.h"
#include "aircraft.h"

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
constexpr float MAX_FOG_LIFT = 15.0f * TILE; ///< How much further away the haze starts at most when looking down from an aircraft, see #CabViewCamera::altitude.
constexpr float AIRCRAFT_UNITS = 0.4f; ///< Size of a sprite pixel of an aircraft, in world units.
constexpr float SHIP_UNITS = 0.4f; ///< Size of a sprite pixel of a ship, in world units.
constexpr float SHIP_MIN_UPRIGHT = 0.4f; ///< How much the sprite of a ship seen from straight ahead or behind is shrunk upwards, see #Renderer::AddShip.
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
constexpr float PLATFORM_HEIGHT = 0.5f; ///< Heights of the platforms of stations in the 3D view, compared to their boxes; from the cab, real platforms reach well below the eye.
constexpr float BUILDING_HEIGHT = 0.6f; ///< Heights of buildings and vehicles in the 3D view, compared to their heights in the sprites; the isometric view makes them look tall.
constexpr float ROAD_VEHICLE_HEIGHT = 1.6f; ///< Heights and widths of road vehicles in the 3D view, compared to those in their sprites; seen from the cab of a bus, the roof of another bus is above the eye.
constexpr float ROAD_VEHICLE_MAX_WIDTH = 4.0f; ///< Widest a road vehicle is drawn in the 3D view, so it stays in its lane.
constexpr float CUTOUT_UNITS = 0.28f; ///< Size of a sprite pixel of a cut-out, like a tree, in world units.
constexpr float ISO_UNITS = 0.3536f; ///< Width of a sprite pixel across the view, in world units; the isometric view draws a unit as 2 * sqrt(2) pixels.
constexpr float MIN_ROOF_HEIGHT = 3.0f; ///< A building is given a roof when its sprite reaches at least this much above its walls.
constexpr float SIGNAL_HEIGHT = 6.0f; ///< Height of the top of a signal above the track.
constexpr float SIGNAL_HEAD_HEIGHT = 2.4f; ///< Height of the head of a signal, with its lamps.
constexpr float SIGNAL_HEAD_WIDTH = 1.1f; ///< Width of the head of a signal.
constexpr float SIGNAL_LAMP_SIZE = 0.7f; ///< Size of a lamp of a signal; larger than real, so it can be seen from afar.
constexpr float CROSSING_POST_HEIGHT = 5.5f; ///< Height of the posts of the lights of a level crossing.
constexpr float CROSSING_POST_ALONG = 2.2f; ///< How far the posts of a level crossing stand from the edges of the tile, along the road.
constexpr float CROSSING_POST_ACROSS = 1.6f; ///< How far the posts of a level crossing stand from the sides of the tile, next to the road.
constexpr float DECK_DEPTH = 2.0f; ///< Thickness of the deck of a bridge, below the track.
constexpr float PILLAR_SIZE = 2.4f; ///< Width of a pillar under a bridge.
constexpr float AQUEDUCT_WALL = 1.2f; ///< Thickness of the walls along the water of an aqueduct.
constexpr float AQUEDUCT_WALL_HEIGHT = 2.0f; ///< Height of the walls along the water of an aqueduct.
constexpr float LOCK_WALL = 1.0f; ///< Thickness of the walls along the sides of a lock.
constexpr float LOCK_WALL_TOP = 1.0f; ///< How far the walls of a lock reach above the land at its upper end.
constexpr float LOCK_GATE = 0.8f; ///< Thickness of the gates of a lock.
constexpr float LOCK_GATE_GAP = 0.12f; ///< Half the width of the seam where the two leaves of a gate of a lock meet.
constexpr float LOCK_WALKWAY = 0.3f; ///< Thickness of the walkway on top of the gates of a lock, and how far it sticks out.
constexpr float BRIDGE_DECK_EDGE = 2.0f; ///< How far the edges of the deck of a bridge are from the sides of its tiles.
constexpr float RAMP_EDGE = 2.5f; ///< How far the edges of the deck of a bridge ramp are from the sides of its tile.
constexpr float BRIDGE_SIDE_PLANE = 13.5f; ///< Where the near side of a bridge stands across its tiles when its sprite does not tell.
constexpr float BRIDGE_SIDE_BOOST = 1.25f; ///< How much more opaque the sides of bridges and fences are made, see #Renderer::GetSideTexture.
constexpr float FENCE_INSET = 0.6f; ///< How far fences stand inside the sides of their tiles, so those of two tiles on the same side don't overlap.
constexpr float FENCE_DIST = 12.0f * TILE; ///< Fences along railways are drawn up to this distance; further away, their posts get thinner than a pixel.
constexpr float FENCE_HEIGHT = 2.0f; ///< Height of the fences along railways, half that of the eye in the cab.
constexpr float FENCE_POST_SPACING = 4.0f; ///< Distance between the posts of fences along railways.
constexpr float FENCE_POST_SIZE = 0.3f; ///< Width of a post of a fence along a railway.
constexpr float FENCE_RAIL_SIZE = 0.25f; ///< Height of a rail of a fence along a railway.
constexpr float PORTAL_HEIGHT = 11.0f; ///< Height of the face of a tunnel portal; its top stands above the hill.
constexpr float PORTAL_OPENING = 4.5f; ///< Half the width of the opening of a tunnel portal, and the radius of its arch.
constexpr float PORTAL_SPRING = 3.0f; ///< Height where the arch of a tunnel portal starts; the top of the arch stays below the hill, which would show in the opening otherwise.
constexpr float PORTAL_RING = 0.9f; ///< Width of the ring of stones around the opening of a tunnel portal.
constexpr float PORTAL_DEPTH = 3.0f; ///< How far the walls of the opening of a tunnel go into the darkness.
constexpr int PORTAL_SEGMENTS = 8; ///< Number of pieces of the arch of a tunnel.
constexpr float TUNNEL_STEP = 4.0f; ///< Length of the pieces of the vault inside a tunnel; every other one is a bit darker, so the walls are seen to pass by.
constexpr float TUNNEL_GLOW = 12.0f; ///< Distance from an entrance of a tunnel over which most of the daylight inside it fades.

constexpr float PAVEMENT_WIDTH = 3.0f; ///< Width of the pavements of town roads, from the side of the tile to the kerb, as the graphics paint them.
constexpr float KERB_HEIGHT = 0.3f; ///< How high the pavements of town roads stand above the roadway.
constexpr float TRAM_RAIL_WIDTH = 0.3f; ///< Width of the rails of tram track, sunk into the road.
constexpr int ROAD_ARC_SEGMENTS = 8; ///< Number of pieces of the arcs of curved roads.
constexpr float ROAD_LINE_LENGTH = 1.6f; ///< Length of the dashes of the line in the middle of roads.
constexpr float ROAD_LINE_WIDTH = 0.3f; ///< Width of the line in the middle of roads.
constexpr float ROAD_LINE_SPACING = 4.0f; ///< Distance between the dashes of the line in the middle of roads, as the graphics paint them.

/** Direction each way something can face goes to, in world coordinates. */
constexpr std::array<std::pair<float, float>, DIR_END> HEADINGS = {{
	{-1, -1}, {-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1},
}};

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
constexpr RGB CROSSING_CROSS_COLOUR = {236, 232, 224}; ///< Colour of the cross on the posts of a level crossing.
constexpr RGB SIGNAL_POST_COLOUR = {96, 96, 100}; ///< Colour of the post of a signal.
constexpr RGB SIGNAL_HEAD_COLOUR = {30, 30, 32}; ///< Colour of the head of a signal.
constexpr RGB SIGNAL_RED_LIT = {255, 40, 24}; ///< Colour of the red lamp of a signal showing stop.
constexpr RGB SIGNAL_RED_DARK = {84, 28, 22}; ///< Colour of the red lamp of a signal showing go.
constexpr RGB SIGNAL_GREEN_LIT = {48, 255, 96}; ///< Colour of the green lamp of a signal showing go.
constexpr RGB SIGNAL_GREEN_DARK = {22, 72, 38}; ///< Colour of the green lamp of a signal showing stop.
constexpr RGB SEMAPHORE_ARM_COLOUR = {196, 28, 24}; ///< Colour of the arm of a semaphore signal.
constexpr RGB PORTAL_COLOUR = {124, 114, 100}; ///< Colour of the stone face of a tunnel portal.
constexpr RGB PORTAL_RING_COLOUR = {170, 158, 140}; ///< Colour of the ring of stones around the opening of a tunnel portal.
constexpr RGB TUNNEL_WALL_COLOUR = {44, 40, 36}; ///< Colour of the walls just inside a tunnel.
constexpr RGB TUNNEL_DARK = {6, 6, 6}; ///< Colour of the darkness inside a tunnel.
constexpr RGB TUNNEL_VAULT_LIT = {84, 77, 68}; ///< Colour of the vault inside a tunnel at its entrances, in the daylight.
constexpr RGB TUNNEL_VAULT_DARK = {18, 17, 15}; ///< Colour of the vault deep inside a tunnel.
constexpr RGB TUNNEL_FLOOR_LIT = {70, 62, 54}; ///< Colour of the ballast inside a tunnel at its entrances.
constexpr RGB TUNNEL_FLOOR_DARK = {12, 11, 10}; ///< Colour of the ballast deep inside a tunnel.
constexpr RGB TUNNEL_RAIL_LIT = {180, 180, 182}; ///< Colour of the rails inside a tunnel at its entrances.
constexpr RGB TUNNEL_RAIL_DARK = {56, 56, 58}; ///< Colour of the rails deep inside a tunnel, which still catch the light from its ends.
constexpr RGB TRAM_RAIL_COLOUR = {150, 150, 154}; ///< Colour of the rails of tram track.
constexpr RGB KERB_COLOUR = {178, 176, 170}; ///< Colour of the kerbs of town roads.
constexpr RGB ROAD_LINE_COLOUR = {236, 236, 228}; ///< Colour of the line in the middle of roads.
constexpr RGB ROAD_LINE_FAR_COLOUR = {116, 116, 116}; ///< Colour of the line in the middle of roads far away, about that of the road.
constexpr RGB SHELTER_FRAME_COLOUR = {196, 198, 204}; ///< Colour of the posts of bus shelters.
constexpr RGB SHELTER_GLASS_COLOUR = {112, 146, 170}; ///< Colour of the glass of the walls and roofs of bus shelters.
constexpr RGB SHELTER_BENCH_COLOUR = {132, 92, 52}; ///< Colour of the benches in bus shelters.
constexpr RGB BOOTH_COLOUR = {150, 72, 52}; ///< Colour of the brick booths of lorry stops.
constexpr RGB TUNNEL_ROAD_LIT = {84, 84, 84}; ///< Colour of the road inside a tunnel at its entrances.
constexpr RGB TUNNEL_ROAD_DARK = {14, 14, 14}; ///< Colour of the road deep inside a tunnel.
constexpr RGB AQUEDUCT_COLOUR = {178, 174, 166}; ///< Colour of the stone of aqueducts.
constexpr RGB BANK_COLOUR = {120, 112, 100}; ///< Colour of the banks of canals, rivers and the sea, between the water and the land.
constexpr RGB LOCK_COLOUR = {150, 146, 138}; ///< Colour of the stone walls of locks.
constexpr RGB LOCK_GATE_COLOUR = {70, 52, 36}; ///< Colour of the wooden gates of locks.
constexpr RGB LOCK_GATE_GAP_COLOUR = {28, 22, 16}; ///< Colour of the seam between the leaves of a gate of a lock.

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
	RGB mean = {0, 0, 0}; ///< Average colour of the opaque pixels.

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

/** Where a fence stands in its sprite, see #FitFence. */
struct FenceFit {
	float line; ///< Where the bottom of the fence is, relative to where the sprite is drawn, see #FitFence.
	float height; ///< Height of the fence, in rows of the sprite.
	float left; ///< Column of the left side of the fence, relative to where the sprite is drawn.
	float right; ///< Column of the right side of the fence, relative to where the sprite is drawn.
};

/**
 * Whether a sprite is the building of the default rail stations.
 * @param image The sprite.
 * @return True for the building along either axis.
 */
static bool IsDefaultStationBuilding(SpriteID image)
{
	const SpriteID sprite = image & SPRITE_MASK;
	return sprite == SPR_RAIL_PLATFORM_BUILDING_X || sprite == SPR_RAIL_PLATFORM_BUILDING_Y;
}

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

struct RoadLook;

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
		this->fence_fits.clear();
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
	VehicleID own = VehicleID::Invalid(); ///< The vehicle the camera rides on.
	bool frame_valid = false; ///< Whether #frame holds a finished picture.
	CabViewCamera frame_camera; ///< Camera of the picture in #frame.
	float fog_start = FOG_START; ///< Distance where the haze starts in the current picture.
	float fog_end = FOG_END; ///< Distance where everything has melted into the haze in the current picture.
	uint32_t frame_id = 0; ///< Identification of the picture in #frame, see #DrawCabView3D.
	float fixed_ms = 0; ///< Average time (in milliseconds) of the parts of making a picture that don't depend on its size.
	float fill_ms = 0; ///< Average time (in milliseconds) filling a picture of the size of the view takes.

	std::unordered_map<SpriteID, RawSprite> sprites; ///< Sprites loaded so far.
	std::unordered_map<std::vector<int32_t>, GroundTexture, TextureKeyHash> textures; ///< Ground textures put together so far.
	std::unordered_map<uint64_t, GroundTexture> box_textures; ///< Textures of sprites standing in boxes, by sprite and palette.
	std::map<std::tuple<SpriteID, PaletteID, int, int, int, int, int, bool>, BuildingShape> building_shapes; ///< Shapes of buildings worked out so far, see #GetBuildingShape.
	std::map<std::tuple<SpriteID, PaletteID, int, int>, std::optional<FenceFit>> fence_fits; ///< Where fences stand in their sprites, see #GetFenceFit.
	std::vector<GroundSpriteToDraw> ground_sprites; ///< Scratch buffer for collecting ground sprites.
	std::vector<BoxSpriteToDraw> box_sprites; ///< Scratch buffer for collecting sprites standing in boxes.
	std::vector<int32_t> key; ///< Scratch buffer for texture keys.

	const RawSprite &GetRawSprite(SpriteID sprite);
	const GroundTexture &GetGroundTexture(TileIndex tile, int tile_z, bool boxes, std::pair<float, float> smooth);
	const GroundTexture &GetBoxTexture(SpriteID image, PaletteID pal, bool widen = true);
	const GroundTexture &GetCompositeTexture(std::span<const BoxSpriteToDraw *const> boxes, Point origin);
	const GroundTexture &GetSideTexture(SpriteID image, PaletteID pal, Axis axis, float climb = 0, bool detail = false);
	RGB Fog(const RGB &colour, float dist) const;

	std::vector<ScreenTriangle> triangles; ///< Triangles of the current picture, nearest first.
	std::vector<TileIndex> tunnels; ///< Tunnels whose insides are in the current picture, by one of their entrances, see #AddTunnelInside.

	void AddTriangle(const std::array<ClipVertex, 3> &tri, const Paint &paint);
	void SetupTriangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c, const Paint &paint);
	void AddFlatQuad(const std::array<Vec3, 4> &quad, const RGB &colour, float bias, bool lit = false);
	void AddFlatPolygon(std::span<const Vec3> pts, const RGB &colour, float bias, bool lit = false);
	void AddFlatBox(const Vec3 &lo, const Vec3 &hi, const RGB &colour);
	float GetLockWater(TileIndex tile, int lower);
	void AddLock(TileIndex tile, const std::array<float, CORNER_END> &corner_z, float water);
	void AddBanks(TileIndex tile, const std::array<float, CORNER_END> &land_z, const std::array<float, CORNER_END> &water_z);
	void Rasterise(int stripe, int stripes);

	void DrawSky();
	void AddTile(TileIndex tile, float dist);
	void AddTrack(TileIndex tile, TrackBits tracks, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist);
	void AddRoad(TileIndex tile, const RoadLook &look, const GroundTexture &tex, int raw_z, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist);
	void AddRoadStop(TileIndex tile, const std::array<float, CORNER_END> &corner_z, bool split_we);
	void AddBridgeSide(const GroundTexture &tex, float sx, float sy, Axis axis, float plane, float shown_plane, std::pair<float, float> along, std::pair<float, float> deck);
	void AddBridgeMiddle(TileIndex tile, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist);
	void AddBridgePillars(TileIndex tile, Axis axis, float along, std::pair<float, float> across, float under, const std::array<float, CORNER_END> &corner_z, bool split_we, const RGB &colour);
	void AddBridgeRamp(TileIndex tile, int ground_z, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist);
	void AddAqueductWalls(Axis axis, float a0, std::pair<float, float> across, std::pair<float, float> deck);
	void AddTunnelPortal(TileIndex tile, int z);
	bool InLineWithTunnel(TileIndex tile, int z) const;
	void AddTunnelInside(TileIndex tile, int z);
	void AddBox(const BoxSpriteToDraw &box, int ground_z, float height_scale);
	const BuildingShape &GetBuildingShape(const BoxSpriteToDraw &box, bool station);
	void AddBuilding(const BoxSpriteToDraw &box, float height_scale, bool station);
	void AddCutout(const GroundTexture &tex, const Vec3 &foot, float foot_col, float foot_row, float scale_h, float scale_v);
	void AddBillboard(const BoxSpriteToDraw &box);
	bool AddSignal(TileIndex tile, const BoxSpriteToDraw &box);
	void AddCrossing(TileIndex tile, int z);
	const std::optional<FenceFit> &GetFenceFit(SpriteID image, PaletteID pal, Axis axis, float climb);
	void AddFence(const BoxSpriteToDraw &box, TileIndex tile, Axis axis, float edge, const std::array<float, CORNER_END> &corner_z, bool split_we);
	void AddPostFence(TileIndex tile, std::pair<float, float> from, std::pair<float, float> to, const std::array<float, CORNER_END> &corner_z, bool split_we, const RGB &colour);
	void AddFieldFences(TileIndex tile, const std::array<float, CORNER_END> &corner_z, bool split_we);
	bool AddRailFence(TileIndex tile, const BoxSpriteToDraw &box, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist);
	void AddVehicles(VehicleID own);
	Direction GetShownDirection(Direction dir) const;
	void AddShip(const Ship *v);
	void AddAircraft(const Aircraft *v);
	void AddSpriteFace(const GroundTexture &tex, const Placement &pl, std::span<const Vec3> pts, const Vec3 &normal, const std::function<Vec3(const Vec3 &)> &shown, bool cutout);
	void AddTexturedFace(const GroundTexture &tex, float sx, float sy, std::span<const Vec3> world, std::span<const Vec3> shown, const Vec3 &normal, bool cutout);
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
 * Whether a pixel of the ground of a road is part of the line painted in its middle.
 * The original graphics paint it in the brightest white of the palette.
 * @param p The pixel.
 * @return True for the pixels of the line.
 */
static bool IsRoadLine(const SpriteLoader::CommonPixel &p)
{
	return p.m == 15;
}

/** Pixels of the ground sprites that are left out, and filled from the ground around them. */
enum class SkipPixels : uint8_t {
	None, ///< Keep all pixels.
	UnderBuilding, ///< The area under buildings, see #IsUnderBuilding.
	RoadLine, ///< The line in the middle of roads, see #IsRoadLine; the 3D view draws it sharp.
};

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
	tex.mean = mean;
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
 * @param skip Which pixels to leave out.
 */
static void PaintSprite(Picture &pic, std::vector<float> &alpha, const RawSprite &raw, PaletteID pal, int ox, int oy, SkipPixels skip = SkipPixels::None)
{
	const uint8_t *remap = (pal != PAL_NONE) ? GetNonSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1 : nullptr;
	for (int y = 0; y < raw.height; y++) {
		for (int x = 0; x < raw.width; x++) {
			const SpriteLoader::CommonPixel &p = raw.pixels[y * raw.width + x];
			if (p.a == 0) continue;
			if (skip == SkipPixels::UnderBuilding && IsUnderBuilding(p)) continue;
			if (skip == SkipPixels::RoadLine && IsRoadLine(p)) continue;
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

/** How the 3D view draws a road tile, see Renderer::AddRoad. */
struct RoadLook {
	RoadBits road = ROAD_NONE; ///< Pieces of road.
	RoadBits tram = ROAD_NONE; ///< Pieces of tram track.
	bool plain = false; ///< Whether the graphics paint it plainly, so the line they paint in the middle is left out.
	bool line = false; ///< Whether the line in the middle is drawn sharp, instead of the one the graphics paint.
	bool pavement = false; ///< Whether it has raised pavements along it.
	bool stop = false; ///< Whether it is a drive-through road stop of the base graphics, with shelters on its pavements.
};

/**
 * Whether pieces of road make a curve.
 * @param bits The pieces.
 * @return True when they are two neighbouring sides of the tile.
 */
static bool IsRoadCurve(RoadBits bits)
{
	return bits == ROAD_N || bits == ROAD_E || bits == ROAD_S || bits == ROAD_W;
}

/**
 * Find out how the 3D view draws a road tile.
 * The line in the middle of straight and curved roads, and the pavements of town roads, are drawn
 * sharp and the pavements raised, for the road types of the base graphics, whose sprites are known.
 * @param tile The tile.
 * @return What is drawn; nothing for tiles other than plain road and drive-through road stops.
 */
static RoadLook GetRoadLook(TileIndex tile)
{
	RoadLook look;
	/* Drive-through road stops of the base graphics lie on a straight town road, with pavements on both sides. */
	const bool stop = IsDriveThroughStopTile(tile) && !IsRoadWaypoint(tile) && GetRoadStopSpec(tile) == nullptr;
	if (IsTunnelTile(tile) && GetTunnelBridgeTransportType(tile) == TRANSPORT_ROAD) {
		/* The entrance of a road tunnel has plain road in front of it, see SetTunnelGround. */
		const RoadBits bits = AxisToRoadBits(DiagDirToAxis(GetTunnelBridgeDirection(tile)));
		if (HasTileRoadType(tile, RTT_TRAM)) look.tram = bits;
		if (HasTileRoadType(tile, RTT_ROAD)) look.road = bits;
		look.plain = look.road != ROAD_NONE && look.tram == ROAD_NONE && !GetRoadTypeInfo(GetRoadTypeRoad(tile))->UsesOverlay();
		look.line = look.plain;
		return look;
	}
	if (!stop && (!IsTileType(tile, MP_ROAD) || !IsNormalRoad(tile))) return look;
	const RoadBits stop_bits = stop ? AxisToRoadBits(GetDriveThroughStopAxis(tile)) : ROAD_NONE;
	if (HasTileRoadType(tile, RTT_TRAM)) look.tram = stop ? stop_bits : GetRoadBits(tile, RTT_TRAM);
	if (!HasTileRoadType(tile, RTT_ROAD)) return look;
	look.road = stop ? stop_bits : GetRoadBits(tile, RTT_ROAD);
	if (GetRoadTypeInfo(GetRoadTypeRoad(tile))->UsesOverlay()) return look;
	if (stop) {
		look.plain = look.tram == ROAD_NONE;
		look.line = look.plain;
		look.pavement = true;
		look.stop = true;
		return look;
	}
	if (HasRoadWorks(tile)) return look;
	/* The line goes along straight and curved roads, not over junctions; with tram track in the middle of the road, there is none. */
	look.plain = look.tram == ROAD_NONE;
	look.line = look.plain && (look.road == ROAD_X || look.road == ROAD_Y || IsRoadCurve(look.road));
	const Roadside side = GetRoadside(tile);
	look.pavement = !IsOnSnowOrDesert(tile) && (side == Roadside::Paved || side == Roadside::StreetLights || side == Roadside::Trees);
	return look;
}

/**
 * Replace the ground sprites of a tunnel entrance by those of flat ground with its track or road on it.
 * The game draws the slope of the hill with the track going down into it; the 3D view keeps the track
 * level, as it is in the tunnel, and puts a portal where the hill starts, see Renderer::AddTunnelPortal.
 * @param tile The tunnel entrance.
 * @param z Height of the bottom of the tile.
 * @param sprites The ground sprites.
 */
static void SetTunnelGround(TileIndex tile, int z, std::vector<GroundSpriteToDraw> &sprites)
{
	const Point pt = RemapCoords(TileX(tile) * TILE_SIZE, TileY(tile) * TILE_SIZE, z);
	const Axis axis = DiagDirToAxis(GetTunnelBridgeDirection(tile));
	const bool snow = HasTunnelBridgeSnowOrDesert(tile);
	sprites.clear();
	if (GetTunnelBridgeTransportType(tile) != TRANSPORT_RAIL) {
		sprites.push_back({axis == AXIS_X ? SPR_ROAD_X : SPR_ROAD_Y, PAL_NONE, pt.x, pt.y});
		return;
	}
	const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(tile));
	if (rti->UsesOverlay()) {
		sprites.push_back({snow ? SPR_FLAT_SNOW_DESERT_TILE : SPR_FLAT_GRASS_TILE, PAL_NONE, pt.x, pt.y});
		sprites.push_back({GetCustomRailSprite(rti, tile, RTSG_OVERLAY) + (axis == AXIS_X ? RTO_X : RTO_Y), PAL_NONE, pt.x, pt.y});
	} else {
		sprites.push_back({rti->base_sprites.track_y + (axis == AXIS_X ? 1 : 0) + (snow ? rti->snow_offset : 0), PAL_NONE, pt.x, pt.y});
	}
}

/**
 * Whether a tile is the middle part of a lock, where the ships go up and down.
 * @param tile The tile.
 * @return True for the middle of a lock.
 */
static bool IsLockMiddle(TileIndex tile)
{
	return IsTileType(tile, MP_WATER) && IsLock(tile) && GetLockPart(tile) == LockPart::Middle;
}

/**
 * Whether the 3D view shows a tile as water a bit below the land around it, see #CAB_VIEW_3D_WATER_DROP.
 * That are canals, rivers (also where they flow down a slope), the sea, ship depots, the ends of locks,
 * and buoys, the water part of docks and oil rigs. Not the shore, half land and half water, and not the
 * middle of locks, see Renderer::AddLock.
 * @param tile The tile.
 * @return True for such water.
 */
static bool IsLoweredWater(TileIndex tile)
{
	switch (GetTileType(tile)) {
		case MP_WATER: return !IsCoastTile(tile) && !IsLockMiddle(tile);
		case MP_STATION:
		case MP_INDUSTRY: return IsTileOnWater(tile) && GetTileSlope(tile) == SLOPE_FLAT;
		default: return false;
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
	if (IsTunnelTile(tile)) SetTunnelGround(tile, tile_z, this->ground_sprites);
	/* Water below the land has banks of its own, see AddBanks; leave out those painted on the water. */
	if (IsLoweredWater(tile) && this->ground_sprites.size() > 1) this->ground_sprites.resize(1);
	if (IsLevelCrossingTile(tile) && IsCrossingBarred(tile)) {
		/* The lit lamps painted on the ground of a closed crossing are on the posts, see AddCrossing. */
		const SpriteID crossing = GetRailTypeInfo(GetRailType(tile))->base_sprites.crossing;
		for (GroundSpriteToDraw &sprite : this->ground_sprites) {
			const SpriteID image = sprite.image & SPRITE_MASK;
			if (image >= crossing && image < crossing + 12 && HasBit(image - crossing, 1)) sprite.image -= 2;
		}
	}

	/* The line in the middle of the road is drawn sharp, see AddRoad. */
	const bool road_line = GetRoadLook(tile).plain;

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
	if (sprite_keys != 0 && (smooth.first != 0 || smooth.second != 0 || road_line)) {
		this->key.push_back(static_cast<int32_t>(std::lround(smooth.first * 64)));
		this->key.push_back(static_cast<int32_t>(std::lround(smooth.second * 64)));
		this->key.push_back(road_line);
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
		PaintSprite(pic, alpha, raw, this->key[i + 1], this->key[i + 2] + raw.x_offs - left, this->key[i + 3] + raw.y_offs - top,
				building ? SkipPixels::UnderBuilding : road_line ? SkipPixels::RoadLine : SkipPixels::None);
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
	PaintSprite(pic, alpha, raw, pal, 0, 0);
	FinishTexture(tex, std::move(alpha), 4, true, {0, 0}, widen);
	return tex;
}

/**
 * Get the texture of sprites standing in boxes, put on top of each other as the viewport draws them,
 * to dress a surface in, like the deck of a bridge. Sprites that look the same share the texture.
 * @param boxes The sprites.
 * @param origin Where the texture is relative to, in virtual viewport coordinates.
 * @return The texture; without any pixels when the sprites have none.
 */
const GroundTexture &Renderer::GetCompositeTexture(std::span<const BoxSpriteToDraw *const> boxes, Point origin)
{
	/* Starting with a marker that no ground texture has. */
	this->key.clear();
	this->key.push_back(INT32_MIN);
	for (const BoxSpriteToDraw *box : boxes) {
		this->key.push_back(box->image & SPRITE_MASK);
		this->key.push_back(box->pal);
		this->key.push_back(FloorDiv(box->x - origin.x, ZOOM_BASE));
		this->key.push_back(FloorDiv(box->y - origin.y, ZOOM_BASE));
	}
	auto it = this->textures.find(this->key);
	if (it != this->textures.end()) return it->second;
	GroundTexture &tex = this->textures[this->key];

	int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
	for (size_t i = 1; i < this->key.size(); i += 4) {
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
	for (size_t i = 1; i < this->key.size(); i += 4) {
		const RawSprite &raw = this->GetRawSprite(this->key[i]);
		PaintSprite(pic, alpha, raw, this->key[i + 1], this->key[i + 2] + raw.x_offs - left, this->key[i + 3] + raw.y_offs - top);
	}
	FinishTexture(tex, std::move(alpha), 4, false);
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
	float f = std::clamp((dist - this->fog_start) / (this->fog_end - this->fog_start), 0.0f, 1.0f);
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
 * Add a flat coloured quad, shaded by its orientation unless it gives light.
 * @param quad The corners in world coordinates, in order around the quad.
 * @param colour The colour.
 * @param bias Depth bias.
 * @param lit Whether it gives light, like a lamp, and so is not shaded.
 */
void Renderer::AddFlatQuad(const std::array<Vec3, 4> &quad, const RGB &colour, float bias, bool lit)
{
	this->AddFlatPolygon(quad, colour, bias, lit);
}

/**
 * Add a flat coloured polygon, shaded by its orientation unless it gives light.
 * @param pts The corners in world coordinates, in order around the polygon, which is convex.
 * @param colour The colour.
 * @param bias Depth bias.
 * @param lit Whether it gives light, like a lamp, and so is not shaded.
 */
void Renderer::AddFlatPolygon(std::span<const Vec3> pts, const RGB &colour, float bias, bool lit)
{
	/* The corners may start with a sliver; take the normal of the widest corner. */
	Vec3 normal = {0, 0, 0};
	for (size_t i = 1; i + 1 < pts.size(); i++) {
		Vec3 n = (pts[i] - pts[0]).Cross(pts[i + 1] - pts[0]);
		if (n.Dot(n) > normal.Dot(normal)) normal = n;
	}
	if (normal.z < 0) normal = normal * -1;
	Paint paint = {nullptr, lit ? colour : colour * Shade(normal), 1.0f, 0, bias};

	ClipVertex first = {this->cam.ToCamera(pts[0]), {}};
	ClipVertex prev = {this->cam.ToCamera(pts[1]), {}};
	for (size_t i = 2; i < pts.size(); i++) {
		ClipVertex next = {this->cam.ToCamera(pts[i]), {}};
		this->AddTriangle({first, prev, next}, paint);
		prev = next;
	}
}

/**
 * Add a box of a flat colour, without its bottom.
 * @param lo The lowest corner.
 * @param hi The highest corner.
 * @param colour The colour.
 */
void Renderer::AddFlatBox(const Vec3 &lo, const Vec3 &hi, const RGB &colour)
{
	const std::array<Vec3, 4> ring = {Vec3{lo.x, lo.y, 0}, Vec3{hi.x, lo.y, 0}, Vec3{hi.x, hi.y, 0}, Vec3{lo.x, hi.y, 0}};
	for (int i = 0; i < 4; i++) {
		const Vec3 &a = ring[i], &b = ring[(i + 1) % 4];
		this->AddFlatQuad({Vec3{a.x, a.y, lo.z}, Vec3{b.x, b.y, lo.z}, Vec3{b.x, b.y, hi.z}, Vec3{a.x, a.y, hi.z}}, colour, 1.0f);
	}
	this->AddFlatQuad({Vec3{lo.x, lo.y, hi.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{lo.x, hi.y, hi.z}}, colour, 1.0f);
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
	float horizon = this->cam.cy + this->cam.focal * this->cam.fwd.z / std::hypot(this->cam.fwd.x, this->cam.fwd.y);
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
		case MP_TUNNELBRIDGE: return GetTunnelBridgeTransportType(tile) == TRANSPORT_RAIL ? DiagDirToDiagTrackBits(GetTunnelBridgeDirection(tile)) : TRACK_BIT_NONE;
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
	/* The track goes into a tunnel level, see SetTunnelGround. */
	const bool tunnel = IsTunnelTile(tile);
	if (tunnel) slope = SLOPE_FLAT;

	std::array<float, CORNER_END> corner_z;
	for (Corner c : ALL_CORNERS) corner_z[c] = static_cast<float>(z + GetSlopePixelZInCorner(slope, c));
	/* The land, which the sprites of the ground are drawn for. */
	const std::array<float, CORNER_END> land_z = corner_z;
	/* Water lies a bit below the land around it; in the chamber of a lock as high as the ship in it, see AddLock. */
	const bool lock = IsLockMiddle(tile);
	const int drop = IsLoweredWater(tile) ? CAB_VIEW_3D_WATER_DROP : 0;
	const float lock_water = lock ? this->GetLockWater(tile, z) : 0.0f;
	/* The shore slopes into the water: its corners at the water go down with it. */
	const bool shore = IsTileType(tile, MP_WATER) && IsCoastTile(tile);
	const float shore_level = *std::ranges::min_element(corner_z);
	for (Corner c : ALL_CORNERS) {
		if (lock) {
			corner_z[c] = lock_water;
		} else if (shore) {
			if (corner_z[c] == shore_level) corner_z[c] -= CAB_VIEW_3D_WATER_DROP;
		} else {
			corner_z[c] -= drop;
		}
	}
	/* The game splits a slope along the line between the two corners at the same height. */
	bool split_we = corner_z[CORNER_W] == corner_z[CORNER_E] || corner_z[CORNER_N] != corner_z[CORNER_S];

	/* Straight track or road: smooth its ground along it, over whole steps of the pixels of its edges.
	 * Going a unit along it moves two columns in the texture, and a row less how much it climbs. */
	std::pair<float, float> smooth = {0, 0};
	/* The track of a bridge ramp is on the ramp, see AddBridgeRamp. */
	TrackBits tracks = IsBridgeTile(tile) ? TRACK_BIT_NONE : GetTileRailTracks(tile);
	const RoadLook road = GetRoadLook(tile);
	const RoadBits road_bits = road.road | road.tram;
	if (tracks == TRACK_BIT_X || (tracks == TRACK_BIT_NONE && road_bits == ROAD_X)) {
		float climb = (corner_z[CORNER_W] + corner_z[CORNER_S] - corner_z[CORNER_N] - corner_z[CORNER_E]) / (2 * TILE);
		smooth = {-2 * TRACK_SMOOTH_LENGTH, (1 - climb) * TRACK_SMOOTH_LENGTH};
	} else if (tracks == TRACK_BIT_Y || (tracks == TRACK_BIT_NONE && road_bits == ROAD_Y)) {
		float climb = (corner_z[CORNER_E] + corner_z[CORNER_S] - corner_z[CORNER_N] - corner_z[CORNER_W]) / (2 * TILE);
		smooth = {2 * TRACK_SMOOTH_LENGTH, (1 - climb) * TRACK_SMOOTH_LENGTH};
	}

	bool boxes = dist <= BOX_DIST;
	/* Bridges are seen from further away. */
	bool bridge = IsBridgeAbove(tile) || IsBridgeTile(tile);
	const GroundTexture &tex = this->GetGroundTexture(tile, raw_z, boxes || bridge, smooth);
	float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);

	/* Attributes: screen column and row in the texture, and world x and y for the fine pattern. */
	auto vertex = [&](Corner c) {
		Point o = CornerOffset(c);
		ClipVertex v;
		v.c = this->cam.ToCamera({x0 + o.x, y0 + o.y, corner_z[c]});
		v.a = {2.0f * (o.y - o.x), static_cast<float>(o.x + o.y) - (land_z[c] - raw_z), x0 + o.x, y0 + o.y};
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
	if (!tunnel && (z != raw_z || slope != raw_slope)) {
		static const std::array<std::pair<Corner, Corner>, 4> edges = {{{CORNER_N, CORNER_W}, {CORNER_W, CORNER_S}, {CORNER_S, CORNER_E}, {CORNER_E, CORNER_N}}};
		for (auto [a, b] : edges) {
			float ga = static_cast<float>(raw_z + GetSlopePixelZInCorner(RemoveHalftileSlope(raw_slope), a));
			float gb = static_cast<float>(raw_z + GetSlopePixelZInCorner(RemoveHalftileSlope(raw_slope), b));
			if (corner_z[a] <= ga && corner_z[b] <= gb) continue;
			this->AddFlatQuad({p[a], p[b], Vec3{p[b].x, p[b].y, gb}, Vec3{p[a].x, p[a].y, ga}}, FOUNDATION_COLOUR, 1.0f);
		}
	}

	this->AddTrack(tile, tracks, corner_z, split_we, dist);
	if (road_bits != ROAD_NONE) this->AddRoad(tile, road, tex, raw_z, corner_z, split_we, dist);
	if (boxes && road.stop) this->AddRoadStop(tile, corner_z, split_we);
	if (lock) this->AddLock(tile, land_z, lock_water);
	if (drop != 0 || shore) this->AddBanks(tile, land_z, corner_z);
	if (tunnel) this->AddTunnelPortal(tile, raw_z);
	if (IsBridgeTile(tile)) this->AddBridgeRamp(tile, raw_z, corner_z, split_we, dist);
	if (IsBridgeAbove(tile)) this->AddBridgeMiddle(tile, corner_z, split_we, dist);
	if (boxes && IsLevelCrossingTile(tile)) this->AddCrossing(tile, z);
	if (boxes && IsTileType(tile, MP_CLEAR)) this->AddFieldFences(tile, corner_z, split_we);
	/* The walls and gates of locks are drawn whole, see AddLock. */
	if (boxes && !lock) {
		for (BoxSpriteToDraw &box : this->box_sprites) {
			if (box.bridge) continue;
			/* Buoys and ship depots float on the water. */
			box.zmin -= drop;
			box.zmax -= drop;
			switch (GetTileType(tile)) {
				case MP_TUNNELBRIDGE:
					/* Ramps and tunnel portals are drawn as a whole. */
					break;

				case MP_TREES:
					this->AddBillboard(box);
					break;

				case MP_CLEAR:
					/* Fences around fields are drawn whole, see AddFieldFences. */
					break;

				case MP_HOUSE:
				case MP_INDUSTRY:
				case MP_OBJECT:
					if (!box.child) this->AddBuilding(box, BUILDING_HEIGHT, false);
					break;

				case MP_STATION:
					/* The shelters of road stops are drawn whole, see AddRoadStop. */
					if (road.stop) break;
					if (IsAirport(tile)) {
						/* The lights along the runways are drawn in empty boxes; standing up, they looked like heaps. */
						if (box.child || box.zmax < box.zmin) break;
						/* Hangars are drawn as a front and a rear wall in thin boxes; give each the whole tile to find its shape in. */
						if (box.zmax - box.zmin >= 10 && box.zmin <= z + 1) {
							BoxSpriteToDraw building = box;
							int x0 = TileX(tile) * TILE_SIZE, y0 = TileY(tile) * TILE_SIZE;
							if (building.xmax - building.xmin < 2) std::tie(building.xmin, building.xmax) = std::pair(x0, x0 + static_cast<int>(TILE_SIZE) - 1);
							if (building.ymax - building.ymin < 2) std::tie(building.ymin, building.ymax) = std::pair(y0, y0 + static_cast<int>(TILE_SIZE) - 1);
							this->AddBuilding(building, BUILDING_HEIGHT, true);
							break;
						}
					}
					/* Station buildings, standing on the ground, get their real shape; platforms and roofs stay boxes. */
					if (!box.child && box.zmax - box.zmin >= 10 && box.zmin <= z + 1) {
						this->AddBuilding(box, BUILDING_HEIGHT, true);
						if (IsDefaultStationBuilding(box.image)) {
							/* Its platform, around the building. */
							BoxSpriteToDraw platform = box;
							platform.zmax = platform.zmin + 1;
							this->AddBox(platform, z, PLATFORM_HEIGHT);
						}
						break;
					}
					[[fallthrough]];

				case MP_RAILWAY:
					if (!box.child && this->AddRailFence(tile, box, corner_z, split_we, dist)) break;
					if (!box.child && this->AddSignal(tile, box)) break;
					[[fallthrough]];

				case MP_ROAD:
					/* The lights of level crossings are drawn whole, see AddCrossing. */
					if (IsLevelCrossingTile(tile)) break;
					[[fallthrough]];

				default:
					if (box.child) break;
					/* Fences along the edges of the tile don't map well onto their thin boxes; leave them out. */
					if (std::min(box.xmax - box.xmin, box.ymax - box.ymin) == 0 && std::max(box.xmax - box.xmin, box.ymax - box.ymin) >= 8) break;
					/* Thin things like lamps and catenary pylons stand as cut-outs. */
					if (box.xmax - box.xmin < 2 && box.ymax - box.ymin < 2) {
						this->AddBillboard(box);
					} else {
						/* Platforms, the low boxes of stations standing on the ground. */
						const bool platform = IsTileType(tile, MP_STATION) && box.zmin <= z + 1 && box.zmax - box.zmin <= 2;
						this->AddBox(box, z, platform ? PLATFORM_HEIGHT : 1.0f);
					}
					break;
			}
		}
	}
}

/**
 * Get how high the water in the chamber of a lock is: as high as the ship in it, or, when the ship the camera
 * is on is about to sail in, as high as that ship, so it does not jump when it does; else at its lower end.
 * @param tile The middle of the lock.
 * @param lower Height of the lower end of the lock.
 * @return The height of the water.
 */
float Renderer::GetLockWater(TileIndex tile, int lower)
{
	const Vehicle *own = Vehicle::GetIfValid(this->own);
	if (own != nullptr && own->type == VEH_SHIP && !own->vehstatus.Test(VehState::Hidden) && DistanceManhattan(own->tile, tile) <= 1 &&
			(own->tile == tile || (IsTileType(own->tile, MP_WATER) && IsLock(own->tile) && GetLockPart(own->tile) != LockPart::Middle))) {
		/* The eye eases up and down; the water goes with it. */
		return this->cam.pos.z - CAB_VIEW_3D_SHIP_EYE;
	}
	for (const Vehicle *v : VehiclesOnTile(tile)) {
		if (v->type == VEH_SHIP) return GetCabView3DShipWaterZ(v);
	}
	return static_cast<float>(lower - CAB_VIEW_3D_WATER_DROP);
}

/**
 * Add the middle of a lock: stone walls along its sides, from the water up to above the land at its upper end, a
 * stone sill and closed gates at the upper end while the water in the chamber is below the water above it, and closed
 * gates at the lower end while it is above the water below it. Its ground is flat, at the height of the water.
 * @param tile The middle of the lock.
 * @param land_z Height of the corners of the land of the tile, sloping up along the lock.
 * @param water Height of the water in the chamber.
 */
void Renderer::AddLock(TileIndex tile, const std::array<float, CORNER_END> &land_z, float water)
{
	const float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	const float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	const float lower = *std::ranges::min_element(land_z);
	const float upper = *std::ranges::max_element(land_z);
	const float bottom = lower - CAB_VIEW_3D_WATER_DROP;
	const float top_water = upper - CAB_VIEW_3D_WATER_DROP;
	/* The upper end is where both corners are high. */
	const Axis axis = (land_z[CORNER_N] == land_z[CORNER_E] || land_z[CORNER_W] == land_z[CORNER_S]) ? AXIS_X : AXIS_Y;
	const bool up_at_start = axis == AXIS_X ? land_z[CORNER_N] > land_z[CORNER_W] : land_z[CORNER_N] > land_z[CORNER_E];

	/* Boxes in coordinates along and across the lock. */
	auto box = [&](float a0, float a1, float c0, float c1, float z0, float z1, const RGB &colour) {
		if (z1 <= z0) return;
		if (axis == AXIS_X) {
			this->AddFlatBox({x0 + a0, y0 + c0, z0}, {x0 + a1, y0 + c1, z1}, colour);
		} else {
			this->AddFlatBox({x0 + c0, y0 + a0, z0}, {x0 + c1, y0 + a1, z1}, colour);
		}
	};
	box(0, TILE, 0, LOCK_WALL, bottom, upper + LOCK_WALL_TOP, LOCK_COLOUR);
	box(0, TILE, TILE - LOCK_WALL, TILE, bottom, upper + LOCK_WALL_TOP, LOCK_COLOUR);

	const float up_a = up_at_start ? 0 : TILE - LOCK_GATE;
	const float down_a = up_at_start ? TILE - LOCK_GATE : 0;
	/* Closed gates: two wooden leaves meeting in the middle, with a walkway on top. */
	auto gates = [&](float a, float z0, float z1) {
		const float mid = TILE / 2;
		box(a, a + LOCK_GATE, LOCK_WALL, mid - LOCK_GATE_GAP, z0, z1, LOCK_GATE_COLOUR);
		box(a, a + LOCK_GATE, mid + LOCK_GATE_GAP, TILE - LOCK_WALL, z0, z1, LOCK_GATE_COLOUR);
		box(a, a + LOCK_GATE, mid - LOCK_GATE_GAP, mid + LOCK_GATE_GAP, z0, z1, LOCK_GATE_GAP_COLOUR);
		box(a - LOCK_WALKWAY, a + LOCK_GATE + LOCK_WALKWAY, LOCK_WALL, TILE - LOCK_WALL, z1, z1 + LOCK_WALKWAY, LOCK_COLOUR);
	};
	if (water < top_water - 0.25f) {
		/* The sill under the upper gates, and the gates holding back the water above. */
		box(up_a, up_a + LOCK_GATE, LOCK_WALL, TILE - LOCK_WALL, bottom, std::max(water, bottom + 0.1f), LOCK_COLOUR);
		gates(up_a, std::max(water, bottom + 0.1f), top_water + 1);
	} else {
		box(up_a, up_a + LOCK_GATE, LOCK_WALL, TILE - LOCK_WALL, bottom, top_water - 0.2f, LOCK_COLOUR);
	}
	if (water > bottom + 0.25f) {
		/* The lower gates, holding back the water in the chamber. */
		gates(down_a, bottom - 0.5f, water + 1);
	}
}

/**
 * Add banks along the sides of water lying below the land, see #CAB_VIEW_3D_WATER_DROP: from the
 * water up to the land next to it, where that is not water or shore too.
 * @param tile The tile with the water, or the shore.
 * @param land_z Height of the corners of the land of the tile.
 * @param water_z Height of the corners of the tile as drawn: below the land where there is water.
 */
void Renderer::AddBanks(TileIndex tile, const std::array<float, CORNER_END> &land_z, const std::array<float, CORNER_END> &water_z)
{
	static const std::array<std::pair<Corner, Corner>, DIAGDIR_END> edges = {{{CORNER_N, CORNER_E}, {CORNER_E, CORNER_S}, {CORNER_S, CORNER_W}, {CORNER_W, CORNER_N}}};
	const float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	const float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	for (DiagDirection dir = DIAGDIR_BEGIN; dir < DIAGDIR_END; dir++) {
		TileIndex next = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(dir));
		if (IsValidTile(next) && (IsLoweredWater(next) || IsLockMiddle(next) || (IsTileType(next, MP_WATER) && IsCoastTile(next)))) continue;
		auto [a, b] = edges[dir];
		if (water_z[a] == land_z[a] && water_z[b] == land_z[b]) continue;
		const float xa = x0 + CornerOffset(a).x, ya = y0 + CornerOffset(a).y;
		const float xb = x0 + CornerOffset(b).x, yb = y0 + CornerOffset(b).y;
		this->AddFlatQuad({Vec3{xa, ya, land_z[a]}, Vec3{xb, yb, land_z[b]}, Vec3{xb, yb, water_z[b]}, Vec3{xa, ya, water_z[a]}}, BANK_COLOUR, 1.0f);
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
	std::array<Vec3, 4> world, pictured;
	for (size_t i = 0; i < pts.size(); i++) {
		world[i] = pl.ToWorld(pts[i]);
		pictured[i] = shown(pts[i]);
	}
	this->AddTexturedFace(tex, pl.sx, pl.sy, std::span(world).first(pts.size()), std::span(pictured).first(pts.size()), pl.Turn(normal), cutout);
}

/**
 * Add a flat side of a thing in the world, dressed in a sprite: each corner gets the colour of the sprite where
 * the isometric view draws a point, see #RemapCoords.
 * @param tex The texture of the sprite.
 * @param sx Screen column where the viewport draws the sprite, in normal zoom pixels.
 * @param sy Screen row where the viewport draws the sprite, in normal zoom pixels.
 * @param world The corners of the side in the world, in order around it; at most four.
 * @param shown For each corner, the point whose picture it gets.
 * @param normal Direction the side faces; sides facing away from the camera are left out.
 * @param cutout Whether pixels where the sprite is transparent are left out.
 */
void Renderer::AddTexturedFace(const GroundTexture &tex, float sx, float sy, std::span<const Vec3> world, std::span<const Vec3> shown, const Vec3 &normal, bool cutout)
{
	/* Scaling the heights tilts slanted sides; take the normal from the corners, facing the way it should. */
	Vec3 n = (world[1] - world[0]).Cross(world[2] - world[0]);
	if (n.Dot(normal) < 0) n = n * -1;
	if (n.Dot(n) == 0) return;
	if ((this->cam.pos - world[0]).Dot(n) <= 0) return;
	n = n.Normalised();

	std::array<ClipVertex, 4> v;
	for (size_t i = 0; i < world.size(); i++) {
		const Vec3 &m = shown[i];
		v[i].c = this->cam.ToCamera(world[i]);
		v[i].a[0] = 2.0f * (m.y - m.x) - sx;
		v[i].a[1] = (m.x + m.y - m.z) - sy;
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
	for (size_t i = 1; i + 1 < world.size(); i++) this->AddTriangle({v[0], v[i], v[i + 1]}, paint);
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
 * Get where the opaque pixels of a texture are.
 * @param tex The texture.
 * @return Screen columns of the left and right edge and rows of the top and bottom edge, relative to where the sprite is drawn.
 */
static std::array<float, 4> OpaqueBounds(const GroundTexture &tex)
{
	const Picture &pic = tex.levels[0];
	int left = pic.width, right = 0, top = pic.height, bottom = 0;
	for (int y = 0; y < pic.height; y++) {
		for (int x = 0; x < pic.width; x++) {
			if (!pic.alpha.empty() && pic.alpha[static_cast<size_t>(y) * pic.width + x] < 0.5f) continue;
			left = std::min(left, x);
			right = std::max(right, x + 1);
			top = std::min(top, y);
			bottom = std::max(bottom, y + 1);
		}
	}
	if (right <= left) return {0, 0, 0, 0};
	return {static_cast<float>(tex.left + left), static_cast<float>(tex.left + right), static_cast<float>(tex.top + top), static_cast<float>(tex.top + bottom)};
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

	if (station && IsDefaultStationBuilding(box.image)) {
		/* The building of the default rail stations stands on its platform, which fools the fitting below; its
		 * walls go up two thirds of the box, and its gable roof along the platform up to the top of the box. */
		const bool along_x = (box.image & SPRITE_MASK) == SPR_RAIL_PLATFORM_BUILDING_X;
		shape.x0 = along_x ? 1.5f : 0.0f;
		shape.y0 = along_x ? 0.0f : 1.5f;
		shape.x1 = along_x ? bx - 2 : bx - 1;
		shape.y1 = along_x ? by - 1 : by - 2;
		shape.walls.fill(bz * 2 / 3);
		shape.roof = along_x ? RoofKind::GableX : RoofKind::GableY;
		shape.ridge = bz;
		return shape;
	}

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
	auto valid = [&](const std::array<float, 7> &v, WallSlope slope) {
		/* A roof sloping one way starts at least halfway up; lower, it looks like a ramp, not a building. */
		if (slope != WallSlope::Level && 2 * std::min(v[4], v[5]) < std::max(v[4], v[5])) return false;
		return v[0] >= 0 && v[1] >= 0 && v[2] <= bx && v[3] <= by && v[2] - v[0] >= 1 && v[3] - v[1] >= 1 && v[4] >= 1 && v[5] >= 1 &&
				v[4] <= 1.5f * bz + 4 && v[6] <= v[4] + 1.5f * std::max(v[2] - v[0], v[3] - v[1]);
	};
	auto descend = [&](std::array<float, 7> v, WallSlope slope) {
		int best = fit.Measure(MakeShapeTry(v, slope)).score;
		auto attempt = [&](const std::array<float, 7> &w) {
			if (!valid(w, slope)) return false;
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
			} else if (best_slope != WallSlope::Level) {
				/* Not lower than halfway up on the low side either, see above. */
				const float w = std::max(*west, *east / 2), e = std::max(*east, *west / 2);
				result.walls = best_slope == WallSlope::AlongX ? std::array<float, 4>{w, w, e, e} : std::array<float, 4>{e, w, w, e};
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

/**
 * Add a signal where the viewport draws the sprite of one: a post with a head facing the trains it stops, whose
 * red or green lamp is lit as the signal shows, or the arm of a semaphore raised or not.
 * @param tile The tile.
 * @param box The sprite and its box.
 * @return Whether the box is that of a signal; it is then drawn.
 */
bool Renderer::AddSignal(TileIndex tile, const BoxSpriteToDraw &box)
{
	if (!IsPlainRailTile(tile) || !HasSignals(tile) || box.xmin != box.xmax || box.ymin != box.ymax) return false;

	/* The signals of each track, where the game draws them, see DrawSignals and DrawSingleSignal. */
	struct SignalSpot {
		uint8_t bit; ///< Bit of the signal in the map.
		Track track; ///< Track it stands at.
		Direction dir; ///< Direction the trains it stops go to.
		std::array<std::array<uint8_t, 2>, 2> pos; ///< Where it stands in the tile, when on the left and on the right of the track.
	};
	static const std::array<SignalSpot, 12> spots = {{
		{2, TRACK_LEFT, DIR_S, {{{8, 5}, {14, 1}}}}, {3, TRACK_LEFT, DIR_N, {{{14, 1}, {12, 10}}}},
		{0, TRACK_RIGHT, DIR_S, {{{1, 14}, {4, 6}}}}, {1, TRACK_RIGHT, DIR_N, {{{9, 11}, {1, 14}}}},
		{3, TRACK_UPPER, DIR_E, {{{1, 0}, {10, 4}}}}, {2, TRACK_UPPER, DIR_W, {{{3, 10}, {0, 1}}}},
		{1, TRACK_LOWER, DIR_E, {{{11, 4}, {14, 14}}}}, {0, TRACK_LOWER, DIR_W, {{{14, 14}, {5, 12}}}},
		{3, TRACK_X, DIR_NE, {{{11, 3}, {11, 13}}}}, {2, TRACK_X, DIR_SW, {{{4, 13}, {4, 3}}}},
		{3, TRACK_Y, DIR_SE, {{{3, 4}, {13, 4}}}}, {2, TRACK_Y, DIR_NW, {{{11, 13}, {3, 11}}}},
	}};
	bool right;
	switch (_settings_game.construction.train_signal_side) {
		case 0: right = false; break;
		case 2: right = true; break;
		default: right = _settings_game.vehicle.road_side != 0; break;
	}

	const TrackBits rails = GetTrackBits(tile);
	for (const SignalSpot &spot : spots) {
		/* The same bits are the signals of the straight track or of the diagonal ones, whichever the tile has. */
		bool used;
		switch (spot.track) {
			case TRACK_Y: used = (rails & TRACK_BIT_Y) != TRACK_BIT_NONE; break;
			case TRACK_X: used = (rails & TRACK_BIT_Y) == TRACK_BIT_NONE && (rails & TRACK_BIT_X) != TRACK_BIT_NONE; break;
			default: used = (rails & TRACK_BIT_CROSS) == TRACK_BIT_NONE && (rails & TrackToTrackBits(spot.track)) != TRACK_BIT_NONE; break;
		}
		if (!used) continue;
		if (!IsSignalPresent(tile, spot.bit)) continue;
		if (box.xmin != static_cast<int>(TileX(tile) * TILE_SIZE + spot.pos[right][0]) || box.ymin != static_cast<int>(TileY(tile) * TILE_SIZE + spot.pos[right][1])) continue;

		const bool go = GetSingleSignalState(tile, spot.bit) == SIGNAL_STATE_GREEN;
		const auto [dx, dy] = HEADINGS[spot.dir];
		/* Towards the trains it stops, and to its side, away from the track. */
		const Vec3 face = Vec3{-dx, -dy, 0}.Normalised();
		const Vec3 side = Vec3{dy, -dx, 0}.Normalised() * (right ? 1.0f : -1.0f);
		const Vec3 foot = {box.xmin + 0.5f, box.ymin + 0.5f, static_cast<float>(box.zmin)};

		/* A block from z0 to z1, centred on c, half_face deep and half_side wide. */
		auto block = [&](const Vec3 &c, float half_face, float half_side, float z0, float z1, const RGB &colour) {
			const Vec3 f = face * half_face, w = side * half_side;
			std::array<Vec3, 4> ring = {c + f + w, c + f - w, c - f - w, c - f + w};
			for (int i = 0; i < 4; i++) {
				const Vec3 a = ring[i], b = ring[(i + 1) % 4];
				this->AddFlatQuad({Vec3{a.x, a.y, z0}, Vec3{b.x, b.y, z0}, Vec3{b.x, b.y, z1}, Vec3{a.x, a.y, z1}}, colour, 1.0f);
			}
			this->AddFlatQuad({Vec3{ring[0].x, ring[0].y, z1}, Vec3{ring[1].x, ring[1].y, z1}, Vec3{ring[2].x, ring[2].y, z1}, Vec3{ring[3].x, ring[3].y, z1}}, colour, 1.0f);
		};
		/* A square on the front of the head, lit or not. */
		auto lamp = [&](float z, const RGB &colour, bool lit) {
			Vec3 c = foot + face * 0.32f;
			c.z = 0;
			const Vec3 w = side * (SIGNAL_LAMP_SIZE / 2);
			const float h = SIGNAL_LAMP_SIZE / 2;
			this->AddFlatQuad({c + w + Vec3{0, 0, z - h}, c - w + Vec3{0, 0, z - h}, c - w + Vec3{0, 0, z + h}, c + w + Vec3{0, 0, z + h}}, colour, 1.0f, lit);
		};

		const bool semaphore = GetSignalVariant(tile, spot.track) == SIG_SEMAPHORE;
		const float top = foot.z + SIGNAL_HEIGHT;
		block(foot, 0.15f, 0.15f, foot.z, semaphore ? top : top - SIGNAL_HEAD_HEIGHT, SIGNAL_POST_COLOUR);
		if (semaphore) {
			/* The arm sticks out away from the track; level to stop, raised to let trains go. */
			Vec3 c = foot + face * 0.2f;
			c.z = 0;
			const float rise = go ? 0.7f : 0.0f;
			const float len = SIGNAL_HEAD_HEIGHT;
			const Vec3 tip = c + side * (len * (go ? 0.7f : 1.0f));
			const float z = top - 0.5f;
			this->AddFlatQuad({c + Vec3{0, 0, z - 0.3f}, tip + Vec3{0, 0, z - 0.3f + len * rise}, tip + Vec3{0, 0, z + 0.3f + len * rise}, c + Vec3{0, 0, z + 0.3f}}, SEMAPHORE_ARM_COLOUR, 1.0f);
			/* Its lamp, in a housing that hides it from behind. */
			block(foot + face * 0.17f, 0.14f, SIGNAL_LAMP_SIZE * 0.65f, z - 1.0f - SIGNAL_LAMP_SIZE * 0.65f, z - 1.0f + SIGNAL_LAMP_SIZE * 0.65f, SIGNAL_HEAD_COLOUR);
			lamp(z - 1.0f, go ? SIGNAL_GREEN_LIT : SIGNAL_RED_LIT, true);
		} else {
			block(foot, 0.3f, SIGNAL_HEAD_WIDTH / 2, top - SIGNAL_HEAD_HEIGHT, top, SIGNAL_HEAD_COLOUR);
			lamp(top - SIGNAL_HEAD_HEIGHT * 0.3f, go ? SIGNAL_RED_DARK : SIGNAL_RED_LIT, !go);
			lamp(top - SIGNAL_HEAD_HEIGHT * 0.7f, go ? SIGNAL_GREEN_LIT : SIGNAL_GREEN_DARK, go);
		}
		return true;
	}
	return false;
}


/**
 * Add the warning lights of a level crossing: a post at each side of the road before and after the track, with a
 * cross on top and two lamps towards the road, red when the crossing is closed.
 * @param tile The level crossing.
 * @param z Height of the crossing.
 */
void Renderer::AddCrossing(TileIndex tile, int z)
{
	const bool barred = IsCrossingBarred(tile);
	const Axis road = GetCrossingRoadAxis(tile);
	const float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	const float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	/* Coordinates along the road and across it. */
	auto point = [&](float a, float c, float h) { return road == AXIS_X ? Vec3{x0 + a, y0 + c, z + h} : Vec3{x0 + c, y0 + a, z + h}; };

	for (float a : {CROSSING_POST_ALONG, TILE - CROSSING_POST_ALONG}) {
		/* Towards the road traffic coming to the track. */
		const float out = a < TILE / 2 ? -1.0f : 1.0f;
		for (float c : {CROSSING_POST_ACROSS, TILE - CROSSING_POST_ACROSS}) {
			const Vec3 foot = point(a, c, 0);
			const Vec3 face = point(out, 0, 0) - point(0, 0, 0);
			const Vec3 side = point(0, 1, 0) - point(0, 0, 0);
			auto block = [&](const Vec3 &centre, float half_face, float half_side, float z0, float z1, const RGB &colour) {
				const Vec3 lo = centre - face * half_face - side * half_side;
				const Vec3 hi = centre + face * half_face + side * half_side;
				this->AddFlatBox({std::min(lo.x, hi.x), std::min(lo.y, hi.y), centre.z + z0}, {std::max(lo.x, hi.x), std::max(lo.y, hi.y), centre.z + z1}, colour);
			};
			block(foot, 0.12f, 0.12f, 0, CROSSING_POST_HEIGHT, SIGNAL_POST_COLOUR);
			/* The lamps, side by side. */
			const float lamp_z = CROSSING_POST_HEIGHT - 1.6f;
			block(foot + face * 0.15f, 0.15f, 0.95f, lamp_z - 0.45f, lamp_z + 0.45f, SIGNAL_HEAD_COLOUR);
			for (float w : {-0.5f, 0.5f}) {
				const Vec3 c = foot + face * 0.31f + side * w + Vec3{0, 0, lamp_z};
				const Vec3 dw = side * (SIGNAL_LAMP_SIZE * 0.4f);
				const Vec3 dh = {0, 0, SIGNAL_LAMP_SIZE * 0.4f};
				this->AddFlatQuad({c - dw - dh, c + dw - dh, c + dw + dh, c - dw + dh}, barred ? SIGNAL_RED_LIT : SIGNAL_RED_DARK, 1.0f, barred);
			}
			/* The cross on top. */
			const Vec3 mid = foot + face * 0.14f + Vec3{0, 0, CROSSING_POST_HEIGHT - 0.4f};
			for (float tilt : {-1.0f, 1.0f}) {
				const Vec3 arm = (side + Vec3{0, 0, tilt * 0.6f}).Normalised() * 1.1f;
				const Vec3 width = (side * (-tilt * 0.6f) + Vec3{0, 0, 1}).Normalised() * 0.18f;
				this->AddFlatQuad({mid - arm - width, mid + arm - width, mid + arm + width, mid - arm + width}, CROSSING_CROSS_COLOUR, 1.0f);
			}
		}
	}
}

/**
 * Work out where a fence along an axis stands in its sprite, from its bottom, which stands on the ground. The isometric
 * view draws a point of the fence at the row along + across - z, and at the column 2 * (across - along) for a fence
 * along the x axis, 2 * (along - across) for one along the y axis. With the ground climbing along the fence, the line
 * of the bottom of each column, its row plus or minus (1 - climb) times half its column, is then the same: it is
 * (2 - climb) times across, less how high the ground is where the fence starts along the axis.
 * @param tex The texture of the sprite, see #Renderer::GetBoxTexture.
 * @param axis The direction of the fence.
 * @param climb How much the ground rises per unit along the fence.
 * @return Where the fence stands; nothing when the sprite has no pixels.
 */
static std::optional<FenceFit> FitFence(const GroundTexture &tex, Axis axis, float climb)
{
	const Picture &pic = tex.levels[0];
	if (pic.alpha.empty()) return std::nullopt;
	std::vector<float> lines;
	float height = 0;
	int left = pic.width;
	int right = 0;
	for (int x = 0; x < pic.width; x++) {
		int top = -1;
		int bottom = -1;
		for (int y = 0; y < pic.height; y++) {
			if (pic.alpha[static_cast<size_t>(y) * pic.width + x] < 0.5f) continue;
			if (top < 0) top = y;
			bottom = y;
		}
		if (top < 0) continue;
		const float col = x + tex.left + 0.5f;
		const float row = bottom + tex.top + 0.5f;
		lines.push_back(row + (axis == AXIS_X ? 1.0f : -1.0f) * (1 - climb) * col / 2);
		height = std::max(height, static_cast<float>(bottom - top + 1));
		left = std::min(left, x);
		right = std::max(right, x + 1);
	}
	if (lines.empty()) return std::nullopt;
	/* Between its posts, the bottom of a fence may be a rail above the ground; its lowest parts stand on it. */
	const size_t lowest = lines.size() * 9 / 10;
	std::nth_element(lines.begin(), lines.begin() + lowest, lines.end());
	return FenceFit{lines[lowest], height, static_cast<float>(left + tex.left), static_cast<float>(right + tex.left)};
}

/**
 * Get where a fence stands in its sprite, see #FitFence.
 * @param image The sprite of the fence.
 * @param pal Its palette.
 * @param axis The direction of the fence.
 * @param climb How much the ground rises per unit along the fence.
 * @return Where the fence stands; nothing when the sprite has no pixels.
 */
const std::optional<FenceFit> &Renderer::GetFenceFit(SpriteID image, PaletteID pal, Axis axis, float climb)
{
	const auto key = std::make_tuple(image & SPRITE_MASK, pal, static_cast<int>(axis), static_cast<int>(std::lround(climb * 16)));
	auto it = this->fence_fits.find(key);
	if (it == this->fence_fits.end()) it = this->fence_fits.emplace(key, FitFence(this->GetBoxTexture(image, pal, false), axis, climb)).first;
	return it->second;
}

/**
 * Add a fence along a side of a tile, like the hedges around fields and the fences along railways: an upright cut-out
 * following the ground, dressed in its sprite and seen from both sides, with its height scaled like those of buildings.
 * @param box The sprite of the fence.
 * @param tile The tile.
 * @param axis The direction of the fence.
 * @param edge The side of the tile whose ground the sprite stands on, across the fence: 0 or #TILE.
 * @param corner_z Height of the corners of the ground of the tile.
 * @param split_we How the ground of the tile is split into triangles, see #SurfaceZ.
 */
void Renderer::AddFence(const BoxSpriteToDraw &box, TileIndex tile, Axis axis, float edge, const std::array<float, CORNER_END> &corner_z, bool split_we)
{
	const float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	const float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	/* Coordinates along and across the fence. */
	const float a0 = axis == AXIS_X ? x0 : y0;
	const float c0 = axis == AXIS_X ? y0 : x0;
	auto point = [axis](float a, float c, float z) { return axis == AXIS_X ? Vec3{a, c, z} : Vec3{c, a, z}; };
	/* Height of the ground at a point of the tile, relative to its north corner. */
	auto ground = [&](float a, float c) { return axis == AXIS_X ? SurfaceZ(corner_z, split_we, a, c) : SurfaceZ(corner_z, split_we, c, a); };

	const std::array<float, 2> base = {ground(0, edge), ground(TILE, edge)};
	const float climb = (base[1] - base[0]) / TILE;
	const std::optional<FenceFit> &fit = this->GetFenceFit(box.image, box.pal, axis, climb);
	if (!fit.has_value()) return;
	const float sx = static_cast<float>(box.x) / ZOOM_BASE;
	const float sy = static_cast<float>(box.y) / ZOOM_BASE;
	const float line = fit->line + sy + (axis == AXIS_X ? 1.0f : -1.0f) * (1 - climb) * sx / 2;
	/* Where the sprite shows the fence across the tile, and where it stands in the 3D view. */
	const float shown = (line + base[0] - climb * a0) / (2 - climb);
	const float plane = std::clamp(shown, c0 + FENCE_INSET, c0 + TILE - FENCE_INSET);

	/* Where the sprite shows the ends of the fence along the tile; a fence that ends a little short of the sides of the
	 * tile is stretched to them, so the fences of tiles next to each other join. */
	std::array<float, 2> ends;
	if (axis == AXIS_X) {
		/* Along the x axis, the columns go the other way. */
		ends = {shown - (fit->right + sx) / 2, shown - (fit->left + sx) / 2};
	} else {
		ends = {shown + (fit->left + sx) / 2, shown + (fit->right + sx) / 2};
	}
	ends[0] = std::clamp(ends[0], a0, a0 + 2);
	ends[1] = std::clamp(ends[1], a0 + TILE - 2, a0 + TILE);

	/* The pixels at the bottom reach half a row below the ground. */
	const float low = -0.5f;
	const float high = fit->height;
	const std::array<float, 2> foot = {ground(0, plane - c0), ground(TILE, plane - c0)};
	const std::array<float, 2> pictured_base = {base[0] + climb * (ends[0] - a0), base[0] + climb * (ends[1] - a0)};
	const std::array<Vec3, 4> world = {
		point(a0, plane, foot[0] + low * BUILDING_HEIGHT), point(a0 + TILE, plane, foot[1] + low * BUILDING_HEIGHT),
		point(a0 + TILE, plane, foot[1] + high * BUILDING_HEIGHT), point(a0, plane, foot[0] + high * BUILDING_HEIGHT),
	};
	const std::array<Vec3, 4> pictured = {
		point(ends[0], shown, pictured_base[0] + low), point(ends[1], shown, pictured_base[1] + low),
		point(ends[1], shown, pictured_base[1] + high), point(ends[0], shown, pictured_base[0] + high),
	};
	const GroundTexture &tex = this->GetSideTexture(box.image, box.pal, axis, climb, true);
	const Vec3 normal = point(0, 1, 0);
	this->AddTexturedFace(tex, sx, sy, world, pictured, normal, true);
	this->AddTexturedFace(tex, sx, sy, world, pictured, normal * -1, true);
}

/**
 * Add a fence of posts and rails, like those along railways, between two points of a tile, following its ground.
 * @param tile The tile.
 * @param from Where the fence starts, relative to the north corner of the tile.
 * @param to Where the fence ends, relative to the north corner of the tile.
 * @param corner_z Height of the corners of the ground of the tile.
 * @param split_we How the ground of the tile is split into triangles, see #SurfaceZ.
 * @param colour Colour of the rails; the posts are darker.
 */
void Renderer::AddPostFence(TileIndex tile, std::pair<float, float> from, std::pair<float, float> to, const std::array<float, CORNER_END> &corner_z, bool split_we, const RGB &colour)
{
	const float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	const float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	const float dx = to.first - from.first;
	const float dy = to.second - from.second;
	const int posts = std::max(1, static_cast<int>(std::lround(std::hypot(dx, dy) / FENCE_POST_SPACING)));
	auto at = [&](float t) {
		const float x = from.first + dx * t;
		const float y = from.second + dy * t;
		return Vec3{x0 + x, y0 + y, SurfaceZ(corner_z, split_we, x, y)};
	};

	/* The posts stand in the middle of equal parts of the fence, so those of fences next to each other are as far apart. */
	std::vector<Vec3> points = {at(0)};
	constexpr float HALF_POST = FENCE_POST_SIZE / 2;
	for (int i = 0; i < posts; i++) {
		const Vec3 p = at((i + 0.5f) / posts);
		points.push_back(p);
		this->AddFlatBox({p.x - HALF_POST, p.y - HALF_POST, p.z - 0.5f}, {p.x + HALF_POST, p.y + HALF_POST, p.z + FENCE_HEIGHT + HALF_POST}, colour * 0.7f);
	}
	points.push_back(at(1));

	/* Two rails, along the top and lower down, bending with the ground at the posts. */
	for (size_t i = 0; i + 1 < points.size(); i++) {
		const Vec3 &a = points[i];
		const Vec3 &b = points[i + 1];
		for (float h : {FENCE_HEIGHT - FENCE_RAIL_SIZE, FENCE_HEIGHT * 0.35f}) {
			const Vec3 lo = {0, 0, h};
			const Vec3 hi = {0, 0, h + FENCE_RAIL_SIZE};
			this->AddFlatQuad({a + lo, b + lo, b + hi, a + hi}, colour, 1.0f);
		}
	}
}

/**
 * Add the fences around a field, like hedges and stone walls, see DrawClearLandFence.
 * @param tile The tile.
 * @param corner_z Height of the corners of the ground of the tile.
 * @param split_we How the ground of the tile is split into triangles, see #SurfaceZ.
 */
void Renderer::AddFieldFences(TileIndex tile, const std::array<float, CORNER_END> &corner_z, bool split_we)
{
	if (!IsClearGround(tile, CLEAR_FIELDS) || IsSnowTile(tile)) return;
	/* The sides with a fence, in the order the game draws their sprites, with the side of the tile whose ground each sprite stands on. */
	static const std::array<std::tuple<DiagDirection, Axis, float>, DIAGDIR_END> sides = {{
		{DIAGDIR_NW, AXIS_X, 0.0f}, {DIAGDIR_NE, AXIS_Y, 0.0f}, {DIAGDIR_SW, AXIS_Y, TILE}, {DIAGDIR_SE, AXIS_X, TILE},
	}};
	auto box = this->box_sprites.begin();
	for (const auto &[side, axis, edge] : sides) {
		if (GetFence(tile, side) == 0) continue;
		if (box == this->box_sprites.end() || box->bridge) return;
		this->AddFence(*box, tile, axis, edge, corner_z, split_we);
		++box;
	}
}

/**
 * Add a fence of a railway, where the viewport draws the sprite of one, see DrawTrackDetails.
 * @param tile The tile.
 * @param box The sprite and its box.
 * @param corner_z Height of the corners of the ground of the tile.
 * @param split_we How the ground of the tile is split into triangles, see #SurfaceZ.
 * @param dist Distance of the tile from the camera.
 * @return Whether the box is that of a fence; it is then drawn, when it is close enough.
 */
bool Renderer::AddRailFence(TileIndex tile, const BoxSpriteToDraw &box, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist)
{
	if (!IsPlainRailTile(tile)) return false;
	switch (GetRailGroundType(tile)) {
		case RailGroundType::FenceNW:
		case RailGroundType::FenceSE:
		case RailGroundType::FenceSENW:
		case RailGroundType::FenceNE:
		case RailGroundType::FenceSW:
		case RailGroundType::FenceNESW:
		case RailGroundType::FenceVert1:
		case RailGroundType::FenceVert2:
		case RailGroundType::FenceHoriz1:
		case RailGroundType::FenceHoriz2:
		case RailGroundType::HalfTileWater:
			break;

		default:
			return false;
	}

	/* The boxes of the fences are thin, along a side of the tile, or a dot in its middle for those along a diagonal. */
	constexpr int HALF = TILE_SIZE / 2;
	const int x = box.xmin - static_cast<int>(TileX(tile) * TILE_SIZE);
	const int y = box.ymin - static_cast<int>(TileY(tile) * TILE_SIZE);
	std::pair<float, float> from, to;
	if (box.ymin == box.ymax && box.xmax - box.xmin >= HALF) {
		const float side = y < HALF ? FENCE_INSET : TILE - FENCE_INSET;
		from = {0.0f, side};
		to = {TILE, side};
	} else if (box.xmin == box.xmax && box.ymax - box.ymin >= HALF) {
		const float side = x < HALF ? FENCE_INSET : TILE - FENCE_INSET;
		from = {side, 0.0f};
		to = {side, TILE};
	} else if (box.xmin == box.xmax && box.ymin == box.ymax && x == HALF && y == HALF) {
		/* An upright sprite is a fence from the north corner to the south one, a flat one from the west corner to the east one. */
		const Picture &pic = this->GetBoxTexture(box.image, box.pal, false).levels[0];
		if (pic.height > pic.width) {
			from = {0.0f, 0.0f};
			to = {TILE, TILE};
		} else {
			from = {TILE, 0.0f};
			to = {0.0f, TILE};
		}
	} else {
		return false;
	}
	/* Its colour is that of the sprite. */
	if (dist <= FENCE_DIST) this->AddPostFence(tile, from, to, corner_z, split_we, this->GetBoxTexture(box.image, box.pal, false).mean);
	return true;
}

/**
 * Find where a side of a bridge stands across the bridge, from the bottom of its sprite, which stands on the deck.
 * @param tex The texture of the sprite of the side, see #Renderer::GetBoxTexture.
 * @param sx Screen column where the viewport draws the sprite, in normal zoom pixels.
 * @param sy Screen row where the viewport draws the sprite, in normal zoom pixels.
 * @param axis The axis of the bridge.
 * @param deck Height of the deck.
 * @return Where the side stands: its y for a bridge along the x axis, its x otherwise; nothing when the sprite has no pixels.
 */
static std::optional<float> FindBridgeSide(const GroundTexture &tex, float sx, float sy, Axis axis, float deck)
{
	const Picture &pic = tex.levels[0];
	if (pic.alpha.empty()) return std::nullopt;
	std::vector<float> across;
	for (int x = 0; x < pic.width; x++) {
		for (int y = pic.height - 1; y >= 0; y--) {
			if (pic.alpha[static_cast<size_t>(y) * pic.width + x] < 0.5f) continue;
			/* The isometric view draws a point at the column 2 * (y - x) and the row x + y - z. */
			const float col = x + tex.left + 0.5f + sx;
			const float sum = y + tex.top + 0.5f + sy + deck;
			across.push_back(axis == AXIS_X ? (sum + col / 2) / 2 : (sum - col / 2) / 2);
			break;
		}
	}
	if (across.empty()) return std::nullopt;
	std::nth_element(across.begin(), across.begin() + across.size() / 2, across.end());
	return across[across.size() / 2];
}

/**
 * Get the texture of an upright side along an axis, like a side of a bridge or a fence: a cut-out of thin lines along
 * it and up. In the sprite, lines along the side are steps of pixels, which would be teeth in 3D. The opacity is
 * averaged along the side over a whole step, which makes them straight, and raised a little, so lines a pixel thin stay.
 * @param image The sprite of the side.
 * @param pal Its palette.
 * @param axis The direction of the side.
 * @param climb How much the side rises per unit along the axis, like a fence on a slope.
 * @param detail Whether the colours of the opaque pixels of the sprite stay as they are, like the leaves of a hedge,
 *               and only its outline is averaged.
 * @return The texture; without any pixels when the sprite has none.
 */
const GroundTexture &Renderer::GetSideTexture(SpriteID image, PaletteID pal, Axis axis, float climb, bool detail)
{
	/* Starting with a marker that no other texture has. */
	this->key.clear();
	this->key.push_back(INT32_MIN + 1);
	this->key.push_back(image & SPRITE_MASK);
	this->key.push_back(pal);
	this->key.push_back(axis);
	this->key.push_back(static_cast<int32_t>(std::lround(climb * 16)));
	this->key.push_back(detail);
	auto it = this->textures.find(this->key);
	if (it != this->textures.end()) return it->second;
	GroundTexture &tex = this->textures[this->key];

	const RawSprite &raw = this->GetRawSprite(image & SPRITE_MASK);
	if (raw.pixels.empty()) return tex;
	/* Room around the sprite for the averaging to spread into. */
	constexpr int MARGIN = 2;
	Picture painted;
	painted.width = raw.width + 2 * MARGIN;
	painted.height = raw.height + 2 * MARGIN;
	painted.pixels.assign(static_cast<size_t>(painted.width) * painted.height, {0, 0, 0});
	std::vector<float> painted_alpha(painted.pixels.size(), 0.0f);
	PaintSprite(painted, painted_alpha, raw, pal, MARGIN, MARGIN);

	/* Along the side, the isometric view goes two columns and a row per unit, less how much it climbs. */
	constexpr int SAMPLES = 16;
	const float dx = 2.0f;
	const float dy = (axis == AXIS_X ? -1.0f : 1.0f) * (1 - climb);
	tex.left = raw.x_offs - MARGIN;
	tex.top = raw.y_offs - MARGIN;
	Picture &pic = tex.levels[0];
	pic.width = painted.width;
	pic.height = painted.height;
	pic.pixels.assign(painted.pixels.size(), {0, 0, 0});
	std::vector<float> alpha(painted.pixels.size(), 0.0f);
	for (int y = 0; y < pic.height; y++) {
		for (int x = 0; x < pic.width; x++) {
			RGB sum = {0, 0, 0};
			float alpha_sum = 0;
			for (int i = 0; i < SAMPLES; i++) {
				const float t = (i + 0.5f) / SAMPLES - 0.5f;
				const int px = static_cast<int>(std::floor(x + 0.5f + dx * t));
				const int py = static_cast<int>(std::floor(y + 0.5f + dy * t));
				if (px < 0 || py < 0 || px >= pic.width || py >= pic.height) continue;
				const size_t idx = static_cast<size_t>(py) * pic.width + px;
				sum = sum + painted.pixels[idx];
				alpha_sum += painted_alpha[idx];
			}
			const size_t idx = static_cast<size_t>(y) * pic.width + x;
			alpha[idx] = alpha_sum / SAMPLES;
			if (detail && painted_alpha[idx] >= 0.5f) {
				/* Premultiplied, like the averages. */
				pic.pixels[idx] = painted.pixels[idx] * (alpha[idx] / painted_alpha[idx]);
			} else {
				pic.pixels[idx] = sum * (1.0f / SAMPLES);
			}
		}
	}
	FinishTexture(tex, std::move(alpha), 4, true, {0, 0}, false);
	for (Picture &level : tex.levels) {
		for (float &a : level.alpha) a = std::min(1.0f, a * BRIDGE_SIDE_BOOST);
	}
	return tex;
}

/**
 * Add a side of a bridge, standing on the edge of its deck: an upright cut-out along the bridge, dressed in the
 * sprite of the side, with its heights scaled like those of buildings and vehicles. It is seen from both sides.
 * @param tex The texture of the side, see #Renderer::GetSideTexture.
 * @param sx Screen column where the viewport draws the sprite, in normal zoom pixels.
 * @param sy Screen row where the viewport draws the sprite, in normal zoom pixels.
 * @param axis The axis of the bridge.
 * @param plane Where the side stands across the bridge: its y for a bridge along the x axis, its x otherwise.
 * @param shown_plane Where the side the sprite shows stands; for a side the sprite does not show, the opposite one.
 * @param along Where the side starts and ends along the bridge.
 * @param deck Height of the deck where the side starts and ends.
 */
void Renderer::AddBridgeSide(const GroundTexture &tex, float sx, float sy, Axis axis, float plane, float shown_plane, std::pair<float, float> along, std::pair<float, float> deck)
{
	std::optional<int> top = TopRow(tex);
	if (!top.has_value()) return;
	/* The isometric view draws a point at the row x + y - z; that gives how high the top of the sprite is above the deck. */
	const float height = std::max(along.first - deck.first, along.second - deck.second) + shown_plane - *top - sy;
	if (height < 1) return;
	/* Some sides reach down the edge of the deck. */
	const float low = -DECK_DEPTH / BUILDING_HEIGHT;

	auto point = [axis](float a, float c, float z) { return axis == AXIS_X ? Vec3{a, c, z} : Vec3{c, a, z}; };
	const std::array<Vec3, 4> world = {
		point(along.first, plane, deck.first + low * BUILDING_HEIGHT), point(along.second, plane, deck.second + low * BUILDING_HEIGHT),
		point(along.second, plane, deck.second + height * BUILDING_HEIGHT), point(along.first, plane, deck.first + height * BUILDING_HEIGHT),
	};
	const std::array<Vec3, 4> shown = {
		point(along.first, shown_plane, deck.first + low), point(along.second, shown_plane, deck.second + low),
		point(along.second, shown_plane, deck.second + height), point(along.first, shown_plane, deck.first + height),
	};
	const Vec3 normal = point(0, 1, 0);
	this->AddTexturedFace(tex, sx, sy, world, shown, normal, true);
	this->AddTexturedFace(tex, sx, sy, world, shown, normal * -1, true);
}

/**
 * Add the part of a bridge above a tile: its deck with the track, dressed in the sprites of the bridge seen from
 * above, its sides cut out of the sprite of the near side, and pillars where the game draws them.
 * @param tile The tile under the bridge.
 * @param corner_z Height of the corners of the ground of the tile.
 * @param split_we How the ground of the tile is split into triangles, see #SurfaceZ.
 * @param dist Distance of the tile from the camera.
 */
void Renderer::AddBridgeMiddle(TileIndex tile, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist)
{
	const TileIndex head = GetSouthernBridgeEnd(tile);
	const Axis axis = GetBridgeAxis(tile);
	const float deck = static_cast<float>(GetBridgePixelHeight(head));
	const float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	const float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	/* Coordinates along and across the bridge. */
	const float a0 = axis == AXIS_X ? x0 : y0;
	const float c0 = axis == AXIS_X ? y0 : x0;
	auto point = [axis](float a, float c, float z) { return axis == AXIS_X ? Vec3{a, c, z} : Vec3{c, a, z}; };

	/* The sprites of the bridge: the near side, what is drawn with the deck, like the far side, and pillars below the deck. */
	std::vector<const BoxSpriteToDraw *> on_deck;
	const BoxSpriteToDraw *near_side = nullptr, *pillar = nullptr;
	for (const BoxSpriteToDraw &box : this->box_sprites) {
		if (!box.bridge) continue;
		if (box.zmax < deck - 1) {
			if (pillar == nullptr) pillar = &box;
			continue;
		}
		const int across = (axis == AXIS_X ? box.ymin : box.xmin) - static_cast<int>(c0);
		const bool side = (axis == AXIS_X ? box.ymax - box.ymin : box.xmax - box.xmin) == 0 && (axis == AXIS_X ? box.xmax - box.xmin : box.ymax - box.ymin) >= static_cast<int>(TILE_SIZE / 2);
		/* The near side, with what is drawn in its box. */
		if (side && across >= static_cast<int>(TILE_SIZE / 2)) {
			if (!box.child) near_side = &box;
			continue;
		}
		on_deck.push_back(&box);
	}
	if (on_deck.empty()) return;

	/* Where the sides stand: the near one where the bottom of its sprite is, the far one as far from the other edge of the tile. */
	float near_plane = c0 + BRIDGE_SIDE_PLANE;
	const GroundTexture *side = nullptr;
	float side_x = 0, side_y = 0;
	if (near_side != nullptr) {
		side_x = static_cast<float>(near_side->x) / ZOOM_BASE;
		side_y = static_cast<float>(near_side->y) / ZOOM_BASE;
		std::optional<float> found = FindBridgeSide(this->GetBoxTexture(near_side->image, near_side->pal, false), side_x, side_y, axis, deck);
		if (found.has_value()) near_plane = std::clamp(*found, c0 + TILE / 2 + 2, c0 + TILE);
		side = &this->GetSideTexture(near_side->image, near_side->pal, axis);
	}
	const float far_plane = 2 * c0 + TILE - near_plane;
	const float c_lo = std::min(c0 + BRIDGE_DECK_EDGE, far_plane - 0.5f);
	const float c_hi = std::max(c0 + TILE - BRIDGE_DECK_EDGE, near_plane + 0.5f);

	/* The deck, with the track on it. */
	const Point origin = RemapCoords(TileX(tile) * TILE_SIZE, TileY(tile) * TILE_SIZE, static_cast<int>(deck));
	const GroundTexture &tex = this->GetCompositeTexture(on_deck, origin);
	const float sx = static_cast<float>(origin.x) / ZOOM_BASE;
	const float sy = static_cast<float>(origin.y) / ZOOM_BASE;
	/* The sprites leave out the near edge of the deck, under the near side; the near half gets the picture of the far half. */
	const float c_mid = c0 + TILE / 2;
	const std::array<Vec3, 4> far_half = {point(a0, c_lo, deck), point(a0 + TILE, c_lo, deck), point(a0 + TILE, c_mid, deck), point(a0, c_mid, deck)};
	const std::array<Vec3, 4> near_half = {point(a0, c_mid, deck), point(a0 + TILE, c_mid, deck), point(a0 + TILE, c_hi, deck), point(a0, c_hi, deck)};
	const std::array<Vec3, 4> near_shown = {point(a0, c_mid, deck), point(a0 + TILE, c_mid, deck), point(a0 + TILE, 2 * c_mid - c_hi, deck), point(a0, 2 * c_mid - c_hi, deck)};
	this->AddTexturedFace(tex, sx, sy, far_half, far_half, {0, 0, 1}, false);
	this->AddTexturedFace(tex, sx, sy, near_half, near_shown, {0, 0, 1}, false);
	if (GetTunnelBridgeTransportType(head) == TRANSPORT_RAIL) {
		std::array<float, CORNER_END> flat;
		flat.fill(deck);
		this->AddTrack(tile, TrackToTrackBits(AxisToTrack(axis)), flat, true, dist);
	}

	/* Its edges and its bottom, in the colour of the pillars; aqueducts are of stone, with walls along the water. */
	const bool aqueduct = GetTunnelBridgeTransportType(head) == TRANSPORT_WATER;
	const RGB colour = aqueduct ? AQUEDUCT_COLOUR : pillar != nullptr ? this->GetBoxTexture(pillar->image, pillar->pal, false).mean : tex.mean * 0.8f;
	if (aqueduct) this->AddAqueductWalls(axis, a0, {c_lo, c_hi}, {deck, deck});
	const float under = deck - DECK_DEPTH;
	for (float c : {c_lo, c_hi}) {
		this->AddFlatQuad({point(a0, c, under), point(a0 + TILE, c, under), point(a0 + TILE, c, deck), point(a0, c, deck)}, colour, 1.0f);
	}
	this->AddFlatQuad({point(a0, c_lo, under), point(a0 + TILE, c_lo, under), point(a0 + TILE, c_hi, under), point(a0, c_hi, under)}, colour * 0.55f, 1.0f, true);

	/* Its sides; the far one, seen from the inside in the sprites, gets the picture of the near one. */
	if (side != nullptr) {
		this->AddBridgeSide(*side, side_x, side_y, axis, near_plane, near_plane, {a0, a0 + TILE}, {deck, deck});
		this->AddBridgeSide(*side, side_x, side_y, axis, far_plane, near_plane, {a0, a0 + TILE}, {deck, deck});
	}

	/* Two pillars with a beam across them, under the middle of the tile; not where a railway, road or water passes under the bridge. */
	if (pillar != nullptr && (IsTileType(tile, MP_CLEAR) || IsTileType(tile, MP_TREES))) {
		this->AddBridgePillars(tile, axis, a0 + TILE / 2, {c_lo, c_hi}, under, corner_z, split_we, colour);
	}
}

/**
 * Add the pillars under a bridge, with a beam across them below the deck.
 * @param tile The tile they stand on.
 * @param axis The axis of the bridge.
 * @param along Where they stand along the bridge.
 * @param across Where the deck starts and ends across the bridge.
 * @param under Height of the bottom of the deck.
 * @param corner_z Height of the corners of the ground of the tile.
 * @param split_we How the ground of the tile is split into triangles, see #SurfaceZ.
 * @param colour Their colour.
 */
void Renderer::AddBridgePillars(TileIndex tile, Axis axis, float along, std::pair<float, float> across, float under, const std::array<float, CORNER_END> &corner_z, bool split_we, const RGB &colour)
{
	const float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	const float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	auto point = [axis](float a, float c, float z) { return axis == AXIS_X ? Vec3{a, c, z} : Vec3{c, a, z}; };
	const float half = PILLAR_SIZE / 2;
	for (float c : {across.first + half + 0.3f, across.second - half - 0.3f}) {
		const Vec3 p = point(along, c, 0);
		const float ground = SurfaceZ(corner_z, split_we, p.x - x0, p.y - y0);
		if (ground > under - 0.5f) continue;
		this->AddFlatBox({p.x - half, p.y - half, ground - 0.5f}, {p.x + half, p.y + half, under}, colour);
	}
	const Vec3 lo = point(along - half, across.first, under - 1.2f);
	const Vec3 hi = point(along + half, across.second, under);
	this->AddFlatBox({std::min(lo.x, hi.x), std::min(lo.y, hi.y), lo.z}, {std::max(lo.x, hi.x), std::max(lo.y, hi.y), hi.z}, colour);
}

/**
 * Add the walls along the water of an aqueduct, on the edges of its deck along a tile.
 * @param axis The axis of the aqueduct.
 * @param a0 Where the tile starts along the aqueduct.
 * @param across Where the deck starts and ends across the aqueduct.
 * @param deck Height of the deck where the tile starts and ends along the aqueduct.
 */
void Renderer::AddAqueductWalls(Axis axis, float a0, std::pair<float, float> across, std::pair<float, float> deck)
{
	auto point = [axis](float a, float c, float z) { return axis == AXIS_X ? Vec3{a, c, z} : Vec3{c, a, z}; };
	const float h = AQUEDUCT_WALL_HEIGHT;
	for (float c : {across.first, across.second - AQUEDUCT_WALL}) {
		const float c1 = c + AQUEDUCT_WALL;
		for (float side : {c, c1}) {
			this->AddFlatQuad({point(a0, side, deck.first), point(a0 + TILE, side, deck.second), point(a0 + TILE, side, deck.second + h), point(a0, side, deck.first + h)}, AQUEDUCT_COLOUR, 1.0f);
		}
		this->AddFlatQuad({point(a0, c, deck.first + h), point(a0 + TILE, c, deck.second + h), point(a0 + TILE, c1, deck.second + h), point(a0, c1, deck.first + h)}, AQUEDUCT_COLOUR, 1.0f);
	}
}

/**
 * Add the ramp of a bridge: a deck like that of the bridge, rising from the ground to the bridge with the track on
 * it and dressed in the sprites of the ramp, on pillars where it meets the bridge.
 * @param tile The bridge ramp.
 * @param ground_z Height of the tile as the viewport uses it, see #GetTilePixelSlope.
 * @param corner_z Height of the corners of the ground of the tile.
 * @param split_we How the ground of the tile is split into triangles, see #SurfaceZ.
 * @param dist Distance of the tile from the camera.
 */
void Renderer::AddBridgeRamp(TileIndex tile, int ground_z, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist)
{
	const DiagDirection dir = GetTunnelBridgeDirection(tile);
	const Axis axis = DiagDirToAxis(dir);
	const float deck = static_cast<float>(GetBridgePixelHeight(tile));
	const float outer = HasBridgeFlatRamp(GetTileSlope(tile), axis) ? deck : deck - TILE_HEIGHT;

	/* The track; the deck is as high as the bridge at the edge towards it. */
	static const std::array<std::pair<Corner, Corner>, DIAGDIR_END> inner_corners = {{
		{CORNER_N, CORNER_E}, {CORNER_E, CORNER_S}, {CORNER_W, CORNER_S}, {CORNER_N, CORNER_W},
	}};
	std::array<float, CORNER_END> ramp_z;
	ramp_z.fill(outer);
	ramp_z[inner_corners[dir].first] = deck;
	ramp_z[inner_corners[dir].second] = deck;
	this->AddTrack(tile, GetTileRailTracks(tile), ramp_z, true, dist);

	/* Coordinates along and across the bridge, and the height of the deck where the tile starts and ends along it. */
	const float a0 = static_cast<float>((axis == AXIS_X ? TileX(tile) : TileY(tile)) * TILE_SIZE);
	const float c0 = static_cast<float>((axis == AXIS_X ? TileY(tile) : TileX(tile)) * TILE_SIZE);
	auto point = [axis](float a, float c, float z) { return axis == AXIS_X ? Vec3{a, c, z} : Vec3{c, a, z}; };
	const bool rising = dir == DIAGDIR_SE || dir == DIAGDIR_SW;
	const float z0 = rising ? outer : deck;
	const float z1 = rising ? deck : outer;
	const float c_lo = c0 + RAMP_EDGE;
	const float c_mid = c0 + TILE / 2;
	const float c_hi = c0 + TILE - RAMP_EDGE;

	/* The deck; its near edge is outside the sprites, so the near half gets the picture of the far half. */
	std::vector<const BoxSpriteToDraw *> parts;
	for (const BoxSpriteToDraw &box : this->box_sprites) {
		if (!box.bridge) parts.push_back(&box);
	}
	RGB colour = FOUNDATION_COLOUR;
	const std::array<Vec3, 4> far_half = {point(a0, c_lo, z0), point(a0 + TILE, c_lo, z1), point(a0 + TILE, c_mid, z1), point(a0, c_mid, z0)};
	const std::array<Vec3, 4> near_half = {point(a0, c_mid, z0), point(a0 + TILE, c_mid, z1), point(a0 + TILE, c_hi, z1), point(a0, c_hi, z0)};
	if (parts.empty()) {
		this->AddFlatQuad(far_half, colour, 1.0f);
		this->AddFlatQuad(near_half, colour, 1.0f);
	} else {
		const Point origin = RemapCoords(TileX(tile) * TILE_SIZE, TileY(tile) * TILE_SIZE, ground_z);
		const GroundTexture &tex = this->GetCompositeTexture(parts, origin);
		const float sx = static_cast<float>(origin.x) / ZOOM_BASE;
		const float sy = static_cast<float>(origin.y) / ZOOM_BASE;
		const std::array<Vec3, 4> near_shown = {point(a0, c_mid, z0), point(a0 + TILE, c_mid, z1), point(a0 + TILE, 2 * c_mid - c_hi, z1), point(a0, 2 * c_mid - c_hi, z0)};
		this->AddTexturedFace(tex, sx, sy, far_half, far_half, {0, 0, 1}, false);
		this->AddTexturedFace(tex, sx, sy, near_half, near_shown, {0, 0, 1}, false);
		colour = tex.mean * 0.8f;
	}

	if (GetTunnelBridgeTransportType(tile) == TRANSPORT_WATER) {
		colour = AQUEDUCT_COLOUR;
		this->AddAqueductWalls(axis, a0, {c_lo, c_hi}, {z0, z1});
	}

	/* Its edges and its bottom. */
	const float d = DECK_DEPTH;
	for (float c : {c_lo, c_hi}) {
		this->AddFlatQuad({point(a0, c, z0 - d), point(a0 + TILE, c, z1 - d), point(a0 + TILE, c, z1), point(a0, c, z0)}, colour, 1.0f);
	}
	this->AddFlatQuad({point(a0, c_lo, z0 - d), point(a0 + TILE, c_lo, z1 - d), point(a0 + TILE, c_hi, z1 - d), point(a0, c_hi, z0 - d)}, colour * 0.55f, 1.0f, true);
	const float a_inner = rising ? a0 + TILE : a0;
	this->AddFlatQuad({point(a_inner, c_lo, deck - d), point(a_inner, c_hi, deck - d), point(a_inner, c_hi, deck), point(a_inner, c_lo, deck)}, colour, 1.0f);

	/* The pillars where it meets the bridge. */
	this->AddBridgePillars(tile, axis, rising ? a_inner - PILLAR_SIZE : a_inner + PILLAR_SIZE, {c_lo, c_hi}, deck - d, corner_z, split_we, colour);
}

/**
 * Get where the face of the portal of a tunnel is, and which way the tunnel goes from it.
 * @param tile The tunnel entrance.
 * @param z Height of the bottom of the tile.
 * @return The middle of the bottom of the face, where the hill starts; the direction into the tunnel; and the direction to its left.
 */
static std::tuple<Vec3, Vec3, Vec3> GetTunnelFace(TileIndex tile, int z)
{
	static const std::array<std::pair<float, float>, DIAGDIR_END> into = {{{-1, 0}, {0, 1}, {1, 0}, {0, -1}}};
	const auto [ix, iy] = into[GetTunnelBridgeDirection(tile)];
	const Vec3 in = {ix, iy, 0};
	const Vec3 face = Vec3{TileX(tile) * TILE + TILE / 2, TileY(tile) * TILE + TILE / 2, static_cast<float>(z)} + in * (TILE / 2);
	return {face, in, Vec3{-iy, ix, 0}};
}

/**
 * Get a point of the arch over the opening of a tunnel.
 * @param i Which point, from 0 at one side over the top to #PORTAL_SEGMENTS at the other.
 * @param radius Radius of the arch.
 * @return How far the point is to the side of the middle of the track, and how high above it.
 */
static std::pair<float, float> GetArchPoint(int i, float radius)
{
	const float t = static_cast<float>(M_PI) * i / PORTAL_SEGMENTS;
	return {-std::cos(t) * radius, PORTAL_SPRING + std::sin(t) * radius};
}

/**
 * Add the portal of a tunnel where the hill starts: a face of stone with an arched opening into the dark,
 * and walls along the track, where the 3D view cuts the hill away to keep the track level, see SetTunnelGround.
 * When the camera is in line with the tunnel, the opening shows the inside of the tunnel instead of darkness.
 * @param tile The tunnel entrance.
 * @param z Height of the bottom of the tile.
 */
void Renderer::AddTunnelPortal(TileIndex tile, int z)
{
	const auto [face, in, side] = GetTunnelFace(tile, z);
	/* A point s to the side and h above the bottom of the portal, d into the hill. */
	auto at = [&](float s, float h, float d = 0) { return face + side * s + in * d + Vec3{0, 0, h}; };
	const float half = TILE / 2;

	/* The arch of the opening, from one side over the top to the other, and the ring of stones around it. */
	constexpr int SEGMENTS = PORTAL_SEGMENTS;
	std::array<std::pair<float, float>, SEGMENTS + 1> arch, ring;
	for (int i = 0; i <= SEGMENTS; i++) {
		arch[i] = GetArchPoint(i, PORTAL_OPENING);
		ring[i] = GetArchPoint(i, PORTAL_OPENING + PORTAL_RING);
	}
	const float edge = PORTAL_OPENING + PORTAL_RING;

	/* The face: beside the ring, from the ground up, and above it. */
	this->AddFlatQuad({at(-half, 0), at(-edge, 0), at(-edge, PORTAL_HEIGHT), at(-half, PORTAL_HEIGHT)}, PORTAL_COLOUR, 1.0f);
	this->AddFlatQuad({at(edge, 0), at(half, 0), at(half, PORTAL_HEIGHT), at(edge, PORTAL_HEIGHT)}, PORTAL_COLOUR, 1.0f);
	for (int i = 0; i < SEGMENTS; i++) {
		const auto [s0, h0] = ring[i];
		const auto [s1, h1] = ring[i + 1];
		this->AddFlatQuad({at(s0, h0), at(s1, h1), at(s1, PORTAL_HEIGHT), at(s0, PORTAL_HEIGHT)}, PORTAL_COLOUR, 1.0f);
	}
	/* The ring, down the sides of the opening and over it. */
	this->AddFlatQuad({at(-edge, 0), at(-PORTAL_OPENING, 0), at(-PORTAL_OPENING, PORTAL_SPRING), at(-edge, PORTAL_SPRING)}, PORTAL_RING_COLOUR, 1.0f);
	this->AddFlatQuad({at(PORTAL_OPENING, 0), at(edge, 0), at(edge, PORTAL_SPRING), at(PORTAL_OPENING, PORTAL_SPRING)}, PORTAL_RING_COLOUR, 1.0f);
	for (int i = 0; i < SEGMENTS; i++) {
		this->AddFlatQuad({at(arch[i].first, arch[i].second), at(ring[i].first, ring[i].second), at(ring[i + 1].first, ring[i + 1].second), at(arch[i + 1].first, arch[i + 1].second)}, PORTAL_RING_COLOUR, 1.0f);
	}

	if (this->InLineWithTunnel(tile, z)) {
		this->AddTunnelInside(tile, z);
	} else {
		/* The walls and floor of the opening, going into the dark. */
		this->AddFlatQuad({at(-PORTAL_OPENING, 0), at(-PORTAL_OPENING, 0, PORTAL_DEPTH), at(-PORTAL_OPENING, PORTAL_SPRING, PORTAL_DEPTH), at(-PORTAL_OPENING, PORTAL_SPRING)}, TUNNEL_WALL_COLOUR, 1.0f, true);
		this->AddFlatQuad({at(PORTAL_OPENING, 0), at(PORTAL_OPENING, 0, PORTAL_DEPTH), at(PORTAL_OPENING, PORTAL_SPRING, PORTAL_DEPTH), at(PORTAL_OPENING, PORTAL_SPRING)}, TUNNEL_WALL_COLOUR, 1.0f, true);
		this->AddFlatQuad({at(-PORTAL_OPENING, 0), at(PORTAL_OPENING, 0), at(PORTAL_OPENING, 0, PORTAL_DEPTH), at(-PORTAL_OPENING, 0, PORTAL_DEPTH)}, TUNNEL_WALL_COLOUR, 1.0f, true);
		std::array<Vec3, SEGMENTS + 3> back;
		back[0] = at(-PORTAL_OPENING, 0, PORTAL_DEPTH);
		back[1] = at(PORTAL_OPENING, 0, PORTAL_DEPTH);
		for (int i = 0; i < SEGMENTS; i++) {
			this->AddFlatQuad({at(arch[i].first, arch[i].second), at(arch[i + 1].first, arch[i + 1].second), at(arch[i + 1].first, arch[i + 1].second, PORTAL_DEPTH), at(arch[i].first, arch[i].second, PORTAL_DEPTH)}, TUNNEL_WALL_COLOUR, 1.0f, true);
		}
		for (int i = 0; i <= SEGMENTS; i++) back[i + 2] = at(arch[SEGMENTS - i].first, arch[SEGMENTS - i].second, PORTAL_DEPTH);
		this->AddFlatPolygon(back, TUNNEL_DARK, 1.0f, true);
	}

	/* The top of the face, where it stands above the hill. */
	constexpr float THICK = 1.5f;
	const float hill = static_cast<float>(TILE_HEIGHT);
	this->AddFlatQuad({at(-half, PORTAL_HEIGHT), at(half, PORTAL_HEIGHT), at(half, PORTAL_HEIGHT, THICK), at(-half, PORTAL_HEIGHT, THICK)}, PORTAL_RING_COLOUR, 1.0f);
	this->AddFlatQuad({at(-half, hill, THICK), at(half, hill, THICK), at(half, PORTAL_HEIGHT, THICK), at(-half, PORTAL_HEIGHT, THICK)}, PORTAL_COLOUR, 1.0f);
	for (float s : {-half, half}) {
		this->AddFlatQuad({at(s, hill), at(s, hill, THICK), at(s, PORTAL_HEIGHT, THICK), at(s, PORTAL_HEIGHT)}, PORTAL_COLOUR, 1.0f);
		/* The wall along the track, up to the slope of the hill beside it. */
		this->AddFlatQuad({at(s, 0, -TILE), at(s, 0), at(s, hill + 0.8f), at(s, 0.8f, -TILE)}, PORTAL_COLOUR, 1.0f);
	}
}

/**
 * Check whether the camera is in line with a tunnel, in it or on the track leading into it, so it looks along it.
 * @param tile An entrance of the tunnel.
 * @param z Height of the bottom of the entrance tile.
 * @return True when the camera is within the opening of the tunnel, seen along it.
 */
bool Renderer::InLineWithTunnel(TileIndex tile, int z) const
{
	const auto [face, in, side] = GetTunnelFace(tile, z);
	const Vec3 rel = this->cam.pos - face;
	const float height = rel.z;
	return std::abs(rel.x * side.x + rel.y * side.y) < PORTAL_OPENING && height > 0 && height < PORTAL_SPRING + PORTAL_OPENING;
}

/**
 * Add the inside of a tunnel, for a camera in it or in line with it: a dark vault over the track or road, lighter
 * towards the entrances, where the daylight comes in. Tunnels are straight, so the far entrance shows as a bright opening.
 * @param tile An entrance of the tunnel.
 * @param z Height of the bottom of the entrance tile, where the track goes into the tunnel.
 */
void Renderer::AddTunnelInside(TileIndex tile, int z)
{
	const TileIndex end = GetOtherTunnelEnd(tile);
	if (std::ranges::find(this->tunnels, tile) != this->tunnels.end() || std::ranges::find(this->tunnels, end) != this->tunnels.end()) return;
	this->tunnels.push_back(tile);

	const auto [face, in, side] = GetTunnelFace(tile, z);
	const bool road = GetTunnelBridgeTransportType(tile) == TRANSPORT_ROAD;
	/* A point s to the side and h above the track or road, d into the tunnel from the face of the portal. */
	auto at = [&](float s, float h, float d) { return face + side * s + in * d + Vec3{0, 0, h}; };

	/* Only the part from a bit behind the camera to where the haze swallows everything. */
	const float length = (DistanceManhattan(tile, end) - 1) * TILE;
	const Vec3 rel = this->cam.pos - face;
	const float camera = rel.x * in.x + rel.y * in.y;
	const float from = std::max(0.0f, std::floor((camera - 2 * TILE) / TUNNEL_STEP) * TUNNEL_STEP);
	const float to = std::min(length, camera + this->fog_end);

	/* The cross section: the walls up to the arch, the arch, and down the other wall. */
	std::array<std::pair<float, float>, PORTAL_SEGMENTS + 3> profile;
	profile.front() = {-PORTAL_OPENING, 0};
	for (int i = 0; i <= PORTAL_SEGMENTS; i++) profile[i + 1] = GetArchPoint(i, PORTAL_OPENING);
	profile.back() = {PORTAL_OPENING, 0};

	for (float d0 = from; d0 < to; d0 += TUNNEL_STEP) {
		const float d1 = std::min(d0 + TUNNEL_STEP, to);
		const float middle = (d0 + d1) / 2;
		/* Daylight comes in at both ends; every other piece is darker, so the walls are seen to pass by. */
		const float day = std::exp(-std::min(middle, length - middle) / TUNNEL_GLOW);
		const float rib = static_cast<int>(d0 / TUNNEL_STEP) % 2 == 0 ? 1.0f : 0.8f;
		const RGB vault = (TUNNEL_VAULT_LIT * day + TUNNEL_VAULT_DARK * (1 - day)) * rib;
		for (size_t i = 0; i + 1 < profile.size(); i++) {
			const auto [s0, h0] = profile[i];
			const auto [s1, h1] = profile[i + 1];
			this->AddFlatQuad({at(s0, h0, d0), at(s1, h1, d0), at(s1, h1, d1), at(s0, h0, d1)}, vault, 1.0f, true);
		}
		const RGB floor = road ? TUNNEL_ROAD_LIT * day + TUNNEL_ROAD_DARK * (1 - day) : TUNNEL_FLOOR_LIT * day + TUNNEL_FLOOR_DARK * (1 - day);
		this->AddFlatQuad({at(-PORTAL_OPENING, 0, d0), at(PORTAL_OPENING, 0, d0), at(PORTAL_OPENING, 0, d1), at(-PORTAL_OPENING, 0, d1)}, floor, 1.0f, true);

		const float dist = std::abs(middle - camera);
		if (road) {
			/* The line in the middle of the road, which catches the light. */
			if (dist < RAIL_DIST) {
				const float hl = ROAD_LINE_LENGTH / 2, hw = ROAD_LINE_WIDTH / 2;
				for (float d = std::ceil(d0 / ROAD_LINE_SPACING) * ROAD_LINE_SPACING + ROAD_LINE_SPACING / 2; d < d1; d += ROAD_LINE_SPACING) {
					this->AddFlatQuad({at(-hw, 0.03f, d - hl), at(hw, 0.03f, d - hl), at(hw, 0.03f, d + hl), at(-hw, 0.03f, d + hl)}, ROAD_LINE_COLOUR * (0.15f + 0.85f * day), 1.0005f, true);
				}
			}
			continue;
		}
		if (dist < SLEEPER_DIST) {
			const float hl = SLEEPER_LENGTH / 2, hw = SLEEPER_WIDTH / 2;
			for (float d = d0 + SLEEPER_SPACING / 2; d < d1; d += SLEEPER_SPACING) {
				this->AddFlatQuad({at(-hl, 0.08f, d - hw), at(hl, 0.08f, d - hw), at(hl, 0.08f, d + hw), at(-hl, 0.08f, d + hw)}, SLEEPER_COLOUR * (0.2f + 0.8f * day), 1.0005f, true);
			}
		}
		if (dist < RAIL_DIST) {
			const RGB rail = TUNNEL_RAIL_LIT * day + TUNNEL_RAIL_DARK * (1 - day);
			const float hw = RAIL_WIDTH / 2;
			for (float s : {-RAIL_GAUGE / 2, RAIL_GAUGE / 2}) {
				this->AddFlatQuad({at(s - hw, 0.18f, d0), at(s + hw, 0.18f, d0), at(s + hw, 0.18f, d1), at(s - hw, 0.18f, d1)}, rail, 1.001f, true);
			}
		}
	}
}

/**
 * Add the vehicles around the camera: trains and road vehicles as boxes dressed in their sprites, aircraft as cut-outs.
 * @param own The vehicle the camera rides on, which is left out.
 */
void Renderer::AddVehicles(VehicleID own)
{
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VEH_TRAIN && v->type != VEH_ROAD && v->type != VEH_SHIP && v->type != VEH_AIRCRAFT) continue;
		if (v->vehstatus.Test(VehState::Hidden) || v->index == own) continue;
		float dx = v->x_pos - this->cam.pos.x;
		float dy = v->y_pos - this->cam.pos.y;
		if (std::hypot(dx, dy) > BOX_DIST) continue;
		if (dx * this->cam.fwd.x + dy * this->cam.fwd.y < -2 * TILE) continue;

		if (v->type == VEH_AIRCRAFT) {
			/* Shadows and rotors are vehicles of their own. */
			if (Aircraft::From(v)->IsNormalAircraft()) this->AddAircraft(Aircraft::From(v));
			continue;
		}
		if (v->type == VEH_SHIP) {
			this->AddShip(Ship::From(v));
			continue;
		}

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
		/* Road vehicles are drawn small for their length; widen them as much as they are made taller, but keep them in their lane. */
		if (v->type == VEH_ROAD) width = std::min(width * ROAD_VEHICLE_HEIGHT, ROAD_VEHICLE_MAX_WIDTH);
		float height = std::clamp(-*top - (length + width) / 2, 2.0f, 40.0f);

		auto [hx, hy] = HEADINGS[v->direction];
		float angle = std::atan2(hy, hx);
		Placement pl;
		pl.origin = {static_cast<float>(v->x_pos), static_cast<float>(v->y_pos), static_cast<float>(v->z_pos)};
		pl.cos = std::cos(angle);
		pl.sin = std::sin(angle);
		pl.height_scale = v->type == VEH_ROAD ? ROAD_VEHICLE_HEIGHT : BUILDING_HEIGHT;

		/* Leave out the vehicle the camera is on. */
		Vec3 d = this->cam.pos - pl.origin;
		float along = d.x * pl.cos + d.y * pl.sin;
		float across = -d.x * pl.sin + d.y * pl.cos;
		if (std::abs(along) <= length / 2 + 1 && std::abs(across) <= width / 2 + 1) continue;

		/* Road vehicles are often seen from right behind, where cutting out the transparent parts of the picture of
		 * their ends left holes between their roofs and their backs; their boxes are solid. */
		this->AddSpriteBox(tex, pl, {-length / 2, -width / 2, 0}, {length / 2, width / 2, height}, false, true, v->type != VEH_ROAD);
	}
}

/**
 * Get the direction of the sprite of a vehicle that shows it from the side the camera sees it from.
 * @param dir The direction the vehicle goes in.
 * @return The direction of the sprite.
 */
Direction Renderer::GetShownDirection(Direction dir) const
{
	/* The map shows a vehicle going in a direction as seen from the south, looking north. Turn that view to the camera. */
	auto heading = [](Direction d) { return std::atan2(HEADINGS[d].second, HEADINGS[d].first); };
	float camera_yaw = std::atan2(this->cam.fwd.y, this->cam.fwd.x);
	float wanted = heading(dir) - camera_yaw + heading(DIR_N);
	Direction shown = DIR_N;
	float best = 4.0f;
	for (Direction d = DIR_BEGIN; d < DIR_END; d++) {
		float off = std::abs(std::remainder(heading(d) - wanted, 2.0f * static_cast<float>(M_PI)));
		if (off < best) {
			best = off;
			shown = d;
		}
	}
	return shown;
}

/**
 * Add a ship as a cut-out facing the camera, floating on the water, see #GetCabView3DShipWaterZ.
 * It shows the sprite of the ship seen from the side the camera sees it from.
 * @param v The ship.
 */
void Renderer::AddShip(const Ship *v)
{
	/* On the map ships show how they are turned, which can lag behind the direction they are going in. */
	VehicleSpriteSeq seq;
	v->GetImage(this->GetShownDirection(v->rotation), EIT_IN_DETAILS, &seq);
	if (!seq.IsValid()) return;
	PaletteID pal = seq.seq[0].pal != 0 ? seq.seq[0].pal : GetVehiclePalette(v);
	const GroundTexture &tex = this->GetBoxTexture(seq.seq[0].sprite, pal, false);
	/* The game draws the sprites of ships well away from their position; stand the middle of the hull on the position.
	 * The sprite is 2 (length + width) wide, and the hull (length + width) / 2 high; its nearer half sinks into the water. */
	auto [left, right, top, bottom] = OpaqueBounds(tex);
	if (right <= left) return;
	Vec3 at = {static_cast<float>(v->x_pos), static_cast<float>(v->y_pos), GetCabView3DShipWaterZ(v)};
	/* The map looks down at 30 degrees, so the length of a ship going away from it or towards it shows upwards in the
	 * sprite. Seen from the water, that has to shrink, or a ship seen from behind stands up like a wall. */
	auto [hx, hy] = HEADINGS[v->rotation];
	Vec3 d = at - this->cam.pos;
	float side = std::abs(hx * d.y - hy * d.x) / std::max(std::hypot(hx, hy) * std::hypot(d.x, d.y), 1.0f);
	float vertical = SHIP_UNITS * (SHIP_MIN_UPRIGHT + (1 - SHIP_MIN_UPRIGHT) * side);
	this->AddCutout(tex, at, (left + right) / 2, bottom - (right - left) / 8, SHIP_UNITS, vertical);
}

/**
 * Add an aircraft as a cut-out facing the camera, at the height it flies in the game.
 * It shows the sprite of the aircraft seen from the side the camera sees it from.
 * @param v The aircraft.
 */
void Renderer::AddAircraft(const Aircraft *v)
{
	Direction shown = this->GetShownDirection(v->direction);

	VehicleSpriteSeq seq;
	v->GetImage(shown, EIT_ON_MAP, &seq);
	if (!seq.IsValid()) return;
	PaletteID pal = seq.seq[0].pal != 0 ? seq.seq[0].pal : GetVehiclePalette(v);
	/* The map draws the sprite with its origin at the position of the aircraft. */
	Vec3 at = {static_cast<float>(v->x_pos), static_cast<float>(v->y_pos), static_cast<float>(v->z_pos)};
	/* The map looks down at 30 degrees, so the length of an aircraft going away from it shows upwards in the sprite.
	 * Seen from lower down, that has to shrink, or an aircraft going away stands up like a pole. */
	Vec3 d = this->cam.pos - at;
	float seen = std::abs(d.z) / std::max(std::hypot(d.x, d.y, d.z), 1.0f);
	float vertical = AIRCRAFT_UNITS * std::clamp(seen / 0.5f, 0.35f, 1.0f);
	this->AddCutout(this->GetBoxTexture(seq.seq[0].sprite, pal, false), at, 0, 0, AIRCRAFT_UNITS, vertical);
}

/**
 * Add rails and sleepers on top of the ground, so they stay sharp up close.
 * @param tile The tile.
 * @param tracks The track pieces.
 * @param corner_z Height of the corners of the surface the track lies on.
 * @param split_we How the tile is split into triangles, see #SurfaceZ.
 * @param dist Distance of the tile from the camera.
 */
void Renderer::AddTrack(TileIndex tile, TrackBits tracks, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist)
{
	if (dist > RAIL_DIST || tracks == TRACK_BIT_NONE) return;

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
 * Add what makes a road three dimensional: the raised pavements of town roads with their kerbs,
 * the line in the middle drawn sharp, and the rails of tram track.
 * @param tile The tile.
 * @param look What to draw, see #GetRoadLook.
 * @param tex The texture of the ground of the tile, which the pavements are dressed in.
 * @param raw_z Height of the tile as the viewport uses it, see #GetTilePixelSlope.
 * @param corner_z Height of the corners of the surface the road lies on.
 * @param split_we How the tile is split into triangles, see #SurfaceZ.
 * @param dist Distance of the tile from the camera.
 */
void Renderer::AddRoad(TileIndex tile, const RoadLook &look, const GroundTexture &tex, int raw_z, const std::array<float, CORNER_END> &corner_z, bool split_we, float dist)
{
	if (dist > RAIL_DIST) return;

	using Point2 = std::pair<float, float>;
	const float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	const float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	auto at = [&](Point2 p, float lift) {
		return Vec3{x0 + p.first, y0 + p.second, SurfaceZ(corner_z, split_we, std::clamp(p.first, 0.0f, TILE), std::clamp(p.second, 0.0f, TILE)) + lift};
	};
	/* A strip of a flat colour along a line on the road, from a to b, half_width to both sides of it. */
	auto strip = [&](Point2 a, Point2 b, float half_width, float lift, const RGB &colour, float bias) {
		const float len = std::hypot(b.first - a.first, b.second - a.second);
		if (len <= 0) return;
		const float px = -(b.second - a.second) / len * half_width, py = (b.first - a.first) / len * half_width;
		this->AddFlatQuad({
			at({a.first - px, a.second - py}, lift), at({b.first - px, b.second - py}, lift),
			at({b.first + px, b.second + py}, lift), at({a.first + px, a.second + py}, lift),
		}, colour, bias);
	};
	/* The corner the pieces of a curve go around, and the directions into the tile from it. */
	auto curve_corner = [](RoadBits bits) {
		const float cx = (bits & ROAD_NE) != ROAD_NONE ? 0.0f : TILE;
		const float cy = (bits & ROAD_NW) != ROAD_NONE ? 0.0f : TILE;
		return std::tuple<float, float, float, float>{cx, cy, cx == 0 ? 1.0f : -1.0f, cy == 0 ? 1.0f : -1.0f};
	};
	/* A point of the arc around the corner of a curve, from the side along x at t = 0 to the side along y at t = pi / 2. */
	auto arc = [](const std::tuple<float, float, float, float> &corner, float r, float t) {
		const auto [cx, cy, ux, uy] = corner;
		return Point2{cx + ux * r * std::cos(t), cy + uy * r * std::sin(t)};
	};
	constexpr float QUARTER = static_cast<float>(M_PI) / 2;
	const float fade = std::clamp((dist - RAIL_FADE_START) / (RAIL_DIST - RAIL_FADE_START), 0.0f, 1.0f);

	if (look.pavement) {
		/* The pavements are the ground of the tile raised by the height of the kerb, so they keep their picture. */
		Vec3 normal = (at({TILE, 0}, 0) - at({0, 0}, 0)).Cross(at({0, TILE}, 0) - at({0, 0}, 0));
		if (normal.z < 0) normal = normal * -1;
		const Paint paint = {&tex, {}, Shade(normal), 0, 1.0f};
		auto vertex = [&](Point2 p) {
			const Vec3 w = at(p, KERB_HEIGHT);
			ClipVertex v;
			v.c = this->cam.ToCamera(w);
			v.a = {2.0f * (p.second - p.first), p.first + p.second - (w.z - KERB_HEIGHT - raw_z), w.x, w.y};
			return v;
		};
		/* A pavement around its first corner, from which all of its edges can be seen. */
		auto pave = [&](std::span<const Point2> poly) {
			const ClipVertex first = vertex(poly[0]);
			ClipVertex prev = vertex(poly[1]);
			for (size_t i = 2; i < poly.size(); i++) {
				ClipVertex next = vertex(poly[i]);
				this->AddTriangle({first, prev, next}, paint);
				prev = next;
			}
		};
		/* The side of a pavement, at the roadway or at the side of the tile. */
		auto kerb = [&](Point2 a, Point2 b) {
			this->AddFlatQuad({at(a, 0), at(b, 0), at(b, KERB_HEIGHT), at(a, KERB_HEIGHT)}, KERB_COLOUR, 1.0f);
		};

		if (IsRoadCurve(look.road)) {
			/* The roadway goes around a corner: a small pavement in that corner, and a large one around the outside. */
			const auto corner = curve_corner(look.road);
			const auto [cx, cy, ux, uy] = corner;
			std::array<Point2, ROAD_ARC_SEGMENTS + 2> inner;
			std::array<Point2, ROAD_ARC_SEGMENTS + 4> outer;
			inner[0] = {cx, cy};
			outer[0] = {TILE - cx, TILE - cy};
			outer[1] = {TILE - cx, cy};
			for (int i = 0; i <= ROAD_ARC_SEGMENTS; i++) {
				const float t = QUARTER * i / ROAD_ARC_SEGMENTS;
				inner[i + 1] = arc(corner, PAVEMENT_WIDTH, t);
				outer[i + 2] = arc(corner, TILE - PAVEMENT_WIDTH, t);
			}
			outer.back() = {cx, TILE - cy};
			pave(inner);
			pave(outer);
			for (int i = 0; i < ROAD_ARC_SEGMENTS; i++) {
				kerb(inner[i + 1], inner[i + 2]);
				kerb(outer[i + 2], outer[i + 3]);
			}
			kerb(inner[0], inner[1]);
			kerb(inner[0], inner.back());
			kerb(outer[1], outer[2]);
			kerb(outer[outer.size() - 2], outer.back());
			kerb(outer.back(), outer[0]);
			kerb(outer[0], outer[1]);
		} else {
			/* The tile in three by three parts: the roadway in the middle and out to the sides with road, pavements elsewhere. */
			const std::array<float, 4> edges = {0, PAVEMENT_WIDTH, TILE - PAVEMENT_WIDTH, TILE};
			auto roadway = [&](int i, int j) {
				if (i == 1 && j == 1) return true;
				if (i == 1) return (look.road & (j == 0 ? ROAD_NW : ROAD_SE)) != ROAD_NONE;
				if (j == 1) return (look.road & (i == 0 ? ROAD_NE : ROAD_SW)) != ROAD_NONE;
				return false;
			};
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++) {
					if (roadway(i, j)) continue;
					const std::array<Point2, 4> cell = {{{edges[i], edges[j]}, {edges[i + 1], edges[j]}, {edges[i + 1], edges[j + 1]}, {edges[i], edges[j + 1]}}};
					pave(cell);
					/* Its sides towards the roadway, and those at the sides of the tile. */
					const std::array<std::pair<int, int>, 4> next = {{{i, j - 1}, {i + 1, j}, {i, j + 1}, {i - 1, j}}};
					for (int k = 0; k < 4; k++) {
						const auto [ni, nj] = next[k];
						const bool outside = ni < 0 || nj < 0 || ni > 2 || nj > 2;
						if (outside || roadway(ni, nj)) kerb(cell[k], cell[(k + 1) % 4]);
					}
				}
			}
		}
	}

	if (look.line) {
		const RGB colour = ROAD_LINE_COLOUR * (1 - fade) + ROAD_LINE_FAR_COLOUR * fade;
		const float half = ROAD_LINE_LENGTH / 2;
		if (IsRoadCurve(look.road)) {
			/* Three dashes along the arc through the middle of the roadway, as the graphics paint them. */
			const auto corner = curve_corner(look.road);
			const float r = TILE / 2;
			const float length = r * QUARTER;
			for (float s : {2.0f, length / 2, length - 2.0f}) {
				const float t0 = (s - half) / r, t1 = (s + half) / r;
				const Point2 a0 = arc(corner, r - ROAD_LINE_WIDTH / 2, t0), a1 = arc(corner, r - ROAD_LINE_WIDTH / 2, t1);
				const Point2 b0 = arc(corner, r + ROAD_LINE_WIDTH / 2, t0), b1 = arc(corner, r + ROAD_LINE_WIDTH / 2, t1);
				this->AddFlatQuad({at(a0, 0.03f), at(a1, 0.03f), at(b1, 0.03f), at(b0, 0.03f)}, colour, 1.0005f);
			}
		} else {
			const bool x = look.road == ROAD_X;
			for (float s = ROAD_LINE_SPACING / 2; s < TILE; s += ROAD_LINE_SPACING) {
				const Point2 a = x ? Point2{s - half, TILE / 2} : Point2{TILE / 2, s - half};
				const Point2 b = x ? Point2{s + half, TILE / 2} : Point2{TILE / 2, s + half};
				strip(a, b, ROAD_LINE_WIDTH / 2, 0.03f, colour, 1.0005f);
			}
		}
	}

	if (look.tram != ROAD_NONE) {
		/* A single track in the middle of the road; curves follow the arc of the road. */
		std::vector<std::vector<Point2>> lines;
		if (IsRoadCurve(look.tram)) {
			const auto corner = curve_corner(look.tram);
			std::vector<Point2> &line = lines.emplace_back();
			for (int i = 0; i <= ROAD_ARC_SEGMENTS; i++) line.push_back(arc(corner, TILE / 2, QUARTER * i / ROAD_ARC_SEGMENTS));
		} else {
			const float m = TILE / 2;
			if ((look.tram & ROAD_X) == ROAD_X) {
				lines.push_back({{0, m}, {TILE, m}});
			} else if ((look.tram & ROAD_X) != ROAD_NONE) {
				lines.push_back({{(look.tram & ROAD_NE) != ROAD_NONE ? 0 : TILE, m}, {m, m}});
			}
			if ((look.tram & ROAD_Y) == ROAD_Y) {
				lines.push_back({{m, 0}, {m, TILE}});
			} else if ((look.tram & ROAD_Y) != ROAD_NONE) {
				lines.push_back({{m, (look.tram & ROAD_NW) != ROAD_NONE ? 0 : TILE}, {m, m}});
			}
		}
		const RGB colour = TRAM_RAIL_COLOUR * (1 - fade) + ROAD_LINE_FAR_COLOUR * fade;
		for (const std::vector<Point2> &line : lines) {
			for (size_t i = 0; i + 1 < line.size(); i++) {
				const auto [ax, ay] = line[i];
				const auto [bx, by] = line[i + 1];
				const float len = std::hypot(bx - ax, by - ay);
				/* Along and across the piece; the rails reach a bit beyond it, so there are no gaps in bends. */
				const float ux = (bx - ax) / len, uy = (by - ay) / len;
				const float ext = TRAM_RAIL_WIDTH / 2;
				for (float side : {-RAIL_GAUGE / 2, RAIL_GAUGE / 2}) {
					strip({ax - ux * ext - uy * side, ay - uy * ext + ux * side}, {bx + ux * ext - uy * side, by + uy * ext + ux * side}, TRAM_RAIL_WIDTH / 2, 0.04f, colour, 1.001f);
				}
			}
		}
	}
}

/**
 * Add the shelters of a drive-through road stop on the pavements at both sides of the road: a bus shelter
 * with a glass roof and back wall, posts and a bench; or a small brick booth for lorries. Next to each
 * stands a post with a sign in the colour of the company.
 * @param tile The road stop.
 * @param corner_z Height of the corners of the surface the road lies on.
 * @param split_we How the tile is split into triangles, see #SurfaceZ.
 */
void Renderer::AddRoadStop(TileIndex tile, const std::array<float, CORNER_END> &corner_z, bool split_we)
{
	const float x0 = static_cast<float>(TileX(tile) * TILE_SIZE);
	const float y0 = static_cast<float>(TileY(tile) * TILE_SIZE);
	const float z = SurfaceZ(corner_z, split_we, TILE / 2, TILE / 2) + KERB_HEIGHT;
	const Axis axis = GetDriveThroughStopAxis(tile);
	const bool bus = IsBusStop(tile);
	const Colour &cc = _cur_palette.palette[GetColourGradient(_company_colours[GetTileOwner(tile)], SHADE_NORMAL).p];
	const RGB sign = {static_cast<float>(cc.r), static_cast<float>(cc.g), static_cast<float>(cc.b)};

	for (int side = 0; side < 2; side++) {
		/* A box from along a0 to a1, from c0 to c1 away from the side of the tile, and from h0 to h1 above the pavement. */
		auto box = [&](float a0, float a1, float c0, float c1, float h0, float h1, const RGB &colour) {
			if (side == 1) std::tie(c0, c1) = std::pair(TILE - c1, TILE - c0);
			const Vec3 lo = axis == AXIS_X ? Vec3{x0 + a0, y0 + c0, z + h0} : Vec3{x0 + c0, y0 + a0, z + h0};
			const Vec3 hi = axis == AXIS_X ? Vec3{x0 + a1, y0 + c1, z + h1} : Vec3{x0 + c1, y0 + a1, z + h1};
			this->AddFlatBox(lo, hi, colour);
		};
		if (bus) {
			box(3.4f, 12.6f, 0.3f, 0.5f, 0.5f, 3.4f, SHELTER_GLASS_COLOUR); // back wall
			box(3.0f, 13.0f, 0.2f, 2.8f, 3.5f, 3.7f, SHELTER_GLASS_COLOUR); // roof
			for (float a : {3.2f, 12.6f}) {
				for (float c : {0.3f, 2.4f}) box(a, a + 0.2f, c, c + 0.2f, 0.0f, 3.5f, SHELTER_FRAME_COLOUR);
			}
			box(5.0f, 11.0f, 0.6f, 1.3f, 0.0f, 0.6f, SHELTER_BENCH_COLOUR);
		} else {
			box(3.0f, 8.0f, 0.3f, 2.6f, 0.0f, 3.0f, BOOTH_COLOUR);
			box(2.8f, 8.2f, 0.1f, 2.8f, 3.0f, 3.3f, SHELTER_GLASS_COLOUR);
		}
		/* The post with the sign, at the end where the vehicles stop. */
		const float a = side == 0 ? 14.2f : 1.6f;
		box(a, a + 0.15f, 2.3f, 2.45f, 0.0f, 4.2f, SHELTER_FRAME_COLOUR);
		box(a - 0.35f, a + 0.5f, 2.3f, 2.45f, 3.4f, 4.3f, sign);
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

	float pitch = PITCH_DEG * static_cast<float>(M_PI) / 180.0f + camera.pitch;
	Vec3 flat_fwd = {std::cos(camera.yaw), std::sin(camera.yaw), 0};
	Vec3 world_up = {0, 0, 1};
	this->cam.pos = {camera.x, camera.y, camera.z};
	this->cam.right = flat_fwd.Cross(world_up);
	this->cam.fwd = flat_fwd * std::cos(pitch) + world_up * std::sin(pitch);
	this->cam.up = world_up * std::cos(pitch) - flat_fwd * std::sin(pitch);
	this->cam.focal = (this->width / 2.0f) / std::tan(FOV_DEG / 2 * static_cast<float>(M_PI) / 180.0f);
	this->cam.cx = this->width / 2.0f;
	this->cam.cy = this->height / 2.0f;

	/* From an aircraft the ground is seen from above, so even what is right below is far from the eye. */
	float lift = std::clamp(camera.altitude, 0.0f, MAX_FOG_LIFT);
	this->fog_start = FOG_START + lift;
	this->fog_end = FOG_END + lift;

	this->DrawSky();

	/* Tiles in the field of view, nearest first, so hidden pixels are not textured. */
	std::vector<std::pair<float, TileIndex>> visible;
	float half_fov = std::atan(this->cam.cx / this->cam.focal);
	float spread = std::tan(std::min(half_fov + 0.1f, 1.4f));
	int cx = static_cast<int>(camera.x / TILE);
	int cy = static_cast<int>(camera.y / TILE);
	/* The haze depends on the distance straight ahead, so at the sides the view reaches further. */
	int reach = static_cast<int>(this->fog_end / TILE * std::hypot(1.0f, spread)) + 2;
	for (int ty = std::max(0, cy - reach); ty <= std::min<int>(Map::MaxY(), cy + reach); ty++) {
		for (int tx = std::max(0, cx - reach); tx <= std::min<int>(Map::MaxX(), cx + reach); tx++) {
			TileIndex tile = TileXY(tx, ty);
			if (IsTileType(tile, MP_VOID)) continue;
			float dx = (tx + 0.5f) * TILE - camera.x;
			float dy = (ty + 0.5f) * TILE - camera.y;
			float ahead = dx * flat_fwd.x + dy * flat_fwd.y;
			float side = dx * this->cam.right.x + dy * this->cam.right.y;
			if (ahead < -TILE * 1.5f) continue;
			/* Looking down from an aircraft, the view is wider at the ground than the distance ahead tells. */
			if (std::abs(side) > (std::max(ahead, 0.0f) + camera.altitude) * spread + TILE * 1.5f) continue;
			if (ahead > this->fog_end + TILE) continue;
			visible.emplace_back(std::hypot(dx, dy), tile);
		}
	}
	std::sort(visible.begin(), visible.end());
	this->triangles.clear();
	this->tunnels.clear();
	/* Inside a tunnel its vault is nearest, and hides the hill around it. */
	if (IsValidTile(camera.tunnel) && IsTunnelTile(camera.tunnel)) this->AddTunnelInside(camera.tunnel, std::get<int>(GetTilePixelSlope(camera.tunnel)));
	this->own = camera.vehicle;
	for (const auto &[dist, tile] : visible) this->AddTile(tile, dist);
	this->AddVehicles(camera.vehicle);

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

/**
 * Height of the surface of the water a ship sails on, as the 3D view shows it.
 * @param v The ship.
 * @return The height, in world coordinates.
 */
float GetCabView3DShipWaterZ(const Vehicle *v)
{
	/* On aqueducts, the water is as high as the deck. */
	if (IsTileType(v->tile, MP_TUNNELBRIDGE)) return static_cast<float>(v->z_pos);
	return static_cast<float>(v->z_pos - CAB_VIEW_3D_WATER_DROP);
}
