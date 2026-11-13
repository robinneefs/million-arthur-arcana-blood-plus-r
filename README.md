# Million Arthur: Arcana Blood +R

A mod that adds rollback netcode to the Steam version of Million Arthur: Arcana Blood.

## Status

**Work in progress, not ready to play yet.**

- Done: saving and restoring the game state every frame, sync tests, rollback with fake lag, online play between two PCs on a home network.
- In progress: our own match setup overlay (lobby, controls, character, colour, assist and stage select, rematch). Build 57 has the screens and the setup protocol but does not start a fight from them yet.
- Planned: booting straight into the fight from the overlay, Steam friend matches.

## Using it

1. Put `arcana_launcher.exe`, `arcana_rollback.dll` and `rollback.ini` in one folder, anywhere outside the game folder.
2. Start `arcana_launcher.exe`. It finds the game through Steam, starts it, and loads the mod.
3. The overlay opens on the title screen. F1 shows or hides it.

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
