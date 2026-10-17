#include "Keepsakes.h"

#include "Game.h"
#include "JsonUtil.h"
#include "Loans.h"
#include "SkyrimNetBridge.h"
#include "Trust.h"

namespace MundaneMatters::Keepsakes
{
	namespace
	{
		constexpr float kApart = 24.0f;        // game hours apart before meeting again counts as a reunion
		constexpr float kNear = 1500.0f;       // "around": seen, and the moment noticed
		constexpr float kHandRange = 500.0f;   // close enough to hand something over
		constexpr float kPendingFor = 1.0f;    // game hours a chance waits for them to come close
		constexpr float kQuietFrom = 12.0f;    // a quiet moment: never at the start of the day
		constexpr float kQuietTo = 23.0f;

		// The moments a gift can come in. Each day every ally or lover has ONE of them, picked at
		// random, so it can't be predicted or farmed.
		enum class Moment
		{
			kReunion,      // meeting again after a day really apart
			kAfterMoment,  // a drink, a meal or a sit together just ended
			kAfterFight,   // a fight they came through together
			kArrival,      // arriving somewhere together (a town, a dungeon)
			kQuiet,        // a quiet moment at some random hour, noon to night
			kCount
		};

		struct Seen
		{
			float lastNear = -1.0f;   // game hours they were last around the player
			float lastGift = -1.0f;
			float pendingUntil = -1.0f;  // a gift is on its way once they're close
			Moment pending = Moment::kReunion;
			bool around = false;         // near the player on the last tick (sleeping or waiting beside them isn't "apart")
			bool fighting = false;       // they or the player were fighting on the last tick
			bool momentEnded = false;    // a shared moment with them just ended
			int armedDay = -1;           // today's moment
			Moment armed = Moment::kReunion;
			float quietHour = 0.0f;
			bool armedUsed = false;
		};

		std::mutex g_lock;
		std::unordered_map<RE::FormID, Seen> g_seen;
		// What the player ever handed each person (gifts, the follower window, loans, trades): never
		// "given back" as a present of theirs.
		std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> g_fromPlayer;
		int g_lastGiftDay = -1;  // one gift a day, across everyone
		RE::FormID g_place = 0;  // the town or dungeon the player is in (arrivals)

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

		const char* Opening(Moment m)
		{
			switch (m) {
			case Moment::kReunion:
				return "Glad to see {player} again after the time apart, ";
			case Moment::kAfterMoment:
				return "As their time together wound down, ";
			case Moment::kAfterFight:
				return "Once the fighting was over and they had caught their breath, ";
			case Moment::kArrival:
				return "As they arrived, ";
			default:
				return "In a quiet moment, ";
			}
		}

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

		std::string Lower(const char* s)
		{
			return JsonUtil::Lower(s ? s : "");
		}

		// 2 lover, 1 ally, 0 anything less.
		int Closeness(RE::Actor* npc)
		{
			auto* base = npc->GetActorBase();
			auto* rel = base ? RE::BGSRelationship::GetRelationship(base, Game::Player()->GetActorBase()) : nullptr;
			using L = RE::BGSRelationship::RELATIONSHIP_LEVEL;
			const auto level = rel ? rel->level.get() : L::kAcquaintance;
			return level == L::kLover ? 2 : level == L::kAlly ? 1 : 0;
		}

		bool Eligible(RE::Actor* npc)
		{
			if (!npc || npc == Game::Player() || npc->IsDead() || npc->IsChild() || npc->IsInCombat() || npc->IsHostileToActor(Game::Player())) {
				return false;
			}
			auto* race = npc->GetRace();
			if (!npc->HasKeywordString("ActorTypeNPC") && !(race && race->HasKeywordString("ActorTypeNPC"))) {
				return false;
			}
			// Hurt or still sore about something: no presents.
			return Closeness(npc) > 0 && !Trust::MakingAmends(npc->GetFormID());
		}

		bool Asleep(RE::Actor* npc)
		{
			auto* state = npc->AsActorState();
			return state && state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping;
		}

