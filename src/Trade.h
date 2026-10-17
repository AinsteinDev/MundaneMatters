#pragma once

// Buying and selling by talking: "how much for an apple?" gets the real price, "I'll take 3" makes
// the deal, "want these pelts?" sells them. The AI does the talking and the haggling; the price,
// the stock and the gold are always the game's own.
namespace MundaneMatters::Trade
{
	// What the player pays a merchant for one, at their Speech (vanilla barter formula).
	std::int32_t BuyPrice(RE::TESBoundObject* item);

	// What a merchant pays the player for one (vanilla: the item's worth over the same factor).
	std::int32_t SellPrice(RE::TESBoundObject* item);

	// Any thread. The player's line asks what something costs, or what it would fetch.
	bool AsksPrice(const std::string& playerLine);

	// Game thread. The player asked a price: look it up in the merchant's real stock (and the
	// player's pack, for what they'd pay) so the NPC quotes the truth. `target` 0 = the vendor
	// the player is talking to (the nearest one).
	void Quote(RE::FormID target, const std::string& playerLine);

	// Any thread. The price note for the NPC's bio, while it's fresh; "" otherwise.
	std::string QuoteLine(RE::FormID npc);

	// Game thread, after the NPC agreed. `item` as said ("apple"), `qty` (-1 = all of them),
	// `agreed` the gold the NPC agreed to (0 = their usual price), `each` when that was per piece.
	bool Buy(RE::Actor* npc, const std::string& item, std::int32_t qty, std::int32_t agreed, bool each);
	bool Sell(RE::Actor* npc, const std::string& item, std::int32_t qty, std::int32_t agreed, bool each);

	// A save was loaded: forget quotes.
	void Reset();
}
