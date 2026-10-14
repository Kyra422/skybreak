// LAUNCH19: save backups at start + Dolphin import/export (see ui_saves.h).
#include "ui_saves.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace nwii::ui::saves {

namespace {
namespace fs = std::filesystem;

fs::path U8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string P8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
const char* L(bool en, const char* nl, const char* e) { return en ? e : nl; }

fs::path TitleDir(const fs::path& root) { return root / "title" / kTitleHigh / kTitleLow; }
fs::path BackupRoot(const fs::path& nand) {
    std::error_code ec;
    fs::path abs = fs::absolute(nand, ec);
    if (ec) abs = nand;
    abs = abs.lexically_normal();
    if (!abs.has_filename()) abs = abs.parent_path(); // "C:/x/nand/" -> "C:/x/nand"
    return abs.parent_path() / "reservekopie";
}
bool IsStamp(const std::string& n) { // yyyy-mm-dd_hhmmss (our own rotating backups only)
    if (n.size() != 17 || n[4] != '-' || n[7] != '-' || n[10] != '_') return false;
    for (size_t i = 0; i < n.size(); ++i)
        if (i != 4 && i != 7 && i != 10 && !std::isdigit(static_cast<unsigned char>(n[i]))) return false;
    return true;
}
int CountFiles(const fs::path& dir) {
    std::error_code ec;
    int n = 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) ++n;
    return n;
}
bool SameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(fb));
}
bool SameTree(const fs::path& a, const fs::path& b) { // same relative files with the same bytes
    std::error_code ec;
    int na = 0;
    for (fs::recursive_directory_iterator it(a, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        ++na;
        if (!SameFile(it->path(), b / fs::relative(it->path(), a, ec))) return false;
    }
    return !ec && na == CountFiles(b);
}
// Copies every file of `from` into `to` (overwriting, nothing deleted). Returns files copied, -1 on error.
int CopyTree(const fs::path& from, const fs::path& to, std::string* err) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        if (err) *err = P8(to) + ": " + ec.message();
        return -1;
    }
    int n = 0;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path rel = fs::relative(it->path(), from, ec);
        if (ec) break;
        if (it->is_directory(ec)) {
            fs::create_directories(to / rel, ec);
        } else if (it->is_regular_file(ec)) {
            fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing, ec);
            if (!ec) ++n;
        }
        if (ec) {
            if (err) *err = P8(it->path()) + ": " + ec.message();
            return -1;
        }
    }
    if (ec) {
        if (err) *err = P8(from) + ": " + ec.message();
        return -1;
    }
    return n;
}
std::vector<std::string> StampDirs(const fs::path& root) { // oldest first
    std::vector<std::string> v;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = P8(it->path().filename());
        if (it->is_directory(ec) && IsStamp(n)) v.push_back(n);
    }
    std::sort(v.begin(), v.end());
    return v;
}
} // namespace

std::string Stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d_%H%M%S", &tm);
    return b;
}

Info Inspect(const std::string& nand_root) {
    Info i;
    const fs::path data = TitleDir(U8(nand_root)) / "data";
    i.data_dir = P8(data);
    i.files = CountFiles(data);
    i.found = i.files > 0;
    const fs::path br = BackupRoot(U8(nand_root));
    i.backup_root = P8(br);
    const std::vector<std::string> v = StampDirs(br);
    i.backups = int(v.size());
    if (!v.empty()) i.last_backup = v.back();
    return i;
}

