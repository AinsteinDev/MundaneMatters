#include "Pledges.h"

#include "Confirm.h"

#include "Bonds.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Keys.h"
#include "Ledger.h"
#include "Promises.h"
#include "Services.h"
#include "SkyrimNetBridge.h"
#include "Trust.h"

namespace MundaneMatters::Pledges
{
	namespace
	{
		constexpr float kVisitWindow = 3.0f;     // game hours after the time said a visit still counts
		constexpr float kAwayDistance = 3000.0f;  // this far (or another building) counts as having gone away
		constexpr float kAwayHours = 1.0f;        // ...for at least this long (game hours): not a goodbye at the door
		constexpr float kMentionRange = 1500.0f;
		constexpr float kShowKeptFor = 24.0f * 5;  // kept/broken stay in the book this long

		enum class State
		{
			kOpen,
			kKept,
			kBroken,
			kTakenBack
		};

		struct Pledge
		{
			RE::FormID npc = 0;
			std::string npcName;
			std::string type;    // bring | find | visit | pay | deliver | check | other
			std::string item;    // "nord mead", "ring"
			RE::FormID itemBase = 0;       // the very thing, when they asked for it (an errand)
			RE::FormID recipientBase = 0;  // deliver / check: the person it's for (their TESNPC)
			std::string recipient;         // their name
			std::int32_t gold = 0;
			std::string words;   // what the player said
			std::string what;    // the promise in a few words ("visit her in Windhelm"), from the classifier
			std::string when;    // "tomorrow", "" when not said
			float made = 0.0f;
			float due = 0.0f;
			State state = State::kOpen;
			float closedAt = -1.0f;
			bool mentioned = false;  // a broken one brought up already
			bool away = false;       // visit: the player has really been away from them since promising
			float awayFrom = -1.0f;  // visit: game hours the player went away (a step out the door isn't a trip)
			std::uint32_t uid = 0;   // this session's handle for the manage popup (not saved)
		};

		std::mutex g_lock;
		std::vector<Pledge> g_pledges;
		std::atomic_uint32_t g_bookKey{ 0 };  // the Promises Book Key, refreshed by Tick
		std::atomic_bool g_ourBook{ false };  // the book screen open is our promises book
		std::uint32_t g_nextUid = 0;

		// The settings page records a key as a Windows virtual-key code ("Set", then press it); the
		// game's input events carry DirectInput scan codes. Windows translates one into the other
		// (extended keys - End, arrows, Home - are 0x80+xx in DirectInput).
		std::uint32_t ScanCodeOf(int vk)
		{
			if (vk <= 0 || vk > 255) {
				return 0;  // unset
			}
			const auto sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
			if (sc == 0) {
				return 0;  // a mouse button or something with no key behind it
			}
			// Windows hands these back as their numpad twins (End = numpad 1); DirectInput keeps them
			// apart with 0x80 (End = 207).
			static constexpr std::array<int, 17> kExtended{ VK_PRIOR, VK_NEXT, VK_END, VK_HOME, VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_INSERT,
				VK_DELETE, VK_DIVIDE, VK_RCONTROL, VK_RMENU, VK_LWIN, VK_RWIN, VK_APPS, VK_NUMLOCK };
			const bool extended = std::ranges::find(kExtended, vk) != kExtended.end();
			return extended ? ((sc & 0x7F) | 0x80) : (sc & 0xFF);
		}

		// An errand an NPC just asked for, waiting for the player to say yes.
		struct Ask
		{
			RE::FormID npc = 0;
			std::string type;  // bring | deliver | check
			RE::FormID itemBase = 0;
			std::string item;
			RE::FormID recipientBase = 0;
			std::string recipient;
			std::chrono::steady_clock::time_point until;
		};
		std::vector<Ask> g_asks;
		std::unordered_map<RE::FormID, float> g_lastAsk;  // NPC -> game hours they last asked
		float g_lastAnyAsk = -100.0f;

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

		float DefaultHours(const std::string& type)
		{
			return type == "find" || type == "other" || type == "deliver" || type == "check" ? 24.0f * 7 : type == "visit" ? 24.0f : 72.0f;
		}

		// Every promise has a time, generous but real: the time said with half again on top, never
		// under what the errand takes (a day to bring or pay, three to find or deliver; three, or five
		// for a delivery, when it means going to another town). A visit keeps the time said (with its
		// own window).
		float DueIn(const std::string& type, float said, bool distant)
		{
			if (said < 1.0f) {
				said = DefaultHours(type);
			}
			if (type == "visit") {
				return said;
			}
			float floor = type == "bring" || type == "pay" ? 24.0f : type == "other" ? 24.0f * 7 : 72.0f;
			if (distant) {
				floor = (std::max)(floor, type == "deliver" || type == "check" ? 120.0f : 72.0f);
			}
			return (std::max)(said * 1.5f, floor);
		}

		std::string TypeOf(std::string t)
		{
			t = JsonUtil::Lower(std::move(t));
			if (t.find("deliver") != std::string::npos || t.find("take") != std::string::npos || t.find("carry") != std::string::npos) {
				return "deliver";
			}
			if (t.find("check") != std::string::npos || t.find("look in") != std::string::npos || t.find("look_in") != std::string::npos) {
				return "check";
			}
			if (t.find("find") != std::string::npos || t.find("search") != std::string::npos || t.find("look") != std::string::npos) {
				return "find";
			}
			if (t.find("pay") != std::string::npos || t.find("gold") != std::string::npos || t.find("debt") != std::string::npos) {
				return "pay";
			}
			if (t.find("visit") != std::string::npos || t.find("come") != std::string::npos || t.find("meet") != std::string::npos ||
				t.find("return") != std::string::npos || t.find("back") != std::string::npos) {
				return "visit";
			}
			if (t.find("bring") != std::string::npos || t.find("give") != std::string::npos || t.find("get") != std::string::npos ||
				t.find("fetch") != std::string::npos || t.find("item") != std::string::npos) {
				return "bring";
			}
			return "other";
		}

		// "a bottle of Nord Mead", "your missing ring", "50 gold", "come see them tomorrow"
		// The promise part of what the player said, in a few words: from "I'll" / "I will" / "I promise"
		// on, at most eight words, "you" made "them" ("ill be sure to visit you" -> "be sure to visit them").
		std::string ShortClause(const std::string& words)
		{
			std::string lower = JsonUtil::Lower(words);
			for (std::size_t at; (at = lower.find("\xE2\x80\x99")) != std::string::npos;) {
				lower.replace(at, 3, "'");
			}
			std::vector<std::string> w;
			std::string cur;
			for (char c : lower + " ") {
				if (std::isalnum(static_cast<unsigned char>(c)) || c == '\'') {
					cur += c;
				} else if (!cur.empty()) {
					w.push_back(cur);
					cur.clear();
				}
			}
			// The last promise in the line ("whenever I'll be in Windhelm I'll be sure to visit you": the second).
			std::size_t start = 0;
			for (std::size_t i = 0; i < w.size(); ++i) {
				if (w[i] == "i'll" || w[i] == "ill" || w[i] == "promise" || w[i] == "swear" || (w[i] == "will" && i > 0 && w[i - 1] == "i")) {
					start = i + 1;
				}
			}
			// "be sure to" / "make sure to" / "definitely": not the promise itself.
			if (start + 1 < w.size() && (w[start] == "be" || w[start] == "make") && w[start + 1] == "sure") {
				start += 2;
				if (start < w.size() && w[start] == "to") {
					++start;
				}
			}
			if (start < w.size() && (w[start] == "definitely" || w[start] == "surely")) {
				++start;
			}
			std::string out;
			for (std::size_t i = start, n = 0; i < w.size() && n < 8; ++i, ++n) {
				const auto& x = w[i] == "you" ? std::string("them") : w[i] == "your" ? std::string("their") : w[i] == "yourself" ? std::string("themself") : w[i];
				out += (out.empty() ? "" : " ") + x;
			}
			return out.empty() ? words.substr(0, 60) : out;
		}

		std::string WhatOf(const Pledge& p)
		{
			if (p.type == "bring") {
				return std::format("bring {}", p.item.empty() ? "what was promised" : p.item);
			}
			if (p.type == "find") {
				return std::format("find {}", p.item.empty() ? "what they lost" : p.item);
			}
			if (p.type == "pay") {
				return p.gold > 0 ? std::format("pay back {} gold", p.gold) : "pay back what is owed";
			}
			if (p.type == "visit") {
				if (!p.what.empty()) {
					return p.what;
				}
				return std::format("come and see them{}", p.when.empty() ? "" : " " + p.when);
			}
			if (p.type == "deliver") {
				return std::format("take {} to {}", p.item.empty() ? "their parcel" : p.item, p.recipient.empty() ? "who they asked" : p.recipient);
			}
			if (p.type == "check") {
				return std::format("look in on {}", p.recipient.empty() ? "who they asked" : p.recipient);
			}
			return !p.what.empty() ? p.what : ShortClause(p.words);
		}

