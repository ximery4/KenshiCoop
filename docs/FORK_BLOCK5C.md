# Fork block5c: working mechanisms

Written by the AI assistant on behalf of the account owner (ximery4). Numbers come from the logs of one live session
(2026-11-04, 21:56-23:12, two PCs over Steam, host `Ximery4`, client `Ame_Ryoki`, Kenshi 1.0.65, RE_Kenshi, 16 mods) and from the
harness regression on two local copies. Nothing here is a claim about other setups. Source of this version: commit
`efa556c` of the fork line (protocol 165, KenshiCoop 0.58.1 base, Release build SHA256
`372E88FB54904F0705D575486D013B0C518EFF2949FBFF7F6F349B9704B800AD`).

## What this version is

The 0.58.1 code with the fork's host-arbitrated item transfer, census, building and production fixes up to "block5b", plus a
**read-only** second book of items ("core2 shadow", `src/core2`, `src/plugin/sync/Core2Shadow.cpp`). The shadow book only reads
the game and writes log lines; it changes nothing in the game. It is enabled with two keys in `coop_config.json`:
`"KENSHICOOP_CORE2_SHADOW": "1"` and `"KENSHICOOP_CORE2_NET": "1"` (the second sends the host's book to the client, which compares it
with its own game).

## Mechanisms that worked in the live session

1. **Steam lobby and tunnel.** Friends-only lobby with rich presence (`+connect_lobby`), client admitted when the host is ready, ENet
   over the Steam P2P tunnel. Client connected 23 s after joining the lobby, world live 58 s later, no disconnect in 76 minutes.
   A Steam client in family mode on either side hid the lobby (the friend shows `gameID=0`); turning it off fixed it.
2. **Item transfers with an answer.** A take or move between two owners is sent as an intent; the owner of the source answers
   `accept`, `partial` or `reject`; the sender rolls its optimistic copy back through the owner's snapshots (`latch`), logs `ROLLBACK`
   for units that landed in its own bag, and treats an already-landed unit as accepted (`ALREADY-LANDED`). In the session: host log
   11 accept and 1 partial; client log 24 accept and 24 reject (the rejects are listed under the problems below).
3. **Inventory snapshots.** Content hash per container, settle windows (350 ms for additions, 1800 ms for removals), safety resend every
   5 s (30 s for big containers), additive-only apply when the author's list was truncated, and one auto-arrange retry when a copy
   has no free block in its grid.
4. **NPC census and presence.** Host census rows, copies adopted by template and distance, divergence freeze/park of copies,
   owner-authoritative death (a copy that dies locally while the owner reports it alive is revived, `[death] veto`) and a reliable
   death/knock-out latch.
5. **Medical, stats, sneak, time.** Per-part health and blood are copied to the other side; world time converges within the first 30 s
   (0.29 game hours off at 21:57:22, within +-0.003 afterwards, 806 samples).
6. **Buildings and production.** A building placed in the session is created on the peer, construction progress and production output
   (`[prod] SEND`) are streamed, deeds are audited. The client built a quarry and a stone-processing bench and the host saw their
   progress and output.
7. **World items.** Drops are captured, sent, claimed and culled (`[wi] DROP-CAP / SEND / CLAIM-APPLY / CULL`).
8. **Core2 shadow book over the real link.** 2.07 MB for 76 minutes (about 27 B/s), 36 snapshots, `bad=0`; the client's
   `[c2net] CHECK` compares per container (452 checks, 5-11 containers differing at a time: about a third only in how stacks are split, the rest as listed below).

## Problems seen in the same session (not fixed in this version)

- A copy of a building gets no owner faction: the host shows "no faction", cannot open its production window, and its transfers
  from that building are refused (`sAuth=-1`).
- A refused transfer did not put the units back into the source copy on the side that took them (a mine showed 1 unit less on the
  client for 75 minutes while the host held it).
- A character copy whose grid has no free block refuses a unit the author held in a bag (`tryAddItem-fail`, 17 on the host).
- Enemy copies are killed locally by the other player's fight and revived by the veto 9195 times in 20 minutes on the client;
  enemies visibly jump (155 `[snap]` events, 14 over 150 u; interpolation `delay=94 ms jit=8.4`).
- A copy shows "unconscious (8)" frozen: its knock-out timer is topped up on purpose, so only the owner's window counts down.
- Buildings loaded from a save have no shared name and are not synchronised (only buildings placed in the session are).
- The proxy telemetry (`SCENARIO PROXY`) was 48% of both 90 MB logs.
- Exit through the menu crashed on both PCs (`0xc0000005`, in `steam_api64.dll` on the client; an unmapped call target on the host),
  after the save had been written.

## Harness regression of this line

Two local copies (UDP 127.0.0.1), scenarios from `scripts/scenarios.psd1`; see `docs/KNOWN_BUGS.md` for failures that were already
present. `store_sync` fails on this line and failed on block5b as well.

## Files

`src/core2/ItemBook.*`, `Session.*` (book and session, tests in `src/core2/tests`), `src/plugin/game/EngineCore2.cpp` (reads items
for the book), `src/plugin/sync/Core2Shadow.*` (the observer).
