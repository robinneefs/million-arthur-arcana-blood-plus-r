// Build 120: the launcher's update check and updater (included into launcher.cpp).
//
// Check: GitHub's release list for this repository; the newest release that is newer than
// this version wins. Its "-files.tar" asset holds the files as they go into our folder, and
// GitHub lists the SHA-256 of every asset ("digest"). Update: download the tar, check the
// SHA-256, copy the old files to update-backup\, unpack the new ones over them (rollback.ini
// is kept), and start the new launcher. If anything fails, the old files go back.
// Nothing downloaded is ever run except the launcher that replaced this one.
//
// rollback.ini [update]: check=0 turns it off; repo=owner/name and test_version=x.y.z are for testing.

#include <winhttp.h>
#include <bcrypt.h>
#include <map>

namespace upd {

// ---- HTTPS GET; progress(got, total) is called while it reads ----
static bool Get(const std::wstring& url, std::string& out, int timeoutMs, const wchar_t* accept = nullptr,
                void (*progress)(uint64_t, uint64_t) = nullptr) {
    out.clear();
    URL_COMPONENTS u{sizeof u};
    wchar_t host[256] = L"", path[2048] = L"";
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path; u.dwUrlPathLength = 2048;
    u.dwSchemeLength = (DWORD)-1; u.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) return false;
    std::wstring p = path;
    if (u.lpszExtraInfo && u.dwExtraInfoLength) p.append(u.lpszExtraInfo, u.dwExtraInfoLength);
    HINTERNET s = WinHttpOpen(L"ArcanaBloodPlusR-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"ArcanaBloodPlusR-launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);   // older Windows
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
            wchar_t len[32] = L"";
            DWORD ls = sizeof len;
            uint64_t total = 0;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &ls, nullptr)) total = _wcstoui64(len, nullptr, 10);
            DWORD n = 0;
            static char buf[65536];
            ok = code == 200;
            while (ok && WinHttpReadData(r, buf, sizeof buf, &n) && n) {
                out.append(buf, n);
                if (progress) progress(out.size(), total);
            }
            if (!ok) Say(L"  (the server answered %lu)", code);
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
// "0.2.0-beta" -> "v0.2.0 beta" for the screen
static std::string Pretty(std::string tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    for (auto& c : tag) if (c == '-') c = ' ';
    return "v" + tag;
}

static std::string Sha256(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char d[32];
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
        BCryptFinishHash(h, d, 32, 0) == 0) {
        char t[3];
        for (int i = 0; i < 32; i++) { snprintf(t, 3, "%02x", d[i]); hex += t; }
    }
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// ---- tar (ustar, no compression): the release's "-files.tar" ----
struct TarFile { std::string name; size_t off, size; };
static bool ReadTar(const std::string& t, std::vector<TarFile>& out) {
    size_t p = 0;
    while (p + 512 <= t.size()) {
        const char* h = t.data() + p;
        if (!h[0]) break;                                   // the two empty blocks at the end
        char name[101] = "", prefix[156] = "", sz[13] = "";
        memcpy(name, h, 100); memcpy(sz, h + 124, 12);
        if (!memcmp(h + 257, "ustar", 5)) memcpy(prefix, h + 345, 155);
        size_t size = (size_t)strtoull(sz, nullptr, 8);
        char type = h[156];
        std::string n = prefix[0] ? std::string(prefix) + "/" + name : std::string(name);
        while (n.size() > 2 && n[0] == '.' && n[1] == '/') n.erase(0, 2);
        if (p + 512 + size > t.size()) return false;
        if ((type == '0' || type == 0) && !n.empty()) {
            if (n.find("..") != std::string::npos || n[0] == '/' || n.find(':') != std::string::npos) return false;   // nothing outside our folder
            out.push_back({n, p + 512, size});
        }
        p += 512 + ((size + 511) / 512) * 512;
    }
    return !out.empty();
}

static std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static bool WriteFileAll(const std::wstring& p, const char* d, size_t n) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static void MakeDirsFor(const std::wstring& root, const std::wstring& rel) {
    for (size_t i = rel.find(L'\\'); i != std::wstring::npos; i = rel.find(L'\\', i + 1)) CreateDirectoryW((root + rel.substr(0, i)).c_str(), nullptr);
}
static void RemoveTree(const std::wstring& d) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = f.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = d + L"\\" + n;
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryW(d.c_str());
}

