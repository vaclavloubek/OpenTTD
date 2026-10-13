class CabTestAI extends AIController {
	L1 = 20; // tiles along X, including station A and the corner
	L2 = 20; // tiles along Y after the corner, including station B
}

function CabTestAI::Flat(t)
{
	return AIMap.IsValidTile(t) && AITile.IsBuildable(t) && AITile.GetSlope(t) == AITile.SLOPE_FLAT;
}

function CabTestAI::RouteFree(x0, y0)
{
	if (x0 < 2 || y0 < 2) return false;
	for (local x = x0 - 1; x < x0 + this.L1; x++) {
		if (!this.Flat(AIMap.GetTileIndex(x, y0))) return false;
	}
	local x1 = x0 + this.L1 - 1;
	for (local y = y0 + 1; y <= y0 + this.L2; y++) {
		if (!this.Flat(AIMap.GetTileIndex(x1, y))) return false;
	}
	return true;
}

function CabTestAI::FindRoute()
{
	local towns = AITownList();
	towns.Valuate(AITown.GetPopulation);
	towns.Sort(AIList.SORT_BY_VALUE, false);
	foreach (town, _ in towns) {
		local c = AITown.GetLocation(town);
		local tx = AIMap.GetTileX(c);
		local ty = AIMap.GetTileY(c);
		for (local d = 3; d <= 8; d++) {
			foreach (dy in [-d, d]) {
				for (local dx = -6; dx <= 2; dx++) {
					if (this.RouteFree(tx + dx, ty + dy)) return [tx + dx, ty + dy];
				}
			}
		}
	}
	for (local y = 10; y < AIMap.GetMapSizeY() - 30; y += 3) {
		for (local x = 10; x < AIMap.GetMapSizeX() - 30; x += 3) {
			if (this.RouteFree(x, y)) return [x, y];
		}
	}
	return null;
}

function CabTestAI::Must(ok, what)
{
	if (!ok) AILog.Error("FAILED " + what + ": " + AIError.GetLastErrorString());
	return ok;
}

/** Is the w x h rectangle at (x0, y0), with a margin of one tile, flat and buildable? */
function CabTestAI::AreaFree(x0, y0, w, h)
{
	if (x0 < 2 || y0 < 2 || x0 + w + 2 >= AIMap.GetMapSizeX() || y0 + h + 2 >= AIMap.GetMapSizeY()) return false;
	for (local y = y0 - 1; y <= y0 + h; y++) {
		for (local x = x0 - 1; x <= x0 + w; x++) {
			if (!this.Flat(AIMap.GetTileIndex(x, y))) return false;
		}
	}
	return true;
}

/** Find a free w x h area, searching outwards from (cx, cy), skipping areas closer than min_dist. */
function CabTestAI::FindArea(cx, cy, w, h, min_dist)
{
	for (local d = min_dist; d < 120; d += 2) {
		for (local i = -d; i <= d; i += 2) {
			foreach (p in [[cx + i, cy - d], [cx + i, cy + d], [cx - d, cy + i], [cx + d, cy + i]]) {
				if (this.AreaFree(p[0], p[1], w, h)) return p;
			}
		}
	}
	return null;
}

/** Cheapest buildable engine of a vehicle type, optionally for a cargo; prefer the fastest. */
function CabTestAI::PickEngine(vt, cargo)
{
	local engines = AIEngineList(vt);
	engines.Valuate(AIEngine.IsWagon);       engines.KeepValue(0);
	engines.Valuate(AIEngine.IsBuildable);   engines.KeepValue(1);
	if (cargo != null) { engines.Valuate(AIEngine.CanRefitCargo, cargo); engines.KeepValue(1); }
	engines.Valuate(AIEngine.GetMaxSpeed);   engines.Sort(AIList.SORT_BY_VALUE, false);
	return engines.IsEmpty() ? null : engines.Begin();
}

