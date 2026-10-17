#include "Confirm.h"

#include "PrismaUI_API.h"
#include "Papyrus.h"

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
		std::string g_openKey;             // title + NPC of the open popup (the same one isn't asked twice)
		bool g_silentAlt = false;          // the open popup's third button is "Refuse silently"

		std::string KeyOf(const Offer& o)
		{
			return o.title + "|" + o.npc;
		}

		// ---- SeverActions' popups (Magelight prompts) ----
		// Its actions run on the NPC's line, ours after it has been spoken: what it asked first, ours
		// doesn't ask again. Watched for a while after each NPC line and while ours waits to show.
		struct OtherPrompt
		{
			const char* kind;
			const char* script;
			const char* fn;
		};
		constexpr std::array<OtherPrompt, 10> kOtherPrompts{ {
			{ "travel", "SeverActionsNativeExt", "Magelight_IsTravelPromptOpen" },
			{ "payment", "SeverActionsNative", "Magelight_IsPaymentPromptOpen" },
			{ "trade", "SeverActionsNativeExt", "Magelight_IsTradePromptOpen" },
			{ "debt", "SeverActionsNativeExt2", "Magelight_IsDebtPromptOpen" },
			{ "arrest", "SeverActionsNative", "Magelight_IsArrestPromptOpen" },
			{ "bounty", "SeverActionsNativeExt", "Magelight_IsBountyPromptOpen" },
			{ "brawl", "SeverActionsNative", "Magelight_IsBrawlPromptOpen" },
			{ "challenge", "SeverActionsNativeExt2", "Magelight_IsChallengePromptOpen" },
			{ "commission", "SeverActionsNativeExt", "Magelight_IsCommissionPromptOpen" },
			{ "retainer", "SeverActionsNativeExt", "Magelight_IsRetainerAssignPromptOpen" },
		} };
		constexpr auto kOtherRemembered = 60s;  // seen up this recently: it asked about this exchange

		std::mutex g_otherLock;
		std::unordered_map<std::string, Clock::time_point> g_otherSeen;  // kind -> last seen open
		Clock::time_point g_watchUntil{};
		bool g_watching = false;
		bool g_otherMissing = false;  // SeverActions' natives aren't there: nothing to watch

		// Game thread: asks each of them whether it is up now.
		void PollOthers()
		{
			for (auto& p : kOtherPrompts) {
				const std::string kind = p.kind;
				const bool called = Papyrus::CallStatic(p.script, p.fn, [kind](const RE::BSScript::Variable& v) {
					if (v.IsBool() && v.GetBool()) {
						std::scoped_lock l(g_otherLock);
						auto it = g_otherSeen.find(kind);
						const bool fresh = it == g_otherSeen.end() || Clock::now() - it->second > 5s;
						g_otherSeen[kind] = Clock::now();
						if (fresh) {
							logger::info("confirm popup: SeverActions' {} popup is up", kind);
						}
					}
				});
				if (!called) {
					std::scoped_lock l(g_otherLock);
					if (!g_otherMissing) {
						logger::info("confirm popup: SeverActions' {} could not be asked; its popups aren't watched", p.fn);
					}
					g_otherMissing = true;
					return;
				}
			}
		}

		void Watch(Clock::duration forHowLong)
		{
			std::scoped_lock l(g_otherLock);
			if (g_otherMissing) {
				return;
			}
			g_watchUntil = (std::max)(g_watchUntil, Clock::now() + forHowLong);
			if (g_watching) {
				return;
			}
			g_watching = true;
			std::thread([] {
				for (;;) {
					SKSE::GetTaskInterface()->AddTask(PollOthers);
					std::this_thread::sleep_for(600ms);
					std::scoped_lock l(g_otherLock);
					if (Clock::now() > g_watchUntil || g_otherMissing) {
						g_watching = false;
						return;
					}
				}
			}).detach();
		}

		// One of `kinds` was up within the last minute (empty: none).
		std::string OtherAsked(const std::vector<std::string>& kinds)
		{
			std::scoped_lock l(g_otherLock);
			for (auto& k : kinds) {
				if (auto it = g_otherSeen.find(k); it != g_otherSeen.end() && Clock::now() - it->second < kOtherRemembered) {
					return k;
				}
			}
			return {};
		}

		// Any of them up right now (seen in the last couple of polls).
		bool OtherUpNow()
		{
			std::scoped_lock l(g_otherLock);
			return std::ranges::any_of(g_otherSeen, [](auto& kv) { return Clock::now() - kv.second < 1500ms; });
		}

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
			// Picker: "pick:<formid hex>:<qty>" (accept) or "alt:<formid hex>:<qty>" (the third button)
			if (choice.starts_with("pick:") || choice.starts_with("alt:")) {
				const bool alt = choice.starts_with("alt:");
				RE::FormID id = 0;
				std::int32_t qty = 1;
				try {
					const auto rest = choice.substr(alt ? 4 : 5);
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
				Finish(ticket, !id ? Choice::kDecline : alt ? Choice::kAlt : Choice::kAccept);
				return;
			}
			bool silent = false;
			{
				std::scoped_lock l(g_lock);
				silent = g_silentAlt;
			}
			// "Refuse silently": a no, with nothing said in the player's name (what callers do on a timeout).
			Finish(ticket, choice == "accept" ? Choice::kAccept : choice == "decline" ? Choice::kDecline : choice == "alt" && !silent ? Choice::kAlt : Choice::kTimeout);
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

	void WatchOthers()
	{
		Watch(40s);
	}

	bool OtherAskedRecently(const std::string& kind)
	{
		return !OtherAsked({ kind }).empty();
	}

	bool Ask(const Offer& offer, std::function<void(Choice)> done)
	{
		if (!Available()) {
			return false;
		}
		if (const auto other = OtherAsked(offer.sameAs); !other.empty()) {
			logger::info("confirm popup: '{}' for {} not shown; SeverActions' {} popup asked this already", offer.title, offer.npc, other);
			SKSE::GetTaskInterface()->AddTask([done = std::move(done)] { done(Choice::kTimeout); });
			return true;
		}
		Watch(5s);
		{
			std::scoped_lock l(g_lock);
			// The same popup for the same person, open or waiting (the request repeated, or two mods'
			// lines leading to it): asked once.
			const auto key = KeyOf(offer);
			if ((g_done && g_openKey == key) || std::ranges::any_of(g_queue, [&](const Queued& q) { return KeyOf(q.offer) == key; })) {
				logger::info("confirm popup: '{}' for {} is already up; not asked again", offer.title, offer.npc);
				return true;
			}
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
			g_openKey = KeyOf(offer);
			g_silentAlt = offer.pick.empty() && offer.altLabel.empty() && offer.silentRefuse;
		}
		const bool silentAlt = offer.pick.empty() && offer.altLabel.empty() && offer.silentRefuse;

		json data{
			{ "title", offer.title },
			{ "npc", offer.npc },
			{ "detail", offer.detail },
			{ "price", offer.price },
			{ "gold", offer.playerGold },
			{ "seconds", offer.seconds },
			{ "showPrice", offer.showPrice },
			{ "receive", offer.receive },
			{ "hint", offer.hint },
			{ "accept", offer.acceptLabel.empty() ? "Accept" : offer.acceptLabel },
			{ "decline", offer.declineLabel.empty() ? "Decline" : offer.declineLabel },
			{ "alt", silentAlt ? std::string("Refuse silently") : offer.altLabel },
			{ "chosen", offer.chosenText.empty() ? "Giving: " : offer.chosenText },
			{ "items", json::array() }
		};
		for (auto& it : offer.pick) {
			data["items"].push_back({ { "id", std::format("{:X}", it.id) }, { "name", it.name }, { "count", it.count },
				{ "value", it.value }, { "equipped", it.equipped }, { "note", it.note } });
		}
		const auto script = std::format("mundaneMattersShow({})", data.dump());
		const auto seconds = offer.seconds;
		const auto sameAs = offer.sameAs;
		const auto what = offer.title;
		Watch(5s);
		// Busy (typing in the chat box, a menu, a loading screen): the popup waits, its timer not yet
		// started, until the player is free. Up to a minute; then it expires unanswered.
		std::thread([ticket, script, seconds, sameAs, what] {
			const auto giveUp = std::chrono::steady_clock::now() + 60s;
			// A first look at SeverActions' popups before showing anything.
			std::this_thread::sleep_for(700ms);
			for (;;) {
				// SeverActions asked the same thing meanwhile: the player answers that one.
				if (const auto other = OtherAsked(sameAs); !other.empty()) {
					logger::info("confirm popup: '{}' stood down; SeverActions' {} popup asked this already", what, other);
					Finish(ticket, Choice::kTimeout);
					return;
				}
				auto busy = std::make_shared<std::atomic_int>(-1);
				SKSE::GetTaskInterface()->AddTask([busy] { *busy = PlayerBusy() || OtherUpNow() ? 1 : 0; });
				for (int i = 0; i < 100 && *busy < 0; ++i) {
					std::this_thread::sleep_for(20ms);
				}
				Watch(2s);
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
