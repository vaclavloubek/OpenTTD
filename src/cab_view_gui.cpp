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
#include "company_func.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "map_func.h"
#include "palette_func.h"
#include "gfx_func.h"
#include "settings_type.h"
#include "core/geometry_func.hpp"

#include "widgets/cab_view_widget.h"

#include "table/strings.h"
#include "table/sprites.h"

#include "safeguards.h"

/** Whether cab views look ahead in the direction of travel. Shared by all cab views. */
static bool _cab_view_look_ahead = true;

/** Time constant (in milliseconds) of the camera easing towards its look-ahead position. */
static constexpr float CAB_VIEW_EASE_MS = 650.0f;
/** Look-ahead when standing still, as fraction of half the viewport size. */
static constexpr float CAB_VIEW_REACH_MIN = 0.30f;
/** Look-ahead at full speed, as fraction of half the viewport size. */
static constexpr float CAB_VIEW_REACH_MAX = 0.60f;
/** How often (in milliseconds) the dashboard is refreshed. */
static constexpr uint CAB_VIEW_DASHBOARD_REFRESH_MS = 100;
/** Minimal height of the speedometer dial, in unscaled GUI pixels. */
static constexpr int CAB_VIEW_DIAL_HEIGHT = 52;
/** Angle (in degrees, counter-clockwise from the right) of the speedometer dial at standstill. */
static constexpr float CAB_VIEW_DIAL_START = 225.0f;
/** Angle (in degrees) the speedometer needle sweeps from standstill to the maximum speed. */
static constexpr float CAB_VIEW_DIAL_SWEEP = 270.0f;
/** Number of major ticks on the speedometer dial. */
static constexpr int CAB_VIEW_DIAL_TICKS = 10;
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
			NWidget(NWID_VIEWPORT, INVALID_COLOUR, WID_CV_VIEWPORT), SetMinimalSize(320, 180), SetResize(1, 1), SetFill(1, 1),
		EndContainer(),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_CV_DASHBOARD), SetResize(1, 0), SetFill(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_CV_ZOOM_IN), SetSpriteTip(SPR_IMG_ZOOMIN, STR_TOOLBAR_TOOLTIP_ZOOM_THE_VIEW_IN),
		NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_CV_ZOOM_OUT), SetSpriteTip(SPR_IMG_ZOOMOUT, STR_TOOLBAR_TOOLTIP_ZOOM_THE_VIEW_OUT),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_CV_LOOK_AHEAD), SetFill(0, 1), SetStringTip(STR_CAB_VIEW_LOOK_AHEAD, STR_CAB_VIEW_LOOK_AHEAD_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_CV_START_STOP), SetFill(1, 1), SetResize(1, 0), SetStringTip(STR_CAB_VIEW_STOP, STR_CAB_VIEW_START_STOP_TOOLTIP),
		NWidget(WWT_RESIZEBOX, COLOUR_GREY),
	EndContainer(),
};

/** Window riding along with a vehicle, like looking out of its cab. */
struct CabViewWindow : Window {
private:
	float offset_x = 0; ///< Current look-ahead offset in virtual viewport coordinates.
	float offset_y = 0; ///< Current look-ahead offset in virtual viewport coordinates.
	uint dashboard_timer = 0; ///< Milliseconds since the dashboard was last redrawn.
	int speed_width = 0; ///< Width of the speed part of the dashboard, dial included.
	int load_width = 0; ///< Width of the load part of the dashboard.