/** Bus on an L-shaped road with a drive-through stop at both ends. */
function CabTestAI::BuildBus(cx, cy, pass)
{
	local rts = AIRoadTypeList(AIRoad.ROADTRAMTYPES_ROAD);
	AIRoad.SetCurrentRoadType(rts.Begin());
	local a = this.FindArea(cx, cy, 14, 12, 6);
	if (a == null) { AILog.Error("NO BUS AREA"); return null; }
	local x0 = a[0], y0 = a[1], x1 = x0 + 12, y1 = y0 + 10;
	local tA = AIMap.GetTileIndex(x0 + 1, y0);
	local tB = AIMap.GetTileIndex(x1, y1 - 1);
	this.Must(AIRoad.BuildRoad(AIMap.GetTileIndex(x0, y0), AIMap.GetTileIndex(x1, y0)), "bus road x");
	this.Must(AIRoad.BuildRoad(AIMap.GetTileIndex(x1, y0), AIMap.GetTileIndex(x1, y1)), "bus road y");
	this.Must(AIRoad.BuildDriveThroughRoadStation(tA, AIMap.GetTileIndex(x0 + 2, y0), AIRoad.ROADVEHTYPE_BUS, AIStation.STATION_NEW), "bus stop A");
	this.Must(AIRoad.BuildDriveThroughRoadStation(tB, AIMap.GetTileIndex(x1, y1), AIRoad.ROADVEHTYPE_BUS, AIStation.STATION_NEW), "bus stop B");
	local tDepot = AIMap.GetTileIndex(x0 + 5, y0 + 1);
	local tFront = AIMap.GetTileIndex(x0 + 5, y0);
	this.Must(AIRoad.BuildRoadDepot(tDepot, tFront), "bus depot");
	this.Must(AIRoad.BuildRoad(tFront, tDepot), "bus depot road");

	local engine = this.PickEngine(AIVehicle.VT_ROAD, pass);
	if (engine == null) { AILog.Error("NO BUS ENGINE"); return null; }
	local bus = AIVehicle.BuildVehicle(tDepot, engine);
	if (!this.Must(AIVehicle.IsValidVehicle(bus), "build bus")) return null;
	this.Must(AIOrder.AppendOrder(bus, tA, AIOrder.OF_NONE), "bus order A");
	this.Must(AIOrder.AppendOrder(bus, tB, AIOrder.OF_NONE), "bus order B");
	this.Must(AIVehicle.StartStopVehicle(bus), "start bus");
	AILog.Info("BUS " + bus + " A=" + tA + " B=" + tB + " " + AIEngine.GetName(engine));
	return bus;
}

/**
 * Two ships sailing in opposite directions between two buoys on a canal, two tiles wide, with a depot at each end.
 * Halfway the canal goes up a level through two locks; a road bridge crosses the lower part, a rail bridge the upper part.
 */
