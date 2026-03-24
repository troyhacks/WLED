// WString_compat.h — Arduino String class compatibility for pure IDF builds.
// Wraps std::string and adds Arduino-specific methods.
// Included from idf_compat.h when WLED_IDF_BUILD is defined.
#pragma once
#ifdef WLED_IDF_BUILD

#include <string>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <algorithm>

class String : public std::string {
public:
    // ─── Constructors ──────────────────────────────────────────────────────────
    String() : std::string() {}
    String(const char* s) : std::string(s ? s : "") {}
    String(const std::string& s) : std::string(s) {}
    String(std::string&& s) : std::string(std::move(s)) {}
    String(char c) : std::string(1, c) {}
    String(int v, int base = 10) {
        char buf[32];
        if (base == 16) snprintf(buf, sizeof(buf), "%x", v);
        else if (base == 8) snprintf(buf, sizeof(buf), "%o", v);
        else snprintf(buf, sizeof(buf), "%d", v);
        assign(buf);
    }
    String(unsigned int v, int base = 10) {
        char buf[32];
        if (base == 16) snprintf(buf, sizeof(buf), "%x", v);
        else snprintf(buf, sizeof(buf), "%u", v);
        assign(buf);
    }
    String(long v, int base = 10) {
        char buf[32];
        if (base == 16) snprintf(buf, sizeof(buf), "%lx", v);
        else snprintf(buf, sizeof(buf), "%ld", v);
        assign(buf);
    }
    String(unsigned long v, int base = 10) {
        char buf[32];
        if (base == 16) snprintf(buf, sizeof(buf), "%lx", v);
        else snprintf(buf, sizeof(buf), "%lu", v);
        assign(buf);
    }
    String(float v, int decimals = 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, (double)v);
        assign(buf);
    }
    String(double v, int decimals = 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        assign(buf);
    }

    // ─── Assignment ───────────────────────────────────────────────────────────
    String& operator=(const char* s)        { std::string::operator=(s ? s : ""); return *this; }
    String& operator=(const std::string& s) { std::string::operator=(s); return *this; }
    String& operator=(char c)               { std::string::assign(1, c); return *this; }

    // ─── Concatenation ────────────────────────────────────────────────────────
    String operator+(const String& rhs)      const { return String(std::string(*this) + std::string(rhs)); }
    String operator+(const char* rhs)        const { return String(std::string(*this) + (rhs ? rhs : "")); }
    String operator+(char rhs)               const { return String(std::string(*this) + rhs); }
    String operator+(int rhs)                const { return String(std::string(*this) + String(rhs).c_str()); }
    String operator+(unsigned int rhs)       const { return String(std::string(*this) + String(rhs).c_str()); }
    String operator+(long rhs)               const { return String(std::string(*this) + String(rhs).c_str()); }
    String operator+(unsigned long rhs)      const { return String(std::string(*this) + String(rhs).c_str()); }
    String operator+(float rhs)              const { return String(std::string(*this) + String(rhs).c_str()); }
    String operator+(double rhs)             const { return String(std::string(*this) + String(rhs).c_str()); }
    String& operator+=(const String& rhs)         { std::string::operator+=(rhs); return *this; }
    String& operator+=(const char* rhs)           { std::string::operator+=(rhs ? rhs : ""); return *this; }
    String& operator+=(char rhs)                  { std::string::operator+=(rhs); return *this; }

    // ─── Comparison ───────────────────────────────────────────────────────────
    bool equals(const String& s) const           { return *this == s; }
    bool equals(const char* s)   const           { return s && *this == s; }
    bool equalsIgnoreCase(const String& s) const {
        if (size() != s.size()) return false;
        for (size_t i = 0; i < size(); i++)
            if (tolower((*this)[i]) != tolower(s[i])) return false;
        return true;
    }
    bool equalsIgnoreCase(const char* s) const { return equalsIgnoreCase(String(s)); }

    // ─── Search ───────────────────────────────────────────────────────────────
    bool startsWith(const String& prefix, unsigned int offset = 0) const {
        if (offset + prefix.size() > size()) return false;
        return compare(offset, prefix.size(), prefix) == 0;
    }
    bool startsWith(const char* prefix) const { return startsWith(String(prefix)); }

    bool endsWith(const String& suffix) const {
        if (suffix.size() > size()) return false;
        return compare(size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    bool endsWith(const char* suffix) const { return endsWith(String(suffix)); }

    int indexOf(char ch, unsigned int fromIndex = 0) const {
        size_t pos = find(ch, fromIndex);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int indexOf(const String& s, unsigned int fromIndex = 0) const {
        size_t pos = find(s, fromIndex);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int indexOf(const char* s, unsigned int fromIndex = 0) const {
        if (!s) return -1;
        size_t pos = find(s, fromIndex);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int lastIndexOf(char ch) const {
        size_t pos = rfind(ch);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int lastIndexOf(const String& s) const {
        size_t pos = rfind(s);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }

    // ─── Substrings ───────────────────────────────────────────────────────────
    String substring(unsigned int left, unsigned int right) const {
        if (left > size()) return String();
        if (right > size()) right = (unsigned int)size();
        if (right < left) return String();
        return String(substr(left, right - left));
    }
    String substring(unsigned int left) const {
        if (left > size()) return String();
        return String(substr(left));
    }

    // ─── Case conversion ──────────────────────────────────────────────────────
    void toLowerCase() {
        for (auto& c : *this) c = (char)tolower((unsigned char)c);
    }
    void toUpperCase() {
        for (auto& c : *this) c = (char)toupper((unsigned char)c);
    }

    // ─── Trim ─────────────────────────────────────────────────────────────────
    void trim() {
        // Left trim
        size_t start = find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { clear(); return; }
        erase(0, start);
        // Right trim
        size_t end = find_last_not_of(" \t\r\n");
        if (end != std::string::npos) erase(end + 1);
    }

    // ─── Replace ──────────────────────────────────────────────────────────────
    void replace(const String& find_str, const String& replace_str) {
        size_t pos = 0;
        while ((pos = std::string::find(find_str, pos)) != std::string::npos) {
            std::string::replace(pos, find_str.size(), replace_str);
            pos += replace_str.size();
        }
    }
    void replace(char find_ch, char replace_ch) {
        for (auto& c : *this) if (c == find_ch) c = replace_ch;
    }

    // ─── Remove ───────────────────────────────────────────────────────────────
    void remove(unsigned int index) {
        if (index < size()) erase(index);
    }
    void remove(unsigned int index, unsigned int count) {
        if (index < size()) erase(index, count);
    }

    // ─── charAt / setCharAt ───────────────────────────────────────────────────
    char charAt(unsigned int index) const {
        if (index >= size()) return '\0';
        return (*this)[index];
    }
    void setCharAt(unsigned int index, char c) {
        if (index < size()) (*this)[index] = c;
    }

    // ─── Numeric conversions ──────────────────────────────────────────────────
    long    toInt()   const { return strtol(c_str(), nullptr, 10); }
    float   toFloat() const { return strtof(c_str(), nullptr); }
    double  toDouble()const { return strtod(c_str(), nullptr); }

    // ─── Length / capacity ────────────────────────────────────────────────────
    // std::string::length() and size() already available
    bool isEmpty() const { return empty(); }
    bool concat(const String& s)    { *this += s; return true; }
    bool concat(const char* s)      { if (s) *this += s; return true; }
    bool concat(char c)             { *this += c; return true; }
    bool reserve(unsigned int size) { std::string::reserve(size); return true; }
    int  compareTo(const String& s) const { return compare(s); }

    // ─── Operator [] already provided by std::string ─────────────────────────
};

// Non-member operator+ so "literal" + String works
inline String operator+(const char* lhs, const String& rhs) { return String(lhs) + rhs; }
inline String operator+(char lhs, const String& rhs)        { return String(lhs) + rhs; }

#endif // WLED_IDF_BUILD
