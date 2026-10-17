#pragma once

// Words with weight, with guards: pay the fine, bribe, persuade or intimidate in plain talk, for a
// bounty with the guard's own hold. The game decides (the bounty, the player's Speech and level),
// never the conversation: the guard is told the real outcome before answering (bio), and the
// bounty only changes when the check really passed.
//   pay_fine   - vanilla PlayerPayCrimeGold: stolen goods taken, no jail.
//   bribe      - clears a non-violent bounty up to 1000; costs more or less than the fine by
//                Speech, and the player keeps their stolen goods.
//   persuade   - Speech against the bounty (non-violent, up to 500).
//   intimidate - Speech and level against the bounty and the guard's level (non-violent, up to 500).
// Violent crimes can't be talked away. A failed persuade or threat can't be retried with that hold
// for a day.
namespace MundaneMatters::Crime
{
	// Game thread. Does the player have a bounty this guard enforces?
	bool HasBounty(RE::Actor* guard);

	// Game thread. Lines for the guard's bio: what each approach would really get the player now.
	std::string GuardFacts(RE::Actor* guard);

	// Game thread, after the guard's reply: `kind` is pay_fine / bribe / persuade / intimidate;
	// `offered` the gold offered (bribe).
	bool Execute(const std::string& kind, RE::Actor* guard, std::int32_t offered);

	// A save was loaded: forget failed attempts.
	void Reset();
}
