#include "TurnPipeline.h"

#include "Papyrus.h"
#include "Bonds.h"
#include "Pledges.h"
#include "Brawls.h"
#include "Confirm.h"
#include "Gestures.h"
#include "Moments.h"
#include "MoveIn.h"
#include "VanillaLines.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Ledger.h"
#include "Promises.h"
#include "Services.h"
#include "Trade.h"
#include "Trust.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::TurnPipeline
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr RE::FormID kPlayer = 0x14;
		constexpr auto kPairWindow = 60s;       // NPC reply must follow the player line within this
		constexpr auto kMenuDedupWindow = 20s;  // someone else opened the menu already
		constexpr std::size_t kRecentLines = 6;
		constexpr float kMaxServiceDistance = 1500.0f;

		constexpr auto kJoinWindow = 20s;  // player lines this close together, unanswered, are one turn

		struct PendingLine
		{
			std::string text;
			RE::FormID target = 0;  // often 0: typed lines frequently arrive without a target
			Clock::time_point at;
			// NPCs that already answered this line. A bystander may answer first ("Aye, Hulda's
			// prices are steep"); the person actually asked still gets classified when they reply.
			std::unordered_set<RE::FormID> answeredBy;
		};

		std::mutex g_lock;
		std::optional<PendingLine> g_pending;
		std::unordered_map<RE::FormID, std::deque<std::string>> g_recent;  // per NPC, "Name: line"
		std::unordered_map<std::string, Clock::time_point> g_menuOpened;

		void Remember(RE::FormID npc, std::string line)
		{
			auto& q = g_recent[npc];
			q.push_back(std::move(line));
			while (q.size() > kRecentLines) {
				q.pop_front();
			}
		}

		// ---- menu tracking (dedup against SkyrimNet dialogue actions / SeverActions) ----

		class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuWatcher* Get()
			{
				static MenuWatcher instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (e && e->opening) {
					std::scoped_lock l(g_lock);
					g_menuOpened[std::string(e->menuName.c_str())] = Clock::now();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		bool MenuOpenedSince(const char* menu, Clock::time_point since)
		{
			std::scoped_lock l(g_lock);
			auto it = g_menuOpened.find(menu);
			return it != g_menuOpened.end() && it->second >= since;
		}

		// ---- waiting for the NPC to stop talking ----

		struct SpeechState
		{
			std::atomic_int queue{ -1 };
			std::atomic_int msSinceAudioEnded{ -1 };
		};

		void PollSpeech(const std::shared_ptr<SpeechState>& s)
		{
			SKSE::GetTaskInterface()->AddTask([s] {
				Papyrus::CallStatic("SkyrimNetApi", "GetSpeechQueueSize",
					[s](const RE::BSScript::Variable& v) { s->queue = v.GetSInt(); });
				Papyrus::CallStatic("SkyrimNetApi", "GetTimeSinceLastAudioEnded",
					[s](const RE::BSScript::Variable& v) { s->msSinceAudioEnded = v.GetSInt(); });
			});
		}

		// Runs `then` on the game thread once SkyrimNet has no speech queued and the reply's audio
		// has ended (audio that ended before the reply arrived does not count). Gives up waiting
		// after 30 s and runs anyway, so a stuck TTS never swallows the service.
		void AfterSpeech(Clock::time_point replyAt, std::function<void()> then)
		{
			std::thread([replyAt, then = std::move(then)] {
				auto s = std::make_shared<SpeechState>();
				const auto deadline = replyAt + 30s;
				while (Clock::now() < deadline) {
					std::this_thread::sleep_for(250ms);
					PollSpeech(s);
					std::this_thread::sleep_for(50ms);

					const auto sinceReply = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - replyAt).count();
					const int queue = s->queue;
					const int sinceAudio = s->msSinceAudioEnded;
					if (sinceReply < 1000 || queue != 0 || sinceAudio < 0) {
						continue;
					}
					// Audio ended after the reply arrived: they spoke and finished.
					const bool spokeAndFinished = sinceAudio > 0 && sinceAudio < sinceReply && sinceAudio >= 200;
					// No audio at all for a while (text-only setups): don't wait forever.
					const bool silentSetup = sinceReply > 8000;
					if (spokeAndFinished || silentSetup) {
						break;
					}
				}
				SKSE::GetTaskInterface()->AddTask(then);
			}).detach();
		}

		// ---- classification ----

		std::optional<json> ParseReply(const std::string& raw)
		{
			// Models sometimes wrap JSON in prose or ``` fences: take the outermost object.
			auto first = raw.find('{');
			auto last = raw.rfind('}');
			if (first == std::string::npos || last == std::string::npos || last < first) {
				return std::nullopt;
			}
			const auto body = raw.substr(first, last - first + 1);
			auto j = json::parse(body, nullptr, false);
			if (!j.is_discarded() && j.is_object()) {
				return j;
			}

			// Repair the typical damage: stray non-ASCII tokens ("confidence":奧0.9), empty values
			// ("confidence":,) and junk where a value should start ("confidence":)[0.9,). Damaged
			// values become null, which every reader treats as "not said".
			std::string fixed;
			for (char c : body) {
				if (static_cast<unsigned char>(c) < 0x80) {
					fixed.push_back(c);
				}
			}
			fixed = std::regex_replace(fixed, std::regex(R"(:\s*(?=[,}\]]))"), ": null");
			fixed = std::regex_replace(fixed, std::regex(R"(:\s*[^\s"\[{\-0-9tfn][^,}\]]*)"), ": null");
			j = json::parse(fixed, nullptr, false);
			if (!j.is_discarded() && j.is_object()) {
				logger::info("classifier JSON was malformed; repaired it");
				return j;
			}

			// Broken JSON (models sometimes garble one value, e.g. "confidence":奧0.9): salvage the
			// string fields we act on. Promises are dropped since their structure can't be trusted.
			json salvaged = json::object();
			for (const char* key : { "request", "response", "confidence" }) {
				std::regex re(std::string("\"") + key + "\"\\s*:\\s*\"([A-Za-z_]+)\"");
				std::smatch m;
				if (std::regex_search(raw, m, re)) {
					salvaged[key] = m[1].str();
				}
			}
			if (!salvaged.contains("request")) {
				return std::nullopt;
			}
			if (!salvaged.contains("response")) {
				salvaged["response"] = "accept";  // the prompt's default when unsure
			}
			if (!salvaged.contains("confidence")) {
				salvaged["confidence"] = "medium";
			}
			logger::info("classifier JSON was malformed; salvaged {}", salvaged.dump());
			return salvaged;
		}

		// The price the NPC named for the service; 0 when none (or junk).
		std::int32_t Price(const json& j)
		{
			auto v = JsonUtil::Num(j, "price");
			return v && *v > 0 && *v < 1e6 ? static_cast<std::int32_t>(*v) : 0;
		}

		// buy/sell: how many ("all" = -1); 1 when unsaid or junk.
		std::int32_t Qty(const json& j)
		{
			const auto text = JsonUtil::Lower(JsonUtil::Str(j, "qty"));
			if (text.find("all") != std::string::npos || text.find("every") != std::string::npos || text.find("lot") != std::string::npos) {
				return -1;
			}
			auto v = JsonUtil::Num(j, "qty");
			return v && *v >= 1 && *v < 1e5 ? static_cast<std::int32_t>(*v) : v && *v < 0 ? -1 : 1;
		}

		// "high"/"medium"/"low" (current prompt), or a 0-1 number, possibly as a string. Junk -> 0.
		double Confidence(const json& j)
		{
			auto it = j.find("confidence");
			if (it == j.end() || it->is_null()) {
				return 0.6;  // not said (or repaired away): treat as "medium"
			}
			if (it->is_string()) {
				auto s = JsonUtil::Lower(it->get<std::string>());
				if (s.find("high") != std::string::npos) return 0.9;
				if (s.find("med") != std::string::npos) return 0.6;
				if (s.find("low") != std::string::npos) return 0.3;
			}
			auto v = JsonUtil::Num(j, "confidence");
			if (!v) {
				return 0.0;
			}
			return *v > 1.0 && *v <= 100.0 ? *v / 100.0 : (*v >= 0.0 && *v <= 1.0 ? *v : 0.0);  // "90" = 90%
		}

		// Models drift from the ids they were given ("barter", "room", "yes"): map them back.
		std::string NormalizeRequest(std::string r)
		{
			r = JsonUtil::Lower(std::move(r));
			// A price question is only talk; the NPC quotes it from the real stock.
			if (r.find("price") != std::string::npos || r.find("quote") != std::string::npos) {
				return "none";
			}
			// Going home together ("visit", "go_home", "visit_home"); before "sit" ("relax") and "follow".
			if (r == "visit" || r.starts_with("visit_") || r.ends_with("_visit") || r == "go_home" || r == "home" || r.find("go_home") != std::string::npos) {
				return "visit";
			}
			// "company", but not "accompany" (that's follow).
			if (r == "pay_fine" || r.find("fine") != std::string::npos || r.find("bounty") != std::string::npos) {
				return "pay_fine";
			}
			if (r.find("bribe") != std::string::npos) {
				return "bribe";
			}
			if (r.find("persuad") != std::string::npos || r.find("convinc") != std::string::npos) {
				return "persuade";
			}
			if (r.find("intimidat") != std::string::npos || r.find("threat") != std::string::npos) {
				return "intimidate";
			}
			if (r == "perform" || r.find("sing") != std::string::npos || r.find("song") != std::string::npos || r.find("music") != std::string::npos ||
				r.find("play_") != std::string::npos || r.find("tune") != std::string::npos || r.find("bard") != std::string::npos) {
				return "perform";
			}
			if (r.find("move_in") != std::string::npos || r.find("movein") != std::string::npos || r.find("live_with") != std::string::npos ||
				r == "move" || r.find("housemate") != std::string::npos) {
				return "move_in";
			}
			if ((r.find("company") != std::string::npos && r.find("accompan") == std::string::npos) || r.find("spend_time") != std::string::npos ||
				r == "stay" || r == "stay_with") {
				return "company";
			}
			if (r == "sit" || r.starts_with("sit_") || r == "seat" || r.find("break") != std::string::npos || r.find("relax") != std::string::npos) {
				return "sit";
			}
			if (r == "drink" || r.find("drink") != std::string::npos || r.find("round") != std::string::npos || r.find("toast") != std::string::npos ||
				r.find("tavern") != std::string::npos) {
				return "drink";
			}
			if (r == "cure" || r.find("disease") != std::string::npos || r.find("cure_") != std::string::npos) {
				return "cure";
			}
			if (r == "heal" || r.find("heal") != std::string::npos || r.find("patch") != std::string::npos || r.find("tend") != std::string::npos) {
				return "heal";
			}
			if (r == "cook" || r.find("cook") != std::string::npos) {
				return "cook";
			}
			if (r == "mend" || r.find("temper") != std::string::npos || r.find("sharpen") != std::string::npos || r.find("repair") != std::string::npos ||
				r.find("improve") != std::string::npos || r.find("smith") != std::string::npos) {
				return "mend";
			}
			if (r == "buy" || r.starts_with("buy_") || r.find("purchas") != std::string::npos) {
				return "buy";
			}
			if (r == "sell" || r.starts_with("sell_")) {
				return "sell";
			}
			if (r.find("trade") != std::string::npos || r.find("barter") != std::string::npos || r.find("shop") != std::string::npos ||
				r.find("wares") != std::string::npos || r.find("merchant") != std::string::npos) {
				return "trade";
			}
			if (r == "retrieve" || r.find("give_back") != std::string::npos || r.find("get_back") != std::string::npos ||
				r.find("return") != std::string::npos || r == "take_back") {
				return "retrieve";
			}
			if (r == "hold" || r.find("keep") != std::string::npos || r.find("store") != std::string::npos || r.find("storage") != std::string::npos ||
				r.find("stash") != std::string::npos ||
				r == "carry" || r == "safekeeping") {
				return "hold";
			}
			if (r == "rest" || r.find("sleep") != std::string::npos || r.find("bed_down") != std::string::npos || r.find("turn_in") != std::string::npos ||
				r.find("camp") != std::string::npos || r == "rest_together" || r == "go_to_bed") {
				return "rest";
			}
			if (r.find("room") != std::string::npos || r.find("rent") != std::string::npos || r.find("lodg") != std::string::npos ||
				r == "inn") {
				return "rent_room";
			}
			if (r.find("train") != std::string::npos || r.find("teach") != std::string::npos || r.find("lesson") != std::string::npos) {
				return "train";
			}
			if (r.find("escort") != std::string::npos || r.find("guide") != std::string::npos || r.find("lead") != std::string::npos ||
				r.find("travel") != std::string::npos || r.find("take_me") != std::string::npos || r.find("show_way") != std::string::npos) {
				return "escort";
			}
			if (r.find("meal") != std::string::npos || r == "eat" || r.starts_with("eat_") || r.ends_with("_eat") || r.find("dine") != std::string::npos ||
				r.find("supper") != std::string::npos || r.find("dinner") != std::string::npos) {
				return "share_meal";
			}
			if (r.find("follow") != std::string::npos || r.find("accompan") != std::string::npos || r.find("tag") != std::string::npos ||
				r == "come" || r == "come_along" || r == "join") {
				return "follow";
			}
			return "none";
		}

		std::string NormalizeResponse(std::string r)
		{
			r = JsonUtil::Lower(std::move(r));
			if (r.find("free") != std::string::npos || r.find("house") != std::string::npos || r.find("waive") != std::string::npos) {
				return "free";
			}
			if (r.find("refus") != std::string::npos || r.find("declin") != std::string::npos || r.find("reject") != std::string::npos ||
				r == "no" || r.find("deny") != std::string::npos || r.find("denied") != std::string::npos) {
				return "refuse";
			}
			if (r.find("counter") != std::string::npos || r.find("condition") != std::string::npos) {
				return "counter";
			}
			return "accept";  // the prompt's default when unsure ("agree", "yes", "ok", "none", junk)
		}

		// "promises" as a list, a single object, or missing.
		json PromiseList(const json& j)
		{
			auto it = j.find("promises");
			if (it == j.end()) {
				return json::array();
			}
			if (it->is_array()) {
				return *it;
			}
			if (it->is_object()) {
				return json::array({ *it });
			}
			return json::array();
		}

		void RunService(RE::FormID npcId, const std::string& service, Services::Args args, Clock::time_point replyAt)
		{
			AfterSpeech(replyAt, [npcId, service, args, replyAt] {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!npc || npc->IsDead() || npc->IsInCombat() || player->IsInCombat()) {
					logger::info("{} skipped: NPC gone or combat", service);
					return;
				}
				// Going home together: the player may already be through the door; the follow catches up.
				if (service != "visit" && Game::DistanceToPlayer(npc) > kMaxServiceDistance) {
					logger::info("{} skipped: {} walked away", service, npc->GetName());
					return;
				}
				auto run = [npcId, service, args, replyAt] {
					auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
					if (!npc || npc->IsDead() || npc->IsInCombat() || (service != "visit" && Game::DistanceToPlayer(npc) > kMaxServiceDistance)) {
						return;
					}
					const char* menu = Services::MenuFor(service);
					if (*menu && MenuOpenedSince(menu, replyAt - kMenuDedupWindow / 2)) {
						logger::info("{} skipped: {} already opened by another mod", service, menu);
						return;
					}
					// The game's own line for it went off in this exchange ("I'd like a room", "I'll pay the
					// fine"): that line did it already; not a second time.
					static const std::unordered_set<std::string> kVanillaService{ "rent_room", "trade", "train", "buy", "sell", "pay_fine", "bribe",
						"persuade", "intimidate" };
					if (kVanillaService.contains(service) && VanillaLines::FiredRecently(npcId, 60)) {
						logger::info("{} skipped: the game's own dialogue line for it was just used", service);
						return;
					}
					bool ok = Services::Execute(service, npc, args);
					logger::info("{} for {} -> {}", service, npc->GetName(), ok ? "started" : "FAILED");
				};
				// Done in person: a few steps away, they come over first (up from a seat if they're in one),
				// then the trade, the room, the lesson. Given 12 s; then it happens anyway if they're close.
				static const std::unordered_set<std::string> kInPerson{ "trade", "buy", "sell", "rent_room", "train", "heal", "cure", "mend", "cook",
					"hold", "retrieve", "pay_fine", "bribe", "persuade", "intimidate" };
				if (!kInPerson.contains(service) || Game::DistanceToPlayer(npc) <= 300.0f) {
					run();
					return;
				}
				logger::info("{}: {} comes over first", service, npc->GetName());
				if (npc->GetOccupiedFurniture().get()) {
					Moments::Release(npcId);
					Papyrus::CallStatic("SeverActions_FurnitureLib", "Stop", nullptr, static_cast<RE::Actor*>(npc));
				}
				auto done = std::make_shared<std::atomic_bool>(false);
				auto once = [done, run] {
					if (!done->exchange(true)) {
						run();
					}
				};
				Papyrus::CallMethod(npc, "Actor", "PathToReference",
					[once](const RE::BSScript::Variable&) { SKSE::GetTaskInterface()->AddTask(once); },
					static_cast<RE::TESObjectREFR*>(player), 1.0f);
				std::thread([npcId, once] {
					std::this_thread::sleep_for(std::chrono::seconds(12));
					SKSE::GetTaskInterface()->AddTask([npcId, once] {
						if (auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId); npc && Game::DistanceToPlayer(npc) < 700.0f) {
							once();
						}
					});
				}).detach();
			});
		}

		// ---- cost gate for NPCs without services ----
		// Not the decision (the classifier judges meaning), only "could this exchange hold a
		// promise at all?". Broad on purpose: a false yes costs one call, a false no loses a promise.

		const std::unordered_set<std::string> kPromiseWords{
			"gold", "coin", "coins", "septim", "septims", "pay", "paid", "payment", "reward", "money", "purse",
			"give", "giving", "gave", "bring", "fetch", "gift", "spare", "share", "lend", "borrow", "owe", "keep", "yours", "free",
			"eat", "drink", "food", "meal", "hungry", "thirsty", "starving", "bowl", "plate", "mug", "cup", "bottle",
			"bread", "ale", "mead", "wine", "soup", "stew", "potion", "potions",
			"sleep", "bed", "beds", "rest", "stay", "room", "roof", "crash", "night", "tonight", "tomorrow", "later", "morning", "evening",
			"promise", "promised", "compensate", "coppers", "drakes", "dinner", "supper", "breakfast", "lunch", "feed", "fed",
			"blanket", "bedroll", "lodging", "shelter", "settle", "earned", "deserve", "present", "brought", "accept", "key", "keys",
			"borrow", "borrowed", "lend", "lent", "loan", "return", "returned", "returning"
		};
		// The player promising something ("I'll bring you mead tomorrow"): checked on the player's line.
		const std::array<std::string_view, 22> kPledgePhrases{
			"i'll bring", "i will bring", "i'll find", "i will find", "i'll get you", "i will get you", "i'll pay", "i will pay", "i'll come",
			"i will come", "i'll be back", "i'll return", "i promise", "you have my word", "i swear", "i'll fetch", "i'll make it up", "i'll help",
			"i will help", "i'll stop by", "i'll visit", "can't bring"
		};

		const std::array<std::string_view, 25> kPromisePhrases{
			"here you", "here's", "here,", "there you go", "take this", "take these", "take it", "take one", "for you", "on the house",
			"on me", "my treat", "help yourself", "my place", "with me", "for your trouble", "least i can do", "hold on to",
			"take that", "can i have", "may i have", "mind if i", "can i take", "may i take", "go ahead"
		};
		// The NPC's reply only counts on what offers or asks back: their long, descriptive lines are full of
		// "morning", "rest", "stew" without anything being given (and every one of those was a classifier call).
		const std::vector<std::string_view> kNpcOfferPhrases{
			"you can have", "you can take", "you can sleep", "you can stay", "you can rest", "you can use", "you can borrow", "take my", "have my",
			"use my", "borrow my", "it's yours", "its yours", "is yours", "are yours", "keep it", "keep them", "stay the night", "sleep here",
			"stay here", "spare bed", "my bed", "always welcome", "you're welcome to", "you are welcome to", "welcome here", "your key", "my key",
			"spare key", "want my", "want it back", "give it back", "give me back", "hand it back", "bring it back", "meet me", "find me at",
			"be there", "i'll be at", "i'll be waiting", "on the table", "in the pot", "by the fire for you", "set aside", "saved you", "here.",
			"take some", "have some", "grab some", "try some", "eat up", "drink up", "pour yourself", "eat with", "with us", "join us",
			"earned this", "earned it", "you've earned", "you deserve", "this is for", "a gift", "for the road"
		};
		// ...or commit, in the same sentence as something a promise is made of ("I'll have the stew ready").
		const std::vector<std::string_view> kNpcCommits{
			"i'll", "i will", "i'd", "i can", "let me", "i'm going to", "i want", "you can", "you may", "we can", "we'll", "i could", "i shall", "i must"
		};
		// Trade, rooms, training: a merchant, innkeeper or trainer's every line went to the classifier.
		// Now only when the talk is about what they offer (or names a number: a price, an amount).
		const std::unordered_set<std::string> kShopWords{
			"buy", "buying", "bought", "sell", "selling", "sold", "sale", "wares", "goods", "trade", "trading", "shop", "store", "stock",
			"price", "prices", "cost", "costs", "pay", "gold", "coin", "coins", "septim", "septims", "merchant", "deal", "purchase", "browse",
			"pelt", "pelts", "afford", "cheap", "expensive", "haggle", "offer", "inventory", "supplies", "barter", "wanna", "selection"
		};
		const std::vector<std::string_view> kShopPhrases{
			"i'll take", "ill take", "i will take", "what do you have", "what have you", "you got", "what've you got", "u got", "for sale",
			"how much", "show me", "have a look", "take a look", "anything good", "got anything", "what's on offer"
		};
		const std::unordered_set<std::string> kRoomWords{
			"room", "rooms", "bed", "beds", "night", "sleep", "stay", "rent", "lodging", "lodgings", "tired", "rest", "crash", "inn", "bunk"
		};
		const std::unordered_set<std::string> kTrainWords{
			"train", "training", "trainer", "teach", "teaching", "taught", "learn", "learning", "lesson", "lessons", "skill", "skills",
			"practice", "improve", "better", "master", "tutor", "instruct", "instruction", "spar"
		};

		std::vector<std::string> Words(std::string_view text)
		{
			std::vector<std::string> out;
			std::string cur;
			for (char c : text) {
				if (std::isalnum(static_cast<unsigned char>(c))) {
					cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
				} else if (!cur.empty()) {
					out.push_back(std::move(cur));
					cur.clear();
				}
			}
			if (!cur.empty()) {
				out.push_back(std::move(cur));
			}
			return out;
		}

		// Words from food and ingredient names in the load order ("cabbage", "salmon", "sweetroll").
		// Built once, on the game thread.
		const std::unordered_set<std::string>& ItemWords()
		{
			static const auto words = [] {
				std::unordered_set<std::string> set;
				auto add = [&](const char* name) {
					std::string joined;
					for (auto& w : Words(name ? name : "")) {
						joined += w;
						if (w.size() >= 4) {
							set.insert(w);
						}
					}
					set.insert(joined);  // "Sweet Roll" also as "sweetroll"
				};
				auto* dh = RE::TESDataHandler::GetSingleton();
				for (auto* f : dh->GetFormArray<RE::AlchemyItem>()) {
					if (f && f->IsFood()) {
						add(f->GetName());
					}
				}
				for (auto* f : dh->GetFormArray<RE::IngredientItem>()) {
					if (f) {
						add(f->GetName());
					}
				}
				logger::info("promise gate: {} food/ingredient words", set.size());
				return set;
			}();
			return words;
		}

		// Letters outside English (Cyrillic, CJK, accented...), ignoring the typographic punctuation
		// AI lines are full of (’ “ ” — … and no-break spaces), which is also non-ASCII.
		bool HasForeignLetters(const std::string& line)
		{
			for (std::size_t i = 0; i < line.size(); ++i) {
				const auto c = static_cast<unsigned char>(line[i]);
				if (c < 0x80) {
					continue;
				}
				const auto next = i + 1 < line.size() ? static_cast<unsigned char>(line[i + 1]) : 0;
				if (c == 0xE2 && (next == 0x80 || next == 0x81)) {  // U+2000-U+207F general punctuation
					i += 2;
					continue;
				}
				if (c == 0xC2) {  // U+0080-U+00BF: no-break space, «», ·, °
					i += 1;
					continue;
				}
				return true;
			}
			return false;
		}

		// Misspelled key words in the player's line ("folow me", "escrot me to", "gide me").
		bool HasMoveWordTypo(const std::string& lower)
		{
			// Not "take"/"lead": "make", "read", "head" are one letter away and everywhere.
			static const std::array<std::string_view, 6> kWords{ "follow", "escort", "guide", "accompany", "direction", "travel" };
			for (auto& w : Words(lower)) {
				for (auto key : kWords) {
					// Short words allow one slip ("gide", "laed"), longer ones two ("acompny").
					const std::size_t allowed = key.size() <= 5 ? 1 : 2;
					if (w.size() + 2 >= key.size() && w.size() <= key.size() + 2 && w != key && JsonUtil::EditDistance(w, key) <= allowed &&
						w.size() >= 4) {
						return true;
					}
				}
			}
			return false;
		}

		// The NPC's answer shows they understood a request to come along or lead the way, whatever
		// the player typed ("Follow me, it's this way", "Lead on, I'm right behind you").
		bool NpcAgreesToMove(const std::string& npcLine)
		{
			static const std::array<std::string_view, 24> kNpc{
				"follow me", "this way", "come with me", "come along", "i'll take you", "i will take you", "i'll show you",
				"i will show you", "i'll lead", "lead the way", "lead on", "walk you", "after me", "let's go", "lets go",
				"on our way", "right behind you", "i'll follow", "i will follow", "after you", "i'm coming", "keep up",
				"stay close", "i'll guide"
			};
			std::string lower(npcLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(kNpc, [&](std::string_view w) { return lower.find(w) != std::string::npos; });
		}

		// The item's main word ("venison", "health potion" -> "potion") is in the text, singular or plural.
		bool ItemNamedIn(const std::string& text, const std::string& item)
		{
			const auto words = JsonUtil::Words(JsonUtil::Lower(item));
			if (words.empty()) {
				return true;
			}
			const auto& head = words.back();
			for (auto& w : JsonUtil::Words(JsonUtil::Lower(text))) {
				if (w == head || w == head + "s" || w == head + "es" || (head.size() > 3 && head.ends_with("s") && w == head.substr(0, head.size() - 1))) {
					return true;
				}
			}
			return false;
		}

		// A yes to going home together / inside: "lead the way", "let's get inside", "fair enough", "aye".
		bool NpcAgreesToVisit(const std::string& npcLine)
		{
			std::string lower(npcLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			auto has = [&](std::initializer_list<const char*> ws) { return std::ranges::any_of(ws, [&](const char* w) { return lower.find(w) != std::string::npos; }); };
			if (has({ "not now", "another time", "some other time", "can't", "cannot", "too far", "rather not", "don't think so", "i don't know you",
					"barely know", "no home", "not tonight", "i'd better not", "i won't", "maybe later" })) {
				return false;
			}
			return NpcAgreesToMove(npcLine) ||
			       has({ "get inside", "go inside", "head inside", "come in", "come on in", "fair enough", "alright", "all right", "sure", "aye", "of course",
				       "why not", "sounds good", "gladly", "i'd like that", "yes", "let's", "lets go", "inside then", "warm up" });
		}

		// The player wants to sit or relax there, not just be shown the way.
		bool RelaxWords(const std::string& line)
		{
			const auto l = JsonUtil::Lower(line);
			return std::ranges::any_of(std::initializer_list<const char*>{ "relax", "sit", "rest", "warm", "a while", "awhile", "unwind", "chill", "drink", "talk" },
				[&](const char* w) { return l.find(w) != std::string::npos; });
		}

		// Follow / escort only when the words say so (the classifier mixes up who leads):
		//  - follow = they come along behind the PLAYER: the player asked ("follow me", "come with me",
		//    "let's go") or they offered ("I'll come with you", "lead the way"). Their own "come with me"
		//    means they lead - never a follow.
		//  - being led about the same building ("somewhere private", "the inner room", "upstairs") is no
		//    walking service at all: the AI (and romance mods) play that out.
		//  - a walk just started for them: a second one isn't stacked on it unless the player asks again.
		// Empty: fine. Otherwise why not.
		std::unordered_map<RE::FormID, Clock::time_point> g_lastMove;  // when a follow/escort last started per NPC

		// The player's line is a "no" ("it seems I am not", "I don't like that", "rather not") and asks for
		// nothing else: whatever the reply makes of it, nothing starts on it.
		// A host asking the player back: "you're welcome back here anytime", "come back any time".
		bool WelcomeBack(const std::string& line)
		{
			std::string p = " ";
			for (auto& w : JsonUtil::Words(JsonUtil::Lower(line))) {
				p += w + " ";
			}
			for (auto* k : { " welcome back ", " come back anytime ", " come back any time ", " welcome here anytime ", " welcome here any time ",
					 " welcome anytime ", " welcome any time ", " always welcome ", " door s always open ", " door is always open ",
					 " always a place for you ", " stay whenever ", " come back whenever " }) {
				if (p.find(k) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// The classifier's "sleep": player_floor / npc_floor / share, or "" (drift tolerated).
		std::string SleepOf(const std::string& raw)
		{
			const auto s = JsonUtil::Lower(raw);
			if (s.find("share") != std::string::npos || s.find("together") != std::string::npos) {
				return "share";
			}
			if (s.find("npc") != std::string::npos || s.find("host") != std::string::npos) {
				return "npc_floor";
			}
			if (s.find("player") != std::string::npos || s.find("guest") != std::string::npos) {
				return "player_floor";
			}
			return "";
		}

		bool PlayerSaidNo(const std::string& line)
		{
			std::string padded = " ";
			for (auto& w : JsonUtil::Words(JsonUtil::Lower(line))) {
				padded += w + " ";
			}
			auto has = [&](std::initializer_list<const char*> ps) {
				return std::ranges::any_of(ps, [&](const char* p) { return padded.find(std::string(" ") + p + " ") != std::string::npos; });
			};
			// "No, come on, sit next to me": a leading "no" that goes on to ask for it is a correction, not a refusal.
			const bool leadingNo = (padded.starts_with(" no ") || padded.starts_with(" nah ") || padded.starts_with(" nope ")) &&
			                       !has({ "come on", "common", "cmon", "c mon", "sit", "with me", "next to me", "join me", "stay", "have this", "have a",
										 "take this", "here" });
			const bool no = leadingNo ||
			                has({ "i am not", "i m not", "im not", "i dont like", "i don t like", "i do not like", "i dont want", "i don t want",
									"i do not want", "rather not", "i ll pass", "ill pass", "not interested", "never mind", "nevermind", "no thanks",
									"no thank you", "not now", "not tonight", "i refuse", "i won t", "i wont", "i will not", "maybe another time",
									"another time", "some other time", "not today", "i m good", "im good", "i m fine", "im fine", "i m okay", "i m ok",
									"no need", "don t bother", "dont bother", "not necessary", "i ll manage", "ill manage", "i ll be fine" });
			const bool asks = has({ "let s", "lets", "can you", "could you", "would you", "will you", "i want to", "i d like", "id like",
				"i would like", "give me", "take me", "show me", "please", "how about", "instead", "come with me", "follow me" });
			// "I'm fine with that" is a yes.
			const bool okWith = has({ "fine with", "good with", "okay with", "ok with" });
			return no && !asks && !okWith;
		}

		std::string MoveProblem(const std::string& request, const std::string& playerLine, const std::string& npcLine, RE::FormID npcId,
			const std::string& place)
		{
			if (request == "move_in") {
				return MoveIn::PlayerAsks(playerLine) || MoveIn::NpcAsks(npcLine) ? std::string() : std::string("nobody asked to live together");
			}
			if (request == "company") {
				return Services::PlayerAsksCompany(playerLine) || Services::NpcOffersCompany(npcLine) ? std::string() : std::string("nobody asked to spend time");
			}
			if (request != "follow" && request != "escort") {
				return {};
			}
			const auto p = JsonUtil::Lower(playerLine);
			const auto n = JsonUtil::Lower(npcLine);
			auto has = [](const std::string& text, std::initializer_list<const char*> words) {
				return std::ranges::any_of(words, [&](const char* w) { return text.find(w) != std::string::npos; });
			};
			const bool playerAsks = has(p, { "follow me", "come with me", "come along", "with me", "tag along", "join me", "let's go", "lets go",
				"accompany me", "stick with me", "stick close", "travel with me", "walk with me", "come on", "take me", "bring me", "lead the way",
				"lead me", "guide me", "show me the way", "show me where", "show me your", "show me the", "walk me", "get me to" });
			if (place.empty() && (has(n, { "somewhere private", "someplace private", "more comfortable", "inner room", "back room", "my room", "upstairs",
								 "downstairs", "in the back", "somewhere quiet", "where we won't be", "where we wont be" }) ||
									 has(p, { "somewhere private", "my room", "your room", "upstairs" }))) {
				return "led about the same place, no walking service";
			}
			if (request == "follow") {
				const bool npcOffers = has(n, { "i'll come with you", "i will come with you", "i'll follow", "i will follow", "right behind you",
					"after you", "lead the way", "lead on", "i'm coming", "i'm with you", "i'll go with you", "i'll tag along" });
				if (!playerAsks && !npcOffers) {
					return "they lead; nobody asked them to follow";
				}
			}
			std::scoped_lock l(g_lock);
			if (auto it = g_lastMove.find(npcId); it != g_lastMove.end() && Clock::now() - it->second < std::chrono::seconds(120) && !playerAsks) {
				return "a walk just started for them";
			}
			return {};
		}

		// The NPC's line is the player's own line again ("Maybe I am, maybe I'm not" back to "maybe i am maybe
		// not"): most of its words, in the same length, and nothing of their own.
		bool Echoes(const std::string& playerLine, const std::string& npcLine)
		{
			auto norm = [](const std::string& t) {
				std::string out;
				for (auto& w : JsonUtil::Words(t)) {
					std::string clean;
					for (char c : w) {
						if (c != '\'') {
							clean += c;
						}
					}
					out += (out.empty() ? "" : " ") + clean;
				}
				return out;
			};
			const auto p = norm(playerLine);
			const auto n = norm(npcLine);
			if (std::ranges::count(p, ' ') < 3 || n.empty()) {
				return false;  // a short line ("so what now?") may well be answered in kind
			}
			// The same sentence give or take a few words: within about a third of its letters.
			return JsonUtil::EditDistance(p, n) * 100 <= (std::max)(p.size(), n.size()) * 36;
		}

		// Could the PLAYER be asking someone to come along or lead the way? Same idea as the promise
		// gate: broad, only deciding whether the classifier is asked at all.
		bool MightMove(const std::string& playerLine, const std::string& npcLine)
		{
			if (NpcAgreesToMove(npcLine)) {
				return true;
			}
			static const std::vector<std::string_view> kMove{
				"follow", "come with", "come along", "come on", "with me", "tag along", "stick with", "stick close", "accompany",
				"join me", "let's go", "lets go", "walk with", "escort", "take me", "bring me to", "lead the way", "lead me",
				"guide", "show me the way", "show me where", "way to", "how do i get", "where is", "where's", "direction",
				"get me to", "walk me", "travel with", "go with me", "'s place", "'s house", "'s home",
				"show me", "where do we", "where do you", "where can i", "where can we", "where do i"
			};
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			std::string lower(playerLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			// "where do you live": "live" as a word, never inside "deliver" or "alive".
			const auto words = Words(lower);
			const bool live = std::ranges::any_of(words, [](const std::string& w) { return w == "live" || w == "lives" || w == "living"; });
			return live || std::ranges::any_of(kMove, [&](std::string_view w) { return lower.find(w) != std::string::npos; }) || HasMoveWordTypo(lower);
		}

		// Could this be "let's eat together"? Broad, like the other gates: the player's words (typos
		// allowed), or the NPC answering as if invited to eat.
		bool MightMeal(const std::string& playerLine, const std::string& npcLine)
		{
			static const std::unordered_set<std::string> kMeal{
				"eat", "eating", "ate", "meal", "meals", "dinner", "supper", "breakfast", "lunch", "food", "hungry", "starving",
				"table", "feast", "dine", "dining", "bite", "share", "cook", "cooking", "stew", "soup", "drink", "drinks", "ale", "mead"
			};
			// One slip only, and only on longer words ("dinnr", "suppr", "brekfast"): "meal"/"mean" or
			// "hungry"/"angry" would match everyday words.
			static const std::array<std::string_view, 4> kTypoKeys{ "dinner", "supper", "breakfast", "hungry" };
			static const std::array<std::string_view, 12> kNpcSays{
				"sit with me", "eat with me", "let's eat", "lets eat", "set the table", "join me", "pull up a chair", "pull up a stool",
				"we'll eat", "we can eat", "share a meal", "supper together"
			};
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			for (auto& w : Words(playerLine)) {
				if (kMeal.contains(w)) {
					return true;
				}
				for (auto key : kTypoKeys) {
					if (w.size() >= 5 && w != key && JsonUtil::EditDistance(w, key) <= 1) {
						return true;
					}
				}
			}
			std::string lower(npcLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(kNpcSays, [&](std::string_view p) { return lower.find(p) != std::string::npos; });
		}

		// Could this be "let's rest"? Same broad shape as MightMeal: the player's words (typos
		// allowed), or the NPC answering as if asked to bed down.
		bool MightRest(const std::string& playerLine, const std::string& npcLine)
		{
			// Rest is offered to anyone (an acquaintance says no), so everyday words like "night" or
			// "turn" only count inside a phrase: "good night" mustn't cost a classifier call.
			static const std::unordered_set<std::string> kRest{
				"sleep", "sleeping", "sleepy", "rest", "resting", "bed", "bedroll", "bedrolls", "tired", "exhausted",
				"camp", "nap", "doze", "kip", "slumber", "bunk", "weary"
			};
			// One slip only, and only on longer words ("slep", "tierd", "bedrol").
			static const std::array<std::string_view, 5> kTypoKeys{ "sleep", "tired", "bedroll", "exhausted", "slumber" };
			static const std::array<std::string_view, 12> kPhrases{
				"bed down", "get some rest", "turn in", "make camp", "get some sleep", "lie down", "time for bed", "off to bed",
				"shut-eye", "shut eye", "call it a night", "for the night"
			};
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			std::string lowerPlayer(playerLine);
			std::ranges::transform(lowerPlayer, lowerPlayer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (std::ranges::any_of(kPhrases, [&](std::string_view p) { return lowerPlayer.find(p) != std::string::npos; })) {
				return true;
			}
			for (auto& w : Words(playerLine)) {
				if (kRest.contains(w)) {
					return true;
				}
				for (auto key : kTypoKeys) {
					if (w.size() >= 4 && w != key && JsonUtil::EditDistance(w, key) <= 1) {
						return true;
					}
				}
			}
			std::string lower(npcLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(kPhrases, [&](std::string_view p) { return lower.find(p) != std::string::npos; });
		}

		// Could this be "hold this for me" / "can I have my sword back"? A keeping verb followed by
		// what is kept ("hold this", "keep my", "carry these"; typos allowed), "back" with "my", or
		// the NPC answering as if handed something to keep.
		bool MightHold(const std::string& playerLine, const std::string& npcLine)
		{
			static const std::array<std::string_view, 8> kVerbs{ "hold", "keep", "carry", "mind", "watch", "guard", "store", "stash" };
			static const std::unordered_set<std::string> kWhat{ "this", "these", "that", "those", "my", "it", "them", "onto", "safe" };
			static const std::unordered_set<std::string> kAlone{ "stash", "safekeeping", "belongings" };
			static const std::array<std::string_view, 12> kPhrases{
				"hold on to", "look after", "keep it safe", "keep them safe", "i'll keep", "i'll hold", "i'll carry", "i'll look after",
				"here's your", "here are your", "give it back", "hand it back"
			};
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			std::string lowerPlayer(playerLine);
			std::ranges::transform(lowerPlayer, lowerPlayer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (std::ranges::any_of(kPhrases, [&](std::string_view p) { return lowerPlayer.find(p) != std::string::npos; })) {
				return true;
			}
			const auto words = Words(playerLine);
			const bool mine = std::ranges::find(words, "my") != words.end();
			for (std::size_t i = 0; i < words.size(); ++i) {
				const auto& w = words[i];
				if (kAlone.contains(w) || (mine && (w == "back" || w == "return"))) {
					return true;
				}
				const bool verb = std::ranges::any_of(kVerbs, [&](std::string_view v) {
					return w == v || (w.size() >= 4 && JsonUtil::EditDistance(w, v) <= 1);
				});
				if (verb && i + 1 < words.size() && kWhat.contains(words[i + 1])) {
					return true;
				}
			}
			std::string lower(npcLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(kPhrases, [&](std::string_view p) { return lower.find(p) != std::string::npos; });
		}

		// Could this be "play us something" / "sing me a song"? "Play" only with what's played: it's
		// an everyday word ("I play cards", "play it safe").
		bool MightPerform(const std::string& playerLine, const std::string& npcLine)
		{
			static const std::unordered_set<std::string> kWords{
				"sing", "singing", "sang", "song", "songs", "music", "tune", "tunes", "lute", "flute", "drum", "drums", "bard", "melody", "ballad",
				"hum", "verse", "instrument", "serenade", "ditty", "shanty", "ragnar", "dragonborn"
			};
			static const std::array<std::string_view, 6> kPhrases{ "play us", "play me", "play something", "play a", "play for", "play your" };
			static const std::array<std::string_view, 3> kTypoKeys{ "sing", "song", "music" };
			static const std::array<std::string_view, 4> kNpcSays{ "a song", "play you", "a tune", "my lute" };
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			std::string lowerPlayer(playerLine);
			std::ranges::transform(lowerPlayer, lowerPlayer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (std::ranges::any_of(kPhrases, [&](std::string_view p) { return lowerPlayer.find(p) != std::string::npos; })) {
				return true;
			}
			for (auto& w : Words(playerLine)) {
				if (kWords.contains(w)) {
					return true;
				}
				for (auto key : kTypoKeys) {
					if (w.size() >= 4 && w != key && JsonUtil::EditDistance(w, key) <= 1) {
						return true;
					}
				}
			}
			// "sing" as a word only: "using", "passing", "missing" have it inside.
			for (auto& w : Words(npcLine)) {
				if (w == "sing" || w == "singing" || w == "sang") {
					return true;
				}
			}
			std::string lower(npcLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(kNpcSays, [&](std::string_view p) { return lower.find(p) != std::string::npos; });
		}

		// Could this be "sit with me a while"? Sitting, company, a break, the fire or the sky (typos
		// allowed on the long words), or the NPC answering as if asked to sit.
		bool MightSit(const std::string& playerLine, const std::string& npcLine)
		{
			static const std::unordered_set<std::string> kWords{
				"sit", "sitting", "seat", "bench", "company", "relax", "fire", "fireside", "hearth", "campfire", "stars", "sunset", "sunrise",
				"linger", "unwind", "lounge"
			};
			// Everyday words only inside a phrase ("I'll be a while" mustn't cost a classifier call).
			static const std::vector<std::string_view> kPhrases{ "a while", "take a break", "rest our feet", "catch our breath", "keep me company",
				"stay with me", "a breather", "rest a bit", "spend time", "spend some time", "hang out", "stick around", "stay a bit", "awhile" };
			{
				std::string lowerPlayer(playerLine);
				std::ranges::transform(lowerPlayer, lowerPlayer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (std::ranges::any_of(kPhrases, [&](std::string_view p) { return lowerPlayer.find(p) != std::string::npos; })) {
					return true;
				}
			}
			static const std::array<std::string_view, 3> kTypoKeys{ "company", "sitting", "campfire" };
			static const std::array<std::string_view, 6> kNpcSays{ "sit with", "sit down", "pull up", "take a seat", "a seat", "join me by" };
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			for (auto& w : Words(playerLine)) {
				if (kWords.contains(w)) {
					return true;
				}
				for (auto key : kTypoKeys) {
					if (w.size() >= 5 && w != key && JsonUtil::EditDistance(w, key) <= 1) {
						return true;
					}
				}
			}
			std::string lower(npcLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(kNpcSays, [&](std::string_view p) { return lower.find(p) != std::string::npos; });
		}

		// Could this be "a round of mead?" Drinking words in the player's line (typos allowed on the
		// long ones), or the NPC answering as if invited for a drink.
		bool MightDrink(const std::string& playerLine, const std::string& npcLine)
		{
			static const std::unordered_set<std::string> kWords{
				"drink", "drinks", "drinking", "ale", "ales", "mead", "meads", "wine", "beer", "pint", "pints", "round", "toast", "cheers",
				"tankard", "flagon", "mug", "booze", "sujamma", "flin", "brandy", "rum", "tavern", "bar", "thirsty", "sip", "dram", "shot"
			};
			static const std::array<std::string_view, 3> kTypoKeys{ "drink", "thirsty", "tankard" };
			static const std::array<std::string_view, 7> kNpcSays{ "pour", "cheers", "a round", "pull up a stool", "to your health", "skal", "bottoms up" };
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			for (auto& w : Words(playerLine)) {
				if (kWords.contains(w)) {
					return true;
				}
				for (auto key : kTypoKeys) {
					if (w.size() >= 4 && w != key && JsonUtil::EditDistance(w, key) <= 1) {
						return true;
					}
				}
			}
			std::string lower(npcLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(kNpcSays, [&](std::string_view p) { return lower.find(p) != std::string::npos; });
		}

		// Could the player be asking to be cared for: healed, fed from their own ingredients, their
		// gear improved, a disease cured? Broad like the other gates (typos allowed on the long words).
		bool MightCare(const std::string& playerLine, const std::string& npcLine)
		{
			static const std::unordered_set<std::string> kWords{
				"hurt", "hurts", "wound", "wounded", "wounds", "bleeding", "bleed", "injured", "injury", "heal", "healing", "patch", "bandage",
				"cook", "cooking", "cooked", "recipe", "stew", "roast",
				"sharpen", "temper", "improve", "mend", "repair", "fix", "hone", "smith", "upgrade",
				"sick", "ill", "disease", "diseased", "cure", "fever", "rockjoint", "ataxia", "caught", "poorly", "infected"
			};
			static const std::array<std::string_view, 5> kTypoKeys{ "wounded", "bleeding", "sharpen", "disease", "injured" };
			static const std::array<std::string_view, 8> kNpcSays{ "let me see that", "hold still", "let me look", "i'll patch", "i'll cook", "i'll sharpen",
				"by the gods, you're", "the temple can" };
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			for (auto& w : Words(playerLine)) {
				if (kWords.contains(w)) {
					return true;
				}
				for (auto key : kTypoKeys) {
					if (w.size() >= 5 && w != key && JsonUtil::EditDistance(w, key) <= 1) {
						return true;
					}
				}
			}
			std::string lower(npcLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(kNpcSays, [&](std::string_view p) { return lower.find(p) != std::string::npos; });
		}

		// Could this hurt someone, or mend things? Only matters for people who trust the player (or
		// hold a grudge), so it's checked for them alone.
		bool MightHurtOrMend(const std::string& playerLine, const std::string& npcLine)
		{
			static const std::unordered_set<std::string> kWords{
				"sorry", "apologize", "apologise", "apology", "apologies", "forgive", "forgiveness", "regret", "fault",
				"idiot", "fool", "stupid", "ugly", "hate", "pathetic", "worthless", "liar", "coward", "useless", "disgusting",
				"dumb", "moron", "bitch", "whore", "bastard", "milkdrinker", "trash", "scum", "filth", "pig", "hag", "wench",
				"slut", "freak", "loser", "shut", "kill", "die", "ashamed", "disgrace"
			};
			static const std::array<std::string_view, 12> kPhrases{
				"make it up", "didn't mean", "my fault", "go away", "get lost", "how dare you", "i trusted you", "i forgive you",
				"apology accepted", "get out", "never speak to me", "you thief"
			};
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			for (const auto* line : { &playerLine, &npcLine }) {
				std::string lower(*line);
				std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (std::ranges::any_of(kPhrases, [&](std::string_view p) { return lower.find(p) != std::string::npos; })) {
					return true;
				}
			}
			return std::ranges::any_of(Words(playerLine), [](const std::string& w) { return kWords.contains(w); });
		}

		bool MightPromise(const std::string& playerLine, const std::string& npcLine)
		{
			auto promiseWord = [](const std::string& w) { return kPromiseWords.contains(w) || ItemWords().contains(w); };
			auto lowered = [](std::string s) {
				std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				for (std::size_t at; (at = s.find("\xE2\x80\x99")) != std::string::npos;) {
					s.replace(at, 3, "'");
				}
				return s;
			};
			// Digits, or a non-English line (the word lists below are English): ask the classifier.
			auto numberOrForeign = [](const std::string& s) {
				return std::ranges::any_of(s, [](unsigned char c) { return std::isdigit(c); }) || HasForeignLetters(s);
			};
			// The player's line: short, so any sign of a promise, a gift, an ask.
			if (numberOrForeign(playerLine)) {
				return true;
			}
			const auto player = lowered(playerLine);
			if (std::ranges::any_of(kPromisePhrases, [&](std::string_view p) { return player.find(p) != std::string::npos; }) ||
				std::ranges::any_of(kPledgePhrases, [&](std::string_view p) { return player.find(p) != std::string::npos; }) ||
				std::ranges::any_of(Words(player), promiseWord)) {
				return true;
			}
			// The NPC's reply: an amount, an offer, or a sentence where they commit to something a promise is made of.
			if (numberOrForeign(npcLine)) {
				return true;
			}
			const auto npc = lowered(npcLine);
			if (std::ranges::any_of(kPromisePhrases, [&](std::string_view p) { return npc.find(p) != std::string::npos; }) ||
				std::ranges::any_of(kNpcOfferPhrases, [&](std::string_view p) { return npc.find(p) != std::string::npos; })) {
				return true;
			}
			auto commits = [&](const std::string& s) {
				const auto padded = " " + s + " ";
				const bool commit = std::ranges::any_of(kNpcCommits, [&](std::string_view c) {
					for (std::size_t at = padded.find(c); at != std::string::npos; at = padded.find(c, at + 1)) {
						const auto before = static_cast<unsigned char>(padded[at - 1]);
						const auto after = static_cast<unsigned char>(padded[at + c.size()]);
						if (!std::isalnum(before) && !std::isalnum(after)) {
							return true;
						}
					}
					return false;
				});
				return commit && std::ranges::any_of(Words(s), promiseWord);
			};
			std::string sentence;
			for (char c : npc + ".") {
				if (c == '.' || c == '!' || c == '?' || c == '\n' || c == '*') {
					if (commits(sentence)) {
						return true;
					}
					sentence.clear();
				} else {
					sentence += c;
				}
			}
			return false;
		}

		// Trade / rooms / training talk (the player's line or the reply): only then are those services asked about.
		bool MightShop(const std::string& playerLine, const std::string& npcLine)
		{
			const auto both = JsonUtil::Lower(playerLine + " " + npcLine);
			return std::ranges::any_of(both, [](unsigned char c) { return std::isdigit(c); }) || HasForeignLetters(playerLine) ||
			       std::ranges::any_of(Words(both), [](const std::string& w) { return kShopWords.contains(w); }) ||
			       std::ranges::any_of(kShopPhrases, [&](std::string_view p) { return both.find(p) != std::string::npos; });
		}

		bool MightRoom(const std::string& playerLine, const std::string& npcLine)
		{
			return HasForeignLetters(playerLine) ||
			       std::ranges::any_of(Words(JsonUtil::Lower(playerLine + " " + npcLine)), [](const std::string& w) { return kRoomWords.contains(w); });
		}

		bool MightTrain(const std::string& playerLine, const std::string& npcLine)
		{
			const auto both = JsonUtil::Lower(playerLine + " " + npcLine);
			return HasForeignLetters(playerLine) || both.find("show me how") != std::string::npos ||
			       std::ranges::any_of(Words(both), [](const std::string& w) { return kTrainWords.contains(w); });
		}

		void RunGift(RE::FormID npcId, std::vector<Promises::Gift> gifts, Clock::time_point replyAt)
		{
			AfterSpeech(replyAt, [npcId, gifts] {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || npc->IsDead() || npc->IsInCombat() || Game::DistanceToPlayer(npc) > kMaxServiceDistance) {
					logger::info("gift skipped: NPC gone, far or fighting");
					return;
				}
				Promises::Receive(npc, gifts);
			});
		}

		void RunPromise(RE::FormID npcId, std::string npcName, Promises::Promise promise, Clock::time_point replyAt)
		{
			// "Dawn. East ridge.": a meeting - IntelEngine walks them there when it's time.
			if (promise.type == "meet") {
				SKSE::GetTaskInterface()->AddTask([npcId, promise] {
					if (auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId)) {
						Services::ScheduleMeeting(npc, promise.item, promise.timeHint, promise.saidPlace);
						SkyrimNet::AddMemory(npcId, std::format("{} agreed to meet {} {}.", npc->GetName(), RE::PlayerCharacter::GetSingleton()->GetName(),
							Promises::Describe(promise).substr(8)), 0.5f);
					}
				});
				return;
			}
			// "You're always welcome here": guest rights, kept on every visit by the ledger.
			if (promise.type == "offer_bed" && promise.always && !Trust::Holds(npcId)) {
				if (!Ledger::IsGuest(npcId)) {
					Ledger::AddGuest(npcId);
					SkyrimNet::AddMemory(npcId, std::format("{} told {} they are always welcome to stay at their home.", npcName,
						RE::PlayerCharacter::GetSingleton()->GetName()), 0.5f);
					Game::Notify(std::format("You're always welcome at {}'s home.", npcName));
					logger::info("{}: the player is now always welcome", npcName);
				}
				return;
			}
			if (promise.when == "later") {
				Ledger::AddPending(npcId, npcName, promise);
				return;
			}
			AfterSpeech(replyAt, [npcId, npcName, promise] {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || npc->IsDead()) {
					return;
				}
				// Not here or busy fighting: hand it over when they are next together.
				if (npc->IsInCombat() || Game::DistanceToPlayer(npc) > kMaxServiceDistance) {
					Ledger::AddPending(npcId, npcName, promise);
					return;
				}
				// Generosity has limits: a budget of free things (food counts half) that refills over
				// time, bigger and faster the closer they are. Paying back what was owed is exempt.
				// A loan comes back, so it doesn't eat into what they'll give away (bread "lent" does).
				const bool handout = (promise.type == "give_item" || promise.type == "give_gold" || promise.type == "allow_take") && !Promises::IsLoan(npc, promise);
				// Angry with the player: Keep says no in their own words, before any budget talk.
				if (Trust::Refuse(npc, Promises::Describe(promise))) {
					return;
				}
				const auto generosity = Promises::GenerosityOf(npc);
				const auto cost = handout ? Promises::HandoutCost(npc, promise) : 0;
				if (handout && !Ledger::IsPending(npcId, promise)) {
					const auto left = Ledger::BudgetLeft(npcId, generosity);
					if (generosity.never || cost > left) {
						const auto window = Ledger::CurrentHandouts(npcId);
						logger::info("{}: can't spare {} (costs {}, {} left of {}; refills every {:.0f} h)", npcName,
							Promises::Describe(promise), cost, left, generosity.budget, generosity.hours);
						const std::string player = Game::Player()->GetName();
						Game::Narrate(generosity.never
										  ? std::format("{} wasn't about to give {} anything for free.", npcName, player)
									  : window && !window->what.empty()
										  ? std::format("{} remembered they had already given {} {} and couldn't spare {} for free as well.", npcName, player, window->what, Promises::Describe(promise))
										  : std::format("{} realised {} was more than they could spare for free.", npcName, Promises::Describe(promise)),
							npc);
						return;
					}
				}
				auto outcome = Promises::Keep(npc, promise);
				if (outcome == Promises::Outcome::kNotYet) {
					Ledger::AddPending(npcId, npcName, promise);
				} else if (outcome == Promises::Outcome::kKept) {
					// "Here's the 100 gold I promised": the one owed for tomorrow is settled now.
					Ledger::Fulfilled(npcId, promise);
					if (handout) {
						Ledger::RecordHandout(npcId, Promises::Describe(promise), cost, generosity.hours);
					}
				}
			});
		}

		void Classify(RE::FormID npcId, std::string playerLine, std::string npcLine, Clock::time_point replyAt)
		{
			// Game thread: read actor data, then hand the LLM call to SkyrimNet's thread pool.
			SKSE::GetTaskInterface()->AddTask([npcId, playerLine = std::move(playerLine), npcLine = std::move(npcLine), replyAt] {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc) {
					return;
				}

				auto services = Services::For(npc);
				const bool promises = SkyrimNet::ConfigBool("features.promises", true);
				// Follow/escort only when the player's line could be asking for it: fewer false
				// triggers, and anyone offers them, so they mustn't defeat the cost gate below.
				const bool move = MightMove(playerLine, npcLine);
				if (!move) {
					std::erase_if(services, [](const Services::Service& s) { return Services::IsMovement(s.id); });
				}
				// A meal together only when food or eating comes up; rest only when sleep does.
				const bool meal = MightMeal(playerLine, npcLine);
				const bool rest = MightRest(playerLine, npcLine);
				const bool hold = MightHold(playerLine, npcLine);
				const bool care = MightCare(playerLine, npcLine);
				const bool drink = MightDrink(playerLine, npcLine);
				const bool sit = MightSit(playerLine, npcLine);
				const bool perform = MightPerform(playerLine, npcLine);
				const bool shop = MightShop(playerLine, npcLine);
				const bool room = MightRoom(playerLine, npcLine);
				const bool train = MightTrain(playerLine, npcLine);
				std::erase_if(services, [&](const Services::Service& s) {
					return (s.id == "share_meal" && !meal) || (s.id == "rest" && !rest) || ((s.id == "hold" || s.id == "retrieve") && !hold) ||
					       (Services::IsCare(s.id) && !care) || (s.id == "drink" && !drink) || ((s.id == "sit" || s.id == "company") && !sit) ||
					       (s.id == "perform" && !perform) || ((s.id == "trade" || s.id == "buy" || s.id == "sell") && !shop) ||
					       (s.id == "rent_room" && !room) || (s.id == "train" && !train) || (s.id == "visit" && !Services::MightVisit(npc, playerLine, npcLine)) || (s.id == "move_in" && !MoveIn::PlayerAsks(playerLine) && !MoveIn::NpcAsks(npcLine));
				});
				const bool activity = std::ranges::any_of(services, [](const Services::Service& s) { return Services::IsActivity(s.id); });
				const bool shopkeeper = std::ranges::any_of(services, [](const Services::Service& s) {
					return !Services::IsMovement(s.id) && !Services::IsActivity(s.id);
				});
				// Asked only when the talk is about one of their services (trade / room / training talk above), a
				// promise, a request to come along / lead the way, hurt words or an apology, or a favour's answer.
				const bool trust = SkyrimNet::ConfigBool("features.trust", true) && Trust::Matters(npc) && MightHurtOrMend(playerLine, npcLine);
				const bool answering = Pledges::Waiting(npcId);  // they just asked a favour: "sure" is an answer
				const bool promise = promises && MightPromise(playerLine, npcLine);
				if (!shopkeeper && !move && !activity && !trust && !answering && !promise) {
					logger::info("{}: small talk, not classified", npc->GetName());
					return;
				}
				if (npc->IsHostileToActor(Game::Player())) {
					return;
				}
				// Why it's worth a classifier call (each one costs the player's AI credits): the log shows it.
				{
					std::string why;
					for (auto& s : services) {
						why += (why.empty() ? "" : ",") + s.id;
					}
					logger::info("{}: classifying ({}{}{}{}{})", npc->GetName(), why.empty() ? "" : "services " + why, promise ? " promise-talk" : "",
						trust ? " hurt/mend" : "", answering ? " answering a favour" : "", move ? " movement" : "");
				}
				std::vector<std::string> serviceIds;
				for (auto& s : services) {
					serviceIds.push_back(s.id);
				}

				json ctx;
				ctx["playerName"] = RE::PlayerCharacter::GetSingleton()->GetName();
				ctx["npcName"] = npc->GetName();
				ctx["playerLine"] = playerLine;
				ctx["npcLine"] = npcLine;
				{
					const int hour = static_cast<int>(Game::HourOfDay());
					const char* part = hour < 5 ? "night" : hour < 8 ? "early morning" : hour < 12 ? "morning" : hour < 14 ? "midday" :
					                   hour < 18 ? "afternoon" : hour < 21 ? "evening" : "night";
					ctx["timeNow"] = std::format("{:02}:00 ({})", hour, part);
				}
				ctx["here"] = Services::WhereIs(npc);
				// The places list also when a time-and-place plan may be in it (the classifier maps a made-up place onto them).
				const auto timeWords = [](const std::string& s) {
					const auto l = JsonUtil::Lower(s);
					return std::ranges::any_of(std::initializer_list<const char*>{ "dawn", "dusk", "tonight", "tomorrow", "noon", "sunset", "sunrise", "midnight" },
						[&](const char* w) { return l.find(w) != std::string::npos; });
				};
				if (move || Services::MeetWords(playerLine) || Services::MeetWords(npcLine) || timeWords(npcLine)) {
					ctx["places"] = Services::PlacesNear(npc, 30);
				}
				ctx["services"] = json::array();
				for (auto& s : services) {
					ctx["services"].push_back({ { "id", s.id }, { "label", s.label }, { "examples", s.examples } });
				}
				{
					std::scoped_lock l(g_lock);
					auto& recent = g_recent[npcId];
					// The last two entries are this exchange; send only what came before it.
					auto n = recent.size() >= 2 ? recent.size() - 2 : 0;
					ctx["recentLines"] = std::vector<std::string>(recent.begin(), recent.begin() + n);
				}
				// Handed over moments ago (a kept promise, a gift): talk about it isn't a second one.
				for (auto& what : Promises::GivenRecently(npcId)) {
					ctx["recentLines"].push_back(std::format("(moments ago, {} handed {} {})", npc->GetName(), ctx["playerName"].get<std::string>(), what));
				}

				std::string npcName = npc->GetName();
				auto onReply = [npcId, npcName, replyAt, serviceIds, promises, playerLineCopy = playerLine, npcLineCopy = npcLine,
							   earlierLines = ctx["recentLines"].dump()](const std::string& raw, bool success) {
					if (!success) {
						logger::warn("classifier failed for {}: {}", npcName, raw);
						return;
					}
					auto j = ParseReply(raw);
					if (!j) {
						logger::warn("classifier returned no JSON for {}: {}", npcName, raw);
						return;
					}
					auto request = NormalizeRequest(JsonUtil::Str(*j, "request", "none"));
					auto response = NormalizeResponse(JsonUtil::Str(*j, "response", "accept"));
					const auto confidence = Confidence(*j);
					const auto promiseList = PromiseList(*j);
					logger::info("classified {}: request={} ({:.2f}) response={} promises={} gives={}",
						npcName, request, confidence, response, promiseList.dump(),
						j->contains("player_gives") ? (*j)["player_gives"].dump() : std::string("[]"));

					// What the PLAYER promised them (or took back), and what they say the player did.
					if (SkyrimNet::ConfigBool("features.player_promises", true) && (j->contains("player_promises") || j->contains("player_kept"))) {
						// A vanilla line went off in this exchange (a quest accepted that way): the journal has it.
						const bool viaVanilla = VanillaLines::FiredRecently(npcId, 90);
						const json pledges = j->contains("player_promises") && !viaVanilla ? (*j)["player_promises"] : json::array();
						const auto kept = JsonUtil::Str(*j, "player_kept");
						const auto said = playerLineCopy;
						SKSE::GetTaskInterface()->AddTask([npcId, pledges, kept, said] { Pledges::FromExchange(npcId, pledges, kept, said); });
					}

					// Who sleeps where at their home (one bed): as agreed in this exchange.
					if (const auto how = SleepOf(JsonUtil::Str(*j, "sleep")); !how.empty() && SkyrimNet::ConfigBool("features.promises", true)) {
						SKSE::GetTaskInterface()->AddTask([npcId, how] { Ledger::AgreeSleeping(npcId, how); });
					}

					// Words that hurt them, or an apology they accepted.
					if (SkyrimNet::ConfigBool("features.trust", true)) {
						const auto accepted = JsonUtil::Lower(JsonUtil::Str(*j, "apology_accepted"));
						Trust::OnExchange(npcId, JsonUtil::Str(*j, "offense"), accepted == "true" || accepted == "yes" || accepted == "1");
					}

					// Going home together: the classifier tends to file it as small talk (or a plain sit / walk).
					// The player's words asked for it (home talk, or "let's go inside" at their door: noted before
					// the reply) and the NPC said yes: it's the visit.
					if (request != "visit" && std::ranges::find(serviceIds, "visit") != serviceIds.end() && !Services::VisitLine(npcId).empty() &&
						(Services::VisitWords(playerLineCopy) || JsonUtil::Lower(playerLineCopy).find("inside") != std::string::npos) && NpcAgreesToVisit(npcLineCopy)) {
						const bool walk = request == "follow" || request == "escort";
						if (request == "none" || request == "sit" || request == "company" || (walk && RelaxWords(playerLineCopy))) {
							logger::info("{}: {} read as visit (asked to go home / inside, and the reply agrees)", npcName, request);
							request = "visit";
							response = "accept";
							if (JsonUtil::Str(*j, "item").empty() || walk) {
								const auto pl = " " + JsonUtil::Lower(playerLineCopy) + " ";
								const bool mine = std::ranges::any_of(std::initializer_list<const char*>{ " my place", " my home", " my house", " mine", " our " },
									[&](const char* w) { return pl.find(w) != std::string::npos; });
								(*j)["item"] = mine ? "mine" : "yours";
							}
						}
					}

					// Only services this NPC really has; the model sometimes invents one.
					const bool offered = std::ranges::find(serviceIds, request) != serviceIds.end();
					const float minConf = SkyrimNet::ConfigFloat("tuning.min_confidence", 0.6f);
					bool serviceStarted = false;
					if (offered && confidence >= minConf) {
						if (const auto why = MoveProblem(request, playerLineCopy, npcLineCopy, npcId, JsonUtil::Str(*j, "place")); !why.empty()) {
							logger::info("{}: {} not started ({})", npcName, request, why);
						} else if (PlayerSaidNo(playerLineCopy)) {
							logger::info("{}: {} not started (the player said no; the reply read it as a yes)", npcName, request);
						} else if (response == "refuse" || response == "counter") {
							Game::DebugNotify(npcName + " declined (" + request + ")");
						} else {
							Game::DebugNotify(npcName + ": " + request);
							const auto ownTrip = JsonUtil::Lower(JsonUtil::Str(*j, "own_trip"));
							RunService(npcId, request,
								{ Price(*j), response == "free", JsonUtil::Str(*j, "place"), ownTrip == "true" || ownTrip == "yes" || ownTrip == "1", JsonUtil::Str(*j, "item"),
									Qty(*j), JsonUtil::Lower(JsonUtil::Str(*j, "price_each")) == "true",
									static_cast<float>(std::clamp(JsonUtil::Num(*j, "hours").value_or(0.0), 0.0, 24.0)), JsonUtil::Str(*j, "how", "any") },
								replyAt);
							serviceStarted = true;
							if (request == "follow" || request == "escort") {
								std::scoped_lock l(g_lock);
								g_lastMove[npcId] = Clock::now();
							}
						}
					}

					// The player handing something over; only listed when the NPC accepts it.
					// Things left for safekeeping are not gifts, even when the model lists them as given.
					// Nor is what changes hands in a deal: the coin paid, the goods sold.
					const bool keeping = offered && (request == "hold" || request == "retrieve" || request == "buy" || request == "sell" || request == "cook" || request == "mend" || request == "pay_fine" || request == "bribe");
					if (keeping && j->contains("player_gives")) {
						logger::info("{}: handed over for {}, not a gift", npcName, request);
					}
					if (auto gifts = j->find("player_gives"); !keeping && gifts != j->end() && SkyrimNet::ConfigBool("features.gifts", true)) {
						const json list = gifts->is_array() ? *gifts : gifts->is_object() ? json::array({ *gifts }) : json::array();
						std::vector<Promises::Gift> all;
						for (auto& gj : list) {
							if (auto g = Promises::GiftFromJson(gj); g && all.size() < 6) {
								// "Here's 100 for a room": that coin is the payment the service already takes.
								if (g->gold > 0 && serviceStarted &&
									(request == "rent_room" || request == "train" || request == "trade" || request == "heal" || request == "cure" || request == "drink")) {
									logger::info("{}: coin in the same line as {} is the payment, not a gift", npcName, request);
									continue;
								}
								// The thing must be the player's word ("here, have the venison"), now or earlier in the talk -
								// never only the NPC's own narration ("*she takes the book*"): then it's unnamed, and
								// the player picks what they hand over.
								if (g->gold <= 0 && !g->item.empty() && !ItemNamedIn(playerLineCopy, g->item) && !ItemNamedIn(earlierLines, g->item)) {
									logger::info("{}: gift '{}' named only in the NPC's line; the player picks", npcName, g->item);
									g->item.clear();
								}
								all.push_back(*g);
							}
						}
						if (!all.empty()) {
							Game::DebugNotify(std::format("{} accepts your gift", npcName));
							RunGift(npcId, std::move(all), replyAt);  // one popup for everything handed over
						}
					}

					if (!promises) {
						return;
					}
					// "Here, take this. Or grab the pick by the fire": one thing, said two ways. What's
					// handed over in this reply isn't also left out to take.
					std::vector<std::string> handed;
					for (auto& pj : promiseList) {
						if (auto p = Promises::FromJson(pj); p && p->type == "give_item" && p->when != "cancel") {
							handed.push_back(p->item);
						}
					}
					for (auto& pj : promiseList) {
						auto p = Promises::FromJson(pj);
						if (!p) {
							continue;
						}
						// The player turned it down ("no thanks", "I'd rather not"): an offer they refused isn't
						// carried out. Taking a promise back still is.
						if (p->when != "cancel" && PlayerSaidNo(playerLineCopy)) {
							logger::info("{}: {} was offered, but the player said no", npcName, Promises::Describe(*p));
							continue;
						}
						// "You're welcome back here anytime" from someone the player is staying with: no figure of
						// speech, but standing guest rights (Ronja, as the player left for the day).
						if (p->type == "offer_bed" && p->when != "cancel" && Ledger::HasBedLoan(npcId) && WelcomeBack(npcLineCopy)) {
							p->always = true;
							p->when = "now";
							logger::info("{}: welcome back any time, said by the host: guest rights", npcName);
						}
						// Only what was really said: a flirt or a figure of speech changes nothing in the world.
						else if (!Promises::Said(*p, playerLineCopy, npcLineCopy)) {
							logger::info("{}: {} wasn't really said (a figure of speech?); not kept", npcName, Promises::Describe(*p));
							continue;
						}
						if (p->type == "allow_take" && std::ranges::any_of(handed, [&](const std::string& h) { return Promises::NameScore(h, p->item.c_str()) > 0.0; })) {
							logger::info("{}: '{}' is already being handed over; not also left out to take", npcName, p->item);
							continue;
						}
						// A rented room already is the bed; don't also lend the innkeeper's own bed.
						if (p->type == "offer_bed" && serviceStarted && request == "rent_room") {
							continue;
						}
						// A shared meal already serves the food.
						if (p->type == "give_item" && serviceStarted && request == "share_meal") {
							continue;
						}
						// Resting together already sorts out the beds.
						if (p->type == "offer_bed" && serviceStarted && request == "rest") {
							continue;
						}
						// Care brings its own: the potion they heal with, the dish they cook, the cure.
						if ((p->type == "give_item" || p->type == "give_gold") && serviceStarted && (Services::IsCare(request) || request == "drink")) {
							logger::info("{}: {} is part of the {}, not a separate gift", npcName, Promises::Describe(*p), request);
							continue;
						}
						// Goods bought are paid for, not given ("here are your apples").
						if ((p->type == "give_item" || p->type == "give_gold") && serviceStarted && (request == "buy" || request == "sell")) {
							logger::info("{}: {} is part of the {}, not a gift", npcName, Promises::Describe(*p), request);
							continue;
						}
						// Things kept for the player change hands through hold/retrieve itself: "here's
						// your sword back" is not also a free sword, nor permission to take one.
						if ((p->type == "give_item" || p->type == "allow_take") && serviceStarted && (request == "retrieve" || request == "hold")) {
							logger::info("{}: {} is the player's own, handled by {}", npcName, Promises::Describe(*p), request);
							continue;
						}
						if (p->when == "cancel") {
							Ledger::Cancel(npcId, *p);
							continue;
						}
						// Repeated mentions ("the bed's yours", "rest now") are the same promise: already
						// waiting or just kept, so no second announcement or attempt.
						// A handover NOW of something owed for later settles it early, so it isn't a repeat.
						// Food, coin and items are never skipped here: "may I have another?" is a real second
						// ask, and the generosity budget decides (giving, or walking it back aloud).
						const bool handout = (p->type == "give_item" || p->type == "give_gold" || p->type == "allow_take") && !p->loan;
						const bool newWelcome = p->type == "offer_bed" && p->always && !Ledger::IsGuest(npcId);
						if (!newWelcome && ((Ledger::IsPending(npcId, *p) && p->when != "now") ||
											   (!handout && Promises::WasKept(npcId, *p)))) {
							logger::info("{}: '{}' already promised, not repeated", npcName, Promises::Describe(*p));
							continue;
						}
						Game::DebugNotify(std::format("{} promised {} ({})", npcName, Promises::Describe(*p), p->when));
						RunPromise(npcId, npcName, *p, replyAt);
					}
				};
				// Runs on a SkyrimNet worker: nothing a model writes may escape as an exception.
				auto safeReply = [onReply, npcName](const std::string& raw, bool success) {
					try {
						onReply(raw, success);
					} catch (const std::exception& e) {
						logger::error("classifier reply for {} could not be handled: {} ({})", npcName, e.what(), raw);
					} catch (...) {
						logger::error("classifier reply for {} could not be handled ({})", npcName, raw);
					}
				};

				// Prefer the fast "mundanematters_classifier" model slot; fall back to SkyrimNet's default.
				if (!SkyrimNet::SendPrompt("mundanematters_turn", "mundanematters_classifier", ctx, safeReply) &&
					!SkyrimNet::SendPrompt("mundanematters_turn", "", ctx, safeReply)) {
					logger::error("could not queue classifier for {}", npc->GetName());
				}
			});
		}

		// ---- SkyrimNet events (thread pool: no RE:: calls here) ----

		struct Event
		{
			std::string type;
			RE::FormID from = 0;
			RE::FormID to = 0;
			json data;
		};

		std::optional<Event> Parse(const std::string& raw)
		{
			auto j = json::parse(raw, nullptr, false);
			if (j.is_discarded()) {
				return std::nullopt;
			}
			Event e;
			e.type = JsonUtil::Str(j, "type");
			e.from = static_cast<RE::FormID>(JsonUtil::Num(j, "originatingActorFormId").value_or(0));
			e.to = static_cast<RE::FormID>(JsonUtil::Num(j, "targetActorFormId").value_or(0));
			if (j.contains("data") && j["data"].is_string()) {
				e.data = json::parse(j["data"].get<std::string>(), nullptr, false);
			}
			if (e.data.is_discarded() || !e.data.is_object()) {
				return std::nullopt;
			}
			return e;
		}

		void OnPlayerLine(const std::string& raw)
		{
			auto e = Parse(raw);
			if (!e || e->from != kPlayer) {
				return;
			}
			auto text = JsonUtil::Str(e->data, "dialogue");
			if (text.empty()) {
				return;
			}
			Game::NoteSpoke();
			if (Game::IsOwnLine(text)) {
				return;  // a popup choice MundaneMatters said for the player: already handled
			}
			logger::info("player: \"{}\" (target {:08X})", text, e->to);
			// "Let's see what I've promised": the promises book.
			if (SkyrimNet::ConfigBool("features.player_promises", true) && Pledges::AsksAboutPromises(text, true)) {
				std::thread([] {
					std::this_thread::sleep_for(std::chrono::milliseconds(1200));
					SKSE::GetTaskInterface()->AddTask(Pledges::OpenBook);
				}).detach();
			}
			// One of the NPC's real dialogue options, said in the player's own words: the vanilla line
			// answers instead of the AI (matched now, before the AI's reply gets going).
			// "Wait here", "go home", "you can go": the follow / escort of theirs ends.
			if (e->to != kPlayer && Services::IsStopWords(text)) {
				SKSE::GetTaskInterface()->AddTask([to = e->to] {
					if (const auto who = to ? to : Game::TalkTarget(); who && Services::WalkingWithPlayer(who)) {
						Services::StopWalking(who, "the player said so");
					}
				});
			}
			// "I got this for you" (no word for what): the player picks first; the NPC answers the real gift.
			if (e->to != kPlayer && SkyrimNet::ConfigBool("features.promises", true) && Promises::UnnamedGiftWords(text)) {
				SKSE::GetTaskInterface()->AddTask([to = e->to] {
					if (const auto who = to ? to : Game::TalkTarget()) {
						Promises::OfferUnnamedGift(RE::TESForm::LookupByID<RE::Actor>(who));
					}
				});
			}
			// "I'll move the bedroll here": the player's guest bedroll at a host's moves to their feet.
			// "Pick up my bedroll": packed away (and stays so). Checked first: "put away" isn't a move.
			if (Ledger::BedrollPickUpWords(text)) {
				SKSE::GetTaskInterface()->AddTask([] { Ledger::PickUpGuestBedroll(); });
			} else if (Ledger::BedrollHereWords(text)) {
				SKSE::GetTaskInterface()->AddTask([] { Ledger::MoveGuestBedroll(); });
			}
			// "This is your bed": the bed the player looks at (or picks) is theirs, at home.
			if (e->to != kPlayer && MoveIn::BedWords(text)) {
				SKSE::GetTaskInterface()->AddTask([to = e->to, text] {
					if (const auto who = to ? to : Game::TalkTarget(); who && MoveIn::Lives(who)) {
						MoveIn::AssignBed(who, text);
					}
				});
			}
			// "Move out": someone living with the player is asked (a popup) whether they go.
			if (e->to != kPlayer && MoveIn::MoveOutWords(text)) {
				SKSE::GetTaskInterface()->AddTask([to = e->to] {
					if (const auto who = to ? to : Game::TalkTarget(); who && MoveIn::Lives(who)) {
						MoveIn::AskMoveOut(who);
					}
				});
			}
			// Spoken to while asleep: they wake and answer. A goodbye ends time spent together.
			if (e->to != kPlayer) {
				SKSE::GetTaskInterface()->AddTask([to = e->to, text] {
					if (const auto who = to ? to : Game::TalkTarget()) {
						Services::WakeIfAsleep(who, text);
						if (Services::InCompany(who) && Services::Leaving(text)) {
							Services::EndCompany(who, "the player said goodbye", "");
						}
					}
				});
			}
			// A threat stays with whoever it was aimed at for a while.
			if (e->to != kPlayer && VanillaLines::IsThreat(text)) {
				SKSE::GetTaskInterface()->AddTask([to = e->to] {
					if (const auto who = to ? to : Game::TalkTarget()) {
						Brawls::Threatened(who);
					}
				});
			}
			if (e->to != kPlayer && SkyrimNet::ConfigBool("features.vanilla_lines", true)) {
				SKSE::GetTaskInterface()->AddTask([to = e->to, text] {
					// No target (typed with no one picked): the one they're facing / the only one near.
					if (const auto who = to ? to : Game::TalkTarget()) {
						VanillaLines::OnPlayerLine(who, text);
					}
				});
			}
			// Talk of meeting up: the real places nearby go into their bio before they answer.
			if (e->to != kPlayer && Services::MeetWords(text)) {
				SKSE::GetTaskInterface()->AddTask([to = e->to] {
					if (const auto who = to ? to : Game::TalkTarget()) {
						Services::NoteMeetTalk(RE::TESForm::LookupByID<RE::Actor>(who));
					}
				});
			}
			// Going home together: whose home is near (or far, or none) and how close they are, before they answer.
			if (e->to != kPlayer && SkyrimNet::ConfigBool("features.visit", true) && (Services::VisitWords(text) || JsonUtil::Lower(text).find("inside") != std::string::npos)) {
				SKSE::GetTaskInterface()->AddTask([to = e->to, text] {
					auto* a = RE::TESForm::LookupByID<RE::Actor>(to ? to : Game::TalkTarget());
					if (a && Services::MightVisit(a, text, "")) {
						Services::NoteVisitTalk(a);
					}
				});
			}
			// "How much for an apple?": the real price goes into the merchant's bio before they answer.
			if (SkyrimNet::ConfigBool("features.trade_talk", true) && Trade::AsksPrice(text)) {
				SKSE::GetTaskInterface()->AddTask([to = e->to, text] {
					if (const auto who = to ? to : Game::TalkTarget()) {
						Trade::Quote(who, text);
					}
				});
			}
			std::scoped_lock l(g_lock);
			const auto now = Clock::now();
			// "I need a room." + "And something to eat." sent back to back, before anyone answered:
			// one turn, so neither half is lost.
			if (g_pending && g_pending->answeredBy.empty() && now - g_pending->at < kJoinWindow &&
				(e->to == 0 || g_pending->target == 0 || e->to == g_pending->target)) {
				g_pending->text += " " + text;
				g_pending->at = now;
				if (e->to != 0) {
					g_pending->target = e->to;
				}
				return;
			}
			g_pending = PendingLine{ text, e->to, now, {} };
		}

		void OnNpcLine(const std::string& raw)
		{
			auto e = Parse(raw);
			if (!e || e->from == kPlayer || e->from == 0) {
				return;
			}
			// Whoever follows the player keeps walking with them through any talk - to the player or to
			// another NPC (SkyrimNet turns speakers to face their listener; it's taken off again).
			for (const auto who : { e->from, e->to }) {
				if (who == 0 || who == kPlayer) {
					continue;
				}
				std::thread([npc = who] {
					for (int ms : { 300, 1200, 2500, 5000 }) {
						std::this_thread::sleep_for(std::chrono::milliseconds(ms));
						SKSE::GetTaskInterface()->AddTask([npc] {
							if (auto* a = RE::TESForm::LookupByID<RE::Actor>(npc); a && !a->GetOccupiedFurniture().get() && Services::FollowsPlayer(a)) {
								Papyrus::CallStatic("SkyrimNetApi", "UnregisterPackage", nullptr, a, RE::BSFixedString("TalkToPlayer"));
								Papyrus::CallStatic("SkyrimNetApi", "UnregisterPackage", nullptr, a, RE::BSFixedString("TalkToNPC"));
							}
						});
					}
				}).detach();
			}
			// AI replies addressed to the player; NPC-to-NPC chatter is ignored. A reply to everyone
			// (group talk, no target) still counts when the player spoke to this NPC directly.
			if (e->to != kPlayer) {
				std::scoped_lock l(g_lock);
				if (e->to != 0 || !g_pending || g_pending->target != e->from) {
					return;
				}
			}
			auto text = JsonUtil::Str(e->data, "dialogue");
			auto speaker = JsonUtil::Str(e->data, "speaker", "NPC");
			Game::NoteSpoke();
			Services::NoteNpcSpoke(e->from);
			// SeverActions' actions go off with this line: its popups are noted, so ours don't ask again.
			Confirm::WatchOthers();
			// "I have to go": the time they were spending with the player ends (their own word).
			if (Services::Leaving(text)) {
				SKSE::GetTaskInterface()->AddTask([from = e->from] {
					if (Services::InCompany(from)) {
						Services::EndCompany(from, "they said they had to go", "");
					} else if (Services::IsFollowing(from)) {
						Services::StopWalking(from, "they said they had to go");
					}
				});
			}
			// The AI answered: a vanilla line matched after this is played without its audio (no double answer).
			VanillaLines::OnNpcLine(e->from, text);
			Gestures::OnNpcLine(e->from, text);
			// Walking with the player: SkyrimNet's "stop and face the player" package off again, so they keep
			// walking while they talk (it goes on as the line starts; taken off a few times to be sure).


			std::optional<std::string> asked;
			{
				std::scoped_lock l(g_lock);
				// Each NPC answers a player line once; their follow-up lines are not new answers.
				if (g_pending && Clock::now() - g_pending->at < kPairWindow &&
					(g_pending->target == 0 || g_pending->target == e->from) &&
					g_pending->answeredBy.insert(e->from).second) {
					asked = g_pending->text;
					Remember(e->from, "Player: " + *asked);
				}
				Remember(e->from, speaker + ": " + text);
			}

			// Parroting: their "reply" is the player's own line again (the model echoing). Cut it and let
			// them answer properly - once a minute at most, so it can't loop.
			if (asked && Echoes(*asked, text)) {
				static std::unordered_map<RE::FormID, Clock::time_point> lastFix;
				std::scoped_lock l(g_lock);
				if (auto& t = lastFix[e->from]; Clock::now() - t > std::chrono::seconds(60)) {
					t = Clock::now();
					logger::info("{} echoed the player's line; asking for a real answer", speaker);
					SKSE::GetTaskInterface()->AddTask([npc = e->from] {
						Papyrus::CallStatic("SkyrimNetApi", "PurgeDialogue", nullptr, false);
						if (auto* a = RE::TESForm::LookupByID<RE::Actor>(npc)) {
							Papyrus::CallStatic("SkyrimNetApi", "DirectNarration", nullptr,
								RE::BSFixedString(std::format("*{} answers what {} just said, in their own words.*", a->GetName(), Game::Player()->GetName())), a,
								static_cast<RE::Actor*>(Game::Player()));
						}
					});
					return;
				}
			}
			if (!asked) {
				// NPC spoke on their own (greeting, comment, narrated reaction): not an answer. It
				// may be their reaction to a gift.
				Promises::OnNpcReply(e->from, text);
				return;
			}
			logger::info("{} replied: \"{}\"", speaker, text);
			SKSE::GetTaskInterface()->AddTask([id = e->from, line = *asked] {
				Bonds::OnExchange(id);
				Pledges::OnExchange(id, line);
			});
			Classify(e->from, *asked, text, Clock::now());
		}
	}

	// ================================================================ self-test
	// Ticking "Run Self-Test" on the settings page sends canned exchanges through the real
	// classifier model and logs PASS/FAIL per case. Classification only: nothing is given,
	// moved, opened or said.

	namespace
	{
		struct TestCase
		{
			const char* name;
			std::vector<std::string> services;
			const char* npc;
			const char* player;
			const char* reply;
			const char* request = "none";   // expected service id
			const char* answer = "-";       // yes | no | free | - (not checked)
			std::vector<std::string> promises;  // expected promise types (exact set)
			const char* when = "";          // expected "when" of the first promise, "" = not checked
			bool always = false;            // expected always-welcome bed
			const char* place = "";         // expected text inside the escort place
			const char* gives = "none";     // none | named | gold | unnamed
			const char* offense = "";       // expected offense ("hurtful" / "none"), "" = not checked
			const char* apology = "";       // expected apology_accepted ("true" / "false"), "" = not checked
			std::vector<std::string> recent{};  // earlier lines ("{p}" = the player's name)
			const char* item = "";          // expected text inside "item", "" = not checked
			int qty = 0;                    // expected qty (-1 = all), 0 = not checked
			const char* loan = "";          // "promise" / "gift": the first one is a loan; "none": nothing is; "" = not checked
			const char* pledge = "";        // the player's own promise: "bring", "find", "pay", "visit", "other", "cancel", "none"; "" = not checked
			const char* sleep = "";         // who sleeps where: "player_floor", "npc_floor", "share", "none"; "" = not checked
		};

		json ServiceJson(const std::string& id)
		{
			static const std::unordered_map<std::string, std::pair<const char*, const char*>> kLabels{
				{ "trade", { "browse their goods in the trade window (looking around, nothing specific agreed)", "\"show me your wares\", \"what are you selling?\"" } },
				{ "buy", { "buy a specific thing from them NOW, the deal agreed in this exchange (set \"item\", \"qty\", \"price\")", "\"I'll take three apples\", \"fine, 20 for the dagger\"" } },
				{ "sell", { "sell them a specific thing of the PLAYER's NOW, the deal agreed in this exchange (set \"item\", \"qty\", \"price\")", "\"want these wolf pelts?\" - \"aye, 30 for the lot\", \"I'll sell you my iron sword\"" } },
				{ "rent_room", { "rent a room / bed for the night here (100 gold)", "\"I need a room\", \"got a bed for the night?\"" } },
				{ "train", { "pay them to train a skill", "\"can you teach me?\", \"train me\"" } },
				{ "follow", { "come along with the PLAYER now, walking behind them (the PLAYER leads)", "\"follow me\", \"come with me\"" } },
				{ "escort", { "lead the PLAYER to a named place, walking ahead so they can follow (the NPC leads; set \"place\")", "\"take me to the Bannered Mare\", \"show me the way to the temple\"" } },
				{ "share_meal", { "sit down and share a meal with the PLAYER now (go to a table, eat together)", "\"let's eat together\", \"join me for supper?\", \"care to share a meal?\"" } },
				{ "pay_fine", { "pay the PLAYER's bounty fine now", "\"I'll pay the fine\"" } },
				{ "bribe", { "the PLAYER offers the guard gold to look the other way (set \"price\" to the gold offered)", "\"here's 50 gold to look the other way\"" } },
				{ "persuade", { "the PLAYER talks the guard out of it with reasons or charm", "\"it was a misunderstanding\"" } },
				{ "intimidate", { "the PLAYER threatens or intimidates the guard into dropping it", "\"do you know who I am?\"" } },
				{ "perform", { "play music and/or sing for the PLAYER now (set \"item\" to the song or instrument named, \"\" for any; \"how\": \"sing\", \"play\" or \"any\")", "\"play us something\", \"sing me a song\", \"can you play Ragnar the Red?\"" } },
				{ "sit", { "sit down together NOW (set \"item\" to where: \"fire\", \"ground\", \"bench\", \"\" when not said; \"hours\" when the PLAYER says how long)", "\"sit with me a while\", \"let's sit by the fire\"" } },
				{ "move_in", { "move into the PLAYER's home to live there for good (the PLAYER picks which house in a popup)", "\"come live with me\", \"move in with me\"" } },
				{ "company", { "stay with the PLAYER for a while, keeping them company here (not walking somewhere: that's follow; not sitting down: that's sit)", "\"keep me company\", \"stay with me a while\", \"spend some time with me\"" } },
				{ "drink", { "sit down and have a drink together NOW (set \"item\" to the drink named, \"\" for any)", "\"a round of mead?\", \"have a drink with me\", \"fancy a pint?\"" } },
				{ "rest", { "go to bed / sleep now, together with or near the PLAYER (beds or bedrolls where they are)", "\"let's turn in\", \"time to get some rest\", \"let's make camp for the night\"" } },
				{ "hold", { "keep or carry some of the PLAYER's things for them for now, to give back later (set \"item\")", "\"hold this for me\", \"can you keep my sword safe?\"" } },
				{ "retrieve", { "give back things they are keeping or carrying for the PLAYER (set \"item\")", "\"can I have my sword back?\", \"give me back my stuff\"" } },
				{ "heal", { "heal the PLAYER's wounds now, with a potion or healing magic", "\"I'm hurt\", \"can you patch me up?\"" } },
				{ "cook", { "cook a meal for the PLAYER from the PLAYER's own ingredients, at a cooking pot (set \"item\" to the dish or ingredients named)", "\"can you cook these?\", \"make me a stew with this venison\"" } },
				{ "mend", { "improve / temper / sharpen / repair a weapon or armour of the PLAYER's at the forge (set \"item\")", "\"can you sharpen my sword?\", \"improve my armour\"" } },
				{ "cure", { "cure the PLAYER of a disease", "\"I've caught something\", \"I feel sick\"" } },
			};
			const auto& [label, examples] = kLabels.at(id);
			return { { "id", id }, { "label", label }, { "examples", examples } };
		}

		const std::vector<TestCase>& TestCases()
		{
			using V = std::vector<std::string>;
			const V shop{ "trade", "buy", "sell" }, inn{ "trade", "buy", "sell", "rent_room" }, move{ "follow", "escort" }, none{};
			static const std::vector<TestCase> cases{
				// trade
				{ "trade plain", shop, "Belethor", "show me your wares", "Take a look, everything's for sale.", "trade", "yes" },
				{ "trade inside RP line", shop, "Belethor", "i got something to sell, but first, show me your wares", "Of course! Browse away.", "trade", "yes" },
				{ "trade typo", shop, "Belethor", "wat u got for sale", "Weapons, armor, potions. Have a look.", "trade", "yes" },
				{ "trade refused", shop, "Belethor", "what've you got, old man?", "Not for the likes of you, milk-drinker. Out.", "trade", "no" },
				{ "trade only mentioned", shop, "Belethor", "I traded with your brother yesterday.", "Lucky you. He overcharges.", "none" },
				{ "purchase is not a gift", shop, "Belethor", "I'll buy this iron sword, here's 100.", "Pleasure doing business.", "buy", "yes", {}, "", false, "", "none", "", "", {}, "sword" },
				// buying and selling by talking
				{ "price ask is talk", shop, "Belethor", "how much for an apple?", "Apples are 3 gold each.", "none" },
				{ "buy after a quote", shop, "Belethor", "okay, I'll take 3", "Here you are, that's 9 gold.", "buy", "yes", {}, "", false, "", "none", "", "",
					{ "{p}: how much for an apple?", "Belethor: Apples are 3 gold each." }, "apple", 3 },
				{ "buy typo", shop, "Belethor", "ill tkae 2 of thos sweetrols", "Two sweetrolls, coming right up.", "buy", "yes", {}, "", false, "", "none", "", "", {}, "sweetroll", 2 },
				{ "haggle countered", shop, "Belethor", "I'll give you 15 for the steel dagger.", "Ha! 18, and not a coin less.", "buy", "no" },
				{ "haggle accepted", shop, "Belethor", "fine, 18 it is", "Pleasure doing business.", "buy", "yes", {}, "", false, "", "none", "", "",
					{ "{p}: I'll give you 15 for the steel dagger.", "Belethor: Ha! 18, and not a coin less." }, "dagger" },
				{ "not for sale", shop, "Belethor", "I'll take that amulet.", "That one's not for sale, friend.", "buy", "no" },
				{ "sell pelts", shop, "Belethor", "want these wolf pelts?", "Aye, I'll give you 30 for the lot.", "sell", "yes", {}, "", false, "", "none", "", "", {}, "pelt", -1 },
				{ "sell refused", shop, "Belethor", "wanna buy my old boots?", "I've no use for worn-out boots.", "sell", "no" },
				// rooms and training
				{ "room with price", inn, "Hulda", "i need a room", "That'll be 100 septims, coin up front.", "rent_room", "yes" },
				{ "room full", inn, "Hulda", "Got a room?", "Sorry, we're full up tonight.", "rent_room", "no" },
				{ "room free", inn, "Hulda", "Got a room?", "For the hero of Whiterun? On the house.", "rent_room", "free" },
				{ "room plus free stew", inn, "Hulda", "A room, and some stew?", "Room's 100, the stew's on me. *ladles a bowl*", "rent_room", "yes", { "give_item" } },
				{ "training", { "train" }, "Aela", "can you teach me archery?", "Sit, let's begin. 300 a lesson.", "train", "yes" },
				// follow / escort
				{ "follow", move, "Lydia", "follow me", "Lead on.", "follow", "yes" },
				{ "follow typo", move, "Lydia", "folow me pls", "Right behind you.", "follow", "yes" },
				{ "escort named place", move, "Ronja", "Take me to the Bannered Mare.", "This way, follow me.", "escort", "yes", {}, "", false, "bannered" },
				{ "escort kind of place", move, "Ronja", "lead me to the brewry", "Of course, it's not far.", "escort", "yes", {}, "", false, "honningbrew" },
				{ "escort someone's home", move, "Ronja", "can you show me where hulda lives?", "Come, I'll take you.", "escort", "yes", {}, "", false, "home:" },
				{ "escort their home", move, "Ronja", "take me to your place", "Very well, my house is this way.", "escort", "yes", {}, "", false, "home" },
				{ "escort refused", move, "Ronja", "Take me to Riverwood.", "I've work to do. Follow the road south.", "escort", "no" },
				{ "directions only", move, "Ronja", "Show me the way to the temple?", "Up the stairs, you can't miss it.", "escort", "no" },
				{ "escort later", move, "Ronja", "Will you show me the inn tomorrow?", "Sure, tomorrow.", "none" },
				{ "small talk", move, "Ronja", "Nice weather.", "Aye, for once.", "none" },
				// shared meals (a friend)
				{ "meal together", { "share_meal" }, "Ronja", "Let's eat together?", "I'd like that. Sit with me, the stew's still warm.", "share_meal", "yes" },
				{ "meal casual + typo", { "share_meal" }, "Ronja", "wanna grab a bite wit me? im starvin", "Ha, come on then, pull up a stool.", "share_meal", "yes" },
				{ "meal declined", { "share_meal" }, "Ronja", "Join me for supper?", "Not tonight, I've already eaten.", "share_meal", "no" },
				{ "meal later", { "share_meal" }, "Ronja", "Dinner tomorrow?", "Tomorrow, then. I'll cook.", "none" },
				// resting together (a companion)
				{ "rest together", { "rest" }, "Lydia", "It's late. Let's turn in for the night.", "Aye, my Thane. I'll bed down by the fire.", "rest", "yes" },
				{ "rest typo", { "rest" }, "Lydia", "im so tierd, lets slep", "Get some sleep, I'll be right here.", "rest", "yes" },
				{ "rest declined", { "rest" }, "Lydia", "Shall we make camp here?", "Not here, it's too exposed. Let's push on.", "rest", "no" },
				{ "rest later", { "rest" }, "Lydia", "We'll rest when we reach Riverwood.", "Fine by me.", "none" },
				// a place to stay, judged on both lines (2026-10-12): the real verdict is checked by the live code
				{ "stay as long as you need", { "sit", "company" }, "Ronja",
					"i know you said i can stay here for couple days and i have worned my welcome, but still havent found place to stay",
					"You've not worn out your welcome, Breton. Stay as long as you need.", "none", "-", { "offer_bed" } },
				{ "stay a few more days (typo)", { "sit", "company" }, "Ronja", "so i can stay at you place for some more days?",
					"Aye, Breton. You can stay for a few more days. I meant what I said.", "none", "-", { "offer_bed" } },
				{ "door's always open as a pleasantry", { "drink" }, "Hulda", "thanks for the drink", "Any time. My door's always open, friend.", "none", "-", {} },
				// a meeting at a place and time: IntelEngine takes it from there
				{ "meet at dawn", { "follow", "company" }, "Yatul", "so tomorrow then...", "Tomorrow. Dawn. East ridge. Don't keep me waiting, Breton.", "none", "-",
					{ "meet" }, "later" },
				{ "a farewell is no meeting", { "follow", "company" }, "Yatul", "farewell then", "Safe travels, Breton. Mind the wolves.", "none", "-", {} },
				{ "where to find me is no meeting", { "trade" }, "Lucan Valerius", "thanks, I'll come back for more", "You know where to find me.", "none", "-", {} },
				{ "most evenings is no meeting", { "drink" }, "Mikael", "where can I hear you play again?", "I'm at the Bannered Mare most evenings.", "none", "-", {} },
				// one bed at her place: who sleeps where, as she says or agrees
				{ "she offers the floor", { "sit", "company" }, "Ronja", "where do I sleep then?", "Only the one bed, but there's a spare bedroll. The floor by the hearth is yours.",
					"none", "-", {}, "", false, "", "-", "", "", {}, "", 0, "", "", "player_floor" },
				{ "player takes the floor", { "sit", "company" }, "Ronja", "it's your bed, I'll take the floor", "Suit yourself. I'll fetch you some furs.",
					"none", "-", {}, "", false, "", "-", "", "", {}, "", 0, "", "", "player_floor" },
				{ "she gives up her bed", { "sit", "company" }, "Ronja", "only one bed?", "Take it. I'll sleep by the fire, I'm used to the hard ground.",
					"none", "-", {}, "", false, "", "-", "", "", {}, "", 0, "", "", "npc_floor" },
				{ "player claims the bed, she refuses", { "sit", "company" }, "Ronja", "I'll take the bed, you sleep on the floor", "Ha! In my own house? No. The floor's yours, guest.",
					"none", "-", {}, "", false, "", "-", "", "", {}, "", 0, "", "", "player_floor" },
				{ "keeping watch is no arrangement", { "sit", "company" }, "Ronja", "only one bed, huh", "Take it. I'll keep watch by the door.",
					"none", "-", {}, "", false, "", "-", "", "", {}, "", 0, "", "", "npc_floor" },
				{ "player sits down on their own", { "sit", "rest", "company" }, "Ronja", "**i take a sit and admire her**", "You're staring, Breton. Worry about your own breakfast.", "none" },
				{ "sit a bit, tired (not sleep)", { "sit", "rest", "company" }, "Ronja", "lets sit a bit here i am tired", "Aye, fair enough. Rest then. I'll keep watch.", "sit", "yes" },
				{ "no, come on, sit", { "sit", "rest", "company" }, "Ronja", "no common sit next to me we can relax", "Ah, you're right. *settles onto the log beside you*", "sit", "yes" },
				{ "innkeeper bed is a room", { "trade", "rent_room", "rest" }, "Hulda", "I need a bed for the night.", "Room's 100 gold.", "rent_room", "yes" },
				// leaving things with someone
				{ "hold vague", { "hold" }, "Lydia", "hold this for me", "Of course, hand it over.", "hold", "yes" },
				{ "hold named, in a story", { "hold" }, "Ronja", "can you keep my sword safe while I go into the barrow?", "I'll guard it with my life.", "hold", "yes" },
				{ "hold refused", { "hold" }, "Ronja", "carry my stuff for me?", "I'm not your pack mule.", "hold", "no" },
				{ "a gift is not hold", { "hold" }, "Ronja", "Here, take this ring, it's yours to keep.", "Oh! Thank you!", "none", "-", {}, "", false, "", "named" },
				{ "get it back", { "hold", "retrieve" }, "Ronja", "can i have my sword back?", "Here you go, safe and sound.", "retrieve", "yes" },
				// trust: hurt, and mended
				{ "cruel words", none, "Ronja", "You're a worthless old hag, nobody wants you around.", "How dare you! Get out of my sight.", "none", "-", {}, "", false, "", "none", "hurtful", "false" },
				{ "friendly teasing", none, "Ronja", "You snore like a mammoth, you know that?", "Ha! And you eat like one.", "none", "-", {}, "", false, "", "none", "none", "false" },
				{ "apology accepted", none, "Ronja", "I'm truly sorry for what I said. I didn't mean it.", "...Alright. I forgive you. Just don't do it again.", "none", "-", {}, "", false, "", "none", "none", "true" },
				{ "apology rejected", none, "Ronja", "Sorry about the other day.", "Sorry doesn't fix it. Leave me be.", "none", "-", {}, "", false, "", "none", "none", "false" },
				// a key to their home
				{ "key offered", none, "Ronja", "I'll miss you.", "Here, take my spare key. Come by whenever you like.", "none", "-", { "give_key" }, "now" },
				{ "key asked", none, "Ronja", "Could I have a key to your place?", "Of course, my love. Here.", "none", "-", { "give_key" }, "now" },
				{ "key later", none, "Ronja", "Can I get a key to your house?", "I'll have one cut for you by tomorrow.", "none", "-", { "give_key" }, "later" },
				{ "key taken back", none, "Ronja", "What's wrong?", "I want my key back. Now.", "none", "-", { "give_key" }, "cancel" },
				{ "a chest key is an item", none, "Ronja", "Got the key to that old chest?", "Here, take it.", "none", "-", { "give_item" }, "now" },
				// promises
				{ "coins and soup", none, "Ronja", "Thanks for the help.", "Least I can do. Take 10 coins and a bowl of cabbage soup.", "none", "-", { "give_gold", "give_item" }, "now" },
				{ "free stew", none, "Ronja", "I'm starving.", "Sit, I'll get you a bowl of stew. *hands it over*", "none", "-", { "give_item" }, "now" },
				{ "no more stew", none, "Ronja", "may I have another?", "No more, you've had yours.", "none", "-", {} },
				{ "sale is not a promise", none, "Ronja", "Any food?", "Bread's 2 gold.", "none", "-", {} },
				{ "bed tonight", none, "Ronja", "Can I crash here?", "The bed in the back is yours tonight.", "none", "-", { "offer_bed" } },
				{ "floor is not a bed", none, "Ronja", "Can I use your bed?", "One night, on the floor, and keep your hands to yourself.", "none", "-", {} },
				{ "always welcome", none, "Viola", "Farewell, Viola.", "Farewell, and you're always welcome at my place.", "none", "-", { "offer_bed" }, "", true },
				{ "a flirt is no bed", none, "Haelga", "farewell fair maiden, I'd like to see you more intimately someday", "Later, then. My door will be open for you, Breton.",
					"none", "-", {}, "", false },
				{ "reward tomorrow", none, "Ronja", "Will you pay me?", "Come back tomorrow and I'll have 100 gold for you.", "none", "-", { "give_gold" }, "later" },
				{ "conditional reward", none, "Ronja", "I'll clear the mine.", "Do that and there's 200 gold in it for you.", "none", "-", {} },
				{ "promise taken back", none, "Ronja", "So, my 100 gold tomorrow?", "About that... I can't pay you after all.", "none", "-", { "give_gold" }, "cancel" },
				{ "permission to take", none, "Ronja", "Mind if I take that bottle of wine?", "Go ahead, I've plenty.", "none", "-", { "allow_take" } },
				{ "vague handover", none, "Ronja", "Hello.", "Here, take this.", "none", "-", {} },
				// gifts
				{ "gift named", none, "Ronja", "Here, take this health potion, you look hurt.", "Thank you, that's kind.", "none", "-", {}, "", false, "", "named" },
				{ "gift gold", none, "Ronja", "Have 50 gold for your trouble.", "Generous of you!", "none", "-", {}, "", false, "", "gold" },
				{ "gift unnamed", none, "Ronja", "I got you something.", "For me? What is it?", "none", "-", {}, "", false, "", "unnamed" },
				{ "gift refused", none, "Ronja", "Take this sword.", "I can't accept that.", "none", "-", {}, "", false, "", "none" },
				// guards: words with weight
				{ "pay the fine", { "pay_fine", "bribe", "persuade", "intimidate" }, "Whiterun Guard", "Fine, I'll pay the fine.", "Smart choice. Hand it over.", "pay_fine", "yes" },
				{ "bribe with an amount", { "pay_fine", "bribe", "persuade", "intimidate" }, "Whiterun Guard", "Here's 50 gold to look the other way.", "...I didn't see a thing. Move along.", "bribe", "yes",
					{}, "", false, "", "-" },
				{ "bribe refused", { "pay_fine", "bribe", "persuade", "intimidate" }, "Whiterun Guard", "Maybe 10 septims makes you forget?", "Are you trying to bribe me? Pay the fine or go to jail.", "bribe", "no" },
				{ "persuade", { "pay_fine", "bribe", "persuade", "intimidate" }, "Whiterun Guard", "it was a misunderstanding, i was just borrowing it", "Hmm. Alright, I'll let it go this time.", "persuade", "yes" },
				{ "intimidate", { "pay_fine", "bribe", "persuade", "intimidate" }, "Whiterun Guard", "Do you know who I am? Walk away.", "I... fine. Just go.", "intimidate", "yes" },
				{ "guard small talk", { "pay_fine", "bribe", "persuade", "intimidate" }, "Whiterun Guard", "Any trouble lately?", "Nothing we can't handle.", "none" },
				// a promised pie, already handed over: talk about it is not a second one
				{ "pie already given, thanks", { "drink", "share_meal" }, "Ronja", "thanks...this smells good. ill save it for later.", "Smart to savor it. That pie keeps well for a good day or so.",
					"none", "-", {}, "", false, "", "none", "", "", { "Ronja: Morning, here's that pie I promised.", "(moments ago, Ronja handed {p} Mystery Pie)" } },
				{ "pie already given, says not yet", { "drink", "share_meal" }, "Ronja", "you havent given me that yet...", "That promise - I keep my word. *lifts the covered pie dish*",
					"none", "-", {}, "", false, "", "none", "", "", { "Ronja: That pie still warm enough for you?", "(moments ago, Ronja handed {p} Mystery Pie)" } },
				// a song
				{ "bard asked for a song", { "trade", "buy", "sell", "rent_room", "drink", "perform" }, "Mikael", "Play us something, bard!", "Ha! For you, a little Ragnar the Red.", "perform", "yes" },
				{ "bard names the song", { "drink", "perform" }, "Mikael", "can u play ragnar the red", "As you wish!", "perform", "yes", {}, "", false, "", "none", "", "", {}, "ragnar" },
				{ "bard refuses", { "drink", "perform" }, "Mikael", "Play me a song.", "Not for you, after what you said. Find another bard.", "perform", "no" },
				{ "companion sings for the road", { "sit", "perform", "follow" }, "Lydia", "Sing something for the road, Lydia.", "Hm. Alright, but don't laugh.", "perform", "yes" },
				{ "by the fire, sing me a song", { "sit", "perform", "drink" }, "Ronja", "can you sing me a song?", "Mm... my grandmother's lullaby, then.", "perform", "yes",
					{}, "", false, "", "none", "", "", { "{p}: Let's sit by the fire.", "Ronja: Mm, that sounds lovely." } },
				{ "play is everyday", { "perform", "sit" }, "Ronja", "Don't play games with me.", "I'm not!", "none" },
				{ "a song later", { "perform" }, "Ronja", "Sing for me sometime?", "Maybe someday.", "none" },
				// sitting a while
				{ "sit a while", { "sit", "rest", "drink" }, "Lydia", "Sit with me a while?", "Gladly, my Thane. My feet could use the rest.", "sit", "yes" },
				{ "move in", { "move_in", "company", "follow" }, "Svana", "would you like to come live with me?", "I... yes. I'd like that very much.", "move_in", "yes" },
				{ "move in refused", { "move_in", "company", "follow" }, "Svana", "move in with me", "I can't leave Aunt Haelga alone with the bunkhouse.", "move_in", "no" },
				{ "keep me company", { "sit", "company", "follow" }, "Haelga", "stay with me a while, keep me company", "Gladly. I've nowhere else to be.", "company", "yes" },
				{ "spend time refused", { "sit", "company", "follow" }, "Haelga", "spend some time with me?", "Not tonight, I've the books to do.", "company", "no" },
				{ "sit by the fire", { "sit", "rest", "drink" }, "Ronja", "lets sit by the fire for a bit", "Mm, that sounds lovely.", "sit", "yes", {}, "", false, "", "none", "", "", {}, "fire" },
				{ "sit till sunset", { "sit", "rest", "drink" }, "Ronja", "Stay with me till sunset?", "I'd like that.", "sit", "yes" },
				{ "where shall we sit (asks back)", { "sit", "rest", "drink" }, "Ronja", "Let's sit for a while.", "Sure, where? By the fire or out on the grass?", "sit", "no" },
				{ "answering where", { "sit", "rest", "drink" }, "Ronja", "by the fire", "Come on then, it's warm there.", "sit", "yes", {}, "", false, "", "none", "", "",
					{ "{p}: Let's sit for a while.", "Ronja: Sure, where? By the fire or out on the grass?" }, "fire" },
				{ "sitting is not sleeping", { "sit", "rest" }, "Lydia", "It's late, let's turn in.", "Aye, I'll bed down.", "rest", "yes" },
				{ "sit later", { "sit", "rest" }, "Ronja", "We should sit and talk sometime.", "Aye, sometime.", "none" },
				// a drink together
				{ "round of mead", { "drink", "share_meal" }, "Ronja", "A round of mead?", "Ha! Now you're talking. Sit, sit.", "drink", "yes", {}, "", false, "", "none", "", "", {}, "mead" },
				{ "drink typo", { "drink", "share_meal" }, "Lydia", "wanna grab a drnk with me?", "I could use one, my Thane.", "drink", "yes" },
				{ "drink declined", { "drink", "share_meal" }, "Ronja", "Join me for a drink?", "Not tonight, I've had my fill.", "drink", "no" },
				{ "a drink is not a meal", { "drink", "share_meal" }, "Ronja", "Let's have supper together.", "I'd like that.", "share_meal", "yes" },
				{ "asking for a mead is a promise", { "drink", "share_meal" }, "Ronja", "Can I have a mead?", "Here, take one from the barrel.", "none", "-", { "give_item" }, "now" },
				{ "buying mead at the bar", { "trade", "buy", "sell", "rent_room", "drink" }, "Hulda", "I'll buy two meads.", "That's 20 gold.", "buy", "yes" },
				// care
				{ "patch me up", { "heal", "cook", "share_meal" }, "Lydia", "ugh, that draugr got me good. im bleeding", "Hold still, let me see that.", "heal", "yes" },
				{ "hurt but refused", { "heal", "cook" }, "Ronja", "I'm hurt, can you help?", "I'm no healer. Try the temple.", "heal", "no" },
				{ "a sore back is small talk", { "heal", "cook" }, "Ronja", "My back hurts from all this walking.", "Ha, you're getting old.", "none" },
				{ "cook these", { "heal", "cook", "share_meal" }, "Ronja", "Could you cook this venison for me?", "Of course, give it here, I'll make a stew.", "cook", "yes",
					{}, "", false, "", "none", "", "", {}, "venison" },
				{ "cook is not a shared meal", { "cook", "share_meal" }, "Ronja", "Let's eat together tonight.", "I'd like that.", "share_meal", "yes" },
				{ "sharpen my sword", { "trade", "buy", "sell", "mend", "heal", "cook" }, "Alvor", "can u sharpen my iron sword?", "Aye, hand it over, I'll put an edge on it.", "mend", "yes",
					{}, "", false, "", "none", "", "", {}, "sword" },
				{ "improve is not buy", { "trade", "buy", "sell", "mend" }, "Alvor", "Could you improve my steel armor?", "For a few coins, sure.", "mend", "yes" },
				{ "cure disease", { "cure", "heal" }, "Danica", "I think I caught something in that cave.", "Come, let me pray over you.", "cure", "yes" },
				{ "healing at the temple", { "cure", "heal" }, "Danica", "I'm badly wounded.", "Lie down, I'll tend to you.", "heal", "yes" },
				// the player's own promises
				{ "promise to bring", none, "Ronja", "I'll bring you a bottle of mead tomorrow.", "I'll hold you to that!", "none", "-", {}, "", false, "", "none",
					"", "", {}, "", 0, "", "bring" },
				{ "promise to find", none, "Ronja", "dont worry ill find ur ring", "Would you? Thank you!", "none", "-", {}, "", false, "", "none",
					"", "", {}, "", 0, "", "find" },
				{ "promise to pay back", none, "Ronja", "I'll pay you back the 50 gold, I swear.", "You'd better.", "none", "-", {}, "", false, "", "none",
					"", "", {}, "", 0, "", "pay" },
				{ "promise to visit", none, "Ronja", "I'll come by tomorrow evening.", "I'll leave the door open.", "none", "-", {}, "", false, "", "none",
					"", "", {}, "", 0, "", "visit" },
				{ "a maybe is not a promise", none, "Ronja", "I might stop by sometime.", "Sure.", "none", "-", {}, "", false, "", "none",
					"", "", {}, "", 0, "", "none" },
				{ "taking a promise back", none, "Ronja", "Sorry, I can't bring the mead after all.", "Oh. Alright.", "none", "-", {}, "", false, "", "none",
					"", "", {}, "", 0, "", "cancel" },
				{ "yes to a favour", none, "Ronja", "Sure, I'll bring some.", "Oh, thank you! I've been craving it.", "none", "-", {}, "", false, "", "none",
					"", "", { "Ronja: Before you go, could you bring me some salmon next time? I've run out." }, "", 0, "", "bring" },
				{ "deliver to her sister", none, "Ronja", "Of course, I'll see she gets it.", "Thank you!", "none", "-", {}, "", false, "", "none",
					"", "", { "Ronja: Would you take this book to my sister Ysolda if you pass the market?" }, "", 0, "", "deliver" },
				{ "NPC's promise is not the player's", none, "Ronja", "Will you be there tomorrow?", "I'll bring you some bread tomorrow, love.", "none", "-",
					{ "give_item" }, "later", false, "", "none", "", "", {}, "", 0, "", "none" },
				// borrowing and lending
				{ "borrow asked", shop, "Alvor", "can i borrow ur pickaxe? need it for the mine", "Aye, take it. Bring it back when you're done.", "none", "-",
					{ "give_item" }, "now", false, "", "none", "", "", {}, "", 0, "promise" },
				{ "lend offered", none, "Ronja", "I'm heading into Bleak Falls Barrow.", "Take my lantern, it's dark down there. I want it back tomorrow, mind.", "none", "-",
					{ "give_item" }, "now", false, "", "none", "", "", {}, "", 0, "promise" },
				{ "lend the one by the forge", shop, "Alvor", "I need a pick for the mine.", "There's one by the forge. Take it, just return it after.", "none", "-",
					{ "allow_take" }, "", false, "", "none", "", "", {}, "", 0, "promise" },
				{ "keep it", none, "Ronja", "Mind if I keep the lantern?", "Keep it, I've got another.", "none", "-",
					{ "give_item" }, "now", false, "", "none", "", "", {}, "", 0, "none" },
				{ "player lends", { "hold" }, "Lydia", "Here, use my spare bow for the road, give it back later.", "Thank you, I'll take good care of it.", "none", "-",
					{}, "", false, "", "named", "", "", {}, "", 0, "gift" },
				{ "giving it back", { "hold", "retrieve" }, "Ronja", "Here's your lantern back, thanks.", "Ah, good, I was wondering.", "none", "-",
					{}, "", false, "", "named", "", "", {}, "", 0, "none" },
				{ "a gift is not a loan", none, "Ronja", "Here, this necklace is for you.", "Oh, it's beautiful! Thank you.", "none", "-",
					{}, "", false, "", "named", "", "", {}, "", 0, "none" },
			};
			return cases;
		}

		std::string Judge(const TestCase& c, const json& j)
		{
			std::vector<std::string> problems;
			const auto request = NormalizeRequest(JsonUtil::Str(j, "request", "none"));
			const auto response = NormalizeResponse(JsonUtil::Str(j, "response", "accept"));
			if (request != c.request) {
				problems.push_back(std::format("request {} (wanted {})", request, c.request));
			}
			const std::string answer = c.answer;
			if (request == c.request && std::string(c.request) != "none" && answer != "-") {
				const bool ok = answer == "yes" ? (response == "accept" || response == "free")
				              : answer == "no"  ? (response == "refuse" || response == "counter")
				                                : response == answer;
				if (!ok) {
					problems.push_back(std::format("response {} (wanted {})", response, answer));
				}
			}
			std::vector<std::string> types;
			std::optional<Promises::Promise> first;
			for (auto& pj : PromiseList(j)) {
				if (auto p = Promises::FromJson(pj)) {
					types.push_back(p->type);
					if (!first) {
						first = *p;
					}
				}
			}
			auto want = c.promises;
			std::ranges::sort(types);
			std::ranges::sort(want);
			if (types != want) {
				std::string got, exp;
				for (auto& t : types) got += t + " ";
				for (auto& t : want) exp += t + " ";
				problems.push_back(std::format("promises [{}] (wanted [{}])", got, exp));
			} else if (first) {
				if (*c.when && first->when != c.when) {
					problems.push_back(std::format("when {} (wanted {})", first->when, c.when));
				}
				if (first->always != c.always) {
					problems.push_back(std::format("always {} (wanted {})", first->always, c.always));
				}
			}
			if (*c.place) {
				const auto place = JsonUtil::Lower(JsonUtil::Str(j, "place"));
				if (place.find(c.place) == std::string::npos) {
					problems.push_back(std::format("place '{}' (wanted it to contain '{}')", place, c.place));
				}
			}
			std::string gives = "none";
			if (auto it = j.find("player_gives"); it != j.end()) {
				const json list = it->is_array() ? *it : it->is_object() ? json::array({ *it }) : json::array();
				for (auto& gj : list) {
					if (auto g = Promises::GiftFromJson(gj)) {
						gives = g->gold > 0 ? "gold" : JsonUtil::Words(g->item).empty() ? "unnamed" : "named";
						break;
					}
				}
			}
			if (std::string_view(c.gives) != "-" && gives != c.gives) {  // "-": not checked
				problems.push_back(std::format("player_gives {} (wanted {})", gives, c.gives));
			}
			if (*c.offense) {
				const auto o = JsonUtil::Lower(JsonUtil::Str(j, "offense", "none"));
				const bool hurt = o.find("hurt") != std::string::npos;
				if (hurt != (std::string_view(c.offense) == "hurtful")) {
					problems.push_back(std::format("offense {} (wanted {})", o, c.offense));
				}
			}
			if (*c.apology) {
				const auto a = JsonUtil::Lower(JsonUtil::Str(j, "apology_accepted", "false"));
				const bool yes = a == "true" || a == "yes" || a == "1";
				if (yes != (std::string_view(c.apology) == "true")) {
					problems.push_back(std::format("apology_accepted {} (wanted {})", a, c.apology));
				}
			}
			if (*c.item) {
				const auto item = JsonUtil::Lower(JsonUtil::Str(j, "item"));
				if (item.find(c.item) == std::string::npos) {
					problems.push_back(std::format("item '{}' (wanted it to contain '{}')", item, c.item));
				}
			}
			if (*c.loan) {
				auto isLoan = [](const json& o) {
					const auto v = JsonUtil::Lower(JsonUtil::Str(o, "loan"));
					return v == "true" || v == "yes" || v == "1";
				};
				bool promiseLoan = false, giftLoan = false;
				for (auto& pj : PromiseList(j)) {
					promiseLoan = promiseLoan || isLoan(pj);
				}
				if (auto it = j.find("player_gives"); it != j.end()) {
					const json list = it->is_array() ? *it : it->is_object() ? json::array({ *it }) : json::array();
					for (auto& gj : list) {
						giftLoan = giftLoan || isLoan(gj);
					}
				}
				const std::string want = c.loan;
				const bool ok = want == "promise" ? promiseLoan && !giftLoan : want == "gift" ? giftLoan && !promiseLoan : !promiseLoan && !giftLoan;
				if (!ok) {
					problems.push_back(std::format("loan: promise {} gift {} (wanted {})", promiseLoan, giftLoan, want));
				}
			}
			if (*c.pledge) {
				std::string got = "none";
				if (auto it = j.find("player_promises"); it != j.end()) {
					const json list = it->is_array() ? *it : it->is_object() ? json::array({ *it }) : json::array();
					for (auto& o : list) {
						if (o.is_object()) {
							got = JsonUtil::Lower(JsonUtil::Str(o, "when")).find("cancel") != std::string::npos ? "cancel" : JsonUtil::Lower(JsonUtil::Str(o, "type"));
							break;
						}
					}
				}
				if (got.find(c.pledge) == std::string::npos) {
					problems.push_back(std::format("player_promises {} (wanted {})", got, c.pledge));
				}
			}
			if (c.qty != 0 && Qty(j) != c.qty) {
				problems.push_back(std::format("qty {} (wanted {})", Qty(j), c.qty));
			}
			if (*c.sleep) {
				auto got = SleepOf(JsonUtil::Str(j, "sleep"));
				if (got.empty()) {
					got = "none";
				}
				if (got != c.sleep) {
					problems.push_back(std::format("sleep {} (wanted {})", got, c.sleep));
				}
			}
			std::string out;
			for (auto& p : problems) {
				out += (out.empty() ? "" : "; ") + p;
			}
			return out;
		}

		std::atomic_int g_testsLeft{ 0 };
		std::atomic_int g_testsPassed{ 0 };
		std::atomic_int g_testsUnreached{ 0 };
		std::atomic_size_t g_nextCase{ 0 };
		std::string g_testPlayer;
		constexpr std::size_t kTestsAtOnce = 4;  // a burst of 100+ calls trips the provider's in-flight limit

		void FinishCase()
		{
			if (--g_testsLeft == 0) {
				const int total = static_cast<int>(TestCases().size());
				const int unreached = g_testsUnreached.load();
				logger::info("SELFTEST DONE: {}/{} passed{}", g_testsPassed.load(), total - unreached,
					unreached ? std::format(" ({} couldn't reach the model and weren't judged)", unreached) : "");
				Game::Notify(std::format("Mundane Matters self-test: {}/{} passed{} (details in MundaneMatters.log)", g_testsPassed.load(), total - unreached,
					unreached ? std::format(", {} unreached", unreached) : ""));
			}
		}

		void SendCase(std::size_t i);

		// The next case in line, if any: a few at a time, each finished one starting the next.
		void SendNext()
		{
			const auto i = g_nextCase++;
			if (i < TestCases().size()) {
				SendCase(i);
			}
		}

		void SendCase(std::size_t i)
		{
			const auto& c = TestCases()[i];
			json ctx;
			ctx["playerName"] = g_testPlayer;
			ctx["npcName"] = c.npc;
			ctx["playerLine"] = c.player;
			ctx["npcLine"] = c.reply;
			ctx["timeNow"] = "21:00 (night)";
			ctx["here"] = "Whiterun (Whiterun Hold)";
			ctx["places"] = { "The Bannered Mare (inn)", "Dragonsreach (palace)", "Temple of Kynareth (temple)",
				"Belethor's General Goods (store)", "Warmaiden's (smithy)", "Honningbrew Meadery (brewery)" };
			ctx["recentLines"] = json::array();
			for (auto line : c.recent) {
				if (auto at = line.find("{p}"); at != std::string::npos) {
					line.replace(at, 3, g_testPlayer);
				}
				ctx["recentLines"].push_back(line);
			}
			ctx["services"] = json::array();
			for (auto& id : c.services) {
				ctx["services"].push_back(ServiceJson(id));
			}
			auto onReply = [i](const std::string& raw, bool success) {
				const auto& c = TestCases()[i];
				try {
					auto j = success ? ParseReply(raw) : std::nullopt;
					if (!success) {
						// The provider, not the classifier: not a verdict on the case.
						++g_testsUnreached;
						logger::info("SELFTEST SKIP  {}: model call failed | {}", c.name, raw.substr(0, 200));
					} else {
						const auto problems = j ? Judge(c, *j) : std::string("no JSON in reply");
						if (problems.empty()) {
							++g_testsPassed;
							logger::info("SELFTEST PASS  {}", c.name);
						} else {
							logger::info("SELFTEST FAIL  {}: {}  | player: \"{}\" npc: \"{}\" | model: {}", c.name, problems, c.player, c.reply, raw);
						}
					}
				} catch (...) {
					logger::info("SELFTEST FAIL  {}: reply could not be read | model: {}", c.name, raw);
				}
				FinishCase();
				SendNext();
			};
			if (!SkyrimNet::SendPrompt("mundanematters_turn", "mundanematters_classifier", ctx, onReply) &&
				!SkyrimNet::SendPrompt("mundanematters_turn", "", ctx, onReply)) {
				logger::info("SELFTEST SKIP  {}: could not reach the model", c.name);
				++g_testsUnreached;
				FinishCase();
				SendNext();
			}
		}

		void RunSelfTest()
		{
			if (g_testsLeft > 0) {
				return;  // one run at a time
			}
			const auto& cases = TestCases();
			g_testsLeft = static_cast<int>(cases.size());
			g_testsPassed = 0;
			g_testsUnreached = 0;
			g_nextCase = 0;
			g_testPlayer = RE::PlayerCharacter::GetSingleton()->GetName();
			logger::info("SELFTEST: starting {} cases with the mundanematters_classifier model, {} at a time", cases.size(), kTestsAtOnce);
			VanillaLines::SelfTest();
			Game::Notify(std::format("Mundane Matters self-test started ({} cases)...", cases.size()));
			for (std::size_t k = 0; k < kTestsAtOnce; ++k) {
				SendNext();
			}
		}
	}

	void PollSelfTest()
	{
		// Runs when the setting is switched on (not merely left on from before).
		static std::optional<bool> last;
		const bool on = SkyrimNet::ConfigBool("debug.self_test", false);
		if (last && !*last && on) {
			RunSelfTest();
		}
		last = on;
	}

	void Reset()
	{
		std::scoped_lock l(g_lock);
		g_pending.reset();
	}

	void Install()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatcher::Get());
		}
		// SkyrimNet worker threads: an exception escaping here would end the game.
		auto guarded = [](void (*fn)(const std::string&), const char* what) {
			return [fn, what](const std::string& raw) {
				try {
					fn(raw);
				} catch (const std::exception& e) {
					logger::error("{} event could not be handled: {}", what, e.what());
				} catch (...) {
					logger::error("{} event could not be handled", what);
				}
			};
		};
		SkyrimNet::OnEvent("dialogue_player_text", guarded(OnPlayerLine, "player line"));
		SkyrimNet::OnEvent("dialogue_player_stt", guarded(OnPlayerLine, "player line"));
		SkyrimNet::OnEvent("dialogue", guarded(OnNpcLine, "npc line"));
	}
}
