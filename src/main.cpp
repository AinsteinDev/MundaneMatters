#include "Bio.h"
#include "Game.h"
#include "Bonds.h"
#include "Brawls.h"
#include "Care.h"
#include "Crime.h"
#include "Confirm.h"
#include "Keepsakes.h"
#include "Loans.h"
#include "Pledges.h"
#include "Moments.h"
#include "Trade.h"
#include "Ledger.h"
#include "Services.h"
#include "Log.h"
#include "Promises.h"
#include "Rest.h"
#include "Safekeeping.h"
#include "Trust.h"
#include "SkyrimNetBridge.h"
#include "TurnPipeline.h"

namespace
{
	void WelcomeOnce();  // below
}

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* msg)
	{
		switch (msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			MundaneMatters::Game::NoteMainThread();
			// SkyrimNet registers its exports during its own load; kDataLoaded is the documented hook.
			if (MundaneMatters::SkyrimNet::Init()) {
				MundaneMatters::TurnPipeline::Install();
				MundaneMatters::Promises::Install();
				MundaneMatters::Bio::Install();
				MundaneMatters::Rest::Install();
				MundaneMatters::Safekeeping::Install();
				MundaneMatters::Trust::Install();
				MundaneMatters::Bonds::Install();
				MundaneMatters::Loans::Install();
				MundaneMatters::Pledges::Install();
				MundaneMatters::Brawls::Install();
				MundaneMatters::Keepsakes::Install();
				MundaneMatters::Confirm::Init();
			}
			break;
		case SKSE::MessagingInterface::kSaveGame:
			MundaneMatters::Ledger::SaveStarted();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			// The moment being loaded never had these popups, lines or pending handovers.
			MundaneMatters::Confirm::Reset();
			MundaneMatters::TurnPipeline::Reset();
			MundaneMatters::Promises::Reset();
			MundaneMatters::Trade::Reset();
			MundaneMatters::Care::Reset();
			MundaneMatters::Crime::Reset();
			MundaneMatters::Moments::Reset();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			MundaneMatters::Services::OnGameLoaded();
			[[fallthrough]];
		case SKSE::MessagingInterface::kNewGame:
			logger::info("game ready (SkyrimNet memory ready: {})", MundaneMatters::SkyrimNet::IsMemoryReady());
			SKSE::GetTaskInterface()->AddTask([] { MundaneMatters::Ledger::ApplyGrants(); });
			WelcomeOnce();
			break;
		default:
			break;
		}
	}
}

namespace
{
	constexpr const char* kVersion = "0.2.1";

	// Once per install or update (not per save): where to pick the AI model and switch features off.
	// Remembered in a small file next to the log; "Remind me next time" leaves it unwritten.
	void WelcomeOnce()
	{
		auto dir = SKSE::log::log_directory();
		if (!dir) {
			return;
		}
		const auto file = *dir / "MundaneMatters_welcome.txt";
		{
			std::ifstream in(file);
			std::string seen;
			if (in && std::getline(in, seen) && seen == kVersion) {
				return;
			}
		}
		std::thread([file] {
			std::this_thread::sleep_for(std::chrono::seconds(10));  // the HUD up, loading screens gone
			SKSE::GetTaskInterface()->AddTask([file] {
				MundaneMatters::Confirm::Offer offer;
				offer.title = std::format("Mundane Matters {} installed", kVersion);
				offer.npc = "Welcome";
				offer.detail =
					"NPCs now do what they say: rooms, gifts, places to stay, meetings, promises.\n\n"
					"AI model: SkyrimNet > AI Models (near the bottom) > Mundane Matters. "
					"Pick any model you like; the default is DeepSeek V4 Flash.\n\n"
					"Features: SkyrimNet > Settings > Plugins > MundaneMatters. "
					"Switch off anything you don't want, and set the Promises Book Key there to a key you don't use "
					"(the default P may clash with other mods).\n\n"
					"Notices starting with MM_ come from this mod. Bugs: the Mundane Matters channel on the SkyrimNet Discord.";
				offer.showPrice = false;
				offer.seconds = 0;
				offer.silentRefuse = false;
				offer.acceptLabel = "Got it";
				offer.declineLabel = "Remind me next time";
				const bool shown = MundaneMatters::Confirm::Ask(offer, [file](MundaneMatters::Confirm::Choice choice) {
					if (choice != MundaneMatters::Confirm::Choice::kAccept) {
						return;
					}
					std::ofstream out(file, std::ios::trunc);
					out << kVersion << "\n";
					logger::info("welcome: shown for {}; won't show again for this version", kVersion);
				});
				if (!shown) {
					logger::info("welcome: popup unavailable (PrismaUI?); shown next time");
				}
			});
		}).detach();
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
	SKSE::Init(skse);
	MundaneMatters::Log::Init();

	logger::info("MundaneMatters {} loading (game {})", kVersion, REL::Module::get().version().string());

	// Co-save callbacks must be registered during plugin load.
	MundaneMatters::Ledger::Install();

	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		logger::critical("could not register SKSE messaging listener");
		return false;
	}
	return true;
}
