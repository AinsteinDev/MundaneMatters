#pragma once

// NPC promises: things an NPC commits to give or allow the player in their own line.
namespace MundaneMatters::Promises
{
	struct Promise
	{
		std::string type;  // give_item | give_gold | offer_bed | allow_take | give_key | meet (item = the place)
		std::string item;  // give_item: plain name ("cabbage soup")
		std::int32_t qty = 1;
		std::int32_t gold = 0;
		std::string when = "now";  // now | later | cancel (the NPC takes it back)
		std::string timeHint;      // "tomorrow", "tonight", ...
		float inHours = 0.0f;      // the classifier's own estimate of when it's due (0 = unknown)
		bool always = false;       // offer_bed: welcome any time, for good ("my home is your home")
		bool loan = false;         // give_item / allow_take: lent, wanted back ("borrow it", "bring it back")
		float backInHours = 0.0f;  // loan: when they want it back (0 = not said: a few days)
		int real = -1;             // offer_bed: the classifier's verdict on the whole exchange (1 a real place to stay, 0 not, -1 not given)
		int confirmed = -1;        // meet: the NPC firmly commits (1), only floats it (0), not given (-1)
		std::string saidPlace;     // meet: the place in their own words, when item is the real place picked for it
	};

	std::optional<Promise> FromJson(const json& j);

	// The exchange really says this promise, in plain words (not a figure of speech or a flirt: "my door
	// is always open for you" lends no bed, "you know where to find me" no key). A bed needs a bed / a
	// place to sleep or stay; a key a key; gold a sum or coin; an item or permission the thing itself,
	// named by either side. Taking back ("cancel") always counts.
	bool Said(const Promise& p, const std::string& playerLine, const std::string& npcLine);
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

	// Items in a container matching what was said ("sword", "my healing potions"), best first.
	// Quest items are skipped. Empty when nothing matches. Game thread.
	struct Found
	{
		RE::TESBoundObject* item = nullptr;
		std::int32_t count = 0;
		bool worn = false;
		double score = 0.0;
	};
	std::vector<Found> FindItems(RE::TESObjectREFR* container, const std::string& text);

	// Score of an item name against what was said (0 = no match).
	double NameScore(const std::string& text, const char* name);

	// "Iron Sword x3" (just the name for one).
	// A kind of thing said ("meat", "fish", "food", "a drink", "pelts", "potion") fits this item (Venison is meat).
	bool KindFits(const std::string& said, RE::TESBoundObject* item);

	std::string Counted(const std::string& name, std::int32_t n);

	// Is this interior the NPC's home (theirs or their household's)? Game thread.
	bool IsHome(RE::TESObjectCELL* cell, RE::Actor* npc);

	// Does this owner (an NPC or a faction) mean the NPC or their household?
	bool Belongs(RE::TESForm* owner, RE::Actor* npc);

	// Is this owner the NPC themselves (their personal bed), not just their household?
	bool IsTheirs(RE::TESForm* owner, RE::Actor* npc);

	// Hospitality, not a handout: the NPC serves the player a dish (their own food first, else a
	// common hot dish). Not counted against generosity. Returns the dish's name, "" if none. Game thread.
	std::string ServeMeal(RE::Actor* npc);

	// Game thread. Is this promise really a loan (so it doesn't eat the generosity budget)? A
	// "loan" of bread or a potion is a handout like any other: used-up things never come back.
	bool IsLoan(RE::Actor* npc, const Promise& p);

	// What a handout costs against that budget, in gold worth: food counts half (a bowl of stew
	// is easy to spare), coin and items in full. Game thread.
	std::int32_t HandoutCost(RE::Actor* npc, const Promise& p);

	// Any thread. Promised hand-outs this NPC gave the player within `within` ("pie", "10 gold").
	std::vector<std::string> GivenRecently(RE::FormID npc, std::chrono::seconds within = std::chrono::seconds(180));

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
		bool loan = false;  // the player lends it ("use my bow, give it back later"), not gives it
	};

	std::optional<Gift> GiftFromJson(const json& j);

	// Game thread. Only ever moves what the player really carries, after the confirm popup:
	// a named item from their inventory (equipped ones allowed, flagged), coin up to their purse,
	// or the vanilla gift menu for an unnamed gift. The NPC is told what they received.
	void Receive(RE::Actor* npc, const std::vector<Gift>& gifts);

	// "Here, I got this for you", "I have something for you": a gift offered without saying what. The
	// player picks it first (the picker opens now and the NPC's reply waits), so they react to the
	// real thing, not to a bundle they haven't been handed.
	bool UnnamedGiftWords(const std::string& line);

	// Game thread. The NPC's pending reply is held back and the picker opens. False when it couldn't.
	bool OfferUnnamedGift(RE::Actor* npc);

	// Any thread. An NPC's line to the player; if it is their reaction to a gift that could
	// warm them up, judge whether they were genuinely pleased.
	void OnNpcReply(RE::FormID npc, const std::string& line);

	// A save was loaded: forget recent transfers, kept promises and awaited gift reactions.
	void Reset();

	// Watches item transfers so a promise already fulfilled by another mod
	// (SeverActions GiveItem/GiveGold) isn't paid twice.
	void Install();
}
