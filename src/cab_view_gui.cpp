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
				int speed_column = GetCharacterHeight(FS_LARGE) + sep + GetGaugeHeight() + sep + GetCharacterHeight(FS_SMALL);
				int text_column = GetCharacterHeight(FS_NORMAL) * 2 + sep;
				size.height = std::max(speed_column, text_column) + WidgetDimensions::scaled.frametext.Vertical() + WidgetDimensions::scaled.bevel.Vertical();
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
		GfxFillRect(ir, PC_BLACK);
		ir = ir.Shrink(WidgetDimensions::scaled.frametext);

		int sep = WidgetDimensions::scaled.vsep_normal;
		int gap = WidgetDimensions::scaled.hsep_wide * 2;
		int column_width = (ir.Width() - 2 * gap);

		/* Left: speedometer. */
		Rect speed = ir.WithWidth(column_width * 3 / 10, rtl);
		int y = speed.top;
		DrawString(speed.left, speed.right, y, GetString(STR_CAB_VIEW_SPEED, PackVelocity(v->GetDisplaySpeed(), v->type)), TC_WHITE, SA_LEFT, false, FS_LARGE);
		y += GetCharacterHeight(FS_LARGE) + sep;

		int max_speed = v->GetDisplayMaxSpeed();
		float speed_frac = max_speed > 0 ? static_cast<float>(v->GetDisplaySpeed()) / max_speed : 0.0f;
		PixelColour speed_colour = speed_frac > 0.95f ? PC_YELLOW : PC_GREEN;
		DrawGauge({speed.left, y, speed.right, y + GetGaugeHeight() - 1}, speed_frac, speed_colour);
		y += GetGaugeHeight() + sep;

		if (max_speed > 0) {
			DrawString(speed.left, speed.right, y, GetString(STR_CAB_VIEW_MAX_SPEED, PackVelocity(max_speed, v->type)), TC_SILVER, SA_LEFT, false, FS_SMALL);
		}

		/* Right: load. */
		Rect load = ir.WithWidth(column_width * 2 / 10, !rtl);
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
		Rect info = ir.Indent(column_width * 3 / 10 + gap, rtl).Indent(column_width * 2 / 10 + gap, !rtl);
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
 * Show the cab view of a vehicle.
 * @param v The (primary) vehicle to ride along with.
 */
void ShowCabViewWindow(const Vehicle *v)
{
	AllocateWindowDescFront<CabViewWindow>(_cab_view_desc, v->index);
}
