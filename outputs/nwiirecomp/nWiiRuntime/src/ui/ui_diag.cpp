// LAUNCH19: diagnostics package (see ui_diag.h).
#include "ui_diag.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "ui_saves.h" // Stamp()

namespace nwii::ui::diag {

namespace {
namespace fs = std::filesystem;
fs::path U8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string P8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
std::string ReadTail(const fs::path& f, size_t max_bytes) {
    std::ifstream in(f, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff from = size > std::streamoff(max_bytes) ? size - std::streamoff(max_bytes) : 0;
    in.seekg(from);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (from > 0) s = "[... first " + std::to_string(from) + " bytes left out ...]\n" + s;
    return s;
}
void Le16(std::string& o, uint32_t v) { o += char(v & 255); o += char((v >> 8) & 255); }
void Le32(std::string& o, uint32_t v) { Le16(o, v & 0xFFFF); Le16(o, v >> 16); }
} // namespace

uint32_t Crc32(const std::string& bytes) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : bytes) c = table[(c ^ b) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool WriteStoredZip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& entries,
                    std::string* err) {
    std::string out, central;
    const uint32_t dos_time = 0, dos_date = (2026 - 1980) << 9 | 1 << 5 | 1; // fixed: the files are text snapshots
    for (const auto& [name, data] : entries) {
        const uint32_t crc = Crc32(data), off = uint32_t(out.size());
        // local header
        Le32(out, 0x04034b50); Le16(out, 20); Le16(out, 0x0800); Le16(out, 0); Le16(out, dos_time); Le16(out, dos_date);
        Le32(out, crc); Le32(out, uint32_t(data.size())); Le32(out, uint32_t(data.size()));
        Le16(out, uint32_t(name.size())); Le16(out, 0);
        out += name;
        out += data;
        // central directory entry
        Le32(central, 0x02014b50); Le16(central, 20); Le16(central, 20); Le16(central, 0x0800); Le16(central, 0);
        Le16(central, dos_time); Le16(central, dos_date); Le32(central, crc); Le32(central, uint32_t(data.size()));
        Le32(central, uint32_t(data.size())); Le16(central, uint32_t(name.size())); Le16(central, 0); Le16(central, 0);
        Le16(central, 0); Le16(central, 0); Le32(central, 0); Le32(central, off);
        central += name;
    }
    const uint32_t cd_off = uint32_t(out.size());
    out += central;
    Le32(out, 0x06054b50); Le16(out, 0); Le16(out, 0); Le16(out, uint32_t(entries.size())); Le16(out, uint32_t(entries.size()));
    Le32(out, uint32_t(central.size())); Le32(out, cd_off); Le16(out, 0);
    const fs::path tmp = U8(path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(out.data(), std::streamsize(out.size()))) {
            if (err) *err = "cannot write " + path;
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, U8(path), ec);
    if (ec && err) *err = ec.message();
    return !ec;
}

std::string MakePackage(const Request& r, std::string* err) {
    std::vector<std::pair<std::string, std::string>> e;
    e.push_back({"systeeminfo.txt", r.system_info});
    std::error_code ec;
    if (!r.settings_path.empty() && fs::is_regular_file(U8(r.settings_path), ec))
        e.push_back({"instellingen.toml", ReadTail(U8(r.settings_path), 1 << 20)});
    std::vector<fs::path> logs;
    for (fs::directory_iterator it(U8(r.log_dir), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".log") logs.push_back(it->path());
    std::sort(logs.begin(), logs.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code e1, e2;
        return fs::last_write_time(a, e1) > fs::last_write_time(b, e2);
    });
    if (logs.size() > 5) logs.resize(5);
    for (const fs::path& l : logs) e.push_back({"logs/" + P8(l.filename()), ReadTail(l, 4u << 20)});
    fs::create_directories(U8(r.out_dir), ec);
    const std::string zip =
        P8(U8(r.out_dir) / U8("SkyBreak-diagnose-" + (r.stamp.empty() ? saves::Stamp() : r.stamp) + ".zip"));
    if (!WriteStoredZip(zip, e, err)) return {};
    return zip;
}

} // namespace nwii::ui::diag
