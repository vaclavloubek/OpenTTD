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

	while (true) {
		this.Sleep(37);
		local t = AIVehicle.GetLocation(train);
		AILog.Info("POS " + train + " " + AIMap.GetTileX(t) + "," + AIMap.GetTileY(t) + " speed " + AIVehicle.GetCurrentSpeed(train) + " state " + AIVehicle.GetState(train));
	}
}
