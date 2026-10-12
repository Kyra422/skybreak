#pragma once
// INSTALL19: minimal JSON value (parse + dump) for install.json, answers, mock prereqs, registry.json and the
// build script's progress lines. Our own code; no third-party dependency.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sb {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : t_(Type::Bool), b_(b) {}
    Json(int v) : t_(Type::Number), n_(v) {}
    Json(int64_t v) : t_(Type::Number), n_(double(v)) {}
    Json(uint64_t v) : t_(Type::Number), n_(double(v)) {}
    Json(double v) : t_(Type::Number), n_(v) {}
    Json(const char* s) : t_(Type::String), s_(s ? s : "") {}
    Json(std::string s) : t_(Type::String), s_(std::move(s)) {}

    static Json Array() { Json j; j.t_ = Type::Array; return j; }
    static Json Object() { Json j; j.t_ = Type::Object; return j; }

    Type type() const { return t_; }
    bool is_null() const { return t_ == Type::Null; }
    bool is_object() const { return t_ == Type::Object; }
    bool is_array() const { return t_ == Type::Array; }
    bool is_string() const { return t_ == Type::String; }
    bool is_number() const { return t_ == Type::Number; }
    bool is_bool() const { return t_ == Type::Bool; }

    // Typed reads with defaults (never throw).
    bool as_bool(bool def = false) const { return t_ == Type::Bool ? b_ : (t_ == Type::Number ? n_ != 0 : def); }
    double as_number(double def = 0) const { return t_ == Type::Number ? n_ : def; }
    int as_int(int def = 0) const { return t_ == Type::Number ? int(n_) : def; }
    const std::string& as_string() const { static const std::string e; return t_ == Type::String ? s_ : e; }

    // Object access.
    bool has(const std::string& k) const;
    const Json& operator[](const std::string& k) const; // null when missing
    Json& set(const std::string& k, Json v);              // insert or replace, returns the stored value
    void erase(const std::string& k);
    const std::vector<std::pair<std::string, Json>>& items() const { return o_; }
    std::string str(const std::string& k, const std::string& def = "") const;
    double num(const std::string& k, double def = 0) const;
    int integer(const std::string& k, int def = 0) const { return int(num(k, def)); }
    bool boolean(const std::string& k, bool def = false) const;

    // Array access.
    size_t size() const { return t_ == Type::Array ? a_.size() : (t_ == Type::Object ? o_.size() : 0); }
    const Json& at(size_t i) const;
    void push(Json v) { if (t_ != Type::Array) { *this = Array(); } a_.push_back(std::move(v)); }
    const std::vector<Json>& elements() const { return a_; }
    std::vector<std::string> strings() const; // array of strings (others skipped)

    static Json Parse(const std::string& text, std::string* err = nullptr);
    std::string Dump(int indent = -1) const;

private:
    void dump(std::string& out, int indent, int level) const;
    Type t_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> a_;
    std::vector<std::pair<std::string, Json>> o_;
};

} // namespace sb
