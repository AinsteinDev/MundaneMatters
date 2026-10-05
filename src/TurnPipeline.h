#pragma once

// The exchange pipeline:
//   player line (dialogue_player_text / _stt) is held as "pending"
//   -> the NPC's reply to the player ("dialogue", target = player) names who was asked
//   -> mundanematters_turn.prompt classifies request / response / promises in one LLM call
//   -> accepted services run once the NPC's speech has finished.
// Replaces DialogueWatcher (milestone 2), which only logged.
namespace MundaneMatters::TurnPipeline
{
	void Install();

	// A save was loaded: forget the last unanswered player line.
	void Reset();

	// Game thread, from the ledger tick: runs the self-test when its setting is switched on.
	void PollSelfTest();
}
