/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cab_view_gui.cpp A large view riding along with a vehicle, looking ahead in the direction of travel, with a dashboard. */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "viewport_func.h"
#include "vehicle_base.h"
#include "vehicle_gui.h"
#include "train.h"
#include "landscape.h"
#include "bridge_map.h"
#include "rail_map.h"
#include "pbs.h"
#include "pathfinder/follow_track.hpp"
#include "company_func.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "map_func.h"
#include "palette_func.h"
#include "gfx_func.h"
#include "settings_type.h"
#include "core/geometry_func.hpp"
#include "cab_view_3d.h"
#include "debug.h"

#include <chrono>
#include <deque>

#include "widgets/cab_view_widget.h"

#include "table/strings.h"
#include "table/sprites.h"

#include "safeguards.h"

/** Whether cab views look ahead in the direction of travel. Shared by all cab views. */
static bool _cab_view_look_ahead = true;
/** Whether cab views of trains show the 3D view instead of the map. Shared by all cab views. */
static bool _cab_view_3d = false;

/** Time constant (in milliseconds) of the camera easing towards its look-ahead position. */
static constexpr float CAB_VIEW_EASE_MS = 650.0f;
/** Look-ahead when standing still, as fraction of half the viewport size. */
static constexpr float CAB_VIEW_REACH_MIN = 0.30f;
/** Look-ahead at full speed, as fraction of half the viewport size. */
static constexpr float CAB_VIEW_REACH_MAX = 0.60f;
/** How many tiles ahead along the track the camera looks at most. */
static constexpr int CAB_VIEW_MAX_TRACK_TILES = 48;
/** How often (in milliseconds) the dashboard is refreshed. */
static constexpr uint CAB_VIEW_DASHBOARD_REFRESH_MS = 100;
/** Minimal height of the speedometer dial, in unscaled GUI pixels. */
static constexpr int CAB_VIEW_DIAL_HEIGHT = 52;
/** Angle (in degrees, counter-clockwise from the right) of the speedometer dial at standstill. */
static constexpr float CAB_VIEW_DIAL_START = 225.0f;
/** Angle (in degrees) the speedometer needle sweeps from standstill to the maximum speed. */
static constexpr float CAB_VIEW_DIAL_SWEEP = 270.0f;
/** Number of ticks on the speedometer dial. */
static constexpr int CAB_VIEW_DIAL_TICKS = 10;
/** Position of the maximum speed of the vehicle on the speedometer dial; beyond it is the red zone. */
static constexpr float CAB_VIEW_DIAL_MAX_SPEED = 0.85f;
/** Height of the eye of the 3D view above the rails: the windows of the cab, well below the decks of bridges and the tops of tunnels. */
static constexpr float CAB_VIEW_3D_EYE = 4.0f;
/** Distance (in world coordinates) over which the 3D view turns most of the way into a new direction of travel. */
static constexpr float CAB_VIEW_3D_TURN_DIST = 6.0f;
/** Distance (in world coordinates) over which the 3D view follows most of a change in height, which on slopes comes in steps. */
static constexpr float CAB_VIEW_3D_CLIMB_DIST = 1.5f;
/**
 * Part of the time between two redraws of the screen that making a picture of the 3D view may take.
 * When it takes longer, the picture is made smaller, and when even that is not enough, the view is drawn
 * only on every second (third...) redraw, so the pace stays even and the game and the rest of the interface
 * keep enough time.
 */
static constexpr float CAB_VIEW_3D_FRAME_BUDGET = 0.65f;
/** Redraws of the screen per picture of the 3D view at most. */
static constexpr int CAB_VIEW_3D_MAX_FRAME_DIVIDER = 4;
/** How long drawing has to be too slow or fast enough before the pace of the 3D view changes, so it does not keep changing. */
static constexpr std::chrono::milliseconds CAB_VIEW_3D_PACE_DELAY{1500};
/**
 * How far (in game ticks) the moment the 3D view shows stays behind the game when a game tick happens.
 * Game ticks come late when drawing takes long, so the view needs some ticks to move smoothly between.
 */
static constexpr float CAB_VIEW_3D_LAG = 1.5f;
/** Time after which the 3D view is redrawn even when the camera did not move. */
static constexpr std::chrono::milliseconds CAB_VIEW_3D_IDLE_FRAME_TIME{1000};
/** Background of the dashboard. */
static constexpr PixelColour CAB_VIEW_DASHBOARD_COLOUR = GREY_SCALE(1);
/** Face of the speedometer dial. */
static constexpr PixelColour CAB_VIEW_DIAL_COLOUR = GREY_SCALE(4);

static constexpr std::initializer_list<NWidgetPart> _nested_cab_view_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_CV_CAPTION),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_DEFSIZEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY),
		NWidget(WWT_INSET, COLOUR_GREY), SetPadding(2, 2, 2, 2),
			NWidget(NWID_SELECTION, INVALID_COLOUR, WID_CV_VIEW_SELECT),
				NWidget(NWID_VIEWPORT, INVALID_COLOUR, WID_CV_VIEWPORT), SetMinimalSize(320, 180), SetResize(1, 1), SetFill(1, 1),
				NWidget(WWT_EMPTY, INVALID_COLOUR, WID_CV_3D_VIEW), SetMinimalSize(320, 180), SetResize(1, 1), SetFill(1, 1),
			EndContainer(),
		EndContainer(),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CV_DASHBOARD), SetResize(1, 0), SetFill(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_CV_ZOOM_IN), SetSpriteTip(SPR_IMG_ZOOMIN, STR_TOOLBAR_TOOLTIP_ZOOM_THE_VIEW_IN),
		NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_CV_ZOOM_OUT), SetSpriteTip(SPR_IMG_ZOOMOUT, STR_TOOLBAR_TOOLTIP_ZOOM_THE_VIEW_OUT),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_CV_LOOK_AHEAD), SetFill(0, 1), SetStringTip(STR_CAB_VIEW_LOOK_AHEAD, STR_CAB_VIEW_LOOK_AHEAD_TOOLTIP),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_CV_3D), SetMinimalSize(30, 0), SetFill(0, 1), SetStringTip(STR_CAB_VIEW_3D, STR_CAB_VIEW_3D_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CV_START_STOP), SetFill(1, 1), SetResize(1, 0), SetStringTip(STR_CAB_VIEW_STOP, STR_CAB_VIEW_START_STOP_TOOLTIP),
		NWidget(WWT_RESIZEBOX, COLOUR_GREY),
	EndContainer(),
};