		// A standing offer has no deadline ("you can work for me", "you can stay with me if things go
		// wrong", "come to me if anyone bothers you"): it never goes overdue, it stays until kept or
		// taken back.
		constexpr float kOpenEnded = 1.0e9f;

		bool OpenEnded(const json& o, const std::string& playerLine)
		{
			if (JsonUtil::Str(o, "open") == "true") {
				return true;
			}
			const auto l = JsonUtil::Lower(playerLine);
			for (auto* w : { "whenever", "next time i'm", "next time im", "next time i am", "next time i'll", "next time ill", "when i'm in", "when im in",
					 "when i'm next", "if i'm ever", "if im ever", "when i pass", "if i pass", "when i come through", "when i'm back in", "when im back in",
					 "work for me", "you can stay with me", "stay with me if", "you can always", "whenever you need", "if you ever need",
					 "if anyone bothers you", "if anyone ever", "if things go wrong", "come to me if", "you know where to find me",
					 "you can come to me", "you can count on me", "any time you need", "anytime you need" }) {
				if (l.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		std::string DueText(float hoursLeft)
		{
			if (hoursLeft > kOpenEnded / 2) {
				return "no set time";
			}
			if (hoursLeft < 0.0f) {
				return "overdue";
			}
			if (hoursLeft < 12.0f) {
				return "before the day is out";
			}
			if (hoursLeft < 36.0f) {
				return "by tomorrow";
			}
			return std::format("within {} days", static_cast<int>(std::lround(hoursLeft / 24.0f)));
		}

		// Under g_lock: records it. The NPC is told by the caller, outside the lock (SkyrimNet may
		// read the bio, and with it this list, while it builds their answer).
		void Kept(Pledge& p, RE::Actor* npc)
		{
			p.state = State::kKept;
			p.closedAt = Game::Hours();
			Game::Notify(std::format("Promise kept: {} ({})", WhatOf(p), p.npcName));
			SkyrimNet::AddMemory(p.npc, std::format("{} kept their word to {}: they promised to {}, and did.", PlayerName(), p.npcName, WhatOf(p)), 0.7f);
			if (npc) {
				Bonds::Add(npc, Bonds::Deed::kWord, 1);
			}
			logger::info("pledge: kept to {}: {}", p.npcName, WhatOf(p));
		}
	}

	// ---- errands: someone close asks the player for something as they part ----

	namespace
	{
		using SteadyClock = std::chrono::steady_clock;
		constexpr float kAskEvery = 72.0f;     // game hours before the same person asks again
		constexpr float kAnyAskEvery = 12.0f;  // and between anyone's asks
		constexpr int kMaxOpen = 5;            // with this many promises open, nobody piles on more
		constexpr auto kAskWaits = std::chrono::minutes(5);

		struct Talk
		{
			RE::FormID npc = 0;
			SteadyClock::time_point at;
			bool handled = false;
		};
		Talk g_talk;  // the last conversation (game thread)

		// The player taking their leave: the words come at the end of what they said ("...anyway, take
		// care"), never in a question, and never "take care of it" (quoting or promising something).
		bool IsFarewell(const std::string& line)
		{
			auto l = JsonUtil::Lower(line);
			if (l.find('?') != std::string::npos) {
				return false;
			}
			for (std::size_t at; (at = l.find("take care of")) != std::string::npos;) {
				l.erase(at, 12);
			}
			while (!l.empty() && (std::isspace(static_cast<unsigned char>(l.back())) || std::ispunct(static_cast<unsigned char>(l.back())))) {
				l.pop_back();
			}
			for (auto* p : { "farewell", "goodbye", "good bye", "bye", "see you", "must go", "gotta go", "got to go", "take care", "i'm off", "be off",
					 "heading to", "heading out", "on my way", "leaving", "safe travels", "until next time", "be going", "need to go", "have to go" }) {
				// At the very end, give or take a few words ("see you around", "heading to Whiterun now").
				if (const auto at = l.rfind(p); at != std::string::npos && l.size() - (at + std::strlen(p)) <= 16) {
					return true;
				}
			}
			return false;
		}

		// Insults and threats: nobody asks a favour of someone who just spoke to them like that.
		bool IsRough(const std::string& line)
		{
			const auto l = JsonUtil::Lower(line);
			// A curse word counts when it's aimed at them ("you fucking elf"), not at the world ("oh fuck, a saber").
			const auto words = JsonUtil::Words(l);
			const bool atThem = std::ranges::any_of(words, [](const std::string& w) { return w == "you" || w == "your" || w == "youre" || w == "u" || w == "ya"; });
			for (auto* p : { "fuck", "bitch", "bastard", "idiot", "scum", "filth" }) {
				if (atThem && l.find(p) != std::string::npos) {
					return true;
				}
			}
			for (auto* p : { "n'wah", "nwah", "n'wa", "piss off", "shut up", "or else", "kill you",
					 "i'll kill", "ill kill", "lose those", "break your", "you'll regret", "youll regret", "don't you dare", "dont you dare", "or you will",
					 "or you'll", "or youll", "cut off", "gut you", "you're dead", "youre dead", "worthless", "pathetic" }) {
				if (l.find(p) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// Not a firm commitment: a maybe ("I might", "perhaps"), a try ("I'll try", "I'll do my best"), a
		// "we'll see" ("I'll see what I can do", "no promises", "if I can"). Only a definite yes is a promise.
		// "I'll see you tomorrow" is seeing someone, not "I'll see".
		// Lowercase, with "guarantee" however it's spelled ("gurantee", "garuntee").
		std::string Hedged(const std::string& line)
		{
			auto l = JsonUtil::Lower(line);
			std::string out, word;
			auto flush = [&] {
				if (word.size() >= 7 && word[0] == 'g' && (word.ends_with("ntee") || word.ends_with("nty") || word.ends_with("ntie"))) {
					word = "guarantee";
				}
				out += word;
				word.clear();
			};
			for (char c : l) {
				if (std::isalpha(static_cast<unsigned char>(c))) {
					word += c;
				} else {
					flush();
					out += c;
				}
			}
			flush();
			return out;
		}

		bool UnsureIn(const std::string& line);
		bool Commits(const std::string& line);  // below

		// A hedge about the promise itself: in the sentence that commits ("I'll try to come by"), or a short
		// one of its own ("I'll be there. Maybe."). Not a "might" in some other sentence ("...and your eyes
		// might make someone lost in them").
		bool Unsure(const std::string& line)
		{
			std::vector<std::string> sentences;
			std::string cur;
			for (char c : line + ".") {
				if (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n') {
					if (!JsonUtil::Words(cur).empty()) {
						sentences.push_back(cur);
					}
					cur.clear();
				} else {
					cur += c;
				}
			}
			bool anyCommit = false;
			for (auto& s : sentences) {
				const bool commits = Commits(s);
				anyCommit = anyCommit || commits;
				if ((commits || JsonUtil::Words(s).size() <= 3) && UnsureIn(s)) {
					return true;
				}
			}
			return !anyCommit && UnsureIn(line);
		}

		bool UnsureIn(const std::string& line)
		{
			// "You might like it", "eyes that might...": someone else's might, not a hedge on the promise.
			auto l = " " + Hedged(line) + " ";
			for (const std::string x : { " you might ", " you may ", " they might ", " she might ", " he might ", " that might ", " which might " }) {
				for (std::size_t at; (at = l.find(x)) != std::string::npos;) {
					l.replace(at, x.size(), " ");
				}
			}
			for (auto* w : { "maybe", "perhaps", "might ", "someday", "some day", "one day", "we'll see", "well see", "possibly", "i'll try", "ill try",
					 "i will try", "i'll do my best", "ill do my best", "do what i can", "see what i can", "if i can", "if i get the chance",
					 "no promise", "can't promise", "cant promise", "cannot promise", "not promising", "not a guarantee", "no guarantee",
					 "can't guarantee", "cant guarantee", "think about it", "don't hold me", "dont hold me", "i'll give it a try", "i'll give it a shot",
					 "ill give it a try", "ill give it a shot", "i'll see if", "ill see if", "i will see if" }) {
				if (l.find(w) != std::string::npos) {
					return true;
				}
			}
			// A bare "I'll see" ("okay, I'll see."), not "I'll see you / her / him / them".
			for (auto* w : { "i'll see", "ill see", "i will see" }) {
				for (auto at = l.find(w); at != std::string::npos; at = l.find(w, at + 1)) {
					const auto rest = l.substr(at + std::strlen(w), 6);
					if (!(rest.starts_with(" you") || rest.starts_with(" u ") || rest.starts_with(" her") || rest.starts_with(" him") ||
							rest.starts_with(" them") || rest.starts_with(" ya"))) {
						return true;
					}
				}
			}
			return false;
		}

		// The player's own line commits to something: "I will", "I'll", "you have my word", or a yes to
		// what was asked ("sure", "deal"). A bare question ("what do you mean?") commits to nothing, even
		// when the NPC answers with what they'd like the player to do.
		bool Commits(const std::string& line)
		{
			// Whole words and phrases only: "yes" isn't in "yesterday", nor "deal" in "dealt".
			std::string lower = JsonUtil::Lower(line);
			for (std::size_t at; (at = lower.find("\xE2\x80\x99")) != std::string::npos;) {
				lower.replace(at, 3, "'");
			}
			std::string l = " ";
			for (char c : lower) {
				l += std::isalnum(static_cast<unsigned char>(c)) || c == '\'' ? c : ' ';
			}
			l += " ";
			for (auto* w : { "i will", "i'll", "ill", "i shall", "i promise", "promise you", "my word", "sure", "yes", "yeah", "yep", "of course",
					 "count on me", "consider it done", "deal", "alright", "all right", "okay", "ok", "fine", "i can do that",
					 "no problem", "will do", "you got it", "gladly", "absolutely", "certainly", "happy to", "i'm on it", "im on it", "leave it to me",
					 "i'll be back", "i will be back", "see you tomorrow", "i'll see you", "ill see you" }) {
				if (l.find(std::string(" ") + w + " ") != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// Words, apostrophes kept, padded with spaces: whole-phrase matching.
		std::string Spaced(const std::string& line)
		{
			std::string lower = JsonUtil::Lower(line);
			for (std::size_t at; (at = lower.find("\xE2\x80\x99")) != std::string::npos;) {
				lower.replace(at, 3, "'");
			}
			std::string l = " ";
			for (char c : lower) {
				l += std::isalnum(static_cast<unsigned char>(c)) || c == '\'' ? c : ' ';
			}
			return l + " ";
		}

		bool HasPhrase(const std::string& spaced, std::initializer_list<const char*> phrases)
		{
			return std::ranges::any_of(phrases, [&](const char* w) { return spaced.find(std::string(" ") + w + " ") != std::string::npos; });
		}

		// A real commitment ("I'll", "I promise", "you have my word"), not just "okay" / "sure".
		bool StrongCommit(const std::string& line)
		{
			return HasPhrase(Spaced(line), { "i will", "i'll", "ill", "i shall", "i promise", "promise you", "my word", "i swear", "count on me",
				"consider it done", "leave it to me", "will do", "you got it", "i'm on it", "im on it" });
		}

		// An oath, not just "I will": what a vague promise (no task to check) needs ("I promise", "you have
		// my word", "I'll make it up to you"). "I will go there" is a plan, not a promise to anyone.
		bool Oath(const std::string& line)
		{
			return HasPhrase(Spaced(line), { "i promise", "promise you", "my word", "i swear", "count on me", "consider it done", "leave it to me",
				"make it up to you", "make it up to them", "you can rely on me", "i give you my word", "on my honor", "on my honour" });
		}

		// Doing it together, on this outing ("let's find you a kill, then we head home"): a plan the two
		// of them share, not something the player owes the NPC later.
		bool JointPlan(const std::string& line)
		{
			return HasPhrase(Spaced(line), { "let's", "lets", "let us", "we", "we'll", "we will", "we can", "we should", "together", "us" });
		}

		// Taking one's leave ("time I take my leave", "I'll be off", "farewell").
		bool Departing(const std::string& line)
		{
			return HasPhrase(Spaced(line), { "take my leave", "time i go", "time i left", "time to go", "i should go", "i should get going",
				"i'll be off", "ill be off", "i'll be going", "ill be going", "be on my way", "farewell", "goodbye", "good bye", "see you later",
				"see you around", "i must go", "i have to go", "i need to go" });
		}

		// A visit promised in so many words ("I'll come back tomorrow", "I'll stop by"). "I'll be off", "see
		// you later" are goodbyes, not a promise to return.
		bool SaysVisit(const std::string& line)
		{
			std::string lower = JsonUtil::Lower(line);
			for (std::size_t at; (at = lower.find("\xE2\x80\x99")) != std::string::npos;) {
				lower.replace(at, 3, "'");
			}
			std::string l = " ";
			for (char c : lower) {
				l += std::isalnum(static_cast<unsigned char>(c)) || c == '\'' ? c : ' ';
			}
			l += " ";
			for (auto* w : { "come back", "be back", "i'll return", "i will return", "ill return", "visit", "stop by", "drop by", "come by", "come see",
					 "come and see", "come over", "see you tomorrow", "see you tonight", "see you in the morning", "i'll be there", "ill be there",
					 "i'll come", "i will come", "ill come", "back tomorrow", "back later", "back soon" }) {
				if (l.find(std::string(" ") + w + " ") != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// Plainly not a promise after all ("it's not a guarantee", "no promises", "don't hold me to it"):
		// the one just made to them is withdrawn - a clarification, not a broken word.
		bool Withdraws(const std::string& line)
		{
			const auto l = Hedged(line);
			for (auto* w : { "no promise", "not a guarantee", "no guarantee", "not guaranteed", "can't promise", "cant promise", "cannot promise",
					 "not promising", "don't hold me to", "dont hold me to", "not a promise", "can't guarantee", "cant guarantee" }) {
				if (l.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		constexpr float kWithdrawWithin = 3.0f;  // game hours: a promise just made can still be taken as "not really a promise"

		std::unordered_map<RE::FormID, SteadyClock::time_point> g_rough;  // when the player last spoke roughly to them (game thread)
		constexpr auto kRoughLasts = std::chrono::minutes(20);

		float AskChance(RE::Actor* npc)
		{
			auto* base = npc->GetActorBase();
			auto* rel = base ? RE::BGSRelationship::GetRelationship(base, Game::Player()->GetActorBase()) : nullptr;
			using L = RE::BGSRelationship::RELATIONSHIP_LEVEL;
			switch (rel ? rel->level.get() : L::kAcquaintance) {
			case L::kLover:
				return 0.30f;
			case L::kAlly:
				return 0.25f;
			case L::kConfidant:
				return 0.20f;
			case L::kFriend:
				return 0.12f;
			case L::kAcquaintance:
				return 0.04f;
			default:
				return 0.0f;  // a rival asks no favours
			}
		}

		// Something they'd plausibly want brought, by what they do: a real item, so handing it
		// over can be recognised.
		RE::TESBoundObject* WantedItem(RE::Actor* npc, std::mt19937& rng)
		{
			auto* base = npc->GetActorBase();
			const auto cls = JsonUtil::Lower(base && base->npcClass && base->npcClass->GetName() ? base->npcClass->GetName() : "");
			std::vector<const char*> names;
			if (cls.find("alchem") != std::string::npos || cls.find("apothec") != std::string::npos || cls.find("herb") != std::string::npos) {
				names = { "Blue Mountain Flower", "Lavender", "Snowberries", "Juniper Berries", "Wheat", "Thistle Branch" };
			} else if (cls.find("smith") != std::string::npos) {
				names = { "Iron Ore", "Leather Strips", "Corundum Ore", "Firewood" };
			} else if (cls.find("innkeeper") != std::string::npos || cls.find("cook") != std::string::npos || Services::RoomOf(npc)) {
				names = { "Salmon Meat", "Venison", "Nord Mead", "Leek", "Cabbage" };
			} else if (cls.find("bard") != std::string::npos) {
				names = { "Alto Wine", "Wine", "Nord Mead", "Sweet Roll" };
			} else {
				names = { "Sweet Roll", "Nord Mead", "Apple Pie", "Salmon Steak", "Wine", "Cheese Wedge", "Blue Mountain Flower" };
			}
			std::ranges::shuffle(names, rng);
			for (auto* want : names) {
				for (const auto form : { RE::FormType::AlchemyItem, RE::FormType::Ingredient, RE::FormType::Misc }) {
					auto& arr = RE::TESDataHandler::GetSingleton()->GetFormArray(form);
					for (auto* f : arr) {
						auto* b = f ? f->As<RE::TESBoundObject>() : nullptr;
						auto* file = f ? f->GetFile(0) : nullptr;
						if (b && file && std::string_view(file->GetFilename()) == "Skyrim.esm" && b->GetName() && _stricmp(b->GetName(), want) == 0) {
							return b;
						}
					}
				}
			}
			return nullptr;
		}

		// Someone they really know in the game's data (family, friends), and what they are to them.
		std::optional<std::pair<RE::TESNPC*, std::string>> Someone(RE::Actor* npc, std::mt19937& rng)
		{
			auto* base = npc->GetActorBase();
			if (!base || !base->relationships) {
				return std::nullopt;
			}
			std::vector<std::pair<RE::TESNPC*, std::string>> out;
			for (auto* rel : *base->relationships) {
				if (!rel || !rel->npc1 || !rel->npc2) {
					continue;
				}
				auto* other = rel->npc1 == base ? rel->npc2 : rel->npc1;
				if (other == Game::Player()->GetActorBase() || !other->IsUnique() || !other->GetName() || !*other->GetName()) {
					continue;
				}
				// Dead, as far as the loaded world knows: no errand to them.
				bool dead = false;
				if (auto* lists = RE::ProcessLists::GetSingleton()) {
					for (auto& h : lists->highActorHandles) {
						if (auto a = h.get(); a && a->GetActorBase() == other) {
							dead = a->IsDead();
						}
					}
				}
				if (dead) {
					continue;
				}
				std::string label = "friend";
				if (rel->assocType) {
					const int role = rel->npc1 == base ? RE::BGSAssociationType::Members::kChild : RE::BGSAssociationType::Members::kParent;
					const int sex = other->GetSex() == RE::SEX::kFemale ? RE::BGSAssociationType::Sexes::kFemale : RE::BGSAssociationType::Sexes::kMale;
					if (const char* l = rel->assocType->associationLabels[role][sex].c_str(); l && *l) {
						label = JsonUtil::Lower(l);
					}
				}
				out.emplace_back(other, label);
			}
			if (out.empty()) {
				return std::nullopt;
			}
			return out[std::uniform_int_distribution<std::size_t>(0, out.size() - 1)(rng)];
		}

		// A small thing of theirs to send along (never worn, quest or valuable).
		RE::TESBoundObject* Parcel(RE::Actor* npc)
		{
			for (auto& [obj, entry] : npc->GetInventory()) {
				if (!obj || entry.first <= 0 || obj == Game::Gold() || obj->GetGoldValue() > 40 || !obj->GetPlayable() || !obj->GetName() || !*obj->GetName() ||
					(entry.second && (entry.second->IsWorn() || entry.second->IsQuestObject())) || obj->IsWeapon() || obj->IsArmor() || obj->Is(RE::FormType::Ammo)) {
					continue;
				}
				if (obj->Is(RE::FormType::AlchemyItem) || obj->Is(RE::FormType::Book) || obj->Is(RE::FormType::Misc) || obj->Is(RE::FormType::Ingredient)) {
					return obj;
				}
			}
			return nullptr;
		}

		void TryAsk(RE::FormID npcId, bool walkedAway)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* player = Game::Player();
			if (!npc || npc->IsDead() || npc->IsChild() || npc->IsInCombat() || player->IsInCombat() || npc->IsHostileToActor(player) ||
				Services::IsCompanion(npc) || !SkyrimNet::ConfigBool("features.npc_errands", true) || !SkyrimNet::ConfigBool("features.player_promises", true) ||
				Trust::Holds(npcId)) {
				return;
			}
			// Just threatened or insulted: no favours.
			if (auto it = g_rough.find(npcId); it != g_rough.end() && SteadyClock::now() - it->second < kRoughLasts) {
				logger::info("errand: {} was just spoken to roughly; no favour asked", npc->GetName());
				return;
			}
			static std::mt19937 rng{ std::random_device{}() };
			const float now = Game::Hours();
			float chance = AskChance(npc);
			Ask ask;
			{
				std::scoped_lock l(g_lock);
				const auto open = std::ranges::count_if(g_pledges, [](const Pledge& p) { return p.state == State::kOpen; });
				const bool waiting = std::ranges::any_of(g_pledges, [&](const Pledge& p) { return p.state == State::kOpen && p.npc == npcId; }) ||
				                     std::ranges::any_of(g_asks, [&](const Ask& a) { return a.npc == npcId && SteadyClock::now() < a.until; });
				// The more the player already owes people, the less anyone asks; nobody asks twice in a row.
				chance *= (std::max)(0.0f, 1.0f - static_cast<float>(open) / kMaxOpen);
				auto it = g_lastAsk.find(npcId);
				if (waiting || open >= kMaxOpen || (it != g_lastAsk.end() && now - it->second < kAskEvery) || now - g_lastAnyAsk < kAnyAskEvery) {
					return;
				}
			}
			if (std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) >= chance) {
				return;
			}
			// What to ask: bring something (always possible), take something to someone they know, or look in on them.
			auto who = Someone(npc, rng);
			auto* parcel = who ? Parcel(npc) : nullptr;
			const int pick = std::uniform_int_distribution<int>(0, 3)(rng);
			ask.npc = npcId;
			ask.until = SteadyClock::now() + kAskWaits;
			std::string request;
			const std::string name = npc->GetName();
			if (who && parcel && pick == 2) {
				ask.type = "deliver";
				ask.itemBase = parcel->GetFormID();
				ask.item = parcel->GetName();
				ask.recipientBase = who->first->GetFormID();
				ask.recipient = who->first->GetName();
				request = std::format("if they would take {} to {}, their {}, when they were passing that way", ask.item, ask.recipient, who->second);
			} else if (who && pick == 3) {
				ask.type = "check";
				ask.recipientBase = who->first->GetFormID();
				ask.recipient = who->first->GetName();
				request = std::format("to look in on {}, their {}, if they passed by; they had been worried", ask.recipient, who->second);
			} else if (auto* item = WantedItem(npc, rng)) {
				ask.type = "bring";
				ask.itemBase = item->GetFormID();
				ask.item = item->GetName();
				request = std::format("to bring back {} next time they came by; they had run short", ask.item);
			} else {
				return;
			}
			{
				std::scoped_lock l(g_lock);
				g_asks.push_back(ask);
				g_lastAsk[npcId] = now;
				g_lastAnyAsk = now;
			}
			logger::info("errand: {} asks {} ({}, {})", name, ask.type, ask.item.empty() ? ask.recipient : ask.item, walkedAway ? "walking away" : "farewell");
			Game::Narrate(std::format("{} {} asked {} {}. (A favour asked in their own words, with no rush and no set day: whenever {} is passing that way. {} hasn't agreed yet.)", name,
							  walkedAway ? "called after them as they walked off and" : "as they were saying goodbye,", PlayerName(), request, PlayerName(), PlayerName()),
				npc);
		}
	}

	bool Grant(RE::Actor* npc, const std::string& type, const std::string& item, const std::string& words, float hours)
	{
		if (!npc) {
			return false;
		}
		const float now = Game::Hours();
		Pledge p;
		p.npc = npc->GetFormID();
		p.npcName = npc->GetName();
		p.type = TypeOf(type);
		p.item = item;
		p.words = words;
		p.made = now;
		p.due = now + (hours > 0.0f ? hours : DueIn(p.type, 0.0f, false));
		std::scoped_lock l(g_lock);
		for (auto& x : g_pledges) {
			if (x.npc == p.npc && x.state == State::kOpen && x.type == p.type && JsonUtil::Lower(x.item) == JsonUtil::Lower(p.item)) {
				return false;
			}
		}
		Game::Notify(std::format("You promised {}: {}", p.npcName, WhatOf(p)));
		SkyrimNet::AddMemory(p.npc, std::format("{} promised {} to {}.", PlayerName(), p.npcName, WhatOf(p)), 0.6f);
		logger::info("pledge: granted to {}: {} (due in {:.0f} h)", p.npcName, WhatOf(p), p.due - now);
		g_pledges.push_back(std::move(p));
		return true;
	}

	bool Waiting(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		return std::ranges::any_of(g_asks, [&](const Ask& a) { return a.npc == npc && SteadyClock::now() < a.until; });
	}

	void OnExchange(RE::FormID npc, const std::string& playerLine)
	{
		g_talk = { npc, SteadyClock::now(), false };
		if (IsRough(playerLine)) {
			g_rough[npc] = SteadyClock::now();
		}
		// "It's not a guarantee, but I'll try": the promise just made to them wasn't one.
		if (Withdraws(playerLine)) {
			std::string what, name;
			{
				std::scoped_lock l(g_lock);
				const float now = Game::Hours();
				Pledge* last = nullptr;
				for (auto& p : g_pledges) {
					if (p.npc == npc && p.state == State::kOpen && now - p.made <= kWithdrawWithin && (!last || p.made > last->made)) {
						last = &p;
					}
				}
				if (last) {
					last->state = State::kTakenBack;
					last->closedAt = now;
					what = WhatOf(*last);
					name = last->npcName;
				}
			}
			if (!what.empty()) {
				SkyrimNet::AddMemory(npc, std::format("{} made clear it was no promise, only that they'd try: {}.", PlayerName(), what), 0.4f);
				Game::Notify(std::format("Not a promise after all: {} ({})", what, name));
				logger::info("pledge: withdrawn as no real promise to {}: {}", name, what);
			}
		}
		if (IsFarewell(playerLine)) {
			g_talk.handled = true;
			// Once their goodbye is said.
			std::thread([npc] {
				std::this_thread::sleep_for(std::chrono::seconds(6));
				SKSE::GetTaskInterface()->AddTask([npc] { TryAsk(npc, false); });
			}).detach();
		}
	}

	void FromExchange(RE::FormID npcId, const json& promises, const std::string& keptText, const std::string& playerLine)
	{
		if (!SkyrimNet::ConfigBool("features.player_promises", true)) {
			return;
		}
		auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
		if (!npc) {
			return;
		}
		const float now = Game::Hours();
		// "Later maybe?", "perhaps someday", "we'll see": a hedge, not a promise.
		{
			const auto l = JsonUtil::Lower(playerLine);
			if (!Commits(playerLine)) {
				if (promises.is_array() && !promises.empty()) {
					logger::info("pledge: \"{}\" commits to nothing (a question, or the NPC's own ask); no promise", playerLine);
				}
				return;
			}
			if (Unsure(playerLine)) {
				if (promises.is_array() && !promises.empty()) {
					logger::info("pledge: \"{}\" is a maybe, not a promise", playerLine);
				}
				return;
			}
		}
		json list = promises.is_array() ? promises : promises.is_object() ? json::array({ promises }) : json::array();
		// A clear yes to a favour they just asked ("sure man, I'll get one"): the promise is that favour,
		// whether or not the classifier listed it (it can miss it, or a vanilla line just went off).
		if (list.empty()) {
			std::scoped_lock l(g_lock);
			if (auto ask = std::ranges::find_if(g_asks, [&](const Ask& a) { return a.npc == npcId && SteadyClock::now() < a.until; }); ask != g_asks.end()) {
				list.push_back({ { "type", ask->type }, { "item", ask->item }, { "when", "now" } });
				logger::info("pledge: \"{}\" says yes to {}'s favour ({} {})", playerLine, npc->GetName(), ask->type, ask->item.empty() ? ask->recipient : ask->item);
			}
		}
		for (auto& o : list) {
			if (!o.is_object()) {
				continue;
			}
			Pledge p;
			p.npc = npcId;
			p.npcName = npc->GetName();
			p.type = TypeOf(JsonUtil::Str(o, "type"));
			if (p.type == "other" && SaysVisit(playerLine)) {
				p.type = "visit";  // "I'll be sure to visit you": kept by coming to see them
			}
			p.what = JsonUtil::Str(o, "what");
			if (p.what.size() > 80) {
				p.what = p.what.substr(0, 77) + "...";
			}
			// A goodbye with an "okay" in it, or a vague "other": only a real "I'll" / "I promise" makes it one.
			if (Departing(playerLine) && !StrongCommit(playerLine)) {
				logger::info("pledge: \"{}\" is a goodbye with an \"okay\", not a promise; not recorded", playerLine);
				continue;
			}
			if (JointPlan(playerLine) && !StrongCommit(playerLine)) {
				logger::info("pledge: \"{}\" is a plan to do it together, not a promise; not recorded", playerLine);
				continue;
			}
			if (p.type == "other" && !Oath(playerLine)) {
				logger::info("pledge: \"{}\" has no oath in it (a plan or an \"I will\"); a vague promise isn't recorded", playerLine);
				continue;
			}
			if (p.type == "visit" && !SaysVisit(playerLine)) {
				logger::info("pledge: \"{}\" promises no visit (a goodbye, or the NPC's own idea); not recorded", playerLine);
				continue;
			}
			p.item = JsonUtil::Str(o, "item");
			p.gold = static_cast<std::int32_t>(std::clamp(JsonUtil::Num(o, "gold").value_or(0.0), 0.0, 1e6));
			p.when = JsonUtil::Str(o, "time_hint");
			p.words = playerLine.size() > 140 ? playerLine.substr(0, 137) + "..." : playerLine;
			p.made = now;
			const float in = static_cast<float>(std::clamp(JsonUtil::Num(o, "in_hours").value_or(0.0), 0.0, 24.0 * 60));
			const auto distantText = JsonUtil::Lower(JsonUtil::Str(o, "far"));
			const bool distant = distantText == "true" || distantText == "yes" || distantText == "1";
			p.due = OpenEnded(o, playerLine) ? now + kOpenEnded : now + DueIn(p.type, in, distant);
			const bool cancel = JsonUtil::Lower(JsonUtil::Str(o, "when")).find("cancel") != std::string::npos;
			std::scoped_lock l(g_lock);
			// The same promise again ("I'll bring the mead, I promise!") is one promise; taking it back closes it.
			auto same = std::ranges::find_if(g_pledges, [&](const Pledge& x) {
				return x.state == State::kOpen && x.npc == npcId && x.type == p.type &&
				       (p.item.empty() || x.item.empty() || Promises::NameScore(p.item, x.item.c_str()) > 0.0 || p.type == "visit" || p.type == "pay");
			});
			if (cancel) {
				if (same != g_pledges.end()) {
					same->state = State::kTakenBack;
					same->closedAt = now;
					SkyrimNet::AddMemory(npcId, std::format("{} took back their promise to {}.", PlayerName(), WhatOf(*same)), 0.5f);
					Game::Notify(std::format("Promise taken back: {} ({})", WhatOf(*same), same->npcName));
					logger::info("pledge: taken back from {}: {}", same->npcName, WhatOf(*same));
				}
				continue;
			}
			if (same != g_pledges.end()) {
				// "Not tomorrow, in a couple of days": the newer time stands (still generous).
				if (in >= 1.0f || distant) {
					same->due = now + DueIn(same->type, in, distant);
					same->when = p.when;
					Game::Notify(std::format("Promise to {}: {} ({})", same->npcName, WhatOf(*same), DueText(same->due - now)));
					logger::info("pledge: {} re-timed, due in {:.0f} h", WhatOf(*same), same->due - now);
				}
				continue;
			}
			// Saying yes to what they just asked: the errand's own thing and person.
			if (auto ask = std::ranges::find_if(g_asks, [&](const Ask& a) {
					// Whatever words the yes came in: it answers what they just asked.
					return a.npc == npcId && std::chrono::steady_clock::now() < a.until && p.type != "pay";
				});
				ask != g_asks.end()) {
				p.type = ask->type;
				p.itemBase = ask->itemBase;
				p.item = ask->item.empty() ? p.item : ask->item;
				p.recipientBase = ask->recipientBase;
				p.recipient = ask->recipient;
				if (in < 1.0f) {
					p.due = now + DueIn(p.type, p.type == "bring" ? 24.0f * 4 : 24.0f * 5, distant);  // a favour asked has no rush to it
				}
				// A delivery: they hand the thing over now.
				if (p.type == "deliver" && p.itemBase) {
					if (auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(p.itemBase); item && Game::ItemCount(npc, item) > 0) {
						npc->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kStoreInTeammate, nullptr, Game::Player());
						Game::Notify(std::format("{} gave you {} to deliver", p.npcName, item->GetName()));
					}
				}
				g_asks.erase(ask);
			}
			Game::Notify(std::format("You promised {}: {}", p.npcName, WhatOf(p)));
			SkyrimNet::AddMemory(npcId, std::format("{} promised {} to {}{}.", PlayerName(), p.npcName, WhatOf(p), p.when.empty() ? "" : " (" + p.when + ")"), 0.6f);
			if (p.due - now > kOpenEnded / 2) {
				logger::info("pledge: to {}: {} (no set time)", p.npcName, WhatOf(p));
			} else {
				logger::info("pledge: to {}: {} (due in {:.0f} h)", p.npcName, WhatOf(p), p.due - now);
			}
			g_pledges.push_back(std::move(p));
		}
		// "Thank you for coming back like you said": what the NPC says the player did.
		if (!JsonUtil::Words(keptText).empty()) {
			std::scoped_lock l(g_lock);
			for (auto& p : g_pledges) {
				// The thing they name must be this promise: a "thank you for coming" doesn't keep "help with the mill".
				if (p.state == State::kOpen && p.npc == npcId &&
					(Promises::NameScore(keptText, p.item.c_str()) > 0.0 || Promises::NameScore(keptText, p.words.c_str()) > 0.0 ||
						Promises::NameScore(keptText, p.what.c_str()) > 0.0)) {
					Kept(p, npc);  // they're already saying so
					break;
				}
			}
		}
	}

	std::int32_t Delivered(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t n, bool* early)
	{
		if (early) {
			*early = false;
		}
		if (!npc || !item || n <= 0) {
			return 0;
		}
		std::scoped_lock l(g_lock);
		auto* base = npc->GetActorBase();
		const auto matches = [&](const Pledge& p) {
			// The thing itself, its name, or the kind of thing promised ("meat": Venison).
			return (p.itemBase && p.itemBase == item->GetFormID()) || p.item.empty() || Promises::NameScore(p.item, item->GetName()) > 0.0 ||
			       Promises::KindFits(p.item, item);
		};
		for (auto& p : g_pledges) {
			if (p.state != State::kOpen || !matches(p)) {
				continue;
			}
			if (p.npc == npc->GetFormID() && (p.type == "bring" || p.type == "find")) {
				// Well ahead of time (more than half the time given still left): they notice.
				if (early && p.due > p.made && p.due - Game::Hours() > (p.due - p.made) * 0.5f) {
					*early = true;
				}
				Kept(p, npc);  // the handover itself tells them
				return 1;      // one promised thing; the rest of the handover is whatever else it is
			}
			// Delivered to the person it was meant for: kept to the one who sent it.
			if (p.type == "deliver" && base && p.recipientBase == base->GetFormID()) {
				Kept(p, RE::TESForm::LookupByID<RE::Actor>(p.npc));
				SkyrimNet::AddMemory(npc->GetFormID(), std::format("{} brought {} {} from {}.", PlayerName(), npc->GetName(), item->GetName(), p.npcName), 0.5f);
				return 1;
			}
		}
		return 0;
	}

	bool Paid(RE::Actor* npc, std::int32_t gold)
	{
		if (!npc || gold <= 0) {
			return false;
		}
		std::scoped_lock l(g_lock);
		for (auto& p : g_pledges) {
			if (p.state == State::kOpen && p.npc == npc->GetFormID() && p.type == "pay" && (p.gold <= 0 || gold >= p.gold)) {
				Kept(p, npc);
				return true;
			}
		}
		return false;
	}

	bool AsksAboutPromises(const std::string& line, bool spoken)
	{
		const auto l = JsonUtil::Lower(line);
		// Said aloud to someone: only plain asks ("I promise, what else can I do?" is not one).
		if (spoken) {
			for (auto* p : { "my promises", "what did i promise", "what i promised", "what have i promised", "check my promises", "promises i have",
					 "promises i've made", "what i owe" }) {
				if (l.find(p) != std::string::npos) {
					return true;
				}
			}
			return false;
		}
		if (l.find("promise") != std::string::npos && (l.find("what") != std::string::npos || l.find("my ") != std::string::npos ||
														  l.find("check") != std::string::npos || l.find("remind") != std::string::npos ||
														  l.find("list") != std::string::npos || l.find("did i") != std::string::npos)) {
			return true;
		}
		const auto words = JsonUtil::Words(line);
		auto has = [&](std::string_view w) { return std::ranges::find(words, std::string(w)) != words.end(); };
		// A thought is the player talking to themself, so it can be broad: an errand word together
		// with checking on it. "I forgot what I wanted to get for Ronja", "let's see if I have anyone
		// to bring something to", "who was I supposed to take this to?", "do I owe anyone anything?"
		// (SkyrimNet rewrites typed thoughts in the character's voice, so wording drifts: "Nobody I
		// know well enough here to bring anything to").
		const bool errand = has("bring") || has("deliver") || has("fetch") || has("owe") || has("errand") || has("errands") || has("favour") ||
		                    has("favor") || has("favours") || has("favors") || has("promised") || has("pay") || has("find") || has("get") ||
		                    has("buy") || has("take") || has("do");
		// ...but only when actually checking: SkyrimNet also makes up thoughts on its own ("Mjoll
		// offered to help find Cassian, more than I've gotten from anyone"), which mustn't open it.
		bool checking = has("forgot") || has("forget") || has("remember") || has("remind") || has("supposed") || has("check") || has("owe");
		for (auto* p : { "see if", "did i", "do i have", "have i", "who was i", "what was i", "what did i", "am i forgetting", "need to bring",
				 "need to get", "have to bring", "have to get", "meant to", "anyone waiting", "anything to bring" }) {
			checking = checking || l.find(p) != std::string::npos;
		}
		return errand && checking;
	}

	namespace
	{
		// Broken: they remember it, bring it up when they next see the player, and the bond suffers.
		// g_lock held.
		void Break(Pledge& p, bool penalty)
		{
			p.state = State::kBroken;
			p.closedAt = Game::Hours();
			p.mentioned = false;
			SkyrimNet::AddMemory(p.npc, std::format("{} promised {} to {}, and never did. A broken promise.", PlayerName(), p.npcName, WhatOf(p)), 0.8f);
			Game::Notify(std::format("Promise broken: {} ({})", WhatOf(p), p.npcName));
			if (penalty) {
				Bonds::Lose(p.npc, 3);
			}
			logger::info("pledge: broken to {}: {}{}", p.npcName, WhatOf(p), penalty ? "" : " (no penalty)");
		}
	}

	void ManageBook()
	{
		static std::atomic_bool open{ false };
		if (open) {
			return;  // the popup is up already (the key pressed again)
		}
		Confirm::Offer offer;
		offer.title = "Promises";
		offer.npc = "Your word";
		offer.detail = "Break one: they're let down, and it costs you with them. Dissolve one: it goes as if never made.";
		offer.showPrice = false;
		offer.seconds = 0;
		offer.acceptLabel = "Break";
		offer.altLabel = "Dissolve";
		offer.declineLabel = "Close";
		offer.hint = "Click a promise, then Break or Dissolve.";
		offer.chosenText = "Chosen: ";
		const float now = Game::Hours();
		{
			std::scoped_lock l(g_lock);
			for (auto& p : g_pledges) {
				if (p.state != State::kOpen) {
					continue;
				}
				if (!p.uid) {
					p.uid = ++g_nextUid;
				}
				offer.pick.push_back({ p.uid, std::format("To {}: {}", p.npcName, WhatOf(p)), 1, 0, false, DueText(p.due - now) });
			}
		}
		if (offer.pick.empty()) {
			Game::Notify("No open promises.");
			return;
		}
		open = true;
		const bool shown = Confirm::Ask(offer, [](Confirm::Choice c) {
			open = false;
			if (c != Confirm::Choice::kAccept && c != Confirm::Choice::kAlt) {
				return;
			}
			const auto uid = Confirm::LastPick().first;
			std::scoped_lock l(g_lock);
			auto it = std::ranges::find_if(g_pledges, [uid](const Pledge& p) { return p.uid == uid && p.state == State::kOpen; });
			if (it == g_pledges.end()) {
				return;  // kept or lapsed meanwhile
			}
			if (c == Confirm::Choice::kAccept) {
				logger::info("pledge: the player broke it from the book");
				Break(*it, true);
				return;
			}
			it->state = State::kTakenBack;
			it->closedAt = Game::Hours();
			Game::Notify(std::format("Promise dissolved: {} ({})", WhatOf(*it), it->npcName));
			logger::info("pledge: dissolved from the book: {} ({})", it->npcName, WhatOf(*it));
		});
		if (!shown) {
			open = false;
			Game::Notify("Promises: the popup needs PrismaUI.");
		}
	}

	void OpenBook()
	{
		// Never while a save is being written: try again in a moment.
		if (Ledger::Saving()) {
			static std::atomic_int waits{ 0 };
			if (++waits <= 30) {
				std::thread([] {
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
					SKSE::GetTaskInterface()->AddTask(OpenBook);
				}).detach();
			} else {
				waits = 0;
			}
			return;
		}
		// Not on top of another menu (dialogue, inventory, the text box the thought was typed in).
		auto* ui = RE::UI::GetSingleton();
		if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::BookMenu::MENU_NAME) || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) ||
			ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) || ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) || ui->IsMenuOpen(RE::BarterMenu::MENU_NAME)) {
			return;
		}
		// A plain journal from the base game, for its look only (nothing is added to the pack).
		static RE::TESObjectBOOK* look = [] {
			RE::TESObjectBOOK* any = nullptr;
			for (auto* b : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESObjectBOOK>()) {
				auto* file = b ? b->GetFile(0) : nullptr;
				if (!file || std::string_view(file->GetFilename()) != "Skyrim.esm" || b->IsNoteScroll() || b->TeachesSpell() || b->TeachesSkill()) {
					continue;
				}
				any = any ? any : b;
				if (JsonUtil::Lower(b->GetName() ? b->GetName() : "").find("journal") != std::string::npos) {
					return b;
				}
			}
			return any;
		}();
		if (!look) {
			return;
		}
		const float now = Game::Hours();
		// The printed book face, not the handwriting (that one is hard to read at book size). Each promise:
		// who, what (capitalised), and when, in three steps of size; a little space between promises.
		// [pagebreak] only works as a paragraph of its own: Their promises on the left page, yours on the
		// right, what was kept or broken lately over the page.
		auto cap = [](std::string s) {
			if (!s.empty()) {
				s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
			}
			return s;
		};
		auto entry = [&](const std::string& who, const std::string& what, const std::string& when) {
			std::string e = std::format("<p align='left'><font size='22' color='#2B1D0E'>{}</font></p><p align='left'><font size='19' color='#3A2A18'>{}</font></p>",
				who, cap(what));
			if (!when.empty()) {
				e += std::format("<p align='left'><font size='16' color='#7A6247'>{}</font></p>", cap(when));
			}
			return e + "<p align='left'><font size='9'> </font></p>";
		};
		auto heading = [](const char* title) {
			return std::format("<p align='center'><font size='28' color='#2B1D0E'>{}</font></p><p align='center'><font size='16' color='#7A6247'>~ ~ ~</font></p>"
							   "<p align='left'><font size='9'> </font></p>",
				title);
		};
		auto empty = [](const char* line) { return std::format("<p align='center'><font size='18' color='#7A6247'>{}</font></p>", line); };
		const std::string pageBreak = "<p align='left'>[pagebreak]</p>";
		std::string open, closed;
		{
			std::scoped_lock l(g_lock);
			for (auto& p : g_pledges) {
				if (p.state == State::kOpen) {
					open += entry(p.npcName, WhatOf(p), DueText(p.due - now));
				} else if (now - p.closedAt < kShowKeptFor) {
					const char* mark = p.state == State::kKept ? "kept" : p.state == State::kBroken ? "not kept" : "taken back";
					closed += entry(p.npcName, WhatOf(p), mark);
				}
			}
		}
		// What they promised the player: things to come, places to stay, meetings.
		// Places to stay are hospitality, not promises: their own page, one entry per person (with a key, if
		// they gave one).
		std::string theirs, stays;
		std::vector<std::pair<std::string, std::pair<std::string, std::string>>> stayList;  // who -> what, note
		for (auto& o : Ledger::TheirPromises()) {
			const std::string when = o.hoursLeft < 0.0f ? "" : o.hoursLeft < 0.5f ? "now, when you're together" : DueText(o.hoursLeft);
			if (!o.stay) {
				theirs += entry(o.who, o.what, when);
				continue;
			}
			auto it = std::ranges::find(stayList, o.who, &decltype(stayList)::value_type::first);
			if (it == stayList.end()) {
				stayList.push_back({ o.who, { o.what, o.note.empty() ? when : o.note } });
			}
		}
		for (auto id : Keys::Holders()) {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			const std::string who = a && a->GetName() ? a->GetName() : "";
			if (who.empty()) {
				continue;
			}
			auto it = std::ranges::find(stayList, who, &decltype(stayList)::value_type::first);
			if (it == stayList.end()) {
				stayList.push_back({ who, { "a key to their home", "" } });
			} else {
				auto& note = it->second.second;
				note += note.empty() ? "you have a key" : "; you have a key";
			}
		}
		for (auto& [who, wn] : stayList) {
			stays += entry(who, wn.first, wn.second);
		}
		for (auto& m : Services::Meetings()) {
			theirs += entry(m.who, m.what, "");
		}
		std::string text = "<font face='$SkyrimBooks' size='19'>";
		text += heading("Their Promises");
		text += theirs.empty() ? empty("Nobody has promised you anything, for now.") : theirs;
		text += pageBreak + heading("Your Promises");
		text += open.empty() ? empty("Nothing promised to anyone, for now.") : open;
		if (!open.empty() && g_bookKey.load()) {
			text += "<p align='center'><font size='14' color='#7A6247'>(press the promises key again to manage these)</font></p>";
		}
		if (!stays.empty()) {
			text += pageBreak + heading("Where You're Welcome") + stays;
		}
		if (!closed.empty()) {
			text += pageBreak + heading("Lately") + closed;
		}
		text += "</font>";
		// The game's own "open this text as a book" (what reading from the inventory does); the
		// same call CommonLib's OpenMenuFromBaseForm makes, with our text instead of the book's.
		using OpenBook_t = void(const RE::BSString&, const RE::ExtraDataList*, RE::TESObjectREFR*, RE::TESObjectBOOK*, const RE::NiPoint3&,
			const RE::NiMatrix3&, float, bool);
		static REL::Relocation<OpenBook_t*> openBook{ RELOCATION_ID(50122, 51053) };
		RE::NiMatrix3 rot{};
		rot.SetEulerAnglesXYZ(-0.05f, -0.05f, 1.50f);
		openBook(RE::BSString(text.c_str()), nullptr, nullptr, look, RE::NiPoint3{}, rot, 1.0f, true);
		g_ourBook = true;
		logger::info("pledge: opened the promises book");
	}

	std::vector<std::string> BioLines(RE::FormID npc)
	{
		std::vector<std::string> out;
		const float now = Game::Hours();
		const auto player = PlayerName();
		std::scoped_lock l(g_lock);
		for (auto& a : g_asks) {
			if (a.npc == npc && SteadyClock::now() < a.until) {
				out.push_back(std::format("Just asked {} a favour: {}. Waiting to hear if they will.", player,
					a.type == "bring" ? "to bring " + a.item : a.type == "deliver" ? "to take " + a.item + " to " + a.recipient : "to look in on " + a.recipient));
			}
		}
		for (auto& p : g_pledges) {
			if (p.npc != npc) {
				continue;
			}
			if (p.state == State::kOpen) {
				out.push_back(now <= p.due ? std::format("{} promised to {} ({}).", player, WhatOf(p), DueText(p.due - now))
				                           : std::format("{} promised to {} and it is overdue; disappointing, though they may still make good.", player, WhatOf(p)));
			} else if (p.state == State::kBroken && now - p.closedAt < kShowKeptFor) {
				out.push_back(std::format("{} broke a promise to {}: they never did. It still stings.", player, WhatOf(p)));
			} else if (p.state == State::kKept && now - p.closedAt < kShowKeptFor) {
				out.push_back(std::format("{} kept their promise to {}.", player, WhatOf(p)));
			}
		}
		return out;
	}

	void Tick()
	{
		g_bookKey = ScanCodeOf(static_cast<int>(SkyrimNet::ConfigFloat("tuning.promises_key", 80.0f)));
		{
			std::scoped_lock l(g_lock);
			std::erase_if(g_asks, [](const Ask& a) { return SteadyClock::now() >= a.until; });  // unanswered favours are forgotten
		}
		if (g_talk.npc && !g_talk.handled) {
			auto* who = RE::TESForm::LookupByID<RE::Actor>(g_talk.npc);
			const auto since = SteadyClock::now() - g_talk.at;
			if (!who || since > std::chrono::minutes(3)) {
				g_talk.npc = 0;
			} else if (const float d = Game::DistanceToPlayer(who); d > 700.0f && d < 2500.0f) {
				g_talk.handled = true;
				TryAsk(g_talk.npc, true);
			}
		}
		const float now = Game::Hours();
		std::vector<std::pair<RE::Actor*, std::string>> mention, visited, checked;
		{
			std::scoped_lock l(g_lock);
			for (auto& p : g_pledges) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(p.npc);
				if (p.state != State::kOpen) {
					// A broken promise is brought up the next time they see the player (once).
					if (p.state == State::kBroken && !p.mentioned && npc && !npc->IsDead() && Game::DistanceToPlayer(npc) < kMentionRange && !npc->IsInCombat() &&
						!Game::Player()->IsInCombat()) {
						p.mentioned = true;
						mention.emplace_back(npc, WhatOf(p));
					}
					continue;
				}
				if (!npc || npc->IsDead()) {
					p.state = State::kTakenBack;  // nobody left to keep it to
					p.closedAt = now;
					continue;
				}
				// Looking in on someone: being with them keeps it (and they hear who sent the player).
				if (p.type == "check" && p.recipientBase) {
					if (auto* lists = RE::ProcessLists::GetSingleton()) {
						for (auto& h : lists->highActorHandles) {
							auto a = h.get();
							if (a && !a->IsDead() && a->GetActorBase() && a->GetActorBase()->GetFormID() == p.recipientBase && Game::DistanceToPlayer(a.get()) < 800.0f) {
								Kept(p, npc);
								checked.emplace_back(a.get(), p.npcName);
								break;
							}
						}
					}
					if (p.state != State::kOpen) {
						continue;
					}
				}
				// A visit is coming BACK: only after the player has really been away (left the building,
				// or far off), being with them again keeps it. Standing beside them all along isn't a visit.
				if (p.type == "visit") {
					const float d = Game::DistanceToPlayer(npc);  // "infinitely far" in another cell
					if (d > kAwayDistance) {
						if (p.awayFrom < 0.0f) {
							p.awayFrom = now;
						}
						// Gone a while (an hour of game time, waiting and travel included): really away.
						if (now - p.awayFrom >= kAwayHours) {
							p.away = true;
						}
					} else if (!p.away) {
						p.awayFrom = -1.0f;  // back before it counted (met at the door): not a trip yet
					} else if (d < 800.0f && now <= p.due + kVisitWindow) {
						Kept(p, npc);
						visited.emplace_back(npc, WhatOf(p));
						continue;
					}
				}
				const float grace = p.type == "visit" ? kVisitWindow : 0.0f;
				// Never apart since promising to come back: nothing to keep or break; it just lapses.
				if (p.type == "visit" && !p.away && now > p.due + grace) {
					p.state = State::kTakenBack;
					p.closedAt = now;
					logger::info("pledge: {} - together all along, the visit lapses quietly", p.npcName);
					continue;
				}
				if (now > p.due + grace) {
					// Vague promises ("I'll make it up to you") can't be checked: they lapse without a penalty.
					Break(p, p.type != "other");
				}
			}
			// Old closed ones drop off after a while.
			std::erase_if(g_pledges, [&](const Pledge& p) { return p.state != State::kOpen && now - p.closedAt > kShowKeptFor * 2; });
		}
		for (auto& [who, sender] : checked) {
			Game::Narrate(std::format("{} came by to look in on {}, because {} had asked them to.", PlayerName(), who->GetName(), sender), who);
		}
		for (auto& [npc, what] : visited) {
			Game::Narrate(std::format("{} had come to see {}, just as they had promised.", PlayerName(), npc->GetName()), npc);
		}
		for (auto& [npc, what] : mention) {
			Game::Narrate(std::format("{} remembered that {} had promised to {} and never did, and brought it up, disappointed.", npc->GetName(), PlayerName(), what), npc);
		}
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json out = json::array();
		for (auto& p : g_pledges) {
			out.push_back({ { "npc", p.npc }, { "npcName", p.npcName }, { "type", p.type }, { "item", p.item }, { "gold", p.gold }, { "words", p.words }, { "what", p.what },
				{ "itemBase", p.itemBase }, { "recipientBase", p.recipientBase }, { "recipient", p.recipient },
				{ "when", p.when }, { "made", p.made }, { "due", p.due }, { "state", static_cast<int>(p.state) }, { "closedAt", p.closedAt }, { "mentioned", p.mentioned }, { "away", p.away }, { "awayFrom", p.awayFrom } });
		}
		return out;
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		std::scoped_lock l(g_lock);
		g_pledges.clear();
		if (!j.is_array()) {
			return;
		}
		for (auto& o : j) {
			const auto raw = static_cast<RE::FormID>(JsonUtil::Num(o, "npc").value_or(0));
			const auto id = raw ? resolve(raw) : 0;
			if (!id) {
				continue;
			}
			Pledge p;
			p.npc = id;
			p.npcName = JsonUtil::Str(o, "npcName");
			p.type = JsonUtil::Str(o, "type", "other");
			p.item = JsonUtil::Str(o, "item");
			p.gold = static_cast<std::int32_t>(JsonUtil::Num(o, "gold").value_or(0));
			p.words = JsonUtil::Str(o, "words");
			p.what = JsonUtil::Str(o, "what");
			if (const auto ib = static_cast<RE::FormID>(JsonUtil::Num(o, "itemBase").value_or(0))) {
				p.itemBase = resolve(ib);
			}
			if (const auto rb = static_cast<RE::FormID>(JsonUtil::Num(o, "recipientBase").value_or(0))) {
				p.recipientBase = resolve(rb);
			}
			p.recipient = JsonUtil::Str(o, "recipient");
			p.when = JsonUtil::Str(o, "when");
			p.made = static_cast<float>(JsonUtil::Num(o, "made").value_or(0));
			p.due = static_cast<float>(JsonUtil::Num(o, "due").value_or(0));
			// Saved before standing offers had no deadline ("you can work for me"): none now.
			if (p.due < kOpenEnded / 2 && OpenEnded(json::object(), JsonUtil::Str(o, "words"))) {
				p.due = p.made + kOpenEnded;
			}
			// Saved as a vague promise that was really a visit ("I'll be sure to visit you").
			if (p.type == "other" && SaysVisit(p.words)) {
				p.type = "visit";
			}
			p.state = static_cast<State>(std::clamp(static_cast<int>(JsonUtil::Num(o, "state").value_or(0)), 0, 3));
			p.closedAt = static_cast<float>(JsonUtil::Num(o, "closedAt").value_or(-1));
			p.mentioned = JsonUtil::Str(o, "mentioned", "false") == "true";
			p.away = JsonUtil::Str(o, "away", "false") == "true";
			p.awayFrom = static_cast<float>(JsonUtil::Num(o, "awayFrom").value_or(-1.0));
			g_pledges.push_back(std::move(p));
		}
		logger::info("co-save: {} player promises", g_pledges.size());
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_pledges.clear();
		g_asks.clear();
		g_lastAsk.clear();
		g_lastAnyAsk = -100.0f;
	}

	namespace
	{
		// The hotkey from the settings (a DirectX scan code; 0 = unbound). Read from the settings every
		// couple of seconds by the ledger tick, never per input event (mouse moves are input events too).
		struct KeySink : RE::BSTEventSink<RE::InputEvent*>
		{
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				const auto key = g_bookKey.load();
				if (!events || key == 0) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (auto* e = *events; e; e = e->next) {
					auto* b = e->AsButtonEvent();
					if (b && b->GetDevice() == RE::INPUT_DEVICE::kKeyboard && b->GetIDCode() == key && b->IsDown()) {
						auto* ui = RE::UI::GetSingleton();
						if (ui && g_ourBook && ui->IsMenuOpen(RE::BookMenu::MENU_NAME)) {
							// Again while the promises book is open: it closes, the Break / Dissolve popup opens.
							g_ourBook = false;
							if (auto* q = RE::UIMessageQueue::GetSingleton()) {
								q->AddMessage(RE::BookMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
							}
							SKSE::GetTaskInterface()->AddTask(ManageBook);
						} else if (ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::Console::MENU_NAME)) {
							SKSE::GetTaskInterface()->AddTask(OpenBook);
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Any other book opened later isn't ours.
		struct MenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (e && !e->opening && e->menuName == RE::BookMenu::MENU_NAME) {
					g_ourBook = false;
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// "What did I promise Ronja?" thought to oneself (SkyrimNet thoughts).
		void OnThought(const std::string& raw)
		{
			auto j = json::parse(raw, nullptr, false);
			if (j.is_discarded()) {
				return;
			}
			json data = j.contains("data") && j["data"].is_string() ? json::parse(j["data"].get<std::string>(), nullptr, false) : j.value("data", json());
			std::string text;
			for (auto* key : { "dialogue", "thought", "text", "content", "message" }) {
				if (data.is_object() && text.empty()) {
					text = JsonUtil::Str(data, key);
				}
				if (text.empty()) {
					text = JsonUtil::Str(j, key);
				}
			}
			logger::info("pledge: thought \"{}\"", text.substr(0, 160));
			if (!text.empty() && SkyrimNet::ConfigBool("features.player_promises", true) && AsksAboutPromises(text, false)) {
				logger::info("pledge: \"{}\" asks about promises; opening the book", text);
				// A moment later: the box the thought was typed in closes first.
				std::thread([] {
					std::this_thread::sleep_for(std::chrono::milliseconds(1200));
					SKSE::GetTaskInterface()->AddTask(OpenBook);
				}).detach();
			}
		}
	}

	void Install()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			static MenuSink menus;
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&menus);
		}
		static KeySink sink;
		if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink<RE::InputEvent*>(&sink);
		}
		// Typed or spoken thoughts arrive as SkyrimNet's "player_thoughts" (rewritten in the
		// character's voice); "dialogue_player_monologue" is kept for other setups.
		for (auto* type : { "player_thoughts", "dialogue_player_monologue" }) {
			SkyrimNet::OnEvent(type, [](const std::string& raw) {
				try {
					OnThought(raw);
				} catch (...) {
				}
			});
		}
	}
}
