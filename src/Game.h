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

	float DistanceToPlayer(RE::TESObjectREFR* ref);

	// Game thread. Who a line with no target is meant for: the NPC under the crosshair, else the one
	// living, non-hostile NPC within talking range (more than one: unknown, 0).
	inline RE::FormID TalkTarget()
	{
		auto* player = Player();
		// Mid-conversation in the dialogue menu: it's for them.
		if (auto* mtm = RE::MenuTopicManager::GetSingleton(); mtm && mtm->menuOpen) {
			if (auto speaker = mtm->speaker.get(); speaker && speaker->As<RE::Actor>()) {
				return speaker->GetFormID();
			}
		}
		auto usable = [&](RE::Actor* a) {
			return a && a != player && !a->IsDead() && !a->IsDisabled() && !a->IsHostileToActor(player) && a->GetRace() &&
			       a->GetRace()->HasKeywordString("ActorTypeNPC");
		};
		if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
			if (auto ref = pick->GetActiveTarget().get(); ref) {
				if (auto* a = ref->As<RE::Actor>(); usable(a) && a->GetPosition().GetDistance(player->GetPosition()) < 700.0f) {
					return a->GetFormID();
				}
			}
		}
		RE::FormID only = 0;
		int count = 0;
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : lists->highActorHandles) {
				auto a = handle.get();
				if (usable(a.get()) && a->GetPosition().GetDistance(player->GetPosition()) < 400.0f) {
					only = a->GetFormID();
					++count;
				}
			}
		}
		return count == 1 ? only : 0;
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

	// An actor's name as the player knows it: SkyrimNet's name for them (generic NPCs get one), else
	// the game's. Any thread.
	std::string NameOf(RE::Actor* a);

	// Call once from the main thread (SKSE's messaging callbacks run there).
	void NoteMainThread();

	// Notification shown only with "Debug Notifications" on.
	void DebugNotify(const std::string& msg);

	// Tells SkyrimNet what just happened (SkyrimNetApi.DirectNarration): the NPC answers it with
	// a fresh line to the player and it lands in their event history. Game thread.
	void Narrate(const std::string& what, RE::Actor* npc);

	// A moment that colours the scene (a drink shared, sitting down, finishing up): narrated when things
	// are quiet; while talk is flowing (a line in the last 8 s) recorded quietly instead - they know it
	// happened and bring it up when it fits, not answering it mid-topic. Results the player must hear
	// ("there's nowhere to sit") use Narrate.
	void Mention(const std::string& what, RE::Actor* npc);

	// Any thread: someone in the conversation with the player just spoke.
	void NoteSpoke();

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
