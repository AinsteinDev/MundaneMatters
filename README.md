<div align="center">

# Mundane Matters

**Your NPCs finally do what they say.**

A **SkyrimNet** add-on for Skyrim SE/AE that turns everyday conversation into real game actions:
trade, inn rooms, shared meals, gifts, guest rights, and promises NPCs actually keep.

![Status](https://img.shields.io/badge/status-work%20in%20progress-orange)
![SkyrimNet](https://img.shields.io/badge/SkyrimNet-beta%2025%2B-8a2be2)
![License](https://img.shields.io/badge/license-MIT-green)

**[⬇ Download the latest release](https://github.com/AinsteinDev/MundaneMatters/releases)**

</div>

---

## Why

SkyrimNet NPCs talk beautifully, but what they say often never happens. The innkeeper says *"here's a hot meal"* and you're still starving.
The farmer says *"take my bed"*, but the bed is still **Owned**.

**Mundane Matters closes that gap.** It reads each exchange by **meaning, not keywords**, and makes the game do what was just agreed,
with real items, real prices and real ownership.

> On my way to Riften I met **Sylgja**. She asked me to deliver a letter to her family, so I went back and did it. When I returned she let me
> stay the night and offered me a meal. In plain SkyrimNet that meal only existed in the dialogue. I play with Survival, where a meal you can
> barely afford matters. **With Mundane Matters, she actually hands it to me.**
>
> In Windhelm, **Viola** agreed to let me sleep in her bed if I helped track down the Butcher. The bed stayed *Owned*, so sleeping in it was
> still a crime. **Now the bed is really mine for the night.**

---

## Features

### 🛒 Trade, rooms and training, in your own words
Ask however you like. If they agree, it happens.

| You say | What happens |
|---|---|
| *"I've got pelts to sell, but first let's see your stock."* | The barter menu opens |
| *"Got a bed for the night?"* | A popup shows the **real** room price → *[I'll take the room] / [I'll pass]* |
| *"Can you teach me archery?"* | The training menu opens |

Haggling counts (*"fine, 70 for you"*), and *"on the house"* means free.

### 🤝 NPCs keep their promises

| The NPC says | What happens |
|---|---|
| *"Here, take 10 coins for your trouble."* | You get 10 gold |
| *"Have a bowl of stew."* | You get the stew, from **their own stock** if they have it |
| *"Come back tomorrow, I'll have your reward."* | They pay you when you next meet |
| *"I'll see you fed at dawn."* | Breakfast comes in the morning |
| *"I can't pay you after all."* | The promise is cancelled |

**They aren't a free buffet.** Each NPC has a generosity budget based on how well they know you:

| Relationship | Gives freely up to | Refills every |
|---|---|---|
| Stranger / acquaintance | 10 gold worth | 24 h |
| Friend | 20 | 12 h |
| Confidant | 60 | 12 h |
| Ally / companion | 80 | 6 h |
| Lover | 100 | 5 h |
| Hostile | nothing | — |

Food counts at half its value. Ask too often and they'll politely say no.

### 🛏️ Beds and guest rights
- *"You can sleep in the spare bed tonight."* The bed is yours for the night, so sleeping in it is no crime.
- *"You're always welcome at my place."* This gives you **guest rights**: on every visit to their home a bed is yours, and it goes back to them when you leave.

Sleeping *"on the floor"* or *"in the barn"* isn't treated as a bed offer.

### 🍷 Permission to take
*"Mind if I take that bottle of wine?"* / *"Go ahead, I've plenty."* Now taking that wine isn't stealing.
This only works in people's homes, never in shops or inns.

### 🍲 Shared meals
*"Let's eat together?"* / *"Sit with me, the stew's still warm."*
They walk to a free seat, sit down, share a dish with you and invite you to join. This only works with friends or closer, about once every 6 hours each.

### 🎁 Gifts
- *"Here, take this health potion."* A popup confirms, then it leaves your pack and goes into theirs.
- *"I brought you something."* A picker opens with your inventory, and you choose what to give.

Only things you actually carry ever move. **Rarely**, a truly good gift that really pleases them raises your relationship one step.
That's capped: only the first gift in a week counts, at most one step per person, and never past Confidant. You can't buy friendship.

### 🚶 Follow and escort *(uses SeverActions)*
- *"Follow me."* / *"Come with me, I could use the company."*
- *"Take me to the brewery."* They understand you mean **Honningbrew Meadery**.
- *"Show me where Hulda lives."* / *"Take me to your place."*

Locals won't walk you across Skyrim. A Riften shopkeeper turns down *"take me to Whiterun"* unless they're going there anyway.
Companions are the exception.

### 🧠 NPCs know the facts
Merchants quote their **real** prices and innkeepers know their real room cost. NPCs remember what they've promised you and what you've given them.

---

## Built to play nice
- **Typos, slang and roleplay sentences are fine.** *"folow me"*, *"tkae me to ur place"* and *"grab a bite wit me?"* all work.
- **Popup buttons are your character's own lines**, and the NPC answers in character. A popup waits until you've finished typing or closed other menus.
- **Works alongside SeverActions.** No double menus, payments or follows.
- **Cheap to run.** Small talk is skipped. Only lines that might matter cost one small AI call, and you can route that to its own fast, cheap model.
- **No ESP and no save bloat.** Everything lives in a small SKSE co-save.

---

## Requirements
- **SkyrimNet** beta 25 or newer (and its own requirements)
- **SeverActions**, optional: only needed for follow and escort

Tested on Skyrim AE 1.6.1170.

## Installation
1. Download `MundaneMatters-vX.Y.Z.zip` from [Releases](https://github.com/AinsteinDev/MundaneMatters/releases).
2. Install it with **Mod Organizer 2** or **Vortex** like any other mod, or extract it into your `Data` folder.
3. Start the game. **Mundane Matters** appears in SkyrimNet's Plugins list.
4. *(Recommended)* In SkyrimNet's AI settings, set the **`mundanematters_classifier`** model slot to a fast, cheap model that writes clean JSON
   (low temperature works best). If you leave it unset, it uses your default model.

**Uninstalling:** there's no ESP, so you can turn it off at any time. Its co-save data is just ignored. A bed that was lent to you when you uninstalled stays yours.

## Settings
In-game: **SkyrimNet → Settings → Plugins → MundaneMatters**. Every feature can be switched on or off.

| Setting | Default | What it does |
|---|---|---|
| Trade / Inn Rooms / Training | on | Services from natural speech |
| Promises | on | NPCs hand over the items, gold and beds they promise |
| Gifts / Confirm Gifts / Gift Menu | on | You give things to NPCs, with a confirm popup and an inventory picker |
| Gifts Improve Relationship | on | Rare, capped relationship gain from a really good gift |
| Permission To Take | on | *"Go ahead, take it"* in homes |
| Shared Meals | on | Eat together with friends |
| Follow / Escort / Local Escorts Only | on | Come along, lead the way, no cross-hold trips by locals |
| Confirm Follow & Escort / Confirm Paid Services | on | Accept/Decline popups |
| Popup Seconds | 15 | How long a paid-service popup waits (gift popups never time out) |
| Max Promised Gold / Max Promised Item Value | 200 / 100 | Caps on what NPCs can hand over |
| Bed Loan Hours | 24 | How long a lent bed stays yours |
| Intent Confidence | 0.6 | How sure the classifier must be before a service opens |
| Debug Notifications | on | On-screen notes for every decision (turn off for normal play) |
| Run Self-Test | off | Sends ~40 sample exchanges to your classifier model and logs the score |

## Troubleshooting
- **Log file:** `Documents\My Games\Skyrim Special Edition\SKSE\MundaneMatters.log`. Please attach it to bug reports.
- **Nothing happens:** check that the log says *"game ready"*, and that the NPC actually **agreed** in their reply (a refusal does nothing, by design).
- **Follow or escort does nothing:** SeverActions isn't installed, or the destination is in another hold and *Local Escorts Only* is on.
- **Is my model good enough?** Turn on **Run Self-Test** in game, wait for the score notification, then turn it off again.

---

## How it works
1. SkyrimNet reports what the player said and how the NPC replied.
2. A cheap **gate** in C++ skips exchanges that can't contain a service, promise, gift or meal (small talk costs nothing).
3. Otherwise, **one** small prompt (`mundanematters_turn.prompt`) asks the classifier model for a JSON verdict:
   what was requested, how the NPC answered, what they promised, and what the player handed over.
4. The C++ side checks that against the real game, including ownership, inventories, prices, relationship rank, location and the generosity budget,
   then runs it with vanilla game functions: barter or training menus, inn rental, item and gold transfers, bed ownership and furniture.
5. Open promises, lent beds, guest rights, budgets and permissions are saved in an SKSE co-save.
6. A bio decorator (`mundanematters_bio`) tells each NPC the true facts, such as their room price and what they owe you, so their lines match what will happen.

## Building from source
**Needs:** Visual Studio 2022 (C++ desktop workload), CMake 3.21+, Ninja, and vcpkg (the copy bundled with VS works).

```powershell
git clone --recursive https://github.com/AinsteinDev/MundaneMatters.git
cd MundaneMatters
.\build.ps1          # configures + builds MundaneMatters.dll into data\SKSE\Plugins
.\tools\package.ps1  # makes dist\MundaneMatters-vX.Y.Z.zip
```

`deploy.ps1` mirrors `data\` into an MO2 mod folder for quick testing; edit the path inside it first.
`tools/logic_check.py` mirrors the deterministic C++ logic in Python, so rule changes can be checked without starting the game.

| Path | What's there |
|---|---|
| `src/` | The SKSE plugin (CommonLibSSE-NG, C++23) |
| `data/SKSE/Plugins/SkyrimNet/external/mundanematters.core/` | SkyrimNet plugin: prompts, bio submodule, settings schema |
| `data/PrismaUI/views/MundaneMatters/` | The popup (HTML) |
| `include/` | SkyrimNet public API and PrismaUI API headers |
| `tools/` | Logic check, packaging |

## Contributing
Bug reports and ideas are welcome; use the issue templates. For bugs, please include your `MundaneMatters.log` and the exact lines you and the NPC said.

## Credits
- **SkyrimNet** and its team: the platform this builds on.
- **SeverActions**: follow and travel, whose author kindly allows building on it.
- Made by **Ainstein**. This is my first mod, made with help from Claude.

## License
[MIT](LICENSE). Third-party headers in `include/` keep their own terms.
