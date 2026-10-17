#include "Services.h"

#include "Confirm.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Papyrus.h"
#include "Promises.h"
#include "Rest.h"
#include "Bonds.h"
#include "Care.h"
#include "Crime.h"
#include "Keepsakes.h"
#include "Moments.h"
#include "MoveIn.h"
#include "Safekeeping.h"
#include "Loans.h"
#include "Trade.h"
#include "Trust.h"
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
						logger::info("rent_room at {} for {} gold -> {}", Game::NameOf(npc), price, ok ? "rented" : "FAILED (bed owner unchanged)");
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
							Game::Mention(std::format("{} gave {} a room for the night, free of charge.", Game::NameOf(npc), player->GetName()), npc);
						} else {
							Game::Notify(std::format("Rented a room for {} gold.", price));
							Game::Mention(std::format("{} paid {} {} gold and rented a room for the night.", player->GetName(), Game::NameOf(npc), price), npc);
						}
					});
				},
				{ arg });
			if (!started) {
				logger::error("rent_room: could not call RentRoom on {}", Game::NameOf(npc));
			}
		}

		// ---- shared activities: a meal together ----

		std::unordered_map<RE::FormID, float> g_lastMeal;  // game hours of the last shared meal
		constexpr float kMealEvery = 6.0f;

		// A seat for the NPC: when the player is already sitting, the free one closest to them;
		// otherwise near the NPC, preferring one with another seat close by (a table for two).
		RE::TESObjectREFR* FindSeat(RE::Actor* npc)
		{
			auto* player = Game::Player();
			const bool playerSits = static_cast<bool>(player->GetOccupiedFurniture().get());
			RE::TESObjectREFR* anchor = playerSits && Game::DistanceToPlayer(npc) < 3000.0f ? static_cast<RE::TESObjectREFR*>(player) : npc;
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
					taken.contains(ref) || ref->GetPosition().GetDistance(anchor->GetPosition()) >= 1500.0f) {
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
			auto* cell = anchor->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				cell->ForEachReference(consider);
			} else if (auto* tes = RE::TES::GetSingleton()) {
				tes->ForEachReferenceInRange(anchor, 1500.0f, consider);
			}
			RE::TESObjectREFR* best = nullptr;
			float bestScore = (std::numeric_limits<float>::max)();
			for (auto* s : seats) {
				const bool paired = std::ranges::any_of(seats, [s](RE::TESObjectREFR* o) {
					return o != s && o->GetPosition().GetDistance(s->GetPosition()) < 300.0f;
				});
				// Next to the seated player: the closest at THEIR table (chairs at one table face it - the
				// player's chair is ahead of or beside this one and the other way round; the next table's
				// chair sits back to back). Otherwise a table for two wins.
				float away = 0.0f;
				if (anchor != npc) {
					if (auto pf = Game::Player()->GetOccupiedFurniture().get()) {
						const auto a = s->GetPosition();
						const auto b = pf->GetPosition();
						const float dx = b.x - a.x, dy = b.y - a.y;
						const float len = std::sqrt(dx * dx + dy * dy);
						if (len > 1.0f) {
							const float sa = s->GetAngleZ(), pa = pf->GetAngleZ();
							const float towardPlayer = (std::sin(sa) * dx + std::cos(sa) * dy) / len;    // this chair faces the player's?
							const float towardThis = (std::sin(pa) * -dx + std::cos(pa) * -dy) / len;     // and the player's faces this one?
							if (towardPlayer < -0.3f || towardThis < -0.3f || len > 400.0f) {
								away = 5000.0f;  // another table
							}
						}
					}
				}
				const float score = s->GetPosition().GetDistance(anchor->GetPosition()) + away + (paired || anchor != npc ? 0.0f : 5000.0f);
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
				Game::Narrate(std::format("{} reminded {} with a smile that they had only just eaten together.", Game::NameOf(npc), player), npc);
				return true;
			}
			g_lastMeal[id] = now;
			Bonds::Add(npc, Bonds::Deed::kMeal, 2);
			// The meal winds down a little later: a moment a loved one might mark with a small gift.
			std::thread([id] {
				std::this_thread::sleep_for(std::chrono::seconds(90));
				SKSE::GetTaskInterface()->AddTask([id] { Keepsakes::OnMoment(id); });
			}).detach();
			const auto dish = Promises::ServeMeal(npc);
			const auto what = dish.empty() ? std::string("what food they had") : dish;
			// Already sitting together: the meal comes to them.
			if (Moments::IsSitting(npc->GetFormID())) {
				Game::Mention(std::format("{} shared {} with {} right where they sat, and they ate together.", Game::NameOf(npc), what, player), npc);
				return true;
			}
			auto* seat = FindSeat(npc);
			if (!seat) {
				logger::info("meal: {} has no seat nearby; sharing it where they stand", Game::NameOf(npc));
				Game::Mention(std::format("{} shared {} with {} right there, and they ate together.", Game::NameOf(npc), what, player), npc);
				return true;
			}
			logger::info("meal: {} heads to seat {:08X}", Game::NameOf(npc), seat->GetFormID());
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
							logger::info("meal: {} couldn't reach the seat; eating where they are", Game::NameOf(npc));
							Game::Mention(std::format("{} shared {} with {} right there, and they ate together.", Game::NameOf(npc), what, player), npc);
							return;
						}
						Papyrus::CallMethod(seat, "ObjectReference", "Activate", nullptr, static_cast<RE::TESObjectREFR*>(npc), false);
						Game::Mention(std::format("{} sat down at the table with {} and invited {} to sit and eat with them.", Game::NameOf(npc), what, player), npc);
					});
				},
				static_cast<RE::TESObjectREFR*>(seat), 1.0f);
			if (!walking) {
				Game::Mention(std::format("{} shared {} with {}, and they ate together.", Game::NameOf(npc), what, player), npc);
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
			offer.npc = Game::NameOf(npc);
			offer.detail = detail;
			offer.showPrice = false;
			offer.seconds = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f));
			offer.acceptLabel = lines.accept;
			offer.declineLabel = lines.decline;
			// SeverActions' TravelToPlace asks with its own destination popup: one question, not two.
			if (title == "Escort" || title == "Show you") {
				offer.sameAs = { "travel" };
			}
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
		void NoteWalkingSoon(RE::FormID npc, bool follow);                                 // below
		void StartFollow(RE::FormID id);                                                    // below
		void StartEscort(RE::FormID id, RE::TESObjectREFR* dest, const std::string& place);  // below

		bool Follow(RE::Actor* npc)
		{
			const auto npcId = npc->GetFormID();
			if (IsCompanion(npc)) {
				logger::info("follow: {} is already a companion", Game::NameOf(npc));
				return true;
			}
			if (FollowsPlayer(npc)) {
				logger::info("follow: {} already follows the player (ours or SeverActions')", Game::NameOf(npc));
				return true;
			}
			const Lines lines{
				Game::Pick({ "Yes, come with me.", "Good. Stay close.", "Come on then, follow me." }),
				Game::Pick({ "Actually, stay here. I'll manage.", "On second thought, never mind.", "No, I'll go alone after all." })
			};
			ConfirmMove(npcId, "Follow you", "Walks with you for a while", lines, [npcId] { StartFollow(npcId); });
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
		bool EndOurFollow(RE::FormID id);  // below
		void MarkFollowAfter(RE::FormID id);  // below
		bool Company(RE::Actor* npc);  // below

		void StartTravel(RE::FormID npcId, RE::TESQuest* quest, std::string place, bool mayRetry)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc) {
				return;
			}
			// Leading now, not following: our follow comes off, or it pulls them back to the player. Once
			// there, they walk with the player again.
			const bool companion = IsCompanion(npc);  // before our follow (and its teammate flag) comes off
			const bool wasFollowing = EndOurFollow(npcId);
			const std::string name = Game::NameOf(npc);
			const bool called = Papyrus::CallMethod(quest, "SeverActions_Travel", "TravelToPlace",
				[npcId, quest, name, place, mayRetry, wasFollowing](const RE::BSScript::Variable& started) {
					const bool ok = !started.IsBool() || started.GetBool();
					logger::info("escort: {} to '{}' -> {}", name, place, ok ? "on the way" : "SeverActions could not place it");
					if (ok) {
						NoteWalkingSoon(npcId, false);
						if (wasFollowing) {
							MarkFollowAfter(npcId);
						}
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
										  Game::NameOf(npc), Game::Player()->GetName(), place),
							npc);
					});
				},
				// A companion keeps following afterwards (stopFollowing false); anyone else goes back
				// to their day after waiting for the player.
				static_cast<RE::Actor*>(npc), RE::BSFixedString(place), 24.0f, !companion, static_cast<std::int32_t>(0), true);
			if (!called) {
				logger::error("escort: SeverActions TravelToPlace could not be called");
			}
		}

		// ---- showing a spot inside a building: their bed, the hearth, the table, the kitchen ----

		struct SpotWords
		{
			const char* kind;
			const char* label;
			std::vector<std::string> phrases;  // whole words, matched inside the place said
		};
		const std::vector<SpotWords>& Spots()
		{
			static const std::vector<SpotWords> kSpots{
				{ "bed", "their bed", { "bed", "beds", "bedroom", "your room", "my room", "her room", "his room", "their room", "guest room",
										  "where you sleep", "where i sleep", "where i can sleep", "where do you sleep", "where do i sleep", "where can i sleep", "sleeping quarters", "bedchamber" } },
				{ "fire", "the hearth", { "fire", "fireplace", "hearth", "firepit", "fire pit", "fireside" } },
				{ "table", "the table", { "table", "eating area", "dining area", "dining table", "dining room", "dinner table", "where we eat",
											"where you eat", "where i can eat", "eating table", "where do we eat", "where do you eat", "where can we eat", "where can i eat" } },
				{ "kitchen", "the kitchen", { "kitchen", "cooking pot", "cookpot", "stove", "oven", "cooking fire" } },
				{ "forge", "the forge", { "forge", "anvil", "workbench", "grindstone", "smelter" } },
				{ "alchemy", "the alchemy table", { "alchemy", "alchemy lab", "alchemy table", "alchemy station" } },
				{ "enchanting", "the enchanter", { "enchanter", "arcane enchanter", "enchanting table", "enchanting" } },
				{ "chest", "the chest", { "chest", "strongbox", "storage chest", "trunk" } },
				{ "bar", "the counter", { "bar", "counter", "bar counter" } },
			};
			return kSpots;
		}

		// "spot:bed" (the classifier) or the words themselves ("your room", "the fireplace"): which spot,
		// "" when it's a place, not a spot. Whole words only ("Candlehearth Hall" is no hearth).
		std::string SpotOf(const std::string& place)
		{
			auto lower = Lower(place);
			if (lower.starts_with("spot:")) {
				lower = lower.substr(5);
				for (auto& s : Spots()) {
					if (lower == s.kind) {
						return s.kind;
					}
				}
			}
			std::string padded = " ";
			for (auto& w : JsonUtil::Words(lower)) {
				padded += w + " ";
			}
			// A more specific spot first: "cooking fire" is the kitchen, not the hearth.
			for (const char* first : { "kitchen", "alchemy", "enchanting" }) {
				for (auto& s : Spots()) {
					if (s.kind == std::string_view(first) &&
						std::ranges::any_of(s.phrases, [&](const std::string& p) { return padded.find(" " + p + " ") != std::string::npos; })) {
						return s.kind;
					}
				}
			}
			for (auto& s : Spots()) {
				if (std::ranges::any_of(s.phrases, [&](const std::string& p) { return padded.find(" " + p + " ") != std::string::npos; })) {
					return s.kind;
				}
			}
			return {};
		}

		std::string SpotLabel(const std::string& kind)
		{
			for (auto& s : Spots()) {
				if (kind == s.kind) {
					return s.label;
				}
			}
			return kind;
		}

		// The spot in the NPC's building: their own bed; the hearth (a fire pit / fireplace model); the
		// table with the most chairs round it; a cooking pot; a forge, alchemy table, enchanter by
		// work-bench type; a chest; the counter. The nearest one to them when there are several.
		RE::TESObjectREFR* FindSpot(RE::Actor* npc, const std::string& kind)
		{
			auto* cell = npc->GetParentCell();
			if (!cell || !cell->IsInteriorCell()) {
				return nullptr;
			}
			if (kind == "bed") {
				if (auto* bed = Promises::FindBedFor(npc)) {
					return bed;
				}
			}
			using BT = RE::TESFurniture::WorkBenchData::BenchType;
			std::vector<RE::TESObjectREFR*> hits, seats;
			auto* base0 = npc->GetActorBase();
			cell->ForEachReference([&](RE::TESObjectREFR* r) {
				auto* base = r ? r->GetBaseObject() : nullptr;
				if (!base || r->IsDisabled() || r->IsDeleted() || r == npc) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* furn = base->As<RE::TESFurniture>();
				const auto bench = furn ? furn->workBenchData.benchType.get() : BT::kNone;
				auto* m = skyrim_cast<RE::TESModel*>(base);
				const auto model = Lower(m && m->GetModel() ? m->GetModel() : "");
				const auto name = Lower(base->GetName() ? base->GetName() : "");
				auto in = [](const std::string& t, std::initializer_list<const char*> ws) {
					return std::ranges::any_of(ws, [&](const char* w) { return t.find(w) != std::string::npos; });
				};
				if (furn && bench == BT::kNone && furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSit) &&
					!furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep)) {
					seats.push_back(r);
				}
				bool hit = false;
				if (kind == "bed") {
					hit = furn && furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) && (r->GetOwner() == base0 || !r->GetOwner());
				} else if (kind == "fire") {
					hit = in(model, { "firepit", "fireplace", "hearth", "campfire" }) ||
					      (in(model, { "fire" }) && !in(model, { "firewood", "firesalt", "fireball", "fireplacefake" }) && !furn &&
							  !base->Is(RE::FormType::Light));
				} else if (kind == "kitchen") {
					hit = (furn && furn->HasKeywordString("CraftingCookpot")) || in(model, { "oven" });
				} else if (kind == "table") {
					hit = !furn && in(model, { "table" }) && !in(model, { "alchemy", "enchant", "tanning", "workbench", "smelter", "endtable", "nightstand" });
				} else if (kind == "forge") {
					hit = (furn && (furn->HasKeywordString("CraftingSmithingForge") || furn->HasKeywordString("CraftingSmithingSkyforge"))) ||
					      bench == BT::kSmithingWeapon || bench == BT::kSmithingArmor || in(model, { "anvil" });
				} else if (kind == "alchemy") {
					hit = bench == BT::kAlchemy;
				} else if (kind == "enchanting") {
					hit = bench == BT::kEnchanting;
				} else if (kind == "chest") {
					hit = base->Is(RE::FormType::Container) && in(name, { "chest", "strongbox", "trunk" });
				} else if (kind == "bar") {
					hit = in(model, { "counter" });
				}
				if (hit) {
					hits.push_back(r);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			RE::TESObjectREFR* best = nullptr;
			float bestScore = (std::numeric_limits<float>::max)();
			for (auto* h : hits) {
				float score = h->GetPosition().GetDistance(npc->GetPosition());
				if (kind == "table") {
					// The eating table: chairs round it.
					score -= 400.0f * static_cast<float>(std::ranges::count_if(seats, [h](RE::TESObjectREFR* s) {
						return s->GetPosition().GetDistance(h->GetPosition()) < 160.0f;
					}));
				} else if (kind == "bed" && h->GetOwner() == base0) {
					score -= 100000.0f;  // their own
				}
				if (score < bestScore) {
					best = h;
					bestScore = score;
				}
			}
			return best;
		}

		// "Home first, then the spot": asked from outside ("show me your room" in the street).
		struct ThenSpot
		{
			std::string kind;
			std::chrono::steady_clock::time_point at{};
		};
		std::mutex g_thenSpotLock;
		std::unordered_map<RE::FormID, ThenSpot> g_thenSpot;

		std::string TakeThenSpot(RE::FormID id)
		{
			std::scoped_lock l(g_thenSpotLock);
			auto it = g_thenSpot.find(id);
			if (it == g_thenSpot.end()) {
				return {};
			}
			auto kind = it->second.kind;
			const bool fresh = std::chrono::steady_clock::now() - it->second.at < std::chrono::minutes(15);
			g_thenSpot.erase(it);
			return fresh ? kind : std::string();
		}

		// Leads the player to the spot in the building they're both in (our escort, at a walk). `ask`:
		// the usual popup first. `followAfter`: back to following the player once there (also when
		// they were following before).
		bool ShowSpot(RE::Actor* npc, const std::string& kind, bool ask, bool followAfter)
		{
			const auto npcId = npc->GetFormID();
			const auto label = SpotLabel(kind);
			auto* spot = FindSpot(npc, kind);
			if (!spot) {
				logger::info("escort: {} - no {} in here", Game::NameOf(npc), label);
				Game::Mention(std::format("{} looked around: there is no {} in here to show {}.", Game::NameOf(npc), label.substr(label.find(' ') + 1),
								  Game::Player()->GetName()),
					npc);
				if (followAfter) {
					StartFollow(npcId);
				}
				return false;
			}
			if (spot->GetPosition().GetDistance(npc->GetPosition()) < 250.0f) {
				logger::info("escort: {} - {} is right here", Game::NameOf(npc), label);
				Game::Mention(std::format("{} showed {} {}, right there beside them.", Game::NameOf(npc), Game::Player()->GetName(), label), npc);
				if (followAfter) {
					StartFollow(npcId);
				}
				return true;
			}
			const auto spotId = spot->GetFormID();
			auto go = [npcId, spotId, label, followAfter] {
				auto* spot = RE::TESForm::LookupByID<RE::TESObjectREFR>(spotId);
				if (!spot) {
					return;
				}
				const bool wasFollowing = EndOurFollow(npcId);
				StartEscort(npcId, spot, label);
				if (wasFollowing || followAfter) {
					MarkFollowAfter(npcId);
				}
			};
			logger::info("escort: {} shows the player {} ({:08X})", Game::NameOf(npc), label, spotId);
			if (!ask) {
				go();
				return true;
			}
			const Lines lines{
				Game::Pick({ "Yes, show me.", "Lead on, I'll follow.", "Please, show me." }),
				Game::Pick({ "Never mind.", "I'll find it myself.", "No need, thanks." })
			};
			ConfirmMove(npcId, "Show you", "Shows you " + label, lines, go);
			return true;
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
			// SeverActions' own TravelToPlace went off on this line and asked already (answered or not):
			// not a second trip, nor a second popup.
			if (Confirm::OtherAskedRecently("travel")) {
				logger::info("escort: SeverActions' travel popup asked {} already; not asked again", where);
				return;
			}
			auto travel = [npcId, quest, where] { StartTravel(npcId, quest, where, true); };
			auto decide = [npcId, where, travel](bool theirsAsks) {
				SKSE::GetTaskInterface()->AddTask([npcId, where, travel, theirsAsks] {
					if (theirsAsks) {
						travel();  // SeverActions' own travel prompt asks: one popup, not two
						return;
					}
					const Lines lines{
						Game::Pick({ "Yes, lead the way to " + where + ".", "Please, take me to " + where + ".", "Lead on, I'll follow you." }),
						Game::Pick({ "No, just point me the right way.", "Never mind, I'll find it myself.", "Just tell me the way, I'll go alone." })
					};
					ConfirmMove(npcId, "Escort", "Leads you to " + where, lines, travel);
				});
			};
			const bool teammate = IsCompanion(npc);
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
		bool Escort(RE::Actor* npc, const std::string& placeSaid, bool ownTrip)
		{
			std::string place = placeSaid;
			if (place.empty()) {
				// Usually chatter during a trip ("not far now, keep up") or "where to?": nothing to start.
				logger::info("escort: {} - no destination named", Game::NameOf(npc));
				return false;
			}
			const auto npcId = npc->GetFormID();
			// A spot inside a building ("show me your room", "the fireplace", "where do we eat"): in the
			// building the player is in with them, they walk there; asked from outside ("show me your
			// room" in the street), home first, then the spot.
			if (const auto kind = SpotOf(place); !kind.empty()) {
				auto* cell = npc->GetParentCell();
				if (cell && cell->IsInteriorCell() && cell == Game::Player()->GetParentCell()) {
					ShowSpot(npc, kind, true, false);
					return true;
				}
				const auto lower = Lower(place);
				if (lower.starts_with("spot:") || lower.find("your ") != std::string::npos || lower.find("where you ") != std::string::npos) {
					{
						std::scoped_lock l(g_thenSpotLock);
						g_thenSpot[npcId] = { kind, std::chrono::steady_clock::now() };
					}
					logger::info("escort: {} - home first, then {}", Game::NameOf(npc), SpotLabel(kind));
					place = "home";
				}
			}
			auto* quest = SeverActionsQuest("SeverActions_Travel");
			if (!quest) {
				return false;
			}
			// Already there: "take me to your home" in their home, or the very place the player stands in.
			{
				auto* cell = Game::Player()->GetParentCell();
				const auto want = Lower(place);
				const bool theirHome = want == "home" || want.starts_with("your ") || want.find("where you live") != std::string::npos;
				const char* cellName = cell ? cell->GetName() : nullptr;
				auto* loc = Game::Player()->GetCurrentLocation();
				const char* locName = loc ? loc->GetName() : nullptr;
				const bool hereByName = (cellName && *cellName && want.size() >= 4 && Lower(cellName).find(want) != std::string::npos) ||
				                        (locName && *locName && want.size() >= 4 && Lower(locName) == want);
				if ((theirHome && Promises::IsHome(cell, npc)) || hereByName) {
					logger::info("escort: {} - already at '{}'", Game::NameOf(npc), place);
					return false;
				}
			}
			auto proceed = [npcId, quest, ownTrip, place = ResolveHome(npc, place)](bool busy) {
				SKSE::GetTaskInterface()->AddTask([npcId, quest, ownTrip, place, busy] {
					auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
					if (!npc) {
						return;
					}
					if (busy) {
						logger::info("escort: {} is already on the way somewhere", Game::NameOf(npc));
						return;
					}
					// A onLocal won't walk the player across Skyrim: another hold is days away.
					// Companions and people heading there anyway are exempt.
					if (!SkyrimNet::ConfigBool("features.escort_local_only", true) || ownTrip || IsCompanion(npc)) {
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
							logger::info("escort: {} won't go to {} ({}): another hold", Game::NameOf(npc), place, farHold);
							Game::Narrate(std::format("{} realised {} is in {}, days of travel away, and they can't just leave {} to take {} there.",
											  Game::NameOf(npc), place, farHold, here ? here->GetName() : "home", Game::Player()->GetName()),
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
				logger::warn("rent_room: {} has no rentable bed", Game::NameOf(npc));
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
				Game::NameOf(npc),
				"A bed for the night",
				price,
				purse,
				static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f))
			};
			const std::string acceptLine = Game::Pick({ "I'll take the room.", "Deal. Here's your coin.", "Fair enough, I'll take it." });
			const std::string declineLine = Game::Pick({ "On second thought, I'll pass.", "Too rich for me, sorry.", "Maybe another time." });
			offer.acceptLabel = acceptLine;
			offer.declineLabel = declineLine;
			offer.sameAs = { "payment", "debt" };
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

	namespace
	{
		bool NearHomeDoor(RE::Actor* npc);  // below
	}

	std::vector<Service> For(RE::Actor* npc)
	{
		std::vector<Service> out;
		if (!npc || npc->IsDead() || npc->IsHostileToActor(Game::Player())) {
			return out;
		}

		if (npc->GetVendorFaction() && SkyrimNet::ConfigBool("features.trade", true)) {
			if (SkyrimNet::ConfigBool("features.trade_talk", true)) {
				out.push_back({ "trade",
					"browse their goods in the trade window (looking around, nothing specific agreed)",
					"\"show me your wares\", \"what are you selling?\", \"got anything good?\", \"let's trade\"" });
				out.push_back({ "buy",
					"buy a specific thing from them NOW, the deal agreed in this exchange (set \"item\", \"qty\", \"price\")",
					"\"I'll take three apples\", \"fine, 20 for the dagger\", \"give me two of those\", \"I'll buy the iron helmet\"" });
				out.push_back({ "sell",
					"sell them a specific thing of the PLAYER's NOW, the deal agreed in this exchange (set \"item\", \"qty\", \"price\")",
					"\"want these wolf pelts?\" - \"aye, 30 for the lot\", \"I'll sell you my iron sword\", \"deal, they're yours\"" });
			} else {
				out.push_back({ "trade",
					"trade with them: see their goods, buy, or sell to them",
					"\"show me your wares\", \"what are you selling?\", \"got anything good?\", \"I need to buy supplies\", \"want to buy some pelts?\"" });
			}
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
		if (SkyrimNet::ConfigBool("features.follow", true) && !IsCompanion(npc) && SeverActionsQuest("SeverActions_Follow")) {
			out.push_back({ "follow",
				"come along with the PLAYER now, walking behind them (the PLAYER leads)",
				"\"follow me\", \"come with me\", \"tag along\", \"stick close\", \"walk with me to the market\", \"come on, let's go\"" });
		}
		if (SkyrimNet::ConfigBool("features.share_meal", true) && (Promises::IsClose(npc) || FollowsPlayer(npc) || InCompany(npc->GetFormID()))) {
			out.push_back({ "share_meal",
				"sit down and share a meal with the PLAYER now (go to a table, eat together)",
				"\"let's eat together\", \"join me for supper?\", \"care to share a meal?\", \"grab a bite with me?\", \"you hungry? let's eat\"" });
		}
		// A guard, and a bounty with their hold: settle it in plain talk (the game decides).
		if (SkyrimNet::ConfigBool("features.guards", true) && Crime::HasBounty(npc)) {
			out.push_back({ "pay_fine", "pay the PLAYER's bounty fine now", "\"I'll pay the fine\", \"how much do I owe? here\", \"fine, I'll pay\"" });
			out.push_back({ "bribe", "the PLAYER offers the guard gold to look the other way (set \"price\" to the gold offered)",
				"\"here's 50 gold to look the other way\", \"maybe this coin can make you forget\"" });
			out.push_back({ "persuade", "the PLAYER talks the guard out of it with reasons or charm", "\"it was a misunderstanding\", \"I was just borrowing it, I swear\", \"come on, let it slide\"" });
			out.push_back({ "intimidate", "the PLAYER threatens or intimidates the guard into dropping it", "\"do you know who I am?\", \"walk away while you still can\"" });
		}
		// A song: anyone may be asked to play or sing (Moments decides how, by who they are and what
		// they carry; whether they will is theirs to say).
		if (SkyrimNet::ConfigBool("features.perform", true)) {
			out.push_back({ "perform",
				"play music and/or sing for the PLAYER now (set \"item\" to the song or instrument named, \"\" for any; \"how\": \"sing\", \"play\" or \"any\")",
				"\"play us something\", \"sing me a song\", \"can you play Ragnar the Red?\", \"give us a tune on your lute\", \"sing for the road\"" });
		}
		// Sitting a while together: anyone may be asked (Moments decides by the relationship and place).
		if (SkyrimNet::ConfigBool("features.sit", true)) {
			out.push_back({ "sit",
				"sit down together NOW (set \"item\" to where: \"fire\", \"ground\", \"bench\", \"\" when not said; \"hours\" when the PLAYER says how long)",
				"\"sit with me a while\", \"let's take a break\", \"let's sit by the fire\", \"watch the stars with me\"" });
		}
		// Going home together to relax: the NPC's home or the player's (they come along; there, they sit).
		if (SkyrimNet::ConfigBool("features.visit", true)) {
			std::string label = "go TOGETHER to a home now, to relax / sit there (set \"item\": \"yours\" = the NPC's own home, \"mine\" = the PLAYER's home or the name of it; someone living with the PLAYER: always \"mine\")";
			if (NearHomeDoor(npc)) {
				label += " - the NPC's home is right here, so \"let's go inside\" means it";
			}
			out.push_back({ "visit", label,
				"\"let's go to your place and relax\", \"come back to mine for a while\", \"let's go home and sit\", \"let's go inside and sit\" at the NPC's door" });
		}
		// Moving in: someone close comes to live in one of the player's houses (SeverActions' home routine).
		if (SkyrimNet::ConfigBool("features.move_in", true) && SeverActionsQuest("SeverActions_FollowerManager")) {
			out.push_back({ "move_in",
				"move into the PLAYER's home to live there for good (the PLAYER picks which house in a popup)",
				"\"come live with me\", \"move in with me\", \"would you share my home?\", the NPC: \"could I live with you?\" and the PLAYER agreeing" });
		}
		// Spending time: staying with the player a while, wherever they are (our follow, kept close).
		if (SkyrimNet::ConfigBool("features.company", true) && !IsCompanion(npc)) {
			out.push_back({ "company",
				"stay with the PLAYER for a while, keeping them company here (not walking somewhere: that's follow; not sitting down: that's sit)",
				"\"keep me company\", \"stay with me a while\", \"spend some time with me\", \"stick around for a bit\", the NPC: \"I'll keep you company\"" });
		}
		// A drink together: anyone may be asked (Moments decides by the real relationship and the place).
		if (SkyrimNet::ConfigBool("features.drink", true)) {
			out.push_back({ "drink",
				"sit down and have a drink together NOW (set \"item\" to the drink named, \"\" for any)",
				"\"a round of mead?\", \"have a drink with me\", \"let's grab an ale\", \"fancy a pint?\", \"join me for a drink\"" });
		}
		// Anyone may be asked; Rest decides by the real relationship (companions and friends say
		// yes, an acquaintance won't bed down with someone they barely know) and shows it.
		if (SkyrimNet::ConfigBool("features.rest", true)) {
			out.push_back({ "rest",
				"go to bed / sleep now, together with or near the PLAYER (beds or bedrolls where they are)",
				"\"let's turn in\", \"time to get some rest\", \"we should sleep\", \"let's make camp for the night\", \"I'm tired, let's bed down\"" });
		}
		// Leaving things with someone: anyone may be asked (a stranger says no, by the real
		// relationship); getting them back from whoever has them.
		if (SkyrimNet::ConfigBool("features.hold", true)) {
			out.push_back({ "hold",
				"keep or carry some of the PLAYER's things for them for now, to give back later (set \"item\")",
				"\"hold this for me\", \"can you keep my sword safe?\", \"carry these potions for a while\", \"watch my pack while I go in\"" });
			if (IsCompanion(npc) || Safekeeping::IsHolding(npc->GetFormID()) || Loans::HasLentTo(npc->GetFormID())) {
				out.push_back({ "retrieve",
					"give back things they are keeping or carrying for the PLAYER (set \"item\")",
					"\"can I have my sword back?\", \"give me back my stuff\", \"I'll take my things now\"" });
			}
		}
		// Care: anyone can be asked to patch the player up or cook (Care decides by the real
		// relationship and their calling); only smiths improve gear, only priests cure disease.
		if (SkyrimNet::ConfigBool("features.care", true)) {
			out.push_back({ "heal",
				"heal the PLAYER's wounds now, with a potion or healing magic",
				"\"I'm hurt\", \"can you patch me up?\", \"I need healing\", \"look at this wound\", \"tend my wounds\"" });
			out.push_back({ "cook",
				"cook a meal for the PLAYER from the PLAYER's own ingredients, at a cooking pot (set \"item\" to the dish or ingredients named)",
				"\"can you cook these?\", \"make me a stew with this venison\", \"cook something with what I've got\"" });
			if (Care::IsSmith(npc)) {
				out.push_back({ "mend",
					"improve / temper / sharpen / repair a weapon or armour of the PLAYER's at the forge (set \"item\")",
					"\"can you sharpen my sword?\", \"improve my armour\", \"fix up this axe\", \"temper my blade\"" });
			}
			if (Care::IsPriest(npc)) {
				out.push_back({ "cure",
					"cure the PLAYER of a disease",
					"\"I've caught something\", \"I feel sick\", \"can you cure my disease?\", \"I think I have rockjoint\"" });
			}
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

	RE::TESQuest* SeverActionsQuestFor(const char* script)
	{
		return SeverActionsQuest(script);
	}

	bool IsActivity(const std::string& id)
	{
		return id == "share_meal" || id == "drink" || id == "sit" || id == "visit" || id == "company" || id == "move_in" || id == "perform" || id == "rest" || id == "hold" || id == "retrieve" ||
		       IsCare(id);
	}

	RE::TESObjectREFR* SeatFor(RE::Actor* npc)
	{
		return npc ? FindSeat(npc) : nullptr;
	}

	std::string NearestOfKind(RE::Actor* npc, const std::string& kind)
	{
		return npc ? RealPlaceFor(npc, "the " + kind) : "";
	}

	namespace
	{
		// Our own follow and escort: Mundane Matters owns them start to finish (kept in our save data),
		// so no other mod's seats, sweeps or cleanups can drop them.
		//  - follow: SkyrimNet's own "FollowPlayer" package (SkyrimNet then treats them as a companion:
		//    no stopping to talk, no being pulled into other NPCs' chats), put back whenever it's missing;
		//  - escort: they walk to the place SeverActions' resolver names, and wait whenever the player
		//    falls behind;
		//  - left behind through a door, or far away: placed right behind the player.
		// Ends only on the player's word ("wait here", "go home", "you can go") or an escort arriving.
		struct Walk
		{
			bool follow = false;  // a follow, else an escort
			RE::FormID dest = 0;  // escort: the marker they lead to
			std::string place;    // escort: its name
			std::chrono::steady_clock::time_point since{};
			std::chrono::steady_clock::time_point lastMove{};     // last placed behind the player
			std::chrono::steady_clock::time_point lastPath{};     // escort: last told to walk on
			std::chrono::steady_clock::time_point lastFollow{};   // follow: last (re)applied
			bool waiting = false;  // escort: holding for the player
			bool followAfter = false;  // escort: was following the player before; follows again once there
			std::uint32_t pathSeq = 0;  // escort: the latest walk order (an older one's end is ignored)
			std::chrono::steady_clock::time_point sceneStopped{};
			int apart = 0;         // ticks in a row away from the player (another cell, or far)
			bool company = false;  // follow: spending time together (ends on its own, see InCompany)
			float until = 0.0f;    // company: game hours it lasts until
			float lastHours = 0.0f;  // company: game hours at the last look (a jump = slept / waited)
			float bondAt = 0.0f;   // company: game hours of the last Bonds point
			int bonds = 0;         // company: Bonds points given so far
			std::chrono::steady_clock::time_point playerSatAt{};  // company: when the player sat down (0 = standing)
			bool satWith = false;  // company: sat down beside the player this time
			bool teammate = false;  // follow: made the player's teammate by us (taken off again when it ends)
		};
		std::mutex g_walkLock;
		std::unordered_map<RE::FormID, Walk> g_walking;
		std::chrono::steady_clock::time_point g_loadedAt{};
		constexpr std::int32_t kFollowPriority = 60;

		void NoteWalking(RE::FormID npc, bool follow)
		{
			std::scoped_lock l(g_walkLock);
			auto& w = g_walking[npc];
			w.since = std::chrono::steady_clock::now();
			w.follow = follow;
		}

		// SeverActions' "actively following" faction. Its daily schedule (home, work, relax spots;
		// packages above ours) stands aside only for someone in it who also has the FollowPlayer
		// package. Without it, an NPC with a SeverActions home walks off home mid-follow.
		void MarkFollowing(RE::Actor* a, bool on)
		{
			if (!a) {
				return;
			}
			// A stray blow in a shared fight doesn't turn them on the player (Trust calms them if it does).
			if (!IsCompanion(a)) {
				Papyrus::CallMethod(a, "Actor", "IgnoreFriendlyHits", nullptr, on);
			}
			auto* faction = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESFaction>(0x0F0809, "SeverActions.esp");
			if (!faction) {
				return;
			}
			if (on) {
				Papyrus::CallMethod(a, "Actor", "SetFactionRank", nullptr, static_cast<RE::TESFaction*>(faction), static_cast<std::int32_t>(0));
			} else {
				Papyrus::CallMethod(a, "Actor", "RemoveFromFaction", nullptr, static_cast<RE::TESFaction*>(faction));
			}
		}

		// The game's teammate flag while they walk with the player: only a teammate shrugs off a stray
		// blow in a shared fight (anyone else turns on the player at once). No favours (that's the
		// follower system's), and taken off when the follow ends.
		void SetTeammate(RE::Actor* a, bool on)
		{
			Papyrus::CallMethod(a, "Actor", "SetPlayerTeammate", nullptr, on, false);
			logger::info("follow: {} {} the player's teammate", a->GetName(), on ? "is now" : "is no longer");
		}

		void ApplyFollow(RE::Actor* a)
		{
			Papyrus::CallStatic("SkyrimNetApi", "RegisterPackage", nullptr, a, RE::BSFixedString("FollowPlayer"), kFollowPriority, static_cast<std::int32_t>(0), true);
			MarkFollowing(a, true);
			bool team = false;
			{
				std::scoped_lock l(g_walkLock);
				if (auto it = g_walking.find(a->GetFormID()); it != g_walking.end() && it->second.follow && !it->second.teammate && !a->IsPlayerTeammate()) {
					it->second.teammate = team = true;
				}
			}
			if (team) {
				SetTeammate(a, true);
			}
			Papyrus::CallMethod(a, "Actor", "EvaluatePackage", nullptr);
		}

		// Beside the player, a step ahead: never behind them, where the door they just came through is
		// (placed on it, they'd go straight back through).
		void PlaceBehindPlayer(RE::Actor* a)
		{
			auto* p = Game::Player();
			const float angle = p->GetAngleZ();
			const float fx = std::sin(angle), fy = std::cos(angle);  // forward
			const float rx = std::cos(angle), ry = -std::sin(angle);  // right
			Papyrus::CallMethod(a, "ObjectReference", "MoveTo", nullptr, static_cast<RE::TESObjectREFR*>(p), fx * 90.0f + rx * 110.0f, fy * 90.0f + ry * 110.0f, 0.0f,
				true);
			Papyrus::CallMethod(a, "Actor", "EvaluatePackage", nullptr);
		}

		void OnPathDone(RE::FormID id, std::uint32_t seq, bool ok);  // below

		// Escort: on towards the place, at a walk.
		void WalkOn(RE::FormID id, RE::FormID destId)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			auto* dest = RE::TESForm::LookupByID<RE::TESObjectREFR>(destId);
			if (!a || !dest) {
				return;
			}
			std::uint32_t seq = 0;
			{
				std::scoped_lock l(g_walkLock);
				if (auto it = g_walking.find(id); it != g_walking.end()) {
					seq = ++it->second.pathSeq;
				}
			}
			Papyrus::CallMethod(a, "Actor", "SetDontMove", nullptr, false);
			Papyrus::CallMethod(a, "Actor", "PathToReference",
				[id, seq](const RE::BSScript::Variable& r) {
					const bool ok = r.IsBool() && r.GetBool();
					SKSE::GetTaskInterface()->AddTask([id, seq, ok] { OnPathDone(id, seq, ok); });
				},
				dest, 0.6f);
		}

		// SeverActions' casual follow (its StartFollowing action) links them to the player with this keyword.
		RE::BGSKeyword* SeverFollowKeyword()
		{
			static RE::BGSKeyword* kw = [] {
				for (auto* k : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::BGSKeyword>()) {
					if (k && k->formEditorID == "SeverActions_FollowerFollowKW") {
						return k;
					}
				}
				return static_cast<RE::BGSKeyword*>(nullptr);
			}();
			return kw;
		}

		// A real companion, or following through SeverActions: their own follow, not ours to run.
		bool FollowState(RE::Actor* npc)
		{
			if (npc->IsPlayerTeammate()) {
				return true;
			}
			auto* kw = SeverFollowKeyword();
			return kw && npc->GetLinkedRef(kw) == Game::Player();
		}

		void Arrived(RE::FormID id, const std::string& place)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			bool followAfter = false;
			{
				std::scoped_lock l(g_walkLock);
				if (auto it = g_walking.find(id); it != g_walking.end()) {
					followAfter = it->second.followAfter;
					g_walking.erase(it);
				}
			}
			if (!a) {
				return;
			}
			Papyrus::CallMethod(a, "Actor", "SetDontMove", nullptr, false);
			Papyrus::CallMethod(a, "Actor", "EvaluatePackage", nullptr);
			logger::info("escort: {} brought the player to {}{}", a->GetName(), place, followAfter ? "; walking with the player again" : "");
			Game::Mention(std::format("{} brought {} to {}.", a->GetName(), Game::Player()->GetName(), place), a);
			if (followAfter) {
				StartFollow(id);
			}
		}

		void OnPathDone(RE::FormID id, std::uint32_t seq, bool ok)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			std::string place;
			RE::FormID destId = 0;
			{
				std::scoped_lock l(g_walkLock);
				auto it = g_walking.find(id);
				if (!a || it == g_walking.end() || it->second.follow || it->second.pathSeq != seq) {
					return;  // gone, or an older walk order's end
				}
				place = it->second.place;
				destId = it->second.dest;
				it->second.lastPath = {};  // walk on again at the next look, unless there
			}
			auto* dest = RE::TESForm::LookupByID<RE::TESObjectREFR>(destId);
			if (dest && a->GetPosition().GetDistance(dest->GetPosition()) < 500.0f) {
				Arrived(id, place);
			} else if (!ok) {
				logger::info("escort: {}'s path to {} broke off; walking on", a->GetName(), place);
			}
		}
	}

	namespace
	{
		void NoteWalkingSoon(RE::FormID npc, bool follow)
		{
			NoteWalking(npc, follow);
		}

		// SeverActions lets go of them first (its casual follow, its seat, its load sweeps), so only
		// one follow runs - ours.
		void ReleaseFromSeverActions(RE::Actor* a, std::function<void()> then)
		{
			auto* quest = SeverActionsQuest("SeverActions_Follow");
			auto done = [then](const RE::BSScript::Variable&) { SKSE::GetTaskInterface()->AddTask(then); };
			if (!quest || !Papyrus::CallMethod(quest, "SeverActions_Follow", "StopFollowing", done, static_cast<RE::Actor*>(a))) {
				then();
			}
		}

		void StartFollow(RE::FormID id)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!a) {
				return;
			}
			bool wasEscort = false;
			{
				std::scoped_lock l(g_walkLock);
				auto& w = g_walking[id];
				wasEscort = !w.follow && w.dest;
				w = Walk{};
				w.follow = true;
				w.since = std::chrono::steady_clock::now();
				// Not "missing" while SeverActions lets go and ours goes on (below): putting it back
				// meanwhile only gets cleared again by SeverActions' clean-up.
				w.lastFollow = w.since + std::chrono::seconds(30);
			}
			if (wasEscort) {
				Papyrus::CallMethod(a, "Actor", "SetDontMove", nullptr, false);
			}
			// SeverActions lets go once, and its own clean-up (which also removes SkyrimNet's follow
			// package) is given 2 s to finish before ours goes on.
			ReleaseFromSeverActions(a, [id] {
				std::thread([id] {
					std::this_thread::sleep_for(std::chrono::seconds(2));
					SKSE::GetTaskInterface()->AddTask([id] {
						if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && WalkingWithPlayer(id)) {
							ApplyFollow(a);
							{
								std::scoped_lock l(g_walkLock);
								if (auto it = g_walking.find(id); it != g_walking.end()) {
									it->second.lastFollow = std::chrono::steady_clock::now();
								}
							}
							logger::info("follow: {} now follows the player (our follow)", a->GetName());
						}
					});
				}).detach();
			});
		}

		void StartEscort(RE::FormID id, RE::TESObjectREFR* dest, const std::string& place)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!a || !dest) {
				return;
			}
			{
				std::scoped_lock l(g_walkLock);
				auto& w = g_walking[id];
				w = Walk{};
				w.dest = dest->GetFormID();
				w.place = place;
				w.since = w.lastPath = std::chrono::steady_clock::now();
			}
			// Leading now, not following: any follow of theirs ends first.
			const auto destId = dest->GetFormID();
			ReleaseFromSeverActions(a, [id, destId] { WalkOn(id, destId); });
			logger::info("escort: {} leads the player to {} (our escort)", a->GetName(), place);
		}
	}

	namespace
	{
		void MarkFollowAfter(RE::FormID id)
		{
			std::scoped_lock l(g_walkLock);
			if (auto it = g_walking.find(id); it != g_walking.end()) {
				it->second.followAfter = true;
			}
		}

		bool EndOurFollow(RE::FormID id)
		{
			bool had = false, team = false;
			{
				std::scoped_lock l(g_walkLock);
				auto it = g_walking.find(id);
				if (it != g_walking.end() && it->second.follow) {
					team = it->second.teammate;
					g_walking.erase(it);
					had = true;
				}
			}
			if (had) {
				if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) {
					Papyrus::CallStatic("SkyrimNetApi", "UnregisterPackage", nullptr, a, RE::BSFixedString("FollowPlayer"));
					MarkFollowing(a, false);
					if (team) {
						SetTeammate(a, false);
					}
					logger::info("follow: {} now leads the way instead", a->GetName());
				}
			}
			return had;
		}
	}

	namespace
	{
		std::string Padded(const std::string& line)
		{
			std::string l = line;
			for (std::size_t at; (at = l.find("\xE2\x80\x99")) != std::string::npos;) {
				l.replace(at, 3, "'");
			}
			std::string out = " ";
			for (auto& w : JsonUtil::Words(JsonUtil::Lower(l))) {
				out += w + " ";
			}
			return out;
		}

		bool HasAny(const std::string& padded, std::initializer_list<const char*> phrases)
		{
			return std::ranges::any_of(phrases, [&](const char* p) { return padded.find(std::string(" ") + p + " ") != std::string::npos; });
		}

		// The line without its questions: "I must go. Will you be all right?" -> "I must go."
		std::string Statements(const std::string& line)
		{
			std::string out, cur;
			for (char c : line) {
				cur += c;
				if (c == '.' || c == '!' || c == '?' || c == '\n') {
					if (c != '?') {
						out += cur + " ";
					}
					cur.clear();
				}
			}
			return out + cur;
		}

		// "Keep me company": our follow, close by, with the company ends (TickCompany).
		void StartCompany(RE::FormID id)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!a) {
				return;
			}
			StartFollow(id);
			const float now = Game::Hours();
			const float hours = SkyrimNet::ConfigFloat("tuning.company_hours", 3.0f);
			{
				std::scoped_lock l(g_walkLock);
				if (auto it = g_walking.find(id); it != g_walking.end()) {
					it->second.company = true;
					it->second.until = now + (std::max)(0.5f, hours);
					it->second.lastHours = now;
					it->second.bondAt = now;
				}
			}
			logger::info("company: {} spends time with the player (until {:.1f} h from now)", a->GetName(), hours);
		}

		// True when it ended. Game hours: over at its end, or when time jumps (slept, waited).
		// Each hour together is a Bonds point (up to 3).
		bool TickCompany(RE::FormID id, RE::Actor* a)
		{
			const float now = Game::Hours();
			bool skipped = false, over = false;
			bool bond = false;
			{
				std::scoped_lock l(g_walkLock);
				auto it = g_walking.find(id);
				if (it == g_walking.end() || !it->second.company) {
					return false;
				}
				auto& w = it->second;
				// Hours passed sitting together (Moments' "pass the time") are time spent together, not a skip.
				skipped = w.lastHours > 0.0f && now - w.lastHours > 1.0f && !Moments::IsSitting(id);
				w.lastHours = now;
				over = now >= w.until;
				if (!skipped && !over && now - w.bondAt >= 1.0f && w.bonds < 3) {
					w.bondAt = now;
					++w.bonds;
					bond = true;
				}
			}
			if (bond) {
				Bonds::Add(a, Bonds::Deed::kSat, 1);
				logger::info("company: an hour with {}", a->GetName());
			}
			// The player sits down: after a moment, so do they, beside them (where the relationship lets
			// them: a friend anywhere, anyone outdoors; Moments has the rest of the rules).
			{
				const bool playerSits = Moments::PlayerSitting();
				const auto realNow = std::chrono::steady_clock::now();
				bool sit = false;
				{
					std::scoped_lock l(g_walkLock);
					if (auto it = g_walking.find(id); it != g_walking.end()) {
						auto& w = it->second;
						if (!playerSits) {
							w.playerSatAt = {};
							w.satWith = false;
						} else if (w.playerSatAt == std::chrono::steady_clock::time_point{}) {
							w.playerSatAt = realNow;
						} else if (!w.satWith && realNow - w.playerSatAt > std::chrono::seconds(3)) {
							w.satWith = true;
							// Spending time together: they sit with the player whoever they are (a seat, the
							// edge of a bed, the ground: Moments picks).
							sit = !Moments::IsSitting(id) && !a->GetOccupiedFurniture().get() && Game::DistanceToPlayer(a) < 800.0f;
						}
					}
				}
				if (sit) {
					logger::info("company: the player sat down; {} sits too", a->GetName());
					Moments::Sit(a, "", 0.0f);
				}
			}
			const std::string player = Game::Player()->GetName();
			if (skipped) {
				EndCompany(id, "time passed (slept / waited)", std::format("The hours had passed; {} went back to their own day, the time with {} over for now.", a->GetName(), player));
				return true;
			}
			if (over) {
				EndCompany(id, "the hours were up", std::format("{} realised how late it had got, and had to get on with their own day.", a->GetName()));
				return true;
			}
			return false;
		}

		bool Company(RE::Actor* npc)
		{
			const auto npcId = npc->GetFormID();
			if (FollowState(npc)) {
				logger::info("company: {} is with the player already (a companion, or following through SeverActions)", Game::NameOf(npc));
				return true;
			}
			if (InCompany(npcId)) {
				logger::info("company: {} is already spending time with the player", Game::NameOf(npc));
				return true;
			}
			const Lines lines{
				Game::Pick({ "Stay a while, then.", "Good. Stay with me.", "I'd like that." }),
				Game::Pick({ "Actually, another time.", "On second thought, never mind.", "No, go on with your day." })
			};
			ConfirmMove(npcId, "Spend time", "Stays with you a while", lines, [npcId] { StartCompany(npcId); });
			return true;
		}

		// Woken by the player's voice: up and by the player, answering; back to bed after a quiet while.
		struct Woken
		{
			std::chrono::steady_clock::time_point lastTalk{};
			std::chrono::steady_clock::time_point at{};
			std::string line;
			bool answered = false;
			bool nudged = false;
		};
		std::mutex g_wokenLock;
		std::unordered_map<RE::FormID, Woken> g_woken;

		void TickWoken()
		{
			const auto now = std::chrono::steady_clock::now();
			std::vector<RE::FormID> sleepAgain;
			std::vector<std::pair<RE::FormID, std::string>> nudge;
			{
				std::scoped_lock l(g_wokenLock);
				for (auto it = g_woken.begin(); it != g_woken.end();) {
					auto& w = it->second;
					// Woken but silent (SkyrimNet skipped them while they slept): asked to answer.
					if (!w.answered && !w.nudged && now - w.at > std::chrono::seconds(5)) {
						w.nudged = true;
						nudge.emplace_back(it->first, w.line);
					}
					if (now - w.lastTalk > std::chrono::seconds(90)) {
						sleepAgain.push_back(it->first);
						it = g_woken.erase(it);
					} else {
						++it;
					}
				}
			}
			for (auto& [id, line] : nudge) {
				if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) {
					Game::Narrate(std::format("{} had been asleep, and {}'s voice woke them: \"{}\"", a->GetName(), Game::Player()->GetName(), line), a);
				}
			}
			for (auto id : sleepAgain) {
				auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!a || WalkingWithPlayer(id)) {
					continue;  // following / spending time now: theirs to keep
				}
				Papyrus::CallStatic("SkyrimNetApi", "UnregisterPackage", nullptr, a, RE::BSFixedString("FollowPlayer"));
				MarkFollowing(a, false);
				Papyrus::CallMethod(a, "Actor", "EvaluatePackage", nullptr);
				logger::info("wake: {} goes back to their day (no talk for a while)", a->GetName());
			}
		}
	}

	// ---- going home together ("let's go to your place and relax", "come back to mine") ----
	// They come along (our follow, or the follow they already have), and only as far as the house: there
	// the follow ends and they sit down, asking the player to join them. From then on it's a sit, never a
	// follow. Another door on the way gets a comment; a few hours without getting there, they give up.
	// Someone who moved in with the player: "my", "your" or "our" home is the one they share.
	namespace
	{
		enum class Whose
		{
			kTheirs = 1,
			kPlayers = 2
		};

		struct Visit
		{
			Whose whose = Whose::kTheirs;
			float until = 0.0f;        // game hours: given up after this
			RE::FormID wrongCell = 0;  // the last interior commented on (or the one they set off from)
			RE::FormID home = 0;       // theirs: the home cell; the player's: the house location (0 = any of theirs)
			bool ours = false;         // our follow, ended at the door; else their own (a companion's) goes on
			std::string name;          // the house
		};
		std::unordered_map<RE::FormID, Visit> g_visits;  // under g_walkLock
		constexpr float kVisitHours = 3.0f;

		std::string TownOf(RE::BGSLocation* loc)
		{
			for (int depth = 0; loc && depth < 12; ++depth, loc = loc->parentLoc) {
				if (loc->HasKeywordString("LocTypeCity") || loc->HasKeywordString("LocTypeTown") || loc->HasKeywordString("LocTypeSettlement")) {
					return loc->GetName() ? loc->GetName() : "";
				}
			}
			return "";
		}

		// Somewhere to walk to now: the same town, or (a farm, a lone house) the same hold. Unknown counts as near.
		bool WalkableFromHere(RE::BGSLocation* loc)
		{
			auto* here = Game::Player()->GetCurrentLocation();
			const auto there = TownOf(loc), town = TownOf(here);
			if (!there.empty() && !town.empty()) {
				return there == town;
			}
			auto* a = HoldAbove(loc);
			auto* b = HoldAbove(here);
			return !a || !b || a == b;
		}

		std::string FarName(RE::BGSLocation* loc)
		{
			if (auto town = TownOf(loc); !town.empty()) {
				return town;
			}
			auto* hold = HoldAbove(loc);
			return hold && hold->GetName() ? hold->GetName() : "another part of Skyrim";
		}

		bool IsPlayerHouse(RE::TESObjectCELL* cell)
		{
			if (!cell || !cell->IsInteriorCell()) {
				return false;
			}
			if (cell->GetOwner() == Game::Player()->GetActorBase()) {
				return true;
			}
			auto* loc = cell->GetLocation();
			return loc && (loc->HasKeywordString("LocTypePlayerHouse") || (loc->parentLoc && loc->parentLoc->HasKeywordString("LocTypePlayerHouse")));
		}

		bool InHouse(RE::TESObjectCELL* cell, RE::FormID house)
		{
			auto* loc = cell ? cell->GetLocation() : nullptr;
			return loc && (loc->GetFormID() == house || (loc->parentLoc && loc->parentLoc->GetFormID() == house));
		}

		// Their home interior: the one they're placed in when it counts as home, else an interior they
		// own (their own before their household's). Cached: the game data doesn't change.
		RE::TESObjectCELL* HomeCellOf(RE::Actor* npc)
		{
			if (auto* x = npc->extraList.GetByType<RE::ExtraPersistentCell>();
				x && x->persistentCell && x->persistentCell->IsInteriorCell() && Promises::IsHome(x->persistentCell, npc)) {
				return x->persistentCell;
			}
			static std::unordered_map<RE::FormID, RE::FormID> cache;  // game thread only
			if (auto it = cache.find(npc->GetFormID()); it != cache.end()) {
				return it->second ? RE::TESForm::LookupByID<RE::TESObjectCELL>(it->second) : nullptr;
			}
			RE::TESObjectCELL* own = nullptr;
			RE::TESObjectCELL* household = nullptr;
			auto* base = npc->GetActorBase();
			for (auto* cell : RE::TESDataHandler::GetSingleton()->interiorCells) {
				auto* owner = cell ? cell->GetOwner() : nullptr;
				if (!owner) {
					continue;
				}
				if (owner == base) {
					own = cell;
					break;
				}
				if (!household && owner->As<RE::TESFaction>() && Promises::Belongs(owner, npc) && cell->GetFullName() && *cell->GetFullName()) {
					household = cell;
				}
			}
			auto* home = own ? own : household;
			cache[npc->GetFormID()] = home ? home->GetFormID() : 0;
			return home;
		}

		struct VisitPlan
		{
			bool ok = false;
			const char* why = "";  // not ok: "no home", "too far"
			std::string name;      // the house
			std::string farIn;       // too far: the town or hold it's in
			RE::FormID home = 0;   // see Visit::home
		};

		VisitPlan PlanVisit(RE::Actor* npc, Whose whose, const std::string& said = "")
		{
			VisitPlan p;
			if (whose == Whose::kTheirs) {
				auto* cell = HomeCellOf(npc);
				if (!cell) {
					p.why = "no home";
					return p;
				}
				p.home = cell->GetFormID();
				const char* full = cell->GetFullName();
				p.name = full && *full ? full : HomeName(npc);
				if (p.name.empty()) {
					p.name = std::format("{}'s home", Game::NameOf(npc));
				}
				if (!WalkableFromHere(cell->GetLocation())) {
					p.why = "too far";
					p.farIn = FarName(cell->GetLocation());
					return p;
				}
				p.ok = true;
				return p;
			}
			// Living with the player: the house they share.
			if (auto [loc, name] = MoveIn::SharedHouse(npc->GetFormID()); loc) {
				p.home = loc->GetFormID();
				p.name = name.empty() && loc->GetName() ? loc->GetName() : name;
				if (!WalkableFromHere(loc)) {
					p.why = "too far";
					p.farIn = FarName(loc);
					return p;
				}
				p.ok = true;
				return p;
			}
			// The player's own houses. One named ("Breezehome") is the one; else those in walking reach:
			// one is that one, several are any of them (the player leads: the door they take decides).
			const auto houses = MoveIn::PlayerHouses();
			if (houses.empty()) {
				p.why = "no home";
				return p;
			}
			const auto saidLower = Lower(said);
			const std::pair<RE::BGSLocation*, std::string>* named = nullptr;
			std::vector<const std::pair<RE::BGSLocation*, std::string>*> inReach;
			for (auto& h : houses) {
				const auto n = Lower(h.second);
				if (!named && saidLower.size() >= 4 && n.size() >= 4 && (saidLower.find(n) != std::string::npos || n.find(saidLower) != std::string::npos)) {
					named = &h;
				}
				if (WalkableFromHere(h.first)) {
					inReach.push_back(&h);
				}
			}
			if (named) {
				p.home = named->first->GetFormID();
				p.name = named->second;
				if (!WalkableFromHere(named->first)) {
					p.why = "too far";
					p.farIn = FarName(named->first);
					return p;
				}
				p.ok = true;
				return p;
			}
			if (inReach.size() == 1) {
				p.ok = true;
				p.home = inReach.front()->first->GetFormID();
				p.name = inReach.front()->second;
				return p;
			}
			if (inReach.size() > 1) {
				p.ok = true;
				p.home = 0;  // any of them
				for (std::size_t i = 0; i < inReach.size(); ++i) {
					p.name += std::string(i == 0 ? "" : i + 1 == inReach.size() ? " or " : ", ") + inReach[i]->second;
				}
				return p;
			}
			p.why = "too far";
			p.name = houses.front().second;
			p.farIn = FarName(houses.front().first);
			return p;
		}

		bool AtVisitHome(RE::TESObjectCELL* cell, RE::Actor* npc, Whose whose, RE::FormID home)
		{
			if (!cell || !cell->IsInteriorCell()) {
				return false;
			}
			if (whose == Whose::kPlayers) {
				return home ? InHouse(cell, home) : IsPlayerHouse(cell);
			}
			return cell->GetFormID() == home || Promises::IsHome(cell, npc);
		}

		Whose WhoseOf(RE::Actor* npc, const std::string& item)
		{
			if (MoveIn::SharedHouse(npc->GetFormID()).first) {
				return Whose::kPlayers;  // "my", "your", "our": the one home they share
			}
			const auto p = Padded(item);
			if (HasAny(p, { "mine", "my", "our", "ours", "player", "players", "player s" })) {
				return Whose::kPlayers;
			}
			// One of the player's houses by name ("Breezehome").
			const auto said = Lower(item);
			for (auto& [loc, name] : MoveIn::PlayerHouses()) {
				const auto n = Lower(name);
				if (said.size() >= 4 && n.size() >= 4 && (said.find(n) != std::string::npos || n.find(said) != std::string::npos)) {
					return Whose::kPlayers;
				}
			}
			return Whose::kTheirs;
		}

		// What the NPC is told when it can't happen (they say so, in their own words).
		std::string CantVisit(RE::Actor* npc, Whose whose, const VisitPlan& p)
		{
			const std::string name = Game::NameOf(npc), player = Game::Player()->GetName();
			const bool noHome = std::string_view(p.why) == "no home";
			if (whose == Whose::kTheirs) {
				return noHome ? std::format("{} had no home of their own to take {} back to.", name, player) :
				                std::format("{}'s home, {}, is in {}: much too far to go and sit there now.", name, p.name, p.farIn);
			}
			return noHome ? std::format("{} has no home of their own to go back to.", player) :
			                std::format("{}'s home, {}, is in {}: much too far to go and sit there now.", player, p.name, p.farIn);
		}

		bool OurWalk(RE::FormID id)
		{
			std::scoped_lock l(g_walkLock);
			auto it = g_walking.find(id);
			return it != g_walking.end() && it->second.follow && !it->second.company;
		}

		// There: the follow (ours) ends, and a moment later they sit down and ask the player to join.
		void ArriveHome(RE::FormID id, const Visit& v)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!a) {
				return;
			}
			if (v.ours) {
				StopWalking(id, "home reached");
			}
			logger::info("visit: {} reached {} with the player; sits down", a->GetName(), v.name);
			std::thread([id, v] {
				std::this_thread::sleep_for(std::chrono::seconds(2));  // the follow's clean-up first
				SKSE::GetTaskInterface()->AddTask([id, v] {
					auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
					if (!a || a->IsDead() || a->IsInCombat()) {
						return;
					}
					Moments::Sit(a, "", 0.0f);
					if (!Moments::IsSitting(id)) {
						return;  // no room to sit: Moments has told them
					}
					const std::string name = a->GetName(), player = Game::Player()->GetName();
					Game::Notify(std::format("{} sits down and invites you to join.", name));
					Game::Narrate(v.whose == Whose::kTheirs ?
									  std::format("Back home at {}, {} sat down and asked {} to sit with them and relax a while.", v.name, name, player) :
									  std::format("Home at {}, {} sat down and asked {} to come and sit with them a while.", v.name, name, player),
						a);
				});
			}).detach();
		}

		void TickVisits()
		{
			auto* p = Game::Player();
			auto* cell = p->GetParentCell();
			std::vector<std::pair<RE::FormID, Visit>> list;
			{
				std::scoped_lock l(g_walkLock);
				list.assign(g_visits.begin(), g_visits.end());
			}
			const float now = Game::Hours();
			auto drop = [](RE::FormID id) {
				std::scoped_lock l(g_walkLock);
				g_visits.erase(id);
			};
			for (auto& [id, v] : list) {
				auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!a || a->IsDead()) {
					drop(id);
					continue;
				}
				if (a->IsInCombat() || p->IsInCombat()) {
					continue;
				}
				// The follow ended some other way ("wait here", a goodbye): no visit either.
				if (v.ours ? !OurWalk(id) : !FollowsPlayer(a)) {
					drop(id);
					logger::info("visit: {} no longer walking with the player; the visit is off", a->GetName());
					continue;
				}
				if (now > v.until) {
					drop(id);
					if (v.ours) {
						StopWalking(id, "the visit took too long");
					}
					logger::info("visit: {} gave up on going to {}", a->GetName(), v.name);
					Game::Mention(std::format("{} and {} never did get to {}; {} gave up on it and went back to their own day.", a->GetName(),
									  p->GetName(), v.name, a->GetName()),
						a);
					continue;
				}
				if (!cell || !cell->IsInteriorCell() || a->GetParentCell() != cell) {
					continue;
				}
				if (AtVisitHome(cell, a, v.whose, v.home)) {
					drop(id);
					ArriveHome(id, v);
					continue;
				}
				if (cell->GetFormID() != v.wrongCell) {
					{
						std::scoped_lock l(g_walkLock);
						if (auto it = g_visits.find(id); it != g_visits.end()) {
							it->second.wrongCell = cell->GetFormID();
						}
					}
					const char* here = cell->GetFullName();
					logger::info("visit: {} followed the player into {} on the way to {}", a->GetName(), here ? here : "?", v.name);
					Game::Narrate(std::format("{} followed {} into {}, though they were meant to be going to {} to relax.", a->GetName(), p->GetName(),
									  here && *here ? here : "somewhere else", v.name),
						a);
				}
			}
		}

		bool StartVisit(RE::Actor* npc, const std::string& item)
		{
			const auto id = npc->GetFormID();
			const auto whose = WhoseOf(npc, item);
			const auto plan = PlanVisit(npc, whose, item);
			auto* player = Game::Player();
			if (!plan.ok) {
				logger::info("visit: {} - {} ({})", Game::NameOf(npc), plan.why, plan.name.empty() ? "-" : plan.name);
				Game::Narrate(CantVisit(npc, whose, plan), npc);
				return true;
			}
			auto* cell = player->GetParentCell();
			// Already there together: straight to the sit.
			if (AtVisitHome(cell, npc, whose, plan.home) && npc->GetParentCell() == cell) {
				logger::info("visit: {} - already at {}; sits", Game::NameOf(npc), plan.name);
				return Moments::Sit(npc, "", 0.0f);
			}
			Visit v;
			v.whose = whose;
			v.until = Game::Hours() + kVisitHours;
			v.wrongCell = cell && cell->IsInteriorCell() ? cell->GetFormID() : 0;  // where they set off from: no comment
			v.home = plan.home;
			v.name = plan.name;
			if (OurWalk(id)) {
				v.ours = true;  // following already: it ends there all the same
			} else if (FollowsPlayer(npc)) {
				v.ours = false;  // a companion, SeverActions' follow, time together: theirs goes on after
			} else {
				Moments::Release(id);
				StartFollow(id);
				v.ours = true;
			}
			{
				std::scoped_lock l(g_walkLock);
				g_visits[id] = v;
			}
			logger::info("visit: {} goes with the player to {} ({} follow)", Game::NameOf(npc), plan.name, v.ours ? "our" : "their own");
			Game::Notify(std::format("{} comes with you to {}.", Game::NameOf(npc), plan.name));
			return true;
		}

		// The NPC's home's door within a short walk of the player ("let's go inside" outside it).
		bool NearHomeDoor(RE::Actor* npc)
		{
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			auto* tes = RE::TES::GetSingleton();
			if (!cell || cell->IsInteriorCell() || !tes) {
				return false;
			}
			bool found = false;
			tes->ForEachReferenceInRange(player, 1500.0f, [&](RE::TESObjectREFR* ref) {
				auto* base = ref ? ref->GetBaseObject() : nullptr;
				if (base && base->Is(RE::FormType::Door)) {
					auto* x = ref->extraList.GetByType<RE::ExtraTeleport>();
					auto linked = x && x->teleportData ? x->teleportData->linkedDoor.get() : nullptr;
					if (linked && Promises::IsHome(linked->GetParentCell(), npc)) {
						found = true;
						return RE::BSContainer::ForEachResult::kStop;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return found;
		}

		bool InsideWords(const std::string& line)
		{
			return HasAny(Padded(line), { "go inside", "go in", "head inside", "get inside", "come inside", "step inside", "inside and" });
		}

		std::mutex g_visitTalkLock;
		std::unordered_map<RE::FormID, std::pair<std::string, std::chrono::steady_clock::time_point>> g_visitTalk;
	}

	bool VisitWords(const std::string& line)
	{
		return HasAny(Padded(line), { "your place", "your home", "your house", "your hut", "your farm", "your shack", "your cabin", "back to yours",
			"to yours", "my place", "my home", "my house", "back to mine", "to mine", "back to my", "our place", "our home", "our house", "go home",
			"head home", "home together" });
	}

	bool MightVisit(RE::Actor* npc, const std::string& playerLine, const std::string& npcLine)
	{
		return VisitWords(playerLine) || VisitWords(npcLine) || (npc && InsideWords(playerLine) && NearHomeDoor(npc));
	}

	void NoteVisitTalk(RE::Actor* npc)
	{
		if (!npc) {
			return;
		}
		const std::string player = Game::Player()->GetName();
		auto fact = [&](Whose whose) {
			const auto p = PlanVisit(npc, whose);
			const bool theirs = whose == Whose::kTheirs;
			if (p.ok) {
				return theirs ? std::format("your home, {}, is close by", p.name) : std::format("{}'s home, {}, is close by", player, p.name);
			}
			if (std::string_view(p.why) == "no home") {
				return theirs ? std::string("you have no home of your own to take them to (say so)") :
				                std::format("{} owns no home (point that out)", player);
			}
			return theirs ? std::format("your home, {}, is in {}, too far to go now (say so)", p.name, p.farIn) :
			                std::format("{}'s home, {}, is in {}, too far to go now (say so)", player, p.name, p.farIn);
		};
		std::string line;
		if (MoveIn::SharedHouse(npc->GetFormID()).first) {
			line = std::format("If {} is asking to go home to relax together: you live with them, so \"my\", \"your\" or \"our\" home all mean the one you share; {}.",
				player, fact(Whose::kPlayers));
		} else {
			line = std::format("If {} is asking to go to your home or theirs to relax together: {}; {}.", player, fact(Whose::kTheirs), fact(Whose::kPlayers));
		}
		if (!Promises::IsClose(npc) && !MoveIn::Lives(npc->GetFormID())) {
			line += std::format(" You don't know {} well enough to go home together yet: you would most likely turn it down, in your own way.", player);
		} else {
			line += std::format(" If you agree, you walk there together with {} going first (\"lead the way\", \"I'll follow you\").", player);
		}
		std::scoped_lock l(g_visitTalkLock);
		g_visitTalk[npc->GetFormID()] = { std::move(line), std::chrono::steady_clock::now() };
	}

	std::string VisitLine(RE::FormID npc)
	{
		std::scoped_lock l(g_visitTalkLock);
		auto it = g_visitTalk.find(npc);
		if (it == g_visitTalk.end() || std::chrono::steady_clock::now() - it->second.second > std::chrono::minutes(3)) {
			return "";
		}
		return it->second.first;
	}

	bool IsFollowing(RE::FormID npc)
	{
		std::scoped_lock l(g_walkLock);
		auto it = g_walking.find(npc);
		return it != g_walking.end() && it->second.follow && !it->second.company;
	}

	bool InCompany(RE::FormID npc)
	{
		std::scoped_lock l(g_walkLock);
		auto it = g_walking.find(npc);
		return it != g_walking.end() && it->second.company;
	}

	bool Leaving(const std::string& line)
	{
		// Questions don't count ("do you have to go?"); the rest of the line still does ("I must go. Will you
		// be all right?").
		const auto p = Padded(Statements(line));
		// Saying goodbye.
		if (HasAny(p, { "goodbye", "good bye", "farewell", "see you later", "see you around", "see you soon", "until next time", "good night", "goodnight",
				"safe travels" })) {
			return true;
		}
		if (HasAny(p, { "don t have to go", "dont have to go", "do not have to go", "not leaving", "won t leave", "wont leave", "no need to go",
				"never leave", "not going anywhere", "go with you", "come with you", "off duty", "off work", "later", "tomorrow", "soon", "in a bit",
				"after", "tonight" })) {
			return false;  // staying, coming along, or a later plan
		}
		return HasAny(p, { "i have to go", "i ve got to go", "i got to go", "i gotta go", "gotta go", "i need to go", "i must go", "i should go",
			"i should get going", "i d better go", "i better go", "i d best be going", "i d best be off", "i should be going", "i must be going",
			"i need to be going", "i have to be going", "i ll be going", "i ll be off", "i must be off", "i should be off", "i need to get back",
			"i have to get back", "i should get back", "i must get back", "i ll leave you", "i ll take my leave", "i must take my leave",
			"time for me to go", "time i was going", "duty calls", "you d better go", "you should leave", "you need to leave",
			"you should go now", "you should be going", "you should be on your way", "leave me be", "leave me alone", "get out of my" }) ||
		       // "Get out!" / "get out of here" sends the player off; "let's get out of here" goes together.
		       ((p.ends_with(" get out ") || HasAny(p, { "get out of here", "now get out", "get out now" })) &&
			       !HasAny(p, { "let s", "lets", "let us", "we", "us", "we ll", "we should" }));
	}

	bool PlayerAsksCompany(const std::string& line)
	{
		return HasAny(Padded(line), { "keep me company", "keep me some company", "keep each other company", "stay with me", "stay here with me",
			"spend time", "spend some time", "spend the evening", "spend the day", "spend a while", "hang out", "stay a while", "stay awhile",
			"stay for a while", "stay a bit", "stay for a bit", "stick around", "be with me", "don t leave", "dont leave", "don t go", "dont go" });
	}

	bool NpcOffersCompany(const std::string& line)
	{
		return HasAny(Padded(line), { "i ll keep you company", "i will keep you company", "i can keep you company", "i ll stay with you",
			"i will stay with you", "i can stay with you", "i ll stay a while", "i can stay a while", "i ll stay for a while", "i ll stick around",
			"i can stick around", "i ll spend some time", "i d love to stay", "i ll stay" });
	}

	void EndCompany(RE::FormID npc, const char* why, const std::string& narration)
	{
		if (!InCompany(npc)) {
			return;
		}
		StopWalking(npc, why);
		if (auto* a = RE::TESForm::LookupByID<RE::Actor>(npc)) {
			logger::info("company: {}'s time with the player ended ({})", a->GetName(), why);
			Game::Notify(std::format("{} goes back to their day.", a->GetName()));
			if (!narration.empty()) {
				Game::Mention(narration, a);
			}
		}
	}

	void WakeIfAsleep(RE::FormID npc, const std::string& playerLine)
	{
		auto* a = RE::TESForm::LookupByID<RE::Actor>(npc);
		auto* state = a ? a->AsActorState() : nullptr;
		const auto now = std::chrono::steady_clock::now();
		{
			std::scoped_lock l(g_wokenLock);
			if (auto it = g_woken.find(npc); it != g_woken.end()) {
				it->second.lastTalk = now;  // still talking: stays up
				return;
			}
		}
		if (!state || state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kIsSleeping || a->IsDead() || Game::DistanceToPlayer(a) > 500.0f ||
			a->IsHostileToActor(Game::Player())) {
			return;
		}
		{
			std::scoped_lock l(g_wokenLock);
			g_woken[npc] = { now, now, playerLine, false, false };
		}
		// A package that isn't sleep: they get up (the game's own getting-up) and stand by the player.
		Papyrus::CallStatic("SkyrimNetApi", "RegisterPackage", nullptr, a, RE::BSFixedString("FollowPlayer"), kFollowPriority, static_cast<std::int32_t>(0), false);
		MarkFollowing(a, true);  // or SeverActions' schedule puts them straight back to bed
		Papyrus::CallMethod(a, "Actor", "EvaluatePackage", nullptr);
		logger::info("wake: {} was asleep; the player's voice woke them", a->GetName());
	}

	void NoteNpcSpoke(RE::FormID npc)
	{
		std::scoped_lock l(g_wokenLock);
		if (auto it = g_woken.find(npc); it != g_woken.end()) {
			it->second.answered = true;
			it->second.lastTalk = std::chrono::steady_clock::now();
		}
	}

	bool WalkingWithPlayer(RE::FormID npc)
	{
		std::scoped_lock l(g_walkLock);
		return g_walking.contains(npc);
	}

	bool IsCompanion(RE::Actor* npc)
	{
		if (!npc || !npc->IsPlayerTeammate()) {
			return false;
		}
		std::scoped_lock l(g_walkLock);
		auto it = g_walking.find(npc->GetFormID());
		return it == g_walking.end() || !it->second.teammate;
	}

	namespace
	{
		std::mutex g_meetLock;
		std::unordered_map<RE::FormID, std::pair<std::string, std::chrono::steady_clock::time_point>> g_meetPlaces;  // npc -> bio line
		std::unordered_map<RE::FormID, std::pair<std::string, float>> g_booked;  // npc -> place|time booked, game hours
		std::unordered_map<RE::FormID, Meeting> g_meetings;                     // npc -> the meeting as shown in the book
	}

	bool MeetWords(const std::string& line)
	{
		std::string p = " ";
		for (auto& w : JsonUtil::Words(JsonUtil::Lower(line))) {
			p += w + " ";
		}
		// Talk of meeting up, not every "tomorrow" (that would put meeting places into everyday chat).
		for (auto* k : { " meet ", " meeting ", " meet me ", " meet you ", " be there ", " see you at ", " see you there ", " where do we ",
				 " where should we ", " where shall we ", " hunt together ", " go together " }) {
			if (p.find(k) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	void NoteMeetTalk(RE::Actor* npc)
	{
		if (!npc) {
			return;
		}
		std::string names;
		int n = 0;
		for (auto& entry : PlacesNear(npc, 8)) {
			names += (names.empty() ? "" : ", ") + entry;
			if (++n >= 6) {
				break;
			}
		}
		if (names.empty()) {
			return;
		}
		std::scoped_lock l(g_meetLock);
		g_meetPlaces[npc->GetFormID()] = { std::format("If arranging to meet {} somewhere later, name one of these real places nearby (not an invented spot): {}.",
											   Game::Player()->GetName(), names),
			std::chrono::steady_clock::now() };
	}

	std::vector<Meeting> Meetings()
	{
		std::vector<Meeting> out;
		const float now = Game::Hours();
		std::scoped_lock l(g_meetLock);
		std::erase_if(g_meetings, [&](const auto& kv) { return now - kv.second.bookedAt > 30.0f; });  // a day and a bit: met or missed by then
		for (auto& [id, m] : g_meetings) {
			out.push_back(m);
		}
		return out;
	}

	std::string MeetPlacesLine(RE::FormID npc)
	{
		std::scoped_lock l(g_meetLock);
		auto it = g_meetPlaces.find(npc);
		if (it == g_meetPlaces.end() || std::chrono::steady_clock::now() - it->second.second > std::chrono::minutes(3)) {
			return "";
		}
		return it->second.first;
	}

	bool ScheduleMeeting(RE::Actor* npc, const std::string& place, const std::string& when, const std::string& saidPlace)
	{
		auto* quest = npc ? RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESQuest>(0xD61, "IntelEngine.esp") : nullptr;
		if (!quest) {
			logger::info("meet: {} - IntelEngine isn't installed; the meeting is only words", npc ? Game::NameOf(npc) : "?");
			return false;
		}
		const auto id = npc->GetFormID();
		const std::string time = when.empty() ? "tomorrow" : when;
		// Said again ("Dawn, east ridge" five times over): booked once.
		{
			const auto key = JsonUtil::Lower(place) + "|" + JsonUtil::Lower(time);
			std::scoped_lock l(g_meetLock);
			if (auto it = g_booked.find(id); it != g_booked.end() && it->second.first == key && Game::Hours() - it->second.second < 24.0f) {
				logger::info("meet: {} - already booked ({} {})", Game::NameOf(npc), place, time);
				return true;
			}
			g_booked[id] = { key, Game::Hours() };
		}
		// Where they are now: the fallback for a place IntelEngine can't find (one they made up, "east ridge").
		std::string here;
		if (auto* loc = npc->GetCurrentLocation(); loc && loc->GetName() && *loc->GetName()) {
			here = loc->GetName();
		}
		auto call = [quest, id, time](const std::string& dest, std::function<void(bool)> then) {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			return a && Papyrus::CallMethod(quest, "IntelEngine_Schedule", "ScheduleMeeting",
							[then](const RE::BSScript::Variable& r) {
								const bool ok = !r.IsBool() || r.GetBool();
								SKSE::GetTaskInterface()->AddTask([then, ok] { then(ok); });
							},
							static_cast<RE::Actor*>(a), RE::BSFixedString(dest), RE::BSFixedString(time));
		};
		auto done = [id, time, saidPlace](const std::string& dest, bool ok) {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!a) {
				return;
			}
			logger::info("meet: {} at '{}' {} -> {}", a->GetName(), dest, time, ok ? "scheduled (IntelEngine)" : "IntelEngine couldn't place it");
			if (ok) {
				{
					std::scoped_lock l(g_meetLock);
					g_meetings[id] = { a->GetName(), std::format("meet at {}, {}", dest, time), Game::Hours() };
				}
				const bool renamed = !saidPlace.empty() && JsonUtil::Lower(saidPlace) != JsonUtil::Lower(dest);
				Game::Notify(renamed ? std::format("{} will meet you at {} (\"{}\", as they put it), {}", a->GetName(), dest, saidPlace, time)
				                     : std::format("{} will meet you: {}, {}", a->GetName(), dest, time));
			} else {
				std::scoped_lock l(g_meetLock);
				g_booked.erase(id);  // not booked after all: a later "yes" may try again
			}
		};
		// "Honningbrew Meadery (brewery)" as the places list writes it: the name alone.
		std::string named = place;
		if (const auto paren = named.find(" ("); paren != std::string::npos) {
			named.erase(paren);
		}
		const std::string dest = named.empty() ? (here.empty() ? std::string("here") : here) : named;
		const bool sent = call(dest, [call, done, dest, here](bool ok) {
			if (ok || here.empty() || dest == here) {
				done(dest, ok);
				return;
			}
			// Not a place IntelEngine knows: where they are now instead.
			call(here, [done, here](bool ok2) { done(here, ok2); });
		});
		if (!sent) {
			logger::warn("meet: IntelEngine_Schedule.ScheduleMeeting could not be called for {}", Game::NameOf(npc));
		}
		return sent;
	}

	bool FollowsPlayer(RE::Actor* npc)
	{
		return npc && (FollowState(npc) || WalkingWithPlayer(npc->GetFormID()));
	}

	bool IsStopWords(const std::string& line)
	{
		// Questions aren't orders ("do you want to go home?"), unless asked politely ("could you wait here?").
		std::string said;
		{
			std::string cur;
			for (char c : line + ".") {
				cur += c;
				if (c == '.' || c == '!' || c == '?' || c == '\n') {
					const auto q = Padded(cur);
					if (c != '?' || q.starts_with(" can you ") || q.starts_with(" could you ") || q.starts_with(" would you ") ||
						q.starts_with(" will you ") || q.starts_with(" please ")) {
						said += cur + " ";
					}
					cur.clear();
				}
			}
		}
		const auto p = Padded(said);
		// "Don't leave me", "stay here with me": the very opposite of letting them go.
		if (HasAny(p, { "don t leave", "dont leave", "do not leave", "don t go", "dont go", "never leave", "won t leave", "wont leave", "with me", "with us" })) {
			return false;
		}
		// Plainly parting, whatever else is said.
		if (HasAny(p, { "stop following", "don t follow", "dont follow", "no need to follow", "you re dismissed", "youre dismissed", "we part here",
				"part ways", "that s far enough", "thats far enough", "go on without me" })) {
			return true;
		}
		// Going on together, or being taken somewhere: "let's go home", "take me back to your place", "you can go first".
		if (HasAny(p, { "let s", "lets", "let us", "we", "we ll", "together", "take me", "show me", "bring me", "lead me", "walk me", "you can go first",
				"you can go ahead", "you go first", "after you" })) {
			return false;
		}
		if (HasAny(p, { "wait here", "stay here", "wait for me here", "wait right here", "stay put", "you can go", "you may go", "go home", "head home",
				"go back home", "you can leave", "leave me", "i ll go alone", "ill go alone", "i ll go on alone", "go about your", "back to your work",
				"back to your business", "back to your post", "back to your day", "back to your life" })) {
			return true;
		}
		// "You can do whatever you like": free to go about their own day. A "do" aimed at them, not "I'll do
		// whatever you want", "take whatever you want", "do what you want with it" (an item), "I'm free to go".
		if (!HasAny(p, { "i ll do", "ill do", "i will do", "i can do", "i d do", "i would do", "i do", "with it", "with them", "with this", "with that",
				"with those" }) &&
			HasAny(p, { "you can do whatever", "you can do what ever", "you can do what you", "go do whatever", "go do what ever", "go do what you",
				"do whatever you want", "do whatever you like", "do what ever you want", "do what ever you like", "do what you want",
				"do what you like", "do as you please", "go do your", "do your thing", "do your own thing", "you re free to go", "youre free to go",
				"you are free to go", "you re free to do", "youre free to do", "you are free to do", "important to do you can do" })) {
			return true;
		}
		return false;
	}

	void StopWalking(RE::FormID id, const char* why)
	{
		Walk w;
		{
			std::scoped_lock l(g_walkLock);
			auto it = g_walking.find(id);
			if (it == g_walking.end()) {
				return;
			}
			w = it->second;
			g_walking.erase(it);
		}
		auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
		if (!a) {
			return;
		}
		if (!w.follow) {
			Papyrus::CallMethod(a, "Actor", "SetDontMove", nullptr, false);
		}
		// Every follow of theirs, ours and any SeverActions one, ends.
		Papyrus::CallStatic("SkyrimNetApi", "UnregisterPackage", nullptr, a, RE::BSFixedString("FollowPlayer"));
		MarkFollowing(a, false);
		if (w.teammate) {
			SetTeammate(a, false);
		}
		ReleaseFromSeverActions(a, [id] {
			if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) {
				Papyrus::CallMethod(a, "Actor", "EvaluatePackage", nullptr);
			}
		});
		logger::info("{}: {} stops ({})", w.follow ? "follow" : "escort", a->GetName(), why);
	}

	// Every couple of seconds: keeps every follow and escort of ours going.
	void TickWalking()
	{
		TickWoken();
		if (auto* pl = Game::Player(); pl && pl->Is3DLoaded()) {
			TickVisits();
		}
		const auto now = std::chrono::steady_clock::now();
		auto* p = Game::Player();
		if (!p || !p->Is3DLoaded()) {
			return;
		}
		std::vector<std::pair<RE::FormID, Walk>> list;
		{
			std::scoped_lock l(g_walkLock);
			list.assign(g_walking.begin(), g_walking.end());
		}
		for (auto& [id, w] : list) {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!a || a->IsDead()) {
				std::scoped_lock l(g_walkLock);
				g_walking.erase(id);
				continue;
			}
			if (a->IsInCombat() || p->IsInCombat()) {
				continue;  // fights are their own business
			}
			// A scripted scene trying to take them: stopped (played another time), so they stay with the
			// player. Only the main story's and the guilds' / factions' quest scenes play out (stopping
			// those can break a quest); they're back with the player when it's over.
			if (auto* scene = a->GetCurrentScene()) {
				using T = RE::QUEST_DATA::Type;
				auto* q = scene->parentQuest;
				const auto type = q ? q->GetType() : T::kNone;
				const bool story = q && !q->objectives.empty() && type != T::kNone && type != T::kMiscellaneous && type != T::kSideQuest;
				if (!story) {
					bool stop = false;
					{
						std::scoped_lock l(g_walkLock);
						if (auto it = g_walking.find(id); it != g_walking.end() && now - it->second.sceneStopped > std::chrono::seconds(10)) {
							it->second.sceneStopped = now;
							stop = true;
						}
					}
					if (stop) {
						Papyrus::CallMethod(scene, "Scene", "Stop", nullptr);
						logger::info("{}: {} stays with the player; scene ({}) stopped", w.follow ? "follow" : "escort", a->GetName(),
							q && q->GetName() ? q->GetName() : "?");
					}
				}
				continue;
			}
			// Together: the same cell, or out in the same world; then near enough.
			auto* ws = a->GetWorldspace();
			const bool sameWorld = a->GetParentCell() == p->GetParentCell() || (ws && ws == p->GetWorldspace());
			const float d = sameWorld ? Game::DistanceToPlayer(a) : 1.0e9f;
			bool placed = false;
			{
				std::scoped_lock l(g_walkLock);
				auto it = g_walking.find(id);
				if (it == g_walking.end()) {
					continue;
				}
				auto& cur = it->second;
				cur.apart = (cur.follow || cur.dest) && (!sameWorld || d > 4000.0f) ? cur.apart + 1 : 0;
				// Spending time: the player walking off ends it (they stay where they are).
				if (cur.company && cur.apart >= 3) {
					placed = false;
					cur.apart = -1000;  // marks "left behind" for below
				} else if (cur.apart >= 3 && now - cur.lastMove > std::chrono::seconds(8)) {
					cur.apart = 0;
					cur.lastMove = now;
					cur.lastPath = {};
					placed = true;
				}
			}
			if (placed) {
				PlaceBehindPlayer(a);
				logger::info("{}: {} was left behind; now beside the player", w.follow ? "follow" : "escort", a->GetName());
				continue;
			}
			if (w.company) {
				int apartNow = 0;
				{
					std::scoped_lock l(g_walkLock);
					if (auto it = g_walking.find(id); it != g_walking.end()) {
						apartNow = it->second.apart;
					}
				}
				if (apartNow <= -1000) {
					EndCompany(id, "the player left", std::format("{} went off, and {} stayed behind.", Game::Player()->GetName(), a->GetName()));
					continue;
				}
				if (TickCompany(id, a)) {
					continue;  // ended
				}
			}
			if (w.follow) {
				// The package back if it went missing (a load, another mod clearing it).
				const bool asked = Papyrus::CallStatic("SkyrimNetApi", "HasPackage",
					[id](const RE::BSScript::Variable& has) {
						if (has.IsInt() && has.GetSInt() > 0) {
							return;
						}
						SKSE::GetTaskInterface()->AddTask([id] {
							auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
							bool apply = false;
							{
								std::scoped_lock l(g_walkLock);
								auto it = g_walking.find(id);
								if (a && it != g_walking.end() && it->second.follow &&
									std::chrono::steady_clock::now() - it->second.lastFollow > std::chrono::seconds(5)) {
									it->second.lastFollow = std::chrono::steady_clock::now();
									apply = true;
								}
							}
							// Just ours again - never SeverActions' "let go" here: that also removes this very
							// package a moment later, which would loop (on, off, on...).
							if (apply) {
								ApplyFollow(a);
								logger::info("follow: {}'s follow was gone; put back", a->GetName());
							}
						});
					},
					static_cast<RE::Actor*>(a), RE::BSFixedString("FollowPlayer"));
				(void)asked;
				continue;
			}
			// A SeverActions trip (theirs to walk): guarded against scenes (above); over when it is.
			if (!w.dest) {
				if (now - w.since > std::chrono::seconds(20)) {
					Papyrus::CallStatic("SeverActionsNativeExt2", "Travel_IsTravelingByActor",
						[id](const RE::BSScript::Variable& traveling) {
							if (traveling.IsBool() && !traveling.GetBool()) {
								SKSE::GetTaskInterface()->AddTask([id] {
									bool followAfter = false;
									{
										std::scoped_lock l(g_walkLock);
										auto it = g_walking.find(id);
										if (it == g_walking.end() || it->second.follow || it->second.dest) {
											return;
										}
										followAfter = it->second.followAfter;
										g_walking.erase(it);
									}
									// Home was the way to a spot in it ("show me your room"): on to it.
									if (const auto spot = TakeThenSpot(id); !spot.empty()) {
										if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) {
											logger::info("escort: home reached; on to {}", SpotLabel(spot));
											ShowSpot(a, spot, false, followAfter);
											return;
										}
									}
									logger::info("escort: trip over{}", followAfter ? "; walking with the player again" : "");
									if (followAfter) {
										StartFollow(id);
									}
								});
							}
						},
						static_cast<RE::Actor*>(a));
				}
				continue;
			}
			// Escort.
			auto* dest = RE::TESForm::LookupByID<RE::TESObjectREFR>(w.dest);
			if (!dest) {
				StopWalking(id, "the place is gone");
				continue;
			}
			if (a->GetPosition().GetDistance(dest->GetPosition()) < 350.0f && a->GetParentCell() == dest->GetParentCell()) {
				Arrived(id, w.place);
				continue;
			}
			// Hold on while the player is behind; walk on once they've caught up.
			if (sameWorld && d > 700.0f && !w.waiting) {
				Papyrus::CallMethod(a, "Actor", "SetDontMove", nullptr, true);
				std::scoped_lock l(g_walkLock);
				if (auto it = g_walking.find(id); it != g_walking.end()) {
					it->second.waiting = true;
				}
				continue;
			}
			if (w.waiting && sameWorld && d < 400.0f) {
				{
					std::scoped_lock l(g_walkLock);
					if (auto it = g_walking.find(id); it != g_walking.end()) {
						it->second.waiting = false;
						it->second.lastPath = now;
					}
				}
				WalkOn(id, w.dest);
				continue;
			}
			// Standing about (a path that ended short, a load): on again.
			if (!w.waiting && !a->IsMoving() && now - w.lastPath > std::chrono::seconds(6)) {
				{
					std::scoped_lock l(g_walkLock);
					if (auto it = g_walking.find(id); it != g_walking.end()) {
						it->second.lastPath = now;
					}
				}
				WalkOn(id, w.dest);
			}
		}
	}

	void OnGameLoaded()
	{
		std::scoped_lock l(g_walkLock);
		g_loadedAt = std::chrono::steady_clock::now();
		for (auto& [id, w] : g_walking) {
			w.lastPath = {};
			w.lastFollow = {};
			w.waiting = false;
			w.apart = 0;
		}
	}

	json SaveWalking()
	{
		std::scoped_lock l(g_walkLock);
		json out = json::array();
		for (auto& [id, w] : g_walking) {
			out.push_back({ { "npc", id }, { "follow", w.follow }, { "dest", w.dest }, { "place", w.place }, { "company", w.company }, { "until", w.until },
				{ "bondAt", w.bondAt }, { "bonds", w.bonds }, { "teammate", w.teammate } });
		}
		for (auto& [id, v] : g_visits) {
			out.push_back({ { "npc", id }, { "visit", static_cast<int>(v.whose) }, { "until", v.until }, { "wrongCell", v.wrongCell }, { "home", v.home },
				{ "ours", v.ours }, { "place", v.name } });
		}
		return out;
	}

	void LoadWalking(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		std::scoped_lock l(g_walkLock);
		g_walking.clear();
		g_visits.clear();
		if (!j.is_array()) {
			return;
		}
		for (auto& o : j) {
			const auto raw = static_cast<RE::FormID>(JsonUtil::Num(o, "npc").value_or(0));
			const auto id = raw ? resolve(raw) : 0;
			if (!id) {
				continue;
			}
			if (o.contains("visit")) {
				Visit v;
				v.whose = static_cast<int>(JsonUtil::Num(o, "visit").value_or(1)) == 2 ? Whose::kPlayers : Whose::kTheirs;
				v.until = static_cast<float>(JsonUtil::Num(o, "until").value_or(0));
				const auto rawCell = static_cast<RE::FormID>(JsonUtil::Num(o, "wrongCell").value_or(0));
				v.wrongCell = rawCell ? resolve(rawCell) : 0;
				const auto rawHome = static_cast<RE::FormID>(JsonUtil::Num(o, "home").value_or(0));
				v.home = rawHome ? resolve(rawHome) : 0;
				v.ours = o.contains("ours") && o["ours"].is_boolean() && o["ours"].get<bool>();
				v.name = JsonUtil::Str(o, "place");
				g_visits[id] = v;
				continue;
			}
			Walk w;
			w.follow = !o.contains("follow") || JsonUtil::Str(o, "follow", "true") == "true";
			const auto rawDest = static_cast<RE::FormID>(JsonUtil::Num(o, "dest").value_or(0));
			w.dest = rawDest ? resolve(rawDest) : 0;
			w.place = JsonUtil::Str(o, "place");
			w.since = std::chrono::steady_clock::now();
			w.company = o.contains("company") && o["company"].is_boolean() && o["company"].get<bool>();
			w.until = static_cast<float>(JsonUtil::Num(o, "until").value_or(0));
			w.bondAt = static_cast<float>(JsonUtil::Num(o, "bondAt").value_or(0));
			w.bonds = static_cast<int>(JsonUtil::Num(o, "bonds").value_or(0));
			w.teammate = o.contains("teammate") && o["teammate"].is_boolean() && o["teammate"].get<bool>();
			w.lastHours = 0.0f;  // a load is no skip
			if (w.follow || w.dest) {
				g_walking[id] = w;
			}
		}
	}

	void RevertWalking()
	{
		std::scoped_lock l(g_walkLock);
		g_walking.clear();
		g_visits.clear();
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
		// Favours wait until they've forgiven the player; business (trade, rooms, training) and
		// getting the player's own things back don't.
		if ((IsActivity(id) || IsMovement(id)) && id != "retrieve" &&
			Trust::Refuse(npc, id == "share_meal" ? "to share a meal" : id == "drink" ? "to have a drink together" : id == "sit" ? "to sit with them a while" : id == "visit" ? "to go home with them" : id == "company" ? "to spend time with them" : id == "move_in" ? "to move in with them" : id == "perform" ? "to play or sing for them" : id == "rest" ? "to rest together" : id == "hold" ? "to keep their things" :
			                   id == "heal" ? "to patch them up" : id == "cook" ? "to cook for them" : id == "mend" ? "to work on their gear" :
			                   id == "cure" ? "to cure them" : "to go with them")) {
			return true;
		}
		if (id == "trade") {
			// SkyrimNet's own merchant line may have opened it already: once is enough.
			if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::BarterMenu::MENU_NAME)) {
				logger::info("trade: the barter window is already open (SkyrimNet); not again");
				return true;
			}
			// Same call the vanilla "What have you got for sale?" fragment makes.
			return Papyrus::CallMethod(npc, "Actor", "ShowBarterMenu", nullptr);
		}
		if (id == "pay_fine" || id == "bribe" || id == "persuade" || id == "intimidate") {
			return Crime::Execute(id, npc, args.price);
		}
		if (id == "buy") {
			return Trade::Buy(npc, args.item, args.qty, args.price, args.each);
		}
		if (id == "sell") {
			return Trade::Sell(npc, args.item, args.qty, args.price, args.each);
		}
		if (id == "train") {
			return Papyrus::CallStatic("Game", "ShowTrainingMenu", nullptr, static_cast<RE::Actor*>(npc));
		}
		if (id == "rent_room") {
			return RentRoom(npc, args.price, args.free);
		}
		// Walking off, or a meal: whatever seat a drink or a sit held them in is no longer theirs to keep.
		if (id == "follow" || id == "escort" || id == "company" || id == "share_meal" || id == "rest" || id == "cook") {
			if (id != "share_meal" || !Moments::IsSitting(npc->GetFormID())) {
				Moments::Release(npc->GetFormID());
			}
		}
		if (id == "follow") {
			return Follow(npc);
		}
		if (id == "company") {
			return Company(npc);
		}
		if (id == "move_in") {
			return MoveIn::Invite(npc);
		}
		if (id == "escort") {
			return Escort(npc, args.place, args.ownTrip);
		}
		if (id == "share_meal") {
			return ShareMeal(npc);
		}
		if (id == "rest") {
			return Rest::Start(npc);
		}
		if (id == "hold") {
			return Safekeeping::Hold(npc, args.item);
		}
		if (id == "retrieve") {
			return Safekeeping::Retrieve(npc, args.item);
		}
		if (id == "drink") {
			return Moments::Drink(npc, args.item);
		}
		if (id == "sit") {
			return Moments::Sit(npc, args.item, args.hours);
		}
		if (id == "visit") {
			return StartVisit(npc, args.item.empty() ? args.place : args.item);
		}
		if (id == "perform") {
			return Moments::Perform(npc, args.item, args.how);
		}
		if (id == "heal") {
			return Care::Heal(npc);
		}
		if (id == "cook" || id == "mend") {
			// SeverActions' CookMeal / CraftItem went off on this line and is already at the pot or forge:
			// one dish, not two.
			const auto npcId = npc->GetFormID();
			auto go = [npcId, id, item = args.item](bool crafting) {
				SKSE::GetTaskInterface()->AddTask([npcId, id, item, crafting] {
					auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId);
					if (!a) {
						return;
					}
					if (crafting) {
						logger::info("{}: SeverActions is already crafting for {}; not a second time", id, a->GetName());
						return;
					}
					id == "cook" ? Care::Cook(a, item) : Care::Mend(a, item);
				});
			};
			const bool asked = Papyrus::CallStatic("SeverActionsNativeExt", "Craft_GetActiveCount",
				[go](const RE::BSScript::Variable& n) { go(n.IsInt() && n.GetSInt() > 0); });
			if (!asked) {
				go(false);
			}
			return true;
		}
		if (id == "cure") {
			return Care::Cure(npc);
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
		if (id == "hold" || id == "retrieve") {
			return RE::ContainerMenu::MENU_NAME.data();  // the follower window, if another mod beat us to it
		}
		return "";
	}
}
