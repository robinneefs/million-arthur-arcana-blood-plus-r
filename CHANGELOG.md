# Changelog

Versions follow major.minor.patch: the middle number goes up for new features, the last one for fixes.
"build" is the internal test build number (also in `src/mod/version.h`).

## 0.1.0 beta (build 106, not released yet)

First version with a version number.

- The game boots into our own main menu (the game's title screen and menus are skipped): Room search, Create room, Settings, Help, Quit, and a My setup card.
- My setup: character, colour, Support Knights presets and stage, remembered between sessions.
- Settings:
  - Controls: remap the game's actions (Weak, Medium, Strong, Elemental Blade, Roll, Throw, Burst, Enchant Boost) for controller and keyboard, vibration, and a button check that shows what the game reads.
  - Sound: the game's music, sound effect and voice volumes, our menu sounds and menu music.
  - Display: the game's resolution, display mode and V-Sync (used after a restart).
  - Online: your Steam name, input delay, connection info in a match.
  - Logs: Pack logs (one zip on the Desktop, private details hidden) and Open folder.
- Help: playing online, connection trouble, reporting a problem, links (Steam page, Mizuumi wiki, Million Arthur Discord, this repository) and credits.
- The version is shown on the main menu, in Help and in the launcher; online, both players must run the same build.
- The window title is the game's title with "+R".
- A first version of rooms (Steam lobbies: create, search, join, invite), not yet tested between two players. Matches are still started from the F1 overlay.
- Test keys F4-F11 only work with `[dev] tools=1` in `rollback.ini`.

## Before 0.1.0 (builds 1-89)

Rollback engine, online play over Steam, the F1 match setup overlay and booting straight into the fight. See the git history.
