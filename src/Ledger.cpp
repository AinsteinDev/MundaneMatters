#include "Ledger.h"

#include "Bio.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Papyrus.h"
#include "SkyrimNetBridge.h"
#include "TurnPipeline.h"

namespace MundaneMatters::Ledger
{
	namespace
	{
		constexpr std::uint32_t kUniqueID = 'MNDM';
		constexpr std::uint32_t kRecord = 'LDGR';
		constexpr std::uint32_t kVersion = 1;
		constexpr float kKeepDistance = 1000.0f;  // NPC must be this close to hand over a "later" promise
		constexpr float kExpireHours = 72.0f;     // a pending promise lapses this long after it was due

		struct BedLoan
		{
			RE::FormID bed = 0;
			RE::FormID npc = 0;
			RE::FormID originalOwner = 0;  // 0 = the bed had no owner of its own
			float until = 0.0f;
			bool guest = false;            // lasts while the player is in that home
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

		struct ItemLoan
		{
			RE::FormID ref = 0;
			RE::FormID originalOwner = 0;
			float until = 0.0f;
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
				auto* bed = RE::TESForm::LookupByID<RE::TESObjectREFR>(b.bed);
				if (!bed) {
					continue;
				}
				SetBedOwner(bed, b.originalOwner ? RE::TESForm::LookupByID(b.originalOwner) : nullptr);
				logger::info("bed loan over: bed {:08X} back to owner {:08X}", b.bed, b.originalOwner);
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
					Game::Notify(std::format("You're welcome to sleep at {}'s.", npc->GetName()));
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
				}
				const auto cost = Promises::HandoutCost(npc, p.promise);
				auto outcome = Promises::Keep(npc, p.promise);
				if (outcome == Promises::Outcome::kKept && p.promise.type != "offer_bed") {
					// Owed, so never refused; it still counts against what they'll spare today.
					RecordHandout(p.npc, Promises::Describe(p.promise), cost, Promises::GenerosityOf(npc).hours);
					// Said long ago: let the NPC mention it as they hand it over.
					Game::Narrate(std::format("{} remembered the promise and handed {} {}.", p.npcName, player->GetName(), Promises::Describe(p.promise)), npc);
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
			Promises::SampleCrosshair();
			TickItems(now);
			TickBeds(now);
			TickPending(now);
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
				j["beds"].push_back({ { "bed", b.bed }, { "npc", b.npc }, { "owner", b.originalOwner }, { "until", b.until }, { "guest", b.guest } });
			}
			for (auto& p : g_pending) {
				j["pending"].push_back({ { "npc", p.npc }, { "name", p.npcName }, { "promise", Promises::ToJson(p.promise) }, { "due", p.due }, { "expires", p.expires } });
			}
			j["items"] = json::array();
			for (auto& i : g_items) {
				j["items"].push_back({ { "ref", i.ref }, { "owner", i.originalOwner }, { "until", i.until } });
			}
			j["guests"] = json::array();
			for (auto& [npc, since] : g_guests) {
				j["guests"].push_back({ { "npc", npc }, { "since", since } });
			}
			j["warmth"] = json::array();
			for (auto& [npc, w] : g_warmth) {
				j["warmth"].push_back({ { "npc", npc }, { "from", w.windowFrom }, { "warmed", w.warmed } });
			}
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

		void OnSave(SKSE::SerializationInterface* intfc)
		{
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
						g_items.push_back({ ref, id("owner"), static_cast<float>(JsonUtil::Num(ij, "until").value_or(0)) });
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
				logger::info("co-save: {} bed loans, {} pending promises", g_beds.size(), g_pending.size());
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
			g_items.clear();
			g_lastBedCheckCell = nullptr;
		}
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
		auto* xOwner = bed->extraList.GetByType<RE::ExtraOwnership>();
		BedLoan loan{ bed->GetFormID(), npc->GetFormID(), xOwner && xOwner->owner ? xOwner->owner->GetFormID() : 0, Game::Hours() + hours, guest };
		SetBedOwner(bed, Game::Player()->GetActorBase());
		if (guest) {
			logger::info("bed {:08X} of {} is the player's for this visit (always welcome)", loan.bed, npc->GetName());
		} else {
			logger::info("bed {:08X} of {} lent to the player for {} h", loan.bed, npc->GetName(), hours);
		}
		std::scoped_lock l(g_lock);
		g_beds.push_back(loan);
	}

	void AllowTake(RE::TESObjectREFR* ref, float hours)
	{
		auto* xOwner = ref->extraList.GetByType<RE::ExtraOwnership>();
		const ItemLoan loan{ ref->GetFormID(), xOwner && xOwner->owner ? xOwner->owner->GetFormID() : 0, Game::Hours() + hours };
		SetBedOwner(ref, Game::Player()->GetActorBase());  // the same ownership call works for any object
		std::scoped_lock l(g_lock);
		g_items.push_back(loan);
	}

	void AddGuest(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		g_guests.try_emplace(npc, Game::Hours());
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
