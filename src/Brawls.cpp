#include "Brawls.h"
#include "Services.h"

#include "Bonds.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Papyrus.h"
#include "Promises.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Brawls
{
	namespace
	{
		constexpr float kFresh = 2.0f;        // game hours it's "just now"
		constexpr float kRemembered = 72.0f;  // and how long it's still on their mind

		enum class Outcome
		{
			kKnockout,  // knocked down
			kConceded,  // gave up, or lowered their fists
			kNone       // fizzled, broke off, turned into a real fight: nobody won
		};

		struct Result
		{
			bool npcWon = false;
			bool spar = false;  // a friendly bout between people close to each other
			Outcome how = Outcome::kKnockout;
			float at = 0.0f;
			std::string npcName;
		};

		std::mutex g_lock;
		std::unordered_map<RE::FormID, Result> g_results;
		std::unordered_map<RE::FormID, float> g_threatened;  // game hour the player last threatened them
		constexpr float kShaken = 2.0f;                       // game hours they're still shaken
		constexpr float kWary = 6.0f;                         // and wary until

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

		// SeverActions' end reasons (SeverActions_Brawl.psc OnBrawlEnded): 1 knocked down, 2 gave up,
		// 6 lowered their fists; 3 walked away, 4 turned into a real fight, 7 a weapon came out and
		// anything else (an abort) name no winner.
		Outcome OutcomeOf(int reason)
		{
			switch (reason) {
			case 1:
				return Outcome::kKnockout;
			case 2:
			case 6:
				return Outcome::kConceded;
			default:
				return Outcome::kNone;
			}
		}

		// Game thread: SeverActions said who won.
		void OnResult(RE::FormID winnerId, RE::FormID loserId, int reason, bool wasTeammate)
		{
			if (!SkyrimNet::ConfigBool("features.brawls", true)) {
				return;
			}
			const auto how = OutcomeOf(reason);
			if (how == Outcome::kNone) {
				return;  // no winner to know about
			}
			const bool playerWon = winnerId == 0x14;
			if (!playerWon && loserId != 0x14) {
				return;  // NPCs brawling among themselves
			}
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(playerWon ? loserId : winnerId);
			if (!npc || npc->IsDead()) {
				return;
			}
			Result r;
			r.npcWon = !playerWon;
			// SeverActions takes a follower out of the team for the bout and puts them back after it:
			// its own note of that (or the restored flag) says they're a companion.
			r.spar = wasTeammate || Services::IsCompanion(npc) || Promises::IsClose(npc);
			r.how = how;
			r.at = Game::Hours();
			r.npcName = npc->GetName();
			{
				std::scoped_lock l(g_lock);
				g_results[npc->GetFormID()] = r;
			}
			const auto player = PlayerName();
			const char* way = how == Outcome::kConceded ? "gave up" : "was knocked to their knees";
			std::string memory;
			if (r.npcWon) {
				memory = std::format("{} beat {} in a brawl{}.", r.npcName, player, r.spar ? ", a friendly bout between friends" : "");
			} else {
				memory = r.spar ? std::format("{} lost a friendly brawl to {}: {} fair and square. A good fight.", r.npcName, player, way)
				                : std::format("{} lost a brawl to {}: {}, beaten and shown their place.", r.npcName, player, way);
			}
			SkyrimNet::AddMemory(npc->GetFormID(), memory, 0.8f);
			// A fair fight between friends brings them closer, whoever won.
			if (r.spar) {
				Bonds::Add(npc, Bonds::Deed::kSpar, 1);
			}
			logger::info("brawl: {} {} ({}, {})", r.npcName, r.npcWon ? "won" : "lost", r.spar ? "friendly spar" : "real fight",
				how == Outcome::kConceded ? "conceded" : "knockout");
		}

		RE::FormID IdOf(const RE::BSScript::Variable& v)
		{
			auto* a = v.IsObject() ? v.Unpack<RE::Actor*>() : nullptr;
			return a ? a->GetFormID() : 0;
		}

		// SeverActions' own event when a brawl ends (numArg = the reason). The winner and loser come
		// from its natives, exactly as its own script reads them.
		void OnEnded(int reason)
		{
			if (OutcomeOf(reason) == Outcome::kNone) {
				logger::info("brawl: ended with no winner (reason {})", reason);
				return;
			}
			Papyrus::CallStatic("SeverActionsNativeExt", "Brawl_GetLastWinner", [reason](const RE::BSScript::Variable& w) {
				const auto winner = IdOf(w);
				Papyrus::CallStatic("SeverActionsNativeExt", "Brawl_GetLastLoser", [reason, winner](const RE::BSScript::Variable& l) {
					const auto loser = IdOf(l);
					logger::info("brawl: ended, reason {}, winner {:08X}, loser {:08X}", reason, winner, loser);
					if (!winner || !loser || (winner != 0x14 && loser != 0x14)) {
						return;
					}
					auto* npc = RE::TESForm::LookupByID<RE::TESForm>(winner == 0x14 ? loser : winner);
					const bool asked = Papyrus::CallStatic("StorageUtil", "GetIntValue", [reason, winner, loser](const RE::BSScript::Variable& t) {
						const bool wasTeammate = t.IsInt() && t.GetSInt() == 1;
						SKSE::GetTaskInterface()->AddTask([=] { OnResult(winner, loser, reason, wasTeammate); });
					}, npc, RE::BSFixedString("SeverBrawl_WasTeammate"), 0);
					if (!asked) {  // no PapyrusUtil: the teammate flag (restored by then) and Bonds decide
						SKSE::GetTaskInterface()->AddTask([=] { OnResult(winner, loser, reason, false); });
					}
				});
			});
		}

		class EndSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* e, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (e && e->eventName == "SeverBrawl_Ended") {
					const int reason = static_cast<int>(e->numArg);
					SKSE::GetTaskInterface()->AddTask([reason] { OnEnded(reason); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		EndSink g_sink;
	}

	void Threatened(RE::FormID npc)
	{
		auto* a = RE::TESForm::LookupByID<RE::Actor>(npc);
		if (!a || a->IsDead() || a->IsChild() || !SkyrimNet::ConfigBool("features.brawls", true)) {
			return;
		}
		{
			std::scoped_lock l(g_lock);
			g_threatened[npc] = Game::Hours();
		}
		logger::info("threat: {} was threatened by the player; shaken for {:.0f} game hours", a->GetName(), kShaken);
	}

	namespace
	{
		// Threatened: shaken (or, if they stood up to it, tense and resentful), then wary.
		std::vector<std::string> ThreatLines(RE::FormID npc)
		{
			float at = -1.0f;
			{
				std::scoped_lock l(g_lock);
				if (auto it = g_threatened.find(npc); it != g_threatened.end()) {
					at = it->second;
				}
			}
			const float ago = Game::Hours() - at;
			if (at < 0.0f || ago > kWary) {
				return {};
			}
			const auto player = PlayerName();
			if (ago < kShaken) {
				return { std::format("{} threatened them {} - it still sits with them and colours how they answer anything {} says now: careful with "
				                     "every word, no backtalk or jokes at {}'s expense, quick to agree, keeping a wary distance (if they stood up to the "
				                     "threat instead, they're tense and resentful rather than scared). They never act as if it didn't happen.",
					player, ago < 0.25f ? "just now" : "a little while ago", player, player) };
			}
			return { std::format("{} threatened them earlier today: still guarded and wary around {}, not friendly yet.", player, player) };
		}
	}

	namespace
	{
		std::string BrawlLine(const Result& r, float ago)
		{
			const auto player = PlayerName();
			const bool fresh = ago < kFresh;
			const std::string when = fresh ? "just now" : ago < 24.0f ? "earlier today" : "a few days ago";
			const char* how = r.how == Outcome::kConceded ? "and gave up" : "and was knocked to their knees";
			if (!r.npcWon && r.spar) {
				return std::format("LOST a friendly brawl to {} {} {}: fairly beaten, and they know it. Takes it in good spirit - 'good fight', a bit sore, "
				                     "maybe a rematch some day - and never claims to have won.",
					player, when, how);
			}
			if (!r.npcWon) {
				return std::format("LOST a brawl to {} {} {}: beaten and shown their place. Pride bruised; warier and more respectful around {} now "
				                     "(sullen or grudging, as is their nature), the swagger gone. Never claims to have won or pretends it didn't happen.",
					player, when, how, player);
			}
			if (r.spar) {
				return std::format("Beat {} in a friendly brawl {}: gracious about it, maybe teasing them a little, happy to go again.", player, when);
			}
			return std::format("Beat {} in a brawl {}: pleased with themself, and lets it show.", player, when);
		}
	}

	std::vector<std::string> BioLines(RE::FormID npc)
	{
		auto threat = ThreatLines(npc);
		Result r;
		{
			std::scoped_lock l(g_lock);
			auto it = g_results.find(npc);
			if (it == g_results.end()) {
				return threat;
			}
			r = it->second;
		}
		const float ago = Game::Hours() - r.at;
		if (ago > kRemembered) {
			return threat;
		}
		auto brawl = BrawlLine(r, ago);
		threat.insert(threat.begin(), std::move(brawl));
		return threat;
	}

	void Install()
	{
		if (auto* source = SKSE::GetModCallbackEventSource()) {
			source->AddEventSink(&g_sink);
		}
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json out = json::array();
		for (auto& [id, r] : g_results) {
			out.push_back({ { "npc", id }, { "npcWon", r.npcWon }, { "spar", r.spar }, { "how", static_cast<int>(r.how) }, { "at", r.at }, { "name", r.npcName } });
		}
		for (auto& [id, at] : g_threatened) {
			out.push_back({ { "npc", id }, { "threat", true }, { "at", at } });
		}
		return out;
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		std::scoped_lock l(g_lock);
		g_results.clear();
		g_threatened.clear();
		if (!j.is_array()) {
			return;
		}
		for (auto& o : j) {
			const auto raw = static_cast<RE::FormID>(JsonUtil::Num(o, "npc").value_or(0));
			const auto id = raw ? resolve(raw) : 0;
			if (id && o.is_object() && o.contains("threat")) {
				g_threatened[id] = static_cast<float>(JsonUtil::Num(o, "at").value_or(0));
				continue;
			}
			if (id) {
				Result r;
				r.npcWon = JsonUtil::Str(o, "npcWon", "false") == "true";
				r.spar = JsonUtil::Str(o, "spar", "false") == "true";
				r.how = static_cast<Outcome>(std::clamp(static_cast<int>(JsonUtil::Num(o, "how").value_or(0)), 0, 2));
				r.at = static_cast<float>(JsonUtil::Num(o, "at").value_or(0));
				r.npcName = JsonUtil::Str(o, "name");
				g_results[id] = std::move(r);
			}
		}
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_results.clear();
		g_threatened.clear();
	}
}
