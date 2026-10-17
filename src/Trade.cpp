#include "Trade.h"

#include "Confirm.h"
#include "Game.h"
#include "JsonUtil.h"
#include "Loans.h"
#include "Papyrus.h"
#include "Promises.h"
#include "SkyrimNetBridge.h"

namespace MundaneMatters::Trade
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto kQuoteFresh = 3min;
		constexpr float kVendorRange = 600.0f;

		struct Quoted
		{
			std::string text;
			Clock::time_point at;
		};
		std::mutex g_lock;
		std::unordered_map<RE::FormID, Quoted> g_quotes;

		std::string PlayerName()
		{
			return Game::Player()->GetName();
		}

		// fBarterMax at Speech 0 down to fBarterMin at Speech 100, as the barter menu does.
		float BarterFactor()
		{
			float barterMax = 3.3f, barterMin = 2.0f;
			if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
				if (auto* s = gs->GetSetting("fBarterMax")) {
					barterMax = s->GetFloat();
				}
				if (auto* s = gs->GetSetting("fBarterMin")) {
					barterMin = s->GetFloat();
				}
			}
			const float speech = std::clamp(Game::Player()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech), 0.0f, 100.0f);
			return (std::max)(1.0f, (std::max)(barterMin, barterMax - (barterMax - barterMin) * speech / 100.0f));
		}

		// Where a merchant's goods are: the merchant chest first (vanilla stock and gold), then what
		// they carry themselves.
		std::vector<RE::TESObjectREFR*> Stores(RE::Actor* npc)
		{
			std::vector<RE::TESObjectREFR*> out;
			if (auto* f = npc->GetVendorFaction(); f && f->vendorData.merchantContainer) {
				out.push_back(f->vendorData.merchantContainer);
			}
			out.push_back(npc);
			return out;
		}

		// The merchant's purse: the chest's gold (what the barter menu shows), else their own.
		RE::TESObjectREFR* Till(RE::Actor* npc)
		{
			return Stores(npc).front();
		}

		struct Ware
		{
			RE::TESBoundObject* item = nullptr;
			std::int32_t count = 0;
			double score = 0.0;
			bool stolen = false;  // the player's: not theirs to sell
		};

		bool Sellable(RE::TESBoundObject* obj, RE::InventoryEntryData* entry)
		{
			return obj && obj != Game::Gold() && obj->GetName() && *obj->GetName() && obj->GetPlayable() &&
			       !(entry && (entry->IsQuestObject() || entry->IsWorn()));
		}

		// The merchant's stock matching what was said, best first (one entry per item).
		std::vector<Ware> StockMatching(RE::Actor* npc, const std::string& said)
		{
			std::vector<Ware> out;
			for (auto* store : Stores(npc)) {
				const bool own = store == npc;
				std::unordered_set<RE::TESBoundObject*> worn;
				if (own) {
					for (auto& [obj, entry] : npc->GetInventory()) {
						if (entry.second && entry.second->IsWorn()) {
							worn.insert(obj);
						}
					}
				}
				for (auto& f : Promises::FindItems(store, said)) {
					// Never what they wear, nor what the player lent them: that isn't theirs to sell.
					if (!f.item || f.item == Game::Gold() || !f.item->GetPlayable() || (own && (f.worn || worn.contains(f.item) || Loans::LentCount(npc->GetFormID(), f.item) > 0))) {
						continue;
					}
					auto it = std::ranges::find_if(out, [&](const Ware& w) { return w.item == f.item; });
					if (it == out.end()) {
						out.push_back({ f.item, f.count, f.score });
					} else {
						it->count += f.count;
					}
				}
			}
			std::ranges::sort(out, [](const Ware& a, const Ware& b) { return a.score > b.score; });
			return out;
		}

		// The player's things matching what was said, best first. Worn gear and quest items stay.
		std::vector<Ware> PackMatching(const std::string& said)
		{
			std::vector<Ware> out;
			auto* player = Game::Player();
			auto inv = player->GetInventory();
			for (auto& f : Promises::FindItems(player, said)) {
				auto it = inv.find(f.item);
				auto* entry = it != inv.end() ? it->second.second.get() : nullptr;
				if (!Sellable(f.item, entry)) {
					continue;
				}
				out.push_back({ f.item, f.count, f.score, entry && !entry->IsOwnedBy(player, true) });
			}
			return out;
		}

		// "Apple" with three kinds of apple in stock is still clear when one name fits far better.
		std::vector<Ware> Close(const std::vector<Ware>& all)
		{
			std::vector<Ware> out;
			for (auto& w : all) {
				if (w.score > all.front().score - 0.75) {
					out.push_back(w);
				}
			}
			return out;
		}

		// Does this merchant deal in it? Their faction's buy list (keywords such as VendorItemFood),
		// inverted when the list is what they DON'T buy; fences alone take stolen goods.
		bool Buys(RE::Actor* npc, RE::TESBoundObject* item, bool stolen)
		{
			auto* f = npc->GetVendorFaction();
			if (!f) {
				return false;
			}
			const auto& v = f->vendorData.vendorValues;
			if (stolen ? !v.buysStolen : !v.buysNonStolen) {
				return false;
			}
			auto* list = f->vendorData.vendorSellBuyList;
			if (!list) {
				return true;
			}
			bool listed = list->HasForm(item);
			if (auto* kw = item->As<RE::BGSKeywordForm>(); kw && !listed) {
				kw->ForEachKeyword([&](RE::BGSKeyword* k) {
					if (k && list->HasForm(k)) {
						listed = true;
						return RE::BSContainer::ForEachResult::kStop;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			return v.notBuySell ? !listed : listed;
		}

		// The price for the whole deal: what the NPC agreed to, kept between half and one and a
		// half times the real price, so a misheard number or a smooth tongue can't make it free
		// or ruinous (and buying cheap to sell back never pays).
		std::int32_t Agreed(std::int32_t listTotal, std::int32_t agreed, bool each, std::int32_t n)
		{
			if (agreed <= 0) {
				return listTotal;
			}
			const auto total = each ? agreed * n : agreed;
			return std::clamp(total, (std::max)(1, listTotal / 2), (std::max)(1, (listTotal * 3 + 1) / 2));
		}

		std::int32_t Count(std::int32_t asked, std::int32_t have)
		{
			return asked <= 0 ? have : (std::min)(asked, have);
		}

		void Practise(std::int32_t gold)
		{
			Game::Player()->AddSkillExperience(RE::ActorValue::kSpeech, static_cast<float>(gold));
		}

		// ---- the deal itself (game thread, everything checked again) ----

		void DoBuy(RE::FormID npcId, RE::TESBoundObject* item, std::int32_t n, std::int32_t price)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* player = Game::Player();
			if (!npc || npc->IsDead()) {
				return;
			}
			std::int32_t stock = 0;
			for (auto* s : Stores(npc)) {
				stock += Game::ItemCount(s, item);
			}
			if (stock < n || Game::ItemCount(player, Game::Gold()) < price) {
				Game::Notify("The deal fell through.");
				logger::info("buy: {} x{} for {} no longer possible (stock {}, purse {})", item->GetName(), n, price, stock,
					Game::ItemCount(player, Game::Gold()));
				return;
			}
			player->RemoveItem(Game::Gold(), price, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, Till(npc));
			auto left = n;
			for (auto* s : Stores(npc)) {
				const auto take = (std::min)(left, Game::ItemCount(s, item));
				if (take > 0) {
					s->RemoveItem(item, take, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
					left -= take;
				}
			}
			Practise(price);
			const auto what = Promises::Counted(item->GetName(), n);
			Game::Notify(std::format("Bought {} for {} gold", what, price));
			Game::Narrate(std::format("{} paid {} {} gold and got {}.", PlayerName(), npc->GetName(), price, what), npc);
			logger::info("buy: {} bought {} from {} for {}", PlayerName(), what, npc->GetName(), price);
		}

		void DoSell(RE::FormID npcId, RE::TESBoundObject* item, std::int32_t n, std::int32_t price)
		{
			auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
			auto* player = Game::Player();
			if (!npc || npc->IsDead()) {
				return;
			}
			auto* till = Till(npc);
			if (Game::ItemCount(player, item) < n || Game::ItemCount(till, Game::Gold()) < price) {
				Game::Notify("The deal fell through.");
				logger::info("sell: {} x{} for {} no longer possible", item->GetName(), n, price);
				return;
			}
			player->RemoveItem(item, n, RE::ITEM_REMOVE_REASON::kSelling, nullptr, till);
			till->RemoveItem(Game::Gold(), price, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
			Practise(price);
			const auto what = Promises::Counted(item->GetName(), n);
			Game::Notify(std::format("Sold {} for {} gold", what, price));
			Game::Narrate(std::format("{} sold {} to {} and was paid {} gold.", PlayerName(), what, npc->GetName(), price), npc);
			logger::info("sell: {} sold {} to {} for {}", PlayerName(), what, npc->GetName(), price);
		}

		// Yes/no before the money moves, when "Confirm Paid Services" is on; otherwise straight ahead
		// (asking was consent enough).
		void AskDeal(RE::Actor* npc, bool buying, RE::TESBoundObject* item, std::int32_t n, std::int32_t price)
		{
			const auto npcId = npc->GetFormID();
			auto go = [npcId, buying, item, n, price] {
				buying ? DoBuy(npcId, item, n, price) : DoSell(npcId, item, n, price);
			};
			if (!SkyrimNet::ConfigBool("features.confirm", true) || !Confirm::Available()) {
				go();
				return;
			}
			const std::string accept = buying ? Game::Pick({ "Deal, I'll take it.", "Here's your coin.", "Done. I'll take it." })
			                                  : Game::Pick({ "Deal.", "It's yours.", "Sold." });
			const std::string decline = buying ? Game::Pick({ "On second thought, no.", "Too rich for me.", "I'll pass." })
			                                   : Game::Pick({ "I'll hold on to it.", "Not for that price.", "Maybe another time." });
			Confirm::Offer offer;
			offer.title = buying ? "Buy" : "Sell";
			offer.npc = npc->GetName();
			offer.sameAs = { "trade", "payment" };  // SeverActions' BuyItem / SellItem, if switched on
			offer.detail = Promises::Counted(item->GetName(), n);
			offer.price = price;
			offer.playerGold = Game::ItemCount(Game::Player(), Game::Gold());
			offer.receive = !buying;
			offer.seconds = static_cast<std::int32_t>(SkyrimNet::ConfigFloat("tuning.confirm_seconds", 15.0f));
			offer.acceptLabel = accept;
			offer.declineLabel = decline;
			Confirm::Ask(offer, [npcId, go, accept, decline](Confirm::Choice choice) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || choice == Confirm::Choice::kTimeout) {
					return;  // unanswered: no deal, and nothing said in the player's name
				}
				if (choice == Confirm::Choice::kDecline) {
					Game::Say(decline, npc, true);
					return;
				}
				Game::Say(accept, npc, false);  // the deal's narration makes them answer
				go();
			});
		}

		// "A sword" with five in stock: the player picks which (prices shown); picking is the yes.
		void Picker(RE::Actor* npc, bool buying, const std::vector<Ware>& wares)
		{
			const auto npcId = npc->GetFormID();
			Confirm::Offer offer;
			offer.title = buying ? "Buy" : "Sell";
			offer.npc = npc->GetName();
			offer.sameAs = { "trade", "payment" };  // SeverActions' BuyItem / SellItem, if switched on
			offer.detail = "Which one?";
			offer.showPrice = false;
			offer.seconds = 0;
			offer.acceptLabel = buying ? "Buy" : "Sell";
			offer.declineLabel = "Never mind";
			offer.hint = buying ? "Click an item, then Buy. Prices are each." : "Click an item, then Sell. Prices are each.";
			for (auto& w : wares) {
				if (!buying && !Buys(npc, w.item, w.stolen)) {
					continue;
				}
				offer.pick.push_back({ w.item->GetFormID(), w.item->GetName(), w.count, buying ? BuyPrice(w.item) : SellPrice(w.item), false });
				if (offer.pick.size() >= 30) {
					break;
				}
			}
			if (offer.pick.empty() || !Confirm::Available()) {
				Game::Narrate(std::format("{} couldn't make out which one {} meant.", npc->GetName(), PlayerName()), npc);
				return;
			}
			Confirm::Ask(offer, [npcId, buying](Confirm::Choice choice) {
				auto* npc = RE::TESForm::LookupByID<RE::Actor>(npcId);
				if (!npc || choice != Confirm::Choice::kAccept) {
					return;
				}
				const auto [id, qty] = Confirm::LastPick();
				auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
				if (!item || qty <= 0) {
					return;
				}
				const auto price = (buying ? BuyPrice(item) : SellPrice(item)) * qty;
				const auto purse = Game::ItemCount(buying ? static_cast<RE::TESObjectREFR*>(Game::Player()) : Till(npc), Game::Gold());
				if (purse < price) {
					Game::Notify(buying ? std::format("You need {} gold.", price) : std::format("{} can't afford {} gold.", npc->GetName(), price));
					return;
				}
				buying ? DoBuy(npcId, item, qty, price) : DoSell(npcId, item, qty, price);
			});
		}
	}

	std::int32_t BuyPrice(RE::TESBoundObject* item)
	{
		return item ? (std::max)(1, static_cast<std::int32_t>(std::lround(item->GetGoldValue() * BarterFactor()))) : 0;
	}

	std::int32_t SellPrice(RE::TESBoundObject* item)
	{
		return item ? (std::max)(1, static_cast<std::int32_t>(item->GetGoldValue() / BarterFactor())) : 0;
	}

	bool AsksPrice(const std::string& playerLine)
	{
		static const std::array<std::string_view, 12> kPhrases{
			"how much", "hw much", "how mch", "how mutch", "what do you want for", "what'll you give", "what will you give",
			"would you give", "would you pay", "will you pay", "what do i owe", "what's it worth"
		};
		static const std::unordered_set<std::string> kWords{
			"price", "prices", "pricing", "cost", "costs", "worth", "charge", "sell", "selling", "buy", "buying", "afford"
		};
		const auto lower = JsonUtil::Lower(playerLine);
		if (std::ranges::any_of(kPhrases, [&](std::string_view p) { return lower.find(p) != std::string::npos; })) {
			return true;
		}
		return std::ranges::any_of(JsonUtil::Words(lower), [](const std::string& w) { return kWords.contains(w); });
	}

	void Quote(RE::FormID target, const std::string& playerLine)
	{
		if (!SkyrimNet::ConfigBool("features.trade_talk", true)) {
			return;
		}
		auto* npc = target ? RE::TESForm::LookupByID<RE::Actor>(target) : nullptr;
		if (npc && !npc->GetVendorFaction()) {
			return;  // asked someone who sells nothing: no merchant's prices go into anyone's bio
		}
		if (!npc) {
			// No target in the event: the merchant the player is standing at.
			float best = kVendorRange;
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& h : lists->highActorHandles) {
					auto a = h.get();
					if (a && !a->IsDead() && a->GetVendorFaction()) {
						if (const float d = Game::DistanceToPlayer(a.get()); d < best) {
							best = d;
							npc = a.get();
						}
					}
				}
			}
		}
		if (!npc) {
			return;
		}
		const std::string player = PlayerName();
		std::string note;
		if (auto stock = StockMatching(npc, playerLine); !stock.empty()) {
			std::string list;
			for (auto& w : Close(stock)) {
				list += std::format("{}{} {} gold each ({} in stock)", list.empty() ? "" : "; ", w.item->GetName(), BuyPrice(w.item), w.count);
				if (list.size() > 300) {
					break;
				}
			}
			note = std::format("{} just asked about prices. For sale, at what {} would really pay: {}. Quote these; a good haggler might talk a price down a little.",
				player, player, list);
		}
		// What they'd pay for the player's own: only when selling is what the line is about (or
		// nothing in stock fits), so "how much for an apple?" doesn't also price the player's apples.
		static const std::array<std::string_view, 10> kSelling{ "sell", "buy", "would you", "will you", "give", "pay", "worth", "offer", "want th", "interested" };
		const auto lower = JsonUtil::Lower(playerLine);
		const bool selling = std::ranges::any_of(kSelling, [&](std::string_view w) { return lower.find(w) != std::string::npos; });
		if (auto pack = selling || note.empty() ? PackMatching(playerLine) : std::vector<Ware>{}; !pack.empty()) {
			std::string list;
			for (auto& w : Close(pack)) {
				const bool deals = Buys(npc, w.item, w.stolen);
				list += std::format("{}{}: {}", list.empty() ? "" : "; ", w.item->GetName(),
					deals             ? std::format("would pay {} gold each", SellPrice(w.item)) :
					w.stolen          ? std::string("won't touch it, it's stolen") :
										std::string("doesn't deal in it"));
				if (list.size() > 300) {
					break;
				}
			}
			note += std::format("{}For {}'s own: {}. Has {} gold to spend.", note.empty() ? "" : " ", player, list,
				Game::ItemCount(Till(npc), Game::Gold()));
		}
		if (note.empty()) {
			note = std::format("{} just asked about prices (\"{}\"). Has nothing like that for sale: if they named something, say so; the rest of the stock is in the trade window.",
				player, playerLine);
		}
		logger::info("quote for {}: {}", npc->GetName(), note);
		std::scoped_lock l(g_lock);
		g_quotes[npc->GetFormID()] = { std::move(note), Clock::now() };
	}

	std::string QuoteLine(RE::FormID npc)
	{
		std::scoped_lock l(g_lock);
		auto it = g_quotes.find(npc);
		if (it == g_quotes.end() || Clock::now() - it->second.at > kQuoteFresh) {
			return "";
		}
		return it->second.text;
	}

	bool Buy(RE::Actor* npc, const std::string& item, std::int32_t qty, std::int32_t agreed, bool each)
	{
		if (!npc || !npc->GetVendorFaction()) {
			return false;
		}
		const std::string name = npc->GetName();
		// Nothing named ("I'll buy something"): the shelves are in the trade window.
		if (JsonUtil::Words(item).empty()) {
			return Papyrus::CallMethod(npc, "Actor", "ShowBarterMenu", nullptr);
		}
		const auto stock = StockMatching(npc, item);
		if (stock.empty()) {
			Game::Notify(std::format("{} has no {} for sale", name, item));
			Game::Narrate(std::format("{} looked through their stock: no {} for sale.", name, item), npc);
			return true;
		}
		const auto close = Close(stock);
		if (close.size() > 1) {
			Picker(npc, true, close);
			return true;
		}
		const auto& w = close.front();
		const auto n = Count(qty, w.count);
		if (qty > w.count) {
			Game::Notify(std::format("{} has only {}", name, Promises::Counted(w.item->GetName(), w.count)));
		}
		const auto price = Agreed(BuyPrice(w.item) * n, agreed, each, n);
		const auto purse = Game::ItemCount(Game::Player(), Game::Gold());
		if (purse < price) {
			Game::Notify(std::format("You need {} gold for {}.", price, Promises::Counted(w.item->GetName(), n)));
			Game::Narrate(std::format("{} checked their purse: only {} gold, not the {} gold it costs.", PlayerName(), purse, price), npc);
			return true;
		}
		AskDeal(npc, true, w.item, n, price);
		return true;
	}

	bool Sell(RE::Actor* npc, const std::string& item, std::int32_t qty, std::int32_t agreed, bool each)
	{
		if (!npc || !npc->GetVendorFaction()) {
			return false;
		}
		const std::string name = npc->GetName();
		if (JsonUtil::Words(item).empty()) {
			return Papyrus::CallMethod(npc, "Actor", "ShowBarterMenu", nullptr);
		}
		const auto pack = PackMatching(item);
		if (pack.empty()) {
			Game::Notify(std::format("You have no {} to sell (worn gear stays on)", item));
			return true;
		}
		const auto close = Close(pack);
		if (close.size() > 1) {
			Picker(npc, false, close);
			return true;
		}
		const auto& w = close.front();
		if (!Buys(npc, w.item, w.stolen)) {
			Game::Notify(w.stolen ? std::format("{} won't buy stolen goods", name) : std::format("{} doesn't deal in {}", name, w.item->GetName()));
			Game::Narrate(w.stolen ? std::format("{} recognised the {} as stolen and wanted nothing to do with it.", name, w.item->GetName())
			                       : std::format("{} had no use for {}; it isn't the kind of thing they trade in.", name, w.item->GetName()),
				npc);
			return true;
		}
		const auto n = Count(qty, w.count);
		const auto price = Agreed(SellPrice(w.item) * n, agreed, each, n);
		const auto till = Game::ItemCount(Till(npc), Game::Gold());
		if (till < price) {
			Game::Notify(std::format("{} has only {} gold", name, till));
			Game::Narrate(std::format("{} counted their coin: only {} gold, not the {} gold that would cost.", name, till, price), npc);
			return true;
		}
		AskDeal(npc, false, w.item, n, price);
		return true;
	}

	void Reset()
	{
		std::scoped_lock l(g_lock);
		g_quotes.clear();
	}
}
