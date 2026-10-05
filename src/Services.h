#pragma once

namespace MundaneMatters::Services
{
	struct Service
	{
		std::string id;        // classifier id: "trade", "rent_room", "train", "follow", "escort"
		std::string label;     // what the classifier is told
		std::string examples;  // a few paraphrases, to anchor meaning (never matched literally)
	};

	// Services this NPC really offers, from game data (vendor faction, trainer class,
	// RentRoomScript; follow/escort for anyone when SeverActions is installed), filtered by the
	// feature toggles. Game thread only.
	std::vector<Service> For(RE::Actor* npc);

	// Follow/escort: anyone can offer them, so they don't make an NPC a "service NPC" for the
	// cost gate.
	bool IsMovement(const std::string& id);

	// Shared activities with people close to the player (a meal together; later more). Like
	// movement, they don't make an NPC a "service NPC".
	bool IsActivity(const std::string& id);

	struct Room
	{
		RE::TESObjectREFR* bed = nullptr;
		std::int32_t cost = 0;
		bool rentedByPlayer = false;
	};

	// The innkeeper's rentable room (vanilla RentRoomScript), or nullopt for non-innkeepers.
	std::optional<Room> RoomOf(RE::Actor* npc);

	struct Args
	{
		std::int32_t price = 0;  // what the NPC asked in their line (0 = not said; haggling honoured within limits)
		bool free = false;       // the NPC waived the price
		std::string place;       // escort: where to
		bool ownTrip = false;    // escort: the NPC is heading there anyway
	};

	// The hold a reference is in (Whiterun Hold, the Rift...), or null when unknown.
	RE::BGSLocation* HoldOf(RE::TESObjectREFR* ref);

	// "Whiterun (Whiterun Hold)" for the classifier: places without a town are meant locally.
	std::string WhereIs(RE::Actor* npc);

	// Named places in the NPC's hold, nearest first, with their kind ("Honningbrew Meadery
	// (brewery)"), so "the brewery" can be written as the real place. Game thread.
	std::vector<std::string> PlacesNear(RE::Actor* npc, std::size_t max);

	// Runs the service on the game thread. False when it could not start.
	bool Execute(const std::string& id, RE::Actor* npc, const Args& args);

	// Menus the services open; TurnPipeline skips a service whose menu opened recently
	// (SkyrimNet's own dialogue actions or SeverActions may already have opened it).
	const char* MenuFor(const std::string& id);
}
