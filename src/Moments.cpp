#include "Moments.h"

#include "Bonds.h"
#include "Confirm.h"
#include "Game.h"
#include "Keepsakes.h"
#include "JsonUtil.h"
#include "Papyrus.h"
#include "Promises.h"
#include "Rest.h"
#include "Ledger.h"
#include "Services.h"
#include "SkyrimNetBridge.h"
#include "Trade.h"

namespace MundaneMatters::Moments
{
	namespace
	{
		constexpr float kDrinkEvery = 6.0f;       // game hours between drinks with the same person
		constexpr auto kSitFor = std::chrono::seconds(120);
		constexpr float kStayNear = 1500.0f;
		constexpr std::int32_t kRoundFallback = 10;

		std::mutex g_lock;
		std::unordered_map<RE::FormID, float> g_lastDrink;

		// "A drink at the Bee and Barb?" agreed in the street: had once both are at a tavern.
		struct DrinkPlan
		{
			RE::FormID npc = 0;
			std::string said;
			std::chrono::steady_clock::time_point until{};
		};
		std::optional<DrinkPlan> g_drinkPlan;  // game thread only
		constexpr auto kDrinkPlanFor = std::chrono::minutes(20);

		// Who a drink is holding in a seat, and which hold it is (game thread). Getting them up
		// later only happens if nothing else took them over meanwhile (bed, a meal, following).
		std::unordered_map<RE::FormID, std::uint64_t> g_held;
		std::uint64_t g_nextHold = 1;

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

		std::string Lower(const char* s)
		{
			return JsonUtil::Lower(s ? s : "");
		}

		bool HasSeverActions()
		{
			static const bool has = RE::TESDataHandler::GetSingleton()->LookupModByName("SeverActions.esp") != nullptr;
			return has;
		}

		bool IsDrink(RE::TESBoundObject* obj)
		{
			auto* a = obj ? obj->As<RE::AlchemyItem>() : nullptr;
			if (!a || !a->IsFood() || a->IsPoison()) {
				return false;
			}
			const auto name = Lower(a->GetName());
			for (auto* w : { "ale", "mead", "wine", "beer", "sujamma", "flin", "brandy", "rum", "shein", "mazte", "matze", "cyrodilic", "bloodwine",
					 "surilie", "dragon's breath", "honningbrew", "black-briar", "spiced", "emberbrand" }) {
				if (name.find(w) != std::string::npos && name.find("skooma") == std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// The drink in `from` closest to what was asked for ("mead"), else the cheapest one.
		RE::TESBoundObject* DrinkIn(RE::TESObjectREFR* from, const std::string& said)
		{
			if (!from) {
				return nullptr;
			}
			RE::TESBoundObject* best = nullptr;
			double bestScore = -1.0;
			for (auto& [obj, count] : from->GetInventoryCounts()) {
				if (count <= 0 || !IsDrink(obj)) {
					continue;
				}
				const double s = Promises::NameScore(said, obj->GetName()) * 100.0 - obj->GetGoldValue() * 0.01;
				if (s > bestScore) {
					best = obj;
					bestScore = s;
				}
			}
			return best;
		}

		// A common drink of the land, for a home's own stores: what was asked for if it's ordinary,
		// else plain ale.
		RE::TESBoundObject* CommonDrink(const std::string& said)
		{
			RE::TESBoundObject* best = nullptr;
			double bestScore = -1.0;
			for (auto* a : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::AlchemyItem>()) {
				auto* file = a ? a->GetFile(0) : nullptr;
				if (!file || std::string_view(file->GetFilename()) != "Skyrim.esm" || !IsDrink(a) || a->GetGoldValue() > 30) {
					continue;
				}
				const auto name = Lower(a->GetName());
				const double s = Promises::NameScore(said, a->GetName()) * 100.0 + (name == "ale" ? 1.0 : 0.0);
				if (s > bestScore) {
					best = a;
					bestScore = s;
				}
			}
			return best;
		}

		enum class Place
		{
			kInn,
			kHome,       // theirs or the player's
			kElsewhere,  // a camp, a bench in town: anywhere with a seat
			kNotHere     // a temple, shop, jail, palace, barracks
		};

		// Where sitting on the floor fits: out of doors, a dungeon / cave / ruin / camp, the player's home,
		// the NPC's home, or a home the player is staying in. Not a tavern, a shop, someone else's house.
		bool FloorFits(RE::Actor* npc)
		{
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			if (!cell || !cell->IsInteriorCell()) {
				return true;
			}
			auto* loc = cell->GetLocation();
			if (Promises::IsHome(cell, npc) || (loc && loc->HasKeywordString("LocTypePlayerHouse")) || !Ledger::StayAt(npc->GetFormID()).empty()) {
				return true;
			}
			for (auto* l = player->GetCurrentLocation(); l; l = l->parentLoc) {
				for (auto* kw : { "LocTypeDungeon", "LocTypeCave", "LocTypeAnimalDen", "LocTypeBanditCamp", "LocTypeMine", "LocTypeNordicRuin",
						 "LocTypeDwarvenAutomatons", "LocTypeFalmerHive", "LocTypeForswornCamp", "LocTypeGiantCamp", "LocTypeMilitaryCamp",
						 "LocTypeClearable", "LocTypeShip" }) {
					if (l->HasKeywordString(kw)) {
						return true;
					}
				}
			}
			return false;
		}

		Place PlaceOf(RE::Actor* npc)
		{
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			bool inn = false;
			for (auto* loc = player->GetCurrentLocation(); loc; loc = loc->parentLoc) {
				for (auto* kw : { "LocTypeTemple", "LocTypeStore", "LocTypeJail", "LocTypeCastle", "LocTypeBarracks" }) {
					if (loc->HasKeywordString(kw) && cell && cell->IsInteriorCell()) {
						return Place::kNotHere;
					}
				}
				inn = inn || loc->HasKeywordString("LocTypeInn");
			}
			if (inn) {
				return Place::kInn;
			}
			if (cell && cell->IsInteriorCell()) {
				auto* loc = cell->GetLocation();
				if (Promises::IsHome(cell, npc) || (loc && loc->HasKeywordString("LocTypePlayerHouse"))) {
					return Place::kHome;
				}
			}
			return Place::kElsewhere;
		}

		// The innkeeper here, for the drinks and their price.
		RE::Actor* Innkeeper()
		{
			auto* cell = Game::Player()->GetParentCell();
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					auto a = h.get();
					if (a && !a->IsDead() && a->GetParentCell() == cell && Services::RoomOf(a.get())) {
						return a.get();
					}
				}
			}
			return nullptr;
		}

		RE::TESObjectREFR* StockOf(RE::Actor* merchant)
		{
			auto* f = merchant ? merchant->GetVendorFaction() : nullptr;
			return f && f->vendorData.merchantContainer ? f->vendorData.merchantContainer : merchant;
		}

		// Sit them down (SeverActions keeps them seated; vanilla Activate as a fallback), and get
		// them up again after a couple of minutes, or as soon as the player leaves or a fight starts.
		void SitAndDrink(RE::FormID npcId, RE::FormID seatId, std::string drink)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* seat = RE::TESForm::LookupByID<RE::TESObjectREFR>(seatId);
			if (!npc || !seat) {
				return;
			}
			const bool held = HasSeverActions() &&
			                  Papyrus::CallStatic("SeverActions_FurnitureLib", "UseRef", nullptr, static_cast<RE::Actor*>(npc), static_cast<RE::TESObjectREFR*>(seat));
			if (!held) {
				Papyrus::CallMethod(npc, "Actor", "PathToReference",
					[npcId, seatId](const RE::BSScript::Variable&) {
						SKSE::GetTaskInterface()->AddTask([npcId, seatId] {
							auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
							auto* seat = RE::TESForm::LookupByID<RE::TESObjectREFR>(seatId);
							if (npc && seat && seat->GetPosition().GetDistance(npc->GetPosition()) < 400.0f) {
								Papyrus::CallMethod(seat, "ObjectReference", "Activate", nullptr, static_cast<RE::TESObjectREFR*>(npc), false);
							}
						});
					},
					static_cast<RE::TESObjectREFR*>(seat), 1.0f);
			}
			const auto ticket = g_nextHold++;
			g_held[npcId] = ticket;
			std::thread([npcId, held, drink, ticket] {
				const auto until = std::chrono::steady_clock::now() + kSitFor;
				auto done = std::make_shared<std::atomic_int>(0);  // 0 still drinking, 1 finished, 2 cut short
				while (std::chrono::steady_clock::now() < until && *done == 0) {
					std::this_thread::sleep_for(3s);
					SKSE::GetTaskInterface()->AddTask([npcId, done] {
						auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
						if (!npc || npc->IsDead() || npc->IsInCombat() || Game::Player()->IsInCombat() || Game::DistanceToPlayer(npc) > kStayNear) {
							*done = 2;
						}
					});
				}
				const bool cut = *done == 2;
				SKSE::GetTaskInterface()->AddTask([npcId, held, drink, cut, ticket] {
					auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
					// Taken over by something else (bed, a meal, following): not ours to stand up.
					if (auto it = g_held.find(npcId); it == g_held.end() || it->second != ticket) {
						return;
					}
					g_held.erase(npcId);
					if (!npc) {
						return;
					}
					if (held) {
						Papyrus::CallStatic("SeverActions_FurnitureLib", "Stop", nullptr, static_cast<RE::Actor*>(npc));
					} else {
						Papyrus::CallMethod(npc, "Actor", "EvaluatePackage", nullptr);
					}
					logger::info("drink: {} gets up ({})", Game::NameOf(npc), cut ? "cut short" : "finished");
					if (!cut) {
						Keepsakes::OnMoment(npcId);
					}
					if (!cut && !npc->IsDead()) {
						Game::Mention(std::format("{} finished their {}, set the cup down and got up from the seat beside {}.", Game::NameOf(npc), drink, PlayerName()), npc);
					}
				});
			}).detach();
		}

