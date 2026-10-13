/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cab_view_rec.h Recording videos frame by frame (development tool, only active with REC_SCRIPT set). */

#ifndef CAB_VIEW_REC_H
#define CAB_VIEW_REC_H

#include <chrono>

bool RecActive();
std::chrono::steady_clock::time_point RecNow();
void RecBeginFrame();
void RecEndFrame(std::chrono::steady_clock::duration interval);

#endif /* CAB_VIEW_REC_H */
