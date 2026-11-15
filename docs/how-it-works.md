# How the mod works

This is a short overview for people (or AIs) who want to build on the mod.

## Pieces

- **Launcher** (`src/launcher/launcher.cpp`): finds the Steam install of the game, starts it, and loads `arcana_rollback.dll` into it. It never changes files in the game folder.
- **Mod DLL** (`src/mod/`): runs inside the game.
  - `mod.cpp`: start-up, hooks on the game's frame loop and input, saving and restoring the game state (rollback), sync and rollback tests, hotkeys.
  - `addr.h`: the game addresses the mod uses, and the original bytes it expects at each patch site, so it refuses to run on a different game version.
  - `net_session.inc`: online sessions (connect, input exchange, rollback timing, clock sync).
  - `transport.cpp` / `transport.h`: the network layer (loopback stand-in, home network UDP, Steam).
  - `lobby.inc`: the match setup protocol between the two players (lobby, ready, picks, start, rematch).
  - `boot.inc`: starts the fight from the overlay's settings and brings both players back to the overlay afterwards.
  - `overlay.inc`: the in-game overlay (Dear ImGui on the game's Direct3D 9 device).
  - `ui_theme.h`: every colour, font and size of the overlay, in one file.
  - `util.cpp` / `util.h`: logging, ini reading, small helpers.
  - `memlist_builtin.h`: the memory list (see below), built into the mod.
  - `perf.inc`, `heapdiag.inc`, `crashdiag.inc`, `vmfree.inc`: timing, heap and crash reports for the log.
- **Dear ImGui** (`third_party/imgui`): the UI library, version 1.82, MIT licence.

## Frame loop and rollback

The mod hooks the game's update once per frame. Before each update it saves the memory that holds the fight (only the pages that changed since the last save). When the other player's input arrives late and differs from the guess, the mod restores an older frame and replays the frames up to now with the right inputs, all within one displayed frame.

## What is in the saved state

The mod saves the player structs, the game's fight data and its object pools from the first frame of each fight. On top of that a memory watch marks the rest of the game's writable memory read-only at fight start, and any page the fight logic writes for the first time joins the saved state on the spot. Pages that showed up in earlier test fights (all characters and stages) are listed in `memlist_builtin.h` and are in the state from the start. `rollback_memlist.txt` next to the DLL collects anything new a fight finds.

Lessons from testing:
- Small object pools (under 64 KB) are not covered by the memory watch and must be listed in `addr.h` (`POOL_PTRS`). A missing one crashes a rollback only when that kind of object is used (for example one assist's super cut-in).
- Heap blocks between 8 and 32 MB are watched page by page, so only the written pages are saved.
- Some stages have timers that start ticking before the fight, a different number of times on each PC. Online, the mod resets them at fight start so both PCs start alike.

## Checks

- Online, both PCs exchange a fingerprint of the fight state every few frames; a mismatch is logged as a desync.
- In the test modes the mod also rewinds and replays the last frames with the final inputs and compares the result (`verify_every`). Each check replays only as many frames as fit in `verify_budget_ms`, so the test does not slow down a slower PC.

## Online sessions

Online play goes through Steam (the game's own Steam networking interface, on a channel of our own). Each frame both PCs send their inputs for the coming frames, plus how far ahead their clock is. The PC that runs ahead stalls one frame now and then, so neither side has to roll back much further than the other.

## Match setup

The overlay does the whole match setup itself: connect, lobby and ready, controls, character, colour, assist and stage picks, and rematch. Each side sends its full lobby state until the other confirms it. When both are ready, player 1 sends the final settings with a checksum and player 2 confirms them. The game's own menus and online lobbies are not used.

## Booting into the fight

When the match setup is final, the mod takes the game from its main menu to the screen change that would normally open character select. At that point it writes both players' character, colour, assists and stage into the game's own match settings and sends the game to the loading screen instead, so the fight starts right away. During the fight the game's own "Request rematch?" box is answered for both players, and when the result is in the mod ends the fight screen and sends the game back to its main menu. There the overlay opens again with Rematch and Back to lobby. Online, only the loading screen and the fight are kept in step between the two PCs; everything before and after is handled by the overlay.

## Settings

`config/rollback.ini` lists every setting with a comment. The mod reads `rollback.ini` from its own folder.

## Logs

The mod writes `rollback_log.txt` next to itself and keeps the previous session's log as `rollback_log_previous.txt`. A crash writes the crash point, the registers and the call chain to the log.
