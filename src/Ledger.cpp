#include "Ledger.h"

#include <fstream>
#include <set>

#include "Bio.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Bonds.h"
#include "Keys.h"
#include "Loans.h"
#include "Brawls.h"
#include "Keepsakes.h"
#include "Pledges.h"
#include "MoveIn.h"
#include "Moments.h"
#include "Papyrus.h"
#include "Rest.h"
#include "Safekeeping.h"
#include "Services.h"
#include "Trust.h"
#include "SkyrimNetBridge.h"
#include "TurnPipeline.h"

namespace MundaneMatters::Ledger
{
	namespace
	{
		constexpr std::uint32_t kUniqueID = 'MNDM';
		constexpr std::uint32_t kRecord = 'LDGR';
		constexpr std::uint32_t kVersion = 1;
		constexpr float kKeepDistance = 400.0f;  // NPC must be this close (about 6 m) to hand over a "later" promise
		constexpr float kExpireHours = 72.0f;     // a pending promise lapses this long after it was due

		struct BedLoan
		{
			RE::FormID bed = 0;
			RE::FormID npc = 0;
			RE::FormID originalOwner = 0;  // 0 = the bed had no owner of its own
			float until = 0.0f;
			bool guest = false;            // lasts while the player is in that home
			// Who sleeps where, in a home with one bed (what was said, else by how close they are):
			// "player_floor" (the loan is a bedroll of the player's; the host keeps their bed),
			// "npc_floor" (the host's bed is the player's; the host takes a bedroll), "share".
			std::string how;
			RE::FormID roll = 0;          // the bedroll laid for this stay, removed when it ends
			RE::FormID hostBed = 0;       // player_floor: the host's own bed (given back to them)
			RE::FormID hostBedOwner = 0;  // and its owner before the loan
			RE::FormID hostSpot = 0;      // where the host lies down when the player goes to sleep
			bool hostDown = false;        // not saved: the host was sent to lie down
			bool needRoll = false;        // the bedroll is still to be laid (where the player stands, near the bed)
			bool held = false;            // the player packed it up: laid again only when they say where
		};

		struct Pending
		{
			RE::FormID npc = 0;
			std::string npcName;
			Promises::Promise promise;
			float due = 0.0f;
			float expires = 0.0f;
		};

		struct Handout
		{
			float from = 0.0f;  // game hours the current budget window opened
			std::int32_t spent = 0;
			std::string what;   // the last thing given
		};

		std::mutex g_lock;
		std::vector<BedLoan> g_beds;
		std::vector<Pending> g_pending;
		std::unordered_map<RE::FormID, Handout> g_handouts;
		std::unordered_map<RE::FormID, float> g_guests;  // host -> game hours the welcome was given
		std::unordered_map<RE::FormID, std::string> g_arrange;  // host -> who sleeps where, as agreed
		std::vector<RE::FormID> g_relay;  // loans (by bed) whose bedroll was laid the old way: lay it again
		std::set<std::string> g_grantsDone;  // MundaneMatters_grants.json entries already applied in this save

		struct ItemLoan
		{
			RE::FormID ref = 0;
			RE::FormID originalOwner = 0;
			float until = 0.0f;
			RE::FormID npc = 0;  // who gave the permission
		};
		std::vector<ItemLoan> g_items;

		struct Warmth
		{
			float windowFrom = -1.0f;  // game hours of the gift that opened the current window
			bool warmed = false;       // the one-step raise is spent
		};
		std::unordered_map<RE::FormID, Warmth> g_warmth;
		constexpr float kWarmthWindow = 24.0f * 7;
		RE::TESObjectCELL* g_lastBedCheckCell = nullptr;
		std::chrono::steady_clock::time_point g_lastBedScan{};

		// Game hours until the moment the NPC named, from the words they used; nullopt when the
		// words give no clear time ("soon", "after the job"), then the classifier's estimate decides.
		std::optional<float> DueFromHint(std::string hint)
		{
			hint = JsonUtil::Lower(std::move(hint));
			if (hint.empty()) {
				return std::nullopt;
			}
			const float h = Game::HourOfDay();
			auto has = [&](const char* s) { return hint.find(s) != std::string::npos; };
			// Next time the clock reaches `hour`, but never less than an hour away.
			auto next = [h](float hour) { return h + 1.0f < hour ? hour - h : (24.0f - h) + hour; };
			const bool tomorrow = has("tomorrow") || has("morrow");
			auto at = [&](float hour) { return tomorrow ? (24.0f - h) + hour : next(hour); };

			// Tamrielic weekdays: the next one, around mid-morning.
			static const std::array<const char*, 7> kDays{ "sundas", "morndas", "tirdas", "middas", "turdas", "fredas", "loredas" };
			for (std::size_t d = 0; d < kDays.size(); ++d) {
				if (has(kDays[d])) {
					const auto today = static_cast<std::size_t>(RE::Calendar::GetSingleton()->GetDayOfWeek());
					auto ahead = static_cast<int>((d + 7 - today % 7) % 7);
					if (ahead == 0) {
						ahead = 7;
					}
					return ahead * 24.0f - h + 9.0f;
				}
			}
			// "in two hours", "in an hour", "a few days"
			if (auto pos = hint.find("hour"); pos != std::string::npos) {
				return (std::max)(1.0f, static_cast<float>(JsonUtil::ParseNumber(hint.substr(0, pos)).value_or(1.0)));
			}
			if (auto pos = hint.find("day"); pos != std::string::npos && !has("midday") && !has("today") && !has("daybreak")) {
				return 24.0f * (std::max)(1.0f, static_cast<float>(JsonUtil::ParseNumber(hint.substr(0, pos)).value_or(1.0)));
			}
			if (has("fortnight")) {
				return 24.0f * 14;
			}
			if (has("week")) {
				return 24.0f * 7;
			}
			if (has("midnight")) return at(0.0f);
			if (has("dawn") || has("sunrise") || has("first light") || has("daybreak") || has("sun's up") || has("sun is up")) return at(6.0f);
			if (has("breakfast")) return at(7.0f);
			if (has("morning")) return at(8.0f);
			if (has("afternoon")) return at(15.0f);  // before "noon": "afternoon" contains it
			if (has("noon") || has("midday") || has("lunch")) return at(12.0f);
			if (has("dusk") || has("sunset") || has("sundown")) return at(19.0f);
			if (has("evening") || has("supper") || has("dinner") || has("shift") || has("after work") || has("closing")) return at(18.0f);
			if (has("tonight") || has("night")) return h < 20.0f ? 20.0f - h : 1.0f;
			if (tomorrow) return (24.0f - h) + 9.0f;
			return std::nullopt;
		}

		// Papyrus owner setters: the engine path, so the change is recorded in the save.
		void SetBedOwner(RE::TESObjectREFR* bed, RE::TESForm* owner)
		{
			// At once in the engine (the very next activation sees it), and through Papyrus too so the
			// change is recorded in the save.
			bed->SetOwner(owner);
			if (auto* faction = owner ? owner->As<RE::TESFaction>() : nullptr) {
				Papyrus::CallMethod(bed, "ObjectReference", "SetFactionOwner", nullptr, static_cast<RE::TESFaction*>(faction));
			} else {
				Papyrus::CallMethod(bed, "ObjectReference", "SetActorOwner", nullptr, owner ? owner->As<RE::TESNPC>() : static_cast<RE::TESNPC*>(nullptr));
			}
		}

		bool PlayerAsleep()
		{
			return Game::Player()->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping;
		}

		// Permissions not used in time: the object, still lying there, is the owner's again.
		void TickItems(float now)
		{
			std::vector<ItemLoan> due;
			{
				std::scoped_lock l(g_lock);
				std::erase_if(g_items, [&](const ItemLoan& i) {
					if (now < i.until) {
						return false;
					}
					due.push_back(i);
					return true;
				});
			}
			for (auto& i : due) {
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(i.ref);
				// Taken (in someone's pack now) or gone: nothing to hand back.
				if (!ref || ref->IsDeleted() || ref->IsDisabled() || !ref->GetParentCell()) {
					continue;
				}
				SetBedOwner(ref, i.originalOwner ? RE::TESForm::LookupByID(i.originalOwner) : nullptr);
				logger::info("permission to take {:08X} lapsed; owner restored", i.ref);
			}
		}

