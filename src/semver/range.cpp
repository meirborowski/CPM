#include "semver/range.hpp"

#include <algorithm>
#include <charconv>

#include "core/strings.hpp"

namespace cpm::semver {

namespace {

// A possibly incomplete version from a range: missing components are wildcards ("1.2", "1.x", "*").
struct Partial {
    std::optional<std::uint64_t> major, minor, patch;
    std::vector<std::string> pre;
    bool full() const { return major && minor && patch; }
};

std::optional<std::uint64_t> number(std::string_view s) {
    if (s.empty() || (s.size() > 1 && s[0] == '0')) return std::nullopt;
    std::uint64_t value = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return value;
}

std::optional<Partial> parse_partial(std::string_view s) {
    if (s.empty()) return std::nullopt;
    std::size_t cut = s.find_first_of("-+");
    std::vector<std::string> parts = str::split(s.substr(0, cut), '.');
    if (parts.size() > 3) return std::nullopt;
    Partial p;
    std::optional<std::uint64_t>* slots[3] = {&p.major, &p.minor, &p.patch};
    bool wildcard = false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "x" || parts[i] == "X" || parts[i] == "*") {
            wildcard = true;
            continue;
        }
        if (wildcard) return std::nullopt;  // "1.x.3"
        auto n = number(parts[i]);
        if (!n) return std::nullopt;
        *slots[i] = n;
    }
    if (cut != std::string_view::npos) {
        if (!p.full()) return std::nullopt;  // prerelease or build only on a full version
        auto v = Version::parse(s);
        if (!v) return std::nullopt;
        p.pre = v->pre;
    }
    return p;
}

Version make(std::uint64_t major, std::uint64_t minor, std::uint64_t patch, std::vector<std::string> pre = {}) {
    Version v;
    v.major = major;
    v.minor = minor;
    v.patch = patch;
    v.pre = std::move(pre);
    return v;
}

// The smallest possible version of major.minor.patch: "-0" sorts before every other prerelease.
Version floor_of(std::uint64_t major, std::uint64_t minor, std::uint64_t patch) {
    return make(major, minor, patch, {"0"});
}

Version filled(const Partial& p) { return make(p.major.value_or(0), p.minor.value_or(0), p.patch.value_or(0), p.pre); }

}  // namespace

bool Range::in(const Interval& i, const Version& v) {
    if (i.nothing) return false;
    if (i.lo) {
        auto c = compare(v, i.lo->version);
        if (c < 0 || (c == 0 && !i.lo->inclusive)) return false;
    }
    if (i.hi) {
        auto c = compare(v, i.hi->version);
        if (c > 0 || (c == 0 && !i.hi->inclusive)) return false;
    }
    if (v.is_prerelease()) {
        std::array<std::uint64_t, 3> tuple = {v.major, v.minor, v.patch};
        return std::find(i.prerelease_ok.begin(), i.prerelease_ok.end(), tuple) != i.prerelease_ok.end();
    }
    return true;
}

Range::Interval Range::intersect(const Interval& a, const Interval& b) {
    Interval r;
    r.nothing = a.nothing || b.nothing;
    auto tighter_lo = [](const std::optional<Bound>& x, const std::optional<Bound>& y) -> std::optional<Bound> {
        if (!x) return y;
        if (!y) return x;
        auto c = compare(x->version, y->version);
        if (c != 0) return c > 0 ? x : y;
        return Bound{x->version, x->inclusive && y->inclusive};
    };
    auto tighter_hi = [](const std::optional<Bound>& x, const std::optional<Bound>& y) -> std::optional<Bound> {
        if (!x) return y;
        if (!y) return x;
        auto c = compare(x->version, y->version);
        if (c != 0) return c < 0 ? x : y;
        return Bound{x->version, x->inclusive && y->inclusive};
    };
    r.lo = tighter_lo(a.lo, b.lo);
    r.hi = tighter_hi(a.hi, b.hi);
    return r;
}

