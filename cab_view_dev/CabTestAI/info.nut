class CabTestAI extends AIInfo {
	function GetAuthor()      { return "cab-view test"; }
	function GetName()        { return "CabTestAI"; }
	function GetShortName()   { return "CABT"; }
	function GetDescription() { return "Builds an L-shaped rail line with a passenger train, a bus line, a canal with a ship and two airports with a plane, to try the cab view with. Sells the bus after 150 days."; }
	function GetVersion()     { return 1; }
	function GetDate()        { return "2026-10-12"; }
	function CreateInstance() { return "CabTestAI"; }
	function GetAPIVersion()  { return "15"; }
}
RegisterAI(CabTestAI());
