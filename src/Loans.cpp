#include "Loans.h"

#include "Bonds.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Promises.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Loans
{
	namespace
	{
		constexpr float kDefaultHours = 72.0f;     // "bring it back" with no time said: a few days
		constexpr float kPickupHours = 24.0f;      // "take the one by the fire": the offer stands this long
		constexpr float kRemindEvery = 24.0f;      // an overdue loan is brought up at most once a day
		constexpr float kRemindRange = 1500.0f;
		constexpr float kUsedHours = 2.0f;
		constexpr float kKeepAfter = 0.5f;         // "keep it" only for a loan that isn't brand new (a repeated "take it" isn't "keep it")         // borrowed at least this long before bringing it back counts for Bonds

		// Something the player borrowed from an NPC.
		struct Borrowed
		{
			RE::FormID npc = 0;
			RE::FormID item = 0;
			std::int32_t count = 1;
			std::string name;     // read off the game thread (bio)
			std::string npcName;
			float since = 0.0f;   // game hours: lent (or offered, until picked up)
			float back = kDefaultHours;  // hours after pickup it's wanted back
			float due = 0.0f;
			bool pickedUp = true;
			std::int32_t baseline = 0;   // the player's count when it was offered (pickup = more than this)
			float lastAsked = -1.0f;
		};

		// Something the player lent an NPC.
		struct Lent
		{
			RE::FormID npc = 0;
			RE::FormID item = 0;
			std::int32_t count = 1;
			std::string name;
		};

		std::mutex g_lock;
		std::vector<Borrowed> g_borrowed;
		std::vector<Lent> g_lent;

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

		std::string Lower(const char* s)
		{
			return JsonUtil::Lower(s ? s : "");
		}

		bool IsTool(RE::TESBoundObject* item)
		{
			const auto name = Lower(item ? item->GetName() : "");
			return name.find("pickaxe") != std::string::npos || name.find("woodcutter") != std::string::npos;
		}

		// "in a day", "in 3 days", "overdue by 2 days"
		std::string Span(float hours)
		{
			const auto h = std::abs(hours);
			if (h < 20.0f) {
				return std::format("{} hours", (std::max)(1, static_cast<int>(h)));
			}
			const int days = static_cast<int>(std::lround(h / 24.0f));
			return days <= 1 ? "a day" : std::format("{} days", days);
		}

		void Settled(RE::Actor* npc, const std::string& what, bool used)
		{
			Game::Notify(std::format("Returned {} to {}", what, Game::NameOf(npc)));
			SkyrimNet::AddMemory(npc->GetFormID(), std::format("{} brought back the {} {} had lent them.", PlayerName(), what, Game::NameOf(npc)), 0.5f);
			// Trustworthy: what's lent comes back. Borrowed and handed straight back isn't a deed.
			if (used) {
				Bonds::Add(npc, Bonds::Deed::kReturned, 1);
			}
		}

		// The player put a borrowed thing back in the lender's home (on a table, in a chest), or
		// dropped it there: it's back.
		void PutBack(RE::TESBoundObject* item, std::int32_t n, RE::TESObjectREFR* into)
		{
			auto* cell = into ? into->GetParentCell() : Game::Player()->GetParentCell();
			if (!item || !cell || !cell->IsInteriorCell()) {
				return;
			}
			std::vector<RE::FormID> lenders;
			{
				std::scoped_lock l(g_lock);
				for (auto& b : g_borrowed) {
					if (b.pickedUp && b.item == item->GetFormID()) {
						lenders.push_back(b.npc);
					}
				}
			}
			for (auto id : lenders) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!npc || !Promises::IsHome(cell, npc)) {
					continue;
				}
				if (const auto settled = GiveBack(npc, item, n); settled > 0) {
					const auto what = Promises::Counted(item->GetName(), settled);
					if (Game::DistanceToPlayer(npc) < kRemindRange) {
						Game::Narrate(std::format("{} put the {} back where it belongs in {}'s home.", PlayerName(), what, Game::NameOf(npc)), npc);
					}
					logger::info("loans: {} put {} back in {}'s home", PlayerName(), what, Game::NameOf(npc));
				}
				return;
			}
		}

		struct ContainerSink : RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* e, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (!e || e->oldContainer != 0x14 || e->itemCount <= 0) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto base = e->baseObj, to = e->newContainer;
				const auto n = e->itemCount;
				SKSE::GetTaskInterface()->AddTask([base, to, n] {
					auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(base);
					if (!item || !Owes(0, item)) {
						return;
					}
					if (!to) {
						PutBack(item, n, nullptr);  // dropped
						return;
					}
					// Handed to a person: the gift and give-back paths settle that themselves.
					auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(to);
					if (ref && !ref->As<RE::Actor>()) {
						PutBack(item, n, ref);
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	bool Lendable(RE::TESBoundObject* item, bool& consumable)
	{
		consumable = false;
		if (!item) {
			return false;
		}
		if (item->Is(RE::FormType::AlchemyItem) || item->Is(RE::FormType::Ingredient) || item->Is(RE::FormType::Ammo) ||
			item->Is(RE::FormType::Scroll) || item->Is(RE::FormType::Light) || item->Is(RE::FormType::SoulGem)) {
			consumable = true;  // used up: a gift, not a loan
			return false;
		}
		if (item->IsArmor() || item->Is(RE::FormType::KeyMaster) || (item->IsWeapon() && !IsTool(item))) {
			return false;  // nobody lends out armour or a real weapon
		}
		return item->GetGoldValue() <= static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.max_loan_value", 50.0f));
	}

	RE::TESBoundObject* TradeTool(RE::Actor* npc, const std::string& said)
	{
		if (!npc) {
			return nullptr;
		}
		const auto words = JsonUtil::Words(said);
		auto any = [&](std::initializer_list<std::string_view> keys) {
			return std::ranges::any_of(words, [&](const std::string& w) {
				return std::ranges::any_of(keys, [&](std::string_view k) { return w.starts_with(k); });
			});
		};
		const bool pick = any({ "pick", "pike" });
		const bool axe = !pick && (any({ "woodcut", "hatchet" }) || std::ranges::find(words, "axe") != words.end());
		if (!pick && !axe) {
			return nullptr;
		}
		// Do they work with it? Their class, what their shop deals in, or where they work.
		auto* base = npc->GetActorBase();
		const auto cls = Lower(base && base->npcClass ? base->npcClass->GetName() : "");
		bool works = cls.find("smith") != std::string::npos || (pick ? cls.find("miner") != std::string::npos : cls.find("lumber") != std::string::npos);
		if (auto* f = npc->GetVendorFaction(); !works && f && f->vendorData.vendorSellBuyList) {
			for (auto* kw : { "VendorItemWeapon", "VendorItemOreIngot" }) {
				auto* k = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kw);
				works = works || (k && f->vendorData.vendorSellBuyList->HasForm(k) != f->vendorData.vendorValues.notBuySell);
			}
		}
		for (auto* loc = npc->GetCurrentLocation(); !works && loc; loc = loc->parentLoc) {
			const auto name = Lower(loc->GetName());
			works = pick ? loc->HasKeywordString("LocTypeMine") || name.find("mine") != std::string::npos
			             : name.find("mill") != std::string::npos || name.find("lumber") != std::string::npos;
		}
		if (!works) {
			return nullptr;
		}
		// The plainest one in the game.
		RE::TESBoundObject* best = nullptr;
		for (auto* w : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESObjectWEAP>()) {
			const auto name = Lower(w ? w->GetName() : "");
			if (w && w->GetPlayable() && name.find(pick ? "pickaxe" : "woodcutter") != std::string::npos &&
				(!best || w->GetGoldValue() < best->GetGoldValue())) {
				best = w;
			}
		}
		logger::info("loans: {} works with {} -> {}", Game::NameOf(npc), pick ? "pickaxes" : "axes", best ? best->GetName() : "none found");
		return best;
	}

	void Borrow(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t n, float backInHours, bool handed)
	{
		if (!npc || !item || n <= 0) {
			return;
		}
		Borrowed b;
		b.npc = npc->GetFormID();
		b.item = item->GetFormID();
		b.count = n;
		b.name = item->GetName();
		b.npcName = Game::NameOf(npc);
		b.since = Game::Hours();
		b.back = backInHours >= 1.0f ? backInHours : kDefaultHours;
		b.due = b.since + b.back;
		b.pickedUp = handed;
		b.baseline = handed ? 0 : Game::ItemCount(Game::Player(), item);
		{
			std::scoped_lock l(g_lock);
			g_borrowed.push_back(b);
		}
		const auto what = Promises::Counted(b.name, n);
		Game::Notify(handed ? std::format("{} lent you {} (back in {})", b.npcName, what, Span(b.back))
		                    : std::format("You may borrow {}'s {} (back in {})", b.npcName, what, Span(b.back)));
		SkyrimNet::AddMemory(b.npc, std::format("{} lent {} {}, to have it back within {}.", b.npcName, PlayerName(), what, Span(b.back)), 0.4f);
		logger::info("loans: {} lent {} {} for {:.0f} h{}", b.npcName, PlayerName(), what, b.back, handed ? "" : " (to pick up)");
	}

	bool Owes(RE::FormID npc, RE::TESBoundObject* item)
	{
		std::scoped_lock l(g_lock);
		return item && std::ranges::any_of(g_borrowed, [&](const Borrowed& b) {
			return b.pickedUp && b.item == item->GetFormID() && (npc == 0 || b.npc == npc);
		});
	}

	std::int32_t GiveBack(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t n)
	{
		if (!npc || !item || n <= 0) {
			return 0;
		}
		std::int32_t settled = 0;
		bool used = false;
		{
			std::scoped_lock l(g_lock);
			for (auto& b : g_borrowed) {
				if (settled < n && b.pickedUp && b.npc == npc->GetFormID() && b.item == item->GetFormID()) {
					const auto take = (std::min)(b.count, n - settled);
					b.count -= take;
					settled += take;
					used = used || Game::Hours() - b.since >= kUsedHours;
				}
			}
			std::erase_if(g_borrowed, [](const Borrowed& b) { return b.count <= 0; });
		}
		if (settled > 0) {
			Settled(npc, Promises::Counted(item->GetName(), settled), used);
			logger::info("loans: {} gave back {} x{} to {}", PlayerName(), item->GetName(), settled, Game::NameOf(npc));
		}
		return settled;
	}

	std::string PayFor(RE::Actor* npc, std::int32_t gold)
	{
		if (!npc || gold <= 0) {
			return "";
		}
		std::string name;
		{
			std::scoped_lock l(g_lock);
			for (auto it = g_borrowed.begin(); it != g_borrowed.end(); ++it) {
				auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(it->item);
				if (!it->pickedUp || it->npc != npc->GetFormID() || !item || Game::ItemCount(Game::Player(), item) > 0) {
					continue;  // still has it: hand it back instead
				}
				const auto worth = (std::max)(1, item->GetGoldValue()) * it->count;
				if (gold >= worth) {
					name = Promises::Counted(it->name, it->count);
					g_borrowed.erase(it);
					break;
				}
			}
		}
		if (!name.empty()) {
			Game::Notify(std::format("Paid {} for the lost {}", Game::NameOf(npc), name));
			SkyrimNet::AddMemory(npc->GetFormID(), std::format("{} lost the {} {} had lent them, and paid for it.", PlayerName(), name, Game::NameOf(npc)), 0.4f);
			Bonds::Add(npc, Bonds::Deed::kReturned, 1);
			logger::info("loans: {} paid {} {} gold for the lost {}", PlayerName(), Game::NameOf(npc), gold, name);
		}
		return name;
	}

	std::string LetKeep(RE::Actor* npc, const std::string& said)
	{
		if (!npc) {
			return "";
		}
		std::string name;
		{
			std::scoped_lock l(g_lock);
			for (auto it = g_borrowed.begin(); it != g_borrowed.end(); ++it) {
				if (it->pickedUp && it->npc == npc->GetFormID() && Game::Hours() - it->since >= kKeepAfter &&
					Promises::NameScore(said, it->name.c_str()) > 0.0) {
					name = it->name;
					g_borrowed.erase(it);
					break;
				}
			}
		}
		if (!name.empty()) {
			SkyrimNet::AddMemory(npc->GetFormID(), std::format("{} let {} keep the {} they had lent.", Game::NameOf(npc), PlayerName(), name), 0.4f);
			logger::info("loans: {} let {} keep the {}", Game::NameOf(npc), PlayerName(), name);
		}
		return name;
	}

	void LendTo(RE::Actor* npc, RE::TESBoundObject* item, std::int32_t n)
	{
		if (!npc || !item || n <= 0) {
			return;
		}
		{
			std::scoped_lock l(g_lock);
			auto it = std::ranges::find_if(g_lent, [&](const Lent& x) { return x.npc == npc->GetFormID() && x.item == item->GetFormID(); });
			if (it != g_lent.end()) {
				it->count += n;
			} else {
				g_lent.push_back({ npc->GetFormID(), item->GetFormID(), n, item->GetName() });
			}
		}
		const auto what = Promises::Counted(item->GetName(), n);
		Game::Notify(std::format("You lent {} {}", Game::NameOf(npc), what));
		SkyrimNet::AddMemory(npc->GetFormID(), std::format("{} lent {} their {}, to give back when asked.", PlayerName(), Game::NameOf(npc), what), 0.4f);
		logger::info("loans: {} lent {} {}", PlayerName(), Game::NameOf(npc), what);
	}

	bool HasLentTo(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		return std::ranges::any_of(g_lent, [&](const Lent& x) { return x.npc == npc; });
	}

	std::int32_t LentCount(RE::FormID npc, RE::TESBoundObject* item)
	{
		std::scoped_lock l(g_lock);
		std::int32_t n = 0;
		for (auto& x : g_lent) {
			if (item && x.npc == npc && x.item == item->GetFormID()) {
				n += x.count;
			}
		}
		return n;
	}

	void TookBack(RE::FormID npc, RE::TESBoundObject* item, std::int32_t n)
	{
		std::scoped_lock l(g_lock);
		for (auto& x : g_lent) {
			if (item && n > 0 && x.npc == npc && x.item == item->GetFormID()) {
				const auto take = (std::min)(x.count, n);
				x.count -= take;
				n -= take;
			}
		}
		std::erase_if(g_lent, [](const Lent& x) { return x.count <= 0; });
	}

	bool Retrieve(RE::Actor* npc, const std::string& said)
	{
		if (!npc) {
			return false;
		}
		std::vector<Lent> match;
		{
			std::scoped_lock l(g_lock);
			for (auto& x : g_lent) {
				if (x.npc == npc->GetFormID() && (JsonUtil::Words(said).empty() || Promises::NameScore(said, x.name.c_str()) > 0.0)) {
					match.push_back(x);
				}
			}
		}
		if (match.empty()) {
			return false;
		}
		std::string back;
		for (auto& x : match) {
			auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(x.item);
			const auto n = item ? (std::min)(x.count, Game::ItemCount(npc, item)) : 0;
			if (n > 0) {
				npc->RemoveItem(item, n, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, Game::Player());  // unequips it if in use
				back += (back.empty() ? "" : ", ") + Promises::Counted(x.name, n);
			}
			TookBack(npc->GetFormID(), item, x.count);  // given back, or gone (lost, sold): either way no longer lent
		}
		if (back.empty()) {
			Game::Narrate(std::format("{} looked for what {} had lent them, but no longer had it.", Game::NameOf(npc), PlayerName()), npc);
			return true;
		}
		Game::Notify(std::format("{} gave back: {}", Game::NameOf(npc), back));
		Game::Narrate(std::format("{} handed back the {} {} had lent them.", Game::NameOf(npc), back, PlayerName()), npc);
		return true;
	}

	std::vector<std::string> BioLines(RE::FormID npc)
	{
		std::vector<std::string> out;
		const auto player = PlayerName();
		const float now = Game::Hours();
		std::scoped_lock l(g_lock);
		for (auto& b : g_borrowed) {
			if (b.npc != npc) {
				continue;
			}
			if (!b.pickedUp) {
				out.push_back(std::format("Told {} they could borrow the {} lying in their place; {} hasn't picked it up yet.", player, b.name, player));
			} else if (now <= b.due) {
				out.push_back(std::format("Lent {} their {} {} ago; wants it back within {}.", player, Promises::Counted(b.name, b.count), Span(now - b.since), Span(b.due - now)));
			} else {
				out.push_back(std::format("Lent {} their {} {} ago; it is overdue by {} and they'd like it back (they mention it, nothing worse).",
					player, Promises::Counted(b.name, b.count), Span(now - b.since), Span(now - b.due)));
			}
		}
		std::string lent;
		for (auto& x : g_lent) {
			if (x.npc == npc) {
				lent += (lent.empty() ? "" : ", ") + Promises::Counted(x.name, x.count);
			}
		}
		if (!lent.empty()) {
			out.push_back(std::format("Has {}'s {}, lent to them to use; gives it back when asked.", player, lent));
		}
		return out;
	}

	void Tick()
	{
		const float now = Game::Hours();
		std::vector<Borrowed> remind;
		{
			std::scoped_lock l(g_lock);
			std::erase_if(g_borrowed, [&](Borrowed& b) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(b.npc);
				if (!npc || npc->IsDead()) {
					logger::info("loans: {} is gone; the {} is the player's now", b.npcName, b.name);
					return true;
				}
				if (!b.pickedUp) {
					auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(b.item);
					if (item && Game::ItemCount(Game::Player(), item) > b.baseline) {
						b.pickedUp = true;
						b.since = now;
						b.due = now + b.back;
						logger::info("loans: the player picked up {}'s {}", b.npcName, b.name);
					} else if (now - b.since > kPickupHours) {
						logger::info("loans: {}'s {} was never picked up; no loan", b.npcName, b.name);
						return true;
					}
					return false;
				}
				// Overdue: they bring it up when they see the player, once a day. Nothing worse.
				if (now > b.due && (b.lastAsked < 0.0f || now - b.lastAsked >= kRemindEvery) && Game::DistanceToPlayer(npc) < kRemindRange &&
					!npc->IsInCombat() && !Game::Player()->IsInCombat()) {
					b.lastAsked = now;
					remind.push_back(b);
				}
				return false;
			});
			std::erase_if(g_lent, [](const Lent& x) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(x.npc);
				return !npc || npc->IsDead();  // whatever they had is on their body
			});
		}
		for (auto& b : remind) {
			if (auto* npc = RE::TESForm::LookupByID<RE::Actor>(b.npc)) {
				Game::Narrate(std::format("{} remembered that {} still had the {} they lent, due back {} ago, and brought it up.", b.npcName, PlayerName(),
								  Promises::Counted(b.name, b.count), Span(now - b.due)),
					npc);
			}
		}
	}

	json Save()
	{
		std::scoped_lock l(g_lock);
		json borrowed = json::array(), lent = json::array();
		for (auto& b : g_borrowed) {
			borrowed.push_back({ { "npc", b.npc }, { "item", b.item }, { "count", b.count }, { "name", b.name }, { "npcName", b.npcName },
				{ "since", b.since }, { "back", b.back }, { "due", b.due }, { "pickedUp", b.pickedUp }, { "baseline", b.baseline }, { "lastAsked", b.lastAsked } });
		}
		for (auto& x : g_lent) {
			lent.push_back({ { "npc", x.npc }, { "item", x.item }, { "count", x.count }, { "name", x.name } });
		}
		return { { "borrowed", borrowed }, { "lent", lent } };
	}

	void Load(const json& j, const std::function<RE::FormID(RE::FormID)>& resolve)
	{
		std::scoped_lock l(g_lock);
		g_borrowed.clear();
		g_lent.clear();
		auto id = [&](const json& o, const char* key) -> RE::FormID {
			const auto raw = static_cast<RE::FormID>(JsonUtil::Num(o, key).value_or(0));
			return raw ? resolve(raw) : 0;
		};
		if (j.is_object() && j.contains("borrowed") && j["borrowed"].is_array()) {
			for (auto& o : j["borrowed"]) {
				Borrowed b;
				b.npc = id(o, "npc");
				b.item = id(o, "item");
				if (!b.npc || !b.item) {
					continue;
				}
				b.count = static_cast<std::int32_t>(JsonUtil::Num(o, "count").value_or(1));
				b.name = JsonUtil::Str(o, "name");
				b.npcName = JsonUtil::Str(o, "npcName");
				b.since = static_cast<float>(JsonUtil::Num(o, "since").value_or(0));
				b.back = static_cast<float>(JsonUtil::Num(o, "back").value_or(kDefaultHours));
				b.due = static_cast<float>(JsonUtil::Num(o, "due").value_or(0));
				b.pickedUp = JsonUtil::Str(o, "pickedUp", "true") == "true";
				b.baseline = static_cast<std::int32_t>(JsonUtil::Num(o, "baseline").value_or(0));
				b.lastAsked = static_cast<float>(JsonUtil::Num(o, "lastAsked").value_or(-1));
				g_borrowed.push_back(std::move(b));
			}
		}
		if (j.is_object() && j.contains("lent") && j["lent"].is_array()) {
			for (auto& o : j["lent"]) {
				Lent x{ id(o, "npc"), id(o, "item"), static_cast<std::int32_t>(JsonUtil::Num(o, "count").value_or(1)), JsonUtil::Str(o, "name") };
				if (x.npc && x.item) {
					g_lent.push_back(std::move(x));
				}
			}
		}
		logger::info("co-save: {} borrowed, {} lent", g_borrowed.size(), g_lent.size());
	}

	void Revert()
	{
		std::scoped_lock l(g_lock);
		g_borrowed.clear();
		g_lent.clear();
	}

	void Install()
	{
		static ContainerSink sink;
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESContainerChangedEvent>(&sink);
		}
	}
}