function CabTestAI::BuildShip(cx, cy, pass)
{
	local W = 28;
	local a = this.FindArea(cx, cy, W, 4, 14);
	if (a == null) { AILog.Error("NO SHIP AREA"); return null; }
	local x0 = a[0], y0 = a[1], xm = x0 + 14;
	this.RaiseCorners(xm + 1, y0, x0 + W, y0 + 4);
	foreach (y in [y0 + 1, y0 + 2]) this.Must(AIMarine.BuildLock(AIMap.GetTileIndex(xm, y)), "lock " + xm + "," + y);
	for (local y = y0 + 1; y <= y0 + 2; y++) {
		for (local x = x0; x < x0 + W; x++) {
			if (x >= xm - 1 && x <= xm + 1) continue;
			this.Must(AIMarine.BuildCanal(AIMap.GetTileIndex(x, y)), "canal " + x + "," + y);
		}
	}
	/* A depot at each end, so the ships meet on the way. */
	local depots = [];
	foreach (dx in [1, W - 5]) {
		for (local x = x0 + dx; x <= x0 + dx + 3; x++) this.Must(AIMarine.BuildCanal(AIMap.GetTileIndex(x, y0 + 3)), "canal " + x + "," + (y0 + 3));
		local tDepot = AIMap.GetTileIndex(x0 + dx + 1, y0 + 3);
		this.Must(AIMarine.BuildWaterDepot(tDepot, AIMap.GetTileIndex(x0 + dx + 2, y0 + 3)), "ship depot");
		depots.append(tDepot);
	}
	/* A dock on the slope beside the locks; its water, two tiles of it, by the lower canal. */
	foreach (x in [xm - 2, xm - 1]) this.Must(AIMarine.BuildCanal(AIMap.GetTileIndex(x, y0 + 3)), "dock canal");
	if (this.Must(AIMarine.BuildDock(AIMap.GetTileIndex(xm, y0 + 3), AIStation.STATION_NEW), "dock")) AILog.Info("DOCK " + AIMap.GetTileIndex(xm, y0 + 3));

	local types = AIBridgeList_Length(4);
	if (!types.IsEmpty() && this.Must(AIBridge.BuildBridge(AIVehicle.VT_ROAD, types.Begin(), AIMap.GetTileIndex(x0 + 7, y0), AIMap.GetTileIndex(x0 + 7, y0 + 3)), "road bridge over canal")) {
		AIRoad.BuildRoad(AIMap.GetTileIndex(x0 + 7, y0 - 2), AIMap.GetTileIndex(x0 + 7, y0));
		AIRoad.BuildRoad(AIMap.GetTileIndex(x0 + 7, y0 + 3), AIMap.GetTileIndex(x0 + 7, y0 + 5));
	}
	this.BuildRailBridge(AIMap.GetTileIndex(x0 + 21, y0), AIMap.GetTileIndex(x0 + 21, y0 + 3), 1);
	/* Trees along the banks. */
	for (local x = x0; x < x0 + W; x += 2) {
		foreach (y in [y0 - 1, y0 + 4]) AITile.PlantTree(AIMap.GetTileIndex(x, y));
	}

	local tA = AIMap.GetTileIndex(x0 + 1, y0 + 1);
	local tB = AIMap.GetTileIndex(x0 + W - 2, y0 + 2);
	this.Must(AIMarine.BuildBuoy(tA), "buoy A");
	this.Must(AIMarine.BuildBuoy(tB), "buoy B");

	/* A ferry rather than the fastest, the low hovercraft. */
	local engines = AIEngineList(AIVehicle.VT_WATER);
	engines.Valuate(AIEngine.IsBuildable); engines.KeepValue(1);
	engines.Valuate(AIEngine.CanRefitCargo, pass); engines.KeepValue(1);
	engines.Valuate(AIEngine.GetMaxSpeed); engines.Sort(AIList.SORT_BY_VALUE, false);
	if (engines.Count() > 1) engines.RemoveItem(engines.Begin());
	local engine = engines.IsEmpty() ? this.PickEngine(AIVehicle.VT_WATER, null) : engines.Begin();
	if (engine == null) { AILog.Error("NO SHIP ENGINE"); return null; }
	local ships = [];
	foreach (i, orders in [[tA, tB], [tB, tA]]) {
		local ship = AIVehicle.BuildVehicle(depots[i], engine);
		if (!this.Must(AIVehicle.IsValidVehicle(ship), "build ship")) continue;
		foreach (t in orders) this.Must(AIOrder.AppendOrder(ship, t, AIOrder.OF_NONE), "ship order");
		this.Must(AIVehicle.StartStopVehicle(ship), "start ship");
		AILog.Info((i == 0 ? "SHIP " : "SHIP2 ") + ship + " A=" + tA + " B=" + tB + " lock " + xm + "," + (y0 + 1) + " " + AIEngine.GetName(engine));
		ships.append(ship);
	}
	return ships.len() > 0 ? ships[0] : null;
}

/** Ship sailing between two docks on the coast, when the map has sea near (cx, cy). */
function CabTestAI::BuildSeaShip(cx, cy, pass)
{
	local docks = [];
	for (local d = 4; d < 80 && docks.len() < 2; d += 2) {
		for (local i = -d; i <= d && docks.len() < 2; i += 2) {
			foreach (p in [[cx + i, cy - d], [cx + i, cy + d], [cx - d, cy + i], [cx + d, cy + i]]) {
				local t = AIMap.GetTileIndex(p[0], p[1]);
				if (!AIMap.IsValidTile(t) || !AITile.IsCoastTile(t)) continue;
				if (docks.len() == 1 && AIMap.DistanceManhattan(t, docks[0]) < 25) continue;
				if (AIMarine.BuildDock(t, AIStation.STATION_NEW)) { docks.append(t); AILog.Info("SEADOCK " + t); break; }
			}
		}
	}
	if (docks.len() < 2) { AILog.Warning("NO SEA DOCKS"); return null; }
	/* A depot on open sea near the first dock. */
	local depot = null;
	for (local d = 2; d < 12 && depot == null; d++) {
		local x = AIMap.GetTileX(docks[0]), y = AIMap.GetTileY(docks[0]);
		foreach (p in [[x + d, y], [x - d, y], [x, y + d], [x, y - d]]) {
			local t = AIMap.GetTileIndex(p[0], p[1]);
			if (AIMarine.BuildWaterDepot(t, AIMap.GetTileIndex(p[0] + 1, p[1]))) { depot = t; break; }
		}
	}
	if (depot == null) { AILog.Warning("NO SEA DEPOT"); return null; }
	local engines = AIEngineList(AIVehicle.VT_WATER);
	engines.Valuate(AIEngine.IsBuildable); engines.KeepValue(1);
	engines.Valuate(AIEngine.CanRefitCargo, pass); engines.KeepValue(1);
	engines.Valuate(AIEngine.GetMaxSpeed); engines.Sort(AIList.SORT_BY_VALUE, false);
	if (engines.IsEmpty()) return null;
	local ship = AIVehicle.BuildVehicle(depot, engines.Begin());
	if (!this.Must(AIVehicle.IsValidVehicle(ship), "build sea ship")) return null;
	foreach (t in docks) this.Must(AIOrder.AppendOrder(ship, t, AIOrder.OF_NONE), "sea ship order");
	this.Must(AIVehicle.StartStopVehicle(ship), "start sea ship");
	AILog.Info("SEASHIP " + ship + " " + AIEngine.GetName(engines.Begin()));
	return ship;
}

