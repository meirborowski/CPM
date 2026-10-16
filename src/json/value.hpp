#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cpm::json {

// Byte offsets [begin, end) into the source text a value was parsed from.
struct Span {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

class Value;
struct Member;
using Array = std::vector<Value>;
using Object = std::vector<Member>;  // insertion-ordered

enum class Type { Null, Bool, Int, Double, String, Array, Object };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t);
    Value(bool b);
    Value(int i);
    Value(std::int64_t i);
    Value(double d);
    Value(const char* s);
    Value(std::string_view s);
    Value(std::string s);
    Value(Array a);
    Value(Object o);

    static Value array();
    static Value object();

    Type type() const noexcept { return static_cast<Type>(data_.index()); }
    std::string_view type_name() const noexcept;
    bool is_null() const noexcept { return type() == Type::Null; }
    bool is_bool() const noexcept { return type() == Type::Bool; }
    bool is_int() const noexcept { return type() == Type::Int; }
    bool is_number() const noexcept { return type() == Type::Int || type() == Type::Double; }
    bool is_string() const noexcept { return type() == Type::String; }
    bool is_array() const noexcept { return type() == Type::Array; }
    bool is_object() const noexcept { return type() == Type::Object; }

    // Accessors throw cpm::Error on a type mismatch.
    bool as_bool() const;
    std::int64_t as_int() const;
    double as_double() const;
    const std::string& as_string() const;
    const Array& as_array() const;
    Array& as_array();
    const Object& as_object() const;
    Object& as_object();

    // Object lookup; nullptr if this is not an object or the key is absent.
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    // Object lookup that throws cpm::Error if the key is absent.
    const Value& at(std::string_view key) const;
    const Member* find_member(std::string_view key) const;

    // Object: replace the member's value, or append a new member. Returns the stored value.
    Value& set(std::string key, Value value);
    // Array: append.
    void push_back(Value value);

    Span span() const noexcept { return span_; }
    void set_span(Span s) noexcept { span_ = s; }

    // Structural equality; spans are ignored and object member order matters.
    friend bool operator==(const Value& a, const Value& b);

private:
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> data_;
    Span span_;
};

struct Member {
    std::string key;
    Value value;
    Span key_span;
};

}  // namespace cpm::json