		struct Choice
		{
			RE::TESBoundObject* item = nullptr;
			std::string why;  // in their words
			int score = 0;
			bool invented = false;  // picked, found or bought on the way: not out of their pack
		};

		// The small things anyone might come by on a day out: a flower picked by the road, an apple,
		// berries, a sweet roll from the market. From the base game, by name; cheap by nature.
		Choice Invent(bool lover, std::mt19937& rng)
		{
			struct Small
			{
				const char* name;
				const char* why;
				int weight;  // lovers lean to flowers
			};
			static const std::array<Small, 11> kSmall{ {
				{ "Blue Mountain Flower", "had picked it by the road, thinking of {player}", 3 },
				{ "Purple Mountain Flower", "had picked it by the road, thinking of {player}", 3 },
				{ "Red Mountain Flower", "had picked it by the road, thinking of {player}", 3 },
				{ "Lavender", "had picked a sprig because it smelled like a good day, and thought of {player}", 3 },
				{ "Dragon's Tongue", "had found it growing somewhere warm and thought {player} would like the colour", 2 },
				{ "Red Apple", "had saved the best apple of the lot for {player}", 2 },
				{ "Green Apple", "had saved a crisp apple for {player}", 2 },
				{ "Snowberries", "had gathered a handful of snowberries and kept some for {player}", 1 },
				{ "Juniper Berries", "had picked juniper berries and kept a few for {player}", 1 },
				{ "Sweet Roll", "had bought a sweet roll at the market, thinking {player} could use a treat", 2 },
				{ "Bread", "had a fresh loaf and wanted {player} to have some", 1 },
			} };
			std::vector<std::pair<RE::TESBoundObject*, const Small*>> pool;
			std::vector<double> weights;
			auto* dh = RE::TESDataHandler::GetSingleton();
			for (auto& s : kSmall) {
				for (const auto type : { RE::FormType::Ingredient, RE::FormType::AlchemyItem }) {
					for (auto* f : dh->GetFormArray(type)) {
						auto* b = f ? f->As<RE::TESBoundObject>() : nullptr;
						auto* file = f ? f->GetFile(0) : nullptr;
						if (b && file && std::string_view(file->GetFilename()) == "Skyrim.esm" && b->GetName() && _stricmp(b->GetName(), s.name) == 0) {
							pool.emplace_back(b, &s);
							const bool flower = std::string_view(s.why).find("picked") != std::string_view::npos;
							weights.push_back(s.weight * (lover && flower ? 2.0 : 1.0));
							break;
						}
					}
					if (!pool.empty() && pool.back().second == &s) {
						break;
					}
				}
			}
			if (pool.empty()) {
				return {};
			}
			const auto& [item, chosen] = pool[std::discrete_distribution<std::size_t>(weights.begin(), weights.end())(rng)];
			return { item, chosen->why, 1, true };
		}

