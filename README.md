<div align="center">

# Mundane Matters

**NPCs do what they say.**

A roleplay add-on for **SkyrimNet** for Skyrim that lets the everyday, *mundane* things you talk about actually happen.
Ask for a room, a drink, a seat by the fire, a place to stay. Promise to bring someone mead. Let a someone you are hunting with carry your kill.
If they agree in the conversation, the game makes it real.

And because it's real, the people you meet stop being shopkeepers and quest-givers: they become people you *know*.
You share their table, sit by their fire, keep your word to them, and one day they make room for you at their hearth.

![Version](https://img.shields.io/badge/version-0.2.1-blue)
![SkyrimNet](https://img.shields.io/badge/SkyrimNet-beta%2025%2B-8a2be2)
![License](https://img.shields.io/badge/license-MIT-green)

**[⬇ Download the latest release](https://github.com/AinsteinDev/MundaneMatters/releases)**

</div>

---

## What it is

SkyrimNet NPCs talk beautifully, but what they say rarely happens. The innkeeper says *"here, have some stew"* and you're still hungry.
The hunter says *"stay as long as you need"*, and her bed is still **Owned**. Your friend says *"Have a drink with me"* and keeps standing their staring.

**Mundane Matters closes that gap for the small, everyday parts of life.** It reads each exchange (what you said and how the NPC
answered) by meaning, not keywords, and then does what was agreed with real items, real prices, real beds and real furniture.
Nothing happens unless the NPC actually agrees, and a "no" stays a no.

It's a roleplay mod first: no quests, no new NPCs, no menus to learn. You just talk, the way you already do with SkyrimNet,
and the world quietly keeps up.

> On my way to Riften I met **Sylgja**. She asked me to deliver a letter to her family, so I went back and did it. When I returned she let me
> stay the night and offered me a meal. In plain SkyrimNet that meal only existed in the dialogue. I play with Survival, where a meal you can
> barely afford matters. **With Mundane Matters, she actually hands it to me.**
>
> Out hunting with **Ronja** (a modded NPC in Whiterun), I mentioned the inn costs me a hundred a night. She offered me a corner by her
> hearth. That evening she made up a bedroll for me on her floor and kept her own bed.

> ⚠️ **Made and tested by one person.** Mundane Matters is a solo project, tested only on my own game and mod list.
> It will have rough edges on setups I haven't seen, so please [report what you find](#reporting-bugs).

---

## Features

Every feature can be switched on or off in the settings. The examples are things you can type or say; the NPC answers in their own words.

### 🛒 Shops, inns and trainers
| You | What happens |
|---|---|
| *"Show me what you've got."* | The barter menu opens |
| *"How much for an apple?"* | They quote their **real** price from their **real** stock |
| *"I'll take three."* / *"Want these wolf pelts?"* | You buy or sell by talking, haggling included (*"give me 2 for 5 gold?"*) |
| *"Got a bed for the night?"* | A popup shows the real room price; the room is yours if you take it |
| *"Can you teach me archery?"* | The training menu opens |

### 🤝 Their promises, kept
| The NPC says | What happens |
|---|---|
| *"Here, take 10 coins for your trouble."* | You get 10 gold |
| *"Have a bowl of stew."* | You get the stew, from their own pack if they have it |
| *"Come back tomorrow, I'll have your reward."* | They hand it over when you next meet |
| *"Go ahead, take the wine."* | Taking it from their home isn't theft |
| *"Here's my spare key."* (someone close) | A key to their home |
| *"Dawn. The east ridge. Don't be late."* | A real meeting: with **IntelEngine** installed they actually go there (an invented spot becomes the nearest real place) |

They aren't a free buffet: each NPC has a generosity budget based on how well they know you, and asking too often gets a polite no.

### 🛏️ A place to stay
- *"You can sleep in the spare bed tonight."* The bed is yours for the stay, so sleeping in it isn't a crime.
- *"Stay as long as you need."* / *"You're always welcome here."* You become a standing guest, with a place to sleep on every visit.
- **Only one bed in the house?** What's said decides who sleeps where: *"the floor's yours, there's a spare bedroll"*, *"take my bed, I'll sleep by the fire"*, or sharing.
- *"I'll move my bedroll here."* / *"\*I pack my bedroll\*"* Your bedroll moves to your feet, or is packed away.
- When you lie down, your host goes to bed too. When a stay runs out, you're told.

### 📜 Your promises, remembered
- *"I'll bring you a bottle of mead tomorrow."* It's written down with a fair deadline. Keep it and they're glad; break it and they remember.
- *"I'll be back by dusk."* It only counts as kept once you've really been away and come back.
- People you know sometimes ask a small favour as you part (*"if you're passing, bring me an apple pie?"*). Say yes and it's a promise.
- **The Promises Book** (a hotkey, or ask *"what did I promise?"*): their promises on the left page, yours on the right, recent ones over the page.

### 🎁 Gifts, loans and keeping things
- *"Here, have a mead."* It moves from your pack to theirs, and they react in their own words.
- *"I got this for you."* (nothing named) A picker opens first, and they react to what you really gave.
- *"Can I borrow your pickaxe?"* / *"Use my spare bow."* Borrow and lend everyday things, and give them back.
- *"Hold this for me."* A companion or friend carries your things (a carcass from a hunting mod too) and gives them back when asked.
- A really good gift can, rarely, warm someone up a little. You can't buy friendship.

### 🚶 Company
- *"Follow me."* / *"Take me to the brewery."* They come along, or lead the way (they know you mean Honningbrew Meadery).
- *"Let's go to your place and relax."* You walk to their home together and sit down.
- *"Stay with me a while."* They spend time with you, walking along and sitting when you sit, then go back to their own day.
- *"You can do whatever you like."* / *"Wait here."* They stop following.
- A stray hit in a shared fight is forgiven; it doesn't turn your companion against you.

### 🔥 Shared moments
The small things are how you get to know someone, so they're the heart of the mod.

- **A quiet evening.** *"Let's sit by the fire."* They settle into a chair, the edge of a bed, or the floor if it suits the place,
  and you sit down beside them. Pass an hour talking, or just watch the flames.
- **Company that feels like company.** Sit down after a long road and whoever travels with you sits too: a chair nearby if there's one,
  the grass or the floor out in the wilds or at home, never a tavern floor.
- **Their table, their mead.** *"Let's eat together."* / *"A round on me?"* You share a meal at their table, or a drink at the inn.
- **A song for you.** *"Play us something!"* A bard picks up the lute; a friend just hums a tune for you, half under their breath.
- **Nights on the road.** *"Let's make camp."* Your companions lay out bedrolls and bed down beside you.
- **Looked after.** *"I'm hurt."* / *"Can you cook this venison?"* / *"Can you sharpen my sword?"* A friend patches you up, cooks your catch,
  works on your gear; a priest cures what ails you.
- **A home together.** *"Come live with me."* Someone close moves into your house, with a bed of their own.

### ⚖️ Trust, bonds and guards
- **Bonds:** a stranger becomes an acquaintance, a friend, someone you'd trust with your life, through what you do together:
  meals shared, help given, promises kept, evenings spent. Pretty words alone won't do it.
- **Trust:** steal from them, insult them or hit them out of nowhere, and they take back what they gave you until you make amends.
- **Guards:** with a bounty, *"I'll pay the fine"*, a bribe, persuasion or a threat, decided by your real Speech skill, not the chat.
- **Vanilla dialogue in your own words:** say what one of the NPC's real dialogue options says, and they answer with the original recorded line, quest scripts and all.

### 🧠 NPCs know the facts
Merchants quote real prices, innkeepers know their room cost, and every NPC knows what they've promised you, what you owe them
and who's staying where, so what they say matches what will happen.

---

## Built to play nice
- **Typos, slang and roleplay are fine.** *"folow me"*, *"tkae me to ur place"* and *"\*waving, I walk out the door\*"* all work.
- **A no is a no.** If the NPC refuses, hesitates or only says *"maybe"*, nothing happens.
- **Popups are your character's lines.** Paid services and quest lines ask first; gifts just happen once accepted (a popup is optional).
- **Works alongside SeverActions** without double menus, double payments or double follows.
- **Cheap to run.** Small talk is skipped; an exchange that might matter costs one small AI call.
- **No ESP and no save bloat.** Everything lives in a small SKSE co-save.

---

## Requirements
- **SkyrimNet** beta 25 or newer, with its own requirements (SKSE, Address Library, **PrismaUI**)
- **SeverActions** (follow, escort, travel and furniture)

**Optional:**
- **IntelEngine**: NPCs really turn up to the meetings they agree to.
- A hunting mod that turns carcasses into items (e.g. Carry Your Carcasses) works with *"hold this for me"*.

Tested on Skyrim AE 1.6.1170. I play it with the **Gate to Sovngarde (GTS)** mod list, so it works great with that.

## Installation
1. Download `MundaneMatters-v0.2.1.zip` from [Releases](https://github.com/AinsteinDev/MundaneMatters/releases).
2. Install it with **Mod Organizer 2** or **Vortex**, or extract it into your `Data` folder.
3. Start the game. **Mundane Matters** appears in SkyrimNet's Plugins list.

**The AI model.** Mundane Matters makes one small JSON call after each exchange that might matter. By default it uses
**DeepSeek V4 Flash** with **Gemma 4 31B** as the fallback, at temperature **0.70**, through OpenRouter. You can change it under
*SkyrimNet → Plugins → MundaneMatters → AI Model*, or clear it to use your SkyrimNet base model. A fast, cheap model that writes clean JSON works best.

**Uninstalling:** there's no ESP, so you can remove it any time. End any follows and stays first: a bed lent to you at that moment stays yours.

## Settings
In game: **SkyrimNet → Settings → Plugins → MundaneMatters**.

| Group | Default | What it covers |
|---|---|---|
| Shops & Services | on | Barter, prices, buying and selling by talk, inn rooms, training |
| Guards: Talk Your Way Out | on | Fines, bribes, persuasion, threats |
| Follow & Escort | on | Coming along and leading the way |
| Gifts | on | Gifts, the gift picker, permission to take |
| Their Promises / Your Promises & Favours | on | Promises both ways, favours, the Promises Book |
| Borrow & Lend | on | Loans both ways |
| Shared Moments | on | Meals, drinks, sitting, songs, resting, little gifts, gestures |
| Care | on | Healing, cooking, gear, cures |
| Hold & Keys | on | Keeping your things, house keys |
| Trust & Bonds | on | Relationships that grow and break through deeds |
| Confirm Popups | on | Accept/Decline before paying, following and handing things over |
| Confirm Gifts | **off** | A popup before an accepted gift leaves your pack |
| Confirm Quest Lines | on | A popup before a quest dialogue line is said for you |
| Sit Down With You | on | Your companions sit when you sit |
| Vanilla Dialogue by Talking | on | Real dialogue options in your own words |
| Debug Notifications | on | On-screen notes for every decision (turn off once you trust it) |

Tuning values (popup time, gift and gold caps, bed and company hours, the Promises Book key) are in the same menu.
**Run Self-Test** is for the developer only; leave it off (it sends 100+ paid AI calls).

## Troubleshooting
- **Log file:** `Documents\My Games\Skyrim Special Edition\SKSE\MundaneMatters.log`. Please attach it to bug reports.
- **Nothing happened:** check that the NPC really agreed in their reply. The log says what was understood and why something did or didn't happen.
- **Follow or escort does nothing:** SeverActions isn't installed, or it's a long trip to another hold (locals won't walk you across Skyrim).
- **A meeting didn't happen:** IntelEngine isn't installed, or the NPC never firmly committed to a time.

---

## How it works
1. SkyrimNet reports what you said and how the NPC replied.
2. A cheap C++ gate skips small talk, at no cost.
3. Otherwise, one small prompt asks the classifier model for a JSON verdict: what was asked, how they answered, what was promised and what changed hands.
4. The C++ side checks that against the real game (ownership, inventories, prices, relationship, location, the generosity budget) and carries it out with vanilla game functions.
5. Promises, stays, guest rights, budgets and permissions are kept in an SKSE co-save.
6. A bio decorator tells each NPC the true facts, so their lines match what will happen.

## Building from source
**Needs:** Visual Studio 2022 (C++ desktop workload), CMake 3.21+, Ninja and vcpkg (the copy bundled with VS works).

```powershell
git clone --recursive https://github.com/AinsteinDev/MundaneMatters.git
cd MundaneMatters
.\build.ps1          # builds MundaneMatters.dll into data\SKSE\Plugins
.\tools\package.ps1  # makes dist\MundaneMatters-vX.Y.Z.zip
```

`deploy.ps1` mirrors `data\` into an MO2 mod folder for testing (edit the path inside it first).
`tools/logic_check.py` mirrors the deterministic C++ rules in Python, so they can be checked without the game.

| Path | What's there |
|---|---|
| `src/` | The SKSE plugin (CommonLibSSE-NG, C++23) |
| `data/SKSE/Plugins/SkyrimNet/external/mundanematters.core/` | SkyrimNet plugin: prompts, bio submodule, settings |
| `data/PrismaUI/views/MundaneMatters/` | The popup |
| `include/` | SkyrimNet and PrismaUI API headers |
| `tools/` | Logic check, packaging |

## Reporting bugs
I'm a solo developer and the only tester so far, so every report helps.

- **Where:** post in the **Mundane Matters channel on the SkyrimNet Discord**, or DM me there.
- **What to include:**
  - what you said and what the NPC answered (the exact lines if you can);
  - what you expected, and what happened instead;
  - your `MundaneMatters.log` (`Documents\My Games\Skyrim Special Edition\SKSE\`). Not comfortable sharing the whole log? Just paste the few lines
    around the moment it went wrong; every decision is logged with the time and the NPC's name.
- Anything on screen starting with **`MM_`** came from Mundane Matters, so a screenshot of that helps too.

Ideas are welcome in the same places.

## Roadmap
Things I'm working on or planning, not promises:

- **Hunting (in progress):** a companion who says *"I've got this shot"* really shoots the animal you point at, and *"dress the kill"* /
  *"carry it home"* actually happens. Still being built and tested.
- **Fishing (next):** fishing trips together, by talking: *"let's go fishing at the river"*, a companion who knows the good spots, and the catch shared or cooked after.

## Credits
- **SkyrimNet** and its team: the platform this builds on.
- **SeverActions**: follow, travel and furniture, whose author kindly allows building on it.
- **IntelEngine**: scheduling, used for meetings when it's installed.
- Made by **Ainstein**. This is my first mod, made with help from Claude.

## License
[MIT](LICENSE). Third-party headers in `include/` keep their own terms.