/** Window riding along with a vehicle, like looking out of its cab. */
struct CabViewWindow : Window {
private:
	float offset_x = 0; ///< Current look-ahead offset in virtual viewport coordinates.
	float offset_y = 0; ///< Current look-ahead offset in virtual viewport coordinates.
	ZoomLevel map_zoom = ZoomLevel::Normal; ///< Zoom of the map view, kept while the 3D view is shown, see #UpdateViewport.
	uint dashboard_timer = 0; ///< Milliseconds since the dashboard was last redrawn.
	int speed_width = 0; ///< Width of the speed part of the dashboard, dial included.
	int load_width = 0; ///< Width of the load part of the dashboard.
	float yaw = 0; ///< Direction of view of the 3D view, eased towards the direction of travel.
	float height = 0; ///< Height of the train for the 3D view, eased so steps on slopes don't make it jump.
	float train_x = 0; ///< X position of the front of the train at the last game tick, see #GetExactPosition.
	float train_y = 0; ///< Y position of the front of the train at the last game tick, see #GetExactPosition.
	/** Camera of the 3D view at a game tick. */
	struct TickCamera {
		uint32_t tick; ///< Number of the game tick, counted by this window.
		CabViewCamera camera; ///< The camera at that tick.
	};
	std::deque<TickCamera> tick_cameras; ///< Cameras of the last few game ticks, oldest first.
	uint32_t tick_count = 0; ///< Number of game ticks this window placed the camera at.
	double view_tick = 0; ///< The moment the 3D view shows, in game ticks; runs evenly a bit behind the game.
	float tick_interval = MILLISECONDS_PER_TICK; ///< Average time (in milliseconds) between game ticks.
	float view_lag = CAB_VIEW_3D_LAG; ///< Average of how far (in game ticks) #view_tick is behind when a game tick happens.
	std::chrono::steady_clock::time_point tick_time; ///< When the last game tick happened.
	std::chrono::steady_clock::time_point clock_time; ///< When #view_tick was last advanced.
	std::chrono::steady_clock::time_point frame_time; ///< When the 3D view was last redrawn.
	std::chrono::steady_clock::time_point redraw_time; ///< When the screen was last redrawn, see #OnRealtimeTick.
	float redraw_interval = 1000.0f / 60; ///< Average time (in milliseconds) between redraws of the screen.
	int frame_divider = 1; ///< Redraws of the screen per picture of the 3D view.
	float frame_budget = 0; ///< Time (in milliseconds) making a picture of the 3D view may take.
	int redraws_since_frame = 0; ///< Redraws of the screen since the last picture of the 3D view.
	std::chrono::steady_clock::time_point pace_time; ///< When #frame_divider was last right for how long drawing takes.
	CabViewCamera drawn_camera; ///< Camera of the last picture of the 3D view.
	uint32_t frame_id = 0; ///< Number of the last picture of the 3D view.

	/** Whether this window shows the 3D view. */
	bool Is3D() const
	{
		return _cab_view_3d && Vehicle::Get(this->window_number)->type == VEH_TRAIN;
	}

	/**
	 * Get the position of a vehicle including how far it got towards its next step.
	 * Vehicles move in whole steps, a train at 90 km/h only every third game tick or so,
	 * which would make the 3D view jerk forward.
	 * @param v The vehicle.
	 * @return X and Y position in world coordinates.
	 */
	static std::pair<float, float> GetExactPosition(const Vehicle *v)
	{
		/* The progress collects the speed until there is enough for a step, see GroundVehicle::DoUpdateSpeed(). */
		uint step = (v->direction & 1) ? TILE_AXIAL_DISTANCE : TILE_CORNER_DISTANCE * 2; // As Vehicle::GetAdvanceDistance().
		float part = std::min<float>(v->progress, step) / step;
		TileIndexDiffC diff = TileIndexDiffCByDir(v->direction);
		return {v->x_pos + diff.x * part, v->y_pos + diff.y * part};
	}

	/**
	 * Get the height of the track of a train at a point near its front, where the eye of the 3D view is.
	 * The height of the train is that of its middle, which on slopes and ramps is lower or higher than its front.
	 * @param v The train.
	 * @param x X coordinate of the point.
	 * @param y Y coordinate of the point.
	 * @return The height of the track.
	 */
	static float GetTrackHeight(const Vehicle *v, float x, float y)
	{
		auto height = [v](int x, int y) {
			if (!IsInsideBS(x, 0, Map::SizeX() * TILE_SIZE) || !IsInsideBS(y, 0, Map::SizeY() * TILE_SIZE)) return v->z_pos;
			TileIndex tile = TileVirtXY(x, y);
			/* On a bridge, the track is as high as the bridge, up to the ramp at its other end. */
			if (IsBridgeAbove(tile)) {
				int bridge = GetBridgePixelHeight(GetSouthernBridgeEnd(tile));
				if (v->z_pos > bridge - static_cast<int>(TILE_HEIGHT)) return bridge;
			}
			/* Beyond the end of the line or the entrance of a tunnel, keep the height of the train. */
			if (GetTileRailType(tile) == INVALID_RAILTYPE) return v->z_pos;
			return GetSlopePixelZ(x, y, true);
		};
		/* Heights are in whole units; blend those of the points around, so the eye does not climb in steps. */
		int ix = static_cast<int>(std::floor(x));
		int iy = static_cast<int>(std::floor(y));
		float fx = x - ix;
		float fy = y - iy;
		return (height(ix, iy) * (1 - fx) + height(ix + 1, iy) * fx) * (1 - fy) + (height(ix, iy + 1) * (1 - fx) + height(ix + 1, iy + 1) * fx) * fy;
	}

