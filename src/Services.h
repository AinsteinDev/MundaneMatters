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

	// SeverActions' quest carrying this script (cached), or null without SeverActions.
	RE::TESQuest* SeverActionsQuestFor(const char* script);

	// Follow/escort: anyone can offer them, so they don't make an NPC a "service NPC" for the
	// cost gate.
	bool IsMovement(const std::string& id);

	// Any thread: Mundane Matters started this NPC following / escorting the player recently (45 min).
	bool WalkingWithPlayer(RE::FormID npc);

	// Game thread: following the player right now - a companion, SeverActions' casual follow (its
	// follow link to the player, kept in the save), or one we started. Survives a reload.
	// A real companion: the player's teammate through the game or a follower framework, not one we
	// made a teammate only while they walk with the player (that flag is for friendly fire alone).
	bool IsCompanion(RE::Actor* npc);

	bool FollowsPlayer(RE::Actor* npc);

	// "Dawn. East ridge.": the NPC really goes there at that time, through IntelEngine's own scheduling
	// (its ScheduleMeeting). A place IntelEngine can't find is tried again as where they are now. False
	// without IntelEngine. Game thread.
	bool ScheduleMeeting(RE::Actor* npc, const std::string& place, const std::string& when, const std::string& saidPlace = "");

	// Talk of meeting up ("dawn, east ridge", "meet me at..."): the real places nearby are noted for the
	// NPC's bio, so they name one (and the classifier maps a made-up one onto them). Game thread.
	bool MeetWords(const std::string& line);
	void NoteMeetTalk(RE::Actor* npc);
	// Any thread: the bio line while meeting talk is fresh (a couple of minutes), else "".
	std::string MeetPlacesLine(RE::FormID npc);

	// Any thread. Meetings booked (for the promises book): who, "meet at X, at dawn", game hours since.
	struct Meeting
	{
		std::string who, what;
		float bookedAt = 0.0f;
	};
	std::vector<Meeting> Meetings();

	// "Wait here", "go home", "you can go": the player ending a follow / escort.
	bool IsStopWords(const std::string& line);
	// Game thread: ends our follow / escort of this NPC.
	void StopWalking(RE::FormID npc, const char* why);

	// ---- spending time together ("keep me company", "stay with me a while") ----
	// Our follow, kept close, with its own ends: a few game hours, the night slept (or any time
	// skipped), the player leaving them behind, a goodbye from either side, or the NPC saying they
	// have to go. Bonds grow with each hour of it.

	// Any thread. Spending time with the player right now.
	bool InCompany(RE::FormID npc);
	// Any thread. Following the player (our follow, not spending time, not leading the way).
	bool IsFollowing(RE::FormID npc);
	// Any thread. A line saying its speaker is leaving / sending the other off ("I have to go", "I'd
	// best be off", "farewell", "you should go"); not a question, a "not", or a later plan.
	bool Leaving(const std::string& line);
	// Any thread. The player asks them to stay a while, or the NPC offers to.
	bool PlayerAsksCompany(const std::string& line);
	bool NpcOffersCompany(const std::string& line);
	// Game thread. Ends it; `narration` (optional) tells the NPC how it ended.
	void EndCompany(RE::FormID npc, const char* why, const std::string& narration);

	// Game thread. The player spoke to someone asleep a few steps away: they wake (get up, stand by
	// the player) and answer; back to bed after a while without talk.
	void WakeIfAsleep(RE::FormID npc, const std::string& playerLine);
	// Any thread. The NPC spoke (a woken one has answered).
	void NoteNpcSpoke(RE::FormID npc);

	// ---- going home together ("let's go to your place and relax") ----
	// Any thread. The line talks of going to the NPC's, the player's or a shared home.
	bool VisitWords(const std::string& line);
	// Game thread. The visit service is worth offering: home talk, or "let's go inside" at the NPC's door.
	bool MightVisit(RE::Actor* npc, const std::string& playerLine, const std::string& npcLine);
	// Game thread. Before they answer: whose home is near, too far or missing, and how close they are to
	// the player (not friends: a nudge towards no), for their bio.
	void NoteVisitTalk(RE::Actor* npc);
	// Any thread: that bio line while fresh (a few minutes), else "".
	std::string VisitLine(RE::FormID npc);

	// Game thread, every couple of seconds: keeps every follow and escort of ours going.
	void TickWalking();
	// A save was loaded (from kPostLoadGame).
	void OnGameLoaded();
	json SaveWalking();
	void LoadWalking(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void RevertWalking();

	// Shared activities with people close to the player (a meal together; later more). Like
	// movement, they don't make an NPC a "service NPC".
	bool IsActivity(const std::string& id);

	// Care for the player: heal, cook, mend, cure (activities too).
	inline bool IsCare(const std::string& id)
	{
		return id == "heal" || id == "cook" || id == "mend" || id == "cure";
	}

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
		std::string item;        // hold / retrieve / buy / sell: what was named ("sword"), "" when vague
		std::int32_t qty = 1;    // buy / sell: how many (-1 = all of them)
		bool each = false;       // buy / sell: `price` was per piece
		float hours = 0.0f;      // sit: how long the player said ("till sunset"), 0 = not said
		std::string how;         // perform: "sing", "play" or "any"
	};

	// The hold a reference is in (Whiterun Hold, the Rift...), or null when unknown.
	RE::BGSLocation* HoldOf(RE::TESObjectREFR* ref);

	// "Whiterun (Whiterun Hold)" for the classifier: places without a town are meant locally.
	std::string WhereIs(RE::Actor* npc);

	// Named places in the NPC's hold, nearest first, with their kind ("Honningbrew Meadery
	// (brewery)"), so "the brewery" can be written as the real place. Game thread.
	std::vector<std::string> PlacesNear(RE::Actor* npc, std::size_t max);

	// The real name of the nearest place of a kind in the NPC's hold ("inn" -> "The Bannered Mare"),
	// or "" when there is none. Game thread.
	std::string NearestOfKind(RE::Actor* npc, const std::string& kind);

	// A free seat for the NPC (no work station, bed or throne): next to the player when they're
	// sitting, else near the NPC. Null when none within reach. Game thread.
	RE::TESObjectREFR* SeatFor(RE::Actor* npc);

	// Runs the service on the game thread. False when it could not start.
	bool Execute(const std::string& id, RE::Actor* npc, const Args& args);

	// Menus the services open; TurnPipeline skips a service whose menu opened recently
	// (SkyrimNet's own dialogue actions or SeverActions may already have opened it).
	const char* MenuFor(const std::string& id);
}
