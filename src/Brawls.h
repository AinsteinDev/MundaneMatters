#pragma once

// After a brawl (SeverActions runs the fight itself): the NPC really knows who won, and carries it.
// SeverActions sends its own "SeverBrawl_Ended" event (exact reason, winner, loser); an LLM can still read "drops to one knee" the
// wrong way round and gloat over a fight it lost. So both sides get the plain fact in their bio, by
// the kind of fight:
//   a friendly spar (a friend, a companion): "good fight", sore and in good spirits, a rematch some
//     day; a Bonds deed either way.
//   a real fight (anyone else): the loser has been shown their place - bruised pride, warier and
//     more respectful around the player; the winner is smug.
// Strongest for the first couple of hours, fading over three days; a memory stays for good.
namespace MundaneMatters::Brawls
{
	// Any thread. The fact for this NPC's bio while it's fresh.
	std::vector<std::string> BioLines(RE::FormID npc);

	// Game thread: the player just threatened / intimidated this NPC. They stay shaken around the
	// player for a couple of game hours, wary for a few more.
	void Threatened(RE::FormID npc);

	// Listens for SeverActions' SeverBrawl_Ended mod event.
	void Install();

	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();
}
