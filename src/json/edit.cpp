#include "json/edit.hpp"

#include <algorithm>
#include <format>

#include "core/error.hpp"
#include "json/parse.hpp"
#include "json/write.hpp"

namespace cpm::json::edit {

namespace {

struct Edit {
    std::size_t begin, end;  // replaced range
    std::string text;
};

// Applies non-overlapping edits. Insertions at the same position end up in the order they are listed.
std::string apply_edits(std::string_view text, const std::vector<Edit>& edits) {
    std::vector<std::size_t> order(edits.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    // Back to front, so earlier offsets stay valid; for equal offsets, the later-listed edit goes first.
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (edits[a].begin != edits[b].begin) return edits[a].begin > edits[b].begin;
        return a > b;
    });
    std::string out(text);
    for (std::size_t i : order) out.replace(edits[i].begin, edits[i].end - edits[i].begin, edits[i].text);
    return out;
}

bool is_blank(char c) { return c == ' ' || c == '\t'; }

std::size_t line_start(std::string_view text, std::size_t pos) {
    while (pos > 0 && text[pos - 1] != '\n') --pos;
    return pos;
}

std::string indentation_at(std::string_view text, std::size_t pos) {
    std::size_t start = line_start(text, pos);
    std::size_t end = start;
    while (end < text.size() && is_blank(text[end])) ++end;
    return std::string(text.substr(start, end - start));
}

bool first_on_line(std::string_view text, std::size_t pos) {
    for (std::size_t i = line_start(text, pos); i < pos; ++i) {
        if (!is_blank(text[i])) return false;
    }
    return true;
}

std::string newline_of(std::string_view text) { return text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n"; }

// One indentation level as used by the file (the first member's indentation), or two spaces.
std::string indent_unit(std::string_view text, const Value& root) {
    if (root.is_object() && !root.as_object().empty()) {
        std::size_t key = root.as_object().front().key_span.begin;
        if (first_on_line(text, key)) {
            std::string indent = indentation_at(text, key);
            if (!indent.empty()) return indent;
        }
    }
    return "  ";
}

// Objects on several lines, arrays and scalars on one line.
std::string format_value(const Value& v, const std::string& indent, const std::string& unit, const std::string& nl) {
    if (v.is_object()) {
        const Object& obj = v.as_object();
        if (obj.empty()) return "{}";
        std::string out = "{" + nl;
        for (std::size_t i = 0; i < obj.size(); ++i) {
            out += indent + unit + quote(obj[i].key) + ": " + format_value(obj[i].value, indent + unit, unit, nl);
            out += (i + 1 < obj.size() ? "," : "") + nl;
        }
        return out + indent + "}";
    }
    if (v.is_array()) {
        std::string out = "[";
        const Array& arr = v.as_array();
        for (std::size_t i = 0; i < arr.size(); ++i) out += (i ? ", " : "") + format_value(arr[i], indent, unit, nl);
        return out + "]";
    }
    return write(v);
}

// Position just after a value: past a following comma (reported in `comma`) and a same-line // comment.
std::size_t end_of_entry(std::string_view text, std::size_t value_end, bool& comma, std::size_t& comma_pos) {
    std::size_t p = value_end;
    while (p < text.size() && is_blank(text[p])) ++p;
    comma = p < text.size() && text[p] == ',';
    comma_pos = p;
    if (comma) ++p;
    std::size_t q = p;
    while (q < text.size() && is_blank(text[q])) ++q;
    if (text.substr(q, 2) == "//") {
        while (q < text.size() && text[q] != '\n' && text[q] != '\r') ++q;
        return q;
    }
    return p;
}

// Start of the comment lines directly above `pos`'s line (no blank lines in between), or the line itself.
std::size_t attached_comments_start(std::string_view text, std::size_t pos) {
    std::size_t cur = line_start(text, pos);
    while (cur > 0) {
        std::size_t prev = line_start(text, cur - 1);
        std::string_view line = text.substr(prev, cur - prev);
        std::size_t a = 0;
        while (a < line.size() && is_blank(line[a])) ++a;
        line.remove_prefix(a);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || is_blank(line.back()))) line.remove_suffix(1);
        bool comment = line.starts_with("//") || (line.starts_with("/*") && line.ends_with("*/"));
        if (!comment) break;
        cur = prev;
    }
    return cur;
}

