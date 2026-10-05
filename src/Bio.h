#pragma once

// The "mundanematters_bio" decorator used by prompts/submodules/character_bio/0190_mundanematters.prompt:
// tells an NPC what MundaneMatters will really do (room price, open promises, lent bed), so what
// they say matches what the game does.
namespace MundaneMatters::Bio
{
	void Install();

	// Game thread: refreshes cached facts (room price) for actors near the player.
	// The decorator runs on SkyrimNet's threads and only reads this cache.
	void RefreshNearby();

	// Something an NPC knows just changed (a handout): refresh on the next tick, not in 10 s.
	void Invalidate();
}
