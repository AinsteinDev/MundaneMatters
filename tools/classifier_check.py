"""
MundaneMatters classifier check: sends RP exchanges through the REAL classifier prompt
(mundanematters_turn.prompt) to the same model SkyrimNet uses (OpenRouter), several runs each, and
reports how often the request comes back right - before and after the C++ fallbacks (mirrored here).
No game needed. Costs a few OpenRouter calls (cheap model).

Run: python tools/classifier_check.py [runs per case, default 3] [case name filter]
Keep the service labels below in sync with Services::For (Services.cpp).
"""
import json
import re
import sys
import urllib.request
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parent.parent
PROMPT = ROOT / "data/SKSE/Plugins/SkyrimNet/external/mundanematters.core/prompts/mundanematters_turn.prompt"
# Your OpenRouter key: the OPENROUTER_API_KEY environment variable, or read from your SkyrimNet Providers.yaml
# (set MM_PROVIDERS_YAML to its path). Every case is one paid API call.
PROVIDERS_YAML = Path(__import__("os").environ.get("MM_PROVIDERS_YAML", "Providers.yaml"))
MODEL = __import__("os").environ.get("MM_MODEL", "google/gemma-4-31b-it")
TEMPERATURE = float(__import__("os").environ.get("MM_TEMP", "1.0"))  # as configured for the mundanematters_classifier variant

# ---------------------------------------------------------------- services (Services::For)
SIT = ("sit", 'sit down together NOW (set "item" to where: "fire", "ground", "bench", "" when not said; "hours" when the PLAYER says how long)',
       '"sit with me a while", "let\'s take a break", "let\'s sit by the fire", "watch the stars with me"')
VISIT_LABEL = ('go TOGETHER to a home now, to relax / sit there (set "item": "yours" = the NPC\'s own home, "mine" = the PLAYER\'s home or the name of it; '
               'someone living with the PLAYER: always "mine")')
VISIT_EX = '"let\'s go to your place and relax", "come back to mine for a while", "let\'s go home and sit", "let\'s go inside and sit" at the NPC\'s door'
FOLLOW = ("follow", "come along with the PLAYER now, walking behind them (the PLAYER leads)",
          '"follow me", "come with me", "tag along", "stick close", "walk with me to the market", "come on, let\'s go"')
COMPANY = ("company", "stay with the PLAYER for a while, keeping them company here (not walking somewhere: that's follow; not sitting down: that's sit)",
           '"keep me company", "stay with me a while", "spend some time with me", "stick around for a bit", the NPC: "I\'ll keep you company"')
DRINK = ("drink", 'sit down and have a drink together NOW (set "item" to the drink named, "" for any)',
         '"a round of mead?", "have a drink with me", "let\'s grab an ale", "fancy a pint?", "join me for a drink"')
REST = ("rest", "go to bed / sleep now, together with or near the PLAYER (beds or bedrolls where they are)",
        '"let\'s turn in", "time to get some rest", "we should sleep", "let\'s make camp for the night", "I\'m tired, let\'s bed down"')


def visit(right_here: bool):
    return ("visit", VISIT_LABEL + (' - the NPC\'s home is right here, so "let\'s go inside" means it' if right_here else ""), VISIT_EX)


def services(right_here=False, offer_visit=True, escort=False):
    out = [FOLLOW, SIT, COMPANY, DRINK, REST]
    if offer_visit:
        out.insert(2, visit(right_here))
    if escort:
        out.insert(1, ("escort", "lead the PLAYER somewhere now, walking ahead (set \"place\")", '"take me to the inn", "show me the way to the temple"'))
    return out


