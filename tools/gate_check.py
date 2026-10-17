"""
Gate check (no API calls): which exchanges go to the classifier.

1. Every self-test case (TestCases() in TurnPipeline.cpp) that expects something - a service, a promise,
   a gift, the player's own promise, a loan, a sleeping arrangement - must still pass the gate. A case
   the gate drops would be a case the mod no longer catches: that must never happen.
2. The real exchanges in the last MundaneMatters.log: how many the old and the new gate send.

Word lists are read from TurnPipeline.cpp, so this checks the code as it is. ItemWords (food and
ingredient names from the load order) is approximated with the vanilla sample in logic_check.py.
Run: python tools/gate_check.py
"""
import importlib.util
import re
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")
ROOT = Path(__file__).resolve().parent.parent
SRC = (ROOT / "src/TurnPipeline.cpp").read_text(encoding="utf-8")
LOG = Path.home() / "Documents/My Games/Skyrim Special Edition/SKSE/MundaneMatters.log"

spec = importlib.util.spec_from_file_location("st", ROOT / "tools/selftest_offline.py")
st = importlib.util.module_from_spec(spec)
spec.loader.exec_module(st)
lc = st.lc


def cpp_strings(name):
    m = re.search(name + r"\s*\{(.*?)\};", SRC, re.S)
    return re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))


PROMISE_WORDS = set(cpp_strings("kPromiseWords"))
PROMISE_PHRASES = cpp_strings("kPromisePhrases")
PLEDGE_PHRASES = cpp_strings("kPledgePhrases")
NPC_OFFERS = cpp_strings("kNpcOfferPhrases") if "kNpcOfferPhrases" in SRC else []
COMMITS = cpp_strings("kNpcCommits") if "kNpcCommits" in SRC else []
SHOP_WORDS = set(cpp_strings("kShopWords")) if "kShopWords" in SRC else set()
SHOP_PHRASES = cpp_strings("kShopPhrases") if "kShopPhrases" in SRC else []
ROOM_WORDS = set(cpp_strings("kRoomWords")) if "kRoomWords" in SRC else set()
TRAIN_WORDS = set(cpp_strings("kTrainWords")) if "kTrainWords" in SRC else set()
ITEM_WORDS = lc.ITEM_WORDS


def words(s):
    return re.findall(r"[a-z0-9]+", s.lower())


def digits_or_foreign(s):
    return any(c.isdigit() for c in s) or lc.has_foreign_letters(s)


def old_promise(player, npc):
    for line in (player, npc):
        if digits_or_foreign(line):
            return True
        low = line.lower()
        if any(p in low for p in PROMISE_PHRASES) or any(p in low for p in PLEDGE_PHRASES):
            return True
        if any(w in PROMISE_WORDS or w in ITEM_WORDS for w in words(low)):
            return True
    return False


def sentences(text):
    return [s for s in re.split(r"[.!?\n*]+", text.lower()) if s.strip()]


# ---- the new gate: the player's line as before; the NPC's reply only on what offers or commits ----
def new_promise(player, npc):
    if digits_or_foreign(player):
        return True
    low = player.lower()
    if any(p in low for p in PROMISE_PHRASES) or any(p in low for p in PLEDGE_PHRASES):
        return True
    if any(w in PROMISE_WORDS or w in ITEM_WORDS for w in words(low)):
        return True
    # The NPC: a number (a price, an amount), an offer ("take this", "you can sleep here"), or a sentence
    # where they commit ("I'll", "let me") to something a promise is made of.
    if digits_or_foreign(npc):
        return True
    nlow = npc.lower().replace("\u2019", "'")
    if any(p in nlow for p in PROMISE_PHRASES) or any(p in nlow for p in NPC_OFFERS):
        return True
    for s in re.split(r"[.!?*\n]", nlow + "."):
        pad = " " + s + " "
        commit = False
        for c in COMMITS:
            at = pad.find(c)
            while at != -1:
                if not pad[at - 1].isalnum() and not pad[at + len(c)].isalnum():
                    commit = True
                    break
                at = pad.find(c, at + 1)
        if commit and any(w in PROMISE_WORDS or w in ITEM_WORDS for w in words(s)):
            return True
    return False


