#pragma once

// Vanilla dialogue by talking - with the vanilla dialogue menu open, the player can say an option
// instead of clicking it. Plain rules only (no model guesses), so it behaves the same every time:
//   1. The player says an option exactly or closely (most of its own words), or plainly threatens with
//      an "(Intimidate)" option on screen: that option is clicked in the menu at once - the NPC's real
//      voice, the game's own logic (speech checks rolled by the game), the follow-up options appear.
//      The AI's reply to the same line is cut, actions and all.
//   2. The NPC's AI reply repeats most of the real answer of an option not said yet, and the player's
//      line fits that option: it's clicked, its recording skipped (the AI already said it).
//   3. Anything else is ordinary SkyrimNet talk.
// With the menu closed nothing is picked (only the menu shows what's really available, in order).
namespace MundaneMatters::VanillaLines
{
	// Game thread: the player spoke to this NPC.
	void OnPlayerLine(RE::FormID npc, std::string text);

	// Any thread: this NPC said something (an AI reply, or the vanilla line itself as SkyrimNet records it).
	void OnNpcLine(RE::FormID npc, const std::string& text);

	// Any thread. After a quest line went off: the NPC stands by it and doesn't repeat it.
	std::vector<std::string> BioLines(RE::FormID npc);

	// Any thread: a vanilla line was triggered for this NPC within the last `seconds` (a quest line
	// accepted that way lives in the journal, not the player's promises book).
	bool FiredRecently(RE::FormID npc, int seconds);

	// A plain threat ("or else", "you'll lose your hands", "don't you dare"...).
	bool IsThreat(const std::string& line);

	// Nothing to run: the rules are plain and checked offline (tools/logic_check.py).
	void SelfTest();
}