	/**
	 * Place the camera of the 3D view for the current game tick: at the windows of the front of the train.
	 * The direction and height ease with the distance travelled instead of with time, so
	 * the view follows the track through curves and over slopes at any speed.
	 * @param jump Place the camera at once, without easing or moving smoothly from where it was.
	 */
	void Update3DTick(bool jump)
	{
		const Vehicle *v = Vehicle::Get(this->window_number);
		auto [x, y] = GetExactPosition(v);
		float moved = std::hypot(x - this->train_x, y - this->train_y);
		this->train_x = x;
		this->train_y = y;

		TileIndexDiffC diff = TileIndexDiffCByDir(v->direction);
		float target = std::atan2(static_cast<float>(diff.y), static_cast<float>(diff.x));
		float turn = std::remainder(target - this->yaw, 2.0f * static_cast<float>(M_PI));
		bool dark = v->vehstatus.Test(VehState::Hidden);
		/* Jump when starting, when the train reverses, or when it moved too far to follow. */
		jump |= this->tick_cameras.empty() || std::abs(turn) > 0.75f * static_cast<float>(M_PI) || moved > 2 * TILE_SIZE || dark != this->tick_cameras.back().camera.dark;
		if (jump) {
			this->yaw = target;
		} else {
			this->yaw += turn * (1.0f - std::exp(-moved / CAB_VIEW_3D_TURN_DIST));
		}

		CabViewCamera camera;
		/* Nothing to see from a depot or tunnel. */
		camera.dark = dark;
		camera.yaw = this->yaw;
		/* The position of a vehicle is its middle; move the camera to the front. */
		float half_length = Train::From(v)->gcache.cached_veh_length / 2.0f;
		camera.x = x + std::cos(this->yaw) * half_length;
		camera.y = y + std::sin(this->yaw) * half_length;
		float track = GetTrackHeight(v, camera.x, camera.y);
		if (jump) {
			this->height = track;
		} else {
			this->height += (track - this->height) * (1.0f - std::exp(-moved / CAB_VIEW_3D_CLIMB_DIST));
		}
		camera.z = this->height + CAB_VIEW_3D_EYE;

		auto now = std::chrono::steady_clock::now();
		if (jump) {
			this->tick_cameras.clear();
		} else {
			/* Ticks often come late or in pairs when drawing took long, so only the long-term average counts. Skip pauses. */
			float ms = std::chrono::duration<float, std::milli>(now - this->tick_time).count();
			if (ms < 4 * MILLISECONDS_PER_TICK) this->tick_interval += (ms - this->tick_interval) * 0.02f;
		}
		this->tick_time = now;
		this->tick_count++;
		this->tick_cameras.push_back({this->tick_count, camera});
		if (this->tick_cameras.size() > 8) this->tick_cameras.pop_front();
		if (jump) {
			this->view_tick = static_cast<double>(this->tick_count) - CAB_VIEW_3D_LAG;
			this->view_lag = CAB_VIEW_3D_LAG;
			this->clock_time = now;
		} else {
			/* How far behind the view is now; see Advance3DClock for what is done with it. */
			double view_now = this->view_tick + std::chrono::duration<double, std::milli>(now - this->clock_time).count() / this->tick_interval * this->Get3DClockRate();
			this->view_lag += (static_cast<float>(this->tick_count - view_now) - this->view_lag) * 0.05f;
		}
	}

	/**
	 * Get how fast the moment the 3D view shows runs compared to the game: a bit faster when it got behind
	 * more than intended, a bit slower when it got too close to the game.
	 * @return The factor for the speed of the game.
	 */
	float Get3DClockRate() const
	{
		return 1.0f + std::clamp((this->view_lag - CAB_VIEW_3D_LAG) * 0.05f, -0.1f, 0.1f);
	}

	/**
	 * Let the moment the 3D view shows run on evenly, a bit behind the game.
	 * Game ticks and redraws of the screen come at different rates and ticks often come late or in pairs,
	 * so following the ticks directly would make the view jerk. Instead the view runs with the clock,
	 * and only its speed is adjusted, slightly, so it stays the intended distance behind the game.
	 */
	void Advance3DClock()
	{
		auto now = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(now - this->clock_time).count();
		this->clock_time = now;

		this->view_tick += ms / std::max(this->tick_interval, 1.0f) * this->Get3DClockRate();
		/* Never run ahead of the game; catch up at once when far behind, e.g. after the game was very busy. */
		this->view_tick = std::min<double>(this->view_tick, this->tick_count);
		if (this->tick_count - this->view_tick > 4 * CAB_VIEW_3D_LAG) {
			this->view_tick = static_cast<double>(this->tick_count) - CAB_VIEW_3D_LAG;
			this->view_lag = CAB_VIEW_3D_LAG;
		}
	}

