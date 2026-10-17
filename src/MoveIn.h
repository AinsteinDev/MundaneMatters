#pragma once

// "Come live with me": someone close (Friend and above) moves into one of the player's houses, picked
// in a popup. They walk there on their own (SeverActions travel; placed there if they haven't arrived
// after a game day or can't find the way), and SeverActions' home routine takes their days over: home
// hours there, sleep in a bed there. It ends when the player asks them to move out, or on their own if
// things go sour between them (no longer close, or a grudge).
namespace MundaneMatters::MoveIn
{
	// Any thread. The player's line asks them to come live with them ("come live with me", "move in
	// with me"), or the NPC's line asks to ("could I live with you?").
	bool PlayerAsks(const std::string& line);
	bool NpcAsks(const std::string& line);
	// Any thread. The player asking them to move out ("move out", "go back to your old home").
	bool MoveOutWords(const std::string& line);

	// Game thread. The player's houses (a key carried, rooms owned, or standing in it): location and name.
	std::vector<std::pair<RE::BGSLocation*, std::string>> PlayerHouses();

	// Any thread. Lives with the player (or is on the way to).
	bool Lives(RE::FormID npc);
	// Game thread. A bedroll laid for someone living with the player (theirs tonight: never someone else's).
	bool IsHousemateBedroll(RE::FormID ref);

	// Any thread. The player's house they moved into (or are moving to), and its name; null when none.
	std::pair<RE::BGSLocation*, std::string> SharedHouse(RE::FormID npc);
	// Game thread. Lives with the player in the house this cell belongs to.
	bool LivesIn(RE::FormID npc, RE::TESObjectCELL* cell);

	// Any thread. "This is your bed", "you can sleep here", "pick a bed" said to someone living with the player.
	bool BedWords(const std::string& line);
	// Game thread. Their bed in the house they live in (only there): the bed the player looks at, or a
	// popup of the room's beds. Never the player's own bed, unless they're a Lover (then shared).
	void AssignBed(RE::FormID npc, const std::string& line);

	// Game thread, after the NPC agreed: closeness checked, then the house popup.
	bool Invite(RE::Actor* npc);
	// Game thread. "Move out?" popup; yes and they go back to their old life.
	void AskMoveOut(RE::FormID npc);

	// Game thread, every ledger tick: the trip, settling in, the bed, and whether they still want to.
	void Tick();

	std::vector<std::string> BioLines(RE::FormID npc);

	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();
}