/** Plane flying between two small (or, later in the game, commuter) airports. */
function CabTestAI::BuildPlane(cx, cy, pass)
{
	/* The small airport is only available until 1959, the commuter airport from 1983. */
	local type = null;
	foreach (t in [AIAirport.AT_SMALL, AIAirport.AT_COMMUTER, AIAirport.AT_LARGE]) {
		if (AIAirport.IsValidAirportType(t)) { type = t; break; }
	}
	if (type == null) { AILog.Error("NO AIRPORT TYPE"); return null; }
	local w = AIAirport.GetAirportWidth(type);
	local h = AIAirport.GetAirportHeight(type);
	local a = this.FindArea(cx, cy, w, h, 20);
	if (a == null) { AILog.Error("NO AIRPORT AREA A"); return null; }
	local tA = AIMap.GetTileIndex(a[0], a[1]);
	this.Must(AIAirport.BuildAirport(tA, type, AIStation.STATION_NEW), "airport A");
	local b = this.FindArea(a[0], a[1], w, h, 40);
	if (b == null) { AILog.Error("NO AIRPORT AREA B"); return null; }
	local tB = AIMap.GetTileIndex(b[0], b[1]);
	this.Must(AIAirport.BuildAirport(tB, type, AIStation.STATION_NEW), "airport B");

	local engines = AIEngineList(AIVehicle.VT_AIR);
	engines.Valuate(AIEngine.IsBuildable);   engines.KeepValue(1);
	engines.Valuate(AIEngine.GetPlaneType);  engines.RemoveValue(AIAirport.PT_BIG_PLANE);
	engines.Valuate(AIEngine.GetMaxSpeed);   engines.Sort(AIList.SORT_BY_VALUE, false);
	if (engines.IsEmpty()) { AILog.Error("NO PLANE ENGINE"); return null; }
	local engine = engines.Begin();
	local plane = AIVehicle.BuildVehicle(AIAirport.GetHangarOfAirport(tA), engine);
	if (!this.Must(AIVehicle.IsValidVehicle(plane), "build plane")) return null;
	this.Must(AIOrder.AppendOrder(plane, tA, AIOrder.OF_NONE), "plane order A");
	this.Must(AIOrder.AppendOrder(plane, tB, AIOrder.OF_NONE), "plane order B");
	this.Must(AIVehicle.StartStopVehicle(plane), "start plane");
	AILog.Info("PLANE " + plane + " A=" + tA + " B=" + tB + " " + AIEngine.GetName(engine));
	return plane;
}

/** Raise the corners from (cx0, cy0) to (cx1, cy1) by one level. */
function CabTestAI::RaiseCorners(cx0, cy0, cx1, cy1)
{
	for (local cy = cy0; cy <= cy1; cy++) {
		for (local cx = cx0; cx <= cx1; cx++) {
			if (!AITile.RaiseTile(AIMap.GetTileIndex(cx, cy), AITile.SLOPE_N)) AILog.Warning("raise " + cx + "," + cy + ": " + AIError.GetLastErrorString());
		}
	}
}