Outcome BackupAtStart(const std::string& nand_root, int keep, bool en, const std::string& stamp) {
    Outcome o;
    if (keep <= 0) {
        o.ok = o.skipped = true;
        o.message = L(en, "Reservekopie bij de start staat uit.", "Backup at start is off.");
        return o;
    }
    const fs::path nand = U8(nand_root);
    const fs::path data = TitleDir(nand) / "data";
    if (CountFiles(data) == 0) {
        o.ok = o.skipped = true;
        o.message = L(en, "Nog geen save: geen reservekopie nodig.", "No save yet: no backup needed.");
        return o;
    }
    const fs::path br = BackupRoot(nand);
    std::vector<std::string> v = StampDirs(br);
    if (!v.empty() && SameTree(data, TitleDir(br / U8(v.back())) / "data")) {
        o.ok = o.skipped = true;
        o.backup = P8(br / U8(v.back()));
        o.message = std::string(L(en, "Save ongewijzigd sinds reservekopie ", "Save unchanged since backup ")) + v.back();
    } else {
        const std::string name = stamp.empty() ? Stamp() : stamp;
        std::string err;
        const fs::path dst = TitleDir(br / U8(name)) / "data";
        const int n = CopyTree(data, dst, &err);
        if (n < 0) {
            o.message = std::string(L(en, "Reservekopie mislukt: ", "Backup failed: ")) + err;
            return o;
        }
        o.ok = true;
        o.files = n;
        o.backup = P8(br / U8(name));
        o.message = std::string(L(en, "Reservekopie gemaakt: ", "Backup made: ")) + name;
        v = StampDirs(br);
    }
    // keep the newest `keep` (only our own yyyy-mm-dd_hhmmss folders)
    std::error_code ec;
    while (int(v.size()) > keep) {
        fs::remove_all(br / U8(v.front()), ec);
        v.erase(v.begin());
    }
    return o;
}

std::string FindDolphinSave(const std::string& picked, bool en, std::string* why) {
    std::error_code ec;
    const fs::path p = U8(picked);
    // candidates: <user>/Wii/title/..., <Wii>/title/..., <title>/00010000/..., the game folder, its data folder
    std::vector<fs::path> cands = {p / "Wii" / "title" / kTitleHigh / kTitleLow / "data",
                                   p / "title" / kTitleHigh / kTitleLow / "data", p / kTitleHigh / kTitleLow / "data",
                                   p / kTitleLow / "data"};
    if (P8(p.filename()) == kTitleLow) cands.push_back(p / "data");
    if (P8(p.filename()) == "data" && P8(p.parent_path().filename()) == kTitleLow) cands.push_back(p);
    for (const fs::path& c : cands)
        if (fs::is_directory(c, ec) && CountFiles(c) > 0) return P8(c);
    // another region / game: explain instead of copying the wrong save
    for (const fs::path& t : {p / "Wii" / "title" / kTitleHigh, p / "title" / kTitleHigh, p / kTitleHigh}) {
        if (!fs::is_directory(t, ec)) continue;
        for (const char* other : {"534f5550", "534f554a", "534f554b"}) // SOUP (Europe), SOUJ (Japan), SOUK (Korea)
            if (fs::is_directory(t / other / "data", ec)) {
                if (why)
                    *why = L(en, "Hier staat alleen een save van een andere versie van Skyward Sword (niet USA / SOUE01). "
                                 "SkyBreak gebruikt alleen de USA-versie.",
                             "Only a save of another version of Skyward Sword is here (not USA / SOUE01).");
                return "";
            }
        if (why) *why = L(en, "In deze Dolphin-map staat geen save van Skyward Sword (USA).", "No Skyward Sword (USA) save in this Dolphin folder.");
        return "";
    }
    if (why)
        *why = L(en, "Dit is geen Dolphin-gebruikersmap: kies de map 'Dolphin Emulator' (met de map Wii erin).",
                 "Not a Dolphin user folder: pick the 'Dolphin Emulator' folder (with the Wii folder in it).");
    return "";
}

