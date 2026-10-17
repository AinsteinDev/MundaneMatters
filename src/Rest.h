#pragma once

// "Let's rest": the NPC (and the companions nearby) go to bed with the player. Real beds where
// the place has them (their home, the player's, the rented inn room, a camp); otherwise a bedroll
// appears beside them, never on city streets. They lie down now and get up when the player wakes,
// or after a while if the player doesn't sleep. Spawned bedrolls go away once the player leaves.
namespace MundaneMatters::Rest
{
	// Game thread. Runs "let's rest" with this NPC. The real relationship decides: companions and
	// friends rest with the player, an acquaintance declines. False when nothing could start.
	bool Start(RE::Actor* npc);

	// Game thread, every ledger tick: wake-up, give-up and bedroll clean-up.
	void Tick();

	// Game thread. A plain bedroll laid at `at` facing `angle`, for a host's home (who sleeps where);
	// not packed away by Rest: the caller removes it with PackBedroll. Null if none could be made.
	RE::TESObjectREFR* LayBedroll(const RE::NiPoint3& at, float angle);
	// Game thread. Bedrolls laid in this cell (placed in game, the plain bedroll this mod lays), oldest
	// first, leaving out the ones Rest keeps for a rest of its own.
	std::vector<RE::TESObjectREFR*> BedrollsIn(RE::TESObjectCELL* cell);
	void PackBedroll(RE::FormID ref);

	// Game thread. Into that bed or bedroll now (walking there first), and back up again.
	void LieDown(RE::Actor* a, RE::TESObjectREFR* bed);
	void GetUp(RE::Actor* a);

	// "Ally", "Friend", "Acquaintance"... (or "Companion"), from the real game relationship.
	std::string RelationName(RE::Actor* npc);

	// Co-save: the spawned bedrolls still standing and the current rest, as JSON.
	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();

	// Registers the sleep-stop listener. Call once after the data is loaded.
	void Install();
}
