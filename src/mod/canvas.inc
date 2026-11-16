// ===========================================================================
// Shared pieces for our menus (was the build 90 UI playground; the playground,
// its style editor and ui_theme.ini were removed in build 106).
//   - the 1920x1080 design canvas, scaled to the picture and centred in a 16:9 area (CV / CP)
//   - PG: whether our menu is open (game thread opens / closes it)
//   - font lookup (<mod folder>\fonts\ first, then Windows' fonts)
// ===========================================================================

static float g_cv = 1.0f;          // canvas scale (set every frame by UiFrame)
static ImVec2 g_cvO(0, 0);         // top left of the 16:9 canvas area on the picture
static ImVec2 g_disp(1920, 1080);  // whole picture
static inline float CV(float v) { return v * g_cv; }
static inline ImVec2 CV2(float x, float y) { return ImVec2(x * g_cv, y * g_cv); }
static inline ImVec2 CP(float x, float y) { return ImVec2(g_cvO.x + x * g_cv, g_cvO.y + y * g_cv); }

static struct Pg {
    std::atomic<bool> open{false};
    std::atomic<bool> freeArt{false};       // drawing thread: let go of the game pictures
} PG;
static bool g_fontsDirty = false;           // rebuild the font atlas before the next frame

#ifndef UI_HARNESS
// Full path of a font file name (mod fonts folder first), "" if not found.
static bool PgFontPath(const char* file, char* out, size_t n) {
    char mod[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, util::ModDir(), -1, mod, sizeof mod, nullptr, nullptr);
    snprintf(out, n, "%sfonts\\%s", mod, file);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    snprintf(out, n, "%s\\Fonts\\%s", win, file);
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}
#endif

// Game thread: open / close our menu.
static void PgSetOpen(bool on) {
    if (PG.open.load() == on) return;
    PG.open = on;
    if (on) {
        U.visible = true;
        Log("ui: our menu opened");
    } else {
        U.visible = false;
        U.drain = true;
        PG.freeArt = true;
        Log("ui: our menu closed");
    }
}

static inline ImU32 Col(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
static float PgAnim(ImGuiID id, bool on, float speed = 10.0f) {   // 0..1, eases toward on
    ImGuiStorage* st = ImGui::GetStateStorage();
    float v = st->GetFloat(id, 0.0f);
    float k = theme::T().animSpeed <= 0.01f ? 1.0f : ImMin(1.0f, ImGui::GetIO().DeltaTime * speed * theme::T().animSpeed);
    v += ((on ? 1.0f : 0.0f) - v) * k;
    st->SetFloat(id, v);
    return v;
}
