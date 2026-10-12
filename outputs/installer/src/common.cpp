// INSTALL19: shared helpers (see common.h).
#include "common.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace sb {

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string U(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string U(const fs::path& p) { return U(p.wstring()); }
fs::path P(const std::string& s) { return fs::path(W(s)); }

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
bool StartsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool EndsWith(const std::string& s, const std::string& x) { return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0; }
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> r;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); r.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) { if (cur.back() == '\r') cur.pop_back(); r.push_back(cur); }
    return r;
}
std::string Format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return {};
    if (size_t(n) < sizeof buf) return std::string(buf, size_t(n));
    std::string big(size_t(n) + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(big.data(), big.size(), fmt, ap);
    va_end(ap);
    big.resize(size_t(n));
    return big;
}
std::string Replace(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}

bool ReadText(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), std::streamsize(text.size()));
        if (!f) return false;
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool AppendText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::app);
    if (!f) return false;
    f.write(text.data(), std::streamsize(text.size()));
    return bool(f);
}

namespace {
struct Sha256Ctx {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    ULONG len = 32;
    explicit Sha256Ctx(LPCWSTR algo = BCRYPT_SHA256_ALGORITHM, ULONG digest = 32) : len(digest) { // INSTALL19 10 Oct: also MD5 (python.org listing)
        if (BCryptOpenAlgorithmProvider(&alg, algo, nullptr, 0) != 0) return;
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) return;
        ok = true;
    }
    ~Sha256Ctx() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void update(const void* d, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(d);
        while (n) {
            ULONG c = ULONG(n > 0x40000000 ? 0x40000000 : n);
            BCryptHashData(h, PUCHAR(p), c, 0);
            p += c;
            n -= c;
        }
    }
    std::string hex() {
        uint8_t out[32];
        if (len > 32 || BCryptFinishHash(h, out, len, 0) != 0) return {};
        static const char* d = "0123456789abcdef";
        std::string s;
        for (ULONG i = 0; i < len; ++i) { s += d[out[i] >> 4]; s += d[out[i] & 15]; }
        return s;
    }
};
} // namespace

std::string Sha256Bytes(const void* data, size_t n) {
    Sha256Ctx c;
    if (!c.ok) return {};
    c.update(data, n);
    return c.hex();
}

static std::string HashFile(const fs::path& p, LPCWSTR algo, ULONG digest) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    Sha256Ctx c(algo, digest);
    std::vector<uint8_t> buf(1 << 20);
    DWORD got = 0;
    bool ok = c.ok;
    while (ok && ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got) c.update(buf.data(), got);
    CloseHandle(f);
    return ok ? c.hex() : std::string();
}
std::string Sha256File(const fs::path& p) { return HashFile(p, BCRYPT_SHA256_ALGORITHM, 32); }
std::string Md5File(const fs::path& p) { return HashFile(p, BCRYPT_MD5_ALGORITHM, 16); } // INSTALL19 10 Oct

std::string NowStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return Format("%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
std::string NowIso() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return Format("%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

fs::path Normal(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) a = p;
    return a.lexically_normal();
}

bool IsInside(const fs::path& p, const fs::path& dir) {
    std::wstring a = Normal(p).wstring(), b = Normal(dir).wstring();
    while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
    if (a.size() < b.size()) return false;
    if (CompareStringOrdinal(a.c_str(), int(b.size()), b.c_str(), int(b.size()), TRUE) != CSTR_EQUAL) return false;
    return a.size() == b.size() || a[b.size()] == L'\\' || a[b.size()] == L'/';
}

fs::path ExePath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return fs::path(std::wstring(buf, n));
}
fs::path ExeDir() { return ExePath().parent_path(); }

namespace {
std::mutex g_log_mu;
fs::path g_log;
} // namespace

void SetLogFile(const fs::path& p) {
    std::lock_guard<std::mutex> l(g_log_mu);
    g_log = p;
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
}
fs::path LogFile() {
    std::lock_guard<std::mutex> l(g_log_mu);
    return g_log;
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::string line = "[" + NowIso() + "] " + buf + "\n";
    OutputDebugStringW(W(line).c_str());
    std::lock_guard<std::mutex> l(g_log_mu);
    if (!g_log.empty()) {
        std::ofstream f(g_log, std::ios::binary | std::ios::app);
        f << line;
    }
}

uint64_t DirSize(const fs::path& dir) {
    std::error_code ec;
    uint64_t total = 0;
    if (!fs::exists(dir, ec)) return 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

int CompareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& s) {
        std::vector<int> v;
        int cur = 0;
        bool any = false;
        for (char c : s) {
            if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); any = true; }
            else if (c == '.') { v.push_back(cur); cur = 0; any = false; }
            else break;
        }
        if (any || !v.empty()) v.push_back(cur);
        return v;
    };
    auto x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

} // namespace sb
