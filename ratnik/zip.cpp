#include "zip.h"

static unsigned int Crc32(const unsigned char* p, size_t n) {
    static unsigned int table[256]; static bool init = false;
    if (!init) {
        for (unsigned int i = 0; i < 256; i++) {
            unsigned int c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    unsigned int c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void Zip::Add(const std::string& name, const Bytes& data) {
    if (data.size() > (1u << 26)) return;  // skip >64 MB entries
    entries_.push_back({ name, data });
}

void Zip::AddText(const std::string& name, const std::string& text) {
    Add(name, Bytes(text.begin(), text.end()));
}

void Zip::AddFile(const std::string& name, const fs::path& path) {
    Bytes b = ReadAll(path);
    if (!b.empty()) Add(name, b);
}

void Zip::AddTree(const std::string& prefix, const fs::path& dir, uint64_t maxBytes) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    uint64_t total = 0;
    for (auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        uint64_t sz = e.file_size(ec);
        if (ec || sz == 0) continue;
        if (total + sz > maxBytes) break;
        fs::path rel = fs::relative(e.path(), dir, ec);
        if (ec) continue;
        Bytes b = ReadAll(e.path());
        if (b.empty()) continue;
        total += b.size();
        Add(prefix + "/" + ToUtf8(rel.wstring()), b);
    }
}

static void Put16(Bytes& b, unsigned short v) {
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)(v >> 8));
}
static void Put32(Bytes& b, unsigned int v) {
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)((v >> 8) & 0xFF));
    b.push_back((unsigned char)((v >> 16) & 0xFF));
    b.push_back((unsigned char)((v >> 24) & 0xFF));
}

bool Zip::Save(const fs::path& path) {
    Bytes out;
    std::vector<unsigned int> offsets;
    SYSTEMTIME st; GetLocalTime(&st);
    unsigned short dosTime = (unsigned short)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    unsigned short dosDate = (unsigned short)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);

    for (auto& e : entries_) {
        offsets.push_back((unsigned int)out.size());
        unsigned int crc = Crc32(e.data.data(), e.data.size());
        Put32(out, 0x04034B50);
        Put16(out, 20);
        Put16(out, 0x0800);            // UTF-8 names
        Put16(out, 0);                 // stored
        Put16(out, dosTime);
        Put16(out, dosDate);
        Put32(out, crc);
        Put32(out, (unsigned int)e.data.size());
        Put32(out, (unsigned int)e.data.size());
        Put16(out, (unsigned short)e.name.size());
        Put16(out, 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), e.data.begin(), e.data.end());
    }

    unsigned int cdStart = (unsigned int)out.size();
    for (size_t i = 0; i < entries_.size(); i++) {
        auto& e = entries_[i];
        unsigned int crc = Crc32(e.data.data(), e.data.size());
        Put32(out, 0x02014B50);
        Put16(out, 20);
        Put16(out, 20);
        Put16(out, 0x0800);
        Put16(out, 0);
        Put16(out, dosTime);
        Put16(out, dosDate);
        Put32(out, crc);
        Put32(out, (unsigned int)e.data.size());
        Put32(out, (unsigned int)e.data.size());
        Put16(out, (unsigned short)e.name.size());
        Put16(out, 0);
        Put16(out, 0);
        Put16(out, 0);
        Put16(out, 0);
        Put32(out, 0);
        Put32(out, offsets[i]);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    unsigned int cdSize = (unsigned int)out.size() - cdStart;

    Put32(out, 0x06054B50);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, (unsigned short)entries_.size());
    Put16(out, (unsigned short)entries_.size());
    Put32(out, cdSize);
    Put32(out, cdStart);
    Put16(out, 0);

    return WriteAll(path, out);
}
