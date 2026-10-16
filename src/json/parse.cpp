#include "json/parse.hpp"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>

#include "core/strings.hpp"

namespace cpm::json {

namespace {

// `token` is already a valid JSON number. Apple's libc++ has no floating-point std::from_chars yet (it does
// not define __cpp_lib_to_chars), so strtod stands in there: CPM never changes the C locale, so the decimal
// point is always '.'.
std::optional<double> to_double(std::string_view token) {
#if defined(__cpp_lib_to_chars)
    double value = 0;
    auto [p, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc() || p != token.data() + token.size()) return std::nullopt;
    return value;
#else
    std::string copy(token);
    char* end = nullptr;
    errno = 0;
    double value = std::strtod(copy.c_str(), &end);
    if (errno == ERANGE || end != copy.c_str() + copy.size()) return std::nullopt;
    return value;
#endif
}

class Parser {
public:
    Parser(std::string_view text, const ParseOptions& options) : text_(text), opt_(options) {
        if (text_.starts_with("\xEF\xBB\xBF")) pos_ = 3;  // UTF-8 BOM
    }

    Value parse_document() {
        skip_space();
        if (at_end()) fail("expected a JSON value but the document is empty");
        Value v = parse_value(0);
        skip_space();
        if (!at_end()) fail("unexpected content after the JSON value");
        return v;
    }

private:
    std::string_view text_;
    ParseOptions opt_;
    std::size_t pos_ = 0;

    bool at_end() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }

    [[noreturn]] void fail(std::string message) const { fail_at(std::move(message), pos_); }
    [[noreturn]] void fail_at(std::string message, std::size_t at) const {
        throw ParseError(std::move(message), static_cast<std::uint32_t>(std::min(at, text_.size())));
    }

    std::string describe_current() const {
        if (at_end()) return "end of input";
        char c = peek();
        if (static_cast<unsigned char>(c) < 0x20) return std::format("character 0x{:02x}", static_cast<int>(c));
        return std::format("'{}'", c);
    }

    void skip_space() {
        while (!at_end()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
                if (!opt_.comments) fail("comments are not allowed here");
                if (text_[pos_ + 1] == '/') {
                    while (!at_end() && peek() != '\n') ++pos_;
                } else {
                    std::size_t start = pos_;
                    std::size_t end = text_.find("*/", pos_ + 2);
                    if (end == std::string_view::npos) fail_at("unterminated block comment", start);
                    pos_ = end + 2;
                }
            } else {
                break;
            }
        }
    }

