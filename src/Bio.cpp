#include "Bio.h"

#include "Brawls.h"
#include "VanillaLines.h"
#include "Crime.h"
#include "Game.h"
#include "Ledger.h"
#include "Keys.h"
#include "Loans.h"
#include "Pledges.h"
#include "Moments.h"
#include "MoveIn.h"
#include "Safekeeping.h"
#include "Trade.h"
#include "Trust.h"
#include "Promises.h"
#include "Services.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Bio
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr float kRange = 4000.0f;
		constexpr auto kRefreshEvery = 10s;

		std::mutex g_lock;
		std::unordered_map<RE::FormID, std::string> g_facts;  // NPC -> service facts
		Clock::time_point g_lastRefresh;

		std::string RoomFacts(RE::Actor* npc)
		{
			if (!SkyrimNet::ConfigBool("features.rent_room", true)) {
				return "";
			}
			auto room = Services::RoomOf(npc);
			if (!room || !room->bed) {
				return "";
			}
			const std::string player = Game::Player()->GetName();
			if (room->rentedByPlayer) {
				return std::format("{} has already paid for the room tonight: it is theirs, nothing more to arrange. "
				                   "Inn rooms here have no key; the room is simply open to them.", player);
			}
			return std::format("Rents out a room for {} gold a night (the real price; quote this one). "
			                   "Inn rooms here have no key: once paid, the room is simply open to the guest.", room->cost);
		}

		// Food and drink the merchant really stocks, with real prices, so they stop inventing them.
		std::string PriceFacts(RE::Actor* npc)
		{
			auto* faction = npc->GetVendorFaction();
			auto* chest = faction ? faction->vendorData.merchantContainer : nullptr;
			if (!chest) {
				return "";
			}
			std::vector<std::pair<std::string, std::int32_t>> foods;
			for (auto& [obj, count] : chest->GetInventoryCounts()) {
				auto* food = obj ? obj->As<RE::AlchemyItem>() : nullptr;
				if (count > 0 && food && food->IsFood() && food->GetName() && *food->GetName()) {
					foods.emplace_back(food->GetName(), Trade::BuyPrice(obj));
				}
			}
			if (foods.empty()) {
				return "";
			}
			std::ranges::sort(foods, [](auto& a, auto& b) { return a.second < b.second; });
			// The 12 cheapest keep the bio short; the full list is in the trade window.
			if (foods.size() > 12) {
				foods.resize(12);
			}
			std::string list;
			for (auto& [name, price] : foods) {
				list += std::format("{}{} {} gold", list.empty() ? "" : ", ", name, price);
			}
			return std::format("Food and drink for sale, at what {} would really pay: {}. Quote these prices; anything else is in the trade window.",
				Game::Player()->GetName(), list);
		}

		// So they say no themselves ("Whiterun? That's days away") instead of agreeing and backing out.
		std::string EscortFacts(RE::Actor* npc)
		{
			if (!SkyrimNet::ConfigBool("features.escort", true) || !SkyrimNet::ConfigBool("features.escort_local_only", true) ||
				Services::IsCompanion(npc)) {
				return "";
			}
			auto* hold = Services::HoldOf(npc);
			if (!hold || !hold->GetName() || !*hold->GetName()) {
				return "";
			}
			return std::format("Will show people the way around {}, but won't escort anyone to another hold (days of travel) unless already going there.",
				hold->GetName());
		}

		// Recent generosity, so they don't hand out food or coin every time they're asked. Built
		// here, on the game thread (it reads relationship data), and cached like the rest.
		std::string GenerosityFacts(RE::Actor* npc)
		{
			const auto g = Promises::GenerosityOf(npc);
			const std::string player = Game::Player()->GetName();
			if (g.never) {
				return std::format("Gives {} nothing for free.", player);
			}
			auto window = Ledger::CurrentHandouts(npc->GetFormID());
			if (!window || window->since >= g.hours) {
				return "";  // nothing given lately: generous as their nature allows
			}
			const auto left = Ledger::BudgetLeft(npc->GetFormID(), g);
			const auto ago = window->since < 1.0f ? "just now" : window->since < 12.0f ? "earlier today" : "yesterday";
			const float rest = g.hours - window->since;
			const auto until = rest <= 3.0f ? "a little later" : rest <= 12.0f ? "later today" : "tomorrow";
			if (left < 2) {
				return std::format("Already gave {} {} {}. Must NOT give {} any more food, coin or items for free until {}; if asked, decline kindly in their own words.",
					player, window->what, ago, player, until);
			}
			return std::format("Already gave {} {} {}. Can spare at most about {} gold worth more for free until {} (food counts half: a bowl of stew is about 4); anything more, decline kindly.",
				player, window->what, ago, left, until);
		}

		std::string Facts(RE::Actor* npc)
		{
			std::string out;
			for (auto&& f : { RoomFacts(npc), PriceFacts(npc), EscortFacts(npc), GenerosityFacts(npc), Crime::GuardFacts(npc) }) {
				if (!f.empty()) {
					out += (out.empty() ? "" : "\n- ") + f;
				}
			}
			return out;
		}

		std::string Decorate(RE::Actor* npc)
		{
			if (!npc) {
				return "";
			}
			const auto id = npc->GetFormID();
			std::vector<std::string> lines;
			{
				std::scoped_lock l(g_lock);
				if (auto it = g_facts.find(id); it != g_facts.end() && !it->second.empty()) {
					lines.push_back(it->second);
				}
			}
			for (auto& l : Ledger::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			for (auto& l : Safekeeping::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			for (auto& l : Keys::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			for (auto& l : Loans::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			for (auto& l : Moments::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			for (auto& l : Pledges::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			for (auto& l : Brawls::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			for (auto& l : VanillaLines::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			for (auto& l : Trust::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			for (auto& l : MoveIn::BioLines(id)) {
				lines.push_back(std::move(l));
			}
			// Arranging to meet: the real places nearby, so they don't invent "the east ridge".
			if (auto m = Services::MeetPlacesLine(id); !m.empty()) {
				lines.push_back(std::move(m));
			}
			// Going home together: whose home is near, and whether they'd go at all.
			if (auto v = Services::VisitLine(id); !v.empty()) {
				lines.push_back(std::move(v));
			}
			// A price the player just asked about, looked up in the real stock.
			if (auto q = Trade::QuoteLine(id); !q.empty()) {
				lines.push_back(std::move(q));
			}
			std::string out;
			for (auto& l : lines) {
				out += "- " + l + "\n";
			}
			return out;
		}
	}

	void Install()
	{
		SkyrimNet::RegisterDecorator("mundanematters_bio", "Mundane Matters: the NPC's real services, open promises and lent bed (markdown list, empty when none)", Decorate);
	}

	void Invalidate()
	{
		g_lastRefresh = {};
		std::scoped_lock l(g_lock);
		g_facts.clear();  // a loaded save is another timeline: nothing cached carries over
	}

	void RefreshNearby()
	{
		const auto now = Clock::now();
		if (now - g_lastRefresh < kRefreshEvery) {
			return;
		}
		g_lastRefresh = now;

		std::unordered_map<RE::FormID, std::string> facts;
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return;
		}
		for (auto& handle : lists->highActorHandles) {
			auto actor = handle.get();
			if (actor && Game::DistanceToPlayer(actor.get()) < kRange) {
				facts[actor->GetFormID()] = Facts(actor.get());
			}
		}
		// Replaced whole, never merged: someone who walked out of range (or whose room, prices or
		// escort facts changed) must not keep yesterday's facts in their prompt.
		std::scoped_lock l(g_lock);
		g_facts = std::move(facts);
	}
}
