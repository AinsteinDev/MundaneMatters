#pragma once

// Keepsakes: an ally or a lover now and then gives the player a small thing of their own, unasked:
// food they saved, a flower they picked, a potion because they worry, a little token. Only from
// what they really carry; never what they wear, need or were lent. Each day every ally or lover
// has one kind of moment picked at random (meeting again after a day apart, the end of a drink,
// meal or sit together, the end of a fight, arriving somewhere together, a quiet hour from noon
// to night; never the start of the day): if it comes, a chance of a gift. Capped: one a day
// across everyone, and a week between gifts from the same person.
namespace MundaneMatters::Keepsakes
{
	// Game thread, every ledger tick.
	void Tick();

	// Game thread. A drink, a meal or a sit together with this NPC just ended.
	void OnMoment(RE::FormID npc);

	// Watches what the player hands people (never given back as a present). After data load.
	void Install();

	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();
}
