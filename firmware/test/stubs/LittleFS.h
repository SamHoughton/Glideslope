#pragma once
// An in-memory file system for the native tests (8.1): files are byte
// vectors in a map, so code that saves and loads (the logbook, the day's
// tally) can be tested. It starts empty, so loading saved state starts
// clean as before; LittleFSStub::wipe() empties it between tests.
#include <Arduino.h>
#include <map>
#include <string>
#include <vector>

struct MemFiles
{
    std::map<std::string, std::vector<uint8_t>> files;
};
inline MemFiles &memFiles()
{
    static MemFiles m;
    return m;
}

class File
{
public:
    File() = default;
    File(const std::string &path, bool append) : _path(path), _ok(true)
    {
        if (append) _pos = memFiles().files[_path].size();
    }
    explicit operator bool() const { return _ok; }
    size_t read(uint8_t *buf, size_t n)
    {
        if (!_ok) return 0;
        const std::vector<uint8_t> &d = memFiles().files[_path];
        const size_t k = _pos < d.size() ? std::min(n, d.size() - _pos) : 0;
        if (k) memcpy(buf, d.data() + _pos, k);
        _pos += k;
        return k;
    }
    size_t write(const uint8_t *buf, size_t n)
    {
        if (!_ok) return 0;
        std::vector<uint8_t> &d = memFiles().files[_path];
        if (d.size() < _pos + n) d.resize(_pos + n);
        memcpy(d.data() + _pos, buf, n);
        _pos += n;
        return n;
    }
    size_t write(uint8_t c) { return write(&c, 1); }
    bool seek(uint32_t pos) { _pos = pos; return _ok; }
    size_t size() const { return _ok ? memFiles().files[_path].size() : 0; }
    size_t position() const { return _pos; }
    int available() { return _ok ? (int)(size() > _pos ? size() - _pos : 0) : 0; }
    size_t print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t println(const char *s) { return print(s) + print("\n"); }
    size_t println(const String &s) { return println(s.c_str()); }
    String readStringUntil(char end)
    {
        String out;
        uint8_t c;
        while (read(&c, 1) == 1 && (char)c != end) out += (char)c;
        return out;
    }
    void close() { _ok = false; }

private:
    std::string _path;
    size_t      _pos = 0;
    bool        _ok = false;
};

struct LittleFSStub
{
    File open(const char *path, const char *mode = "r")
    {
        auto &f = memFiles().files;
        const bool exists = f.count(path) > 0;
        if (mode[0] == 'r' && !exists) return File();
        if (mode[0] == 'w') f[path].clear();
        return File(path, mode[0] == 'a');
    }
    File open(const String &p, const char *m = "r") { return open(p.c_str(), m); }
    bool exists(const char *path) { return memFiles().files.count(path) > 0; }
    bool exists(const String &p) { return exists(p.c_str()); }
    bool remove(const char *path) { return memFiles().files.erase(path) > 0; }
    bool rename(const char *a, const char *b)
    {
        auto &f = memFiles().files;
        if (!f.count(a)) return false;
        f[b] = f[a];
        f.erase(a);
        return true;
    }
    bool mkdir(const char *) { return true; }
    bool truncate(const char *path, size_t len)
    {
        auto &f = memFiles().files;
        if (!f.count(path) || f[path].size() < len) return false;
        f[path].resize(len);
        return true;
    }
    // Space as LittleFS counts it: whole 4 KB blocks a file, on the board's
    // 1.5 MB partition; extraUsed stands in for everything else on it.
    size_t total = 1572864, extraUsed = 0;
    size_t totalBytes() { return total; }
    size_t usedBytes()
    {
        size_t n = extraUsed;
        for (const auto &kv : memFiles().files) n += (kv.second.size() + 4095) / 4096 * 4096;
        return n;
    }
    void wipe() { memFiles().files.clear(); extraUsed = 0; }
};
inline LittleFSStub LittleFS;