# ---------------------------------------------------------------- cases
# expect: set of acceptable requests; item: acceptable items for visit (None = any); not_accept: the visit must not run.
CASES = [
    dict(name="ronja_inside_door", here="Whiterun, outside Ronja's House", svc=services(right_here=True),
         player="common lets go inside and relax it chilly out here",
         npc="Fair enough, Breton. Let's get inside before the evening settles in. *She gestures toward the door, her step already turning.* I'll bank the fire.",
         expect={"visit"}, item={"yours"}),
    dict(name="inn_your_place", here="The Bannered Mare, Whiterun", svc=services(),
         player="let's go to your place and relax for a while", npc="Aye, why not. Lead the way, I'll follow you.",
         expect={"visit"}, item={"yours"}),
    dict(name="inn_my_place", here="The Bannered Mare, Whiterun", svc=services(),
         player="come back to mine for a while, I've got a fire going", npc="I'd like that. After you.",
         expect={"visit"}, item={"mine"}),
    dict(name="moved_in_our_house", here="Whiterun market", svc=services(),
         player="let's head back to our house and put our feet up", npc="Gladly. I'm right behind you, love.",
         expect={"visit"}, item=None),
    dict(name="named_house", here="The Bannered Mare, Whiterun", svc=services(),
         player="let's go to Breezehome and unwind", npc="Sounds good. Lead on.",
         expect={"visit"}, item={"mine", "breezehome"}),
    dict(name="stranger_declines", here="The Bannered Mare, Whiterun", svc=services(),
         player="let's go to your place and relax", npc="My place? I barely know you, stranger. I'll stay right here.",
         expect={"visit", "none"}, not_accept=True),
    dict(name="too_far", here="The Bannered Mare, Whiterun", svc=services(),
         player="let's go to your house and sit a while", npc="My house is in Riften, friend. That's days away - let's just sit here.",
         expect={"visit", "sit", "none"}, not_accept_visit=True),
    dict(name="plain_sit", here="Whiterun, by the well", svc=services(offer_visit=False),
         player="sit with me a while", npc="Alright, just for a bit.", expect={"sit"}),
    dict(name="plain_follow", here="Whiterun market", svc=services(offer_visit=False),
         player="follow me, I need you at the gate", npc="Right behind you.", expect={"follow"}),
    dict(name="show_home_escort", here="Whiterun market", svc=services(escort=True),
         player="where do you live? show me", npc="This way, it's just past the smithy. Follow me.", expect={"escort", "none"}),
]


# ---------------------------------------------------------------- fallback mirror (TurnPipeline.cpp)
NPC_MOVE = ["follow me", "this way", "come with me", "come along", "i'll take you", "i will take you", "i'll show you", "i will show you", "i'll lead",
            "lead the way", "lead on", "walk you", "after me", "let's go", "lets go", "on our way", "right behind you", "i'll follow", "i will follow",
            "after you", "i'm coming", "keep up", "stay close", "i'll guide"]
VISIT_NO = ["not now", "another time", "some other time", "can't", "cannot", "too far", "rather not", "don't think so", "i don't know you", "barely know",
            "no home", "not tonight", "i'd better not", "i won't", "maybe later"]
VISIT_YES = ["get inside", "go inside", "head inside", "come in", "come on in", "fair enough", "alright", "all right", "sure", "aye", "of course", "why not",
             "sounds good", "gladly", "i'd like that", "yes", "let's", "lets go", "inside then", "warm up"]
RELAX = ["relax", "sit", "rest", "warm", "a while", "awhile", "unwind", "chill", "drink", "talk"]
VISIT_WORDS = ["your place", "your home", "your house", "your hut", "your farm", "your shack", "your cabin", "back to yours", "to yours", "my place",
               "my home", "my house", "back to mine", "to mine", "back to my", "our place", "our home", "our house", "go home", "head home", "home together"]


def padded(s):
    return " " + " ".join(re.findall(r"[a-z0-9]+", s.lower().replace("\u2019", "'"))) + " "


def visit_words(line):
    p = padded(line)
    return any(f" {w} " in p for w in VISIT_WORDS)


def npc_agrees_to_visit(line):
    l = line.lower()
    if any(w in l for w in VISIT_NO):
        return False
    return any(w in l for w in NPC_MOVE) or any(w in l for w in VISIT_YES)