		// Something small of their own that says they thought of the player. Never worn, quest,
		// coin, weapons or armour (a cheap ring or amulet aside), nor anything the player lent them.
		Choice Pick(RE::Actor* npc, bool lover, Moment moment)
		{
			// After a fight that left the player hurt, a potion comes first; after time together,
			// something to eat or a flower.
			auto* av = Game::Player()->AsActorValueOwner();
			const float maxHp = av->GetPermanentActorValue(RE::ActorValue::kHealth);
			const bool hurt = maxHp > 0.0f && av->GetActorValue(RE::ActorValue::kHealth) / maxHp < 0.6f;
			const auto cap = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.npc_gift_value", 30.0f)) * (lover ? 2 : 1);
			std::vector<Choice> found;
			std::unordered_set<RE::FormID> theirs;  // what came from the player: never a present of theirs
			{
				std::scoped_lock l(g_lock);
				if (auto it = g_fromPlayer.find(npc->GetFormID()); it != g_fromPlayer.end()) {
					theirs = it->second;
				}
			}
			for (auto& [obj, entry] : npc->GetInventory()) {
				if (obj && theirs.contains(obj->GetFormID())) {
					continue;
				}
				if (!obj || entry.first <= 0 || obj == Game::Gold() || !obj->GetName() || !*obj->GetName() || obj->GetGoldValue() > cap ||
					(entry.second && (entry.second->IsWorn() || entry.second->IsQuestObject())) || Loans::LentCount(npc->GetFormID(), obj) > 0 ||
					!obj->GetPlayable()) {
					continue;
				}
				const auto name = Lower(obj->GetName());
				Choice c{ obj, "", 0 };
				if (auto* a = obj->As<RE::AlchemyItem>()) {
					if (a->IsPoison()) {
						continue;
					}
					if (a->IsFood()) {
						c.score = 2;
						c.why = "had saved some of their food for {player}, made with a little extra care";
					} else {
						c.score = 2;
						c.why = "worried about {player} out there and wanted them to have a potion, just in case";
					}
				} else if (obj->Is(RE::FormType::Ingredient)) {
					const bool flower = name.find("flower") != std::string::npos || name.find("lavender") != std::string::npos || name.find("blossom") != std::string::npos ||
					                    name.find("rose") != std::string::npos || name.find("thistle") != std::string::npos || name.find("blue mountain") != std::string::npos;
					if (!flower) {
						continue;  // a bear claw isn't a present
					}
					c.score = lover ? 4 : 3;
					c.why = "had picked it on the way, thinking of {player}";
				} else if (auto* book = obj->As<RE::TESObjectBOOK>()) {
					if (book->TeachesSpell() || book->TeachesSkill()) {
						continue;
					}
					c.score = 2;
					c.why = "had finished reading it and thought {player} would enjoy it";
				} else if (auto* armor = obj->As<RE::TESObjectARMO>()) {
					if (!armor->HasKeywordString("ArmorJewelry")) {
						continue;
					}
					c.score = lover ? 5 : 1;
					c.why = lover ? "had been keeping it as a little token for {player}" : "had no use for it and thought it would suit {player}";
				} else {
					continue;
				}
				if (moment == Moment::kAfterFight && hurt && c.why.find("potion") != std::string::npos) {
					c.score += 4;
				} else if (moment == Moment::kAfterMoment && (obj->Is(RE::FormType::Ingredient) || c.why.find("food") != std::string::npos)) {
					c.score += 1;
				}
				found.push_back(std::move(c));
			}
			static std::mt19937 rng{ std::random_device{}() };
			// Nothing of their own that fits, or (now and then) a flower picked or an apple saved on
			// the way simply feels right: something small come by, not from their pack. A potion for a
			// hurt player after a fight is never made up.
			const bool potionWanted = moment == Moment::kAfterFight && hurt;
			if (found.empty() || (!potionWanted && std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < 0.35f)) {
				if (auto c = Invent(lover, rng); c.item && c.item->GetGoldValue() <= cap) {
					return c;
				}
			}
			if (found.empty()) {
				return {};
			}
			std::ranges::shuffle(found, rng);
			std::ranges::stable_sort(found, [](const Choice& a, const Choice& b) { return a.score > b.score; });
			return found.front();
		}

		void Give(RE::Actor* npc, const Choice& c, bool lover, Moment moment)
		{
			auto* player = Game::Player();
			if (c.invented) {
				player->AddObjectToContainer(c.item, nullptr, 1, npc);
			} else {
				npc->RemoveItem(c.item, 1, RE::ITEM_REMOVE_REASON::kStoreInTeammate, nullptr, player);
			}
			const std::string name = Game::NameOf(npc);
			const std::string item = c.item->GetName();
			auto why = c.why;
			if (auto at = why.find("{player}"); at != std::string::npos) {
				why.replace(at, 8, PlayerName());
			}
			Game::Notify(std::format("{} gave you {}", name, item));
			SkyrimNet::AddMemory(npc->GetFormID(), std::format("{} gave {} a {}, unasked: they {}.", name, PlayerName(), item, why), 0.6f);
			std::string opening = Opening(moment);
			if (auto at = opening.find("{player}"); at != std::string::npos) {
				opening.replace(at, 8, PlayerName());
			}
			Game::Narrate(std::format("{}without being asked, {} pressed a {} into {}'s hand{}: they {}.", opening, name, item, PlayerName(),
							  lover ? ", a little shy about it" : "", why),
				npc);
			logger::info("keepsake: {} gave {} {} ({}{})", name, PlayerName(), item, lover ? "lover" : "ally", c.invented ? ", come by on the way" : ", their own");
		}
	}

