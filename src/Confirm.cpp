#include "Confirm.h"

#include "PrismaUI_API.h"

namespace MundaneMatters::Confirm
{
	bool Show(const Offer& offer, std::function<void(Choice)> done);

	namespace
	{
		using Clock = std::chrono::steady_clock;

		PRISMA_UI_API::IVPrismaUI1* g_ui = nullptr;
		PrismaView g_view = 0;
		std::atomic_bool g_ready{ false };

		std::mutex g_lock;
		std::function<void(Choice)> g_done;  // set while a popup is open
		std::pair<RE::FormID, std::int32_t> g_lastPick{ 0, 0 };
		std::uint64_t g_ticket = 0;        // identifies the open popup (guards the fallback timer)

		// Popups asked for while one is open ("a room please, and here's a tip"): shown in turn.
		struct Queued
		{
			Offer offer;
			std::function<void(Choice)> done;
			std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
		};
		// A queued popup older than this is about a moment that has passed (the NPC walked off,
		// a save was loaded): it expires as unanswered instead of popping up out of nowhere.
		constexpr auto kQueuedLifetime = 60s;
		std::deque<Queued> g_queue;
		constexpr std::size_t kMaxQueued = 3;

		// Closes the popup and runs its callback once, whoever answers first (JS, timer, fallback).
		void Finish(std::uint64_t ticket, Choice choice)
		{
			std::function<void(Choice)> done;
			{
				std::scoped_lock l(g_lock);
				if (!g_done || ticket != g_ticket) {
					return;
				}
				done = std::move(g_done);
				g_done = nullptr;
			}
			SKSE::GetTaskInterface()->AddTask([done = std::move(done), choice] {
				if (g_ui && g_ui->IsValid(g_view)) {
					g_ui->Unfocus(g_view);
					g_ui->Hide(g_view);
				}
				done(choice);
				// Next one waiting, if any; stale ones expire unanswered.
				for (;;) {
					std::optional<Queued> next;
					{
						std::scoped_lock l(g_lock);
						if (g_queue.empty() || g_done) {
							break;
						}
						next = std::move(g_queue.front());
						g_queue.pop_front();
					}
					if (std::chrono::steady_clock::now() - next->at > kQueuedLifetime) {
						logger::info("confirm popup: '{}' expired in the queue", next->offer.title);
						next->done(Choice::kTimeout);
						continue;
					}
					Show(next->offer, std::move(next->done));
					break;
				}
			});
		}

		// JS: window.mundaneMattersChoice("accept" | "decline" | "timeout")
		void OnChoice(const char* arg)
		{
			const std::string choice = arg ? arg : "";
			std::uint64_t ticket;
			{
				std::scoped_lock l(g_lock);
				ticket = g_ticket;
			}
			logger::info("confirm popup: {}", choice);
			// Picker: "pick:<formid hex>:<qty>"
			if (choice.starts_with("pick:")) {
				RE::FormID id = 0;
				std::int32_t qty = 1;
				try {
					const auto rest = choice.substr(5);
					const auto colon = rest.find(':');
					id = static_cast<RE::FormID>(std::stoul(rest.substr(0, colon), nullptr, 16));
					qty = colon == std::string::npos ? 1 : std::stoi(rest.substr(colon + 1));
				} catch (...) {
					id = 0;
				}
				{
					std::scoped_lock l(g_lock);
					g_lastPick = { id, (std::max)(1, qty) };
				}
				Finish(ticket, id ? Choice::kAccept : Choice::kDecline);
				return;
			}
			Finish(ticket, choice == "accept" ? Choice::kAccept : choice == "decline" ? Choice::kDecline : Choice::kTimeout);
		}
	}

	namespace
	{
		// Typing in SkyrimNet's chat box (a PrismaUI view), in a menu, or loading: a popup now would
		// steal keystrokes ("e" would accept) or go unseen.
		bool PlayerBusy()
		{
			if (g_ui && g_ui->HasAnyActiveFocus() && !g_ui->HasFocus(g_view)) {
				return true;
			}
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			static const std::array<std::string_view, 16> kMenus{
				"Console", "InventoryMenu", "BarterMenu", "Dialogue Menu", "TweenMenu", "MagicMenu", "ContainerMenu",
				"GiftMenu", "TrainingMenu", "Journal Menu", "MapMenu", "Sleep/Wait Menu", "Crafting Menu", "Book Menu",
				"Lockpicking Menu", "Loading Menu"
			};
			return ui->GameIsPaused() || std::ranges::any_of(kMenus, [ui](std::string_view m) { return ui->IsMenuOpen(m); });
		}