def fallback(request, response, item, case):
    offered = any(s[0] == "visit" for s in case["svc"])
    # VisitLine non-empty: MightVisit was true for the player line (home words, or "inside" at their door).
    noted = visit_words(case["player"]) or ("inside" in case["player"].lower() and "right here" in json.dumps(case["svc"]))
    if request != "visit" and offered and noted and (visit_words(case["player"]) or "inside" in case["player"].lower()) and npc_agrees_to_visit(case["npc"]):
        walk = request in ("follow", "escort")
        if request in ("none", "sit", "company") or (walk and any(w in case["player"].lower() for w in RELAX)):
            pl = " " + case["player"].lower() + " "
            mine = any(w in pl for w in (" my place", " my home", " my house", " mine", " our "))
            return "visit", "accept", (item if item and not walk else ("mine" if mine else "yours")), True
    return request, response, item, False


# ---------------------------------------------------------------- prompt + call
def system_block():
    text = PROMPT.read_text(encoding="utf-8")
    text = re.sub(r"\{#.*?#\}", "", text, flags=re.S)
    sys_part = text.split("[ system ]", 1)[1].split("[ end system ]", 1)[0]
    return sys_part.replace("{% raw %}", "").replace("{% endraw %}", "").strip()


def user_block(case):
    lines = [f'The PLAYER is Zoro. The NPC is {case.get("npc_name", "Ronja")}. It is now 19:00 (evening). Here: {case["here"]}.', "",
             f'Services {case.get("npc_name", "Ronja")} offers:']
    for sid, label, ex in case["svc"]:
        lines.append(f'- "{sid}": {label} (e.g. {ex})')
    lines += ["", "The exchange:", f'PLAYER (Zoro): "{case["player"]}"', f'NPC ({case.get("npc_name", "Ronja")}): "{case["npc"]}"']
    return "\n".join(lines)


def api_key():
    env = __import__("os").environ.get("OPENROUTER_API_KEY")
    if env:
        return env
    m = re.search(r"sk-or-[A-Za-z0-9_-]+", PROVIDERS_YAML.read_text(encoding="utf-8"))
    if not m:
        raise SystemExit("no OpenRouter key found in " + str(PROVIDERS_YAML))
    return m.group(0)


def classify(system, user, key):
    body = json.dumps({"model": MODEL, "temperature": TEMPERATURE, "max_tokens": int(__import__("os").environ.get("MM_MAX_TOKENS", "4096")),
                       "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}]}).encode()
    req = urllib.request.Request("https://openrouter.ai/api/v1/chat/completions", data=body,
                                 headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=90) as r:
        out = json.loads(r.read())
    raw = out["choices"][0]["message"]["content"] or ""
    m = re.search(r"\{.*\}", raw, re.S)
    try:
        return json.loads(m.group(0)) if m else {}
    except json.JSONDecodeError:
        return {}


def main():
    runs = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    only = sys.argv[2] if len(sys.argv) > 2 else ""
    key, system = api_key(), system_block()
    total_ok = total = 0
    for case in CASES:
        if only and only not in case["name"]:
            continue
        ok = 0
        detail = []
        for _ in range(runs):
            j = classify(system, user_block(case), key)
            req = str(j.get("request", "none")).lower()
            resp = str(j.get("response", "none")).lower()
            item = str(j.get("item", "")).lower()
            freq, fresp, fitem, used = fallback(req, resp, item, case)
            runs_visit = freq == "visit" and fresp not in ("refuse", "counter")
            good = freq in case["expect"]
            if good and freq == "visit" and case.get("item") and runs_visit:
                good = any(i in fitem for i in case["item"])
            if case.get("not_accept") or case.get("not_accept_visit"):
                good = good and not runs_visit
            ok += good
            detail.append(f"{req}/{resp}/{item or '-'}" + (f" -> fallback {freq}/{fitem}" if used else "") + ("" if good else "  <-- WRONG"))
        total_ok += ok
        total += runs
        print(f"[{'PASS' if ok == runs else 'FAIL'}] {case['name']}: {ok}/{runs}")
        for d in detail:
            print("        " + d)
    print(f"\n{total_ok}/{total} runs right")
    sys.exit(0 if total_ok == total else 1)


if __name__ == "__main__":
    main()