Outcome ImportFromDolphin(const std::string& picked, const std::string& nand_root, bool en, const std::string& stamp) {
    Outcome o;
    std::string why;
    const std::string src = FindDolphinSave(picked, en, &why);
    if (src.empty()) {
        o.message = why;
        return o;
    }
    const fs::path nand = U8(nand_root);
    const fs::path data = TitleDir(nand) / "data";
    std::error_code ec;
    if (fs::weakly_canonical(U8(src), ec) == fs::weakly_canonical(data, ec)) {
        o.message = L(en, "Dat is je eigen opslagmap.", "That is your own data folder.");
        return o;
    }
    std::string err;
    if (CountFiles(data) > 0) { // the current save first
        const std::string name = (stamp.empty() ? Stamp() : stamp) + "-voor-import";
        const fs::path bk = BackupRoot(nand) / U8(name);
        if (CopyTree(data, TitleDir(bk) / "data", &err) < 0) {
            o.message = std::string(L(en, "Reservekopie mislukt, niets gekopieerd: ", "Backup failed, nothing copied: ")) + err;
            return o;
        }
        o.backup = P8(bk);
    }
    const int n = CopyTree(U8(src), data, &err);
    if (n < 0) {
        o.message = std::string(L(en, "Kopi\xC3\xABren mislukt: ", "Copy failed: ")) + err;
        return o;
    }
    o.ok = true;
    o.files = n;
    o.message = std::string(L(en, "Save uit Dolphin overgenomen (", "Save imported from Dolphin (")) + std::to_string(n) +
                (n == 1 ? L(en, " bestand).", " file).") : L(en, " bestanden).", " files).")) +
                (o.backup.empty() ? std::string() : std::string(L(en, "\nJe vorige save staat in ", "\nYour previous save is in ")) + o.backup);
    return o;
}

Outcome ExportToDolphin(const std::string& nand_root, const std::string& picked, bool en, const std::string& stamp) {
    Outcome o;
    const fs::path nand = U8(nand_root);
    const fs::path data = TitleDir(nand) / "data";
    if (CountFiles(data) == 0) {
        o.message = L(en, "Er is nog geen save om te exporteren.", "There is no save to export yet.");
        return o;
    }
    std::error_code ec;
    const fs::path p = U8(picked);
    fs::path wii;
    if (fs::is_directory(p / "Wii", ec)) wii = p / "Wii";
    else if (P8(p.filename()) == "Wii" || fs::is_directory(p / "title", ec)) wii = p;
    if (wii.empty()) {
        o.message = L(en, "Dit is geen Dolphin-gebruikersmap: kies de map 'Dolphin Emulator' (met de map Wii erin).",
                      "Not a Dolphin user folder: pick the 'Dolphin Emulator' folder (with the Wii folder in it).");
        return o;
    }
    const fs::path dst = TitleDir(wii) / "data";
    if (fs::weakly_canonical(dst, ec) == fs::weakly_canonical(data, ec)) {
        o.message = L(en, "Dat is je eigen opslagmap.", "That is your own data folder.");
        return o;
    }
    std::string err;
    if (CountFiles(dst) > 0) { // Dolphin's current save first
        const std::string name = (stamp.empty() ? Stamp() : stamp) + "-dolphin-voor-export";
        const fs::path bk = BackupRoot(nand) / U8(name);
        if (CopyTree(dst, TitleDir(bk) / "data", &err) < 0) {
            o.message = std::string(L(en, "Reservekopie mislukt, niets gekopieerd: ", "Backup failed, nothing copied: ")) + err;
            return o;
        }
        o.backup = P8(bk);
    }
    const int n = CopyTree(data, dst, &err);
    if (n < 0) {
        o.message = std::string(L(en, "Kopi\xC3\xABren mislukt: ", "Copy failed: ")) + err;
        return o;
    }
    o.ok = true;
    o.files = n;
    o.message = std::string(L(en, "Save naar Dolphin gekopieerd (", "Save copied to Dolphin (")) + std::to_string(n) +
                (n == 1 ? L(en, " bestand).", " file).") : L(en, " bestanden).", " files).")) +
                (o.backup.empty() ? std::string() : std::string(L(en, "\nDe save die Dolphin had staat in ", "\nDolphin's previous save is in ")) + o.backup);
    return o;
}

} // namespace nwii::ui::saves