	/**
	 * Get the camera of the 3D view at the moment it shows, between the cameras of the game ticks around it.
	 * @return The camera.
	 */
	CabViewCamera Get3DCamera() const
	{
		if (this->tick_cameras.empty()) return {};
		auto next = std::ranges::find_if(this->tick_cameras, [this](const TickCamera &tc) { return tc.tick > this->view_tick; });
		if (next == this->tick_cameras.begin()) return this->tick_cameras.front().camera;
		if (next == this->tick_cameras.end()) return this->tick_cameras.back().camera;

		const CabViewCamera &from = std::prev(next)->camera;
		const CabViewCamera &to = next->camera;
		float f = static_cast<float>((this->view_tick - std::prev(next)->tick) / (next->tick - std::prev(next)->tick));
		CabViewCamera camera = to;
		camera.x = from.x + (to.x - from.x) * f;
		camera.y = from.y + (to.y - from.y) * f;
		camera.z = from.z + (to.z - from.z) * f;
		camera.yaw = from.yaw + std::remainder(to.yaw - from.yaw, 2.0f * static_cast<float>(M_PI)) * f;
		return camera;
	}

	/**
	 * Find the point ahead along the track of a train where the camera should look.
	 * Walks the track from the front of the train until it leaves the ellipse with the
	 * given half axes around the train, so in curves the camera looks around the corner.
	 * At junctions it follows the reserved path, if there is one, and otherwise stops.
	 * @param t The train.
	 * @param ax Horizontal half axis of the ellipse, in virtual viewport coordinates.
	 * @param ay Vertical half axis of the ellipse, in virtual viewport coordinates.
	 * @return Offset from the train in virtual viewport coordinates, or std::nullopt when the track cannot be followed.
	 */
	static std::optional<Point> GetTrackLookAheadTarget(const Train *t, float ax, float ay)
	{
		/* In depots, tunnels and on bridges, fall back to the direction of travel. */
		if (t->track == TRACK_BIT_DEPOT || t->track == TRACK_BIT_WORMHOLE) return std::nullopt;
		Trackdir td = t->GetVehicleTrackdir();
		if (td == INVALID_TRACKDIR) return std::nullopt;

		Point base = RemapCoords(t->x_pos, t->y_pos, t->z_pos);
		Point last = {0, 0};
		TileIndex tile = t->tile;
		CFollowTrackRail ft(t);

		for (int i = 0; i < CAB_VIEW_MAX_TRACK_TILES; i++) {
			if (!ft.Follow(tile, td)) break;
			/* Do not turn around at the end of the line. */
			if (ft.new_tile == tile) break;

			TrackdirBits options = ft.new_td_bits;
			if (KillFirstBit(options) != TRACKDIR_BIT_NONE) {
				/* A junction: only continue when we know which way the train goes. */
				TrackdirBits reserved = options & TrackBitsToTrackdirBits(GetReservedTrackbits(ft.new_tile));
				if (!HasExactlyOneBit(reserved)) break;
				options = reserved;
			}

			tile = ft.new_tile;
			td = FindFirstTrackdir(options);

			int x = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
			int y = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
			Point pt = RemapCoords(x, y, GetSlopePixelZ(x, y));
			Point d = {pt.x - base.x, pt.y - base.y};

			float e = (d.x / ax) * (d.x / ax) + (d.y / ay) * (d.y / ay);
			if (e >= 1.0f) {
				/* Bring the point back onto the edge of the ellipse. */
				float f = 1.0f / std::sqrt(e);
				return Point{static_cast<int>(d.x * f), static_cast<int>(d.y * f)};
			}
			last = d;
		}

		return last;
	}

	/**
	 * Determine where the camera should be, relative to the vehicle.
	 * @param v The vehicle we ride along with.
	 * @return Offset in virtual viewport coordinates.
	 */
	Point GetLookAheadTarget(const Vehicle *v) const
	{
		if (!_cab_view_look_ahead) return {0, 0};

		/* Look further ahead the faster we go, but keep some look-ahead when standing still. */
		int max_speed = v->GetDisplayMaxSpeed();
		float speed_frac = max_speed > 0 ? std::clamp(static_cast<float>(v->GetDisplaySpeed()) / max_speed, 0.0f, 1.0f) : 0.0f;
		float reach = CAB_VIEW_REACH_MIN + (CAB_VIEW_REACH_MAX - CAB_VIEW_REACH_MIN) * speed_frac;
		float ax = reach * this->viewport->virtual_width / 2;
		float ay = reach * this->viewport->virtual_height / 2;
		if (ax < 1 || ay < 1) return {0, 0};

		if (v->type == VEH_TRAIN) {
			std::optional<Point> target = GetTrackLookAheadTarget(Train::From(v), ax, ay);
			if (target.has_value()) return *target;
		}

		/* Direction of travel projected onto the screen, see RemapCoords. */
		TileIndexDiffC diff = TileIndexDiffCByDir(v->direction);
		float sx = (diff.y - diff.x) * 2.0f;
		float sy = static_cast<float>(diff.y + diff.x);
		float len = std::hypot(sx, sy);
		if (len == 0) return {0, 0};

		return {static_cast<int>(sx / len * ax), static_cast<int>(sy / len * ay)};
	}

	/** Push the current look-ahead offset to the viewport. */
	void ApplyOffset()
	{
		this->viewport->follow_offset_x = static_cast<int32_t>(std::lround(this->offset_x));
		this->viewport->follow_offset_y = static_cast<int32_t>(std::lround(this->offset_y));
	}

	/**
	 * Have a viewport only while the map view is shown. Left behind the 3D view, it would still follow the train,
	 * and as a viewport moves the pixels on the screen along when it scrolls, the 3D view would shake to and fro.
	 */
	void UpdateViewport()
	{
		if (this->Is3D()) {
			if (this->viewport == nullptr) return;
			this->map_zoom = this->viewport->zoom;
			this->viewport.reset();
		} else if (this->viewport == nullptr) {
			this->GetWidget<NWidgetViewport>(WID_CV_VIEWPORT)->InitializeViewport(this, static_cast<VehicleID>(this->window_number), this->map_zoom);
			this->ApplyOffset();
		}
	}

