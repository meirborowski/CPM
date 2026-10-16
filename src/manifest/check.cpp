#include "manifest/check.hpp"

#include <algorithm>
#include <format>

#include "json/parse.hpp"

namespace cpm::manifest {

Diagnostic Checker::make(Severity sev, json::Span span, std::string message, std::string hint) const {
    Diagnostic d;
    d.severity = sev;
    d.message = std::move(message);
    d.hint = std::move(hint);
    d.loc = json::locate(text_, span, file_);
    return d;
}

void Checker::error(json::Span span, std::string message, std::string hint) {
    errors_.emplace_back(span.begin, make(Severity::Error, span, std::move(message), std::move(hint)));
}

void Checker::warning(json::Span span, std::string message, std::string hint) {
    warnings_.emplace_back(span.begin, make(Severity::Warning, span, std::move(message), std::move(hint)));
}

void Checker::type_error(const json::Value& v, std::string_view what, std::string_view expected) {
    error(v.span(), std::format("{} must be {}, found {}", what, expected, v.type_name()));
}

std::optional<std::string> Checker::string(const json::Value& obj, std::string_view key) {
    const json::Value* v = obj.find(key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_string()) {
        type_error(*v, std::format("\"{}\"", key), "a string");
        return std::nullopt;
    }
    return v->as_string();
}

std::optional<bool> Checker::boolean(const json::Value& obj, std::string_view key) {
    const json::Value* v = obj.find(key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_bool()) {
        type_error(*v, std::format("\"{}\"", key), "true or false");
        return std::nullopt;
    }
    return v->as_bool();
}

std::optional<std::vector<std::string>> Checker::strings(const json::Value& obj, std::string_view key) {
    const json::Value* v = obj.find(key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_array()) {
        type_error(*v, std::format("\"{}\"", key), "an array of strings");
        return std::nullopt;
    }
    std::vector<std::string> out;
    bool ok = true;
    for (const auto& item : v->as_array()) {
        if (!item.is_string()) {
            type_error(item, std::format("each entry of \"{}\"", key), "a string");
            ok = false;
            continue;
        }
        out.push_back(item.as_string());
    }
    if (!ok) return std::nullopt;
    return out;
}

const json::Value* Checker::object(const json::Value& obj, std::string_view key) {
    const json::Value* v = obj.find(key);
    if (v == nullptr) return nullptr;
    if (!v->is_object()) {
        type_error(*v, std::format("\"{}\"", key), "an object");
        return nullptr;
    }
    return v;
}

void Checker::unknown_keys(const json::Value& obj, std::span<const std::string_view> known, std::string_view where) {
    if (!obj.is_object()) return;
    for (const auto& m : obj.as_object()) {
        if (std::find(known.begin(), known.end(), m.key) != known.end()) continue;
        if (m.key == "$schema") continue;
        std::string suggestion = closest_match(m.key, known);
        warning(m.key_span, std::format("unknown key \"{}\" in {}", m.key, where),
                suggestion.empty() ? std::string("it is ignored") : std::format("did you mean \"{}\"?", suggestion));
    }
}

namespace {
std::vector<Diagnostic> sorted(std::vector<std::pair<std::uint32_t, Diagnostic>> items) {
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Diagnostic> out;
    for (auto& [pos, d] : items) out.push_back(std::move(d));
    return out;
}
}  // namespace

std::vector<Diagnostic> Checker::take_all() {
    auto all = std::move(errors_);
    for (auto& w : warnings_) all.push_back(std::move(w));
    errors_.clear();
    warnings_.clear();
    return sorted(std::move(all));
}

std::vector<Diagnostic> Checker::take_warnings() {
    auto w = std::move(warnings_);
    warnings_.clear();
    return sorted(std::move(w));
}

std::string closest_match(std::string_view word, std::span<const std::string_view> candidates) {
    auto distance = [](std::string_view a, std::string_view b) {
        std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
        for (std::size_t i = 1; i <= a.size(); ++i) {
            cur[0] = i;
            for (std::size_t j = 1; j <= b.size(); ++j) {
                std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
                cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            }
            std::swap(prev, cur);
        }
        return prev[b.size()];
    };
    std::string best;
    std::size_t best_d = 3;
    for (auto c : candidates) {
        std::size_t d = distance(word, c);
        if (d < best_d) {
            best_d = d;
            best = std::string(c);
        }
    }
    return best;
}

}  // namespace cpm::manifest