	void OnMoment(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		if (auto it = g_seen.find(npc); it != g_seen.end()) {
			it->second.momentEnded = true;
		}
	}

	void Tick()
	{
		if (!SkyrimNet::ConfigBool("features.npc_gifts", true)) {
			return;
		}
		auto* lists = RE::ProcessLists::GetSingleton();
		auto* player = Game::Player();
		if (!lists) {
			return;
		}
		const float now = Game::Hours();
		const int day = static_cast<int>(now / 24.0f);
		const float hour = Game::HourOfDay();
		const float every = SkyrimNet::ConfigFloat("tuning.npc_gift_days", 7.0f) * 24.0f;
		const bool playerFighting = player->IsInCombat();
		static std::mt19937 rng{ std::random_device{}() };
		std::uniform_real_distribution<float> roll(0.0f, 1.0f);
		// Arriving somewhere: the town or dungeon the player is in changed.
		const auto place = PlaceOf(player->GetCurrentLocation());
		const bool arrived = place && g_place && place != g_place;
		if (place) {
			g_place = place;
		}

		std::vector<std::tuple<RE::Actor*, bool, Moment>> give;
		{
			std::scoped_lock l(g_lock);
			std::unordered_set<RE::FormID> aroundNow;
			for (auto& h : lists->highActorHandles) {
				auto a = h.get();
				if (!a || Game::DistanceToPlayer(a.get()) > kNear || !Eligible(a.get())) {
					continue;
				}
				auto& s = g_seen[a->GetFormID()];
				aroundNow.insert(a->GetFormID());
				// Today's moment, picked at random (a quiet moment: some hour from noon to night).
				if (s.armedDay != day) {
					s.armedDay = day;
					s.armedUsed = false;
					s.armed = static_cast<Moment>(std::uniform_int_distribution<int>(0, static_cast<int>(Moment::kCount) - 1)(rng));
					s.quietHour = std::uniform_real_distribution<float>((std::max)(kQuietFrom, hour + 0.5f), kQuietTo + 0.99f)(rng);
					if (s.armed == Moment::kQuiet && s.quietHour > kQuietTo + 0.99f) {
						s.armed = Moment::kReunion;  // too late in the day for a quiet moment
					}
				}
				const bool fightingNow = playerFighting || a->IsInCombat();
				bool happened = false;
				switch (s.armed) {
				case Moment::kReunion:
					happened = !s.around && s.lastNear >= 0.0f && now - s.lastNear >= kApart;  // really apart, not a night side by side
					break;
				case Moment::kAfterMoment:
					happened = s.momentEnded;
					break;
				case Moment::kAfterFight:
					happened = s.fighting && !fightingNow;
					break;
				case Moment::kArrival:
					happened = arrived;
					break;
				default:
					happened = hour >= s.quietHour && !fightingNow;
					break;
				}
				// Never at the start of the day, whatever the moment.
				if (hour >= 4.0f && hour < 11.0f) {
					happened = false;
				}
				s.around = true;
				s.lastNear = now;
				s.fighting = fightingNow;
				s.momentEnded = false;
				if (happened && !s.armedUsed && s.pendingUntil < now) {
					s.armedUsed = true;  // today's moment came; one roll
					const bool lover = Closeness(a.get()) == 2;
					const bool capped = g_lastGiftDay == day || (s.lastGift >= 0.0f && now - s.lastGift < every);
					if (!capped && roll(rng) < (lover ? 0.35f : 0.25f)) {
						s.pendingUntil = now + kPendingFor;
						s.pending = s.armed;
						logger::info("keepsake: {} means to give {} something (moment {})", a->GetName(), PlayerName(), static_cast<int>(s.armed));
					}
				}
				// On its way: handed over once they're close, awake, not fighting, and the day's gift isn't taken.
				if (s.pendingUntil >= now && !fightingNow && Game::DistanceToPlayer(a.get()) < kHandRange && !Asleep(a.get()) && g_lastGiftDay != day) {
					s.pendingUntil = -1.0f;
					give.emplace_back(a.get(), Closeness(a.get()) == 2, s.pending);
				}
			}
			for (auto& [id, s] : g_seen) {
				if (!aroundNow.contains(id)) {
					s.around = false;
					s.fighting = false;
					s.momentEnded = false;
				}
			}
		}
		for (auto& [npc, lover, moment] : give) {
			auto c = Pick(npc, lover, moment);
			if (!c.item) {
				logger::info("keepsake: {} had nothing small of their own to give", Game::NameOf(npc));
				continue;  // nothing invented: no gift this time
			}
			{
				std::scoped_lock l(g_lock);
				if (g_lastGiftDay == day) {
					break;
				}
				g_lastGiftDay = day;
				g_seen[npc->GetFormID()].lastGift = now;
			}
			Give(npc, c, lover, moment);
			break;  // one a day
		}
	}

