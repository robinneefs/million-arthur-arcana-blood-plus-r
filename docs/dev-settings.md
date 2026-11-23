# Developer settings (rollback.ini)

The rollback.ini that ships with the mod only holds the player-facing options. Everything below
still works when you add it to rollback.ini by hand; each line shows the default the mod uses when
the key is missing. These are for testing and debugging the netcode, not for normal play.

Test keys F4-F11 only work with `[dev] tools=1`.

```ini
[dev]
; 1 = a test player sits across from you in your own room and copies READY (one-PC tests)
room_standin=0
; 1 = the test keys F4-F11 work (sync tests, profiling, loopback connect). Leave at 0 to play.
tools=0

[rollback]
; How many frames F9 profiles (60 = 1 second)
profile_frames=600
; Sync test rewind depth at start (1, 2, 4 or 8). F11 changes it in game.
depth=1
; 1 = restore memory written both inside and outside the game update (needed)
include_contested=1
; How many desyncs get a detailed report in the log
max_diff_reports=30
; 1 = also find and restore fight data in heap memory (needed)
heap=1
; 1 = also restore the heap memory the profile found (needed for an exact match)
heap_restore=1
; Frames of "writer check" that run right after the F9 profile (0 = off)
writer_frames=240
; 1 = sync test leaves out memory that other game threads write
skip_thread_chunks=1
; 1 = sync test saves whole heap blocks that hold fight data
whole_blocks=1
; 1 = while the sync test runs, find rarely changed fight memory and rewind it too
hunt=1
; 1 = save and restore only the memory that changed since the last save (faster; 0 = copy everything every time)
changed_memory_only=1
; Build 73: heap blocks bigger than this many KB are never watched, so they stay out of the saved state
; (test 72a: a 16 MB screen-sized block joining mid-fight froze the game for a quarter second)
watch_block_max_kb=8192
; ---- Rollback test (F7) ----
; Frames until the "network" input arrives (0-6). 4 frames is about 67 ms.
fake_lag_frames=4
; Up to this many extra frames, at random, per input
fake_jitter_frames=2
; Percent of inputs that arrive 4 frames later still (like a lost packet that gets resent)
fake_loss_percent=5
; Which player's input is treated as coming over the network (1 or 2)
fake_remote_player=1
; Every N frames, rewind 8 frames and check the replay matches exactly (0 = off)
verify_every=4
; The same check in online fights (build 57: off, it made the laptop fall behind in test 56)
online_verify_every=0
; build 88: a check replays only as many frames as fit in this many ms (measured per PC), so the test
; does not slow a slower PC down (0 = always replay the full rollback depth)
verify_budget_ms=8
; build 79: game code whose random numbers use their own seed (hex, comma separated; empty = none).
; (test 79a: giving a caller its own seed breaks the rollback check, so off by default)
own_seed_callers=
; build 80: online, reset the game's random-interval timers at fight start so both PCs start them alike
; (test 79a: a Stage 06 timer had ticked a different number of times on each PC before the fight)
reset_stage_timers=1
; build 81: rollback_memlist.txt next to the DLL collects the memory the memory watch finds in each fight,
; and the next session starts with it in the fight state (0 = neither read nor write the file)
memory_list=1
; build 82: the big heap blocks left out of the saved state are made read-only during a fight to log who writes them
big_block_watch=1
; build 83: heap blocks over watch_block_max_kb up to this size are watched page by page (a written page joins the state)
watch_big_block_max_kb=32768
; build 84: up to 3 addresses (hex) the CPU watches for writes during a fight; the log lists which game code wrote them
hw_watch=

; ---- Online play (step 3c). F4 connects / disconnects. ----
[net]
; loopback = test on this PC alone against a stand-in player
; lan      = test between two PCs on your home network
; steam    = play a Steam friend
mode=steam
; Your input is used this many frames after you press it (0-8). More = fewer rewinds.
input_delay=2
; Which player you want to be: 0 = decide automatically, 1 or 2
side=0
; 1 = rollback in fights, 0 = always wait for the other player's input (slower, but simple)
rollback=1
; Home network test: the other PC's address, or auto to find it by itself
lan_peer=auto
lan_port=7845
; Steam: the friend's Steam ID (a long number). Leave empty to pick with F5 in game.
steam_friend=
; Loopback test only: lag of the fake connection in frames each way, extra random lag, lost packets
loopback_lag_frames=3
loopback_jitter_frames=1
loopback_loss_percent=5
; Loopback test only: mirror = the stand-in player copies your input, idle = it does nothing
loopback_other_player=mirror
; Loopback test only: from this frame of each fight on, the stand-in pretends to be out of sync (0 = off)
loopback_test_desync_frame=0
; Seconds without packets before the session ends, in milliseconds
timeout_ms=8000
; Build 57: 1 = keep the two games' clocks together using both PCs' view (0 = off, for comparing)
clock_sync=1

; ---- Overlay ----
[ui]
; Your controller: auto (build 114: the pad used in the menus, follows a switch) or a fixed slot 0-9
controller=auto
; Build 72, for the boss-crash test only: 1 = the boss (Corrupted Iai Arthur) is back in the character list
show_boss=0
; Build 101: 1 = the game's own title screen (logos, intro, press start, its menu) is kept on hold
; while our main menu is up; 0 = let it run underneath (only if something at start-up breaks)
hold_title=1

; ---- Frame rate (build 123) ----
[perf]
; 1 = our own frame pacer replaces the game's frame limiter (fixed 60 fps schedule, precise timer,
; clock sync lengthens frames slightly instead of pausing whole frames); 0 = the game's own limiter
pacer=1

[display]
; 1 = the launcher sets Windows' graphics choice for the game to "High performance" (the fast
; graphics card on laptops with two); 0 = remove that again (only if the launcher set it)
fast_gpu=1
```