	/**
	 * Get the text describing where the vehicle is heading.
	 * @param v The vehicle.
	 * @return The destination text.
	 */
	static std::string GetDestinationString(const Vehicle *v)
	{
		switch (v->current_order.GetType()) {
			case OT_GOTO_STATION:
				return GetString(STR_CAB_VIEW_NEXT_STATION, v->current_order.GetDestination());

			case OT_GOTO_WAYPOINT:
				return GetString(STR_CAB_VIEW_NEXT_WAYPOINT, v->current_order.GetDestination());

			case OT_GOTO_DEPOT:
				if (v->current_order.GetDestination() == DepotID::Invalid()) break;
				return GetString(STR_CAB_VIEW_NEXT_DEPOT, v->type, v->current_order.GetDestination());

			case OT_LOADING:
				if (v->last_station_visited == StationID::Invalid()) break;
				return GetString(STR_CAB_VIEW_AT_STATION, v->last_station_visited);

			default:
				break;
		}
		return GetString(STR_CAB_VIEW_NO_DESTINATION);
	}

	/**
	 * Get the text describing what the vehicle is doing.
	 * @param v The vehicle.
	 * @return The state text.
	 */
	static std::string GetStateString(const Vehicle *v)
	{
		if (v->vehstatus.Test(VehState::Crashed)) return GetString(STR_VEHICLE_STATUS_CRASHED);
		if (v->type != VEH_AIRCRAFT && v->breakdown_ctr == 1) return GetString(STR_VEHICLE_STATUS_BROKEN_DOWN);
		if (v->vehstatus.Test(VehState::Stopped)) return GetString(STR_VEHICLE_STATUS_STOPPED);
		if (v->current_order.IsType(OT_LOADING)) return GetString(STR_VEHICLE_STATUS_LOADING_UNLOADING);
		if (v->type == VEH_TRAIN && Train::From(v)->flags.Test(VehicleRailFlag::Stuck)) return GetString(STR_VEHICLE_STATUS_TRAIN_STUCK);
		if (v->current_order.IsType(OT_LEAVESTATION) && v->type != VEH_AIRCRAFT) return GetString(STR_VEHICLE_STATUS_LEAVING);
		return GetString(v->GetDisplaySpeed() == 0 ? STR_CAB_VIEW_STATE_STANDING : STR_CAB_VIEW_STATE_RUNNING);
	}

	/**
	 * Draw a horizontal gauge.
	 * @param r Area of the gauge.
	 * @param fraction How much of the gauge is filled, in the range 0..1.
	 * @param colour Colour of the filled part.
	 */
	static void DrawGauge(const Rect &r, float fraction, PixelColour colour)
	{
		bool rtl = _current_text_dir == TD_RTL;
		GfxFillRect(r, PC_DARK_GREY);
		int filled = static_cast<int>(std::round(r.Width() * std::clamp(fraction, 0.0f, 1.0f)));
		if (filled > 0) GfxFillRect(r.WithWidth(filled, rtl), colour);
	}

	/** Height of the gauges on the dashboard. */
	static int GetGaugeHeight()
	{
		return ScaleGUITrad(5);
	}

	/**
	 * Get the point on a circle at a given position of the speedometer dial.
	 * @param cx Horizontal centre of the dial.
	 * @param cy Vertical centre of the dial.
	 * @param radius Distance from the centre.
	 * @param fraction Position on the dial, 0 for standstill and 1 for maximum speed.
	 * @return The point on the screen.
	 */
	static Point GetDialPoint(int cx, int cy, float radius, float fraction)
	{
		float angle = (CAB_VIEW_DIAL_START - CAB_VIEW_DIAL_SWEEP * fraction) * static_cast<float>(M_PI) / 180.0f;
		return {cx + static_cast<int>(std::lround(radius * std::cos(angle))), cy - static_cast<int>(std::lround(radius * std::sin(angle)))};
	}

	/**
	 * Draw an arc of the speedometer dial.
	 * @param cx Horizontal centre of the dial.
	 * @param cy Vertical centre of the dial.
	 * @param radius Radius of the arc.
	 * @param from Start of the arc, as fraction of the dial.
	 * @param to End of the arc, as fraction of the dial.
	 * @param colour Colour of the arc.
	 * @param width Width of the line.
	 */
	static void DrawDialArc(int cx, int cy, float radius, float from, float to, PixelColour colour, int width)
	{
		if (to <= from) return;
		/* Roughly one segment per 5 degrees. */
		int segments = std::max(1, static_cast<int>(std::ceil((to - from) * CAB_VIEW_DIAL_SWEEP / 5.0f)));
		Point prev = GetDialPoint(cx, cy, radius, from);
		for (int i = 1; i <= segments; i++) {
			Point next = GetDialPoint(cx, cy, radius, from + (to - from) * i / segments);
			GfxDrawLine(prev.x, prev.y, next.x, next.y, colour, width);
			prev = next;
		}
	}

