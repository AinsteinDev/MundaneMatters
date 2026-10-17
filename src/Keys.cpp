#include "Keys.h"

#include "Game.h"
#include "JsonUtil.h"
#include "Ledger.h"
#include "Papyrus.h"
#include "Promises.h"
#include "Rest.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Keys
{
	namespace
	{
		constexpr float kDoorRange = 600.0f;     // a door this close to the player opens for them
		constexpr float kRelockRange = 3000.0f;  // and locks again once they're this far

		struct Holder
		{
			RE::FormID key = 0;  // the house's real key, once handed over (0 = none yet / the lock has none)
		};

		struct Opened
		{
			RE::FormID door = 0;
			RE::FormID npc = 0;
		};

		std::mutex g_lock;
		std::unordered_map<RE::FormID, Holder> g_holders;  // NPC -> their key
		std::vector<Opened> g_opened;                      // doors unlocked for the player

		// A key is for the closest bonds only: an ally or a lover, never a mere friend.
		bool CloseEnough(RE::Actor* npc)
		{
			auto* base = npc->GetActorBase();
			auto* playerBase = Game::Player()->GetActorBase();
			auto* rel = base && playerBase ? RE::BGSRelationship::GetRelationship(base, playerBase) : nullptr;
			using L = RE::BGSRelationship::RELATIONSHIP_LEVEL;
			const auto level = rel ? rel->level.get() : L::kAcquaintance;
			return level == L::kAlly || level == L::kLover;
		}

		// The cell a door leads into (the house behind an exterior door).
		RE::TESObjectCELL* LeadsTo(RE::TESObjectREFR* door)
		{
			auto* x = door->extraList.GetByType<RE::ExtraTeleport>();
			auto linked = x && x->teleportData ? x->teleportData->linkedDoor.get() : nullptr;
			return linked ? linked->GetParentCell() : nullptr;
		}

		template <class F>
		void ForEachNearbyDoor(F&& fn)
		{
			auto* player = Game::Player();
			auto visit = [&](RE::TESObjectREFR* ref) {
				auto* base = ref ? ref->GetBaseObject() : nullptr;
				if (base && base->Is(RE::FormType::Door) && !ref->IsDisabled() && ref->GetPosition().GetDistance(player->GetPosition()) < kDoorRange) {
					fn(ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			};
			auto* cell = player->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				cell->ForEachReference(visit);
			} else if (auto* tes = RE::TES::GetSingleton()) {
				tes->ForEachReferenceInRange(player, kDoorRange, visit);
			}
		}

		// The house's own key goes to the player too, when its lock has one: from the NPC's pocket
		// if they carry it, otherwise a copy.
		void HandOverRealKey(RE::Actor* npc, RE::TESObjectREFR* door)
		{
			auto* lock = door->GetLock();
			auto* key = lock ? lock->key : nullptr;
			if (!key) {
				return;
			}
			{
				std::scoped_lock l(g_lock);
				auto it = g_holders.find(npc->GetFormID());
				if (it == g_holders.end() || it->second.key) {
					return;
				}
				it->second.key = key->GetFormID();
			}
			auto* player = Game::Player();
			if (Game::ItemCount(player, key) > 0) {
				return;
			}
			if (Game::ItemCount(npc, key) > 0) {
				npc->RemoveItem(key, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
			} else {
				player->AddObjectToContainer(key, nullptr, 1, nullptr);
			}
			Game::Notify(std::format("You have {}.", key->GetName()));
			logger::info("key: {} handed over {}", npc->GetName(), key->GetName());
		}

		void Lock(RE::TESObjectREFR* door, bool locked)
		{
			// As the owner would: an open door they left open, not a door the player broke into.
			Papyrus::CallMethod(door, "ObjectReference", "Lock", nullptr, static_cast<bool>(locked), true);
		}
	}

	bool Give(RE::Actor* npc)
	{
		if (!npc || npc->IsDead()) {
			return false;
		}
		const auto id = npc->GetFormID();
		const std::string name = npc->GetName();
		const std::string player = Game::Player()->GetName();
		if (Has(id)) {
			return true;  // mentioned again; it's already on the player's ring
		}
		if (!CloseEnough(npc)) {
			Game::Notify(std::format("{} ({}): not close enough to give you a key", name, Rest::RelationName(npc)));
			Game::Narrate(std::format("{} reached for a key to their home, then hesitated: they didn't know {} well enough for that yet.", name, player), npc);
			logger::info("key: {} isn't close enough ({})", name, Rest::RelationName(npc));
			return false;
		}
		{
			std::scoped_lock l(g_lock);
			g_holders[id] = {};
		}
		// Someone who gives you their key welcomes you to stay, too.
		Ledger::AddGuest(id);
		SkyrimNet::AddMemory(id, std::format("{} gave {} a key to their home, trusting them to come and go as they please.", name, player), 0.7f);
		Game::Notify(std::format("{} gave you a key to their home.", name));
		logger::info("key: {} gave the player a key to their home", name);
		// Standing at (or inside) the house already: the real key now.
		ForEachNearbyDoor([&](RE::TESObjectREFR* door) {
			if (auto* to = LeadsTo(door); to && Promises::IsHome(to, npc)) {
				HandOverRealKey(npc, door);
			}
		});
		if (auto* cell = Game::Player()->GetParentCell(); cell && Promises::IsHome(cell, npc)) {
			ForEachNearbyDoor([&](RE::TESObjectREFR* door) {
				auto* x = door->extraList.GetByType<RE::ExtraTeleport>();
				if (auto outside = x && x->teleportData ? x->teleportData->linkedDoor.get() : nullptr) {
					HandOverRealKey(npc, outside.get());
				}
			});
		}
		return true;
	}

	void Revoke(RE::FormID npcId, bool notify)
	{
		Holder holder;
		std::vector<Opened> relock;
		{
			std::scoped_lock l(g_lock);
			auto it = g_holders.find(npcId);
			if (it == g_holders.end()) {
				return;
			}
			holder = it->second;
			g_holders.erase(it);
			std::erase_if(g_opened, [&](const Opened& o) {
				if (o.npc == npcId) {
					relock.push_back(o);
					return true;
				}
				return false;
			});
		}
		for (auto& o : relock) {
			if (auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(o.door)) {
				Lock(door, true);
			}
		}
		auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
		auto* player = Game::Player();
		if (auto* key = holder.key ? RE::TESForm::LookupByID<RE::TESBoundObject>(holder.key) : nullptr; key && Game::ItemCount(player, key) > 0) {
			player->RemoveItem(key, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, npc && !npc->IsDead() ? npc : nullptr);
		}
		if (notify) {
			Game::Notify(std::format("{} took their key back.", npc ? npc->GetName() : "They"));
		}
		logger::info("key: {:08X} took their key back", npcId);
	}

	bool Has(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		return g_holders.contains(npc);
	}

	std::vector<RE::FormID> Holders()
	{
		std::scoped_lock l(g_lock);
		std::vector<RE::FormID> out;
		for (auto& [npc, h] : g_holders) {
			out.push_back(npc);
		}
		return out;
	}

	std::vector<std::string> BioLines(RE::FormID npc)
	{
		if (!Has(npc)) {
			return {};
		}
		return { std::format("Has given {} a key to their home; {} may come and go as they please.", Game::Player()->GetName(), Game::Player()->GetName()) };
	}

	void Tick()
	{
		std::vector<RE::FormID> holders;
		{
			std::scoped_lock l(g_lock);
			if (g_holders.empty() && g_opened.empty()) {
				return;
			}
			for (auto& [npc, h] : g_holders) {
				holders.push_back(npc);
			}
		}
		auto* player = Game::Player();
		// A holder who died or turned on the player: the key means nothing now.
		for (auto id : holders) {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
			if (npc && npc->IsHostileToActor(player)) {
				Revoke(id);
			}
		}
		// Doors the player walks up to.
		ForEachNearbyDoor([&](RE::TESObjectREFR* door) {
			if (!door->IsLocked()) {
				return;
			}
			auto* to = LeadsTo(door);
			if (!to) {
				return;
			}
			for (auto id : holders) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!npc || !Promises::IsHome(to, npc)) {
					continue;
				}
				Lock(door, false);
				HandOverRealKey(npc, door);
				{
					std::scoped_lock l(g_lock);
					if (std::ranges::none_of(g_opened, [&](const Opened& o) { return o.door == door->GetFormID(); })) {
						g_opened.push_back({ door->GetFormID(), id });
					}
				}
				logger::info("key: {}'s door opens for the player", npc->GetName());
				break;
			}
		});
		// Doors left behind lock again, the way the household keeps them.
		std::vector<RE::FormID> relock;
		{
			std::scoped_lock l(g_lock);
			std::erase_if(g_opened, [&](const Opened& o) {
				auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(o.door);
				if (!door) {
					return true;
				}
				auto* to = LeadsTo(door);
				const bool inside = to && player->GetParentCell() == to;
				if (!inside && Game::DistanceToPlayer(door) > kRelockRange) {
					relock.push_back(o.door);
					return true;
				}
				return false;
			});
		}
		for (auto id : relock) {
			if (auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id)) {
				Lock(door, true);
			}
		}
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json j{ { "holders", json::array() }, { "opened", json::array() } };
		for (auto& [npc, h] : g_holders) {
			j["holders"].push_back({ { "npc", npc }, { "key", h.key } });
		}
		for (auto& o : g_opened) {
			j["opened"].push_back({ { "door", o.door }, { "npc", o.npc } });
		}
		return j;
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		auto id = [&](const json& o, const char* k) {
			const auto raw = static_cast<RE::FormID>(JsonUtil::Num(o, k).value_or(0));
			return raw == 0 ? RE::FormID{ 0 } : resolve(raw);
		};
		std::scoped_lock l(g_lock);
		g_holders.clear();
		g_opened.clear();
		if (!j.is_object()) {
			return;
		}
		if (j.contains("holders") && j["holders"].is_array()) {
			for (auto& h : j["holders"]) {
				if (auto npc = id(h, "npc")) {
					g_holders[npc] = { id(h, "key") };
				}
			}
		}
		if (j.contains("opened") && j["opened"].is_array()) {
			for (auto& o : j["opened"]) {
				if (auto door = id(o, "door")) {
					g_opened.push_back({ door, id(o, "npc") });
				}
			}
		}
		logger::info("co-save: {} house keys", g_holders.size());
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_holders.clear();
		g_opened.clear();
	}
}
