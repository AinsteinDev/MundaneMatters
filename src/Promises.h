#pragma once

// NPC promises: things an NPC commits to give or allow the player in their own line.
namespace MundaneMatters::Promises
{
	struct Promise
	{
		std::string type;  // give_item | give_gold | offer_bed | allow_take
		std::string item;  // give_item: plain name ("cabbage soup")
		std::int32_t qty = 1;
		std::int32_t gold = 0;
		std::string when = "now";  // now | later | cancel (the NPC takes it back)
		std::string timeHint;      // "tomorrow", "tonight", ...
		float inHours = 0.0f;      // the classifier's own estimate of when it's due (0 = unknown)
		bool always = false;       // offer_bed: welcome any time, for good ("my home is your home")
	};

	std::optional<Promise> FromJson(const json& j);
	json ToJson(const Promise& p);

	// "a cabbage soup", "10 gold", "a place to sleep"
	std::string Describe(const Promise& p);

	// Same promise? Gold is gold, a bed is a bed; items compare by name ("soup" ~ "cabbage soup").
	bool Same(const Promise& a, const Promise& b);

	enum class Outcome
	{
		kKept,      // done
		kNotYet,    // can't right now (no bed in this cell, NPC not here): keep it pending
		kImpossible // never possible (unknown item, invalid NPC): drop it
	};

	// How much an NPC will hand the player for free: a budget in gold worth that refills every
	// `hours`, by how close they are (stranger 10 / 24 h ... lover 100 / 5 h). never = hostile.
	struct Generosity
	{
		float hours = 24.0f;
		std::int32_t budget = 10;
		bool never = false;
	};
	Generosity GenerosityOf(RE::Actor* npc);

	// Friend or closer (friend, confidant, ally, lover) or a companion.
	bool IsClose(RE::Actor* npc);

	// Hospitality, not a handout: the NPC serves the player a dish (their own food first, else a
	// common hot dish). Not counted against generosity. Returns the dish's name, "" if none. Game thread.
	std::string ServeMeal(RE::Actor* npc);

	// What a handout costs against that budget, in gold worth: food counts half (a bowl of stew
	// is easy to spare), coin and items in full. Game thread.
	std::int32_t HandoutCost(RE::Actor* npc, const Promise& p);

	// Any thread. True when this NPC kept the same promise within `within` (default: a few minutes).
	bool WasKept(RE::FormID npc, const Promise& p, std::chrono::seconds within = std::chrono::seconds(600));

	// Game thread, from the ledger tick: remembers items the player looked at lately, so "can I
	// take that wine" means the bottle they were looking at.
	void SampleCrosshair();

	// A bed of the NPC's in the player's cell that can be lent (their own, their household's,
	// or any in their home). Game thread.
	RE::TESObjectREFR* FindBedFor(RE::Actor* npc);

	// Keeps the promise right now. Game thread only.
	Outcome Keep(RE::Actor* npc, const Promise& p);

	// ---- the player giving to an NPC ----

	struct Gift
	{
		std::string item;  // as said ("health potion"); empty = unnamed ("I have something for you")
		std::int32_t qty = 1;
		std::int32_t gold = 0;
	};

	std::optional<Gift> GiftFromJson(const json& j);

	// Game thread. Only ever moves what the player really carries, after the confirm popup:
	// a named item from their inventory (equipped ones allowed, flagged), coin up to their purse,
	// or the vanilla gift menu for an unnamed gift. The NPC is told what they received.
	void Receive(RE::Actor* npc, const std::vector<Gift>& gifts);

	// Any thread. An NPC's line to the player; if it is their reaction to a gift that could
	// warm them up, judge whether they were genuinely pleased.
	void OnNpcReply(RE::FormID npc, const std::string& line);

	// A save was loaded: forget recent transfers, kept promises and awaited gift reactions.
	void Reset();

	// Watches item transfers so a promise already fulfilled by another mod
	// (SeverActions GiveItem/GiveGold) isn't paid twice.
	void Install();
}
