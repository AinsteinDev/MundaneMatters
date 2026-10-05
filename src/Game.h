#pragma once

// Small game-state helpers shared by services, promises and the ledger. Game thread only.
namespace MundaneMatters::Game
{
	inline RE::TESObjectMISC* Gold()
	{
		return RE::TESForm::LookupByID<RE::TESObjectMISC>(0xF);  // Gold001
	}

	inline std::int32_t ItemCount(RE::TESObjectREFR* ref, RE::TESBoundObject* item)
	{
		if (!ref || !item) {
			return 0;
		}
		auto counts = ref->GetInventoryCounts([item](RE::TESBoundObject& o) { return &o == item; });
		auto it = counts.find(item);
		return it == counts.end() ? 0 : it->second;
	}

	// Game hours since the start of the playthrough (survives save/load, follows waiting and sleeping).
	inline float Hours()
	{
		auto* cal = RE::Calendar::GetSingleton();
		return cal ? cal->GetHoursPassed() : 0.0f;
	}

	inline float HourOfDay()
	{
		auto* cal = RE::Calendar::GetSingleton();
		return cal ? cal->GetHour() : 12.0f;
	}

	inline RE::PlayerCharacter* Player()
	{
		return RE::PlayerCharacter::GetSingleton();
	}

	inline float DistanceToPlayer(RE::TESObjectREFR* ref)
	{
		auto* p = Player();
		if (!ref || !p) {
			return (std::numeric_limits<float>::max)();
		}
		// Coordinates only compare within one interior, or between exterior cells of a worldspace.
		auto* a = ref->GetParentCell();
		auto* b = p->GetParentCell();
		if (a != b && (!a || !b || a->IsInteriorCell() || b->IsInteriorCell())) {
			return (std::numeric_limits<float>::max)();
		}
		return ref->GetPosition().GetDistance(p->GetPosition());
	}

	// Vanilla corner notification. Safe from any thread.
	void Notify(const std::string& msg);

	// Notification shown only with "Debug Notifications" on.
	void DebugNotify(const std::string& msg);

	// Tells SkyrimNet what just happened (SkyrimNetApi.DirectNarration): the NPC answers it with
	// a fresh line to the player and it lands in their event history. Game thread.
	void Narrate(const std::string& what, RE::Actor* npc);

	// The player says `line` to the NPC (it goes into SkyrimNet's dialogue history as theirs).
	// respond: the NPC answers it now (false when something else already makes them speak).
	// Game thread.
	void Say(const std::string& line, RE::Actor* npc, bool respond);

	// Any thread. True for a line MundaneMatters itself just said for the player (a popup choice), so
	// it is never classified as a new request.
	bool IsOwnLine(const std::string& text);

	// One of a few phrasings, so the same choice doesn't always read the same.
	std::string Pick(const std::vector<std::string>& options);
}
