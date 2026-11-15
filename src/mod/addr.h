// Fixed addresses inside "Million Arthur.exe" (Steam build, AppID 989550).
// The exe has no ASLR (it always loads at 0x400000), so these never move
// for this exact build. Every patch site is checked against the expected
// bytes before we touch it; if the game is a different version, the mod
// logs it and does nothing.
#pragma once
#include <cstdint>

namespace addr {

// .data/.bss section: where the game keeps (almost) all its live state.
// The game barely uses the heap (about 100 "new" call sites in total).
constexpr uint32_t DATA_START = 0x008E8000;
constexpr uint32_t DATA_SIZE  = 0x04905320;   // ~73 MB, mostly zero-filled
constexpr uint32_t PAGE       = 0x1000;
constexpr uint32_t DATA_PAGES = (DATA_SIZE + PAGE - 1) / PAGE;

// Game thread frame function (sub_620FD0) calls the scene "update" here.
// Update and drawing are separate calls, which is what makes rollback easy:
// we can run update several times per frame without drawing.
constexpr uint32_t CALL_UPDATE_OVERLAY = 0x00621104;  // call 0x59ED40 (overlay scene)
constexpr uint32_t CALL_UPDATE_MAIN    = 0x0062112E;  // call 0x59ED40 (main scene)
constexpr uint32_t FN_SCENE_UPDATE     = 0x0059ED40;  // __thiscall uint32 (scene*)

// C runtime rand()/srand(). Their seed lives in per-thread heap memory,
// outside .data, so we replace them with our own copy that we can save.
constexpr uint32_t FN_RAND  = 0x0063AE83;
constexpr uint32_t FN_SRAND = 0x0063AEA4;

// Sound manager (..\\ActView\\common\\SoundMgr.cpp): cue pointer table
// [2 players][0xC98][2] (IXACT3Cue*), and two single cue pointers.
constexpr uint32_t G_CUE_TABLE = 0x04B7B290;
constexpr uint32_t G_CUE_SINGLES[] = {0x04B87C10, 0x04B7A5F4};
// Sound bank wrappers: the first dword is an IXACT3SoundBank*. The game makes
// cues through FN_CUE_PLAY(this=wrapper, cue index, IXACT3Cue** out, offset),
// which calls SoundBank::Play (function table slot 4).
constexpr uint32_t G_SOUNDBANK_WRAPS[] = {0x0515EAB0, 0x0515EC40};

// Globals
constexpr uint32_t G_MAIN_SCENE    = 0x051E9F08;  // current scene object (CML*)
constexpr uint32_t G_OVERLAY_SCENE = 0x051E9F0C;
constexpr uint32_t G_HWND          = 0x051CA9C4;  // game window
constexpr uint32_t G_CRT_HEAP      = 0x00E7994C;  // heap used by malloc/new
constexpr uint32_t RET_IN_MALLOC   = 0x0063ACA0;  // return address of HeapAlloc inside malloc
constexpr uint32_t RET_IN_NEW      = 0x0063A19C;  // return address of malloc inside operator new
constexpr uint32_t FN_GETPTD       = 0x006439EF;  // CRT _getptd

struct Range { uint32_t start, end; };

// .data pages we never restore even though the game logic writes them:
// they are also written by other threads or hold timing/profiler data.
//   0x051CD000: D3D device-lost flags (main window thread)
//   0x051EA000: frame-rate counters, QueryPerformanceCounter profiler table.
//     Build 32/test 32: the same page also holds the stage effect timer object
//     (0x051EAC18, 6 timers at +0x40 that call rand when they fire), so only
//     that part of the page is saved and restored (PART_RANGE below).
constexpr uint32_t NEVER_RESTORE_PAGES[] = {0x051CD000};
constexpr uint32_t PART_PAGE = 0x051EA000;
// Build 44: object pools the game allocates once and keeps (pointers to them).
// The profile only adds a heap block to the state when it sees it change, and
// these change only when that kind of object is in use (test 43: the
// 1000 x 0x98 effect pool, new[] at 0x5D7CC0, was missing from the state in
// most fights and the first effect after the profile desynced).
//   0x04DCAD80: effect pool (152004-byte block, pointer = block + 4)
//   0x051E50A8: 108800-byte pool (allocated at 0x573920)
//   player struct + 0x7B68 / + 0x7B7C (build 45): each player has two object
//     managers (at +0x7B64 and +0x7B78, set up at 0x46E170) with a 552244-byte
//     pool of 0x104 x 0x84C objects each (pointer = block + 4). Test 44: a
//     fight had only one of the four in the state and desynced.
//   0x04DCADB4 (build 86): the super/cut-in owner pool, 40 x 0x20 nodes in a
//     0x504-byte block (new[] at 0x5EE98F, pointer = block + 4), list head
//     0x04DCADB8, free list 0x04DCADBC. Too small for the memory watch (64 KB+
//     blocks only), so a rollback rewound the list heads but not the nodes:
//     Clone Uathach's cut-in then walked a freed node with owner NULL and
//     crashed at 0x4B7233 (tests 82a-85a).
constexpr uint32_t POOL_PTRS[] = {0x04DCAD80, 0x051E50A8, 0x04DCADB4,
                                  0x05130CD8 + 0x7B68, 0x05130CD8 + 0x7B7C,
                                  0x05130CD8 + 0xA520 + 0x7B68, 0x05130CD8 + 0xA520 + 0x7B7C};
constexpr Range PART_RANGE = {0x051EAC14, 0x051EAD70};
// Build 49 (no profile any more): memory that is always in the fight state
// from the start, on top of FORCE_RANGES. Everything else the game logic
// writes is added the first time it does (memory watch).
//   0x05130000-0x05146000: the two player structs (0x05130CD8, 2 x 0xA520)
constexpr Range FIXED_RANGES[] = {{0x05130000, 0x05146000}};
// Heap blocks that held fight data in every profile so far (tests 41-48),
// found by size among the game's own blocks.
constexpr uint32_t FIGHT_BLOCK_SIZES[] = {507304, 986972, 3270964};
// Build 49: the intro pick. The fight header builder (0x431C00) calls
// 0x43AEB0(player 1 struct, player 2 struct) here; it picks which intro the
// two characters play (P+0x961C) with rand, during the loading screen, which
// is not synced between the two PCs (test 48: different intros).
constexpr uint32_t CALL_INTRO_PICK = 0x00432702;
constexpr uint32_t CALL_CHILD_UPDATE = 0x005BE272;  // game main -> current screen's update (call 0x59ED40)
constexpr uint8_t EXPECT_CALL_CHILD_UPDATE[] = {0xE8, 0xC9, 0x0A, 0xFE, 0xFF};
constexpr uint32_t FN_INTRO_PICK   = 0x0043AEB0;   // cdecl (void* p1, void* p2)
constexpr uint8_t EXPECT_CALL_INTRO_PICK[] = {0xE8, 0xA9, 0x87, 0x00, 0x00};
// The game's own random generator (xorshift) used when the gate at 0x4C9200
// says so; its 4 state words.
constexpr uint32_t G_XORSHIFT = 0x00E76E0C;

// Import table slots (pointers the game calls through).
constexpr uint32_t IAT_timeGetTime   = 0x0065C2A0;
constexpr uint32_t IAT_QPC           = 0x0065C160;
constexpr uint32_t IAT_GetTickCount  = 0x0065C1E4;
constexpr uint32_t IAT_HeapAlloc     = 0x0065C1A4;
constexpr uint32_t IAT_HeapFree      = 0x0065C1A0;
constexpr uint32_t IAT_HeapReAlloc   = 0x0065C1D0;
constexpr uint32_t IAT_ReadFile      = 0x0065C0B4;
// GetSystemTimeAsFileTime: used by the C runtime's _time64 (0x63AEB4). The
// fight code stores the date in its replay header (singleton 0x4DCADA8, +0x20).
constexpr uint32_t IAT_GetSystemTimeAsFileTime = 0x0065C0A4;

// CRT thread start (_threadstartex) and the FLS slot of its per-thread data.
constexpr uint32_t FN_THREADSTARTEX  = 0x0063AE2C;
constexpr uint32_t G_CRT_FLS_INDEX   = 0x008E86B4;

// Byte ranges written by the input code that runs just before update
// (sub_524E00 and the pad functions it calls). During a resimulation we
// put back the recorded input of each frame into these ranges.
// Memory always saved and restored, even if the profile did not see it
// change: the sound manager's tables (which sounds are playing, per-sound
// flags and counters). Test 15: replays picked other sounds than the first
// run, because some of these change too rarely for the profile to notice.
constexpr Range FORCE_RANGES[] = {
    {0x04B79000, 0x04B89000},
    // Found by the state hunt in test 19 (written by the game logic, no other threads):
    {0x008E9000, 0x008EA000},   // found in test 26
    {0x008EB000, 0x008ED000},   // 0x008EB000 found in test 26
    {0x0513E000, 0x0513F000},   // found in test 26
    {0x00A7E000, 0x00A7F000},
    {0x00A95000, 0x00A96000},
    {0x00B71000, 0x00B72000},
    {0x00B9C000, 0x00B9E000},
    {0x00D49000, 0x00D4A000},
    {0x00E52000, 0x00E53000},
    {0x00E7A000, 0x00E7B000},
    {0x04B8A000, 0x04B8B000},
    {0x04DC1000, 0x04DC3000},
    {0x05131000, 0x05133000},
    {0x05141000, 0x05142000},
    {0x0515C000, 0x0515E000},
    {0x051B4000, 0x051B5000},
    {0x051BC000, 0x051C1000},   // + 0x051BF000 (test 20), 0x051C0000 (test 26)
    {0x051C9000, 0x051CA000},   // found in test 20
    {0x051EB000, 0x051EC000},
};
constexpr Range INPUT_RANGES[] = {
    {0x04DC3100, 0x04DC3700},   // menu key repeat table (2 players x 9 keys)
    {0x051D3200, 0x051D3C00},   // pad device state
    {0x051E3D00, 0x051E4900},   // pad table: 10 entries x 248 bytes
};

// ---- Step 3: controller input ---------------------------------------------
// The frame loop calls the input function (sub_524E00) right before the
// scene update. Offline it calls sub_521C80, which reads every device into a
// table of 10 pads (0-3 XInput, 4-7 DirectInput, 8-9 keyboard) and then works
// out "held" / "just pressed" for each button. A pad whose +0x0C flag is set
// takes its buttons from +0x10/+0x14 instead of the device: the game's own
// online mode feeds the other player's input that way (utilPadSetInput).
// One player's input for one frame is therefore 8 bytes (raw0, raw1).
constexpr uint32_t CALL_INPUT      = 0x006210AB;  // call 0x524E00 (frame loop)
constexpr uint32_t FN_INPUT        = 0x00524E00;  // void cdecl ()
constexpr uint32_t CALL_DEVREAD    = 0x00521CBF;  // call 0x522D00 (read pads 0-7)
constexpr uint32_t FN_DEVREAD      = 0x00522D00;
constexpr uint32_t CALL_KBREAD     = 0x00521CC4;  // call 0x523590 (keyboard)
constexpr uint32_t FN_KBREAD       = 0x00523590;
constexpr uint32_t FN_PAD_OF_PLAYER = 0x00524410; // uint8 cdecl (uint8 player): pad index or 0xFF
constexpr uint32_t PAD_TABLE   = 0x051E3DC0;      // 10 x 0xF8
constexpr uint32_t PAD_STRIDE  = 0xF8;
constexpr uint32_t PAD_COUNT   = 10;
constexpr uint32_t PAD_RAW0    = 0x00, PAD_RAW1 = 0x04, PAD_EXT_FLAG = 0x0C, PAD_EXT0 = 0x10, PAD_EXT1 = 0x14;

// Files. The fight HUD opens a file per sprite (CreateFileW at 0x5CD6A2) and
// keeps the handle at sprite+0xA0; test 28's last desync (0x051C9C3C) was a
// replay getting a different handle number for the same file.
constexpr uint32_t IAT_CreateFileW   = 0x0065C0AC;
constexpr uint32_t IAT_CloseHandle   = 0x0065C0B8;
constexpr uint32_t IAT_CreateFileA   = 0x0065C120;

constexpr uint8_t EXPECT_CALL_INPUT[]   = {0xE8, 0x50, 0x3D, 0xF0, 0xFF};
constexpr uint8_t EXPECT_CALL_DEVREAD[] = {0xE8, 0x3C, 0x10, 0x00, 0x00};
constexpr uint8_t EXPECT_CALL_KBREAD[]  = {0xE8, 0xC7, 0x18, 0x00, 0x00};
constexpr uint8_t EXPECT_PAD_OF_PLAYER[] = {0x55, 0x8B, 0xEC, 0x0F, 0xB6, 0x45, 0x08, 0x3D};

// Expected original bytes at patch sites (from the decrypted exe).
constexpr uint8_t EXPECT_CALL_OVERLAY[] = {0xE8, 0x37, 0xDC, 0xF7, 0xFF};
constexpr uint8_t EXPECT_CALL_MAIN[]    = {0xE8, 0x0D, 0xDC, 0xF7, 0xFF};
constexpr uint8_t EXPECT_RAND[]         = {0xE8, 0x67, 0x8B, 0x00, 0x00};
constexpr uint8_t EXPECT_SRAND[]        = {0x55, 0x8B, 0xEC, 0xE8, 0x43};
constexpr uint8_t EXPECT_UPDATE[]       = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x89, 0x4D};

