#pragma once

// The player's own promises, the mirror of NPCs keeping theirs. "I'll bring you a bottle of mead
// tomorrow", "I'll find your ring", "I'll pay you back the 50", "I'll come by tomorrow evening":
// tracked with the deadline said (or a fair one). Kept (the thing handed over, the coin paid, the
// visit made) and they're glad: a Bonds deed and a warm memory. Broken (the deadline passes) and
// they're disappointed: they remember, bring it up, and a little of the bond is lost. Not a grudge.
// The list opens in the game's own book screen: a hotkey, or a thought or line like "what did I
// promise Ronja?".
namespace MundaneMatters::Pledges
{
	// Game thread. The classifier's "player_promises" for this exchange (made, or taken back), and
	// "player_kept" (what the NPC says the player did as promised).
	void FromExchange(RE::FormID npc, const json& promises, const std::string& kept, const std::string& playerLine);

	// Game thread. The player handed `n` of `item` to the NPC: how many of them keep a promise
	// ("bring" / "find"); those aren't a gift. `early`: set when it came well before the time promised.
	std::int32_t Delivered(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t n, bool* early = nullptr);

	// Game thread. Coin handed over: does it keep a "pay back" promise?
	bool Paid(RE::Actor* npc, std::int32_t gold);

	// Game thread. The player and this NPC just had an exchange: a goodbye (or walking off soon
	// after) is when someone close might ask a favour (bring something, take something to someone
	// they know, look in on them), by chance: the closer, the likelier; the more the player already
	// owes, the rarer. Saying yes makes it a promise.
	void OnExchange(RE::FormID npc, const std::string& playerLine);

	// Any thread. This NPC just asked a favour and is waiting for the player's answer (so even a
	// plain "sure" gets classified).
	bool Waiting(RE::FormID npc);

	// Game thread. A promise of the player's added by hand (the grants file): "bring" / "visit" / ...,
	// due in `hours` game hours. False when the same one is already open.
	bool Grant(RE::Actor* npc, const std::string& type, const std::string& item, const std::string& words, float hours);

	// Any thread. A thought or line asking what the player promised ("what did I promise Ronja?",
	// "I forgot what I wanted to get for Hulda"). `spoken`: said aloud to someone, so only plain asks.
	bool AsksAboutPromises(const std::string& line, bool spoken);

	// Game thread. Opens the promises in the book screen (when no other menu is up).
	void OpenBook();

	// Game thread. The open promises in a popup: Break one (broken, with what that costs) or
	// Dissolve it (gone, no consequence). The promises key pressed again while the book is open.
	void ManageBook();

	// Any thread. Lines for the NPC's bio: what the player promised them.
	std::vector<std::string> BioLines(RE::FormID npc);

	// Game thread, every ledger tick: visits made, deadlines passed, broken promises brought up.
	void Tick();

	json Save();
	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve);
	void Revert();

	// The hotkey and the thought listener. After data load.
	void Install();
}