    Value parse_value(std::size_t depth) {
        if (depth > opt_.max_depth) fail("JSON nesting is too deep");
        if (at_end()) fail("expected a value but reached the end of input");
        std::size_t start = pos_;
        Value v;
        switch (peek()) {
            case '{': v = parse_object(depth); break;
            case '[': v = parse_array(depth); break;
            case '"': v = Value(parse_string()); break;
            case 't': expect_word("true"); v = Value(true); break;
            case 'f': expect_word("false"); v = Value(false); break;
            case 'n': expect_word("null"); v = Value(nullptr); break;
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                    v = parse_number();
                } else {
                    fail(std::format("unexpected {}; expected a value", describe_current()));
                }
        }
        v.set_span({static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(pos_)});
        return v;
    }

    void expect_word(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) fail(std::format("unexpected {}; expected a value", describe_current()));
        pos_ += word.size();
    }

    Value parse_object(std::size_t depth) {
        ++pos_;  // '{'
        Object obj;
        skip_space();
        if (!at_end() && peek() == '}') {
            ++pos_;
            return Value(std::move(obj));
        }
        while (true) {
            skip_space();
            if (at_end() || peek() != '"') {
                if (!at_end() && peek() == '}' && opt_.trailing_commas && !obj.empty()) {
                    ++pos_;
                    break;
                }
                fail(std::format("unexpected {}; expected a string key", describe_current()));
            }
            std::size_t key_start = pos_;
            std::string key = parse_string();
            Span key_span{static_cast<std::uint32_t>(key_start), static_cast<std::uint32_t>(pos_)};
            for (const auto& m : obj) {
                if (m.key == key) fail_at(std::format("duplicate key \"{}\"", key), key_start);
            }
            skip_space();
            if (at_end() || peek() != ':') fail(std::format("unexpected {}; expected ':' after the key", describe_current()));
            ++pos_;
            skip_space();
            Value value = parse_value(depth + 1);
            obj.push_back(Member{std::move(key), std::move(value), key_span});
            skip_space();
            if (at_end()) fail("unterminated object; expected ',' or '}'");
            if (peek() == ',') {
                ++pos_;
                skip_space();
                if (!at_end() && peek() == '}') {
                    if (!opt_.trailing_commas) fail("trailing commas are not allowed here");
                    ++pos_;
                    break;
                }
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                break;
            }
            fail(std::format("unexpected {}; expected ',' or '}}'", describe_current()));
        }
        return Value(std::move(obj));
    }

    Value parse_array(std::size_t depth) {
        ++pos_;  // '['
        Array arr;
        skip_space();
        if (!at_end() && peek() == ']') {
            ++pos_;
            return Value(std::move(arr));
        }
        while (true) {
            skip_space();
            arr.push_back(parse_value(depth + 1));
            skip_space();
            if (at_end()) fail("unterminated array; expected ',' or ']'");
            if (peek() == ',') {
                ++pos_;
                skip_space();
                if (!at_end() && peek() == ']') {
                    if (!opt_.trailing_commas) fail("trailing commas are not allowed here");
                    ++pos_;
                    break;
                }
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            fail(std::format("unexpected {}; expected ',' or ']'", describe_current()));
        }
        return Value(std::move(arr));
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::uint32_t parse_hex4() {
        if (pos_ + 4 > text_.size()) fail("incomplete \\u escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_ + i];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail_at("invalid hex digit in \\u escape", pos_ + i);
        }
        pos_ += 4;
        return value;
    }

    std::string parse_string() {
        std::size_t start = pos_;
        ++pos_;  // opening quote
        std::string out;
        while (true) {
            if (at_end()) fail_at("unterminated string", start);
            char c = peek();
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) fail("control characters must be escaped in strings");
            if (c != '\\') {
                out += c;
                ++pos_;
                continue;
            }
            std::size_t esc = pos_;
            ++pos_;
            if (at_end()) fail_at("unterminated string", start);
            char e = peek();
            ++pos_;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (text_.substr(pos_, 2) != "\\u") fail_at("unpaired UTF-16 surrogate in \\u escape", esc);
                        pos_ += 2;
                        std::uint32_t lo = parse_hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail_at("unpaired UTF-16 surrogate in \\u escape", esc);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail_at("unpaired UTF-16 surrogate in \\u escape", esc);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail_at(std::format("invalid escape '\\{}'", e), esc);
            }
        }
    }

    Value parse_number() {
        std::size_t start = pos_;
        bool is_integer = true;
        if (peek() == '-') ++pos_;
        if (at_end()) fail("invalid number");
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            fail("invalid number");
        }
        if (!at_end() && peek() == '.') {
            is_integer = false;
            ++pos_;
            if (at_end() || peek() < '0' || peek() > '9') fail("invalid number: expected a digit after '.'");
            while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            is_integer = false;
            ++pos_;
            if (!at_end() && (peek() == '+' || peek() == '-')) ++pos_;
            if (at_end() || peek() < '0' || peek() > '9') fail("invalid number: expected a digit in the exponent");
            while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        std::string_view token = text_.substr(start, pos_ - start);
        if (is_integer) {
            std::int64_t value = 0;
            auto [p, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (ec == std::errc() && p == token.data() + token.size()) return Value(value);
        }
        std::optional<double> value = to_double(token);
        if (!value) fail_at("number out of range", start);
        return Value(*value);
    }
};

}  // namespace

Value parse(std::string_view text, const ParseOptions& options) {
    return Parser(text, options).parse_document();
}

SourceLoc locate(std::string_view text, Span span, std::string file) {
    std::size_t offset = std::min<std::size_t>(span.begin, text.size());
    std::size_t line_start = 0;
    std::uint32_t line = 1;
    for (std::size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    std::size_t line_end = text.find('\n', offset);
    if (line_end == std::string_view::npos) line_end = text.size();

    SourceLoc loc;
    loc.file = std::move(file);
    loc.line = line;
    loc.col = static_cast<std::uint32_t>(str::utf8_length(text.substr(line_start, offset - line_start)) + 1);
    std::string_view line_text = text.substr(line_start, line_end - line_start);
    if (!line_text.empty() && line_text.back() == '\r') line_text.remove_suffix(1);
    if (line_start == 0 && line_text.starts_with("\xEF\xBB\xBF")) {
        line_text.remove_prefix(3);
        if (loc.col > 1) --loc.col;
    }
    loc.line_text = std::string(line_text);
    std::size_t end = std::min<std::size_t>(std::max(span.end, span.begin), text.size());
    std::size_t span_end_on_line = std::min(end, line_end);
    std::size_t len = span_end_on_line > offset ? str::utf8_length(text.substr(offset, span_end_on_line - offset)) : 1;
    loc.length = static_cast<std::uint32_t>(std::max<std::size_t>(len, 1));
    return loc;
}

}  // namespace cpm::json