	/**
	 * Draw the speedometer dial.
	 * @param r Area to draw the dial in; it is centred in it.
	 * @param speed_frac Current speed as fraction of the maximum speed of the vehicle.
	 */
	static void DrawSpeedometer(const Rect &r, float speed_frac)
	{
		/* The dial sweeps 270 degrees, so it is a bit wider than high. */
		int radius = std::min(r.Width() / 2, r.Height() / 2);
		int cx = (r.left + r.right) / 2;
		int cy = (r.top + r.bottom) / 2;
		int line = std::max(1, ScaleGUITrad(1));

		/* The face. */
		std::vector<Point> face;
		for (int i = 0; i < 48; i++) {
			float angle = i * 2.0f * static_cast<float>(M_PI) / 48;
			face.push_back({cx + static_cast<int>(std::lround(radius * std::cos(angle))), cy + static_cast<int>(std::lround(radius * std::sin(angle)))});
		}
		GfxFillPolygon(face, CAB_VIEW_DIAL_COLOUR);

		/* The scale, with a red zone beyond the maximum speed of the vehicle. */
		float scale_radius = radius - 2.0f * line;
		DrawDialArc(cx, cy, scale_radius, 0.0f, CAB_VIEW_DIAL_MAX_SPEED, PC_GREY, line);
		DrawDialArc(cx, cy, scale_radius, CAB_VIEW_DIAL_MAX_SPEED, 1.0f, PC_RED, line * 2);
		for (int i = 0; i <= CAB_VIEW_DIAL_TICKS; i++) {
			float f = static_cast<float>(i) / CAB_VIEW_DIAL_TICKS;
			bool major = i % 2 == 0;
			Point outer = GetDialPoint(cx, cy, scale_radius, f);
			Point inner = GetDialPoint(cx, cy, scale_radius - (major ? 4 : 2) * line, f);
			GfxDrawLine(outer.x, outer.y, inner.x, inner.y, major ? PC_WHITE : PC_GREY, line);
		}

		/* What part of the scale we are using. */
		float clamped = std::clamp(speed_frac * CAB_VIEW_DIAL_MAX_SPEED, 0.0f, 1.0f);
		DrawDialArc(cx, cy, scale_radius - 6.0f * line, 0.0f, clamped, speed_frac > 0.95f ? PC_YELLOW : PC_GREEN, line * 2);

		/* The needle, with its hub. */
		Point tip = GetDialPoint(cx, cy, scale_radius - 2.0f * line, clamped);
		GfxDrawLine(cx, cy, tip.x, tip.y, PC_ORANGE, line * 2);
		int hub = 2 * line;
		GfxFillRect(cx - hub, cy - hub, cx + hub, cy + hub, PC_WHITE);
	}

public:
	CabViewWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->flags.Set(WindowFlag::DisableVpScroll);
		this->InitNested(window_number);

		const Vehicle *v = Vehicle::Get(window_number);
		this->owner = v->owner;

		this->map_zoom = std::clamp(ScaleZoomGUI(ZoomLevel::In2x), _settings_client.gui.zoom_min, _settings_client.gui.zoom_max);
		this->GetWidget<NWidgetViewport>(WID_CV_VIEWPORT)->InitializeViewport(this, static_cast<VehicleID>(window_number), this->map_zoom);

		/* Start in the look-ahead position, instead of sliding into it. */
		Point target = this->GetLookAheadTarget(v);
		this->offset_x = target.x;
		this->offset_y = target.y;
		this->ApplyOffset();

		if (v->type == VEH_TRAIN) this->Update3DTick(true);
		this->OnInvalidateData();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_CV_DASHBOARD: {
				int sep = WidgetDimensions::scaled.vsep_normal;
				int speed_column = std::max(ScaleGUITrad(CAB_VIEW_DIAL_HEIGHT), GetCharacterHeight(FS_LARGE) + sep + GetCharacterHeight(FS_SMALL));
				int text_column = GetCharacterHeight(FS_NORMAL) * 2 + sep + GetGaugeHeight();
				size.height = std::max(speed_column, text_column) + WidgetDimensions::scaled.frametext.Vertical() + WidgetDimensions::scaled.bevel.Vertical();

				/* Make room for the speed of this vehicle, but at least for three digits. */
				const Vehicle *v = Vehicle::Get(this->window_number);
				uint64_t widest_speed = PackVelocity(std::max(v->GetDisplayMaxSpeed(), 999), v->type);
				int digits_width = std::max(GetStringBoundingBox(GetString(STR_CAB_VIEW_SPEED, widest_speed), FS_LARGE).width, GetStringBoundingBox(GetString(STR_CAB_VIEW_MAX_SPEED, widest_speed), FS_SMALL).width);
				int gap = WidgetDimensions::scaled.hsep_wide * 2;
				this->speed_width = speed_column + gap + digits_width;
				this->load_width = std::max<int>(GetStringBoundingBox(GetString(STR_CAB_VIEW_LOAD, 100)).width, ScaleGUITrad(80));

				int info_width = ScaleGUITrad(160);
				size.width = std::max<uint>(size.width, this->speed_width + gap + info_width + gap + this->load_width + WidgetDimensions::scaled.frametext.Horizontal() + WidgetDimensions::scaled.bevel.Horizontal());
				break;
			}

			case WID_CV_START_STOP: {
				Dimension d = maxdim(GetStringBoundingBox(STR_CAB_VIEW_START), GetStringBoundingBox(STR_CAB_VIEW_STOP));
				size = maxdim(size, {d.width + padding.width, d.height + padding.height});
				break;
			}
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_CV_CAPTION:
				return GetString(STR_CAB_VIEW_CAPTION, Vehicle::Get(this->window_number)->index);

			case WID_CV_START_STOP:
				return GetString(Vehicle::Get(this->window_number)->vehstatus.Test(VehState::Stopped) ? STR_CAB_VIEW_START : STR_CAB_VIEW_STOP);

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void OnPaint() override
	{
		const Vehicle *v = Vehicle::Get(this->window_number);
		this->SetWidgetDisabledState(WID_CV_START_STOP, v->owner != _local_company);
		this->DrawWidgets();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_CV_3D_VIEW) {
			DrawCabView3D(r, this->drawn_camera, this->frame_id, this->frame_budget);
			return;
		}
		if (widget != WID_CV_DASHBOARD) return;

		const Vehicle *v = Vehicle::Get(this->window_number);
		bool rtl = _current_text_dir == TD_RTL;