// ---- what the window shows (written by the worker threads, read by the window) ----
enum State { S_CHECKING, S_UPTODATE, S_OFFLINE, S_AVAILABLE, S_UPDATING, S_UPDATED, S_FAILED };
struct Release { std::string tag, pretty, tarUrl, sha; std::vector<std::string> notes; uint64_t size = 0; Ver v; };
static std::atomic<int> g_state{S_CHECKING};
static std::atomic<int> g_step{0};                    // updating: 0 download, 1 check, 2 put in place
static std::atomic<uint64_t> g_got{0}, g_total{0};
static CRITICAL_SECTION g_lock;
static Release g_rel;                                 // the newer release (S_AVAILABLE and later)
static std::string g_failWhy;
static bool g_canUpdate = false;                      // the release has a tar with a checksum
static std::wstring g_dir, g_repo;

static void OnProgress(uint64_t got, uint64_t total) { g_got = got; if (total) g_total = total; }

static DWORD WINAPI CheckThread(LPVOID) {
    std::wstring ini = g_dir + L"rollback.ini";
    if (!GetPrivateProfileIntW(L"update", L"check", 1, ini.c_str())) { Say(L"Update check off (check=0)."); g_state = S_UPTODATE; return 0; }
    wchar_t tv[32];
    GetPrivateProfileStringW(L"update", L"test_version", L"", tv, 32, ini.c_str());
    char tva[32];
    WideCharToMultiByte(CP_UTF8, 0, tv, -1, tva, sizeof tva, nullptr, nullptr);
    Ver mine = ParseVer(tva[0] ? std::string(tva) : std::string(MOD_VERSION));
    Say(L"Checking for a newer version...");
    std::string body;
    if (!Get(L"https://api.github.com/repos/" + g_repo + L"/releases?per_page=20", body, 6000, L"application/vnd.github+json")) {
        Say(L"  Could not reach GitHub.");
        g_state = S_OFFLINE;
        return 0;
    }
    J list;
    JP jp{body.data(), body.data() + body.size()};
    if (!jp.val(list) || list.t != J::ARR) { Say(L"  Could not read the release list."); g_state = S_OFFLINE; return 0; }
    Release best;
    for (const J& r : list.a) {
        if (r["draft"].b) continue;
        Release c;
        c.tag = r["tag_name"].s;
        c.v = ParseVer(c.tag);
        if (!c.v.ok || (best.v.ok && Cmp(c.v, best.v) <= 0)) continue;
        c.pretty = Pretty(c.tag);
        for (const J& a : r["assets"].a) {
            const std::string& n = a["name"].s;
            if (n.size() > 10 && !_stricmp(n.c_str() + n.size() - 10, "-files.tar")) {
                c.tarUrl = a["browser_download_url"].s;
                c.size = strtoull(a["size"].s.c_str(), nullptr, 10);
                const std::string& d = a["digest"].s;          // "sha256:<hex>"
                if (!d.compare(0, 7, "sha256:")) c.sha = d.substr(7);
            }
        }
        // what's new: the first lines of the release notes, without list marks and headings
        std::string notes = r["body"].s;
        size_t i = 0;
        while (i < notes.size() && c.notes.size() < 3) {
            size_t e = notes.find('\n', i);
            std::string l = notes.substr(i, e == std::string::npos ? std::string::npos : e - i);
            i = e == std::string::npos ? notes.size() : e + 1;
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            size_t k = l.find_first_not_of(" \t-*#>");
            if (k == std::string::npos) continue;
            l = l.substr(k);
            std::string clean;
            for (char ch : l) if (ch != '*' && ch != '`') clean += ch;
            if (clean.size() > 90) clean = clean.substr(0, 87) + "...";
            if (!clean.empty() && l[0] != '|') c.notes.push_back(clean);
        }
        best = c;
    }
    if (!best.v.ok || Cmp(best.v, mine) <= 0) { Say(L"  You have the newest version."); g_state = S_UPTODATE; return 0; }
    Say(L"  %hs is available.", best.pretty.c_str());
    EnterCriticalSection(&g_lock);
    g_rel = best;
    g_canUpdate = !best.tarUrl.empty() && best.sha.size() == 64;
    LeaveCriticalSection(&g_lock);
    g_state = S_AVAILABLE;
    return 0;
}