		// ---- who sleeps where (a home with one bed) ----

		int BedsInCell(RE::TESObjectCELL* cell)
		{
			int count = 0;
			if (cell) {
				cell->ForEachReference([&](RE::TESObjectREFR* ref) {
					auto* base = ref ? ref->GetBaseObject() : nullptr;
					auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
					if (furn && furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) && !ref->IsDisabled()) {
						++count;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			return count;
		}

		bool IsDoubleBed(RE::TESObjectREFR* bed)
		{
			auto* base = bed ? bed->GetBaseObject() : nullptr;
			auto* model = base ? base->As<RE::TESModel>() : nullptr;
			std::string m = model && model->GetModel() ? model->GetModel() : "";
			std::ranges::transform(m, m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return m.find("double") != std::string::npos;
		}

		// Bedrolls in this cell that belong to nobody: not a stay's, not a housemate's (move-in), not Rest's,
		// and nobody lying in them. Oldest first.
		std::vector<RE::TESObjectREFR*> UnclaimedBedrolls(RE::TESObjectCELL* cell)
		{
			std::unordered_set<RE::FormID> inUse;
			{
				std::scoped_lock l(g_lock);
				for (auto& b : g_beds) {
					inUse.insert(b.roll);
				}
			}
			std::unordered_set<RE::TESObjectREFR*> lainIn;
			auto* player = Game::Player();
			if (auto f = player->GetOccupiedFurniture().get()) {
				lainIn.insert(f.get());
			}
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					if (auto a = h.get()) {
						if (auto f = a->GetOccupiedFurniture().get()) {
							lainIn.insert(f.get());
						}
					}
				}
			}
			std::vector<RE::TESObjectREFR*> out;
			for (auto* r : Rest::BedrollsIn(cell)) {
				if (!inUse.contains(r->GetFormID()) && !MoveIn::IsHousemateBedroll(r->GetFormID()) && !lainIn.contains(r)) {
					out.push_back(r);
				}
			}
			return out;
		}

		// Laid for a stay in a host's home, belonging to nobody now (left from an earlier visit, a save that
		// lost track of it): taken up again, never a second one beside it.
		RE::TESObjectREFR* LeftoverBedroll(RE::TESObjectCELL* cell)
		{
			auto list = UnclaimedBedrolls(cell);
			return list.empty() ? nullptr : list.front();
		}

		// A bedroll where the player stands: floor they're on is open floor (a spot worked out from the bed
		// can land behind a wall or in the fire pit). `reuse`: one already lying in this home is used instead.
		RE::TESObjectREFR* BedrollHere(RE::TESForm* owner, bool reuse = true)
		{
			auto* player = Game::Player();
			if (auto* old = reuse ? LeftoverBedroll(player->GetParentCell()) : nullptr) {
				SetBedOwner(old, owner);
				logger::info("sleeping: bedroll {:08X} already lies here; used again, no new one", old->GetFormID());
				return old;
			}
			auto* roll = Rest::LayBedroll(player->GetPosition(), player->GetAngleZ());
			if (roll) {
				SetBedOwner(roll, owner);
			}
			return roll;
		}

		// Game thread. The arrangement for this host's loan, laid out now (or changed: the old one undone).
		void Arrange(RE::FormID npcId)
		{
			std::optional<BedLoan> found;
			std::string agreed;
			{
				std::scoped_lock l(g_lock);
				if (auto it = std::ranges::find_if(g_beds, [npcId](const BedLoan& b) { return b.npc == npcId; }); it != g_beds.end()) {
					found = *it;
				}
				if (auto a = g_arrange.find(npcId); a != g_arrange.end()) {
					agreed = a->second;
				}
			}
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!found || !npc) {
				return;
			}
			BedLoan b = *found;
			const RE::FormID key = b.bed;
			auto* player = Game::Player();
			auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.roll && b.bed == b.roll ? b.hostBed : b.bed);
			if (!bed) {
				return;
			}
			// Nothing said: only a home with one bed needs it (an acquaintance gets the floor, a friend the bed).
			std::string how = agreed;
			if (how.empty() && BedsInCell(bed->GetParentCell()) - (b.roll ? 1 : 0) <= 1) {
				how = Services::IsCompanion(npc) || Promises::IsClose(npc) ? "npc_floor" : "player_floor";
			}
			if (how.empty() || how == b.how) {
				return;
			}
			// Undo what's laid out now: the host's bed back to the player, the bedroll gone.
			if (b.hostDown) {
				Rest::GetUp(npc);
				b.hostDown = false;
			}
			if (b.roll && b.bed == b.roll && b.hostBed) {
				SetBedOwner(bed, player->GetActorBase());
				b.bed = b.hostBed;
				b.originalOwner = b.hostBedOwner;
			}
			if (b.roll) {
				Rest::PackBedroll(b.roll);
			}
			b.roll = b.hostBed = b.hostBedOwner = b.hostSpot = 0;
			b.how.clear();
			b.needRoll = false;

			// The bedroll itself is laid once the player is in the room by the bed (TickHosts).
			b.how = how;
			if (how == "share" && IsDoubleBed(bed)) {
				b.hostSpot = bed->GetFormID();
				Game::Notify(std::format("You'll share {}'s bed.", npc->GetName()));
			} else {
				b.needRoll = true;
			}
			const std::string name = npc->GetName();
			logger::info("sleeping at {}'s: {} (bed {:08X}{}){}", name, b.how, b.bed, b.needRoll ? ", bedroll once the player is by the bed" : "",
				agreed.empty() ? " - not said; by how close they are" : "");
			(void)player;
			std::scoped_lock l(g_lock);
			if (auto it = std::ranges::find_if(g_beds, [key](const BedLoan& x) { return x.bed == key; }); it != g_beds.end()) {
				*it = b;
			}
		}

		// The player lies down in their place at the host's: the host goes to theirs; up again after.
		// Game thread. The bedroll for this loan, laid where the player stands, and who sleeps where.
		void LayRoll(BedLoan& b, RE::Actor* npc)
		{
			auto* player = Game::Player();
			auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.bed);
			if (!bed) {
				return;
			}
			const std::string name = npc->GetName();
			if (b.how == "player_floor") {
				auto* roll = BedrollHere(player->GetActorBase());
				if (!roll) {
					return;
				}
				SetBedOwner(bed, b.originalOwner ? RE::TESForm::LookupByID(b.originalOwner) : nullptr);
				b.hostBed = bed->GetFormID();
				b.hostBedOwner = b.originalOwner;
				b.bed = b.roll = roll->GetFormID();
				b.originalOwner = 0;
				b.hostSpot = b.hostBed;
				Game::Notify(std::format("{} made up a bedroll for you on the floor.", name));
			} else {
				auto* roll = BedrollHere(npc->GetActorBase());
				if (!roll) {
					return;
				}
				b.roll = b.hostSpot = roll->GetFormID();
				Game::Notify(b.how == "share" ? std::format("{}'s bed is too narrow for two: they'll lie on a bedroll right beside you.", name)
				                              : std::format("{} gave you their bed and will sleep on a bedroll.", name));
			}
			b.needRoll = false;
			b.held = false;
			logger::info("sleeping at {}'s: bedroll {:08X} laid where the player stood ({})", name, b.roll, b.how);
		}