		bool SittingWith(RE::FormID npc);  // below (sit a while)
		RE::TESObjectREFR* SitSpot();

		// The drink itself: the player gets theirs and drinks it (its own effects), they get theirs.
		void Pour(RE::FormID npcId, RE::FormID seatId, RE::TESBoundObject* playerDrink, RE::TESObjectREFR* playerFrom, RE::TESBoundObject* npcDrink,
			RE::TESObjectREFR* npcFrom, std::int32_t fee, RE::FormID payTo, const std::string& where)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* player = Game::Player();
			if (!npc || !playerDrink) {
				return;
			}
			if (fee > 0) {
				if (Game::ItemCount(player, Game::Gold()) < fee) {
					Game::Notify(std::format("You need {} gold for the round.", fee));
					Game::Narrate(std::format("{} reached for coin for the round but came up short.", PlayerName()), npc);
					return;
				}
				auto* till = RE::TESForm::LookupByID<RE::Actor>(payTo);
				player->RemoveItem(Game::Gold(), fee, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, till ? StockOf(till) : npc);
			}
			// The player's: handed over (unless it's already theirs), then drunk.
			if (playerFrom && playerFrom != player && Game::ItemCount(playerFrom, playerDrink) > 0) {
				playerFrom->RemoveItem(playerDrink, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
			} else if (playerFrom != player) {
				player->AddObjectToContainer(playerDrink, nullptr, 1, npc);  // the house's (or the bar's) own pour
			}
			if (Game::ItemCount(player, playerDrink) > 0) {
				RE::ActorEquipManager::GetSingleton()->EquipObject(player, playerDrink);
			}
			// Theirs: from where it came (the bar, their pack, or one of the player's).
			if (npcDrink && npcFrom && Game::ItemCount(npcFrom, npcDrink) > 0) {
				npcFrom->RemoveItem(npcDrink, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
			}
			{
				std::scoped_lock l(g_lock);
				g_lastDrink[npcId] = Game::Hours();
			}
			Bonds::Add(npc, Bonds::Deed::kDrink, 1);
			const std::string drink = playerDrink->GetName();
			Game::Notify(std::format("A {} with {}", drink, Game::NameOf(npc)));
			Game::Mention(std::format("{} sat down beside {} {} and they shared a {} together{}.", Game::NameOf(npc), PlayerName(), where, drink,
							  fee > 0 ? std::format(", {} having paid {} gold for the round", PlayerName(), fee) : ""),
				npc);
			logger::info("drink: {} and {} share {} ({}{})", Game::NameOf(npc), PlayerName(), drink, where, fee > 0 ? std::format(", {} gold", fee) : "");
			if (!SittingWith(npcId)) {
				SitAndDrink(npcId, seatId, drink);  // sitting together already: they stay as they are
			}
		}
	}