// `sorted_position`: a leaf member goes where it sorts when the keys are sorted; a newly created
// object always goes last (manifest.md §10).
std::vector<Edit> insert_member(std::string_view text, const Value& obj, const std::string& key, const Value& value,
                                const std::string& unit, bool sorted_position) {
    const std::string nl = newline_of(text);
    const Object& members = obj.as_object();
    std::size_t open = obj.span().begin;
    std::size_t close = obj.span().end - 1;

    if (members.empty()) {
        std::string parent = indentation_at(text, open);
        std::string indent = parent + unit;
        std::string member = quote(key) + ": " + format_value(value, indent, unit, nl);
        std::string_view inner = text.substr(open + 1, close - open - 1);
        if (std::all_of(inner.begin(), inner.end(), [](char c) { return is_blank(c) || c == '\n' || c == '\r'; })) {
            return {{open + 1, close, nl + indent + member + nl + parent}};
        }
        std::size_t pos = close;  // after the last comment inside the braces
        while (pos > open + 1 && (is_blank(text[pos - 1]) || text[pos - 1] == '\n' || text[pos - 1] == '\r')) --pos;
        return {{pos, pos, nl + indent + member}};
    }

    std::size_t first_key = members.front().key_span.begin;
    bool multi_line = first_on_line(text, first_key);
    std::string indent = multi_line ? indentation_at(text, first_key) : std::string();
    std::string member = quote(key) + ": " + format_value(value, indent, unit, nl);

    bool sorted = sorted_position && std::is_sorted(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.key < b.key; });
    std::size_t index = members.size();
    if (sorted) {
        index = static_cast<std::size_t>(std::find_if(members.begin(), members.end(),
                                                      [&](const Member& m) { return m.key > key; }) -
                                         members.begin());
    }

    if (index < members.size()) {
        std::size_t key_pos = members[index].key_span.begin;
        if (!multi_line) return {{key_pos, key_pos, member + ", "}};
        std::size_t at = attached_comments_start(text, key_pos);
        return {{at, at, indent + member + "," + nl}};
    }

    const Member& last = members.back();
    bool comma = false;
    std::size_t comma_pos = 0;
    std::size_t end = end_of_entry(text, last.value.span().end, comma, comma_pos);
    if (!multi_line) {
        if (comma) return {{comma_pos + 1, comma_pos + 1, " " + member + ","}};
        return {{last.value.span().end, last.value.span().end, ", " + member}};
    }
    if (comma) return {{end, end, nl + indent + member + ","}};  // keep the trailing-comma style
    return {{last.value.span().end, last.value.span().end, ","}, {end, end, nl + indent + member}};
}

const Value* navigate(const Value& root, const std::vector<std::string>& path, std::size_t count) {
    const Value* cur = &root;
    for (std::size_t i = 0; i < count; ++i) {
        cur = cur->find(path[i]);
        if (cur == nullptr) return nullptr;
    }
    return cur;
}

}  // namespace

std::string set_member(std::string_view text, const std::vector<std::string>& path, const Value& value) {
    if (path.empty()) throw Error("internal error: empty edit path");
    Value root = parse(text);
    if (!root.is_object()) throw Error("cannot edit: the document is not a JSON object");
    const std::string unit = indent_unit(text, root);

    std::vector<Edit> edits;
    const Value* current = &root;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const Member* m = current->find_member(path[i]);
        bool last = i + 1 == path.size();
        if (m != nullptr) {
            if (last) {
                if (m->value == value) return std::string(text);
                std::string indent = first_on_line(text, m->key_span.begin) ? indentation_at(text, m->key_span.begin) : "";
                edits.push_back({m->value.span().begin, m->value.span().end,
                                 format_value(value, indent, unit, newline_of(text))});
                break;
            }
            if (!m->value.is_object()) throw Error(std::format("cannot edit: \"{}\" is not an object", path[i]));
            current = &m->value;
            continue;
        }
        Value nested = value;  // the remaining path becomes nested objects
        for (std::size_t j = path.size() - 1; j > i; --j) {
            Value wrapper = Value::object();
            wrapper.set(path[j], std::move(nested));
            nested = std::move(wrapper);
        }
        edits = insert_member(text, *current, path[i], nested, unit, last);
        break;
    }

    std::string result = apply_edits(text, edits);
    Value reparsed = parse(result);
    const Value* check = navigate(reparsed, path, path.size());  // the edit must do exactly this
    if (check == nullptr || !(*check == value)) throw Error("internal error: JSON edit produced an unexpected result");
    return result;
}

std::string remove_member(std::string_view text, const std::vector<std::string>& path) {
    if (path.empty()) throw Error("internal error: empty edit path");
    Value root = parse(text);
    const Value* obj = navigate(root, path, path.size() - 1);
    if (obj == nullptr || !obj->is_object()) return std::string(text);
    const Object& members = obj->as_object();
    auto it = std::find_if(members.begin(), members.end(), [&](const Member& m) { return m.key == path.back(); });
    if (it == members.end()) return std::string(text);
    std::size_t index = static_cast<std::size_t>(it - members.begin());
    const Member& m = *it;

    std::vector<Edit> edits;
    if (members.size() == 1) {
        edits.push_back({obj->span().begin, obj->span().end, "{}"});
    } else if (!first_on_line(text, m.key_span.begin) ||
               (index + 1 < members.size() && !first_on_line(text, members[index + 1].key_span.begin))) {
        // Members share a line: cut between neighbours.
        if (index + 1 < members.size()) {
            edits.push_back({m.key_span.begin, members[index + 1].key_span.begin, ""});
        } else {
            edits.push_back({members[index - 1].value.span().end, m.value.span().end, ""});
        }
    } else {
        bool comma = false;
        std::size_t comma_pos = 0;
        std::size_t end = end_of_entry(text, m.value.span().end, comma, comma_pos);
        if (text.substr(end, 2) == "\r\n") end += 2;
        else if (end < text.size() && text[end] == '\n') end += 1;
        edits.push_back({line_start(text, m.key_span.begin), end, ""});
        if (index + 1 == members.size() && !comma) {
            // The new last member must not keep a separator for a member that is gone.
            bool prev_comma = false;
            std::size_t prev_comma_pos = 0;
            end_of_entry(text, members[index - 1].value.span().end, prev_comma, prev_comma_pos);
            if (prev_comma) edits.push_back({prev_comma_pos, prev_comma_pos + 1, ""});
        }
    }

    std::string result = apply_edits(text, edits);
    Value reparsed = parse(result);
    const Value* check = navigate(reparsed, path, path.size() - 1);
    if (check == nullptr || check->find(path.back()) != nullptr)
        throw Error("internal error: JSON edit produced an unexpected result");
    return result;
}

}  // namespace cpm::json::edit
