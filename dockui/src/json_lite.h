#pragma once
// Minimal JSON value, parser and writer for the Dock's own settings files (UTF-8).
// Supports objects, arrays, strings (with \uXXXX escapes), numbers, booleans and null.
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dock::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Object, Array } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;                                   // UTF-8
    std::vector<std::pair<std::string, Value>> object;    // insertion order preserved
    std::vector<Value> array;

    static Value MakeObject() { Value v; v.type = Type::Object; return v; }
    static Value MakeNumber(double d) { Value v; v.type = Type::Number; v.number = d; return v; }
    static Value MakeBool(bool b) { Value v; v.type = Type::Bool; v.boolean = b; return v; }
    static Value MakeString(std::string s) { Value v; v.type = Type::String; v.string = std::move(s); return v; }

    bool IsObject() const { return type == Type::Object; }
    const Value* Find(const std::string& key) const;
    void Set(const std::string& key, Value v);
};

// Returns false and fills error on malformed input. Depth and size are bounded.
bool Parse(const std::string& text, Value& out, std::string& error);
std::string Write(const Value& v, int indent = 2);

std::string ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);

}  // namespace dock::json
