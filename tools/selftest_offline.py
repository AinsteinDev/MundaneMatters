"""
MundaneMatters self-test, offline: the same 150-odd cases as the in-game "Run Self-Test" (read straight
from TestCases() in TurnPipeline.cpp, so they never drift), the same prompt and context, judged the same
way (a mirror of Judge), sent to any OpenRouter model. No game needed.

COSTS OpenRouter calls on the user's key (one per case): ask before running.
Run: MM_MODEL=deepseek/deepseek-v4-flash MM_TEMP=0.7 python tools/selftest_offline.py [name filter]
"""
import concurrent.futures as cf
import contextlib
import importlib.util
import io
import json
import os
import re
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")
ROOT = Path(__file__).resolve().parent.parent
SRC = (ROOT / "src/TurnPipeline.cpp").read_text(encoding="utf-8")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sink = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")  # quiet, and still has reconfigure()
    with contextlib.redirect_stdout(sink):
        try:
            spec.loader.exec_module(mod)
        except SystemExit:
            pass
    return mod


cc = load("cc", ROOT / "tools/classifier_check.py")   # prompt, key, the call
lc = load("lc", ROOT / "tools/logic_check.py")         # norm_request / from_json / gift_from_json mirrors
cc.MODEL = os.environ.get("MM_MODEL", cc.MODEL)
cc.TEMPERATURE = float(os.environ.get("MM_TEMP", "0.7"))
PLAYER = "Zoro"


# ---------------------------------------------------------------- C++ initializers -> Python
def tokens(text):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif text.startswith("//", i):
            i = text.index("\n", i)
        elif c == '"':
            j, buf = i + 1, []
            while text[j] != '"':
                if text[j] == "\\":
                    buf.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}.get(text[j + 1], text[j + 1]))
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            yield ("str", "".join(buf))
            i = j + 1
        elif c in "{},":
            yield (c, c)
            i += 1
        elif c == "-" or c.isdigit():
            m = re.match(r"-?\d+", text[i:])
            yield ("num", int(m.group()))
            i += len(m.group())
        else:
            m = re.match(r"[A-Za-z_]\w*", text[i:])
            if not m:
                yield ("other", c)  # past the list (";"): never read
                i += 1
                continue
            yield ("id", m.group())
            i += len(m.group())


def parse(tok, named):
    """One value: a string (adjacent ones joined), a number, true/false, a named list, or a {...} list."""
    kind, v = next(tok)
    if kind == "str":
        out = v
        while tok.peek and tok.peek[0] == "str":
            out += next(tok)[1]
        return out
    if kind == "num":
        return v
    if kind == "id":
        if v in ("true", "false"):
            return v == "true"
        if v == "V" and tok.peek and tok.peek[0] == "{":
            return parse(tok, named)
        return list(named[v])
    if kind == "{":
        items = []
        while tok.peek[0] != "}":
            items.append(parse(tok, named))
            if tok.peek[0] == ",":
                next(tok)
        next(tok)
        return items
    raise ValueError(f"unexpected {kind} {v}")


class Peek:
    def __init__(self, it):
        self.it, self.peek = it, None
        self._adv()

    def _adv(self):
        try:
            self.peek = next(self.it)
        except StopIteration:
            self.peek = None

    def __next__(self):
        v = self.peek
        self._adv()
        return v


FIELDS = ["name", "services", "npc", "player", "reply", "request", "answer", "promises", "when", "always", "place", "gives", "offense",
          "apology", "recent", "item", "qty", "loan", "pledge", "sleep"]
DEFAULTS = {"request": "none", "answer": "-", "promises": [], "when": "", "always": False, "place": "", "gives": "none", "offense": "",
            "apology": "", "recent": [], "item": "", "qty": 0, "loan": "", "pledge": "", "sleep": ""}


