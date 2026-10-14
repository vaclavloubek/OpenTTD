/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cab_view_rec.cpp Recording videos frame by frame (development tool).
 *
 * With REC_SCRIPT set, the game runs on a virtual clock that advances exactly one redraw interval per drawn
 * frame, so the video is smooth however long drawing takes. Every frame is piped to the command in REC_PIPE
 * (raw BGRA, screen size). The script moves the mouse and clicks, by frame number; one command per line:
 *   <frame> move <frames> xy <x> <y> | veh <id> | vehin <window> <id> | wid <window> <widget>
 *   <frame> click
 *   <frame> follow <vehicle id>          (main view follows the vehicle)
 *   <frame> win <window> <x> <y> <w> <h> (move and resize a window)
 *   <frame> news <text, \n for a new line>
 *   <frame> cab <vehicle id>             (open the cab view of the vehicle)
 *   <frame> zoomin                       (zoom the main view in)
 *   <frame> 3d                           (toggle the 3D view in the cab view)
 *   <frame> quit
 * Windows: main, veh (vehicle view), cab (cab view), news.
 */

#include "stdafx.h"
#include "cab_view_rec.h"
#include "window_gui.h"
#include "window_func.h"
#include "viewport_func.h"
#include "vehicle_base.h"
#include "news_func.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "gfx_func.h"
#include "openttd.h"
#include "fileio_func.h"
#include "table/strings.h"
#include "map_func.h"
#include "tile_map.h"
#include "vehicle_gui.h"
#include "widgets/cab_view_widget.h"

#include <fstream>
#include <sstream>
#include <cmath>


namespace {

struct RecCommand {
	int frame;
	std::string cmd;
	std::vector<std::string> args;
};

struct RecState {
	bool init = false;
	bool active = false;
	std::chrono::steady_clock::time_point now;
	std::vector<RecCommand> commands;
	size_t next = 0;
	int frame = 0;
	FILE *pipe = nullptr;
	int release_frame = -1;

	/* Mouse move in progress. */
	int move_start = -1;
	int move_frames = 0;
	Point move_from{};
	std::vector<std::string> move_target;
	Point pos{0, 0};
};

RecState _rec;

void RecInit()
{
	if (_rec.init) return;
	_rec.init = true;
	_rec.now = std::chrono::steady_clock::now();
	const char *script = getenv("REC_SCRIPT");
	if (script == nullptr) return;
	_rec.active = true;
	std::ifstream in(script);
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') continue;
		std::istringstream ls(line);
		RecCommand c;
		if (!(ls >> c.frame >> c.cmd)) continue;
		if (c.cmd == "news") {
			std::string rest;
			std::getline(ls, rest);
			size_t p;
			while ((p = rest.find("\\n")) != std::string::npos) rest.replace(p, 2, "\n");
			c.args.push_back(rest.substr(rest.find_first_not_of(' ')));
		} else {
			std::string a;
			while (ls >> a) c.args.push_back(a);
		}
		_rec.commands.push_back(std::move(c));
	}
	std::stable_sort(_rec.commands.begin(), _rec.commands.end(), [](const RecCommand &a, const RecCommand &b) { return a.frame < b.frame; });
}

WindowClass GetClass(const std::string &name)
{
	if (name == "main") return WC_MAIN_WINDOW;
	if (name == "veh") return WC_VEHICLE_VIEW;
	if (name == "cab") return WC_VEHICLE_CAB_VIEW;
	if (name == "news") return WC_NEWS_WINDOW;
	if (name == "aidebug") return WC_SCRIPT_DEBUG;
	if (name == "toolbar") return WC_MAIN_TOOLBAR;
	if (name == "status") return WC_STATUS_BAR;
	return WC_NONE;
}

Window *GetWindow(const std::string &name)
{
	return FindWindowByClass(GetClass(name));
}

