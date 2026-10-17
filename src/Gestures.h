#pragma once

// The NPC does what their own narration says they do - the small physical things SkyrimNet's actions
// don't cover. Only *starred* narration in their own line, and only about themselves:
//   - a weapon drawn ("*draws her dagger*") or put away ("*sheathes it*"); no fight starts, and it goes
//     away again on its own after a while;
//   - standing up from their seat ("*stands up*"), coming over to the player ("*walks over to you*");
//   - a vanilla gesture: laugh, applaud, salute, wave, bow, point, beckon, head shake, nervous fidget,
//     wipe brow, scratch head, a sip.
// Touching the player, nods and shrugs (no vanilla animation) stay words. Never in combat; no
// gesture while seated or walking (it would break the pose).
namespace MundaneMatters::Gestures
{
	struct Act
	{
		enum class Kind
		{
			kNone,
			kDraw,
			kSheathe,
			kStand,
			kApproach,
			kIdle
		};
		Kind kind = Kind::kNone;
		std::string idle;  // kIdle: the animation event
	};

	// What the narration in `line` has the speaker do (plain; mirrored in tools/logic_check.py).
	std::vector<Act> Read(const std::string& line, const std::string& speakerName);

	// Any thread: the NPC said `line` (their AI reply).
	void OnNpcLine(RE::FormID npc, const std::string& line);
}
