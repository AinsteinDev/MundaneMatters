#include "Game.h"

#include "Papyrus.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Game
{
	void Notify(const std::string& msg)
	{
		SKSE::GetTaskInterface()->AddTask([msg] {
			Papyrus::CallStatic("Debug", "Notification", nullptr, RE::BSFixedString("MM_" + msg));
		});
	}

	namespace
	{
		std::atomic<std::int64_t> g_lastSpoke{ 0 };  // steady-clock ms

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}

	void NoteSpoke()
	{
		g_lastSpoke = NowMs();
	}

	namespace
	{
		std::mutex g_narrLock;
		std::unordered_map<RE::FormID, std::int64_t> g_lastNarrated;  // per NPC, steady-clock ms

		// Prompted to speak a moment ago: a second prompt now would make them talk twice in a row
		// (two reactions landing together). True = go ahead (and noted).
		bool MaySpeakNow(RE::Actor* npc)
		{
			const auto now = NowMs();
			std::scoped_lock l(g_narrLock);
			auto& last = g_lastNarrated[npc->GetFormID()];
			if (now - last < 6000) {
				return false;
			}
			last = now;
			return true;
		}

		void Remember(const std::string& what, RE::Actor* npc)
		{
			Papyrus::CallStatic("SkyrimNetApi", "RegisterPersistentEvent", nullptr, RE::BSFixedString(what), static_cast<RE::Actor*>(npc),
				static_cast<RE::Actor*>(Player()));
		}
	}

	void Narrate(const std::string& what, RE::Actor* npc)
	{
		if (!npc) {
			return;
		}
		if (!MaySpeakNow(npc)) {
			logger::info("narration for {} kept as context (they were just prompted): {}", npc->GetName(), what.substr(0, 120));
			Remember(what, npc);
			return;
		}
		Papyrus::CallStatic("SkyrimNetApi", "DirectNarration", nullptr, RE::BSFixedString(what), static_cast<RE::Actor*>(npc), static_cast<RE::Actor*>(Player()));
	}

	void Mention(const std::string& what, RE::Actor* npc)
	{
		if (!npc) {
			return;
		}
		if (NowMs() - g_lastSpoke.load() < 8000 || !MaySpeakNow(npc)) {
			Papyrus::CallStatic("SkyrimNetApi", "RegisterPersistentEvent", nullptr, RE::BSFixedString(what), static_cast<RE::Actor*>(npc),
				static_cast<RE::Actor*>(Player()));
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
