#include "Services.h"

#include "Confirm.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Papyrus.h"
#include "Promises.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Services
{
	namespace
	{
		bool IsTrainer(RE::Actor* npc)
		{
			auto* base = npc->GetActorBase();
			auto* cls = base ? base->npcClass : nullptr;
			return cls && cls->data.maximumTrainingLevel > 0;
		}

		// The DialogueGeneric quest's script: it holds RoomRentalCost and is RentRoom's argument.
		RE::BSTSmartPointer<RE::BSScript::Object> DialogueGeneric()
		{
			static RE::TESQuest* quest = [] {
				// DialogueGeneric in Skyrim.esm; scan all quests if a mod moved the script.
				auto* q = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESQuest>(0x13EB3, "Skyrim.esm");
				if (q && Papyrus::HasScript(q, "DialogueGenericScript")) {
					return q;
				}
				for (auto* candidate : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESQuest>()) {
					if (candidate && Papyrus::HasScript(candidate, "DialogueGenericScript")) {
						logger::info("DialogueGenericScript found on quest {:08X}", candidate->GetFormID());
						return candidate;
					}
				}
				logger::error("DialogueGenericScript not found - room rental disabled");
				return static_cast<RE::TESQuest*>(nullptr);
			}();
			return Papyrus::GetScript(quest, "DialogueGenericScript");
		}

		// The price the NPC named, kept within reason (half to double the real price) so a
		// misheard or hallucinated number can't make a room free or ruinous.
		std::int32_t Haggled(std::int32_t agreed, std::int32_t cost)
		{
			if (agreed <= 0 || cost <= 0) {
				return cost;
			}
			return std::clamp(agreed, (std::max)(1, cost / 2), cost * 2);
		}

		// Runs vanilla RentRoomScript.RentRoom (bed ownership, timed release), then settles the
		// difference so the player pays exactly `price`.
		void DoRent(RE::FormID npcId, std::int32_t price)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto quest = DialogueGeneric();
			auto* player = Game::Player();
			if (!npc || !quest) {
				return;
			}
			if (auto room = RoomOf(npc); !room || room->rentedByPlayer) {
				return;  // rented meanwhile (a repeated request): never charge twice
			}
			const auto goldBefore = Game::ItemCount(player, Game::Gold());
			if (goldBefore < price) {
				// Spent it while the popup was open.
				Game::Narrate(std::format("{} reached for the {} gold but came up short.", player->GetName(), price), npc);
				return;
			}
			RE::BSScript::Variable arg;
			arg.SetObject(quest);
			bool started = Papyrus::CallMethodWith(npc, "RentRoomScript", "RentRoom",
				[npcId, price, goldBefore](const RE::BSScript::Variable&) {
					SKSE::GetTaskInterface()->AddTask([npcId, price, goldBefore] {
						auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
						auto* player = Game::Player();
						auto* gold = Game::Gold();
						if (!npc) {
							return;
						}
						auto room = RoomOf(npc);
						bool ok = room && room->rentedByPlayer;
						logger::info("rent_room at {} for {} gold -> {}", npc->GetName(), price, ok ? "rented" : "FAILED (bed owner unchanged)");
						if (!ok) {
							Game::Notify("The room could not be rented.");
							return;
						}
						// Vanilla charged its own price (or nothing, if a mod changed RentRoom).
						const auto charged = goldBefore - Game::ItemCount(player, gold);
						const auto diff = price - charged;
						if (diff > 0) {
							player->RemoveItem(gold, diff, RE::ITEM_REMOVE_REASON::kRemove, nullptr, npc);
						} else if (diff < 0) {
							player->AddObjectToContainer(gold, nullptr, -diff, npc);
						}
						if (price == 0) {
							Game::Notify("You were given a room for the night.");
							Game::Narrate(std::format("{} gave {} a room for the night, free of charge.", npc->GetName(), player->GetName()), npc);
						} else {
							Game::Notify(std::format("Rented a room for {} gold.", price));
							Game::Narrate(std::format("{} paid {} {} gold and rented a room for the night.", player->GetName(), npc->GetName(), price), npc);
						}
					});
				},
				{ arg });
			if (!started) {
				logger::error("rent_room: could not call RentRoom on {}", npc->GetName());
			}
		}

		// ---- shared activities: a meal together ----

		std::unordered_map<RE::FormID, float> g_lastMeal;  // game hours of the last shared meal
		constexpr float kMealEvery = 6.0f;

		// A seat for the NPC near them, preferring one with another seat close by (a table for two).
		RE::TESObjectREFR* FindSeat(RE::Actor* npc)
		{
			// Seats someone (or the player) is already in.
			std::unordered_set<RE::TESObjectREFR*> taken;
			auto addTaken = [&](RE::Actor* a) {
				if (a) {
					if (auto f = a->GetOccupiedFurniture().get()) {
						taken.insert(f.get());
					}
				}
			};
			addTaken(Game::Player());
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					addTaken(h.get().get());
				}
			}
			std::vector<RE::TESObjectREFR*> seats;
			auto consider = [&](RE::TESObjectREFR* ref) {
				auto* base = ref ? ref->GetBaseObject() : nullptr;
				auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
				if (!furn || !furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSit) ||
					furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) || ref->IsDisabled() || ref->IsDeleted() ||
					taken.contains(ref) || ref->GetPosition().GetDistance(npc->GetPosition()) >= 1500.0f) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// Not a work station (cooking pot, alchemy lab, workbench) and not a throne.
				if (furn->workBenchData.benchType.get() != RE::TESFurniture::WorkBenchData::BenchType::kNone) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto name = JsonUtil::Lower(base->GetName() ? base->GetName() : "");
				if (name.find("throne") != std::string::npos) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				seats.push_back(ref);
				return RE::BSContainer::ForEachResult::kContinue;
			};
			auto* cell = npc->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				cell->ForEachReference(consider);
			} else if (auto* tes = RE::TES::GetSingleton()) {
				tes->ForEachReferenceInRange(npc, 1500.0f, consider);
			}
			RE::TESObjectREFR* best = nullptr;
			float bestScore = (std::numeric_limits<float>::max)();
			for (auto* s : seats) {
				const bool paired = std::ranges::any_of(seats, [s](RE::TESObjectREFR* o) {
					return o != s && o->GetPosition().GetDistance(s->GetPosition()) < 300.0f;
				});
				const float score = s->GetPosition().GetDistance(npc->GetPosition()) + (paired ? 0.0f : 5000.0f);
				if (score < bestScore) {
					best = s;
					bestScore = score;
				}
			}
			return best;
		}

		// "Let's eat together": they go sit down, share a dish, and invite the player to join.
		bool ShareMeal(RE::Actor* npc)
		{
			const auto id = npc->GetFormID();
			const float now = Game::Hours();
			const std::string player = Game::Player()->GetName();
			if (auto it = g_lastMeal.find(id); it != g_lastMeal.end() && now - it->second < kMealEvery) {
				Game::Narrate(std::format("{} reminded {} with a smile that they had only just eaten together.", npc->GetName(), player), npc);
				return true;
			}
			g_lastMeal[id] = now;
			const auto dish = Promises::ServeMeal(npc);
			const auto what = dish.empty() ? std::string("what food they had") : dish;
			auto* seat = FindSeat(npc);
			if (!seat) {
				logger::info("meal: {} has no seat nearby; sharing it where they stand", npc->GetName());
				Game::Narrate(std::format("{} shared {} with {} right there, and they ate together.", npc->GetName(), what, player), npc);
				return true;
			}
			logger::info("meal: {} heads to seat {:08X}", npc->GetName(), seat->GetFormID());
			const auto seatId = seat->GetFormID();
			// Walk there, sit, then invite the player over.
			const bool walking = Papyrus::CallMethod(npc, "Actor", "PathToReference",
				[id, seatId, what, player](const RE::BSScript::Variable& arrived) {
					const bool reached = !arrived.IsBool() || arrived.GetBool();
					SKSE::GetTaskInterface()->AddTask([id, seatId, what, player, reached] {
						auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
						auto* seat = RE::TESForm::LookupByID<RE::TESObjectREFR>(seatId);
						if (!npc || !seat) {
							return;
						}
						// Couldn't get there (a locked door, upstairs out of reach): no teleporting
						// into a far chair; they share the meal where they are.
						if (!reached || seat->GetPosition().GetDistance(npc->GetPosition()) > 400.0f) {
							logger::info("meal: {} couldn't reach the seat; eating where they are", npc->GetName());
							Game::Narrate(std::format("{} shared {} with {} right there, and they ate together.", npc->GetName(), what, player), npc);
							return;
						}
						Papyrus::CallMethod(seat, "ObjectReference", "Activate", nullptr, static_cast<RE::TESObjectREFR*>(npc), false);
						Game::Narrate(std::format("{} sat down at the table with {} and invited {} to sit and eat with them.", npc->GetName(), what, player), npc);
					});
				},
				static_cast<RE::TESObjectREFR*>(seat), 1.0f);
			if (!walking) {
				Game::Narrate(std::format("{} shared {} with {}, and they ate together.", npc->GetName(), what, player), npc);
			}
			return true;
		}

		// SeverActions' quest that carries a script (SeverActions_Follow, SeverActions_Travel):
		// its main quest first, then any quest of SeverActions.esp. Null without SeverActions.
		RE::TESQuest* SeverActionsQuest(const char* script)
		{
			static std::unordered_map<std::string, RE::TESQuest*> cache;
			if (auto it = cache.find(script); it != cache.end()) {
				return it->second;
			}
			auto* dh = RE::TESDataHandler::GetSingleton();
			RE::TESQuest* found = nullptr;
			if (auto* main = dh->LookupForm<RE::TESQuest>(0xD62, "SeverActions.esp"); main && Papyrus::HasScript(main, script)) {
				found = main;
			} else if (dh->LookupModByName("SeverActions.esp")) {
				for (auto* q : dh->GetFormArray<RE::TESQuest>()) {
					auto* file = q ? q->GetFile(0) : nullptr;
					if (file && std::string_view(file->GetFilename()) == "SeverActions.esp" && Papyrus::HasScript(q, script)) {
						found = q;
						break;
					}
				}
			}
			logger::info("SeverActions {}: {}", script, found ? std::format("quest {:08X}", found->GetFormID()) : std::string("not found - follow/escort off"));
			cache[script] = found;
			return found;
		}

		// Follow/escort confirm: the popup when it's on and available, else straight ahead.
		// Decline (or the timer) leaves things as they are and lets the NPC react.
		struct Lines
		{
			std::string accept;   // what the player says on Accept
			std::string decline;  // ... and on Decline / timeout
		};

		void ConfirmMove(RE::FormID npcId, const std::string& title, const std::string& detail, const Lines& lines, std::function<void()> go)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc) {
				return;
			}
			if (!SkyrimNet::ConfigBool("features.confirm_move", true) || !Confirm::Available()) {
				go();
				return;
			}
			Confirm::Offer offer;
			offer.title = title;
			offer.npc = npc->GetName();
			offer.detail = detail;
			offer.showPrice = false;
			offer.seconds = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f));
			offer.acceptLabel = lines.accept;
			offer.declineLabel = lines.decline;
			const bool shown = Confirm::Ask(offer, [npcId, title, go, lines](Confirm::Choice choice) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || npc->IsDead()) {
					return;
				}
				if (choice == Confirm::Choice::kTimeout) {
					logger::info("{}: popup timed out, nothing happens", title);
					return;
				}
				// The chosen line is the player's answer; the NPC replies to it in character.
				const bool accepted = choice == Confirm::Choice::kAccept;
				Game::Say(accepted ? lines.accept : lines.decline, npc, true);
				if (!accepted) {
					logger::info("{}: declined in the popup", title);
					return;
				}
				// Up to 15 s have passed: a fight may have started.
				if (npc->IsInCombat() || Game::Player()->IsInCombat()) {
					logger::info("{}: accepted, but combat started meanwhile", title);
					return;
				}
				go();
			});
			if (!shown) {
				logger::info("{}: a popup is already open, request ignored", title);
			}
		}

		// "Follow me": SeverActions' casual follow (walks behind the player, no roster seat).
		bool Follow(RE::Actor* npc)
		{
			auto* quest = SeverActionsQuest("SeverActions_Follow");
			if (!quest) {
				return false;
			}
			const auto npcId = npc->GetFormID();
			auto proceed = [npcId, quest](bool ok) {
					SKSE::GetTaskInterface()->AddTask([npcId, quest, ok] {
						auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
						if (!npc) {
							return;
						}
						if (!ok) {
							logger::info("follow: {} already following or not eligible", npc->GetName());
							return;
						}
						const Lines lines{
							Game::Pick({ "Yes, come with me.", "Good. Stay close.", "Come on then, follow me." }),
							Game::Pick({ "Actually, stay here. I'll manage.", "On second thought, never mind.", "No, I'll go alone after all." })
						};
						ConfirmMove(npcId, "Follow you", "Walks behind you for a while", lines, [npcId, quest] {
							auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
							if (!npc) {
								return;
							}
							if (!Papyrus::CallMethod(quest, "SeverActions_Follow", "StartFollowing", nullptr, static_cast<RE::Actor*>(npc))) {
								logger::error("follow: SeverActions StartFollowing could not be called");
								return;
							}
							logger::info("follow: {} now follows the player", npc->GetName());
						});
					});
			};
			// Its own eligibility check: false when already following (SkyrimNet's StartFollowing
			// action may have fired on the same line), a vanilla follower, or in combat. Missing in
			// another SeverActions version: decide here instead.
			auto checkEligible = [npcId, proceed] {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc) {
					return;
				}
				const bool asked = Papyrus::CallStatic("SeverActions_Follow", "StartFollowing_IsEligible",
					[proceed](const RE::BSScript::Variable& eligible) { proceed(!eligible.IsBool() || eligible.GetBool()); },
					static_cast<RE::Actor*>(npc));
				if (!asked) {
					logger::warn("follow: StartFollowing_IsEligible unavailable, checking ourselves");
					proceed(!npc->IsPlayerTeammate() && !npc->IsInCombat());
				}
			};
			// Leading the player somewhere right now ("follow me, it's this way" mid-escort): that's
			// not a request to follow.
			const bool asked = Papyrus::CallStatic("SeverActionsNativeExt2", "Travel_IsTravelingByActor",
				[checkEligible](const RE::BSScript::Variable& traveling) {
					if (traveling.IsBool() && traveling.GetBool()) {
						logger::info("follow: they're on a journey, not starting a follow");
						return;
					}
					SKSE::GetTaskInterface()->AddTask(checkEligible);
				},
				static_cast<RE::Actor*>(npc));
			if (!asked) {
				checkEligible();
			}
			return true;
		}

		std::string Lower(std::string s)
		{
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		// Name of where someone lives: the interior they are assigned to, else their home location.
		std::string HomeName(RE::Actor* who)
		{
			if (auto* x = who->extraList.GetByType<RE::ExtraPersistentCell>(); x && x->persistentCell && x->persistentCell->IsInteriorCell()) {
				if (auto* n = x->persistentCell->GetFullName(); n && *n) {
					return n;
				}
			}
			if (auto* loc = who->GetEditorLocation1(); loc && loc->GetName() && *loc->GetName()) {
				return loc->GetName();
			}
			return "";
		}

		// A person by name, typos allowed: "Hulda", "hlda", "Mjoll" for "Mjoll the Lioness".
		// People nearby win over namesakes elsewhere.
		RE::Actor* FindPerson(const std::string& wanted)
		{
			const auto want = Lower(wanted);
			if (want.size() < 3) {
				return nullptr;
			}
			auto* player = Game::Player();
			RE::Actor* best = nullptr;
			std::size_t bestScore = 99;
			float bestDist = (std::numeric_limits<float>::max)();
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			[[maybe_unused]] const RE::BSReadLockGuard guard{ lock };
			if (!map) {
				return nullptr;
			}
			for (auto& [id, form] : *map) {
				if (!form || form->GetFormType() != RE::FormType::ActorCharacter) {
					continue;
				}
				auto* actor = static_cast<RE::Actor*>(form);
				if (actor == player || actor->IsDead()) {
					continue;
				}
				const char* raw = actor->GetName();
				if (!raw || !*raw) {
					continue;
				}
				const auto full = Lower(raw);
				const auto first = full.substr(0, full.find(' '));
				std::size_t score = 99;
				if (full == want) {
					score = 0;
				} else if (first == want) {
					score = 1;
				} else {
					const std::size_t allowed = want.size() >= 6 ? 2 : 1;
					const auto d = (std::min)(JsonUtil::EditDistance(full, want), JsonUtil::EditDistance(first, want));
					if (d <= allowed) {
						score = 2 + d;
					}
				}
				if (score == 99) {
					continue;
				}
				const float dist = Game::DistanceToPlayer(actor);
				if (score < bestScore || (score == bestScore && dist < bestDist)) {
					best = actor;
					bestScore = score;
					bestDist = dist;
				}
			}
			return best;
		}

		// Home destinations aren't place names SeverActions can look up: "home" (the speaker's own,
		// "your place") and "home:Hulda" ("where Hulda lives", "Hulda's place") become the name of
		// that person's house.
		std::string ResolveHome(RE::Actor* npc, std::string place)
		{
			auto lower = Lower(place);
			static const std::array<std::string_view, 12> kTheirs{
				"home", "your home", "your house", "your place", "where you live", "your hut", "your shack", "your farm",
				"his home", "her home", "their home", "npc_home"
			};
			RE::Actor* who = nullptr;
			std::string person;
			if (std::ranges::find(kTheirs, std::string_view(lower)) != kTheirs.end()) {
				who = npc;
			} else {
				// "home:Hulda", "home of Hulda", "Hulda's home/house/place"
				// Curly apostrophes (3 bytes in UTF-8) would confuse the byte-wise regex.
				for (std::size_t at; (at = place.find("\xE2\x80\x99")) != std::string::npos;) {
					place.replace(at, 3, "'");
				}
				std::smatch m;
				static const std::regex prefixed(R"(^\s*home\s*(?::|of)\s*(.+?)\s*$)", std::regex::icase);
				static const std::regex possessive(R"(^\s*(.+?)'s\s+(?:home|house|place|hut|shack|farm)\s*$)", std::regex::icase);
				if (std::regex_match(place, m, prefixed) || std::regex_match(place, m, possessive)) {
					person = m[1].str();
					who = FindPerson(person);
				} else {
					return place;
				}
			}
			const auto name = who ? HomeName(who) : std::string();
			logger::info("escort: '{}' -> {}'s home -> '{}'", place, who ? who->GetName() : person.c_str(),
				name.empty() ? "unknown" : name);
			if (!name.empty()) {
				return name;
			}
			// Unknown home: let SeverActions try the person's own words.
			return person.empty() ? place : person + "'s house";
		}

		RE::BGSLocation* HoldAbove(RE::BGSLocation* loc)
		{
			for (int depth = 0; loc && depth < 12; ++depth, loc = loc->parentLoc) {
				if (loc->HasKeywordString("LocTypeHold")) {
					return loc;
				}
			}
			return nullptr;
		}

		// The city, town or settlement a reference is in ("Whiterun", "Riverwood"), or "".
		std::string TownName(RE::TESObjectREFR* ref)
		{
			RE::BGSLocation* loc = ref->GetCurrentLocation();
			for (int depth = 0; loc && depth < 12; ++depth, loc = loc->parentLoc) {
				if (loc->HasKeywordString("LocTypeCity") || loc->HasKeywordString("LocTypeTown") || loc->HasKeywordString("LocTypeSettlement")) {
					return loc->GetName() ? loc->GetName() : "";
				}
			}
			return "";
		}

		// "Whiterun Hold" -> "Whiterun", "The Rift" stays.
		std::string HoldShortName(RE::TESObjectREFR* ref)
		{
			auto* hold = HoldOf(ref);
			std::string name = hold && hold->GetName() ? hold->GetName() : "";
			if (name.size() > 5 && name.ends_with(" Hold")) {
				name.resize(name.size() - 5);
			}
			return name;
		}

		// Where someone lives, as a hold: their assigned interior's location, else their home location.
		RE::BGSLocation* HomeHold(RE::Actor* who)
		{
			if (auto* x = who->extraList.GetByType<RE::ExtraPersistentCell>(); x && x->persistentCell) {
				if (auto* hold = HoldAbove(x->persistentCell->GetLocation())) {
					return hold;
				}
			}
			return HoldAbove(who->GetEditorLocation1());
		}

		// ---- kinds of place ("the inn", "the brewery") -> the real one nearby ----

		struct PlaceKind
		{
			const char* label;
			std::vector<std::string_view> said;      // what players call it
			std::vector<std::string_view> named;     // words in real place names
			std::vector<std::string_view> keywords;  // location type keywords
		};

		const std::vector<PlaceKind>& Kinds()
		{
			static const std::vector<PlaceKind> kinds{
				{ "brewery", { "brewery", "meadery", "distillery", "mead hall" }, { "meadery", "brewery", "distillery" }, {} },
				{ "inn", { "inn", "tavern", "pub", "alehouse", "bar" }, { "inn", "tavern" }, { "LocTypeInn" } },
				{ "temple", { "temple", "shrine", "chapel", "priest" }, { "temple", "chapel" }, { "LocTypeTemple" } },
				{ "store", { "store", "shop", "general goods", "trader", "market", "merchant" }, { "goods", "trader", "store", "market" }, { "LocTypeStore" } },
				{ "smithy", { "smith", "blacksmith", "smithy", "forge", "armorer" }, { "smith", "forge", "hammer", "anvil", "warmaiden" }, {} },
				{ "alchemist", { "alchemist", "apothecary", "potion shop" }, { "cauldron", "apothecary", "alchem" }, {} },
				{ "palace", { "palace", "castle", "jarl", "keep", "longhouse", "court" }, { "palace", "castle", "keep", "dragonsreach", "longhouse" }, { "LocTypeCastle" } },
				{ "stables", { "stable", "stables", "carriage" }, { "stables" }, {} },
				{ "farm", { "farm", "farmstead" }, { "farm" }, { "LocTypeFarm" } },
				{ "mill", { "mill", "lumber mill", "sawmill" }, { "mill" }, { "LocTypeLumberMill" } },
				{ "mine", { "mine" }, { "mine" }, { "LocTypeMine" } },
				{ "cemetery", { "cemetery", "graveyard", "hall of the dead", "catacombs" }, { "hall of the dead", "cemetery" }, { "LocTypeCemetery" } },
				{ "jail", { "jail", "prison", "dungeon cell" }, { "jail", "prison" }, { "LocTypeJail" } },
				{ "barracks", { "barracks" }, { "barracks" }, { "LocTypeBarracks" } },
			};
			return kinds;
		}

		bool HasWord(const std::string& text, std::string_view word)
		{
			for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
				const bool start = at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1]));
				const bool end = at + word.size() >= text.size() || !std::isalnum(static_cast<unsigned char>(text[at + word.size()]));
				if (start && end) {
					return true;
				}
			}
			return false;
		}

		// The kind a phrase names, typos allowed ("brewry" -> brewery); null when none.
		const PlaceKind* KindOf(const std::string& text)
		{
			const auto lower = JsonUtil::Lower(text);
			std::vector<std::string> words;
			std::string cur;
			for (char c : lower) {
				if (std::isalnum(static_cast<unsigned char>(c))) {
					cur.push_back(c);
				} else if (!cur.empty()) {
					words.push_back(std::move(cur));
					cur.clear();
				}
			}
			if (!cur.empty()) {
				words.push_back(cur);
			}
			for (auto& kind : Kinds()) {
				for (auto said : kind.said) {
					if (HasWord(lower, said)) {
						return &kind;
					}
					if (said.size() >= 5 && said.find(' ') == std::string_view::npos) {
						for (auto& w : words) {
							if (w.size() >= 4 && JsonUtil::EditDistance(w, said) <= 1) {
								return &kind;
							}
						}
					}
				}
			}
			return nullptr;
		}

		// "the brewery", "brewry", "the local inn", "the inn in Whiterun": only a kind, no name of
		// its own. Every word left after dropping fillers and the town/hold must be a kind word.
		bool OnlyAKind(const std::string& text, const PlaceKind& kind, const std::string& town, const std::string& hold)
		{
			static const std::unordered_set<std::string> kFiller{
				"the", "a", "an", "local", "nearest", "closest", "town", "city", "this", "here", "near", "in", "of", "s", "hold", "old"
			};
			std::unordered_set<std::string> placeWords;
			for (auto& w : JsonUtil::Words(town + " " + hold)) {
				placeWords.insert(w);
			}
			for (auto& w : JsonUtil::Words(text)) {
				if (kFiller.contains(w) || placeWords.contains(w)) {
					continue;
				}
				const bool kindWord = std::ranges::any_of(kind.said, [&](std::string_view s) {
					return HasWord(std::string(s), w) || (w.size() >= 4 && s.size() >= 5 && JsonUtil::EditDistance(w, s) <= 1);
				});
				if (!kindWord) {
					return false;
				}
			}
			return true;
		}

		bool IsOfKind(RE::BGSLocation* loc, const PlaceKind& kind)
		{
			for (auto kw : kind.keywords) {
				if (loc->HasKeywordString(kw)) {
					return true;
				}
			}
			const auto name = JsonUtil::Lower(loc->GetName() ? loc->GetName() : "");
			return std::ranges::any_of(kind.named, [&](std::string_view w) { return name.find(w) != std::string::npos; });
		}

		// How far a location is from the player: by its map marker in the same worldspace; other
		// worldspaces (a city vs. the countryside) rank after, unmarked ones last.
		// Where the player is on the map: their own position outdoors; indoors, the map marker of
		// the house/town they're in (interior coordinates mean nothing outside).
		std::pair<RE::TESWorldSpace*, RE::NiPoint3> PlayerOnMap()
		{
			auto* player = Game::Player();
			if (auto* ws = player->GetWorldspace()) {
				return { ws, player->GetPosition() };
			}
			for (auto* loc = player->GetCurrentLocation(); loc; loc = loc->parentLoc) {
				if (auto marker = loc->worldLocMarker.get(); marker && marker->GetWorldspace()) {
					return { marker->GetWorldspace(), marker->GetPosition() };
				}
			}
			return { nullptr, player->GetPosition() };
		}

		float LocationDistance(RE::BGSLocation* loc, const std::pair<RE::TESWorldSpace*, RE::NiPoint3>& from)
		{
			auto marker = loc->worldLocMarker.get();
			if (!marker) {
				return 2.0e7f;
			}
			if (!from.first || marker->GetWorldspace() != from.first) {
				return 1.0e7f + marker->GetPosition().GetDistance(from.second) * 0.001f;
			}
			return marker->GetPosition().GetDistance(from.second);
		}

		// Named locations of the NPC's hold, nearest first (kind = null: every place worth naming).
		std::vector<std::pair<RE::BGSLocation*, const PlaceKind*>> LocationsInHold(RE::Actor* npc, const PlaceKind* only)
		{
			std::vector<std::pair<RE::BGSLocation*, const PlaceKind*>> out;
			auto* hold = HoldOf(npc);
			if (!hold) {
				return out;
			}
			for (auto* loc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::BGSLocation>()) {
				if (!loc || !loc->GetName() || !*loc->GetName() || loc == hold || HoldAbove(loc) != hold) {
					continue;
				}
				const PlaceKind* kind = nullptr;
				if (only) {
					if (IsOfKind(loc, *only)) {
						kind = only;
					}
				} else {
					for (auto& k : Kinds()) {
						if (IsOfKind(loc, k)) {
							kind = &k;
							break;
						}
					}
				}
				if (kind) {
					out.emplace_back(loc, kind);
				}
			}
			const auto from = PlayerOnMap();
			std::ranges::sort(out, [&](auto& a, auto& b) { return LocationDistance(a.first, from) < LocationDistance(b.first, from); });
			return out;
		}

		// "the brewery" near Whiterun -> "Honningbrew Meadery". Empty when the phrase is a real
		// name already, or nothing of that kind is in this hold.
		std::string RealPlaceFor(RE::Actor* npc, const std::string& place)
		{
			const auto* kind = KindOf(place);
			if (!kind) {
				return "";
			}
			auto* hold = HoldOf(npc);
			if (!OnlyAKind(place, *kind, TownName(npc), hold && hold->GetName() ? hold->GetName() : "")) {
				return "";  // "Honningbrew Meadery" names itself
			}
			auto found = LocationsInHold(npc, kind);
			if (found.empty()) {
				return "";
			}
			logger::info("escort: '{}' means the nearest {}: {}", place, kind->label, found.front().first->GetName());
			return found.front().first->GetName();
		}

		// Starts SeverActions' trip. If SeverActions can't place the destination, a bare name may
		// mean that person's home ("take me to Hulda"); after that the NPC admits they don't know
		// the way, instead of having agreed and then standing still.
		void StartTravel(RE::FormID npcId, RE::TESQuest* quest, std::string place, bool mayRetry)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc) {
				return;
			}
			const std::string name = npc->GetName();
			const bool called = Papyrus::CallMethod(quest, "SeverActions_Travel", "TravelToPlace",
				[npcId, quest, name, place, mayRetry](const RE::BSScript::Variable& started) {
					const bool ok = !started.IsBool() || started.GetBool();
					logger::info("escort: {} to '{}' -> {}", name, place, ok ? "on the way" : "SeverActions could not place it");
					if (ok) {
						return;
					}
					SKSE::GetTaskInterface()->AddTask([npcId, quest, place, mayRetry] {
						auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
						if (!npc) {
							return;
						}
						if (mayRetry) {
							if (auto* who = FindPerson(place); who && who != npc) {
								if (auto home = HomeName(who); !home.empty() && home != place) {
									logger::info("escort: '{}' is a person; trying their home '{}'", place, home);
									StartTravel(npcId, quest, home, false);
									return;
								}
							}
						}
						Game::Narrate(std::format("{} set off to lead {} to {}, but realised they don't actually know the way there.",
										  npc->GetName(), Game::Player()->GetName(), place),
							npc);
					});
				},
				// A companion keeps following afterwards (stopFollowing false); anyone else goes back
				// to their day after waiting for the player.
				static_cast<RE::Actor*>(npc), RE::BSFixedString(place), 24.0f, !npc->IsPlayerTeammate(), static_cast<std::int32_t>(0), true);
			if (!called) {
				logger::error("escort: SeverActions TravelToPlace could not be called");
			}
		}

		// "Take me to X": SeverActions' TravelToPlace at walking pace, waiting there for the player,
		// so the NPC leads and the player keeps up. SeverActions resolves the place name (fuzzy,
		// city aliases) and may show its own destination-confirm popup.
		// The escort popup (unless SeverActions' own destination popup will ask), then the trip.
		void AskAndTravel(RE::FormID npcId, RE::TESQuest* quest, const std::string& where)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc) {
				return;
			}
			auto travel = [npcId, quest, where] { StartTravel(npcId, quest, where, true); };
			auto decide = [npcId, where, travel](bool theirsAsks) {
				SKSE::GetTaskInterface()->AddTask([npcId, where, travel, theirsAsks] {
					if (theirsAsks) {
						travel();
						return;
					}
					const Lines lines{
						Game::Pick({ "Yes, lead the way to " + where + ".", "Please, take me to " + where + ".", "Lead on, I'll follow you." }),
						Game::Pick({ "No, just point me the right way.", "Never mind, I'll find it myself.", "Just tell me the way, I'll go alone." })
					};
					ConfirmMove(npcId, "Escort", "Leads you to " + where, lines, travel);
				});
			};
			// SeverActions' own popup settings (on for everyone, or for followers only); if they
			// can't be read, MundaneMatters asks itself.
			const bool teammate = npc->IsPlayerTeammate();
			const bool asked = Papyrus::CallStatic("SeverActionsNative", "Magelight_IsTravelPopupEnabled",
				[decide, teammate](const RE::BSScript::Variable& enabled) {
					if (!(enabled.IsBool() && enabled.GetBool())) {
						decide(false);
						return;
					}
					const bool asked2 = Papyrus::CallStatic("SeverActionsNative", "Magelight_IsTravelPopupFollowersOnly",
						[decide, teammate](const RE::BSScript::Variable& followersOnly) {
							decide(!(followersOnly.IsBool() && followersOnly.GetBool()) || teammate);
						});
					if (!asked2) {
						decide(false);
					}
				});
			if (!asked) {
				decide(false);
			}
		}

		// Tries each name with SeverActions' resolver; the first that lands in the NPC's hold (or
		// their home hold) is the one meant. "The Brewery" alone may match one across Skyrim;
		// "The Brewery, Whiterun" finds the local one.
		void FindLocal(RE::FormID npcId, std::vector<std::string> names, std::size_t i, std::string farHold,
			std::function<void(const std::string&)> onLocal, std::function<void(const std::string&)> onFar)
		{
			if (i >= names.size()) {
				// Somewhere else entirely, or nowhere SeverActions knows (then let it try anyway).
				if (farHold.empty()) {
					onLocal(names.front());
				} else {
					onFar(farHold);
				}
				return;
			}
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc) {
				return;
			}
			const auto name = names[i];
			const bool called = Papyrus::CallStatic("SeverActionsNative", "ResolveDestination",
				[npcId, names, i, farHold, onLocal, onFar, name](const RE::BSScript::Variable& marker) {
					RE::TESObjectREFR* ref = marker.IsObject() && !marker.IsNoneObject() ? marker.Unpack<RE::TESObjectREFR*>() : nullptr;
					SKSE::GetTaskInterface()->AddTask([npcId, names, i, farHold, onLocal, onFar, name, ref] {
						auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
						if (!npc) {
							return;
						}
						auto* there = ref ? HoldOf(ref) : nullptr;
						auto* here = HoldOf(npc);
						if (ref && (!there || !here || there == here || there == HomeHold(npc))) {
							logger::info("escort: '{}' is local", name);
							onLocal(name);
							return;
						}
						std::string farName = farHold;
						if (ref && there && farName.empty()) {
							farName = there->GetName();
							logger::info("escort: '{}' resolves to {}, trying a nearer match", name, farName);
						}
						FindLocal(npcId, names, i + 1, farName, onLocal, onFar);
					});
				},
				static_cast<RE::Actor*>(npc), RE::BSFixedString(name));
			if (!called) {
				onLocal(names.front());  // no resolver to ask: let SeverActions try
			}
		}

		// "Take me to X": SeverActions' TravelToPlace at walking pace, waiting there for the player,
		// so the NPC leads and the player keeps up. SeverActions resolves the place name (fuzzy,
		// city aliases) and may show its own destination-confirm popup.
		bool Escort(RE::Actor* npc, const std::string& place, bool ownTrip)
		{
			auto* quest = SeverActionsQuest("SeverActions_Travel");
			if (!quest) {
				return false;
			}
			if (place.empty()) {
				// Usually chatter during a trip ("not far now, keep up") or "where to?": nothing to start.
				logger::info("escort: {} - no destination named", npc->GetName());
				return false;
			}
			const auto npcId = npc->GetFormID();
			auto proceed = [npcId, quest, ownTrip, place = ResolveHome(npc, place)](bool busy) {
				SKSE::GetTaskInterface()->AddTask([npcId, quest, ownTrip, place, busy] {
					auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
					if (!npc) {
						return;
					}
					if (busy) {
						logger::info("escort: {} is already on the way somewhere", npc->GetName());
						return;
					}
					// A onLocal won't walk the player across Skyrim: another hold is days away.
					// Companions and people heading there anyway are exempt.
					if (!SkyrimNet::ConfigBool("features.escort_local_only", true) || ownTrip || npc->IsPlayerTeammate()) {
						const auto real = RealPlaceFor(npc, place);
						AskAndTravel(npcId, quest, real.empty() ? place : real);
						return;
					}
					// The real place when only a kind was said ("the brewery"), then the name as said,
					// with this town, with this hold.
					std::vector<std::string> names;
					if (auto real = RealPlaceFor(npc, place); !real.empty()) {
						names.push_back(real);
					}
					names.push_back(place);
					auto addWith = [&](const std::string& suffix) {
						if (!suffix.empty() && JsonUtil::Lower(place).find(JsonUtil::Lower(suffix)) == std::string::npos) {
							names.push_back(place + ", " + suffix);
						}
					};
					addWith(TownName(npc));
					addWith(HoldShortName(npc));
					FindLocal(npcId, names, 0, "",
						[npcId, quest](const std::string& where) { AskAndTravel(npcId, quest, where); },
						[npcId, place](const std::string& farHold) {
							auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
							auto* here = npc ? HoldOf(npc) : nullptr;
							if (!npc) {
								return;
							}
							logger::info("escort: {} won't go to {} ({}): another hold", npc->GetName(), place, farHold);
							Game::Narrate(std::format("{} realised {} is in {}, days of travel away, and they can't just leave {} to take {} there.",
											  npc->GetName(), place, farHold, here ? here->GetName() : "home", Game::Player()->GetName()),
								npc);
						});
				});
			};
			const bool asked = Papyrus::CallStatic("SeverActionsNativeExt2", "Travel_IsTravelingByActor",
				[proceed](const RE::BSScript::Variable& traveling) { proceed(traveling.IsBool() && traveling.GetBool()); },
				static_cast<RE::Actor*>(npc));
			if (!asked) {
				logger::warn("escort: Travel_IsTravelingByActor unavailable, assuming not travelling");
				proceed(false);
			}
			return true;
		}

		bool RentRoom(RE::Actor* npc, std::int32_t agreedPrice, bool free)
		{
			auto room = RoomOf(npc);
			auto* player = Game::Player();
			if (!room || !room->bed || !DialogueGeneric()) {
				logger::warn("rent_room: {} has no rentable bed", npc->GetName());
				return false;
			}
			if (room->rentedByPlayer) {
				Game::Notify("You already have a room here.");
				return true;
			}
			const RE::FormID npcId = npc->GetFormID();
			if (free) {
				// Nothing to agree to: vanilla RentRoom charges, DoRent refunds it all.
				DoRent(npcId, 0);
				return true;
			}
			const auto price = Haggled(agreedPrice, room->cost);
			const auto purse = Game::ItemCount(player, Game::Gold());
			if (purse < price) {
				Game::Notify(std::format("You need {} gold for a room.", price));
				Game::Narrate(std::format("{} checked their purse: only {} gold, not the {} gold a room costs.", player->GetName(), purse, price), npc);
				return true;
			}

			if (!SkyrimNet::ConfigBool("features.confirm", true) || !Confirm::Available()) {
				DoRent(npcId, price);  // no popup wanted (or no PrismaUI): asking was consent enough
				return true;
			}
			Confirm::Offer offer{
				"Rent a room",
				npc->GetName(),
				"A bed for the night",
				price,
				purse,
				static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f))
			};
			const std::string acceptLine = Game::Pick({ "I'll take the room.", "Deal. Here's your coin.", "Fair enough, I'll take it." });
			const std::string declineLine = Game::Pick({ "On second thought, I'll pass.", "Too rich for me, sorry.", "Maybe another time." });
			offer.acceptLabel = acceptLine;
			offer.declineLabel = declineLine;
			bool shown = Confirm::Ask(offer, [npcId, price, acceptLine, declineLine](Confirm::Choice choice) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || choice == Confirm::Choice::kTimeout) {
					return;  // unanswered: no room, and nothing said in the player's name
				}
				if (choice == Confirm::Choice::kAccept) {
					// The payment narration in DoRent makes them answer; one reply, not two.
					Game::Say(acceptLine, npc, false);
					DoRent(npcId, price);
				} else {
					Game::Say(declineLine, npc, true);
				}
			});
			if (!shown) {
				// A popup is already open (the player repeated the request): that one decides.
				logger::info("rent_room: popup already open, request ignored");
			}
			return true;
		}
	}

	std::vector<Service> For(RE::Actor* npc)
	{
		std::vector<Service> out;
		if (!npc || npc->IsDead() || npc->IsHostileToActor(Game::Player())) {
			return out;
		}

		if (npc->GetVendorFaction() && SkyrimNet::ConfigBool("features.trade", true)) {
			out.push_back({ "trade",
				"trade with them: see their goods, buy, or sell to them",
				"\"show me your wares\", \"what are you selling?\", \"got anything good?\", \"I need to buy supplies\", \"want to buy some pelts?\"" });
		}
		if (SkyrimNet::ConfigBool("features.rent_room", true)) {
			// Already rented: the room only comes up in passing ("your room's ready") and must not
			// read as a new request.
			if (auto room = RoomOf(npc); room && !room->rentedByPlayer) {
				out.push_back({ "rent_room",
					std::format("rent a room / bed for the night here ({} gold)", room->cost),
					"\"I need a room\", \"got a bed for the night?\", \"I'd like to rest here\", \"can I stay the night?\", \"how much for a room?\" followed by agreeing" });
			}
		}
		if (IsTrainer(npc) && SkyrimNet::ConfigBool("features.training", true)) {
			out.push_back({ "train",
				"pay them to train a skill",
				"\"can you teach me?\", \"I want to get better with a bow\", \"train me\", \"I'd like some lessons\"" });
		}
		// Anyone can come along or show the way, through SeverActions.
		if (SkyrimNet::ConfigBool("features.follow", true) && !npc->IsPlayerTeammate() && SeverActionsQuest("SeverActions_Follow")) {
			out.push_back({ "follow",
				"come along with the PLAYER now, walking behind them (the PLAYER leads)",
				"\"follow me\", \"come with me\", \"tag along\", \"stick close\", \"walk with me to the market\", \"come on, let's go\"" });
		}
		if (SkyrimNet::ConfigBool("features.share_meal", true) && Promises::IsClose(npc)) {
			out.push_back({ "share_meal",
				"sit down and share a meal with the PLAYER now (go to a table, eat together)",
				"\"let's eat together\", \"join me for supper?\", \"care to share a meal?\", \"grab a bite with me?\", \"have a drink with me\", \"you hungry? let's eat\"" });
		}
		if (SkyrimNet::ConfigBool("features.escort", true) && SeverActionsQuest("SeverActions_Travel")) {
			out.push_back({ "escort",
				"lead the PLAYER to a named place, walking ahead so they can follow (the NPC leads; set \"place\")",
				"\"take me to the Bannered Mare\", \"show me the way to the temple\", \"can you guide me to Riverwood?\", \"lead the way to Dragonsreach\", \"escort me to the market\"" });
		}
		return out;
	}

	RE::BGSLocation* HoldOf(RE::TESObjectREFR* ref)
	{
		if (!ref) {
			return nullptr;
		}
		RE::BGSLocation* loc = ref->GetCurrentLocation();
		if (!loc) {
			loc = ref->GetEditorLocation();
		}
		if (!loc) {
			if (auto* cell = ref->GetParentCell()) {
				loc = cell->GetLocation();
			}
		}
		return HoldAbove(loc);
	}

	std::vector<std::string> PlacesNear(RE::Actor* npc, std::size_t max)
	{
		std::vector<std::string> out;
		for (auto& [loc, kind] : LocationsInHold(npc, nullptr)) {
			auto entry = std::format("{} ({})", loc->GetName(), kind->label);
			if (std::ranges::find(out, entry) == out.end()) {
				out.push_back(std::move(entry));
			}
			if (out.size() >= max) {
				break;
			}
		}
		return out;
	}

	std::string WhereIs(RE::Actor* npc)
	{
		const auto town = TownName(npc);
		auto* hold = HoldOf(npc);
		const std::string holdName = hold && hold->GetName() ? hold->GetName() : "";
		if (town.empty()) {
			return holdName;
		}
		return holdName.empty() || holdName == town ? town : std::format("{} ({})", town, holdName);
	}

	bool IsActivity(const std::string& id)
	{
		return id == "share_meal";
	}

	bool IsMovement(const std::string& id)
	{
		return id == "follow" || id == "escort";
	}

	std::optional<Room> RoomOf(RE::Actor* npc)
	{
		auto script = Papyrus::GetScript(npc, "RentRoomScript");
		if (!script) {
			return std::nullopt;
		}
		Room room;
		room.bed = Papyrus::FormProperty<RE::TESObjectREFR>(script, "Bed");
		if (auto quest = DialogueGeneric()) {
			if (auto* cost = Papyrus::FormProperty<RE::TESGlobal>(quest, "RoomRentalCost")) {
				room.cost = static_cast<std::int32_t>(cost->value);
			}
		}
		if (room.bed) {
			room.rentedByPlayer = room.bed->GetOwner() == Game::Player()->GetActorBase();
		}
		return room;
	}

	bool Execute(const std::string& id, RE::Actor* npc, const Args& args)
	{
		if (!npc) {
			return false;
		}
		if (id == "trade") {
			// Same call the vanilla "What have you got for sale?" fragment makes.
			return Papyrus::CallMethod(npc, "Actor", "ShowBarterMenu", nullptr);
		}
		if (id == "train") {
			return Papyrus::CallStatic("Game", "ShowTrainingMenu", nullptr, static_cast<RE::Actor*>(npc));
		}
		if (id == "rent_room") {
			return RentRoom(npc, args.price, args.free);
		}
		if (id == "follow") {
			return Follow(npc);
		}
		if (id == "escort") {
			return Escort(npc, args.place, args.ownTrip);
		}
		if (id == "share_meal") {
			return ShareMeal(npc);
		}
		logger::warn("unknown service '{}'", id);
		return false;
	}

	const char* MenuFor(const std::string& id)
	{
		if (id == "trade") {
			return RE::BarterMenu::MENU_NAME.data();
		}
		if (id == "train") {
			return RE::TrainingMenu::MENU_NAME.data();
		}
		return "";
	}
}
