"""pull the websocket fork and turn its client lock on.

A websocket client can be DELETED at any moment: the library deletes a
client on its async task when the TCP connection drops, and it does that
inside the removal of the client from its list - so the client is ALREADY
GONE by the time our own disconnect handler runs.

The fork (troyhacks/ESPAsyncWebServer) has the fix: the library takes a
recursive mutex across the removal, and the application takes the SAME
mutex for every fetch-and-use of a client.  Both halves of it are behind
WLED_ASYNCWS_CLIENT_LOCK, so the macro has to be defined for the library
AND for the application.

Without it the application walks a client's message queue after the client
has been freed:

    Guru Meditation Error: Core 0 panic'ed (Load address misaligned)
    MEPC 0x400ad974  LinkedList<AsyncWebSocketMessage*>::length()
                       AsyncWebSocketClient::queueLength()
    RA  0x400a2c7e  wsSendTempo()   wled00/ws.cpp:105
         handleWs()                wled00/ws.cpp:593

Two things are needed and neither of them was in place:

1. The macro has to be defined.  Without it the library's guarded paths
   are compiled out, and the application's own guard (wled.h's
   WsClientGuard) falls back to a mutex of its own, which cannot stop the
   library's delete.

2. The library's source in .pio/libdeps has to be the fork's.  PlatformIO
   clones a library once and never updates it, so a tree that was cloned
   before the fork got the guard keeps the old copy for ever - which is
   what happened here.
"""

import glob
import os
import subprocess

Import("env")  # noqa: F821  (injected by SCons)

# The application's own guard uses the library's lock, so the library has
# to have been built with it too.
env.Append(CPPDEFINES=["WLED_ASYNCWS_CLIENT_LOCK"])

libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
if not libdeps or not os.path.isdir(libdeps):
    print("[asyncws] no libdeps folder yet - nothing to do")
    raise SystemExit(0)

for path in sorted(glob.glob(os.path.join(libdeps, "*", "ESPAsyncWebServer*"))):
    if not os.path.isdir(os.path.join(path, ".git")):
        continue
    # The library is a build artefact, so it is brought to the fork's head
    # rather than patched in place.
    try:
        out = subprocess.run(
            ["git", "-C", path, "pull", "--ff-only"],
            capture_output=True, text=True, timeout=120,
        )
    except Exception as exc:  # noqa: BLE001  (a build must not stop here)
        print("[asyncws] could not pull %s: %s" % (path, exc))
        continue
    if out.returncode == 0:
        head = subprocess.run(
            ["git", "-C", path, "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True,
        ).stdout.strip()
        print("[asyncws] %s is at %s - the clients are guarded" % (path, head))
    else:
        # A tree that cannot be brought forward is not fatal: the guarded
        # code is simply not in it, and the crash stays possible.  Say so.
        print("[asyncws] could not bring %s forward - the websocket's client "
              "lock is not in it and a client can still be deleted under the "
              "application" % path)
        for line in (out.stderr or "").splitlines()[:4]:
            print("[asyncws]   " + line)
