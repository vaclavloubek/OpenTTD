class CabRoadAI extends AIController {
}

function CabRoadAI::Must(ok, what)
{
	if (!ok) AILog.Error("FAILED " + what + ": " + AIError.GetLastErrorString());
	return ok;
}

function CabRoadAI::Tile(x, y)
{
	return AIMap.GetTileIndex(x, y);
}

/** Is this a plain road tile with road going straight through along dx/dy and nowhere else? */
function CabRoadAI::Straight(x, y, dx, dy)
{
	local t = this.Tile(x, y);
	if (!AIRoad.IsRoadTile(t) || AIRoad.IsRoadStationTile(t) || AIRoad.IsRoadDepotTile(t) || AITile.GetSlope(t) != AITile.SLOPE_FLAT) return false;
	if (AIBridge.IsBridgeTile(t) || AITunnel.IsTunnelTile(t) || AIRail.IsLevelCrossingTile(t)) return false;
	if (!AIRoad.AreRoadTilesConnected(t, this.Tile(x + dx, y + dy)) || !AIRoad.AreRoadTilesConnected(t, this.Tile(x - dx, y - dy))) return false;
	if (AIRoad.AreRoadTilesConnected(t, this.Tile(x + dy, y + dx)) || AIRoad.AreRoadTilesConnected(t, this.Tile(x - dy, y - dx))) return false;
	return true;
}

/** Straight road tiles of the town around its centre, as [x, y, dx, dy]. */
function CabRoadAI::StraightTiles(tx, ty, r)
{
	local list = [];
	for (local y = ty - r; y <= ty + r; y++) {
		for (local x = tx - r; x <= tx + r; x++) {
			if (!AIMap.IsValidTile(this.Tile(x, y))) continue;
			if (this.Straight(x, y, 1, 0)) list.append([x, y, 1, 0]);
			else if (this.Straight(x, y, 0, 1)) list.append([x, y, 0, 1]);
		}
	}
	return list;
}

function CabRoadAI::BuildStop(s)
{
	local t = this.Tile(s[0], s[1]);
	return this.Must(AIRoad.BuildDriveThroughRoadStation(t, this.Tile(s[0] + s[2], s[1] + s[3]), AIRoad.ROADVEHTYPE_BUS, AIStation.STATION_NEW), "stop at " + s[0] + "," + s[1]);
}

