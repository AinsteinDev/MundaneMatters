#include "TurnPipeline.h"

#include "Papyrus.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Ledger.h"
#include "Promises.h"
#include "Services.h"
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
			if (r.find("trade") != std::string::npos || r.find("barter") != std::string::npos || r.find("shop") != std::string::npos ||
				r == "buy" || r == "sell" || r.find("wares") != std::string::npos || r.find("merchant") != std::string::npos) {
				return "trade";
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
			if (r.find("meal") != std::string::npos || r.find("eat") != std::string::npos || r.find("dine") != std::string::npos ||
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
				if (Game::DistanceToPlayer(npc) > kMaxServiceDistance) {
					logger::info("{} skipped: {} walked away", service, npc->GetName());
					return;
				}
				const char* menu = Services::MenuFor(service);
				if (*menu && MenuOpenedSince(menu, replyAt - kMenuDedupWindow / 2)) {
					logger::info("{} skipped: {} already opened by another mod", service, menu);
					return;
				}
				bool ok = Services::Execute(service, npc, args);
				logger::info("{} for {} -> {}", service, npc->GetName(), ok ? "started" : "FAILED");
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
			"blanket", "bedroll", "lodging", "shelter", "settle", "earned", "deserve", "present", "brought", "accept"
		};
		const std::array<std::string_view, 25> kPromisePhrases{
			"here you", "here's", "here,", "there you go", "take this", "take these", "take it", "take one", "for you", "on the house",
			"on me", "my treat", "help yourself", "my place", "with me", "for your trouble", "least i can do", "hold on to",
			"take that", "can i have", "may i have", "mind if i", "can i take", "may i take", "go ahead"
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

		// Could the PLAYER be asking someone to come along or lead the way? Same idea as the promise
		// gate: broad, only deciding whether the classifier is asked at all.
		bool MightMove(const std::string& playerLine, const std::string& npcLine)
		{
			if (NpcAgreesToMove(npcLine)) {
				return true;
			}
			static const std::array<std::string_view, 34> kMove{
				"follow", "come with", "come along", "come on", "with me", "tag along", "stick with", "stick close", "accompany",
				"join me", "let's go", "lets go", "walk with", "escort", "take me", "bring me to", "lead the way", "lead me",
				"guide", "show me the way", "show me where", "way to", "how do i get", "where is", "where's", "direction",
				"get me to", "walk me", "travel with", "go with me", "live", "'s place", "'s house", "'s home"
			};
			if (HasForeignLetters(playerLine)) {
				return true;
			}
			std::string lower(playerLine);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(kMove, [&](std::string_view w) { return lower.find(w) != std::string::npos; }) || HasMoveWordTypo(lower);
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

		bool MightPromise(const std::string& playerLine, const std::string& npcLine)
		{
			for (const auto* line : { &playerLine, &npcLine }) {
				// Digits, or a non-English line (the word lists below are English): ask the classifier.
				if (std::ranges::any_of(*line, [](unsigned char c) { return std::isdigit(c); }) || HasForeignLetters(*line)) {
					return true;
				}
				std::string lower(*line);
				std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				for (auto phrase : kPromisePhrases) {
					if (lower.find(phrase) != std::string::npos) {
						return true;
					}
				}
				for (auto& w : Words(lower)) {
					if (kPromiseWords.contains(w) || ItemWords().contains(w)) {
						return true;
					}
				}
			}
			return false;
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
			// "You're always welcome here": guest rights, kept on every visit by the ledger.
			if (promise.type == "offer_bed" && promise.always) {
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
				const bool handout = promise.type == "give_item" || promise.type == "give_gold" || promise.type == "allow_take";
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
				// A meal together only when food or eating comes up.
				const bool meal = MightMeal(playerLine, npcLine);
				if (!meal) {
					std::erase_if(services, [](const Services::Service& s) { return Services::IsActivity(s.id); });
				}
				const bool shopkeeper = std::ranges::any_of(services, [](const Services::Service& s) {
					return !Services::IsMovement(s.id) && !Services::IsActivity(s.id);
				});
				// Service NPCs are always asked; anyone else only when the line might hold a promise
				// (with promises on) or a request to come along / lead the way.
				if (!shopkeeper && !move && !meal && !(promises && MightPromise(playerLine, npcLine))) {
					logger::info("{}: small talk, not classified", npc->GetName());
					return;
				}
				if (npc->IsHostileToActor(Game::Player())) {
					return;
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
				if (move) {
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

				std::string npcName = npc->GetName();
				auto onReply = [npcId, npcName, replyAt, serviceIds, promises](const std::string& raw, bool success) {
					if (!success) {
						logger::warn("classifier failed for {}: {}", npcName, raw);
						return;
					}
					auto j = ParseReply(raw);
					if (!j) {
						logger::warn("classifier returned no JSON for {}: {}", npcName, raw);
						return;
					}
					const auto request = NormalizeRequest(JsonUtil::Str(*j, "request", "none"));
					const auto response = NormalizeResponse(JsonUtil::Str(*j, "response", "accept"));
					const auto confidence = Confidence(*j);
					const auto promiseList = PromiseList(*j);
					logger::info("classified {}: request={} ({:.2f}) response={} promises={} gives={}",
						npcName, request, confidence, response, promiseList.dump(),
						j->contains("player_gives") ? (*j)["player_gives"].dump() : std::string("[]"));

					// Only services this NPC really has; the model sometimes invents one.
					const bool offered = std::ranges::find(serviceIds, request) != serviceIds.end();
					const float minConf = SkyrimNet::ConfigFloat("tuning.min_confidence", 0.6f);
					bool serviceStarted = false;
					if (offered && confidence >= minConf) {
						if (response == "refuse" || response == "counter") {
							Game::DebugNotify(npcName + " declined (" + request + ")");
						} else {
							Game::DebugNotify(npcName + ": " + request);
							const auto ownTrip = JsonUtil::Lower(JsonUtil::Str(*j, "own_trip"));
							RunService(npcId, request, { Price(*j), response == "free", JsonUtil::Str(*j, "place"), ownTrip == "true" || ownTrip == "yes" || ownTrip == "1" }, replyAt);
							serviceStarted = true;
						}
					}

					// The player handing something over; only listed when the NPC accepts it.
					if (auto gifts = j->find("player_gives"); gifts != j->end() && SkyrimNet::ConfigBool("features.gifts", true)) {
						const json list = gifts->is_array() ? *gifts : gifts->is_object() ? json::array({ *gifts }) : json::array();
						std::vector<Promises::Gift> all;
						for (auto& gj : list) {
							if (auto g = Promises::GiftFromJson(gj); g && all.size() < 6) {
								// "Here's 100 for a room": that coin is the payment the service already takes.
								if (g->gold > 0 && serviceStarted && (request == "rent_room" || request == "train" || request == "trade")) {
									logger::info("{}: coin in the same line as {} is the payment, not a gift", npcName, request);
									continue;
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
					for (auto& pj : promiseList) {
						auto p = Promises::FromJson(pj);
						if (!p) {
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
						if (p->when == "cancel") {
							Ledger::Cancel(npcId, *p);
							continue;
						}
						// Repeated mentions ("the bed's yours", "rest now") are the same promise: already
						// waiting or just kept, so no second announcement or attempt.
						// A handover NOW of something owed for later settles it early, so it isn't a repeat.
						// Food, coin and items are never skipped here: "may I have another?" is a real second
						// ask, and the generosity budget decides (giving, or walking it back aloud).
						const bool handout = p->type == "give_item" || p->type == "give_gold" || p->type == "allow_take";
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
			if (Game::IsOwnLine(text)) {
				return;  // a popup choice MundaneMatters said for the player: already handled
			}
			logger::info("player: \"{}\" (target {:08X})", text, e->to);
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

			if (!asked) {
				// NPC spoke on their own (greeting, comment, narrated reaction): not an answer. It
				// may be their reaction to a gift.
				Promises::OnNpcReply(e->from, text);
				return;
			}
			logger::info("{} replied: \"{}\"", speaker, text);
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
		};

		json ServiceJson(const std::string& id)
		{
			static const std::unordered_map<std::string, std::pair<const char*, const char*>> kLabels{
				{ "trade", { "trade with them: see their goods, buy, or sell to them", "\"show me your wares\", \"what are you selling?\"" } },
				{ "rent_room", { "rent a room / bed for the night here (100 gold)", "\"I need a room\", \"got a bed for the night?\"" } },
				{ "train", { "pay them to train a skill", "\"can you teach me?\", \"train me\"" } },
				{ "follow", { "come along with the PLAYER now, walking behind them (the PLAYER leads)", "\"follow me\", \"come with me\"" } },
				{ "escort", { "lead the PLAYER to a named place, walking ahead so they can follow (the NPC leads; set \"place\")", "\"take me to the Bannered Mare\", \"show me the way to the temple\"" } },
				{ "share_meal", { "sit down and share a meal with the PLAYER now (go to a table, eat together)", "\"let's eat together\", \"join me for supper?\", \"care to share a meal?\"" } },
			};
			const auto& [label, examples] = kLabels.at(id);
			return { { "id", id }, { "label", label }, { "examples", examples } };
		}

		const std::vector<TestCase>& TestCases()
		{
			using V = std::vector<std::string>;
			const V shop{ "trade" }, inn{ "trade", "rent_room" }, move{ "follow", "escort" }, none{};
			static const std::vector<TestCase> cases{
				// trade
				{ "trade plain", shop, "Belethor", "show me your wares", "Take a look, everything's for sale.", "trade", "yes" },
				{ "trade inside RP line", shop, "Belethor", "i got something to sell, but first, show me your wares", "Of course! Browse away.", "trade", "yes" },
				{ "trade typo", shop, "Belethor", "wat u got for sale", "Weapons, armor, potions. Have a look.", "trade", "yes" },
				{ "trade refused", shop, "Belethor", "what've you got, old man?", "Not for the likes of you, milk-drinker. Out.", "trade", "no" },
				{ "trade only mentioned", shop, "Belethor", "I traded with your brother yesterday.", "Lucky you. He overcharges.", "none" },
				{ "purchase is not a gift", shop, "Belethor", "I'll buy this sword, here's 100.", "Pleasure doing business.", "trade", "-" },
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
				// promises
				{ "coins and soup", none, "Ronja", "Thanks for the help.", "Least I can do. Take 10 coins and a bowl of cabbage soup.", "none", "-", { "give_gold", "give_item" }, "now" },
				{ "free stew", none, "Ronja", "I'm starving.", "Sit, I'll get you a bowl of stew. *hands it over*", "none", "-", { "give_item" }, "now" },
				{ "no more stew", none, "Ronja", "may I have another?", "No more, you've had yours.", "none", "-", {} },
				{ "sale is not a promise", none, "Ronja", "Any food?", "Bread's 2 gold.", "none", "-", {} },
				{ "bed tonight", none, "Ronja", "Can I crash here?", "The bed in the back is yours tonight.", "none", "-", { "offer_bed" } },
				{ "floor is not a bed", none, "Ronja", "Can I use your bed?", "One night, on the floor, and keep your hands to yourself.", "none", "-", {} },
				{ "always welcome", none, "Viola", "Farewell, Viola.", "Farewell, and you're always welcome at my place.", "none", "-", { "offer_bed" }, "", true },
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
			if (gives != c.gives) {
				problems.push_back(std::format("player_gives {} (wanted {})", gives, c.gives));
			}
			std::string out;
			for (auto& p : problems) {
				out += (out.empty() ? "" : "; ") + p;
			}
			return out;
		}

		std::atomic_int g_testsLeft{ 0 };
		std::atomic_int g_testsPassed{ 0 };

		void RunSelfTest()
		{
			if (g_testsLeft > 0) {
				return;  // one run at a time
			}
			const auto& cases = TestCases();
			g_testsLeft = static_cast<int>(cases.size());
			g_testsPassed = 0;
			const std::string playerName = RE::PlayerCharacter::GetSingleton()->GetName();
			logger::info("SELFTEST: starting {} cases with the mundanematters_classifier model", cases.size());
			Game::Notify(std::format("Mundane Matters self-test started ({} cases)...", cases.size()));
			for (std::size_t i = 0; i < cases.size(); ++i) {
				const auto& c = cases[i];
				json ctx;
				ctx["playerName"] = playerName;
				ctx["npcName"] = c.npc;
				ctx["playerLine"] = c.player;
				ctx["npcLine"] = c.reply;
				ctx["timeNow"] = "21:00 (night)";
				ctx["here"] = "Whiterun (Whiterun Hold)";
				ctx["places"] = { "The Bannered Mare (inn)", "Dragonsreach (palace)", "Temple of Kynareth (temple)",
					"Belethor's General Goods (store)", "Warmaiden's (smithy)", "Honningbrew Meadery (brewery)" };
				ctx["recentLines"] = json::array();
				ctx["services"] = json::array();
				for (auto& id : c.services) {
					ctx["services"].push_back(ServiceJson(id));
				}
				auto onReply = [i](const std::string& raw, bool success) {
					const auto& c = TestCases()[i];
					try {
						auto j = success ? ParseReply(raw) : std::nullopt;
						const auto problems = j ? Judge(c, *j) : std::string(success ? "no JSON in reply" : "model call failed");
						if (problems.empty()) {
							++g_testsPassed;
							logger::info("SELFTEST PASS  {}", c.name);
						} else {
							logger::info("SELFTEST FAIL  {}: {}  | player: \"{}\" npc: \"{}\" | model: {}", c.name, problems, c.player, c.reply, raw);
						}
					} catch (...) {
						logger::info("SELFTEST FAIL  {}: reply could not be read | model: {}", c.name, raw);
					}
					if (--g_testsLeft == 0) {
						const int total = static_cast<int>(TestCases().size());
						logger::info("SELFTEST DONE: {}/{} passed", g_testsPassed.load(), total);
						Game::Notify(std::format("Mundane Matters self-test: {}/{} passed (details in MundaneMatters.log)", g_testsPassed.load(), total));
					}
				};
				if (!SkyrimNet::SendPrompt("mundanematters_turn", "mundanematters_classifier", ctx, onReply) &&
					!SkyrimNet::SendPrompt("mundanematters_turn", "", ctx, onReply)) {
					logger::info("SELFTEST FAIL  {}: could not reach the model", c.name);
					--g_testsLeft;
				}
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