	/**
	 * Determine where the camera should be, relative to the vehicle.
	 * @param v The vehicle we ride along with.
	 * @return Offset in virtual viewport coordinates.
	 */
	Point GetLookAheadTarget(const Vehicle *v) const
	{
		if (!_cab_view_look_ahead) return {0, 0};

		/* Direction of travel projected onto the screen, see RemapCoords. */
		TileIndexDiffC diff = TileIndexDiffCByDir(v->direction);
		float sx = (diff.y - diff.x) * 2.0f;
		float sy = static_cast<float>(diff.y + diff.x);
		float len = std::hypot(sx, sy);
		if (len == 0) return {0, 0};
		sx /= len;
		sy /= len;

		/* Look further ahead the faster we go, but keep some look-ahead when standing still. */
		int max_speed = v->GetDisplayMaxSpeed();
		float speed_frac = max_speed > 0 ? std::clamp(static_cast<float>(v->GetDisplaySpeed()) / max_speed, 0.0f, 1.0f) : 0.0f;
		float reach = CAB_VIEW_REACH_MIN + (CAB_VIEW_REACH_MAX - CAB_VIEW_REACH_MIN) * speed_frac;

		return {static_cast<int>(sx * reach * this->viewport->virtual_width / 2), static_cast<int>(sy * reach * this->viewport->virtual_height / 2)};
	}

	/** Push the current look-ahead offset to the viewport. */
	void ApplyOffset()
	{
		this->viewport->follow_offset_x = static_cast<int32_t>(std::lround(this->offset_x));
		this->viewport->follow_offset_y = static_cast<int32_t>(std::lround(this->offset_y));
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
	 * @param speed_frac Current speed as fraction of the maximum speed.
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

		/* The scale, with a warning zone near the maximum speed. */
		float scale_radius = radius - 2.0f * line;
		DrawDialArc(cx, cy, scale_radius, 0.0f, 0.9f, PC_GREY, line);
		DrawDialArc(cx, cy, scale_radius, 0.9f, 1.0f, PC_RED, line * 2);
		for (int i = 0; i <= CAB_VIEW_DIAL_TICKS; i++) {
			float f = static_cast<float>(i) / CAB_VIEW_DIAL_TICKS;
			bool major = i % 2 == 0;
			Point outer = GetDialPoint(cx, cy, scale_radius, f);
			Point inner = GetDialPoint(cx, cy, scale_radius - (major ? 4 : 2) * line, f);
			GfxDrawLine(outer.x, outer.y, inner.x, inner.y, major ? PC_WHITE : PC_GREY, line);
		}

		/* What part of the scale we are using. */
		float clamped = std::clamp(speed_frac, 0.0f, 1.0f);
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

		ZoomLevel zoom = std::clamp(ScaleZoomGUI(ZoomLevel::In2x), _settings_client.gui.zoom_min, _settings_client.gui.zoom_max);
		this->GetWidget<NWidgetViewport>(WID_CV_VIEWPORT)->InitializeViewport(this, static_cast<VehicleID>(window_number), zoom);

		/* Start in the look-ahead position, instead of sliding into it. */
		Point target = this->GetLookAheadTarget(v);
		this->offset_x = target.x;
		this->offset_y = target.y;
		this->ApplyOffset();

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

			case WID_CV_START_STOP:
				StartStopVehicle(Vehicle::Get(this->window_number), true);
				break;
		}
	}

	void OnRealtimeTick(uint delta_ms) override
	{
		if (this->viewport == nullptr) return;

		/* Ease towards the target, so turns and reversing don't make the camera jump. */
		const Vehicle *v = Vehicle::Get(this->window_number);
		Point target = this->GetLookAheadTarget(v);
		float alpha = 1.0f - std::exp(-static_cast<float>(delta_ms) / CAB_VIEW_EASE_MS);
		this->offset_x += (target.x - this->offset_x) * alpha;
		this->offset_y += (target.y - this->offset_y) * alpha;
		this->ApplyOffset();

		this->dashboard_timer += delta_ms;
		if (this->dashboard_timer >= CAB_VIEW_DASHBOARD_REFRESH_MS) {
			this->dashboard_timer = 0;
			this->SetWidgetDirty(WID_CV_DASHBOARD);
			this->SetWidgetDirty(WID_CV_START_STOP);
		}
	}

	void OnMouseWheel(int wheel, WidgetID widget) override
	{
		if (widget != WID_CV_VIEWPORT) return;
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

		this->SetWidgetLoweredState(WID_CV_LOOK_AHEAD, _cab_view_look_ahead);
		if (this->viewport != nullptr) HandleZoomMessage(this, *this->viewport, WID_CV_ZOOM_IN, WID_CV_ZOOM_OUT);
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
