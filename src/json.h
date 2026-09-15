#pragma once

#include <string>
#include <vector>
#include <utility>

namespace json {

// Minimal dependency-free JSON parser for the small server payloads
// this app deals with (Ollama / OpenAI-compatible APIs).
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    // Look up a member of an object value.
    const Value* Find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : object) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    const Value* At(size_t index) const {
        if (type != Type::Array || index >= array.size()) return nullptr;
        return &array[index];
    }

    size_t ArraySize() const {
        return type == Type::Array ? array.size() : 0;
    }

    bool IsString() const {
        return type == Type::String;
    }

    bool IsObject() const {
        return type == Type::Object;
    }

    bool IsArray() const {
        return type == Type::Array;
    }

    const std::string& Str() const {
        return string;
    }
};

// Parse a complete JSON document. On failure returns a Null value and
// fills errorOut.
Value Parse(const std::string& text, std::string& errorOut);

} // namespace json