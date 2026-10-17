#include "VanillaLines.h"

#include "Brawls.h"
#include "Confirm.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Papyrus.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::VanillaLines
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto kTooLate = std::chrono::seconds(25);  // a reply this late belongs to another moment
		constexpr auto kHush = std::chrono::seconds(12);     // AI lines cut while the vanilla line plays
		constexpr auto kOwnStory = std::chrono::minutes(3);  // a quest line stays "just said" this long

		struct Candidate
		{
			RE::FormID topic = 0;
			RE::FormID info = 0;      // the response the game would give now
			std::string text;         // the option as the menu shows it
			std::string answer;       // what the NPC says to it (the recorded line's subtitle)
			bool quest = false;       // belongs to a quest (main, guild, side, daedric, favour)
			std::string questName;
			bool saidBefore = false;  // already said (still listed because it can be asked again)
			int menuIndex = -1;       // its row in the menu
		};

		struct Offer
		{
			std::vector<Candidate> options;  // the menu's options when the player spoke
			std::string line;                // what the player said
			Clock::time_point at{};
			bool replied = false;            // the NPC's reply to it has been looked at
			std::string playing;             // the vanilla line's words, while it has the floor
			Clock::time_point firedAt{};     // when it went off (its own record arrives within a moment)
			Clock::time_point hushUntil{};   // cut AI lines until then
		};

		struct Said
		{
			std::string answer;
			Clock::time_point at;
		};

		std::mutex g_lock;
		std::unordered_map<RE::FormID, Offer> g_offers;
		std::unordered_map<RE::FormID, Clock::time_point> g_fired;  // when a vanilla line last went off, per NPC
		std::unordered_map<RE::FormID, Said> g_said;                 // the last quest line per NPC, for their bio

		// ---- words ----

		const std::unordered_set<std::string>& Stop()
		{
			static const std::unordered_set<std::string> words{ "what", "about", "with", "your", "you", "that", "this", "have", "there", "they", "them",
				"tell", "know", "want", "need", "would", "could", "should", "will", "just", "here", "from", "were", "been", "some", "more", "like",
				"does", "doing", "anything", "something", "going", "into", "then", "than", "well", "right", "okay", "yeah", "please", "very", "much",
				"sure", "good", "fine", "where", "when", "which", "who's", "it's", "that's", "what's", "you've", "you're", "you'll", "you'd", "i've",
				"i'll", "i'd", "we're", "they're", "he's", "she's" };
			return words;
		}

		std::vector<std::string> Content(const std::string& text)
		{
			std::vector<std::string> out;
			for (auto& w : JsonUtil::Words(text)) {
				if (w.size() >= 4 && !Stop().contains(w)) {
					out.push_back(w);
				}
			}
			return out;
		}

		// The words of a line, without the menu's speech-check tags ("(Persuade)", "(25 gold)").
		std::string Untagged(const std::string& text)
		{
			std::string out;
			int depth = 0;
			for (char ch : text) {
				if (ch == '(' || ch == '[') {
					++depth;
				} else if ((ch == ')' || ch == ']') && depth > 0) {
					--depth;
				} else if (depth == 0) {
					out += ch;
				}
			}
			return out;
		}

		bool Tagged(const std::string& text)
		{
			return text.find('(') != std::string::npos;
		}

		std::string Phrase(const std::string& text)
		{
			std::string out;
			for (auto& w : JsonUtil::Words(Untagged(text))) {
				out += (out.empty() ? "" : " ") + w;
			}
			return out;
		}

		bool SameWord(const std::string& w, const std::string& o)
		{
			return w == o || (o.size() >= 5 && w.size() >= 5 && (JsonUtil::EditDistance(w, o) <= 1 || w.starts_with(o.substr(0, 5)) || o.starts_with(w.substr(0, 5))));
		}

		int Hits(const std::vector<std::string>& said, const std::vector<std::string>& option)
		{
			int hits = 0;
			for (auto& o : option) {
				hits += std::ranges::any_of(said, [&](const std::string& w) { return SameWord(w, o); }) ? 1 : 0;
			}
			return hits;
		}

		// Nearly the option's own sentence ("do you work here" = "Do you work here?").
		bool SamePhrase(const std::string& said, const std::string& option)
		{
			if (said.empty() || option.size() < 8) {
				return false;
			}
			if (said.find(option) != std::string::npos) {
				return true;
			}
			const auto longer = (std::max)(said.size(), option.size());
			return JsonUtil::EditDistance(said, option) * 5 <= longer;  // within 20%
		}

		// Said closely: 2+ of the option's own words and a third of them or more ("you should not let
		// customers harass you, stand up to them" = "You shouldn't let customers give you a hard time.
		// Stand up to them.").
		bool SaidClosely(const std::string& line, const std::string& option)
		{
			const auto words = Content(Untagged(option));
			const int hits = Hits(Content(line), words);
			return !words.empty() && hits >= 2 && hits * 3 >= static_cast<int>(words.size());
		}

		bool Threatens(const std::string& line)
		{
			const auto l = JsonUtil::Lower(line);
			for (auto* w : { "or else", "or i'll", "or ill", "or i will", "or you'll", "or youll", "or you will", "don't you dare", "dont you dare",
					 "kill you", " rip ", "break your", "lose your", "lose those", "loose your", "loose those", "cut off", "gut you", "you'll regret",
					 "youll regret", "you're dead", "youre dead", "hurt you", "make you pay", "last warning", "i swear i'll", "i swear ill", "or you're",
					 "or youre" }) {
				if (l.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// Share of an answer's words the reply repeats (answers of 4+ words only).
		double Recited(const std::string& reply, const std::string& answer)
		{
			const auto words = Content(answer);
			if (words.size() < 4) {
				return 0.0;
			}
			return static_cast<double>(Hits(Content(reply), words)) / static_cast<double>(words.size());
		}

		// The player's line can be this option: shares its words, or it's a short reply ("I'll do it.").
		bool Fits(const std::string& line, const Candidate& c)
		{
			const auto words = Content(Untagged(c.text));
			return words.size() <= 2 || Hits(Content(line), words) >= 1 || SamePhrase(Phrase(line), Phrase(c.text));
		}

		// The game's background service lines ("for sale", "a room", "follow me", "the fine"): Mundane
		// Matters' own services do these.
		bool ServiceLine(RE::TESQuest* q, const std::string& text)
		{
			if (!q || q->GetType() != RE::QUEST_DATA::Type::kNone || (q->GetName() && *q->GetName())) {
				return false;
			}
			const auto t = JsonUtil::Lower(text);
			for (auto* w : { "for sale", "trade", "buy", "sell", "wares", "room", "bed", "follow", "wait here", "part ways", "dismiss", "train", "teach",
					 "carriage", "ride", "fine", "bounty", "jail", "crime", "look the other way" }) {
				if (t.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// ---- the decisions (plain; mirrored in tools/logic_check.py) ----

		// On the player's line: the option it plainly is (1-based), or 0. Never an option already said
		// unless said word for word.
		int PickForLine(const std::vector<Candidate>& options, const std::string& line)
		{
			const auto said = Phrase(line);
			auto unique = [&](auto&& test) {
				int found = 0, count = 0;
				for (std::size_t k = 0; k < options.size(); ++k) {
					if (test(options[k])) {
						found = static_cast<int>(k) + 1;
						++count;
					}
				}
				return count == 1 ? found : 0;
			};
			// 1. Its own sentence, near enough (an already-said one too: the player asked it again).
			if (const int k = unique([&](const Candidate& c) { return !Tagged(c.text) && SamePhrase(said, Phrase(c.text)); })) {
				return k;
			}
			// 2. A threat, with an (Intimidate) option on screen (several: the one sharing the subject).
			if (Threatens(line)) {
				int best = 0, bestHits = -1, count = 0;
				for (std::size_t k = 0; k < options.size(); ++k) {
					if (JsonUtil::Lower(options[k].text).find("(intimidate)") == std::string::npos) {
						continue;
					}
					++count;
					if (const int hits = Hits(Content(line), Content(Untagged(options[k].text))); hits > bestHits) {
						bestHits = hits;
						best = static_cast<int>(k) + 1;
					}
				}
				if (count == 1 || bestHits > 0) {
					return best;
				}
			}
			// 3. Said closely: that one; several, the one whose words the line covers best, when it's clearly
			// ahead ("join the companions" = "I would like to join the Companions.", not "Tell me about the
			// Companions and how to join their ranks."). A tie is no pick: the reply decides.
			int best = 0, count = 0;
			double bestCover = -1.0, second = -1.0;
			for (std::size_t k = 0; k < options.size(); ++k) {
				const auto& c = options[k];
				if (c.saidBefore || !SaidClosely(line, c.text)) {
					continue;
				}
				++count;
				const auto words = Content(Untagged(c.text));
				const double cover = static_cast<double>(Hits(Content(line), words)) / static_cast<double>(words.size());
				if (cover > bestCover) {
					second = bestCover;
					bestCover = cover;
					best = static_cast<int>(k) + 1;
				} else if (cover > second) {
					second = cover;
				}
			}
			return count == 1 || (count > 1 && bestCover > second) ? best : 0;
		}

		// On the NPC's AI reply: the not-yet-said option whose real answer it repeats (half its words or more)
		// and that the player's line fits, or 0. Speech checks never: their outcome is the game's roll.
		int PickForReply(const std::vector<Candidate>& options, const std::string& line, const std::string& reply)
		{
			int best = 0;
			double bestShare = 0.0;
			for (std::size_t k = 0; k < options.size(); ++k) {
				const auto& c = options[k];
				if (c.saidBefore || Tagged(c.text) || !Fits(line, c)) {
					continue;
				}
				if (const double share = Recited(reply, c.answer); share > bestShare) {
					bestShare = share;
					best = static_cast<int>(k) + 1;
				}
			}
			return bestShare >= 0.5 ? best : 0;
		}

		// ---- the game ----

		bool IsQuest(RE::TESQuest* q)
		{
			return q && q->GetType() != RE::QUEST_DATA::Type::kNone;
		}

		std::string AnswerOf(RE::TESTopicInfo* info, RE::Actor* npc)
		{
			std::string out;
			try {
				auto item = info->GetDialogueData(npc);
				for (auto* r : item.responses) {
					if (r && r->text.c_str() && *r->text.c_str()) {
						out += (out.empty() ? "" : " ") + std::string(r->text.c_str());
					}
				}
			} catch (...) {
			}
			return out;
		}

		std::string ResponsesText(RE::BSSimpleList<RE::DialogueResponse*>& responses)
		{
			std::string out;
			for (auto* r : responses) {
				if (r && r->text.c_str() && *r->text.c_str()) {
					out += (out.empty() ? "" : " ") + std::string(r->text.c_str());
				}
			}
			return out;
		}

		// An info's conditions, checked the way the dialogue menu checks them: with its quest as the
		// context (alias conditions read the quest; without it the game crashes). Conditions on package
		// or story-event data can't be answered here: such infos never count.
		bool Holds(RE::TESTopicInfo* info, RE::Actor* npc, RE::TESQuest* quest)
		{
			if (!info || !npc || !quest) {
				return false;
			}
			for (auto* item = info->objConditions.head; item; item = item->next) {
				const auto on = item->data.object.get();
				if (on == RE::CONDITIONITEMOBJECT::kPackData || on == RE::CONDITIONITEMOBJECT::kEventData || item->data.flags.usePackData) {
					return false;
				}
			}
			RE::ConditionCheckParams params(npc, Game::Player());
			params.quest = quest;
			// As the game reads a condition list: OR binds a run of items to the next one, AND joins the runs.
			bool inOr = false, orValue = false;
			for (auto* item = info->objConditions.head; item; item = item->next) {
				const bool value = item->IsTrue(params);
				if (item->data.flags.isOR) {
					inOr = true;
					orValue = orValue || value;
					continue;
				}
				const bool group = inOr ? (orValue || value) : value;
				inOr = orValue = false;
				if (!group) {
					return false;
				}
			}
			return !inOr || orValue;
		}

		// The response the game would give to this topic now: the first whose conditions pass.
		RE::TESTopicInfo* OutcomeOf(RE::TESTopic* topic, RE::Actor* npc)
		{
			if (!topic || !topic->topicInfos) {
				return nullptr;
			}
			for (std::uint32_t i = 0; i < topic->numTopicInfos; ++i) {
				auto* info = topic->topicInfos[i];
				if (info && !(info->saidOnce && info->data.flags.any(RE::TOPIC_INFO_DATA::TOPIC_INFO_FLAGS::kSayOnce)) && Holds(info, npc, topic->ownerQuest)) {
					return info;
				}
			}
			return nullptr;
		}

		// The open dialogue menu, if it's with this NPC.
		RE::MenuTopicManager* MenuWith(RE::Actor* npc)
		{
			auto* mtm = RE::MenuTopicManager::GetSingleton();
			if (!mtm || !mtm->menuOpen || !mtm->dialogueList || !npc) {
				return nullptr;
			}
			auto speaker = mtm->speaker.get();
			return speaker && speaker.get() == npc ? mtm : nullptr;
		}

		// Game thread. The options on screen in the open menu with this NPC (none when it's closed).
		std::vector<Candidate> OnScreen(RE::Actor* npc)
		{
			std::vector<Candidate> out;
			auto* mtm = MenuWith(npc);
			if (!mtm) {
				return out;
			}
			int row = -1;
			for (auto* d : *mtm->dialogueList) {
				++row;
				if (!d || !d->parentTopic) {
					continue;
				}
				const std::string text = d->topicText.c_str() ? d->topicText.c_str() : "";
				if (text.empty() || ServiceLine(d->parentQuest, text)) {
					continue;
				}
				// The menu's own copy of the answer can be stale; the game's pick, worked out now, is it.
				auto* outcome = OutcomeOf(d->parentTopic, npc);
				auto* info = outcome ? outcome : d->parentTopicInfo;
				std::string answer = outcome ? AnswerOf(outcome, npc) : std::string();
				if (answer.empty()) {
					answer = ResponsesText(d->responses);
				}
				const char* qn = d->parentQuest ? d->parentQuest->GetName() : nullptr;
				Candidate c;
				c.topic = d->parentTopic->GetFormID();
				c.info = info ? info->GetFormID() : 0;
				c.text = text;
				c.answer = answer;
				c.quest = IsQuest(d->parentQuest);
				c.questName = qn ? qn : "";
				c.saidBefore = info && info->saidOnce;
				c.menuIndex = row;
				out.push_back(std::move(c));
			}
			return out;
		}

		// Drops SkyrimNet's queued lines and the actions bound to them. deferred: the line playing now
		// may finish (only what follows it goes).
		void Purge(bool deferred = false)
		{
			Papyrus::CallStatic("SkyrimNetApi", "PurgeDialogue", nullptr, deferred);
		}

		void MarkSaid(RE::TESTopicInfo* info)
		{
			if (info) {
				info->saidOnce = true;
				info->AddChange(RE::TESTopicInfo::ChangeFlags::kSaidOnce);
			}
		}

		// The line's own scripts (begin, then end fragments), run without its audio.
		int RunScripts(RE::TESTopicInfo* info, RE::Actor* npc)
		{
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm || !info || !npc) {
				return 0;
			}
			auto* policy = vm->GetObjectHandlePolicy();
			const auto handle = policy->GetHandleForObject(static_cast<RE::VMTypeID>(RE::FormType::Info), info);
			if (handle == policy->EmptyHandle()) {
				return 0;
			}
			std::vector<std::pair<RE::BSTSmartPointer<RE::BSScript::Object>, std::string>> calls;
			{
				RE::BSSpinLockGuard guard(vm->attachedScriptsLock);
				if (auto it = vm->attachedScripts.find(handle); it != vm->attachedScripts.end()) {
					for (auto& attached : it->second) {
						RE::BSTSmartPointer<RE::BSScript::Object> obj(attached.get());
						auto* type = obj ? obj->GetTypeInfo() : nullptr;
						if (!type) {
							continue;
						}
						auto* fns = type->GetMemberFuncIter();
						for (std::uint32_t i = 0; fns && i < type->GetNumMemberFuncs(); ++i) {
							if (fns[i].func) {
								std::string name = fns[i].func->GetName().c_str();
								if (name.starts_with("Fragment_")) {
									calls.emplace_back(obj, std::move(name));
								}
							}
						}
					}
				}
			}
			auto number = [](const std::string& n) { return std::atoi(n.c_str() + 9); };
			std::ranges::sort(calls, [&](auto& a, auto& b) { return number(a.second) < number(b.second); });
			for (auto& [obj, name] : calls) {
				auto args = RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(npc));
				RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
				vm->DispatchMethodCall(obj, name.c_str(), args, none);
			}
			return static_cast<int>(calls.size());
		}

		// UI thread: picks the option in the open menu exactly as a click does. False: not found.
		bool ClickInMenu(const std::string& text, int row)
		{
			auto* ui = RE::UI::GetSingleton();
			auto menu = ui ? ui->GetMenu(RE::DialogueMenu::MENU_NAME) : nullptr;
			if (!menu || !menu->uiMovie) {
				return false;
			}
			auto& movie = menu->uiMovie;
			RE::GFxValue list;
			if (!movie->GetVariable(&list, "_root.DialogueMenu_mc.TopicListHolder.List_mc.entryList") || !list.IsArray() || list.GetArraySize() == 0) {
				return false;
			}
			const auto want = JsonUtil::Lower(text);
			const auto wantPhrase = Phrase(text);
			int index = -1;
			for (std::uint32_t i = 0; i < list.GetArraySize() && index < 0; ++i) {
				RE::GFxValue entry, label;
				if (list.GetElement(i, &entry) && entry.GetMember("text", &label) && label.IsString() && label.GetString()) {
					const std::string shown = label.GetString();
					if (JsonUtil::Lower(shown) == want || (Phrase(shown) == wantPhrase && Tagged(shown) == Tagged(text))) {
						index = static_cast<int>(i);
					}
				}
			}
			// Its row, if what's shown there is the same line in other formatting (most of its words).
			if (index < 0 && row >= 0 && static_cast<std::uint32_t>(row) < list.GetArraySize()) {
				RE::GFxValue entry, label;
				if (list.GetElement(static_cast<std::uint32_t>(row), &entry) && entry.GetMember("text", &label) && label.IsString() && label.GetString()) {
					const auto words = Content(Untagged(text));
					if (!words.empty() && Hits(Content(Untagged(label.GetString())), words) * 2 >= static_cast<int>(words.size())) {
						index = row;
					}
				}
			}
			if (index < 0) {
				std::string shown;
				for (std::uint32_t i = 0; i < list.GetArraySize(); ++i) {
					RE::GFxValue entry, label;
					if (list.GetElement(i, &entry) && entry.GetMember("text", &label) && label.IsString() && label.GetString()) {
						shown += std::format("{}\"{}\"", shown.empty() ? "" : ", ", label.GetString());
					}
				}
				logger::info("vanilla: \"{}\" not found; the menu shows [{}]", text, shown);
				return false;
			}
			RE::GFxValue arg(static_cast<double>(index));
			movie->Invoke("_root.DialogueMenu_mc.TopicListHolder.List_mc.doSetSelectedIndex", nullptr, &arg, 1);
			return movie->Invoke("_root.DialogueMenu_mc.onSelectionClick", nullptr, nullptr, 0);
		}

		bool MenuListEmpty()
		{
			auto* ui = RE::UI::GetSingleton();
			auto menu = ui ? ui->GetMenu(RE::DialogueMenu::MENU_NAME) : nullptr;
			RE::GFxValue list;
			return menu && menu->uiMovie && menu->uiMovie->GetVariable(&list, "_root.DialogueMenu_mc.TopicListHolder.List_mc.entryList") && list.IsArray() &&
			       list.GetArraySize() == 0;
		}

		// UI thread: clicks once the menu's list is filled (it's empty for a moment while the menu opens or
		// moves on; up to ~3 s), then `then(clicked)`.
		void TryClick(std::string text, int row, int tries, std::function<void(bool)> then)
		{
			if (MenuListEmpty() && tries < 15) {
				std::thread([text = std::move(text), row, tries, then = std::move(then)]() mutable {
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
					SKSE::GetTaskInterface()->AddUITask([text = std::move(text), row, tries, then = std::move(then)]() mutable {
						TryClick(std::move(text), row, tries + 1, std::move(then));
					});
				}).detach();
				return;
			}
			then(ClickInMenu(text, row));
		}

		// UI thread, repeatedly: skips the NPC's recorded line(s), as pressing to skip does, until they've
		// finished. Gives up after ~6 s.
		void SkipVoice(int tries, bool seenTalking)
		{
			auto* mtm = RE::MenuTopicManager::GetSingleton();
			if (!mtm || !mtm->menuOpen || tries > 30) {
				return;
			}
			const bool talking = mtm->currentTopicInfo != nullptr;
			if (talking) {
				if (auto* ui = RE::UI::GetSingleton()) {
					if (auto menu = ui->GetMenu(RE::DialogueMenu::MENU_NAME); menu && menu->uiMovie) {
						menu->uiMovie->Invoke("_root.DialogueMenu_mc.SkipText", nullptr, nullptr, 0);
					}
				}
			} else if (seenTalking) {
				logger::info("vanilla: recorded line skipped; the menu moved on");
				return;
			}
			std::thread([tries, seen = seenTalking || talking] {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				SKSE::GetTaskInterface()->AddUITask([tries, seen] { SkipVoice(tries + 1, seen); });
			}).detach();
		}

		// Game thread: it went off. A quest beat (a task given, a reward handed over): the NPC stands by it.
		void NoteFired(RE::FormID npcId, const Candidate& c, bool recording = true)
		{
			std::scoped_lock l(g_lock);
			g_fired[npcId] = Clock::now();
			auto& o = g_offers[npcId];
			o.options.clear();
			// The recording has the floor for a moment; with the AI's own answer instead, nothing to hush.
			o.hushUntil = recording ? Clock::now() + kHush : Clock::now();
			o.playing = JsonUtil::Lower(c.answer);
			o.firedAt = Clock::now();
			if (c.quest && !c.answer.empty()) {
				g_said[npcId] = { c.answer, Clock::now() };
			}
		}

		// Game thread: the original line said outside the menu (the click didn't find it).
		void SayLine(RE::FormID npcId, const Candidate& c)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* topic = RE::TESForm::LookupByID<RE::TESTopic>(c.topic);
			if (!npc || !topic) {
				return;
			}
			Papyrus::CallMethod(npc, "ObjectReference", "Say", nullptr, topic, static_cast<RE::Actor*>(nullptr), false);
			MarkSaid(RE::TESForm::LookupByID<RE::TESTopicInfo>(c.info));
			logger::info("vanilla: {} says \"{}\" (not clickable; said instead)", Game::NameOf(npc), c.text);
		}

		// Game thread: this option, picked. aiSpoke: the NPC's AI already said its answer.
		void Play(RE::FormID npcId, const Candidate& c, bool aiSpoke)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc || npc->IsDead() || npc->IsInCombat() || Game::Player()->IsInCombat() || !MenuWith(npc)) {
				return;
			}
			logger::info("vanilla: {} - \"{}\"{}", Game::NameOf(npc), c.text, aiSpoke ? " (the AI said its answer: skipping the recording)" : "");
			NoteFired(npcId, c, !aiSpoke);
			if (JsonUtil::Lower(c.text).find("(intimidate)") != std::string::npos) {
				Brawls::Threatened(npcId);
			}
			// The AI's reply: before it, the whole of it goes (with any action it carries). After it, nothing
			// is purged: SkyrimNet's purge also drops the reply's voice not yet played (Jenassa's fee line
			// was written, purged before its audio started, and then the recording skipped: silence).
			if (!aiSpoke) {
				Purge(false);
			}
			SKSE::GetTaskInterface()->AddUITask([npcId, c, aiSpoke] {
				TryClick(c.text, c.menuIndex, 0, [npcId, c, aiSpoke](bool clicked) {
					if (clicked) {
						if (aiSpoke) {
							SkipVoice(0, false);
						}
						return;
					}
					SKSE::GetTaskInterface()->AddTask([npcId, c, aiSpoke] {
						auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId);
						auto* info = RE::TESForm::LookupByID<RE::TESTopicInfo>(c.info);
						if (aiSpoke) {
							RunScripts(info, a);
							MarkSaid(info);
							logger::info("vanilla: \"{}\" ran without its recording", c.text);
						} else {
							SayLine(npcId, c);
						}
					});
				});
			});
		}

		// Game thread: picked; asks first when "Confirm Quest Lines" is on.
		void Pick(RE::FormID npcId, const Candidate& c, bool aiSpoke)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc || !c.quest || !SkyrimNet::ConfigBool("features.confirm_quest_lines", true) || !Confirm::Available()) {
				Play(npcId, c, aiSpoke);
				return;
			}
			if (!aiSpoke) {
				Purge();  // the AI waits for the answer
			}
			Confirm::Offer offer;
			offer.title = c.questName.empty() ? "Say this?" : "Say this? (" + c.questName + ")";
			offer.npc = Game::NameOf(npc);
			offer.detail = "\"" + c.text + "\"";
			offer.showPrice = false;
			offer.seconds = 0;
			offer.acceptLabel = "Say it";
			offer.declineLabel = "Not that";
			Confirm::Ask(offer, [npcId, c, aiSpoke](Confirm::Choice choice) {
				if (choice == Confirm::Choice::kAccept) {
					Play(npcId, c, aiSpoke);
				} else if (!aiSpoke) {
					if (auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId)) {
						Papyrus::CallStatic("SkyrimNetApi", "DirectNarration", nullptr, RE::BSFixedString(""), a, static_cast<RE::Actor*>(Game::Player()));
					}
				}
			});
		}

		// Game thread: the NPC's reply to the player's line, looked at once.
		void OnReply(RE::FormID npcId, std::string reply)
		{
			std::vector<Candidate> options;
			std::string line;
			{
				std::scoped_lock l(g_lock);
				auto it = g_offers.find(npcId);
				if (it == g_offers.end() || it->second.options.empty() || Clock::now() - it->second.at > kTooLate) {
					return;
				}
				options = it->second.options;
				line = it->second.line;
				it->second.options.clear();
			}
			if (const int k = PickForReply(options, line, reply); k > 0) {
				Pick(npcId, options[k - 1], true);
			}
		}
	}

	void OnPlayerLine(RE::FormID npcId, std::string text)
	{
		auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
		if (!npc || npc->IsDead() || npc->IsInCombat() || !SkyrimNet::ConfigBool("features.vanilla_lines", true)) {
			return;
		}
		auto options = OnScreen(npc);
		{
			std::scoped_lock l(g_lock);
			auto& o = g_offers[npcId];
			o = Offer{};
			if (options.empty()) {
				return;
			}
			o.options = options;
			o.line = text;
			o.at = Clock::now();
		}
		if (const int k = PickForLine(options, text); k > 0) {
			Pick(npcId, options[k - 1], false);
			return;
		}
		std::string listed;
		for (auto& c : options) {
			listed += (listed.empty() ? "\"" : ", \"") + c.text + "\"";
		}
		logger::info("vanilla: \"{}\" to {} isn't one of the {} option(s) on screen; the reply decides ({})", text, Game::NameOf(npc), options.size(), listed);
	}

	void OnNpcLine(RE::FormID npcId, const std::string& text)
	{
		bool hush = false;
		bool judge = false;
		{
			std::scoped_lock l(g_lock);
			auto it = g_offers.find(npcId);
			if (it == g_offers.end()) {
				return;
			}
			auto& o = it->second;
			if (Clock::now() < o.hushUntil) {
				// The vanilla line's own record arrives right after it fires, word for word. An AI line
				// copying those words comes seconds later - and brings its own actions: cut.
				const auto said = JsonUtil::Lower(text);
				const auto head = said.substr(0, (std::min)(said.size(), std::size_t{ 24 }));
				const bool itsOwnRecord = !said.empty() && !o.playing.empty() && o.playing.find(head) != std::string::npos &&
				                          Clock::now() - o.firedAt < std::chrono::milliseconds(1500);
				hush = !itsOwnRecord;
			} else if (!o.options.empty() && !o.replied) {
				o.replied = true;
				judge = true;
			}
		}
		if (judge) {
			SKSE::GetTaskInterface()->AddTask([npcId, text] { OnReply(npcId, text); });
		}
		if (hush) {
			logger::info("vanilla: an AI line came through while the vanilla line has the floor; cut");
			SKSE::GetTaskInterface()->AddTask([] { Purge(); });
		}
	}

	std::vector<std::string> BioLines(RE::FormID npcId)
	{
		std::scoped_lock l(g_lock);
		auto s = g_said.find(npcId);
		if (s == g_said.end() || Clock::now() - s->second.at > kOwnStory) {
			return {};
		}
		const std::string player = Game::Player()->GetName();
		return { std::format("Already said to {} a moment ago, and meant it: \"{}\". Don't say it again or bring it back up; answer what {} "
							 "says now. Anything it handed over (a reward, a favour) was theirs to give, gladly: never take it back, regret it, "
							 "or add to it.",
			player, s->second.answer, player) };
	}

	bool IsThreat(const std::string& line)
	{
		return Threatens(line);
	}

	bool FiredRecently(RE::FormID npcId, int seconds)
	{
		std::scoped_lock l(g_lock);
		auto it = g_fired.find(npcId);
		return it != g_fired.end() && Clock::now() - it->second < std::chrono::seconds(seconds);
	}

	void SelfTest()
	{
		logger::info("SELFTEST vanilla: plain rules, no model - checked offline by tools/logic_check.py");
	}
}
