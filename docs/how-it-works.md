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
- **Dear ImGui** (`third_party/imgui`): the UI library, version 1.82, MIT licence.

## Frame loop and rollback

The mod hooks the game's update once per frame. Before each update it saves the memory that holds the fight (only the pages that changed since the last save). When the other player's input arrives late and differs from the guess, the mod restores an older frame and replays the frames up to now with the right inputs, all within one displayed frame.

## Online sessions

Each frame both PCs send their inputs for the coming frames, plus how far ahead their clock is. The PC that runs ahead stalls one frame now and then, so neither side has to roll back much further than the other.

## Match setup

The overlay does the whole match setup itself: connect, lobby and ready, controls, character, colour, assist and stage picks, and rematch. Each side sends its full lobby state until the other confirms it. When both are ready, player 1 sends the final settings with a checksum and player 2 confirms them. The game's own menus and online lobbies are not used.

## Booting into the fight

When the match setup is final, the mod takes the game from its main menu to the screen change that would normally open character select. At that point it writes both players' character, colour, assists and stage into the game's own match settings and sends the game to the loading screen instead, so the fight starts right away. During the fight the game's own "Request rematch?" box is answered for both players, and when the result is in the mod ends the fight screen and sends the game back to its main menu. There the overlay opens again with Rematch and Back to lobby. Online, only the loading screen and the fight are kept in step between the two PCs; everything before and after is handled by the overlay.

## Settings

`config/rollback.ini` lists every setting with a comment. The mod reads `rollback.ini` from its own folder.
