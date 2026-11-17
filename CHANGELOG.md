# Changelog

Versions follow major.minor.patch: the middle number goes up for new features, the last one for fixes.
"build" is the internal test build number (also in `src/mod/version.h`).

## Unreleased (test build 119)

**Install and start (antivirus)**
- Windows Defender deleted the mod on some PCs: the launcher loaded the mod into the game the way cheat tools do. The launcher is gone. The mod now comes as a small `dinput8.dll` plus an `ArcanaPlusR` folder in the game's folder; the game loads it by itself. No game file is changed.
- Start the game from Steam (or the installer's shortcut). Hold Shift while it starts to play without the mod, or set `[mod] enabled=0` in rollback.ini.
- The installer finds the game through Steam, takes over Settings and My setup from the old install and removes it.
- Update check: the main menu says when a new version is out; a click opens the download page. The mod no longer downloads files itself.
- The DLLs and the installer have proper file details (name, version, description).

**Fixes**
- The DESYNC count could go up for a few seconds after an in-place rematch while the game was fine (a false alarm: the restarted fight's data moved in memory).
- Players whose game files differ can no longer sit at the same table and fail silently: joining the room says so, and a match that can't start shows a pop-up.

## 0.1.0 beta (build 118, first release)

The first public version. Everything below is new compared to the old test builds.

**Playing online**
- Rooms (Steam lobbies): Create room (public or private, 2-8 players), Room search for public rooms, Steam friend invites, and a code for private rooms (Room search > Join with code).
- Tables in rooms: sit down, both players press READY, the fight starts after 3-2-1 with each player's My setup. Other players in the room see who plays and the score.
- Room settings for the host: rounds, round timer, set length (games per set) and room size; the host can remove a player. Text chat.
- After a fight: Rematch (starts again at once, no versus or loading screen) or Back to room. Back to room wins if the players pick differently. After the set length, only Back to room.
- Connection lost pop-up with the reason, and a waiting banner when the other player's packets stop for a moment.
- In fights: a small info bar (ping, input delay, rollback, stutters) and the player names under the life bars. Pausing is off online.
- The controller you used in the menus is the one you play with, and it follows you if you switch to another pad.

**Front end**
- The game boots into the mod's own main menu: Room search, Create room, Settings, Help, Quit, and a My setup card. The game's own title screen and menus are skipped.
- My setup: character, colour, Support Knights presets and stage, remembered between sessions.
- Settings: controls (remap controller and keyboard, button check, vibration), sound (game volumes, menu music and sounds), display (resolution, display mode, borderless, V-Sync), online (input delay, connection info), logs (Pack logs, open folder).
- Help: playing online, connection trouble, reporting a problem, links and credits.
- Mouse everywhere in the menus: click buttons and the button prompts, right click = back.
- F1 (or Back + Start) opens and closes the main menu.

**Launcher and install**
- Installer (per user, Start menu and desktop shortcut, uninstaller) next to the zip.
- The launcher checks for a new version at start and asks before installing it (download checked with SHA-256, settings kept, old files backed up). Off with `[update] check=0`.
- No console window any more; problems show in a pop-up and in `launcher_log.txt`.
- The `rollback.ini` that ships is short; settings for testing the netcode are in `docs/dev-settings.md`.

**Fixes from the first two-player tests**
- A controller that read nothing in the first fight (the session used the game's player 1 pad instead of the one in use).
- The desync label counting up at the start of every fight because of values left over from loading (not from play).
- The game's own end-of-match boxes flashing over the versus screen.

## Before 0.1.0 (builds 1-89)

Rollback engine, online play over Steam, the old F1 match setup overlay (removed in 0.1.0) and booting straight into the fight. See the git history.
