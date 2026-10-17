#include "Rest.h"

#include "Bonds.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Ledger.h"
#include "Moments.h"
#include "Papyrus.h"
#include "Promises.h"
#include "Services.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Rest
{
	namespace
	{
		constexpr float kCompanionRange = 1500.0f;  // companions this close come to bed too
		constexpr float kBedRange = 2000.0f;        // outdoors: beds and bedrolls this close count
		constexpr float kGiveUpHours = 2.0f;        // the player never went to sleep: they get up
		constexpr float kLeaveDistance = 3000.0f;   // the player walked off
		constexpr float kBedrollLife = 12.0f;       // a spawned bedroll is packed away after this
		constexpr float kBedrollSpacing = 120.0f;

		// Where the player is decides where anyone may sleep.
		enum class Place
		{
			kNpcHome,     // the NPC's own home
			kHostHome,    // the home of someone who welcomed the player (guest rights, a lent bed)
			kPlayerHome,  // a house of the player's
			kInnRoom,     // an inn, room rented
			kInnNoRoom,   // an inn, no room yet
			kTown,        // city, town or village streets
			kOthersHome,  // somebody else's house, uninvited
			kNoPlace,     // shops, temples, palaces, jails, barracks
			kWild         // wilderness, camps, caves and ruins
		};

		struct Sleeper
		{
			RE::FormID npc = 0;
			RE::FormID bed = 0;
		};

		struct Session
		{
			bool active = false;
			float started = 0.0f;
			bool slept = false;  // the player has been asleep since
			RE::FormID cell = 0;
			RE::NiPoint3 anchor{};
			std::vector<Sleeper> sleepers;
		};

		struct Bedroll
		{
			RE::FormID ref = 0;
			float placed = 0.0f;
		};

		std::mutex g_lock;
		Session g_session;
		std::vector<Bedroll> g_bedrolls;

		using L = RE::BGSRelationship::RELATIONSHIP_LEVEL;

		L LevelOf(RE::Actor* npc)
		{
			auto* base = npc ? npc->GetActorBase() : nullptr;
			auto* playerBase = Game::Player()->GetActorBase();
			auto* rel = base && playerBase ? RE::BGSRelationship::GetRelationship(base, playerBase) : nullptr;
			return rel ? rel->level.get() : L::kAcquaintance;
		}

		int RankOf(RE::Actor* npc)
		{
			switch (LevelOf(npc)) {
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
			case L::kRival:
				return -1;
			case L::kFoe:
				return -2;
			case L::kEnemy:
				return -3;
			default:
				return -4;
			}
		}

		bool IsLover(RE::Actor* npc)
		{
			return LevelOf(npc) == L::kLover;
		}

		bool PlayerAsleep()
		{
			return Game::Player()->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping;
		}

		bool HasSeverActions()
		{
			static const bool has = RE::TESDataHandler::GetSingleton()->LookupModByName("SeverActions.esp") != nullptr;
			return has;
		}

		std::string Lower(const char* s)
		{
			return JsonUtil::Lower(s ? s : "");
		}

		std::string ModelOf(RE::TESBoundObject* base)
		{
			auto* model = base ? base->As<RE::TESModel>() : nullptr;
			return model ? Lower(model->GetModel()) : "";
		}

		bool IsDouble(RE::TESObjectREFR* bed)
		{
			return bed && ModelOf(bed->GetBaseObject()).find("double") != std::string::npos;
		}

		RE::TESForm* OwnerOf(RE::TESObjectREFR* ref)
		{
			auto* x = ref->extraList.GetByType<RE::ExtraOwnership>();
			return x ? x->owner : nullptr;
		}

		bool ChildOwned(RE::TESForm* owner)
		{
			auto* n = owner ? owner->As<RE::TESNPC>() : nullptr;
			return n && n->race && n->race->data.flags.any(RE::RACE_DATA::Flag::kChild);
		}

		// The location itself only: a house inside Whiterun is not "the city".
		bool LocHas(RE::BGSLocation* loc, const char* kw)
		{
			return loc && loc->HasKeywordString(kw);
		}

		// The location or any above it: the streets of Whiterun sit under the city.
		bool LocOrParentHas(RE::BGSLocation* loc, const char* kw)
		{
			for (int i = 0; loc && i < 8; loc = loc->parentLoc, ++i) {
				if (loc->HasKeywordString(kw)) {
					return true;
				}
			}
			return false;
		}

		std::vector<RE::Actor*> NearbyActors(RE::TESObjectCELL* cell)
		{
			std::vector<RE::Actor*> out;
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					auto a = h.get();
					if (a && !a->IsDead() && (!cell || a->GetParentCell() == cell)) {
						out.push_back(a.get());
					}
				}
			}
			return out;
		}

		template <class F>
		void ForEachNearbyRef(F&& fn)
		{
			auto* player = Game::Player();
			auto visit = [&](RE::TESObjectREFR* ref) {
				if (ref) {
					fn(ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			};
			auto* cell = player->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				cell->ForEachReference(visit);
			} else if (auto* tes = RE::TES::GetSingleton()) {
				tes->ForEachReferenceInRange(player, kBedRange, visit);
			}
		}

		bool IsSleepFurniture(RE::TESObjectREFR* ref)
		{
			auto* base = ref->GetBaseObject();
			auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
			return furn && furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) && !ref->IsDisabled() && !ref->IsDeleted();
		}

		struct Where
		{
			Place place = Place::kWild;
			RE::Actor* host = nullptr;               // kNpcHome / kHostHome
			RE::TESObjectREFR* roomBed = nullptr;    // kInnRoom
			RE::TESObjectREFR* playerBed = nullptr;  // a bed of the player's here already (theirs, rented or lent)
		};

		Where Classify(RE::Actor* npc)
		{
			Where w;
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			auto* loc = player->GetCurrentLocation();
			auto* playerBase = player->GetActorBase();
			if (!cell) {
				w.place = Place::kNoPlace;
				return w;
			}
			if (!cell->IsInteriorCell()) {
				w.place = LocOrParentHas(loc, "LocTypeCity") || LocOrParentHas(loc, "LocTypeTown") || LocOrParentHas(loc, "LocTypeSettlement")
				              ? Place::kTown
				              : Place::kWild;
				return w;
			}
			// A bed here that is already the player's: their own, the rented room, a lent one.
			float nearest = (std::numeric_limits<float>::max)();
			ForEachNearbyRef([&](RE::TESObjectREFR* ref) {
				if (IsSleepFurniture(ref) && OwnerOf(ref) == playerBase) {
					const float d = ref->GetPosition().GetDistance(player->GetPosition());
					if (d < nearest) {
						nearest = d;
						w.playerBed = ref;
					}
				}
			});
			if (Promises::IsHome(cell, npc)) {
				w.place = Place::kNpcHome;
				w.host = npc;
				return w;
			}
			if (cell->GetOwner() == playerBase || LocHas(loc, "LocTypePlayerHouse")) {
				w.place = Place::kPlayerHome;
				return w;
			}
			bool inn = LocHas(loc, "LocTypeInn");
			for (auto* a : NearbyActors(cell)) {
				if (auto room = Services::RoomOf(a)) {
					inn = true;
					if (room->rentedByPlayer && room->bed) {
						w.roomBed = room->bed;
					}
				}
			}
			if (inn) {
				w.place = w.roomBed ? Place::kInnRoom : Place::kInnNoRoom;
				return w;
			}
			for (auto* a : NearbyActors(cell)) {
				if (Ledger::IsGuest(a->GetFormID()) && Promises::IsHome(cell, a)) {
					w.place = Place::kHostHome;
					w.host = a;
					return w;
				}
			}
			if (w.playerBed) {
				w.place = Place::kHostHome;  // a bed lent to the player here
				return w;
			}
			for (auto* kw : { "LocTypeStore", "LocTypeTemple", "LocTypeCastle", "LocTypeJail", "LocTypeBarracks", "LocTypeGuild" }) {
				if (LocHas(loc, kw)) {
					w.place = Place::kNoPlace;
					return w;
				}
			}
			if (cell->GetOwner() || LocHas(loc, "LocTypeDwelling") || LocHas(loc, "LocTypeHouse")) {
				w.place = Place::kOthersHome;
				return w;
			}
			w.place = Place::kWild;  // caves, ruins, forts, an empty shack
			return w;
		}

		// Free beds the place lets these people use, nearest to the player first.
		std::vector<RE::TESObjectREFR*> FreeBeds(const Where& w)
		{
			auto* player = Game::Player();
			auto* playerBase = player->GetActorBase();
			std::unordered_set<RE::TESObjectREFR*> taken;
			auto addTaken = [&](RE::Actor* a) {
				if (auto f = a ? a->GetOccupiedFurniture().get() : nullptr) {
					taken.insert(f.get());
				}
			};
			addTaken(player);
			for (auto* a : NearbyActors(nullptr)) {
				addTaken(a);
			}
			std::vector<RE::TESObjectREFR*> beds;
			ForEachNearbyRef([&](RE::TESObjectREFR* ref) {
				if (!IsSleepFurniture(ref) || taken.contains(ref)) {
					return;
				}
				auto* owner = OwnerOf(ref);
				if (ChildOwned(owner)) {
					return;
				}
				bool ok = false;
				switch (w.place) {
				case Place::kPlayerHome:
					ok = true;
					break;
				case Place::kInnRoom:
					ok = ref == w.roomBed || !owner || owner == playerBase;
					break;
				case Place::kNpcHome:
				case Place::kHostHome:
					ok = !owner || owner == playerBase || (w.host && Promises::Belongs(owner, w.host));
					break;
				case Place::kWild:
					ok = !owner || owner == playerBase;
					break;
				default:
					break;
				}
				if (ok) {
					beds.push_back(ref);
				}
			});
			std::ranges::sort(beds, [&](RE::TESObjectREFR* a, RE::TESObjectREFR* b) {
				return a->GetPosition().GetDistance(player->GetPosition()) < b->GetPosition().GetDistance(player->GetPosition());
			});
			return beds;
		}

		// A plain bedroll from the base game, found by its model (no plugin of ours needed).
		RE::TESFurniture* BedrollBase()
		{
			static RE::TESFurniture* cached = nullptr;
			static bool searched = false;
			if (searched) {
				return cached;
			}
			searched = true;
			// The ordinary bedroll the game itself uses: Bedroll01F (Skyrim.esm 03B3CD).
			if (auto* plain = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESFurniture>(0x03B3CD, "Skyrim.esm");
				plain && plain->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep)) {
				cached = plain;
				logger::info("rest: bedroll base {:08X} (Bedroll01F)", cached->GetFormID());
				return cached;
			}
			// Else any cloth bedroll (Bedroll01.nif). Never the hay pile (bedrollhay01), nor
			// BedrollGround.nif: that one is an invisible lie-down marker for scenes (DA16LayDownMarker01F).
			int best = 0;
			for (auto* f : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESFurniture>()) {
				auto* file = f ? f->GetFile(0) : nullptr;
				if (!f || !file || std::string_view(file->GetFilename()) != "Skyrim.esm" ||
					!f->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep)) {
					continue;
				}
				const auto model = ModelOf(f);
				if (model.find("bedroll") == std::string::npos || model.find("hay") != std::string::npos || model.find("ground") != std::string::npos ||
					model.find("marker") != std::string::npos) {
					continue;
				}
				const int rank = model.find("bedroll01") != std::string::npos ? 2 : 1;
				if (rank > best) {
					best = rank;
					cached = f;
				}
			}
			logger::info("rest: bedroll base {}", cached ? std::format("{:08X}", cached->GetFormID()) : "not found");
			return cached;
		}

		bool CarriesBedroll(RE::Actor* npc)
		{
			auto counts = npc->GetInventoryCounts([](RE::TESBoundObject& o) {
				const auto name = Lower(o.GetName());
				return name.find("bedroll") != std::string::npos || name.find("bed roll") != std::string::npos;
			});
			return std::ranges::any_of(counts, [](auto& kv) { return kv.second > 0; });
		}

		// A bedroll at their feet, moved off the player and any other bedroll.
		RE::TESObjectREFR* SpawnBedroll(RE::Actor* sleeper, std::vector<RE::NiPoint3>& used)
		{
			auto* base = BedrollBase();
			if (!base) {
				return nullptr;
			}
			auto* player = Game::Player();
			RE::NiPoint3 at = sleeper->GetPosition();
			RE::NiPoint3 away = at - player->GetPosition();
			away.z = 0.0f;
			const float len = away.Length();
			away = len > 1.0f ? away / len : RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
			auto tooClose = [&](const RE::NiPoint3& p) {
				if (RE::NiPoint3{ p.x - player->GetPositionX(), p.y - player->GetPositionY(), 0.0f }.Length() < kBedrollSpacing) {
					return true;
				}
				return std::ranges::any_of(used, [&](const RE::NiPoint3& u) {
					return RE::NiPoint3{ p.x - u.x, p.y - u.y, 0.0f }.Length() < kBedrollSpacing;
				});
			};
			for (int i = 0; i < 4 && tooClose(at); ++i) {
				at += away * kBedrollSpacing;
			}
			auto ref = sleeper->PlaceObjectAtMe(base, false);
			if (!ref) {
				return nullptr;
			}
			ref->SetPosition(at);
			ref->SetAngle({ 0.0f, 0.0f, sleeper->GetAngleZ() });
			used.push_back(at);
			{
				std::scoped_lock l(g_lock);
				g_bedrolls.push_back({ ref->GetFormID(), Game::Hours() });
			}
			logger::info("rest: bedroll {:08X} laid for {}", ref->GetFormID(), sleeper->GetName());
			return ref.get();
		}

		// Packed already: the game takes a moment to remove it, and it's never packed (or counted) twice.
		std::unordered_set<RE::FormID> g_packed;  // game thread

		void PackAway(RE::FormID id)
		{
			if (!g_packed.insert(id).second) {
				return;
			}
			if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id)) {
				Papyrus::CallMethod(ref, "ObjectReference", "Disable", nullptr, false);
				Papyrus::CallMethod(ref, "ObjectReference", "Delete", nullptr);
				logger::info("rest: bedroll {:08X} packed away", id);
			}
		}

		// Into bed. SeverActions' furniture package walks them there and keeps them lying down
		// (and stands them up if the player walks away); without it, walk there and lie down.
		void SendToBed(RE::Actor* a, RE::TESObjectREFR* bed)
		{
			Moments::Release(a->GetFormID());  // a drink or a sit won't stand them up out of bed later
			if (HasSeverActions() &&
				Papyrus::CallStatic("SeverActions_FurnitureLib", "UseRef", nullptr, static_cast<RE::Actor*>(a), static_cast<RE::TESObjectREFR*>(bed))) {
				return;
			}
			const auto id = a->GetFormID();
			const auto bedId = bed->GetFormID();
			const bool walking = Papyrus::CallMethod(a, "Actor", "PathToReference",
				[id, bedId](const RE::BSScript::Variable&) {
					SKSE::GetTaskInterface()->AddTask([id, bedId] {
						auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
						auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(bedId);
						if (a && bed && bed->GetPosition().GetDistance(a->GetPosition()) < 400.0f) {
							Papyrus::CallMethod(bed, "ObjectReference", "Activate", nullptr, static_cast<RE::TESObjectREFR*>(a), false);
						}
					});
				},
				static_cast<RE::TESObjectREFR*>(bed), 1.0f);
			if (!walking) {
				Papyrus::CallMethod(bed, "ObjectReference", "Activate", nullptr, static_cast<RE::TESObjectREFR*>(a), false);
			}
		}

		void StandUp(RE::Actor* a)
		{
			if (!a) {
				return;
			}
			if (HasSeverActions() && Papyrus::CallStatic("SeverActions_FurnitureLib", "Stop", nullptr, static_cast<RE::Actor*>(a))) {
				return;
			}
			Papyrus::CallMethod(a, "Actor", "EvaluatePackage", nullptr);
		}

		// Game thread. Everyone up; the rest is over.
		void End(const char* why, bool narrate)
		{
			Session s;
			{
				std::scoped_lock l(g_lock);
				if (!g_session.active) {
					return;
				}
				s = std::move(g_session);
				g_session = {};
			}
			RE::Actor* first = nullptr;
			const bool slept = s.slept || std::string_view(why) == "player woke";
			for (auto& sl : s.sleepers) {
				auto* a = RE::TESForm::LookupByID<RE::Actor>(sl.npc);
				if (a && !a->IsDead()) {
					StandUp(a);
					first = first ? first : a;
					if (slept) {
						Bonds::Add(a, Bonds::Deed::kRest, 2);
					}
				}
			}
			logger::info("rest: over ({}), {} up again", why, s.sleepers.size());
			if (narrate && first && Game::DistanceToPlayer(first) < kLeaveDistance) {
				Game::Narrate(std::format("{} stirred and got up as {} woke.", first->GetName(), Game::Player()->GetName()), first);
			}
		}

		struct SleepSink : RE::BSTEventSink<RE::TESSleepStopEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent*, RE::BSTEventSource<RE::TESSleepStopEvent>*) override
			{
				SKSE::GetTaskInterface()->AddTask([] {
					Bonds::OnPlayerSlept();
					End("player woke", true);
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		std::string Join(const std::vector<std::string>& parts)
		{
			std::string out;
			for (std::size_t i = 0; i < parts.size(); ++i) {
				out += i == 0 ? "" : (i + 1 == parts.size() ? " and " : ", ");
				out += parts[i];
			}
			return out;
		}

		// Game thread. Says no in character (SkyrimNet makes the NPC answer it) and shows why.
		bool Decline(RE::Actor* npc, const std::string& why, const std::string& note)
		{
			Game::Notify(std::format("{} ({}): {}", npc->GetName(), RelationName(npc), note));
			Game::Narrate(why, npc);
			logger::info("rest: {} declined ({})", npc->GetName(), note);
			return true;
		}
	}

	std::string RelationName(RE::Actor* npc)
	{
		if (!npc) {
			return "";
		}
		switch (LevelOf(npc)) {
		case L::kLover:
			return "Lover";
		case L::kAlly:
			return "Ally";
		case L::kConfidant:
			return "Confidant";
		case L::kFriend:
			return "Friend";
		case L::kRival:
			return "Rival";
		case L::kFoe:
			return "Foe";
		case L::kEnemy:
			return "Enemy";
		case L::kArchnemesis:
			return "Archnemesis";
		default:
			return Services::IsCompanion(npc) ? "Companion" : "Acquaintance";
		}
	}

	bool Start(RE::Actor* npc)
	{
		if (!npc) {
			return false;
		}
		auto* player = Game::Player();
		const std::string playerName = player->GetName();
		const std::string name = npc->GetName();

		// Staying at their place already: what was agreed decides who sleeps where (a bedroll, their bed,
		// sharing). Going to bed is then just going to bed - nothing to refuse, nothing to re-lend; the
		// host lies down in their own place when the player does (Ledger).
		if (const auto stay = Ledger::StayAt(npc->GetFormID()); !stay.empty()) {
			Game::Notify(stay == "player_floor" ? std::string("Your bedroll is ready. Sleep well.")
			             : stay == "packed"     ? std::string("Your bedroll is rolled up: say where to lay it.")
			             : stay == "share"      ? std::format("{} will lie down with you when you turn in.", name)
			                                    : std::format("{}'s bed is yours tonight.", name));
			logger::info("rest: staying at {}'s ({}): sleeping as agreed", name, stay);
			return true;
		}

		// The real relationship decides, not a few warm words: companions and friends only.
		if (!Services::IsCompanion(npc) && !Promises::IsClose(npc)) {
			// They said yes, though: the bed is the player's to use (a loan needs no closeness), only
			// not with them in it.
			const bool hadBed = Ledger::HasBedLoan(npc->GetFormID());
			Promises::Promise bed;
			bed.type = "offer_bed";
			const auto outcome = hadBed ? Promises::Outcome::kKept : Promises::Keep(npc, bed);
			// Not at their place yet (out in the street): the bed is theirs to lend once the player is there.
			if (outcome == Promises::Outcome::kNotYet) {
				if (!Ledger::IsPending(npc->GetFormID(), bed)) {
					Ledger::AddPending(npc->GetFormID(), name, bed);
				}
				Game::Notify(std::format("{}'s bed will be yours to sleep in at their place.", name));
				logger::info("rest: {} will lend the bed once the player is at their place ({})", name, RelationName(npc));
				return true;
			}
			if (outcome == Promises::Outcome::kKept) {
				if (hadBed) {
					Game::Notify(std::format("{}'s bed is yours to sleep in.", name));
				}
				Game::Notify(std::format("{} ({}): won't lie down beside you yet", name, RelationName(npc)));
				Game::Narrate(std::format("{} let {} have the bed to rest in, but didn't lie down beside them; they barely knew each other yet.", name, playerName), npc);
				logger::info("rest: {} lends the bed but won't rest with the player ({})", name, RelationName(npc));
				return true;
			}
			return Decline(npc, std::format("{} barely knew {} ({} at most), and wasn't about to bed down with them.", name, playerName, Lower(RelationName(npc).c_str())),
				"won't rest with you");
		}
		if (player->IsInCombat()) {
			return Decline(npc, std::format("{} pointed out it was no time to sleep, not with danger about.", name), "not while there's danger");
		}

		const auto where = Classify(npc);
		switch (where.place) {
		case Place::kTown:
			{
				const auto inn = Services::NearestOfKind(npc, "inn");
				const auto town = Services::WhereIs(npc);
				return Decline(npc,
					inn.empty() ? std::format("{} said they couldn't bed down in the middle of {}; they'd need a proper bed indoors.", name, town)
								: std::format("{} said they couldn't bed down in the middle of {}, and suggested {} for the night.", name, town, inn),
					inn.empty() ? "not on the streets" : std::format("not on the streets; try {}", inn));
			}
		case Place::kInnNoRoom:
			return Decline(npc, std::format("{} reminded {} they'd need to rent a room before they could turn in.", name, playerName), "you need a room first");
		case Place::kOthersHome:
			return Decline(npc, std::format("{} pointed out this wasn't their home to sleep in.", name), "not someone else's home");
		case Place::kNoPlace:
			return Decline(npc, std::format("{} said this was no place to sleep.", name), "no place to sleep");
		default:
			break;
		}

		// Who sleeps: the one asked, and the companions close by. A lover first, then the closest.
		std::vector<RE::Actor*> sleepers{ npc };
		for (auto* a : NearbyActors(nullptr)) {
			if (a != npc && Services::IsCompanion(a) && !a->IsInCombat() && Game::DistanceToPlayer(a) < kCompanionRange) {
				sleepers.push_back(a);
			}
		}
		std::ranges::stable_sort(sleepers, [](RE::Actor* a, RE::Actor* b) { return RankOf(a) > RankOf(b); });

		const bool bedrolls = SkyrimNet::ConfigBool("features.rest_bedrolls", true);
		auto beds = FreeBeds(where);
		auto takeBed = [&](RE::TESObjectREFR* bed) { std::erase(beds, bed); };

		// The player's bed: the rented room, their own bed, or a bed lent here. In someone's home
		// without one, a spare bed of the household is lent for the stay (the host keeps their own).
		RE::TESObjectREFR* playerBed = where.place == Place::kInnRoom ? where.roomBed : where.playerBed;
		auto* host = where.host;
		const bool hostIsLover = host && IsLover(host) && std::ranges::find(sleepers, host) != sleepers.end();
		if (host && hostIsLover) {
			// A lover shares their own bed with the player.
			for (auto* b : beds) {
				if (Promises::IsTheirs(OwnerOf(b), host)) {
					playerBed = b;
					Ledger::LoanBed(host, b, 0.0f, true);
					break;
				}
			}
		} else if (host && !playerBed) {
			for (auto* b : beds) {
				if (!Promises::IsTheirs(OwnerOf(b), host)) {
					playerBed = b;
					Ledger::LoanBed(host, b, 0.0f, true);
					break;
				}
			}
		}
		// Outdoors, the bed or bedroll right by the player (their own Campfire bedroll, a tent) is theirs.
		if (!playerBed && where.place == Place::kWild && !beds.empty() &&
			beds.front()->GetPosition().GetDistance(player->GetPosition()) < 400.0f) {
			playerBed = beds.front();
		}
		if (playerBed) {
			takeBed(playerBed);
		}

		std::vector<RE::NiPoint3> used;
		if (playerBed) {
			used.push_back(playerBed->GetPosition());
		}
		std::vector<std::string> told;    // for the narration
		std::vector<std::string> notes;   // for the notification
		Session session{ true, Game::Hours(), false, player->GetParentCell() ? player->GetParentCell()->GetFormID() : 0, player->GetPosition(), {} };
		bool sharedPlayerBed = false;

		for (auto* a : sleepers) {
			const std::string who = a->GetName();
			RE::TESObjectREFR* bed = nullptr;
			std::string what;  // narration
			std::string note;  // notification
			if (!sharedPlayerBed && playerBed && IsLover(a) && (IsDouble(playerBed) || a == host)) {
				// Only a lover shares the player's bed: a double one, or their own at home.
				bed = playerBed;
				sharedPlayerBed = true;
				what = std::format("lay down beside {} in the same bed", playerName);
				note = "shares your bed";
			} else if (a == host) {
				for (auto* b : beds) {
					if (Promises::IsTheirs(OwnerOf(b), a)) {
						bed = b;
						break;
					}
				}
				what = "went to their own bed";
				note = "their own bed";
			}
			if (!bed) {
				// A free bed; never someone's personal bed (the host's stays the host's).
				for (auto* b : beds) {
					auto* owner = OwnerOf(b);
					if (owner && owner->As<RE::TESNPC>() && !Promises::IsTheirs(owner, a)) {
						continue;
					}
					bed = b;
					break;
				}
				what = "took a free bed nearby";
				note = "a bed nearby";
			}
			if (!bed && bedrolls && (Services::IsCompanion(a) || CarriesBedroll(a))) {
				bed = SpawnBedroll(a, used);
				what = where.place == Place::kWild ? std::format("unrolled a bedroll beside {}", playerName) : "laid a bedroll on the floor";
				note = "bedroll beside you";
			}
			if (bed && bed != playerBed) {
				takeBed(bed);
			}
			if (!bed) {
				what = where.place == Place::kWild ? "had no bedroll and settled down by the fire" : "found a spot to doze where they were";
				note = "no bed, dozes nearby";
			}
			told.push_back(std::format("{} {}", who, what));
			notes.push_back(std::format("{} ({}): {}", who, RelationName(a), note));
			if (bed) {
				session.sleepers.push_back({ a->GetFormID(), bed->GetFormID() });
				SendToBed(a, bed);
			}
		}

		if (session.sleepers.empty() && told.empty()) {
			return false;
		}
		{
			std::scoped_lock l(g_lock);
			g_session = session;
		}
		for (auto& n : notes) {
			Game::Notify(n);
		}
		logger::info("rest: {} ({} sleepers, player bed {})", Join(told), session.sleepers.size(),
			playerBed ? std::format("{:08X}", playerBed->GetFormID()) : "none");
		Game::Narrate(std::format("{} agreed it was time to rest: {}.", name, Join(told)), npc);
		return true;
	}

	void Tick()
	{
		const float now = Game::Hours();
		const bool asleep = PlayerAsleep();
		bool end = false;
		const char* why = "";
		{
			std::scoped_lock l(g_lock);
			if (g_session.active) {
				auto* player = Game::Player();
				auto* cell = player->GetParentCell();
				const bool moved = !cell || cell->GetFormID() != g_session.cell ||
				                   player->GetPosition().GetDistance(g_session.anchor) > kLeaveDistance;
				if (asleep) {
					g_session.slept = true;
				} else if (g_session.slept) {
					end = true, why = "player woke";
				} else if (now - g_session.started > kGiveUpHours) {
					end = true, why = "the player never slept";
				} else if (moved) {
					end = true, why = "the player left";
				}
			}
		}
		if (end) {
			End(why, false);
		}
		if (asleep) {
			return;  // never pack a bedroll away from under anyone
		}
		std::vector<RE::FormID> pack;
		{
			std::scoped_lock l(g_lock);
			if (g_session.active) {
				return;
			}
			std::erase_if(g_bedrolls, [&](const Bedroll& b) {
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.ref);
				if (!ref) {
					return true;
				}
				if (Game::DistanceToPlayer(ref) > kLeaveDistance || now - b.placed > kBedrollLife) {
					pack.push_back(b.ref);
					return true;
				}
				return false;
			});
		}
		for (auto id : pack) {
			PackAway(id);
		}
	}

	RE::TESObjectREFR* LayBedroll(const RE::NiPoint3& at, float angle)
	{
		auto* base = BedrollBase();
		if (!base) {
			return nullptr;
		}
		auto ref = Game::Player()->PlaceObjectAtMe(base, false);
		if (!ref) {
			return nullptr;
		}
		ref->SetPosition(at);
		ref->SetAngle({ 0.0f, 0.0f, angle });
		logger::info("rest: bedroll {:08X} laid in a host's home", ref->GetFormID());
		return ref.get();
	}

	std::vector<RE::TESObjectREFR*> BedrollsIn(RE::TESObjectCELL* cell)
	{
		std::vector<RE::TESObjectREFR*> out;
		auto* base = BedrollBase();
		if (!cell || !base) {
			return out;
		}
		std::unordered_set<RE::FormID> ours;
		{
			std::scoped_lock l(g_lock);
			for (auto& b : g_bedrolls) {
				ours.insert(b.ref);
			}
		}
		cell->ForEachReference([&](RE::TESObjectREFR* ref) {
			if (ref && ref->GetBaseObject() == base && ref->IsDynamicForm() && !ref->IsDisabled() && !ref->IsMarkedForDeletion() &&
				!ours.contains(ref->GetFormID()) && !g_packed.contains(ref->GetFormID())) {
				out.push_back(ref);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		std::ranges::sort(out, {}, [](RE::TESObjectREFR* r) { return r->GetFormID(); });
		return out;
	}

	void PackBedroll(RE::FormID ref)
	{
		PackAway(ref);
	}

	void LieDown(RE::Actor* a, RE::TESObjectREFR* bed)
	{
		if (a && bed) {
			SendToBed(a, bed);
		}
	}

	void GetUp(RE::Actor* a)
	{
		StandUp(a);
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json j{ { "bedrolls", json::array() } };
		for (auto& b : g_bedrolls) {
			j["bedrolls"].push_back({ { "ref", b.ref }, { "placed", b.placed } });
		}
		if (g_session.active) {
			json s{ { "started", g_session.started }, { "slept", g_session.slept }, { "cell", g_session.cell },
				{ "x", g_session.anchor.x }, { "y", g_session.anchor.y }, { "z", g_session.anchor.z }, { "sleepers", json::array() } };
			for (auto& sl : g_session.sleepers) {
				s["sleepers"].push_back({ { "npc", sl.npc }, { "bed", sl.bed } });
			}
			j["session"] = s;
		}
		return j;
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		// Spawned references (FF......) are not remapped by load order: keep them as they are.
		auto id = [&](const json& o, const char* k) {
			const auto raw = static_cast<RE::FormID>(JsonUtil::Num(o, k).value_or(0));
			if (raw == 0) {
				return RE::FormID{ 0 };
			}
			if ((raw >> 24) == 0xFF) {
				return raw;
			}
			return resolve(raw);
		};
		std::scoped_lock l(g_lock);
		g_bedrolls.clear();
		g_session = {};
		if (!j.is_object()) {
			return;
		}
		if (j.contains("bedrolls") && j["bedrolls"].is_array()) {
			for (auto& b : j["bedrolls"]) {
				if (auto ref = id(b, "ref")) {
					g_bedrolls.push_back({ ref, static_cast<float>(JsonUtil::Num(b, "placed").value_or(0)) });
				}
			}
		}
		if (j.contains("session") && j["session"].is_object()) {
			auto& s = j["session"];
			g_session.active = true;
			g_session.started = static_cast<float>(JsonUtil::Num(s, "started").value_or(0));
			g_session.slept = s.contains("slept") && s["slept"].is_boolean() && s["slept"].get<bool>();
			g_session.cell = id(s, "cell");
			g_session.anchor = { static_cast<float>(JsonUtil::Num(s, "x").value_or(0)), static_cast<float>(JsonUtil::Num(s, "y").value_or(0)),
				static_cast<float>(JsonUtil::Num(s, "z").value_or(0)) };
			if (s.contains("sleepers") && s["sleepers"].is_array()) {
				for (auto& sl : s["sleepers"]) {
					g_session.sleepers.push_back({ id(sl, "npc"), id(sl, "bed") });
				}
			}
		}
		logger::info("co-save: {} bedrolls, rest {}", g_bedrolls.size(), g_session.active ? "in progress" : "none");
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_bedrolls.clear();
		g_session = {};
		g_packed.clear();  // another save: the same ids may be new bedrolls
	}

	void Install()
	{
		static SleepSink sink;
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESSleepStopEvent>(&sink);
		}
	}
}
