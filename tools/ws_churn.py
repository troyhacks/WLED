#!/usr/bin/env python
"""
Churn the websocket clients, to exercise the delete.

The crash of 2026-11-25 was a use after free: the tempo push fetched a
client and used it, while the async task deleted it because its
connection had dropped.  The fix puts the library's client list under one
lock, and the application holds that lock around its fetch and use.

This makes the race happen on purpose: it connects, subscribes to the
TEMPO stream - the exact path that faulted - and then drops the
connection, sometimes cleanly and sometimes by killing the socket, while
the board is pushing the beat.

Usage:
    python tools/ws_churn.py [seconds] [the pause between clients]
"""
import asyncio
import json
import os
import random
import sys
import time

import websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
URL = f"ws://{HOST}/ws"


async def one_client(n, abrupt):
    """One client: connect, take the tempo stream, and go away."""
    try:
        async with websockets.connect(URL, max_size=None, open_timeout=10,
                                      close_timeout=2) as ws:
            await ws.send(json.dumps({"tempo": True}))
            # Take a couple of frames, so the board is really pushing to
            # this client when it goes.
            got = 0
            deadline = time.time() + 1.0
            while got < 3 and time.time() < deadline:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=0.4)
                except asyncio.TimeoutError:
                    continue
                except Exception:
                    break
                if isinstance(raw, str) and raw.startswith("{"):
                    got += 1
            if abrupt:
                # Kill the socket: no close frame, so the server sees an
                # abrupt disconnect - the worst case for the delete.
                try:
                    ws.transport.abort()
                except Exception:
                    pass
            return got, True
    except Exception as e:
        return 0, False


async def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 600.0
    pause = float(sys.argv[2]) if len(sys.argv) > 2 else 0.4
    t0 = time.time()
    n = 0
    frames = 0
    failed = 0
    abrupt_count = 0
    while time.time() - t0 < seconds:
        # Every third client is killed rather than closed.
        abrupt = (n % 3) == 2
        got, ok = await one_client(n, abrupt)
        if abrupt:
            abrupt_count += 1
        if ok:
            frames += got
        else:
            failed += 1
        n += 1
        if n % 20 == 0:
            print("[%6.1f s] %d clients, %d frames, %d failed, %d killed"
                  % (time.time() - t0, n, frames, failed, abrupt_count),
                  flush=True)
        await asyncio.sleep(pause)
    print("[%6.1f s] done: %d clients, %d frames, %d failed, %d killed"
          % (time.time() - t0, n, frames, failed, abrupt_count))


if __name__ == "__main__":
    asyncio.run(main())