		// A bedroll left in a host's home beside the one our stay laid (one the save lost track of): the
		// stay keeps the older, the other is packed away. Only bedrolls that belong to nobody are ever packed:
		// another stay's, a housemate's (move-in), Rest's, or one someone lies in are left alone.
		void TidyBedrolls()
		{
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			if (!cell || !cell->IsInteriorCell() || player->GetOccupiedFurniture().get()) {
				return;
			}
			auto spare = UnclaimedBedrolls(cell);
			if (spare.empty()) {
				return;
			}
			// A stay of ours with its bedroll in this home, nobody lying in it.
			std::optional<BedLoan> loan;
			{
				std::scoped_lock l(g_lock);
				for (auto& b : g_beds) {
					auto* r = b.roll ? RE::TESForm::LookupByID<RE::TESObjectREFR>(b.roll) : nullptr;
					if (r && r->GetParentCell() == cell) {
						loan = b;
						break;
					}
				}
			}
			if (!loan) {
				return;  // no stay of ours here: not ours to tidy
			}
			auto* mine = RE::TESForm::LookupByID<RE::TESObjectREFR>(loan->roll);
			if (auto* host = RE::TESForm::LookupByID<RE::Actor>(loan->npc); host && host->GetOccupiedFurniture().get().get() == mine) {
				return;
			}
			// The older one stays (it was there first): the stay moves onto it.
			if (spare.front()->GetFormID() < loan->roll) {
				auto* keep = spare.front();
				const auto oldRoll = loan->roll;
				SetBedOwner(keep, mine ? mine->GetOwner() : nullptr);
				{
					std::scoped_lock l(g_lock);
					if (auto it = std::ranges::find_if(g_beds, [&](const BedLoan& x) { return x.roll == oldRoll; }); it != g_beds.end()) {
						for (auto* f : { &it->bed, &it->roll, &it->hostSpot }) {
							if (*f == oldRoll) {
								*f = keep->GetFormID();
							}
						}
					}
				}
				spare.erase(spare.begin());
				spare.push_back(mine);
				logger::info("sleeping: bedroll {:08X} was here first; the stay uses it", keep->GetFormID());
			}
			for (auto* r : spare) {
				if (r) {
					logger::info("sleeping: a second bedroll {:08X} in this home; packed away", r->GetFormID());
					Rest::PackBedroll(r->GetFormID());
				}
			}
		}

