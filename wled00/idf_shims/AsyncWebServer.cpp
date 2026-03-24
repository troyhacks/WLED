// AsyncWebServer.cpp — out-of-line definitions for the AsyncWebServer IDF shim.
// Only compiled in WLED_IDF_BUILD (CMakeLists.txt GLOB_RECURSE picks this up).
#ifdef WLED_IDF_BUILD

#include "AsyncWebServer.h"

// Global pointer used by the 404 error-handler lambda (which cannot carry user_ctx).
// WLED has exactly one AsyncWebServer instance so a static pointer is safe.
AsyncWebServer* _g_aws_instance = nullptr;

#endif // WLED_IDF_BUILD
