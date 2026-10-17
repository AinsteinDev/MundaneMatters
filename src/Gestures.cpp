#include "Gestures.h"

#include "Game.h"
#include "JsonUtil.h"
#include "Moments.h"
#include "Papyrus.h"
#include "Services.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Gestures
{
	namespace
	{
		struct Rule
		{
			std::vector<const char*> words;
			Act act;
		};

		const std::vector<Rule>& Rules()
		{
			using K = Act::Kind;
			static const std::vector<Rule> rules{
				{ { "draws her", "draws his", "draws their", "draws a ", "draws the", "unsheathes", "pulls out", "pulls a ", "brandishes", "bares her",
					  "bares his", "drew her", "drew his" },
					{ K::kDraw, "" } },
				{ { "sheathes", "sheaths", "puts away her", "puts away his", "puts the blade away", "puts the dagger away", "slides it back",
					  "lowers her blade", "lowers his blade", "holsters" },
					{ K::kSheathe, "" } },
				{ { "stands up", "rises from", "rises to her feet", "rises to his feet", "gets up", "gets to her feet", "gets to his feet",
					  "pushes back her chair", "pushes back his chair", "stands from" },
					{ K::kStand, "" } },
				{ { "walks over to you", "walks up to you", "comes over to you", "steps closer to you", "steps closer", "moves closer to you",
					  "approaches you", "crosses the room to you", "comes closer to you" },
					{ K::kApproach, "" } },
				{ { "~laugh", "~laughs", "~laughing", "~chuckle", "~chuckles", "~chuckling", "~giggle", "~giggles" }, { K::kIdle, "IdleLaugh" } },
				{ { "claps", "applauds", "slow clap" }, { K::kIdle, "IdleApplaud2" } },
				{ { "salutes", "a salute" }, { K::kIdle, "IdleSalute" } },
				{ { "~wave", "~waves", "~waving" }, { K::kIdle, "IdleWave" } },
				{ { "bows", "a bow", "a slight bow" }, { K::kIdle, "IdleSilentBow" } },
				{ { "points at", "points to", "points toward", "points towards", "gestures toward", "gestures towards", "gestures back toward",
					  "gestures back towards", "gestures at", "gestures to the", "nods toward", "nods towards" },
					{ K::kIdle, "IdlePointClose" } },
				{ { "beckons", "motions for you to follow", "gestures for you to follow", "waves you over", "this way" }, { K::kIdle, "IdleComeThisWay" } },
				{ { "shakes her head", "shakes his head", "shakes their head", "shaking her head", "shaking his head" }, { K::kIdle, "IdleHeadShake" } },
				{ { "fidgets", "wrings her hands", "wrings his hands", "nervously" }, { K::kIdle, "IdleNervous" } },
				{ { "wipes her brow", "wipes his brow", "wipes sweat" }, { K::kIdle, "IdleWipeBrow" } },
				{ { "scratches her head", "scratches his head" }, { K::kIdle, "IdleScratchHead" } },
				{ { "takes a sip", "takes a swig", "sips her", "sips his", "drinks from", "takes a long drink" }, { K::kIdle, "IdleDrink" } },
			};
			return rules;
		}

		// The *starred* parts of a line.
		std::vector<std::string> Starred(const std::string& line)
		{
			std::vector<std::string> out;
			for (std::size_t at = line.find('*'); at != std::string::npos;) {
				const auto end = line.find('*', at + 1);
				if (end == std::string::npos) {
					break;
				}
				if (end > at + 1) {
					out.push_back(line.substr(at + 1, end - at - 1));
				}
				at = line.find('*', end + 1);
			}
			return out;
		}

		// A clause about the speaker: it starts with their name, a pronoun, an article or the verb itself -
		// not with someone else's name ("Svana laughs" in Haelga's line).
		bool AboutSpeaker(const std::string& clause, const std::string& speakerName)
		{
			std::size_t i = 0;
			while (i < clause.size() && !std::isalpha(static_cast<unsigned char>(clause[i]))) {
				++i;
			}
			if (i >= clause.size()) {
				return false;
			}
			std::size_t j = i;
			while (j < clause.size() && (std::isalpha(static_cast<unsigned char>(clause[j])) || clause[j] == '\'')) {
				++j;
			}
			const std::string first = clause.substr(i, j - i);
			if (!std::isupper(static_cast<unsigned char>(first[0]))) {
				return true;  // "draws her dagger"
			}
			const auto lower = JsonUtil::Lower(first);
			for (auto* ok : { "she", "he", "they", "her", "his", "their", "i", "my", "a", "an", "the", "with", "then", "slowly", "quickly", "finally" }) {
				if (lower == ok) {
					return true;
				}
			}
			const auto speaker = JsonUtil::Lower(speakerName);
			const auto firstName = speaker.substr(0, speaker.find(' '));
			return lower == firstName || lower == firstName + "'s";
		}
	}

	std::vector<Act> Read(const std::string& line, const std::string& speakerName)
	{
		std::vector<Act> out;
		for (auto& part : Starred(line)) {
			// Clause by clause: "*She laughs, then Svana draws her knife*" - only her own.
			std::size_t start = 0;
			while (start < part.size()) {
				auto end = part.find_first_of(".;!?", start);
				if (end == std::string::npos) {
					end = part.size();
				}
				const std::string clause = part.substr(start, end - start);
				start = end + 1;
				if (!AboutSpeaker(clause, speakerName)) {
					continue;
				}
				const auto l = " " + JsonUtil::Lower(clause) + " ";
				const auto words = JsonUtil::Words(clause);
				// Drawing / sheathing is about a weapon ("draws a breath", "draws her cloak tighter" are not).
				const bool weapon = std::ranges::any_of(std::initializer_list<const char*>{ "dagger", "sword", "blade", "knife", "axe", "mace", "weapon",
										  "bow ", "steel", "hammer", "hilt", "dirk", "sabre", "saber", "katana", "scimitar" },
					[&](const char* w) { return l.find(w) != std::string::npos; });
				for (auto& r : Rules()) {
					if ((r.act.kind == Act::Kind::kDraw || r.act.kind == Act::Kind::kSheathe) && !weapon) {
						continue;
					}
					// "~word": that whole word anywhere ("a small, tired wave", "a low chuckle").
					const bool hit = std::ranges::any_of(r.words, [&](const char* w) {
						if (w[0] == '~') {
							return std::ranges::find(words, std::string(w + 1)) != words.end();
						}
						return l.find(w) != std::string::npos;
					});
					const bool dup = std::ranges::any_of(out, [&](const Act& a) { return a.kind == r.act.kind && a.idle == r.act.idle; });
					if (hit && !dup) {
						out.push_back(r.act);
					}
				}
			}
		}
		return out;
	}

	namespace
	{
		void StandUp(RE::Actor* npc)
		{
			if (!npc->GetOccupiedFurniture().get()) {
				return;
			}
			Moments::Release(npc->GetFormID());
			Papyrus::CallStatic("SeverActions_FurnitureLib", "Stop", nullptr, static_cast<RE::Actor*>(npc));
			Papyrus::CallMethod(npc, "Actor", "EvaluatePackage", nullptr);
		}

		void Do(RE::FormID npcId, Act act)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* player = Game::Player();
			// Never in a fight, and never in a scripted scene (a quest moment is the game's to play out).
			if (!npc || npc->IsDead() || npc->IsInCombat() || player->IsInCombat() || Game::DistanceToPlayer(npc) > 2000.0f || npc->GetCurrentScene()) {
				return;
			}
			using K = Act::Kind;
			switch (act.kind) {
			case K::kDraw:
				if (!npc->AsActorState()->IsWeaponDrawn()) {
					Papyrus::CallMethod(npc, "Actor", "DrawWeapon", nullptr);
					logger::info("gesture: {} draws their weapon", npc->GetName());
					// Nothing came of it: put away again after a while.
					std::thread([npcId] {
						std::this_thread::sleep_for(std::chrono::seconds(60));
						SKSE::GetTaskInterface()->AddTask([npcId] {
							if (auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId); a && !a->IsInCombat() && a->AsActorState()->IsWeaponDrawn()) {
								Papyrus::CallMethod(a, "Actor", "SheatheWeapon", nullptr);
							}
						});
					}).detach();
				}
				break;
			case K::kSheathe:
				if (npc->AsActorState()->IsWeaponDrawn()) {
					Papyrus::CallMethod(npc, "Actor", "SheatheWeapon", nullptr);
					logger::info("gesture: {} puts their weapon away", npc->GetName());
				}
				break;
			case K::kStand:
				if (npc->GetOccupiedFurniture().get()) {
					StandUp(npc);
					logger::info("gesture: {} stands up", npc->GetName());
				}
				break;
			case K::kApproach:
				if (Game::DistanceToPlayer(npc) > 200.0f) {
					StandUp(npc);
					Papyrus::CallMethod(npc, "Actor", "PathToReference", nullptr, static_cast<RE::TESObjectREFR*>(player), 0.5f);
					logger::info("gesture: {} comes over to the player", npc->GetName());
				}
				break;
			case K::kIdle:
				// Not while seated (it would break the pose), on the move, or walking with the player (only
				// stopped to talk). Back to normal a few seconds after, so they never stay stuck in it.
				if (!npc->GetOccupiedFurniture().get() && !npc->IsMoving() && !npc->AsActorState()->IsWeaponDrawn() && !Services::FollowsPlayer(npc)) {
					Papyrus::CallStatic("Debug", "SendAnimationEvent", nullptr, static_cast<RE::TESObjectREFR*>(npc), RE::BSFixedString(act.idle));
					logger::info("gesture: {} - {}", npc->GetName(), act.idle);
					std::thread([npcId] {
						std::this_thread::sleep_for(std::chrono::seconds(4));
						SKSE::GetTaskInterface()->AddTask([npcId] {
							auto* a = RE::TESForm::LookupByID<RE::Actor>(npcId);
							if (a && !a->IsDead() && !a->GetOccupiedFurniture().get() && !a->IsInCombat()) {
								Papyrus::CallStatic("Debug", "SendAnimationEvent", nullptr, static_cast<RE::TESObjectREFR*>(a), RE::BSFixedString("IdleForceDefaultState"));
								Papyrus::CallMethod(a, "Actor", "EvaluatePackage", nullptr);
							}
						});
					}).detach();
				}
				break;
			default:
				break;
			}
		}
	}

	void OnNpcLine(RE::FormID npcId, const std::string& line)
	{
		if (line.find('*') == std::string::npos || !SkyrimNet::ConfigBool("features.gestures", true)) {
			return;
		}
		SKSE::GetTaskInterface()->AddTask([npcId, line] {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc) {
				return;
			}
			auto acts = Read(line, npc->GetName());
			// One idle at a time: the first described.
			bool idled = false;
			for (auto& a : acts) {
				if (a.kind == Act::Kind::kIdle) {
					if (idled) {
						continue;
					}
					idled = true;
				}
				Do(npcId, a);
			}
		});
	}
}