		void TickHosts()
		{
			auto* player = Game::Player();
			TidyBedrolls();
			// Old bedrolls: packed away, the host's bed back as it was, and laid again where the player stands.
			std::vector<RE::FormID> relay;
			{
				std::scoped_lock l(g_lock);
				relay.swap(g_relay);
			}
			for (auto key : relay) {
				std::optional<BedLoan> found;
				{
					std::scoped_lock l(g_lock);
					if (auto it = std::ranges::find_if(g_beds, [key](const BedLoan& x) { return x.bed == key; }); it != g_beds.end()) {
						found = *it;
					}
				}
				if (!found || !found->roll) {
					continue;
				}
				BedLoan b = *found;
				if (b.bed == b.roll && b.hostBed) {
					if (auto* hostBed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.hostBed)) {
						SetBedOwner(hostBed, player->GetActorBase());
					}
					b.bed = b.hostBed;
					b.originalOwner = b.hostBedOwner;
				}
				Rest::PackBedroll(b.roll);
				b.roll = b.hostBed = b.hostBedOwner = 0;
				b.hostSpot = 0;
				b.needRoll = true;
				logger::info("sleeping: an old bedroll ({}) is laid again where the player stands", b.how);
				std::scoped_lock l(g_lock);
				if (auto it = std::ranges::find_if(g_beds, [key](const BedLoan& x) { return x.bed == key; }); it != g_beds.end()) {
					*it = b;
				}
			}
			// Bedrolls still to lay: the player in the room by the bed, standing still, not sitting.
			{
				std::vector<BedLoan> lay;
				{
					std::scoped_lock l(g_lock);
					for (auto& b : g_beds) {
						if (b.needRoll && !b.held) {
							lay.push_back(b);
						}
					}
				}
				for (auto& b : lay) {
					auto* npc = RE::TESForm::LookupByID<RE::Actor>(b.npc);
					auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.bed);
					static std::chrono::steady_clock::time_point stillSince{};
					const auto nowReal = std::chrono::steady_clock::now();
					if (!npc || !bed || bed->GetParentCell() != player->GetParentCell() || Game::DistanceToPlayer(bed) > 300.0f || player->IsMoving() ||
						player->GetOccupiedFurniture().get() || player->IsInCombat()) {
						stillSince = {};
						continue;
					}
					// In the room by the bed and standing still a few seconds (not just passing the door).
					if (stillSince == std::chrono::steady_clock::time_point{}) {
						stillSince = nowReal;
					}
					if (nowReal - stillSince < std::chrono::seconds(3)) {
						continue;
					}
					stillSince = {};
					const RE::FormID key = b.bed;
					LayRoll(b, npc);
					std::scoped_lock l(g_lock);
					if (auto it = std::ranges::find_if(g_beds, [key](const BedLoan& x) { return x.bed == key; }); it != g_beds.end()) {
						*it = b;
					}
				}
			}
			std::vector<BedLoan> loans;
			{
				std::scoped_lock l(g_lock);
				for (auto& b : g_beds) {
					if (b.hostSpot) {
						loans.push_back(b);
					}
				}
			}
			auto occupied = player->GetOccupiedFurniture().get();
			for (auto& b : loans) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(b.npc);
				auto* spot = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.hostSpot);
				if (!npc || !spot || npc->IsDead() || !npc->Is3DLoaded() || npc->IsInCombat() || npc->GetParentCell() != player->GetParentCell()) {
					continue;
				}
				const bool inBed = occupied && occupied->GetFormID() == b.bed;
				if (inBed == b.hostDown) {
					continue;
				}
				if (inBed) {
					Rest::LieDown(npc, spot);
					logger::info("sleeping at {}'s: the player lies down; {} goes to bed too", npc->GetName(), npc->GetName());
				} else {
					Rest::GetUp(npc);
				}
				std::scoped_lock l(g_lock);
				if (auto it = std::ranges::find_if(g_beds, [&](const BedLoan& x) { return x.bed == b.bed; }); it != g_beds.end()) {
					it->hostDown = inBed;
				}
			}
		}

		void TickBeds(float now)
		{
			std::vector<BedLoan> due;
			{
				std::scoped_lock l(g_lock);
				if (PlayerAsleep()) {
					return;  // never pull the bed out from under the player
				}
				std::erase_if(g_beds, [&](const BedLoan& b) {
					// A host who died or turned on the player takes the bed back at once.
					auto* npc = RE::TESForm::LookupByID<RE::Actor>(b.npc);
					const bool hostGone = !npc || npc->IsDead() || npc->IsHostileToActor(Game::Player());
					if (b.guest) {
						// A guest's bed is theirs while they're in that home.
						auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.bed);
						const bool left = !bed || bed->GetParentCell() != Game::Player()->GetParentCell();
						if (!left && !hostGone) {
							return false;
						}
					} else if (now < b.until && !hostGone) {
						return false;
					}
					due.push_back(b);
					return true;
				});
			}
			for (auto& b : due) {
				if (b.hostDown) {
					Rest::GetUp(RE::TESForm::LookupByID<RE::Actor>(b.npc));
				}
				auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.bed);
				if (bed) {
					SetBedOwner(bed, b.originalOwner ? RE::TESForm::LookupByID(b.originalOwner) : nullptr);
					logger::info("bed loan over: bed {:08X} back to owner {:08X}", b.bed, b.originalOwner);
				}
				// A stay that ran its time (not a guest leaving): say so, rather than the bed quietly going.
				if (!b.guest) {
					if (auto* host = RE::TESForm::LookupByID<RE::Actor>(b.npc)) {
						Game::Notify(std::format("Your stay at {}'s has run its course.", host->GetName()));
					}
				}
				if (b.roll) {
					Rest::PackBedroll(b.roll);
				}
			}
			// A bed on loan that isn't the player's in the game (an older save, something else setting it
			// back): the player's again, every half minute at most, logged so a fight shows up.
			static std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> lastFix;
			std::vector<RE::FormID> lent;
			{
				std::scoped_lock l(g_lock);
				for (auto& b : g_beds) {
					if (!b.held) {
						lent.push_back(b.bed);
					}
				}
			}
			auto* playerBase = Game::Player()->GetActorBase();
			for (auto id : lent) {
				auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!bed || bed->GetParentCell() != Game::Player()->GetParentCell() || bed->GetOwner() == playerBase) {
					continue;
				}
				auto& at = lastFix[id];
				if (std::chrono::steady_clock::now() - at < 30s) {
					continue;
				}
				at = std::chrono::steady_clock::now();
				auto* was = bed->GetOwner();
				SetBedOwner(bed, playerBase);
				logger::warn("bed {:08X} is on loan but was owned by {}; the player's again (now {})", id, was && was->GetName() ? was->GetName() : "nobody",
					bed->GetOwner() == playerBase ? "ok" : "STILL NOT");
			}
		}

		// Beds left the player's by older versions (a loan given back to the player): on entering a
		// building, a bed owned by the player and not on loan now goes back to the building's owner.
		// Not in an inn (a room rented there is the player's for the night, by the game's own doing).
		RE::TESObjectCELL* g_lastRepairCell = nullptr;

		void RepairBeds(RE::TESObjectCELL* cell)
		{
			if (!cell || !cell->IsInteriorCell()) {
				return;
			}
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					auto a = h.get();
					if (a && a->GetParentCell() == cell && Services::RoomOf(a.get())) {
						return;  // an inn
					}
				}
			}
			auto* playerBase = Game::Player()->GetActorBase();
			std::unordered_set<RE::FormID> lent;
			{
				std::scoped_lock l(g_lock);
				for (auto& b : g_beds) {
					lent.insert(b.bed);
				}
			}
			std::vector<RE::TESObjectREFR*> fix;
			cell->ForEachReference([&](RE::TESObjectREFR* r) {
				auto* furn = r && r->GetBaseObject() ? r->GetBaseObject()->As<RE::TESFurniture>() : nullptr;
				if (furn && furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) && !lent.contains(r->GetFormID())) {
					auto* x = r->extraList.GetByType<RE::ExtraOwnership>();
					if (x && x->owner && x->owner == playerBase) {
						fix.push_back(r);
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (auto* r : fix) {
				SetBedOwner(r, nullptr);
				logger::info("bed {:08X} in {} was still the player's from an old loan; back to the building's owner", r->GetFormID(),
					cell->GetName() ? cell->GetName() : "?");
			}
		}

		// Always-welcome homes: on each visit, a bed there is the player's until they leave.
		void TickGuests()
		{
			std::vector<RE::FormID> hosts;
			{
				std::scoped_lock l(g_lock);
				for (auto& [npc, since] : g_guests) {
					hosts.push_back(npc);
				}
			}
			auto* cell = Game::Player()->GetParentCell();
			if (!cell || !cell->IsInteriorCell()) {
				return;
			}
			for (auto id : hosts) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!npc || npc->IsDead()) {
					RemoveGuest(id);
					continue;
				}
				if (npc->IsHostileToActor(Game::Player()) || HasBedLoan(id)) {
					continue;
				}
				if (auto* bed = Promises::FindBedFor(npc); bed && bed->GetParentCell() == cell) {
					LoanBed(npc, bed, 0.0f, true);
					// Just invited ("you're always welcome"): that notice said it already.
					float since = -100.0f;
					{
						std::scoped_lock l(g_lock);
						if (auto it = g_guests.find(id); it != g_guests.end()) {
							since = it->second;
						}
					}
					if (Game::Hours() - since > 0.5f) {
						Game::Notify(std::format("You're welcome to sleep at {}'s.", npc->GetName()));
					}
				}
			}
		}

		void TickPending(float now)
		{
			std::vector<Pending> work;
			{
				std::scoped_lock l(g_lock);
				work = g_pending;
			}
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			// Bed scans: on entering a new cell (their home, maybe), and every 30 s in case the
			// player was already inside when the promise came due.
			const auto realNow = std::chrono::steady_clock::now();
			const bool scanBeds = cell != g_lastBedCheckCell || realNow - g_lastBedScan > 30s;
			g_lastBedCheckCell = cell;
			if (scanBeds) {
				g_lastBedScan = realNow;
			}

			if (scanBeds) {
				TickGuests();
			}
			if (cell != g_lastRepairCell) {
				g_lastRepairCell = cell;
				RepairBeds(cell);
			}

			std::vector<std::size_t> done;
			for (std::size_t i = 0; i < work.size(); ++i) {
				auto& p = work[i];
				if (now > p.expires) {
					logger::info("promise lapsed: {} -> {}", p.npcName, Promises::Describe(p.promise));
					// An unused bed offer just lapses; an unkept gift is worth remembering.
					if (p.promise.type != "offer_bed") {
						SkyrimNet::AddMemory(p.npc, std::format("{} never got to give {} {} as promised.", p.npcName, player->GetName(), Promises::Describe(p.promise)), 0.3f);
					}
					done.push_back(i);
					continue;
				}
				if (now < p.due) {
					continue;
				}
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(p.npc);
				if (!npc || npc->IsDead()) {
					done.push_back(i);
					continue;
				}
				if (p.promise.type == "offer_bed") {
					if (!scanBeds) {
						continue;
					}
				} else if (!npc->Is3DLoaded() || Game::DistanceToPlayer(npc) > kKeepDistance || npc->IsInCombat() || player->IsInCombat()) {
					continue;
				} else if (auto* st = npc->AsActorState(); st && st->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping) {
					continue;  // asleep: when they're up and about
				}
				const auto cost = Promises::HandoutCost(npc, p.promise);
				auto outcome = Promises::Keep(npc, p.promise);
				if (outcome == Promises::Outcome::kKept && p.promise.type != "offer_bed") {
					// Owed, so never refused; it still counts against what they'll spare today.
					RecordHandout(p.npc, Promises::Describe(p.promise), cost, Promises::GenerosityOf(npc).hours);
					// Said long ago: the NPC says so as they hand it over now, not as if it were old news.
					Game::Narrate(std::format("{} hands {} {} right now, keeping the promise made earlier.", p.npcName, player->GetName(), Promises::Describe(p.promise)), npc);
				}
				if (outcome != Promises::Outcome::kNotYet) {
					logger::info("pending promise {} -> {}: {}", p.npcName, Promises::Describe(p.promise), outcome == Promises::Outcome::kKept ? "kept" : "impossible");
					done.push_back(i);
				}
			}
			if (done.empty()) {
				return;
			}
			std::scoped_lock l(g_lock);
			// Match by identity: AddPending may have appended entries meanwhile.
			for (auto i : done) {
				auto& w = work[i];
				std::erase_if(g_pending, [&](const Pending& p) {
					return p.npc == w.npc && p.due == w.due && p.promise.type == w.promise.type && p.promise.item == w.promise.item;
				});
			}
		}

		void Tick()
		{
			auto* player = Game::Player();
			if (!player || !player->Is3DLoaded()) {
				return;
			}
			const float now = Game::Hours();
			Services::TickWalking();
			Promises::SampleCrosshair();
			TickItems(now);
			TickHosts();
			TickBeds(now);
			TickPending(now);
			Rest::Tick();
			Safekeeping::Tick();
			Keys::Tick();
			Trust::Tick();
			Bonds::Tick();
			Loans::Tick();
			Moments::Tick();
			Keepsakes::Tick();
			Pledges::Tick();
			MoveIn::Tick();
			Bio::RefreshNearby();
			TurnPipeline::PollSelfTest();
		}

		void StartTicker()
		{
			std::thread([] {
				for (;;) {
					std::this_thread::sleep_for(2s);
					SKSE::GetTaskInterface()->AddTask(Tick);
				}
			}).detach();
		}

		// ---- co-save ----

		json Serialize()
		{
			std::scoped_lock l(g_lock);
			json j{ { "beds", json::array() }, { "pending", json::array() } };
			for (auto& b : g_beds) {
				j["beds"].push_back({ { "bed", b.bed }, { "npc", b.npc }, { "owner", b.originalOwner }, { "until", b.until }, { "guest", b.guest }, { "how", b.how },
					{ "roll", b.roll }, { "hostBed", b.hostBed }, { "hostBedOwner", b.hostBedOwner }, { "hostSpot", b.hostSpot }, { "needRoll", b.needRoll },
					{ "held", b.held }, { "rollV", 3 } });
			}
			for (auto& p : g_pending) {
				j["pending"].push_back({ { "npc", p.npc }, { "name", p.npcName }, { "promise", Promises::ToJson(p.promise) }, { "due", p.due }, { "expires", p.expires } });
			}
			j["items"] = json::array();
			for (auto& i : g_items) {
				j["items"].push_back({ { "ref", i.ref }, { "owner", i.originalOwner }, { "until", i.until }, { "npc", i.npc } });
			}
			j["guests"] = json::array();
			for (auto& [npc, since] : g_guests) {
				j["guests"].push_back({ { "npc", npc }, { "since", since } });
			}
			j["guestsRechecked"] = true;
			j["arrange"] = json::array();
			for (auto& [npc, how] : g_arrange) {
				j["arrange"].push_back({ { "npc", npc }, { "how", how } });
			}
			j["grants"] = g_grantsDone;
			j["warmth"] = json::array();
			for (auto& [npc, w] : g_warmth) {
				j["warmth"].push_back({ { "npc", npc }, { "from", w.windowFrom }, { "warmed", w.warmed } });
			}
			j["rest"] = Rest::Save();
			j["kept"] = Safekeeping::Save();
			j["keys"] = Keys::Save();
			j["trust"] = Trust::Save();
			j["bonds"] = Bonds::Save();
			j["loans"] = Loans::Save();
			j["keepsakes"] = Keepsakes::Save();
			j["pledges"] = Pledges::Save();
			j["walking"] = Services::SaveWalking();
			j["brawls"] = Brawls::Save();
			j["movein"] = MoveIn::Save();
			j["handouts"] = json::array();
			for (auto& [npc, h] : g_handouts) {
				j["handouts"].push_back({ { "npc", npc }, { "from", h.from }, { "spent", h.spent }, { "what", h.what } });
			}
			return j;
		}

		RE::FormID Resolve(const SKSE::SerializationInterface* intfc, RE::FormID id)
		{
			if (id == 0) {
				return 0;
			}
			RE::FormID out = 0;
			return intfc->ResolveFormID(id, out) ? out : 0;
		}

		std::atomic<std::int64_t> g_saveStarted{ 0 };  // steady-clock ms
		std::atomic<std::int64_t> g_saveDone{ 0 };

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void OnSave(SKSE::SerializationInterface* intfc)
		{
			struct Done
			{
				~Done() { g_saveDone = NowMs(); }
			} done;
			auto data = Serialize().dump();
			if (!intfc->OpenRecord(kRecord, kVersion) || !intfc->WriteRecordData(static_cast<std::uint32_t>(data.size())) ||
				!intfc->WriteRecordData(data.data(), static_cast<std::uint32_t>(data.size()))) {
				logger::error("co-save: could not write ledger");
			}
		}

		void OnLoad(SKSE::SerializationInterface* intfc)
		{
			std::uint32_t type, version, length;
			while (intfc->GetNextRecordInfo(type, version, length)) {
				if (type != kRecord || version != kVersion) {
					continue;
				}
				std::uint32_t size = 0;
				intfc->ReadRecordData(size);
				std::string data(size, '\0');
				intfc->ReadRecordData(data.data(), size);
				auto j = json::parse(data, nullptr, false);
				if (j.is_discarded()) {
					logger::error("co-save: ledger unreadable");
					continue;
				}

				std::scoped_lock l(g_lock);
				g_beds.clear();
				g_pending.clear();
				g_items.clear();
				const json items = j.contains("items") && j["items"].is_array() ? j["items"] : json::array();
				for (auto& ij : items) {
					auto id = [&](const char* k) { return Resolve(intfc, static_cast<RE::FormID>(JsonUtil::Num(ij, k).value_or(0))); };
					if (auto ref = id("ref")) {
						g_items.push_back({ ref, id("owner"), static_cast<float>(JsonUtil::Num(ij, "until").value_or(0)), id("npc") });
					}
				}
				g_grantsDone.clear();
				if (j.contains("grants") && j["grants"].is_array()) {
					for (auto& g : j["grants"]) {
						if (g.is_string()) {
							g_grantsDone.insert(g.get<std::string>());
						}
					}
				}
				g_arrange.clear();
				const json arrange = j.contains("arrange") && j["arrange"].is_array() ? j["arrange"] : json::array();
				for (auto& aj : arrange) {
					if (const auto npc = Resolve(intfc, static_cast<RE::FormID>(JsonUtil::Num(aj, "npc").value_or(0)))) {
						g_arrange[npc] = JsonUtil::Str(aj, "how");
					}
				}
				g_guests.clear();
				const json guests = j.contains("guests") && j["guests"].is_array() ? j["guests"] : json::array();
				for (auto& gj : guests) {
					const auto npc = Resolve(intfc, static_cast<RE::FormID>(JsonUtil::Num(gj, "npc").value_or(0)));
					if (npc) {
						g_guests[npc] = static_cast<float>(JsonUtil::Num(gj, "since").value_or(0));
					}
				}
				g_warmth.clear();
				const json warmth = j.contains("warmth") && j["warmth"].is_array() ? j["warmth"] : json::array();
				for (auto& w : warmth) {
					const auto npc = Resolve(intfc, static_cast<RE::FormID>(JsonUtil::Num(w, "npc").value_or(0)));
					if (npc) {
						g_warmth[npc] = { static_cast<float>(JsonUtil::Num(w, "from").value_or(-1)), w.contains("warmed") && w["warmed"].is_boolean() && w["warmed"].get<bool>() };
					}
				}
				Rest::Load(j.contains("rest") ? j["rest"] : json::object(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				Safekeeping::Load(j.contains("kept") ? j["kept"] : json::array(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				Keys::Load(j.contains("keys") ? j["keys"] : json::object(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				Trust::Load(j.contains("trust") ? j["trust"] : json::array(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				Bonds::Load(j.contains("bonds") ? j["bonds"] : json::object(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				Loans::Load(j.contains("loans") ? j["loans"] : json::object(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				Keepsakes::Load(j.contains("keepsakes") ? j["keepsakes"] : json::object(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				Pledges::Load(j.contains("pledges") ? j["pledges"] : json::array(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				Services::LoadWalking(j.contains("walking") ? j["walking"] : json::array(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				Brawls::Load(j.contains("brawls") ? j["brawls"] : json::array(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				MoveIn::Load(j.contains("movein") ? j["movein"] : json::array(), [intfc](RE::FormID id) { return Resolve(intfc, id); });
				// Saves from before the stricter "always welcome" rule: a flirt could make the player a
				// guest for good. Once, those go; whoever gave the player a key stays a host.
				if (!j.contains("guestsRechecked")) {
					std::erase_if(g_guests, [](const auto& g) {
						if (Keys::Has(g.first)) {
							return false;
						}
						logger::info("guest rights at {:08X} dropped (granted under the old rule)", g.first);
						return true;
					});
				}
				g_handouts.clear();
				const json handouts = j.contains("handouts") && j["handouts"].is_array() ? j["handouts"] : json::array();
				for (auto& h : handouts) {
					const auto npc = Resolve(intfc, static_cast<RE::FormID>(JsonUtil::Num(h, "npc").value_or(0)));
					if (npc) {
						g_handouts[npc] = { static_cast<float>(JsonUtil::Num(h, "from").value_or(JsonUtil::Num(h, "at").value_or(0))),
							static_cast<std::int32_t>(JsonUtil::Num(h, "spent").value_or(10)), JsonUtil::Str(h, "what") };
					}
				}
				const json beds = j.contains("beds") && j["beds"].is_array() ? j["beds"] : json::array();
				const json pending = j.contains("pending") && j["pending"].is_array() ? j["pending"] : json::array();
				for (auto& b : beds) {
					auto id = [&](const char* k) { return Resolve(intfc, static_cast<RE::FormID>(JsonUtil::Num(b, k).value_or(0))); };
					BedLoan loan{ id("bed"), id("npc"), id("owner"), static_cast<float>(JsonUtil::Num(b, "until").value_or(0)),
						b.contains("guest") && b["guest"].is_boolean() && b["guest"].get<bool>() };
					loan.how = JsonUtil::Str(b, "how");
					loan.roll = id("roll");
					loan.hostBed = id("hostBed");
					loan.hostBedOwner = id("hostBedOwner");
					loan.hostSpot = id("hostSpot");
					loan.needRoll = b.contains("needRoll") && b["needRoll"].is_boolean() && b["needRoll"].get<bool>();
					loan.held = b.contains("held") && b["held"].is_boolean() && b["held"].get<bool>();
					// Laid by an older version (v1 a fixed distance from the bed: through walls, into fire pits;
					// v2 an invisible lie-down marker instead of a bedroll): laid again.
					if (loan.roll && JsonUtil::Num(b, "rollV").value_or(1) < 3) {
						g_relay.push_back(loan.bed);
					}
					if (loan.bed) {
						g_beds.push_back(loan);
					}
				}
				for (auto& p : pending) {
					auto promise = Promises::FromJson(p.contains("promise") ? p["promise"] : json::object());
					auto npc = Resolve(intfc, static_cast<RE::FormID>(JsonUtil::Num(p, "npc").value_or(0)));
					if (promise && npc) {
						g_pending.push_back({ npc, JsonUtil::Str(p, "name"), *promise,
							static_cast<float>(JsonUtil::Num(p, "due").value_or(0)), static_cast<float>(JsonUtil::Num(p, "expires").value_or(0)) });
					}
				}
				logger::info("co-save: {} bed loans, {} pending promises, guest at {} home(s)", g_beds.size(), g_pending.size(), g_guests.size());
			}
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			std::scoped_lock l(g_lock);
			g_beds.clear();
			g_pending.clear();
			g_handouts.clear();
			g_warmth.clear();
			g_guests.clear();
			g_arrange.clear();
			g_relay.clear();
			g_grantsDone.clear();
			g_items.clear();
			g_lastBedCheckCell = nullptr;
			Rest::Revert();
			MoveIn::Revert();
			Safekeeping::Revert();
			Keys::Revert();
			Trust::Revert();
			Bonds::Revert();
			Loans::Revert();
			Keepsakes::Revert();
			Pledges::Revert();
			Services::RevertWalking();
			Brawls::Revert();
		}
	}

	void SaveStarted()
	{
		g_saveStarted = NowMs();
	}

	bool Saving()
	{
		const auto started = g_saveStarted.load();
		const auto done = g_saveDone.load();
		const auto now = NowMs();
		// Started and not done (give up after 30 s), or just done.
		return (started > done && now - started < 30000) || (done > 0 && now - done < 1500);
	}

	void Install()
	{
		auto* ser = SKSE::GetSerializationInterface();
		ser->SetUniqueID(kUniqueID);
		ser->SetSaveCallback(OnSave);
		ser->SetLoadCallback(OnLoad);
		ser->SetRevertCallback(OnRevert);
		StartTicker();
	}

	void LoanBed(RE::Actor* npc, RE::TESObjectREFR* bed, float hours, bool guest)
	{
		{
			// Lent already: the later end counts. A second loan would note the PLAYER as the owner to
			// give it back to, and the bed would stay theirs for good.
			std::scoped_lock l(g_lock);
			if (auto it = std::ranges::find_if(g_beds, [bed](const BedLoan& b) { return b.bed == bed->GetFormID(); }); it != g_beds.end()) {
				it->until = (std::max)(it->until, Game::Hours() + hours);
				it->guest = it->guest && guest;
				logger::info("bed {:08X} of {} already lent; the loan runs on", it->bed, npc->GetName());
				return;
			}
		}
		auto* xOwner = bed->extraList.GetByType<RE::ExtraOwnership>();
		auto* playerBase = Game::Player()->GetActorBase();
		RE::FormID original = xOwner && xOwner->owner ? xOwner->owner->GetFormID() : 0;
		if (playerBase && original == playerBase->GetFormID()) {
			original = 0;  // the player's already (an old loan given back wrong): no owner of its own, the building's then
		}
		BedLoan loan{ bed->GetFormID(), npc->GetFormID(), original, Game::Hours() + hours, guest };
		SetBedOwner(bed, Game::Player()->GetActorBase());
		logger::info("bed {:08X}: owner now {}", bed->GetFormID(), bed->GetOwner() == Game::Player()->GetActorBase() ? "the player" : "NOT the player");
		if (guest) {
			logger::info("bed {:08X} of {} is the player's for this visit (always welcome)", loan.bed, npc->GetName());
		} else {
			logger::info("bed {:08X} of {} lent to the player for {} h", loan.bed, npc->GetName(), hours);
		}
		{
			std::scoped_lock l(g_lock);
			g_beds.push_back(loan);
		}
		Arrange(npc->GetFormID());
	}

	void ApplyGrants()
	{
		std::ifstream f("Data/SKSE/Plugins/MundaneMatters_grants.json");
		if (!f) {
			return;
		}
		const auto all = json::parse(f, nullptr, false);
		if (!all.is_array()) {
			logger::warn("grants: MundaneMatters_grants.json isn't a JSON list; ignored");
			return;
		}
		for (auto& g : all) {
			const auto id = JsonUtil::Str(g, "id");
			const auto type = JsonUtil::Str(g, "type");
			if (id.empty()) {
				continue;
			}
			{
				std::scoped_lock l(g_lock);
				if (g_grantsDone.contains(id)) {
					continue;
				}
			}
			// "Plugin.esp|0x06CBC5" (any load order) or "0x1706CBC5" (this load order).
			const auto who = JsonUtil::Str(g, "npc");
			RE::FormID form = 0;
			if (auto bar = who.find('|'); bar != std::string::npos) {
				const auto local = static_cast<RE::FormID>(std::strtoul(who.substr(bar + 1).c_str(), nullptr, 16));
				form = RE::TESDataHandler::GetSingleton()->LookupFormID(local, who.substr(0, bar));
			} else {
				form = static_cast<RE::FormID>(std::strtoul(who.c_str(), nullptr, 16));
			}
			auto* npc = form ? RE::TESForm::LookupByID<RE::Actor>(form) : nullptr;
			if (!npc) {
				logger::warn("grants: '{}': no one found for '{}'", id, who);
				continue;
			}
			const std::string name = npc->GetName();
			if (type == "offer_bed") {
				// The place to sleep they offered in a conversation that wasn't caught: waiting at their home.
				Promises::Promise p;
				p.type = "offer_bed";
				p.backInHours = static_cast<float>(JsonUtil::Num(g, "hours").value_or(24.0));
				const float now = Game::Hours();
				{
					std::scoped_lock l(g_lock);
					g_pending.push_back({ npc->GetFormID(), name, p, now, now + (std::max)(p.backInHours, 24.0f) + 24.0f });
				}
				if (const auto how = JsonUtil::Str(g, "sleep"); !how.empty()) {
					std::scoped_lock l(g_lock);
					g_arrange[npc->GetFormID()] = how;
				}
				Game::Notify(std::format("{} offered you a place to sleep at their home.", name));
			} else if (type == "pledge") {
				// A promise of the player's the mod missed ("sure man, I'll get one"): opened by hand.
				Pledges::Grant(npc, JsonUtil::Str(g, "kind", "bring"), JsonUtil::Str(g, "item"), JsonUtil::Str(g, "words"),
					static_cast<float>(JsonUtil::Num(g, "hours").value_or(0.0)));
			} else if (type == "guest") {
				AddGuest(npc->GetFormID());
				if (const auto how = JsonUtil::Str(g, "sleep"); !how.empty()) {
					std::scoped_lock l(g_lock);
					g_arrange[npc->GetFormID()] = how;
				}
				Game::Notify(std::format("You're always welcome at {}'s home.", name));
			} else {
				logger::warn("grants: '{}': unknown type '{}'", id, type);
				continue;
			}
			logger::info("grants: '{}' applied: {} for {} ({})", id, type, name, JsonUtil::Str(g, "note"));
			std::scoped_lock l(g_lock);
			g_grantsDone.insert(id);
		}
	}

	std::vector<Owed> TheirPromises()
	{
		std::vector<Owed> out;
		const float now = Game::Hours();
		std::scoped_lock l(g_lock);
		for (auto& p : g_pending) {
			const bool bed = p.promise.type == "offer_bed";
			if (bed && g_guests.contains(p.npc)) {
				continue;  // always welcome there: that says it
			}
			out.push_back({ p.npcName, Promises::Describe(p.promise), p.due - now, bed });
		}
		auto nameOf = [](RE::FormID id) {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			return std::string(a && a->GetName() ? a->GetName() : "Someone");
		};
		for (auto& b : g_beds) {
			if (b.guest || g_guests.contains(b.npc)) {
				continue;  // a guest's bed for the visit: said below as the guest rights
			}
			const char* what = b.how == "player_floor" ? "a bedroll by their hearth" : b.how == "npc_floor" ? "their own bed" :
			                   b.how == "share"        ? "a share of their bed" :
			                                             "a bed to sleep in";
			out.push_back({ nameOf(b.npc), what, b.until - now, true });
		}
		for (auto& [npc, since] : g_guests) {
			std::string note;
			if (auto a = g_arrange.find(npc); a != g_arrange.end()) {
				note = a->second == "player_floor" ? "you sleep on a bedroll by their hearth" : a->second == "npc_floor" ? "they give you their own bed" :
				       a->second == "share"        ? "you share their bed" :
				                                     "";
			}
			out.push_back({ nameOf(npc), "always welcome to stay at their home", -1.0f, true, note });
		}
		return out;
	}

	std::string StayAt(RE::FormID npc)
	{
		auto* cell = Game::Player()->GetParentCell();
		std::scoped_lock l(g_lock);
		for (auto& b : g_beds) {
			auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.bed);
			if (b.npc == npc && bed && bed->GetParentCell() == cell) {
				if (b.held) {
					return "packed";
				}
				return b.how.empty() ? "bed" : b.how;
			}
		}
		return "";
	}

	bool BedrollHereWords(const std::string& line)
	{
		const auto words = JsonUtil::Words(JsonUtil::Lower(line));
		bool roll = false, move = false;
		for (std::size_t i = 0; i < words.size(); ++i) {
			const auto& w = words[i];
			// bedroll, bedrolls, bedrool, bedrol; "bed roll"
			if ((w.starts_with("bedro") && w.find('l') != std::string::npos) ||
				(w == "bed" && i + 1 < words.size() && words[i + 1].starts_with("rol"))) {
				roll = true;
			}
			if (w == "move" || w == "put" || w == "place" || w == "lay" || w == "drag" || w == "shift" || w == "set" || w == "bring" || w == "moving" ||
				w == "moved" || w == "placed" || w == "laid" || w == "placing" || w == "dragged" || w == "unroll" || w == "unrolled") {
				move = true;
			}
		}
		return roll && move;
	}

	bool MoveGuestBedroll()
	{
		auto* player = Game::Player();
		std::optional<BedLoan> found;
		{
			std::scoped_lock l(g_lock);
			for (auto& b : g_beds) {
				if (b.how != "player_floor") {
					continue;
				}
				// The bed (or bedroll) is here: their home, whether they're in or out.
				auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.bed);
				if (bed && bed->GetParentCell() == player->GetParentCell()) {
					found = b;
					break;
				}
			}
		}
		if (!found) {
			return false;
		}
		BedLoan b = *found;
		const RE::FormID key = b.bed;
		auto* npc = RE::TESForm::LookupByID<RE::Actor>(b.npc);
		if (b.needRoll) {
			if (npc) {
				LayRoll(b, npc);  // not laid yet: right here, now
			}
		} else if (auto* roll = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.roll)) {
			if (player->GetOccupiedFurniture().get().get() == roll) {
				return true;  // lying in it already
			}
			// To the player's feet: packed up and laid again here (moving a placed bedroll in place
			// doesn't take in the game).
			auto* fresh = BedrollHere(player->GetActorBase(), false);
			if (!fresh) {
				return false;
			}
			Rest::PackBedroll(b.roll);
			logger::info("sleeping: the player moved their bedroll {:08X} -> {:08X} to where they stand", b.roll, fresh->GetFormID());
			b.bed = b.roll = fresh->GetFormID();
			Game::Notify("You moved your bedroll here.");
		} else {
			return false;
		}
		std::scoped_lock l(g_lock);
		if (auto it = std::ranges::find_if(g_beds, [key](const BedLoan& x) { return x.bed == key; }); it != g_beds.end()) {
			*it = b;
		}
		return true;
	}

	bool BedrollPickUpWords(const std::string& line)
	{
		const auto words = JsonUtil::Words(JsonUtil::Lower(line));
		std::string padded = " ";
		bool roll = false;
		for (std::size_t i = 0; i < words.size(); ++i) {
			const auto& w = words[i];
			padded += w + " ";
			if ((w.starts_with("bedro") && w.find('l') != std::string::npos) ||
				(w == "bed" && i + 1 < words.size() && words[i + 1].starts_with("rol"))) {
				roll = true;
			}
		}
		if (!roll) {
			return false;
		}
		// Said or acted ("*I picked up my bed roll*"): pick up / pack / roll up / put away, any tense.
		for (auto* p : { " pick up ", " pick it up ", " picking up ", " picked up ", " picked it up ", " pack ", " pack up ", " packing ", " packed ",
				 " roll up ", " roll it up ", " rolled up ", " rolling up ", " put away ", " put it away ", " fold ", " folded ", " take up ", " remove ",
				 " removed " }) {
			if (padded.find(p) != std::string::npos) {
				return true;
			}
		}
		// "pick my bedroll up", "I'll pick my bedroll": picking it, unless it's going somewhere ("pick a spot to put it").
		const bool somewhere = std::ranges::any_of(std::initializer_list<const char*>{ " here ", " there ", " place ", " put ", " move ", " spot " },
			[&](const char* p) { return padded.find(p) != std::string::npos; });
		return (padded.find(" pick ") != std::string::npos || padded.find(" picked ") != std::string::npos ||
				   (padded.find(" roll ") != std::string::npos && padded.ends_with(" up "))) &&
		       !somewhere;
	}

	bool PickUpGuestBedroll()
	{
		auto* player = Game::Player();
		std::optional<BedLoan> found;
		{
			std::scoped_lock l(g_lock);
			for (auto& b : g_beds) {
				auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.bed);
				if (b.how == "player_floor" && bed && bed->GetParentCell() == player->GetParentCell()) {
					found = b;
					break;
				}
			}
		}
		if (!found) {
			return false;
		}
		BedLoan b = *found;
		const RE::FormID key = b.bed;
		if (b.held) {
			return true;  // packed already
		}
		if (b.roll) {
			if (player->GetOccupiedFurniture().get().get() == RE::TESForm::LookupByID<RE::TESObjectREFR>(b.roll)) {
				return true;  // lying on it
			}
			Rest::PackBedroll(b.roll);
			b.bed = b.hostBed ? b.hostBed : b.bed;
			b.originalOwner = b.hostBedOwner;
			b.roll = b.hostBed = b.hostBedOwner = b.hostSpot = 0;
		} else if (auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.bed)) {
			// Never laid: the host's bed (lent meanwhile) goes back to them.
			SetBedOwner(bed, b.originalOwner ? RE::TESForm::LookupByID(b.originalOwner) : nullptr);
		}
		b.needRoll = true;
		b.held = true;
		Game::Notify("You rolled up your bedroll.");
		logger::info("sleeping: the player packed up their bedroll; laid again where they say");
		std::scoped_lock l(g_lock);
		if (auto it = std::ranges::find_if(g_beds, [key](const BedLoan& x) { return x.bed == key; }); it != g_beds.end()) {
			*it = b;
		}
		return true;
	}

	void AgreeSleeping(RE::FormID npc, const std::string& how)
	{
		{
			std::scoped_lock l(g_lock);
			g_arrange[npc] = how;
		}
		logger::info("sleeping: {:08X} agreed '{}'", npc, how);
		Arrange(npc);  // already lent: laid out again now
	}

	void AllowTake(RE::TESObjectREFR* ref, float hours, RE::FormID npc)
	{
		auto* xOwner = ref->extraList.GetByType<RE::ExtraOwnership>();
		const ItemLoan loan{ ref->GetFormID(), xOwner && xOwner->owner ? xOwner->owner->GetFormID() : 0, Game::Hours() + hours, npc };
		SetBedOwner(ref, Game::Player()->GetActorBase());  // the same ownership call works for any object
		std::scoped_lock l(g_lock);
		g_items.push_back(loan);
	}

	bool HasPermissions(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		return std::ranges::any_of(g_items, [npc](const ItemLoan& i) { return i.npc == npc; });
	}

	std::vector<RE::FormID> Hosts()
	{
		std::scoped_lock l(g_lock);
		std::vector<RE::FormID> out;
		for (auto& [npc, since] : g_guests) {
			out.push_back(npc);
		}
		for (auto& b : g_beds) {
			out.push_back(b.npc);
		}
		for (auto& i : g_items) {
			if (i.npc) {
				out.push_back(i.npc);
			}
		}
		return out;
	}

	std::vector<std::string> RevokeAll(RE::FormID npc)
	{
		std::vector<std::string> lost;
		std::scoped_lock l(g_lock);
		if (g_guests.erase(npc)) {
			lost.push_back("guest rights");
		}
		bool bed = false;
		for (auto& b : g_beds) {
			if (b.npc == npc) {
				b.guest = false;
				b.until = 0.0f;  // the ticker hands it back
				bed = true;
			}
		}
		if (bed) {
			lost.push_back("the bed");
		}
		bool items = false;
		for (auto& i : g_items) {
			if (i.npc == npc) {
				i.until = 0.0f;  // the object, still lying there, is theirs again
				items = true;
			}
		}
		if (items) {
			lost.push_back("permission to take things");
		}
		const auto before = g_pending.size();
		std::erase_if(g_pending, [npc](const Pending& p) { return p.npc == npc; });
		if (g_pending.size() != before) {
			lost.push_back("what they had promised");
		}
		return lost;
	}

	void AddGuest(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		g_guests.try_emplace(npc, Game::Hours());
		// Always welcome now: a bed offered for one night there is part of that, not a second thing.
		std::erase_if(g_pending, [npc](const auto& p) { return p.npc == npc && p.promise.type == "offer_bed"; });
	}

	void RemoveGuest(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		if (g_guests.erase(npc)) {
			// Any bed lent for the current visit goes back on the next tick.
			for (auto& b : g_beds) {
				if (b.npc == npc && b.guest) {
					b.guest = false;
					b.until = 0.0f;
				}
			}
			logger::info("guest rights at {:08X} ended", npc);
		}
	}

	bool IsGuest(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		return g_guests.contains(npc);
	}

	bool HasBedLoan(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		return std::ranges::any_of(g_beds, [npc](const BedLoan& b) { return b.npc == npc; });
	}

	void AddPending(RE::FormID npc, const std::string& npcName, const Promises::Promise& p)
	{
		SKSE::GetTaskInterface()->AddTask([npc, npcName, p] {
			const float now = Game::Hours();
			float in = 0.0f;
			if (p.when == "later") {
				// The NPC's own words first (deterministic), then the classifier's estimate, then "soon".
				in = DueFromHint(p.timeHint).value_or(p.inHours >= 0.5f ? p.inHours : 1.0f);
			}
			const float due = now + in;
			std::scoped_lock l(g_lock);
			// The same promise repeated in conversation is one promise.
			for (auto& existing : g_pending) {
				if (existing.npc == npc && Promises::Same(existing.promise, p)) {
					return;
				}
			}
			// A bed is offered for a night, not for three.
			g_pending.push_back({ npc, npcName, p, due, due + (p.type == "offer_bed" ? 24.0f : kExpireHours) });
			logger::info("pending promise: {} -> {} (due in {:.1f} h)", npcName, Promises::Describe(p), due - now);
			// A bed offered away from their home: say so now, so the player knows it counted.
			if (p.type == "offer_bed") {
				Game::Notify(std::format("{} offered you a place to sleep at their home.", npcName));
			}
		});
	}

	bool GiftCounts(RE::FormID npc)
	{
		const float now = Game::Hours();
		std::scoped_lock l(g_lock);
		auto& w = g_warmth[npc];
		if (w.warmed || (w.windowFrom >= 0.0f && now - w.windowFrom < kWarmthWindow)) {
			return false;
		}
		w.windowFrom = now;
		return true;
	}

	void MarkWarmed(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		g_warmth[npc].warmed = true;
	}

	std::int32_t BudgetLeft(RE::FormID npc, const Promises::Generosity& g)
	{
		if (g.never) {
			return 0;
		}
		const float now = Game::Hours();
		std::scoped_lock l(g_lock);
		auto it = g_handouts.find(npc);
		if (it == g_handouts.end() || now - it->second.from >= g.hours) {
			return g.budget;  // a fresh window
		}
		return (std::max)(0, g.budget - it->second.spent);
	}

	void RecordHandout(RE::FormID npc, const std::string& what, std::int32_t cost, float hours)
	{
		const float now = Game::Hours();
		{
			std::scoped_lock l(g_lock);
			auto& h = g_handouts[npc];
			if (h.what.empty() || now - h.from >= hours) {
				h = { now, 0, "" };  // the window opens with the first handout
			}
			h.spent += (std::max)(0, cost);
			h.what = what;
		}
		Bio::Invalidate();
	}

	std::optional<HandoutWindow> CurrentHandouts(RE::FormID npc)
	{
		const float now = Game::Hours();
		std::scoped_lock l(g_lock);
		auto it = g_handouts.find(npc);
		if (it == g_handouts.end()) {
			return std::nullopt;
		}
		return HandoutWindow{ now - it->second.from, it->second.spent, it->second.what };
	}

	bool IsPending(RE::FormID npc, const Promises::Promise& p)
	{
		std::scoped_lock l(g_lock);
		return std::ranges::any_of(g_pending, [&](const Pending& e) { return e.npc == npc && Promises::Same(e.promise, p); }) ||
		       (p.type == "offer_bed" && std::ranges::any_of(g_beds, [npc](const BedLoan& b) { return b.npc == npc; }));
	}

	void Fulfilled(RE::FormID npc, const Promises::Promise& p)
	{
		std::scoped_lock l(g_lock);
		const auto before = g_pending.size();
		std::erase_if(g_pending, [&](const Pending& e) { return e.npc == npc && Promises::Same(e.promise, p); });
		if (g_pending.size() != before) {
			logger::info("pending promise settled early: {:08X} {}", npc, Promises::Describe(p));
		}
	}

	void Cancel(RE::FormID npc, const Promises::Promise& p)
	{
		bool found = false;
		{
			std::scoped_lock l(g_lock);
			const auto before = g_pending.size();
			std::erase_if(g_pending, [&](const Pending& e) { return e.npc == npc && Promises::Same(e.promise, p); });
			found = g_pending.size() != before;
			if (p.type == "offer_bed") {
				// "You're no longer welcome here" also ends guest rights.
				found = g_guests.erase(npc) > 0 || found;
				// Taking back the bed also ends a loan already given; the ticker hands it back.
				for (auto& b : g_beds) {
					if (b.npc == npc) {
						b.until = 0.0f;
						found = true;
					}
				}
			}
		}
		if (p.type == "give_key" && Keys::Has(npc)) {
			SKSE::GetTaskInterface()->AddTask([npc] { Keys::Revoke(npc); });
			found = true;
		}
		logger::info("promise taken back: {:08X} {}{}", npc, Promises::Describe(p), found ? "" : " (nothing pending)");
	}

	std::vector<std::string> BioLines(RE::FormID npc)
	{
		std::vector<std::string> out;
		const float now = Game::Hours();
		const std::string player = Game::Player()->GetName();
		std::scoped_lock l(g_lock);
		for (auto& p : g_pending) {
			if (p.npc != npc) {
				continue;
			}
			const float in = p.due - now;
			std::string when = in > 20.0f ? "in a day or so" : in > 2.0f ? "later today" : "now, once they are together";
			if (!p.promise.timeHint.empty()) {
				when = std::format("{} (\"{}\")", when, p.promise.timeHint);
			}
			// A bed is hospitality, not a promise: offered, for when they're there.
			if (p.promise.type == "offer_bed") {
				out.push_back(std::format("Has offered {} {}, for {} (an offer of hospitality, not a promise).", player, Promises::Describe(p.promise), when));
				continue;
			}
			out.push_back(std::format("Has promised {} {}: due {}.", player, Promises::Describe(p.promise), when));
		}
		if (g_guests.contains(npc)) {
			out.push_back(std::format("{} is always welcome to stay and sleep in their home.", player));
		}
		for (auto& b : g_beds) {
			if (b.npc == npc && !b.guest) {
				out.push_back(std::format("Is letting {} sleep in their bed for now.", player));
			}
		}
		return out;
	}
}