/** A depot on a free tile next to a road tile near (x, y). */
function CabRoadAI::BuildDepot(x, y)
{
	for (local d = 1; d < 8; d++) {
		for (local yy = y - d; yy <= y + d; yy++) {
			for (local xx = x - d; xx <= x + d; xx++) {
				local front = this.Tile(xx, yy);
				if (!AIRoad.IsRoadTile(front) || AIRoad.IsRoadStationTile(front)) continue;
				foreach (o in [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
					local t = this.Tile(xx + o[0], yy + o[1]);
					if (!AITile.IsBuildable(t) || AITile.GetSlope(t) != AITile.SLOPE_FLAT) continue;
					if (AIRoad.BuildRoadDepot(t, front) && AIRoad.BuildRoad(front, t)) return t;
				}
			}
		}
	}
	return null;
}

function CabRoadAI::Start()
{
	AICompany.SetLoanAmount(AICompany.GetMaxLoanAmount());
	AIRoad.SetCurrentRoadType(AIRoadTypeList(AIRoad.ROADTRAMTYPES_ROAD).Begin());

	local towns = AITownList();
	towns.Valuate(AITown.GetPopulation);
	towns.Sort(AIList.SORT_BY_VALUE, false);
	local c = AITown.GetLocation(towns.Begin());
	local tx = AIMap.GetTileX(c), ty = AIMap.GetTileY(c);

	/* Stops: one near the centre, one on the far side of the town. */
	local tiles = this.StraightTiles(tx, ty, 14);
	if (tiles.len() < 2) { AILog.Error("NO TOWN ROADS"); return; }
	local a = null, best = 1000;
	foreach (s in tiles) {
		local d = abs(s[0] - tx) + abs(s[1] - ty);
		if (d >= 2 && d < best) { best = d; a = s; }
	}
	local b = null; best = 0;
	foreach (s in tiles) {
		local d = abs(s[0] - a[0]) + abs(s[1] - a[1]);
		if (d > best) { best = d; b = s; }
	}
	this.BuildStop(a);
	this.BuildStop(b);
	AILog.Info("STOP A " + a[0] + "," + a[1] + " B " + b[0] + "," + b[1]);

	/* A road out of the town from beyond stop B, away from the centre, with a stop at its end. */
	local dx = b[2], dy = b[3];
	if ((b[0] - tx) * dx + (b[1] - ty) * dy < 0) { dx = -dx; dy = -dy; }
	local x = b[0], y = b[1];
	while (AIRoad.IsRoadTile(this.Tile(x + dx, y + dy))) { x += dx; y += dy; }
	/* Leave the town before the hill. */
	for (local i = 0; i < 5 && AIRoad.BuildRoad(this.Tile(x, y), this.Tile(x + dx, y + dy)); i++) { x += dx; y += dy; }
	local cstop = null;
	/* Tile i of the road out, counted from the last road tile. */
	local sx = x, sy = y;
	local P = function(i) : (sx, sy, dx, dy) { return [sx + dx * i, sy + dy * i]; };
	/* A hill across the road, two levels high, with a tunnel from tile 8 to tile 14 of the road out. */
	local e = 8, f = 14;
	foreach (step in [[0, -2, 3], [1, -1, 2]]) {
		local lo = (dx + dy > 0) ? e + 1 + step[0] : -f + 1 + step[0];
		local hi = (dx + dy > 0) ? f - step[0] : -e - step[0];
		for (local a = lo; a <= hi; a++) {
			for (local j = step[1]; j <= step[2]; j++) {
				local cx = (dx != 0) ? sx + a : sx + j;
				local cy = (dy != 0) ? sy + a : sy + j;
				AITile.RaiseTile(this.Tile(cx, cy), AITile.SLOPE_N);
			}
		}
	}
	local p = P(e), p0 = P(e - 1);
	this.Must(AIRoad.BuildRoad(this.Tile(sx, sy), this.Tile(p0[0], p0[1])), "road to tunnel");
	if (this.Must(AITunnel.BuildTunnel(AIVehicle.VT_ROAD, this.Tile(p[0], p[1])), "road tunnel")) {
		local end = AITunnel.GetOtherTunnelEnd(this.Tile(p[0], p[1]));
		AILog.Info("TUNNEL " + p[0] + "," + p[1] + " to " + AIMap.GetTileX(end) + "," + AIMap.GetTileY(end));
		f = AIMap.DistanceManhattan(end, this.Tile(sx, sy));
	}
	/* On beyond the tunnel, over a railway on a level crossing, to a stop at the end. */
	local q = P(f + 1), r = P(f + 12);
	if (this.Must(AIRoad.BuildRoad(this.Tile(q[0], q[1]), this.Tile(r[0], r[1])), "road after tunnel")) {
		local qe = P(f);
		AIRoad.BuildRoad(this.Tile(qe[0], qe[1]), this.Tile(q[0], q[1]));
		AIRoad.BuildRoad(this.Tile(p0[0], p0[1]), this.Tile(p[0], p[1]));
		local rts = AIRailTypeList();
		AIRail.SetCurrentRailType(rts.Begin());
		local c = P(f + 5);
		local track = (dx != 0) ? AIRail.RAILTRACK_NW_SE : AIRail.RAILTRACK_NE_SW;
		for (local j = -4; j <= 4; j++) {
			local t = this.Tile(c[0] + dy * j, c[1] + dx * j);
			AIRail.BuildRailTrack(t, track);
		}
		AIRoad.BuildRoad(this.Tile(r[0], r[1]), this.Tile(r[0] + dx, r[1] + dy));
		local st = P(f + 11);
		if (this.BuildStop([st[0], st[1], dx, dy])) cstop = this.Tile(st[0], st[1]);
		AILog.Info("ROAD OUT to " + r[0] + "," + r[1] + " crossing at " + c[0] + "," + c[1]);
	}

	/* Tram track along some town streets: on straight pieces near the centre, away from the stops. */
	local trams = AIRoadTypeList(AIRoad.ROADTRAMTYPES_TRAM);
	if (!trams.IsEmpty()) {
		AIRoad.SetCurrentRoadType(trams.Begin());
		local n = 0;
		foreach (s in tiles) {
			if (abs(s[0] - tx) + abs(s[1] - ty) > 6) continue;
			if ((s[0] == a[0] && s[1] == a[1]) || (s[0] == b[0] && s[1] == b[1])) continue;
			if (AIRoad.BuildRoad(this.Tile(s[0] - s[2], s[1] - s[3]), this.Tile(s[0] + s[2], s[1] + s[3]))) n++;
		}
		AILog.Info("TRAM pieces " + n);
		AIRoad.SetCurrentRoadType(AIRoadTypeList(AIRoad.ROADTRAMTYPES_ROAD).Begin());
	}

	local depot = this.BuildDepot(a[0], a[1]);
	if (depot == null) { AILog.Error("NO DEPOT"); return; }
	local pass = null;
	foreach (cg, _ in AICargoList()) if (AICargo.HasCargoClass(cg, AICargo.CC_PASSENGERS)) { pass = cg; break; }
	local engines = AIEngineList(AIVehicle.VT_ROAD);
	engines.Valuate(AIEngine.IsBuildable); engines.KeepValue(1);
	engines.Valuate(AIEngine.GetCargoType); engines.KeepValue(pass);
	engines.Valuate(AIEngine.GetMaxSpeed); engines.Sort(AIList.SORT_BY_VALUE, false);
	if (engines.IsEmpty()) { AILog.Error("NO BUS ENGINE"); return; }

	local ta = this.Tile(a[0], a[1]), tb = this.Tile(b[0], b[1]);
	local orders = [[ta, tb], [tb, ta]];
	if (cstop != null) orders = [[ta, tb, cstop], [cstop, tb, ta]];
	foreach (o in orders) {
		local bus = AIVehicle.BuildVehicle(depot, engines.Begin());
		if (!this.Must(AIVehicle.IsValidVehicle(bus), "build bus")) continue;
		foreach (t in o) this.Must(AIOrder.AppendOrder(bus, t, AIOrder.OF_NON_STOP_INTERMEDIATE), "order");
		AIVehicle.StartStopVehicle(bus);
		AILog.Info("BUS " + bus);
		this.Sleep(30);
	}
	while (true) this.Sleep(1000);
}
