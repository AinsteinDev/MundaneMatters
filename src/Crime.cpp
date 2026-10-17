#include "Crime.h"

#include "Confirm.h"
#include "Game.h"
#include "Papyrus.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Crime
{
	namespace
	{
		constexpr std::int32_t kBribeMax = 1000;   // bigger than this, nobody looks the other way
		constexpr std::int32_t kTalkMax = 500;     // persuade / intimidate: only for small matters
		constexpr float kRetryAfter = 24.0f;       // game hours before trying the same trick on that hold again

		std::unordered_map<RE::FormID, float> g_failed;  // crime faction -> game hours of a failed persuade/threat

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

		float Speech()
		{
			return std::clamp(Game::Player()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech), 0.0f, 100.0f);
		}

		struct Bounty
		{
			RE::TESFaction* faction = nullptr;
			std::int32_t calm = 0;     // theft, trespass, pickpocketing
			std::int32_t violent = 0;  // assault, murder
		};

		Bounty BountyFor(RE::Actor* guard)
		{
			Bounty b;
			if (!guard || !guard->IsGuard()) {
				return b;
			}
			b.faction = guard->GetCrimeFaction();
			if (b.faction) {
				auto* player = Game::Player();
				b.calm = static_cast<std::int32_t>(player->GetNonViolentCrimeGoldValue(b.faction));
				b.violent = static_cast<std::int32_t>(player->GetViolentCrimeGoldValue(b.faction));
			}
			return b;
		}

		// What a bribe takes: about the fine at middling Speech, less for a smooth talker, more for a clumsy one.
		std::int32_t BribeCost(std::int32_t bounty)
		{
			return (std::max)(10, static_cast<std::int32_t>(std::lround(bounty * (1.25f - 0.5f * Speech() / 100.0f))));
		}

		bool PersuadeWorks(std::int32_t bounty)
		{
			return Speech() >= 30.0f + bounty / 10.0f;
		}

		bool IntimidateWorks(std::int32_t bounty, RE::Actor* guard)
		{
			const float score = Speech() + 2.0f * (static_cast<float>(Game::Player()->GetLevel()) - static_cast<float>(guard->GetLevel()));
			return score >= 40.0f + bounty / 10.0f;
		}

		bool RecentlyFailed(RE::TESFaction* f)
		{
			auto it = f ? g_failed.find(f->GetFormID()) : g_failed.end();
			return it != g_failed.end() && Game::Hours() - it->second < kRetryAfter;
		}

		// The matter is settled: the guard and the alarm stand down.
		void StandDown(RE::Actor* guard)
		{
			Papyrus::CallMethod(guard, "Actor", "StopCombat", nullptr);
			Papyrus::CallMethod(Game::Player(), "Actor", "StopCombatAlarm", nullptr);
		}

		void Cleared(RE::Actor* guard, const Bounty& b, const char* how)
		{
			Papyrus::CallMethod(b.faction, "Faction", "SetCrimeGold", nullptr, static_cast<std::int32_t>(0));
			StandDown(guard);
			Game::Player()->AddSkillExperience(RE::ActorValue::kSpeech, static_cast<float>(b.calm) * 0.5f);
			Game::Notify(std::format("Bounty cleared ({})", how));
			logger::info("crime: {} cleared {} gold of bounty with {} ({})", PlayerName(), b.calm, guard->GetName(), how);
		}
	}

	bool HasBounty(RE::Actor* guard)
	{
		const auto b = BountyFor(guard);
		return b.faction && (b.calm > 0 || b.violent > 0);
	}

	std::string GuardFacts(RE::Actor* guard)
	{
		const auto b = BountyFor(guard);
		if (!b.faction || (b.calm <= 0 && b.violent <= 0)) {
			return "";
		}
		const auto player = PlayerName();
		const auto total = b.calm + b.violent;
		std::string out = std::format("{} has a bounty of {} gold in this hold{}. Paying the fine settles it (stolen goods are taken).", player, total,
			b.violent > 0 ? " for violent crimes" : "");
		if (b.violent > 0) {
			return out + " Violent crimes can't be bribed, sweet-talked or threatened away: refuse any such attempt.";
		}
		const bool failed = RecentlyFailed(b.faction);
		out += b.calm <= kBribeMax ? std::format(" A bribe of {} gold or more would make you look the other way; less is an insult.", BribeCost(b.calm))
		                           : " The matter is too serious for any bribe.";
		if (b.calm <= kTalkMax && !failed) {
			out += PersuadeWorks(b.calm) ? std::format(" If {} tries to talk you out of it, they are persuasive enough: let it go.", player)
			                             : std::format(" If {} tries to talk you out of it, they aren't convincing: refuse.", player);
			out += IntimidateWorks(b.calm, guard) ? std::format(" If {} threatens or intimidates you, you back down.", player)
			                                      : std::format(" If {} threatens or intimidates you, you are not impressed: refuse.", player);
		} else {
			out += std::format(" Talking or threatening won't get {} out of this{}: refuse.", player, failed ? " (they already tried)" : "");
		}
		return out;
	}

	bool Execute(const std::string& kind, RE::Actor* guard, std::int32_t offered)
	{
		const auto b = BountyFor(guard);
		if (!guard || !b.faction || (b.calm <= 0 && b.violent <= 0)) {
			return false;
		}
		const std::string name = guard->GetName();
		auto* player = Game::Player();
		const auto purse = Game::ItemCount(player, Game::Gold());
		const auto guardId = guard->GetFormID();

		if (kind == "pay_fine") {
			const auto total = b.calm + b.violent;
			if (purse < total) {
				Game::Notify(std::format("You need {} gold for the fine.", total));
				Game::Narrate(std::format("{} reached for their purse but didn't have the {} gold the fine came to.", PlayerName(), total), guard);
				return true;
			}
			auto pay = [guardId, total] {
				auto* guard = RE::TESForm::LookupByID<RE::Actor>(guardId);
				auto now = BountyFor(guard);
				if (!guard || !now.faction) {
					return;
				}
				// The vanilla way: the fine paid, stolen goods taken, no jail.
				Papyrus::CallMethod(now.faction, "Faction", "PlayerPayCrimeGold", nullptr, true, false);
				StandDown(guard);
				Game::Notify(std::format("Paid the fine ({} gold)", total));
				Game::Narrate(std::format("{} paid the {} gold fine to {}; the guard took the stolen goods and let the matter rest.", PlayerName(), total, guard->GetName()), guard);
				logger::info("crime: {} paid {} gold fine to {}", PlayerName(), total, guard->GetName());
			};
			if (!SkyrimNet::ConfigBool("features.confirm", true) || !Confirm::Available()) {
				pay();
				return true;
			}
			Confirm::Offer offer;
			offer.title = "Pay the fine";
			offer.npc = name;
			offer.sameAs = { "arrest", "bounty", "payment" };  // SeverActions' arrest popup offers the same
			offer.detail = "Your bounty in this hold (stolen goods are taken)";
			offer.price = total;
			offer.playerGold = purse;
			offer.seconds = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f));
			offer.acceptLabel = "I'll pay.";
			offer.declineLabel = "Not yet.";
			Confirm::Ask(offer, [guardId, pay](Confirm::Choice c) {
				auto* guard = RE::TESForm::LookupByID<RE::Actor>(guardId);
				if (!guard || c == Confirm::Choice::kTimeout) {
					return;
				}
				if (c == Confirm::Choice::kDecline) {
					Game::Say("Not yet.", guard, true);
					return;
				}
				Game::Say("I'll pay.", guard, false);
				pay();
			});
			return true;
		}

		// Nothing talks away blood.
		if (b.violent > 0) {
			Game::Narrate(std::format("{} wasn't having it: a violent crime is not something to bargain over.", name), guard);
			return true;
		}

		if (kind == "bribe") {
			const auto cost = BribeCost(b.calm);
			if (b.calm > kBribeMax) {
				Game::Narrate(std::format("{} eyed the coin, but the matter was far too serious to be bought off.", name), guard);
				return true;
			}
			if (offered > 0 && offered < cost) {
				Game::Notify(std::format("Not enough: about {} gold would do it", cost));
				Game::Narrate(std::format("{} looked at the {} gold offered and snorted: not nearly enough to look the other way.", name, offered), guard);
				return true;
			}
			const auto amount = offered > 0 ? offered : cost;
			if (purse < amount) {
				Game::Notify(std::format("You need {} gold.", amount));
				Game::Narrate(std::format("{} offered a bribe they couldn't actually pay.", PlayerName()), guard);
				return true;
			}
			auto pay = [guardId, amount] {
				auto* guard = RE::TESForm::LookupByID<RE::Actor>(guardId);
				auto now = BountyFor(guard);
				if (!guard || !now.faction || Game::ItemCount(Game::Player(), Game::Gold()) < amount) {
					return;
				}
				Game::Player()->RemoveItem(Game::Gold(), amount, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, guard);
				Cleared(guard, now, "bribe");
				Game::Narrate(std::format("{} pocketed the {} gold, glanced around, and decided they hadn't seen a thing.", guard->GetName(), amount), guard);
			};
			if (!SkyrimNet::ConfigBool("features.confirm", true) || !Confirm::Available()) {
				pay();
				return true;
			}
			Confirm::Offer offer;
			offer.title = "Bribe";
			offer.npc = name;
			offer.sameAs = { "arrest", "bounty", "payment" };
			offer.detail = std::format("Make a {} gold bounty go away (you keep what you took)", b.calm);
			offer.price = amount;
			offer.playerGold = purse;
			offer.seconds = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f));
			offer.acceptLabel = "Take it. You never saw me.";
			offer.declineLabel = "Never mind.";
			Confirm::Ask(offer, [guardId, pay](Confirm::Choice c) {
				auto* guard = RE::TESForm::LookupByID<RE::Actor>(guardId);
				if (!guard || c == Confirm::Choice::kTimeout) {
					return;
				}
				if (c == Confirm::Choice::kDecline) {
					Game::Say("Never mind.", guard, true);
					return;
				}
				Game::Say("Take it. You never saw me.", guard, false);
				pay();
			});
			return true;
		}

		if (kind == "persuade" || kind == "intimidate") {
			const bool threat = kind == "intimidate";
			if (b.calm > kTalkMax || RecentlyFailed(b.faction)) {
				Game::Narrate(std::format("{} had heard enough: no amount of {} would make this go away.", name, threat ? "threats" : "talk"), guard);
				return true;
			}
			const bool works = threat ? IntimidateWorks(b.calm, guard) : PersuadeWorks(b.calm);
			if (!works) {
				g_failed[b.faction->GetFormID()] = Game::Hours();
				Game::Notify(threat ? "Intimidation failed" : "Persuasion failed");
				Game::Narrate(std::format("{} wasn't {} by {}.", name, threat ? "intimidated" : "convinced", PlayerName()), guard);
				logger::info("crime: {} failed to {} {} (Speech {:.0f}, bounty {})", PlayerName(), kind, name, Speech(), b.calm);
				return true;
			}
			Cleared(guard, b, threat ? "intimidated" : "persuaded");
			Game::Narrate(threat ? std::format("{} swallowed hard and decided {} wasn't worth the trouble; the bounty was dropped.", name, PlayerName())
			                     : std::format("{} let {} talk them round and dropped the bounty.", name, PlayerName()),
				guard);
			return true;
		}
		return false;
	}

	void Reset()
	{
		g_failed.clear();
	}
}
