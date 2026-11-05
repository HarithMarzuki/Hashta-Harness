// A small JSON reader, enough for the engine's event envelopes.
//
// The producer is ours and the shapes are tiny, so this is a deliberate
// alternative to a 1 MB header: it still handles escapes, \uXXXX and
// surrogate pairs, because replies carry emoji.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value* find(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& entry : object) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }

    std::string str(const char* key, const char* fallback = "") const {
        const Value* value = find(key);
        return value && value->type == Type::String ? value->string
                                                    : std::string(fallback);
    }

    double num(const char* key, double fallback = 0.0) const {
        const Value* value = find(key);
        return value && value->type == Type::Number ? value->number : fallback;
    }

    bool flag(const char* key, bool fallback = false) const {
        const Value* value = find(key);
        return value && value->type == Type::Bool ? value->boolean : fallback;
    }
};

namespace detail {

inline void AppendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

struct Parser {
    const std::string& text;
    size_t pos = 0;

    explicit Parser(const std::string& source) : text(source) {}

    void SkipSpace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    bool Hex4(unsigned int& out) {
        if (pos + 4 > text.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= (unsigned)(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool ParseString(std::string& out) {
        if (pos >= text.size() || text[pos] != '"') return false;
        ++pos;
        out.clear();
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos >= text.size()) return false;
            const char escape = text[pos++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= text.size() &&
                        text[pos] == '\\' && text[pos + 1] == 'u') {
                        const size_t save = pos;
                        pos += 2;
                        unsigned int low = 0;
                        if (Hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos = save;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool ParseNumber(double& out) {
        const char* start = text.c_str() + pos;
        char* end = nullptr;
        out = std::strtod(start, &end);
        if (end == start) return false;
        pos += (size_t)(end - start);
        return true;
    }

    bool ParseValue(Value& out) {
        SkipSpace();
        if (pos >= text.size()) return false;

        const char c = text[pos];
        if (c == '{') {
            ++pos;
            out.type = Value::Type::Object;
            SkipSpace();
            if (pos < text.size() && text[pos] == '}') {
                ++pos;
                return true;
            }
            while (true) {
                SkipSpace();
                std::string key;
                if (!ParseString(key)) return false;
                SkipSpace();
                if (pos >= text.size() || text[pos] != ':') return false;
                ++pos;
                Value value;
                if (!ParseValue(value)) return false;
                out.object.emplace_back(std::move(key), std::move(value));
                SkipSpace();
                if (pos >= text.size()) return false;
                if (text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (text[pos] == '}') {
                    ++pos;
                    return true;
                }
                return false;
            }
        }

        if (c == '[') {
            ++pos;
            out.type = Value::Type::Array;
            SkipSpace();
            if (pos < text.size() && text[pos] == ']') {
                ++pos;
                return true;
            }
            while (true) {
                Value value;
                if (!ParseValue(value)) return false;
                out.array.push_back(std::move(value));
                SkipSpace();
                if (pos >= text.size()) return false;
                if (text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (text[pos] == ']') {
                    ++pos;
                    return true;
                }
                return false;
            }
        }

        if (c == '"') {
            out.type = Value::Type::String;
            return ParseString(out.string);
        }

        if (text.compare(pos, 4, "true") == 0) {
            pos += 4;
            out.type = Value::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (text.compare(pos, 5, "false") == 0) {
            pos += 5;
            out.type = Value::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (text.compare(pos, 4, "null") == 0) {
            pos += 4;
            out.type = Value::Type::Null;
            return true;
        }

        out.type = Value::Type::Number;
        return ParseNumber(out.number);
    }
};

}  // namespace detail

inline bool Parse(const std::string& text, Value& out) {
    detail::Parser parser(text);
    if (!parser.ParseValue(out)) return false;
    parser.SkipSpace();
    return parser.pos == text.size();
}

// Escapes a UTF-8 string for use inside a JSON string literal.
inline std::string Quote(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() + 8);
    for (unsigned char c : utf8) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X", c);
                    out += buffer;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

}  // namespace json
