// Minimal, exception-free JSON for switch-agentd.
// Sysmodules build with -fno-exceptions/-fno-rtti, so this uses return codes
// and std::string/std::vector only. Not a general-purpose library: it supports
// exactly what the protocol needs (objects, arrays, strings, doubles, ints,
// bools, null) and favours small code size over speed.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

enum class Type { Null, Bool, Int, Double, String, Array, Object };

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
   public:
    Value() : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(int64_t i) : type_(Type::Int), i_(i) {}
    Value(int i) : type_(Type::Int), i_(i) {}
    Value(uint64_t i) : type_(Type::Int), i_(static_cast<int64_t>(i)) {}
    Value(double d) : type_(Type::Double), d_(d) {}
    Value(const char* s) : type_(Type::String), s_(s) {}
    Value(const std::string& s) : type_(Type::String), s_(s) {}
    Value(std::string&& s) : type_(Type::String), s_(std::move(s)) {}
    Value(Array a) : type_(Type::Array), arr_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), obj_(std::move(o)) {}

    Type type() const { return type_; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Int || type_ == Type::Double; }

    // Typed accessors with defaults — never throw.
    bool as_bool(bool d = false) const { return type_ == Type::Bool ? b_ : d; }
    int64_t as_int(int64_t d = 0) const {
        if (type_ == Type::Int) return i_;
        if (type_ == Type::Double) return static_cast<int64_t>(d_);
        return d;
    }
    double as_double(double d = 0) const {
        if (type_ == Type::Double) return d_;
        if (type_ == Type::Int) return static_cast<double>(i_);
        return d;
    }
    const std::string& as_string(const std::string& d = kEmpty) const {
        return type_ == Type::String ? s_ : d;
    }
    const Array& as_array() const { return type_ == Type::Array ? arr_ : kEmptyArr; }
    const Object& as_object() const { return type_ == Type::Object ? obj_ : kEmptyObj; }

    // Object member access; returns Null value if absent or not an object.
    const Value& operator[](const std::string& k) const {
        if (type_ != Type::Object) return kNull;
        auto it = obj_.find(k);
        return it == obj_.end() ? kNull : it->second;
    }
    bool has(const std::string& k) const {
        return type_ == Type::Object && obj_.count(k) > 0;
    }

    // There is no integer index operator: array elements come from as_array().
    // Without this deletion `v[0]` compiles, because 0 converts to a null
    // const char* and then to std::string — which is undefined behaviour that
    // looks like ordinary array indexing. Make it a compile error instead.
    const Value& operator[](int) const = delete;
    const Value& operator[](std::nullptr_t) const = delete;

    // Object builder: v.set("a", 1).set("b", "x")
    Value& set(const std::string& k, Value v) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            obj_.clear();
        }
        obj_[k] = std::move(v);
        return *this;
    }

    static Value object() { return Value(Object{}); }
    static Value array() { return Value(Array{}); }

    void push(Value v) {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            arr_.clear();
        }
        arr_.push_back(std::move(v));
    }

    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }

    // Parse; returns true on success. On failure `out` is Null.
    static bool parse(const std::string& text, Value& out);

   private:
    void write(std::string& out) const;
    static void write_string(const std::string& s, std::string& out);

    Type type_;
    bool b_ = false;
    int64_t i_ = 0;
    double d_ = 0;
    std::string s_;
    Array arr_;
    Object obj_;

    static const std::string kEmpty;
    static const Array kEmptyArr;
    static const Object kEmptyObj;
    static const Value kNull;
};

}  // namespace json
