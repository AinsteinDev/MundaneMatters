#pragma once

// "Here's a spare key": a lover or an ally (never a mere friend) trusts the player with their
// home. Its locked doors open for the player from then on, as if they had always been given the
// key (the house's real key too, when the lock has one). Either side can bring it up.
namespace MundaneMatters::Keys
{
	// Game thread. The NPC hands over a key to their home. False when they aren't close enough
	// (they think better of it, in character, and the notification shows the relationship).
	bool Give(RE::Actor* npc);

	// Game thread. "I want my key back": the doors lock again and the key goes back.
	void Revoke(RE::FormID npc, bool notify = true);

	// Any thread.
	bool Has(RE::FormID npc);
	std::vector<RE::FormID> Holders();
	std::vector<std::string> BioLines(RE::FormID npc);

	// Game thread, every ledger tick: unlocks a key holder's door as the player walks up to it,
	// and locks it again once they're gone.
	void Tick();

	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();
}
