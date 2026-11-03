# Fork block 4 (blocks 3, 4, 4b-4d on top of v0.58.1)

This branch (`fork-block4`) continues v0.58.1 (wire protocol 165, 2-8 players).
Priority: **keep characters and inventories intact first, world state second.**
Everything here was written and tested by one person with an AI assistant; treat
it as experimental. Source of truth is the code; this page is a summary.

## What changed

- **Inventory / loot**
  - Nested bags (bag inside a bag) are created and emptied correctly on copies
    (`KENSHICOOP_NESTED_EMPTY`, default on).
  - Transfers out of a backpack; item condition sync; census holds dropped for
    gains still pending.
  - Corpse looting: refusal protection (#40) kept; new `[xfer] MOVE0` diagnostic
    line says why a transfer moved nothing. The root cause of the refusals seen in
    a real session is **not found yet**.
- **Death / KO**: real death applied on copies, KO latch self-heal, copies cannot
  KO themselves.
- **Traders**: traders are paired across machines by template/faction/position.
- **Squads (block 4d)**: when a unit is moved between squad tabs and control flips
  to the other machine, the flip is now announced (`flipWire_`), which should stop
  the duplicated unit seen after moving a bought animal to another squad.
  **Untested in a live session.**
- **Corpse census hold (4d)**: a corpse that briefly drops out of the 1 Hz census
  is kept for `KENSHICOOP_CORPSE_HOLD_MS` (default 15000) instead of being
  forgotten at once. Aimed at "item next to the looted one disappears".
  **Untested live.**
- **Beds (4d)**: if a copy stays in a bed while its owner is moving, the fast-exit
  escalates after 4 attempts (explicit furniture exit + halt). **Unverified.**
- **Steam lobby status (4d)**: the join no longer stays on "Connecting to host..."
  once the session is up.
- **Logging (4c)**: `KENSHICOOP_*` keys in `coop_config.json` are now exported to
  the environment (so debug switches can be set from the file); `[inv] RECV-CORPSE`
  lines.

## Test evidence (honest)

- Local two-copy lab, block4d Harness build: regression battery of 15 scenarios
  -> 13 PASS. `inv_nested_bag` and `inv_join_bag` FAIL inside the battery because
  of the battery's save/dump settings (known); run with `Run-Block4Bags.ps1`
  `inv_join_bag` passes (nested-bag was not re-checked separately for 4d).
  `squad_sync` and `recruit_join` PASS.
- `corpse_loot` is **flaky** (fails in some runs, also with the nested-empty fix
  disabled). Not fixed; the census hold may reduce it, not measured.
- 4 game windows on one PC (LAN/UDP transport): cross-owner transfers worked in
  all four windows, no body-state mismatches; world divergence was measured and
  stayed small but not zero.
- Steam: one real session on two different Steam accounts, over an hour, no
  disconnects (blocks 4b/4c builds). Single-account loopback cannot validate Steam.
  The 4d changes have **not** been run over Steam yet.

## Problems seen in the real Steam sessions (not fixed)

- Loot refusals: the second player's take returns `moved=0` although corpse
  hashes match; likely dual authorship of the same corpse by both censuses.
- Weather (e.g. acid rain) is not synchronised; needs a protocol bump (166).
- Built furniture (a placed bedroll): the furniture path has no key translation
  for session-built objects, so a joiner may appear standing on the host's bed.
- NPCs can freeze for long periods in the census (`FREEZE`).
- A rollback loop on gamedata 1913 (an AI character taking wheat from storage).
- A harmless crash while the game is shutting down.
- Pre-existing items in `KNOWN_BUGS.md` still apply.

## Build

Same as upstream: see `BUILD_SETUP.md`. Release DLL SHA256 for the tagged build is
in the release notes.