	bool Drink(RE::Actor* npc, const std::string& said)
	{
		if (!npc) {
			return false;
		}
		const std::string name = Game::NameOf(npc);
		auto* player = Game::Player();
		const auto id = npc->GetFormID();
		{
			std::scoped_lock l(g_lock);
			if (auto it = g_lastDrink.find(id); it != g_lastDrink.end() && Game::Hours() - it->second < kDrinkEvery) {
				Game::Narrate(std::format("{} laughed: they had only just had a drink with {}, one more would be too many.", name, PlayerName()), npc);
				return true;
			}
		}
		const auto place = PlaceOf(npc);
		if (place == Place::kNotHere) {
			Game::Narrate(std::format("{} shook their head: this was no place for drinking.", name), npc);
			return true;
		}
		// Following the player or spending time with them counts as close, whatever the relationship
		// (the user's call: out hunting together, an acquaintance is company enough).
		const bool close = Services::IsCompanion(npc) || Promises::IsClose(npc) || Services::InCompany(npc->GetFormID()) || Services::FollowsPlayer(npc);
		auto* cell = player->GetParentCell();
		const bool outdoors = cell && !cell->IsInteriorCell();
		// Out in the street, with someone they don't know well (or nothing to share): a drink at the
		// tavern, had once both are there. Agreed, so nobody is turned down.
		auto plan = [&] {
			g_drinkPlan = DrinkPlan{ id, said, std::chrono::steady_clock::now() + kDrinkPlanFor };
			Game::Notify(std::format("A drink with {} at the tavern: head there together.", name));
			Game::Mention(std::format("{} and {} agreed to have a drink together at the tavern, once they got there.", name, PlayerName()), npc);
			logger::info("drink: {} - out of doors; the drink waits for the tavern", name);
			return true;
		};
		if (!close && place != Place::kInn && outdoors) {
			return plan();
		}
		// Indoors, anywhere: they said yes, and the relationship doesn't overrule them.
		// Already sitting together: the drink comes to them, nobody moves seats.
		auto* seat = SittingWith(npc->GetFormID()) ? SitSpot() : Services::SeatFor(npc);
		if (!seat) {
			// No free chair for it: they sit down wherever there's room (the floor too), and drink there.
			Sit(npc, "", 0.0f);
			seat = SittingWith(npc->GetFormID()) ? SitSpot() : nullptr;
		}
		if (!seat) {
			Game::Narrate(std::format("{} looked around: there was nowhere to sit down for a drink.", name), npc);
			logger::info("drink: {} - nowhere to sit", name);
			return true;
		}

		// Where the drinks come from.
		RE::TESBoundObject *playerDrink = nullptr, *npcDrink = nullptr;
		RE::TESObjectREFR *playerFrom = nullptr, *npcFrom = nullptr;
		std::int32_t fee = 0;
		RE::FormID payTo = 0;
		std::string where = "at their seats";
		if (place == Place::kInn) {
			auto* keeper = Innkeeper();
			auto* stock = StockOf(keeper);
			playerDrink = DrinkIn(stock, said);
			if (!playerDrink) {
				playerDrink = CommonDrink(said);
			}
			npcDrink = playerDrink;
			playerFrom = npcFrom = stock;
			where = "at the tavern";
			if (!close) {
				fee = playerDrink ? (std::max)(2, Trade::BuyPrice(playerDrink)) * 2 : kRoundFallback;
				payTo = keeper ? keeper->GetFormID() : 0;
			}
		} else if (place == Place::kHome) {
			npcDrink = DrinkIn(npc, said);
			npcFrom = npc;
			playerDrink = npcDrink ? npcDrink : CommonDrink(said);  // the house's own stores
			where = "at home";
		} else {
			// Out here: their bottle, else one of the player's to share.
			npcDrink = DrinkIn(npc, said);
			npcFrom = npc;
			if (!npcDrink) {
				npcDrink = DrinkIn(player, said);
				npcFrom = player;
			}
			playerDrink = npcDrink;
			playerFrom = npcFrom;
			where = "for a while";
			if (!npcDrink) {
				if (outdoors) {
					return plan();  // nothing to share out here: the tavern, then
				}
				Game::Narrate(std::format("{} patted their pack: neither of them had anything to drink.", name), npc);
				return true;
			}
		}
		if (!playerDrink) {
			Game::Narrate(std::format("{} looked for something to pour, but there was nothing to drink.", name), npc);
			return true;
		}
		const auto seatId = seat->GetFormID();
		auto go = [id, seatId, playerDrink, playerFrom, npcDrink, npcFrom, fee, payTo, where] {
			Pour(id, seatId, playerDrink, playerFrom, npcDrink, npcFrom, fee, payTo, where);
		};
		// Buying the round: a popup when "Confirm Paid Services" is on. A friend's treat just happens.
		if (fee <= 0 || !SkyrimNet::ConfigBool("features.confirm", true) || !Confirm::Available()) {
			go();
			return true;
		}
		const std::string accept = Game::Pick({ "This round's on me.", "Two mugs, then. I'm buying.", "Let me get these." });
		const std::string decline = Game::Pick({ "On second thought, another time.", "Maybe later.", "Never mind." });
		Confirm::Offer offer;
		offer.title = "Buy a round";
		offer.npc = name;
		offer.sameAs = { "payment", "trade" };
		offer.detail = std::format("Two {}", playerDrink->GetName());
		offer.price = fee;
		offer.playerGold = Game::ItemCount(player, Game::Gold());
		offer.seconds = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f));
		offer.acceptLabel = accept;
		offer.declineLabel = decline;
		Confirm::Ask(offer, [id, go, accept, decline](Confirm::Choice choice) {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!npc || choice == Confirm::Choice::kTimeout) {
				return;
			}
			if (choice == Confirm::Choice::kDecline) {
				Game::Say(decline, npc, true);
				return;
			}
			Game::Say(accept, npc, false);
			go();
		});
		return true;
	}

	// ---------------------------------------------------------------- sit a while

	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr float kFireSearch = 1500.0f;
		constexpr float kNearFire = 600.0f;
		constexpr auto kWaitForPlayer = std::chrono::seconds(120);
		constexpr auto kStoodUp = std::chrono::seconds(4);
		constexpr auto kTogetherFor = std::chrono::seconds(180);  // real minutes sat together that count as time spent
		constexpr float kMaxSkip = 4.0f;

		enum class Where
		{
			kAuto,
			kBed,  // the edge of a bed
			kFire,
			kGround,
			kSeat
		};

		struct Session
		{
			RE::FormID npc = 0;
			RE::FormID spot = 0;                 // what they sit on
			std::vector<RE::FormID> placed;      // ground spots we put down (removed afterwards)
			RE::FormID fire = 0;
			std::string where;                   // "by the fire", "on the bench"
			float hours = 0.0f;                  // how long the player said ("till sunset"), 0 = not said
			bool close = false;                  // companion, friend or closer: time may pass
			bool lover = false;
			bool held = false;                   // SeverActions keeps them seated
			bool playerSeated = false;
			bool offered = false;
			bool counted = false;
			Clock::time_point started, seatedAt, stoodAt;
			float startHours = 0.0f;             // game hours when they sat down (time passed by waiting counts)
		};
		std::optional<Session> g_sit;   // game thread only
		bool g_quietSit = false;        // a sit the NPC starts by themselves: no narrated "no seat here"
		std::pair<RE::FormID, std::string> g_sitBio;  // for the bio, read off the game thread (g_lock)

		void SetBio(RE::FormID npc, std::string line)
		{
			std::scoped_lock l(g_lock);
			g_sitBio = { npc, std::move(line) };
		}

		bool SittingWith(RE::FormID npc)
		{
			return g_sit && g_sit->npc == npc;
		}

		RE::TESObjectREFR* SitSpot()
		{
			return g_sit ? RE::TESForm::LookupByID<RE::TESObjectREFR>(g_sit->spot) : nullptr;
		}
		int g_bonusDay = -1;  // the Rested bonus, at most once a day

		// Fire first, then a named seat ("the bench there"), then the ground; whole words, so "there"
		// isn't "here".
		Where WhereOf(const std::string& said)
		{
			const auto w = JsonUtil::Lower(said);
			{
				const auto bedWords = JsonUtil::Words(said);
				if (std::ranges::any_of(bedWords, [](const std::string& x) { return x == "bed" || x == "beds" || x == "mattress"; })) {
					return Where::kBed;
				}
			}
			for (auto* k : { "fire", "hearth", "flame", "warm" }) {
				if (w.find(k) != std::string::npos) {
					return Where::kFire;
				}
			}
			const auto words = JsonUtil::Words(said);
			auto any = [&](std::initializer_list<std::string_view> keys) {
				return std::ranges::any_of(words, [&](const std::string& x) { return std::ranges::find(keys, std::string_view(x)) != keys.end(); });
			};
			if (any({ "bench", "benches", "chair", "chairs", "table", "stool", "stools", "seat", "seats", "log", "logs", "bar" })) {
				return Where::kSeat;
			}
			// Not "here": "let's sit here" is this place, any seat in it.
			if (any({ "ground", "grass", "floor", "dirt", "rock", "rocks", "stone", "meadow", "hill" })) {
				return Where::kGround;
			}
			return Where::kAuto;
		}

		std::string ModelOf(RE::TESBoundObject* base)
		{
			auto* model = base ? base->As<RE::TESModel>() : nullptr;
			return Lower(model && model->GetModel() ? model->GetModel() : "");
		}

		template <class Fn>
		void AroundPlayer(float range, Fn&& fn)
		{
			auto* player = Game::Player();
			auto visit = [&](RE::TESObjectREFR* ref) {
				if (ref && !ref->IsDisabled() && !ref->IsDeleted() && ref->GetPosition().GetDistance(player->GetPosition()) < range) {
					fn(ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			};
			auto* cell = player->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				cell->ForEachReference(visit);
			} else if (auto* tes = RE::TES::GetSingleton()) {
				tes->ForEachReferenceInRange(player, range, visit);
			}
		}

		// A fire, hearth or campfire near the player (whatever mod placed it).
		RE::TESObjectREFR* NearestFire()
		{
			auto* player = Game::Player();
			RE::TESObjectREFR* best = nullptr;
			float bestDist = kFireSearch;
			AroundPlayer(kFireSearch, [&](RE::TESObjectREFR* ref) {
				auto* base = ref->GetBaseObject();
				if (!base || base->Is(RE::FormType::NPC) || base->Is(RE::FormType::Furniture)) {
					return;
				}
				const auto model = ModelOf(base);
				const auto name = Lower(base->GetName());
				bool fire = false;
				// The base game's burning fires are effects meshes too ("FXFireWithEmbersLogs01", "FXfireLogs").
				for (auto* k : { "campfire", "firepit", "fireplace", "hearth", "cookfire", "fire_pit", "camp_fire", "fxfire", "firewithembers",
						 "firelogs", "fireembers" }) {
					fire = fire || model.find(k) != std::string::npos || name.find(k) != std::string::npos;
				}
				if (fire) {
					if (const float d = ref->GetPosition().GetDistance(player->GetPosition()); d < bestDist) {
						bestDist = d;
						best = ref;
					}
				}
			});
			return best;
		}

		// Furniture someone is using right now (a seat, a bed someone sleeps in).
		std::unordered_set<RE::TESObjectREFR*> TakenFurniture()
		{
			std::unordered_set<RE::TESObjectREFR*> taken;
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					if (auto a = h.get()) {
						if (auto f = a->GetOccupiedFurniture().get()) {
							taken.insert(f.get());
						}
					}
				}
			}
			if (auto f = Game::Player()->GetOccupiedFurniture().get()) {
				taken.insert(f.get());
			}
			return taken;
		}

		// A free seat (no bed, work station or throne) closest to `at`, within `range`.
		RE::TESObjectREFR* SeatNear(const RE::NiPoint3& at, float range)
		{
			const auto taken = TakenFurniture();
			std::vector<std::pair<float, RE::TESObjectREFR*>> seats;
			AroundPlayer(kFireSearch + range, [&](RE::TESObjectREFR* ref) {
				auto* base = ref->GetBaseObject();
				auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
				if (!furn || !furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSit) || furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) ||
					furn->workBenchData.benchType.get() != RE::TESFurniture::WorkBenchData::BenchType::kNone || taken.contains(ref) ||
					Lower(base->GetName()).find("throne") != std::string::npos) {
					return;
				}
				if (const float d = ref->GetPosition().GetDistance(at); d < range) {
					seats.emplace_back(d, ref);
				}
			});
			std::ranges::sort(seats, {}, &std::pair<float, RE::TESObjectREFR*>::first);
			// Indoors, only a seat the player can see: the nearest one may be behind a wall, in another room.
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			const bool inside = cell && cell->IsInteriorCell();
			for (auto& [d, ref] : seats) {
				bool unused = false;
				const float howFar = ref->GetPosition().GetDistance(player->GetPosition());
				// Line of sight to small furniture can fail with it in plain view: a seat this close counts.
				if (!inside || howFar < 350.0f || player->HasLineOfSight(ref, unused)) {
					return ref;
				}
				logger::info("sit: seat {:08X} skipped - out of the player's sight ({:.0f} away)", ref->GetFormID(), howFar);
			}
			return nullptr;
		}

		// The base game's sit-on-the-ground spot (cross-legged).
		RE::TESFurniture* GroundSpot()
		{
			static RE::TESFurniture* cached = nullptr;
			static bool searched = false;
			if (!searched) {
				searched = true;
				for (auto* f : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESFurniture>()) {
					auto* file = f ? f->GetFile(0) : nullptr;
					if (file && std::string_view(file->GetFilename()) == "Skyrim.esm" && f->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSit) &&
						ModelOf(f).find("crossleg") != std::string::npos) {
						cached = f;
						break;
					}
				}
				logger::info("sit: ground spot {}", cached ? std::format("{:08X}", cached->GetFormID()) : "not found");
			}
			return cached;
		}

		// The base game's invisible chair (sitting at chair height on nothing visible): the edge of a bed.
		RE::TESFurniture* ChairSpot()
		{
			static RE::TESFurniture* cached = nullptr;
			static bool searched = false;
			if (!searched) {
				searched = true;
				for (auto* f : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESFurniture>()) {
					auto* file = f ? f->GetFile(0) : nullptr;
					// FurnitureMarker01Sit: the game's own invisible chair (FurnitureMarkerSit.nif).
					if (file && std::string_view(file->GetFilename()) == "Skyrim.esm" && f->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSit) &&
						(ModelOf(f).find("furnituremarkersit") != std::string::npos || ModelOf(f).find("invisiblesitmarker") != std::string::npos)) {
						cached = f;
						break;
					}
				}
				logger::info("sit: bed-edge spot {}", cached ? std::format("{:08X}", cached->GetFormID()) : "not found");
			}
			return cached;
		}

		struct BedEdge
		{
			RE::NiPoint3 at;    // where to sit, middle of the long side nearest the player
			RE::NiPoint3 face;  // looking away from the bed
			RE::NiPoint3 along;  // unit vector along that side (a second spot goes beside the first)
			RE::NiPoint3 in;     // unit vector into the bed, from that side
			float top = 0.0f;    // the mattress height (world z)
		};

		// A raised bed (not a bedroll or hay pile) near the player that nobody sleeps in now.
		std::optional<BedEdge> BedEdgeNear(float range)
		{
			const auto taken = TakenFurniture();
			const auto p = Game::Player()->GetPosition();
			RE::TESObjectREFR* best = nullptr;
			float bestDist = range;
			AroundPlayer(range, [&](RE::TESObjectREFR* ref) {
				auto* base = ref->GetBaseObject();
				auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
				if (!furn || !furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) || taken.contains(ref) || ref->IsDisabled()) {
					return;
				}
				const auto n = Lower(base->GetName()) + " " + ModelOf(base);
				if (n.find("bedroll") != std::string::npos || n.find("hay") != std::string::npos || n.find("coffin") != std::string::npos ||
					n.find("ground") != std::string::npos) {
					return;
				}
				// A bed the player can see (not one behind a wall, in the next room).
				bool unused = false;
				if (auto* cell = Game::Player()->GetParentCell(); cell && cell->IsInteriorCell() && !Game::Player()->HasLineOfSight(ref, unused)) {
					return;
				}
				if (const float d = ref->GetPosition().GetDistance(p); d < bestDist) {
					bestDist = d;
					best = ref;
				}
			});
			if (!best) {
				return std::nullopt;
			}
			const auto lo = best->GetBoundMin(), hi = best->GetBoundMax();
			const float sx = hi.x - lo.x, sy = hi.y - lo.y;
			if (sx < 40.0f || sy < 40.0f || hi.z - lo.z < 15.0f) {
				return std::nullopt;  // no real bounds: can't tell where the edge is
			}
			// Local -> world: +Y is the bed's forward, +X its right.
			const float a = best->GetAngleZ();
			const auto world = [&](float lx, float ly) {
				const auto o = best->GetPosition();
				return RE::NiPoint3{ o.x + lx * std::cos(a) + ly * std::sin(a), o.y - lx * std::sin(a) + ly * std::cos(a), o.z };
			};
			const float cx = (lo.x + hi.x) / 2.0f, cy = (lo.y + hi.y) / 2.0f;
			const bool longY = sy >= sx;
			// The two long sides; the one nearer the player. Sat a hand's width in from the edge.
			const float inset = 12.0f;
			RE::NiPoint3 sideA = longY ? world(hi.x - inset, cy) : world(cx, hi.y - inset);
			RE::NiPoint3 sideB = longY ? world(lo.x + inset, cy) : world(cx, lo.y + inset);
			const bool useA = sideA.GetDistance(p) <= sideB.GetDistance(p);
			BedEdge e;
			e.at = useA ? sideA : sideB;
			const auto mid = world(cx, cy);
			e.face = { e.at.x + (e.at.x - mid.x) * 5.0f, e.at.y + (e.at.y - mid.y) * 5.0f, e.at.z };
			const auto alongEnd = longY ? world(cx, hi.y) : world(hi.x, cy);
			const float len = std::hypot(alongEnd.x - mid.x, alongEnd.y - mid.y);
			e.along = len > 0.0f ? RE::NiPoint3{ (alongEnd.x - mid.x) / len, (alongEnd.y - mid.y) / len, 0.0f } : RE::NiPoint3{};
			const float inLen = std::hypot(mid.x - e.at.x, mid.y - e.at.y);
			e.in = inLen > 0.0f ? RE::NiPoint3{ (mid.x - e.at.x) / inLen, (mid.y - e.at.y) / inLen, 0.0f } : RE::NiPoint3{};
			// The bounds run up to the headboard: the mattress of a Skyrim bed is about 35-45 units up.
			e.top = best->GetPositionZ() + (std::min)(hi.z, 40.0f);
			logger::info("sit: bed edge on '{}' {:08X}", best->GetBaseObject()->GetName(), best->GetFormID());
			return e;
		}

		// The player's spot beside them is invisible (nothing to aim at), so the player is sat on it
		// once the NPC is down - unless they've sat somewhere else or walked off meanwhile.
		void SeatPlayerSoon(RE::FormID spot)
		{
			std::thread([spot] {
				std::this_thread::sleep_for(2500ms);
				SKSE::GetTaskInterface()->AddTask([spot] {
					auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(spot);
					auto* player = Game::Player();
					if (!ref || ref->IsDisabled() || player->GetOccupiedFurniture().get() || ref->GetPosition().GetDistance(player->GetPosition()) > 500.0f ||
						player->IsInCombat()) {
						return;
					}
					Papyrus::CallMethod(ref, "ObjectReference", "Activate", nullptr, static_cast<RE::TESObjectREFR*>(player), false);
					logger::info("sit: the player sits on the spot beside them");
				});
			}).detach();
		}

		// Puts a ground spot (or, with `chair`, an invisible chair) down at `pos`, facing `face`.
		RE::TESObjectREFR* PlaceSpot(const RE::NiPoint3& pos, const RE::NiPoint3& face, bool chair = false)
		{
			RE::TESBoundObject* base = chair ? static_cast<RE::TESBoundObject*>(ChairSpot()) : GroundSpot();
			if (!base) {
				return nullptr;
			}
			auto ref = Game::Player()->PlaceObjectAtMe(base, false);
			if (!ref) {
				return nullptr;
			}
			ref->SetPosition(pos);
			ref->SetAngle({ 0.0f, 0.0f, std::atan2(face.x - pos.x, face.y - pos.y) });
			return ref.get();
		}

		void RemoveSpots(const std::vector<RE::FormID>& ids)
		{
			for (auto id : ids) {
				if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id)) {
					Papyrus::CallMethod(ref, "ObjectReference", "Disable", nullptr, false);
					Papyrus::CallMethod(ref, "ObjectReference", "Delete", nullptr);
				}
			}
		}

		bool HostilesNear()
		{
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					auto a = h.get();
					if (a && !a->IsDead() && a->IsHostileToActor(Game::Player()) && Game::DistanceToPlayer(a.get()) < 4000.0f) {
						return true;
					}
				}
			}
			return false;
		}

		void End(const char* why, bool say)
		{
			if (!g_sit) {
				return;
			}
			auto s = *g_sit;
			g_sit.reset();
			SetBio(0, "");
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(s.npc);
			if (npc) {
				if (s.held) {
					Papyrus::CallStatic("SeverActions_FurnitureLib", "Stop", nullptr, static_cast<RE::Actor*>(npc));
				} else {
					Papyrus::CallMethod(npc, "Actor", "EvaluatePackage", nullptr);
				}
			}
			// Let them stand before the spot they sat on goes.
			std::thread([placed = s.placed] {
				std::this_thread::sleep_for(3s);
				SKSE::GetTaskInterface()->AddTask([placed] { RemoveSpots(placed); });
			}).detach();
			logger::info("sit: {} gets up ({})", npc ? Game::NameOf(npc) : "?", why);
			if (npc && (std::string_view(why) == "player stood" || std::string_view(why) == "player walked off" || std::string_view(why) == "time to get on")) {
				Keepsakes::OnMoment(s.npc);
			}
			if (npc && say && !npc->IsDead()) {
				const std::string w = why;
				Game::Narrate(w == "player stood"
								  ? std::format("{} got up as well when {} stood, stretching after the rest.", Game::NameOf(npc), PlayerName())
							  : w == "player walked off"
								  ? std::format("{} got up when {} wandered off, and went after them.", Game::NameOf(npc), PlayerName())
							  : w == "time to get on"
								  ? std::format("After a good while sitting together, {} stretched and got up: they had things of their own to see to, and said what.",
										Game::NameOf(npc))
								  : std::format("{} waited a while for {} to sit down beside them, then got up again.", Game::NameOf(npc), PlayerName()),
					npc);
			}
		}

		// Day/month rollover the way the calendar counts it (months 0-11, days from 1).
		void AdvanceHours(float hours)
		{
			auto* cal = RE::Calendar::GetSingleton();
			if (!cal || !cal->gameHour || !cal->gameDaysPassed) {
				return;
			}
			static constexpr std::array<int, 12> kDays{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
			cal->gameDaysPassed->value += hours / 24.0f;
			cal->gameHour->value += hours;
			while (cal->gameHour->value >= 24.0f) {
				cal->gameHour->value -= 24.0f;
				if (cal->gameDay && cal->gameMonth) {
					cal->gameDay->value += 1.0f;
					const int month = std::clamp(static_cast<int>(cal->gameMonth->value), 0, 11);
					if (cal->gameDay->value > kDays[month]) {
						cal->gameDay->value = 1.0f;
						cal->gameMonth->value += 1.0f;
						if (cal->gameMonth->value > 11.0f) {
							cal->gameMonth->value = 0.0f;
							if (cal->gameYear) {
								cal->gameYear->value += 1.0f;
							}
						}
					}
				}
			}
		}

		RE::SpellItem* SpellNamed(const char* name)
		{
			for (auto* s : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::SpellItem>()) {
				auto* file = s ? s->GetFile(0) : nullptr;
				if (file && std::string_view(file->GetFilename()) == "Skyrim.esm" && s->GetName() && std::string_view(s->GetName()) == name) {
					return s;
				}
			}
			return nullptr;
		}

		// By a fire, an hour or more with someone close: the game's own Rested (Lover's Comfort with a
		// lover), at most once a day and never on top of a sleep bonus.
		void FiresideBonus(const Session& s, float hours)
		{
			if (!s.fire || hours < 1.0f || !s.close || !SkyrimNet::ConfigBool("features.sit_bonus", true)) {
				return;
			}
			const int day = static_cast<int>(Game::Hours() / 24.0f);
			if (g_bonusDay == day) {
				return;
			}
			auto* player = Game::Player();
			auto* target = player->AsMagicTarget();
			for (auto* name : { "Well Rested", "Rested", "Lover's Comfort" }) {
				if (auto* sp = SpellNamed(name); sp && target && !sp->effects.empty() && sp->effects[0] && target->HasMagicEffect(sp->effects[0]->baseEffect)) {
					return;  // already has a rest bonus from sleeping
				}
			}
			auto* spell = SpellNamed(s.lover ? "Lover's Comfort" : "Rested");
			auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
			if (spell && caster) {
				caster->CastSpellImmediate(spell, false, player, 1.0f, false, 0.0f, nullptr);
				g_bonusDay = day;
				Game::Notify(std::format("{} by the fire: {}", s.lover ? "Close together" : "A good rest", spell->GetName()));
				logger::info("sit: fireside bonus {}", spell->GetName());
			}
		}

		void PassTime(float hours)
		{
			if (!g_sit || hours <= 0.0f) {
				return;
			}
			hours = (std::min)(hours, kMaxSkip);
			if (HostilesNear()) {
				if (auto* npc = RE::TESForm::LookupByID<RE::Actor>(g_sit->npc)) {
					Game::Narrate(std::format("{} kept glancing into the dark: this was no place to let the hours slip by.", Game::NameOf(npc)), npc);
				}
				return;
			}
			Papyrus::CallStatic("Game", "FadeOutGame", nullptr, true, true, 0.0f, 1.0f);
			std::thread([hours] {
				std::this_thread::sleep_for(1500ms);
				SKSE::GetTaskInterface()->AddTask([hours] {
					auto* player = Game::Player();
					AdvanceHours(hours);
					// Like waiting: the body recovers with the hours.
					auto* av = player->AsActorValueOwner();
					for (auto v : { RE::ActorValue::kHealth, RE::ActorValue::kStamina, RE::ActorValue::kMagicka }) {
						av->RestoreActorValue(v, av->GetPermanentActorValue(v) * (std::min)(1.0f, hours / 2.0f));
					}
					Papyrus::CallStatic("Game", "FadeOutGame", nullptr, false, true, 0.5f, 1.5f);
					if (!g_sit) {
						return;
					}
					auto* npc = RE::TESForm::LookupByID<RE::Actor>(g_sit->npc);
					if (!npc) {
						return;
					}
					const auto span = hours < 1.0f ? std::string("Half an hour") : hours < 1.5f ? std::string("An hour") : std::format("{} hours", static_cast<int>(hours));
					FiresideBonus(*g_sit, hours);
					if (!g_sit->counted) {
						g_sit->counted = true;
						Bonds::Add(npc, Bonds::Deed::kSat, 1);
					}
					SkyrimNet::AddMemory(npc->GetFormID(), std::format("{} sat with {} {} for {} , talking quietly and resting.", Game::NameOf(npc), PlayerName(), g_sit->where,
																 Lower(span.c_str())),
						0.5f);
					Game::Mention(std::format("{} slipped by as {} and {} sat together {}, talking now and then. {} looked up, surprised how late it had got.", span,
									  Game::NameOf(npc), PlayerName(), g_sit->where, Game::NameOf(npc)),
						npc);
					logger::info("sit: {:.1f} hours passed with {}", hours, Game::NameOf(npc));
				});
			}).detach();
		}

		// Both sitting now: time may pass, if the player wants (the hours they said, or a choice).
		void OnBothSeated()
		{
			auto& s = *g_sit;
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(s.npc);
			if (!npc || s.offered) {
				return;
			}
			s.offered = true;
			if (!s.close || !SkyrimNet::ConfigBool("features.sit_skip", true)) {
				return;  // just sitting: no hours passing with someone they barely know, or the player turned it off
			}
			const auto npcId = s.npc;
			if (s.hours > 0.0f) {
				const float h = (std::min)(s.hours, kMaxSkip);
				if (!Confirm::Available()) {
					PassTime(h);
					return;
				}
				Confirm::Offer offer;
				offer.title = "Pass the time";
				offer.npc = Game::NameOf(npc);
				offer.detail = std::format("Sit {} for {} hour{}", s.where, h < 1.0f ? "half an" : std::to_string(static_cast<int>(h)), h >= 2.0f ? "s" : "");
				offer.showPrice = false;
				offer.seconds = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f));
				offer.acceptLabel = "Let the time pass";
				offer.declineLabel = "Just sit";
				Confirm::Ask(offer, [npcId, h](Confirm::Choice c) {
					if (c == Confirm::Choice::kAccept && g_sit && g_sit->npc == npcId) {
						PassTime(h);
					}
				});
				return;
			}
			if (!Confirm::Available()) {
				return;
			}
			Confirm::Offer offer;
			offer.title = "Pass the time?";
			offer.npc = Game::NameOf(npc);
			offer.detail = std::format("Sitting {}", s.where);
			offer.showPrice = false;
			offer.seconds = 0;
			offer.acceptLabel = "Let it pass";
			offer.declineLabel = "Just sit";
			offer.hint = "Pick how long, or Just sit (no time passes).";
			offer.chosenText = "Time: ";
			offer.pick = { { 1, "Half an hour", 1, 0, false }, { 2, "An hour", 1, 0, false }, { 3, "Two hours", 1, 0, false }, { 4, "Four hours", 1, 0, false } };
			Confirm::Ask(offer, [npcId](Confirm::Choice c) {
				if (c != Confirm::Choice::kAccept || !g_sit || g_sit->npc != npcId) {
					return;
				}
				static constexpr std::array<float, 5> kHours{ 0.0f, 0.5f, 1.0f, 2.0f, 4.0f };
				const auto [id, qty] = Confirm::LastPick();
				if (id >= 1 && id <= 4) {
					PassTime(kHours[id]);
				}
			});
		}
	}

	bool Sit(RE::Actor* npc, const std::string& said, float hours)
	{
		if (!npc) {
			return false;
		}
		const std::string name = Game::NameOf(npc);
		if (SittingWith(npc->GetFormID())) {
			logger::info("sit: {} is already sitting with the player; nothing to change", name);
			return true;
		}
		End("another sit", false);
		auto* player = Game::Player();
		const auto place = PlaceOf(npc);
		// A shop, a temple, a palace: they agreed, so a seat if there's one in sight, but never the floor. A
		// tavern or someone else's house too, unless the floor was asked for: seats there, not the boards.
		const bool seatOnly = place == Place::kNotHere || (!FloorFits(npc) && WhereOf(said) != Where::kGround);
		const bool lover = [&] {
			auto* base = npc->GetActorBase();
			auto* rel = base ? RE::BGSRelationship::GetRelationship(base, player->GetActorBase()) : nullptr;
			return rel && rel->level.get() == RE::BGSRelationship::RELATIONSHIP_LEVEL::kLover;
		}();
		// Following the player or spending time with them counts as close, whatever the relationship
		// (the user's call: out hunting together, an acquaintance is company enough).
		const bool close = Services::IsCompanion(npc) || Promises::IsClose(npc) || Services::InCompany(npc->GetFormID()) || Services::FollowsPlayer(npc);
		const auto cell = player->GetParentCell();
		const bool outside = cell && !cell->IsInteriorCell();
		// They said yes: where they asked or were asked, whatever the relationship (it only colours
		// how they sit: a polite space, or close beside).
		auto want = WhereOf(said);
		if (seatOnly) {
			want = Where::kSeat;
		}
		auto* fire = (want == Where::kFire || want == Where::kAuto) ? NearestFire() : nullptr;
		if (want == Where::kFire && !fire) {
			// "By the hearth" with no fire burning (or only words about a fire): they still sit, where they can.
			logger::info("sit: {} - no fire near; sitting wherever there's room", name);
			want = Where::kAuto;
		}

		// Where they sit: beside the seated player first; by the fire; a seat; else the ground.
		Session s;
		s.npc = npc->GetFormID();
		s.hours = hours;
		s.close = close;
		s.lover = lover;
		s.started = Clock::now();
		s.startHours = Game::Hours();
		RE::TESObjectREFR* spot = nullptr;
		const bool playerSits = PlayerSitting();
		const auto center = fire ? fire->GetPosition() : player->GetPosition();
		// Already sitting near the player (a drink together): they stay right where they are, and the
		// drink's end no longer stands them up.
		if (auto f = npc->GetOccupiedFurniture().get(); f && IsSeat(f.get()) && Game::DistanceToPlayer(npc) < 600.0f) {
			g_held.erase(npc->GetFormID());
			s.spot = f->GetFormID();
			s.held = HasSeverActions();
			s.where = "together";
			const std::string where = s.where;
			g_sit = s;
			SetBio(s.npc, std::format("Sitting {} with {} right now: a quiet moment, no hurry, the kind of time for easy talk.", where, PlayerName()));
			logger::info("sit: {} stays in their seat (spot {:08X}), the drink becomes a sit", name, s.spot);
			return true;
		}
		if (want != Where::kGround && want != Where::kBed) {
			spot = playerSits ? SeatNear(player->GetPosition(), 600.0f) : fire ? SeatNear(center, kNearFire) : SeatNear(player->GetPosition(), 1200.0f);
		}
		// No seat free, indoors, with someone close: side by side on the edge of a bed.
		if (!spot && want != Where::kGround && !fire && !outside && !seatOnly) {
			// (asked for the bed, or no seat free)
			if (auto edge = BedEdgeNear(playerSits ? 400.0f : 800.0f)) {
				// An invisible chair at the edge if the game has one; else (the base game has none) sitting
				// cross-legged on the mattress, a little in from the edge.
				const bool chair = ChairSpot() != nullptr;
				auto at = [&](float along) {
					RE::NiPoint3 p{ edge->at.x + edge->along.x * along, edge->at.y + edge->along.y * along, edge->at.z };
					if (!chair) {
						p.x += edge->in.x * 25.0f;
						p.y += edge->in.y * 25.0f;
						p.z = edge->top;
					}
					return p;
				};
				// Shifted along the side, so the player has room beside them (or sits on the other spot).
				if (auto* bedSpot = PlaceSpot(at(40.0f), edge->face, chair)) {
					s.placed.push_back(bedSpot->GetFormID());
					if (!playerSits) {
						if (auto* yourSpot = PlaceSpot(at(-40.0f), edge->face, chair)) {
							s.placed.push_back(yourSpot->GetFormID());
							SeatPlayerSoon(yourSpot->GetFormID());
						}
					}
					spot = bedSpot;
					s.where = chair ? "on the edge of the bed" : "on the bed";
				}
			}
		}
		if (!spot && want == Where::kBed) {  // also out of doors: no bed to sit on, not the grass instead
			Game::Narrate(std::format("{} looked around: there was no bed near enough to sit on.", name), npc);
			logger::info("sit: {} - not sat ({})", name, "no bed near enough");
			return true;
		}
		if (spot && !s.where.empty()) {
			// placed above: where is already said
		} else if (spot) {
			const auto seatName = Lower(spot->GetBaseObject()->GetName());
			s.where = fire && spot->GetPosition().GetDistance(fire->GetPosition()) < kNearFire ? "by the fire"
			        : seatName.find("bench") != std::string::npos           ? "on the bench"
			        : seatName.find("stool") != std::string::npos           ? "on a stool"
			                                                                 : "together";
		} else if (seatOnly) {
			if (!g_quietSit) {  // sitting down by themselves (the player sat): no seat, they just stay standing
				Game::Narrate(std::format("{} looked around for a seat, but there was none free in this place.", name), npc);
			}
			logger::info("sit: {} - not sat ({})", name, "no free seat, and no sitting on the floor here");
			return true;
		} else {
			// No seat in sight (or none free): the floor beside the player, even when a chair was asked for.
			// The ground: a spot for them, and one beside it for the player (unless already sitting).
			const auto p = player->GetPosition();
			RE::NiPoint3 mine, yours;
			if (fire) {
				// Around the fire, facing it, side by side.
				const float base = std::atan2(p.x - center.x, p.y - center.y);
				mine = { center.x + std::sin(base + 0.5f) * 150.0f, center.y + std::cos(base + 0.5f) * 150.0f, p.z };
				yours = { center.x + std::sin(base - 0.2f) * 150.0f, center.y + std::cos(base - 0.2f) * 150.0f, p.z };
			} else {
				const float a = player->GetAngleZ();
				mine = { p.x + std::sin(a + 1.4f) * 90.0f, p.y + std::cos(a + 1.4f) * 90.0f, p.z };
				yours = p;
			}
			const RE::NiPoint3 face = fire ? center : RE::NiPoint3{ p.x + std::sin(player->GetAngleZ()) * 500.0f, p.y + std::cos(player->GetAngleZ()) * 500.0f, p.z };
			spot = PlaceSpot(mine, face);
			if (!spot) {
				Game::Narrate(std::format("{} looked for a dry patch of ground to sit on, but found none.", name), npc);
				logger::info("sit: {} - not sat ({})", name, "no spot on the ground");
				return true;
			}
			s.placed.push_back(spot->GetFormID());
			if (!playerSits) {
				if (auto* yourSpot = PlaceSpot(yours, face)) {
					s.placed.push_back(yourSpot->GetFormID());
					SeatPlayerSoon(yourSpot->GetFormID());
				}
			}
			s.where = fire ? "on the ground by the fire" : outside ? "on the grass" : "on the floor";
		}
		s.spot = spot->GetFormID();
		s.fire = fire ? fire->GetFormID() : 0;
		s.held = HasSeverActions() &&
		         Papyrus::CallStatic("SeverActions_FurnitureLib", "UseRef", nullptr, static_cast<RE::Actor*>(npc), static_cast<RE::TESObjectREFR*>(spot));
		if (!s.held) {
			Papyrus::CallMethod(spot, "ObjectReference", "Activate", nullptr, static_cast<RE::TESObjectREFR*>(npc), false);
		}
		const std::string where = s.where;
		g_sit = s;
		SetBio(s.npc, std::format("Sitting {} with {} right now: a quiet moment, no hurry, the kind of time for easy talk.", where, PlayerName()));
		logger::info("sit: {} sits {} (spot {:08X}{}, hours said {:.1f})", name, where, s.spot, s.placed.empty() ? "" : ", placed", hours);
		// In their own words, by how close they are.
		Game::Mention(lover   ? std::format("{} sat down {} and made room for {} close beside them, glad of a quiet moment together.", name, where, PlayerName()) :
		              close   ? std::format("{} sat down {} with a contented sigh and nodded at the spot beside them for {}.", name, where, PlayerName()) :
		                        std::format("{} sat down {} for a short rest, leaving a polite space for {}.", name, where, PlayerName()),
			npc);
		return true;
	}

	void Release(RE::FormID npc)
	{
		g_held.erase(npc);  // a drink's stand-up later won't touch them
		if (g_sit && g_sit->npc == npc) {
			auto placed = g_sit->placed;
			g_sit.reset();
			SetBio(0, "");
			std::thread([placed] {
				std::this_thread::sleep_for(3s);
				SKSE::GetTaskInterface()->AddTask([placed] { RemoveSpots(placed); });
			}).detach();
			logger::info("moments: {:08X} released (taken over by another activity)", npc);
		}
	}

	bool IsSitting(RE::FormID npc)
	{
		return SittingWith(npc);
	}

	void SongTick();  // below

	// A real seat: a chair, bench or stool - not a cooking spit, forge, workbench, cart or throne.
	bool IsSeat(RE::TESObjectREFR* ref)
	{
		auto* base = ref ? ref->GetBaseObject() : nullptr;
		auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
		if (!furn || !furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSit) ||
			furn->workBenchData.benchType.get() != RE::TESFurniture::WorkBenchData::BenchType::kNone) {
			return false;
		}
		const auto name = Lower(base->GetName()) + " " + ModelOf(base);
		for (auto* k : { "cart", "carriage", "wagon", "throne", "horse", "spit", "cook", "forge", "anvil", "tanning", "smelter", "grindstone",
				 "alchemy", "enchant" }) {
			if (name.find(k) != std::string::npos) {
				return false;
			}
		}
		return true;
	}

	bool PlayerSitting()
	{
		auto* player = Game::Player();
		auto* state = player->AsActorState();
		if (!state || state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kIsSitting || player->IsOnMount()) {
			return false;  // standing, lying in bed, riding
		}
		// On furniture: a real seat - not a carriage, a forge or workbench, a throne.
		if (auto f = player->GetOccupiedFurniture().get(); f && !IsSeat(f.get())) {
			return false;
		}
		return true;  // a seat, or the floor (a sitting mod's animation, no furniture)
	}

	namespace
	{
		std::optional<Clock::time_point> g_playerSatAt;
		bool g_autoDone = false;

		// The player sits down (a chair, or the floor): after a moment, whoever walks with them sits too
		// - the nearest; spending time together has its own (Services). Once per sitting down.
		void AutoSit()
		{
			if (!SkyrimNet::ConfigBool("features.sit_with_player", true)) {
				return;
			}
			auto* player = Game::Player();
			if (!PlayerSitting() || player->IsInCombat()) {
				g_playerSatAt.reset();
				g_autoDone = false;
				return;
			}
			const auto now = Clock::now();
			if (!g_playerSatAt) {
				g_playerSatAt = now;
				return;
			}
			if (g_autoDone || now - *g_playerSatAt < 3s || g_sit) {
				return;
			}
			g_autoDone = true;
			RE::Actor* best = nullptr;
			float bestDist = 800.0f;
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					auto a = h.get();
					if (!a || a.get() == player || a->IsDead() || a->IsInCombat() || a->GetOccupiedFurniture().get() ||
						!Services::FollowsPlayer(a.get()) || Services::InCompany(a->GetFormID())) {
						continue;
					}
					if (const float d = Game::DistanceToPlayer(a.get()); d < bestDist) {
						bestDist = d;
						best = a.get();
					}
				}
			}
			if (best) {
				logger::info("sit: the player sat down; {} (walking with them) sits too", best->GetName());
				g_quietSit = true;
				Sit(best, "", 0.0f);
				g_quietSit = false;
			}
		}
	}

	void Tick()
	{
		SongTick();
		// A drink agreed out of doors: now, at the tavern, with them along.
		if (g_drinkPlan) {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(g_drinkPlan->npc);
			if (!npc || npc->IsDead() || std::chrono::steady_clock::now() > g_drinkPlan->until) {
				logger::info("drink: the tavern plan lapsed");
				g_drinkPlan.reset();
			} else if (PlaceOf(npc) == Place::kInn && npc->GetParentCell() == Game::Player()->GetParentCell() && Game::DistanceToPlayer(npc) < 1500.0f &&
					   !npc->IsInCombat()) {
				const auto said = g_drinkPlan->said;
				g_drinkPlan.reset();
				logger::info("drink: at the tavern with {}; the drink they agreed on", Game::NameOf(npc));
				Drink(npc, said);
			}
		}
		AutoSit();
		if (!g_sit) {
			return;
		}
		auto& s = *g_sit;
		auto* npc = RE::TESForm::LookupByID<RE::Actor>(s.npc);
		auto* player = Game::Player();
		if (!npc || npc->IsDead() || npc->IsInCombat() || player->IsInCombat() || Game::DistanceToPlayer(npc) > kStayNear) {
			End("cut short", false);
			return;
		}
		const auto now = Clock::now();
		const bool seated = PlayerSitting() && Game::DistanceToPlayer(npc) < 600.0f;
		if (!s.playerSeated) {
			if (seated) {
				s.playerSeated = true;
				s.seatedAt = now;
				s.stoodAt = {};
				OnBothSeated();
			} else if (now - s.started > kWaitForPlayer) {
				End("player never sat", true);
			}
			return;
		}
		if (!seated) {
			// Spending time together: the player getting up to stretch or fetch something doesn't end it.
			// They stay seated while the player is about (same room, round the fire); only walking off
			// for a while - or the time being up - gets them up.
			const bool company = Services::InCompany(s.npc);
			if (company && Game::DistanceToPlayer(npc) < 600.0f) {
				s.stoodAt = {};
				const float limit = s.hours > 0.0f ? s.hours : SkyrimNet::ConfigFloat("tuning.sit_hours", 2.0f);
				if (Game::Hours() - s.startHours >= limit) {
					End("time to get on", true);
				}
				return;
			}
			if (s.stoodAt == Clock::time_point{}) {
				s.stoodAt = now;
			} else if (now - s.stoodAt > (company ? std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(10)) : std::chrono::duration_cast<Clock::duration>(kStoodUp))) {
				End(company ? "player walked off" : "player stood", true);
			}
			return;
		}
		s.stoodAt = {};
		// Not for ever: the hours the player named, else a couple of game hours (waiting counts too). Then
		// they have their own day to see to.
		{
			const float limit = s.hours > 0.0f ? s.hours : SkyrimNet::ConfigFloat("tuning.sit_hours", 2.0f);
			if (Game::Hours() - s.startHours >= limit) {
				End("time to get on", true);
				return;
			}
		}
		// A few real minutes side by side is time spent together, even without the hours passing.
		if (!s.counted && now - s.seatedAt > kTogetherFor) {
			s.counted = true;
			Bonds::Add(npc, Bonds::Deed::kSat, 1);
		}
	}

	// ---------------------------------------------------------------- a song

	namespace
	{
		constexpr float kSongEvery = 1.0f;  // game hours before the same person performs again
		constexpr auto kSongLasts = std::chrono::minutes(4);

		struct Performance
		{
			RE::FormID npc = 0;
			bool scene = false;  // a vanilla bard scene we started (to stop on a fight)
			Clock::time_point until;
		};
		std::optional<Performance> g_song;  // game thread only
		std::unordered_map<RE::FormID, float> g_lastSong;
		std::pair<RE::FormID, std::string> g_songBio;  // g_lock

		void SetSongBio(RE::FormID npc, std::string line)
		{
			std::scoped_lock l(g_lock);
			g_songBio = { npc, std::move(line) };
		}

		// The base game's bard songs (BardSongsScript): any actor can be put in a scene with an
		// instrument; the sung songs exist only for the bards' voices.
		RE::TESQuest* BardSongs()
		{
			static RE::TESQuest* cached = nullptr;
			static bool searched = false;
			if (!searched) {
				searched = true;
				for (auto* q : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESQuest>()) {
					auto* file = q ? q->GetFile(0) : nullptr;
					if (file && std::string_view(file->GetFilename()) == "Skyrim.esm" && Papyrus::HasScript(q, "BardSongsScript")) {
						cached = q;
						break;
					}
				}
				logger::info("song: vanilla bard songs {}", cached ? std::format("{:08X}", cached->GetFormID()) : "not found");
			}
			return cached;
		}

		void StopVanillaSongs()
		{
			if (auto* q = BardSongs()) {
				Papyrus::CallMethod(q, "BardSongsScript", "StopAllSongs", nullptr);
			}
		}

		// SkyrimNet's own AI bard songs, when the player has them on: then bards are SkyrimNet's.
		bool SkyrimNetSings()
		{
			static std::optional<bool> cached;
			if (!cached) {
				std::ifstream f("Data/SKSE/Plugins/SkyrimNet/config/BardSinging.yaml");
				std::string line;
				bool inSection = false;
				cached = false;
				while (std::getline(f, line)) {
					const auto l = JsonUtil::Lower(line);
					if (l.starts_with("bard_singing:")) {
						inSection = true;
					} else if (inSection && l.find("enabled:") != std::string::npos && l.find_first_not_of(' ') == 2) {
						cached = l.find("true") != std::string::npos;
						break;
					}
				}
				logger::info("song: SkyrimNet bard singing {}", *cached ? "on (bards are SkyrimNet's)" : "off");
			}
			return *cached;
		}

		bool IsBard(RE::Actor* npc)
		{
			auto* base = npc->GetActorBase();
			return Lower(base && base->npcClass ? base->npcClass->GetName() : "").find("bard") != std::string::npos;
		}

		// An instrument they carry: "Lute", "Flute", "Drum" (what the bard scenes know), or "".
		std::string InstrumentOf(RE::Actor* npc, const std::string& wanted)
		{
			std::string found;
			for (auto& [obj, count] : npc->GetInventoryCounts()) {
				if (count <= 0 || !obj) {
					continue;
				}
				const auto name = Lower(obj->GetName());
				// "flute" contains "lute": the flute first.
				const char* kind = name.find("flute") != std::string::npos  ? "Flute" :
				                   name.find("lute") != std::string::npos   ? "Lute" :
				                   name.find("drum") != std::string::npos   ? "Drum" :
				                                                              nullptr;
				const auto asked = JsonUtil::Words(wanted);
				if (kind && (found.empty() || std::ranges::find(asked, Lower(kind)) != asked.end())) {
					found = kind;
				}
			}
			return found;
		}

		// A vanilla song by name, for the bards' voices: 1 Ragnar the Red, 2 Age of Aggression /
		// Oppression, 3 The Dragonborn Comes, 10 Tale of the Tongues; 0 = their choice.
		std::int32_t SongId(const std::string& said)
		{
			const auto s = JsonUtil::Lower(said);
			if (s.find("ragnar") != std::string::npos) {
				return 1;
			}
			if (s.find("aggression") != std::string::npos || s.find("oppression") != std::string::npos) {
				return 2;
			}
			if (s.find("dragonborn") != std::string::npos) {
				return 3;
			}
			if (s.find("tongue") != std::string::npos) {
				return 10;
			}
			return 0;
		}

		void Begin(RE::Actor* npc, bool scene, const std::string& what, Clock::duration lasts = kSongLasts)
		{
			const auto id = npc->GetFormID();
			g_song = Performance{ id, scene, Clock::now() + lasts };
			g_lastSong[id] = Game::Hours();
			SetSongBio(id, std::format("Is {} for {} right now, at their asking.", what, PlayerName()));
			Bonds::Add(npc, Bonds::Deed::kSong, 1);
		}
	}

	bool Perform(RE::Actor* npc, const std::string& said, const std::string& how)
	{
		if (!npc) {
			return false;
		}
		const std::string name = Game::NameOf(npc);
		auto* player = Game::Player();
		const auto id = npc->GetFormID();
		if (auto it = g_lastSong.find(id); it != g_lastSong.end() && Game::Hours() - it->second < kSongEvery) {
			Game::Narrate(std::format("{} laughed and waved it off: they had only just performed, their voice needed a rest.", name), npc);
			return true;
		}
		const bool bard = IsBard(npc);
		// SkyrimNet's AI bard songs are on: a bard's performance is theirs to play (their "Perform").
		if (bard && SkyrimNetSings()) {
			logger::info("song: {} is a bard and SkyrimNet bard singing is on; leaving it to SkyrimNet", name);
			return true;
		}
		const auto h = JsonUtil::Lower(how);
		const bool wantsSong = h.find("sing") != std::string::npos;
		const bool wantsPlay = h.find("play") != std::string::npos;
		const std::string song = said;
		// On the move, or sitting together: no standing up for a bard scene. They sing as they are.
		const bool seated = static_cast<bool>(npc->GetOccupiedFurniture().get()) || (g_sit && g_sit->npc == id);
		const bool onTheRoad = Services::IsCompanion(npc) && (player->IsMoving() || npc->IsMoving()) && !seated;
		const auto instrument = InstrumentOf(npc, said);

		// Not a bard: no singing voice to speak of, so they hum (a sung line at most). A spoken
		// "song" of a dozen lines sounds like a recital; a hum and a half-sung line sounds real.
		auto sing = [&](const char* setting) {
			StopVanillaSongs();
			// Only the hummed reply itself: left on longer, every line after it comes out half-sung (and the
			// voice reads it in a sing-song, "auto-tuned" way).
			Begin(npc, false, "humming a tune", std::chrono::seconds(25));
			const bool ownSong = song.empty() || (SongId(song) == 0 && JsonUtil::Words(song).size() <= 1);  // "something", "a song"
			const auto what = ownSong ? std::string("an old tune") : "\"" + song + "\"";
			Game::Narrate(std::format("{} {}hummed {} softly for {}, half under their breath. Their next words are only that: a hum, and at most one or two "
			                          "sung words or a single short line of it, soft and unpolished, nothing more.",
							  name, setting, what, PlayerName()),
				npc);
			logger::info("song: {} hums ({})", name, setting);
		};

		if (onTheRoad) {
			sing("kept walking and ");
			return true;
		}
		if (seated) {
			sing(wantsPlay && !instrument.empty() ? "had no room to play sitting there, so instead " : "");
			return true;
		}
		auto* quest = BardSongs();
		if (quest) {
			Release(id);  // a performance stands them up: not ours to stand up again later
		}
		// A bard: the real songs, sung or played, from where they stand.
		if (bard && quest) {
			StopVanillaSongs();
			if (wantsPlay && !wantsSong) {
				// A bard's scene brings its own instrument: the one asked for, theirs, or whatever suits.
				const auto asked = JsonUtil::Lower(song);
				const auto inst = asked.find("flute") != std::string::npos ? std::string("Flute") : asked.find("lute") != std::string::npos ? std::string("Lute") :
				                  asked.find("drum") != std::string::npos ? std::string("Drum") : instrument.empty() ? std::string("Instrumental") : instrument;
				Papyrus::CallMethod(quest, "BardSongsScript", "PlaySong", nullptr, static_cast<RE::Actor*>(npc), RE::BSFixedString(inst), true);
				Begin(npc, true, "playing their " + Lower(inst.c_str()));
			} else {
				Papyrus::CallMethod(quest, "BardSongsScript", "PlaySongRequest", nullptr, static_cast<RE::Actor*>(npc), RE::BSFixedString("Any"), true,
					static_cast<std::int32_t>(SongId(song)), true);
				Begin(npc, true, "performing a song");
			}
			Game::Narrate(std::format("{} took up their instrument and began to perform{} for {}; the room quieted to listen.", name,
							  SongId(song) ? " the song " + song : std::string(""), PlayerName()),
				npc);
			logger::info("song: {} performs (vanilla bard scene)", name);
			return true;
		}
		// Anyone else: their own instrument if they carry one; else their own voice.
		if (!instrument.empty() && quest && !wantsSong) {
			StopVanillaSongs();
			Papyrus::CallMethod(quest, "BardSongsScript", "PlaySong", nullptr, static_cast<RE::Actor*>(npc), RE::BSFixedString(instrument), true);
			Begin(npc, true, "playing their " + Lower(instrument.c_str()));
			Game::Narrate(std::format("{} took out their {} and played a tune for {}.", name, Lower(instrument.c_str()), PlayerName()), npc);
			logger::info("song: {} plays their {}", name, instrument);
			return true;
		}
		if (wantsPlay && !wantsSong && instrument.empty()) {
			sing("had no instrument with them, so instead ");
			return true;
		}
		sing("");
		return true;
	}

	void SongTick()
	{
		if (!g_song) {
			return;
		}
		auto* npc = RE::TESForm::LookupByID<RE::Actor>(g_song->npc);
		auto* player = Game::Player();
		const bool fight = player->IsInCombat() || (npc && npc->IsInCombat());
		const bool gone = !npc || npc->IsDead() || Game::DistanceToPlayer(npc) > 4000.0f;
		if (fight || gone || Clock::now() > g_song->until) {
			if (g_song->scene && (fight || gone)) {
				StopVanillaSongs();  // a fight breaks out, or the player walked off: the music stops
			}
			logger::info("song: {}'s performance ends ({})", npc ? Game::NameOf(npc) : "?", fight ? "fight" : gone ? "player left" : "done");
			g_song.reset();
			SetSongBio(0, "");
		}
	}

	std::vector<std::string> BioLines(RE::FormID npc)
	{
		std::vector<std::string> out;
		std::scoped_lock l(g_lock);
		if (g_sitBio.first == npc && !g_sitBio.second.empty()) {
			out.push_back(g_sitBio.second);
		}
		if (g_songBio.first == npc && !g_songBio.second.empty()) {
			out.push_back(g_songBio.second);
		}
		return out;
	}

	void Reset()
	{
		g_drinkPlan.reset();
		{
			std::scoped_lock l(g_lock);
			g_lastDrink.clear();
			g_sitBio = {};
			g_songBio = {};
		}
		g_sit.reset();  // spots from the other timeline stay behind invisible; nothing to stand up
		g_bonusDay = -1;
		g_song.reset();
		g_lastSong.clear();
		g_held.clear();
	}
}
