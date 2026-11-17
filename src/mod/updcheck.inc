// ===========================================================================
// Build 119: update check inside the mod (included into mod.cpp). Before, the launcher
// asked GitHub and could download and install the new files itself; the launcher is gone
// now (the mod loads through dinput8.dll), and a program that downloads programs is one of
// the things antivirus scanners dislike. So the mod only asks GitHub which releases there
// are; when one is newer, the main menu says so and a click opens the download page.
//
// rollback.ini [update]: check=0 turns it off; repo=owner/name and test_version=x.y.z are
// for testing.
// ===========================================================================
#include <winhttp.h>
#include <map>

namespace upd {

// ---- HTTPS GET (WinHTTP follows GitHub's redirects to its file servers) ----
static bool Get(const std::wstring& url, std::string& out, int timeoutMs, const wchar_t* accept = nullptr) {
    out.clear();
    URL_COMPONENTS u{sizeof u};
    wchar_t host[256] = L"", path[2048] = L"";
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path; u.dwUrlPathLength = 2048;
    u.dwSchemeLength = (DWORD)-1; u.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) return false;
    std::wstring p = path;
    if (u.lpszExtraInfo && u.dwExtraInfoLength) p.append(u.lpszExtraInfo, u.dwExtraInfoLength);
    HINTERNET s = WinHttpOpen(L"ArcanaBloodPlusR", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"ArcanaBloodPlusR", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);   // older Windows
    if (!s) return false;
    WinHttpSetTimeouts(s, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", p.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (r) {
        std::wstring hdr = accept ? std::wstring(L"Accept: ") + accept + L"\r\n" : L"";
        if (WinHttpSendRequest(r, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), (DWORD)-1, nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
            DWORD n = 0;
            char buf[65536];
            ok = code == 200;
            while (ok && WinHttpReadData(r, buf, sizeof buf, &n) && n) out.append(buf, n);
            if (!ok) Log("update check: the server answered %lu", code);
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// ---- a small JSON reader (enough for GitHub's release list) ----
struct J {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    std::string s;
    std::vector<J> a;
    std::map<std::string, J> o;
    const J& operator[](const char* k) const { static J none; auto i = o.find(k); return i == o.end() ? none : i->second; }
};
struct JP {
    const char* p; const char* e; int depth = 0;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                char c = *p++;
                if (c == 'n') out += '\n'; else if (c == 't') out += '\t'; else if (c == 'r') {}
                else if (c == 'u' && e - p >= 4) {
                    unsigned v = (unsigned)strtoul(std::string(p, 4).c_str(), nullptr, 16); p += 4;
                    if (v < 0x80) out += (char)v;
                    else if (v < 0x800) { out += (char)(0xC0 | (v >> 6)); out += (char)(0x80 | (v & 63)); }
                    else { out += (char)(0xE0 | (v >> 12)); out += (char)(0x80 | ((v >> 6) & 63)); out += (char)(0x80 | (v & 63)); }
                } else out += c;
            } else out += *p++;
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool val(J& j) {
        ws();
        if (p >= e || ++depth > 64) return false;
        bool ok = true;
        if (*p == '"') { j.t = J::STR; ok = str(j.s); }
        else if (*p == '{') {
            j.t = J::OBJ; p++; ws();
            if (p < e && *p == '}') p++;
            else for (;;) {
                ws(); std::string k;
                if (!str(k)) { ok = false; break; }
                ws(); if (p >= e || *p != ':') { ok = false; break; } p++;
                if (!val(j.o[k])) { ok = false; break; }
                ws(); if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false; break;
            }
        } else if (*p == '[') {
            j.t = J::ARR; p++; ws();
            if (p < e && *p == ']') p++;
            else for (;;) {
                j.a.emplace_back();
                if (!val(j.a.back())) { ok = false; break; }
                ws(); if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false; break;
            }
        } else if (!strncmp(p, "true", 4)) { j.t = J::BOOL; j.b = true; p += 4; }
        else if (!strncmp(p, "false", 5)) { j.t = J::BOOL; p += 5; }
        else if (!strncmp(p, "null", 4)) p += 4;
        else { j.t = J::NUM; const char* q = p; while (p < e && (isdigit((unsigned char)*p) || strchr("+-.eE", *p))) p++; j.s.assign(q, p); ok = p > q; }
        depth--;
        return ok;
    }
};

// ---- versions: "v0.2.0-beta" -> {0,2,0} + "beta"; a final release is newer than its betas ----
struct Ver { int n[3] = {0, 0, 0}; std::string pre; bool ok = false; };
static Ver ParseVer(std::string s) {
    Ver v;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    size_t dash = s.find('-');
    if (dash != std::string::npos) { v.pre = s.substr(dash + 1); s.resize(dash); }
    v.ok = sscanf(s.c_str(), "%d.%d.%d", &v.n[0], &v.n[1], &v.n[2]) >= 2;
    return v;
}
static int Cmp(const Ver& a, const Ver& b) {
    for (int i = 0; i < 3; i++) if (a.n[i] != b.n[i]) return a.n[i] < b.n[i] ? -1 : 1;
    if (a.pre == b.pre) return 0;
    if (a.pre.empty()) return 1;
    if (b.pre.empty()) return -1;
    return a.pre < b.pre ? -1 : 1;      // beta < beta2 < rc ...
}


static char g_newVer[40] = "";          // "0.2.0-beta" once a newer release is found (then never changes)
static std::atomic<bool> g_have{false};
static wchar_t g_repo[128] = L"robinneefs/million-arthur-arcana-blood-plus-r";

static DWORD WINAPI CheckThread(LPVOID) {
    wchar_t ini[MAX_PATH], tv[32];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    if (!GetPrivateProfileIntW(L"update", L"check", 1, ini)) { Log("update check: off (check=0)"); return 0; }
    GetPrivateProfileStringW(L"update", L"repo", L"robinneefs/million-arthur-arcana-blood-plus-r", g_repo, 128, ini);
    GetPrivateProfileStringW(L"update", L"test_version", L"", tv, 32, ini);
    char tva[32];
    WideCharToMultiByte(CP_UTF8, 0, tv, -1, tva, sizeof tva, nullptr, nullptr);
    Ver mine = ParseVer(tva[0] ? std::string(tva) : std::string(MOD_VERSION));
    std::string body;
    if (!Get(std::wstring(L"https://api.github.com/repos/") + g_repo + L"/releases?per_page=20", body, 8000, L"application/vnd.github+json")) {
        Log("update check: could not reach GitHub, skipped");
        return 0;
    }
    J list;
    JP jp{body.data(), body.data() + body.size()};
    if (!jp.val(list) || list.t != J::ARR) { Log("update check: could not read the release list"); return 0; }
    Ver best;
    std::string tag;
    for (const J& r : list.a) {
        if (r["draft"].b) continue;
        Ver v = ParseVer(r["tag_name"].s);
        if (!v.ok || (best.ok && Cmp(v, best) <= 0)) continue;
        best = v;
        tag = r["tag_name"].s;
    }
    if (!best.ok || Cmp(best, mine) <= 0) { Log("update check: this is the newest version"); return 0; }
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    snprintf(g_newVer, sizeof g_newVer, "%s", tag.c_str());
    g_have = true;
    Log("update check: version %s is available", g_newVer);
    return 0;
}
static void Start() { CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr); }
static const char* NewVersion() { return g_have.load() ? g_newVer : nullptr; }
static void OpenDownloadPage() {
    wchar_t url[256];
    _snwprintf(url, 256, L"https://github.com/%ls/releases", g_repo);
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    Log("update check: opened the download page");
}

}  // namespace upd
