#pragma once
#include "common.h"

struct ZipEntry {
    std::string name;
    Bytes data;
};

class Zip {
public:
    void Add(const std::string& name, const Bytes& data);
    void AddText(const std::string& name, const std::string& text);
    void AddFile(const std::string& name, const fs::path& path);
    void AddTree(const std::string& prefix, const fs::path& dir, uint64_t maxBytes);
    bool Save(const fs::path& path);
    size_t Count() const { return entries_.size(); }
private:
    std::vector<ZipEntry> entries_;
};
