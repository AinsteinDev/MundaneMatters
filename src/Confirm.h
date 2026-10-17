#pragma once

// Accept/decline popup for paid services and follow/escort (PrismaUI view "MundaneMatters/index.html").
namespace MundaneMatters::Confirm
{
	struct Offer
	{
		std::string title;   // "Rent a room"
		std::string npc;     // "Hulda"
		std::string detail;  // "A bed for the night"
		std::int32_t price = 0;
		std::int32_t playerGold = 0;
		std::int32_t seconds = 15;  // 0 = no timer: waits for the player (gifts)
		bool showPrice = true;  // false for things that cost nothing (follow, escort)
		bool receive = false;   // the player is paid this price (selling), not paying it
		std::string hint;       // picker: the line under the list; empty = "Click an item, then Give."
		std::string acceptLabel;  // the player's line on the buttons ("Yes, lead the way"); empty = "Accept"
		std::string declineLabel;
		std::string altLabel;     // a third button ("Dissolve"); empty = none. Answers kAlt
		std::string chosenText;   // picker: "Giving: " before the chosen item's name
		bool silentRefuse = true; // yes/no popups: a "Refuse silently" button (no altLabel): answers kTimeout,
		                          // a no with nothing said in the player's name
		// SeverActions' own popups that ask the same thing ("travel", "payment", "trade", "debt", "arrest",
		// "bounty", "retainer", ...): when one of them was up a moment ago, the player has been asked
		// already, and this popup isn't shown (answers kTimeout).
		std::vector<std::string> sameAs;

		// Picker mode: the player chooses one of these (and how many) instead of a plain yes/no.
		struct Item
		{
			RE::FormID id = 0;
			std::string name;
			std::int32_t count = 1;
			std::int32_t value = 0;
			bool equipped = false;
			std::string note;  // shown on the right instead of the value ("due tomorrow")
		};
		std::vector<Item> pick;
	};

	enum class Choice
	{
		kAccept,
		kDecline,  // the player clicked Decline
		kAlt,      // the third button (altLabel)
		kTimeout   // nobody answered: treated as "no", but nothing is said in the player's name
	};

	// Creates the view. Call on kDataLoaded. False when PrismaUI is missing.
	bool Init();
	bool Available();

	// Game thread. Shows the popup; `done(choice)` runs on the game thread once the player
	// answers or the timer runs out. While another popup is open it waits its turn (up to 3
	// queued). False only when PrismaUI is unavailable or the queue is full.
	bool Ask(const Offer& offer, std::function<void(Choice)> done);

	// After a picker popup answered kAccept / kAlt: what was chosen and how many. Game thread.
	std::pair<RE::FormID, std::int32_t> LastPick();

	// A save was loaded: drop open and queued popups (they belong to the other timeline).
	void Reset();

	// Any thread. An NPC just spoke: for a while, watch which of SeverActions' popups come up, so a
	// popup of ours for the same thing can stand down (Offer::sameAs).
	void WatchOthers();

	// Any thread. SeverActions' popup of this kind ("travel", "payment", ...) was up within the last minute.
	bool OtherAskedRecently(const std::string& kind);
}
