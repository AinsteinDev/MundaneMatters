#include "Game.h"

#include "Papyrus.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Game
{
	void Notify(const std::string& msg)
	{
		SKSE::GetTaskInterface()->AddTask([msg] {
			Papyrus::CallStatic("Debug", "Notification", nullptr, RE::BSFixedString(msg));
		});
	}

	void Narrate(const std::string& what, RE::Actor* npc)
	{
		if (!npc) {
			return;
		}
		Papyrus::CallStatic("SkyrimNetApi", "DirectNarration", nullptr, RE::BSFixedString(what), static_cast<RE::Actor*>(npc), static_cast<RE::Actor*>(Player()));
	}

	namespace
	{
		std::mutex g_ownLock;
		std::deque<std::pair<std::string, std::chrono::steady_clock::time_point>> g_ownLines;
	}

	bool IsOwnLine(const std::string& text)
	{
		std::scoped_lock l(g_ownLock);
		const auto cutoff = std::chrono::steady_clock::now() - 30s;
		std::erase_if(g_ownLines, [&](const auto& e) { return e.second < cutoff; });
		return std::ranges::any_of(g_ownLines, [&](const auto& e) { return e.first == text; });
	}

	void Say(const std::string& line, RE::Actor* npc, bool respond)
	{
		if (!npc || line.empty()) {
			return;
		}
		{
			std::scoped_lock l(g_ownLock);
			g_ownLines.emplace_back(line, std::chrono::steady_clock::now());
			while (g_ownLines.size() > 16) {
				g_ownLines.pop_front();
			}
		}
		auto* player = static_cast<RE::Actor*>(Player());
		Papyrus::CallStatic("SkyrimNetApi", "RegisterDialogueToListener", nullptr, static_cast<RE::Actor*>(player), static_cast<RE::Actor*>(npc), RE::BSFixedString(line));
		if (respond) {
			// Empty narration: "answer what was just said", without a stored event of its own.
			Papyrus::CallStatic("SkyrimNetApi", "DirectNarration", nullptr, RE::BSFixedString(""), static_cast<RE::Actor*>(npc), static_cast<RE::Actor*>(player));
		}
	}

	std::string Pick(const std::vector<std::string>& options)
	{
		static std::mt19937 rng{ std::random_device{}() };
		return options[std::uniform_int_distribution<std::size_t>(0, options.size() - 1)(rng)];
	}

	void DebugNotify(const std::string& msg)
	{
		if (SkyrimNet::ConfigBool("debug.notify", true)) {
			Notify(msg);
		}
	}
}
