#include "Game.h"

#include "Papyrus.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Game
{
	namespace
	{
		std::thread::id g_mainThread;
		std::mutex g_nameLock;
		std::unordered_map<RE::FormID, std::string> g_names;  // SkyrimNet's names, looked up on the main thread
	}

	void NoteMainThread()
	{
		g_mainThread = std::this_thread::get_id();
	}

	std::string NameOf(RE::Actor* a)
	{
		if (!a) {
			return "";
		}
		const std::string own = a->GetName() ? a->GetName() : "";
		if (a->IsPlayerRef()) {
			return own;
		}
		const auto id = a->GetFormID();
		{
			std::scoped_lock l(g_nameLock);
			if (auto it = g_names.find(id); it != g_names.end()) {
				return it->second.empty() ? own : it->second;
			}
		}
		// SkyrimNet is asked only from the game's main thread: elsewhere (a SkyrimNet worker building a
		// prompt) it could be waiting on itself. Meanwhile the game's own name, and the lookup queued.
		if (g_mainThread != std::thread::id{} && std::this_thread::get_id() == g_mainThread) {
			auto n = SkyrimNet::ActorName(id);
			if (n.empty()) {
				return own;  // not known to SkyrimNet (yet): asked again next time
			}
			std::scoped_lock l(g_nameLock);
			g_names[id] = n;
			return n;
		}
		// One lookup queued per actor at a time.
		static std::unordered_set<RE::FormID> queued;
		{
			std::scoped_lock l(g_nameLock);
			if (!queued.insert(id).second) {
				return own;
			}
		}
		SKSE::GetTaskInterface()->AddTask([id] {
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
				NameOf(actor);
			}
			std::scoped_lock l(g_nameLock);
			queued.erase(id);
		});
		return own;
	}

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
