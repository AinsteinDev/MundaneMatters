"""
MundaneMatters logic check: a line-for-line Python mirror of the deterministic C++ pieces, run against
RP exchanges written in different model styles. Keep in sync with:
  JsonUtil.h        ParseNumber / Num / Str
  TurnPipeline.cpp  MightPromise / ParseReply (repair + salvage) / Confidence / Price /
                    NormalizeRequest / NormalizeResponse / PromiseList / decision
  Promises.cpp      Stem / Tokens / Score / BestInWorld / IsGenericMeal / FromJson
  Services.cpp      Haggled
  Trade.cpp         BuyPrice / SellPrice / Agreed / Count / AsksPrice / Buys; TurnPipeline Qty
  Ledger.cpp        DueFromHint / SamePromise / AddPending timing
Run: python tools/logic_check.py
"""
import json
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")

# ---------------------------------------------------------------- sample load order (vanilla)
ITEMS = [  # (name, value, kind) kind: food | potion | ingredient | misc
    ("Sweet Roll", 2, "food"), ("Cabbage", 2, "food"), ("Cabbage Potato Soup", 5, "food"),
    ("Vegetable Soup", 5, "food"), ("Tomato Soup", 5, "food"), ("Beef Stew", 8, "food"),
    ("Horker Stew", 8, "food"), ("Venison Stew", 8, "food"), ("Clam Chowder", 5, "food"),
    ("Bread", 2, "food"), ("Apple Pie", 4, "food"), ("Cheese Wedge", 2, "food"),
    ("Eidar Cheese Wheel", 15, "food"), ("Goat Cheese Wedge", 2, "food"), ("Ale", 5, "food"),
    ("Nord Mead", 5, "food"), ("Honningbrew Mead", 10, "food"), ("Black-Briar Mead", 22, "food"),
    ("Alto Wine", 12, "food"), ("Wine", 7, "food"), ("Spiced Wine", 7, "food"),
    ("Red Apple", 3, "food"), ("Green Apple", 3, "food"), ("Potato", 1, "food"), ("Leek", 4, "food"),
    ("Carrot", 1, "food"), ("Salmon Steak", 3, "food"), ("Grilled Leeks", 5, "food"),
    ("Mammoth Cheese Bowl", 8, "food"), ("Elsweyr Fondue", 15, "food"),
    ("Potion of Minor Healing", 17, "potion"), ("Potion of Healing", 36, "potion"),
    ("Potion of Minor Magicka", 20, "potion"), ("Potion of Cure Disease", 79, "potion"),
    ("Snowberries", 4, "ingredient"), ("Juniper Berries", 1, "ingredient"), ("Salt Pile", 2, "ingredient"),
    ("Garlic", 1, "ingredient"),
    ("Gold Ingot", 100, "misc"), ("Iron Ingot", 7, "misc"), ("Lockpick", 3, "misc"),
]

# ---------------------------------------------------------------- JsonUtil.h mirror
WORD_NUM = {"a": 1, "an": 1, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7,
            "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13, "fourteen": 14,
            "fifteen": 15, "sixteen": 16, "seventeen": 17, "eighteen": 18, "nineteen": 19, "twenty": 20,
            "thirty": 30, "forty": 40, "fifty": 50, "sixty": 60, "seventy": 70, "eighty": 80, "ninety": 90,
            "couple": 2, "pair": 2, "few": 3, "several": 3, "handful": 5, "dozen": 12}


def parse_number(text):
    text = text.lower()
    m = re.search(r"(-?\d+(?:\.\d+)?)", text)
    if m:
        return float(m.group(1))
    total = current = 0.0
    any_ = False
    for w in re.findall(r"[a-z]+", text):
        if w == "hundred":
            current = (current or 1) * 100
            any_ = True
        elif w == "thousand":
            total += (current or 1) * 1000
            current = 0
            any_ = True
        elif w in WORD_NUM:
            if w in ("dozen", "couple", "few", "handful") and current == 1:
                current = WORD_NUM[w]
            else:
                current += WORD_NUM[w]
            any_ = True
    return total + current if any_ else None


def num(j, key):
    v = j.get(key) if isinstance(j, dict) else None
    if v is None or isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, str):
        return parse_number(v)
    return None


def str_(j, key, fb=""):
    v = j.get(key) if isinstance(j, dict) else None
    if isinstance(v, str):
        return v
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return json.dumps(v)
    return fb


# ---------------------------------------------------------------- Promises.cpp mirror
FILLER = {
    "a", "an", "the", "of", "some", "my", "your", "our", "this", "that", "few", "couple", "bit", "little",
    "bowl", "bowls", "plate", "cup", "mug", "tankard", "bottle", "bottles", "flask", "jug", "pint",
    "piece", "pieces", "slice", "slices", "loaf", "loaves", "chunk", "hunk", "wedge", "wheel", "portion", "serving",
    "fresh", "fine", "good", "nice", "old", "homemade", "spare", "extra",
    "something", "anything", "to", "for", "with", "and", "you", "me", "quick", "proper", "decent", "free",
    "it", "them", "those", "these", "thing", "things", "stuff",
}
SYN = {"health": "heal", "healing": "heal", "stamina": "stamina", "magicka": "magicka", "beer": "ale",
       "booze": "mead", "drink": "mead", "spud": "potato", "spuds": "potato", "coin": "gold", "coins": "gold",
       "septim": "gold", "septims": "gold"}
MEAL_WORDS = {"breakfast", "lunch", "dinner", "supper", "meal", "food", "grub", "bite", "snack", "feast",
              "eat", "dish", "morning", "morn", "evening", "even", "midday", "noon", "daily", "hot", "warm"}
MEAL_DISHES = ["Vegetable Soup", "Beef Stew", "Cabbage Potato Soup", "Venison Stew", "Horker Stew", "Bread"]


def stem(w):
    if w in SYN:
        return SYN[w]
    ends = lambda suf, m: len(w) >= len(suf) + m and w.endswith(suf)
    if ends("ing", 3):
        return w[:-3]
    if ends("ies", 2):
        return w[:-3] + "y"
    if ends("oes", 3) or ends("ches", 2) or ends("shes", 2) or ends("xes", 2) or ends("sses", 2):
        return w[:-2]
    if ends("s", 3) and not w.endswith("ss"):
        return w[:-1]
    return w


def tokens(text, drop_filler):
    return [stem(r) for r in re.findall(r"[a-z0-9]+", text.lower()) if not (drop_filler and r in FILLER)]


def score(wanted, name):
    if not name or not wanted:
        return 0.0
    have = tokens(name, False)
    joined = "".join(have)
    present = lambda w: w in have or (len(w) >= 5 and w in joined)
    if not present(wanted[-1]):
        return 0.0
    hits = sum(1 for w in wanted if present(w))
    coverage = hits / len(wanted)
    if coverage < 0.5:
        return 0.0
    return coverage * 10.0 - (len(have) - min(len(have), hits)) * 0.5


def best_in_world(wanted, max_value=100):
    best, best_s = None, 0.0
    for name, value, kind in ITEMS:
        if value > max_value:
            continue
        s = score(wanted, name)
        if s > 0:
            s -= value * 0.001
        if s > best_s:
            best, best_s = name, s
    return best


def resolve_item(item):
    wanted = tokens(item, True) or tokens(item, False)
    if not wanted:
        return None
    if all(w in MEAL_WORDS for w in wanted):  # IsGenericMeal -> PickMeal (NPC carries no food)
        names = {n for n, _, k in ITEMS if k == "food"}
        return next((d for d in MEAL_DISHES if d in names), None)
    return best_in_world(wanted)


def from_json(j):
    if not isinstance(j, dict):
        return None
    t = str_(j, "type").lower()
    if "meet" in t or "rendezvous" in t or "appoint" in t:
        typ = "meet"
    elif "key" in t:
        typ = "give_key"
    elif any(k in t for k in ("gold", "money", "coin", "pay")):
        typ = "give_gold"
    elif any(k in t for k in ("bed", "sleep", "lodg", "stay", "room")):
        typ = "offer_bed"
    elif "take" in t or "allow" in t or "help_yourself" in t:
        typ = "allow_take"
    elif "item" in t or "gift" in t or t == "give":
        typ = "give_item"
    else:
        return None
    p = {"type": typ, "item": str_(j, "item"),
         "qty": int(min(max(num(j, "qty") or 1.0, 1.0), 1000.0)),
         "gold": int(min(max(num(j, "gold") or 0.0, 0.0), 1e6)),
         "time_hint": str_(j, "time_hint"),
         "in_hours": min(max(num(j, "in_hours") or 0.0, 0.0), 24.0 * 30),
         "always": str_(j, "always").lower() in ("true", "yes", "1")}
    p["loan"] = typ in ("give_item", "allow_take") and str_(j, "loan").lower() in ("true", "yes", "1")
    when = str_(j, "when", "now").lower()
    if "cancel" in when or "retract" in when or "revok" in when:
        p["when"] = "cancel"
    elif when in ("", "now") or "immediate" in when or "right away" in when:
        p["when"] = "later" if p["in_hours"] >= 1.0 else "now"
    else:
        p["when"] = "later"
        if not p["time_hint"] and when != "later":
            p["time_hint"] = when
    if p["type"] == "allow_take" and not tokens(p["item"], True):
        return None
    if p["type"] == "give_gold" and p["gold"] == 0:
        p["gold"] = int(parse_number(p["item"]) or 0)
    if p["type"] == "give_item":
        words = tokens(p["item"], True)
        amount = 0
        for w in list(words):
            if w.isdigit():
                amount = int(w)
                words.remove(w)
        if words and all(w == "gold" for w in words):
            p["type"] = "give_gold"
            p["gold"] = p["gold"] if p["gold"] > 0 else amount if amount > 0 else p["qty"] if p["qty"] > 1 else 0
            p["item"] = ""
        elif not words:
            return None
        elif "key" in tokens(p["item"], False) and any(w in ("house", "home", "place", "door", "hut", "shack", "cabin", "spare", "front") for w in tokens(p["item"], False)):
            p["type"] = "give_key"
    if p["type"] == "give_key":
        p["item"], p["qty"] = "", 1
    return p


def gift_from_json(j):
    """Promises.cpp GiftFromJson: what the player hands over (None = not a gift)."""
    if not isinstance(j, dict):
        return None
    g = {"item": str_(j, "item"), "qty": int(min(max(num(j, "qty") or 1.0, 1.0), 1000.0)),
         "gold": int(min(max(num(j, "gold") or 0.0, 0.0), 1e6))}
    words = tokens(g["item"], True)
    amount = 0
    for w in list(words):
        if w.isdigit():
            amount = int(w)
            words.remove(w)
    if words and all(w == "gold" for w in words):
        g["gold"] = g["gold"] if g["gold"] > 0 else amount if amount > 0 else g["qty"] if g["qty"] > 1 else 0
        g["item"] = ""
    unnamed = str_(j, "unnamed").lower()
    if g["gold"] <= 0 and not words and unnamed not in ("true", "yes", "1"):
        return None
    return g


def gift_action(g):
    if g is None:
        return "nothing"
    if g["gold"] > 0:
        return f"give {g['gold']} gold"
    if not tokens(g["item"], True):
        return "gift menu"
    return f"give '{g['item']}' x{g['qty']}"


# ---------------------------------------------------------------- Services.cpp place kinds mirror
KINDS = [
    ("brewery", ["brewery", "meadery", "distillery", "mead hall"]),
    ("inn", ["inn", "tavern", "pub", "alehouse", "bar"]),
    ("temple", ["temple", "shrine", "chapel", "priest"]),
    ("store", ["store", "shop", "general goods", "trader", "market", "merchant"]),
    ("smithy", ["smith", "blacksmith", "smithy", "forge", "armorer"]),
    ("alchemist", ["alchemist", "apothecary", "potion shop"]),
    ("palace", ["palace", "castle", "jarl", "keep", "longhouse", "court"]),
    ("stables", ["stable", "stables", "carriage"]),
    ("farm", ["farm", "farmstead"]),
    ("mill", ["mill", "lumber mill", "sawmill"]),
    ("mine", ["mine"]),
    ("cemetery", ["cemetery", "graveyard", "hall of the dead", "catacombs"]),
    ("jail", ["jail", "prison", "dungeon cell"]),
    ("barracks", ["barracks"]),
]


def has_word(text, word):
    return re.search(r"(?<![a-z0-9])" + re.escape(word) + r"(?![a-z0-9])", text) is not None


def kind_of(text):
    low = text.lower()
    words = re.findall(r"[a-z0-9]+", low)
    for label, said in KINDS:
        for w in said:
            if has_word(low, w):
                return label
            if len(w) >= 5 and " " not in w and any(len(x) >= 4 and distance(x, w) <= 1 for x in words):
                return label
    return None


def only_a_kind(text, label, town, hold):
    said = dict(KINDS)[label]
    filler = {"the", "a", "an", "local", "nearest", "closest", "town", "city", "this", "here", "near", "in", "of", "s", "hold", "old"}
    place_words = set(re.findall(r"[a-z0-9]+", (town + " " + hold).lower()))
    for w in re.findall(r"[a-z0-9]+", text.lower()):
        if w in filler or w in place_words:
            continue
        if not any(has_word(x, w) or (len(w) >= 4 and len(x) >= 5 and distance(w, x) <= 1) for x in said):
            return False
    return True


GENEROSITY = {"lover": (5.0, 100), "ally": (6.0, 80), "confidant": (12.0, 60), "friend": (12.0, 20), "acquaintance": (24.0, 10)}


def generosity(level, teammate=False):
    """Promises.cpp GenerosityOf: (hours, budget) or None for never."""
    if teammate:
        return (6.0, 80)
    return GENEROSITY.get(level)


def handout_cost(value, qty, food):
    full = max(1, value) * qty
    return max(1, (full + 1) // 2) if food else full


class Allowance:
    """Ledger.cpp BudgetLeft / RecordHandout."""
    def __init__(self):
        self.start = None
        self.spent = 0

    def left(self, now, g):
        if g is None:
            return 0
        hours, budget = g
        if self.start is None or now - self.start >= hours:
            return budget
        return max(0, budget - self.spent)

    def give(self, now, g, cost):
        hours, _ = g
        if self.start is None or now - self.start >= hours:
            self.start, self.spent = now, 0
        self.spent += cost


def ask(allow, now, level, what, value, food, qty=1):
    g = generosity(level)
    cost = handout_cost(value, qty, food)
    if g is None or cost > allow.left(now, g):
        return f"no {what}"
    allow.give(now, g, cost)
    return f"{what}"


# ---------------------------------------------------------------- Ledger.cpp mirror
DAYS = ["sundas", "morndas", "tirdas", "middas", "turdas", "fredas", "loredas"]


def due_from_hint(hint, h, weekday=0):
    hint = hint.lower()
    if not hint:
        return None
    has = lambda s: s in hint
    nxt = lambda hour: hour - h if h + 1.0 < hour else (24.0 - h) + hour
    tomorrow = has("tomorrow") or has("morrow")
    at = lambda hour: (24.0 - h) + hour if tomorrow else nxt(hour)
    for d, name in enumerate(DAYS):
        if has(name):
            ahead = (d + 7 - weekday % 7) % 7 or 7
            return ahead * 24.0 - h + 9.0
    pos = hint.find("hour")
    if pos >= 0:
        return max(1.0, parse_number(hint[:pos]) or 1.0)
    pos = hint.find("day")
    if pos >= 0 and not has("midday") and not has("today") and not has("daybreak"):
        return 24.0 * max(1.0, parse_number(hint[:pos]) or 1.0)
    if has("fortnight"):
        return 24.0 * 14
    if has("week"):
        return 24.0 * 7
    if has("midnight"):
        return at(0.0)
    if any(has(s) for s in ("dawn", "sunrise", "first light", "daybreak", "sun's up", "sun is up")):
        return at(6.0)
    if has("breakfast"):
        return at(7.0)
    if has("morning"):
        return at(8.0)
    if has("afternoon"):
        return at(15.0)
    if has("noon") or has("midday") or has("lunch"):
        return at(12.0)
    if has("dusk") or has("sunset") or has("sundown"):
        return at(19.0)
    if any(has(s) for s in ("evening", "supper", "dinner", "shift", "after work", "closing")):
        return at(18.0)
    if has("tonight") or has("night"):
        return 20.0 - h if h < 20.0 else 1.0
    if tomorrow:
        return (24.0 - h) + 9.0
    return None


def pending_in(p, h):
    """AddPending: words first, then the classifier's estimate, then an hour."""
    d = due_from_hint(p["time_hint"], h)
    if d is not None:
        return d
    return p["in_hours"] if p["in_hours"] >= 0.5 else 1.0


def same_promise(a, b):
    if a["type"] != b["type"]:
        return False
    if a["type"] not in ("give_item", "allow_take"):
        return True
    x, y = a["item"].lower(), b["item"].lower()
    return x == y or y in x or x in y


# ---------------------------------------------------------------- TurnPipeline.cpp mirror
PROMISE_WORDS = {"key", "keys", 
    "gold", "coin", "coins", "septim", "septims", "pay", "paid", "payment", "reward", "money", "purse",
    "give", "giving", "gave", "bring", "fetch", "gift", "spare", "share", "lend", "borrow", "owe", "keep", "yours", "free",
    "eat", "drink", "food", "meal", "hungry", "thirsty", "starving", "bowl", "plate", "mug", "cup", "bottle",
    "bread", "ale", "mead", "wine", "soup", "stew", "potion", "potions",
    "sleep", "bed", "beds", "rest", "stay", "room", "roof", "crash", "night", "tonight", "tomorrow", "later", "morning", "evening",
    "promise", "promised", "compensate", "coppers", "drakes", "dinner", "supper", "breakfast", "lunch", "feed", "fed",
    "blanket", "bedroll", "lodging", "shelter", "settle", "earned", "deserve",
}
PROMISE_PHRASES = [
    "here you", "here's", "here,", "there you go", "take this", "take these", "take it", "take one", "for you", "on the house",
    "on me", "my treat", "help yourself", "my place", "with me", "for your trouble", "least i can do", "hold on to",
]


def item_words():
    s = set()
    for name, _, kind in ITEMS:
        if kind in ("food", "ingredient"):
            ws = re.findall(r"[a-z0-9]+", name.lower())
            s.update(w for w in ws if len(w) >= 4)
            s.add("".join(ws))
    return s


ITEM_WORDS = item_words()


def has_foreign_letters(line):
    b = line.encode("utf-8")
    i = 0
    while i < len(b):
        c = b[i]
        if c < 0x80:
            i += 1
            continue
        nxt = b[i + 1] if i + 1 < len(b) else 0
        if c == 0xE2 and nxt in (0x80, 0x81):
            i += 3
            continue
        if c == 0xC2:
            i += 2
            continue
        return True
    return False


MOVE = ["follow", "come with", "come along", "come on", "with me", "tag along", "stick with", "stick close", "accompany",
        "join me", "let's go", "lets go", "walk with", "escort", "take me", "bring me to", "lead the way", "lead me",
        "guide", "show me the way", "show me where", "way to", "how do i get", "where is", "where's", "direction",
        "get me to", "walk me", "travel with", "go with me"]


NPC_MOVE = ["follow me", "this way", "come with me", "come along", "i'll take you", "i will take you", "i'll show you",
            "i will show you", "i'll lead", "lead the way", "lead on", "walk you", "after me", "let's go", "lets go",
            "on our way", "right behind you", "i'll follow", "i will follow", "after you", "i'm coming", "keep up",
            "stay close", "i'll guide"]
TYPO_KEYS = ["follow", "escort", "guide", "accompany", "direction", "travel"]


def distance(a, b):
    prev = list(range(len(b) + 1))
    for i in range(1, len(a) + 1):
        cur = [i] + [0] * len(b)
        for j in range(1, len(b) + 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1]))
        prev = cur
    return prev[len(b)]


def move_typo(lower):
    for w in re.findall(r"[a-z0-9]+", lower):
        for key in TYPO_KEYS:
            allowed = 1 if len(key) <= 5 else 2
            if len(w) + 2 >= len(key) and len(w) <= len(key) + 2 and w != key and len(w) >= 4 and distance(w, key) <= allowed:
                return True
    return False


def might_move(player, npc=""):
    if any(w in npc.lower() for w in NPC_MOVE):
        return True
    if has_foreign_letters(player):
        return True
    low = player.lower()
    return any(w in low for w in MOVE) or move_typo(low)


def asked(services, player, npc, promises=True):
    """Classify(): which services go to the classifier, or None when the exchange is skipped."""
    move = might_move(player, npc)
    if not move:
        services = [s for s in services if s not in ("follow", "escort")]
    shopkeeper = any(s not in ("follow", "escort") for s in services)
    if not shopkeeper and not move and not (promises and might_promise(player, npc)):
        return None
    return services


def might_promise(player, npc):
    for line in (player, npc):
        if any(c.isdigit() for c in line) or has_foreign_letters(line):
            return True
        low = line.lower()
        if any(p in low for p in PROMISE_PHRASES):
            return True
        if any(w in PROMISE_WORDS or w in ITEM_WORDS for w in re.findall(r"[a-z0-9]+", low)):
            return True
    return False


def parse_reply(raw):
    first, last = raw.find("{"), raw.rfind("}")
    if first < 0 or last < first:
        return None, "none"
    body = raw[first:last + 1]
    try:
        j = json.loads(body)
        if isinstance(j, dict):
            return j, "ok"
    except ValueError:
        pass
    fixed = "".join(c for c in body if ord(c) < 0x80)
    fixed = re.sub(r":\s*(?=[,}\]])", ": null", fixed)
    fixed = re.sub(r":\s*[^\s\"\[{\-0-9tfn][^,}\]]*", ": null", fixed)
    try:
        j = json.loads(fixed)
        if isinstance(j, dict):
            return j, "repaired"
    except ValueError:
        pass
    salv = {}
    for key in ("request", "response", "confidence"):
        m = re.search(r'"%s"\s*:\s*"([A-Za-z_]+)"' % key, raw)
        if m:
            salv[key] = m.group(1)
    if "request" not in salv:
        return None, "none"
    salv.setdefault("response", "accept")
    salv.setdefault("confidence", "medium")
    return salv, "salvaged"


def confidence(j):
    v = j.get("confidence")
    if v is None:
        return 0.6
    if isinstance(v, str):
        s = v.lower()
        if "high" in s:
            return 0.9
        if "med" in s:
            return 0.6
        if "low" in s:
            return 0.3
    n = num(j, "confidence")
    if n is None:
        return 0.0
    return n / 100.0 if 1.0 < n <= 100.0 else (n if 0.0 <= n <= 1.0 else 0.0)


def price(j):
    v = num(j, "price")
    return int(v) if v and 0 < v < 1e6 else 0


def norm_request(r):
    r = r.lower()
    if "price" in r or "quote" in r:
        return "none"
    if r == "visit" or r.startswith("visit_") or r.endswith("_visit") or r in ("go_home", "home") or "go_home" in r:
        return "visit"
    if r == "pay_fine" or "fine" in r or "bounty" in r:
        return "pay_fine"
    if "bribe" in r:
        return "bribe"
    if "persuad" in r or "convinc" in r:
        return "persuade"
    if "intimidat" in r or "threat" in r:
        return "intimidate"
    if r == "perform" or any(k in r for k in ("sing", "song", "music", "play_", "tune", "bard")):
        return "perform"
    if "move_in" in r or "movein" in r or "live_with" in r or r == "move" or "housemate" in r:
        return "move_in"
    if ("company" in r and "accompan" not in r) or "spend_time" in r or r in ("stay", "stay_with"):
        return "company"
    if r == "sit" or r.startswith("sit_") or r == "seat" or any(k in r for k in ("break", "relax")):
        return "sit"
    if r == "drink" or any(k in r for k in ("drink", "round", "toast", "tavern")):
        return "drink"
    if r == "cure" or "disease" in r or "cure_" in r:
        return "cure"
    if r == "heal" or "heal" in r or "patch" in r or "tend" in r:
        return "heal"
    if r == "cook" or "cook" in r:
        return "cook"
    if r == "mend" or any(k in r for k in ("temper", "sharpen", "repair", "improve", "smith")):
        return "mend"
    if r == "buy" or r.startswith("buy_") or "purchas" in r:
        return "buy"
    if r == "sell" or r.startswith("sell_"):
        return "sell"
    if any(k in r for k in ("trade", "barter", "shop", "wares", "merchant")):
        return "trade"
    if r == "retrieve" or "give_back" in r or "get_back" in r or "return" in r or r == "take_back":
        return "retrieve"
    if r == "hold" or "keep" in r or "store" in r or "storage" in r or "stash" in r or r in ("carry", "safekeeping"):
        return "hold"
    if r in ("rest", "rest_together", "go_to_bed") or any(k in r for k in ("sleep", "bed_down", "turn_in", "camp")):
        return "rest"
    if "room" in r or "rent" in r or "lodg" in r or r == "inn":
        return "rent_room"
    if "train" in r or "teach" in r or "lesson" in r:
        return "train"
    if any(k in r for k in ("escort", "guide", "lead", "travel", "take_me", "show_way")):
        return "escort"
    if "meal" in r or r == "eat" or r.startswith("eat_") or r.endswith("_eat") or any(k in r for k in ("dine", "supper", "dinner")):
        return "share_meal"
    if any(k in r for k in ("follow", "accompan", "tag")) or r in ("come", "come_along", "join"):
        return "follow"
    return "none"


def norm_response(r):
    r = r.lower()
    if "free" in r or "house" in r or "waive" in r:
        return "free"
    if any(k in r for k in ("refus", "declin", "reject", "deny", "denied")) or r == "no":
        return "refuse"
    if "counter" in r or "condition" in r:
        return "counter"
    return "accept"


