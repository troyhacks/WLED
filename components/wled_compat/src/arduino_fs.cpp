// arduino_fs.cpp - the Arduino filesystem classes, over ESP-IDF's esp_vfs.
//
// The filesystem is LittleFS, mounted through the esp_vfs_littlefs driver.
// The partition is labelled "spiffs" - the label the Arduino build used - so
// a board can be flashed with either build without losing its data.

#include "Arduino.h"
#include "FS.h"
#include "LittleFS.h"

extern "C" {
#include "esp_vfs_littlefs.h"
#include "esp_littlefs.h"
#include "esp_vfs.h"
#include "esp_log.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/param.h>
}

static const char *TAG = "wled_fs";

// The mount point and the partition label the Arduino build used.
static const char *FS_BASE_PATH = "/littlefs";
static const char *FS_PARTITION_LABEL = "spiffs";

// --- File -------------------------------------------------------------------

size_t File::write(uint8_t c)
{
    return write(&c, 1);
}

size_t File::write(const uint8_t *buffer, size_t size)
{
    if (_fd < 0 || !buffer || !size) {
        return 0;
    }
    ssize_t written = ::write(_fd, buffer, size);
    return written < 0 ? 0 : (size_t)written;
}

size_t File::read(uint8_t *buffer, size_t size)
{
    if (_fd < 0 || !buffer || !size) {
        return 0;
    }
    ssize_t got = ::read(_fd, buffer, size);
    return got < 0 ? 0 : (size_t)got;
}

int File::available()
{
    if (_fd < 0) {
        return 0;
    }
    off_t here = lseek(_fd, 0, SEEK_CUR);
    off_t end = lseek(_fd, 0, SEEK_END);
    lseek(_fd, here, SEEK_SET);
    if (end <= here) {
        return 0;
    }
    return (int)(end - here);
}

int File::read()
{
    uint8_t byte = 0;
    if (read(&byte, 1) != 1) {
        return -1;
    }
    return (int)byte;
}

int File::peek()
{
    if (_fd < 0) {
        return -1;
    }
    off_t here = lseek(_fd, 0, SEEK_CUR);
    uint8_t byte = 0;
    ssize_t got = ::read(_fd, &byte, 1);
    lseek(_fd, here, SEEK_SET);
    return got == 1 ? (int)byte : -1;
}

void File::flush()
{
    if (_fd >= 0) {
        fsync(_fd);
    }
}

size_t File::seek(uint32_t pos)
{
    if (_fd < 0) {
        return 0;
    }
    off_t at = lseek(_fd, pos, SEEK_SET);
    return at < 0 ? 0 : (size_t)at;
}

size_t File::position()
{
    if (_fd < 0) {
        return 0;
    }
    off_t here = lseek(_fd, 0, SEEK_CUR);
    return here < 0 ? 0 : (size_t)here;
}

size_t File::size()
{
    if (_fd < 0) {
        return 0;
    }
    struct stat st;
    if (fstat(_fd, &st) != 0) {
        return 0;
    }
    return (size_t)st.st_size;
}

bool File::find(const char *needle)
{
    return find(needle, 0);
}

bool File::find(const char *needle, size_t offset)
{
    if (_fd < 0 || !needle) {
        return false;
    }
    size_t needleLen = strlen(needle);
    if (!needleLen) {
        return true;
    }
    size_t length = size();
    if (offset >= length) {
        return false;
    }
    seek((uint32_t)offset);
    uint8_t window[64];
    size_t windowLen = 0;
    size_t matched = 0;
    while (true) {
        size_t got = read(window + windowLen, sizeof(window) - windowLen);
        if (!got) {
            return false;
        }
        windowLen += got;
        for (size_t i = 0; i < windowLen; i++) {
            if (window[i] == needle[matched]) {
                matched++;
                if (matched == needleLen) {
                    return true;
                }
            } else {
                matched = (window[i] == needle[0]) ? 1 : 0;
            }
        }
        // Keep the tail, in case the needle straddles two reads.
        if (windowLen >= sizeof(window) - needleLen) {
            size_t keep = needleLen - 1;
            memmove(window, window + windowLen - keep, keep);
            windowLen = keep;
        }
    }
}