		/* The dashboard itself: a dark display. */
		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		GfxFillRect(ir, CAB_VIEW_DASHBOARD_COLOUR);
		ir = ir.Shrink(WidgetDimensions::scaled.frametext);

		int sep = WidgetDimensions::scaled.vsep_normal;
		int gap = WidgetDimensions::scaled.hsep_wide * 2;

		/* Left: speedometer dial with the speed next to it. */
		Rect speed = ir.WithWidth(this->speed_width, rtl);
		int max_speed = v->GetDisplayMaxSpeed();
		float speed_frac = max_speed > 0 ? static_cast<float>(v->GetDisplaySpeed()) / max_speed : 0.0f;

		Rect dial = speed.WithWidth(speed.Height(), rtl);
		DrawSpeedometer(dial, speed_frac);

		Rect digits = speed.Indent(dial.Width() + gap, rtl);
		int text_height = GetCharacterHeight(FS_LARGE) + sep + GetCharacterHeight(FS_SMALL);
		int y = CentreBounds(digits.top, digits.bottom, text_height);
		DrawString(digits.left, digits.right, y, GetString(STR_CAB_VIEW_SPEED, PackVelocity(v->GetDisplaySpeed(), v->type)), TC_WHITE, SA_LEFT, false, FS_LARGE);
		y += GetCharacterHeight(FS_LARGE) + sep;
		if (max_speed > 0) {
			DrawString(digits.left, digits.right, y, GetString(STR_CAB_VIEW_MAX_SPEED, PackVelocity(max_speed, v->type)), TC_SILVER, SA_LEFT, false, FS_SMALL);
		}

		/* Right: load. */
		Rect load = ir.WithWidth(this->load_width, !rtl);
		uint capacity = 0;
		uint stored = 0;
		for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
			capacity += u->cargo_cap;
			stored += u->cargo.StoredCount();
		}
		y = load.top;
		if (capacity > 0) {
			DrawString(load.left, load.right, y, GetString(STR_CAB_VIEW_LOAD, stored * 100 / capacity), TC_SILVER);
			y += GetCharacterHeight(FS_NORMAL) + sep;
			DrawGauge({load.left, y, load.right, y + GetGaugeHeight() - 1}, static_cast<float>(stored) / capacity, PC_LIGHT_BLUE);
		} else {
			DrawString(load.left, load.right, y, GetString(STR_CAB_VIEW_LOAD_NONE), TC_SILVER);
		}

		/* Middle: where we are heading and what we are doing. */
		Rect info = ir.Indent(this->speed_width + gap, rtl).Indent(this->load_width + gap, !rtl);
		y = info.top;
		DrawString(info.left, info.right, y, GetDestinationString(v), TC_SILVER);
		y += GetCharacterHeight(FS_NORMAL) + sep;
		DrawString(info.left, info.right, y, GetStateString(v), TC_WHITE);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_CV_ZOOM_IN: DoZoomInOutWindow(ZOOM_IN, this); break;
			case WID_CV_ZOOM_OUT: DoZoomInOutWindow(ZOOM_OUT, this); break;

			case WID_CV_LOOK_AHEAD:
				_cab_view_look_ahead = !_cab_view_look_ahead;
				InvalidateWindowClassesData(WC_VEHICLE_CAB_VIEW);
				break;

			case WID_CV_3D:
				_cab_view_3d = !_cab_view_3d;
				/* Start with fresh graphics, in case they changed since the last time. */
				if (_cab_view_3d) ResetCabView3D();
				InvalidateWindowClassesData(WC_VEHICLE_CAB_VIEW);
				break;

			case WID_CV_START_STOP:
				StartStopVehicle(Vehicle::Get(this->window_number), true);
				break;
		}
	}

	void OnGameTick() override
	{
		if (this->Is3D()) this->Update3DTick(false);
	}

	void OnRealtimeTick(uint delta_ms) override
	{
		if (this->viewport != nullptr) {
			/* Ease towards the target, so turns and reversing don't make the camera jump. */
			const Vehicle *v = Vehicle::Get(this->window_number);
			Point target = this->GetLookAheadTarget(v);
			float alpha = 1.0f - std::exp(-static_cast<float>(delta_ms) / CAB_VIEW_EASE_MS);
			this->offset_x += (target.x - this->offset_x) * alpha;
			this->offset_y += (target.y - this->offset_y) * alpha;
			this->ApplyOffset();
		}

		if (this->Is3D()) {
			/* The time is measured here, as delta_ms is rounded down and would make the pace uneven. */
			this->Advance3DClock();
			auto now = std::chrono::steady_clock::now();
			float ms = std::chrono::duration<float, std::milli>(now - this->redraw_time).count();
			this->redraw_time = now;
			if (ms < 100) this->redraw_interval += (ms - this->redraw_interval) * 0.05f;

			/* Draw the 3D view on every redraw of the screen when it can be done quickly enough, as fewer
			 * pictures than the screen shows look jerky; if needed, the picture is made smaller for that.
			 * Otherwise draw it on every second (third...) redraw, evenly.
			 * Slow drawing delays the redraws, so the measured time between them is only trusted when it is shorter than intended. */
			float interval = std::min(this->redraw_interval, 1000.0f / std::max<int>(_settings_client.gui.refresh_rate, 1));
			float budget = interval * CAB_VIEW_3D_FRAME_BUDGET;
			float cost = GetCabView3DFrameTime(CAB_VIEW_3D_MIN_SCALE);
			int divider = this->frame_divider;
			if (cost > budget * divider) {
				divider = std::min(divider + 1, CAB_VIEW_3D_MAX_FRAME_DIVIDER);
			} else if (cost < budget * (divider - 1) * 0.75f) {
				divider--;
			}
			if (divider == this->frame_divider) {
				this->pace_time = now;
			} else if (now - this->pace_time >= CAB_VIEW_3D_PACE_DELAY) {
				Debug(misc, 3, "[CabView3D] picture on every {}. redraw: drawing takes at least {:.1f} ms, redraws every {:.1f} ms", divider, cost, interval);
				this->frame_divider = divider;
				this->pace_time = now;
			}
			this->frame_budget = budget * this->frame_divider;

			/* Redraw the 3D view when the camera moved; when standing still, still now and then, as the world around may change. */
			this->redraws_since_frame++;
			CabViewCamera camera = this->Get3DCamera();
			if ((this->redraws_since_frame >= this->frame_divider && camera != this->drawn_camera) || now - this->frame_time >= CAB_VIEW_3D_IDLE_FRAME_TIME) {
				this->frame_time = now;
				this->redraws_since_frame = 0;
				this->drawn_camera = camera;
				this->frame_id++;
				Debug(misc, 4, "[CabView3D] picture at {:.3f} ms: camera {:.3f},{:.3f}, game tick {:.3f} of {}",
					std::chrono::duration<double, std::milli>(now.time_since_epoch()).count(), camera.x, camera.y, this->view_tick, this->tick_count);
				this->SetWidgetDirty(WID_CV_3D_VIEW);
			}
		}

		this->dashboard_timer += delta_ms;
		if (this->dashboard_timer >= CAB_VIEW_DASHBOARD_REFRESH_MS) {
			this->dashboard_timer = 0;
			this->SetWidgetDirty(WID_CV_DASHBOARD);
			this->SetWidgetDirty(WID_CV_START_STOP);
		}
	}

	void OnMouseWheel(int wheel, WidgetID widget) override
	{
		if (widget != WID_CV_VIEWPORT || this->Is3D()) return;
		if (_settings_client.gui.scrollwheel_scrolling != SWS_OFF) {
			DoZoomInOutWindow(wheel < 0 ? ZOOM_IN : ZOOM_OUT, this);
		}
	}

	void OnResize() override
	{
		if (this->viewport != nullptr) {
			NWidgetViewport *nvp = this->GetWidget<NWidgetViewport>(WID_CV_VIEWPORT);
			nvp->UpdateViewportCoordinates(this);
		}
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (data == VIWD_AUTOREPLACE || !gui_scope) return;

		const Vehicle *v = Vehicle::Get(this->window_number);
		bool is_3d = this->Is3D();
		this->SetWidgetLoweredState(WID_CV_LOOK_AHEAD, _cab_view_look_ahead);
		this->SetWidgetLoweredState(WID_CV_3D, is_3d);
		this->SetWidgetDisabledState(WID_CV_3D, v->type != VEH_TRAIN);

		NWidgetStacked *selection = this->GetWidget<NWidgetStacked>(WID_CV_VIEW_SELECT);
		if (selection->shown_plane != (is_3d ? 1 : 0)) {
			selection->SetDisplayedPlane(is_3d ? 1 : 0);
			this->ReInit();
			this->OnResize();
			if (is_3d) {
				this->Update3DTick(true);
				this->drawn_camera = this->Get3DCamera();
			}
		}
		this->UpdateViewport();

		if (this->viewport != nullptr) HandleZoomMessage(this, *this->viewport, WID_CV_ZOOM_IN, WID_CV_ZOOM_OUT);
		/* Zoom and look-ahead are for the map view only. */
		if (is_3d) this->SetWidgetsDisabledState(true, WID_CV_ZOOM_IN, WID_CV_ZOOM_OUT);
		this->SetWidgetDisabledState(WID_CV_LOOK_AHEAD, is_3d);
		this->SetDirty();
	}
};

