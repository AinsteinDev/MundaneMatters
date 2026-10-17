#pragma once

// Care: patching the player up, cooking their ingredients, tempering their gear, curing their
// diseases. What happens is the game's own (a real potion or healing spell, real cooking-pot
// recipes, one real tempering step, the diseases really removed); who does it, and whether it's
// free, goes by the real relationship and the NPC's calling:
//   heal  - priests/healers for anyone, free; companions and friends free; an acquaintance who
//           knows a healing spell for a small fee; nobody uses their own potion on a stranger.
//   cook  - companions and friends free; innkeepers and cooks for a few coins a dish.
//   mend  - smiths only: free for friends, for coin otherwise, up to what their skill allows.
//   cure  - priests and healers, always free.
// "Confirm Care" asks first for paid care and for the free cure; free healing, cooking and
// mending just happen.
namespace MundaneMatters::Care
{
	bool IsPriest(RE::Actor* npc);
	bool IsSmith(RE::Actor* npc);

	// Game thread, after the NPC agreed. `said`: what was named ("my sword", "the salmon").
	bool Heal(RE::Actor* npc);
	bool Cook(RE::Actor* npc, const std::string& said);
	bool Mend(RE::Actor* npc, const std::string& said);
	bool Cure(RE::Actor* npc);

	// A save was loaded: forget heal cooldowns.
	void Reset();
}
