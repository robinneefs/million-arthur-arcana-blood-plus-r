// Build 115: update check (included into launcher.cpp).
//
// Each start, the launcher asks GitHub for the mod's releases. When one is newer than this
// version it asks the player (Update / Not now). Update downloads the release zip, checks its
// SHA-256 against the one GitHub lists for the file, unpacks it and copies the new files over
// the old ones. rollback.ini (settings, My setup) is kept. The old files go to
// update-backup\ first and come back if anything fails. Then the new launcher starts.
// "Not now" starts the game as usual; the main menu then shows "update available".
//
// rollback.ini [update]: check=0 turns it off; repo=owner/name and test_version=x.y.z are for testing.

#include <winhttp.h>
#include <bcrypt.h>
#include <map>
#include <memory>

namespace upd {

static std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string A(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

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
            DWORD n = 0;
            char buf[65536];
            ok = code == 200;
            while (ok && WinHttpReadData(r, buf, sizeof buf, &n) && n) out.append(buf, n);
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

// ---- files ----
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
static bool WriteFileAll(const std::wstring& p, const std::string& d) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    return fclose(f) == 0 && ok;
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
static void ListFiles(const std::wstring& root, const std::wstring& rel, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((root + rel + L"*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = f.cFileName;
        if (n == L"." || n == L"..") continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(root, rel + n + L"\\", out);
        else out.push_back(rel + n);
    } while (FindNextFileW(h, &f));
    FindClose(h);
}
// the unpacked folder that holds the mod (the zip has one folder in it)
static std::wstring FindModRoot(const std::wstring& d, int depth) {
    if (FileExists(d + L"\\arcana_rollback.dll")) return d;
    if (depth >= 2) return L"";
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &f);
    std::wstring r;
    if (h == INVALID_HANDLE_VALUE) return r;
    do {
        std::wstring n = f.cFileName;
        if (n != L"." && n != L".." && (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) r = FindModRoot(d + L"\\" + n, depth + 1);
    } while (r.empty() && FindNextFileW(h, &f));
    FindClose(h);
    return r;
}
static bool RunHidden(std::wstring cmd) {
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}
static bool Unzip(const std::wstring& zip, const std::wstring& to) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    // Windows 10 1803+ has tar.exe, which also unpacks zip files; else PowerShell
    if (FileExists(std::wstring(sys) + L"\\tar.exe") &&
        RunHidden(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + to + L"\"")) return true;
    return RunHidden(L"powershell.exe -NoProfile -NonInteractive -Command \"Expand-Archive -LiteralPath '" + zip + L"' -DestinationPath '" + to + L"' -Force\"");
}

struct Release { std::string tag, notes, zipName, zipUrl, sha; Ver v; };

// Returns true when the launcher should exit because the updated one was started.
static bool CheckAndUpdate(const std::wstring& dir, int argc, wchar_t** argv) {
    std::wstring ini = dir + L"rollback.ini";
    DeleteFileW((dir + L"arcana_launcher.exe.old").c_str());     // left from the last update
    if (!GetPrivateProfileIntW(L"update", L"check", 1, ini.c_str())) return false;
    for (int i = 1; i < argc; i++) if (_wcsicmp(argv[i], L"--updated") == 0) return false;

    wchar_t repo[128], tv[32];
    GetPrivateProfileStringW(L"update", L"repo", L"robinneefs/million-arthur-arcana-blood-plus-r", repo, 128, ini.c_str());
    GetPrivateProfileStringW(L"update", L"test_version", L"", tv, 32, ini.c_str());
    Ver mine = ParseVer(tv[0] ? A(tv) : std::string(MOD_VERSION));

    Say(L"Checking for a newer version...");
    std::string body;
    if (!Get(std::wstring(L"https://api.github.com/repos/") + repo + L"/releases?per_page=20", body, 5000, L"application/vnd.github+json")) {
        Say(L"  Could not reach GitHub, skipped.");
        return false;
    }
    J list;
    JP jp{body.data(), body.data() + body.size()};
    if (!jp.val(list) || list.t != J::ARR) { Say(L"  Could not read the release list, skipped."); return false; }
    Release best;
    for (const J& r : list.a) {
        if (r["draft"].b) continue;
        Release c;
        c.tag = r["tag_name"].s;
        c.v = ParseVer(c.tag);
        if (!c.v.ok) continue;
        if (best.v.ok && Cmp(c.v, best.v) <= 0) continue;
        c.notes = r["body"].s;
        std::string shaFile;
        for (const J& a : r["assets"].a) {
            const std::string& n = a["name"].s;
            if (n.size() > 4 && !_stricmp(n.c_str() + n.size() - 4, ".zip") && c.zipUrl.empty()) {
                c.zipName = n; c.zipUrl = a["browser_download_url"].s;
                const std::string& d = a["digest"].s;          // "sha256:<hex>" (GitHub lists it for each file)
                if (!d.compare(0, 7, "sha256:")) c.sha = d.substr(7);
            }
        }
        for (const J& a : r["assets"].a)                        // or a "<zip>.sha256" file next to it
            if (!c.zipName.empty() && a["name"].s == c.zipName + ".sha256") shaFile = a["browser_download_url"].s;
        if (c.zipUrl.empty()) continue;
        if (c.sha.empty() && !shaFile.empty()) {
            std::string t;
            if (Get(W(shaFile), t, 5000)) c.sha = t.substr(0, t.find_first_of(" \t\r\n"));
        }
        best = c;
    }
    if (!best.v.ok || Cmp(best.v, mine) <= 0) { Say(L"  You have the newest version."); return false; }

    std::string ver = best.tag[0] == 'v' || best.tag[0] == 'V' ? best.tag.substr(1) : best.tag;
    Say(L"  Version %hs is available.", ver.c_str());
    SetEnvironmentVariableW(L"ARCANA_UPDATE", W(ver).c_str());   // the main menu says so if the player picks "Not now"
    std::string notes = best.notes;
    for (size_t i; (i = notes.find('\r')) != std::string::npos;) notes.erase(i, 1);
    if (notes.size() > 900) { notes.resize(900); notes += "..."; }
    std::wstring msg = L"A new version of Arcana Blood +R is available.\n\nYou have:  " + W(MOD_VERSION) + L"\nNew:  " + W(ver) +
                       (notes.empty() ? L"" : L"\n\nWhat's new:\n" + W(notes)) +
                       L"\n\nInstall it now? Your settings and My setup are kept.\n(Yes = update now, No = not now, start the game as it is.)";
    if (MessageBoxW(nullptr, msg.c_str(), L"Arcana Blood +R - update", MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND) != IDYES) {
        Say(L"  Not now. Starting the game as it is.");
        return false;
    }

    auto fail = [&](const wchar_t* why) {
        Say(L"  Update failed: %ls", why);
        MessageBoxW(nullptr, (std::wstring(L"The update did not work:\n") + why + L"\n\nNothing was changed. The game starts with the version you have.").c_str(),
                    L"Arcana Blood +R - update", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    };
    if (best.sha.size() != 64) return fail(L"GitHub did not list a checksum for the download, so it can't be checked.");
    Say(L"  Downloading %hs...", best.zipName.c_str());
    std::string zip;
    if (!Get(W(best.zipUrl), zip, 60000)) return fail(L"The download did not finish.");
    std::string got = Sha256(zip);
    if (_stricmp(got.c_str(), best.sha.c_str())) return fail(L"The downloaded file is damaged (checksum does not match).");
    Say(L"  Download checked (SHA-256 matches).");

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"ArcanaBloodPlusR-update";
    RemoveTree(work);
    CreateDirectoryW(work.c_str(), nullptr);
    std::wstring zipPath = work + L"\\update.zip", out = work + L"\\files";
    CreateDirectoryW(out.c_str(), nullptr);
    if (!WriteFileAll(zipPath, zip)) return fail(L"Could not save the download.");
    if (!Unzip(zipPath, out)) return fail(L"Could not unpack the download.");
    std::wstring src = FindModRoot(out, 0);
    if (src.empty()) return fail(L"The download does not contain the mod.");
    src += L"\\";

    // copy: keep the player's rollback.ini; old files go to update-backup\ first
    std::vector<std::wstring> files;
    ListFiles(src, L"", files);
    std::wstring bak = dir + L"update-backup\\";
    RemoveTree(bak.substr(0, bak.size() - 1));
    CreateDirectoryW(bak.c_str(), nullptr);
    std::vector<std::wstring> done;     // replaced files (rel), for undoing
    std::wstring self = dir + L"arcana_launcher.exe";
    bool ok = true;
    for (auto& rel : files) {
        if (!_wcsicmp(rel.c_str(), L"rollback.ini") && FileExists(dir + rel)) continue;
        for (size_t i = rel.find(L'\\'); i != std::wstring::npos; i = rel.find(L'\\', i + 1)) {
            CreateDirectoryW((dir + rel.substr(0, i)).c_str(), nullptr);
            CreateDirectoryW((bak + rel.substr(0, i)).c_str(), nullptr);
        }
        std::wstring dst = dir + rel;
        if (FileExists(dst)) {
            // a running exe can be renamed but not overwritten: the launcher itself moves aside
            bool isSelf = !_wcsicmp(rel.c_str(), L"arcana_launcher.exe");
            if (!CopyFileW(dst.c_str(), (bak + rel).c_str(), FALSE)) { ok = false; break; }
            if (isSelf && !MoveFileExW(dst.c_str(), (dst + L".old").c_str(), MOVEFILE_REPLACE_EXISTING)) { ok = false; break; }
        }
        done.push_back(rel);
        if (!CopyFileW((src + rel).c_str(), dst.c_str(), FALSE)) { ok = false; break; }
    }
    if (!ok) {
        for (auto& rel : done) {
            if (FileExists(bak + rel)) CopyFileW((bak + rel).c_str(), (dir + rel).c_str(), FALSE);
            else DeleteFileW((dir + rel).c_str());
        }
        if (!FileExists(self) && FileExists(self + L".old")) MoveFileExW((self + L".old").c_str(), self.c_str(), 0);
        RemoveTree(work);
        return fail(L"A file could not be replaced (is the game still open?). The old files were put back.");
    }
    RemoveTree(work);
    Say(L"  Updated to %hs (old files in update-backup\\). Starting the new launcher...", ver.c_str());
    SetEnvironmentVariableW(L"ARCANA_UPDATE", nullptr);

    std::wstring cmd = L"\"" + self + L"\" --updated";
    for (int i = 1; i < argc; i++) cmd += std::wstring(L" \"") + argv[i] + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(self.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        Say(L"  Could not start the new launcher.");
        MessageBoxW(nullptr, L"The update is installed, but the new launcher did not start. Start arcana_launcher.exe again.", L"Arcana Blood +R - update", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

}  // namespace upd
