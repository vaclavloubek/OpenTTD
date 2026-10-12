/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cab_view_widget.h Types related to the cab view widgets. */

#ifndef WIDGETS_CAB_VIEW_WIDGET_H
#define WIDGETS_CAB_VIEW_WIDGET_H

/** Widgets of the #CabViewWindow class. */
enum CabViewWidgets : WidgetID {
	WID_CV_CAPTION,    ///< Caption of window.
	WID_CV_VIEWPORT,   ///< The viewport riding along with the vehicle.
	WID_CV_VIEW_SELECT, ///< Selection between the viewport and the 3D view.
	WID_CV_3D_VIEW,    ///< The 3D view from the cab.
	WID_CV_DASHBOARD,  ///< Dashboard with speed, destination, state and load.
	WID_CV_ZOOM_IN,    ///< Zoom in.
	WID_CV_ZOOM_OUT,   ///< Zoom out.
	WID_CV_LOOK_AHEAD, ///< Toggle looking ahead in the direction of travel.
	WID_CV_3D,         ///< Toggle the 3D view.
	WID_CV_START_STOP, ///< Start or stop the vehicle.
};

#endif /* WIDGETS_CAB_VIEW_WIDGET_H */
