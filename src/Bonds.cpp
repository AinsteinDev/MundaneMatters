#include "Bonds.h"
#include "Services.h"

#include <bit>
#include <set>

#include "Game.h"
#include "Ledger.h"
#include "MoveIn.h"
#include "JsonUtil.h"
#include "Papyrus.h"
#include "Promises.h"
#include "SkyrimNetBridge.h"
#include "Trust.h"

namespace MundaneMatters::Bonds
{
	namespace
	{
		constexpr int kDailyCap = 3;              // points per person per game day, whatever happens
		constexpr int kTalkExchanges = 3;         // exchanges in a day that make a real talk
		constexpr float kTogether = 1500.0f;      // at the player's side (travel)
		constexpr float kInTheFight = 3000.0f;    // close enough to have been in the same fight
		constexpr float kHurt = 0.5f;             // health fraction below which help is a rescue

		// What each step needs since the last one: points, separate days, different kinds of deeds.
		struct Step
		{
			int rank;  // the rank it leads to
			int points;
			int days;
			int kinds;
			const char* name;
		};
		constexpr std::array<Step, 3> kSteps{ {
			// Days are the brake: Confidant takes about a week of days together, Ally over two.
			{ 1, 6, 3, 2, "Friend" },
			{ 2, 12, 4, 3, "Confidant" },
			{ 3, 24, 10, 4, "Ally" },
		} };

		struct Bond
		{
			int points = 0;            // toward the next step
			std::uint32_t kinds = 0;   // bit per Deed, since the last step
			std::set<int> days;        // game days with a deed, since the last step
			int today = -1;            // game day of `todayPoints`
			int todayPoints = 0;
			std::uint32_t todayKinds = 0;  // each kind of deed counts once a day (no hold-and-take-back loops)
			int talkDay = -1;              // game day of `talks`
			int talks = 0;                 // exchanges with the player that day
		};

		std::mutex g_lock;
		std::unordered_map<RE::FormID, Bond> g_bonds;
		std::unordered_set<RE::FormID> g_everFollowed;  // anyone who has ever been the player's follower
		RE::FormID g_place = 0;  // the town or dungeon the player is in (travel)

		int Day()
		{
			return static_cast<int>(Game::Hours() / 24.0f);
		}

		int RankOf(RE::Actor* npc)
		{
			auto* base = npc->GetActorBase();
			auto* playerBase = Game::Player()->GetActorBase();
			auto* rel = base && playerBase ? RE::BGSRelationship::GetRelationship(base, playerBase) : nullptr;
			using L = RE::BGSRelationship::RELATIONSHIP_LEVEL;
			switch (rel ? rel->level.get() : L::kAcquaintance) {
			case L::kLover:
				return 4;
			case L::kAlly:
				return 3;
			case L::kConfidant:
				return 2;
			case L::kFriend:
				return 1;
			case L::kAcquaintance:
				return 0;
			default:
				return -1;  // rival and worse
			}
		}

		// Ordinary people only, and never anyone the game (or another mod) has already taken further.
		// With "Bonds with Followers" off, followers - past ones too - are left to a follower
		// relationship mod (those keep tracking someone after they're dismissed).
		bool Eligible(RE::Actor* npc)
		{
			if (!npc || npc == Game::Player() || npc->IsDead() || !SkyrimNet::ConfigBool("features.bonds", true)) {
				return false;
			}
			auto* race = npc->GetRace();
			const bool person = npc->HasKeywordString("ActorTypeNPC") || (race && race->HasKeywordString("ActorTypeNPC"));
			if (!person || npc->IsHostileToActor(Game::Player())) {
				return false;
			}
			const int rank = RankOf(npc);
			if (rank < 0 || rank >= 3) {
				return false;  // a rival, or already an ally or lover: nothing for us to do
			}
			if (npc->IsChild() && rank >= 1) {
				return false;  // children stop at Friend
			}
			if (!SkyrimNet::ConfigBool("features.bonds_followers", true)) {
				std::scoped_lock l(g_lock);
				if (Services::IsCompanion(npc) || g_everFollowed.contains(npc->GetFormID())) {
					return false;
				}
			}
			return true;
		}

