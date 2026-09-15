#include "json.h"
#include <cstdlib>
#include <cstdio>

namespace json {

namespace {

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void SkipSpace(const std::string& s, size_t& pos) {
    while (pos < s.size() && IsSpace(s[pos])) pos++;
}

// Append one Unicode code point as UTF-8.
void AppendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseString(const std::string& s, size_t& pos, std::string& out, std::string& err) {
    // Assumes s[pos] == '"'
    pos++; // skip opening quote
    out.clear();
    while (pos < s.size()) {
        char c = s[pos];
        if (c == '"') {
            pos++;
            return true;
        }
        if (c == '\\') {
            if (pos + 1 >= s.size()) break;
            char e = s[pos + 1];
            switch (e) {
                case '"': out += '"'; pos += 2; break;
                case '\\': out += '\\'; pos += 2; break;
                case '/': out += '/'; pos += 2; break;
                case 'b': out += '\b'; pos += 2; break;
                case 'f': out += '\f'; pos += 2; break;
                case 'n': out += '\n'; pos += 2; break;
                case 'r': out += '\r'; pos += 2; break;
                case 't': out += '\t'; pos += 2; break;
                case 'u': {
                    if (pos + 6 > s.size()) { err = "Bad \\u escape."; return false; }
                    unsigned long cp = 0;
                    for (int i = 0; i < 4; i++) {
                        int d = HexDigit(s[pos + 2 + i]);
                        if (d < 0) { err = "Bad \\u escape."; return false; }
                        cp = (cp << 4) | (unsigned long)d;
                    }
                    pos += 6;
                    // Surrogate pair handling.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= s.size() &&
                        s[pos] == '\\' && s[pos + 1] == 'u') {
                        unsigned long lo = 0;
                        for (int i = 0; i < 4; i++) {
                            int d = HexDigit(s[pos + 2 + i]);
                            if (d < 0) break;
                            lo = (lo << 4) | (unsigned long)d;
                        }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            pos += 6;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    out += '\\';
                    out += e;
                    pos += 2;
                    break;
            }
        } else {
            out += c;
            pos++;
        }
    }
    err = "Unterminated string.";
    return false;
}

bool ParseValue(const std::string& s, size_t& pos, Value& out, std::string& err);

bool ParseArray(const std::string& s, size_t& pos, Value& out, std::string& err) {
    pos++; // skip '['
    out.type = Value::Type::Array;
    SkipSpace(s, pos);
    if (pos < s.size() && s[pos] == ']') { pos++; return true; }
    while (pos < s.size()) {
        Value item;
        if (!ParseValue(s, pos, item, err)) return false;
        out.array.push_back(item);
        SkipSpace(s, pos);
        if (pos >= s.size()) { err = "Unterminated array."; return false; }
        char c = s[pos];
        if (c == ',') { pos++; SkipSpace(s, pos); continue; }
        if (c == ']') { pos++; return true; }
        err = "Expected ',' or ']'.";
        return false;
    }
    err = "Unterminated array.";
    return false;
}

bool ParseObject(const std::string& s, size_t& pos, Value& out, std::string& err) {
    pos++; // skip '{'
    out.type = Value::Type::Object;
    SkipSpace(s, pos);
    if (pos < s.size() && s[pos] == '}') { pos++; return true; }
    while (pos < s.size()) {
        SkipSpace(s, pos);
        if (pos >= s.size() || s[pos] != '"') { err = "Expected object key string."; return false; }
        std::string key;
        if (!ParseString(s, pos, key, err)) return false;
        SkipSpace(s, pos);
        if (pos >= s.size() || s[pos] != ':') { err = "Expected ':'."; return false; }
        pos++;
        Value val;
        if (!ParseValue(s, pos, val, err)) return false;
        out.object.emplace_back(key, val);
        SkipSpace(s, pos);
        if (pos >= s.size()) { err = "Unterminated object."; return false; }
        char c = s[pos];
        if (c == ',') { pos++; continue; }
        if (c == '}') { pos++; return true; }
        err = "Expected ',' or '}'.";
        return false;
    }
    err = "Unterminated object.";
    return false;
}

bool ParseValue(const std::string& s, size_t& pos, Value& out, std::string& err) {
    SkipSpace(s, pos);
    if (pos >= s.size()) { err = "Unexpected end of input."; return false; }
    char c = s[pos];
    switch (c) {
        case '{': return ParseObject(s, pos, out, err);
        case '[': return ParseArray(s, pos, out, err);
        case '"': {
            out.type = Value::Type::String;
            return ParseString(s, pos, out.string, err);
        }
        case 't':
            if (s.compare(pos, 4, "true") == 0) { out.type = Value::Type::Bool; out.boolean = true; pos += 4; return true; }
            err = "Invalid token.";
            return false;
        case 'f':
            if (s.compare(pos, 5, "false") == 0) { out.type = Value::Type::Bool; out.boolean = false; pos += 5; return true; }
            err = "Invalid token.";
            return false;
        case 'n':
            if (s.compare(pos, 4, "null") == 0) { out.type = Value::Type::Null; pos += 4; return true; }
            err = "Invalid token.";
            return false;
        default: {
            if (c == '-' || (c >= '0' && c <= '9')) {
                char* end = nullptr;
                double d = strtod(s.c_str() + pos, &end);
                if (end == s.c_str() + pos) { err = "Invalid number."; return false; }
                pos += (size_t)(end - (s.c_str() + pos));
                out.type = Value::Type::Number;
                out.number = d;
                return true;
            }
            err = "Unexpected character.";
            return false;
        }
    }
}

} // namespace

Value Parse(const std::string& text, std::string& errorOut) {
    Value result;
    errorOut.clear();
    size_t pos = 0;
    if (!ParseValue(text, pos, result, errorOut)) {
        errorOut = "JSON parse error: " + errorOut;
        return Value();
    }
    return result;
}

} // namespace json