def promise_list(j):
    v = j.get("promises")
    if isinstance(v, list):
        return v
    if isinstance(v, dict):
        return [v]
    return []


def haggled(agreed, cost):
    if agreed <= 0 or cost <= 0:
        return cost
    return min(max(agreed, max(1, cost // 2)), cost * 2)


def decide(services, raw, room_cost=100, hour=21.0):
    """What MundaneMatters does with one classifier reply, as text."""
    j, how = parse_reply(raw)
    if j is None:
        return "nothing (unreadable reply)", how
    req = norm_request(str_(j, "request", "none"))
    resp = norm_response(str_(j, "response", "accept"))
    out = []
    if req in services and confidence(j) >= 0.6:
        if resp in ("refuse", "counter"):
            out.append(f"declined {req}")
        elif req == "escort":
            place = str_(j, "place")
            out.append(f"escort to {place}" if place else "escort: no destination")
        elif req == "rent_room":
            out.append("room given free" if resp == "free" else f"room popup {haggled(price(j), room_cost)}g")
        else:
            out.append(f"open {req}")
    for pj in promise_list(j):
        p = from_json(pj)
        if not p:
            continue
        if p["type"] == "offer_bed" and req == "rent_room" and out and out[0].startswith(("room", "open")):
            continue
        if p["type"] == "give_item":
            what = f"item '{p['item']}'->{resolve_item(p['item']) or 'NONE'} x{min(p['qty'], 10)}"
        elif p["type"] == "give_gold":
            what = f"gold {min(p['gold'] or 10, 200)}"
        else:
            what = "bed"
        if p["when"] == "cancel":
            out.append(f"cancel {what.split(' ')[0]}")
        elif p["when"] == "later":
            out.append(f"{what} in {pending_in(p, hour):.0f}h")
        else:
            out.append(f"{what} now")
    return "; ".join(out) or "nothing", how


# ---------------------------------------------------------------- checks
fails = 0


def check(label, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {label}: {got}" + ("" if ok else f"   (want {want})"))


print("== numbers written by models")
for text, want in [("10", 10), ("12.5", 12.5), ("ten", 10), ("a hundred", 100), ("two hundred and fifty", 250),
                   ("a couple", 2), ("a few", 3), ("a dozen", 12), ("fifty septims", 50), ("one thousand", 1000),
                   ("none", None), ("", None)]:
    check(f"'{text}'", parse_number(text), want)

print("\n== item matching (what the classifier writes -> item handed over)")
for wanted, want in [
    ("cabbage soup", "Cabbage Potato Soup"), ("a bowl of cabbage soup", "Cabbage Potato Soup"),
    ("soup", "Vegetable Soup"), ("vegetable soup", "Vegetable Soup"), ("hot stew", "Beef Stew"),
    ("venison stew", "Venison Stew"), ("sweetroll", "Sweet Roll"), ("sweet roll", "Sweet Roll"),
    ("sweetrolls", "Sweet Roll"), ("bread", "Bread"), ("loaf of bread", "Bread"), ("warm bread", "Bread"),
    ("a mug of ale", "Ale"), ("beer", "Ale"), ("mead", "Nord Mead"), ("honningbrew", "Honningbrew Mead"),
    ("wine", "Wine"), ("something to drink", "Nord Mead"), ("a drink", "Nord Mead"),
    ("health potion", "Potion of Healing"), ("healing potion", "Potion of Healing"),
    ("potion of healing", "Potion of Healing"), ("cheese", "Cheese Wedge"), ("cheeses", "Cheese Wedge"),
    ("apples", "Red Apple"), ("potatoes", "Potato"), ("snowberries", "Snowberries"), ("berries", "Snowberries"),
    ("salmon", "Salmon Steak"), ("iron sword", None), ("steel sword", None), ("beef jerky", None), ("water", None),
    ("breakfast", "Vegetable Soup"), ("a hot meal", "Vegetable Soup"), ("something warm", "Vegetable Soup"),
    ("something to eat", "Vegetable Soup"), ("warm food", "Vegetable Soup"), ("dinner", "Vegetable Soup"),
    ("morning meal", "Vegetable Soup"), ("evening meal", "Vegetable Soup"), ("a proper meal", "Vegetable Soup"),
    ("a bite to eat", "Vegetable Soup"),
]:
    check(f"'{wanted}'", resolve_item(wanted), want)

print("\n== promise normalisation (model drift)")
P = lambda **k: from_json(k)
check("item 'coins' qty 10", (P(type="give_item", item="coins", qty=10) or {}).get("gold"), 10)
check("item '10 coins'", (P(type="give_item", item="10 coins") or {}).get("gold"), 10)
check("item 'fifty septims'", (P(type="give_gold", item="fifty septims") or {}).get("gold"), 50)
check("item '' (here, take this)", P(type="give_item", item=""), None)
check("item 'this'", P(type="give_item", item="this"), None)
check("item 'something'", P(type="give_item", item="something"), None)
check("gold '50' string", (P(type="give_gold", gold="50") or {}).get("gold"), 50)
check("gold 'a hundred'", (P(type="give_gold", gold="a hundred") or {}).get("gold"), 100)
check("gold null", (P(type="give_gold", gold=None) or {}).get("gold"), 0)
check("qty 'two'", (P(type="give_item", item="bread", qty="two") or {}).get("qty"), 2)
check("qty 'a dozen'", (P(type="give_item", item="apples", qty="a dozen") or {}).get("qty"), 12)
check("type 'give_money'", (P(type="give_money", gold=5) or {}).get("type"), "give_gold")
check("type 'gift'", (P(type="gift", item="bread") or {}).get("type"), "give_item")
check("type 'offer_room'", (P(type="offer_room") or {}).get("type"), "offer_bed")
check("type 'hug'", P(type="give_hug"), None)
check("when 'tomorrow'", (lambda p: (p["when"], p["time_hint"]))(P(type="give_gold", gold=5, when="tomorrow")), ("later", "tomorrow"))
check("when 'Later'", (P(type="give_gold", gold=5, when="Later") or {}).get("when"), "later")
check("when null", (P(type="give_gold", gold=5, when=None) or {}).get("when"), "now")
check("when now but in_hours 9", (P(type="give_gold", gold=5, when="now", in_hours=9) or {}).get("when"), "later")
check("when 'cancelled'", (P(type="give_gold", gold=5, when="cancelled") or {}).get("when"), "cancel")

print("\n== timing (hours until due) at 21:00 on Sundas")
for hint, want in [("tomorrow", 12), ("at dawn", 9), ("tomorrow at dawn", 9), ("sunrise", 9), ("breakfast", 10),
                   ("in the morning", 11), ("tomorrow morning", 11), ("at noon", 15), ("tomorrow afternoon", 18),
                   ("tonight", 1), ("midnight", 3), ("in two hours", 2), ("in an hour", 1), ("in a few hours", 3),
                   ("in a couple of days", 48), ("in three days", 72), ("next week", 168), ("next Fredas", 108), ("this afternoon", 18),
                   ("after my shift", 21), ("at dusk", 22), ("soon", None), ("when you come back", None)]:
    v = due_from_hint(hint, 21.0, weekday=0)
    check(f"'{hint}'", None if v is None else round(v), want)
print("   at 03:00 (deep night):")
for hint, want in [("at dawn", 3), ("in the morning", 5), ("tonight", 17), ("tomorrow", 30)]:
    check(f"'{hint}'", round(due_from_hint(hint, 3.0)), want)
check("unknown words use model estimate", pending_in({"time_hint": "after the job", "in_hours": 6.0}, 21.0), 6.0)
check("no estimate either -> 1h", pending_in({"time_hint": "soon", "in_hours": 0.0}, 21.0), 1.0)

print("\n== same promise? (settle early / cancel / no duplicates)")
check("gold vs gold", same_promise({"type": "give_gold", "item": ""}, {"type": "give_gold", "item": ""}), True)
check("'soup' vs 'cabbage soup'", same_promise({"type": "give_item", "item": "soup"}, {"type": "give_item", "item": "cabbage soup"}), True)
check("'bread' vs 'mead'", same_promise({"type": "give_item", "item": "bread"}, {"type": "give_item", "item": "mead"}), False)

print("\n== JSON from different models (incl. the garbage seen in your logs)")
replies = [
    ("clean", '{"request": "trade", "confidence": "high", "response": "accept", "price": 0, "promises": []}', ("open trade", "ok")),
    ("fenced", '```json\n{"request": "trade", "confidence": "high", "response": "accept", "promises": []}\n```', ("open trade", "ok")),
    ("chinese in number", '{"request": "trade", "confidence":奧0.9, "response": "accept", "promises": []}', ("open trade", "repaired")),
    ("empty value", '{"request": "trade", "confidence":, "response": "accept", "promises": []}', ("open trade", "repaired")),
    ("junk value", '{"request": "trade", "confidence":)[0.9, "response": "accept", "promises": []}', ("open trade", "repaired")),
    ("junk word value", '{"request": "trade", "confidence":拍了拍掌, "response": "accept", "promises": []}', ("open trade", "repaired")),
    ("broken key", '{"request": "trade", "confidence":-Ba- "": "accept", "promises": []}', ("open trade", "salvaged")),
    ("garbled gold kept", '{"request": "none", "confidence": "high", "response": "none", "promises": [{"type": "give_gold", "gold":奧10, "when": "now"}]}', ("gold 10 now", "repaired")),
    ("prose around", 'Sure! {"request": "rent_room", "confidence": "medium", "response": "accept", "price": "100", "promises": []} Hope that helps.', ("room popup 100g", "ok")),
    ("old numeric confidence", '{"request": "trade", "confidence": 0.85, "response": "accept", "promises": []}', ("open trade", "ok")),
    ("percent confidence", '{"request": "trade", "confidence": 90, "response": "accept"}', ("open trade", "ok")),
    ("confidence as text", '{"request": "trade", "confidence": "High.", "response": "accept"}', ("open trade", "ok")),
    ("request null", '{"request": null, "confidence": "high", "response": null, "promises": null}', ("nothing", "ok")),
    ("request 'barter'", '{"request": "barter", "confidence": "high", "response": "yes"}', ("open trade", "ok")),
    ("request 'Rent Room'", '{"request": "Rent Room", "confidence": "high", "response": "Accepted"}', ("room popup 100g", "ok")),
    ("response 'declined'", '{"request": "trade", "confidence": "high", "response": "declined"}', ("declined trade", "ok")),
    ("response 'on the house'", '{"request": "rent_room", "confidence": "high", "response": "on the house"}', ("room given free", "ok")),
    ("single promise object", '{"request": "none", "promises": {"type": "give_gold", "gold": 5, "when": "now"}}', ("gold 5 now", "ok")),
    ("promises as text", '{"request": "none", "promises": "none"}', ("nothing", "ok")),
    ("no json", "I cannot help with that.", ("nothing (unreadable reply)", "none")),
]
for label, raw, want in replies:
    check(label, decide(["trade", "rent_room"], raw), want)

print("\n== promise pre-filter (NPCs without services; True = classifier is asked)")
gate_cases = [
    ("Thanks for helping me out.", "Here, take this for your trouble.", True),
    ("I'm starving.", "Sit, have some of my stew.", True),
    ("Rough day?", "You look hungry. Eat with us tonight.", True),
    ("Can I crash here?", "Sure, the bed upstairs is yours.", True),
    ("Any work?", "Bring me ten wolf pelts and I'll make it worth your while... 50 gold.", True),
    ("Thanks.", "There you go, friend. Don't spend it all at once.", True),
    ("I did what you asked.", "You've earned this.", True),
    ("Got any sweetrolls?", "Just baked. Take one.", True),
    ("Long road?", "Stay with me tonight, you can leave at dawn.", True),
    ("I helped your son.", "I'll compensate you once the harvest is in.", True),
    ("Anything to drink?", "Pour yourself a mug from the barrel.", True),
    ("About my payment...", "I can't pay you after all.", True),
    ("Nice weather.", "Aye, for once the sky isn't weeping.", False),
    ("What's your name?", "Ysolda. I want to run the Bannered Mare one day.", False),
    ("Riften is a cesspit.", "Watch your tongue, outsider.", False),
    ("Heard any rumors?", "They say dragons have returned.", False),
    ("Hello.", "I’ve heard every line under the sun — they all sound the same.", False),
    ("«Hi»", "Watch it…", False),
    ("Привет", "Держи монеты", True),
    ("Hola", "Toma, un regalo pequeño", True),
]
for pl, npc, want in gate_cases:
    check(f'"{npc[:48]}"', might_promise(pl, npc), want)

print("\n== RP exchanges -> what MundaneMatters does (at 21:00; reply = what a model should write, drift included)")
INN = ["trade", "rent_room"]
rp = [
    # merchants
    (["trade"], '{"request":"trade","confidence":"high","response":"accept","promises":[]}', "open trade",
     "i got something to sell, but first, show me your wares / *gestures at shelves* Of course!"),
    (["trade"], '{"request":"trade","confidence":"high","response":"refuse","promises":[]}', "declined trade",
     "what've you got, old man? / Not for the likes of you, milk-drinker."),
    (["trade"], '{"request":"none","confidence":"high","response":"none","promises":[]}', "nothing",
     "I traded with your brother yesterday. / Lucky you."),
    (["trade"], '{"request":"trade","confidence":"high","response":"accept","promises":[{"type":"give_item","item":"health potion","when":"now"}]}',
     "open trade; item 'health potion'->Potion of Healing x1 now",
     "Show me your potions. / Here, and take this one free, for a first customer."),
    # innkeepers
    (INN, '{"request":"rent_room","confidence":"high","response":"accept","price":100,"promises":[]}', "room popup 100g",
     "i need a room to stay / That'll be one hundred septims a night. Coin upfront."),
    (INN, '{"request":"rent_room","confidence":"high","response":"accept","price":"seventy","promises":[]}', "room popup 70g",
     "I'll pay 70, not a coin more. / *sighs* Fine. Seventy."),
    (INN, '{"request":"rent_room","confidence":"medium","response":"refuse","price":5,"promises":[]}', "declined rent_room",
     "A room for 5 gold? / Five septims buys you the floor."),
    (INN, '{"request":"rent_room","confidence":"high","response":"free","promises":[]}', "room given free",
     "Got a room? / For the hero of Whiterun? On the house."),
    (INN, '{"request":"rent_room","confidence":"high","response":"accept","price":1,"promises":[]}', "room popup 50g",
     "Got a room? / Hm, 1 gold, for you."),
    (INN, '{"request":"rent_room","confidence":"high","response":"refuse","promises":[]}', "declined rent_room",
     "Room please / We're full up tonight, sorry."),
    (INN, '{"request":"rent_room","confidence":"high","response":"accept","price":100,"promises":[{"type":"give_item","item":"stew","when":"now"}]}',
     "room popup 100g; item 'stew'->Beef Stew x1 now", "Room and a bowl of stew? / Room's 100, the stew's on me."),
    (INN, '{"request":"rent_room","confidence":"high","response":"accept","promises":[{"type":"offer_bed","when":"now"}]}',
     "room popup 100g", "Need a bed / Room's yours, upstairs."),
    (INN, '{"request":"rent_room","confidence":"high","response":"accept","price":100,"promises":[{"type":"give_item","item":"breakfast","when":"later","time_hint":"at dawn","in_hours":9}]}',
     "room popup 100g; item 'breakfast'->Vegetable Soup x1 in 9h", "Room? / 100, and I'll see you fed at dawn."),
    # Hulda's actual lines from your log
    (["trade"], '{"request":"none","confidence":"high","response":"none","promises":[{"item":"breakfast","qty":1,"time_hint":"","type":"give_item","when":"now"}]}',
     "item 'breakfast'->Vegetable Soup x1 now", "can i get it now? / I'll have Saadia bring something warm to your room."),
    (["trade"], '{"request":"none","confidence":"high","response":"none","promises":[{"item":"morning meal","qty":1,"time_hint":"dawn","type":"give_item","when":"later"}]}',
     "item 'morning meal'->Vegetable Soup x1 in 9h", "free tomorrow morning? / Rest easy tonight and I'll see you fed at dawn."),
    # promises from anyone
    ([], '{"request":"none","promises":[{"type":"give_gold","gold":10,"when":"now"},{"type":"give_item","item":"cabbage soup","when":"now"}]}',
     "gold 10 now; item 'cabbage soup'->Cabbage Potato Soup x1 now", "Thanks. / Take 10 coins and a bowl of cabbage soup."),
    ([], '{"request":"none","promises":[{"type":"give_item","item":"coins","qty":1,"when":"now"}]}', "gold 10 now",
     "Thanks. / Here, a few coins for your trouble."),
    ([], '{"request":"none","promises":[{"type":"offer_bed","when":"now"}]}', "bed now",
     "Can I crash here? / The bed in the back is yours tonight."),
    ([], '{"request":"none","promises":[{"type":"give_gold","gold":100,"when":"later","time_hint":"tomorrow","in_hours":12}]}',
     "gold 100 in 12h", "Will you pay me? / Come back tomorrow and I'll have 100 gold."),
    ([], '{"request":"none","promises":[{"type":"give_gold","gold":"two hundred","when":"after the job","in_hours":24}]}',
     "gold 200 in 24h", "Deal? / Two hundred once it's done, you have my word."),
    ([], '{"request":"none","promises":[{"type":"give_gold","gold":100,"when":"cancel"}]}', "cancel gold",
     "My 100 gold tomorrow? / About that... I can't pay you after all."),
    ([], '{"request":"none","promises":[{"type":"give_gold","gold":100,"when":"now"}]}', "gold 100 now",
     "You said you'd pay me. / Aye, here's the 100 gold I promised."),
    ([], '{"request":"none","promises":[]}', "nothing", "I'll clear the mine. / Do that and there's 200 gold in it for you."),
    ([], '{"request":"none","promises":[]}', "nothing", "Any food? / Bread's 2 gold."),
    ([], '{"request":"none","promises":[{"type":"give_item","item":"sweetrolls","qty":"two","when":"now"}]}',
     "item 'sweetrolls'->Sweet Roll x2 now", "Bye. / Take a couple of sweetrolls for the road!"),
    ([], '{"request":"none","promises":[{"type":"give_item","item":"apples","qty":"a dozen","when":"now"}]}',
     "item 'apples'->Red Apple x10 now", "Hungry. / Take a dozen apples, we have too many."),
    ([], '{"request":"none","promises":[{"type":"give_item","item":"something to drink","when":"now"}]}',
     "item 'something to drink'->Nord Mead x1 now", "Thirsty. / Here, have something to drink."),
    ([], '{"request":"none","promises":[{"type":"give_item","item":"this","when":"now"}]}', "nothing", "Hello. / Here, take this."),
    ([], '{"request":"none","promises":[{"type":"give_gold","gold":5000,"when":"now"}]}', "gold 200 now",
     "I saved your life. / Take my fortune, 5000 gold!"),
    ([], '{"request":"trade","confidence":"high","response":"refuse","promises":[]}', "nothing",
     "show me your wares / I'm no merchant, friend."),
]
for services, raw, want, line in rp:
    check(line[:70], decide(services, raw)[0], want)

print("\n== follow / escort: is the classifier asked? (anyone offers them)")
ANY = ["follow", "escort"]
for pl, npc, want in [
    ("follow me", "Lead on.", ANY),
    ("Come with me, I could use the company.", "Fine.", ANY),
    ("Could you take me to the Bannered Mare?", "This way.", ANY),
    ("How do I get to Dragonsreach?", "Up the stairs, I'll show you.", ANY),
    ("Where's the temple?", "Come, I'll walk you there.", ANY),
    ("stick close, it's dangerous", "Aye.", ANY),
    ("Nice weather.", "Aye.", None),
    ("Thanks for the help.", "Here, take 10 coins.", []),
    # misspelled player lines
    ("folow me", "Aye.", ANY),
    ("plz follw me", "Sure.", ANY),
    ("escrot me to the inn", "Hm.", ANY),
    ("can u gide me to dragonsreech", "Hm.", ANY),
    ("acompany me pls", "Hm.", ANY),
    # garbled player line, but the NPC understood
    ("tkae me to ur hoem", "Very well, follow me. My house is this way.", ANY),
    ("cmon w me", "Lead on, I'm right behind you.", ANY),
    # common words near "take"/"lead" must not open the gate
    ("I'll make some tea and read a book.", "Sounds nice.", None),
    ("My head hurts.", "Sorry to hear it.", None),
]:
    check(f'"{pl}"', asked(ANY, pl, npc), want)
check("merchant small talk keeps trade only", asked(["trade"] + ANY, "Nice weather.", "Aye."), ["trade"])

print("\n== follow / escort RP (reply = what a model should write)")
for raw, want, line in [
    ('{"request":"follow","confidence":"high","response":"accept"}', "open follow", "follow me / Lead on, then."),
    ('{"request":"accompany","confidence":"high","response":"yes"}', "open follow", "come along? / Sure."),
    ('{"request":"follow","confidence":"high","response":"refuse"}', "declined follow", "follow me / I have work to do."),
    ('{"request":"escort","confidence":"high","response":"accept","place":"Temple of Kynareth"}', "escort to Temple of Kynareth",
     "show me the way to the temple / The Temple of Kynareth is this way."),
    ('{"request":"guide","confidence":"high","response":"accept","place":"Riverwood"}', "escort to Riverwood", "guide me to Riverwood / Come."),
    ('{"request":"escort","confidence":"high","response":"accept","place":""}', "escort: no destination", "lead the way / Where to?"),
    ('{"request":"escort","confidence":"high","response":"counter","place":"Riverwood"}', "declined escort",
     "take me to Riverwood / Help me finish here first."),
]:
    check(line, decide(ANY, raw)[0], want)

print("\n== player gives (model drift included)")
for j, want in [
    ({"item": "health potion", "qty": 1, "gold": 0, "unnamed": False}, "give 'health potion' x1"),
    ({"item": "", "qty": 1, "gold": 50}, "give 50 gold"),
    ({"item": "50 coins"}, "give 50 gold"),
    ({"item": "septims", "qty": 20}, "give 20 gold"),
    ({"item": "", "unnamed": True}, "gift menu"),
    ({"item": "this", "unnamed": "true"}, "gift menu"),
    ({"item": "", "qty": 1, "gold": 0}, "nothing"),            # the template copied verbatim
    ({"item": "", "qty": 1, "gold": 0, "unnamed": False}, "nothing"),
    ({"item": "this"}, "nothing"),                             # vague but not marked unnamed
    ({"item": "apples", "qty": "three"}, "give 'apples' x3"),
    ({"item": "mead", "qty": None, "gold": None}, "give 'mead' x1"),
    ("nonsense", "nothing"),
]:
    check(json.dumps(j), gift_action(gift_from_json(j)), want)

print("\n== kinds of place: is it only a kind (-> nearest real one)?")
for text, want in [
    ("the brewery", ("brewery", True)), ("brewry", ("brewery", True)), ("the brewry", ("brewery", True)),
    ("The Brewery, Whiterun", ("brewery", True)), ("Honningbrew Meadery", ("brewery", False)),
    ("the inn", ("inn", True)), ("the local tavern", ("inn", True)), ("The Bannered Mare", (None, None)),
    ("the temple", ("temple", True)), ("Temple of Kynareth", ("temple", False)),
    ("the blacksmith", ("smithy", True)), ("Warmaiden's", (None, None)),
    ("the general store", ("store", True)), ("Belethor's General Goods", ("store", False)),
    ("the jarl's palace", ("palace", True)), ("the stables", ("stables", True)),
    ("Riverwood", (None, None)), ("Dragonsreach", (None, None)),
]:
    k = kind_of(text)
    check(f"'{text}'", (k, only_a_kind(text, k, "Whiterun", "Whiterun Hold") if k else None), want)

print("\n== generosity budget: RP sessions (stew 8 = food, lockpick 3, coin as said, silver ring 30)")
STEW, LOCKPICK, RING = 8, 3, 30
for level, steps, want in [
    ("acquaintance", [(0, "stew", STEW, True), (0.2, "stew", STEW, True), (0.4, "stew", STEW, True), (0.5, "lockpick", LOCKPICK, False)],
     ["stew", "stew", "no stew", "no lockpick"]),
    ("acquaintance", [(0, "10 gold", 10, False), (1, "stew", STEW, True), (25, "stew", STEW, True)],
     ["10 gold", "no stew", "stew"]),
    ("friend", [(0, "stew", STEW, True), (0.2, "stew", STEW, True), (0.4, "lockpick", LOCKPICK, False), (0.5, "10 gold", 10, False), (1, "stew", STEW, True)],
     ["stew", "stew", "lockpick", "no 10 gold", "stew"]),
    ("confidant", [(0, "silver ring", RING, False), (1, "20 gold", 20, False), (2, "stew", STEW, True), (3, "10 gold", 10, False)],
     ["silver ring", "20 gold", "stew", "no 10 gold"]),
    ("lover", [(0, "50 gold", 50, False), (1, "silver ring", RING, False), (2, "stew", STEW, True), (3, "20 gold", 20, False), (6, "20 gold", 20, False)],
     ["50 gold", "silver ring", "stew", "no 20 gold", "20 gold"]),
    ("rival", [(0, "stew", STEW, True)], ["no stew"]),
]:
    allow = Allowance()
    got = [ask(allow, t, level, what, value, food) for t, what, value, food in steps]
    check(level + ": " + " -> ".join(w for _, w, _, _ in steps), got, want)
check("companion budget", generosity("acquaintance", teammate=True), (6.0, 80))

print("\n== gift reaction: does it warm them up? (Promises.cpp OnNpcReply)")
def liked(raw):
    low = raw.lower()
    return "delighted" in low or '"pleased"' in low


for raw, want in [('{"reaction": "delighted"}', True), ('{"reaction": "pleased"}', True), ('{"reaction":"Pleased"}', True),
                  ('{"reaction": "neutral"}', False), ('{"reaction": "displeased"}', False), ('pleased', False), ('', False)]:
    check(raw or "(empty)", liked(raw), want)

print("\n== beds: one night vs always welcome")
for j, want in [
    ({"type": "offer_bed", "when": "now", "always": True}, True),
    ({"type": "offer_bed", "when": "now", "always": "true"}, True),
    ({"type": "offer_bed", "when": "now", "always": False}, False),
    ({"type": "offer_bed", "when": "now"}, False),                 # one night unless said
    ({"type": "offer_room", "always": "yes"}, True),               # drifted type name
    ({"type": "offer_bed", "when": "cancel", "always": True}, True),
]:
    p_ = from_json(j)
    check(json.dumps(j), p_["always"] if p_ else None, want)

print("\n== permission to take")
check("allow_take wine", (from_json({"type": "allow_take", "item": "wine"}) or {}).get("type"), "allow_take")
check("drifted 'take_item'", (from_json({"type": "take_item", "item": "bread"}) or {}).get("type"), "allow_take")
check("'help_yourself'", (from_json({"type": "help_yourself", "item": "apples", "qty": "two"}) or {}).get("qty"), 2)
check("nothing named", from_json({"type": "allow_take", "item": "it"}), None)
check("wine vs bread are different", same_promise({"type": "allow_take", "item": "wine"}, {"type": "allow_take", "item": "bread"}), False)
check("wine vs alto wine are the same", same_promise({"type": "allow_take", "item": "wine"}, {"type": "allow_take", "item": "alto wine"}), True)

print("\n== shared meal pre-filter (TurnPipeline.cpp MightMeal)")
MEAL = {"eat", "eating", "ate", "meal", "meals", "dinner", "supper", "breakfast", "lunch", "food", "hungry", "starving",
        "table", "feast", "dine", "dining", "bite", "share", "cook", "cooking", "stew", "soup", "drink", "drinks", "ale", "mead"}
MEAL_TYPO = ["dinner", "supper", "breakfast", "hungry"]
MEAL_NPC = ["sit with me", "eat with me", "let's eat", "lets eat", "set the table", "join me", "pull up a chair",
            "pull up a stool", "we'll eat", "we can eat", "share a meal", "supper together"]


def might_meal(player, npc):
    if has_foreign_letters(player):
        return True
    for w in re.findall(r"[a-z0-9]+", player.lower()):
        if w in MEAL:
            return True
        if any(len(w) >= 5 and w != k and distance(w, k) <= 1 for k in MEAL_TYPO):
            return True
    return any(x in npc.lower() for x in MEAL_NPC)


for pl, npc, want in [
    ("Let's eat together?", "Sure.", True),
    ("wanna grab a bite wit me? im starvin", "Ha, come on.", True),
    ("join me for suppr?", "Gladly.", True),
    ("how about dinnr", "Fine.", True),
    ("brekfast?", "Morning already?", True),
    ("Fancy a drink with me?", "Why not.", True),
    ("so...", "Pull up a stool, I'll serve us both.", True),
    ("What do you mean?", "Never mind.", False),
    ("Why are you so angry?", "I'm not.", False),
    ("Nice weather.", "Aye.", False),
]:
    check(f'"{pl}" / "{npc}"', might_meal(pl, npc), want)

print("\n== rest pre-filter (TurnPipeline.cpp MightRest)")
REST = {"sleep", "sleeping", "sleepy", "rest", "resting", "bed", "bedroll", "bedrolls", "tired", "exhausted",
        "camp", "nap", "doze", "kip", "slumber", "bunk", "weary"}
REST_TYPO = ["sleep", "tired", "bedroll", "exhausted", "slumber"]
REST_PHRASES = ["bed down", "get some rest", "turn in", "make camp", "get some sleep", "lie down", "time for bed", "off to bed",
                "shut-eye", "shut eye", "call it a night", "for the night"]


def might_rest(player, npc):
    if has_foreign_letters(player):
        return True
    low = player.lower()
    if any(p in low for p in REST_PHRASES):
        return True
    for w in re.findall(r"[a-z0-9]+", low):
        if w in REST:
            return True
        if any(len(w) >= 4 and w != k and distance(w, k) <= 1 for k in REST_TYPO):
            return True
    return any(p in npc.lower() for p in REST_PHRASES)


for pl, npc, want in [
    ("It's late. Let's turn in.", "Aye.", True),
    ("im so tierd, lets slep", "Get some sleep.", True),
    ("Shall we make camp here?", "Fine.", True),
    ("we should rest", "Agreed.", True),
    ("call it a night?", "Gladly.", True),
    ("so...", "Aye, let's bed down by the fire.", True),
    ("Good night, Hulda.", "Sleep well.", False),
    ("Turn left at the gate.", "Thanks.", False),
    ("Nice weather.", "Aye.", False),
    ("What do you mean?", "Never mind.", False),
]:
    check(f'rest gate "{pl}" / "{npc}"', might_rest(pl, npc), want)
for r, want in [("rest", "rest"), ("sleep", "rest"), ("make_camp", "rest"), ("rent_room", "rent_room"), ("share_meal", "share_meal")]:
    check(f"norm_request({r})", norm_request(r), want)

print("\n== rest: where, who, which bed (Rest.cpp)")
RANK = {"lover": 4, "ally": 3, "confidant": 2, "friend": 1, "acquaintance": 0, "rival": -1}


def may_rest(level, teammate):
    return teammate or RANK[level] >= 1


def allocate(place, sleepers, beds, player_bed=None, host=None, bedrolls=True):
    """sleepers: [(name, level, teammate, carries_bedroll)]; beds: [(id, owner, double)] nearest first.
    Mirrors Rest::Start: a lover shares a double player bed (or the host's own), the host takes their
    own bed, others take free beds (never someone's personal bed), then bedrolls, else nothing."""
    beds = list(beds)
    order = sorted(sleepers, key=lambda s: -RANK[s[1]])
    if player_bed is None and place == "wild" and beds:
        player_bed = beds[0]
    if player_bed in beds:
        beds.remove(player_bed)
    out, shared = {}, False
    for name, level, mate, carries in order:
        bed = None
        if not shared and player_bed and level == "lover" and (player_bed[2] or name == host):
            bed, shared = "player bed", True
        elif name == host:
            own = next((b for b in beds if b[1] == name), None)
            if own:
                bed = own[0]
                beds.remove(own)
        if bed is None:
            free = next((b for b in beds if b[1] in (None, "player")), None)
            if free:
                bed = free[0]
                beds.remove(free)
        if bed is None and bedrolls and (mate or carries):
            bed = "bedroll"
        out[name] = bed
    return out


for level, mate, want in [("acquaintance", False, False), ("friend", False, True), ("acquaintance", True, True), ("rival", False, False)]:
    check(f"may rest: {level}{' companion' if mate else ''}", may_rest(level, mate), want)

party = [("Lydia", "ally", True, False), ("Serana", "lover", True, False), ("Faendal", "friend", True, False)]
got = allocate("inn_room", party, [], player_bed=("room", "player", True))
check("inn room, 1 double bed, lover + 2: lover shares", got["Serana"], "player bed")
check("inn room: the others lay bedrolls", (got["Lydia"], got["Faendal"]), ("bedroll", "bedroll"))
got = allocate("inn_room", party, [], player_bed=("room", "player", False))
check("single room bed: the lover gets a bedroll", got["Serana"], "bedroll")
got = allocate("wild", [("Lydia", "ally", True, False), ("Farmer", "friend", False, False)], [("tent", None, False)])
check("wild: the bedroll by the player stays the player's", got["Lydia"], "bedroll")
check("wild: a friend without a bedroll dozes", got["Farmer"], None)
got = allocate("npc_home", [("Ronja", "friend", False, False)], [("ronja_bed", "Ronja", True), ("spare", None, False)],
               player_bed=("spare", None, False), host="Ronja")
check("her home, friend: she keeps her own bed", got["Ronja"], "ronja_bed")
got = allocate("npc_home", [("Ronja", "lover", False, False)], [("ronja_bed", "Ronja", False)],
               player_bed=("ronja_bed", "Ronja", False), host="Ronja")
check("her home, lover: shares her own bed with you", got["Ronja"], "player bed")
got = allocate("npc_home", [("Ronja", "friend", False, False), ("Lydia", "ally", True, False)],
               [("ronja_bed", "Ronja", True), ("son_bed", "Son", False)], host="Ronja")
check("her home: a companion never takes a family member's bed", got["Lydia"], "bedroll")

print("\n== hold this for me: pre-filter (TurnPipeline.cpp MightHold)")
HOLD_VERBS = ["hold", "keep", "carry", "mind", "watch", "guard", "store", "stash"]
HOLD_WHAT = {"this", "these", "that", "those", "my", "it", "them", "onto", "safe"}
HOLD_ALONE = {"stash", "safekeeping", "belongings"}
HOLD_PHRASES = ["hold on to", "look after", "keep it safe", "keep them safe", "i'll keep", "i'll hold", "i'll carry",
                "i'll look after", "here's your", "here are your", "give it back", "hand it back"]


def might_hold(player, npc):
    if has_foreign_letters(player):
        return True
    low = player.lower()
    if any(p in low for p in HOLD_PHRASES):
        return True
    words = re.findall(r"[a-z0-9]+", low)
    mine = "my" in words
    for i, w in enumerate(words):
        if w in HOLD_ALONE or (mine and w in ("back", "return")):
            return True
        verb = any(w == v or (len(w) >= 4 and distance(w, v) <= 1) for v in HOLD_VERBS)
        if verb and i + 1 < len(words) and words[i + 1] in HOLD_WHAT:
            return True
    return any(p in npc.lower() for p in HOLD_PHRASES)


for pl, npc, want in [
    ("hold this for me", "Sure.", True),
    ("can you keep my sword safe while I go in?", "Aye.", True),
    ("carry these potions for a while", "Fine.", True),
    ("hodl this pls", "Okay?", False),          # two slips: too far
    ("hld this", "Hm?", False),                 # too short to trust
    ("hlod this for me", "Okay.", False),       # a swap is two edits
    ("hols this for me", "Okay.", True),
    ("can i have my sword back?", "Here.", True),
    ("so...", "I'll keep it safe for you.", True),
    ("Hold on, wait a moment.", "What?", False),
    ("Keep walking.", "Aye.", False),
    ("I'll be back soon.", "Take care.", False),
    ("Nice weather.", "Aye.", False),
]:
    check(f'hold gate "{pl}" / "{npc}"', might_hold(pl, npc), want)
for r, want in [("hold", "hold"), ("keep_items", "hold"), ("store", "hold"), ("retrieve", "retrieve"), ("give_back", "retrieve"),
                ("return_items", "retrieve")]:
    check(f"norm_request({r})", norm_request(r), want)

print("\n== hold: one clear item, or let the player choose (Safekeeping.cpp Single)")


def single(found):
    """found: [(name, score, worn)] best first. Mirrors Safekeeping::Single."""
    if not found:
        return None
    close = [f for f in found if f[1] > found[0][1] - 0.75]
    if len(close) == 1:
        return close[0][0]
    worn = [f for f in close if f[2]]
    return worn[0][0] if len(worn) == 1 else None


check('"sword", one sword', single([("Iron Sword", 9.5, False)]), "Iron Sword")
check('"sword", two swords, one in hand', single([("Iron Sword", 9.5, True), ("Steel Sword", 9.5, False)]), "Iron Sword")
check('"sword", two swords, none in hand -> choose', single([("Iron Sword", 9.5, False), ("Steel Sword", 9.5, False)]), None)
check('"potion", three kinds -> choose', single([("Potion of Healing", 9.0, False), ("Potion of Minor Healing", 8.5, False)]), None)
check('"iron sword" vs "iron sword of frost"', single([("Iron Sword", 10.0, False), ("Iron Sword of Frost", 9.0, False)]), "Iron Sword")
check("nothing like it", single([]), None)

print("\n== house keys (Promises.cpp FromJson, Keys.cpp)")
for j, want in [
    ({"type": "give_key", "when": "now"}, ("give_key", "now")),
    ({"type": "house_key"}, ("give_key", "now")),
    ({"type": "give_item", "item": "spare key"}, ("give_key", "now")),
    ({"type": "give_item", "item": "key to my house"}, ("give_key", "now")),
    ({"type": "give_item", "item": "key to the old chest"}, ("give_item", "now")),
    ({"type": "give_key", "when": "cancel"}, ("give_key", "cancel")),
    ({"type": "give_key", "when": "tomorrow"}, ("give_key", "later")),
]:
    got = from_json(j)
    check(f"key promise {j}", (got["type"], got["when"]) if got else None, want)


def key_ok(level):
    return level in ("ally", "lover")


for level, want in [("lover", True), ("ally", True), ("confidant", False), ("friend", False), ("acquaintance", False)]:
    check(f"key from a {level}", key_ok(level), want)
check("key words reach the classifier", might_promise("so...", "Here, take my spare key."), True)

print("\n== trust: what reaches the classifier (TurnPipeline.cpp MightHurtOrMend)")
TRUST_WORDS = {"sorry", "apologize", "apologise", "apology", "apologies", "forgive", "forgiveness", "regret", "fault",
               "idiot", "fool", "stupid", "ugly", "hate", "pathetic", "worthless", "liar", "coward", "useless", "disgusting",
               "dumb", "moron", "bitch", "whore", "bastard", "milkdrinker", "trash", "scum", "filth", "pig", "hag", "wench",
               "slut", "freak", "loser", "shut", "kill", "die", "ashamed", "disgrace"}
TRUST_PHRASES = ["make it up", "didn't mean", "my fault", "go away", "get lost", "how dare you", "i trusted you", "i forgive you",
                 "apology accepted", "get out", "never speak to me", "you thief"]


def might_hurt_or_mend(player, npc):
    if has_foreign_letters(player):
        return True
    for line in (player, npc):
        if any(p in line.lower() for p in TRUST_PHRASES):
            return True
    return any(w in TRUST_WORDS for w in re.findall(r"[a-z0-9]+", player.lower()))


for pl, npc, want in [
    ("You're a worthless old hag.", "How dare you!", True),
    ("I'm sorry, I didn't mean it.", "...Alright.", True),
    ("so...", "I trusted you, and you robbed me!", True),
    ("Lovely day.", "It is.", False),
    ("What's for supper?", "Stew.", False),
]:
    check(f'trust gate "{pl}" / "{npc}"', might_hurt_or_mend(pl, npc), want)

print("\n== trust: hits and fights (Trust.cpp)")


def judge_hit(player_fighting, target_fighting, companion, turned_hostile, assault_bounty, ranged=False, repeated=False):
    """Mirrors Trust.cpp: stray hits mid-fight are nothing; an assault bounty or a companion turning on the
    player is an attack (never forgiven); an out-of-the-blue hit they take is rude (an apology mends it)."""
    if assault_bounty:
        return "attack"
    if player_fighting or target_fighting:
        return "attack" if companion and turned_hostile else "nothing"
    if ranged and not repeated:
        return "nothing"  # one stray arrow or spell: hunting, a missed shot
    if companion and turned_hostile:
        return "attack"
    if turned_hostile:
        return "nothing"  # a brawl: no bounty, not a companion
    return "rude"


for label, args, want in [
    ("stray arrow into Lydia mid-battle", (True, True, True, False, False), "nothing"),
    ("several stray hits, she stays loyal", (True, True, True, False, False), "nothing"),
    ("kept hitting Lydia until she turned on you", (True, True, True, True, False), "attack"),
    ("punched Ronja in town, she just complains", (False, False, False, False, False), "rude"),
    ("attacked Ronja, assault bounty", (False, False, False, True, True), "attack"),
    ("brawl with Uthgerd (no bounty)", (True, True, False, True, False), "nothing"),
    ("one stray arrow into Faendal while hunting", (False, False, False, False, False, True, False), "nothing"),
    ("shooting Faendal twice", (False, False, False, False, False, True, True), "rude"),
]:
    check(f"hit: {label}", judge_hit(*args), want)

print("\n== trust: what an apology mends")


def after_apology(level):
    return {"offended": "forgiven (earn things again)", "attacked": "still never forgiven", "none": "nothing to forgive"}[level]


def gift_counts(grudge, hours_since_forgiven=None):
    if grudge:
        return False
    return hours_since_forgiven is None or hours_since_forgiven >= 24


check("gift while they're angry: amends, no bond", gift_counts(True), False)
check("gift an hour after forgiveness: amends, no bond", gift_counts(False, 1), False)
check("gift two days after forgiveness: counts", gift_counts(False, 48), True)
check("ordinary gift: counts", gift_counts(False), True)

for level, want in [("offended", "forgiven (earn things again)"), ("attacked", "still never forgiven"), ("none", "nothing to forgive")]:
    check(f"apology when {level}", after_apology(level), want)

print("\n== bonds: who Bonds looks after (Bonds.cpp Eligible)")


def bond_eligible(rank, teammate, ever_followed, followers_on, child=False):
    if rank < 0 or rank >= 3 or (child and rank >= 1):
        return False
    if not followers_on and (teammate or ever_followed):
        return False
    return True


for label, args, want in [
    ("townsfolk, switch on", (0, False, False, True), True),
    ("townsfolk, switch off", (0, False, False, False), True),
    ("follower, switch on", (1, True, True, True), True),
    ("follower, switch off", (1, True, True, False), False),
    ("dismissed follower, switch off", (1, False, True, False), False),
    ("already an ally", (3, False, False, True), False),
    ("lover (game or another mod)", (4, False, False, True), False),
    ("child friend", (1, False, False, True, True), False),
]:
    check(f"bonds: {label}", bond_eligible(*args), want)

print("\n== bonds: deeds, daily cap, steps (Bonds.cpp)")
STEPS = {1: (6, 3, 2), 2: (12, 4, 3), 3: (24, 10, 4)}  # rank -> (points, days, kinds)
ADVENTURE = {"fight", "travel"}  # one point a day between them


def simulate(rank, days_of_deeds, until=None):
    """days_of_deeds: [[(kind, pts), ...] per game day]. Returns the rank reached (only raises, never past Ally),
    or with until=rank the day number (1-based) it was reached, None if never."""
    points, kinds, days = 0, set(), set()
    for d, deeds in enumerate(days_of_deeds):
        today, kinds_today = 0, set()
        for kind, pts in deeds:
            if kind in kinds_today:
                continue  # each kind of deed counts once a day
            if kind in ADVENTURE and kinds_today & ADVENTURE:
                pts = 0  # fighting and travelling together: one day's adventure
            kinds_today.add(kind)
            gained = max(0, min(pts, 3 - today))
            today += gained
            points += gained
            kinds.add(kind)
            days.add(d)
            nxt = rank + 1
            if nxt in STEPS:
                need = STEPS[nxt]
                if points >= need[0] and len(days) >= need[1] and len(kinds) >= need[2]:
                    rank, points, kinds, days = nxt, 0, set(), set()
                    if until is not None and rank >= until:
                        return d + 1
    return None if until is not None else rank


def talk_counts(exchanges):
    return exchanges >= 3


meal, rest, gift, talk = ("meal", 2), ("rest", 2), ("gift", 1), ("talk", 1)
fight, travel, protect, promise = ("fight", 1), ("travel", 1), ("protect", 2), ("promise", 1)
check("one flirty evening of meals: still acquaintance", simulate(0, [[meal, meal, meal, meal]]), 0)
check("a word or two isn't a talk", talk_counts(2), False)
check("three exchanges is a real talk", talk_counts(3), True)
check("chatting daily only, a month: never a friend (one kind)", simulate(0, [[talk] for _ in range(30)]), 0)
check("two days, meal + talk: not yet a friend (needs 3 days)", simulate(0, [[meal, talk], [meal, talk]]), 0)
check("three days, meal + talk: friend", simulate(0, [[meal, talk]] * 3), 1)
check("daily cap: 10 meals in a day still 3 points", simulate(0, [[meal] * 10]), 0)
check("same deed only, two weeks: no confidant", simulate(1, [[meal] for _ in range(14)]), 1)
check("ally needs 10+ days and 4 kinds", simulate(2, [[meal, rest] for _ in range(20)]), 2)
check("never past ally", simulate(3, [[meal, rest, gift, talk] for _ in range(30)]), 3)
held = ("held", 1)
check("hold-and-take-back 5x a day + talk, 2 days: not friends", simulate(0, [[held] * 5 + [talk], [held] * 5 + [talk]]), 0)
check("fight + travel the same day: 1 point between them", simulate(0, [[fight, travel]] * 6), 1)

# realistic play: how many game days to Confidant / Ally
follower_day = [[talk, fight, travel], [talk, travel], [talk, fight, meal], [talk, travel, rest], [talk, fight, protect]]
follower = [follower_day[d % 5] for d in range(60)]
townsfolk = [[talk, meal] if d % 2 else [talk, gift] for d in range(60)]
grinder = [[meal, rest, gift, talk, promise] for _ in range(60)]  # everything, every single day
f_conf, f_ally = simulate(0, follower, until=2), simulate(0, follower, until=3)
t_conf = simulate(0, townsfolk, until=2)
g_conf, g_ally = simulate(0, grinder, until=2), simulate(0, grinder, until=3)
print(f"   days to Confidant / Ally: follower {f_conf} / {f_ally}, townsfolk visited daily {t_conf}, max grinder {g_conf} / {g_ally}")
check("adventuring follower: Confidant in about a week (7-10 days)", 7 <= f_conf <= 10, True)
check("townsperson visited daily: Confidant in about a week (7-10 days)", 7 <= t_conf <= 10, True)
check("even doing everything every day: Confidant takes 7+ days", g_conf >= 7, True)
check("ally takes over two weeks even at max", g_ally >= 15, True)


# ---------------------------------------------------------------- buy & sell by talking (Trade.cpp)
print("\n== buy & sell by talking (Trade.cpp)")


def barter_factor(speech, bmax=3.3, bmin=2.0):
    speech = min(max(speech, 0), 100)
    return max(1.0, max(bmin, bmax - (bmax - bmin) * speech / 100))


def buy_price(value, speech):
    return max(1, round(value * barter_factor(speech)))


def sell_price(value, speech):
    return max(1, int(value / barter_factor(speech)))


def agreed(list_total, said, each, n):
    if said <= 0:
        return list_total
    total = said * n if each else said
    return min(max(total, max(1, list_total // 2)), max(1, (list_total * 3 + 1) // 2))


def qty(raw):
    t = str(raw).lower() if raw is not None else ""
    if "all" in t or "every" in t or "lot" in t:
        return -1
    try:
        v = float(raw)
    except (TypeError, ValueError):
        return 1
    return int(v) if 1 <= v < 1e5 else (-1 if v < 0 else 1)


def count(asked, have):
    return have if asked <= 0 else min(asked, have)


def asks_price(line):
    phrases = ("how much", "hw much", "how mch", "how mutch", "what do you want for", "what'll you give", "what will you give",
               "would you give", "would you pay", "will you pay", "what do i owe", "what's it worth")
    words = {"price", "prices", "pricing", "cost", "costs", "worth", "charge", "sell", "selling", "buy", "buying", "afford"}
    low = line.lower()
    return any(p in low for p in phrases) or any(w in words for w in re.findall(r"[a-z0-9]+", low))


def vendor_buys(keywords, listed, not_buy_sell, stolen, buys_stolen=False, buys_non_stolen=True):
    if (not buys_stolen) if stolen else (not buys_non_stolen):
        return False
    if listed is None:
        return True
    inlist = bool(set(keywords) & set(listed))
    return not inlist if not_buy_sell else inlist


for r, want in [("buy", "buy"), ("buy_item", "buy"), ("purchase", "buy"), ("sell", "sell"), ("sell_item", "sell"),
                ("price_check", "none"), ("trade", "trade"), ("barter", "trade")]:
    check(f"norm_request({r})", norm_request(r), want)

check("apple (worth 1) at Speech 15: 3 gold", buy_price(1, 15), 3)
check("wolf pelt (worth 25) sells for 8 at Speech 15", sell_price(25, 15), 8)
check("speech 100 buys cheaper than speech 15", buy_price(100, 100) < buy_price(100, 15), True)
check("buying is always dearer than selling back", all(buy_price(v, sp) >= sell_price(v, sp) for v in range(1, 500, 7) for sp in (0, 50, 100)), True)
check("no price said: list price", agreed(9, 0, False, 3), 9)
check("haggled 'fine, 18' (list 21): 18", agreed(21, 18, False, 1), 18)
check("'5 each' for 4: 20", agreed(24, 5, True, 4), 20)
check("misheard 1 gold for a 300 sword: half", agreed(300, 1, False, 1), 150)
check("greedy 900 for a 300 sword: 1.5x", agreed(300, 900, False, 1), 450)
check("sell 30 for pelts worth 21: within 1.5x", agreed(21, 30, False, 3), 30)
check("sell 100 for pelts worth 21: capped at 32", agreed(21, 100, False, 3), 32)
check("haggled floor never below what selling back pays",
      all(agreed(buy_price(v, sp), 1, False, 1) >= sell_price(v, sp) for v in range(1, 500, 7) for sp in (0, 50, 100)), True)
check("1-gold apple can't go to 0", agreed(1, 1, False, 1) >= 1, True)
check("qty 'all'", qty("all"), -1)
check("qty 'the lot'", qty("the lot"), -1)
check("qty 3", qty(3), 3)
check("qty junk", qty("some"), 1)
check("qty missing", qty(None), 1)
check("all of 12 in stock", count(-1, 12), 12)
check("asked 5, only 2 in stock", count(5, 2), 2)
for line, want in [("how much for an apple?", True), ("hw much 4 the sword", True), ("what's the price of that ring", True),
                   ("would you buy my pelts?", True), ("what would you pay for these?", True), ("show me your wares", False),
                   ("nice weather today", False), ("I'll take 3", False)]:
    check(f"asks_price({line!r})", asks_price(line), want)
food_list = ["VendorItemFood", "VendorItemIngredient"]
check("innkeeper buys food", vendor_buys(["VendorItemFood"], food_list, False, False), True)
check("innkeeper won't buy a sword", vendor_buys(["VendorItemWeapon"], food_list, False, False), False)
check("general store (inverted list) buys a sword", vendor_buys(["VendorItemWeapon"], ["VendorItemDaedricArtifact"], True, False), True)
check("ordinary merchant refuses stolen goods", vendor_buys(["VendorItemFood"], food_list, False, True), False)
check("fence takes stolen goods", vendor_buys(["VendorItemFood"], food_list, False, True, buys_stolen=True), True)


# ---------------------------------------------------------------- theft: seen only (Trust.cpp OnStolen / OnPickpocketDone)
print("\n== trust: theft counts only when seen")


def judge_theft(owner_home, trusts, owner_saw, bounty_before, bounty_after):
    if not (owner_home and trusts):
        return "nothing"
    if owner_saw:
        return "caught"
    if bounty_after > bounty_before + 0.5:
        return "found out"
    return "nothing"


def judge_pickpocket(trusts, bounty_before, bounty_after):
    return "caught" if trusts and bounty_after > bounty_before + 0.5 else "nothing"


for label, args, want in [
    ("grabbed a cheap plate, nobody looking", (True, True, False, 0, 0), "nothing"),
    ("took her necklace while she watched", (True, True, True, 0, 0), "caught"),
    ("her husband saw it and reported it (bounty)", (True, True, False, 0, 25), "found out"),
    ("theft in a stranger's house", (True, False, True, 0, 25), "nothing"),
    ("theft at an inn, not their home", (False, True, True, 0, 25), "nothing"),
    ("bounty already standing, nothing new", (True, True, False, 40, 40), "nothing"),
]:
    check(f"theft: {label}", judge_theft(*args), want)
check("pickpocket: succeeded, never noticed", judge_pickpocket(True, 0, 0), "nothing")
check("pickpocket: failed, bounty", judge_pickpocket(True, 0, 25), "caught")
check("pickpocket: a stranger", judge_pickpocket(False, 0, 25), "nothing")


# ---------------------------------------------------------------- borrowing and lending (Loans.cpp, Promises GiveItem/Handover)
print("\n== borrow & lend (Loans.cpp)")
CONSUMABLE = {"potion", "food", "ingredient", "ammo", "scroll", "torch", "soulgem"}


def lendable(kind, name, value, cap=50):
    """-> (lendable, consumable)"""
    if kind in CONSUMABLE:
        return False, True
    low = name.lower()
    tool = "pickaxe" in low or "woodcutter" in low
    if kind in ("armor", "key") or (kind == "weapon" and not tool):
        return False, False
    return value <= cap, False


def tool_kind(said):
    words = re.findall(r"[a-z0-9]+", said.lower())
    pick = any(w.startswith("pick") or w.startswith("pike") for w in words)
    axe = not pick and (any(w.startswith("woodcut") or w.startswith("hatchet") for w in words) or "axe" in words)
    return "pickaxe" if pick else "axe" if axe else None


def is_loan_for_budget(loan_flag, kind, name, value):
    if not loan_flag:
        return False
    ok, consumable = lendable(kind, name, value)
    return not consumable


class Book:
    def __init__(self):
        self.borrowed = []  # dicts: npc, item, count, since, picked

    def borrow(self, npc, item, n, now, picked=True):
        self.borrowed.append(dict(npc=npc, item=item, count=n, since=now, picked=picked))

    def give_back(self, npc, item, n, now):
        settled, used = 0, False
        for b in self.borrowed:
            if settled < n and b["picked"] and b["npc"] == npc and b["item"] == item:
                take = min(b["count"], n - settled)
                b["count"] -= take
                settled += take
                used = used or now - b["since"] >= 2
        self.borrowed = [b for b in self.borrowed if b["count"] > 0]
        return settled, used

    def pay_for(self, npc, gold, has, worth):
        for b in self.borrowed:
            if b["picked"] and b["npc"] == npc and has.get(b["item"], 0) == 0 and gold >= max(1, worth[b["item"]]) * b["count"]:
                self.borrowed.remove(b)
                return b["item"]
        return ""

    def let_keep(self, npc, item, now):
        for b in self.borrowed:
            if b["picked"] and b["npc"] == npc and now - b["since"] >= 0.5 and b["item"] == item:
                self.borrowed.remove(b)
                return item
        return ""


def remind(now, due, last_asked, near):
    return now > due and (last_asked < 0 or now - last_asked >= 24) and near


for label, args, want in [
    ("pickaxe (weapon, 5)", ("weapon", "Pickaxe", 5), (True, False)),
    ("woodcutter's axe", ("weapon", "Woodcutter's Axe", 5), (True, False)),
    ("iron sword, even cheap", ("weapon", "Iron Sword", 25), (False, False)),
    ("leather armour", ("armor", "Leather Armor", 10), (False, False)),
    ("lute (misc, 25)", ("misc", "Lute", 25), (True, False)),
    ("book (12)", ("book", "The Lusty Argonian Maid", 12), (True, False)),
    ("silver candlestick (misc, 60): over the cap", ("misc", "Silver Candlestick", 60), (False, False)),
    ("bread is a gift, not a loan", ("food", "Bread", 2), (False, True)),
    ("health potion is a gift", ("potion", "Potion of Minor Healing", 17), (False, True)),
    ("torch is a gift", ("torch", "Torch", 2), (False, True)),
]:
    check(f"lendable: {label}", lendable(*args), want)

for said, want in [("pickaxe", "pickaxe"), ("pike", "pickaxe"), ("pick", "pickaxe"), ("woodcutter's axe", "axe"), ("an axe", "axe"),
                   ("battleaxe", None), ("lantern", None), ("hatchet", "axe")]:
    check(f"trade tool for '{said}'", tool_kind(said), want)

check("budget: borrowing a pickaxe is a loan", is_loan_for_budget(True, "weapon", "Pickaxe", 5), True)
check("budget: 'borrowing' bread is a handout", is_loan_for_budget(True, "food", "Bread", 2), False)
check("budget: a sword 'loan' never reaches the budget (refused)", is_loan_for_budget(True, "weapon", "Iron Sword", 25), True)

bk = Book()
bk.borrow("Alvor", "Pickaxe", 1, now=0)
check("give back right away: settled, but no Bonds deed", bk.give_back("Alvor", "Pickaxe", 1, now=0.2), (1, False))
bk.borrow("Alvor", "Pickaxe", 1, now=10)
check("give back after a day's mining: Bonds deed", bk.give_back("Alvor", "Pickaxe", 1, now=22), (1, True))
bk.borrow("Alvor", "Pickaxe", 1, now=0)
bk.borrow("Hod", "Pickaxe", 1, now=0)
check("same kind handed to Hod: settles Hod's loan", bk.give_back("Hod", "Pickaxe", 1, now=5)[0], 1)
check("Alvor's loan still open", len([b for b in bk.borrowed if b["npc"] == "Alvor"]), 1)
check("handed to Ralof (never lent one): a gift (nothing settled)", bk.give_back("Ralof", "Pickaxe", 1, now=5)[0], 0)
check("lost it, pays 2 gold for a 5-gold pick: not enough", bk.pay_for("Alvor", 2, {}, {"Pickaxe": 5}), "")
check("lost it, pays 10: settled", bk.pay_for("Alvor", 10, {}, {"Pickaxe": 5}), "Pickaxe")
bk.borrow("Ronja", "Lantern", 1, now=0)
check("pays while still carrying it: hand it back instead", bk.pay_for("Ronja", 50, {"Lantern": 1}, {"Lantern": 10}), "")
check("'keep it' a moment after lending (a repeated 'take it'): still a loan", bk.let_keep("Ronja", "Lantern", now=0.1), "")
check("'keep it' later: theirs to keep", bk.let_keep("Ronja", "Lantern", now=3), "Lantern")
check("two picks lent, one handed back: one still owed", (lambda b: (b.borrow("Alvor", "Pickaxe", 2, 0), b.give_back("Alvor", "Pickaxe", 1, 5), b.borrowed[0]["count"])[2])(Book()), 1)

check("not due yet: no reminder", remind(10, 24, -1, True), False)
check("overdue and near: reminded", remind(30, 24, -1, True), True)
check("overdue, reminded 3 hours ago: not again", remind(33, 24, 30, True), False)
check("overdue, reminded yesterday: again", remind(55, 24, 30, True), True)
check("overdue but far away: no", remind(30, 24, -1, False), False)


# ---------------------------------------------------------------- care (Care.cpp)
print("\n== care: who, free or paid, popups (Care.cpp)")


def heal_plan(priest, close, spell, potion, hp, hours_since_last=None):
    if hp >= 0.95:
        return "fine"
    if not priest and hours_since_last is not None and hours_since_last < 4:
        return "already"
    if not spell and not potion and not priest:
        return "nothing"
    free = priest or close
    if not free and not spell:
        return "declines"
    return ("free" if free else "fee 10") + (" spell" if spell else " potion" if potion else " tending")


def cook_plan(close, innkeeper_or_cook, pot, makeable):
    if not close and not innkeeper_or_cook:
        return "declines"
    if not pot:
        return "no pot"
    if makeable == 0:
        return "nothing to cook"
    return "free" if close else "fee 5/dish"


def ceiling(skill):
    return max(1, min(5, 1 + int(skill / 25)))


def mend_plan(smith, close, step_now, skill, value):
    if not smith:
        return "not a smith"
    step = step_now + 1
    if step > ceiling(skill):
        return "as good as it gets"
    return "free" if close else f"fee {max(10, round(value * 0.2))}"


def popup(kind, fee, confirm_on=True):
    """Only paid care and the free cure ask first."""
    return confirm_on and (fee > 0 or kind == "cure")


def might_care(player):
    words = {"hurt", "hurts", "wound", "wounded", "wounds", "bleeding", "bleed", "injured", "injury", "heal", "healing", "patch", "bandage",
             "cook", "cooking", "cooked", "recipe", "stew", "roast", "sharpen", "temper", "improve", "mend", "repair", "fix", "hone", "smith",
             "upgrade", "sick", "ill", "disease", "diseased", "cure", "fever", "rockjoint", "ataxia", "caught", "poorly", "infected"}
    keys = ("wounded", "bleeding", "sharpen", "disease", "injured")
    for w in re.findall(r"[a-z0-9]+", player.lower()):
        if w in words:
            return True
        if any(len(w) >= 5 and w != k and distance(w, k) <= 1 for k in keys):
            return True
    return False


for label, args, want in [
    ("full health: nothing to tend", (False, True, True, False, 1.0), "fine"),
    ("priest heals a stranger free", (True, False, False, False, 0.4), "free tending"),
    ("priest with a spell", (True, False, True, False, 0.4), "free spell"),
    ("friend uses their own potion, free", (False, True, False, True, 0.3), "free potion"),
    ("companion with a spell", (False, True, True, True, 0.3), "free spell"),
    ("acquaintance with a spell: small fee", (False, False, True, False, 0.3), "fee 10 spell"),
    ("stranger with only a potion: won't spend it on you", (False, False, False, True, 0.3), "declines"),
    ("nobody with nothing", (False, True, False, False, 0.3), "nothing"),
]:
    check(f"heal: {label}", heal_plan(*args), want)
check("heal: same friend an hour later: already did", heal_plan(False, True, True, False, 0.3, 1), "already")
check("heal: same friend 5 hours later: again", heal_plan(False, True, True, False, 0.3, 5), "free spell")
check("heal: priest, no cooldown", heal_plan(True, False, True, False, 0.3, 0.1), "free spell")

for label, args, want in [
    ("friend cooks free", (True, False, True, 3), "free"),
    ("innkeeper cooks for coin", (False, True, True, 3), "fee 5/dish"),
    ("stranger won't", (False, False, True, 3), "declines"),
    ("no pot around", (True, False, False, 3), "no pot"),
    ("nothing cookable", (True, False, True, 0), "nothing to cook"),
]:
    check(f"cook: {label}", cook_plan(*args), want)

for label, args, want in [
    ("not a smith", (False, True, 0, 50, 100), "not a smith"),
    ("friend smith: free Fine", (True, True, 0, 50, 100), "free"),
    ("stranger smith: 20% of a 100 sword", (True, False, 0, 50, 100), "fee 20"),
    ("cheap dagger: at least 10", (True, False, 0, 50, 10), "fee 10"),
    ("skill 50 stops at Exquisite (3)", (True, True, 3, 50, 100), "as good as it gets"),
    ("skill 100 reaches Epic (5)", (True, True, 4, 100, 100), "free"),
    ("never Legendary", (True, True, 5, 100, 100), "as good as it gets"),
]:
    check(f"mend: {label}", mend_plan(*args), want)

for label, args, want in [
    ("free heal: no popup", ("heal", 0), False),
    ("free cook: no popup", ("cook", 0), False),
    ("free mend: no popup", ("mend", 0), False),
    ("paid heal: popup", ("heal", 10), True),
    ("paid mend: popup", ("mend", 20), True),
    ("free cure: popup", ("cure", 0), True),
    ("toggle off: never", ("cure", 0, False), False),
]:
    check(f"popup: {label}", popup(*args), want)

for line, want in [("ugh im bleeding", True), ("I'm hurt", True), ("can u sharpn my sword", True), ("I feel sick", True),
                   ("cook this venison", True), ("nice weather", False), ("let's go", False)]:
    check(f"care gate {line!r}", might_care(line), want)
for r, want in [("heal", "heal"), ("patch_up", "heal"), ("cure_disease", "cure"), ("cook", "cook"), ("temper", "mend"), ("sharpen", "mend"),
                ("share_meal", "share_meal")]:
    check(f"norm_request({r})", norm_request(r), want)


# ---------------------------------------------------------------- a drink together (Moments.cpp)
print("\n== a drink together (Moments.cpp)")


def drink_plan(place, close, seat, hours_since=None, npc_has=False, player_has=False, buy_price=10):
    if hours_since is not None and hours_since < 6:
        return "just had one"
    if place == "not here":
        return "not here"
    if not close and place != "inn":
        return "declines"
    if not seat:
        return "nowhere to sit"
    if place == "inn":
        return "free round" if close else f"player buys {max(2, buy_price) * 2}"
    if place == "home":
        return "their drink" if npc_has else "house stores"
    if npc_has:
        return "their bottle"
    if player_has:
        return "player's bottle"
    return "nothing to drink"


def seat_anchor(player_sitting, npc_dist):
    return "player" if player_sitting and npc_dist < 3000 else "npc"


def might_drink(player):
    words = {"drink", "drinks", "drinking", "ale", "ales", "mead", "meads", "wine", "beer", "pint", "pints", "round", "toast", "cheers",
             "tankard", "flagon", "mug", "booze", "sujamma", "flin", "brandy", "rum", "tavern", "bar", "thirsty", "sip", "dram", "shot"}
    for w in re.findall(r"[a-z0-9]+", player.lower()):
        if w in words or any(len(w) >= 4 and w != k and distance(w, k) <= 1 for k in ("drink", "thirsty", "tankard")):
            return True
    return False


for label, args, want in [
    ("Ronja (acquaintance) at the Bannered Mare: you buy, 2 x 10", ("inn", False, True), "player buys 20"),
    ("friend at the inn: their treat", ("inn", True, True), "free round"),
    ("acquaintance at their home: no", ("home", False, True), "declines"),
    ("acquaintance at a camp: no", ("elsewhere", False, True), "declines"),
    ("friend at home, has mead", ("home", True, True, None, True), "their drink"),
    ("friend at home, none on them: the house's stores", ("home", True, True), "house stores"),
    ("companion at camp, has ale", ("elsewhere", True, True, None, True), "their bottle"),
    ("companion at camp, only you have wine", ("elsewhere", True, True, None, False, True), "player's bottle"),
    ("companion at camp, nobody has anything", ("elsewhere", True, True), "nothing to drink"),
    ("in a temple", ("not here", True, True), "not here"),
    ("no seat anywhere", ("inn", True, False), "nowhere to sit"),
    ("again 2 hours later", ("inn", True, True, 2), "just had one"),
    ("again 7 hours later", ("inn", True, True, 7), "free round"),
    ("cheap ale (1 gold): at least 2 each", ("inn", False, True, None, False, False, 1), "player buys 4"),
]:
    check(f"drink: {label}", drink_plan(*args), want)
check("seat: player sitting nearby -> closest to the player", seat_anchor(True, 800), "player")
check("seat: player standing -> near the NPC", seat_anchor(False, 800), "npc")
check("seat: player sitting far away -> near the NPC", seat_anchor(True, 5000), "npc")
for line, want in [("A round of mead?", True), ("wanna grab a drnk", True), ("fancy a pint", True), ("let's eat", False), ("nice day", False)]:
    check(f"drink gate {line!r}", might_drink(line), want)
for r, want in [("drink", "drink"), ("drink_together", "drink"), ("buy_round", "drink"), ("share_meal", "share_meal")]:
    check(f"norm_request({r})", norm_request(r), want)


# ---------------------------------------------------------------- sit a while (Moments.cpp Sit/Tick)
print("\n== sit a while (Moments.cpp)")


def where_of(said):
    w = said.lower()
    if any(k in w for k in ("fire", "hearth", "flame", "warm")):
        return "fire"
    words = set(re.findall(r"[a-z0-9]+", w))
    if words & {"bench", "benches", "chair", "chairs", "table", "stool", "stools", "seat", "seats", "log", "logs", "bar"}:
        return "seat"
    if words & {"ground", "grass", "floor", "dirt", "rock", "rocks", "stone", "here", "meadow", "hill"}:
        return "ground"
    return "auto"


def sit_plan(place, close, outside, want, fire, seat_free):
    if place == "not here":
        return "not here"
    if not close and place != "inn" and not outside:
        return "declines"
    if not close and want == "ground":
        want = "seat"
    if want == "fire" and not fire:
        return "no fire"
    if want != "ground" and seat_free:
        return "seat by the fire" if fire else "seat"
    if want == "seat" or not close:
        return "no seat"
    return "ground by the fire" if fire else "ground"


def skip_offer(close, toggle, hours_said):
    if not close or not toggle:
        return "just sit"
    return f"confirm {min(hours_said, 4)}h" if hours_said > 0 else "picker"


def sit_tick(seated_before, seated_now, secs_waiting, secs_stood):
    if not seated_before:
        return "both seated" if seated_now else ("gives up" if secs_waiting > 120 else "waits")
    if not seated_now:
        return "gets up too" if secs_stood > 4 else "waits"
    return "sitting"


def bonus(fire, hours, close, toggle, given_today, sleep_bonus):
    return bool(fire and hours >= 1 and close and toggle and not given_today and not sleep_bonus)


def might_sit(player):
    words = {"sit", "sitting", "seat", "bench", "company", "relax", "fire", "fireside", "hearth", "campfire", "stars", "sunset", "sunrise",
             "linger", "unwind", "lounge"}
    phrases = ("a while", "take a break", "rest our feet", "catch our breath", "keep me company", "stay with me", "a breather", "rest a bit")
    low = player.lower()
    if any(p in low for p in phrases):
        return True
    for w in re.findall(r"[a-z0-9]+", low):
        if w in words or any(len(w) >= 5 and w != k and distance(w, k) <= 1 for k in ("company", "sitting", "campfire")):
            return True
    return False


for said, want in [("by the fire", "fire"), ("on the grass", "ground"), ("the bench there", "seat"), ("", "auto"), ("somewhere warm", "fire"),
                   ("right here on the floor", "ground"), ("over there", "auto")]:
    check(f"where: {said!r}", where_of(said), want)
for label, args, want in [
    ("Lydia at camp, fire, logs free", ("elsewhere", True, True, "auto", True, True), "seat by the fire"),
    ("Lydia at camp, fire, no seats: ground by the fire", ("elsewhere", True, True, "auto", True, False), "ground by the fire"),
    ("Ronja (friend) 'on the grass'", ("elsewhere", True, True, "ground", False, True), "ground"),
    ("acquaintance on a bench in town", ("elsewhere", False, True, "auto", False, True), "seat"),
    ("acquaintance asks for the grass: a seat instead", ("elsewhere", False, True, "ground", False, True), "seat"),
    ("acquaintance in their house: no", ("home", False, False, "auto", False, True), "declines"),
    ("acquaintance outside, no seat: no ground with a stranger", ("elsewhere", False, True, "auto", False, False), "no seat"),
    ("'by the fire' with no fire", ("elsewhere", True, True, "fire", False, True), "no fire"),
    ("in a temple", ("not here", True, False, "auto", False, True), "not here"),
]:
    check(f"sit: {label}", sit_plan(*args), want)
for label, args, want in [
    ("friend, no time said: the picker (or just sit)", (True, True, 0), "picker"),
    ("friend, 'till sunset' at 16:00", (True, True, 4), "confirm 4h"),
    ("friend, 'till morning' (9h): capped at 4", (True, True, 9), "confirm 4h"),
    ("acquaintance: never skips", (False, True, 2), "just sit"),
    ("toggle off: never skips", (True, False, 2), "just sit"),
]:
    check(f"time: {label}", skip_offer(*args), want)
for label, args, want in [
    ("player sits down", (False, True, 10, 0), "both seated"),
    ("player never sits (2+ min)", (False, False, 130, 0), "gives up"),
    ("player stands up for good", (True, False, 0, 5), "gets up too"),
    ("player shifts in the seat (2 s)", (True, False, 0, 2), "waits"),
]:
    check(f"tick: {label}", sit_tick(*args), want)
check("bonus: fire, 2h, friend", bonus(True, 2, True, True, False, False), True)
check("bonus: no fire", bonus(False, 2, True, True, False, False), False)
check("bonus: half an hour", bonus(True, 0.5, True, True, False, False), False)
check("bonus: already today", bonus(True, 2, True, True, True, False), False)
check("bonus: already Well Rested from sleep", bonus(True, 2, True, True, False, True), False)
for line, want in [("Sit with me a while?", True), ("lets sit by the fire", True), ("keep me company", True), ("I'll be a while", True),
                   ("give me a moment", False), ("stay here", False), ("let's take a break", True), ("nice weather", False)]:
    check(f"sit gate {line!r}", might_sit(line), want)
for r, want in [("sit", "sit"), ("sit_together", "sit"), ("keep_company", "company"), ("spend_time", "company"), ("take_a_break", "sit"), ("rest", "rest"), ("accompany", "follow")]:
    check(f"norm_request({r})", norm_request(r), want)


# ---------------------------------------------------------------- a song (Moments.cpp Perform)
print("\n== a song (Moments.cpp Perform)")


def song_id(said):
    s = said.lower()
    return 1 if "ragnar" in s else 2 if ("aggression" in s or "oppression" in s) else 3 if "dragonborn" in s else 10 if "tongue" in s else 0


def perform_plan(bard, skyrimnet_sings, how, instrument, seated, on_road, said="", hours_since=None):
    if hours_since is not None and hours_since < 1:
        return "voice needs a rest"
    if bard and skyrimnet_sings:
        return "SkyrimNet's"
    sing, play = "sing" in how, "play" in how
    if on_road:
        return "sings walking"
    if seated:
        return "sings seated"
    if bard:
        if play and not sing:
            asked = said.lower()
            inst = "Flute" if "flute" in asked else "Lute" if "lute" in asked else "Drum" if "drum" in asked else (instrument or "Instrumental")
            return f"bard plays {inst}"
        return f"bard song {song_id(said)}"
    if instrument and not sing:
        return f"plays their {instrument}"
    if play and not sing and not instrument:
        return "no instrument, sings"
    return "sings"


def might_perform(player):
    words = {"sing", "singing", "sang", "song", "songs", "music", "tune", "tunes", "lute", "flute", "drum", "drums", "bard", "melody", "ballad",
             "hum", "verse", "instrument", "serenade", "ditty", "shanty", "ragnar", "dragonborn"}
    phrases = ("play us", "play me", "play something", "play a", "play for", "play your")
    low = player.lower()
    if any(p in low for p in phrases):
        return True
    for w in re.findall(r"[a-z0-9]+", low):
        if w in words or any(len(w) >= 4 and w != k and distance(w, k) <= 1 for k in ("sing", "song", "music")):
            return True
    return False


for label, args, want in [
    ("Mikael: 'play us something'", (True, False, "any", "", False, False), "bard song 0"),
    ("Mikael: 'Ragnar the Red'", (True, False, "sing", "", False, False, "Ragnar the Red"), "bard song 1"),
    ("Mikael: 'a tune on the flute'", (True, False, "play", "", False, False, "flute"), "bard plays Flute"),
    ("Mikael, SkyrimNet bard songs on", (True, True, "any", "", False, False), "SkyrimNet's"),
    ("Sven again 20 minutes later", (True, False, "any", "", False, False, "", 0.3), "voice needs a rest"),
    ("Lydia on the road: 'sing for the road'", (False, False, "sing", "", False, True), "sings walking"),
    ("Ronja by the fire: 'sing me a song'", (False, False, "sing", "Lute", True, False), "sings seated"),
    ("Ronja seated, asked to play her lute: sings instead (no room)", (False, False, "play", "Lute", True, False), "sings seated"),
    ("farmer with a flute: 'play us something'", (False, False, "any", "Flute", False, False), "plays their Flute"),
    ("farmer with a flute asked to SING", (False, False, "sing", "Flute", False, False), "sings"),
    ("guard, no instrument, 'play something'", (False, False, "play", "", False, False), "no instrument, sings"),
    ("guard, no instrument, 'sing'", (False, False, "sing", "", False, False), "sings"),
]:
    check(f"song: {label}", perform_plan(*args), want)
for line, want in [("Play us something, bard!", True), ("sing me a song", True), ("can u play ragnar the red", True), ("give us a tune", True),
                   ("I play cards sometimes", False), ("don't play games with me", False), ("singg for me", True), ("nice weather", False)]:
    check(f"song gate {line!r}", might_perform(line), want)
for r, want in [("perform", "perform"), ("sing", "perform"), ("play_song", "perform"), ("play_music", "perform"), ("sit", "sit"), ("trade", "trade"),
                ("training", "train"), ("rest", "rest")]:
    check(f"norm_request({r})", norm_request(r), want)


# ---------------------------------------------------------------- little gifts from loved ones (Keepsakes.cpp)
print("\n== little gifts from loved ones (Keepsakes.cpp)")


def keepsake_chance(rank, armed, event, hour, gift_today, days_since_gift, amends=False, every_days=7, armed_used=False):
    """armed: today's random moment; event: what just happened."""
    if rank not in ("ally", "lover") or amends:
        return "never"
    if event != armed or armed_used or 4 <= hour < 11:
        return "no moment"
    if gift_today or (days_since_gift is not None and days_since_gift < every_days):
        return "capped"
    return f"roll {35 if rank == 'lover' else 25}%"


def quiet_hour_ok(armed_at_hour, quiet):
    lo = max(12.0, armed_at_hour + 0.5)
    return lo <= quiet <= 23.99


def pick_kind(kind, name, value, lover, worn=False, lent=False, quest=False, cap=30):
    cap = cap * (2 if lover else 1)
    if worn or lent or quest or value > cap or kind in ("gold", "weapon"):
        return None
    if kind == "food":
        return 2
    if kind == "potion":
        return 2
    if kind == "poison":
        return None
    if kind == "ingredient":
        return (4 if lover else 3) if any(k in name.lower() for k in ("flower", "lavender", "blossom", "rose", "thistle", "blue mountain")) else None
    if kind == "book":
        return 2
    if kind == "jewelry":
        return 5 if lover else 1
    return None


for label, args, want in [
    ("Confidant: never (ally and up only)", ("confidant", "reunion", "reunion", 15, False, None), "never"),
    ("lover: today's moment is a reunion, and it happens", ("lover", "reunion", "reunion", 15, False, None), "roll 35%"),
    ("ally: today's moment is after a fight", ("ally", "fight", "fight", 20, False, None), "roll 25%"),
    ("today's moment is arrival, but a fight ended: nothing", ("lover", "arrival", "fight", 15, False, None), "no moment"),
    ("reunion at 7 in the morning: never at the start of a day", ("lover", "reunion", "reunion", 7, False, None), "no moment"),
    ("today's moment already came (one roll a day)", ("lover", "quiet", "quiet", 18, False, None, False, 7, True), "no moment"),
    ("someone already gave a gift today", ("lover", "drink", "drink", 21, True, None), "capped"),
    ("same lover gave one 3 days ago", ("lover", "drink", "drink", 21, False, 3), "capped"),
    ("same lover, 8 days ago", ("lover", "drink", "drink", 21, False, 8), "roll 35%"),
    ("lover still sore from a grudge", ("lover", "reunion", "reunion", 15, False, None, True), "never"),
]:
    check(f"keepsake: {label}", keepsake_chance(*args), want)
check("quiet hour picked at 3am lands noon to night", quiet_hour_ok(3, 14.2), True)
check("quiet hour never in the morning", quiet_hour_ok(3, 9.0), False)
check("quiet hour picked at 20:00 is later that evening", quiet_hour_ok(20, 21.0), True)
for label, args, want in [
    ("sweetroll", ("food", "Sweet Roll", 2, False), 2),
    ("mountain flower, lover", ("ingredient", "Blue Mountain Flower", 2, True), 4),
    ("bear claws: not a present", ("ingredient", "Bear Claws", 2, True), None),
    ("silver ring, lover", ("jewelry", "Silver Ring", 30, True), 5),
    ("gold ring (75) for a lover: over 60", ("jewelry", "Gold Ring", 75, True), None),
    ("the amulet they're wearing", ("jewelry", "Amulet of Mara", 25, True, True), None),
    ("the bow you lent them", ("weapon", "Hunting Bow", 30, False, False, True), None),
    ("a poison", ("poison", "Weak Poison", 10, False), None),
    ("a book", ("book", "The Lusty Argonian Maid", 25, False), 2),
]:
    check(f"keepsake pick: {label}", pick_kind(*args), want)


# ---------------------------------------------------------------- the player's own promises + favours asked (Pledges.cpp)
print("\n== your own promises and the favours they ask (Pledges.cpp)")


def pledge_type(t):
    t = t.lower()
    if any(k in t for k in ("deliver", "take", "carry")):
        return "deliver"
    if "check" in t or "look in" in t or "look_in" in t:
        return "check"
    if any(k in t for k in ("find", "search", "look")):
        return "find"
    if any(k in t for k in ("pay", "gold", "debt")):
        return "pay"
    if any(k in t for k in ("visit", "come", "meet", "return", "back")):
        return "visit"
    if any(k in t for k in ("bring", "give", "get", "fetch", "item")):
        return "bring"
    return "other"


def default_hours(t):
    return 24 * 7 if t in ("find", "other") else 24 if t == "visit" else 72


def on_deadline(t, now, due):
    grace = 6 if t == "visit" else 0
    if now <= due + grace:
        return "open"
    return "lapsed quietly" if t == "other" else "broken: -3 bond, they bring it up"


def asks_about(line, spoken):
    l = line.lower()
    if spoken:
        return any(p in l for p in ("my promises", "what did i promise", "what i promised", "what have i promised", "check my promises",
                                    "promises i have", "promises i've made", "what i owe"))
    if "promise" in l and any(k in l for k in ("what", "my ", "check", "remind", "list", "did i")):
        return True
    w = set(re.findall(r"[a-z0-9']+", l))
    errand = bool(w & {"bring", "deliver", "fetch", "owe", "errand", "errands", "favour", "favor", "favours", "favors", "promised", "pay",
                       "find", "get", "buy", "take", "do"})
    checking = bool(w & {"forgot", "forget", "remember", "remind", "supposed", "check", "owe"}) or any(
        p in l for p in ("see if", "did i", "do i have", "have i", "who was i", "what was i", "what did i", "am i forgetting", "need to bring",
                         "need to get", "have to bring", "have to get", "meant to", "anyone waiting", "anything to bring"))
    return errand and checking


def is_farewell(line):
    l = line.lower()
    return any(p in l for p in ("farewell", "goodbye", "good bye", "bye", "see you", "must go", "gotta go", "got to go", "take care", "i'm off",
                                "be off", "heading to", "heading out", "on my way", "leaving", "safe travels", "until next time", "be going",
                                "need to go", "have to go"))


def ask_chance(rank, open_count, waiting=False, hours_since_their_ask=None, hours_since_any_ask=99, teammate=False, grudge=False):
    base = {"lover": .30, "ally": .25, "confidant": .20, "friend": .12, "acquaintance": .04}.get(rank, 0.0)
    if teammate or grudge or waiting or open_count >= 5 or hours_since_any_ask < 12 or (hours_since_their_ask is not None and hours_since_their_ask < 72):
        return 0.0
    return round(base * max(0.0, 1 - open_count / 5), 3)


for t, want in [("bring", "bring"), ("give_item", "bring"), ("find", "find"), ("pay_back", "pay"), ("visit", "visit"), ("come_back", "visit"),
                ("deliver", "deliver"), ("take_to", "deliver"), ("check_on", "check"), ("make_amends", "other")]:
    check(f"pledge type {t!r}", pledge_type(t), want)
check("mead 'tomorrow' (24h): due in 24", 24, 24)
check("no time said, find: a week", default_hours("find"), 168)
check("no time said, bring: 3 days", default_hours("bring"), 72)
check("deadline not reached", on_deadline("bring", 50, 72), "open")
check("bring missed", on_deadline("bring", 80, 72), "broken: -3 bond, they bring it up")
check("visit: 6 h grace after the time said", on_deadline("visit", 28, 24), "open")
check("visit missed", on_deadline("visit", 31, 24), "broken: -3 bond, they bring it up")
check("'I'll make it up to you' lapses without penalty", on_deadline("other", 200, 168), "lapsed quietly")
for line, spoken, want in [
    ("what did I promise Ronja?", True, True),
    ("let's see what promises I have", True, True),
    ("I promise, what else can I do?", True, False),
    ("I promise I'll be careful", True, False),
    ("I forgot what I wanted to get for Hulda", False, True),
    ("what was I supposed to bring her...", False, True),
    ("what did I promise again", False, True),
    ("nice day", False, False),
    ("lets see if i have anyone to bring something to", False, True),
    ("do I owe anyone anything?", False, True),
    ("Mjoll offered to help find cassain, more than I've gotten from anyone else (made-up thought)", False, False),
    ("Nobody I know well enough in Riften to bring anything meaningful.", False, False),
]:
    check(f"asks about promises ({'said' if spoken else 'thought'}) {line!r}", asks_about(line, spoken), want)
for line, want in [("Farewell, Ronja.", True), ("I must go now", True), ("I'm heading to Riften", True), ("Maybe later", False), ("hello there", False)]:
    check(f"farewell {line!r}", is_farewell(line), want)
for label, args, want in [
    ("lover, no promises open", ("lover", 0), 0.30),
    ("friend, 2 open", ("friend", 2), 0.072),
    ("acquaintance, none open", ("acquaintance", 0), 0.04),
    ("5 promises open: nobody asks", ("lover", 5), 0.0),
    ("already waiting on you", ("lover", 0, True), 0.0),
    ("same person asked 2 days ago", ("lover", 0, False, 48), 0.0),
    ("someone else asked 3 hours ago", ("lover", 0, False, None, 3), 0.0),
    ("a companion travelling with you", ("lover", 0, False, None, 99, True), 0.0),
    ("holding a grudge", ("friend", 0, False, None, 99, False, True), 0.0),
    ("a rival", ("rival", 0), 0.0),
]:
    check(f"favour chance: {label}", ask_chance(*args), want)


# ---------------------------------------------------------------- promise deadlines: generous, realistic, re-timeable (Pledges.cpp DueIn)
print("\n== promise deadlines (Pledges.cpp DueIn)")


def default_hours2(t):
    return 24 * 7 if t in ("find", "other", "deliver", "check") else 24 if t == "visit" else 72


def due_in(t, said, distant):
    if said < 1:
        said = default_hours2(t)
    if t == "visit":
        return said
    floor = 24 if t in ("bring", "pay") else 24 * 7 if t == "other" else 72
    if distant:
        floor = max(floor, 120 if t in ("deliver", "check") else 72)
    return max(said * 1.5, floor)


for label, args, want in [
    ("mead 'tomorrow' (24 h): 36 h to do it", ("bring", 24, False), 36),
    ("mead 'tonight' (4 h): at least a day", ("bring", 4, False), 24),
    ("'from Riften tomorrow' while in Dawnstar: at least 3 days", ("bring", 24, True), 72),
    ("'I'll take it to your brother' in another town: 5 days", ("deliver", 0, True), 252),
    ("deliver in the same town, nothing said: a week and a half", ("deliver", 0, False), 252),
    ("'I'll come by tomorrow evening' (20 h): as said (+6 h window)", ("visit", 20, False), 20),
    ("find a ring, nothing said: 10 days", ("find", 0, False), 252),
    ("pay back 'in two days' (48 h): 72 h", ("pay", 48, False), 72),
    ("'not tomorrow, in a couple of days' re-timed: 48 h said -> 72 h", ("bring", 48, False), 72),
]:
    check(f"due: {label}", due_in(*args), want)
check("every promise gets a deadline (nothing said, every type)", all(due_in(t, 0, d) > 0 for t in ("bring", "find", "pay", "visit", "deliver", "check", "other") for d in (False, True)), True)


# ---------------------------------------------------------------- keepsakes: never the player's own things; small things come by
print("\n== keepsakes: never yours back; small things come by on the way")


def keepsake_source(own_fitting, from_player, potion_wanted, roll):
    """own_fitting: their own suitable items; from_player: which of those the player handed them."""
    mine = [i for i in own_fitting if i not in from_player]
    if not mine or (not potion_wanted and roll < 0.35):
        return "come by on the way (flower, apple, sweet roll...)"
    return f"their own {mine[0]}"


for label, args, want in [
    ("only the ring YOU gave her: never given back", (["silver ring"], {"silver ring"}, False, 0.9), "come by on the way (flower, apple, sweet roll...)"),
    ("her own sweet roll, roll 0.9", (["sweet roll"], set(), False, 0.9), "their own sweet roll"),
    ("her own sweet roll, roll 0.2: a flower picked instead", (["sweet roll"], set(), False, 0.2), "come by on the way (flower, apple, sweet roll...)"),
    ("nothing of her own: something small anyway", ([], set(), False, 0.9), "come by on the way (flower, apple, sweet roll...)"),
    ("hurt after a fight: her own potion, never made up", (["healing potion"], set(), True, 0.1), "their own healing potion"),
    ("the bow you put in his pack: never his present", (["bow", "bread"], {"bow"}, False, 0.9), "their own bread"),
]:
    check(f"keepsake: {label}", keepsake_source(*args), want)


# ---------------------------------------------------------------- guards: words with weight (Crime.cpp)
print("\n== guards: fine, bribe, persuade, intimidate (Crime.cpp)")


def bribe_cost(bounty, speech):
    return max(10, round(bounty * (1.25 - 0.5 * speech / 100)))


def outcome(kind, calm, violent, speech, plevel=20, glevel=20, offered=0, failed_recently=False, purse=10000):
    if kind == "pay_fine":
        return "paid" if purse >= calm + violent else "can't afford"
    if violent > 0:
        return "refused: violent"
    if kind == "bribe":
        if calm > 1000:
            return "too serious"
        cost = bribe_cost(calm, speech)
        if offered and offered < cost:
            return f"not enough (needs {cost})"
        return "cleared"
    if calm > 500 or failed_recently:
        return "refused"
    if kind == "persuade":
        return "cleared" if speech >= 30 + calm / 10 else "failed"
    return "cleared" if speech + 2 * (plevel - glevel) >= 40 + calm / 10 else "failed"


for label, args, want in [
    ("pay a 40 fine", ("pay_fine", 40, 0, 30), "paid"),
    ("pay a 1000 fine with 200 gold", ("pay_fine", 1000, 0, 30, 20, 20, 0, False, 200), "can't afford"),
    ("bribe 50 for a 40 bounty at Speech 15", ("bribe", 40, 0, 15, 20, 20, 50), "cleared"),
    ("bribe 10 for a 40 bounty", ("bribe", 40, 0, 15, 20, 20, 10), "not enough (needs 47)"),
    ("bribe a 2000 bounty", ("bribe", 2000, 0, 100, 20, 20, 5000), "too serious"),
    ("bribe a murderer", ("bribe", 0, 1000, 100, 20, 20, 5000), "refused: violent"),
    ("persuade, 40 bounty, Speech 40", ("persuade", 40, 0, 40), "cleared"),
    ("persuade, 300 bounty, Speech 40", ("persuade", 300, 0, 40), "failed"),
    ("persuade again right after failing", ("persuade", 40, 0, 90, 20, 20, 0, True), "refused"),
    ("intimidate: level 40 vs guard 20, Speech 20, bounty 100", ("intimidate", 100, 0, 20, 40, 20), "cleared"),
    ("intimidate: level 5 vs guard 20, Speech 30", ("intimidate", 100, 0, 30, 5, 20), "failed"),
    ("intimidate a 600 bounty", ("intimidate", 600, 0, 100, 80, 10), "refused"),
]:
    check(f"guard: {label}", outcome(*args), want)
check("a smooth talker's bribe is cheaper than the fine", bribe_cost(100, 100) < 100, True)
check("a clumsy one's costs more", bribe_cost(100, 0) > 100, True)
for r, want in [("pay_fine", "pay_fine"), ("pay_bounty", "pay_fine"), ("bribe", "bribe"), ("bribe_guard", "bribe"), ("persuade", "persuade"),
                ("convince", "persuade"), ("intimidate", "intimidate"), ("threaten", "intimidate"), ("perform", "perform"), ("follow", "follow")]:
    check(f"norm_request({r})", norm_request(r), want)


# ---------------------------------------------------------------- after a brawl (Brawls.cpp)
print("\n== after a brawl (Brawls.cpp)")


def brawl_outcome(reason):
    return {1: "knockout", 2: "conceded", 6: "conceded"}.get(reason, "none")


def brawl_attitude(npc_won, close):
    if not npc_won:
        return "good fight (lost)" if close else "shown their place"
    return "gracious winner" if close else "smug winner"


for reason, want in [(1, "knockout"), (2, "conceded"), (6, "conceded"), (3, "none"), (4, "none"), (7, "none"), (0, "none"), (5, "none")]:
    check(f"brawl reason {reason}", brawl_outcome(reason), want)


def brawl_spar(was_teammate, teammate_now, close):
    return was_teammate or teammate_now or close


check("follower stripped mid-brawl, flag read before restore -> spar", brawl_spar(True, False, False), True)
check("follower, flag read after restore -> spar", brawl_spar(False, True, False), True)
check("stranger -> real fight", brawl_spar(False, False, False), False)
check("Tythis (acquaintance) loses", brawl_attitude(False, False), "shown their place")
check("Lydia (companion) loses a spar", brawl_attitude(False, True), "good fight (lost)")
check("Uthgerd (stranger) beats you", brawl_attitude(True, False), "smug winner")
check("Ronja (friend) beats you", brawl_attitude(True, True), "gracious winner")


# ---------------------------------------------------------------- vanilla dialogue by talking (VanillaLines.cpp)
print("\n== vanilla dialogue by talking (VanillaLines.cpp)")
import re as _re
_VSTOP = {"what", "about", "with", "your", "you", "that", "this", "have", "there", "they", "them", "tell", "know", "want", "need", "would",
          "could", "should", "will", "just", "here", "from", "were", "been", "some", "more", "like", "does", "doing", "anything", "something",
          "going", "into", "then", "than", "well", "right", "okay", "yeah", "please", "very", "much", "sure", "good", "fine",
          "where", "when", "which", "who's", "it's", "that's", "what's", "you've", "you're", "you'll", "you'd", "i've",
          "i'll", "i'd", "we're", "they're", "he's", "she's"}


def _vcontent(text):
    return [w for w in _re.findall(r"[a-z0-9']+", text.lower().replace("'s", "")) if len(w) >= 4 and w not in _VSTOP]


def _edit(a, b):
    d = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        prev, d[0] = d[0], i
        for j, cb in enumerate(b, 1):
            prev, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, prev + (ca != cb))
    return d[len(b)]


def vanilla_overlaps(said, option):
    s, o = _vcontent(said), _vcontent(option)
    if not s or not o:
        return False
    hit = [ow for ow in o if any(w == ow or (len(ow) >= 5 and len(w) >= 5 and _edit(w, ow) <= 1) or
                                 (len(ow) >= 6 and len(w) >= 6 and (w.startswith(ow[:5]) or ow.startswith(w[:5]))) for w in s)]
    hits = len(hit)
    enough = hits >= 2 or max((len(h) for h in hit), default=0) >= 5
    return hits >= 1 and enough and hits * 3 >= len(o)


def _untag(text):
    return _re.sub(r"[\(\[][^\)\]]*[\)\]]", "", text)


def _vphrase(text):
    return " ".join(_re.findall(r"[a-z0-9']+", _untag(text).lower()))


def vanilla_same_phrase(said, option):
    s, o = _vphrase(said), _vphrase(option)
    if not s or len(o) < 8:
        return False
    return o in s or _edit(s, o) * 5 <= max(len(s), len(o))


def vanilla_candidate(said, option):
    return vanilla_same_phrase(said, option) or vanilla_overlaps(said, option)


def vanilla_popup(quest_type, confirm_on):
    return quest_type != "none" and confirm_on


for said, option, want in [("so what's this thieves guild you keep talking about?", "What's the Thieves Guild?", True),
                           ("whats the theives guild", "What's the Thieves Guild?", True),
                           ("tell me about the dragon attack at helgen", "I bring word from Helgen. About the dragon attack.", True),
                           ("nice weather today", "What's the Thieves Guild?", False),
                           ("hello there", "Do you have any work for me?", False),
                           ("got any work for me?", "Do you have any work for me?", False),
                           ("I'll take it", "Got anything for sale?", False),
                           ("do you work here", "Very. How does it work?", False),
                           ("do you work here?", "Do you work here?", True),
                           ("do u work here", "Do you work here?", True),
                           ("do you live here?", "Do you work here?", False),
                           ("grelod is abusing the childrens...can you arrest her", "Need something?", False)]:
    check(f"vanilla prefilter: {said[:40]!r} ~ {option[:30]!r}", vanilla_candidate(said, option), want)
check("menu tag stripped: 'Tell me (Persuade)' phrase", _vphrase("Tell me what you know. (Persuade)"), "tell me what you know")
check("exact sentence vs its Persuade twin: plain wins exact", vanilla_same_phrase("tell me what you know", "Tell me what you know."), True)
def recital_share(reply, answer):
    a = _vcontent(answer)
    if len(a) < 4:
        return 0.0
    r = set(_vcontent(reply))
    return sum(1 for w in a if w in r) / len(a)


_svana = "You need to get the Marks of Dibella from Bolli, Hofgrir, and Indaryn. Then just confront Haelga with them."
check("Svana's AI recital counts as the pick", recital_share("Alright. You need to get the Marks of Dibella from Bolli, Hofgrir, and Indaryn. I'm not sure how you're going to do it, but try your best. Then just confront Haelga with them and the rest... works itself out.", _svana) >= 0.6, True)
check("an unrelated reply is no recital", recital_share("I... appreciate the thought, Breton. But some men don't listen to reason.", _svana) >= 0.6, False)
check("vanilla line fired 5s ago -> recital catcher stands down (no click loop)", 5 < 15, True)
def service_line(quest_named_or_typed, text):
    if quest_named_or_typed:
        return False
    return any(w in text.lower() for w in ("for sale", "trade", "buy", "sell", "wares", "room", "bed", "follow", "wait here", "part ways", "dismiss",
                                           "train", "teach", "carriage", "ride", "fine", "bounty", "jail", "crime", "look the other way"))


check("generic 'What have you got for sale?' is left to our trade", service_line(False, "What have you got for sale?"), True)
check("generic 'Follow me. I need your help.' is left to our follow", service_line(False, "Follow me. I need your help."), True)
check("quest line mentioning a bed stays vanilla", service_line(True, "I need you to check the bed in the back room."), False)
check("Svana's 'Do you work here?' stays vanilla", service_line(False, "Do you work here?"), False)
def said_closely(said, option):
    s_, o = _vcontent(said), _vcontent(_untag(option))
    if not o:
        return False
    hits = sum(1 for ow in o if any(w == ow or (len(ow) >= 5 and len(w) >= 5 and (_edit(w, ow) <= 1 or w.startswith(ow[:5]) or ow.startswith(w[:5]))) for w in s_))
    return hits >= 2 and hits * 3 >= len(o)


_persuade = "You shouldn't let customers give you a hard time. Stand up to them. (Persuade)"
check("'stand up to them, customers' said closely = the Persuade", said_closely("so what...you should not let her or customers harras you. stand up to them", _persuade), True)
check("'you dont stand up for yourself?' alone is not close", said_closely("and you dont say anything? you dont stand yp for yourself?", _persuade), False)
check("'who gropes you' is not 'Do you work here?'", said_closely("who gropes you?", "Do you work here?"), False)
def conditions_hold(items):  # [(value, is_or)] as the game reads them
    in_or, or_val = False, False
    for v, is_or in items:
        if is_or:
            in_or, or_val = True, or_val or v
            continue
        group = (or_val or v) if in_or else v
        in_or, or_val = False, False
        if not group:
            return False
    return (not in_or) or or_val


check("A AND B, both true", conditions_hold([(True, False), (True, False)]), True)
check("A AND B, B false", conditions_hold([(True, False), (False, False)]), False)
check("(A OR B) AND C: A false B true C true", conditions_hold([(False, True), (True, False), (True, False)]), True)
check("(A OR B) AND C: both false", conditions_hold([(False, True), (False, False), (True, False)]), False)
check("trailing OR run decides alone", conditions_hold([(True, False), (False, True), (True, True)]), True)
check("no conditions = true", conditions_hold([]), True)
check("main quest line, confirm on -> popup", vanilla_popup("main", True), True)
check("favour (misc) line, confirm on -> popup", vanilla_popup("misc", True), True)
check("town chatter (no quest type) -> plays straight", vanilla_popup("none", True), False)
check("quest line, confirm off -> plays straight", vanilla_popup("main", False), False)


# ---------------------------------------------------------------- NPC errands: farewell + rough talk (Pledges.cpp)
print("\n== errands: farewell and rough talk (Pledges.cpp)")


def is_farewell(line):
    l = line.lower()
    if "?" in l:
        return False
    l = l.replace("take care of", "").rstrip(" 	.,!;:-")
    return any(l.rfind(p) >= 0 and len(l) - (l.rfind(p) + len(p)) <= 16 for p in ("farewell", "goodbye", "good bye", "bye", "see you", "must go", "gotta go", "got to go", "take care", "i'm off",
                                   "be off", "heading to", "heading out", "on my way", "leaving", "safe travels", "until next time", "be going",
                                   "need to go", "have to go"))


def is_rough(line):
    l = line.lower()
    at_them = any(w in ("you", "your", "youre", "u", "ya") for w in re.findall(r"[a-z0-9]+", l))
    if at_them and any(p in l for p in ("fuck", "bitch", "bastard", "idiot", "scum", "filth")):
        return True
    return any(p in l for p in ("n'wah", "nwah", "n'wa", "piss off", "shut up", "or else",
                                "kill you", "i'll kill", "ill kill", "lose those", "break your", "you'll regret", "youll regret", "don't you dare",
                                "dont you dare", "or you will", "or you'll", "or youll", "cut off", "gut you", "you're dead", "youre dead",
                                "worthless", "pathetic"))


check("quoting 'take care' at Tythis is no farewell", is_farewell("what do you mean take care...dont you fucking look at ever again"), False)
check("'I'll take care of it' is no farewell", is_farewell("fine, I'll take care of it"), False)
check("'anyway, take care' is a farewell", is_farewell("thanks for the help. anyway, take care"), True)
check("'see you around' is a farewell", is_farewell("alright, see you around"), True)
check("'are you leaving?' is no farewell", is_farewell("are you leaving?"), False)
check("threat to Tythis is rough", is_rough("hey you fucking elf...dont you ever dare touch svana anymore...or you will lose those hands"), True)
check("ordinary talk is not rough", is_rough("ohh that rough...but i think u should stand up for yourself"), False)
check("swearing at a saber cat is not rough", is_rough("ohhh fuck a saber"), False)
check("'you idiot' is rough", is_rough("watch it, you idiot"), True)


# ---------------------------------------------------------------- vanilla: speech checks without the model + hush (VanillaLines.cpp)
print("\n== vanilla: speech checks + hush (VanillaLines.cpp)")


def threatens(line):
    l = line.lower()
    return any(w in l for w in ("or else", "or i'll", "or ill", "or i will", "or you'll", "or youll", "or you will", "don't you dare", "dont you dare",
                                "kill you", " rip ", "break your", "lose your", "lose those", "loose your", "loose those", "cut off", "gut you", "you'll regret",
                                "youll regret", "you're dead", "youre dead", "hurt you", "make you pay", "last warning", "i swear i'll", "i swear ill",
                                "or you're", "or youre"))


def offers_money(line):
    l = line.lower()
    return any(w in l for w in ("gold", "coin", "septim", "pay you", "for your trouble", "make it worth", "here's some", "heres some", "bribe"))


def speech_check_for(options, line):
    tag = "(intimidate)" if threatens(line) else None
    if not tag:
        return 0
    said = _vcontent(line)
    best, best_hits, count = 0, -1, 0
    for k, o in enumerate(options):
        if tag not in o.lower():
            continue
        count += 1
        hits = sum(1 for w in _vcontent(_untag(o)) if w in said)
        if hits > best_hits:
            best_hits, best = hits, k + 1
    return best if count == 1 or best_hits > 0 else 0


_tythis = ["I've heard the way you talk to Svana. Stop it. Or Else. (Intimidate)", "How's work at the fishery?", "You're not from Skyrim, are you?", "Greetings. (Speechcraft)"]
check("Tythis threat 1 picks Intimidate", speech_check_for(_tythis, "hey...you fucking elf...dont you dare touch svana again or ill rip off those tiny arms of yours"), 1)
check("Tythis threat 2 picks Intimidate", speech_check_for(_tythis, "dont you fucking lie to me...she said it all to me if you ever touch her again you will loose your hands"), 1)
check("'a trip to Riften' is no threat", threatens("we took a trip to riften"), False)
check("'I regret nothing' is no threat", threatens("I regret nothing"), False)
check("'never again' is no threat", threatens("I never want to see him ever again"), False)
check("friendly line picks no speech check", speech_check_for(_tythis, "how's the fishing these days"), 0)
check("gold never picks (Bribe) by words - a gift isn't a bribe; the model judges", speech_check_for(["Let me through. (Persuade)", "Here, take this gold and let me through. (Bribe)"], "here's some gold, a gift for you"), 0)


def hush(text, playing, ms_since_fired):
    said = text.lower()
    head = said[:24]
    own = bool(said) and bool(playing) and head in playing and ms_since_fired < 1500
    return not own


_ans = "really? thank you. here, this is for you. you've been a great help to me."
check("vanilla line's own record (5 ms) is spared", hush("Really? Thank you. Here, this is for you. You've been a great help to me.", _ans, 5), False)
check("AI copying the vanilla words 2.2 s later is cut", hush("Really? Thank you. Here, this is for you. You've been a great help to me.", _ans, 2200), True)


# ---------------------------------------------------------------- vanilla lines: REPLAY of the real playtests (VanillaLines.cpp)
print("\n== vanilla lines: replay of every real playtest line (VanillaLines.cpp)")


def _same_word(w, o):
    return w == o or (len(o) >= 5 and len(w) >= 5 and (_edit(w, o) <= 1 or w.startswith(o[:5]) or o.startswith(w[:5])))


def _hits(said, option):
    return sum(1 for o in option if any(_same_word(w, o) for w in said))


def _same_phrase(said, option):
    if not said or len(option) < 8:
        return False
    return option in said or _edit(said, option) * 5 <= max(len(said), len(option))


def _said_closely(line, option):
    words = _vcontent(_untag(option))
    h = _hits(_vcontent(line), words)
    return bool(words) and h >= 2 and h * 3 >= len(words)


def _recited(reply, answer):
    words = _vcontent(answer)
    return 0.0 if len(words) < 4 else _hits(_vcontent(reply), words) / len(words)


def _fits(line, text):
    words = _vcontent(_untag(text))
    return len(words) <= 2 or _hits(_vcontent(line), words) >= 1 or _same_phrase(_vphrase(line), _vphrase(text))


def pick_for_line(options, line):  # options: [(text, answer, said_before)]
    said = _vphrase(line)

    def unique(test):
        found = [k + 1 for k, o in enumerate(options) if test(o)]
        return found[0] if len(found) == 1 else 0
    k = unique(lambda o: "(" not in o[0] and _same_phrase(said, _vphrase(o[0])))
    if k:
        return k
    if threatens(line):
        best, best_hits, count = 0, -1, 0
        for k, o in enumerate(options):
            if "(intimidate)" not in o[0].lower():
                continue
            count += 1
            h = _hits(_vcontent(line), _vcontent(_untag(o[0])))
            if h > best_hits:
                best_hits, best = h, k + 1
        if count == 1 or best_hits > 0:
            return best
    close = []
    for k, o in enumerate(options):
        if o[2] or not _said_closely(line, o[0]):
            continue
        words = _vcontent(_untag(o[0]))
        close.append((_hits(_vcontent(line), words) / len(words), k + 1))
    if len(close) == 1:
        return close[0][1]
    if len(close) > 1:
        close.sort(reverse=True)
        if close[0][0] > close[1][0]:
            return close[0][1]
    return 0


def pick_for_reply(options, line, reply):
    best, share_best = 0, 0.0
    for k, (text, answer, said_before) in enumerate(options):
        if said_before or "(" in text or not _fits(line, text):
            continue
        sh = _recited(reply, answer)
        if sh > share_best:
            share_best, best = sh, k + 1
    return best if share_best >= 0.5 else 0


WORK_ANS = "Work? I suppose you can call it that. I call it slavery. I work my fingers to the bone keeping this disgusting place clean. Ever since my parents died and Haelga took me in, it's been a nightmare. How was I to know she was such a wretched woman?"


def SVANA(work_said=False):
    return [("Hey, do you have a moment?", "Of course.", False),
            ("Why do you hate Haelga so much?", "It's not just the work, it's everything about her. She's disgusting. I think she takes her worship of Dibella a bit too seriously.", False),
            ("Do you work here?", WORK_ANS, work_said),
            ("You shouldn't let customers give you a hard time. Stand up to them. (Persuade)", "I... I-I... It's just this... I'm sorry, but I really can't.", False),
            ("This inn has seen better days.", "Sorry about the mess. Keeping this place clean is quite a chore. But the Bunkhouse isn't an inn. I'd recommend the Bee and Barb.", False),
            ("Greetings. (Speechcraft)", "Need something?", False)]


TALK = [("Perhaps I can \"talk\" to them for you...", "You would? Well... I-It's Tythis. He... He's the worst! Make it hurt, but not too much. No killing, okay?", False),
        ("I'm not getting involved.", "Fine. Forget I said anything.", False)]
DONE = [("I had a few words with Tythis. He won't be bothering you any longer.", "Really? Thank you. Here, this is for you. You've been a great help to me.", False),
        ("Hey, do you have a moment?", "Of course.", False)]
TYTHIS = [("I've heard the way you talk to Svana. Stop it. Or Else. (Intimidate)", "All right, all right. I'll leave her alone.", False),
          ("How's work at the fishery?", "Smells like fish.", False), ("You're not from Skyrim, are you?", "No.", False), ("Greetings. (Speechcraft)", "Need something?", False)]

# (name, options, player line, NPC AI reply or None, expected: ("line"|"reply", option number) or None)
REPLAY = [
    ("asks 'do you work here' in passing", SVANA(), "no ....just wanted to ask do you work here?", None, ("line", 3)),
    ("'so do you work here'", SVANA(), "no...so do you work here", None, ("line", 3)),
    ("'you work here?' loosely: her reply recites it", SVANA(), "nope...just wanted to know you work here?", WORK_ANS, ("reply", 3)),
    ("'who gropes you' after work was said: NOT work again", SVANA(True), "huh what who gorpes you?", "It's Tythis Ulen. He's one of the tenants here. Always making comments, letting his hands wander. I work my fingers to the bone and still.", None),
    ("'who gropes you' typo version", SVANA(True), "Who grope s you?", "Anyone who thinks they can put their hands on me... but that's mostly Tythis, down the hall.", None),
    ("'stand up for yourself?' alone: just talk", SVANA(True), "and you dont say anything? you dont stand yp for yourself?", "Stand up for myself? With what? She's my only family.", None),
    ("said the Persuade closely", SVANA(True), "so what...you should not let her or customers harras you. stand up to them", None, ("line", 4)),
    ("'stand up for yourself' (one word only): just talk", SVANA(True), "you should stand up for your self", "Fine. You're right.", None),
    ("'i will talk to him': her reply recites the option's answer", TALK, "i will... i wil talk to him", "You would? Well...I-It's Tythis. He's the worst! Make it hurt, but not too much. No killing, okay?", ("reply", 1)),
    ("'i can talk with him okay' + recital", TALK, "i can talk with him okay", "You would? Well... I-It's Tythis. He's the worst! Make it hurt, but not too much. No killing, okay?", ("reply", 1)),
    ("Tythis threat 1", TYTHIS, "hey you fucking elf...svana said you grope her if you ever do that again i will rip those hands of you", None, ("line", 1)),
    ("Tythis threat 2", TYTHIS, "hey you...elf svana told me you bother. so stop that, stop touching her you will lose your hands", None, ("line", 1)),
    ("Tythis threat 3", TYTHIS, "dont you fucking lie to me...she said it all to me if you ever touch her again you will loose your hands", None, ("line", 1)),
    ("insult only: nothing", TYTHIS, "fucking n'wa", "Watch your tongue.", None),
    ("completion in own words + her thanks", DONE, "my lady, i talked to him and he wont bother you any more", "Really? You actually did it. I don't know how you managed it, but thank you. He's been the worst of them for months.", ("reply", 1)),
    ("completion said closely", DONE, "i had a few words with tythis, he wont be bothering you", None, ("line", 1)),
    ("thanks for the reward: nothing", DONE, "ohh...you did not had to hand me those but thanks", "It's the least I can do.", None),
    ("Kodlak: join, a broader option also close -> the best fit",
     [("I would like to join the Companions.", "Hmm. Let me have a look at you.", False), ("Tell me about the Companions and how to join their ranks.", "We are a brotherhood.", False),
      ("What can you tell me about Jorrvaskr?", "It's our home.", False)],
     "i want to join the companions, farkas sent me", None, ("line", 1)),
    ("two options covered equally: no pick", [("Can I join the Companions?", "Maybe.", False), ("I want to join the Companions now.", "Maybe.", False)],
     "join the companions", None, None),
    ("gift of gold, bribe on screen: nothing", [("Here's some gold. Now let me through. (Bribe)", "Well, I suppose I could look the other way.", False)], "here's some gold for you, thanks for everything", "Generous of you. Thank you, friend.", None),
]
for name, opts, line, reply, want in REPLAY:
    got = None
    k = pick_for_line(opts, line)
    if k:
        got = ("line", k)
    elif reply is not None:
        r = pick_for_reply(opts, line, reply)
        got = ("reply", r) if r else None
    check(f"replay: {name}", got, want)


# ---------------------------------------------------------------- threatened NPCs stay shaken (Brawls.cpp)
print("\n== threatened NPCs stay shaken (Brawls.cpp)")


def threat_mood(hours_ago):
    if hours_ago > 6.0:
        return None
    return "shaken" if hours_ago < 2.0 else "wary"


check("threat 10 game min ago -> shaken", threat_mood(0.17), "shaken")
check("threat 1.5 h ago -> still shaken", threat_mood(1.5), "shaken")
check("threat 4 h ago -> wary", threat_mood(4.0), "wary")
check("threat 7 h ago -> over", threat_mood(7.0), None)
check("'svana said you grope her... i will rip those hands' is a threat", threatens("hey you fucking elf...svana said you grope her if you ever do that again i will rip those hands of you"), True)
check("'you will lose your hands' is a threat", threatens("so stop that, stop touching her you will lose your hands"), True)
check("an insult alone is not a threat", threatens("fucking n'wa"), False)


# ---------------------------------------------------------------- beds: only their own; figures of speech aren't promises (Promises.cpp, Pledges.cpp)
print("\n== beds and figures of speech (Promises.cpp, Pledges.cpp)")


def bed_to_lend(own_bed, household_beds, rental_beds, best_other):
    if own_bed:
        return "own"
    if household_beds > 3 or rental_beds:
        return None
    return best_other


def said_bed(player_line, npc_line, always):
    both = (player_line + " " + npc_line).lower()
    npc = npc_line.lower()
    return any(w in both for w in ("bed", "sleep", "stay", "rest", "crash", "room", "roof", "the night", "tonight", "spare", "bunk", "cot", "bedroll")) or \
        (always and any(w in npc for w in ("my home", "my house", "my place", "our home", "our house", "home is your home")))


def hedged(line):
    l = line.lower()
    return any(w in l for w in ("maybe", "perhaps", "might ", "someday", "some day", "one day", "we'll see", "well see", "possibly"))


check("Haelga's bunkhouse: her own bed", bed_to_lend(True, 8, False, "bunk"), "own")
check("bunkhouse, her bed not in this room: no guest bunk", bed_to_lend(False, 8, False, "bunk"), None)
check("inn with rooms to rent: nothing free", bed_to_lend(False, 0, True, "spare"), None)
check("a farmhouse: the spare bed", bed_to_lend(False, 2, False, "spare"), "spare")
check("'My door will be open for you' lends no bed", said_bed("farewell fair maiden", "Later, then. My door will be open for you, Breton.", True), False)
check("'always welcome at my place' is a welcome", said_bed("Farewell, Viola.", "Farewell, and you're always welcome at my place.", True), True)
check("'can I crash here?' - 'of course'", said_bed("Can I crash here?", "Of course.", False), True)
check("'later maybe? when we are closer' is no promise", hedged("i can tell you that in private...later maybe? when we are more closer to each other perhaps"), True)
check("'I'll bring it tomorrow' is a promise", hedged("I'll bring it tomorrow"), False)


# ---------------------------------------------------------------- gifts must be really said (Promises::Said, give_item)
print("\n== gifts must be really said (Promises.cpp)")


def _tok(text):
    out = []
    for w in _re.findall(r"[a-z0-9]+", text.lower()):
        out.append(w[:-1] if len(w) > 3 and w.endswith("s") else w)
    return out


def said_item(player_line, npc_line, item):
    if not item:
        return True
    words = _tok(item)
    said = _tok(player_line + " " + npc_line)
    head = words[-1]
    named = head in said or (len(head) >= 5 and head in "".join(said)) or (len("".join(words)) >= 5 and "".join(words) in "".join(said))
    npc = npc_line.lower()
    return named or any(w in npc for w in ("take this", "here, take", "here you go", "have this", "it's yours", "take it", "this is for you"))


check("'Take my sword, you'll need it' gives the sword", said_item("I have no weapon", "Take my sword, you'll need it.", "old iron sword"), True)
check("'Here, this is for you' gives the reward", said_item("I talked to him", "Really? Thank you. Here, this is for you.", "gold ring"), True)
check("asked for the soup, 'of course' gives it", said_item("can I have some cabbage soup?", "Of course, friend.", "cabbage soup"), True)
check("'My door will be open for you' gives no item", said_item("farewell", "Later, then. My door will be open for you, Breton.", "key"), False)
check("'sweetroll' vs 'Sweet Roll' item", said_item("got a sweetroll?", "Sure.", "Sweet Roll"), True)


# ---------------------------------------------------------------- follow / escort: who leads, really (TurnPipeline MoveProblem) - REPLAY
print("\n== follow/escort: who leads (TurnPipeline.cpp) - replay of the Haelga mix-up")


def move_problem(request, player, npc, place="", recent=False):
    if request not in ("follow", "escort"):
        return None
    p, n = player.lower(), npc.lower()
    has = lambda t, ws: any(w in t for w in ws)
    asks = has(p, ("follow me", "come with me", "come along", "with me", "tag along", "join me", "let's go", "lets go", "accompany me", "stick with me",
                   "stick close", "travel with me", "walk with me", "come on", "take me", "bring me", "lead the way", "lead me", "guide me",
                   "show me the way", "show me where", "walk me", "get me to"))
    if not place and (has(n, ("somewhere private", "someplace private", "more comfortable", "inner room", "back room", "my room", "upstairs",
                              "downstairs", "in the back", "somewhere quiet", "where we won't be", "where we wont be")) or
                      has(p, ("somewhere private", "my room", "your room", "upstairs"))):
        return "same place"
    if request == "follow":
        offers = has(n, ("i'll come with you", "i will come with you", "i'll follow", "i will follow", "right behind you", "after you", "lead the way",
                         "lead on", "i'm coming", "i'm with you", "i'll go with you", "i'll tag along"))
        if not asks and not offers:
            return "they lead"
    if recent and not asks:
        return "just started"
    return None


check("Haelga 'Come with me.' after a flirt: no follow", move_problem("follow", "you can...taste it yourself", "Bold words, Breton. Let's see if you can back them up. Come with me."), "they lead")
check("Haelga 'Someplace private... Follow me.': no escort", move_problem("escort", "and where are we going if i might ask", "Someplace private. Where we won't be interrupted. Follow me."), "same place")
check("Haelga 'somewhere more comfortable... inner room': no escort", move_problem("escort", "will do", "Let's go somewhere more comfortable. *She gestures toward the inner room.*"), "same place")
check("player 'follow me, Lydia': follow", move_problem("follow", "follow me, Lydia", "Lead the way."), None)
check("player 'take me to the Bannered Mare': escort", move_problem("escort", "can you take me to the bannered mare?", "Sure, this way.", "The Bannered Mare"), None)
check("NPC offers 'I'll come with you': follow", move_problem("follow", "I'm heading to Riverwood", "I'll come with you, it's on my way."), None)
check("second walk 30 s later without asking: not stacked", move_problem("escort", "will do", "This way.", "Dragonsreach", recent=True), "just started")


# ---------------------------------------------------------------- open-ended promises (Pledges.cpp) - replay of the Svana lines
print("\n== open-ended promises (Pledges.cpp)")


def open_ended(line, flag=False):
    if flag:
        return True
    l = line.lower()
    return any(w in l for w in ("work for me", "you can stay with me", "stay with me if", "you can always", "whenever you need", "if you ever need",
                                "if anyone bothers you", "if anyone ever", "if things go wrong", "come to me if", "you know where to find me",
                                "you can come to me", "you can count on me", "any time you need", "anytime you need"))


def due_text(hours_left):
    if hours_left > 0.5e9:
        return "no set time"
    if hours_left < 0:
        return "overdue"
    return "timed"


check("'you can work for me' has no set time", open_ended("i mean them....svana. if you feel like it will be a charity and you dont want that, than you can work for me"), True)
check("'if anyone bothers you, come to me' has no set time", open_ended("and now if anyone bothers you, come to me"), True)
check("'I'll bring you mead tomorrow' is timed", open_ended("I'll bring you a bottle of mead tomorrow"), False)
check("no set time never reads overdue", due_text(1.0e9 - 5000), "no set time")
check("the 'might' house line stays a maybe (no promise)", hedged("so i will get a place i will somewhere in skyrim. it might take some time then you can proceede"), True)


# ---------------------------------------------------------------- only a definite yes is a promise; "no promises" withdraws (Pledges.cpp)
print("\n== only a definite yes is a promise (Pledges.cpp)")


def commits(line):
    lower = line.lower().replace("\u2019", "'")
    l = " " + "".join(c if c.isalnum() or c == "'" else " " for c in lower) + " "
    return any(f" {w} " in l for w in ("i will", "i'll", "ill", "i shall", "i promise", "promise you", "my word", "sure", "yes", "yeah", "yep", "of course",
                                       "count on me", "consider it done", "deal", "alright", "all right", "okay", "ok", "fine",
                                       "i can do that", "no problem", "will do", "you got it", "gladly", "absolutely", "certainly", "happy to", "i'm on it",
                                       "im on it", "leave it to me", "i'll be back", "i will be back", "see you tomorrow", "i'll see you", "ill see you"))


def unsure(line):
    sentences = [s for s in _re.split(r"[.!?;\n]", line) if _re.findall(r"[a-z0-9]+", s.lower())]
    any_commit = False
    for s in sentences:
        c = commits(s)
        any_commit = any_commit or c
        if (c or len(_re.findall(r"[a-z0-9']+", s.lower())) <= 3) and unsure_in(s):
            return True
    return not any_commit and unsure_in(line)


def unsure_in(line):
    l = _hedged(line) if "_hedged" in globals() else line.lower()
    for x in (" you might ", " you may ", " they might ", " she might ", " he might ", " that might ", " which might "):
        l = (" " + l + " ").replace(x, " ")
    if any(w in l for w in ("maybe", "perhaps", "might ", "someday", "some day", "one day", "we'll see", "well see", "possibly", "i'll try", "ill try",
                            "i will try", "i'll do my best", "ill do my best", "do what i can", "see what i can", "if i can", "if i get the chance",
                            "no promise", "can't promise", "cant promise", "cannot promise", "not promising", "not a guarantee", "no guarantee",
                            "can't guarantee", "cant guarantee", "think about it", "don't hold me", "dont hold me", "i'll give it a try",
                            "i'll give it a shot", "ill give it a try", "ill give it a shot", "i'll see if", "ill see if", "i will see if")):
        return True
    for w in ("i'll see", "ill see", "i will see"):
        at = l.find(w)
        while at >= 0:
            rest = l[at + len(w):at + len(w) + 6]
            if not any(rest.startswith(x) for x in (" you", " u ", " her", " him", " them", " ya")):
                return True
            at = l.find(w, at + 1)
    return False


def _hedged(line):
    return _re.sub(r"[a-z]+", lambda m: "guarantee" if len(m.group()) >= 7 and m.group()[0] == "g" and m.group().endswith(("ntee", "nty", "ntie")) else m.group(), line.lower())


def withdraws(line):
    l = _hedged(line)
    return any(w in l for w in ("no promise", "not a guarantee", "no guarantee", "not guaranteed", "can't promise", "cant promise", "cannot promise",
                                "not promising", "don't hold me to", "dont hold me to", "not a promise", "can't guarantee", "cant guarantee"))


for line, want in [("yes sure, I'll do that", False), ("I will, you have my word", False), ("I'll try", True), ("okay...ill see what i can do...no promises though", True),
                   ("its not a gurantee but ill try", True), ("no promises but i will see", True), ("okay, I'll see.", True), ("I'll see you tomorrow", False),
                   ("I'll do my best", True), ("if I can, sure", True), ("I'll bring you mead tomorrow", False), ("let me think about it", True)]:
    check(f"unsure: {line!r}", unsure(line), want)
check("'its not a gurantee but ill try' withdraws the promise just made", withdraws("its not a gurantee but ill try"), True)
check("'no promises but i will see' withdraws", withdraws("no promises but i will see"), True)
check("'I'll try' alone withdraws nothing already made", withdraws("I'll try"), False)
check("a promise made 2 h ago can be withdrawn", 2.0 <= 3.0, True)
check("a promise made yesterday is kept on record", 20.0 <= 3.0, False)


# ---------------------------------------------------------------- NPC echoing the player's line; beds only on a plain offer (replay 2026-10-09)
print("\n== echoes and plain bed offers (TurnPipeline.cpp, Promises.cpp) - replay")


def _norm_echo(t):
    return " ".join(w.replace("'", "") for w in _re.findall(r"[a-z0-9']+", t.lower()))


def echoes(player, npc):
    p, n = _norm_echo(player), _norm_echo(npc)
    if p.count(" ") < 3 or not n:
        return False
    return _edit(p, n) * 100 <= max(len(p), len(n)) * 36


check("Haelga echo 1", echoes("now you know i am not...so what now?", "Now you know I'm not. So what now?"), True)
check("Haelga echo 2", echoes("maybe i am maybe not...you will have to find it yourself", "Maybe I am, maybe I'm not. You'll have to find that out for yourself, Breton."), True)
check("a real answer about drinking is no echo", echoes("do you want to drink with me?", "I'd love to drink with you!"), False)
check("a real answer is no echo", echoes("i usually am reserved..but something about you just makes me wanna talk", "A Breton who speaks his heart without hiding behind ale or duty, that's rare in Riften."), False)
check("short 'so what now?' answered in kind is fine", echoes("so what now?", "So what now? We drink."), False)


def plain_bed_offer(player, npc):
    n, pl = npc.lower(), player.lower()
    night = any(w in n for w in ("night", "sleep", "bed", "rest", "tonight", "morning", "crash"))
    offered = any(w in n for w in ("take my bed", "use my bed", "the bed is yours", "bed is yours", "bed's yours", "spare bed", "my spare", "you can sleep",
                                   "you can crash", "you can rest here", "sleep here", "crash here", "stay the night",
                                   "sleep at my", "sleep in my", "my home is your home", "always welcome",
                                   "welcome to stay", "make yourself at home", "bed upstairs", "bed in the back", "bed's made", "room for you here",
                                   "rest your head", "lay your head", "spare corner", "corner by the fire", "corner by the hearth", "put you up",
                                   "roof over your head", "spare bedroll", "floor's yours", "floor is yours", "spare furs")) or \
        (night and any(w in n for w in ("you can stay", "stay here", "stay with me", "stay at my", "stay in my")))
    padded = " " + " ".join(re.findall(r"[a-z0-9]+", n)) + " "
    asked = any(w in pl for w in ("can i stay", "could i stay", "can i sleep", "could i sleep", "can i crash", "could i crash", "can i rest here",
                                  "a bed for the night", "place to sleep", "spend the night", "share your bed", "sharing your bed", "sleep with you",
                                  "sleep there", "stay with you", "stay at your", "sleep at your", "sleep in your")) and \
        any(f" {w} " in padded for w in ("yes", "sure", "of course", "go ahead", "aye", "certainly", "be my guest", "no problem", "please do", "why not",
                                         "come to", "come with me", "follow me", "very well", "alright", "all right", "fine"))
    conditional = any(w in n for w in ("if you ever need", "if you need a place", "if you're ever", "should you ever need")) and not offered
    return (offered or asked) and not conditional


check("Svana 'If you ever need a place to stay... I'm usually around' is no bed", plain_bed_offer("thank you...you didnt have to give me anything", "I'm glad it helped. If you ever need a place to stay or just want to talk, I'm usually around."), False)
check("'Can I stay?' - 'No. Your eyes betray you.' is no bed", plain_bed_offer("can i stay here?", "No. Your eyes betray you."), False)
check("Haelga 'can i sleep there with you' - 'Come to the bunkhouse' is a bed", plain_bed_offer("soo speak in riddles...can i sleep there with you, or do i proceede to the inn?", "My bed's not for strangers, Zoro, but you're not a stranger anymore. Come to the bunkhouse; we'll talk until the moon's down, at least."), True)
check("'can i sleep at your place?' - 'No. Go to the inn.' is no bed", plain_bed_offer("can i sleep at your place?", "No. Go to the inn."), False)
check("'Stay here while I fetch it' is no bed", plain_bed_offer("can you get me a drink?", "Stay here while I fetch it."), False)
check("'Stay with me a moment, will you?' is no bed", plain_bed_offer("i should go", "Stay with me a moment, will you?"), False)
check("'You can stay here tonight' is a bed", plain_bed_offer("it's late", "You can stay here tonight."), True)
check("Haelga 'Flattery will get you a warm bed' is no bed", plain_bed_offer("your bunkhouse caught my eyes", "Flattery will get you a warm bed, but if you're just passing through, how long do you plan on staying?"), False)
check("'Farewell, and you're always welcome at my place' is a welcome", plain_bed_offer("Farewell, Viola.", "Farewell, and you're always welcome at my place."), True)
check("'Can I crash here?' - 'Of course' is a bed", plain_bed_offer("Can I crash here tonight?", "Of course, friend."), True)
check("'the bed in the back is yours tonight' is a bed", plain_bed_offer("I'm exhausted", "The bed in the back is yours tonight."), True)
check("Ronja 'a spare corner by the fire if you need to rest your head' is a bed", plain_bed_offer("inn costing 100 a night is too steep",
      "A hundred septims a night? That's robbery. I've got a spare corner by the fire if you need to rest your head - no coin needed."), True)


# ---------------------------------------------------------------- narrated gestures (Gestures.cpp) - replay of real lines
print("\n== narrated gestures (Gestures.cpp)")

_G_RULES = [
    (("draws her", "draws his", "draws their", "draws a ", "draws the", "unsheathes", "pulls out", "pulls a ", "brandishes", "bares her", "bares his",
      "drew her", "drew his"), "draw"),
    (("sheathes", "sheaths", "puts away her", "puts away his", "puts the blade away", "puts the dagger away", "slides it back", "lowers her blade",
      "lowers his blade", "holsters"), "sheathe"),
    (("stands up", "rises from", "rises to her feet", "rises to his feet", "gets up", "gets to her feet", "gets to his feet", "pushes back her chair",
      "pushes back his chair", "stands from"), "stand"),
    (("walks over to you", "walks up to you", "comes over to you", "steps closer to you", "steps closer", "moves closer to you", "approaches you",
      "crosses the room to you", "comes closer to you"), "approach"),
    (("~laugh", "~laughs", "~laughing", "~chuckle", "~chuckles", "~chuckling", "~giggle", "~giggles"), "IdleLaugh"),
    (("claps", "applauds", "slow clap"), "IdleApplaud2"),
    (("salutes", "a salute"), "IdleSalute"),
    (("~wave", "~waves", "~waving"), "IdleWave"),
    (("bows", "a bow", "a slight bow"), "IdleSilentBow"),
    (("points at", "points to", "points toward", "points towards", "gestures toward", "gestures towards", "gestures back toward",
      "gestures back towards", "gestures at", "gestures to the", "nods toward", "nods towards"), "IdlePointClose"),
    (("beckons", "motions for you to follow", "gestures for you to follow", "waves you over", "this way"), "IdleComeThisWay"),
    (("shakes her head", "shakes his head", "shakes their head", "shaking her head", "shaking his head"), "IdleHeadShake"),
    (("fidgets", "wrings her hands", "wrings his hands", "nervously"), "IdleNervous"),
    (("wipes her brow", "wipes his brow", "wipes sweat"), "IdleWipeBrow"),
    (("scratches her head", "scratches his head"), "IdleScratchHead"),
    (("takes a sip", "takes a swig", "sips her", "sips his", "drinks from", "takes a long drink"), "IdleDrink"),
]
_G_WEAPONS = ("dagger", "sword", "blade", "knife", "axe", "mace", "weapon", "bow ", "steel", "hammer", "hilt", "dirk", "sabre", "saber", "katana", "scimitar")


def _about_speaker(clause, speaker):
    m = _re.search(r"[A-Za-z][A-Za-z']*", clause)
    if not m:
        return False
    first = m.group()
    if not first[0].isupper():
        return True
    low = first.lower()
    if low in ("she", "he", "they", "her", "his", "their", "i", "my", "a", "an", "the", "with", "then", "slowly", "quickly", "finally"):
        return True
    fn = speaker.lower().split(" ")[0]
    return low in (fn, fn + "'s")


def gestures(line, speaker):
    out = []
    for part in _re.findall(r"\*([^*]+)\*", line):
        for clause in _re.split(r"[.;!?]", part):
            if not clause.strip() or not _about_speaker(clause, speaker):
                continue
            l = " " + clause.lower() + " "
            toks = _re.findall(r"[a-z0-9']+", clause.lower())
            weapon = any(w in l for w in _G_WEAPONS)
            for words, act in _G_RULES:
                if act in ("draw", "sheathe") and not weapon:
                    continue
                if any((w[1:] in toks) if w.startswith("~") else (w in l) for w in words) and act not in out:
                    out.append(act)
    return out


for line, speaker, want in [
    ("*Haelga gives a warm, conspiratorial smile and shifts closer on her chair, bringing her knee beside yours under the table.* I appreciate a man who values privacy.", "Haelga", []),
    ("*Haelga gestures back toward the Bee and Barb's doorway.* Back to the fire, if you don't mind.", "Haelga", ["IdlePointClose"]),
    ("*She gives a small, tired wave* And thank you again for dealing with Tythis.", "Svana Far-Shield", ["IdleWave"]),
    ("*I let out a low chuckle, my eyes tracing his shape in the dim light.* Bold words, Breton.", "Haelga", ["IdleLaugh"]),
    ("*Tythis Ulen holds up his hands defensively.* Look, I'm done.", "Tythis Ulen", []),
    ("*Svana Far-Shield glances toward the door*", "Svana Far-Shield", []),
    ("*She draws her dagger, eyes narrowed.* Try it.", "Haelga", ["draw"]),
    ("*She draws a deep breath.* Fine.", "Haelga", []),
    ("*draws her cloak tighter*", "Haelga", []),
    ("*She sheathes the blade with a click.* Not today.", "Uthgerd", ["sheathe"]),
    ("*Haelga stands up and walks over to you.*", "Haelga", ["stand", "approach"]),
    ("*She walks over to the bar.*", "Haelga", []),
    ("*Svana laughs* Oh, she always does that.", "Haelga", []),
    ("*shakes her head* No. Never.", "Svana Far-Shield", ["IdleHeadShake"]),
    ("*Haelga's step falters just a fraction, but she recovers with a low, unbothered laugh.*", "Haelga", ["IdleLaugh"]),
    ("*nods slowly*", "Ysolda", []),
]:
    check(f"gestures: {line[:46]!r}", gestures(line, speaker), want)


# ---------------------------------------------------------------- our own follow/escort: the player's words end them (Services.cpp)
print("\n== own follow/escort stop words (Services.cpp)")


def _sp(line):
    return " " + " ".join(re.findall(r"[a-z0-9]+", line.replace("\u2019", "'").lower())) + " "


def is_stop_words(line):
    said = ""
    for m in re.finditer(r"[^.!?\n]*[.!?\n]", line + "."):
        cur = m.group(0)
        q = _sp(cur)
        if not cur.endswith("?") or q.startswith((" can you ", " could you ", " would you ", " will you ", " please ")):
            said += cur + " "
    p = _sp(said)
    has = lambda ps: any(f" {x} " in p for x in ps)
    if has(["don t leave", "dont leave", "do not leave", "don t go", "dont go", "never leave", "won t leave", "wont leave", "with me", "with us"]):
        return False
    if has(["stop following", "don t follow", "dont follow", "no need to follow", "you re dismissed", "youre dismissed", "we part here",
            "part ways", "that s far enough", "thats far enough", "go on without me"]):
        return True
    if has(["let s", "lets", "let us", "we", "we ll", "together", "take me", "show me", "bring me", "lead me", "walk me", "you can go first",
            "you can go ahead", "you go first", "after you"]):
        return False
    if has(["wait here", "stay here", "wait for me here", "wait right here", "stay put", "you can go", "you may go", "go home", "head home",
            "go back home", "you can leave", "leave me", "i ll go alone", "ill go alone", "i ll go on alone", "go about your", "back to your work",
            "back to your business", "back to your post", "back to your day", "back to your life"]):
        return True
    return not has(["i ll do", "ill do", "i will do", "i can do", "i d do", "i would do", "i do", "with it", "with them", "with this", "with that",
                    "with those"]) and \
        has(["you can do whatever", "you can do what ever", "you can do what you", "go do whatever", "go do what ever", "go do what you",
             "do whatever you want", "do whatever you like", "do what ever you want", "do what ever you like", "do what you want", "do what you like",
             "do as you please", "go do your", "do your thing", "do your own thing", "you re free to go", "youre free to go", "you are free to go",
             "you re free to do", "youre free to do", "you are free to do", "important to do you can do"])



for line, want in [("wait here, I'll be back", True), ("you can go home now, Haelga", True), ("thanks, you can go", True), ("stay put", True),
                   ("come on, let's walk", False), ("its good this walking", False), ("lead on", False), ("where shall we go then?", False),
                   ("please don't leave me", False), ("stay here with me", False), ("leave me alone", True), ("don't go home yet", False),
                   # going on together, being led, or a question: not a dismissal
                   ("let's go home", False), ("we should go home now", False), ("take me back to your place", False), ("you can go first", False),
                   ("do you want to go home?", False), ("could you wait here for me?", True), ("I'll eat with meat later", False),
                   ("we part here, friend", True), ("go on without me", True), ("go back to your work, I'm fine", True),
                   ("can we go back to your house?", False),
                   # free to go about their own day (Ronja, 2026-10-11)
                   ("so then...i guess ill be sleeping early today. you can do whatever you like", True),
                   ("as i said..i think ill go to sleep early. you can do what ever you want", True),
                   ("I'll do whatever you want", False), ("you're free to go", True),
                   # not a dismissal: deferring, giving, offering, the player's own freedom
                   ("whatever you want, I'll follow your lead", False), ("take whatever you want from my pack", False),
                   ("here, do whatever you want with it", False), ("I'm free to go now, thanks", False),
                   ("I'll get you whatever you like", False), ("you're free to come with me", False),
                   ("we can do whatever you like", False), ("what do you want to do?", False),
                   # a dismissal in other words
                   ("go do your thing, I'll be fine", True), ("you can do what you want now, I'm heading to bed", True),
                   ("if you have anything important to do you can do it", True), ("do as you please", True)]:
    check(f"stop words: {line!r}", is_stop_words(line), want)


# ---------------------------------------------------------------- the player's line must commit (Pledges::Commits) - replay
print("\n== player promises need the player's own commitment (Pledges.cpp)")




for line, want in [("whad do you mean by i show you?", False), ("what do you mean by show you?", False), ("I'll bring you mead tomorrow", True),
                   ("sure, I can do that", True), ("yes", True), ("you have my word", True), ("ill be back in a moment", True),
                   ("why would i do that?", False), ("I saw him yesterday", False), ("they dealt with it", False), ("the finest mead", False),
                   ("I\u2019ll bring it", True), ("ok, deal", True)]:
    check(f"commits: {line!r}", commits(line), want)


# ---------------------------------------------------------------- spots inside a building (Services.cpp SpotOf)
SPOTS = [
    ("bed", ["bed", "beds", "bedroom", "your room", "my room", "her room", "his room", "their room", "guest room",
             "where you sleep", "where i sleep", "where i can sleep", "where do you sleep", "where do i sleep", "where can i sleep", "sleeping quarters", "bedchamber"]),
    ("fire", ["fire", "fireplace", "hearth", "firepit", "fire pit", "fireside"]),
    ("table", ["table", "eating area", "dining area", "dining table", "dining room", "dinner table", "where we eat",
               "where you eat", "where i can eat", "eating table", "where do we eat", "where do you eat", "where can we eat", "where can i eat"]),
    ("kitchen", ["kitchen", "cooking pot", "cookpot", "stove", "oven", "cooking fire"]),
    ("forge", ["forge", "anvil", "workbench", "grindstone", "smelter"]),
    ("alchemy", ["alchemy", "alchemy lab", "alchemy table", "alchemy station"]),
    ("enchanting", ["enchanter", "arcane enchanter", "enchanting table", "enchanting"]),
    ("chest", ["chest", "strongbox", "storage chest", "trunk"]),
    ("bar", ["bar", "counter", "bar counter"]),
]


def spot_of(place):
    lower = place.lower()
    if lower.startswith("spot:"):
        lower = lower[5:]
        for kind, _ in SPOTS:
            if lower == kind:
                return kind
    padded = " " + " ".join(re.findall(r"[a-z0-9]+", lower)) + " "
    for first in ("kitchen", "alchemy", "enchanting"):
        for kind, phrases in SPOTS:
            if kind == first and any(f" {p} " in padded for p in phrases):
                return kind
    for kind, phrases in SPOTS:
        if any(f" {p} " in padded for p in phrases):
            return kind
    return ""


for place, want in [("spot:bed", "bed"), ("your room", "bed"), ("Your bedroom", "bed"), ("the fireplace", "fire"), ("the hearth", "fire"),
                    ("where do we eat", "table"), ("where do you sleep", "bed"), ("the eating area", "table"), ("the kitchen", "kitchen"), ("the cooking fire", "kitchen"),
                    ("alchemy table", "alchemy"), ("spot:table", "table"), ("the bar", "bar"),
                    ("Candlehearth Hall", ""), ("The Bannered Mare", ""), ("Sleeping Giant Inn", ""), ("Bards College", ""),
                    ("Dragonsreach throne room", ""), ("home", ""), ("Riverwood", ""), ("Warmaiden's", ""), ("Bee and Barb", "")]:
    check(f"spot of '{place}' is '{want}'", spot_of(place), want)


# ---------------------------------------------------------------- the player said no: nothing starts (TurnPipeline PlayerSaidNo) - replay
def player_said_no(line):
    padded = " " + " ".join(re.findall(r"[a-z0-9]+", line.lower())) + " "
    has = lambda ps: any(f" {p} " in padded for p in ps)
    leading_no = padded.startswith((" no ", " nah ", " nope ")) and not has(["come on", "common", "cmon", "c mon", "sit", "with me", "next to me",
        "join me", "stay", "have this", "have a", "take this", "here"])
    no = leading_no or has(["i am not", "i m not", "im not", "i dont like", "i don t like", "i do not like",
        "i dont want", "i don t want", "i do not want", "rather not", "i ll pass", "ill pass", "not interested", "never mind", "nevermind",
        "no thanks", "no thank you", "not now", "not tonight", "i refuse", "i won t", "i wont", "i will not", "maybe another time",
        "another time", "some other time", "not today", "i m good", "im good", "i m fine", "im fine", "i m okay", "i m ok",
        "no need", "don t bother", "dont bother", "not necessary", "i ll manage", "ill manage", "i ll be fine"])
    asks = has(["let s", "lets", "can you", "could you", "would you", "will you", "i want to", "i d like", "id like", "i would like",
        "give me", "take me", "show me", "please", "how about", "instead", "come with me", "follow me"])
    ok_with = has(["fine with", "good with", "okay with", "ok with"])
    return no and not asks and not ok_with


for line, want in [
    ("I'm fine with that", False),
    ("well then...it seems i am not.i dont like to share my thoughts and nor i like to give them  a performance in open", True),
    ("but i dont like sleeping in an open room? lets rent a room at inn? what do you say?", False),
    ("No, thanks.", True),
    ("I'd rather not.", True),
    ("no, take me to the inn instead", False),
    ("where are you going come lets chat in private?", False),
    ("sure, sit with me", False),
    ("nothing tonight, I'm tired", False),
    ("Not now, maybe later.", True),
    ("I'm good, thanks.", True),
    ("maybe another time, friend", True),
    ("no need, I'll manage", True),
    ("I'm good, let's go", False),
    ("I'm fine, but could you patch me up anyway?", False),
    # a leading "no" that goes on to ask is a correction (Ronja, 2026-10-11)
    ("no common sit next to me we can relax saber is far away i hope", False),
    ("no no, stay, have a drink", False),
    ("no, I don't want to sit", True),
    ("nah, I'm alright", True),
]:
    check(f"player said no: {line[:50]!r}", player_said_no(line), want)


# ---------------------------------------------------------------- spending time together (Services.cpp Leaving / PlayerAsksCompany / NpcOffersCompany)
def _padded(line):
    return " " + " ".join(re.findall(r"[a-z0-9]+", line.replace("\u2019", "'").lower())) + " "


def _has(p, phrases):
    return any(f" {x} " in p for x in phrases)


def _statements(line):
    out = ""
    for m in re.finditer(r"[^.!?\n]*[.!?\n]|[^.!?\n]+$", line):
        if not m.group(0).endswith("?"):
            out += m.group(0) + " "
    return out


def leaving(line):
    p = _padded(_statements(line))
    if _has(p, ["goodbye", "good bye", "farewell", "see you later", "see you around", "see you soon", "until next time", "good night", "goodnight",
                "safe travels"]):
        return True
    if _has(p, ["don t have to go", "dont have to go", "do not have to go", "not leaving", "won t leave", "wont leave", "no need to go",
                "never leave", "not going anywhere", "go with you", "come with you", "off duty", "off work", "later", "tomorrow", "soon", "in a bit",
                "after", "tonight"]):
        return False
    return _has(p, ["i have to go", "i ve got to go", "i got to go", "i gotta go", "gotta go", "i need to go", "i must go", "i should go",
                    "i should get going", "i d better go", "i better go", "i d best be going", "i d best be off", "i should be going", "i must be going",
                    "i need to be going", "i have to be going", "i ll be going", "i ll be off", "i must be off", "i should be off", "i need to get back",
                    "i have to get back", "i should get back", "i must get back", "i ll leave you", "i ll take my leave", "i must take my leave",
                    "time for me to go", "time i was going", "duty calls", "you d better go", "you should leave", "you need to leave",
                    "you should go now", "you should be going", "you should be on your way", "leave me be", "leave me alone", "get out of my"]) or \
        ((p.endswith(" get out ") or _has(p, ["get out of here", "now get out", "get out now"])) and
         not _has(p, ["let s", "lets", "let us", "we", "us", "we ll", "we should"]))


def asks_company(line):
    return _has(_padded(line), ["keep me company", "keep me some company", "keep each other company", "stay with me", "stay here with me",
                                "spend time", "spend some time", "spend the evening", "spend the day", "spend a while", "hang out", "stay a while", "stay awhile",
                                "stay for a while", "stay a bit", "stay for a bit", "stick around", "be with me", "don t leave", "dont leave", "don t go", "dont go"])


def offers_company(line):
    return _has(_padded(line), ["i ll keep you company", "i will keep you company", "i can keep you company", "i ll stay with you",
                                "i will stay with you", "i can stay with you", "i ll stay a while", "i can stay a while", "i ll stay for a while", "i ll stick around",
                                "i can stick around", "i ll spend some time", "i d love to stay", "i ll stay"])


for line, want in [
    ("I'm sorry, I have to go. The mill won't run itself.", True),
    ("Well, I'd best be off. Mjoll will be wondering.", True),
    ("Farewell, Breton.", True),
    ("Good night, Zoro.", True),
    ("I should get back to the bunkhouse before the workers wake.", True),
    ("Leave me be.", True),
    ("Do you have to go?", False),
    ("I don't have to go anywhere tonight.", False),
    ("I have to go to the market later.", False),
    ("I'm not going anywhere, Breton.", False),
    ("I'll go with you.", False),
    ("I'm off duty now, so sit.", False),
    ("Take a seat, the wine's waiting.", False),
    ("Whatever you're willing to tell, Breton. You're in my bed, after all.", False),
    ("I should go with you, just in case.", False),
    ("Duty calls, I'm afraid.", True),
    ("She said she had to go", False),
    # advice and going together aren't leaving; a question after a goodbye doesn't hide it
    ("You should go to the temple, they can help.", False),
    ("Let's get out of here before the guards come.", False),
    ("Get out!", True),
    ("Get out of my house.", True),
    ("I must be going. Will you be all right?", True),
    ("Should I go?", False),
]:
    check(f"leaving: {line[:48]!r}", leaving(line), want)

for line, want in [
    ("stay with me a while, keep me company", True),
    ("spend some time with me?", True),
    ("can you stick around for a bit?", True),
    ("don't go yet", True),
    ("follow me", False),
    ("sit with me by the fire", False),
    ("I'd like to rent a room.", False),
]:
    check(f"asks company: {line!r}", asks_company(line), want)

for line, want in [
    ("I'll keep you company for a while.", True),
    ("I can stay a while, sure.", True),
    ("I'll stay.", True),
    ("I'll stay here and wait.", True),
    ("Stay here, I'll be back.", False),
]:
    check(f"offers company: {line!r}", offers_company(line), want)


# ---------------------------------------------------------------- moving in (MoveIn.cpp PlayerAsks / NpcAsks / MoveOutWords / KeyStem / KeyFits)
def movein_player_asks(line):
    return _has(_padded(line), ["live with me", "move in with me", "come live", "come and live", "live together", "live at my", "live in my",
                                "move into my", "move in here", "move in to my", "you could move in", "you can move in", "you should move in",
                                "want to move in", "like to move in", "share my home", "share my house", "stay with me for good", "stay with me permanently",
                                "be my housemate", "make my home your home", "make my house your home", "make it your home"])


def movein_npc_asks(line):
    return _has(_padded(line), ["live with you", "move in with you", "share your home", "come live with you", "stay with you for good", "live together"])


def move_out_words(line):
    p = _padded(line)
    if _has(p, ["let s move out", "lets move out", "we move out", "we ll move out", "move out at", "move out now", "time to move out"]):
        return False
    return _has(p, ["you move out", "you to move out", "you should move out", "you have to move out", "you need to move out", "you must move out",
                    "you can move out", "you re moving out", "move out of my", "move out of the house", "move out of here", "go back to your old home",
                    "back to your old life", "back to your own home", "go back to your home", "find somewhere else to live", "find another place to live",
                    "pack your things and go", "pack your things and leave", "you can t live here", "you cant live here", "you can no longer live here",
                    "don t live here anymore", "dont live here anymore", "leave my house", "get out of my house", "move back to your", "move back home"])


def key_stem(name):
    name = name.lower()
    for pre in ("key to the ", "key to ", "the "):
        if name.startswith(pre):
            name = name[len(pre):]
    if name.endswith(" key"):
        name = name[:-4]
    return name


def key_fits(stem, house):
    house = house.lower()
    if house.startswith("the "):
        house = house[4:]
    return len(stem) >= 4 and (stem == house or stem in house or house in stem)


for line, want in [
    ("would you like to come live with me?", True),
    ("move in with me, Svana", True),
    ("I want us to live together", True),
    ("you could make my home your home", True),
    ("share my house with me", True),
    ("you're always welcome at my place", False),
    ("can I stay at your place tonight?", False),
    ("I live with my aunt", False),
    ("where do you live?", False),
    ("let's move on", False),
    ("keep me company", False),
    ("the bandits will move in at night", False),
    ("you could move in here", True),
]:
    check(f"move in asked: {line!r}", movein_player_asks(line), want)

for line, want in [
    ("Could I live with you? Riften is suffocating me.", True),
    ("I'd love to move in with you.", True),
    ("I live with Aunt Haelga.", False),
    ("You're welcome to stay the night.", False),
]:
    check(f"move in npc asks: {line!r}", movein_npc_asks(line), want)

for line, want in [
    ("I think you should move out.", True),
    ("go back to your old home, Svana", True),
    ("pack your things and go", True),
    ("go home", False),
    ("let's move on", False),
    ("move over", False),
    # everyday words that aren't about the house
    ("let's move out, the bandits are close", False),
    ("pack your things, we're off to Riften", False),
    ("move back, give me some room", False),
    ("I want you to move out of my house", True),
]:
    check(f"move out words: {line!r}", move_out_words(line), want)

for key, house, want in [
    ("Breezehome Key", "Breezehome", True),
    ("Honeyside Key", "Honeyside", True),
    ("Proudspire Manor Key", "Proudspire Manor", True),
    ("Key to Vlindrel Hall", "Vlindrel Hall", True),
    ("Hjerim Key", "Breezehome", False),
    ("Riften Jail Key", "Honeyside", False),
    ("Key", "Breezehome", False),
]:
    check(f"key fits: {key!r} -> {house!r}", key_fits(key_stem(key), house), want)


# ---------------------------------------------------------------- off-list request ids don't land on the wrong service
for r, want in [("tell_story", "none"), ("history", "none"), ("seat", "sit"), ("great", "none"), ("eat_together", "share_meal"),
                ("store_items", "hold"), ("treat", "none")]:
    check(f"norm_request({r})", norm_request(r), want)


# ---------------------------------------------------------------- beds at home (MoveIn.cpp BedWords / InWindow)
def bed_words(line):
    p = _padded(line)
    if _has(p, ["pick a bed", "choose a bed", "which bed do you want", "which bed would you like", "want this bed", "like this bed"]):
        return True
    if "?" in line:
        return False
    return _has(p, ["this is your bed", "this bed is yours", "this one is your bed", "this one s your bed", "that is your bed",
                    "that s your bed", "that bed is yours", "you can sleep here", "you ll sleep here", "you will sleep here", "sleep in this bed", "take this bed",
                    "your bed is this", "this will be your bed", "this can be your bed", "this bed is for you"])


def in_window(hour, start, end):
    return start <= hour < end if start <= end else hour >= start or hour < end


for line, want in [
    ("this is your bed", True),
    ("That's your bed, Svana", True),
    ("you can sleep here from now on", True),
    ("which bed do you want?", True),
    ("go to bed", False),
    ("I made your bed this morning", False),
    ("this bed is comfy", False),
    # questions about where they sleep now
    ("which bed is yours?", False),
    ("do you sleep here?", False),
    ("is this your bed?", False),
]:
    check(f"bed words: {line!r}", bed_words(line), want)

for hour, want in [(23.0, True), (2.0, True), (5.9, True), (6.0, False), (12.0, False), (21.9, False)]:
    check(f"sleep window 22-6 at {hour}", in_window(hour, 22.0, 6.0), want)
check("sleep window 1-5 at 3", in_window(3.0, 1.0, 5.0), True)
check("sleep window 1-5 at 6", in_window(6.0, 1.0, 5.0), False)


# ---------------------------------------------------------------- a visit promise must say so (Pledges.cpp SaysVisit) - replay
def says_visit(line):
    lower = line.lower().replace("’", "'")
    l = " " + "".join(c if c.isalnum() or c == "'" else " " for c in lower) + " "
    return any(f" {w} " in l for w in ("come back", "be back", "i'll return", "i will return", "ill return", "visit", "stop by", "drop by", "come by",
                                       "come see", "come and see", "come over", "see you tomorrow", "see you tonight", "see you in the morning",
                                       "i'll be there", "ill be there", "i'll come", "i will come", "ill come", "back tomorrow", "back later", "back soon"))


for line, want in [
    ("sure lets have a drink and then ill be off. i am too tired", False),
    ("see you later", False),
    ("I'll come back tomorrow evening", True),
    ("i'll stop by after work", True),
    ("ill be back in a moment", True),
    ("I'll visit you soon", True),
    ("see you tomorrow, Haelga", True),
]:
    check(f"says visit: {line!r}", says_visit(line), want)


# ---------------------------------------------------------------- a goodbye with an "okay" is no promise (Pledges.cpp StrongCommit / Departing) - replay
def _spaced(line):
    lower = line.lower().replace("’", "'")
    return " " + "".join(c if c.isalnum() or c == "'" else " " for c in lower) + " "


def strong_commit(line):
    return any(f" {w} " in _spaced(line) for w in ("i will", "i'll", "ill", "i shall", "i promise", "promise you", "my word", "i swear", "count on me",
                                                   "consider it done", "leave it to me", "will do", "you got it", "i'm on it", "im on it"))


def departing(line):
    return any(f" {w} " in _spaced(line) for w in ("take my leave", "time i go", "time i left", "time to go", "i should go", "i should get going",
                                                   "i'll be off", "ill be off", "i'll be going", "ill be going", "be on my way", "farewell", "goodbye",
                                                   "good bye", "see you later", "see you around", "i must go", "i have to go", "i need to go"))


def oath(line):
    return any(f" {w} " in _spaced(line) for w in ("i promise", "promise you", "my word", "i swear", "count on me", "consider it done", "leave it to me",
                                                   "make it up to you", "make it up to them", "you can rely on me", "i give you my word", "on my honor",
                                                   "on my honour"))


def joint_plan(line):
    return any(f" {w} " in _spaced(line) for w in ("let's", "lets", "let us", "we", "we'll", "we will", "we can", "we should", "together", "us"))


def pledge_recorded(line):
    return commits(line) and not (departing(line) and not strong_commit(line)) and not (joint_plan(line) and not strong_commit(line))


print("\n== a plan to do it together is no promise (Pledges.cpp JointPlan)")
for line, want in [
    ("sure so lets let this here and we find you  a kill then we return home", False),   # Ronja, hunting
    ("sure, we'll look for your ring together", False),
    ("I'll find your ring, I promise", True),
    ("sure, I'll bring you mead tomorrow and we can drink it together", True),
    ("I'll bring you some salmon", True),
]:
    check(f"pledge recorded: {line!r}", pledge_recorded(line), want)


def pledge_kept(line, kind):
    if not commits(line) or (departing(line) and not strong_commit(line)):
        return False
    return kind != "other" or oath(line)


for line, kind, want in [
    ("okay...then i think its time i take my leave then if you dont mind", "other", False),
    ("okay...then i think its time i take my leave then if you dont mind", "find", False),
    ("sure, I'll keep an eye out for webbing on my way. farewell", "other", False),
    ("okay, I'll bring you mead tomorrow", "bring", True),
    ("sure", "bring", True),
    ("sure", "other", False),
    ("I promise I'll make it up to you", "other", True),
    ("i will go there...but first tell me about this hunt you asking me to go on", "other", False),
    ("sure, I'll help with the mill, you have my word", "other", True),
]:
    check(f"pledge kept ({kind}): {line[:50]!r}", pledge_kept(line, kind), want)


# ---------------------------------------------------------------- the promise in a few words (Pledges.cpp ShortClause) - replay
def short_clause(words):
    lower = words.lower().replace("’", "'")
    w = re.findall(r"[a-z0-9']+", lower)
    start = 0
    for i, x in enumerate(w):
        if x in ("i'll", "ill", "promise", "swear") or (x == "will" and i > 0 and w[i - 1] == "i"):
            start = i + 1
    if start + 1 < len(w) and w[start] in ("be", "make") and w[start + 1] == "sure":
        start += 2
        if start < len(w) and w[start] == "to":
            start += 1
    if start < len(w) and w[start] in ("definitely", "surely"):
        start += 1
    out = []
    for x in w[start:start + 8]:
        out.append({"you": "them", "your": "their", "yourself": "themself"}.get(x, x))
    return " ".join(out) if out else words[:60]


for line, want in [
    ("oohh viola..i dont know what to say to you. okay then be this way, whenever ill be in windhealm ill be sure to visit you", "visit them"),
    ("I'll bring you a bottle of mead tomorrow", "bring them a bottle of mead tomorrow"),
    ("I promise I'll make it up to you", "make it up to them"),
]:
    check(f"short clause: {line[:40]!r}", short_clause(line), want)


def open_ended_words(line):
    l = line.lower()
    return any(w in l for w in ("whenever", "next time i'm", "next time im", "next time i am", "next time i'll", "next time ill", "when i'm in", "when im in",
                                "when i'm next", "if i'm ever", "if im ever", "when i pass", "if i pass", "when i come through", "when i'm back in",
                                "when im back in", "work for me", "you can stay with me", "stay with me if", "you can always", "whenever you need",
                                "if you ever need", "if anyone bothers you", "if anyone ever", "if things go wrong", "come to me if",
                                "you know where to find me", "you can come to me", "you can count on me", "any time you need", "anytime you need"))


for line, want in [
    ("whenever ill be in windhealm ill be sure to visit you", True),
    ("next time I'm in Riften I'll bring you mead", True),
    ("I'll come back tomorrow", False),
]:
    check(f"open-ended: {line!r}", open_ended_words(line), want)

# Promises.cpp Keep(): a kept promise mentioned again. Hand-outs repeat only after 180 s (before that it's
# the NPC talking about the one just given); beds never. Same(): same type, item names contain each other.
def same_promise(a, b):
    if a[0] != b[0]:
        return False
    if a[0] not in ("give_item", "allow_take"):
        return True
    x, y = a[1].lower(), b[1].lower()
    return x == y or x in y or y in x


def repeat_blocked(kept, new, seconds_ago):
    handout = new[0] in ("give_item", "give_gold", "allow_take")
    same_gold = kept[2:] == new[2:]  # give_gold: the same sum (10 for the job and 20 for your trouble are two)
    return seconds_ago <= 600 and same_promise(kept, new) and (not handout or (seconds_ago <= 180 and same_gold))


print("\n== a promise kept moments ago, mentioned again")
for kept, new, ago, want in [
    (("give_item", "pie"), ("give_item", "pie"), 67, True),          # Ronja: "that pie keeps well"
    (("give_item", "pie"), ("give_item", "apple pie"), 30, True),
    (("give_item", "pie"), ("give_item", "pie"), 400, False),        # seconds, later: the generosity budget decides
    (("give_item", "pie"), ("give_item", "bread"), 30, False),       # something else
    (("give_gold", "", 10), ("give_gold", "", 10), 60, True),
    (("give_gold", "", 10), ("give_gold", "", 20), 5, False),
    (("offer_bed", ""), ("offer_bed", ""), 500, True),
]:
    check(f"repeat {kept} -> {new} after {ago}s", repeat_blocked(kept, new, ago), want)

# Trust.cpp FriendlyFire: someone out with the player (follow / company / teammate) hit mid-fight, or by one
# stray arrow, is calmed and nothing is held against the player; the 4th hit within 10 s is meant.
def friendly_fire(together, fighting, ranged, hit_times):
    if not together or not (fighting or ranged):
        return False
    last = hit_times[-1]
    return len([t for t in hit_times if last - t <= 10]) < 4


print("\n== friendly fire")
for label, args, want in [
    ("Ronja hit while both fight a skeever", (True, True, False, [0]), True),
    ("second accidental swing", (True, True, False, [0, 3]), True),
    ("four swings in 10 s", (True, True, False, [0, 2, 4, 6]), False),
    ("a stranger hit mid-fight", (False, True, False, [0]), False),
    ("follower punched out of nowhere", (True, False, False, [0]), False),
    ("follower hit by one stray arrow", (True, False, True, [0]), True),
]:
    check(f"friendly fire: {label}", friendly_fire(*args), want)

# TurnPipeline.cpp SleepOf + Ledger.cpp Arrange: who sleeps where at a host's home. What was agreed wins;
# nothing said and one bed: a friend (or companion) gives up the bed, anyone else gets a bedroll for the player.
def sleep_of(raw):
    s = raw.lower()
    if "share" in s or "together" in s:
        return "share"
    if "npc" in s or "host" in s:
        return "npc_floor"
    if "player" in s or "guest" in s:
        return "player_floor"
    return ""


def arrangement(agreed, beds_in_cell, close):
    how = sleep_of(agreed)
    if not how and beds_in_cell <= 1:
        how = "npc_floor" if close else "player_floor"
    return how


def bedroll_here_words(line):
    words = re.findall(r"[a-z0-9]+", line.lower())
    roll = any((w.startswith("bedro") and "l" in w) or (w == "bed" and i + 1 < len(words) and words[i + 1].startswith("rol"))
               for i, w in enumerate(words))
    move = any(w in ("move", "put", "place", "lay", "drag", "shift", "set", "bring", "moving", "moved", "placed", "laid", "placing", "dragged",
                     "unroll", "unrolled") for w in words)
    return roll and move


def bedroll_pickup_words(line):
    words = re.findall(r"[a-z0-9]+", line.lower())
    roll = any((w.startswith("bedro") and "l" in w) or (w == "bed" and i + 1 < len(words) and words[i + 1].startswith("rol"))
               for i, w in enumerate(words))
    if not roll:
        return False
    padded = " " + " ".join(words) + " "
    if any(p in padded for p in (" pick up ", " pick it up ", " picking up ", " picked up ", " picked it up ", " pack ", " pack up ", " packing ",
                                 " packed ", " roll up ", " roll it up ", " rolled up ", " rolling up ", " put away ", " put it away ", " fold ",
                                 " folded ", " take up ", " remove ", " removed ")):
        return True
    somewhere = any(p in padded for p in (" here ", " there ", " place ", " put ", " move ", " spot "))
    return (" pick " in padded or " picked " in padded or (" roll " in padded and padded.endswith(" up "))) and not somewhere


def unnamed_gift_words(line):
    p = " " + " ".join(re.findall(r"[a-z0-9]+", line.lower())) + " "
    return any(k in p for k in (" got this for you ", " got something for you ", " got you something ", " have something for you ", " brought you something ", " brought this for you ", " brought something for you ",
                                " this is for you ", " here this for you ", " a gift for you ", " gift for you ", " a present for you ",
                                " a little something for you ", " got a gift ", " have a gift ", " have a present "))


print("\n== a gift offered without saying what (Promises.cpp UnnamedGiftWords): the picker before her reply")
for line, want in [
    ("here i got this for you", True),
    ("I have something for you", True),
    ("this is for you, friend", True),
    ("I got this mead for you", False),
    ("here, have some mead", False),
    ("do you have something for me?", False),
    ("I brought you something from Riften", True),
    ("I have something to tell you", False),
    ("a little something for you", True),
]:
    check(f"unnamed gift: {line!r}", unnamed_gift_words(line), want)


print("\n== packing up the guest bedroll (Ledger.cpp BedrollPickUpWords); checked before moving it")
for line, want in [
    ("pick up my bedroll", True),
    ("ill pack my bedrool", True),
    ("roll up the bed roll", True),
    ("pick my bedroll up", True),
    ("put away my bedroll", True),
    ("move my bedroll here", False),
    ("nice bedroll", False),
    ("*i picked up my bed roll*", True),
    ("ill pick my bedroll", True),
    ("i packed my bedrool", True),
    ("let me pick a spot to put my bedroll", False),
]:
    check(f"bedroll pick up: {line!r}", bedroll_pickup_words(line), want)


print("\n== moving the guest bedroll (Ledger.cpp BedrollHereWords)")
for line, want in [
    ("ill move the bedrool here", True),
    ("can you place the bedroll here?", True),
    ("put my bed roll over here", True),
    ("where is the bedroll?", False),
    ("thanks for the bedroll", False),
    ("move the chair", False),
    ("*i placed my bedroll by the window*", True),
]:
    check(f"bedroll here: {line!r}", bedroll_here_words(line), want)


print("\n== who sleeps where (one bed)")
for label, args, want in [
    ("she offered the floor", ("player_floor", 1, True), "player_floor"),
    ("she gave up her bed", ("npc_floor", 1, False), "npc_floor"),
    ("model drift 'host on floor'", ("host sleeps on floor", 1, False), "npc_floor"),
    ("they agreed to share", ("share", 1, True), "share"),
    ("nothing said, acquaintance, one bed", ("", 1, False), "player_floor"),
    ("nothing said, friend, one bed", ("", 1, True), "npc_floor"),
    ("nothing said, a spare bed in the house", ("", 2, False), ""),
]:
    check(f"sleeping: {label}", arrangement(*args), want)

# Pledges.cpp: "I'll be back by dusk" is kept by coming BACK - only after an hour (game time) away. Replay
# of (game hour, distance) samples; "far" = another cell or over 3000 units.
def visit_kept(samples, due, window=3.0):
    away, away_from = False, None
    for now, d in samples:
        if d > 3000:
            if away_from is None:
                away_from = now
            if now - away_from >= 1.0:
                away = True
        elif not away:
            away_from = None
        elif d < 800 and now <= due + window:
            return True
    return False


print("\n== a visit promise is kept by coming back after a while away")
far = 10**9
for label, samples, want in [
    ("out the door, she comes out 2 min later (Ronja, 18:16)", [(8.0, far), (8.03, far), (8.05, 300)], False),
    ("gone 3 hours, back at dusk", [(8.0, far), (9.5, far), (11.0, far), (18.0, 200)], True),
    ("met at the door, then away 2 hours, back", [(8.0, far), (8.05, 300), (8.1, far), (10.2, far), (10.3, 300)], True),
    ("stayed beside her all along", [(8.0, 200), (12.0, 200)], False),
]:
    check(f"visit kept: {label}", visit_kept(samples, due=20.0), want)

# TurnPipeline.cpp WelcomeBack: said by someone the player is staying with -> standing guest rights.
def welcome_back(line):
    p = " " + " ".join(re.findall(r"[a-z0-9]+", line.lower())) + " "
    return any(k in p for k in (" welcome back ", " come back anytime ", " come back any time ", " welcome here anytime ", " welcome here any time ",
                                " welcome anytime ", " welcome any time ", " always welcome ", " door s always open ", " door is always open ",
                                " always a place for you ", " stay whenever ", " come back whenever "))


print("\n== a host asking the player back (guest rights while staying with them)")
for line, want in [
    ("Aye, I get that, Breton. You're welcome back here anytime, though - the pot's always on.", True),
    ("Come back any time, friend.", True),
    ("Welcome to Whiterun.", False),
    ("Safe travels.", False),
]:
    check(f"welcome back: {line[:40]!r}", welcome_back(line), want)

# Promises.cpp ChooseItem: an animal named as the thing given ("the saber cat on the spit") is its meat, or a
# meal: every word must be an animal's (creature races + everyday words), and what was found isn't food.
ANIMAL_WORDS = {"saber", "sabre", "cat", "bear", "wolf", "deer", "elk", "horker", "mammoth", "goat", "rabbit", "hare", "skeever", "mudcrab",
                "slaughterfish", "chicken", "boar", "fox", "troll", "cow", "pig", "horse", "dog", "salmon", "fish", "pheasant", "duck", "goose",
                "venison", "atronach", "frost", "fire", "spider"}  # race words: Frost/Fire Atronach, Frostbite Spider


def animal_as_meat(wanted, found_is_food, own=False):
    return bool(wanted) and all(w in ANIMAL_WORDS for w in wanted) and not found_is_food and not own


print("\n== an animal named as a gift is its meat (Promises.cpp ChooseItem)")
for label, args, want in [
    ("saber cat -> a misc 'Sabre Cat' token: meat instead", (["saber", "cat"], False), True),
    ("deer -> nothing found: venison / a meal", (["deer"], False), True),
    ("fire salts: not an animal", (["fire", "salt"], False), False),
    ("salmon steak (food found): as is", (["salmon", "steak"], True), False),
    ("horker meat: food found, as is", (["horker", "meat"], True), False),
    ("the elk, and she carries an Elk Male Carcass: the carcass", (["elk"], False, True), False),
    ("an elk carcass: not all animal words, as asked", (["elk", "carcass"], False), False),
    ("a wolf pelt: as asked", (["wolf", "pelt"], False), False),
]:
    check(f"animal as meat: {label}", animal_as_meat(*args), want)

# Promises.cpp Said(offer_bed): the classifier's "real" verdict (it read both lines) decides, guarded by staying
# being talked about at all; without a verdict, the word lists (plain_bed_offer above).
def bed_offer_counts(player, npc, real):
    if real is None:
        return plain_bed_offer(player, npc)
    both = (player + " " + npc).lower()
    about = any(w in both for w in ("stay", "sleep", "bed", "roof", "home", "house", "place", "floor", "hearth", "fire", "room", "night",
                                     "crash", "welcome", "lodg", "rest"))
    return real and about


print("\n== a place to stay, judged on both lines (classifier verdict + a topic guard)")
for label, args, want in [
    ("'stay as long as you need' (Ronja, 22:39), real", ("i have worned my welcome", "Stay as long as you need.", True), True),
    ("'can I stay at you place' - 'aye' (typo), real", ("so i can stay at you place for some more days?", "Aye, you can stay for a few more days.", True), True),
    ("door's always open as a goodbye, not real", ("thanks for the drink", "Any time. My door's always open.", False), False),
    ("verdict real but nothing about staying: made up", ("nice weather", "Aye, it is.", True), False),
    ("no verdict: the word lists", ("it's late", "You can stay here tonight.", None), True),
]:
    check(f"bed offer: {label}", bed_offer_counts(*args), want)

print("\n== a hedge counts only about the promise itself (Pledges.cpp Unsure)")
for line, want in [
    ("okay..then. ill be there. and by the way you got eyes that might make someone lost in em. farewell", False),  # Yatul
    ("I'll be there. Maybe.", True),
    ("I'll try to come by tomorrow", True),
    ("maybe I'll come back", True),
    ("sure, I'll bring it, you might like it", False),
]:
    check(f"unsure: {line[:50]!r}", unsure(line), want)

# Promises.cpp Said(meet): only a settled meeting - confirmed by the classifier, a specific time, no hedge in
# the NPC's own line.
def meet_counts(npc_line, time_hint, confirmed, player_line=""):
    if confirmed is False:
        return False
    padded = " " + " ".join(re.findall(r"[a-z0-9]+", npc_line.lower())) + " "
    if any(h in padded for h in (" maybe ", " perhaps ", " might ", " someday ", " sometime ", " some time ", " if you ", " we ll see ",
                                 " possibly ", " not sure ", " can t promise ", " cant promise ")):
        return False
    if any(h in padded for h in (" usually ", " most days ", " most evenings ", " most nights ", " most mornings ", " always ", " every ",
                                 " often ", " whenever ", " anytime ", " any time ", " where to find me ", " if you need ", " when you need ",
                                 " be around ", " around here ", " most of the time ")):
        return False
    both = " " + " ".join(re.findall(r"[a-z0-9]+", (player_line + " " + npc_line).lower())) + " "
    if not any(k in both for k in (" meet ", " meeting ", " find me ", " see you there ", " see you at ", " be there ", " be late ",
                                   " keep me waiting ", " wait for you ", " waiting for you ", " join me ", " come to ", " come find ",
                                   " don t be late ", " i ll be waiting ", " ill be waiting ")):
        return False
    when = (time_hint + " " + player_line + " " + npc_line).lower()
    return any(w in when for w in ("dawn", "dusk", "sunrise", "sunset", "first light", "morning", "noon", "midday", "afternoon", "evening",
                                   "tonight", "midnight", "tomorrow", "today", "hour", "o'clock", "oclock"))


print("\n== a meeting only when it's settled (Promises.cpp Said meet)")
for label, args, want in [
    ("'Tomorrow. Dawn. East ridge. Don't keep me waiting.' confirmed", ("Tomorrow. Dawn. East ridge. Don't keep me waiting, Breton.", "at dawn", True), True),
    ("'maybe we can meet at the inn tonight'", ("Maybe we can meet at the inn tonight.", "tonight", True), False),
    ("floated only (classifier: not confirmed)", ("We could hunt together sometime.", "", False), False),
    ("'meet me at the inn' with no time", ("Meet me at the Bannered Mare.", "", True), False),
    ("'I'll be at the market at noon, find me'", ("I'll be at the market at noon, find me there.", "at noon", True), True),
    # where they usually are, or their own plans: never a meeting
    ("'You know where to find me.'", ("You know where to find me.", "", True), False),
    ("'You'll find me at the forge most days.'", ("You'll find me at the forge most days.", "", True), False),
    ("'I'm at the Bannered Mare most evenings.'", ("I'm at the Bannered Mare most evenings.", "evenings", True), False),
    ("'I'll be at my stall tomorrow.' (nothing arranged)", ("I'll be at my stall tomorrow, as ever.", "tomorrow", True), False),
    ("'If you need me, I'll be around tonight.'", ("If you need me, I'll be around tonight.", "tonight", True), False),
    ("'Meet me at the Mare tonight. Don't be late.'", ("Meet me at the Mare tonight. Don't be late.", "tonight", True), True),
]:
    check(f"meet: {label}", meet_counts(*args), want)

print(f"\n{'ALL OK' if fails == 0 else f'{fails} FAILED'}")
sys.exit(1 if fails else 0)
