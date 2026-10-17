#include "MoveIn.h"

#include "Bonds.h"
#include "Confirm.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Papyrus.h"
#include "Promises.h"
#include "Rest.h"
#include "Services.h"
#include "SkyrimNetBridge.h"
#include "Trust.h"

namespace MundaneMatters::MoveIn
{
	bool LivesIn(RE::FormID npc, RE::TESObjectCELL* cell);  // below

	namespace
	{
		enum class Stage
		{
			kTravel,      // on the way there
			kSettle,      // there; the home is set once the player is outdoors or with them
			kFollowerWait,  // a companion: the home is set when the player brings them there
			kHome         // lives there
		};

		struct Mover
		{
			RE::FormID house = 0;  // the BGSLocation
			std::string name;      // its name, as SeverActions resolves places
			Stage stage = Stage::kTravel;
			float since = 0.0f;    // game hours the move started
			bool placeNow = false;  // the walk couldn't start: placed there once out of sight
			bool bedPending = false;  // the bed there is claimed the first time the player is there with them
			bool busy = false;     // a Papyrus step is under way (not saved)
		};

		std::mutex g_lock;
		std::unordered_map<RE::FormID, Mover> g_movers;
		std::chrono::steady_clock::time_point g_lastTick{};
		std::chrono::steady_clock::time_point g_lastHomeCheck{};

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

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

		// "Breezehome Key" -> "breezehome"; "Key to Honeyside" -> "honeyside"
		std::string KeyStem(std::string name)
		{
			name = JsonUtil::Lower(std::move(name));
			for (const char* pre : { "key to the ", "key to ", "the " }) {
				if (name.starts_with(pre)) {
					name = name.substr(std::strlen(pre));
				}
			}
			if (name.ends_with(" key")) {
				name.resize(name.size() - 4);
			}
			return name;
		}

		bool KeyFits(const std::string& stem, std::string house)
		{
			house = JsonUtil::Lower(std::move(house));
			if (house.starts_with("the ")) {
				house = house.substr(4);
			}
			return stem.size() >= 4 && (stem == house || house.find(stem) != std::string::npos || stem.find(house) != std::string::npos);
		}

		bool InHouse(RE::Actor* a, RE::BGSLocation* house)
		{
			auto* loc = a ? a->GetCurrentLocation() : nullptr;
			return house && loc && (loc == house || loc->parentLoc == house);
		}

		struct House
		{
			RE::BGSLocation* loc = nullptr;
			std::string name;
			const char* why = "";
		};

		// The player's houses: player-house locations whose key they carry (the game's own houses and
		// most mod houses), whose rooms are owned by them or their faction, or the one they stand in.
		std::vector<House> Houses()
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			auto* player = Game::Player();
			auto* playerBase = player->GetActorBase();
			auto* playerFaction = dh->LookupForm<RE::TESFaction>(0xDB1, "Skyrim.esm");
			std::vector<std::string> keys;
			for (auto& [obj, data] : player->GetInventory([](RE::TESBoundObject& o) { return o.Is(RE::FormType::KeyMaster); })) {
				if (data.first > 0 && obj->GetName()) {
					keys.push_back(KeyStem(obj->GetName()));
				}
			}
			std::unordered_set<RE::BGSLocation*> owned;
			for (auto* cell : dh->interiorCells) {
				auto* owner = cell ? cell->GetOwner() : nullptr;
				if (owner && (owner == playerBase || owner == playerFaction)) {
					if (auto* loc = cell->GetLocation()) {
						owned.insert(loc);
					}
				}
			}
			auto* here = player->GetCurrentLocation();
			static bool logged = false;
			std::vector<House> out;
			std::unordered_set<std::string> names;
			for (auto* loc : dh->GetFormArray<RE::BGSLocation>()) {
				if (!loc || !loc->HasKeywordString("LocTypePlayerHouse") || !loc->GetName() || !*loc->GetName()) {
					continue;
				}
				const std::string name = loc->GetName();
				const char* why = std::ranges::any_of(keys, [&](const std::string& k) { return KeyFits(k, name); }) ? "key" :
				                  owned.contains(loc)                                                             ? "owned" :
				                  (here == loc || (here && here->parentLoc == loc))                                ? "here" :
				                                                                                                     "";
				if (!logged) {
					logger::info("move in: house '{}' ({:08X}): {}", name, loc->GetFormID(), *why ? why : "not the player's");
				}
				if (*why && names.insert(JsonUtil::Lower(name)).second) {
					out.push_back({ loc, name, why });
				}
			}
			logged = true;
			std::ranges::sort(out, {}, &House::name);
			return out;
		}

		// A shop, an inn, or a quest they're part of may stop while they live elsewhere.
		bool HasDuties(RE::Actor* npc)
		{
			if (npc->GetVendorFaction() || Services::RoomOf(npc)) {
				return true;
			}
			if (auto* x = npc->extraList.GetByType<RE::ExtraAliasInstanceArray>()) {
				for (auto* d : x->aliases) {
					auto* q = d ? const_cast<RE::TESQuest*>(d->quest) : nullptr;
					using T = RE::QUEST_DATA::Type;
					if (q && q->IsRunning() && !q->objectives.empty() && q->GetType() != T::kNone && q->GetType() != T::kMiscellaneous) {
						return true;
					}
				}
			}
			return false;
		}

		RE::TESQuest* Travel()
		{
			return Services::SeverActionsQuestFor("SeverActions_Travel");
		}

		RE::TESQuest* Followers()
		{
			return Services::SeverActionsQuestFor("SeverActions_FollowerManager");
		}

		void SetBusy(RE::FormID id, bool busy)
		{
			std::scoped_lock l(g_lock);
			if (auto it = g_movers.find(id); it != g_movers.end()) {
				it->second.busy = busy;
			}
		}

		void SetStage(RE::FormID id, Stage stage)
		{
			std::scoped_lock l(g_lock);
			if (auto it = g_movers.find(id); it != g_movers.end()) {
				it->second.stage = stage;
				it->second.busy = false;
			}
		}

