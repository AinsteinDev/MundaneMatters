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
		std::string acceptLabel;  // the player's line on the buttons ("Yes, lead the way"); empty = "Accept"
		std::string declineLabel;

		// Picker mode: the player chooses one of these (and how many) instead of a plain yes/no.
		struct Item
		{
			RE::FormID id = 0;
			std::string name;
			std::int32_t count = 1;
			std::int32_t value = 0;
			bool equipped = false;
		};
		std::vector<Item> pick;
	};

	enum class Choice
	{
		kAccept,
		kDecline,  // the player clicked Decline
		kTimeout   // nobody answered: treated as "no", but nothing is said in the player's name
	};

	// Creates the view. Call on kDataLoaded. False when PrismaUI is missing.
	bool Init();
	bool Available();

	// Game thread. Shows the popup; `done(choice)` runs on the game thread once the player
	// answers or the timer runs out. While another popup is open it waits its turn (up to 3
	// queued). False only when PrismaUI is unavailable or the queue is full.
	bool Ask(const Offer& offer, std::function<void(Choice)> done);

	// After a picker popup answered kAccept: what was chosen and how many. Game thread.
	std::pair<RE::FormID, std::int32_t> LastPick();

	// A save was loaded: drop open and queued popups (they belong to the other timeline).
	void Reset();
}
