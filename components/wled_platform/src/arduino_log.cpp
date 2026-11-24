// arduino_log.cpp - the Arduino core's log helpers.
//
// These are the functions the core declares in its logging header: the file
// name out of a path, and the watchdog switches for the second core.  They
// live here, with the header, because the filesystem needs them - and the
// filesystem is in this component.

#include <string.h>

#include "arduino_log.h"

const char *pathToFileName(const char *path)
{
    if (!path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash > slash) {
        slash = backslash;
    }
    return slash ? slash + 1 : path;
}

// The core's watchdog switches for the second core.  The P4's watchdog is
// looked after by WLED itself, so these do nothing.
void enableCore0WDT() {}
bool disableCore0WDT() { return true; }
void enableCore1WDT() {}
bool disableCore1WDT() { return true; }
