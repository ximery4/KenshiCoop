# Fork blocks 3-4d (on top of v0.58.1)

This branch (`fork-block4`) continues v0.58.1 (wire protocol 165, 2-8 players).
Priority: **keep characters and inventories intact first, world state second.**
Everything here was written and tested by one person with an AI assistant; treat
it as experimental. The code is the source of truth; this page is a summary.

> **Corrected 2026-11-03.** An earlier revision of this page overstated the evidence
> and named the wrong cause for several problems. The corrections are listed at the
> end ("What was wrong in the first revision"). **Block 4d does NOT fix item loss when
> looting corpses** - see "Known problems".

## What changed in blocks 3-4d

- **Inventory**: nested bags (bag inside a bag) are created and emptied correctly on
  copies (`KENSHICOOP_NESTED_EMPTY`, default on); transfers out of a backpack; item
  condition sync; census holds dropped for gains still pending.
- **Death / KO**: real death applied on copies, KO latch self-heal, copies cannot KO
  themselves, wound values below zero are written to copies.
- **Traders**: traders are paired across machines by template/faction/position.
- **Squads (4d)**: when a unit is moved between squad tabs and control flips to the
  other machine, the flip is announced (`flipWire_`). Tested in the two-copy lab
  (`squad_sync`, `recruit_join` PASS); **not verified in a live session**, and the lab
  case has matching serials while the live case did not.
- **Corpse census hold (4d)**: a corpse that briefly drops out of the 1 Hz census is
  kept for `KENSHICOOP_CORPSE_HOLD_MS` (default 15000). It does not address the item
  loss described below.
- **Beds (4d)**: after repeated failed exits the fast-exit escalates (explicit exit +
  halt). Unverified; see below for the likely real cause.
- **Steam lobby status (4d)**: the join no longer stays on "Connecting to host...".
- **Logging (4c/4d)**: `KENSHICOOP_*` keys in `coop_config.json` are exported to the
  environment; `[inv] RECV-CORPSE` and `[xfer] MOVE0` diagnostic lines.

## Test evidence

- Local two-copy lab, block 4d Harness build: 15-scenario regression battery, 13 PASS.
  The two FAILs (`inv_nested_bag`, `inv_join_bag`) were a **setup error in the battery
  script** (wrong save / missing `KENSHICOOP_INV_DUMP`); run with the right setup both
  PASS (`reports/block4_bags.txt`). `squad_sync` and `recruit_join` PASS.
- `corpse_loot` is **flaky** in the lab (also with the nested-empty fix disabled).
- 4 game windows on one PC (LAN/UDP transport, bots): cross-owner transfers worked in
  all four windows, body state and inventories matched, **but the position check
  failed**: about 5% of character comparisons were outside the 120-unit tolerance
  (mean error 26-39 units, worst case 4791), 23 persistent differences against a limit
  of 2. NPC positions stayed within tolerance but only about 10 NPCs were compared.
- Steam: two real sessions on two different Steam accounts (54 and 20 minutes, with a
  save/reload between them), no disconnects, RTT 5-8 ms. Single-account loopback
  cannot validate Steam. Block 4d itself has **not** been run over Steam.

## Known problems (found in the Steam sessions, analysis in the maintainer's notes)

- **Item loss when looting corpses (not fixed in 4d).** Reconstructed from both logs:
  the owner's inventory snapshot reaches the other machine before the transfer intent;
  that machine's copy of the character is then full, so the engine refuses the add
  (`[mk] tryAddItem-fail`), the receiver answers "moved 0", the author rolls the loot
  back out of its own character, and the other author of the same corpse then
  broadcasts the corpse without those items. 28 units were lost in the first session
  (38 intents, 26 incomplete). Rollback also removes by item template, not by instance,
  so an identical item already in the bag can be removed.
- **Stale key after a squad move.** A moved character gets a different key on the two
  machines; inventory snapshots and transfers kept using the wire key, so one
  character's inventory stopped updating on the join for 50 minutes (613 empty applies)
  and six production pickups were refused.
- **A duplicate pet entered the save.** The host created a stand-in in a new squad tab
  (host fallback) and saved it; after loading, both sides had the extra tab.
- **Beds.** One character stayed in a bed on the join while the owner ran around
  (1174 fast-exit calls, none succeeded). The owner never sent a bed-enter event, so
  the first guess (missing key translation for a built bedroll) is probably wrong; the
  join's own AI laying the copy down again is the likelier cause. Unverified.
- Weather (e.g. acid rain) is not synchronised; needs a protocol bump (166).
- NPCs outside the stream are simulated independently on both machines and reconciled
  by teleport/freeze; a join had one NPC frozen for most of a 52-minute session.
- A harmless crash while the game is shutting down.
- Pre-existing items in `KNOWN_BUGS.md` still apply.

## What was wrong in the first revision of this page

| Earlier claim | Correction |
|---|---|
| Loot refusals: root cause unknown, likely dual corpse authorship | Two causes found: the copy refuses the add, and a stale key after squad moves. Dual authorship turns the refusal into a permanent loss |
| Real Steam session "over an hour" | Two sessions, 54 and 20 minutes |
| 4-window test: divergence "small" / "no mismatches" | Bodies and inventories matched; positions did not (see above) |
| Rollback loop on gamedata 1913 in the real sessions | It appeared only in bot runs, not in the Steam logs |
| `inv_nested_bag` not re-checked | PASS on 4d with the correct battery setup |
| Corpse hold addresses "item next to the looted one disappears" | It does not; see Known problems |
| Bedroll key translation is the bed bug | Probably not (no enter event was sent) |

## Build

Same as upstream: see `BUILD_SETUP.md`. Release DLL SHA256 for each tag is in the
release notes.
