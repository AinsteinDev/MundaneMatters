#include "Care.h"

#include "Bonds.h"
#include "Confirm.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Papyrus.h"
#include "Promises.h"
#include "Rest.h"
#include "Services.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Care
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr std::int32_t kHealFee = 10;     // an acquaintance's healing spell
		constexpr std::int32_t kCookFee = 5;      // an innkeeper or cook, per dish
		constexpr float kHealAgain = 4.0f;        // game hours before the same (non-priest) person patches you up again
		constexpr float kPotRange = 3000.0f;
		constexpr float kHealthy = 0.95f;

		std::mutex g_lock;
		std::unordered_map<RE::FormID, float> g_lastHeal;  // NPC -> game hours

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

		std::string Lower(const char* s)
		{
			return JsonUtil::Lower(s ? s : "");
		}

		std::string ClassOf(RE::Actor* npc)
		{
			auto* base = npc->GetActorBase();
			return Lower(base && base->npcClass ? base->npcClass->GetName() : "");
		}

		bool HasSeverActions()
		{
			static const bool has = RE::TESDataHandler::GetSingleton()->LookupModByName("SeverActions.esp") != nullptr;
			return has;
		}

		// "Mira (Acquaintance): won't ..." - the real relationship, so the player sees why.
		void Decline(RE::Actor* npc, const std::string& note, const std::string& narration)
		{
			Game::Notify(std::format("{} ({}): {}", npc->GetName(), Rest::RelationName(npc), note));
			Game::Narrate(narration, npc);
		}

		// No fee from friends, or from someone out with the player right now (following, spending time).
		bool Free(RE::Actor* npc)
		{
			return Services::IsCompanion(npc) || Promises::IsClose(npc) || Services::FollowsPlayer(npc) || Services::InCompany(npc->GetFormID());
		}

		bool Pay(std::int32_t fee, RE::Actor* npc)
		{
			auto* player = Game::Player();
			if (fee <= 0) {
				return true;
			}
			if (Game::ItemCount(player, Game::Gold()) < fee) {
				Game::Notify(std::format("You need {} gold.", fee));
				Game::Narrate(std::format("{} checked their purse: not even the {} gold it would cost.", PlayerName(), fee), npc);
				return false;
			}
			player->RemoveItem(Game::Gold(), fee, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, npc);
			return true;
		}

		// Paid care, or the free cure: a popup when "Confirm Care" is on. Free healing, cooking and
		// mending just happen (the player asked).
		void Ask(RE::Actor* npc, const std::string& title, const std::string& detail, std::int32_t fee, bool popup, std::function<void()> go)
		{
			if (!popup || !SkyrimNet::ConfigBool("features.confirm_care", true) || !Confirm::Available()) {
				go();
				return;
			}
			const auto npcId = npc->GetFormID();
			const std::string accept = fee > 0 ? Game::Pick({ "Deal. Here's the coin.", "Fair enough, go ahead.", "Do it." })
			                                   : Game::Pick({ "Yes, please.", "Thank you, I'd be grateful.", "Please do." });
			const std::string decline = Game::Pick({ "On second thought, never mind.", "Maybe later.", "No, it's fine." });
			Confirm::Offer offer;
			offer.title = title;
			offer.npc = npc->GetName();
			offer.detail = detail;
			offer.price = fee;
			offer.playerGold = Game::ItemCount(Game::Player(), Game::Gold());
			offer.showPrice = fee > 0;
			offer.seconds = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f));
			offer.acceptLabel = accept;
			offer.declineLabel = decline;
			if (fee > 0) {
				offer.sameAs = { "payment" };
			}
			Confirm::Ask(offer, [npcId, go, accept, decline](Confirm::Choice choice) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || choice == Confirm::Choice::kTimeout) {
					return;
				}
				if (choice == Confirm::Choice::kDecline) {
					Game::Say(decline, npc, true);
					return;
				}
				Game::Say(accept, npc, false);
				go();
			});
		}

		// ---- healing ----

		bool HealsHealth(RE::EffectSetting* e)
		{
			return e && !e->IsHostile() && !e->IsDetrimental() && e->data.primaryAV == RE::ActorValue::kHealth &&
			       (e->HasArchetype(RE::EffectSetting::Archetype::kValueModifier) || e->HasArchetype(RE::EffectSetting::Archetype::kPeakValueModifier));
		}

		// The healing they could do with a spell they know: health restored, 0 when none.
		float SpellHealing(RE::Actor* npc)
		{
			float best = 0.0f;
			auto consider = [&](RE::SpellItem* spell) {
				if (!spell || spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell) {
					return;
				}
				const bool stream = spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
				for (auto* eff : spell->effects) {
					if (eff && HealsHealth(eff->baseEffect)) {
						best = (std::max)(best, eff->effectItem.magnitude * (stream ? 6.0f : 1.0f));
					}
				}
			};
			if (auto* base = npc->GetActorBase(); base && base->actorEffects) {
				for (std::uint32_t i = 0; i < base->actorEffects->numSpells; ++i) {
					consider(base->actorEffects->spells[i]);
				}
			}
			for (auto* spell : npc->GetActorRuntimeData().addedSpells) {
				consider(spell);
			}
			return best;
		}

		// Their best healing potion (not food, not poison).
		RE::AlchemyItem* HealingPotion(RE::Actor* npc)
		{
			RE::AlchemyItem* best = nullptr;
			float bestMag = 0.0f;
			for (auto& [obj, entry] : npc->GetInventory()) {
				auto* potion = obj ? obj->As<RE::AlchemyItem>() : nullptr;
				if (!potion || entry.first <= 0 || potion->IsFood() || potion->IsPoison()) {
					continue;
				}
				for (auto* eff : potion->effects) {
					if (eff && HealsHealth(eff->baseEffect) && eff->effectItem.magnitude > bestMag) {
						bestMag = eff->effectItem.magnitude;
						best = potion;
					}
				}
			}
			return best;
		}

		float HealthFraction(RE::Actor* a)
		{
			auto* av = a->AsActorValueOwner();
			const float max = av->GetPermanentActorValue(RE::ActorValue::kHealth);
			return max > 0.0f ? av->GetActorValue(RE::ActorValue::kHealth) / max : 1.0f;
		}

		void Restore(float amount)
		{
			Game::Player()->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kHealth, amount);
		}

		// ---- cooking ----

		struct Recipe
		{
			RE::BGSConstructibleObject* cobj = nullptr;
			RE::TESBoundObject* dish = nullptr;
			std::int32_t makeable = 0;  // how many times the player's ingredients allow
			double score = 0.0;
		};

		std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> Needs(RE::BGSConstructibleObject* c)
		{
			std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> out;
			c->requiredItems.ForEachContainerObject([&](RE::ContainerObject& o) {
				if (o.obj && o.count > 0) {
					out.emplace_back(o.obj, o.count);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return out;
		}

		// Cooking-pot recipes the player's own ingredients can make, scored against what was said.
		std::vector<Recipe> Cookable(const std::string& said)
		{
			std::vector<Recipe> out;
			auto* pot = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingCookpot");
			if (!pot) {
				return out;
			}
			auto* player = Game::Player();
			const auto counts = player->GetInventoryCounts();
			for (auto* c : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::BGSConstructibleObject>()) {
				auto* dish = c && c->benchKeyword == pot && c->createdItem ? c->createdItem->As<RE::TESBoundObject>() : nullptr;
				if (!dish || !dish->GetName() || !*dish->GetName()) {
					continue;
				}
				const auto needs = Needs(c);
				if (needs.empty()) {
					continue;
				}
				std::int32_t n = 1000;
				double s = Promises::NameScore(said, dish->GetName()) * 2.0;
				for (auto& [obj, count] : needs) {
					const auto have = counts.contains(obj) ? counts.at(obj) : 0;
					n = (std::min)(n, have / count);
					s += Promises::NameScore(said, obj->GetName());
				}
				if (n > 0) {
					out.push_back({ c, dish, n, s });
				}
			}
			std::ranges::sort(out, [](const Recipe& a, const Recipe& b) { return a.score != b.score ? a.score > b.score : a.dish->GetGoldValue() > b.dish->GetGoldValue(); });
			return out;
		}

		RE::TESObjectREFR* NearestPot()
		{
			auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingCookpot");
			auto* player = Game::Player();
			RE::TESObjectREFR* best = nullptr;
			float bestDist = kPotRange;
			auto visit = [&](RE::TESObjectREFR* ref) {
				auto* base = ref ? ref->GetBaseObject() : nullptr;
				auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
				if (furn && kw && furn->HasKeyword(kw) && !ref->IsDisabled()) {
					if (const float d = Game::DistanceToPlayer(ref); d < bestDist) {
						bestDist = d;
						best = ref;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			};
			auto* cell = player->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				cell->ForEachReference(visit);
			} else if (auto* tes = RE::TES::GetSingleton()) {
				tes->ForEachReferenceInRange(player, kPotRange, visit);
			}
			return best;
		}

		void After(std::chrono::milliseconds wait, std::function<void()> then)
		{
			std::thread([wait, then = std::move(then)] {
				std::this_thread::sleep_for(wait);
				SKSE::GetTaskInterface()->AddTask(then);
			}).detach();
		}

		// The ingredients go into the pot now; the dish comes out a little later.
		void DoCook(RE::FormID npcId, RE::BGSConstructibleObject* c, std::int32_t n, std::int32_t fee)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* player = Game::Player();
			auto* dish = c->createdItem ? c->createdItem->As<RE::TESBoundObject>() : nullptr;
			if (!npc || !dish) {
				return;
			}
			const auto needs = Needs(c);
			for (auto& [obj, count] : needs) {
				n = (std::min)(n, Game::ItemCount(player, obj) / count);
			}
			if (n <= 0) {
				Game::Narrate(std::format("{} went to cook, but {} no longer had the ingredients.", npc->GetName(), PlayerName()), npc);
				return;
			}
			if (!Pay(fee * n, npc)) {
				return;
			}
			std::string used;
			for (auto& [obj, count] : needs) {
				player->RemoveItem(obj, count * n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
				used += (used.empty() ? "" : ", ") + Promises::Counted(obj->GetName(), count * n);
			}
			const auto made = (std::max)(1, static_cast<std::int32_t>(c->data.numConstructed)) * n;
			auto* pot = NearestPot();
			const bool atPot = pot && HasSeverActions() &&
			                   Papyrus::CallStatic("SeverActions_FurnitureLib", "UseRef", nullptr, static_cast<RE::Actor*>(npc), static_cast<RE::TESObjectREFR*>(pot));
			const std::string what = Promises::Counted(dish->GetName(), made);
			logger::info("care: {} cooks {} from {} for {}{}", npc->GetName(), what, used, PlayerName(), fee > 0 ? std::format(" ({} gold)", fee * n) : "");
			After(atPot ? 9000ms : 1500ms, [npcId, dish, made, what, atPot, free = fee == 0] {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (atPot && npc) {
					Papyrus::CallStatic("SeverActions_FurnitureLib", "Stop", nullptr, static_cast<RE::Actor*>(npc));
				}
				Game::Player()->AddObjectToContainer(dish, nullptr, made, npc);
				Game::Notify(std::format("{} cooked you {}", npc ? npc->GetName() : "They", what));
				if (npc) {
					Game::Narrate(std::format("{} finished cooking at the pot and handed {} the {}.", npc->GetName(), PlayerName(), what), npc);
					if (free) {
						Bonds::Add(npc, Bonds::Deed::kCared, 1);
					}
				}
			});
		}

		// ---- tempering ----

		constexpr std::array<const char*, 7> kQuality{ "plain", "Fine", "Superior", "Exquisite", "Flawless", "Epic", "Legendary" };

		int StepOf(float health)
		{
			return std::clamp(static_cast<int>(std::lround((health - 1.0f) * 10.0f)), 0, 6);
		}

		// How far this smith can take a piece: their Smithing skill, never Legendary.
		int CeilingOf(RE::Actor* npc)
		{
			const float skill = npc->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSmithing);
			return std::clamp(1 + static_cast<int>(skill / 25.0f), 1, 5);
		}

		struct Piece
		{
			RE::InventoryEntryData* entry = nullptr;
			RE::ExtraDataList* list = nullptr;  // the instance; null = a plain one (gets its own list)
			float health = 1.0f;
		};

		// One instance of the item in the player's pack: the worn one if `worn`, else the least tempered.
		std::optional<Piece> PieceOf(RE::TESBoundObject* item, bool worn)
		{
			auto* changes = Game::Player()->GetInventoryChanges();
			if (!changes || !changes->entryList) {
				return std::nullopt;
			}
			for (auto* entry : *changes->entryList) {
				if (!entry || entry->object != item) {
					continue;
				}
				std::optional<Piece> best;
				std::int32_t listed = 0;
				if (entry->extraLists) {
					for (auto* list : *entry->extraLists) {
						if (!list) {
							continue;
						}
						listed += (std::max)(1, list->GetCount());
						const bool isWorn = list->HasType<RE::ExtraWorn>() || list->HasType<RE::ExtraWornLeft>();
						auto* h = list->GetByType<RE::ExtraHealth>();
						const float health = h ? h->health : 1.0f;
						if (worn ? isWorn : !best || health < best->health) {
							best = Piece{ entry, list, health };
							if (worn) {
								break;
							}
						}
					}
				}
				// A plain copy (no instance data at all) is the least tempered there is.
				if (!worn && Game::ItemCount(Game::Player(), item) > listed) {
					best = Piece{ entry, nullptr, 1.0f };
				}
				return best;
			}
			return std::nullopt;
		}

		bool Temper(RE::TESBoundObject* item, bool worn, float to)
		{
			auto piece = PieceOf(item, worn);
			if (!piece) {
				return false;
			}
			auto* list = piece->list;
			if (!list) {
				// A plain copy has no instance data to hold its quality: set it down for a moment,
				// temper it as an object in the world, and pick it straight back up (its data comes along).
				auto* player = Game::Player();
				auto handle = player->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
				auto ref = handle.get();
				if (!ref) {
					return false;
				}
				ref->extraList.Add(new RE::ExtraHealth(to));
				player->PickUpObject(ref.get(), 1, false, false);
				return true;
			}
			if (auto* h = list->GetByType<RE::ExtraHealth>()) {
				h->health = to;
			} else {
				list->Add(new RE::ExtraHealth(to));
			}
			return true;
		}

		bool IsGear(RE::TESBoundObject* obj)
		{
			return obj && (obj->IsWeapon() || obj->IsArmor()) && obj->GetPlayable() && obj->GetName() && *obj->GetName();
		}

		void DoMend(RE::FormID npcId, RE::TESBoundObject* item, bool worn, int step, std::int32_t fee)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc) {
				return;
			}
			auto piece = PieceOf(item, worn);
			if (!piece || StepOf(piece->health) + 1 != step) {
				Game::Notify("The piece isn't what it was a moment ago; nothing done.");
				return;
			}
			if (!Pay(fee, npc) || !Temper(item, worn, 1.0f + step / 10.0f)) {
				return;
			}
			const std::string name = item->GetName();
			Game::Notify(std::format("{} improved your {} ({})", npc->GetName(), name, kQuality[step]));
			Game::Narrate(std::format("{} worked {}'s {} at the grindstone and workbench until it was {} quality{}.", npc->GetName(), PlayerName(), name,
							  kQuality[step], fee > 0 ? std::format(", for {} gold", fee) : ", as a favour"),
				npc);
			logger::info("care: {} tempered {} to {} ({} gold)", npc->GetName(), name, kQuality[step], fee);
			if (fee == 0) {
				Bonds::Add(npc, Bonds::Deed::kCared, 1);
			}
		}

		void MendOne(RE::Actor* npc, RE::TESBoundObject* item, bool worn)
		{
			auto piece = PieceOf(item, worn);
			if (!piece) {
				Game::Narrate(std::format("{} reached for {}'s {}, but it wasn't there.", npc->GetName(), PlayerName(), item->GetName()), npc);
				return;
			}
			const int step = StepOf(piece->health) + 1;
			const int ceiling = CeilingOf(npc);
			if (step > ceiling) {
				Game::Notify(std::format("{}: that's as good as I can make it", npc->GetName()));
				Game::Narrate(std::format("{} looked {}'s {} over: it was already {} work, as good as {} could make it.", npc->GetName(), PlayerName(),
								  item->GetName(), kQuality[step - 1], npc->GetName()),
					npc);
				return;
			}
			const bool free = Free(npc);
			const std::int32_t fee = free ? 0 : (std::max)(10, static_cast<std::int32_t>(std::lround(item->GetGoldValue() * 0.2)));
			const auto npcId = npc->GetFormID();
			Ask(npc, "Improve", std::format("{} -> {}", item->GetName(), kQuality[step]), fee, fee > 0,
				[npcId, item, worn, step, fee] { DoMend(npcId, item, worn, step, fee); });
		}

		// ---- diseases ----

		std::vector<RE::SpellItem*> Diseases()
		{
			std::vector<RE::SpellItem*> out;
			for (auto* spell : Game::Player()->GetActorRuntimeData().addedSpells) {
				if (spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kDisease) {
					out.push_back(spell);
				}
			}
			return out;
		}
	}

	bool IsPriest(RE::Actor* npc)
	{
		if (!npc) {
			return false;
		}
		const auto cls = ClassOf(npc);
		for (auto* w : { "priest", "healer", "monk", "cleric", "acolyte", "temple" }) {
			if (cls.find(w) != std::string::npos) {
				return true;
			}
		}
		// Someone of the temple who knows healing magic.
		for (auto* loc = npc->GetCurrentLocation(); loc; loc = loc->parentLoc) {
			if (loc->HasKeywordString("LocTypeTemple")) {
				return SpellHealing(npc) > 0.0f;
			}
		}
		return false;
	}

	bool IsSmith(RE::Actor* npc)
	{
		if (!npc) {
			return false;
		}
		if (ClassOf(npc).find("smith") != std::string::npos) {
			return true;
		}
		// A shop that deals in weapons or ore (not a general store's "everything but...").
		if (auto* f = npc->GetVendorFaction(); f && f->vendorData.vendorSellBuyList && !f->vendorData.vendorValues.notBuySell) {
			for (auto* kw : { "VendorItemWeapon", "VendorItemOreIngot" }) {
				auto* k = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kw);
				if (k && f->vendorData.vendorSellBuyList->HasForm(k)) {
					return true;
				}
			}
		}
		return false;
	}

	bool Heal(RE::Actor* npc)
	{
		if (!npc) {
			return false;
		}
		const std::string name = npc->GetName();
		const float frac = HealthFraction(Game::Player());
		if (frac >= kHealthy) {
			Game::Narrate(std::format("{} looked {} over: there wasn't a scratch worth tending.", name, PlayerName()), npc);
			return true;
		}
		const bool priest = IsPriest(npc);
		const bool free = priest || Free(npc);
		const float now = Game::Hours();
		if (!priest) {
			std::scoped_lock l(g_lock);
			if (auto it = g_lastHeal.find(npc->GetFormID()); it != g_lastHeal.end() && now - it->second < kHealAgain) {
				Game::Narrate(std::format("{} had already patched {} up a little while ago and had nothing more to give for now.", name, PlayerName()), npc);
				return true;
			}
		}
		const float spell = SpellHealing(npc);
		auto* potion = spell > 0.0f ? nullptr : HealingPotion(npc);
		if (spell <= 0.0f && !potion && !priest) {
			Game::Notify(std::format("{}: no potion or healing magic", name));
			Game::Narrate(std::format("{} had no healing potion and no healing magic; all they could offer was a look at the wound.", name), npc);
			return true;
		}
		// A stranger's own potion: they agreed to see to it, so the relationship doesn't overrule them.
		const auto npcId = npc->GetFormID();
		const std::int32_t fee = free ? 0 : kHealFee;
		auto go = [npcId, spell, potion, priest, fee] {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* player = Game::Player();
			if (!npc || !Pay(fee, npc)) {
				return;
			}
			const float max = player->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth);
			if (potion && Game::ItemCount(npc, potion) > 0) {
				npc->RemoveItem(potion, 1, RE::ITEM_REMOVE_REASON::kStoreInTeammate, nullptr, player);
				RE::ActorEquipManager::GetSingleton()->EquipObject(player, potion);  // drinks it: its real effect
				Game::Notify(std::format("{} gave you {} to drink", npc->GetName(), potion->GetName()));
				Game::Narrate(std::format("{} pressed a {} into {}'s hands and watched them drink it.", npc->GetName(), potion->GetName(), PlayerName()), npc);
			} else {
				// Their spell's real strength; a priest's care at least half mends the wounds.
				const float amount = spell > 0.0f ? (std::max)(spell, 25.0f) : max * 0.5f;
				Restore(amount);
				Game::Notify(std::format("{} healed you", npc->GetName()));
				Game::Narrate(std::format("{} laid their hands on {}'s wounds{}.", npc->GetName(), PlayerName(),
								  spell > 0.0f ? " and Restoration magic closed them" : priest ? ", cleaned and dressed them with a prayer" : ""),
					npc);
			}
			logger::info("care: {} healed {} ({}{})", npc->GetName(), PlayerName(), potion ? potion->GetName() : spell > 0.0f ? "spell" : "tending",
				fee > 0 ? std::format(", {} gold", fee) : "");
			{
				std::scoped_lock l(g_lock);
				g_lastHeal[npcId] = Game::Hours();
			}
			if (fee == 0 && !priest) {
				Bonds::Add(npc, Bonds::Deed::kCared, 1);
			}
		};
		Ask(npc, "Heal", spell > 0.0f ? "Restoration magic" : "Tend your wounds", fee, fee > 0, go);
		return true;
	}

	bool Cook(RE::Actor* npc, const std::string& said)
	{
		if (!npc) {
			return false;
		}
		const std::string name = npc->GetName();
		const auto cls = ClassOf(npc);
		const bool trade = Services::RoomOf(npc).has_value() || cls.find("cook") != std::string::npos || cls.find("chef") != std::string::npos ||
		                   cls.find("innkeeper") != std::string::npos;
		// Not their trade but they agreed: a favour, no fee. Cooks and innkeepers charge strangers.
		const bool free = Free(npc) || !trade;
		if (!NearestPot()) {
			Game::Narrate(std::format("{} looked around: there was no cooking pot anywhere near.", name), npc);
			return true;
		}
		auto recipes = Cookable(said);
		if (recipes.empty()) {
			Game::Narrate(std::format("{} looked at what {} carried: not enough of anything to make a dish.", name, PlayerName()), npc);
			return true;
		}
		const std::int32_t fee = free ? 0 : kCookFee;
		const auto npcId = npc->GetFormID();
		const bool clear = !JsonUtil::Words(said).empty() && recipes.front().score > 0.0 &&
		                   (recipes.size() == 1 || recipes.front().score > recipes[1].score + 0.5);
		if (clear) {
			auto r = recipes.front();
			Ask(npc, "Cook", std::format("{} from your ingredients", r.dish->GetName()), fee, fee > 0, [npcId, r, fee] { DoCook(npcId, r.cobj, 1, fee); });
			return true;
		}
		// "Cook these" / "make something": the player picks from what their ingredients can make.
		if (!Confirm::Available()) {
			DoCook(npcId, recipes.front().cobj, 1, fee);
			return true;
		}
		Confirm::Offer offer;
		offer.title = "Cook";
		offer.npc = name;
		offer.detail = fee > 0 ? std::format("What should they cook? ({} gold a dish)", fee) : "What should they cook?";
		offer.showPrice = false;
		offer.seconds = 0;
		offer.acceptLabel = "Cook it";
		offer.declineLabel = "Never mind";
		offer.hint = "Click a dish, then Cook it. Counts are how many your ingredients make.";
		for (auto& r : recipes) {
			offer.pick.push_back({ r.cobj->GetFormID(), r.dish->GetName(), r.makeable, r.dish->GetGoldValue(), false });
			if (offer.pick.size() >= 40) {
				break;
			}
		}
		Confirm::Ask(offer, [npcId, fee](Confirm::Choice choice) {
			if (choice != Confirm::Choice::kAccept) {
				return;
			}
			const auto [id, qty] = Confirm::LastPick();
			if (auto* c = RE::TESForm::LookupByID<RE::BGSConstructibleObject>(id); c && qty > 0) {
				DoCook(npcId, c, qty, fee);
			}
		});
		return true;
	}

	bool Mend(RE::Actor* npc, const std::string& said)
	{
		if (!npc) {
			return false;
		}
		const std::string name = npc->GetName();
		if (!IsSmith(npc)) {
			Game::Narrate(std::format("{} was no smith and had no way to improve {}'s gear.", name, PlayerName()), npc);
			return true;
		}
		auto* player = Game::Player();
		std::vector<Promises::Found> found;
		for (auto& f : Promises::FindItems(player, said)) {
			if (IsGear(f.item)) {
				found.push_back(f);
			}
		}
		// One clear piece, or the one in hand among close matches.
		std::optional<Promises::Found> one;
		if (!found.empty()) {
			std::vector<Promises::Found> close, worn;
			for (auto& f : found) {
				if (f.score > found.front().score - 0.75) {
					close.push_back(f);
					if (f.worn) {
						worn.push_back(f);
					}
				}
			}
			one = close.size() == 1 ? std::optional(close.front()) : worn.size() == 1 ? std::optional(worn.front()) : std::nullopt;
		}
		if (one) {
			MendOne(npc, one->item, one->worn);
			return true;
		}
		// "Sharpen my gear": the player picks (worn things first).
		Confirm::Offer offer;
		offer.title = "Improve";
		offer.npc = name;
		offer.detail = Free(npc) ? "Which piece? (a favour)" : "Which piece? (for coin)";
		offer.showPrice = false;
		offer.seconds = 0;
		offer.acceptLabel = "This one";
		offer.declineLabel = "Never mind";
		offer.hint = "Click a weapon or armour, then This one.";
		for (auto& [obj, entry] : player->GetInventory()) {
			if (IsGear(obj) && entry.first > 0 && !(entry.second && entry.second->IsQuestObject()) && (found.empty() || std::ranges::any_of(found, [&](auto& f) { return f.item == obj; }))) {
				offer.pick.push_back({ obj->GetFormID(), obj->GetName(), 1, obj->GetGoldValue(), entry.second && entry.second->IsWorn() });
			}
		}
		std::ranges::sort(offer.pick, [](auto& a, auto& b) { return a.equipped != b.equipped ? a.equipped : a.value > b.value; });
		if (offer.pick.size() > 40) {
			offer.pick.resize(40);
		}
		if (offer.pick.empty() || !Confirm::Available()) {
			Game::Narrate(std::format("{} waited for {} to show which piece they meant.", name, PlayerName()), npc);
			return true;
		}
		const auto npcId = npc->GetFormID();
		Confirm::Ask(offer, [npcId](Confirm::Choice choice) {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			if (!npc || choice != Confirm::Choice::kAccept) {
				return;
			}
			const auto [id, qty] = Confirm::LastPick();
			if (auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(id)) {
				auto inv = Game::Player()->GetInventory();
				const bool worn = inv.contains(item) && inv.at(item).second && inv.at(item).second->IsWorn();
				MendOne(npc, item, worn);
			}
		});
		return true;
	}

	bool Cure(RE::Actor* npc)
	{
		if (!npc) {
			return false;
		}
		const std::string name = npc->GetName();
		if (!IsPriest(npc)) {
			Game::Narrate(std::format("{} was no healer; a priest at the temple could cure that.", name), npc);
			return true;
		}
		const auto diseases = Diseases();
		if (diseases.empty()) {
			Game::Narrate(std::format("{} examined {}: no sign of any disease.", name, PlayerName()), npc);
			return true;
		}
		std::string list;
		for (auto* d : diseases) {
			list += (list.empty() ? "" : ", ") + std::string(d->GetName());
		}
		const auto npcId = npc->GetFormID();
		// Free, but asked first (the one free kind of care with a popup).
		Ask(npc, "Cure disease", list + " (free)", 0, true, [npcId, list] {
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* player = Game::Player();
			if (!npc) {
				return;
			}
			for (auto* d : Diseases()) {
				player->RemoveSpell(d);
			}
			Game::Notify(std::format("{} cured you of {}", npc->GetName(), list));
			Game::Narrate(std::format("{} prayed over {} and the {} left them.", npc->GetName(), PlayerName(), list), npc);
			logger::info("care: {} cured {} of {}", npc->GetName(), PlayerName(), list);
			Bonds::Add(npc, Bonds::Deed::kCared, 1);
		});
		return true;
	}

	void Reset()
	{
		std::scoped_lock l(g_lock);
		g_lastHeal.clear();
	}
}