// Scene class names by vtable address (from the exe's RTTI), for the log.
struct VtName { uint32_t vt; const char* name; };
constexpr VtName SCENES[] = {
    {0x008D19B0, "CMLAnotherSelect"}, {0x008C9868, "CMLBtl"},
    {0x008D1BF8, "CMLCharaSel"},      {0x008D2494, "CMLDemo"},
    {0x008D1D5C, "CMLDemoBase"},      {0x008CA5F4, "CMLDialogBase"},
    {0x008D2060, "CMLGallery"},       {0x008D24D4, "CMLGameMain"},
    {0x008D2454, "CMLGameOver"},      {0x008D2604, "CMLLoading"},
    {0x008D25C4, "CMLNetWait"},       {0x008CA4B0, "CMLNetWork"},
    {0x008D26D8, "CMLNetworkCustomize"}, {0x008D2718, "CMLNews"},
    {0x008D27DC, "CMLNextStage"},     {0x008D2840, "CMLOption"},
    {0x008D2888, "CMLRanking"},       {0x008CA50C, "CMLRePlayTheater"},
    {0x008D290C, "CMLScoreAttackResult"}, {0x008D2514, "CMLStaffRoll"},
    {0x008CA5B4, "CMLSystem"},        {0x008D2584, "CMLTestMode"},
    {0x008D2CBC, "CMLTitle"},         {0x008D2414, "CMLWinner"},
};

inline const char* SceneName(uint32_t vt) {
    for (auto& s : SCENES) if (s.vt == vt) return s.name;
    return "?";
}


// Build 38: two-object check (called for every pair of fight objects from
// 0x597640); starts with push ebp / lea ebp,[esp-0x70].
constexpr uint32_t FN_HIT_CHECK = 0x00597C40;
constexpr uint8_t EXPECT_HIT_CHECK[] = {0x55, 0x8D, 0x6C, 0x24, 0x90};
} // namespace addr
