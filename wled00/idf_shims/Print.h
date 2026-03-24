// Print.h shim for WLED IDF builds.
// LovyanGFX includes <Print.h> when ARDUINO is defined; we define ARDUINO=10819
// to satisfy WLED's version checks, so we must provide this compatibility header.
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>

class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buf, size_t size) {
        size_t n = 0;
        while (size--) n += write(*buf++);
        return n;
    }
    size_t write(const char *str) {
        if (!str) return 0;
        return write(reinterpret_cast<const uint8_t*>(str), strlen(str));
    }
    size_t write(const char *buf, size_t size) {
        return write(reinterpret_cast<const uint8_t*>(buf), size);
    }
    int availableForWrite() { return 0; }
    void flush() {}

    size_t print(const char *s)        { return write(s ? s : ""); }
    size_t print(char c)               { return write(static_cast<uint8_t>(c)); }
    size_t print(int n, int = 10)      { char b[16]; snprintf(b,16,"%d",n);  return write(b); }
    size_t print(unsigned int n, int = 10) { char b[16]; snprintf(b,16,"%u",n); return write(b); }
    size_t print(long n, int = 10)     { char b[24]; snprintf(b,24,"%ld",n); return write(b); }
    size_t print(unsigned long n, int = 10) { char b[24]; snprintf(b,24,"%lu",n); return write(b); }
    size_t print(double n, int d = 2)  { char b[32]; snprintf(b,32,"%.*f",d,n); return write(b); }

    size_t println(const char *s = "") { size_t r = print(s); r += write("\r\n"); return r; }
    size_t println(char c)             { size_t r = print(c); r += write("\r\n"); return r; }
    size_t println(int n, int b = 10)  { size_t r = print(n,b); r += write("\r\n"); return r; }
    size_t println(unsigned int n, int b = 10) { size_t r = print(n,b); r += write("\r\n"); return r; }
    size_t println(long n, int b = 10) { size_t r = print(n,b); r += write("\r\n"); return r; }
    size_t println(unsigned long n, int b = 10) { size_t r = print(n,b); r += write("\r\n"); return r; }
    size_t println(double n, int d = 2){ size_t r = print(n,d); r += write("\r\n"); return r; }
};
