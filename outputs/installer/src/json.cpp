// INSTALL19: minimal JSON (see json.h).
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sb {

namespace {
const Json& NullJson() { static const Json n; return n; }

struct Parser {
    const char* p;
    const char* end;
    std::string err;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    static void put_utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
        else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 0x3F)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    }
    bool hex4(uint32_t& v) {
        if (end - p < 4) return fail("short \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& o) {
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { o += c; continue; }
            if (p >= end) return fail("bad escape");
            char e = *p++;
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                uint32_t v;
                if (!hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(o, v);
                break;
            }
            default: return fail("bad escape");
            }
        }
        if (p >= end) return fail("unterminated string");
        ++p;
        return true;
    }
    bool value(Json& out, int depth) {
        if (depth > 64) return fail("too deep");
        ws();
        if (p >= end) return fail("unexpected end");
        char c = *p;
        if (c == '{') {
            ++p;
            out = Json::Object();
            ws();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                ++p;
                Json v;
                if (!value(v, depth + 1)) return false;
                out.set(k, std::move(v));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            out = Json::Array();
            ws();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                Json v;
                if (!value(v, depth + 1)) return false;
                out.push(std::move(v));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!string(s)) return false;
            out = Json(std::move(s));
            return true;
        }
        if (end - p >= 4 && !strncmp(p, "true", 4)) { p += 4; out = Json(true); return true; }
        if (end - p >= 5 && !strncmp(p, "false", 5)) { p += 5; out = Json(false); return true; }
        if (end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::string num;
            while (p < end && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) num += *p++;
            char* e = nullptr;
            double v = strtod(num.c_str(), &e);
            if (!e || *e) return fail("bad number");
            out = Json(v);
            return true;
        }
        return fail("unexpected character");
    }
};

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += char(c);
            }
        }
    }
    out += '"';
}
} // namespace

bool Json::has(const std::string& k) const {
    if (t_ != Type::Object) return false;
    for (auto& kv : o_) if (kv.first == k) return true;
    return false;
}

const Json& Json::operator[](const std::string& k) const {
    if (t_ == Type::Object)
        for (auto& kv : o_) if (kv.first == k) return kv.second;
    return NullJson();
}

Json& Json::set(const std::string& k, Json v) {
    if (t_ != Type::Object) *this = Object();
    for (auto& kv : o_) if (kv.first == k) { kv.second = std::move(v); return kv.second; }
    o_.emplace_back(k, std::move(v));
    return o_.back().second;
}

void Json::erase(const std::string& k) {
    for (size_t i = 0; i < o_.size(); ++i)
        if (o_[i].first == k) { o_.erase(o_.begin() + long(i)); return; }
}

std::string Json::str(const std::string& k, const std::string& def) const {
    const Json& v = (*this)[k];
    return v.is_string() ? v.s_ : def;
}
double Json::num(const std::string& k, double def) const {
    const Json& v = (*this)[k];
    return v.is_number() ? v.n_ : def;
}
bool Json::boolean(const std::string& k, bool def) const {
    const Json& v = (*this)[k];
    return v.is_bool() ? v.b_ : (v.is_number() ? v.n_ != 0 : def);
}

const Json& Json::at(size_t i) const {
    if (t_ == Type::Array && i < a_.size()) return a_[i];
    return NullJson();
}

std::vector<std::string> Json::strings() const {
    std::vector<std::string> r;
    for (auto& e : a_) if (e.is_string()) r.push_back(e.s_);
    return r;
}

Json Json::Parse(const std::string& text, std::string* err) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    // skip a UTF-8 BOM
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    Json out;
    if (!ps.value(out, 0)) {
        if (err) *err = ps.err;
        return Json();
    }
    ps.ws();
    if (ps.p != ps.end) {
        if (err) *err = "trailing characters";
        return Json();
    }
    if (err) err->clear();
    return out;
}

void Json::dump(std::string& out, int indent, int level) const {
    auto nl = [&](int lv) {
        if (indent < 0) return;
        out += '\n';
        out.append(size_t(indent * lv), ' ');
    };
    switch (t_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: {
        char b[64];
        if (std::isfinite(n_) && n_ == std::floor(n_) && std::fabs(n_) < 9.0e15) snprintf(b, sizeof b, "%.0f", n_);
        else snprintf(b, sizeof b, "%.6g", n_);
        out += b;
        break;
    }
    case Type::String: escape(out, s_); break;
    case Type::Array:
        out += '[';
        for (size_t i = 0; i < a_.size(); ++i) {
            if (i) out += ',';
            nl(level + 1);
            a_[i].dump(out, indent, level + 1);
        }
        if (!a_.empty()) nl(level);
        out += ']';
        break;
    case Type::Object:
        out += '{';
        for (size_t i = 0; i < o_.size(); ++i) {
            if (i) out += ',';
            nl(level + 1);
            escape(out, o_[i].first);
            out += indent < 0 ? ":" : ": ";
            o_[i].second.dump(out, indent, level + 1);
        }
        if (!o_.empty()) nl(level);
        out += '}';
        break;
    }
}

std::string Json::Dump(int indent) const {
    std::string out;
    dump(out, indent, 0);
    return out;
}

} // namespace sb
