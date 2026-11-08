# Fork block5g: role re-apply at connect and container fixes

Written by the AI assistant on behalf of the account owner (ximery4). Numbers come from the logs of one live two-PC Steam
session (2026-11-07) and from the test harness on local game copies (one PC, 2 or 4 windows). Nothing here is a claim about other
setups. Source of this version: commit `5e67264` of the fork line (protocol 165, KenshiCoop 0.58.1 base). Release build in `dist/`:
SHA256 `1B628199A309DCA97BE9AF83BD1F87C32FF26A1FA94FE45C0B5BCB72EAE2907D`.

**Every player must use the same DLL.** Since block5f the host compares the DLL hash and the mod list at join and refuses a
different build (`KENSHICOOP_BUILD_CHECK=0` turns the check off).

## Main finding

Every player's game starts in the HOST role; the MULTIPLAYER panel switches a joining player to JOIN only when they connect.
Settings that were computed from the role at load time kept their host values on the client:

- the container and corpse census (`storeSync`) stayed ON on the client, so the client also sent full contents of storages,
  benches and corpses to the host. Effects seen in the session: the rest of a stack vanished from a corpse after one item was taken,
  and storages counted a transfer twice (9 building materials became 18; copper, stone and weapons the same way);
- damage reports for NPC fights (`reportCombat`) stayed OFF;
- trust in a matching NPC AI (`gateAuthority`) stayed OFF;
- the trader restock gate was not installed, so traders restocked on the client by themselves.

The harness did not see this because its client copies started directly in JOIN. `KENSHICOOP_TEST_ROLE_SWITCH=1` now makes a
test copy boot as HOST and switch at connect, like a real player.

## Changes since block5c

block5g (this commit range's main part); each change has an off switch:

| Change | Off switch |
|---|---|
| Role-dependent settings are re-applied at every connect (`[role] apply ...` log line) | `KENSHICOOP_ROLE_REAPPLY=0` |
| The host ignores a peer's contents for anything it censuses itself (`[inv] CENSUS-GUARD`) | `KENSHICOOP_CENSUS_GUARD=0` |
| Container and corpse census around every player character, not only squad leaders (on a large base, benches used by ordinary workers had no owner) | `KENSHICOOP_CENSUS_ALL_MEMBERS=0` |
| Storage furniture from mods (chests, barrels, safes: building class USABLE with an inventory) is synced like a storage | `KENSHICOOP_CENSUS_USABLE=0` |
| Census limit 48 -> 96 rows; diagnostics `CONT-CAP`, `CONT-QUERY saturated`, `CONT-SKIP` | `KENSHICOOP_CENSUS_DIAG=0` |
| Construction progress made by the other player's builder flows back to the player who placed the site; progress no longer jumps back (`STATE-BACK`) | `KENSHICOOP_BUILD_BACKFLOW=0` |
| Construction sites loaded from a save: the builder's pose finds them, progress and completion are sent (`BAKED-SITE`) | `KENSHICOOP_BAKED_SITES=0` |
| A transfer rollback restores at most what was removed, and nothing after `src-empty` / `src-short` (`RESTORE-CAP`) | `KENSHICOOP_RESTORE_CAP=0` |
| A remembered drag source expires after 250 ms | `KENSHICOOP_STALE_SRC_FIX=0` |
| When the save barrier gives up it lists what had not settled (`[save] UNSETTLED`) | - |

Earlier unpublished steps included here:

- **block5d:** copies of a peer's building get an owner faction; a refused or partial transfer puts the units back into the
  source copy; an item that does not fit a character copy's grid goes into a worn bag; quieter proxy/death logs.
- **block5e:** guard against the engine destroying a whole stack when a part is removed (`[inv] OVER-REMOVE`,
  `KENSHICOOP_REMOVE_GUARD=0`).
- **block5f:** DLL hash and mod list in HELLO/WELCOME (see above); after a manual save the host saves again under the same name
  once cross-owner moves have settled (`KENSHICOOP_SAVE_BARRIER=0`); transfer answers carry a reason; log rotation.
- **core2 (not used by the game yet):** `src/core2/RegionMap` - region ownership logic with headless tests; the read-only shadow
  book from block5c.
- **Harness:** `ScenarioFunc` (function tests), bot commands for machines, work orders, `collect`, `feed`, `cont`, `chests`.

## Tests of this version

| Test | Result |
|---|---|
| `corpse_loot` with the role switch: fix OFF / ON | FAIL / PASS (the session bug reproduced and fixed) |
| Regression, 19 scenarios, client boots as HOST and switches | 19/19 PASS (twice) |
| Regression, 19 scenarios, client boots directly as JOIN | 19/19 PASS |
| 4 windows on one PC (host + 3 copies), large base save with 65 characters | positions and cross-player transfers OK, no crash; one worker that hauls between benches non-stop lags 1-2 units on the host and catches up; one NPC body state mismatch |
| 3 transfers in a row into one storage, all 4 windows | +9 / -9 in every window (this setup also passes without the fixes, so it only shows nothing broke) |

## Known open problems

- A worker with automatic jobs (about 600 moves in 10 minutes) can be 1-2 units ahead or behind on the host for a moment; a host
  save at that moment can store the difference.
- The "follow" order is not shown in the active task panel; a copy of a ranged fighter sometimes runs into melee on the other
  window; dismantling a building that came from a save can lose the item; an NPC (wandering assassin) can freeze.
- Tested with up to 4 windows on one PC and 2 real PCs. Three or more real PCs over Steam have not been tested yet.
