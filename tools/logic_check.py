"""
MundaneMatters logic check: a line-for-line Python mirror of the deterministic C++ pieces, run against
RP exchanges written in different model styles. Keep in sync with:
  JsonUtil.h        ParseNumber / Num / Str
  TurnPipeline.cpp  MightPromise / ParseReply (repair + salvage) / Confidence / Price /
                    NormalizeRequest / NormalizeResponse / PromiseList / decision
  Promises.cpp      Stem / Tokens / Score / BestInWorld / IsGenericMeal / FromJson
  Services.cpp      Haggled
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
    if any(k in t for k in ("gold", "money", "coin", "pay")):
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
PROMISE_WORDS = {
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
    if any(k in r for k in ("trade", "barter", "shop", "wares", "merchant")) or r in ("buy", "sell"):
        return "trade"
    if "room" in r or "rent" in r or "lodg" in r or r == "inn":
        return "rent_room"
    if "train" in r or "teach" in r or "lesson" in r:
        return "train"
    if any(k in r for k in ("escort", "guide", "lead", "travel", "take_me", "show_way")):
        return "escort"
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

print(f"\n{'ALL OK' if fails == 0 else f'{fails} FAILED'}")
sys.exit(1 if fails else 0)