Point VehicleScreenPos(const Window *w, const Vehicle *v)
{
	const Viewport &vp = *w->viewport;
	int x = (v->coord.left + v->coord.right) / 2;
	int y = (v->coord.top + v->coord.bottom) / 2;
	return {UnScaleByZoom(x - vp.virtual_left, vp.zoom) + vp.left, UnScaleByZoom(y - vp.virtual_top, vp.zoom) + vp.top};
}

/** Where the target of a move is now; targets on moving things move along. */
Point GetTarget(const std::vector<std::string> &t, Point fallback)
{
	if (t.empty()) return fallback;
	if (t[0] == "xy" && t.size() >= 3) return {std::stoi(t[1]), std::stoi(t[2])};
	if ((t[0] == "veh" && t.size() >= 2) || (t[0] == "vehin" && t.size() >= 3)) {
		Window *w = t[0] == "veh" ? GetWindow("main") : GetWindow(t[1]);
		const Vehicle *v = Vehicle::GetIfValid(VehicleID(std::stoi(t.back())));
		if (w == nullptr || w->viewport == nullptr || v == nullptr) return fallback;
		return VehicleScreenPos(w, v);
	}
	if (t[0] == "wid" && t.size() >= 3) {
		Window *w = GetWindow(t[1]);
		if (w == nullptr) { fprintf(stderr, "[rec] no window %s\n", t[1].c_str()); return fallback; }
		const NWidgetBase *nw = w->GetWidget<NWidgetBase>(std::stoi(t[2]));
		if (nw == nullptr) { fprintf(stderr, "[rec] no widget %s\n", t[2].c_str()); return fallback; }
		return {w->left + nw->pos_x + static_cast<int>(nw->current_x) / 2, w->top + nw->pos_y + static_cast<int>(nw->current_y) / 2};
	}
	return fallback;
}

void SetMouse(Point p)
{
	_rec.pos = p;
	_cursor.in_window = true;
	_cursor.UpdateCursorPosition(p.x, p.y);
}

void Execute(const RecCommand &c)
{
	if (c.cmd == "move") {
		_rec.move_start = _rec.frame;
		_rec.move_frames = std::max(1, std::stoi(c.args[0]));
		_rec.move_from = _rec.pos;
		_rec.move_target.assign(c.args.begin() + 1, c.args.end());
	} else if (c.cmd == "click") {
		_left_button_down = true;
		_left_button_clicked = false;
		_rec.release_frame = _rec.frame + 4;
	} else if (c.cmd == "follow") {
		Window *w = GetWindow("main");
		const Vehicle *v = Vehicle::GetIfValid(VehicleID(std::stoi(c.args[0])));
		if (w != nullptr && v != nullptr) {
			ScrollMainWindowTo(v->x_pos, v->y_pos, v->z_pos, true);
			w->viewport->follow_vehicle = v->index;
		}
	} else if (c.cmd == "unfollow") {
		Window *w = GetWindow("main");
		if (w != nullptr) w->viewport->follow_vehicle = VehicleID::Invalid();
	} else if (c.cmd == "win") {
		Window *w = GetWindow(c.args[0]);
		if (w == nullptr) return;
		w->SetDirty();
		int dx = std::stoi(c.args[1]) - w->left;
		int dy = std::stoi(c.args[2]) - w->top;
		w->left += dx;
		w->top += dy;
		if (w->viewport != nullptr) {
			w->viewport->left += dx;
			w->viewport->top += dy;
		}
		ResizeWindow(w, std::stoi(c.args[3]) - w->width, std::stoi(c.args[4]) - w->height);
		w->SetDirty();
	} else if (c.cmd == "news") {
		AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, c.args[0]), NewsType::General, NewsStyle::Normal, {NewsFlag::InColour});
	} else if (c.cmd == "close") {
		CloseWindowByClass(GetClass(c.args[0]));
	} else if (c.cmd == "list") {
		for (const Vehicle *v : Vehicle::Iterate()) {
			if (!v->IsPrimaryVehicle()) continue;
			if (c.args.size() > 0 && v->index.base() != std::stoi(c.args[0])) continue;
			fprintf(stderr, "[rec] f%d vehicle %d type %d unit %d tile %d,%d tt %d speed %d\n", _rec.frame, v->index.base(), (int)v->type, v->unitnumber, TileX(v->tile), TileY(v->tile), (int)GetTileType(v->tile), v->cur_speed);
		}
	} else if (c.cmd == "windows") {
		for (const Window *w : Window::Iterate()) fprintf(stderr, "[rec] window class %d num %d at %d,%d size %d,%d\n", (int)w->window_class, (int)w->window_number, w->left, w->top, w->width, w->height);
	} else if (c.cmd == "cab") {
		const Vehicle *v = Vehicle::GetIfValid(VehicleID(std::stoi(c.args[0])));
		if (v != nullptr) ShowCabViewWindow(v);
	} else if (c.cmd == "3d") {
		Window *w = GetWindow("cab");
		if (w != nullptr) w->OnClick({}, WID_CV_3D, 1);
	} else if (c.cmd == "zoomin") {
		Window *w = GetWindow("main");
		if (w != nullptr) DoZoomInOutWindow(ZOOM_IN, w);
	} else if (c.cmd == "quit") {
		_exit_game = true;
	}
}

} // namespace

