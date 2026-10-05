#include "SkyrimNetBridge.h"

#include <skyrimnet/PublicAPI.h>

namespace MundaneMatters::SkyrimNet
{
	namespace
	{
		std::atomic_bool g_available{ false };
		int g_version = 0;
	}

	bool Init()
	{
		if (!FindFunctions() || !PublicGetVersion) {
			logger::warn("SkyrimNet.dll not found or too old - MundaneMatters is inactive");
			return false;
		}

		g_version = PublicGetVersion();
		logger::info("SkyrimNet public API v{}", g_version);

		if (g_version < 8 || !PublicRegisterEventCallback || !PublicSendCustomPromptToLLM) {
			logger::error("SkyrimNet API v8+ required (event callbacks + custom prompts), found v{}", g_version);
			return false;
		}

		g_available = true;
		return true;
	}

	bool IsAvailable() { return g_available; }
	int Version() { return g_version; }

	bool IsMemoryReady()
	{
		return g_available && PublicIsMemorySystemReady && PublicIsMemorySystemReady();
	}

	std::uint64_t OnEvent(const char* eventType, EventCallback cb)
	{
		if (!g_available) {
			return 0;
		}
		auto id = PublicRegisterEventCallback(eventType, [cb = std::move(cb)](const char* raw) {
			try {
				cb(raw ? std::string(raw) : std::string{});
			} catch (const std::exception& e) {
				logger::error("event callback threw: {}", e.what());
			}
		});
		logger::info("listening for SkyrimNet '{}' events (id {})", eventType, id);
		return id;
	}

	bool SendPrompt(const std::string& promptName, const std::string& variant, const json& context, LLMCallback cb)
	{
		if (!g_available) {
			return false;
		}
		auto ctx = context.dump();
		return PublicSendCustomPromptToLLM(promptName.c_str(), variant.c_str(), ctx.c_str(),
			[cb = std::move(cb)](const char* response, int success) {
				try {
					cb(response ? std::string(response) : std::string{}, success == 1);
				} catch (const std::exception& e) {
					logger::error("LLM callback threw: {}", e.what());
				}
			});
	}

	bool RegisterDecorator(const char* name, const char* description, DecoratorFn fn)
	{
		if (!g_available || !PublicRegisterDecorator) {
			return false;
		}
		bool ok = PublicRegisterDecorator(name, description, std::move(fn));
		logger::info("decorator '{}' {}", name, ok ? "registered" : "FAILED");
		return ok;
	}

	std::string RecentDialogue(RE::FormID npc, int maxExchanges)
	{
		if (!IsMemoryReady() || !PublicGetRecentDialogue) {
			return "[]";
		}
		return PublicGetRecentDialogue(npc, maxExchanges);
	}

	int AddMemory(RE::FormID npc, const std::string& text, float importance, const char* type)
	{
		if (!IsMemoryReady() || !PublicAddMemory) {
			return 0;
		}
		return PublicAddMemory(npc, text.c_str(), importance, type, "", "", "[\"mundanematters\"]", "[20]");
	}

	bool SetBusy(RE::FormID npc, const char* reason)
	{
		return g_available && PublicSetActorBusy && PublicSetActorBusy(npc, reason);
	}

	bool ClearBusy(RE::FormID npc)
	{
		return g_available && PublicClearActorBusy && PublicClearActorBusy(npc);
	}

	namespace
	{
		std::optional<std::string> ConfigValue(const char* path)
		{
			if (!g_available || !PublicGetPluginConfigValue) {
				return std::nullopt;
			}
			auto v = PublicGetPluginConfigValue("MundaneMatters", path, "");
			return v.empty() ? std::nullopt : std::optional{ v };
		}
	}

	bool ConfigBool(const char* path, bool fallback)
	{
		auto v = ConfigValue(path);
		if (!v) {
			return fallback;
		}
		return *v == "true" || *v == "1";
	}

	float ConfigFloat(const char* path, float fallback)
	{
		auto v = ConfigValue(path);
		if (!v) {
			return fallback;
		}
		try {
			return std::stof(*v);
		} catch (...) {
			return fallback;
		}
	}
}
