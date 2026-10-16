#include "json/write.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

#include "core/error.hpp"

namespace cpm::json {

std::string quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

namespace {

void write_value(std::string& out, const Value& v, const WriteOptions& opt, int depth);

void newline(std::string& out, const WriteOptions& opt, int depth) {
    if (opt.compact) return;
    out += '\n';
    out.append(static_cast<std::size_t>(opt.indent * depth), ' ');
}

void write_double(std::string& out, double d) {
    if (!std::isfinite(d)) throw Error("cannot write a non-finite number as JSON");
    char buf[64];
    auto [p, ec] = std::to_chars(buf, buf + sizeof buf, d);
    std::string_view s(buf, static_cast<std::size_t>(p - buf));
    out += s;
    // Keep the value a JSON number that reads back as a double.
    if (s.find_first_of(".eE") == std::string_view::npos) out += ".0";
}

void write_value(std::string& out, const Value& v, const WriteOptions& opt, int depth) {
    switch (v.type()) {
        case Type::Null: out += "null"; return;
        case Type::Bool: out += v.as_bool() ? "true" : "false"; return;
        case Type::Int: out += std::to_string(v.as_int()); return;
        case Type::Double: write_double(out, v.as_double()); return;
        case Type::String: out += quote(v.as_string()); return;
        case Type::Array: {
            const Array& arr = v.as_array();
            if (arr.empty()) {
                out += "[]";
                return;
            }
            out += '[';
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (i != 0) out += ',';
                newline(out, opt, depth + 1);
                write_value(out, arr[i], opt, depth + 1);
            }
            newline(out, opt, depth);
            out += ']';
            return;
        }
        case Type::Object: {
            const Object& obj = v.as_object();
            if (obj.empty()) {
                out += "{}";
                return;
            }
            std::vector<const Member*> members;
            members.reserve(obj.size());
            for (const auto& m : obj) members.push_back(&m);
            if (opt.sort_keys) {
                std::sort(members.begin(), members.end(),
                          [](const Member* a, const Member* b) { return a->key < b->key; });
            }
            out += '{';
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (i != 0) out += ',';
                newline(out, opt, depth + 1);
                out += quote(members[i]->key);
                out += opt.compact ? ":" : ": ";
                write_value(out, members[i]->value, opt, depth + 1);
            }
            newline(out, opt, depth);
            out += '}';
            return;
        }
    }
}

}  // namespace

std::string write(const Value& value, const WriteOptions& options) {
    std::string out;
    write_value(out, value, options, 0);
    return out;
}

}  // namespace cpm::json
