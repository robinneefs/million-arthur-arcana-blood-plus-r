# Million Arthur: Arcana Blood +R

A mod that adds rollback netcode to the Steam version of Million Arthur: Arcana Blood.

## Status

**Work in progress, not ready for general use yet.**

- Done: saving and restoring the game state every frame, rollback, our own match setup overlay (lobby, controls, character, colour, assist and stage select with real names), booting straight into the fight and back to our Rematch / Back to lobby screen. Online play over Steam works: the first test between two players over the internet (8 fights) had no desyncs and felt close to offline play.
- Next: a clearer connect flow and messages (for example when the other player leaves), release logging, a nicer select screen. The boss character stays hidden for now.

## Using it

1. Put `arcana_launcher.exe`, `arcana_rollback.dll` and `rollback.ini` in one folder, anywhere outside the game folder.
2. Start `arcana_launcher.exe`. It finds the game through Steam, starts it, and loads the mod.
3. The overlay opens on the main menu. F1 shows or hides it. Choose "Steam friend", refresh the list, pick your friend and connect.
4. The mod writes `rollback_log.txt` next to itself (the previous session's log is kept as `rollback_log_previous.txt`). If something goes wrong, those files help find the cause.

Only the Steam version the mod was made for is supported. If the game version differs, the mod logs this and stays off.

## Building

The mod and launcher are 32-bit Windows programs built with MinGW-w64. They need no runtimes (no .NET, no Visual C++ redistributable).

```sh
# Ubuntu / WSL
sudo apt install g++-mingw-w64-i686
./build.sh
```

The output goes to `build/`.

## Folder layout

| Folder | What is in it |
| --- | --- |
| `src/mod/` | The mod DLL that runs inside the game |
| `src/launcher/` | The launcher that starts the game with the mod |
| `third_party/imgui/` | Dear ImGui 1.82, the overlay's UI library |
| `config/` | The default `rollback.ini` |
| `tools/ui-preview/` | Builds the overlay screens on their own, to preview them without the game |
| `docs/` | How the mod works |

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut and contributors, MIT licence (`third_party/imgui/LICENSE.txt`).

## Licence

MIT, see `LICENSE`. This project is not affiliated with the game's makers. You need your own copy of the game.
