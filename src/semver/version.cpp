#include "semver/version.hpp"

#include <charconv>

#include "core/strings.hpp"

namespace cpm::semver {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_ident_char(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

bool all_digits(std::string_view s) {
    for (char c : s) {
        if (!is_digit(c)) return false;
    }
    return !s.empty();
}

std::optional<std::uint64_t> parse_numeric(std::string_view s) {
    if (!all_digits(s)) return std::nullopt;
    if (s.size() > 1 && s[0] == '0') return std::nullopt;
    std::uint64_t value = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return value;
}

bool valid_identifiers(std::string_view s, bool numeric_no_leading_zero) {
    for (const auto& id : str::split(s, '.')) {
        if (id.empty()) return false;
        for (char c : id) {
            if (!is_ident_char(c)) return false;
        }
        if (numeric_no_leading_zero && all_digits(id) && id.size() > 1 && id[0] == '0') return false;
    }
    return true;
}

std::strong_ordering compare_identifier(const std::string& a, const std::string& b) {
    bool an = all_digits(a);
    bool bn = all_digits(b);
    if (an && bn) {
        // No leading zeros, so longer means larger; equal length compares lexically.
        if (a.size() != b.size()) return a.size() <=> b.size();
        return a.compare(b) <=> 0;
    }
    if (an) return std::strong_ordering::less;  // numeric identifiers have lower precedence
    if (bn) return std::strong_ordering::greater;
    return a.compare(b) <=> 0;
}

}  // namespace

std::optional<Version> Version::parse(std::string_view text) {
    Version v;
    std::string_view core = text;
    if (auto plus = core.find('+'); plus != std::string_view::npos) {
        std::string_view build = core.substr(plus + 1);
        if (!valid_identifiers(build, false)) return std::nullopt;
        v.build = std::string(build);
        core = core.substr(0, plus);
    }
    if (auto dash = core.find('-'); dash != std::string_view::npos) {
        std::string_view pre = core.substr(dash + 1);
        if (!valid_identifiers(pre, true)) return std::nullopt;
        v.pre = str::split(pre, '.');
        core = core.substr(0, dash);
    }
    auto parts = str::split(core, '.');
    if (parts.size() != 3) return std::nullopt;
    auto major = parse_numeric(parts[0]);
    auto minor = parse_numeric(parts[1]);
    auto patch = parse_numeric(parts[2]);
    if (!major || !minor || !patch) return std::nullopt;
    v.major = *major;
    v.minor = *minor;
    v.patch = *patch;
    return v;
}

std::string Version::to_string() const {
    std::string s = std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
    if (!pre.empty()) s += '-' + str::join(pre, ".");
    if (!build.empty()) s += '+' + build;
    return s;
}

std::strong_ordering compare(const Version& a, const Version& b) {
    if (auto c = a.major <=> b.major; c != 0) return c;
    if (auto c = a.minor <=> b.minor; c != 0) return c;
    if (auto c = a.patch <=> b.patch; c != 0) return c;
    // A version without prerelease identifiers has higher precedence.
    if (a.pre.empty() && b.pre.empty()) return std::strong_ordering::equal;
    if (a.pre.empty()) return std::strong_ordering::greater;
    if (b.pre.empty()) return std::strong_ordering::less;
    std::size_t n = std::min(a.pre.size(), b.pre.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (auto c = compare_identifier(a.pre[i], b.pre[i]); c != 0) return c;
    }
    return a.pre.size() <=> b.pre.size();
}

}  // namespace cpm::semver
