# Changelog

Versions follow major.minor.patch: the middle number goes up for new features, the last one for fixes.
"build" is the internal test build number (also in `src/mod/version.h`).

## 0.2.0 beta (build 120, 2026-11-18)

**Launcher and install (antivirus)**
- Windows Defender deleted the mod on some PCs: the old launcher loaded the mod into the game the way cheat tools do. Nothing is injected any more: the game loads a small `dinput8.dll` from its own folder, and that loads the mod.
- The launcher is back, in the main menu's style: it checks for a new version each time it opens, can install it with one click (download, checksum check, backup, a few seconds; nothing changes if it fails), and Play starts the game with the mod.
- The mod only starts when the launcher starts the game. Started from Steam, the game is the plain game. Hold Shift while it starts to skip the mod once.
- The game folder only gets `dinput8.dll`, and the launcher checks it on every Play, so a reinstalled or moved game gets it back by itself. Another mod's `dinput8.dll` is kept as `dinput8.dll.before-plusr` (the launcher asks first) and comes back on uninstall.
- The installer goes back to its own folder (%LOCALAPPDATA%\Arcana Blood +R) and takes over Settings and My setup from older installs.
- The main menu still says when a new version came out while you play; the launcher installs it.
- The DLLs, the launcher and the installer have proper file details (name, version, description).
- The launcher is now called `Arcana Blood +R.exe` (was `arcana_launcher.exe`), with bigger, sharper text, and it no longer asks for admin rights (no UAC prompt) when it starts.

**Look and feel**
- A pass over every screen: panels, tables, buttons and prompts line up on one grid with the same padding on all sides (main menu, room, Room search, Create room, My setup, Settings, after-match screen, Connection lost pop-up).
- Small text in the mod's menus (like the HOST tag) is drawn from a font made at that size, so it stays sharp.
- My setup: each colour square is centred on the character's face; the colour squares, character and stage grids fit their boxes; the preview is wider.
- After a fight, CHOOSING stays in place while its dots come and go.
- Settings > Logs shows the real version.

**Fixes**
- The DESYNC count could go up for a few seconds after an in-place rematch while the game was fine (a false alarm: the restarted fight's data moved in memory).
- Players whose game files differ can no longer sit at the same table and fail silently: joining the room says so, and a match that can't start shows a pop-up.
- Room search showed the old host's name after the host left the room; it now shows the new host.
- Each stage preview in My setup now loads the stage it shows (7 of the 8 previews pointed at another stage).
- Creating a room while Steam is in Offline Mode said "code 3"; it now says Steam is offline and how to go online.
- Settings > Controls: Vibration sat next to Button check, and Right toggled it instead of moving on. Button check and Vibration are now one below the other.

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
