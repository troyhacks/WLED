// LittleFS.h shim for WLED IDF builds.
// The IDF LittleFS VFS is mounted at /littlefs in main_idf.cpp.
// This header provides Arduino-compatible File and FS objects backed by POSIX.
#pragma once
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include "idf_compat.h"   // String, millis, etc.
#include "esp_littlefs.h"

#define LITTLEFS_BASE "/littlefs"

// ── SeekMode ──────────────────────────────────────────────────────────────────
enum SeekMode { SeekSet = SEEK_SET, SeekCur = SEEK_CUR, SeekEnd = SEEK_END };

// ── FSInfo ────────────────────────────────────────────────────────────────────
struct FSInfo {
    size_t totalBytes = 0;
    size_t usedBytes  = 0;
    size_t blockSize  = 4096;
    size_t pageSize   = 256;
    size_t maxOpenFiles = 10;
    size_t maxPathLength = 255;
};

// ── File class ────────────────────────────────────────────────────────────────
class File {
    FILE*  _fp   = nullptr;
    DIR*   _dir  = nullptr;  // set when opened as a directory
    String _name;
    String _dir_base;        // absolute path used for openNextFile
    size_t _size = 0;
    bool   _valid = false;

    void _calcSize() {
        if (!_fp) return;
        long cur = ftell(_fp);
        fseek(_fp, 0, SEEK_END);
        _size = (size_t)ftell(_fp);
        fseek(_fp, cur, SEEK_SET);
    }

public:
    File() = default;
    // Constructor for regular files
    File(FILE* fp, const char* name) : _fp(fp), _name(name), _valid(fp != nullptr) {
        if (_fp) _calcSize();
    }
    // Constructor for directory handles
    File(DIR* dir, const char* abs_path, const char* display_name)
        : _dir(dir), _name(display_name), _dir_base(abs_path), _valid(dir != nullptr) {}

    operator bool() const { return _valid && (_fp != nullptr || _dir != nullptr); }

    void close() {
        if (_fp)  { fclose(_fp);   _fp  = nullptr; }
        if (_dir) { closedir(_dir); _dir = nullptr; }
        _valid = false;
    }

