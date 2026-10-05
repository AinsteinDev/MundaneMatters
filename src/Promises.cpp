#include "Promises.h"

#include "Confirm.h"
#include "Game.h"
#include "Papyrus.h"
#include "JsonUtil.h"
#include "Ledger.h"
#include "Services.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Promises
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto kDedupWindow = 45s;
		constexpr std::int32_t kMaxItems = 10;

		// ---- fuzzy item names ----

		// Containers, portions and adjectives people wrap around an item name.
		const std::unordered_set<std::string> kFiller{
			"a", "an", "the", "of", "some", "my", "your", "our", "this", "that", "few", "couple", "bit", "little",
			"bowl", "bowls", "plate", "cup", "mug", "tankard", "bottle", "bottles", "flask", "jug", "pint",
			"piece", "pieces", "slice", "slices", "loaf", "loaves", "chunk", "hunk", "wedge", "wheel", "portion", "serving",
			"fresh", "fine", "good", "nice", "old", "homemade", "spare", "extra",
			"something", "anything", "to", "for", "with", "and", "you", "me", "quick", "proper", "decent", "free",
			"it", "them", "those", "these", "thing", "things", "stuff"
		};

		// Words people say vs. words in item names.
		const std::unordered_map<std::string, std::string> kSynonyms{
			{ "health", "heal" }, { "healing", "heal" }, { "stamina", "stamina" }, { "magicka", "magicka" },
			{ "beer", "ale" }, { "booze", "mead" }, { "drink", "mead" }, { "spud", "potato" }, { "spuds", "potato" },
			{ "coin", "gold" }, { "coins", "gold" }, { "septim", "gold" }, { "septims", "gold" }
		};

		std::string Stem(std::string w)
		{
			if (auto it = kSynonyms.find(w); it != kSynonyms.end()) {
				return it->second;
			}
			auto ends = [&](std::string_view suf, std::size_t minStem) {
				return w.size() >= suf.size() + minStem && w.ends_with(suf);
			};
			if (ends("ing", 3)) {  // salting -> salt (healing is a synonym above)
				w.resize(w.size() - 3);
			} else if (ends("ies", 2)) {  // berries -> berry
				w.resize(w.size() - 3);
				w += "y";
			} else if (ends("oes", 3) || ends("ches", 2) || ends("shes", 2) || ends("xes", 2) || ends("sses", 2)) {
				w.resize(w.size() - 2);  // potatoes -> potato, boxes -> box
			} else if (ends("s", 3) && !w.ends_with("ss")) {
				w.resize(w.size() - 1);  // apples -> apple, cheeses -> cheese; not "glass"
			}
			return w;
		}

		std::vector<std::string> Tokens(std::string_view text, bool dropFiller)
		{
			std::vector<std::string> out;
			std::string cur;
			auto flush = [&] {
				if (!cur.empty() && !(dropFiller && kFiller.contains(cur))) {
					out.push_back(Stem(cur));
				}
				cur.clear();
			};
			for (char c : text) {
				if (std::isalnum(static_cast<unsigned char>(c))) {
					cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
				} else {
					flush();
				}
			}
			flush();
			return out;
		}

		// 0 = no match. Higher is better: every wanted word present, few extra words.
		double Score(const std::vector<std::string>& wanted, const char* name)
		{
			if (!name || !*name || wanted.empty()) {
				return 0.0;
			}
			auto have = Tokens(name, false);
			std::string haveJoined;
			for (auto& h : have) {
				haveJoined += h;
			}
			auto present = [&](const std::string& w) {
				// "sweetroll" vs "Sweet Roll": a wanted word may span words of the name.
				return std::find(have.begin(), have.end(), w) != have.end() || (w.size() >= 5 && haveJoined.find(w) != std::string::npos);
			};
			// The head noun (last word) must match: "cabbage soup" may become another soup, but
			// "iron sword" never becomes an iron ingot.
			if (!present(wanted.back())) {
				return 0.0;
			}
			std::size_t hits = 0;
			for (auto& w : wanted) {
				if (present(w)) {
					++hits;
				}
			}
			const double coverage = static_cast<double>(hits) / wanted.size();
			if (coverage < 0.5) {
				return 0.0;
			}
			const double extra = static_cast<double>(have.size() - (std::min)(have.size(), hits));
			return coverage * 10.0 - extra * 0.5;
		}

		struct Match
		{
			RE::TESBoundObject* obj = nullptr;
			double score = 0.0;
			RE::TESObjectREFR* from = nullptr;  // container that holds it; null = fetched from nowhere
			bool worn = false;
		};

		// The NPC's own pockets, then (for merchants) the stock they sell from: a free sample
		// comes off the shelf.
		std::vector<RE::TESObjectREFR*> Stores(RE::Actor* npc)
		{
			std::vector<RE::TESObjectREFR*> out{ npc };
			if (auto* faction = npc->GetVendorFaction(); faction && faction->vendorData.merchantContainer) {
				out.push_back(faction->vendorData.merchantContainer);
			}
			return out;
		}

		Match BestIn(RE::TESObjectREFR* container, const std::vector<std::string>& wanted, bool allowWorn = false)
		{
			Match best;
			for (auto& [obj, entry] : container->GetInventory()) {
				if (!obj || entry.first <= 0 || obj == Game::Gold()) {
					continue;
				}
				// Quest items stay where the quest needs them. Worn gear stays on (a promised
				// "sword" should not strip the guard) unless the owner chose it (allowWorn).
				const bool worn = entry.second && entry.second->IsWorn();
				if (entry.second && entry.second->IsQuestObject() || (worn && !allowWorn)) {
					continue;
				}
				// A spare unworn copy beats the one in hand.
				double s = Score(wanted, obj->GetName()) - (worn ? 0.25 : 0.0);
				if (s > best.score) {
					best = { obj, s, container, worn };
				}
			}
			return best;
		}

		Match BestInInventory(RE::Actor* npc, const std::vector<std::string>& wanted)
		{
			Match best;
			for (auto* store : Stores(npc)) {
				auto m = BestIn(store, wanted);
				if (m.score > best.score) {  // ties stay with their own pockets
					best = m;
				}
			}
			return best;
		}

		// Cheap consumables and misc items the NPC could plausibly fetch (soup, bread, ale,
		// potions, ingredients). Gear is never conjured: it must come from their inventory.
		Match BestInWorld(const std::vector<std::string>& wanted, std::int32_t maxValue)
		{
			Match best;
			auto consider = [&](RE::TESBoundObject* obj) {
				if (!obj || obj->GetGoldValue() > maxValue) {
					return;
				}
				double s = Score(wanted, obj->GetName());
				// Ties go to the cheaper, more ordinary item.
				if (s > 0.0) {
					s -= obj->GetGoldValue() * 0.001;
				}
				if (s > best.score) {
					best = { obj, s };
				}
			};
			auto* dh = RE::TESDataHandler::GetSingleton();
			for (auto* f : dh->GetFormArray<RE::AlchemyItem>()) {
				if (f && !f->IsPoison()) {
					consider(f);
				}
			}
			for (auto* f : dh->GetFormArray<RE::IngredientItem>()) {
				consider(f);
			}
			for (auto* f : dh->GetFormArray<RE::TESObjectMISC>()) {
				if (f && f != Game::Gold()) {
					consider(f);
				}
			}
			return best;
		}

		// ---- dedup with other mods' transfers ----

		struct Transfer
		{
			RE::FormID from = 0;
			RE::FormID to = 0;
			RE::FormID item = 0;
			std::int32_t count = 0;
			Clock::time_point at;
		};

		std::mutex g_lock;
		std::deque<Transfer> g_transfers;

		// Promises kept recently, so a repeated mention doesn't pay twice.
		constexpr auto kKeptMemory = 10min;
		struct Kept
		{
			RE::FormID npc = 0;
			Promise promise;
			Clock::time_point at;
		};
		std::vector<Kept> g_kept;

		class ContainerWatcher : public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			static ContainerWatcher* Get()
			{
				static ContainerWatcher instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* e, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// NPC -> player (promises kept by another mod) and player -> NPC (gifts).
				const bool toPlayer = e && e->newContainer == 0x14 && e->oldContainer != 0;
				const bool fromPlayer = e && e->oldContainer == 0x14 && e->newContainer != 0 && e->newContainer != 0x14;
				if (fromPlayer) {
					logger::debug("transfer: player -> {:08X}: {:08X} x{}", e->newContainer, e->baseObj, e->itemCount);
				}
				if (toPlayer || fromPlayer) {
					std::scoped_lock l(g_lock);
					g_transfers.push_back({ e->oldContainer, e->newContainer, e->baseObj, e->itemCount, Clock::now() });
					while (g_transfers.size() > 128) {
						g_transfers.pop_front();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// True when this NPC already handed the player something matching, moments ago.
		bool AlreadyGiven(RE::Actor* npc, std::function<bool(RE::TESForm*)> matches)
		{
			std::scoped_lock l(g_lock);
			const auto cutoff = Clock::now() - kDedupWindow;
			for (auto& t : g_transfers) {
				if (t.at >= cutoff && t.from == npc->GetFormID() && t.to == 0x14 && matches(RE::TESForm::LookupByID(t.item))) {
					return true;
				}
			}
			return false;
		}

		void Remember(RE::Actor* npc, const std::string& what)
		{
			SkyrimNet::AddMemory(npc->GetFormID(), what, 0.4f);
		}

		// ---- the three promise kinds ----

		// ---- generic meals ----
		// "breakfast", "a hot meal", "something to eat": no item is called that, so the NPC serves
		// a real dish instead.

		const std::unordered_set<std::string> kMealWords{
			"breakfast", "lunch", "dinner", "supper", "meal", "food", "grub", "bite", "snack", "feast",
			"eat", "dish", "morning", "morn", "evening", "even", "midday", "noon", "daily", "hot", "warm"  // "morn"/"even": Stem() trims -ing
		};

		bool IsGenericMeal(const std::vector<std::string>& wanted)
		{
			return !wanted.empty() && std::ranges::all_of(wanted, [](const std::string& w) { return kMealWords.contains(w); });
		}

		bool IsFood(RE::TESForm* f)
		{
			auto* a = f ? f->As<RE::AlchemyItem>() : nullptr;
			return a && a->IsFood();
		}

		// The NPC's best dish on hand (the most valuable food they carry), else a common hot dish.
		Match PickMeal(RE::Actor* npc, std::int32_t maxValue)
		{
			Match best;
			for (auto* store : Stores(npc)) {
				for (auto& [obj, entry] : store->GetInventory()) {
					if (obj && entry.first > 0 && IsFood(obj) && obj->GetGoldValue() <= maxValue &&
						!(entry.second && entry.second->IsQuestObject()) &&
						obj->GetGoldValue() + 1.0 > best.score) {
						best = { obj, obj->GetGoldValue() + 1.0, store };
					}
				}
				if (best.obj) {
					return best;  // their own food first, then the shop's
				}
			}
			static const std::array<const char*, 6> kDishes{ "Vegetable Soup", "Beef Stew", "Cabbage Potato Soup", "Venison Stew", "Horker Stew", "Bread" };
			const auto& foods = RE::TESDataHandler::GetSingleton()->GetFormArray<RE::AlchemyItem>();
			for (auto* dish : kDishes) {
				for (auto* f : foods) {
					if (f && f->IsFood() && f->GetGoldValue() <= maxValue && _stricmp(f->GetName(), dish) == 0) {
						return { f, 0.0 };
					}
				}
			}
			return best;
		}

		// The item a give_item promise means: their own (pockets, then shop stock) unless the
		// world has a clearly better name match they could fetch.
		struct ItemChoice
		{
			Match own, world;
			RE::TESBoundObject* item = nullptr;
		};

		ItemChoice ChooseItem(RE::Actor* npc, const std::vector<std::string>& wanted, std::int32_t maxValue)
		{
			ItemChoice c;
			if (IsGenericMeal(wanted)) {
				auto dish = PickMeal(npc, maxValue);
				(dish.from ? c.own : c.world) = dish;
			} else {
				c.own = BestInInventory(npc, wanted);
				c.world = BestInWorld(wanted, maxValue);
			}
			c.item = c.own.obj && c.own.score + 1.0 >= c.world.score ? c.own.obj : c.world.obj;
			return c;
		}

		std::vector<std::string> WantedOf(const Promise& p)
		{
			auto wanted = Tokens(p.item, true);
			return wanted.empty() ? Tokens(p.item, false) : wanted;
		}

		Outcome GiveItem(RE::Actor* npc, const Promise& p)
		{
			auto* player = Game::Player();
			auto wanted = WantedOf(p);
			if (wanted.empty()) {
				return Outcome::kImpossible;
			}
			const bool meal = IsGenericMeal(wanted);
			if (AlreadyGiven(npc, [&](RE::TESForm* f) { return f && (meal ? IsFood(f) : Score(wanted, f->GetName()) > 0.0); })) {
				logger::info("give_item '{}': {} already handed it over (another mod)", p.item, npc->GetName());
				return Outcome::kKept;
			}

			const auto qty = std::clamp(p.qty, 1, kMaxItems);
			const auto maxValue = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.max_promise_item_value", 100.0f));

			auto choice = ChooseItem(npc, wanted, maxValue);
			auto& own = choice.own;
			auto* item = choice.item;
			if (!item) {
				logger::info("give_item '{}': no matching item for {}", p.item, npc->GetName());
				Game::DebugNotify(std::format("Mundane Matters: no item matches '{}'", p.item));
				return Outcome::kImpossible;
			}

			std::int32_t fromInventory = 0;
			if (item == own.obj && own.from) {
				fromInventory = (std::min)(qty, Game::ItemCount(own.from, item));
				own.from->RemoveItem(item, fromInventory, RE::ITEM_REMOVE_REASON::kStoreInTeammate, nullptr, player);
			}
			std::int32_t conjured = 0;
			if (fromInventory < qty && item->GetGoldValue() <= maxValue) {
				conjured = qty - fromInventory;
				player->AddObjectToContainer(item, nullptr, conjured, npc);
			}
			const auto given = fromInventory + conjured;
			if (given <= 0) {
				return Outcome::kImpossible;
			}

			const std::string name = item->GetName();
			logger::info("give_item '{}' -> {} x{} ({} from {}) from {}", p.item, name, given, fromInventory,
				fromInventory == 0 ? "nowhere" : own.from == npc ? "their pockets" : "their shop stock", npc->GetName());
			Game::Notify(given > 1 ? std::format("{} gave you {} ({}).", npc->GetName(), name, given) : std::format("{} gave you {}.", npc->GetName(), name));
			Remember(npc, std::format("{} gave {} {}{} as promised.", npc->GetName(), player->GetName(), given > 1 ? std::format("{} ", given) : "", name));
			return Outcome::kKept;
		}

		Outcome GiveGold(RE::Actor* npc, const Promise& p)
		{
			auto* player = Game::Player();
			auto* gold = Game::Gold();
			if (AlreadyGiven(npc, [gold](RE::TESForm* f) { return f == gold; })) {
				logger::info("give_gold: {} already paid (another mod)", npc->GetName());
				return Outcome::kKept;
			}
			const auto cap = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.max_promise_gold", 200.0f));
			const auto amount = std::clamp(p.gold > 0 ? p.gold : 10, 1, (std::max)(cap, 1));

			// Paid from their purse, then (merchants) the till; the rest is the "reward allowance"
			// (they fetch it from savings).
			std::int32_t fromPurse = 0;
			for (auto* store : Stores(npc)) {
				const auto take = (std::min)(amount - fromPurse, Game::ItemCount(store, gold));
				if (take > 0) {
					store->RemoveItem(gold, take, RE::ITEM_REMOVE_REASON::kStoreInTeammate, nullptr, player);
					fromPurse += take;
				}
			}
			if (amount > fromPurse) {
				player->AddObjectToContainer(gold, nullptr, amount - fromPurse, npc);
			}
			logger::info("give_gold {} (promised {}, {} from purse) from {}", amount, p.gold, fromPurse, npc->GetName());
			Game::Notify(std::format("{} gave you {} gold.", npc->GetName(), amount));
			Remember(npc, std::format("{} paid {} {} gold as promised.", npc->GetName(), player->GetName(), amount));
			return Outcome::kKept;
		}

		// Every record that is "her": the reference's base, the base a leveled/templated actor was
		// generated from, and its template chain. Ownership is often set on one of the others.
		std::vector<RE::TESForm*> Selves(RE::Actor* npc)
		{
			std::vector<RE::TESForm*> out;
			auto add = [&](RE::TESForm* f) {
				if (f && std::ranges::find(out, f) == out.end()) {
					out.push_back(f);
				}
			};
			auto* base = npc->GetActorBase();
			add(base);
			if (auto* x = npc->extraList.GetByType<RE::ExtraLeveledCreature>()) {
				add(x->originalBase);
			}
			for (std::size_t i = 0; i < out.size() && i < 8; ++i) {
				if (auto* n = out[i]->As<RE::TESNPC>()) {
					add(n->faceNPC);
					add(n->baseTemplateForm);
				}
			}
			return out;
		}

		bool IsHer(RE::TESForm* owner, RE::Actor* npc)
		{
			auto selves = Selves(npc);
			return owner && std::ranges::find(selves, owner) != selves.end();
		}

		bool OwnedBy(RE::TESForm* owner, RE::Actor* npc)
		{
			if (!owner) {
				return false;
			}
			if (IsHer(owner, npc)) {
				return true;
			}
			auto* faction = owner->As<RE::TESFaction>();
			return faction && npc->IsInFaction(faction);
		}

		// Is this interior the NPC's home, even with her outside? Registered to her (or her
		// household faction), or the place her character is assigned to in the game data.
		bool IsHomeOf(RE::TESObjectCELL* cell, RE::Actor* npc)
		{
			if (!cell || !cell->IsInteriorCell()) {
				return false;
			}
			if (OwnedBy(cell->GetOwner(), npc)) {
				return true;
			}
			auto* home = npc->GetEditorLocation1();
			if (home && cell->GetLocation() == home) {
				return true;
			}
			auto* persistent = npc->extraList.GetByType<RE::ExtraPersistentCell>();
			return persistent && persistent->persistentCell == cell;
		}

		// The NPC's own bed in the player's current interior: their personal bed first, then a bed of
		// their household faction, then an unowned bed in a cell they own. Nearest to the player wins.
		RE::TESObjectREFR* FindBed(RE::Actor* npc)
		{
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			if (!cell) {
				return nullptr;
			}
			const bool interior = cell->IsInteriorCell();
			const bool home = interior && IsHomeOf(cell, npc);
			auto* playerBase = player->GetActorBase();

			// Inn rooms are never lent for free, even when the innkeeper's family offers a bed.
			std::vector<RE::TESObjectREFR*> rentalBeds;
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					auto a = handle.get();
					if (a && a->GetParentCell() == cell) {
						if (auto room = Services::RoomOf(a.get()); room && room->bed) {
							rentalBeds.push_back(room->bed);
						}
					}
				}
			}
			auto isChild = [](RE::TESForm* owner) {
				auto* n = owner ? owner->As<RE::TESNPC>() : nullptr;
				return n && n->race && n->race->data.flags.any(RE::RACE_DATA::Flag::kChild);
			};
			RE::TESObjectREFR* best = nullptr;
			int bestRank = 0;
			float bestDist = (std::numeric_limits<float>::max)();
			auto consider = [&](RE::TESObjectREFR* ref) {
				auto* base = ref ? ref->GetBaseObject() : nullptr;
				auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
				if (!furn || !furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) || ref->IsDisabled() || ref->IsDeleted() ||
					std::ranges::find(rentalBeds, ref) != rentalBeds.end()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* xOwner = ref->extraList.GetByType<RE::ExtraOwnership>();
				auto* owner = xOwner ? xOwner->owner : nullptr;
				int rank = 0;
				if (owner == playerBase) {
					rank = 0;  // already the player's (a rented or lent bed)
				} else if (IsHer(owner, npc)) {
					rank = 4;
				} else if (OwnedBy(owner, npc)) {
					rank = 3;  // her household faction's
				} else if (home && !isChild(owner)) {
					// Her home records bed ownership loosely: nobody's bed, then a relative's
					// (never a child's).
					rank = owner ? 1 : 2;
				}
				const float dist = ref->GetPosition().GetDistance(player->GetPosition());
				if (rank > bestRank || (rank == bestRank && rank > 0 && dist < bestDist)) {
					best = ref;
					bestRank = rank;
					bestDist = dist;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			};
			if (interior) {
				cell->ForEachReference(consider);
			} else if (auto* tes = RE::TES::GetSingleton()) {
				// Outdoors (a camp, "my bedroll by the fire"): only beds that are really theirs, nearby.
				tes->ForEachReferenceInRange(player, 2000.0f, consider);
			}
			return bestRank > 0 ? best : nullptr;
		}

		Outcome OfferBed(RE::Actor* npc, const Promise&)
		{
			if (Ledger::HasBedLoan(npc->GetFormID())) {
				return Outcome::kKept;
			}
			auto* bed = FindBed(npc);
			if (!bed) {
				// Usually "come stay at my place": kept once the player is inside their home.
				// Logged once per cell, with what beds are there, so a miss can be diagnosed.
				static std::unordered_map<RE::FormID, RE::TESObjectCELL*> lastLogged;
				auto* cell = Game::Player()->GetParentCell();
				if (lastLogged[npc->GetFormID()] != cell) {
					lastLogged[npc->GetFormID()] = cell;
					if (IsHomeOf(cell, npc)) {
						Game::Notify(std::format("There's no bed here {} can lend you.", npc->GetName()));
					}
					std::string beds;
					int count = 0;
					if (cell && cell->IsInteriorCell()) {
						cell->ForEachReference([&](RE::TESObjectREFR* ref) {
							auto* base = ref ? ref->GetBaseObject() : nullptr;
							auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
							if (furn && furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep)) {
								auto* x = ref->extraList.GetByType<RE::ExtraOwnership>();
								auto* owner = x ? x->owner : nullptr;
								if (++count <= 6) {
									beds += std::format("{}{:08X} owner {}", beds.empty() ? "" : ", ", ref->GetFormID(),
										owner ? std::format("{} ({:08X})", owner->GetName(), owner->GetFormID()) : std::string("none"));
								}
							}
							return RE::BSContainer::ForEachResult::kContinue;
						});
					}
					const auto* cellOwner = cell ? cell->GetOwner() : nullptr;
					auto* home = npc->GetEditorLocation1();
					logger::info("offer_bed: no bed of {} (base {:08X}) here; cell {} owner {}, her home: {} (home location {}, this {}); {} beds{}{}",
						npc->GetName(), npc->GetActorBase() ? npc->GetActorBase()->GetFormID() : 0,
						cell ? (cell->IsInteriorCell() ? "interior" : "exterior") : "none",
						cellOwner ? cellOwner->GetName() : "none", IsHomeOf(cell, npc) ? "yes" : "no",
						home ? home->GetName() : "none", cell && cell->GetLocation() ? cell->GetLocation()->GetName() : "none",
						count, beds.empty() ? "" : ": ", beds);
				}
				return Outcome::kNotYet;
			}
			const float hours = SkyrimNet::ConfigFloat("tuning.bed_hours", 24.0f);
			Ledger::LoanBed(npc, bed, hours);
			Game::Notify(std::format("You may sleep in {}'s bed.", npc->GetName()));
			Remember(npc, std::format("{} let {} sleep in their bed.", npc->GetName(), Game::Player()->GetName()));
			return Outcome::kKept;
		}
	}

	// ================================================================ the player giving

	namespace
	{
		// The player already handed it over moments ago (SeverActions' TakeItem / CollectPayment).
		bool AlreadyReceived(RE::Actor* npc, std::function<bool(RE::TESForm*)> matches, std::optional<Clock::time_point> since = std::nullopt)
		{
			std::scoped_lock l(g_lock);
			const auto cutoff = since.value_or(Clock::now() - kDedupWindow);
			return std::ranges::any_of(g_transfers, [&](const Transfer& t) {
				return t.at >= cutoff && t.from == 0x14 && t.to == npc->GetFormID() && matches(RE::TESForm::LookupByID(t.item));
			});
		}

		// ---- gifts and how the NPC feels about you ----
		// Rare by design: only the first gift in a 7-day window is considered, it must be really
		// good for them, they must be genuinely pleased (judged from their reaction), and gifts
		// can raise an NPC's rank by one step in total, never past Confidant.

		struct AwaitedReaction
		{
			std::string gift;
			Clock::time_point until;
		};
		std::unordered_map<RE::FormID, AwaitedReaction> g_awaiting;

		// Worth 100+, or something that suits them: food for someone poor, arms for a fighter,
		// ingredients or potions for an alchemist.
		bool ReallyGood(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t value)
		{
			if (value >= 100) {
				return true;
			}
			if (!item || value < 10) {
				return false;
			}
			auto* base = npc->GetActorBase();
			const auto cls = JsonUtil::Lower(base && base->npcClass && base->npcClass->GetName() ? base->npcClass->GetName() : "");
			auto has = [&](const char* w) { return cls.find(w) != std::string::npos; };
			if (IsFood(item) && (Game::ItemCount(npc, Game::Gold()) < 20 || has("beggar"))) {
				return true;
			}
			if ((item->IsWeapon() || item->IsArmor()) && (has("guard") || has("warrior") || has("soldier") || has("knight") || has("barbarian"))) {
				return true;
			}
			if ((item->Is(RE::FormType::Ingredient) || item->Is(RE::FormType::AlchemyItem)) && !IsFood(item) && (has("alchem") || has("apothecar"))) {
				return true;
			}
			return false;
		}

		void ConsiderWarmth(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t value, const std::string& what)
		{
			if (!SkyrimNet::ConfigBool("features.gift_disposition", true)) {
				return;
			}
			const auto id = npc->GetFormID();
			if (value < 10) {
				return;  // a token (a coin, an apple) doesn't use up the week's one chance
			}
			if (!Ledger::GiftCounts(id)) {
				return;  // not the first real gift of the window, or the one step is already spent
			}
			if (!ReallyGood(npc, item, value)) {
				logger::info("gift: {} - not special enough to change how they feel ({} worth {})", npc->GetName(), what, value);
				return;
			}
			std::scoped_lock l(g_lock);
			g_awaiting[id] = { what, Clock::now() + 45s };
			logger::info("gift: waiting for {}'s reaction to {}", npc->GetName(), what);
		}

		void RaiseOneStep(RE::FormID id)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
			auto* base = npc ? npc->GetActorBase() : nullptr;
			auto* playerBase = Game::Player()->GetActorBase();
			if (!base || !playerBase) {
				return;
			}
			auto* rel = RE::BGSRelationship::GetRelationship(base, playerBase);
			using L = RE::BGSRelationship::RELATIONSHIP_LEVEL;
			static const std::unordered_map<L, int> kRank{ { L::kLover, 4 }, { L::kAlly, 3 }, { L::kConfidant, 2 }, { L::kFriend, 1 },
				{ L::kAcquaintance, 0 }, { L::kRival, -1 }, { L::kFoe, -2 }, { L::kEnemy, -3 }, { L::kArchnemesis, -4 } };
			const auto found = rel ? kRank.find(rel->level.get()) : kRank.end();
			const int rank = found != kRank.end() ? found->second : 0;
			if (rank >= 2) {
				logger::info("gift: {} is already confidant or closer; no change", npc->GetName());
				return;
			}
			Papyrus::CallMethod(npc, "Actor", "SetRelationshipRank", nullptr, static_cast<RE::Actor*>(Game::Player()), static_cast<std::int32_t>(rank + 1));
			Ledger::MarkWarmed(id);
			logger::info("gift: {} relationship {} -> {} (the one step gifts can give)", npc->GetName(), rank, rank + 1);
			Game::DebugNotify(std::format("{} thinks better of you.", npc->GetName()));
		}

		// After the handover: the NPC reacts to what they really got, remembers it, maybe warms up.
		void Received(RE::Actor* npc, const std::string& what, std::int32_t value, bool alreadySpoke, RE::TESBoundObject* mainItem)
		{
			const std::string player = Game::Player()->GetName();
			logger::info("gift: {} gave {} {} (worth {})", player, npc->GetName(), what, value);
			Game::Notify(std::format("You gave {} {}.", npc->GetName(), what));
			if (!alreadySpoke) {
				Game::Narrate(std::format("{} gave {} {}.", player, npc->GetName(), what), npc);
			} else {
				SkyrimNet::AddMemory(npc->GetFormID(), std::format("{} gave {} {}.", player, npc->GetName(), what), 0.4f);
			}
			ConsiderWarmth(npc, mainItem, value, what);
		}

		std::string Count(const std::string& name, std::int32_t n)
		{
			return n > 1 ? std::format("{} x{}", name, n) : name;
		}

		// Popup (or straight through when off / no PrismaUI), then `give()`; the player's chosen
		// line is said, the NPC answers the handover itself.
		void ConfirmGift(RE::FormID npcId, const std::string& detail, std::function<void()> give)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc) {
				return;
			}
			if (!SkyrimNet::ConfigBool("features.confirm_gift", true) || !Confirm::Available()) {
				give();
				return;
			}
			const std::string accept = Game::Pick({ "Here, take it.", "It's yours.", "Please, I want you to have it." });
			const std::string decline = Game::Pick({ "On second thought, I'll keep it.", "Actually, never mind.", "Hm, maybe another time." });
			Confirm::Offer offer;
			offer.title = "Give";
			offer.npc = npc->GetName();
			offer.detail = detail;
			offer.showPrice = false;
			offer.seconds = 0;  // gifts wait for the player
			offer.acceptLabel = accept;
			offer.declineLabel = decline;
			const bool shown = Confirm::Ask(offer, [npcId, give, accept, decline](Confirm::Choice choice) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || choice == Confirm::Choice::kTimeout) {
					return;  // unanswered: nothing moves, nothing said
				}
				if (choice == Confirm::Choice::kDecline) {
					Game::Say(decline, npc, true);
					return;
				}
				Game::Say(accept, npc, false);  // their reaction to the handover answers it
				give();
			});
			if (!shown) {
				logger::info("gift: a popup is already open, gift ignored");
			}
		}

		// The vanilla gift menu for "I have something for you": the player picks; on close the NPC
		// is told exactly what they got.
		RE::FormID g_giftMenuNpc = 0;
		Clock::time_point g_giftMenuOpened;
		// What the player carried when the menu opened: the menu doesn't announce each handover,
		// so what's missing afterwards (and now in the NPC's pack) is what they gave.
		std::unordered_map<RE::TESBoundObject*, std::int32_t> g_giftSnapshot;

		void OpenGiftMenu(RE::Actor* npc)
		{
			if (!SkyrimNet::ConfigBool("features.gift_menu", true)) {
				return;
			}
			g_giftMenuNpc = npc->GetFormID();
			g_giftMenuOpened = Clock::now();
			g_giftSnapshot.clear();
			for (auto& [obj, count] : Game::Player()->GetInventoryCounts()) {
				if (obj && count > 0) {
					g_giftSnapshot[obj] = count;
				}
			}
			logger::info("gift: opening the gift menu for {} ({} kinds of item carried)", npc->GetName(), g_giftSnapshot.size());
			Papyrus::CallMethod(npc, "Actor", "ShowGiftMenu", nullptr, false, static_cast<RE::BGSListForm*>(nullptr), false, false);
		}

		// "I have something for you": the player picks from their own pack in MundaneMatters' popup (the
		// vanilla gift menu doesn't reliably hand items to non-followers).
		bool OpenPicker(RE::Actor* npc)
		{
			if (!Confirm::Available()) {
				return false;
			}
			Confirm::Offer offer;
			offer.title = "Give";
			offer.npc = npc->GetName();
			offer.detail = "Pick something to give";
			offer.showPrice = false;
			offer.seconds = 0;  // take your time choosing
			offer.acceptLabel = "Give";
			offer.declineLabel = "Never mind";
			auto* player = Game::Player();
			for (auto& [obj, entry] : player->GetInventory()) {
				if (!obj || entry.first <= 0 || !obj->GetName() || !*obj->GetName() || (entry.second && entry.second->IsQuestObject())) {
					continue;
				}
				offer.pick.push_back({ obj->GetFormID(), obj->GetName(), entry.first, obj == Game::Gold() ? 1 : obj->GetGoldValue(),
					entry.second && entry.second->IsWorn() });
			}
			if (offer.pick.empty()) {
				return false;
			}
			// Gold first, then the most valuable; a long pack is cut to the 60 that matter.
			std::ranges::sort(offer.pick, [](auto& a, auto& b) {
				const bool ag = a.id == 0xF, bg = b.id == 0xF;
				return ag != bg ? ag : a.value > b.value;
			});
			if (offer.pick.size() > 60) {
				offer.pick.resize(60);
			}
			const auto npcId = npc->GetFormID();
			const bool shown = Confirm::Ask(offer, [npcId](Confirm::Choice choice) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				auto* player = Game::Player();
				if (!npc || choice == Confirm::Choice::kTimeout) {
					return;
				}
				if (choice == Confirm::Choice::kDecline) {
					Game::Say(Game::Pick({ "Actually, never mind.", "On second thought, I'll keep it.", "Hm, maybe another time." }), npc, true);
					return;
				}
				const auto [id, qty] = Confirm::LastPick();
				auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
				const auto n = item ? (std::min)(qty, Game::ItemCount(player, item)) : 0;
				if (n <= 0) {
					return;
				}
				player->RemoveItem(item, n, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, npc);
				Game::Say(Game::Pick({ "Here, I want you to have this.", "This is for you.", "Please, take it." }), npc, false);
				const auto what = item == Game::Gold() ? std::format("{} gold", n) : Count(item->GetName(), n);
				Received(npc, what, item == Game::Gold() ? n : item->GetGoldValue() * n, false, item);
			});
			return shown;
		}

		void OnGiftMenuClosed()
		{
			const auto npcId = std::exchange(g_giftMenuNpc, 0);
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			// Our menu never opened (the call failed) and this is some later gift menu: not ours.
			if (!npc || Clock::now() - g_giftMenuOpened > 10min) {
				return;
			}
			// Whatever the player has less of now, and the NPC holds, went to the NPC.
			std::map<RE::FormID, std::int32_t> given;
			auto now = Game::Player()->GetInventoryCounts();
			for (auto& [obj, before] : g_giftSnapshot) {
				const auto after = now.contains(obj) ? now.at(obj) : 0;
				if (after < before && Game::ItemCount(npc, obj) > 0) {
					given[obj->GetFormID()] = (std::min)(before - after, Game::ItemCount(npc, obj));
				} else if (after < before) {
					logger::info("gift menu: {} left the player's pack but isn't in {}'s", obj->GetName(), npc->GetName());
				}
			}
			g_giftSnapshot.clear();
			for (auto& [id, n] : given) {
				logger::info("gift menu: {} x{} to {}", RE::TESForm::LookupByID(id) ? RE::TESForm::LookupByID(id)->GetName() : "?", n, npc->GetName());
			}
			if (given.empty()) {
				logger::info("gift: menu closed, nothing given");
				return;
			}
			std::string list;
			std::int32_t value = 0;
			RE::TESBoundObject* first = nullptr;
			for (auto& [id, n] : given) {
				auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
				if (!obj) {
					continue;
				}
				first = first ? first : obj;
				list += (list.empty() ? "" : ", ") + Count(obj->GetName(), n);
				value += obj->GetGoldValue() * n;
			}
			Received(npc, list, value, false, first);
		}

		class GiftMenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static GiftMenuWatcher* Get()
			{
				static GiftMenuWatcher instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (e && !e->opening && e->menuName == RE::GiftMenu::MENU_NAME && g_giftMenuNpc) {
					// The menu hands items over as it closes: count them a moment later.
					std::thread([] {
						std::this_thread::sleep_for(1200ms);
						SKSE::GetTaskInterface()->AddTask(OnGiftMenuClosed);
					}).detach();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void OnNpcReply(RE::FormID npc, const std::string& line)
	{
		std::string gift;
		{
			std::scoped_lock l(g_lock);
			auto it = g_awaiting.find(npc);
			if (it == g_awaiting.end()) {
				return;
			}
			const bool fresh = Clock::now() <= it->second.until;
			gift = it->second.gift;
			g_awaiting.erase(it);
			if (!fresh) {
				return;
			}
		}
		// One small call: were they genuinely pleased, or just polite?
		json ctx{ { "gift", gift }, { "line", line } };
		auto onReply = [npc, gift](const std::string& raw, bool success) {
			try {
				if (!success) {
					return;
				}
				const auto lower = JsonUtil::Lower(raw);
				const bool liked = lower.find("delighted") != std::string::npos || lower.find("\"pleased\"") != std::string::npos;
				logger::info("gift: reaction to {} -> {}", gift, liked ? "genuinely pleased" : "not enough");
				if (liked) {
					SKSE::GetTaskInterface()->AddTask([npc] { RaiseOneStep(npc); });
				}
			} catch (...) {
			}
		};
		if (!SkyrimNet::SendPrompt("mundanematters_gift_reaction", "mundanematters_classifier", ctx, onReply)) {
			SkyrimNet::SendPrompt("mundanematters_gift_reaction", "", ctx, onReply);
		}
	}

	std::optional<Gift> GiftFromJson(const json& j)
	{
		if (!j.is_object()) {
			return std::nullopt;
		}
		Gift g;
		g.item = JsonUtil::Str(j, "item");
		g.qty = static_cast<std::int32_t>(std::clamp(JsonUtil::Num(j, "qty").value_or(1.0), 1.0, 1000.0));
		g.gold = static_cast<std::int32_t>(std::clamp(JsonUtil::Num(j, "gold").value_or(0.0), 0.0, 1e6));
		// "50 coins" written as an item is coin.
		auto words = Tokens(g.item, true);
		std::int32_t amount = 0;
		std::erase_if(words, [&](const std::string& w) {
			if (!w.empty() && std::ranges::all_of(w, [](unsigned char c) { return std::isdigit(c); })) {
				amount = std::atoi(w.c_str());
				return true;
			}
			return false;
		});
		if (!words.empty() && std::ranges::all_of(words, [](const std::string& w) { return w == "gold"; })) {
			g.gold = g.gold > 0 ? g.gold : amount > 0 ? amount : g.qty > 1 ? g.qty : 0;
			g.item.clear();
		}
		// An empty entry is a model copying the template, not a gift: the gift menu only opens
		// for one explicitly marked unnamed.
		const auto unnamed = JsonUtil::Lower(JsonUtil::Str(j, "unnamed"));
		if (g.gold <= 0 && words.empty() && unnamed != "true" && unnamed != "yes" && unnamed != "1") {
			return std::nullopt;
		}
		return g;
	}

	void Receive(RE::Actor* npc, const std::vector<Gift>& gifts)
	{
		if (!npc || npc->IsDead() || !SkyrimNet::ConfigBool("features.gifts", true) || gifts.empty()) {
			return;
		}
		auto* player = Game::Player();
		auto* gold = Game::Gold();

		// Everything handed over in one line becomes one popup; only what the player really carries.
		struct Part
		{
			RE::TESBoundObject* item = nullptr;
			std::int32_t qty = 0;
			std::string label;
		};
		std::vector<Part> parts;
		std::vector<std::string> missing;
		bool unnamed = false;
		std::int32_t coinPlanned = 0;
		for (auto& g : gifts) {
			if (g.gold > 0) {
				const auto purse = Game::ItemCount(player, gold) - coinPlanned;
				if (purse <= 0) {
					missing.push_back("coin");
					continue;
				}
				if (AlreadyReceived(npc, [gold](RE::TESForm* f) { return f == gold; })) {
					logger::info("gift: {} already got the coin (another mod)", npc->GetName());
					continue;
				}
				const auto amount = std::clamp(g.gold, 1, purse);
				coinPlanned += amount;
				parts.push_back({ gold, amount, std::format("{} gold{}", amount, amount < g.gold ? " (all you have)" : "") });
				continue;
			}
			auto wanted = Tokens(g.item, true);
			if (wanted.empty()) {
				unnamed = true;  // "I have something for you"
				continue;
			}
			// "Some food" = their cheapest food; otherwise the closest name in their pack.
			Match m;
			if (IsGenericMeal(wanted)) {
				for (auto& [obj, entry] : player->GetInventory()) {
					if (obj && entry.first > 0 && IsFood(obj) && !(entry.second && entry.second->IsQuestObject()) &&
						(!m.obj || obj->GetGoldValue() < m.obj->GetGoldValue())) {
						m = { obj, 1.0, player, entry.second && entry.second->IsWorn() };
					}
				}
			} else {
				m = BestIn(player, wanted, true);
			}
			if (!m.obj) {
				missing.push_back(g.item);
				continue;
			}
			auto* item = m.obj;
			if (AlreadyReceived(npc, [item](RE::TESForm* f) { return f == item; })) {
				logger::info("gift: {} already got {} (another mod)", npc->GetName(), item->GetName());
				continue;
			}
			const auto have = Game::ItemCount(player, item);
			const auto qty = std::clamp(g.qty, 1, (std::max)(1, have));
			parts.push_back({ item, qty,
				std::format("{}{}{}", Count(item->GetName(), qty), qty < g.qty ? " (all you have)" : "", m.worn ? " (equipped)" : "") });
		}

		std::string missingText;
		for (auto& m : missing) {
			missingText += (missingText.empty() ? "" : ", ") + m;
		}
		if (parts.empty()) {
			if (!missing.empty()) {
				logger::info("gift: player has none of: {}", missingText);
				Game::Narrate(std::format("{} reached into their pack for {} to give {}, but didn't have any.", player->GetName(), missingText, npc->GetName()), npc);
			} else if (unnamed) {
				// The player picks: MundaneMatters' picker, or the vanilla gift menu without PrismaUI.
				if (!SkyrimNet::ConfigBool("features.gift_menu", true) || !OpenPicker(npc)) {
					OpenGiftMenu(npc);
				}
			}
			return;
		}

		std::string detail;
		for (auto& p : parts) {
			detail += (detail.empty() ? "" : ", ") + p.label;
		}
		const auto npcId = npc->GetFormID();
		const auto askedAt = Clock::now();
		ConfirmGift(npcId, detail, [npcId, parts, missingText, askedAt] {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* player = Game::Player();
			if (!npc) {
				return;
			}
			std::string given;
			std::int32_t value = 0;
			RE::TESBoundObject* first = nullptr;
			for (auto& p : parts) {
				// Taken by another mod while the popup was open, or dropped / sold meanwhile.
				auto* item = p.item;
				if (AlreadyReceived(npc, [item](RE::TESForm* f) { return f == item; }, askedAt)) {
					continue;
				}
				const auto n = (std::min)(p.qty, Game::ItemCount(player, item));
				if (n <= 0) {
					continue;
				}
				player->RemoveItem(item, n, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, npc);
				first = first ? first : item;
				given += (given.empty() ? "" : ", ") + (item == Game::Gold() ? std::format("{} gold", n) : Count(item->GetName(), n));
				value += item == Game::Gold() ? n : item->GetGoldValue() * n;
			}
			if (given.empty()) {
				Game::Narrate(std::format("{} went to hand it over, but no longer had it.", player->GetName()), npc);
				return;
			}
			if (!missingText.empty()) {
				given += std::format(" (they had no {} to give)", missingText);
			}
			Received(npc, given, value, false, first);
		});
	}

	std::optional<Promise> FromJson(const json& j)
	{
		if (!j.is_object()) {
			return std::nullopt;
		}
		Promise p;
		// Models drift from the given type names: "give_money", "gift", "lodging", ...
		const auto type = JsonUtil::Lower(JsonUtil::Str(j, "type"));
		if (type.find("gold") != std::string::npos || type.find("money") != std::string::npos || type.find("coin") != std::string::npos ||
			type.find("pay") != std::string::npos) {
			p.type = "give_gold";
		} else if (type.find("bed") != std::string::npos || type.find("sleep") != std::string::npos || type.find("lodg") != std::string::npos ||
				   type.find("stay") != std::string::npos || type.find("room") != std::string::npos) {
			p.type = "offer_bed";
		} else if (type.find("take") != std::string::npos || type.find("allow") != std::string::npos || type.find("help_yourself") != std::string::npos) {
			p.type = "allow_take";
		} else if (type.find("item") != std::string::npos || type.find("gift") != std::string::npos || type == "give") {
			p.type = "give_item";
		} else {
			return std::nullopt;
		}

		p.item = JsonUtil::Str(j, "item");
		p.qty = static_cast<std::int32_t>(std::clamp(JsonUtil::Num(j, "qty").value_or(1.0), 1.0, 1000.0));
		p.gold = static_cast<std::int32_t>(std::clamp(JsonUtil::Num(j, "gold").value_or(0.0), 0.0, 1e6));
		p.timeHint = JsonUtil::Str(j, "time_hint");
		p.inHours = static_cast<float>(std::clamp(JsonUtil::Num(j, "in_hours").value_or(0.0), 0.0, 24.0 * 30));
		const auto always = JsonUtil::Lower(JsonUtil::Str(j, "always"));
		p.always = always == "true" || always == "yes" || always == "1";

		// "when" should be now/later/cancel; "tomorrow", "at dawn", "soon" all mean later.
		const auto when = JsonUtil::Lower(JsonUtil::Str(j, "when", "now"));
		if (when.find("cancel") != std::string::npos || when.find("retract") != std::string::npos || when.find("revok") != std::string::npos) {
			p.when = "cancel";
		} else if (when.empty() || when == "now" || when.find("immediate") != std::string::npos || when.find("right away") != std::string::npos) {
			p.when = p.inHours >= 1.0f ? "later" : "now";
		} else {
			p.when = "later";
			if (p.timeHint.empty() && when != "later") {
				p.timeHint = when;
			}
		}

		if (p.type == "give_gold" && p.gold == 0) {
			// "gold": "" but "item": "fifty septims"
			p.gold = static_cast<std::int32_t>(JsonUtil::ParseNumber(p.item).value_or(0.0));
		}
		if (p.type == "allow_take" && Tokens(p.item, true).empty()) {
			return std::nullopt;  // "sure, take it" with nothing named
		}
		if (p.type == "give_item") {
			// "Here, take these coins" sometimes comes back as an item called "coins".
			auto words = Tokens(p.item, true);
			std::int32_t amount = 0;
			std::erase_if(words, [&](const std::string& w) {
				if (!w.empty() && std::ranges::all_of(w, [](unsigned char c) { return std::isdigit(c); })) {
					amount = std::atoi(w.c_str());  // "10 coins"
					return true;
				}
				return false;
			});
			if (!words.empty() && std::ranges::all_of(words, [](const std::string& w) { return w == "gold"; })) {
				p.type = "give_gold";
				p.gold = p.gold > 0 ? p.gold : amount > 0 ? amount : p.qty > 1 ? p.qty : 0;
				p.item.clear();
			} else if (words.empty()) {
				return std::nullopt;  // "here, take this": nothing nameable to hand over
			}
		}
		return p;
	}

	json ToJson(const Promise& p)
	{
		return { { "type", p.type }, { "item", p.item }, { "qty", p.qty }, { "gold", p.gold }, { "when", p.when }, { "time_hint", p.timeHint },
			{ "in_hours", p.inHours }, { "always", p.always } };
	}

	std::string Describe(const Promise& p)
	{
		if (p.type == "give_gold") {
			return std::format("{} gold", p.gold > 0 ? p.gold : 10);
		}
		if (p.type == "offer_bed") {
			return p.always ? "a place to stay any time" : "a place to sleep";
		}
		if (p.type == "allow_take") {
			return std::format("permission to take {}", p.item);
		}
		return p.qty > 1 ? std::format("{} {}", p.qty, p.item) : p.item;
	}

	bool Same(const Promise& a, const Promise& b)
	{
		if (a.type != b.type) {
			return false;
		}
		if (a.type != "give_item" && a.type != "allow_take") {
			return true;
		}
		const auto x = JsonUtil::Lower(a.item), y = JsonUtil::Lower(b.item);
		return x == y || x.find(y) != std::string::npos || y.find(x) != std::string::npos;
	}

	Generosity GenerosityOf(RE::Actor* npc)
	{
		if (!npc) {
			return {};
		}
		if (npc->IsPlayerTeammate()) {
			return { 6.0f, 80 };  // companions share like allies
		}
		auto* base = npc->GetActorBase();
		auto* playerBase = Game::Player()->GetActorBase();
		auto* rel = base && playerBase ? RE::BGSRelationship::GetRelationship(base, playerBase) : nullptr;
		using L = RE::BGSRelationship::RELATIONSHIP_LEVEL;
		switch (rel ? rel->level.get() : L::kAcquaintance) {
		case L::kLover:
			return { 5.0f, 100 };
		case L::kAlly:
			return { 6.0f, 80 };
		case L::kConfidant:
			return { 12.0f, 60 };
		case L::kFriend:
			return { 12.0f, 20 };
		case L::kAcquaintance:
			return { 24.0f, 10 };
		default:
			return { 24.0f, 0, true };  // rival, foe, enemy: nothing for free
		}
	}

	namespace
	{
		std::vector<RE::TESObjectREFR*> TakeTargets(RE::Actor* npc, const Promise& p);  // defined below
	}

	bool IsClose(RE::Actor* npc)
	{
		if (!npc) {
			return false;
		}
		if (npc->IsPlayerTeammate()) {
			return true;
		}
		auto* base = npc->GetActorBase();
		auto* playerBase = Game::Player()->GetActorBase();
		auto* rel = base && playerBase ? RE::BGSRelationship::GetRelationship(base, playerBase) : nullptr;
		using L = RE::BGSRelationship::RELATIONSHIP_LEVEL;
		const auto level = rel ? rel->level.get() : L::kAcquaintance;
		return level == L::kFriend || level == L::kConfidant || level == L::kAlly || level == L::kLover;
	}

	std::string ServeMeal(RE::Actor* npc)
	{
		const auto maxValue = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.max_promise_item_value", 100.0f));
		auto dish = PickMeal(npc, maxValue);
		if (!dish.obj) {
			return "";
		}
		auto* player = Game::Player();
		if (dish.from && Game::ItemCount(dish.from, dish.obj) > 0) {
			dish.from->RemoveItem(dish.obj, 1, RE::ITEM_REMOVE_REASON::kStoreInTeammate, nullptr, player);
		} else {
			player->AddObjectToContainer(dish.obj, nullptr, 1, npc);
		}
		const std::string name = dish.obj->GetName();
		Game::Notify(std::format("{} shares {} with you.", npc->GetName(), name));
		Remember(npc, std::format("{} shared a meal of {} with {}.", npc->GetName(), name, player->GetName()));
		return name;
	}

	std::int32_t HandoutCost(RE::Actor* npc, const Promise& p)
	{
		if (p.type == "give_gold") {
			const auto cap = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.max_promise_gold", 200.0f));
			return std::clamp(p.gold > 0 ? p.gold : 10, 1, (std::max)(cap, 1));
		}
		if (p.type == "allow_take" && npc) {
			std::int32_t total = 0;
			for (auto* ref : TakeTargets(npc, p)) {
				auto* base = ref->GetBaseObject();
				const auto v = (std::max)(1, base->GetGoldValue());
				total += IsFood(base) ? (std::max)(1, (v + 1) / 2) : v;
			}
			return total;
		}
		if (p.type != "give_item" || !npc) {
			return 0;
		}
		const auto wanted = WantedOf(p);
		const auto maxValue = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.max_promise_item_value", 100.0f));
		auto* item = wanted.empty() ? nullptr : ChooseItem(npc, wanted, maxValue).item;
		if (!item) {
			return 0;  // nothing to give: Keep says so
		}
		const auto qty = std::clamp(p.qty, 1, kMaxItems);
		const auto full = (std::max)(1, item->GetGoldValue()) * qty;
		return IsFood(item) ? (std::max)(1, (full + 1) / 2) : full;  // food is easy to spare
	}

	namespace
	{
		// Items the player looked at recently (newest last).
		std::deque<std::pair<RE::FormID, Clock::time_point>> g_lookedAt;

		bool IsLooseItem(RE::TESObjectREFR* ref)
		{
			auto* base = ref ? ref->GetBaseObject() : nullptr;
			if (!base || ref->IsDisabled() || ref->IsDeleted() || ref->Is(RE::FormType::ActorCharacter)) {
				return false;
			}
			switch (base->GetFormType()) {
			case RE::FormType::AlchemyItem:
			case RE::FormType::Ingredient:
			case RE::FormType::Misc:
			case RE::FormType::Weapon:
			case RE::FormType::Armor:
			case RE::FormType::Book:
			case RE::FormType::Ammo:
			case RE::FormType::SoulGem:
			case RE::FormType::Scroll:
				return true;
			default:
				return false;
			}
		}

		// Objects lying in the NPC's home that are theirs to let go of, best match first: the one
		// the player looked at, then the closest. Never shops or inns (that's trade).
		std::vector<RE::TESObjectREFR*> TakeTargets(RE::Actor* npc, const Promise& p)
		{
			std::vector<RE::TESObjectREFR*> out;
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			if (!cell || !cell->IsInteriorCell() || !IsHomeOf(cell, npc)) {
				return out;
			}
			if (auto* loc = cell->GetLocation(); loc && (loc->HasKeywordString("LocTypeStore") || loc->HasKeywordString("LocTypeInn"))) {
				return out;
			}
			const auto wanted = WantedOf(p);
			if (wanted.empty()) {
				return out;
			}
			auto* playerBase = player->GetActorBase();
			std::vector<std::pair<RE::TESObjectREFR*, double>> found;
			std::unordered_set<RE::FormID> recent;
			{
				std::scoped_lock l(g_lock);
				const auto cutoff = Clock::now() - 30s;
				for (auto& [id, at] : g_lookedAt) {
					if (at >= cutoff) {
						recent.insert(id);
					}
				}
			}
			cell->ForEachReference([&](RE::TESObjectREFR* ref) {
				if (!IsLooseItem(ref)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* x = ref->extraList.GetByType<RE::ExtraOwnership>();
				auto* owner = x ? x->owner : nullptr;
				// Hers, her household's, or nobody's in her home; never someone else's or already the player's.
				if (owner == playerBase || (owner && !OwnedBy(owner, npc))) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const double s = Score(wanted, ref->GetBaseObject()->GetName());
				if (s <= 0.0) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const float dist = ref->GetPosition().GetDistance(player->GetPosition());
				if (dist > 2500.0f) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// Looked at moments ago beats everything; then name match, then nearness.
				found.emplace_back(ref, (recent.contains(ref->GetFormID()) ? 1000.0 : 0.0) + s * 10.0 - dist / 1000.0);
				return RE::BSContainer::ForEachResult::kContinue;
			});
			std::ranges::sort(found, [](auto& a, auto& b) { return a.second > b.second; });
			const auto qty = static_cast<std::size_t>(std::clamp(p.qty, 1, kMaxItems));
			for (auto& [ref, score] : found) {
				if (out.size() >= qty) {
					break;
				}
				out.push_back(ref);
			}
			return out;
		}

		Outcome AllowTake(RE::Actor* npc, const Promise& p)
		{
			if (!SkyrimNet::ConfigBool("features.allow_take", true)) {
				return Outcome::kImpossible;
			}
			auto targets = TakeTargets(npc, p);
			if (targets.empty()) {
				logger::info("allow_take '{}': nothing of {}'s like that here (homes only)", p.item, npc->GetName());
				Game::DebugNotify(std::format("Mundane Matters: no '{}' of {}'s here to take", p.item, npc->GetName()));
				return Outcome::kImpossible;
			}
			for (auto* ref : targets) {
				Ledger::AllowTake(ref, 24.0f);
			}
			const std::string name = targets.front()->GetBaseObject()->GetName();
			const auto what = targets.size() > 1 ? std::format("{} ({})", name, targets.size()) : name;
			logger::info("allow_take '{}' -> {} may be taken from {}'s home", p.item, what, npc->GetName());
			Game::Notify(std::format("You may take the {}.", what));
			Remember(npc, std::format("{} let {} take {} from their home.", npc->GetName(), Game::Player()->GetName(), what));
			return Outcome::kKept;
		}
	}

	void SampleCrosshair()
	{
		auto* pick = RE::CrosshairPickData::GetSingleton();
		auto target = pick ? pick->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>();
		if (!target || !IsLooseItem(target.get())) {
			return;
		}
		std::scoped_lock l(g_lock);
		if (g_lookedAt.empty() || g_lookedAt.back().first != target->GetFormID()) {
			g_lookedAt.emplace_back(target->GetFormID(), Clock::now());
			while (g_lookedAt.size() > 8) {
				g_lookedAt.pop_front();
			}
		} else {
			g_lookedAt.back().second = Clock::now();
		}
	}

	RE::TESObjectREFR* FindBedFor(RE::Actor* npc)
	{
		return npc ? FindBed(npc) : nullptr;
	}

	bool WasKept(RE::FormID npc, const Promise& p, std::chrono::seconds within)
	{
		std::scoped_lock l(g_lock);
		const auto cutoff = Clock::now() - within;
		return std::ranges::any_of(g_kept, [&](const Kept& k) { return k.at >= cutoff && k.npc == npc && Same(k.promise, p); });
	}

	Outcome Keep(RE::Actor* npc, const Promise& p)
	{
		if (!npc || npc->IsDead()) {
			return Outcome::kImpossible;
		}
		// "Enjoy the soup I gave you" a few lines later is not a second bowl.
		{
			std::scoped_lock l(g_lock);
			const auto cutoff = Clock::now() - kKeptMemory;
			std::erase_if(g_kept, [&](const Kept& k) { return k.at < cutoff; });
			// Beds only: a second bowl or coin is a real ask that the generosity budget decides.
			const bool handout = p.type == "give_item" || p.type == "give_gold" || p.type == "allow_take";
			for (auto& k : g_kept) {
				if (!handout && k.npc == npc->GetFormID() && Same(k.promise, p)) {
					logger::info("{}: '{}' was already kept a moment ago", npc->GetName(), Describe(p));
					return Outcome::kKept;
				}
			}
		}
		Outcome outcome = Outcome::kImpossible;
		if (p.type == "give_item") {
			outcome = GiveItem(npc, p);
		} else if (p.type == "give_gold") {
			outcome = GiveGold(npc, p);
		} else if (p.type == "offer_bed") {
			outcome = OfferBed(npc, p);
		} else if (p.type == "allow_take") {
			outcome = AllowTake(npc, p);
		}
		if (outcome == Outcome::kKept) {
			std::scoped_lock l(g_lock);
			g_kept.push_back({ npc->GetFormID(), p, Clock::now() });
		}
		return outcome;
	}

	void Reset()
	{
		std::scoped_lock l(g_lock);
		g_transfers.clear();
		g_kept.clear();
		g_awaiting.clear();
		g_giftMenuNpc = 0;
	}

	void Install()
	{
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESContainerChangedEvent>(ContainerWatcher::Get());
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(GiftMenuWatcher::Get());
		}
	}
}