		void Display(std::uint64_t ticket, const std::string& script, std::int32_t seconds)
		{
			{
				std::scoped_lock l(g_lock);
				if (!g_done || ticket != g_ticket) {
					return;  // answered or replaced meanwhile
				}
			}
			g_ui->Invoke(g_view, script.c_str());
			g_ui->Show(g_view);
			if (!g_ui->Focus(g_view, false)) {
				logger::warn("confirm popup: could not take focus; only the timer can answer it");
			}
			if (seconds <= 0) {
				return;  // no timer: only the player closes it
			}
			// The page runs the countdown; this backstop covers a page that never answers.
			std::thread([ticket, seconds] {
				std::this_thread::sleep_for(std::chrono::seconds(seconds + 3));
				Finish(ticket, Choice::kTimeout);
			}).detach();
		}
	}

	bool Init()
	{
		g_ui = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
		if (!g_ui) {
			logger::warn("PrismaUI not found - paid services run without a confirm popup");
			return false;
		}
		g_view = g_ui->CreateView("MundaneMatters/index.html", [](PrismaView) {
			g_ready = true;
			logger::info("confirm view ready");
		});
		g_ui->RegisterJSListener(g_view, "mundaneMattersChoice", OnChoice);
		g_ui->Hide(g_view);
		return true;
	}

	bool Available()
	{
		return g_ui && g_ready && g_ui->IsValid(g_view);
	}

	std::pair<RE::FormID, std::int32_t> LastPick()
	{
		std::scoped_lock l(g_lock);
		return g_lastPick;
	}

	void Reset()
	{
		{
			std::scoped_lock l(g_lock);
			g_done = nullptr;
			g_queue.clear();
			++g_ticket;  // any waiter or timer of the old popup finds it gone
		}
		if (g_ui && g_ui->IsValid(g_view)) {
			g_ui->Unfocus(g_view);
			g_ui->Hide(g_view);
		}
	}

	bool Ask(const Offer& offer, std::function<void(Choice)> done)
	{
		if (!Available()) {
			return false;
		}
		{
			std::scoped_lock l(g_lock);
			if (g_done) {
				if (g_queue.size() >= kMaxQueued) {
					return false;
				}
				g_queue.push_back({ offer, std::move(done), std::chrono::steady_clock::now() });
				logger::info("confirm popup: queued '{}' behind the open one", offer.title);
				return true;
			}
		}
		return Show(offer, std::move(done));
	}

	bool Show(const Offer& offer, std::function<void(Choice)> done)
	{
		std::uint64_t ticket;
		{
			std::scoped_lock l(g_lock);
			if (g_done) {
				g_queue.push_front({ offer, std::move(done), std::chrono::steady_clock::now() });
				return true;
			}
			g_done = std::move(done);
			ticket = ++g_ticket;
		}

		json data{
			{ "title", offer.title },
			{ "npc", offer.npc },
			{ "detail", offer.detail },
			{ "price", offer.price },
			{ "gold", offer.playerGold },
			{ "seconds", offer.seconds },
			{ "showPrice", offer.showPrice },
			{ "accept", offer.acceptLabel.empty() ? "Accept" : offer.acceptLabel },
			{ "decline", offer.declineLabel.empty() ? "Decline" : offer.declineLabel },
			{ "items", json::array() }
		};
		for (auto& it : offer.pick) {
			data["items"].push_back({ { "id", std::format("{:X}", it.id) }, { "name", it.name }, { "count", it.count },
				{ "value", it.value }, { "equipped", it.equipped } });
		}
		const auto script = std::format("mundaneMattersShow({})", data.dump());
		const auto seconds = offer.seconds;
		// Busy (typing in the chat box, a menu, a loading screen): the popup waits, its timer not yet
		// started, until the player is free. Up to a minute; then it expires unanswered.
		std::thread([ticket, script, seconds] {
			const auto giveUp = std::chrono::steady_clock::now() + 60s;
			for (;;) {
				auto busy = std::make_shared<std::atomic_int>(-1);
				SKSE::GetTaskInterface()->AddTask([busy] { *busy = PlayerBusy() ? 1 : 0; });
				for (int i = 0; i < 100 && *busy < 0; ++i) {
					std::this_thread::sleep_for(20ms);
				}
				{
					std::scoped_lock l(g_lock);
					if (!g_done || ticket != g_ticket) {
						return;  // answered or replaced meanwhile
					}
				}
				if (*busy == 0) {
					break;
				}
				if (std::chrono::steady_clock::now() > giveUp) {
					logger::info("confirm popup: the player stayed busy, expired unanswered");
					Finish(ticket, Choice::kTimeout);
					return;
				}
				std::this_thread::sleep_for(300ms);
			}
			SKSE::GetTaskInterface()->AddTask([ticket, script, seconds] { Display(ticket, script, seconds); });
		}).detach();
		return true;
	}
}