void File::close()
{
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
}

char *File::openNextFile(const char *path)
{
    // The Arduino core walks a directory one entry at a time.  This keeps a
    // static handle so the walk works, which is enough for WLED's use.
    static DIR *dir = nullptr;
    static char current[128] = {0};

    if (path) {
        if (dir) {
            closedir(dir);
            dir = nullptr;
        }
        dir = opendir(path);
        if (!dir) {
            return nullptr;
        }
    }
    if (!dir) {
        return nullptr;
    }
    struct dirent *entry = readdir(dir);
    if (!entry) {
        closedir(dir);
        dir = nullptr;
        return nullptr;
    }
    snprintf(current, sizeof(current), "%s", entry->d_name);
    return current;
}

String File::readString()
{
    String out;
    uint8_t chunk[64];
    while (true) {
        size_t got = read(chunk, sizeof(chunk));
        if (!got) {
            break;
        }
        out.concat((const char *)chunk, got);
    }
    return out;
}

size_t File::readBytes(char *buffer, size_t length)
{
    return read((uint8_t *)buffer, length);
}

// --- FS ---------------------------------------------------------------------

bool FS::begin(bool formatOnFail, const char *basePath, uint8_t maxOpenFiles, const char *partitionLabel)
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = basePath ? basePath : FS_BASE_PATH;
    conf.partition_label = partitionLabel ? partitionLabel : FS_PARTITION_LABEL;
    conf.format_if_mount_failed = formatOnFail;
    conf.max_files = maxOpenFiles;
    conf.storage_type = LITTLEFS_STORAGE_TYPE_DEFAULT;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not mount %s (err %d)", conf.partition_label, (int)err);
        return false;
    }
    ESP_LOGI(TAG, "mounted %s at %s", conf.partition_label, conf.base_path);
    return true;
}

void FS::end()
{
    esp_vfs_littlefs_unregister(FS_PARTITION_LABEL);
}

File FS::open(const char *path, const char *mode)
{
    int flags = O_RDONLY;
    if (mode && strchr(mode, 'w')) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (mode && strchr(mode, 'a')) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    }
    int fd = ::open(path, flags);
    if (fd < 0) {
        return File();
    }
    File file(fd);
    snprintf(file._name, sizeof(file._name), "%s", path);
    return file;
}

File FS::openDir(const char *path)
{
    // The Arduino core's openDir returns a File that walks the directory.
    // The directory handle lives in the File's openNextFile().
    File file;
    snprintf(file._name, sizeof(file._name), "%s", path ? path : "/");
    return file;
}

bool FS::exists(const char *path)
{
    struct stat st;
    return path && ::stat(path, &st) == 0;
}

bool FS::remove(const char *path)
{
    return path && ::unlink(path) == 0;
}

bool FS::rename(const char *from, const char *to)
{
    return from && to && ::rename(from, to) == 0;
}

size_t FS::totalBytes()
{
    size_t total = 0;
    size_t used = 0;
    if (esp_littlefs_info(FS_BASE_PATH, &total, &used) != ESP_OK) {
        return 0;
    }
    return total;
}

size_t FS::usedBytes()
{
    size_t total = 0;
    size_t used = 0;
    if (esp_littlefs_info(FS_BASE_PATH, &total, &used) != ESP_OK) {
        return 0;
    }
    return used;
}

void FS::info()
{
    ESP_LOGI(TAG, "%u bytes used of %u", (unsigned)usedBytes(), (unsigned)totalBytes());
}

// --- LittleFS ---------------------------------------------------------------

LittleFS LittleFS;

bool LittleFS::begin(bool formatOnFail)
{
    return FS::begin(formatOnFail, FS_BASE_PATH, 10, FS_PARTITION_LABEL);
}