def might_service(services, player, npc):
    """Trade / buy / sell / rent_room / train: only when the talk is about them."""
    both = (player + " " + npc).lower()
    ws = set(words(both))
    wanted = []
    if any(s in services for s in ("trade", "buy", "sell")) and (digits_or_foreign(player + npc) or ws & SHOP_WORDS or any(p in both for p in SHOP_PHRASES)):
        wanted.append("shop")
    if "rent_room" in services and ws & ROOM_WORDS:
        wanted.append("room")
    if "train" in services and (ws & TRAIN_WORDS or "show me how" in both):
        wanted.append("train")
    return wanted


def expects_something(c):
    return (c["request"] != "none" or c["promises"] or c["gives"] not in ("none", "-") or c["pledge"] not in ("", "none") or
            c["loan"] not in ("", "none") or c["sleep"] not in ("", "none"))


def main():
    fails = 0
    cases = st.test_cases()
    shop_ids = ("trade", "buy", "sell", "rent_room", "train")
    print("== 1. self-test cases that expect something must still reach the classifier")
    for c in cases:
        if not expects_something(c):
            continue
        needs_promise = c["request"] == "none" or c["request"] not in c["services"]
        if c["request"] in shop_ids:
            kind = {"trade": "shop", "buy": "shop", "sell": "shop", "rent_room": "room", "train": "train"}[c["request"]]
            if kind not in might_service(c["services"], c["player"], c["reply"]):
                fails += 1
                print(f"  DROPPED (service gate) {c['name']}: {c['player']!r} / {c['reply']!r}")
        if (c["promises"] or c["gives"] not in ("none", "-") or c["pledge"] not in ("", "none") or c["sleep"] not in ("", "none")) or needs_promise:
            # These ride on the promise gate unless an activity gate (unchanged) sends them anyway.
            strict = c["promises"] or c["gives"] not in ("none", "-") or c["pledge"] not in ("", "none") or c["loan"] not in ("", "none")
            if old_promise(c["player"], c["reply"]) and not new_promise(c["player"], c["reply"]) and (strict or c["request"] not in c["services"]):
                fails += 1
                print(f"  DROPPED (promise gate) {c['name']}: {c['player']!r} / {c['reply']!r}")
            elif old_promise(c["player"], c["reply"]) and not new_promise(c["player"], c["reply"]):
                print(f"  note: promise gate now off, the '{c['request']}' gate still sends it: {c['name']}")
    for pl, npc, want in lc.gate_cases:  # logic_check's curated offers and small talk
        if want and not new_promise(pl, npc):
            fails += 1
            print(f"  DROPPED (curated offer) {pl!r} / {npc!r}")
    print(f"  {'ALL KEPT' if not fails else f'{fails} DROPPED'}")

    print("\n== 2. last session's real exchanges (promise gate only; activity / trust gates unchanged)")
    if LOG.exists():
        lines = LOG.read_text(encoding="utf-8", errors="ignore").splitlines()
        pairs, cur = [], None
        for l in lines:
            m = re.search(r'player: "(.*)" \(target', l)
            if m:
                cur = m.group(1)
            m = re.search(r"\] (.+?) replied: \"(.*)\"$", l)
            if m and cur is not None:
                pairs.append((cur, m.group(2)))
                cur = None
        old = sum(old_promise(p, n) for p, n in pairs)
        new = sum(new_promise(p, n) for p, n in pairs)
        print(f"  {len(pairs)} exchanges: old promise gate {old}, new {new}")
        for p, n in pairs:
            if old_promise(p, n) and not new_promise(p, n):
                print(f"  now skipped: {p[:70]!r}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
