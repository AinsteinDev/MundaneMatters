#include "Safekeeping.h"
#include "Services.h"

#include "Bonds.h"
#include "Confirm.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Loans.h"
#include "Papyrus.h"
#include "Promises.h"
#include "Rest.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Safekeeping
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct Item
		{
			std::int32_t count = 0;
			std::string name;  // kept for the bio, which is read off the game thread
		};

		// What a friend keeps for the player, in a hidden chest of theirs.
		struct Kept
		{
			RE::FormID chest = 0;
			std::map<RE::FormID, Item> items;
		};

		std::mutex g_lock;
		std::unordered_map<RE::FormID, Kept> g_kept;

		// The follower window, while it is open: who, and what the player carried before.
		RE::FormID g_windowNpc = 0;
		Clock::time_point g_windowOpened;
		std::unordered_map<RE::TESBoundObject*, std::int32_t> g_snapshot;

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

		bool IsGear(RE::TESBoundObject* item)
		{
			return item && (item->IsWeapon() || item->IsArmor() || item->Is(RE::FormType::Ammo));
		}

		// "Hold this sword": one clear item. Several close matches ("potion" with three kinds)
		// is vague, unless exactly one of them is in hand ("this sword" = the one equipped).
		std::optional<Promises::Found> Single(const std::vector<Promises::Found>& found)
		{
			if (found.empty()) {
				return std::nullopt;
			}
			std::vector<Promises::Found> close;
			for (auto& f : found) {
				if (f.score > found.front().score - 0.75) {
					close.push_back(f);
				}
			}
			if (close.size() == 1) {
				return close.front();
			}
			std::vector<Promises::Found> worn;
			for (auto& f : close) {
				if (f.worn) {
					worn.push_back(f);
				}
			}
			if (worn.size() == 1) {
				return worn.front();
			}
			return std::nullopt;
		}

		// The amount said ("100 gold", "two potions"); otherwise a piece of gear is one and anything
		// else is the whole stack ("hold my potions").
		std::int32_t QtyOf(const Promises::Found& f, const std::string& said = "")
		{
			if (const auto n = JsonUtil::ParseNumber(said); n && *n >= 1.0) {
				return (std::min)(static_cast<std::int32_t>(*n), f.count);
			}
			return IsGear(f.item) ? 1 : f.count;
		}

		// Moves items now (not queued in Papyrus), so the picker's next list and the record match
		// what really moved.
		void Move(RE::TESObjectREFR* from, RE::TESBoundObject* item, std::int32_t n, RE::TESObjectREFR* to)
		{
			n = (std::min)(n, Game::ItemCount(from, item));
			if (n > 0) {
				from->RemoveItem(item, n, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, to);
			}
		}

		// A plain, never-respawning chest from the base game.
		RE::TESObjectCONT* ChestBase()
		{
			static RE::TESObjectCONT* cached = nullptr;
			static bool searched = false;
			if (searched) {
				return cached;
			}
			searched = true;
			for (auto* c : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESObjectCONT>()) {
				auto* file = c ? c->GetFile(0) : nullptr;
				// Empty by design: a loot chest's contents would end up in the player's hands.
				if (!c || !file || std::string_view(file->GetFilename()) != "Skyrim.esm" || c->data.flags.any(RE::CONT_DATA::Flag::kRespawn) ||
					c->numContainerObjects != 0) {
					continue;
				}
				auto* model = c->As<RE::TESModel>();
				const auto path = JsonUtil::Lower(model && model->GetModel() ? model->GetModel() : "");
				if (path.find("chest") != std::string::npos) {
					cached = c;
					break;
				}
			}
			logger::info("hold: storage chest base {}", cached ? std::format("{:08X}", cached->GetFormID()) : "not found");
			return cached;
		}

		// The friend's hidden chest: placed once, persistent and disabled, so it never resets and
		// nobody sees it.
		RE::TESObjectREFR* ChestOf(RE::Actor* npc)
		{
			{
				std::scoped_lock l(g_lock);
				if (auto it = g_kept.find(npc->GetFormID()); it != g_kept.end() && it->second.chest) {
					if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(it->second.chest)) {
						return ref;
					}
				}
			}
			auto* base = ChestBase();
			if (!base) {
				return nullptr;
			}
			auto ref = Game::Player()->PlaceObjectAtMe(base, true);
			if (!ref) {
				return nullptr;
			}
			Papyrus::CallMethod(ref.get(), "ObjectReference", "Disable", nullptr, false);
			{
				std::scoped_lock l(g_lock);
				g_kept[npc->GetFormID()].chest = ref->GetFormID();
			}
			logger::info("hold: storage {:08X} for {}", ref->GetFormID(), Game::NameOf(npc));
			return ref.get();
		}

		void Record(RE::FormID npc, RE::TESBoundObject* item, std::int32_t n)
		{
			std::scoped_lock l(g_lock);
			auto& it = g_kept[npc].items[item->GetFormID()];
			it.count += n;
			it.name = item->GetName();
		}

		std::string ListOf(const std::vector<std::pair<RE::TESBoundObject*, std::int32_t>>& moved)
		{
			std::string out;
			for (auto& [item, n] : moved) {
				out += (out.empty() ? "" : ", ") + (item == Game::Gold() ? std::format("{} gold", n) : Promises::Counted(item->GetName(), n));
			}
			return out;
		}

		// Hands it over now: into a companion's pack, or a friend's safe keeping.
		void HandOver(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t n)
		{
			auto* player = Game::Player();
			n = (std::min)(n, Game::ItemCount(player, item));
			if (n <= 0) {
				return;
			}
			if (Services::IsCompanion(npc)) {
				Move(player, item, n, npc);
				return;
			}
			if (auto* chest = ChestOf(npc)) {
				const auto before = Game::ItemCount(chest, item);
				Move(player, item, n, chest);
				if (const auto moved = Game::ItemCount(chest, item) - before; moved > 0) {
					Record(npc->GetFormID(), item, moved);
				}
			}
		}

		void Told(RE::Actor* npc, const std::string& what)
		{
			Game::Notify(std::format("{} is keeping: {}", Game::NameOf(npc), what));
			Game::Narrate(std::format("{} handed {} {} to keep safe for them, and {} put it away.", PlayerName(), Game::NameOf(npc), what, Game::NameOf(npc)), npc);
			logger::info("hold: {} keeps {}", Game::NameOf(npc), what);
		}

		// One clear item: a popup shows exactly what leaves the pack.
		void ConfirmHandOver(RE::Actor* npc, Promises::Found f, const std::string& said)
		{
			const auto npcId = npc->GetFormID();
			const auto n = QtyOf(f, said);
			const auto what = Promises::Counted(f.item->GetName(), n);
			auto give = [npcId, item = f.item, n, what] {
				if (auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId)) {
					HandOver(npc, item, n);
					Told(npc, what);
				}
			};
			if (!SkyrimNet::ConfigBool("features.confirm_hold", true) || !Confirm::Available()) {
				give();
				return;
			}
			const std::string accept = Game::Pick({ "Keep this safe for me.", "Hold on to this for me.", "Look after this, will you?" });
			const std::string decline = Game::Pick({ "Actually, I'll hang on to it.", "On second thought, I'll keep it.", "Never mind." });
			Confirm::Offer offer;
			offer.title = "Hand over";
			offer.npc = Game::NameOf(npc);
			offer.detail = std::format("{}{} (they keep it for you)", what, f.worn ? " (equipped)" : "");
			offer.showPrice = false;
			offer.seconds = 0;
			offer.acceptLabel = accept;
			offer.declineLabel = decline;
			Confirm::Ask(offer, [npcId, give, accept, decline](Confirm::Choice choice) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || choice == Confirm::Choice::kTimeout) {
					return;
				}
				if (choice == Confirm::Choice::kDecline) {
					Game::Say(decline, npc, true);
					return;
				}
				Game::Say(accept, npc, false);
				give();
			});
		}

		// A friend: the player picks from their pack, one thing after another, until "That's all".
		// `filter` narrows the list to what was named ("potion").
		bool Picker(RE::FormID npcId, std::string filter, std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> given)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc || !Confirm::Available()) {
				return false;
			}
			auto* player = Game::Player();
			Confirm::Offer offer;
			offer.title = "Hold";
			offer.npc = Game::NameOf(npc);
			offer.detail = given.empty() ? std::format("What should {} keep for you?", Game::NameOf(npc)) : "Anything else?";
			offer.showPrice = false;
			offer.seconds = 0;
			offer.acceptLabel = "Hand it over";
			offer.declineLabel = given.empty() ? "Never mind" : "That's all";
			auto collect = [&](bool filtered) {
				offer.pick.clear();
				for (auto& [obj, entry] : player->GetInventory()) {
					if (!obj || entry.first <= 0 || !obj->GetName() || !*obj->GetName() || (entry.second && entry.second->IsQuestObject())) {
						continue;
					}
					if (filtered && Promises::NameScore(filter, obj->GetName()) <= 0.0) {
						continue;
					}
					offer.pick.push_back({ obj->GetFormID(), obj->GetName(), entry.first, obj == Game::Gold() ? 1 : obj->GetGoldValue(),
						entry.second && entry.second->IsWorn() });
				}
			};
			collect(!filter.empty());
			if (offer.pick.empty() && !filter.empty()) {
				collect(false);  // nothing like that: show everything
			}
			if (offer.pick.empty()) {
				return false;
			}
			// Gold first, then carcasses (what "hold this" usually means out hunting), then by value.
			const auto carcass = [](const auto& e) { return JsonUtil::Lower(e.name).find("carcass") != std::string::npos; };
			std::ranges::sort(offer.pick, [&](auto& a, auto& b) {
				const bool ag = a.id == 0xF, bg = b.id == 0xF;
				if (ag != bg) {
					return ag;
				}
				const bool ac = carcass(a), bc = carcass(b);
				return ac != bc ? ac : a.value > b.value;
			});
			if (offer.pick.size() > 60) {
				offer.pick.resize(60);
			}
			return Confirm::Ask(offer, [npcId, filter, given](Confirm::Choice choice) mutable {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || choice == Confirm::Choice::kTimeout) {
					return;
				}
				if (choice == Confirm::Choice::kDecline) {
					if (given.empty()) {
						Game::Say(Game::Pick({ "Actually, never mind.", "On second thought, I'll keep my things." }), npc, true);
					} else {
						Told(npc, ListOf(given));
					}
					return;
				}
				const auto [id, qty] = Confirm::LastPick();
				auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
				const auto n = item ? (std::min)(qty, Game::ItemCount(Game::Player(), item)) : 0;
				if (n > 0) {
					HandOver(npc, item, n);
					given.emplace_back(item, n);
				}
				// Next pick; the list is read again so what's handed over is gone from it.
				SKSE::GetTaskInterface()->AddTask([npcId, filter, given] {
					if (!Picker(npcId, filter, given) && !given.empty()) {
						if (auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId)) {
							Told(npc, ListOf(given));
						}
					}
				});
			});
		}

		// A companion: the vanilla follower window, which moves items itself. What changed hands is
		// worked out when it closes, so the NPC hears about it.
		void OpenFollowerWindow(RE::Actor* npc)
		{
			g_windowNpc = npc->GetFormID();
			g_windowOpened = Clock::now();
			g_snapshot.clear();
			for (auto& [obj, count] : Game::Player()->GetInventoryCounts()) {
				if (obj && count > 0) {
					g_snapshot[obj] = count;
				}
			}
			logger::info("hold: opening {}'s pack", Game::NameOf(npc));
			Papyrus::CallMethod(npc, "Actor", "OpenInventory", nullptr, true);
		}

		void OnWindowClosed()
		{
			const auto npcId = std::exchange(g_windowNpc, 0);
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc || Clock::now() - g_windowOpened > 15min) {
				return;
			}
			std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> gave, took;
			auto now = Game::Player()->GetInventoryCounts();
			for (auto& [obj, before] : g_snapshot) {
				const auto after = now.contains(obj) ? now.at(obj) : 0;
				if (after < before && Game::ItemCount(npc, obj) > 0) {
					// Something they lent, handed back through their pack: that loan is settled.
					if (const auto back = Loans::GiveBack(npc, obj, before - after); back < before - after) {
						gave.emplace_back(obj, before - after - back);
					}
				}
			}
			for (auto& [obj, after] : now) {
				const auto before = g_snapshot.contains(obj) ? g_snapshot.at(obj) : 0;
				if (obj && after > before) {
					took.emplace_back(obj, after - before);
					Loans::TookBack(npcId, obj, after - before);
				}
			}
			g_snapshot.clear();
			if (gave.empty() && took.empty()) {
				logger::info("hold: {}'s pack closed, nothing changed hands", Game::NameOf(npc));
				return;
			}
			std::string what;
			if (!gave.empty()) {
				what = std::format("handed {} {} to carry", Game::NameOf(npc), ListOf(gave));
			}
			if (!took.empty()) {
				what += std::format("{}took back {}", what.empty() ? "" : " and ", ListOf(took));
			}
			logger::info("hold: {} {}", PlayerName(), what);
			Game::Narrate(std::format("{} {}.", PlayerName(), what), npc);
		}

		struct WindowSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (e && !e->opening && e->menuName == RE::ContainerMenu::MENU_NAME && g_windowNpc) {
					std::thread([] {
						std::this_thread::sleep_for(800ms);
						SKSE::GetTaskInterface()->AddTask(OnWindowClosed);
					}).detach();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	bool Hold(RE::Actor* npc, const std::string& item)
	{
		if (!npc) {
			return false;
		}
		const std::string name = Game::NameOf(npc);
		// They agreed to it in their reply: the relationship doesn't overrule a yes (a hunting partner
		// they barely know still carries the carcass). The things stay the player's, kept on record.
		auto* player = Game::Player();
		if (!item.empty()) {
			const auto found = Promises::FindItems(player, item);
			if (found.empty()) {
				Game::Narrate(std::format("{} looked for {} to hand {}, but didn't have any.", PlayerName(), item, name), npc);
				return true;
			}
			auto one = Single(found);
			// Coin with no amount said: the player picks how much rather than handing over the purse.
			if (one && one->item == Game::Gold() && !JsonUtil::ParseNumber(item)) {
				logger::info("hold: gold with no amount; letting the player choose");
				one.reset();
			}
			if (one) {
				ConfirmHandOver(npc, *one, item);
				return true;
			}
			logger::info("hold: '{}' matches {} things; letting the player choose", item, found.size());
		}
		if (Services::IsCompanion(npc)) {
			OpenFollowerWindow(npc);
			return true;
		}
		if (!Picker(npc->GetFormID(), item, {})) {
			Game::Narrate(std::format("{} waited for {} to hand something over, but nothing came.", name, PlayerName()), npc);
		}
		return true;
	}

	bool Retrieve(RE::Actor* npc, const std::string& item)
	{
		if (!npc) {
			return false;
		}
		auto* player = Game::Player();
		const std::string name = Game::NameOf(npc);
		if (Services::IsCompanion(npc)) {
			if (!item.empty()) {
				const auto found = Promises::FindItems(npc, item);
				if (found.empty()) {
					Game::Narrate(std::format("{} checked their pack, but had no {} of {}'s.", name, item, PlayerName()), npc);
					return true;
				}
				if (auto one = Single(found)) {
					const auto n = QtyOf(*one, item);
					Move(npc, one->item, n, player);
					Loans::TookBack(npc->GetFormID(), one->item, n);
					const auto what = Promises::Counted(one->item->GetName(), n);
					Game::Notify(std::format("{} gave back: {}", name, what));
					Game::Narrate(std::format("{} handed {} back to {}.", name, what, PlayerName()), npc);
					Bonds::Add(npc, Bonds::Deed::kHeld, 1);
					return true;
				}
			}
			OpenFollowerWindow(npc);
			return true;
		}

		// Things lent to them to use come back from their own pack.
		const bool lentBack = Loans::Retrieve(npc, item);
		Kept kept;
		{
			std::scoped_lock l(g_lock);
			if (auto it = g_kept.find(npc->GetFormID()); it != g_kept.end()) {
				kept = it->second;
			}
		}
		auto* chest = kept.chest ? RE::TESForm::LookupByID<RE::TESObjectREFR>(kept.chest) : nullptr;
		if (!chest || kept.items.empty()) {
			if (!lentBack) {
				Game::Narrate(std::format("{} reminded {} they weren't keeping anything for them.", name, PlayerName()), npc);
			}
			return true;
		}
		std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> back;
		for (auto& [id, it] : kept.items) {
			auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
			if (obj && (item.empty() || Promises::NameScore(item, it.name.c_str()) > 0.0)) {
				back.emplace_back(obj, (std::min)(it.count, Game::ItemCount(chest, obj)));
			}
		}
		std::erase_if(back, [](auto& b) { return b.second <= 0; });
		if (back.empty() && lentBack) {
			return true;
		}
		if (back.empty()) {
			Game::Narrate(std::format("{} wasn't keeping any {} for {}.", name, item, PlayerName()), npc);
			return true;
		}
		for (auto& [obj, n] : back) {
			Move(chest, obj, n, player);
		}
		{
			std::scoped_lock l(g_lock);
			auto& items = g_kept[npc->GetFormID()].items;
			for (auto& [obj, n] : back) {
				if (auto it = items.find(obj->GetFormID()); it != items.end() && (it->second.count -= n) <= 0) {
					items.erase(it);
				}
			}
		}
		const auto what = ListOf(back);
		Game::Notify(std::format("{} gave back: {}", name, what));
		Game::Narrate(std::format("{} fetched {} and handed it back to {}.", name, what, PlayerName()), npc);
		logger::info("hold: {} gave back {}", name, what);
		Bonds::Add(npc, Bonds::Deed::kHeld, 1);
		return true;
	}

	bool IsHolding(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		auto it = g_kept.find(npc);
		return it != g_kept.end() && !it->second.items.empty();
	}

	std::vector<std::string> BioLines(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		auto it = g_kept.find(npc);
		if (it == g_kept.end() || it->second.items.empty()) {
			return {};
		}
		std::string list;
		for (auto& [id, item] : it->second.items) {
			list += (list.empty() ? "" : ", ") + Promises::Counted(item.name, item.count);
		}
		return { std::format("Is keeping these safe for {}, to give back when asked: {}.", Game::Player()->GetName(), list) };
	}

	void Tick()
	{
		std::vector<std::pair<RE::FormID, Kept>> dead;
		{
			std::scoped_lock l(g_lock);
			for (auto& [npc, kept] : g_kept) {
				if (kept.items.empty()) {
					continue;
				}
				auto* a = RE::TESForm::LookupByID<RE::Actor>(npc);
				if (a && a->IsDead()) {
					dead.emplace_back(npc, kept);
				}
			}
			for (auto& d : dead) {
				g_kept.erase(d.first);
			}
		}
		for (auto& [npcId, kept] : dead) {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* chest = RE::TESForm::LookupByID<RE::TESObjectREFR>(kept.chest);
			if (npc && chest) {
				// What they kept is on their body now: the player can take it back.
				for (auto& [id, item] : kept.items) {
					if (auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(id)) {
						Move(chest, obj, item.count, npc);
					}
				}
				Game::Notify(std::format("{} is dead; your things are on their body.", Game::NameOf(npc)));
				logger::info("hold: {} died; their safekeeping moved to the body", Game::NameOf(npc));
			}
		}
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json out = json::array();
		for (auto& [npc, kept] : g_kept) {
			json items = json::array();
			for (auto& [id, item] : kept.items) {
				items.push_back({ { "id", id }, { "count", item.count }, { "name", item.name } });
			}
			out.push_back({ { "npc", npc }, { "chest", kept.chest }, { "items", items } });
		}
		return out;
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		auto id = [&](const json& o, const char* k) {
			const auto raw = static_cast<RE::FormID>(JsonUtil::Num(o, k).value_or(0));
			return raw == 0 ? RE::FormID{ 0 } : (raw >> 24) == 0xFF ? raw : resolve(raw);  // spawned refs keep their id
		};
		std::scoped_lock l(g_lock);
		g_kept.clear();
		if (!j.is_array()) {
			return;
		}
		for (auto& k : j) {
			const auto npc = id(k, "npc");
			if (!npc) {
				continue;
			}
			Kept kept{ id(k, "chest"), {} };
			if (k.contains("items") && k["items"].is_array()) {
				for (auto& it : k["items"]) {
					if (auto item = id(it, "id")) {
						kept.items[item] = { static_cast<std::int32_t>(JsonUtil::Num(it, "count").value_or(0)), JsonUtil::Str(it, "name") };
					}
				}
			}
			g_kept[npc] = std::move(kept);
		}
		logger::info("co-save: {} people keeping things for the player", g_kept.size());
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_kept.clear();
		g_windowNpc = 0;
		g_snapshot.clear();
	}

	void Install()
	{
		static WindowSink sink;
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&sink);
		}
	}
}