    // openNextFile: iterate directory entries, return next regular file
    File openNextFile() {
        if (!_dir) return File();
        struct dirent* entry;
        while ((entry = readdir(_dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char full[300];
            snprintf(full, sizeof(full), "%s/%s", _dir_base.c_str(), entry->d_name);
            FILE* fp = fopen(full, "r");
            if (fp) return File(fp, entry->d_name);
        }
        return File();
    }

    size_t size() { return _size; }

    size_t read(uint8_t* buf, size_t len) {
        if (!_fp) return 0;
        return fread(buf, 1, len, _fp);
    }
    int read() {
        if (!_fp) return -1;
        int c = fgetc(_fp);
        return c == EOF ? -1 : c;
    }

    size_t write(const uint8_t* buf, size_t len) {
        if (!_fp) return 0;
        return fwrite(buf, 1, len, _fp);
    }
    size_t write(uint8_t b) { return write(&b, 1); }

    bool seek(uint32_t pos, SeekMode mode = SeekSet) {
        if (!_fp) return false;
        return fseek(_fp, pos, (int)mode) == 0;
    }

    size_t position() {
        if (!_fp) return 0;
        long p = ftell(_fp);
        return p < 0 ? 0 : (size_t)p;
    }

    int available() {
        if (!_fp) return 0;
        long cur = ftell(_fp);
        fseek(_fp, 0, SEEK_END);
        long end = ftell(_fp);
        fseek(_fp, cur, SEEK_SET);
        return (int)(end - cur);
    }

    const char* name() const { return _name.c_str(); }

    // Minimal print support for files opened for writing
    size_t print(const char* s)  { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
    size_t println(const char* s="") {
        size_t n = print(s);
        n += write((const uint8_t*)"\n", 1);
        return n;
    }
    size_t print(int v) { char b[16]; snprintf(b,16,"%d",v); return print(b); }
    size_t println(int v) { char b[16]; snprintf(b,16,"%d\n",v); return print(b); }

    // printf: formatted write (used by artnetmap and other usermods)
    template<typename... Args>
    size_t printf(const char* fmt, Args... args) {
        if (!_fp) return 0;
        return (size_t)fprintf(_fp, fmt, args...);
    }

    // peek: read next byte without advancing position
    int peek() {
        if (!_fp) return -1;
        int c = fgetc(_fp);
        if (c != EOF) ungetc(c, _fp);
        return c == EOF ? -1 : c;
    }

    // getLastWrite: not supported in IDF; return 0
    time_t getLastWrite() { return 0; }

    // find: scan forward until target char is found
    bool find(const char* target) {
        if (!_fp || !target) return false;
        size_t tlen = strlen(target);
        if (tlen == 0) return true;
        int c;
        size_t matched = 0;
        while ((c = fgetc(_fp)) != EOF) {
            if ((char)c == target[matched]) {
                if (++matched == tlen) return true;
            } else {
                matched = 0;
            }
        }
        return false;
    }
    bool find(char target) { char t[2] = {target, 0}; return find(t); }

    // findUntil: scan forward until target or terminator
    bool findUntil(const char* target, const char* terminator) {
        if (!_fp || !target) return false;
        int c;
        while ((c = fgetc(_fp)) != EOF) {
            if (terminator && strchr(terminator, c)) return false;
            if ((char)c == target[0]) return true;
        }
        return false;
    }

    // readBytesUntil: read into buf until terminator or len bytes
    size_t readBytesUntil(char terminator, char* buf, size_t len) {
        if (!_fp || len == 0) return 0;
        size_t n = 0;
        int c;
        while (n < len - 1 && (c = fgetc(_fp)) != EOF) {
            if ((char)c == terminator) break;
            buf[n++] = (char)c;
        }
        buf[n] = '\0';
        return n;
    }
    size_t readBytesUntil(char terminator, uint8_t* buf, size_t len) {
        return readBytesUntil(terminator, (char*)buf, len);
    }

    // readStringUntil: read into String until terminator
    String readStringUntil(char terminator) {
        String s;
        if (!_fp) return s;
        int c;
        while ((c = fgetc(_fp)) != EOF && (char)c != terminator)
            s += (char)c;
        return s;
    }
};

// ── LittleFS FS object ────────────────────────────────────────────────────────
class LittleFSClass {
public:
    bool begin(bool formatOnFail = false, const char* basePath = LITTLEFS_BASE,
               uint8_t maxFiles = 10, const char* partLabel = "spiffs") {
        // VFS already mounted in main_idf.cpp; just verify mount point is accessible
        struct stat st;
        return stat(basePath, &st) == 0;
    }
    void end() {}

    File open(const char* path, const char* mode = "r") {
        // Prepend base path if not already there
        char full[300];
        if (path[0] == '/') snprintf(full, sizeof(full), LITTLEFS_BASE "%s", path);
        else                snprintf(full, sizeof(full), LITTLEFS_BASE "/%s", path);
        // Check if it's a directory — return a dir-capable File for openNextFile()
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR* dir = opendir(full);
            return File(dir, full, path);
        }
        FILE* fp = fopen(full, mode);
        return File(fp, path);
    }
    File open(const String& path, const char* mode = "r") { return open(path.c_str(), mode); }

    bool exists(const char* path) {
        char full[300];
        if (path[0] == '/') snprintf(full, sizeof(full), LITTLEFS_BASE "%s", path);
        else                snprintf(full, sizeof(full), LITTLEFS_BASE "/%s", path);
        struct stat st;
        return stat(full, &st) == 0;
    }
    bool exists(const String& path) { return exists(path.c_str()); }

    bool remove(const char* path) {
        char full[300];
        if (path[0] == '/') snprintf(full, sizeof(full), LITTLEFS_BASE "%s", path);
        else                snprintf(full, sizeof(full), LITTLEFS_BASE "/%s", path);
        return unlink(full) == 0;
    }
    bool remove(const String& path) { return remove(path.c_str()); }

    bool rename(const char* from, const char* to) {
        char f[300], t[300];
        snprintf(f, sizeof(f), LITTLEFS_BASE "%s", from[0]=='/' ? from : (String("/")+from).c_str());
        snprintf(t, sizeof(t), LITTLEFS_BASE "%s", to[0]=='/'   ? to   : (String("/")+to).c_str());
        return ::rename(f, t) == 0;
    }

    bool mkdir(const char* path) {
        char full[300];
        if (path[0] == '/') snprintf(full, sizeof(full), LITTLEFS_BASE "%s", path);
        else                snprintf(full, sizeof(full), LITTLEFS_BASE "/%s", path);
        return ::mkdir(full, 0775) == 0;
    }

    bool info(FSInfo& info) {
        size_t total = 0, used = 0;
        if (esp_littlefs_info("spiffs", &total, &used) == ESP_OK) {
            info.totalBytes = total;
            info.usedBytes  = used;
            return true;
        }
        return false;
    }
    // Alias for Arduino FS compat
    bool info64(FSInfo& i) { return info(i); }

    // Convenience properties used directly in some WLED files
    size_t totalBytes() { FSInfo fi; return info(fi) ? fi.totalBytes : 0; }
    size_t usedBytes()  { FSInfo fi; return info(fi) ? fi.usedBytes  : 0; }

    bool format() {
        // Unmount, format, remount via IDF
        esp_littlefs_format("spiffs");
        return true;
    }
};

extern LittleFSClass LittleFS;
// Alias LITTLEFS → LittleFS for lorol-littlefs compat
#ifndef LITTLEFS
#define LITTLEFS LittleFS
#endif
