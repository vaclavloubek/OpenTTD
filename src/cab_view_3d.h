/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cab_view_3d.h Software rendered 3D view from the cab of a train, a ship or an aircraft. */

#ifndef CAB_VIEW_3D_H
#define CAB_VIEW_3D_H

#include "core/geometry_type.hpp"
#include "tile_type.h"
#include "vehicle_type.h"

/** Smallest size of the picture of the 3D view compared to the view, when drawing at full size takes too long. */
constexpr float CAB_VIEW_3D_MIN_SCALE = 0.5f;
/** How far below the level of its tile the 3D view shows the surface of water, so canals, rivers and the sea have banks. */
constexpr int CAB_VIEW_3D_WATER_DROP = 1;
/** Height of the eye of the 3D view from a ship above the water. */
constexpr float CAB_VIEW_3D_SHIP_EYE = 4.0f;

/** Where the camera of the 3D cab view is and where it looks. */
struct CabViewCamera {
	float x = 0; ///< X position of the eye, in world coordinates.
	float y = 0; ///< Y position of the eye, in world coordinates.
	float z = 0; ///< Height of the eye, in world coordinates.
	float yaw = 0; ///< Direction of view in radians; 0 looks towards +x, pi/2 towards +y.
	float pitch = 0; ///< Tilt of the view in radians, upwards positive, on top of the slight look down of the 3D view.
	float altitude = 0; ///< How high above the ground an aircraft flies; the haze then starts further away, as the ground is seen from above.
	VehicleID vehicle = VehicleID::Invalid(); ///< The vehicle the camera rides on, which is not drawn.
	bool dark = false; ///< Nothing to see, e.g. in a depot.
	TileIndex tunnel = INVALID_TILE; ///< An entrance of the tunnel the train is in, whose inside the view shows.

	bool operator==(const CabViewCamera &) const = default;
};

void DrawCabView3D(const Rect &r, const CabViewCamera &camera, uint32_t frame_id, float budget_ms);
float GetCabView3DFrameTime(float scale);
float GetCabView3DShipWaterZ(const Vehicle *v);
void ResetCabView3D();

#endif /* CAB_VIEW_3D_H */
