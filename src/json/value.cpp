#include "json/value.hpp"

#include <format>

#include "core/error.hpp"

namespace cpm::json {

Value::Value(std::nullptr_t) {}
Value::Value(bool b) : data_(b) {}
Value::Value(int i) : data_(static_cast<std::int64_t>(i)) {}
Value::Value(std::int64_t i) : data_(i) {}
Value::Value(double d) : data_(d) {}
Value::Value(const char* s) : data_(std::string(s)) {}
Value::Value(std::string_view s) : data_(std::string(s)) {}
Value::Value(std::string s) : data_(std::move(s)) {}
Value::Value(Array a) : data_(std::move(a)) {}
Value::Value(Object o) : data_(std::move(o)) {}

Value Value::array() { return Value(Array{}); }
Value Value::object() { return Value(Object{}); }

std::string_view Value::type_name() const noexcept {
    switch (type()) {
        case Type::Null: return "null";
        case Type::Bool: return "boolean";
        case Type::Int:
        case Type::Double: return "number";
        case Type::String: return "string";
        case Type::Array: return "array";
        case Type::Object: return "object";
    }
    return "?";
}

namespace {
[[noreturn]] void mismatch(const Value& v, std::string_view wanted) {
    throw Error(std::format("expected {} but found {}", wanted, v.type_name()));
}
}  // namespace

bool Value::as_bool() const {
    if (!is_bool()) mismatch(*this, "boolean");
    return std::get<bool>(data_);
}

std::int64_t Value::as_int() const {
    if (!is_int()) mismatch(*this, "integer");
    return std::get<std::int64_t>(data_);
}

double Value::as_double() const {
    if (is_int()) return static_cast<double>(std::get<std::int64_t>(data_));
    if (type() != Type::Double) mismatch(*this, "number");
    return std::get<double>(data_);
}

const std::string& Value::as_string() const {
    if (!is_string()) mismatch(*this, "string");
    return std::get<std::string>(data_);
}

const Array& Value::as_array() const {
    if (!is_array()) mismatch(*this, "array");
    return std::get<Array>(data_);
}

Array& Value::as_array() {
    if (!is_array()) mismatch(*this, "array");
    return std::get<Array>(data_);
}

const Object& Value::as_object() const {
    if (!is_object()) mismatch(*this, "object");
    return std::get<Object>(data_);
}

Object& Value::as_object() {
    if (!is_object()) mismatch(*this, "object");
    return std::get<Object>(data_);
}

const Member* Value::find_member(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& m : std::get<Object>(data_)) {
        if (m.key == key) return &m;
    }
    return nullptr;
}

const Value* Value::find(std::string_view key) const {
    const Member* m = find_member(key);
    return m ? &m->value : nullptr;
}

Value* Value::find(std::string_view key) {
    return const_cast<Value*>(static_cast<const Value&>(*this).find(key));
}

const Value& Value::at(std::string_view key) const {
    const Value* v = find(key);
    if (v == nullptr) throw Error(std::format("missing key \"{}\"", key));
    return *v;
}

Value& Value::set(std::string key, Value value) {
    Object& obj = as_object();
    for (auto& m : obj) {
        if (m.key == key) {
            m.value = std::move(value);
            return m.value;
        }
    }
    obj.push_back(Member{std::move(key), std::move(value), {}});
    return obj.back().value;
}

void Value::push_back(Value value) { as_array().push_back(std::move(value)); }

bool operator==(const Value& a, const Value& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Type::Null: return true;
        case Type::Bool: return a.as_bool() == b.as_bool();
        case Type::Int: return a.as_int() == b.as_int();
        case Type::Double: return a.as_double() == b.as_double();
        case Type::String: return a.as_string() == b.as_string();
        case Type::Array: return a.as_array() == b.as_array();
        case Type::Object: {
            const Object& x = a.as_object();
            const Object& y = b.as_object();
            if (x.size() != y.size()) return false;
            for (std::size_t i = 0; i < x.size(); ++i) {
                if (x[i].key != y[i].key || !(x[i].value == y[i].value)) return false;
            }
            return true;
        }
    }
    return false;
}

}  // namespace cpm::json
