#include "json_lite.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dock::json {

const Value* Value::Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void Value::Set(const std::string& key, Value v) {
    type = Type::Object;
    for (auto& kv : object) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    }
    object.emplace_back(key, std::move(v));
}

namespace {

class Parser {
public:
    explicit Parser(const std::string& t) : s_(t) {}

    bool Run(Value& out, std::string& error) {
        SkipWs();
        if (!ParseValue(out, 0)) {
            error = error_.empty() ? "invalid JSON" : error_;
            return false;
        }
        SkipWs();
        if (pos_ != s_.size()) {
            error = "trailing characters";
            return false;
        }
        return true;
    }

private:
    bool Fail(const char* msg) {
        if (error_.empty()) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s at offset %zu", msg, pos_);
            error_ = buf;
        }
        return false;
    }
    void SkipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_;
    }
    bool Literal(const char* lit) {
        size_t n = strlen(lit);
        if (s_.compare(pos_, n, lit) != 0) return false;
        pos_ += n;
        return true;
    }
    bool ParseValue(Value& v, int depth) {
        if (depth > 32) return Fail("nesting too deep");
        SkipWs();
        if (pos_ >= s_.size()) return Fail("unexpected end");
        const char c = s_[pos_];
        if (c == '{') return ParseObject(v, depth);
        if (c == '[') return ParseArray(v, depth);
        if (c == '"') {
            v.type = Value::Type::String;
            return ParseString(v.string);
        }
        if (Literal("true")) { v.type = Value::Type::Bool; v.boolean = true; return true; }
        if (Literal("false")) { v.type = Value::Type::Bool; v.boolean = false; return true; }
        if (Literal("null")) { v.type = Value::Type::Null; return true; }
        return ParseNumber(v);
    }
    bool ParseNumber(Value& v) {
        const char* begin = s_.c_str() + pos_;
        char* end = nullptr;
        const double d = strtod(begin, &end);
        if (end == begin) return Fail("unexpected character");
        if (!std::isfinite(d)) return Fail("non-finite number");
        pos_ += static_cast<size_t>(end - begin);
        v.type = Value::Type::Number;
        v.number = d;
        return true;
    }
    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
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
    bool Hex4(unsigned& cp) {
        if (pos_ + 4 > s_.size()) return Fail("bad escape");
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= h - '0';
            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
            else return Fail("bad hex escape");
        }
        return true;
    }
    bool ParseString(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) break;
            const char e = s_[pos_++];
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
                unsigned cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') return Fail("lone surrogate");
                    pos_ += 2;
                    if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return Fail("bad surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }
    bool ParseObject(Value& v, int depth) {
        ++pos_;
        v = Value::MakeObject();
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("expected key");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("expected ':'");
            ++pos_;
            Value child;
            if (!ParseValue(child, depth + 1)) return false;
            v.Set(key, std::move(child));
            SkipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            return Fail("expected ',' or '}'");
        }
    }
    bool ParseArray(Value& v, int depth) {
        ++pos_;
        v.type = Value::Type::Array;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            Value child;
            if (!ParseValue(child, depth + 1)) return false;
            v.array.push_back(std::move(child));
            SkipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            return Fail("expected ',' or ']'");
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string error_;
};

void WriteString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void WriteValue(std::string& out, const Value& v, int indent, int level) {
    const std::string pad(static_cast<size_t>(indent * (level + 1)), ' ');
    const std::string padEnd(static_cast<size_t>(indent * level), ' ');
    switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Type::Number: {
        char buf[48];
        snprintf(buf, sizeof(buf), "%.9g", v.number);
        out += buf;
        break;
    }
    case Value::Type::String: WriteString(out, v.string); break;
    case Value::Type::Object: {
        if (v.object.empty()) { out += "{}"; break; }
        out += "{\n";
        for (size_t i = 0; i < v.object.size(); ++i) {
            out += pad;
            WriteString(out, v.object[i].first);
            out += ": ";
            WriteValue(out, v.object[i].second, indent, level + 1);
            if (i + 1 < v.object.size()) out += ',';
            out += '\n';
        }
        out += padEnd + "}";
        break;
    }
    case Value::Type::Array: {
        out += "[";
        for (size_t i = 0; i < v.array.size(); ++i) {
            if (i) out += ", ";
            WriteValue(out, v.array[i], indent, level + 1);
        }
        out += "]";
        break;
    }
    }
}

}  // namespace

bool Parse(const std::string& text, Value& out, std::string& error) {
    if (text.size() > (4u << 20)) {
        error = "file too large";
        return false;
    }
    size_t start = (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
    const std::string body = text.substr(start);
    Parser p(body);
    return p.Run(out, error);
}

std::string Write(const Value& v, int indent) {
    std::string out;
    WriteValue(out, v, indent, 0);
    out += '\n';
    return out;
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace dock::json
