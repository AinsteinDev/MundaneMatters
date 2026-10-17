#pragma once

// "Hold this for me": the player leaves things with a companion or a friend and gets them back
// when they ask. Companions keep them in their own pack (the vanilla follower window); a friend
// puts them away safely (a hidden chest of theirs, so nothing gets worn, drunk or sold).
namespace MundaneMatters::Safekeeping
{
	// Game thread, after the NPC agreed. `item` is what was named ("sword", "my potions"), or ""
	// for "hold my stuff". A clear single item moves after a confirm popup; anything vague opens a
	// window to choose (the follower window for companions, a picker for friends).
	bool Hold(RE::Actor* npc, const std::string& item);

	// Game thread. "Can I have my sword back?" / "I'll take my things back".
	bool Retrieve(RE::Actor* npc, const std::string& item);

	// Any thread. True when this NPC is keeping something for the player.
	bool IsHolding(RE::FormID npc);

	// Any thread. A line for the NPC's bio: what they are keeping for the player.
	std::vector<std::string> BioLines(RE::FormID npc);

	// Game thread, every ledger tick: a keeper who died leaves the things on their body.
	void Tick();

	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();

	// Watches the follower window closing, to tell the NPC what changed hands. After data load.
	void Install();
}
