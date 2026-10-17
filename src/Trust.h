#pragma once

// Wearing out your welcome. Stealing from someone who trusts you, hurting them with words, or
// hitting them outside a fight breaks that trust: their key, guest rights, lent bed, permissions
// and pending gifts are taken back, and they do the player no favours until an apology they accept.
// Forgiven is not restored: everything has to be earned again. A real attack (they fight back, or
// the game gives an assault bounty) is never forgiven. Stray hits in a fight and brawls don't count.
namespace MundaneMatters::Trust
{
	// Registers the theft, hit and combat listeners. After data load.
	void Install();

	// Any thread. True while this NPC holds a grudge (wronged and not forgiven, or attacked).
	bool Holds(RE::FormID npc);

	// Any thread. How serious a grudge it is, and for how long it has been held: an attack (never
	// forgiven), or hurt for `hours` game hours. Both false / -1 without a grudge.
	bool Attacked(RE::FormID npc);
	float GrudgeHours(RE::FormID npc);

	// Any thread. Holds a grudge, or forgave the player less than `hours` game hours ago: what the
	// player hands over now is making amends (giving back what was taken), not a favour.
	bool MakingAmends(RE::FormID npc, float hours = 24.0f);

	// Game thread. Someone whose trust can be lost: they gave the player something (a key, guest
	// rights, a bed, a permission), are a friend or closer, or already hold a grudge.
	bool Matters(RE::Actor* npc);

	// Game thread. With a grudge: says no in character (`what` = the favour asked) and returns true.
	bool Refuse(RE::Actor* npc, const std::string& what);

	// Any thread, from the classifier: the player's line hurt them ("hurtful"), or the NPC accepted
	// an apology.
	void OnExchange(RE::FormID npc, const std::string& offense, bool apologyAccepted);

	// Any thread. Lines for the NPC's bio: the grudge, or a recent forgiveness.
	std::vector<std::string> BioLines(RE::FormID npc);

	// Game thread, every ledger tick: delivers a reaction the NPC hasn't had yet (they weren't around).
	void Tick();

	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();
}
