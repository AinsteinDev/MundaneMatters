#pragma once

// Bonds: ordinary people come to know the player through real deeds, never through words. Only
// what actually happened in the game counts (a meal shared, a night's rest, a kept promise, a
// gift, a real talk, a fight side by side, a rescue, a journey), with fixed points, a daily cap, and steps that need days and
// different kinds of deeds: Acquaintance -> Friend -> Confidant -> Ally. Never Lover.
//
// Bonds is secondary: whatever rank the game or another mod has set is canon. It only ever raises
// a rank, never past Ally, never Lover. "Bonds with Followers" off leaves followers (past ones too)
// to a follower relationship mod.
namespace MundaneMatters::Bonds
{
	enum class Deed
	{
		kMeal,     // shared a meal
		kRest,     // rested together and the player slept
		kPromise,  // they kept a promise / gave the player something
		kHeld,     // they kept the player's things and gave them back
		kGift,     // the player gave them a gift worth something
		kStay,     // the player slept in their home
		kTalk,     // a real talk: several exchanges in a day (was "time together"; same slot in saves)
		kFight,    // fought side by side: an enemy fell while both were in the fight
		kTravel,   // arrived somewhere (a town, a dungeon) together
		kProtect,  // the player saved them: killed what was attacking them, or healed them, while they were hurt
		kReturned, // the player gave back (or paid for) what they lent
		kCared,    // they took care of the player for free: healed, cooked, mended, cured
		kDrink,    // sat down for a drink together
		kSat,      // sat a while together (hours passed, or real minutes side by side)
		kSong,     // they played or sang for the player, at their asking
		kWord,     // the player kept a promise to them
		kSpar,     // a friendly brawl, fairly fought, whoever won
		kHome      // moved in with the player
	};

	// Logs the setup. After data load.
	void Install();

	// Game thread. Something really happened between them.
	void Add(RE::Actor* npc, Deed deed, int points);

	// Game thread. Trust broken by a deed (theft, a blow): half the progress toward the next step is lost.
	void Setback(RE::FormID npc);

	// Game thread. A small disappointment (a broken promise): `points` lost toward the next step.
	void Lose(RE::FormID npc, int points);

	// Game thread. The player woke up: the people whose home they slept in count it.
	void OnPlayerSlept();

	// Game thread. The player and this NPC just had one exchange (a line and its answer).
	void OnExchange(RE::FormID npc);

	// Game thread, every ledger tick: arriving somewhere together.
	void Tick();

	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();
}