static void Fail(const char* why) {
    Say(L"  Update failed: %hs", why);
    EnterCriticalSection(&g_lock);
    g_failWhy = why;
    LeaveCriticalSection(&g_lock);
    g_state = S_FAILED;
}

// The new launcher to start once the files are in place ("" = none, still running the old one)
static std::wstring g_restart;

static DWORD WINAPI UpdateThread(LPVOID) {
    EnterCriticalSection(&g_lock);
    Release rel = g_rel;
    LeaveCriticalSection(&g_lock);
    g_step = 0; g_got = 0; g_total = rel.size;
    Say(L"Updating to %hs...", rel.pretty.c_str());
    std::string tar;
    if (!Get(W(rel.tarUrl), tar, 30000, nullptr, OnProgress)) { Fail("The download stopped before it was complete."); return 0; }
    g_step = 1;
    if (_stricmp(Sha256(tar).c_str(), rel.sha.c_str())) { Fail("The downloaded file is damaged (its checksum does not match)."); return 0; }
    std::vector<TarFile> files;
    if (!ReadTar(tar, files)) { Fail("The download does not hold the mod's files."); return 0; }
    bool hasLauncher = false;
    for (auto& f : files) hasLauncher |= !_stricmp(f.name.c_str(), "arcana_launcher.exe");
    Say(L"  Download checked (SHA-256 matches), %u files.", (unsigned)files.size());
    g_step = 2;

    // old files to update-backup\ first; the running launcher can be renamed but not overwritten
    std::wstring bak = g_dir + L"update-backup\\";
    RemoveTree(bak.substr(0, bak.size() - 1));
    CreateDirectoryW(bak.c_str(), nullptr);
    std::wstring self = g_dir + L"arcana_launcher.exe";
    DeleteFileW((self + L".old").c_str());
    std::vector<std::wstring> done;
    bool ok = true;
    for (auto& f : files) {
        std::wstring rel8 = W(f.name);
        for (auto& ch : rel8) if (ch == L'/') ch = L'\\';
        if (!_wcsicmp(rel8.c_str(), L"rollback.ini") && GetFileAttributesW((g_dir + rel8).c_str()) != INVALID_FILE_ATTRIBUTES) continue;   // settings stay
        MakeDirsFor(g_dir, rel8);
        MakeDirsFor(bak, rel8);
        std::wstring dst = g_dir + rel8;
        if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!CopyFileW(dst.c_str(), (bak + rel8).c_str(), FALSE)) { ok = false; break; }
            if (!_wcsicmp(rel8.c_str(), L"arcana_launcher.exe") && !MoveFileExW(dst.c_str(), (dst + L".old").c_str(), MOVEFILE_REPLACE_EXISTING)) { ok = false; break; }
        }
        done.push_back(rel8);
        if (!WriteFileAll(dst, tar.data() + f.off, f.size)) { ok = false; break; }
    }
    if (!ok) {
        for (auto& rel8 : done) {
            if (GetFileAttributesW((bak + rel8).c_str()) != INVALID_FILE_ATTRIBUTES) CopyFileW((bak + rel8).c_str(), (g_dir + rel8).c_str(), FALSE);
            else DeleteFileW((g_dir + rel8).c_str());
        }
        if (GetFileAttributesW(self.c_str()) == INVALID_FILE_ATTRIBUTES) MoveFileExW((self + L".old").c_str(), self.c_str(), 0);
        Fail("A file could not be replaced (is the game still open?). The old files were put back.");
        return 0;
    }
    Say(L"  Updated to %hs (old files in update-backup\\).", rel.pretty.c_str());
    if (hasLauncher) {
        EnterCriticalSection(&g_lock);
        g_restart = self;
        LeaveCriticalSection(&g_lock);
    }
    g_state = S_UPDATED;
    return 0;
}

}  // namespace upd
