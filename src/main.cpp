#include "Bio.h"
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
	void OnMessage(SKSE::MessagingInterface::Message* msg)
	{
		switch (msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
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
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
	SKSE::Init(skse);
	MundaneMatters::Log::Init();

	logger::info("MundaneMatters {} loading (game {})", "0.2.0", REL::Module::get().version().string());

	// Co-save callbacks must be registered during plugin load.
	MundaneMatters::Ledger::Install();

	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		logger::critical("could not register SKSE messaging listener");
		return false;
	}
	return true;
}
