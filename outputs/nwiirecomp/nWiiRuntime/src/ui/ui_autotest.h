#pragma once
// UIT14: scripted driver for the launcher, the settings screens and the in-game menu, so these flows run
// without a human (NWII_UI_AUTOTEST=<script file>; ui_app.cpp implements the Host with real SDL input).
// Header-only and free of SDL/ImGui: the parser and the step sequencing are unit-tested with a fake Host
// (outputs/pc-tests/uit14).
//
// Script: one step per line, words separated by blanks, "double quotes" group words, # starts a comment.
//   focus                          give the window keyboard/mouse focus (as the player's click on it would)
//   wait <s>                       wait <s> seconds
//   wait_until <q> <op> <v> <s>    poll query <q> until it compares true (else FAIL after <s> seconds)
//   expect <q> <op> <v> [tol]      check query <q> once (op: == != < <= > >= ~ (contains) approx (|a-b|<=tol))
//   key <k> | keydown <k> | keyup <k>       keyboard (k: esc f1 f11 enter up down a ..., combos alt+enter)
//   pad <b> | paddown <b> | padup <b>        controller button event, as INPUT13's controller thread posts it
//   move <nx> <ny>                 mouse to a window position (0..1)
//   click <label> | click_at <nx> <ny>       left click on a widget (by its label) or a window position
//   mousedown <n> | mouseup <n>    mouse button n (1 left, 2 middle, 3 right) without moving
//   do <args...>                   host action (focus, resize W H, fullscreen 0/1, toast <kind> <name>, shot <png>, quit;
//                                  CTRL18-UI: park = real cursor out of the window, refocus = keyboard focus only,
//                                  wheel <n> = mouse wheel at the virtual cursor, scroll_to <label> = scroll the
//                                  page so that widget <label> is centred)
//   mark <label>                   log a timestamped mark (presented-fps windows are cut at marks)
//   log <text...>
//   remember <name> <q>            CTRL18-UI: store query <q>'s current value as $name; "$name" (or "$query") as the
//                                  value of expect / wait_until compares against it (scripts independent of the
//                                  player's starting settings)
// Every input step is followed by a short pause (step_s, default 0.12 s) so the host loop polls the events
// and the UI runs at least one frame between a press and its release.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map> // CTRL18-UI
#include <string>
#include <utility>
#include <vector>

namespace nwii::ui::autotest {

struct Step {
    int line = 0;
    std::string op;
    std::vector<std::string> args;
    std::string text;
};

inline std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, have = false;
    for (char c : line) {
        if (c == '"') {
            quoted = !quoted;
            have = true;
            continue;
        }
        if (!quoted && c == '#') break;
        if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (have) out.push_back(cur);
            cur.clear();
            have = false;
            continue;
        }
        cur += c;
        have = true;
    }
    if (have) out.push_back(cur);
    return out;
}

// Accepted ops and their argument counts (min, max; -1 = any).
inline bool OpArity(const std::string& op, int& lo, int& hi) {
    struct A { const char* op; int lo, hi; };
    static const A kOps[] = {
        {"focus", 0, 0},    {"wait", 1, 1},     {"wait_until", 4, 4}, {"expect", 3, 4},  {"key", 1, 1},
        {"keydown", 1, 1},  {"keyup", 1, 1},    {"pad", 1, 1},        {"paddown", 1, 1}, {"padup", 1, 1},
        {"move", 2, 2},     {"click", 1, 1},    {"click_at", 2, 2},   {"mousedown", 1, 1}, {"mouseup", 1, 1},
        {"do", 1, -1},      {"mark", 1, 1},     {"log", 0, -1},       {"remember", 2, 2}, // CTRL18-UI
    };
    for (const A& a : kOps)
        if (op == a.op) {
            lo = a.lo;
            hi = a.hi;
            return true;
        }
    return false;
}

inline bool Parse(const std::string& text, std::vector<Step>& out, std::string* err) {
    out.clear();
    int line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        ++line_no;
        std::vector<std::string> t = Tokenize(line);
        if (t.empty()) continue;
        Step s;
        s.line = line_no;
        s.op = t[0];
        s.args.assign(t.begin() + 1, t.end());
        s.text = line;
        while (!s.text.empty() && (s.text.back() == '\r' || s.text.back() == ' ')) s.text.pop_back();
        int lo = 0, hi = 0;
        if (!OpArity(s.op, lo, hi)) {
            if (err) *err = "line " + std::to_string(line_no) + ": unknown step '" + s.op + "'";
            return false;
        }
        const int n = int(s.args.size());
        if (n < lo || (hi >= 0 && n > hi)) {
            if (err) *err = "line " + std::to_string(line_no) + ": '" + s.op + "' takes " + std::to_string(lo) +
                            (hi == lo ? "" : hi < 0 ? "+" : "-" + std::to_string(hi)) + " argument(s)";
            return false;
        }
        out.push_back(std::move(s));
    }
    return true;
}

