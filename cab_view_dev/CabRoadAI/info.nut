class CabRoadAI extends AIInfo {
	function GetAuthor()      { return "cab-view test"; }
	function GetName()        { return "CabRoadAI"; }
	function GetShortName()   { return "CABR"; }
	function GetDescription() { return "Runs two buses through the biggest town and out along a new road, with tram track on some town streets, to try the 3D cab view of road vehicles with."; }
	function GetVersion()     { return 1; }
	function GetDate()        { return "2026-10-13"; }
	function CreateInstance() { return "CabRoadAI"; }
	function GetAPIVersion()  { return "15"; }
}
RegisterAI(CabRoadAI());