/** Rail bridge from a to b; the n-th of the bridge types that fit, fastest first (wrapping around). */
function CabTestAI::BuildRailBridge(a, b, n)
{
	local len = AIMap.DistanceManhattan(a, b) + 1;
	local types = AIBridgeList_Length(len);
	types.Valuate(AIBridge.GetMaxSpeed);
	types.Sort(AIList.SORT_BY_VALUE, false);
	if (types.IsEmpty()) { AILog.Warning("no bridge of length " + len); return false; }
	local type = types.Begin();
	for (local i = 0; i < n % types.Count(); i++) type = types.Next();
	local ok = this.Must(AIBridge.BuildBridge(AIVehicle.VT_RAIL, type, a, b), "bridge " + a + "-" + b);
	if (ok) AILog.Info("BRIDGE " + a + "-" + b + " " + AIBridge.GetName(type, AIVehicle.VT_RAIL));
	return ok;
}

function CabTestAI::Start()
{
	AICompany.SetName("Cab Test Rail");
	AICompany.SetLoanAmount(AICompany.GetMaxLoanAmount());

	local rts = AIRailTypeList();
	rts.Valuate(AIRail.IsRailTypeAvailable);
	rts.KeepValue(1);
	local rt = rts.Begin();
	AIRail.SetCurrentRailType(rt);

	local start = this.FindRoute();
	if (start == null) { AILog.Error("NO ROUTE FOUND"); while (true) this.Sleep(1000); }
	local x0 = start[0];
	local y0 = start[1];
	local x1 = x0 + this.L1 - 1;
	local y1 = y0 + this.L2;
	AILog.Info("ROUTE x0=" + x0 + " y0=" + y0 + " x1=" + x1 + " y1=" + y1);

	local tA = AIMap.GetTileIndex(x0, y0);
	local tB = AIMap.GetTileIndex(x1, y1 - 3);
	local tDepot = AIMap.GetTileIndex(x0 - 1, y0);

	/* A hill two levels high across the line along Y, for a tunnel from y0 + 4 to y0 + 10. */
	this.RaiseCorners(x1 - 2, y0 + 5, x1 + 3, y0 + 10);
	this.RaiseCorners(x1 - 1, y0 + 6, x1 + 2, y0 + 9);

	/* Roads across the line: under the bridges, and over a level crossing. */
	AIRoad.SetCurrentRoadType(AIRoadTypeList(AIRoad.ROADTRAMTYPES_ROAD).Begin());
	this.Must(AIRoad.BuildRoad(AIMap.GetTileIndex(x0 + 12, y0 - 2), AIMap.GetTileIndex(x0 + 12, y0 + 2)), "road under bridge x");
	this.Must(AIRoad.BuildRoad(AIMap.GetTileIndex(x1 - 2, y0 + 13), AIMap.GetTileIndex(x1 + 2, y0 + 13)), "road under bridge y");

	this.Must(AIRail.BuildRailStation(tA, AIRail.RAILTRACK_NE_SW, 1, 4, AIStation.STATION_NEW), "station A");
	for (local x = x0 + 4; x < x1; x++) {
		if (x >= x0 + 10 && x <= x0 + 13) continue;
		this.Must(AIRail.BuildRailTrack(AIMap.GetTileIndex(x, y0), AIRail.RAILTRACK_NE_SW), "track x " + x);
	}
	this.BuildRailBridge(AIMap.GetTileIndex(x0 + 10, y0), AIMap.GetTileIndex(x0 + 13, y0), 0);
	this.Must(AIRoad.BuildRoad(AIMap.GetTileIndex(x0 + 16, y0 - 1), AIMap.GetTileIndex(x0 + 16, y0 + 1)), "level crossing");
	this.Must(AIRail.BuildRailTrack(AIMap.GetTileIndex(x1, y0), AIRail.RAILTRACK_NE_SE), "corner");
	for (local y = y0 + 1; y < y1 - 3; y++) {
		if (y >= y0 + 4 && y <= y0 + 10) continue;
		if (y >= y0 + 12 && y <= y0 + 15) continue;
		this.Must(AIRail.BuildRailTrack(AIMap.GetTileIndex(x1, y), AIRail.RAILTRACK_NW_SE), "track y " + y);
	}
	if (this.Must(AITunnel.BuildTunnel(AIVehicle.VT_RAIL, AIMap.GetTileIndex(x1, y0 + 4)), "tunnel")) {
		local end = AITunnel.GetOtherTunnelEnd(AIMap.GetTileIndex(x1, y0 + 4));
		AILog.Info("TUNNEL " + AIMap.GetTileIndex(x1, y0 + 4) + " to " + AIMap.GetTileX(end) + "," + AIMap.GetTileY(end));
	}
	this.BuildRailBridge(AIMap.GetTileIndex(x1, y0 + 12), AIMap.GetTileIndex(x1, y0 + 15), 3);

	/* A road bridge over the line along X, where there is room for it. */
	local road_bridge = false;
	foreach (x in [x0 + 6, x0 + 5, x0 + 7, x0 + 9]) {
		foreach (ends in [[y0 - 2, y0 + 2], [y0 - 1, y0 + 1], [y0 - 1, y0 + 2], [y0 - 2, y0 + 1]]) {
			local a = AIMap.GetTileIndex(x, ends[0]);
			local b = AIMap.GetTileIndex(x, ends[1]);
			local types = AIBridgeList_Length(ends[1] - ends[0] + 1);
			if (road_bridge || types.IsEmpty()) continue;
			road_bridge = AIBridge.BuildBridge(AIVehicle.VT_ROAD, types.Begin(), a, b);
			if (road_bridge) {
				AILog.Info("ROAD BRIDGE " + x + "," + ends[0] + "-" + ends[1] + " " + AIBridge.GetName(types.Begin(), AIVehicle.VT_ROAD));
				AIRoad.BuildRoad(AIMap.GetTileIndex(x, ends[0] - 2), a);
				AIRoad.BuildRoad(b, AIMap.GetTileIndex(x, ends[1] + 2));
			}
		}
	}
	if (!road_bridge) AILog.Error("FAILED road bridge: " + AIError.GetLastErrorString());

	/* An aqueduct over the line along Y, between valley sides raised from level ground for its ends. */
	AITile.LevelTiles(AIMap.GetTileIndex(x1, y0 + 1), AIMap.GetTileIndex(x1 - 5, y0 + 4));
	AITile.LevelTiles(AIMap.GetTileIndex(x1 + 1, y0 + 1), AIMap.GetTileIndex(x1 + 6, y0 + 4));
	this.RaiseCorners(x1 - 4, y0 + 2, x1 - 2, y0 + 3);
	this.RaiseCorners(x1 + 3, y0 + 2, x1 + 5, y0 + 3);
	if (this.Must(AIBridge.BuildBridge(AIVehicle.VT_WATER, 0, AIMap.GetTileIndex(x1 - 2, y0 + 2), AIMap.GetTileIndex(x1 + 2, y0 + 2)), "aqueduct")) {
		AILog.Info("AQUEDUCT " + (x1 - 2) + "," + (y0 + 2));
	}
	this.Must(AIRail.BuildRailStation(tB, AIRail.RAILTRACK_NW_SE, 1, 4, AIStation.STATION_NEW), "station B");
	this.Must(AIRail.BuildRailDepot(tDepot, tA), "depot");

	local engines = AIEngineList(AIVehicle.VT_RAIL);
	engines.Valuate(AIEngine.IsWagon);         engines.KeepValue(0);
	engines.Valuate(AIEngine.IsBuildable);     engines.KeepValue(1);
	engines.Valuate(AIEngine.HasPowerOnRail, rt); engines.KeepValue(1);
	engines.Valuate(AIEngine.GetMaxSpeed);     engines.Sort(AIList.SORT_BY_VALUE, false);
	local loco = engines.Begin();
	AILog.Info("LOCO " + AIEngine.GetName(loco) + " max " + AIEngine.GetMaxSpeed(loco));

	local train = AIVehicle.BuildVehicle(tDepot, loco);
	this.Must(AIVehicle.IsValidVehicle(train), "build loco");

	local pass = null;
	foreach (c, _ in AICargoList()) if (AICargo.HasCargoClass(c, AICargo.CC_PASSENGERS)) { pass = c; break; }
	local wagons = AIEngineList(AIVehicle.VT_RAIL);
	wagons.Valuate(AIEngine.IsWagon);        wagons.KeepValue(1);
	wagons.Valuate(AIEngine.IsBuildable);    wagons.KeepValue(1);
	wagons.Valuate(AIEngine.CanRunOnRail, rt); wagons.KeepValue(1);
	wagons.Valuate(AIEngine.GetCargoType);   wagons.KeepValue(pass);
	if (!wagons.IsEmpty()) {
		local wagon_engine = wagons.Begin();
		for (local i = 0; i < 3; i++) {
			local w = AIVehicle.BuildVehicle(tDepot, wagon_engine);
			if (AIVehicle.IsValidVehicle(w)) this.Must(AIVehicle.MoveWagon(w, 0, train, AIVehicle.GetNumWagons(train) - 1), "attach wagon");
		}
	}

	this.Must(AIOrder.AppendOrder(train, tA, AIOrder.OF_NONE), "order A");
	this.Must(AIOrder.AppendOrder(train, tB, AIOrder.OF_NONE), "order B");
	this.Must(AIVehicle.StartStopVehicle(train), "start");
	AILog.Info("TRAIN " + train + " depot=" + tDepot + " A=" + tA + " B=" + tB);

	/* Signals along the line, to see them in the 3D view. */
	foreach (x in [x0 + 8, x0 + 14]) {
		local t = AIMap.GetTileIndex(x, y0);
		if (!AIRail.BuildSignal(t, AIMap.GetTileIndex(x - 1, y0), AIRail.SIGNALTYPE_NORMAL_TWOWAY)) AILog.Warning("signal at " + x + " failed");
	}

	/* A second, straight line next to the first, so another train passes by in the 3D view. */
	local ys = y0 - 3;
	local okline = AIRail.BuildRailStation(AIMap.GetTileIndex(x0, ys), AIRail.RAILTRACK_NE_SW, 1, 4, AIStation.STATION_NEW);
	for (local x = x0 + 4; okline && x < x1 - 4; x++) okline = AIRail.BuildRailTrack(AIMap.GetTileIndex(x, ys), AIRail.RAILTRACK_NE_SW);
	okline = okline && AIRail.BuildRailStation(AIMap.GetTileIndex(x1 - 4, ys), AIRail.RAILTRACK_NE_SW, 1, 4, AIStation.STATION_NEW);
	okline = okline && AIRail.BuildRailDepot(AIMap.GetTileIndex(x0 - 1, ys), AIMap.GetTileIndex(x0, ys));
	if (okline) {
		local depot2 = AIMap.GetTileIndex(x0 - 1, ys);
		local train2 = AIVehicle.BuildVehicle(depot2, loco);
		if (AIVehicle.IsValidVehicle(train2)) {
			if (!wagons.IsEmpty()) {
				for (local i = 0; i < 2; i++) {
					local w = AIVehicle.BuildVehicle(depot2, wagons.Begin());
					if (AIVehicle.IsValidVehicle(w)) AIVehicle.MoveWagon(w, 0, train2, AIVehicle.GetNumWagons(train2) - 1);
				}
			}
			AIOrder.AppendOrder(train2, AIMap.GetTileIndex(x1 - 4, ys), AIOrder.OF_NONE);
			AIOrder.AppendOrder(train2, AIMap.GetTileIndex(x0, ys), AIOrder.OF_NONE);
			AIVehicle.StartStopVehicle(train2);
			AILog.Info("TRAIN2 " + train2);
		}
	} else {
		AILog.Warning("second line failed");
	}

	/* Other vehicle types, to try the cab view with. */
	local bus = this.BuildBus(x0, y0, pass);
	this.BuildShip(x0, y0, pass);
	this.BuildSeaShip(x0, y0, pass);
	this.BuildPlane(x0, y0, pass);

	/* After a while, sell the bus, to see its cab view close. */
	local sell_date = AIDate.GetCurrentDate() + 150;
	local selling = false;
	while (true) {
		this.Sleep(37);
		local t = AIVehicle.GetLocation(train);
		AILog.Info("POS " + train + " " + AIMap.GetTileX(t) + "," + AIMap.GetTileY(t) + " speed " + AIVehicle.GetCurrentSpeed(train) + " state " + AIVehicle.GetState(train));

		if (bus != null && !selling && AIDate.GetCurrentDate() >= sell_date) {
			AILog.Info("SELLING BUS " + bus);
			this.Must(AIVehicle.SendVehicleToDepot(bus), "send bus to depot");
			selling = true;
		}
		if (bus != null && selling && AIVehicle.IsStoppedInDepot(bus)) {
			this.Must(AIVehicle.SellVehicle(bus), "sell bus");
			AILog.Info("SOLD BUS " + bus);
			bus = null;
		}
	}
}
