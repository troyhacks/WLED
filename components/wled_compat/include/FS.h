// FS.h - the Arduino filesystem classes, over ESP-IDF's esp_vfs.
//
// A File is a POSIX file descriptor with the Arduino spelling on top, and FS
// is the directory of operations.  The filesystem itself is LittleFS, mounted
// through the esp_vfs_littlefs driver (the "littlefs" managed component).

#pragma once

#include "Arduino.h"
#include "Print.h"
#include "Stream.h"

typedef enum {
    O_READ = 0x01,
    O_WRITE = 0x02,
    O_APPEND = 0x04,
    O_SYNC = 0x08,
    O_TRUNC = 0x10,
    O_AT_END = 0x20,
    O_CREAT = 0x40
} open_mode_t;

// The mode strings the Arduino core's FS.h defines.
#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

class File : public Stream {
public:
    File() : _fd(-1) {}
    explicit File(int fd) : _fd(fd) {}
    ~File() override { close(); }

    operator bool() const { return _fd >= 0; }
    size_t write(uint8_t c);
    size_t write(const uint8_t *buffer, size_t size) override;
    size_t read(uint8_t *buffer, size_t size) override;
    int available() override;
    int read() override;
    int peek() override;
    void flush() override;
    size_t seek(uint32_t pos);
    size_t position();
    size_t size();
    bool find(const char *needle);
    bool find(const char *needle, size_t offset);
    void close();
    const char *name() const { return _name; }
    const char *fullPath() const { return _name; }

    // The Arduino core's File carries the mode it was opened in.
    char *openNextFile(const char *path = nullptr);
    String readString();
    size_t readBytes(char *buffer, size_t length);

private:
    friend class FS;
    int _fd;
    char _name[128] = {0};
};

class FS {
public:
    File open(const char *path, const char *mode = FILE_READ);
    File openDir(const char *path = "/");
    bool exists(const char *path);
    bool remove(const char *path);
    bool rename(const char *from, const char *to);
    size_t totalBytes();
    size_t usedBytes();
    void info();
    bool begin(bool formatOnFail = false, const char *basePath = "/littlefs", uint8_t maxOpenFiles = 10, const char *partitionLabel = "spiffs");
    void end();
};
