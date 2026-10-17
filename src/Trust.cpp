#include "Trust.h"

#include "Bonds.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Keys.h"
#include "Ledger.h"
#include "Papyrus.h"
#include "Promises.h"
#include "Rest.h"
#include "Services.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Trust
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		enum class Level
		{
			kNone,      // forgiven (or never wronged)
			kOffended,  // wronged: theft, hurtful words, a hit outside a fight; an apology can mend it
			kAttacked   // a real attack: never forgiven
		};

		struct Grudge
		{
			Level level = Level::kNone;
			std::string reason;          // "stole from their home"
			float since = 0.0f;
			bool reacted = false;        // the NPC has had their say about it
			float forgivenAt = -1.0f;    // game hours of the last forgiveness (bio memory)
			std::string forgivenReason;
		};

		constexpr float kForgivenMemory = 24.0f * 7;  // "forgave you, but..." stays in their bio a week
		constexpr float kReactRange = 1500.0f;

		std::mutex g_lock;
		std::unordered_map<RE::FormID, Grudge> g_grudges;

		// The last NPC the player hit, for pinning an assault bounty or a fight on someone.
		RE::FormID g_lastHit = 0;
		Clock::time_point g_lastHitAt;
		std::unordered_map<RE::FormID, Clock::time_point> g_prevHit;  // per NPC, to tell one stray arrow from a volley

		// Companions seen recently. A follower pushed too far turns on the player (and leaves the
		// team), which is how the game says "that was deliberate"; nobody else gets judged that way,
		// because a brawl looks like the same fight. For them, only an assault bounty counts.
		std::unordered_map<RE::FormID, Clock::time_point> g_companions;

		// Like vanilla, a theft only matters when it was seen: the owner caught the player, or the
		// game charged a bounty for it. The player's non-violent crime gold, as of the last tick,
		// tells whether a crime was just reported.
		float g_bounty = 0.0f;

		// A pickpocket attempt in progress (the pickpocket window is open): on whom, and the bounty
		// before it. A failed attempt closes the window and is reported as a crime.
		RE::FormID g_pickVictim = 0;
		float g_pickBounty = 0.0f;

		float Bounty()
		{
			float sum = 0.0f;
			for (auto& [faction, gold] : Game::Player()->GetCrimeValue().crimeGoldMap) {
				sum += gold.nonViolentCur + gold.nonViolentInfamy;
			}
			return sum;
		}

		bool WasCompanion(RE::FormID id)
		{
			auto it = g_companions.find(id);
			return it != g_companions.end() && Clock::now() - it->second < 60s;
		}

		std::string PlayerName()
		{
			return Game::Player()->GetName();
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
			default:
				return 0;
			}
		}

		bool HasRights(RE::FormID id)
		{
			return Ledger::IsGuest(id) || Ledger::HasBedLoan(id) || Keys::Has(id) || Ledger::HasPermissions(id);
		}

		void React(RE::Actor* npc, const Grudge& g)
		{
			// Caught in the act; a theft found out (a bounty: someone saw and told); a blow is felt.
			const bool caught = g.reason.find("in front of them") != std::string::npos || g.reason.find("tried to pick") != std::string::npos;
			const bool discovered = g.reason.find("stole") != std::string::npos || g.reason.find("pocket") != std::string::npos;
			Game::Narrate(caught       ? std::format("{} caught {} red-handed: {} {}, after all the trust they had shown them. They were hurt and angry.", npc->GetName(), PlayerName(), PlayerName(), g.reason) :
			              discovered
							  ? std::format("{} found out that {} {}, after all the trust they had shown them, and was hurt and angry.", npc->GetName(), PlayerName(), g.reason)
							  : std::format("{} was shocked and angry that {} {}.", npc->GetName(), PlayerName(), g.reason),
				npc);
		}

		// Game thread. Trust is broken: everything given is taken back.
		void Break(RE::Actor* npc, Level level, const std::string& reason, bool alreadyReacted)
		{
			if (!npc || npc->IsDead()) {
				return;
			}
			const auto id = npc->GetFormID();
			{
				std::scoped_lock l(g_lock);
				auto& g = g_grudges[id];
				if (g.level == Level::kAttacked || (g.level == Level::kOffended && level == Level::kOffended)) {
					return;  // already as bad as it gets for this
				}
				g.level = level;
				g.reason = reason;
				g.since = Game::Hours();
				g.reacted = alreadyReacted;
			}
			if (reason.find("cruel") == std::string::npos) {
				Bonds::Setback(id);  // theft or a blow: game events, not someone's reading of a line
			}
			auto lost = Ledger::RevokeAll(id);
			if (Keys::Has(id)) {
				Keys::Revoke(id, false);
				lost.push_back("their key");
			}
			std::string list;
			for (auto& s : lost) {
				list += (list.empty() ? "" : ", ") + s;
			}
			const std::string name = npc->GetName();
			Game::Notify(list.empty() ? std::format("{} no longer trusts you.", name) : std::format("{} no longer trusts you (lost: {}).", name, list));
			SkyrimNet::AddMemory(id,
				level == Level::kAttacked ? std::format("{} attacked {}. {} will never forgive it.", PlayerName(), name, name)
										  : std::format("{} {}. {} feels betrayed and won't do them favours until they make it up.", PlayerName(), reason, name),
				0.9f);
			logger::info("trust: {} {} ({}){}", name, level == Level::kAttacked ? "was attacked" : "was wronged", reason,
				list.empty() ? "" : std::format("; revoked {}", list));
			if (!alreadyReacted && Game::DistanceToPlayer(npc) < kReactRange && !npc->IsInCombat()) {
				React(npc, { level, reason });
				std::scoped_lock l(g_lock);
				g_grudges[id].reacted = true;
			}
		}

		// ---- theft ----

		void After(std::chrono::milliseconds wait, std::function<void()> then)
		{
			std::thread([wait, then = std::move(then)] {
				std::this_thread::sleep_for(wait);
				SKSE::GetTaskInterface()->AddTask(then);
			}).detach();
		}

		// Whose trust a theft right here would break: the people whose home this is, if they trust
		// the player. Only a theft that was seen counts (vanilla rules): the owner caught the player
		// (they could see them as it happened), or the game charged a bounty for it (someone saw and
		// reported it). An unseen grab, an accidental pickup nobody noticed, does nothing.
		void OnStolen()
		{
			auto* player = Game::Player();
			auto* cell = player->GetParentCell();
			if (!cell || !cell->IsInteriorCell()) {
				return;
			}
			std::vector<RE::FormID> ids = Ledger::Hosts();
			for (auto id : Keys::Holders()) {
				ids.push_back(id);
			}
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					if (auto a = h.get(); a && a->GetParentCell() == cell) {
						ids.push_back(a->GetFormID());
					}
				}
			}
			std::ranges::sort(ids);
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			std::vector<std::pair<RE::FormID, bool>> owners;  // whose home, and did they see it
			for (auto id : ids) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
				if (npc && !npc->IsDead() && Promises::IsHome(cell, npc) && Matters(npc)) {
					const bool saw = npc->GetParentCell() == cell && npc->RequestDetectionLevel(player) > 0;
					owners.emplace_back(id, saw);
				}
			}
			if (owners.empty()) {
				return;
			}
			// The bounty comes a moment later, once a witness reports it.
			const float before = g_bounty;
			After(3000ms, [owners, before] {
				const bool reported = Bounty() > before + 0.5f;
				for (auto& [id, saw] : owners) {
					auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
					if (!npc || npc->IsDead()) {
						continue;
					}
					if (saw) {
						Break(npc, Level::kOffended, "stole from their home, right in front of them", false);
					} else if (reported) {
						// Someone saw and told: they find out, and say so when next they meet.
						Break(npc, Level::kOffended, "stole from their home", false);
					} else {
						logger::info("trust: a theft in {}'s home went unseen; nothing comes of it", npc->GetName());
					}
				}
			});
		}

		// The pickpocket window closed: a failed attempt is reported as a crime (a bounty). A
		// successful one, by definition, went unnoticed.
		void OnPickpocketDone(RE::FormID victimId, float before)
		{
			auto* victim = RE::TESForm::LookupByID<RE::Actor>(victimId);
			if (!victim || victim->IsDead() || !Matters(victim)) {
				return;
			}
			if (Bounty() > before + 0.5f) {
				Break(victim, Level::kOffended, "tried to pick their pocket and got caught", false);
			} else {
				logger::info("trust: {} never noticed the pickpocket", victim->GetName());
			}
		}

		// ---- hits and fights ----

		bool RecentlyHit(RE::FormID id, std::chrono::seconds within)
		{
			return g_lastHit == id && Clock::now() - g_lastHitAt < within;
		}

		// ---- friendly fire ----
		// Someone out with the player (following, spending time, a teammate) caught by a stray blow or
		// arrow mid-fight: the game turns a non-teammate on the player at once. It was an accident, so
		// they're calmed down and nothing is held against the player. Four hits in 10 s is meant.
		std::unordered_map<RE::FormID, std::vector<Clock::time_point>> g_friendlyHits;
		std::unordered_map<RE::FormID, Clock::time_point> g_friendlyCalmed;  // last accident, for the trust checks
		Clock::time_point g_friendlySaid;

		bool FriendlyFireRecent(RE::FormID id, std::chrono::seconds within)
		{
			auto it = g_friendlyCalmed.find(id);
			return it != g_friendlyCalmed.end() && Clock::now() - it->second < within;
		}

		void Calm(RE::FormID id, int tries);

		// Game thread. True when it was treated as an accident.
		bool FriendlyFire(RE::Actor* npc)
		{
			const auto id = npc->GetFormID();
			const auto now = Clock::now();
			auto& hits = g_friendlyHits[id];
			std::erase_if(hits, [&](const Clock::time_point& t) { return now - t > 10s; });
			hits.push_back(now);
			if (hits.size() >= 4) {
				logger::info("friendly fire: {} hit {} times in a row; that's meant", npc->GetName(), hits.size());
				return false;
			}
			g_friendlyCalmed[id] = now;
			Calm(id, 6);
			return true;
		}

		void Calm(RE::FormID id, int tries)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
			auto* player = Game::Player();
			if (!npc || npc->IsDead() || tries <= 0) {
				return;
			}
			auto target = npc->GetActorRuntimeData().currentCombatTarget.get();
			if (npc->IsHostileToActor(player) || target.get() == player) {
				Papyrus::CallMethod(npc, "Actor", "StopCombat", nullptr);
				Papyrus::CallMethod(npc, "Actor", "StopCombatAlarm", nullptr);
				logger::info("friendly fire: {} was hit by accident in the fight; calmed down", npc->GetName());
				if (Clock::now() - g_friendlySaid > 60s) {
					g_friendlySaid = Clock::now();
					Game::Narrate(std::format("{}'s blow caught {} by accident in the fight. {} winced and glared, but knew it wasn't meant.",
									  PlayerName(), npc->GetName(), npc->GetName()),
						npc);
				}
			}
			// The game may turn them again a moment later: look a few more times.
			After(600ms, [id, tries] { Calm(id, tries - 1); });
		}

		struct Sinks :
			RE::BSTEventSink<RE::TESTrackedStatsEvent>,
			RE::BSTEventSink<RE::TESHitEvent>,
			RE::BSTEventSink<RE::TESCombatEvent>,
			RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!e || e->menuName != RE::ContainerMenu::MENU_NAME) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (e->opening) {
					SKSE::GetTaskInterface()->AddTask([] {
						if (RE::ContainerMenu::GetContainerMode() != RE::ContainerMenu::ContainerMode::kPickpocket) {
							return;
						}
						auto ref = RE::TESObjectREFR::LookupByHandle(RE::ContainerMenu::GetTargetRefHandle());
						auto* victim = ref ? ref->As<RE::Actor>() : nullptr;
						g_pickVictim = victim ? victim->GetFormID() : 0;
						g_pickBounty = Bounty();
					});
				} else {
					SKSE::GetTaskInterface()->AddTask([] {
						if (const auto victim = std::exchange(g_pickVictim, 0)) {
							const float before = g_pickBounty;
							After(1500ms, [victim, before] { OnPickpocketDone(victim, before); });
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESTrackedStatsEvent* e, RE::BSTEventSource<RE::TESTrackedStatsEvent>*) override
			{
				if (!e) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string_view stat = e->stat.c_str() ? e->stat.c_str() : "";
				// A successful pickpocket ("Items Pickpocketed") went unnoticed: nothing. A failed one
				// is caught through the pickpocket window above.
				if (stat == "Items Stolen") {
					SKSE::GetTaskInterface()->AddTask(OnStolen);
				} else if (stat == "Assaults") {
					// The game itself called it an assault: on whoever the player just hit.
					SKSE::GetTaskInterface()->AddTask([] {
						if (!RecentlyHit(g_lastHit, 5s)) {
							return;
						}
						if (auto* npc = RE::TESForm::LookupByID<RE::Actor>(g_lastHit); npc && Matters(npc) && !FriendlyFireRecent(g_lastHit, 15s)) {
							Break(npc, Level::kAttacked, "attacked them", true);
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* e, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!e || !e->cause || !e->target || e->cause.get() != Game::Player()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* target = e->target->As<RE::Actor>();
				if (!target || target == Game::Player()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// A healing or helpful spell lands as a "hit" too: only harmful ones are blows.
				if (auto* magic = RE::TESForm::LookupByID<RE::MagicItem>(e->source); magic && !magic->IsHostile()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto id = target->GetFormID();
				// Mid-fight (either of them already fighting: a battle, a brawl): a stray hit, nothing.
				const bool fighting = Game::Player()->IsInCombat() || target->IsInCombat();
				const bool ranged = e->projectile != 0;
				SKSE::GetTaskInterface()->AddTask([id, fighting, ranged] {
					// Out together and mid-fight (or one stray arrow): an accident.
					if (auto* npc = RE::TESForm::LookupByID<RE::Actor>(id); npc && (fighting || ranged) &&
						(Services::IsCompanion(npc) || Services::FollowsPlayer(npc) || Services::InCompany(id)) && FriendlyFire(npc)) {
						return;
					}
					const auto now = Clock::now();
					// A separate shot: not the same arrow's enchantment landing in the same instant.
					const bool repeated = g_prevHit.contains(id) && now - g_prevHit[id] < 10s && now - g_prevHit[id] > 500ms;
					g_prevHit[id] = now;
					g_lastHit = id;
					g_lastHitAt = now;
					// One arrow or spell out of nowhere is a stray (hunting, a missed shot); a blow up
					// close, or a second shot, is meant.
					if (fighting || (ranged && !repeated)) {
						return;
					}
					// A hit out of nowhere. If it turns into a real attack (they fight back, a bounty),
					// that decides; if they just take it, it was rude.
					After(4000ms, [id] {
						auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
						if (!npc || npc->IsDead() || !Matters(npc) || Holds(id)) {
							return;
						}
						if (WasCompanion(id) && npc->IsHostileToActor(Game::Player())) {
							Break(npc, Level::kAttacked, "attacked them", true);
						} else if (!npc->IsHostileToActor(Game::Player())) {
							Break(npc, Level::kOffended, "struck them for no reason", false);
						}
					});
				});
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* e, RE::BSTEventSource<RE::TESCombatEvent>*) override
			{
				if (!e || !e->actor || !e->targetActor || e->targetActor.get() != Game::Player() ||
					!e->newState.any(RE::ACTOR_COMBAT_STATE::kCombat)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* npc = e->actor->As<RE::Actor>();
				if (!npc) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto id = npc->GetFormID();
				// They turned on the player after being hit: the player kept at it. A brawl isn't
				// hostility, and someone who turns on a thief they caught didn't get attacked.
				After(3000ms, [id] {
					auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
					if (npc && !npc->IsDead() && WasCompanion(id) && RecentlyHit(id, 15s) && !FriendlyFireRecent(id, 15s) && npc->IsHostileToActor(Game::Player())) {
						Break(npc, Level::kAttacked, "attacked them", true);
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		static Sinks sinks;
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESHitEvent>(&sinks);
			holder->AddEventSink<RE::TESCombatEvent>(&sinks);
			holder->AddEventSink<RE::TESTrackedStatsEvent>(&sinks);
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&sinks);
		}
	}

	bool Holds(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		auto it = g_grudges.find(npc);
		return it != g_grudges.end() && it->second.level != Level::kNone;
	}

	bool MakingAmends(RE::FormID npc, float hours)
	{
		std::scoped_lock l(g_lock);
		auto it = g_grudges.find(npc);
		if (it == g_grudges.end()) {
			return false;
		}
		return it->second.level != Level::kNone || (it->second.forgivenAt >= 0.0f && Game::Hours() - it->second.forgivenAt < hours);
	}

	bool Matters(RE::Actor* npc)
	{
		if (!npc || npc == Game::Player()) {
			return false;
		}
		const auto id = npc->GetFormID();
		return Services::IsCompanion(npc) || RankOf(npc) >= 1 || HasRights(id) || Holds(id);
	}

	bool Refuse(RE::Actor* npc, const std::string& what)
	{
		if (!npc) {
			return false;
		}
		Grudge g;
		{
			std::scoped_lock l(g_lock);
			auto it = g_grudges.find(npc->GetFormID());
			if (it == g_grudges.end() || it->second.level == Level::kNone) {
				return false;
			}
			g = it->second;
		}
		const std::string name = npc->GetName();
		Game::Notify(std::format("{} ({}): {}", name, g.level == Level::kAttacked ? "never forgave you" : "still upset", "won't do you favours"));
		Game::Narrate(g.level == Level::kAttacked
						  ? std::format("{} refused {}: not after {} attacked them.", name, what, PlayerName())
						  : std::format("{} refused {}: they were still hurt that {} {}, and {} hadn't made it up to them.", name, what, PlayerName(), g.reason, PlayerName()),
			npc);
		logger::info("trust: {} refused {} (grudge: {})", name, what, g.reason);
		return true;
	}

	bool Attacked(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		auto it = g_grudges.find(npc);
		return it != g_grudges.end() && it->second.level == Level::kAttacked;
	}

	float GrudgeHours(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		auto it = g_grudges.find(npc);
		if (it == g_grudges.end() || it->second.level == Level::kNone) {
			return -1.0f;
		}
		return Game::Hours() - it->second.since;
	}

	void OnExchange(RE::FormID npcId, const std::string& offense, bool apologyAccepted)
	{
		const bool hurtful = JsonUtil::Lower(offense).find("hurt") != std::string::npos;
		if (!hurtful && !apologyAccepted) {
			return;
		}
		SKSE::GetTaskInterface()->AddTask([npcId, hurtful, apologyAccepted] {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc || npc->IsDead() || !Matters(npc)) {
				return;
			}
			if (hurtful) {
				// They are already answering it in their own words.
				Break(npc, Level::kOffended, "said something cruel to them", true);
				return;
			}
			std::string reason;
			bool attacked = false;
			{
				std::scoped_lock l(g_lock);
				auto it = g_grudges.find(npcId);
				if (it == g_grudges.end() || it->second.level == Level::kNone) {
					return;  // nothing to forgive
				}
				attacked = it->second.level == Level::kAttacked;
				if (!attacked) {
					reason = it->second.reason;
					it->second.level = Level::kNone;
					it->second.forgivenAt = Game::Hours();
					it->second.forgivenReason = reason;
				}
			}
			const std::string name = npc->GetName();
			if (attacked) {
				Game::Notify(std::format("{} won't forgive an attack.", name));
				logger::info("trust: {} heard an apology, but never forgives an attack", name);
				return;
			}
			Game::Notify(std::format("{} forgave you. Their trust has to be earned again.", name));
			SkyrimNet::AddMemory(npcId, std::format("{} forgave {} for having {}, but {} will have to earn their trust again.", name, PlayerName(), reason, PlayerName()), 0.6f);
			logger::info("trust: {} forgave the player ({})", name, reason);
		});
	}

	std::vector<std::string> BioLines(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		auto it = g_grudges.find(npc);
		if (it == g_grudges.end()) {
			return {};
		}
		const auto& g = it->second;
		const std::string player = Game::Player()->GetName();
		if (g.level == Level::kAttacked) {
			return { std::format("Will never forgive {}: {} attacked them. Gives them nothing and trusts them with nothing.", player, player) };
		}
		if (g.level == Level::kOffended) {
			return { std::format("Is hurt and angry with {}, who {}. Took back everything they had given them (key, bed, welcome) and does them no favours until {} sincerely makes it up.", player, g.reason, player) };
		}
		if (g.forgivenAt >= 0.0f && Game::Hours() - g.forgivenAt < kForgivenMemory) {
			return { std::format("Has forgiven {} for having {}, but trust has to be earned again: what was taken back isn't simply given back.", player, g.forgivenReason) };
		}
		return {};
	}

	void Tick()
	{
		g_bounty = Bounty();
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			for (auto& h : lists->highActorHandles) {
				if (auto a = h.get(); a && Services::IsCompanion(a.get())) {
					g_companions[a->GetFormID()] = Clock::now();
				}
			}
		}
		std::vector<std::pair<RE::FormID, Grudge>> due;
		{
			std::scoped_lock l(g_lock);
			for (auto& [id, g] : g_grudges) {
				if (g.level != Level::kNone && !g.reacted) {
					due.emplace_back(id, g);
				}
			}
		}
		for (auto& [id, g] : due) {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!npc || npc->IsDead()) {
				std::scoped_lock l(g_lock);
				g_grudges[id].reacted = true;
				continue;
			}
			if (Game::DistanceToPlayer(npc) < kReactRange && !npc->IsInCombat() && !Game::Player()->IsInCombat()) {
				React(npc, g);
				std::scoped_lock l(g_lock);
				g_grudges[id].reacted = true;
			}
		}
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json out = json::array();
		for (auto& [id, g] : g_grudges) {
			out.push_back({ { "npc", id }, { "level", static_cast<int>(g.level) }, { "reason", g.reason }, { "since", g.since },
				{ "reacted", g.reacted }, { "forgivenAt", g.forgivenAt }, { "forgivenReason", g.forgivenReason } });
		}
		return out;
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		std::scoped_lock l(g_lock);
		g_grudges.clear();
		if (!j.is_array()) {
			return;
		}
		for (auto& g : j) {
			const auto raw = static_cast<RE::FormID>(JsonUtil::Num(g, "npc").value_or(0));
			const auto id = raw ? resolve(raw) : 0;
			if (!id) {
				continue;
			}
			Grudge out;
			out.level = static_cast<Level>(std::clamp(static_cast<int>(JsonUtil::Num(g, "level").value_or(0)), 0, 2));
			out.reason = JsonUtil::Str(g, "reason");
			out.since = static_cast<float>(JsonUtil::Num(g, "since").value_or(0));
			out.reacted = !g.contains("reacted") || !g["reacted"].is_boolean() || g["reacted"].get<bool>();
			out.forgivenAt = static_cast<float>(JsonUtil::Num(g, "forgivenAt").value_or(-1));
			out.forgivenReason = JsonUtil::Str(g, "forgivenReason");
			g_grudges[id] = std::move(out);
		}
		logger::info("co-save: {} grudges", g_grudges.size());
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_grudges.clear();
		g_lastHit = 0;
		g_companions.clear();
		g_prevHit.clear();
	}
}
