class CabTestAI extends AIInfo {
	function GetAuthor()      { return "cab-view test"; }
	function GetName()        { return "CabTestAI"; }
	function GetShortName()   { return "CABT"; }
	function GetDescription() { return "Builds an L-shaped rail line next to a town and runs one passenger train on it."; }
	function GetVersion()     { return 1; }
	function GetDate()        { return "2026-10-12"; }
	function CreateInstance() { return "CabTestAI"; }
	function GetAPIVersion()  { return "15"; }
}
RegisterAI(CabTestAI());
