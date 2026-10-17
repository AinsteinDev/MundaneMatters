#pragma once

#include "Promises.h"

// Promises that outlive the moment: lent beds (ownership handed back after N game hours)
// and "later" promises (kept when due and the NPC is near the player). Stored in the SKSE co-save.
namespace MundaneMatters::Ledger
{
	// Registers co-save callbacks and the game-thread ticker. Call from SKSEPluginLoad.
	void Install();

	// A save is being written (from the game's save message until our co-save part is done, plus a
	// moment): menus Mundane Matters opens wait for it.
	void SaveStarted();
	bool Saving();

	// Game thread. Hands the bed to the player for `hours` game hours. A guest loan instead lasts
	// while the player is in that home and goes back to the host when they leave.
	void LoanBed(RE::Actor* npc, RE::TESObjectREFR* bed, float hours, bool guest = false);

	// "Go ahead, take the wine": that one object in their home is the player's for `hours`, so
	// picking it up isn't theft; if left where it is, it becomes theirs again. Game thread.
	void AllowTake(RE::TESObjectREFR* ref, float hours, RE::FormID npc = 0);

	// True when this NPC has let the player take something that is still lying there.
	bool HasPermissions(RE::FormID npc);

	// Everyone who has given the player something lasting (guest rights, a bed, a permission).
	std::vector<RE::FormID> Hosts();

	// Trust broken: guest rights, lent beds, permissions and pending gifts from this NPC end.
	// Returns what was taken back ("guest rights", "the bed"...). Any thread.
	std::vector<std::string> RevokeAll(RE::FormID npc);

	// "You're always welcome here": the player may sleep in this NPC's home whenever they visit
	// (a bed of theirs becomes the player's for each visit). Any thread.
	void AddGuest(RE::FormID npc);
	void RemoveGuest(RE::FormID npc);
	bool IsGuest(RE::FormID npc);
	bool HasBedLoan(RE::FormID npc);

	// Game thread. Who sleeps where at this host's home, as agreed in talk: "player_floor" (a bedroll
	// for the player; the host keeps the bed), "npc_floor" (the host's bed for the player, a bedroll for
	// the host) or "share". Laid out when the bed is lent (or now, if it already is).
	void AgreeSleeping(RE::FormID npc, const std::string& how);

	// Game thread. How the player sleeps at this host's home, while in it ("player_floor", "npc_floor",
	// "share", "packed" for a bedroll they rolled up; "bed" for a plain bed loan), or "" when not.
	std::string StayAt(RE::FormID npc);

	// Any thread. NPCs' promises to the player for the promises book: who, what, and game hours left
	// (a negative value = no set time).
	struct Owed
	{
		std::string who, what;
		float hoursLeft = -1.0f;
		bool stay = false;  // a place to sleep / always welcome: not a promise, the book's own page
		std::string note;   // stay: how they sleep there ("a bedroll by the hearth")
	};
	std::vector<Owed> TheirPromises();

	// "I'll move the bedroll here", "can you put the bedroll here": a bedroll word and a moving word
	// (typos too: "bedrool", "bed roll").
	bool BedrollHereWords(const std::string& line);

	// Game thread. The player's guest bedroll at a host's (in this cell) moves to where the player
	// stands (or, not laid yet, is laid there now). False when there's none here.
	bool MoveGuestBedroll();

	// "Pick up my bedroll", "pack the bedroll", "roll up my bedroll": a bedroll word and a packing word.
	bool BedrollPickUpWords(const std::string& line);

	// Game thread. The player's guest bedroll here is packed away and stays packed (not laid again by
	// itself) until they say where to put it. False when there's none here.
	bool PickUpGuestBedroll();

	// Game thread, once the game is ready. Applies Data/SKSE/Plugins/MundaneMatters_grants.json: things
	// an NPC really agreed to in a conversation the mod missed, each entry once per save (by "id").
	// [{"id": "...", "npc": "Plugin.esp|0xLOCALID" or "0xFORMID", "type": "offer_bed" | "guest",
	//   "hours": 48, "sleep": "player_floor" | "npc_floor" | "share", "note": "..."}]
	void ApplyGrants();

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
