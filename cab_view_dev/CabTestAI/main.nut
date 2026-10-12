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

/** Ship sailing between two buoys on a canal, three tiles wide, with a depot at its side. */
function CabTestAI::BuildShip(cx, cy)
{
	local a = this.FindArea(cx, cy, 18, 3, 14);
	if (a == null) { AILog.Error("NO SHIP AREA"); return null; }
	local x0 = a[0], y0 = a[1];
	for (local y = y0; y < y0 + 3; y++) {
		for (local x = x0; x < x0 + 18; x++) this.Must(AIMarine.BuildCanal(AIMap.GetTileIndex(x, y)), "canal " + x + "," + y);
	}
	local tA = AIMap.GetTileIndex(x0 + 1, y0 + 1);
	local tB = AIMap.GetTileIndex(x0 + 16, y0 + 1);
	this.Must(AIMarine.BuildBuoy(tA), "buoy A");
	this.Must(AIMarine.BuildBuoy(tB), "buoy B");
	local tDepot = AIMap.GetTileIndex(x0 + 8, y0 + 2);
	this.Must(AIMarine.BuildWaterDepot(tDepot, AIMap.GetTileIndex(x0 + 9, y0 + 2)), "ship depot");

	local engine = this.PickEngine(AIVehicle.VT_WATER, null);
	if (engine == null) { AILog.Error("NO SHIP ENGINE"); return null; }
	local ship = AIVehicle.BuildVehicle(tDepot, engine);
	if (!this.Must(AIVehicle.IsValidVehicle(ship), "build ship")) return null;
	this.Must(AIOrder.AppendOrder(ship, tA, AIOrder.OF_NONE), "ship order A");
	this.Must(AIOrder.AppendOrder(ship, tB, AIOrder.OF_NONE), "ship order B");
	this.Must(AIVehicle.StartStopVehicle(ship), "start ship");
	AILog.Info("SHIP " + ship + " A=" + tA + " B=" + tB + " " + AIEngine.GetName(engine));
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

	this.Must(AIRail.BuildRailStation(tA, AIRail.RAILTRACK_NE_SW, 1, 4, AIStation.STATION_NEW), "station A");
	for (local x = x0 + 4; x < x1; x++) this.Must(AIRail.BuildRailTrack(AIMap.GetTileIndex(x, y0), AIRail.RAILTRACK_NE_SW), "track x " + x);
	this.Must(AIRail.BuildRailTrack(AIMap.GetTileIndex(x1, y0), AIRail.RAILTRACK_NE_SE), "corner");
	for (local y = y0 + 1; y < y1 - 3; y++) this.Must(AIRail.BuildRailTrack(AIMap.GetTileIndex(x1, y), AIRail.RAILTRACK_NW_SE), "track y " + y);
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

	/* Other vehicle types, to try the cab view with. */
	local bus = this.BuildBus(x0, y0, pass);
	this.BuildShip(x0, y0);
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
