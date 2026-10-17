# Changelog

## [0.2.1] - 2026-10-17

### Added
- **Welcome popup** on the first load after installing or updating: where to pick the AI model, where to switch features off, setting the Promises Book key, and where to report bugs.
- **Tips** for easy-to-miss features, starting with your bedroll (move it or pack it by talking), with "Don't show again".

### Fixed
- NPCs are named the way SkyrimNet names them (a generic "Vampire Fledgling" is "Tinimanwe") in notices, narration and memories.
- A repeated "let's go" right after someone agreed to follow no longer counts as a second follow.
- The popup now keeps line breaks in longer texts.
- A pay promise only carries an amount you actually said ("I'll give you some more gold once I get paid" no longer becomes "pay back 50 gold"), and one that waits on a payday or a job has no deadline.

## [0.2.0] - 2026-10-17

First public release.

### Added
- **Shops, inns and trainers by talking:** barter, real price quotes, buying and selling with haggling, inn rooms, training.
- **Their promises, kept:** items, gold, beds, keys and permission to take, now or later, within a generosity budget; promises can be cancelled.
- **Meetings:** an NPC who firmly agrees to meet you at a place and time really goes there (with IntelEngine installed); invented places map to the nearest real one.
- **A place to stay:** lent beds, standing guest rights, who sleeps where in a one-bed home (bedrolls), moving or packing your bedroll by talking, the host going to bed when you do.
- **Your promises:** tracked with fair deadlines, kept or broken, favours NPCs ask as you part, and the two-page Promises Book.
- **Gifts:** named gifts move once accepted (optional popup); an unnamed gift opens a picker first; a rare, capped warm-up from a really good gift.
- **Borrow and lend**, and **hold my things** (carcasses from hunting mods included).
- **Company:** follow and escort (SeverActions), going home together, spending time together, being dismissed in plain words.
- **Shared moments:** sitting together (seat, bed edge or floor where it fits), companions sitting when you sit, meals, drinks, songs and humming, resting and camping, care (healing, cooking, gear, cures), moving in.
- **Trust and bonds:** relationships grow through deeds and break through theft, cruelty and violence; stray hits in a shared fight are forgiven.
- **Guards:** fines, bribes, persuasion and threats in plain words, decided by Speech.
- **Vanilla dialogue by talking:** real dialogue options in your own words, with the original recorded lines.
- **Bio decorator:** NPCs know their real prices, promises, stays and debts.
- Default classifier model: DeepSeek V4 Flash with Gemma 4 31B as fallback, temperature 0.70.
