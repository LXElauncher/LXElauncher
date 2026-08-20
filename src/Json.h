#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace lxe {

class JsonException : public std::runtime_error {
public:
    explicit JsonException(const std::string& msg) : std::runtime_error(msg) {}
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;

    Json() : v_(nullptr) {}
    Json(std::nullptr_t) : v_(nullptr) {}
    Json(bool b) : v_(b) {}
    Json(double d) : v_(d) {}
    Json(int i) : v_(static_cast<double>(i)) {}
    Json(int64_t i) : v_(static_cast<double>(i)) {}
    Json(uint64_t i) : v_(static_cast<double>(i)) {}
    Json(const char* s) : v_(std::string(s ? s : "")) {}
    Json(std::string s) : v_(std::move(s)) {}

    static Json array() { Json j; j.v_ = Array{}; return j; }
    static Json object() { Json j; j.v_ = Object{}; return j; }

    Type type() const { return static_cast<Type>(v_.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Number; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    bool asBool() const { return std::get<bool>(v_); }
    double asNumber() const { return std::get<double>(v_); }
    const std::string& asString() const { return std::get<std::string>(v_); }
    Array& asArray() { return std::get<Array>(v_); }
    const Array& asArray() const { return std::get<Array>(v_); }
    Object& asObject() { return std::get<Object>(v_); }
    const Object& asObject() const { return std::get<Object>(v_); }

    bool contains(const std::string& key) const {
        return isObject() && asObject().count(key) > 0;
    }

    const Json& at(const std::string& key) const { return asObject().at(key); }

    Json& operator[](const std::string& key) {
        auto& o = asObject();
        return o[key];
    }

    const Json& operator[](const std::string& key) const { return asObject().at(key); }

    size_t size() const {
        if (isArray()) return asArray().size();
        if (isObject()) return asObject().size();
        return 0;
    }

    static Json parse(std::string_view text) {
        size_t i = 0;
        skipWs(text, i);
        Json j = parseValue(text, i);
        skipWs(text, i);
        if (i != text.size()) throw JsonException("trailing characters after JSON value");
        return j;
    }

    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v_;

    static void skipWs(std::string_view s, size_t& i) {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
            else break;
        }
    }

    static void expect(std::string_view s, size_t& i, const char* token) {
        std::string_view t(token);
        if (s.substr(i, t.size()) != t) throw JsonException("unexpected token");
        i += t.size();
    }

    static unsigned parseHex4(std::string_view s, size_t& i) {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            if (i >= s.size()) throw JsonException("bad unicode escape");
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else throw JsonException("bad unicode escape");
        }
        return v;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
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

    static std::string parseString(std::string_view s, size_t& i) {
        if (i >= s.size() || s[i] != '"') throw JsonException("expected string");
        ++i;
        std::string out;
        while (i < s.size()) {
            char c = s[i];
            if (c == '"') { ++i; return out; }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) throw JsonException("bad escape");
                char e = s[i++];
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
                        if (i + 4 > s.size()) throw JsonException("bad unicode escape");
                        unsigned cp = parseHex4(s, i);
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u')
                                throw JsonException("missing low surrogate");
                            i += 2;
                            unsigned lo = parseHex4(s, i);
                            if (!(lo >= 0xDC00 && lo <= 0xDFFF))
                                throw JsonException("bad low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: throw JsonException("bad escape");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                throw JsonException("control character in string");
            } else {
                out += c;
                ++i;
            }
        }
        throw JsonException("unterminated string");
    }

    static Json parseNumber(std::string_view s, size_t& i) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
                throw JsonException("bad exponent");
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        }
        std::string num(s.substr(start, i - start));
        if (num.empty() || num == "-") throw JsonException("bad number");
        try {
            size_t used = 0;
            long long v = std::stoll(num, &used);
            if (used == num.size()) return Json(static_cast<double>(v));
        } catch (...) {}
        char* end = nullptr;
        double d = std::strtod(num.c_str(), &end);
        if (!end || *end) throw JsonException("bad number");
        return Json(d);
    }

    static Json parseValue(std::string_view s, size_t& i);

    static Json parseArray(std::string_view s, size_t& i) {
        ++i;
        Json arr = array();
        skipWs(s, i);
        if (i < s.size() && s[i] == ']') { ++i; return arr; }
        while (true) {
            skipWs(s, i);
            arr.asArray().push_back(parseValue(s, i));
            skipWs(s, i);
            if (i >= s.size()) throw JsonException("unterminated array");
            char c = s[i];
            if (c == ',') { ++i; continue; }
            if (c == ']') { ++i; return arr; }
            throw JsonException("bad array syntax");
        }
    }

    static Json parseObject(std::string_view s, size_t& i) {
        ++i;
        Json obj = object();
        skipWs(s, i);
        if (i < s.size() && s[i] == '}') { ++i; return obj; }
        while (true) {
            skipWs(s, i);
            if (i >= s.size() || s[i] != '"') throw JsonException("expected object key");
            std::string key = parseString(s, i);
            skipWs(s, i);
            if (i >= s.size() || s[i] != ':') throw JsonException("expected ':'");
            ++i;
            skipWs(s, i);
            obj[key] = parseValue(s, i);
            skipWs(s, i);
            if (i >= s.size()) throw JsonException("unterminated object");
            char c = s[i];
            if (c == ',') { ++i; continue; }
            if (c == '}') { ++i; return obj; }
            throw JsonException("bad object syntax");
        }
    }

    static void writeString(std::string& out, const std::string& s) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
    }

    void writeNumber(std::string& out) const {
        double d = std::get<double>(v_);
        if (std::isfinite(d) && std::floor(d) == d && std::abs(d) < 9.0e15) {
            out += std::to_string(static_cast<long long>(d));
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            out += buf;
        }
    }

    void write(std::string& out) const {
        switch (type()) {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += asBool() ? "true" : "false"; break;
            case Type::Number: writeNumber(out); break;
            case Type::String: writeString(out, asString()); break;
            case Type::Array: {
                out += '[';
                bool first = true;
                for (const auto& v : asArray()) {
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
                for (const auto& [k, v] : asObject()) {
                    if (!first) out += ',';
                    first = false;
                    writeString(out, k);
                    out += ':';
                    v.write(out);
                }
                out += '}';
                break;
            }
        }
    }
};

inline Json Json::parseValue(std::string_view s, size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) throw JsonException("unexpected end of input");
    char c = s[i];
    if (c == '{') return parseObject(s, i);
    if (c == '[') return parseArray(s, i);
    if (c == '"') return Json(parseString(s, i));
    if (c == 't') { expect(s, i, "true"); return Json(true); }
    if (c == 'f') { expect(s, i, "false"); return Json(false); }
    if (c == 'n') { expect(s, i, "null"); return Json(nullptr); }
    return parseNumber(s, i);
}

} // namespace lxe