	namespace
	{
		// Anything moving from the player to a person: remembered for that person.
		struct GivenSink : RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* e, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (!e || e->oldContainer != 0x14 || !e->newContainer || e->baseObj == 0xF) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto to = e->newContainer, item = e->baseObj;
				SKSE::GetTaskInterface()->AddTask([to, item] {
					if (RE::TESForm::LookupByID<RE::Actor>(to)) {
						std::scoped_lock l(g_lock);
						auto& set = g_fromPlayer[to];
						if (set.size() < 200) {
							set.insert(item);
						}
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		static GivenSink sink;
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESContainerChangedEvent>(&sink);
		}
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json seen = json::array();
		for (auto& [id, s] : g_seen) {
			seen.push_back({ { "npc", id }, { "lastNear", s.lastNear }, { "lastGift", s.lastGift } });
		}
		json given = json::array();
		for (auto& [npc, items] : g_fromPlayer) {
			for (auto id : items) {
				given.push_back({ npc, id });
			}
		}
		return { { "seen", seen }, { "lastGiftDay", g_lastGiftDay }, { "fromPlayer", given } };
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		std::scoped_lock l(g_lock);
		g_seen.clear();
		g_fromPlayer.clear();
		if (j.is_object() && j.contains("fromPlayer") && j["fromPlayer"].is_array()) {
			for (auto& pair : j["fromPlayer"]) {
				if (pair.is_array() && pair.size() == 2 && pair[0].is_number() && pair[1].is_number()) {
					const auto npc = resolve(pair[0].get<RE::FormID>());
					const auto item = resolve(pair[1].get<RE::FormID>());
					if (npc && item) {
						g_fromPlayer[npc].insert(item);
					}
				}
			}
		}
		g_lastGiftDay = static_cast<int>(JsonUtil::Num(j, "lastGiftDay").value_or(-1));
		if (j.is_object() && j.contains("seen") && j["seen"].is_array()) {
			for (auto& o : j["seen"]) {
				const auto raw = static_cast<RE::FormID>(JsonUtil::Num(o, "npc").value_or(0));
				if (const auto id = raw ? resolve(raw) : 0) {
					auto& s = g_seen[id];
					s.lastNear = static_cast<float>(JsonUtil::Num(o, "lastNear").value_or(-1));
					s.lastGift = static_cast<float>(JsonUtil::Num(o, "lastGift").value_or(-1));
				}
			}
		}
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_seen.clear();
		g_fromPlayer.clear();
		g_lastGiftDay = -1;
		g_place = 0;
	}
}