def test_cases():
    body = SRC[SRC.index("const std::vector<TestCase>& TestCases()"):]
    named = {}
    for name, inner in re.findall(r"(\w+)\{([^{}]*)\}", body[: body.index("static const std::vector<TestCase> cases")]):
        named[name] = [s for _, s in tokens(inner) if _ == "str"] if inner.strip() else []
    start = body.index("cases{", body.index("static const std::vector<TestCase> cases")) + len("cases")
    tok = Peek(tokens(body[start:]))
    raw = parse(tok, named)
    cases = []
    for r in raw:
        c = dict(DEFAULTS)
        c.update(dict(zip(FIELDS, r)))
        cases.append(c)
    return cases


def service_json():
    block = SRC[SRC.index("json ServiceJson(const std::string& id)"):]
    block = block[block.index("kLabels{") + len("kLabels"): block.index("};") + 1]
    out = {}
    for entry in parse(Peek(tokens(block)), {}):
        sid, (label, examples) = entry
        out[sid] = (label, examples)
    return out


LABELS = service_json()


def user_block(c):
    lines = [f"The PLAYER is {PLAYER}. The NPC is {c['npc']}. It is now 21:00 (night). Here: Whiterun (Whiterun Hold).",
             "Places nearby, nearest first: The Bannered Mare (inn); Dragonsreach (palace); Temple of Kynareth (temple); "
             "Belethor's General Goods (store); Warmaiden's (smithy); Honningbrew Meadery (brewery)", "", f"Services {c['npc']} offers:"]
    if c["services"]:
        for sid in c["services"]:
            label, ex = LABELS[sid]
            lines.append(f'- "{sid}": {label} (e.g. {ex})')
    else:
        lines.append('- none (request is "none")')
    if c["recent"]:
        lines += ["", "Earlier in the conversation, oldest first:"] + ["- " + r.replace("{p}", PLAYER) for r in c["recent"]]
    lines += ["", "The exchange:", f'PLAYER ({PLAYER}): "{c["player"]}"', f'NPC ({c["npc"]}): "{c["reply"]}"']
    return "\n".join(lines)


# ---------------------------------------------------------------- Judge (TurnPipeline.cpp)
def norm_response(r):
    r = str(r).lower()
    if any(k in r for k in ("free", "house", "waive")):
        return "free"
    if any(k in r for k in ("refus", "declin", "reject", "deny", "denied")) or r == "no":
        return "refuse"
    if "counter" in r or "condition" in r:
        return "counter"
    return "accept"


def as_list(v):
    return v if isinstance(v, list) else [v] if isinstance(v, dict) else []


def qty_of(j):
    t = str(j.get("qty", "")).lower()
    if any(k in t for k in ("all", "every", "lot")):
        return -1
    try:
        v = float(j.get("qty"))
    except (TypeError, ValueError):
        return 1
    return int(v) if 1 <= v < 1e5 else -1 if v < 0 else 1


def sleep_of(raw):
    s = str(raw).lower()
    if "share" in s or "together" in s:
        return "share"
    if "npc" in s or "host" in s:
        return "npc_floor"
    if "player" in s or "guest" in s:
        return "player_floor"
    return ""


def truthy(v):
    return str(v).lower() in ("true", "yes", "1")


