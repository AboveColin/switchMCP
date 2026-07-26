#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

const std::string Value::kEmpty;
const Array Value::kEmptyArr;
const Object Value::kEmptyObj;
const Value Value::kNull;

void Value::write_string(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
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
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void Value::write(std::string& out) const {
    char buf[32];
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Int:
            std::snprintf(buf, sizeof(buf), "%lld", (long long)i_);
            out += buf;
            break;
        case Type::Double:
            if (std::isfinite(d_)) {
                std::snprintf(buf, sizeof(buf), "%.10g", d_);
                out += buf;
            } else {
                out += "null";  // JSON has no NaN/Inf
            }
            break;
        case Type::String: write_string(s_, out); break;
        case Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& v : arr_) {
                if (!first) out += ',';
                first = false;
                v.write(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& kv : obj_) {
                if (!first) out += ',';
                first = false;
                write_string(kv.first, out);
                out += ':';
                kv.second.write(out);
            }
            out += '}';
            break;
        }
    }
}

namespace {

struct Parser {
    const char* p;
    const char* end;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }

    bool parse_value(Value& out) {
        skip_ws();
        if (p >= end) return false;
        switch (*p) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': {
                std::string s;
                if (!parse_string(s)) return false;
                out = Value(std::move(s));
                return true;
            }
            case 't':
                if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
                    p += 4;
                    out = Value(true);
                    return true;
                }
                return false;
            case 'f':
                if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
                    p += 5;
                    out = Value(false);
                    return true;
                }
                return false;
            case 'n':
                if (end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
                    p += 4;
                    out = Value();
                    return true;
                }
                return false;
            default: return parse_number(out);
        }
    }

    bool parse_string(std::string& out) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                char e = *p++;
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (end - p < 4) return false;
                        unsigned cp = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else return false;
                        }
                        // Encode BMP code point as UTF-8 (surrogate pairs not
                        // needed for our protocol's ASCII-ish payloads).
                        if (cp < 0x80) {
                            out += (char)cp;
                        } else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;  // unterminated
    }

    bool parse_number(Value& out) {
        const char* start = p;
        bool is_double = false;
        if (p < end && (*p == '-' || *p == '+')) p++;
        while (p < end) {
            char c = *p;
            if (c >= '0' && c <= '9') {
                p++;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                is_double = true;
                p++;
            } else {
                break;
            }
        }
        if (p == start) return false;
        std::string num(start, p - start);
        if (is_double) {
            out = Value(std::strtod(num.c_str(), nullptr));
        } else {
            out = Value((int64_t)std::strtoll(num.c_str(), nullptr, 10));
        }
        return true;
    }

    bool parse_array(Value& out) {
        p++;  // '['
        out = Value::array();
        skip_ws();
        if (p < end && *p == ']') {
            p++;
            return true;
        }
        while (true) {
            Value v;
            if (!parse_value(v)) return false;
            out.push(std::move(v));
            skip_ws();
            if (p >= end) return false;
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p == ']') {
                p++;
                return true;
            }
            return false;
        }
    }

    bool parse_object(Value& out) {
        p++;  // '{'
        out = Value::object();
        skip_ws();
        if (p < end && *p == '}') {
            p++;
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return false;
            p++;
            Value v;
            if (!parse_value(v)) return false;
            out.set(key, std::move(v));
            skip_ws();
            if (p >= end) return false;
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p == '}') {
                p++;
                return true;
            }
            return false;
        }
    }
};

}  // namespace

bool Value::parse(const std::string& text, Value& out) {
    Parser parser{text.data(), text.data() + text.size()};
    if (!parser.parse_value(out)) {
        out = Value();
        return false;
    }
    parser.skip_ws();
    return true;  // trailing garbage tolerated (frames are exact-length anyway)
}

}  // namespace json
