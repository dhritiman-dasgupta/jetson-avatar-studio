// Minimal JSON helpers for the line-delimited IPC protocol (flat string objects).
#pragma once
#include <string>
#include <map>
#include <sstream>

namespace jsonmin {

inline std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

// Build a flat {"k":"v",...} object from string pairs.
inline std::string obj(std::initializer_list<std::pair<std::string, std::string>> kv) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (auto &p : kv) {
        if (!first) os << ",";
        first = false;
        os << "\"" << esc(p.first) << "\":\"" << esc(p.second) << "\"";
    }
    os << "}";
    return os.str();
}

// Very small parser: extracts top-level "key":"value" and "key":bareword pairs.
inline std::map<std::string, std::string> parseFlat(const std::string &s) {
    std::map<std::string, std::string> m;
    size_t i = 0, n = s.size();
    auto skipws = [&]() { while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ',' || s[i] == '{' || s[i] == '}')) i++; };
    while (i < n) {
        skipws();
        if (i >= n || s[i] != '"') { i++; continue; }
        i++;                                    // opening quote of key
        std::string key;
        while (i < n && s[i] != '"') key += s[i++];
        if (i < n) i++;                         // closing quote
        skipws();
        if (i < n && s[i] == ':') i++;
        while (i < n && s[i] == ' ') i++;
        std::string val;
        if (i < n && s[i] == '"') {             // string value
            i++;
            while (i < n && s[i] != '"') { if (s[i] == '\\' && i + 1 < n) i++; val += s[i++]; }
            if (i < n) i++;
        } else {                                // bareword (number/bool/null)
            while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ' ') val += s[i++];
        }
        m[key] = val;
    }
    return m;
}

} // namespace jsonmin