def judge(c, j):
    problems = []
    request = lc.norm_request(str(j.get("request", "none")))
    response = norm_response(j.get("response", "accept"))
    if request != c["request"]:
        problems.append(f"request {request} (wanted {c['request']})")
    if request == c["request"] and c["request"] != "none" and c["answer"] != "-":
        a = c["answer"]
        ok = response in ("accept", "free") if a == "yes" else response in ("refuse", "counter") if a == "no" else response == a
        if not ok:
            problems.append(f"response {response} (wanted {a})")
    ps = [p for p in (lc.from_json(x) for x in as_list(j.get("promises"))) if p]
    types = sorted(p["type"] for p in ps)
    if types != sorted(c["promises"]):
        problems.append(f"promises {types} (wanted {sorted(c['promises'])})")
    elif ps:
        if c["when"] and ps[0]["when"] != c["when"]:
            problems.append(f"when {ps[0]['when']} (wanted {c['when']})")
        if ps[0]["always"] != c["always"]:
            problems.append(f"always {ps[0]['always']} (wanted {c['always']})")
    if c["place"] and c["place"] not in str(j.get("place", "")).lower():
        problems.append(f"place '{j.get('place', '')}' (wanted '{c['place']}')")
    gives = "none"
    for gj in as_list(j.get("player_gives")):
        g = lc.gift_from_json(gj)
        if g:
            gives = "gold" if g["gold"] > 0 else "unnamed" if not lc.tokens(g["item"], True) else "named"
            break
    if c["gives"] != "-" and gives != c["gives"]:
        problems.append(f"player_gives {gives} (wanted {c['gives']})")
    if c["offense"]:
        hurt = "hurt" in str(j.get("offense", "none")).lower()
        if hurt != (c["offense"] == "hurtful"):
            problems.append(f"offense {j.get('offense')} (wanted {c['offense']})")
    if c["apology"] and truthy(j.get("apology_accepted", "false")) != (c["apology"] == "true"):
        problems.append(f"apology_accepted {j.get('apology_accepted')} (wanted {c['apology']})")
    if c["item"] and c["item"] not in str(j.get("item", "")).lower():
        problems.append(f"item '{j.get('item', '')}' (wanted '{c['item']}')")
    if c["loan"]:
        pl = any(truthy(x.get("loan")) for x in as_list(j.get("promises")) if isinstance(x, dict))
        gl = any(truthy(x.get("loan")) for x in as_list(j.get("player_gives")) if isinstance(x, dict))
        want = c["loan"]
        ok = (pl and not gl) if want == "promise" else (gl and not pl) if want == "gift" else (not pl and not gl)
        if not ok:
            problems.append(f"loan: promise {pl} gift {gl} (wanted {want})")
    if c["pledge"]:
        got = "none"
        for o in as_list(j.get("player_promises")):
            if isinstance(o, dict):
                got = "cancel" if "cancel" in str(o.get("when", "")).lower() else str(o.get("type", "")).lower()
                break
        if c["pledge"] not in got:
            problems.append(f"player_promises {got} (wanted {c['pledge']})")
    if c["qty"] and qty_of(j) != c["qty"]:
        problems.append(f"qty {qty_of(j)} (wanted {c['qty']})")
    if c["sleep"]:
        got = sleep_of(j.get("sleep", "")) or "none"
        if got != c["sleep"]:
            problems.append(f"sleep {got} (wanted {c['sleep']})")
    return "; ".join(problems)


def run_one(c, system, key):
    try:
        j = cc.classify(system, user_block(c), key)
    except Exception as e:  # the provider, not the classifier
        return c, None, f"call failed: {e}"
    if not j:
        return c, j, "no JSON in reply"
    return c, j, judge(c, j)


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    cases = [c for c in test_cases() if only in c["name"]]
    key, system = cc.api_key(), cc.system_block()
    print(f"{len(cases)} cases, model {cc.MODEL}, temperature {cc.TEMPERATURE}\n")
    passed = skipped = 0
    fails = []
    with cf.ThreadPoolExecutor(4) as ex:  # the in-game test sends 4 at a time too
        for c, j, problems in ex.map(lambda c: run_one(c, system, key), cases):
            if problems.startswith("call failed"):
                skipped += 1
                print(f"SKIP  {c['name']}: {problems}")
            elif problems:
                fails.append((c, j, problems))
                print(f"FAIL  {c['name']}: {problems}")
            else:
                passed += 1
    print(f"\n{passed}/{len(cases) - skipped} passed" + (f" ({skipped} unreached)" if skipped else ""))
    for c, j, problems in fails:
        print(f"\n--- {c['name']}: {problems}\n  player: {c['player']}\n  npc:    {c['reply']}\n  model:  {json.dumps(j)[:400]}")


if __name__ == "__main__":
    main()
