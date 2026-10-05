#pragma once

#include "Promises.h"

// Promises that outlive the moment: lent beds (ownership handed back after N game hours)
// and "later" promises (kept when due and the NPC is near the player). Stored in the SKSE co-save.
namespace MundaneMatters::Ledger
{
	// Registers co-save callbacks and the game-thread ticker. Call from SKSEPluginLoad.
	void Install();

	// Game thread. Hands the bed to the player for `hours` game hours. A guest loan instead lasts
	// while the player is in that home and goes back to the host when they leave.
	void LoanBed(RE::Actor* npc, RE::TESObjectREFR* bed, float hours, bool guest = false);

	// "Go ahead, take the wine": that one object in their home is the player's for `hours`, so
	// picking it up isn't theft; if left where it is, it becomes theirs again. Game thread.
	void AllowTake(RE::TESObjectREFR* ref, float hours);

	// "You're always welcome here": the player may sleep in this NPC's home whenever they visit
	// (a bed of theirs becomes the player's for each visit). Any thread.
	void AddGuest(RE::FormID npc);
	void RemoveGuest(RE::FormID npc);
	bool IsGuest(RE::FormID npc);
	bool HasBedLoan(RE::FormID npc);

	// Any thread. Queues a promise to keep later ("tomorrow", "when you come back"), or one that
	// could not be kept yet (a bed offered while not in the NPC's home).
	void AddPending(RE::FormID npc, const std::string& npcName, const Promises::Promise& p);

	// Any thread. True when the same promise from this NPC is already waiting to be kept.
	bool IsPending(RE::FormID npc, const Promises::Promise& p);

	// Any thread. The NPC kept a promise in conversation ("here's the 100 gold I promised"):
	// drop the matching pending one so it isn't paid twice.
	void Fulfilled(RE::FormID npc, const Promises::Promise& p);

	// Any thread. The NPC took a promise back ("I can't pay you tomorrow after all").
	void Cancel(RE::FormID npc, const Promises::Promise& p);

	// Free handouts to the player (food, coin, items) come out of a budget that refills every
	// Generosity::hours. Any thread.
	std::int32_t BudgetLeft(RE::FormID npc, const Promises::Generosity& g);
	void RecordHandout(RE::FormID npc, const std::string& what, std::int32_t cost, float hours);

	struct HandoutWindow
	{
		float since = 0.0f;  // hours since the window opened
		std::int32_t spent = 0;
		std::string what;    // the last thing given
	};
	std::optional<HandoutWindow> CurrentHandouts(RE::FormID npc);

	// Gift warmth: true when this gift is the one that counts (the first since the last 7-day
	// window opened, and the NPC was never warmed by a gift before). Opens the window.
	bool GiftCounts(RE::FormID npc);
	// The one-step raise was used: gifts never move this NPC's rank again.
	void MarkWarmed(RE::FormID npc);

	// Any thread. Lines for the NPC's bio: open promises and lent beds. Empty when none.
	std::vector<std::string> BioLines(RE::FormID npc);
}
