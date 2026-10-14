// UI13: SHA-1 (FIPS 180-1) and the launcher's disc check (see ui_disc.h).
#include "ui_disc.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace nwii::ui {

namespace {
inline uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
} // namespace

Sha1::Sha1() : h_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u} {}

void Sha1::Block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::Update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len_ += n;
    while (n > 0) {
        const size_t take = std::min(n, sizeof buf_ - used_);
        std::memcpy(buf_ + used_, p, take);
        used_ += take; p += take; n -= take;
        if (used_ == 64) { Block(buf_); used_ = 0; }
    }
}

std::string Sha1::HexDigest() {
    const uint64_t bits = len_ * 8;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (used_ != 56) Update(&zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = uint8_t(bits >> (56 - 8 * i));
    Update(lenb, 8);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (uint32_t v : h_)
        for (int i = 7; i >= 0; --i) s.push_back(hex[(v >> (4 * i)) & 15]);
    return s;
}

std::string Sha1File(const std::string& path_utf8) {
    std::ifstream f(u8path(path_utf8), std::ios::binary);
    if (!f) return {};
    Sha1 h;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        if (f.gcount() > 0) h.Update(buf.data(), size_t(f.gcount()));
    }
    return h.HexDigest();
}

DiscStatus CheckDisc(const std::string& game_dir, const char* expected_dol, const char* expected_rels) {
    DiscStatus st;
    const std::filesystem::path root = u8path(game_dir);
    {
        std::ifstream b(root / "sys" / "boot.bin", std::ios::binary);
        char hdr[8] = {};
        if (b && b.read(hdr, 8)) {
            st.game_id.assign(hdr, 6);
            st.revision = (unsigned char)hdr[7];
        }
    }
    const std::u8string du = (root / "sys" / "main.dol").u8string();
    const std::string dol(du.begin(), du.end());
    const std::u8string ru = (root / "files" / "rels.arc").u8string();
    const std::string rels(ru.begin(), ru.end());
    if (st.game_id.empty()) {
        st.message = "Geen spelbestanden gevonden (sys/boot.bin ontbreekt).";
        return st;
    }
    const char* region = st.game_id.size() >= 4 ? (st.game_id[3] == 'E' ? "NTSC-U" : st.game_id[3] == 'P' ? "PAL"
                                                    : st.game_id[3] == 'J' ? "NTSC-J" : "?") : "?";
    st.detail = "Wii \xC2\xB7 " + st.game_id + " (" + region + ") \xC2\xB7 revisie " + std::to_string(st.revision);
    if (st.game_id != "SOUE01" || st.revision != 0) {
        st.message = "Andere schijf (" + st.game_id + " rev. " + std::to_string(st.revision) +
                     "): alleen SOUE01 revisie 0 wordt ondersteund.";
        return st;
    }
    const std::string dol_sha = Sha1File(dol);
    st.dol_ok = !dol_sha.empty() && dol_sha == (expected_dol ? expected_dol : kMainDolSha1);
    std::error_code ec;
    const bool rels_present = std::filesystem::is_regular_file(u8path(rels), ec);
    const std::string rels_sha = rels_present ? Sha1File(rels) : std::string();
    st.rels_ok = rels_present && rels_sha == (expected_rels ? expected_rels : kRelsArcSha1);
    if (dol_sha.empty()) st.message = "sys/main.dol ontbreekt.";
    else if (!st.dol_ok) st.message = "sys/main.dol wijkt af (SHA-1 klopt niet): schijf beschadigd of aangepast.";
    else if (!rels_present) st.message = "files/rels.arc ontbreekt.";
    else if (!st.rels_ok) st.message = "files/rels.arc wijkt af (SHA-1 klopt niet).";
    else { st.ready = true; st.message = "Schijf gereed."; }
    return st;
}

} // namespace nwii::ui