inline bool ToNumber(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    if (end && *end == '\0') return true;
    // hex masks: 0x...
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        v = double(std::strtoull(s.c_str() + 2, &end, 16));
        return end && *end == '\0';
    }
    return false;
}

// got <op> want. Numbers compare numerically when both sides are numbers; "~" = substring;
// "approx" = |got - want| <= tol; "&" = (got & want) != 0, "!&" = (got & want) == 0 (button masks).
inline bool Compare(const std::string& got, const std::string& op, const std::string& want, double tol = 0.0) {
    double a = 0, b = 0;
    const bool num = ToNumber(got, a) && ToNumber(want, b);
    if (op == "~") return got.find(want) != std::string::npos;
    if (op == "!~") return got.find(want) == std::string::npos;
    if (op == "approx") return num && std::fabs(a - b) <= tol;
    if (op == "&" || op == "!&") {
        if (!num) return false;
        const bool any = (static_cast<unsigned long long>(a) & static_cast<unsigned long long>(b)) != 0;
        return op == "&" ? any : !any;
    }
    if (num) {
        if (op == "==") return a == b;
        if (op == "!=") return a != b;
        if (op == "<") return a < b;
        if (op == "<=") return a <= b;
        if (op == ">") return a > b;
        if (op == ">=") return a >= b;
        return false;
    }
    if (op == "==") return got == want;
    if (op == "!=") return got != want;
    return false;
}

// What the driver drives and observes.
struct Host {
    virtual ~Host() = default;
    virtual double Now() = 0;
    virtual bool Key(const std::string& name, bool down) = 0; // false: unknown key name
    virtual void MouseMove(float nx, float ny) = 0;            // window position 0..1
    virtual void MouseButton(int button, bool down) = 0;
    virtual bool Pad(const std::string& name, bool down) = 0;  // false: unknown button name
    virtual bool WidgetCenter(const std::string& label, float& nx, float& ny) = 0; // last UI frame
    virtual std::string Query(const std::string& what) = 0;    // "?" = unknown query
    virtual std::string Do(const std::vector<std::string>& args) = 0; // "" = ok, else the error
    virtual void Log(const std::string& line) = 0;
};

class Runner {
public:
    explicit Runner(std::vector<Step> steps, double step_s = 0.12) : steps_(std::move(steps)), step_s_(step_s) {}

    // Runs every step that is due; false once the script has finished (the result was logged).
    bool Tick(Host& h) {
        const double now = h.Now();
        for (int guard = 0; guard < 10000; ++guard) {
            if (!acts_.empty()) {
                if (acts_.front().at > now) return true;
                Act a = std::move(acts_.front());
                acts_.pop_front();
                a.fn(h);
                continue;
            }
            if (now < resume_at_) return true;
            if (polling_) {
                const std::string got = h.Query(poll_.args[0]);
                if (Compare(got, poll_.args[1], Value(h, poll_.args[2]))) {
                    Pass(h, poll_, got);
                    polling_ = false;
                } else if (now >= poll_deadline_) {
                    Fail(h, poll_, "timed out, last value " + got);
                    polling_ = false;
                } else {
                    return true;
                }
                continue;
            }
            if (next_ >= steps_.size()) {
                if (!done_) {
                    done_ = true;
                    h.Log("RESULT pass=" + std::to_string(pass_) + " fail=" + std::to_string(fail_));
                }
                return false;
            }
            Start(steps_[next_++], h, now);
        }
        return true;
    }
    int passed() const { return pass_; }
    int failed() const { return fail_; }
    bool done() const { return done_; }
    size_t next_step() const { return next_; }
    size_t step_count() const { return steps_.size(); }

private:
    struct Act {
        double at;
        std::function<void(Host&)> fn;
    };

    void Later(double at, std::function<void(Host&)> fn) { acts_.push_back({at, std::move(fn)}); }
    void Pass(Host& h, const Step& s, const std::string& got) {
        ++pass_;
        h.Log("PASS line " + std::to_string(s.line) + ": " + s.text + "  (got " + got + ")");
    }
    void Fail(Host& h, const Step& s, const std::string& why) {
        ++fail_;
        h.Log("FAIL line " + std::to_string(s.line) + ": " + s.text + "  (" + why + ")");
    }
    static std::vector<std::string> SplitCombo(const std::string& k) {
        std::vector<std::string> parts;
        size_t a = 0;
        while (a <= k.size()) {
            const size_t p = k.find('+', a);
            parts.push_back(k.substr(a, p == std::string::npos ? std::string::npos : p - a));
            if (p == std::string::npos) break;
            a = p + 1;
        }
        return parts;
    }

