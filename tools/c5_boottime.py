"""How long the ESP32-C5 takes to come up.

The board is started from nothing and the time until it answers on the
network is measured.  The board's own numbers come back with the answer,
so the time can be split up:

  * the time before the app started (the ROM, the bootloader) - that is
    the difference between the time here and the board's uptime, and it
    is only visible from out here,
  * the radio (the board's "wifi_ms"),
  * the setup (the board's "setup_ms").

Usage:
  python tools/c5_boottime.py            reboot it and time it
  python tools/c5_boottime.py --flash    flash it and time it
  python tools/c5_boottime.py --times 5  do it five times
"""

import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

HOST = os.environ.get("C5_HOST", "192.168.6.160")
PORT = os.environ.get("C5_PORT", "COM10")
TIMEOUT = 180.0     # how long to wait for the board, at all


def poll(timeout):
    """Waits for the board to answer.  Returns (seconds, answer) or (None, None)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with urllib.request.urlopen("http://%s/json/ar" % HOST, timeout=3) as r:
                return time.time() - t0, json.loads(r.read())
        except Exception:
            time.sleep(0.2)
    return None, None


def reboot():
    """Asks the board to restart, and waits for it to go away.

    Returns the moment it was asked.  The board answers the request and
    then restarts a tenth of a second later, so the wait for it to go
    away matters: without it the poll below would answer with the board
    that is still up, and the time would be nonsense.
    """
    # curl, not urllib: the board answers and then goes away, and urllib
    # waits for the connection to close rather than for the answer.
    asked = time.time()
    subprocess.run(["curl", "-s", "-m", "10", "-X", "POST",
                    "http://%s/c5reboot" % HOST],
                   capture_output=True, text=True)
    while time.time() - asked < 15:
        try:
            urllib.request.urlopen("http://%s/json/info" % HOST, timeout=2).read()
            time.sleep(0.05)
        except Exception:
            return asked
    print("  the board did not go away")
    return asked


def flash():
    """Flashes the board.  Returns the moment the reset happened."""
    py = sys.executable
    cmd = [py, os.path.join("tools", "c5_flash.py"), PORT]
    print("  flashing:", " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout[-800:])
        print(proc.stderr[-800:])
        raise SystemExit("the flash failed")
    # The flash resets the board as it finishes, so this is the moment.
    return time.time()


def report(elapsed, answer):
    me = answer.get("C5Remote", {})
    uptime = me.get("uptime_ms", 0) / 1000.0
    print("  up and answering after   %6.1f s" % elapsed)
    print("  the app had been up for   %6.1f s" % uptime)
    print("  before the app started    %6.1f s" % (elapsed - uptime))
    print("  the radio                 %6.2f s" % (me.get("wifi_ms", 0) / 1000.0))
    print("  the setup                 %6.2f s" % (me.get("setup_ms", 0) / 1000.0))
    print("  the panel                 %s" % ("yes" if me.get("panel") else "NO"))
    print("  the TF card               %s" % (me.get("sd", {}).get("status")))


def main():
    args = sys.argv[1:]
    times = 5
    if "--times" in args:
        times = int(args[args.index("--times") + 1])
    do_flash = "--flash" in args

    for i in range(times):
        print("%d of %d:" % (i + 1, times))
        if do_flash:
            started = flash()
        else:
            started = reboot()
        elapsed, answer = poll(TIMEOUT)
        if answer is None:
            print("  the board did not come up in %.0f s" % TIMEOUT)
            continue
        report(elapsed, answer)


if __name__ == "__main__":
    main()