bool RecActive()
{
	RecInit();
	return _rec.active;
}

std::chrono::steady_clock::time_point RecNow()
{
	if (!RecActive()) return std::chrono::steady_clock::now();
	return _rec.now;
}

void RecBeginFrame()
{
	if (!RecActive()) return;

	while (_rec.next < _rec.commands.size() && _rec.commands[_rec.next].frame <= _rec.frame) {
		Execute(_rec.commands[_rec.next++]);
	}
	if (_rec.frame == _rec.release_frame) {
		_left_button_down = false;
		_left_button_clicked = false;
	}
	if (_rec.move_start >= 0) {
		float t = std::min(1.0f, static_cast<float>(_rec.frame - _rec.move_start) / _rec.move_frames);
		float e = 0.5f - 0.5f * std::cos(t * static_cast<float>(M_PI));
		Point to = GetTarget(_rec.move_target, _rec.pos);
		Point p{_rec.move_from.x + static_cast<int>(std::lround((to.x - _rec.move_from.x) * e)), _rec.move_from.y + static_cast<int>(std::lround((to.y - _rec.move_from.y) * e))};
		if (_rec.frame - _rec.move_start == _rec.move_frames) fprintf(stderr, "[rec] move end at %d,%d target %d,%d\n", p.x, p.y, to.x, to.y);
		if (t >= 1.0f) {
			/* Stay on a moving target until the next move. */
			p = to;
		}
		SetMouse(p);
	} else {
		SetMouse(_rec.pos);
	}
}

void RecEndFrame(std::chrono::steady_clock::duration interval)
{
	if (!RecActive()) return;

	if (_rec.pipe == nullptr && getenv("REC_PIPE") != nullptr) {
		_rec.pipe = popen(getenv("REC_PIPE"), "w");
	}
	if (_rec.pipe != nullptr && _screen.dst_ptr != nullptr) {
		const uint32_t *p = static_cast<const uint32_t *>(_screen.dst_ptr);
		for (int y = 0; y < _screen.height; y++) {
			fwrite(p + static_cast<size_t>(y) * _screen.pitch, 4, _screen.width, _rec.pipe);
		}
	}
	if (_exit_game && _rec.pipe != nullptr) {
		pclose(_rec.pipe);
		_rec.pipe = nullptr;
	}
	if (_rec.frame % 60 == 0) fprintf(stderr, "[rec] frame %d\n", _rec.frame);
	_rec.frame++;
	_rec.now += interval;
}
