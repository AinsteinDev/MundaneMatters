#pragma once

// Thin wrapper over SkyrimNet's runtime C API (include/skyrimnet/PublicAPI.h).
// PublicAPI.h defines its function pointers as globals, so only SkyrimNetBridge.cpp includes it.
namespace MundaneMatters::SkyrimNet
{
	using EventCallback = std::function<void(const std::string& eventJson)>;
	using LLMCallback = std::function<void(const std::string& response, bool success)>;
	using DecoratorFn = std::function<std::string(RE::Actor*)>;

	// Resolve SkyrimNet.dll exports. Call once on kDataLoaded. False when SkyrimNet is missing.
	bool Init();
	bool IsAvailable();
	int Version();

	// The name SkyrimNet knows this actor by (it names generic NPCs: a "Vampire Fledgling" may be
	// "Tinimanwe"), or "" when it has none. Any thread.
	std::string ActorName(RE::FormID formId);
	bool IsMemoryReady();

	// Callbacks run on SkyrimNet's thread pool: never touch RE:: game state inside them,
	// hop to the game thread with SKSE::GetTaskInterface()->AddTask first.
	std::uint64_t OnEvent(const char* eventType, EventCallback cb);

	bool SendPrompt(const std::string& promptName, const std::string& variant, const json& context, LLMCallback cb);

	bool RegisterDecorator(const char* name, const char* description, DecoratorFn fn);

	// JSON array of {speaker, text, gameTime}, newest last. "[]" when unavailable.
	std::string RecentDialogue(RE::FormID npc, int maxExchanges);

	// Persist a memory for the NPC so SkyrimNet remembers what actually happened.
	int AddMemory(RE::FormID npc, const std::string& text, float importance, const char* type = "EXPERIENCE");

	bool SetBusy(RE::FormID npc, const char* reason);
	bool ClearBusy(RE::FormID npc);

	// Values from SkyrimNet's settings page for MundaneMatters (config/plugins/MundaneMatters/manifest.yaml).
	bool ConfigBool(const char* path, bool fallback);
	float ConfigFloat(const char* path, float fallback);
}