std::optional<Range> Range::parse(std::string_view input) {
    Range range;
    range.text_ = std::string(str::trim(input));

    // "1.2 || >=3" -> comparator sets
    std::vector<std::string> sets;
    std::string_view rest = range.text_;
    while (true) {
        std::size_t bar = rest.find("||");
        sets.emplace_back(str::trim(rest.substr(0, bar)));
        if (bar == std::string_view::npos) break;
        rest = rest.substr(bar + 2);
    }

    for (const std::string& set : sets) {
        Interval interval;
        std::vector<std::array<std::uint64_t, 3>> pre_tuples;
        auto note_pre = [&](const Partial& p) {
            if (!p.pre.empty()) pre_tuples.push_back({*p.major, *p.minor, *p.patch});
        };
        auto apply = [&](const Interval& comparator) { interval = intersect(interval, comparator); };

        if (auto dash = set.find(" - "); dash != std::string::npos) {
            // Hyphen range: "A - B"
            auto a = parse_partial(str::trim(std::string_view(set).substr(0, dash)));
            auto b = parse_partial(str::trim(std::string_view(set).substr(dash + 3)));
            if (!a || !b) return std::nullopt;
            Interval c;
            if (a->major) c.lo = Bound{filled(*a), true};
            if (b->full()) c.hi = Bound{filled(*b), true};
            else if (b->major && !b->minor) c.hi = Bound{floor_of(*b->major + 1, 0, 0), false};
            else if (b->major) c.hi = Bound{floor_of(*b->major, *b->minor + 1, 0), false};
            note_pre(*a);
            note_pre(*b);
            apply(c);
        } else {
            // Space-separated comparators; an operator may be separated from its version by spaces.
            auto is_operator = [](std::string_view s) {
                return s == ">" || s == ">=" || s == "<" || s == "<=" || s == "=" || s == "^" || s == "~" || s == "~>";
            };
            std::vector<std::string> tokens;
            for (const auto& raw : str::split(set, ' ')) {
                std::string tok(str::trim(raw));
                if (tok.empty()) continue;
                if (!tokens.empty() && is_operator(tokens.back())) tokens.back() += tok;
                else tokens.push_back(tok);
            }
            for (const std::string& tok : tokens) {
                std::string_view t = tok;
                std::string op;
                for (std::string_view candidate : {"<=", ">=", "~>", "<", ">", "=", "^", "~"}) {
                    if (t.starts_with(candidate)) {
                        op = std::string(candidate == "~>" ? "~" : candidate);
                        t.remove_prefix(candidate.size());
                        break;
                    }
                }
                auto p = parse_partial(t);
                if (!p) return std::nullopt;
                note_pre(*p);
                Interval c;
                const std::uint64_t M = p->major.value_or(0);
                const std::uint64_t m = p->minor.value_or(0);
                if (op.empty() || op == "=") {
                    if (p->full()) {
                        c.lo = Bound{filled(*p), true};
                        c.hi = Bound{filled(*p), true};
                    } else if (p->major && !p->minor) {
                        c.lo = Bound{make(M, 0, 0), true};
                        c.hi = Bound{floor_of(M + 1, 0, 0), false};
                    } else if (p->major) {
                        c.lo = Bound{make(M, m, 0), true};
                        c.hi = Bound{floor_of(M, m + 1, 0), false};
                    }
                } else if (op == "^") {
                    if (p->major) {
                        c.lo = Bound{filled(*p), true};
                        if (M > 0 || !p->minor) c.hi = Bound{floor_of(M + 1, 0, 0), false};
                        else if (m > 0 || !p->patch) c.hi = Bound{floor_of(0, m + 1, 0), false};
                        else c.hi = Bound{floor_of(0, 0, *p->patch + 1), false};
                    }
                } else if (op == "~") {
                    if (p->major) {
                        c.lo = Bound{filled(*p), true};
                        c.hi = p->minor ? Bound{floor_of(M, m + 1, 0), false} : Bound{floor_of(M + 1, 0, 0), false};
                    }
                } else if (op == ">") {
                    if (!p->major) c.nothing = true;
                    else if (p->full()) c.lo = Bound{filled(*p), false};
                    else if (!p->minor) c.lo = Bound{make(M + 1, 0, 0), true};
                    else c.lo = Bound{make(M, m + 1, 0), true};
                } else if (op == ">=") {
                    if (p->major) c.lo = Bound{filled(*p), true};
                } else if (op == "<") {
                    if (!p->major) c.nothing = true;
                    else if (p->full()) c.hi = Bound{filled(*p), false};
                    else c.hi = Bound{floor_of(M, m, 0), false};
                } else if (op == "<=") {
                    if (p->full()) c.hi = Bound{filled(*p), true};
                    else if (p->major && !p->minor) c.hi = Bound{floor_of(M + 1, 0, 0), false};
                    else if (p->major) c.hi = Bound{floor_of(M, m + 1, 0), false};
                }
                apply(c);
            }
        }
        interval.prerelease_ok = std::move(pre_tuples);
        range.sets_.push_back(std::move(interval));
    }
    return range;
}

Range Range::any() {
    Range r;
    r.sets_.push_back(Interval{});
    r.text_ = "*";
    return r;
}

Range Range::exactly(const Version& v) {
    Range r;
    Interval i;
    i.lo = Bound{v, true};
    i.hi = Bound{v, true};
    if (v.is_prerelease()) i.prerelease_ok.push_back({v.major, v.minor, v.patch});
    r.sets_.push_back(std::move(i));
    r.text_ = v.to_string();
    return r;
}

bool Range::contains(const Version& v) const {
    return std::any_of(sets_.begin(), sets_.end(), [&](const Interval& i) { return in(i, v); });
}

Range Range::intersect(const Range& other) const {
    Range r;
    for (const auto& a : sets_) {
        for (const auto& b : other.sets_) {
            Interval i = intersect(a, b);
            for (const auto& t : a.prerelease_ok) {
                if (std::find(b.prerelease_ok.begin(), b.prerelease_ok.end(), t) != b.prerelease_ok.end())
                    i.prerelease_ok.push_back(t);
            }
            r.sets_.push_back(std::move(i));
        }
    }
    r.text_ = text_ + " and " + other.text_;
    return r;
}

}  // namespace cpm::semver