static WindowDesc _cab_view_desc(
	WDP_CENTER, "view_vehicle_cab", 640, 420,
	WC_VEHICLE_CAB_VIEW, WC_NONE,
	{},
	_nested_cab_view_widgets
);

/**
 * Draw the icon of the cab view button: the front of a locomotive, with its windscreen and headlights.
 * It is drawn instead of using a sprite, so it stays sharp at any interface size.
 * @param r Area of the button.
 */
void DrawCabViewIcon(const Rect &r)
{
	/* The icon is designed on a grid of 11x12 units. */
	static constexpr int GRID_W = 11;
	static constexpr int GRID_H = 12;
	int unit = std::max(1, std::min(r.Width() / (GRID_W + 3), r.Height() / (GRID_H + 2)));
	int left = CentreBounds(r.left, r.right, GRID_W * unit);
	int top = CentreBounds(r.top, r.bottom, GRID_H * unit);

	/* Fill grid cells [x1, x2) x [y1, y2). */
	auto cells = [&](int x1, int y1, int x2, int y2, PixelColour colour) {
		GfxFillRect(left + x1 * unit, top + y1 * unit, left + x2 * unit - 1, top + y2 * unit - 1, colour);
	};

	cells(1, 0, 10, 1, PC_DARK_GREY);  // roof
	cells(0, 1, 11, 11, PC_DARK_RED);  // body outline
	cells(1, 1, 10, 10, PC_RED);       // body
	cells(1, 2, 5, 6, PC_LIGHT_BLUE);  // windscreen, left
	cells(6, 2, 10, 6, PC_LIGHT_BLUE); // windscreen, right
	cells(2, 7, 4, 9, PC_YELLOW);      // headlight, left
	cells(7, 7, 9, 9, PC_YELLOW);      // headlight, right
	cells(0, 11, 11, 12, PC_BLACK);    // buffer beam
}

/**
 * Show the cab view of a vehicle.
 * @param v The (primary) vehicle to ride along with.
 */
void ShowCabViewWindow(const Vehicle *v)
{
	AllocateWindowDescFront<CabViewWindow>(_cab_view_desc, v->index);
}