		// ---- beds at home ----
		// The player's own bed in each of their houses (the one they last lay down in there) is owned by
		// the player: SeverActions' bed claim never takes it, and the player always sleeps in it. A Lover
		// living with them shares it, or shares her own double bed the same way (owned by the player, the
		// claim hers). Anyone else gets a bed of their own; with none free, a bedroll for the night.

		std::unordered_map<RE::FormID, RE::FormID> g_playerBeds;  // house (location) -> the player's bed there
		std::unordered_map<RE::FormID, RE::FormID> g_rolls;       // mover -> the bedroll laid for tonight
		std::vector<RE::FormID> g_staleRolls;                     // bedrolls from a loaded save, packed away
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_rollChecked;
		std::atomic_bool g_sleepOn{ true };
		std::atomic<float> g_sleepFrom{ 22.0f };
		std::atomic<float> g_sleepTo{ 6.0f };
		std::chrono::steady_clock::time_point g_sleepRead{};

		bool IsBed(RE::TESObjectREFR* r)
		{
			auto* f = r && r->GetBaseObject() ? r->GetBaseObject()->As<RE::TESFurniture>() : nullptr;
			return f && f->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) && !r->IsDisabled() && !r->IsDeleted();
		}

		bool IsDouble(RE::TESObjectREFR* r)
		{
			auto* m = r && r->GetBaseObject() ? skyrim_cast<RE::TESModel*>(r->GetBaseObject()) : nullptr;
			return m && m->GetModel() && JsonUtil::Lower(m->GetModel()).find("double") != std::string::npos;
		}

		bool IsLover(RE::Actor* a)
		{
			auto* base = a ? a->GetActorBase() : nullptr;
			auto* rel = base ? RE::BGSRelationship::GetRelationship(base, Game::Player()->GetActorBase()) : nullptr;
			return rel && rel->level.get() == RE::BGSRelationship::RELATIONSHIP_LEVEL::kLover;
		}

		// The owner set on the bed itself (not the room's), or null.
		RE::TESForm* OwnOwner(RE::TESObjectREFR* r)
		{
			return r->extraList.GetOwner();
		}

		// For good: at once in the engine, and through Papyrus so the save keeps it.
		void SetOwnerSaved(RE::TESObjectREFR* bed, RE::TESForm* owner)
		{
			bed->SetOwner(owner);
			if (auto* faction = owner ? owner->As<RE::TESFaction>() : nullptr) {
				Papyrus::CallMethod(bed, "ObjectReference", "SetFactionOwner", nullptr, faction);
			} else {
				Papyrus::CallMethod(bed, "ObjectReference", "SetActorOwner", nullptr, owner ? owner->As<RE::TESNPC>() : static_cast<RE::TESNPC*>(nullptr));
			}
		}

		RE::FormID HouseOf(RE::FormID npc)
		{
			std::scoped_lock l(g_lock);
			auto it = g_movers.find(npc);
			return it == g_movers.end() ? 0 : it->second.house;
		}

		RE::TESObjectREFR* PlayerBedIn(RE::FormID house)
		{
			std::scoped_lock l(g_lock);
			auto it = g_playerBeds.find(house);
			return it == g_playerBeds.end() ? nullptr : RE::TESForm::LookupByID<RE::TESObjectREFR>(it->second);
		}

		// The player's bed stays the player's (another mod, or a release, may have changed it).
		void GuardPlayerBed(RE::FormID house)
		{
			auto* bed = PlayerBedIn(house);
			auto* playerBase = Game::Player()->GetActorBase();
			if (bed && OwnOwner(bed) != playerBase) {
				SetOwnerSaved(bed, playerBase);
				logger::info("move in: bed {:08X} is the player's again", bed->GetFormID());
			}
		}

		// SeverActions claims a bed for them in the player's room. `wanted` (0 = any free one): every
		// other bed is the player's for that moment, so the claim can only land on it. `share`: the bed
		// claimed is owned by the player afterwards, the claim stays hers (a Lover, a double bed).
		void ClaimBed(RE::FormID npcId, RE::FormID wantedId, bool share, bool singleFirst = false)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* cell = Game::Player()->GetParentCell();
			if (!a || !cell || !cell->IsInteriorCell()) {
				return;
			}
			auto* playerBase = Game::Player()->GetActorBase();
			auto* wanted = wantedId ? RE::TESForm::LookupByID<RE::TESObjectREFR>(wantedId) : nullptr;
			auto hidden = std::make_shared<std::vector<std::pair<RE::FormID, RE::FormID>>>();  // bed -> its own owner (0 = none)
			if (wanted) {
				cell->ForEachReference([&](RE::TESObjectREFR* r) {
					if (IsBed(r) && r != wanted) {
						auto* owner = OwnOwner(r);
						if (owner != playerBase) {
							hidden->emplace_back(r->GetFormID(), owner ? owner->GetFormID() : 0);
							r->SetOwner(playerBase);
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				if (OwnOwner(wanted) == playerBase) {
					hidden->emplace_back(wanted->GetFormID(), playerBase->GetFormID());  // the player's: open for this one claim
					wanted->SetOwner(nullptr);
				}
			} else if (singleFirst) {
				// The player's bed may not be known yet (never slept here): the double beds wait, a single
				// one is taken first.
				cell->ForEachReference([&](RE::TESObjectREFR* r) {
					if (IsBed(r) && IsDouble(r)) {
						auto* owner = OwnOwner(r);
						if (owner != playerBase) {
							hidden->emplace_back(r->GetFormID(), owner ? owner->GetFormID() : 0);
							r->SetOwner(playerBase);
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			const auto house = HouseOf(npcId);
			const std::string name = a->GetName();
			auto after = [npcId, wantedId, share, singleFirst, hidden, house, name](const RE::BSScript::Variable&) {
				SKSE::GetTaskInterface()->AddTask([npcId, wantedId, share, singleFirst, hidden, house, name] {
					for (auto& [bedId, ownerId] : *hidden) {
						if (auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(bedId)) {
							bed->SetOwner(ownerId ? RE::TESForm::LookupByID(ownerId) : nullptr);
						}
					}
					auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId);
					if (!a) {
						return;
					}
					Papyrus::CallStatic("SeverActionsNative", "Native_BedAssignment_GetBedFormID",
						[npcId, wantedId, share, singleFirst, house, name](const RE::BSScript::Variable& v) {
							const auto got = v.IsInt() ? static_cast<RE::FormID>(v.GetSInt()) : 0u;
							SKSE::GetTaskInterface()->AddTask([npcId, wantedId, share, singleFirst, house, name, got] {
								if (!got && singleFirst) {
									ClaimBed(npcId, 0, share, false);  // no single bed free: a double one, then
									return;
								}
								auto* bed = got ? RE::TESForm::LookupByID<RE::TESObjectREFR>(got) : nullptr;
								const bool shared = bed && share && (IsDouble(bed) || bed == PlayerBedIn(house));
								if (shared) {
									SetOwnerSaved(bed, Game::Player()->GetActorBase());
								} else if (bed) {
									// Theirs for good (the claim set it in the engine; Papyrus keeps it in the save).
									if (auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId)) {
										SetOwnerSaved(bed, a->GetActorBase());
									}
								}
								GuardPlayerBed(house);
								logger::info("move in: {} sleeps in bed {:08X}{}{}", name, got, shared ? " (shared with the player)" : "",
									wantedId && got != wantedId ? " - NOT the one picked" : "");
								if (wantedId) {
									Game::Notify(got == wantedId ? std::format("{} sleeps in that bed now{}.", name, shared ? ", with you" : "") :
									                               std::format("{} couldn't take that bed.", name));
								} else if (!got) {
									Game::Notify(std::format("No free bed for {}: a bedroll at night until there is one.", name));
								}
							});
						},
						static_cast<RE::Actor*>(a));
				});
			};
			// Their earlier bed goes back first, then the claim.
			const bool called = Papyrus::CallStatic("SeverActionsNative", "Native_BedAssignment_Release",
				[npcId, after](const RE::BSScript::Variable&) {
					SKSE::GetTaskInterface()->AddTask([npcId, after] {
						auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId);
						if (!a || !Papyrus::CallStatic("SeverActionsNative", "Native_BedAssignment_Claim", after, static_cast<RE::Actor*>(a))) {
							after(RE::BSScript::Variable{});
						}
					});
				},
				static_cast<RE::Actor*>(a));
			if (!called) {
				after(RE::BSScript::Variable{});
			}
		}

		// A move-in's bed: a Lover takes the player's bed (shared) when there is one, else a bed of their
		// own (a double one shared); anyone else a free bed of their own, never the player's.
		void ClaimHomeBed(RE::FormID npcId)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!a) {
				return;
			}
			const bool lover = IsLover(a);
			auto* playerBed = PlayerBedIn(HouseOf(npcId));
			ClaimBed(npcId, lover && playerBed ? playerBed->GetFormID() : 0, lover, !lover && !playerBed);
		}

		bool InWindow(float hour, float from, float to)
		{
			return from <= to ? hour >= from && hour < to : hour >= from || hour < to;
		}

		RE::TESFurniture* BedrollBase()
		{
			static RE::TESFurniture* cached = [] {
				for (auto* f : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESFurniture>()) {
					auto* file = f ? f->GetFile(0) : nullptr;
					auto* m = f ? skyrim_cast<RE::TESModel*>(f) : nullptr;
					if (file && std::string_view(file->GetFilename()) == "Skyrim.esm" && f->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) && m &&
						m->GetModel() && JsonUtil::Lower(m->GetModel()).find("bedroll") != std::string::npos) {
						return f;
					}
				}
				return static_cast<RE::TESFurniture*>(nullptr);
			}();
			return cached;
		}

		void PackRoll(RE::FormID npcId, RE::FormID rollId)
		{
			if (auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId)) {
				Papyrus::CallStatic("SeverActions_FurnitureLib", "Stop", nullptr, static_cast<RE::Actor*>(a));
			}
			std::thread([rollId] {
				std::this_thread::sleep_for(3s);
				SKSE::GetTaskInterface()->AddTask([rollId] {
					if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(rollId)) {
						Papyrus::CallMethod(ref, "ObjectReference", "Disable", nullptr, false);
						Papyrus::CallMethod(ref, "ObjectReference", "Delete", nullptr);
					}
				});
			}).detach();
		}

		// No bed of their own: at night (SeverActions' sleep hours) a bedroll where they are, packed
		// away in the morning or when they leave the house.
		void TickBedrolls(const std::vector<std::pair<RE::FormID, Mover>>& movers)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - g_sleepRead > 60s) {
				g_sleepRead = now;
				Papyrus::CallStatic("SeverActionsNativeExt2", "Settings_GetBool", [](const RE::BSScript::Variable& v) { g_sleepOn = !v.IsBool() || v.GetBool(); },
					RE::BSFixedString("homeSleepEnabled"));
				Papyrus::CallStatic("SeverActionsNativeExt2", "Settings_GetFloat",
					[](const RE::BSScript::Variable& v) {
						if (v.IsFloat() && v.GetFloat() >= 0.0f && v.GetFloat() <= 24.0f) {
							g_sleepFrom = v.GetFloat();
						}
					},
					RE::BSFixedString("homeSleepStart"));
				Papyrus::CallStatic("SeverActionsNativeExt2", "Settings_GetFloat",
					[](const RE::BSScript::Variable& v) {
						if (v.IsFloat() && v.GetFloat() >= 0.0f && v.GetFloat() <= 24.0f) {
							g_sleepTo = v.GetFloat();
						}
					},
					RE::BSFixedString("homeSleepEnd"));
			}
			for (auto id : std::exchange(g_staleRolls, {})) {
				PackRoll(0, id);
			}
			const bool night = g_sleepOn && InWindow(Game::HourOfDay(), g_sleepFrom, g_sleepTo);
			for (auto& [id, m] : movers) {
				auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!a || m.stage != Stage::kHome) {
					continue;
				}
				const bool home = a->Is3DLoaded() && LivesIn(id, a->GetParentCell()) && !Services::IsCompanion(a) && !Services::WalkingWithPlayer(id);
				if (auto it = g_rolls.find(id); it != g_rolls.end()) {
					if (!night || !home) {
						logger::info("move in: {} gets up from the bedroll", a->GetName());
						PackRoll(id, it->second);
						g_rolls.erase(it);
					}
					continue;
				}
				if (!night || !home || a->GetOccupiedFurniture().get() || a->IsInCombat()) {
					continue;
				}
				if (auto& at = g_rollChecked[id]; now - at < 5min) {
					continue;
				} else {
					at = now;
				}
				Papyrus::CallStatic("SeverActionsNative", "Native_BedAssignment_GetBedFormID",
					[id](const RE::BSScript::Variable& v) {
						if (v.IsInt() && v.GetSInt() != 0) {
							return;  // a bed of their own: SeverActions puts them to bed
						}
						SKSE::GetTaskInterface()->AddTask([id] {
							auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
							auto* base = BedrollBase();
							if (!a || !base || g_rolls.contains(id) || a->GetOccupiedFurniture().get()) {
								return;
							}
							auto ref = a->PlaceObjectAtMe(base, false);
							if (!ref) {
								return;
							}
							ref->SetAngle({ 0.0f, 0.0f, a->GetAngleZ() });
							g_rolls[id] = ref->GetFormID();
							Papyrus::CallStatic("SeverActions_FurnitureLib", "UseRef", nullptr, static_cast<RE::Actor*>(a), static_cast<RE::TESObjectREFR*>(ref.get()));
							logger::info("move in: no bed for {}; a bedroll for the night ({:08X})", a->GetName(), ref->GetFormID());
						});
					},
					static_cast<RE::Actor*>(a));
			}
		}

		// The player lies down in a bed in one of their houses: that's their bed there, theirs to keep.
		struct SleepStartSink : RE::BSTEventSink<RE::TESSleepStartEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStartEvent*, RE::BSTEventSource<RE::TESSleepStartEvent>*) override
			{
				SKSE::GetTaskInterface()->AddTask([] {
					auto* player = Game::Player();
					auto bedPtr = player->GetOccupiedFurniture().get();
					auto* bed = bedPtr.get();
					auto* cell = player->GetParentCell();
					auto* loc = cell ? cell->GetLocation() : nullptr;
					auto* house = loc && loc->HasKeywordString("LocTypePlayerHouse") ? loc :
					              loc && loc->parentLoc && loc->parentLoc->HasKeywordString("LocTypePlayerHouse") ? loc->parentLoc :
					                                                                                        nullptr;
					if (!bed || !IsBed(bed) || !house) {
						return;
					}
					auto* owner = OwnOwner(bed);
					auto* playerBase = player->GetActorBase();
					// Someone else's own bed (a housemate's): borrowed for the night, not taken.
					if (owner && owner != playerBase && owner->As<RE::TESNPC>()) {
						return;
					}
					{
						std::scoped_lock l(g_lock);
						g_playerBeds[house->GetFormID()] = bed->GetFormID();
					}
					if (owner != playerBase) {
						SetOwnerSaved(bed, playerBase);
					}
					logger::info("move in: bed {:08X} is the player's own at {}", bed->GetFormID(), house->GetName());
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		void InstallSleepSink()
		{
			static bool done = false;
			if (done) {
				return;
			}
			if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
				static SleepStartSink sink;
				src->AddEventSink<RE::TESSleepStartEvent>(&sink);
				done = true;
			}
		}

		// The home is theirs: SeverActions' AssignHome (its anchor goes where the player stands, so with
		// the player elsewhere it's moved to them; its bed claim works in the player's cell only, so the
		// bed waits until the player is there with them).
		void Finalize(RE::FormID id, const std::string& house)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			auto* travel = Travel();
			auto* followers = Followers();
			if (!a || !followers) {
				SetBusy(id, false);
				return;
			}
			auto assign = [id, house, followers] {
				SKSE::GetTaskInterface()->AddTask([id, house, followers] {
					auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
					if (!a) {
						return;
					}
					const bool together = a->GetParentCell() == Game::Player()->GetParentCell();
					const bool called = Papyrus::CallMethod(followers, "SeverActions_FollowerManager", "AssignHome",
						[id, house, together](const RE::BSScript::Variable&) {
							SKSE::GetTaskInterface()->AddTask([id, house, together] {
								auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
								if (!a) {
									return;
								}
								if (!together) {
									// The anchor where they are (inside the house), not where the player is.
									Papyrus::CallStatic("StorageUtil", "GetFormValue",
										[id](const RE::BSScript::Variable& v) {
											auto* marker = v.IsObject() && !v.IsNoneObject() ? v.Unpack<RE::TESObjectREFR*>() : nullptr;
											SKSE::GetTaskInterface()->AddTask([id, marker] {
												auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
												if (a && marker) {
													Papyrus::CallMethod(marker, "ObjectReference", "MoveTo", nullptr, static_cast<RE::TESObjectREFR*>(a), 0.0f, 0.0f, 0.0f, true);
													Papyrus::CallMethod(a, "Actor", "EvaluatePackage", nullptr);
													logger::info("move in: {}'s home anchor set where they are", a->GetName());
												} else {
													logger::warn("move in: SeverActions' home anchor not found; it stays where the player was");
												}
											});
										},
										static_cast<RE::TESForm*>(a), RE::BSFixedString("SeverHomeB_Marker"), static_cast<RE::TESForm*>(nullptr));
								}
								{
									std::scoped_lock l(g_lock);
									if (auto it = g_movers.find(id); it != g_movers.end()) {
										it->second.stage = Stage::kHome;
										it->second.bedPending = !together;
										it->second.busy = false;
									}
								}
								if (together) {
									ClaimHomeBed(id);  // AssignHome's own claim may have taken the player's bed: done our way
								}
								Bonds::Add(a, Bonds::Deed::kHome, 3);
								SkyrimNet::AddMemory(id, std::format("{} moved in with {} at {}. It is their home now; they live there together.", a->GetName(), PlayerName(), house), 0.9f);
								Game::Notify(std::format("{} now lives at {}.", a->GetName(), house));
								logger::info("move in: {} lives at {} now{}", a->GetName(), house, together ? "" : " (their bed there is chosen when you're there together)");
							});
						},
						static_cast<RE::Actor*>(a), RE::BSFixedString(house));
					if (!called) {
						logger::error("move in: SeverActions' AssignHome could not be called");
						SetBusy(id, false);
					}
				});
			};
			// The trip (or its arrival wait) ends first, so it doesn't hold them.
			if (!travel || !Papyrus::CallMethod(travel, "SeverActions_Travel", "CancelTravel", [assign](const RE::BSScript::Variable&) { assign(); },
							   static_cast<RE::Actor*>(a), false)) {
				assign();
			}
		}

		// Couldn't walk there (no way found, or a day has passed): placed inside the house, out of sight.
		void PlaceThere(RE::FormID id, const std::string& house)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!a) {
				SetBusy(id, false);
				return;
			}
			const bool asked = Papyrus::CallStatic("SeverActionsNative", "ResolveDestination",
				[id, house](const RE::BSScript::Variable& v) {
					auto* ref = v.IsObject() && !v.IsNoneObject() ? v.Unpack<RE::TESObjectREFR*>() : nullptr;
					SKSE::GetTaskInterface()->AddTask([id, house, ref] {
						auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
						if (!a) {
							return;
						}
						if (!ref) {
							logger::warn("move in: SeverActions doesn't know where '{}' is; {} stays put", house, a->GetName());
							Game::Notify(std::format("{} couldn't find the way to {}.", a->GetName(), house));
							std::scoped_lock l(g_lock);
							g_movers.erase(id);
							return;
						}
						auto place = [id, house](RE::TESObjectREFR* to) {
							auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
							if (!a || !to) {
								SetBusy(id, false);
								return;
							}
							if (auto* travel = Travel()) {
								Papyrus::CallMethod(travel, "SeverActions_Travel", "CancelTravel", nullptr, static_cast<RE::Actor*>(a), false);
							}
							Papyrus::CallMethod(a, "ObjectReference", "MoveTo", nullptr, to, 0.0f, 0.0f, 0.0f, true);
							logger::info("move in: {} placed at {} (out of sight)", a->GetName(), house);
							SetStage(id, Stage::kSettle);
						};
						// A door: on through it, to the inside.
						if (ref->GetBaseObject() && ref->GetBaseObject()->Is(RE::FormType::Door)) {
							const bool inner = Papyrus::CallStatic("SeverActionsNative", "FindInteriorMarkerForDoor",
								[place, ref](const RE::BSScript::Variable& m) {
									auto* marker = m.IsObject() && !m.IsNoneObject() ? m.Unpack<RE::TESObjectREFR*>() : nullptr;
									SKSE::GetTaskInterface()->AddTask([place, marker, ref] { place(marker ? marker : ref); });
								},
								ref, static_cast<RE::Actor*>(a));
							if (!inner) {
								place(ref);
							}
							return;
						}
						place(ref);
					});
				},
				static_cast<RE::Actor*>(a), RE::BSFixedString(house));
			if (!asked) {
				SetBusy(id, false);
			}
		}

		void Start(RE::FormID id, RE::FormID houseId)
		{
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			auto* house = RE::TESForm::LookupByID<RE::BGSLocation>(houseId);
			if (!a || !house || !house->GetName()) {
				return;
			}
			const std::string name = house->GetName();
			{
				std::scoped_lock l(g_lock);
				auto& m = g_movers[id];
				m = Mover{};
				m.house = houseId;
				m.name = name;
				m.since = Game::Hours();
				m.stage = Services::IsCompanion(a) ? Stage::kFollowerWait : Stage::kTravel;
			}
			logger::info("move in: {} moves into {}", a->GetName(), name);
			if (Services::IsCompanion(a)) {
				Game::Notify(std::format("{} will make {} their home once you bring them there.", a->GetName(), name));
				return;
			}
			// Walking with the player or keeping them company ends: they're off to their new home.
			if (Services::WalkingWithPlayer(id)) {
				Services::StopWalking(id, "moving in");
			}
			Game::Mention(std::format("{} went to gather their things and set off for {}, their new home with {}.", a->GetName(), name, PlayerName()), a);
			auto* travel = Travel();
			const bool called = travel && Papyrus::CallMethod(travel, "SeverActions_Travel", "DoTravelToPlace",
											  [id, name](const RE::BSScript::Variable& started) {
												  const bool ok = started.IsBool() && started.GetBool();
												  logger::info("move in: the walk to {} {}", name, ok ? "started" : "could not start; placed there once out of sight");
												  if (!ok) {
													  std::scoped_lock l(g_lock);
													  if (auto it = g_movers.find(id); it != g_movers.end()) {
														  it->second.placeNow = true;
													  }
												  }
											  },
											  static_cast<RE::Actor*>(a), RE::BSFixedString(name), 48.0f, true, static_cast<std::int32_t>(0), false);
			if (!called) {
				std::scoped_lock l(g_lock);
				if (auto it = g_movers.find(id); it != g_movers.end()) {
					it->second.placeNow = true;
				}
			}
		}

		void MoveOut(RE::FormID id, const char* why, const std::string& narration)
		{
			std::string house;
			RE::FormID houseId = 0;
			{
				std::scoped_lock l(g_lock);
				auto it = g_movers.find(id);
				if (it == g_movers.end()) {
					return;
				}
				house = it->second.name;
				houseId = it->second.house;
				g_movers.erase(it);
			}
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!a) {
				return;
			}
			if (auto* travel = Travel()) {
				Papyrus::CallMethod(travel, "SeverActions_Travel", "CancelTravel", nullptr, static_cast<RE::Actor*>(a), false);
			}
			if (auto* followers = Followers()) {
				Papyrus::CallMethod(followers, "SeverActions_FollowerManager", "ClearHome", nullptr, static_cast<RE::Actor*>(a));
			}
			// Their bed goes back to whoever had it before; a shared one was the player's, and stays so.
			if (auto it = g_rolls.find(id); it != g_rolls.end()) {
				PackRoll(id, it->second);
				g_rolls.erase(it);
			}
			std::thread([houseId] {
				std::this_thread::sleep_for(3s);
				SKSE::GetTaskInterface()->AddTask([houseId] { GuardPlayerBed(houseId); });
			}).detach();
			SkyrimNet::AddMemory(id, std::format("{} moved out of {}'s home at {} and went back to their old life.", a->GetName(), PlayerName(), house), 0.8f);
			Game::Notify(std::format("{} moved out of {}.", a->GetName(), house));
			if (!narration.empty()) {
				Game::Mention(narration, a);
			}
			logger::info("move in: {} moved out of {} ({})", a->GetName(), house, why);
		}
	}

	std::vector<std::pair<RE::BGSLocation*, std::string>> PlayerHouses()
	{
		std::vector<std::pair<RE::BGSLocation*, std::string>> out;
		for (auto& h : Houses()) {
			out.emplace_back(h.loc, h.name);
		}
		return out;
	}

	bool PlayerAsks(const std::string& line)
	{
		return HasAny(Padded(line), { "live with me", "move in with me", "come live", "come and live", "live together", "live at my", "live in my",
			"move into my", "move in here", "move in to my", "you could move in", "you can move in", "you should move in", "want to move in",
			"like to move in", "share my home", "share my house", "stay with me for good", "stay with me permanently", "be my housemate",
			"make my home your home", "make my house your home", "make it your home" });
	}

	bool NpcAsks(const std::string& line)
	{
		return HasAny(Padded(line), { "live with you", "move in with you", "share your home", "come live with you", "stay with you for good",
			"live together" });
	}

	bool MoveOutWords(const std::string& line)
	{
		// About them leaving the home, said to them: not "let's move out" (let's go), "move back" (step back)
		// or "pack your things, we're off".
		const auto p = Padded(line);
		if (HasAny(p, { "let s move out", "lets move out", "we move out", "we ll move out", "move out at", "move out now", "time to move out" })) {
			return false;
		}
		return HasAny(p, { "you move out", "you to move out", "you should move out", "you have to move out", "you need to move out", "you must move out",
			"you can move out", "you re moving out", "move out of my", "move out of the house", "move out of here", "go back to your old home",
			"back to your old life", "back to your own home", "go back to your home", "find somewhere else to live", "find another place to live",
			"pack your things and go", "pack your things and leave", "you can t live here", "you cant live here", "you can no longer live here",
			"don t live here anymore", "dont live here anymore", "leave my house", "get out of my house", "move back to your", "move back home" });
	}

	bool Lives(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		return g_movers.contains(npc);
	}

	bool LivesIn(RE::FormID npc, RE::TESObjectCELL* cell)
	{
		RE::FormID house = 0;
		{
			std::scoped_lock l(g_lock);
			auto it = g_movers.find(npc);
			if (it == g_movers.end() || it->second.stage != Stage::kHome) {
				return false;
			}
			house = it->second.house;
		}
		auto* loc = cell ? cell->GetLocation() : nullptr;
		return loc && (loc->GetFormID() == house || (loc->parentLoc && loc->parentLoc->GetFormID() == house));
	}

	bool IsHousemateBedroll(RE::FormID ref)
	{
		return std::ranges::any_of(g_rolls, [ref](const auto& kv) { return kv.second == ref; });
	}

	std::pair<RE::BGSLocation*, std::string> SharedHouse(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		auto it = g_movers.find(npc);
		if (it == g_movers.end()) {
			return { nullptr, "" };
		}
		return { RE::TESForm::LookupByID<RE::BGSLocation>(it->second.house), it->second.name };
	}

	bool BedWords(const std::string& line)
	{
		// Telling, not asking: "which bed is yours?" / "do you sleep here?" ask where they sleep now.
		const auto p = Padded(line);
		if (HasAny(p, { "pick a bed", "choose a bed", "which bed do you want", "which bed would you like", "want this bed", "like this bed" })) {
			return true;
		}
		if (line.find('?') != std::string::npos) {
			return false;
		}
		return HasAny(p, { "this is your bed", "this bed is yours", "this one is your bed", "this one s your bed", "that is your bed",
			"that s your bed", "that bed is yours", "you can sleep here", "you ll sleep here", "you will sleep here", "sleep in this bed", "take this bed",
			"your bed is this", "this will be your bed", "this can be your bed", "this bed is for you" });
	}

	void AssignBed(RE::FormID npc, const std::string& line)
	{
		auto* a = RE::TESForm::LookupByID<RE::Actor>(npc);
		auto* player = Game::Player();
		auto* cell = player->GetParentCell();
		if (!a || !cell) {
			return;
		}
		const std::string name = a->GetName();
		if (!LivesIn(npc, cell)) {
			Game::Notify(std::format("Only in the house {} lives in.", name));
			return;
		}
		auto* playerBed = PlayerBedIn(HouseOf(npc));
		const bool lover = IsLover(a);
		auto* playerBase = player->GetActorBase();
		// May this one be theirs? Not the player's (but a Lover shares it), not someone else's own.
		auto allowed = [&](RE::TESObjectREFR* bed, std::string* why) {
			auto* owner = OwnOwner(bed);
			if (bed == playerBed && !lover) {
				if (why) {
					*why = "That's your own bed: pick another one.";
				}
				return false;
			}
			if (owner && owner != playerBase && owner != a->GetActorBase() && owner->As<RE::TESNPC>()) {
				if (why) {
					*why = std::format("That bed is {}'s.", owner->GetName());
				}
				return false;
			}
			return true;
		};
		// Looking at a bed (and not asking to choose): that one.
		const auto p = Padded(line);
		const bool choose = HasAny(p, { "pick a bed", "choose a bed", "which bed" });
		if (!choose) {
			auto* pick = RE::CrosshairPickData::GetSingleton();
			auto target = pick ? pick->GetActiveTarget().get() : nullptr;
			if (target && IsBed(target.get()) && target->GetParentCell() == cell) {
				std::string why;
				if (!allowed(target.get(), &why)) {
					Game::Notify(why);
					return;
				}
				ClaimBed(npc, target->GetFormID(), lover);
				return;
			}
		}
		// Otherwise the beds of this room to choose from.
		Confirm::Offer offer;
		offer.title = "Their bed";
		offer.npc = name;
		offer.detail = std::format("Which bed is {}'s?", name);
		offer.showPrice = false;
		offer.seconds = 0;
		offer.acceptLabel = "This one";
		offer.declineLabel = "Never mind";
		offer.hint = "Beds in this room.";
		offer.chosenText = "Bed: ";
		const auto here = player->GetPosition();
		cell->ForEachReference([&](RE::TESObjectREFR* r) {
			if (!IsBed(r) || !allowed(r, nullptr) || offer.pick.size() >= 12) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto pos = r->GetPosition();
			const float dz = pos.z - here.z;
			auto* owner = OwnOwner(r);
			std::string label = std::format("{} {}", IsDouble(r) ? "Double bed" : "Bed", dz > 150.0f ? "upstairs" : dz < -150.0f ? "downstairs" : "on this floor");
			std::string note = r == playerBed ? "yours, shared" : owner == a->GetActorBase() ? "theirs now" : std::format("{} m", static_cast<int>(pos.GetDistance(here) / 70.0f));
			offer.pick.push_back({ r->GetFormID(), label, 1, 0, false, note });
			return RE::BSContainer::ForEachResult::kContinue;
		});
		if (offer.pick.empty()) {
			Game::Notify(std::format("No bed in this room can be {}'s.", name));
			return;
		}
		const bool shown = Confirm::Ask(offer, [npc, lover](Confirm::Choice c) {
			if (c == Confirm::Choice::kAccept) {
				ClaimBed(npc, Confirm::LastPick().first, lover);
			}
		});
		if (!shown) {
			Game::Notify("Picking a bed needs PrismaUI: look at the bed and say it again.");
		}
	}

	bool Invite(RE::Actor* npc)
	{
		const std::string name = npc->GetName();
		const auto id = npc->GetFormID();
		// The real relationship decides (Bonds), not a few warm words: Friend and above.
		if (!Promises::IsClose(npc)) {
			Game::Notify(std::format("{} ({}): not close enough to move in with you", name, Rest::RelationName(npc)));
			Game::Narrate(std::format("{} smiled, but shook their head: they didn't know {} nearly well enough to move in with them.", name, PlayerName()), npc);
			logger::info("move in: {} isn't close enough ({})", name, Rest::RelationName(npc));
			return true;
		}
		auto houses = Houses();
		if (houses.empty()) {
			Game::Notify(std::format("You have no house of your own for {} to move into.", name));
			Game::Narrate(std::format("{} would have liked that, but {} had no home of their own to share.", name, PlayerName()), npc);
			return true;
		}
		Confirm::Offer offer;
		offer.title = "Move in";
		offer.npc = name;
		offer.sameAs = { "retainer" };  // SeverActions' "where will they live/work" popup
		offer.detail = std::format("Where will {} live?", name);
		if (HasDuties(npc)) {
			offer.detail += " Their work, shop or a quest they're part of may stop while they live with you.";
		}
		offer.showPrice = false;
		offer.seconds = 0;
		offer.acceptLabel = "Live here";
		offer.declineLabel = "Not now";
		offer.hint = "Pick one of your homes.";
		offer.chosenText = "Home: ";
		{
			std::scoped_lock l(g_lock);
			if (auto it = g_movers.find(id); it != g_movers.end()) {
				offer.detail = std::format("{} lives at {} now. Move them to another of your homes?", name, it->second.name);
			}
		}
		for (auto& h : houses) {
			offer.pick.push_back({ h.loc->GetFormID(), h.name, 1, 0, false, std::string_view(h.why) == "here" ? "you're here" : "" });
		}
		const bool shown = Confirm::Ask(offer, [id](Confirm::Choice c) {
			if (c != Confirm::Choice::kAccept) {
				logger::info("move in: no house picked");
				return;
			}
			const auto picked = Confirm::LastPick().first;
			{
				std::scoped_lock l(g_lock);
				if (auto it = g_movers.find(id); it != g_movers.end() && it->second.house == picked) {
					Game::Notify(std::format("They already live at {}.", it->second.name));
					return;
				}
			}
			Start(id, picked);
		});
		if (!shown) {
			if (houses.size() == 1) {
				Start(id, houses.front().loc->GetFormID());  // no popup to ask with: the one house there is
			} else {
				Game::Notify("Move in: the house popup needs PrismaUI.");
			}
		}
		return true;
	}

	void AskMoveOut(RE::FormID npc)
	{
		auto* a = RE::TESForm::LookupByID<RE::Actor>(npc);
		std::string house;
		{
			std::scoped_lock l(g_lock);
			auto it = g_movers.find(npc);
			if (!a || it == g_movers.end()) {
				return;
			}
			house = it->second.name;
		}
		Confirm::Offer offer;
		offer.title = "Move out";
		offer.npc = a->GetName();
		offer.detail = std::format("Leaves {} and goes back to their old life", house);
		offer.showPrice = false;
		offer.acceptLabel = Game::Pick({ "Yes. Go back to your old home.", "It's time you moved out." });
		offer.declineLabel = Game::Pick({ "No, stay. I didn't mean it.", "Forget I said that." });
		const auto lines = std::make_pair(offer.acceptLabel, offer.declineLabel);
		const bool shown = Confirm::Ask(offer, [npc, lines](Confirm::Choice c) {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(npc);
			if (!a || c == Confirm::Choice::kTimeout) {
				return;
			}
			Game::Say(c == Confirm::Choice::kAccept ? lines.first : lines.second, a, true);
			if (c == Confirm::Choice::kAccept) {
				MoveOut(npc, "the player asked", "");
			}
		});
		if (!shown) {
			MoveOut(npc, "the player asked", "");
		}
	}

	void Tick()
	{
		const auto now = std::chrono::steady_clock::now();
		if (now - g_lastTick < 3s) {
			return;
		}
		g_lastTick = now;
		const bool checkHomes = now - g_lastHomeCheck > 60s;
		if (checkHomes) {
			g_lastHomeCheck = now;
		}
		InstallSleepSink();
		std::vector<std::pair<RE::FormID, Mover>> list;
		{
			std::scoped_lock l(g_lock);
			list.assign(g_movers.begin(), g_movers.end());
		}
		TickBedrolls(list);
		auto* player = Game::Player();
		auto* playerCell = player->GetParentCell();
		const bool playerOutdoors = playerCell && !playerCell->IsInteriorCell();
		for (auto& [id, m] : list) {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			auto* house = RE::TESForm::LookupByID<RE::BGSLocation>(m.house);
			if (!a || a->IsDead() || !house) {
				std::scoped_lock l(g_lock);
				g_movers.erase(id);
				continue;
			}
			if (m.busy) {
				continue;
			}
			switch (m.stage) {
			case Stage::kTravel:
				if (InHouse(a, house)) {
					logger::info("move in: {} arrived at {}", a->GetName(), m.name);
					SetStage(id, Stage::kSettle);
				} else if ((m.placeNow || Game::Hours() - m.since > 24.0f) && Game::DistanceToPlayer(a) > 4000.0f) {
					SetBusy(id, true);
					PlaceThere(id, m.name);
				}
				break;
			case Stage::kSettle:
				// Out of doors (the bed claim then can't take a bed in someone else's house) or with them.
				if (playerOutdoors || playerCell == a->GetParentCell()) {
					SetBusy(id, true);
					Finalize(id, m.name);
				}
				break;
			case Stage::kFollowerWait:
				if (InHouse(a, house) && playerCell == a->GetParentCell()) {
					SetBusy(id, true);
					Finalize(id, m.name);
				}
				break;
			case Stage::kHome:
				// Their bed there: claimed the first time the player is there with them.
				if (m.bedPending && playerCell == a->GetParentCell() && InHouse(player, house)) {
					{
						std::scoped_lock l(g_lock);
						if (auto it = g_movers.find(id); it != g_movers.end()) {
							it->second.bedPending = false;
						}
					}
					ClaimHomeBed(id);
				}
				// Things gone sour: no longer close, attacked, or hurt and not made up within a day (time
				// enough to apologise for a word taken wrong). They move back out, and say why.
				if (!Promises::IsClose(a) || Trust::Attacked(id) || Trust::GrudgeHours(id) > 24.0f) {
					MoveOut(id, "things went sour",
						std::format("{} packed their things and left {}: things between them and {} had soured too much to keep living together.", a->GetName(), m.name,
							PlayerName()));
					break;
				}
				// SeverActions cleared their home itself (its own menu, a dismissal): ours ends quietly too.
				if (checkHomes) {
					Papyrus::CallStatic("SeverActionsNative", "Native_GetHome",
						[id](const RE::BSScript::Variable& v) {
							const std::string home = v.IsString() ? std::string(v.GetString()) : std::string();
							if (home.empty()) {
								SKSE::GetTaskInterface()->AddTask([id] {
									std::scoped_lock l(g_lock);
									if (auto it = g_movers.find(id); it != g_movers.end() && it->second.stage == Stage::kHome) {
										logger::info("move in: SeverActions no longer has a home for {:08X}; ours ends", id);
										g_movers.erase(it);
									}
								});
							}
						},
						static_cast<RE::Actor*>(a));
				}
				break;
			}
		}
	}

	std::vector<std::string> BioLines(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		auto it = g_movers.find(npc);
		if (it == g_movers.end()) {
			return {};
		}
		const auto& m = it->second;
		const auto player = PlayerName();
		switch (m.stage) {
		case Stage::kHome:
			return { std::format("Lives with {} at {} now; it is their home.", player, m.name) };
		case Stage::kFollowerWait:
			return { std::format("Has agreed to make {} their home with {}, once they get there.", m.name, player) };
		default:
			return { std::format("Is moving into {}'s home, {}, and is on the way there.", player, m.name) };
		}
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json out = json::array();
		for (auto& [id, m] : g_movers) {
			out.push_back({ { "npc", id }, { "house", m.house }, { "name", m.name }, { "stage", static_cast<int>(m.stage) }, { "since", m.since },
				{ "placeNow", m.placeNow }, { "bedPending", m.bedPending } });
		}
		json beds = json::array();
		for (auto& [house, bed] : g_playerBeds) {
			beds.push_back({ { "house", house }, { "bed", bed } });
		}
		json rolls = json::array();
		for (auto& [npc, roll] : g_rolls) {
			rolls.push_back(roll);
		}
		return { { "movers", out }, { "playerBeds", beds }, { "rolls", rolls } };
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		std::scoped_lock l(g_lock);
		g_movers.clear();
		g_playerBeds.clear();
		g_rolls.clear();
		g_rollChecked.clear();
		g_staleRolls.clear();
		// Older saves stored just the movers' list.
		const json movers = j.is_array() ? j : j.is_object() && j.contains("movers") && j["movers"].is_array() ? j["movers"] : json::array();
		if (j.is_object()) {
			for (auto& b : j.value("playerBeds", json::array())) {
				const auto house = resolve(static_cast<RE::FormID>(JsonUtil::Num(b, "house").value_or(0)));
				const auto bed = resolve(static_cast<RE::FormID>(JsonUtil::Num(b, "bed").value_or(0)));
				if (house && bed) {
					g_playerBeds[house] = bed;
				}
			}
			// Bedrolls laid the night this save was made: packed away (whoever slept there gets up).
			for (auto& r : j.value("rolls", json::array())) {
				if (r.is_number()) {
					if (const auto id = resolve(r.get<RE::FormID>())) {
						g_staleRolls.push_back(id);
					}
				}
			}
		}
		for (auto& o : movers) {
			const auto id = resolve(static_cast<RE::FormID>(JsonUtil::Num(o, "npc").value_or(0)));
			const auto house = resolve(static_cast<RE::FormID>(JsonUtil::Num(o, "house").value_or(0)));
			if (!id || !house) {
				continue;
			}
			Mover m;
			m.house = house;
			m.name = JsonUtil::Str(o, "name");
			m.stage = static_cast<Stage>(std::clamp(static_cast<int>(JsonUtil::Num(o, "stage").value_or(0)), 0, 3));
			m.since = static_cast<float>(JsonUtil::Num(o, "since").value_or(0));
			m.placeNow = o.contains("placeNow") && o["placeNow"].is_boolean() && o["placeNow"].get<bool>();
			m.bedPending = o.contains("bedPending") && o["bedPending"].is_boolean() && o["bedPending"].get<bool>();
			g_movers[id] = m;
		}
		logger::info("co-save: {} living with the player (or moving in)", g_movers.size());
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_movers.clear();
		g_playerBeds.clear();
		g_rolls.clear();
		g_rollChecked.clear();
		g_staleRolls.clear();
	}
}
