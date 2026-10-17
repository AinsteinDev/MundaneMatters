#pragma once

// Shared moments, the follow-ups to a shared meal. First: a drink together ("A round of mead?").
// Somewhere with a seat (a tavern, a home, a camp), they sit down with the player for a couple of
// minutes over a real drink, and the player drinks theirs (its own effects: the warm, tipsy part).
// Companions and friends say yes anywhere fitting and treat the player; an acquaintance only at
// a tavern, with the player buying the round; nobody drinks in a temple, a shop or a jail.
namespace MundaneMatters::Moments
{
	// Game thread, after the NPC agreed. `said`: the drink named ("mead"), "" for any.
	bool Drink(RE::Actor* npc, const std::string& said);

	// Game thread, after the NPC agreed: "sit with me a while". `said`: where ("fire", "ground",
	// "bench", "" = wherever fits); `hours`: how long the player said (0 = not said). They sit
	// beside the player (the seat closest to them if they're sitting; by a fire if there is one;
	// a seat; else the ground, a spot put down for each). Once both are sitting, time may pass:
	// the hours said, or the player picks (a popup), or just sitting. They get up when the player does.
	bool Sit(RE::Actor* npc, const std::string& said, float hours);

	// The player is sitting: on a chair or bench, or on the floor (a sit-on-the-ground mod's animation
	// with no furniture counts too). Game thread.
	bool PlayerSitting();

	// A real seat (chair, bench, stool), not a cooking spit, workbench, cart or throne.
	bool IsSeat(RE::TESObjectREFR* ref);

	// Game thread, after the NPC agreed: "play us something" / "sing me a song". `said`: the song
	// or instrument named ("Ragnar the Red", "lute", ""); `how`: "sing", "play" or "any".
	//   bards  - the base game's songs and instruments (SkyrimNet's own AI bard songs instead, when
	//            the player has those on);
	//   anyone - their own instrument if they carry one, else they hum (a sung line at most: no
	//            singing voice to speak of, and a spoken "song" would sound like a recital);
	//   on the road with a companion, or sitting together: hummed as they are, no standing up to play.
	// A fight, or the player walking off, stops the music.
	bool Perform(RE::Actor* npc, const std::string& said, const std::string& how);

	// Game thread, every ledger tick: is the player sitting with them, did they get up.
	void Tick();

	// Any thread. A line for the bio while they sit together.
	std::vector<std::string> BioLines(RE::FormID npc);

	// Game thread. Another activity takes this NPC over (bed, a meal, following, leading): a drink
	// or a sit won't stand them up later, and a sit with them ends quietly.
	void Release(RE::FormID npc);

	// Game thread. Sitting a while with the player right now.
	bool IsSitting(RE::FormID npc);

	// A save was loaded: drop the cooldowns.
	void Reset();
}
