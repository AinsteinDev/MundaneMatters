#pragma once

// Borrowing and lending cheap everyday things: "can I borrow your pickaxe?" / "take my lantern,
// bring it back". Only cheap things are lent out to the player (tools, lanterns, books, misc
// items under "Max Loan Value"; never armour or real weapons). Lenient: an overdue loan is
// brought up when they meet, never held against the player. The player lending their own
// things has no limit: the NPC can use them and gives them back when asked.
namespace MundaneMatters::Loans
{
	// Can an NPC lend this out? `consumable` is set for things that get used up (food, potions,
	// torches, arrows): those are never loans, only gifts.
	bool Lendable(RE::TESBoundObject* item, bool& consumable);

	// Game thread. A cheap tool of the NPC's own trade they can fetch from their workshop even
	// when they don't carry one (a blacksmith's or miner's pickaxe, a woodcutter's axe), or null.
	RE::TESBoundObject* TradeTool(RE::Actor* npc, const std::string& said);

	// Game thread. The NPC lent the player `n` of `item`, wanted back in `backInHours` (0 = in a
	// few days). `handed` false: it's lying around for the player to pick up (the loan starts
	// when they do).
	void Borrow(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t n, float backInHours, bool handed);

	// Any thread. Does the player owe this NPC `item` (any count)?
	bool Owes(RE::FormID npc, RE::TESBoundObject* item);

	// Game thread. The player handed `n` of `item` to the NPC: how many of them settle a loan
	// (giving back the same kind of thing is giving it back). The rest is a gift.
	std::int32_t GiveBack(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t n);

	// Game thread. Coin handed to someone the player owes a thing they no longer have ("I lost
	// your pickaxe, here's 10 gold"): settles it when it covers the thing's worth. The name of
	// what was paid for, or "" (then it's a gift).
	std::string PayFor(RE::Actor* npc, std::int32_t gold);

	// Game thread. "Can I keep it?" - "Keep it": a loan matching `said` becomes theirs. The name
	// of what they can keep, or "" when there is no such loan.
	std::string LetKeep(RE::Actor* npc, const std::string& said);

	// Game thread. The player lent the NPC their own things.
	void LendTo(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t n);

	// Any thread. Things the player lent this NPC.
	bool HasLentTo(RE::FormID npc);
	std::int32_t LentCount(RE::FormID npc, RE::TESBoundObject* item);

	// Game thread. The NPC gave `n` of the player's lent `item` back (retrieve).
	void TookBack(RE::FormID npc, RE::TESBoundObject* item, std::int32_t n);

	// Game thread. "Can I have my sword back?" for things lent (not held): hands them over.
	// False when nothing lent matches.
	bool Retrieve(RE::Actor* npc, const std::string& said);

	// Any thread. Lines for the NPC's bio.
	std::vector<std::string> BioLines(RE::FormID npc);

	// Game thread, every ledger tick: pickups, reminders, lenders who died.
	void Tick();

	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();

	// Watches things put back in a lender's home. After data load.
	void Install();
}