    void Start(const Step& s, Host& h, double now) {
        const double d = step_s_;
        const std::string& op = s.op;
        if (op == "wait") {
            double v = 0;
            ToNumber(s.args[0], v);
            resume_at_ = now + v;
        } else if (op == "wait_until") {
            double v = 0;
            ToNumber(s.args[3], v);
            poll_ = s;
            polling_ = true;
            poll_deadline_ = now + v;
        } else if (op == "expect") {
            double tol = 0;
            if (s.args.size() > 3) ToNumber(s.args[3], tol);
            const std::string got = h.Query(s.args[0]);
            if (got == "?") Fail(h, s, "unknown query");
            else if (Compare(got, s.args[1], Value(h, s.args[2]), tol)) Pass(h, s, got);
            else Fail(h, s, "got " + got);
        } else if (op == "key" || op == "keydown" || op == "keyup") {
            const std::vector<std::string> keys = SplitCombo(s.args[0]);
            const bool down = op != "keyup", up = op != "keydown";
            if (down)
                for (const std::string& k : keys)
                    if (!h.Key(k, true)) { Fail(h, s, "unknown key " + k); return; }
            const double t_up = down && up ? now + d : now;
            if (up)
                Later(t_up, [keys, s, this](Host& hh) {
                    for (size_t i = keys.size(); i-- > 0;)
                        if (!hh.Key(keys[i], false)) Fail(hh, s, "unknown key " + keys[i]);
                });
            resume_at_ = t_up + d;
        } else if (op == "pad" || op == "paddown" || op == "padup") {
            const std::string b = s.args[0];
            const bool down = op != "padup", up = op != "paddown";
            if (down && !h.Pad(b, true)) { Fail(h, s, "unknown button " + b); return; }
            const double t_up = down && up ? now + d : now;
            if (up) Later(t_up, [b, s, this](Host& hh) { if (!hh.Pad(b, false)) Fail(hh, s, "unknown button " + b); });
            resume_at_ = t_up + d;
        } else if (op == "move") {
            double x = 0, y = 0;
            ToNumber(s.args[0], x);
            ToNumber(s.args[1], y);
            h.MouseMove(float(x), float(y));
            resume_at_ = now + d;
        } else if (op == "click" || op == "click_at") {
            float x = 0, y = 0;
            if (op == "click") {
                if (!h.WidgetCenter(s.args[0], x, y)) { Fail(h, s, "widget not on screen"); return; }
            } else {
                double a = 0, b = 0;
                ToNumber(s.args[0], a);
                ToNumber(s.args[1], b);
                x = float(a);
                y = float(b);
            }
            h.MouseMove(x, y);
            Later(now + d, [x, y](Host& hh) { hh.MouseMove(x, y); hh.MouseButton(1, true); });
            Later(now + 2 * d, [x, y](Host& hh) { hh.MouseMove(x, y); hh.MouseButton(1, false); });
            resume_at_ = now + 3 * d;
        } else if (op == "mousedown" || op == "mouseup") {
            double b = 1;
            ToNumber(s.args[0], b);
            h.MouseButton(int(b), op == "mousedown");
            resume_at_ = now + d;
        } else if (op == "do" || op == "focus") {
            std::vector<std::string> a = op == "focus" ? std::vector<std::string>{"focus"} : s.args;
            const std::string e = h.Do(a);
            if (!e.empty()) Fail(h, s, e);
            resume_at_ = now + d;
        } else if (op == "remember") { // CTRL18-UI
            vars_[s.args[0]] = h.Query(s.args[1]);
            h.Log("REMEMBER " + s.args[0] + "=" + vars_[s.args[0]]);
        } else if (op == "mark") {
            h.Log("MARK " + s.args[0] + " t=" + std::to_string(now) + " frames=" + h.Query("frames"));
        } else if (op == "log") {
            std::string t;
            for (const std::string& a : s.args) // "$query" = that query's value, e.g. log ptr $gptr_x $gptr_y
                t += (t.empty() ? "" : " ") + (a.size() > 1 && a[0] == '$' ? a.substr(1) + "=" + h.Query(a.substr(1)) : a);
            h.Log("LOG " + t);
        }
    }

    // CTRL18-UI: "$name" = a remembered value, "$query" = that query now; anything else literally.
    std::string Value(Host& h, const std::string& v) {
        if (v.size() < 2 || v[0] != '$') return v;
        const auto it = vars_.find(v.substr(1));
        return it != vars_.end() ? it->second : h.Query(v.substr(1));
    }

    std::map<std::string, std::string> vars_; // CTRL18-UI: remember
    std::vector<Step> steps_;
    double step_s_;
    size_t next_ = 0;
    std::deque<Act> acts_;
    double resume_at_ = 0;
    bool polling_ = false;
    Step poll_;
    double poll_deadline_ = 0;
    int pass_ = 0, fail_ = 0;
    bool done_ = false;
};

} // namespace nwii::ui::autotest