		// The town, settlement or dungeon a location belongs to (a house inside Whiterun is Whiterun).
		RE::FormID PlaceOf(RE::BGSLocation* loc)
		{
			for (int depth = 0; loc && depth < 8; ++depth, loc = loc->parentLoc) {
				for (auto kw : { "LocTypeCity", "LocTypeTown", "LocTypeSettlement", "LocTypeHabitation", "LocTypeDungeon" }) {
					if (loc->HasKeywordString(kw)) {
						return loc->GetFormID();
					}
				}
			}
			return 0;
		}

		float HealthFraction(RE::Actor* a)
		{
			auto* av = a->AsActorValueOwner();
			const float max = av->GetPermanentActorValue(RE::ActorValue::kHealth);
			return max > 0.0f ? av->GetActorValue(RE::ActorValue::kHealth) / max : 1.0f;
		}

		// Fights and rescues, from what the game says happened.
		struct Sinks :
			RE::BSTEventSink<RE::TESDeathEvent>,
			RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
		{
			// An enemy fell: everyone fighting it alongside the player fought side by side; whoever
			// it was attacking, if the player struck it down while they were hurt, was saved.
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* e, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				if (!e || !e->dead || !e->actorDying) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* dying = e->actorDying->As<RE::Actor>();
				auto* player = Game::Player();
				if (!dying || dying == player || !dying->IsHostileToActor(player)) {
					return RE::BSEventNotifyControl::kContinue;  // a chicken, a brawl, a friend: not an enemy
				}
				const bool byPlayer = e->actorKiller && e->actorKiller.get() == player;
				if (!byPlayer && !player->IsInCombat()) {
					return RE::BSEventNotifyControl::kContinue;  // a fight the player wasn't part of
				}
				const auto victim = dying->GetActorRuntimeData().currentCombatTarget.get();
				const RE::FormID attacked = victim ? victim->GetFormID() : 0;
				SKSE::GetTaskInterface()->AddTask([byPlayer, attacked] {
					if (auto* lists = RE::ProcessLists::GetSingleton()) {
						for (auto& h : lists->highActorHandles) {
							auto a = h.get();
							if (!a || a->IsDead() || !a->IsInCombat() || Game::DistanceToPlayer(a.get()) > kInTheFight) {
								continue;
							}
							if (byPlayer && a->GetFormID() == attacked && HealthFraction(a.get()) < kHurt) {
								Bonds::Add(a.get(), Deed::kProtect, 2);
							}
							Bonds::Add(a.get(), Deed::kFight, 1);
						}
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}

			// The player healed them while they were hurt and in danger.
			RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* e, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
			{
				if (!e || !e->caster || !e->target || e->caster.get() != Game::Player()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* effect = RE::TESForm::LookupByID<RE::EffectSetting>(e->magicEffect);
				auto* target = e->target->As<RE::Actor>();
				if (!effect || !target || target == Game::Player() || effect->IsHostile() || effect->IsDetrimental() ||
					effect->data.primaryAV != RE::ActorValue::kHealth ||
					!(effect->HasArchetype(RE::EffectSetting::Archetype::kValueModifier) || effect->HasArchetype(RE::EffectSetting::Archetype::kPeakValueModifier))) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto id = target->GetFormID();
				SKSE::GetTaskInterface()->AddTask([id] {
					auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
					if (a && !a->IsDead() && a->IsInCombat() && HealthFraction(a) < kHurt) {
						Bonds::Add(a, Deed::kProtect, 2);
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Game thread. One step up when everything the step needs is there.
		void MaybeStep(RE::Actor* npc)
		{
			const int rank = RankOf(npc);
			const Step* step = nullptr;
			for (auto& s : kSteps) {
				if (s.rank == rank + 1) {
					step = &s;
				}
			}
			if (!step || (npc->IsChild() && step->rank > 1)) {
				return;
			}
			{
				std::scoped_lock l(g_lock);
				auto& b = g_bonds[npc->GetFormID()];
				const int kinds = std::popcount(b.kinds);
				if (b.points < step->points || static_cast<int>(b.days.size()) < step->days || kinds < step->kinds) {
					return;
				}
				b.points = 0;
				b.kinds = 0;
				b.days.clear();
			}
			Papyrus::CallMethod(npc, "Actor", "SetRelationshipRank", nullptr, static_cast<RE::Actor*>(Game::Player()), static_cast<std::int32_t>(step->rank));
			const std::string name = npc->GetName();
			const std::string player = Game::Player()->GetName();
			Game::Notify(std::format("{}: you are now {} ({}).", name, step->rank == 1 ? "friends" : step->rank == 2 ? "close confidants" : "allies", step->name));
			SkyrimNet::AddMemory(npc->GetFormID(),
				std::format("After everything they have shared, {} has come to see {} as {}.", name, player,
					step->rank == 1 ? "a friend" : step->rank == 2 ? "a close confidant" : "a true ally they would stand by"),
				0.7f);
			logger::info("bonds: {} -> {}", name, step->name);
		}
	}

	void Install()
	{
		static Sinks sinks;
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESDeathEvent>(&sinks);
			holder->AddEventSink<RE::TESMagicEffectApplyEvent>(&sinks);
		}
		logger::info("bonds: ready (followers {})", SkyrimNet::ConfigBool("features.bonds_followers", true) ? "included" : "left to other mods");
	}

	void Add(RE::Actor* npc, Deed deed, int points)
	{
		if (!Eligible(npc) || points <= 0) {
			return;
		}
		// Nothing grows while they're angry with the player.
		if (Trust::Holds(npc->GetFormID())) {
			return;
		}
		const int day = Day();
		int gained = 0;
		{
			std::scoped_lock l(g_lock);
			auto& b = g_bonds[npc->GetFormID()];
			if (b.today != day) {
				b.today = day;
				b.todayPoints = 0;
				b.todayKinds = 0;
			}
			const auto bit = 1u << static_cast<int>(deed);
			if (b.todayKinds & bit) {
				return;
			}
			// A day on the road: fighting and travelling together are one day's adventure, one
			// point between them (each still counts as its own kind of deed).
			const auto fight = 1u << static_cast<int>(Deed::kFight), travel = 1u << static_cast<int>(Deed::kTravel);
			if ((deed == Deed::kFight && (b.todayKinds & travel)) || (deed == Deed::kTravel && (b.todayKinds & fight))) {
				points = 0;
			}
			b.todayKinds |= bit;
			gained = (std::max)(0, (std::min)(points, kDailyCap - b.todayPoints));
			b.todayPoints += gained;
			b.points += gained;
			b.kinds |= bit;
			b.days.insert(day);
		}
		static constexpr std::array<const char*, 19> kNames{ "meal", "rest", "promise", "held", "gift", "stay", "talk", "fight", "travel", "protect", "returned", "cared", "drink", "sat", "song", "word", "spar", "home", "?" };
		logger::info("bonds: {} +{} ({})", npc->GetName(), gained, kNames[(std::min)(static_cast<std::size_t>(deed), kNames.size() - 1)]);
		MaybeStep(npc);
	}

	void Setback(RE::FormID id)
	{
		std::scoped_lock l(g_lock);
		if (auto it = g_bonds.find(id); it != g_bonds.end()) {
			it->second.points /= 2;
			logger::info("bonds: {:08X} lost half their progress", id);
		}
	}

	void Lose(RE::FormID id, int points)
	{
		std::scoped_lock l(g_lock);
		if (auto it = g_bonds.find(id); it != g_bonds.end()) {
			it->second.points = (std::max)(0, it->second.points - points);
			logger::info("bonds: {:08X} lost {} points (a broken promise)", id, points);
		}
	}

	void OnPlayerSlept()
	{
		auto* cell = Game::Player()->GetParentCell();
		if (!cell || !cell->IsInteriorCell()) {
			return;
		}
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			for (auto& h : lists->highActorHandles) {
				auto a = h.get();
				// Their guest (welcome, or a bed they lent), not a paying customer of their inn.
				if (a && a->GetParentCell() == cell && Promises::IsHome(cell, a.get()) &&
					(Ledger::IsGuest(a->GetFormID()) || Ledger::HasBedLoan(a->GetFormID()))) {
					Add(a.get(), Deed::kStay, 1);
				} else if (a && a->GetParentCell() == cell && MoveIn::LivesIn(a->GetFormID(), cell)) {
					Add(a.get(), Deed::kStay, 1);  // a night under the roof they share with the player
				}
			}
		}
	}

	void OnExchange(RE::FormID id)
	{
		auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
		if (!Eligible(npc)) {
			return;
		}
		const int day = Day();
		bool talked = false;
		{
			std::scoped_lock l(g_lock);
			auto& b = g_bonds[id];
			if (b.talkDay != day) {
				b.talkDay = day;
				b.talks = 0;
			}
			talked = ++b.talks == kTalkExchanges;  // a real talk, not a passing word
		}
		if (talked) {
			Add(npc, Deed::kTalk, 1);
		}
	}

	void Tick()
	{
		if (!SkyrimNet::ConfigBool("features.bonds", true)) {
			return;
		}
		auto* lists = RE::ProcessLists::GetSingleton();
		if (lists) {
			for (auto& h : lists->highActorHandles) {
				if (auto a = h.get(); a && Services::IsCompanion(a.get())) {
					std::scoped_lock l(g_lock);
					g_everFollowed.insert(a->GetFormID());
				}
			}
		}
		// Arriving somewhere together: a new town, settlement or dungeon, with them at the player's side.
		const auto place = PlaceOf(Game::Player()->GetCurrentLocation());
		const auto before = std::exchange(g_place, place);
		if (!place || place == before || !before || !lists) {
			return;  // still there, nowhere in particular, or just loaded in
		}
		for (auto& h : lists->highActorHandles) {
			auto a = h.get();
			if (a && !a->IsDead() && !a->IsInCombat() && Game::DistanceToPlayer(a.get()) < kTogether && Eligible(a.get())) {
				Add(a.get(), Deed::kTravel, 1);
			}
		}
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json out = json::array();
		json followed = json::array();
		for (auto id : g_everFollowed) {
			followed.push_back(id);
		}
		for (auto& [id, b] : g_bonds) {
			if (b.points == 0 && b.days.empty()) {
				continue;  // a word or two today, nothing earned yet
			}
			out.push_back({ { "npc", id }, { "points", b.points }, { "kinds", b.kinds }, { "days", std::vector<int>(b.days.begin(), b.days.end()) },
				{ "today", b.today }, { "todayPoints", b.todayPoints }, { "todayKinds", b.todayKinds } });
		}
		return json{ { "bonds", out }, { "followed", followed } };
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		std::scoped_lock l(g_lock);
		g_bonds.clear();
		g_everFollowed.clear();
		g_place = 0;
		if (j.is_object() && j.contains("followed") && j["followed"].is_array()) {
			for (auto& f : j["followed"]) {
				if (f.is_number()) {
					if (auto id = resolve(f.get<RE::FormID>())) {
						g_everFollowed.insert(id);
					}
				}
			}
		}
		const json list = j.is_object() && j.contains("bonds") && j["bonds"].is_array() ? j["bonds"] : j.is_array() ? j : json::array();
		for (auto& b : list) {
			const auto raw = static_cast<RE::FormID>(JsonUtil::Num(b, "npc").value_or(0));
			const auto id = raw ? resolve(raw) : 0;
			if (!id) {
				continue;
			}
			Bond bond;
			bond.points = static_cast<int>(JsonUtil::Num(b, "points").value_or(0));
			bond.kinds = static_cast<std::uint32_t>(JsonUtil::Num(b, "kinds").value_or(0));
			if (b.contains("days") && b["days"].is_array()) {
				for (auto& d : b["days"]) {
					if (d.is_number()) {
						bond.days.insert(d.get<int>());
					}
				}
			}
			bond.today = static_cast<int>(JsonUtil::Num(b, "today").value_or(-1));
			bond.todayPoints = static_cast<int>(JsonUtil::Num(b, "todayPoints").value_or(0));
			bond.todayKinds = static_cast<std::uint32_t>(JsonUtil::Num(b, "todayKinds").value_or(0));
			g_bonds[id] = std::move(bond);
		}
		logger::info("co-save: {} bonds in progress", g_bonds.size());
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_bonds.clear();
		g_everFollowed.clear();
		g_place = 0;
	}
}
