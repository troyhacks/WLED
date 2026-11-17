"""
The handoff: a Pro DJ Link deck arriving, and leaving again.

The board drives the effects from ONE grid, and that grid is whichever
of two is available: the deck's, or the board's own estimate of the
music.  When the deck arrives the grid CHANGES, and when it leaves it
changes back - and a grid that changes under the effects is a light
that teleports.

This measures the teleport.  A light that moves along the bar (the Bar
Mover, whose position IS the bar's phase) is watched through the liveview
while a fake deck is started and stopped, and the position's path is
fitted to a straight line.  The fit's residuals are the movement that
is NOT explained by the bar's steady progress - and at the moment the
grid changes, that residual IS the jump.

  python tools/beat_handoff.py --offset 0     # the deck agrees with the board
  python tools/beat_handoff.py --offset 2     # the deck's downbeat is 2 beats round

The offset matters: the board's beat-in-bar is a CONVENTION (an
audio-only tracker cannot know where the bar starts), and the deck's is
the track's real one, so in the real world the two numberings are
normally rotated against each other.  A handoff that is smooth with
--offset 0 but jumps a quarter of a bar with --offset 2 is smooth only
by luck.

TWO THINGS THIS TEST HAS TO RESPECT:

  - the board's web server wedges after roughly sixty GETs, so the
    reads here are few and slow;
  - the liveview's latency JITTERS by a few pixels' worth of travel, so
    a jump is only believable when it is much bigger than that.  The
    noise floor is printed with the result.
"""
import argparse
import asyncio
import json
import os
import subprocess
import sys
import time
import urllib.request

import websockets

HOST = os.environ.get("WLED_HOST", "192.168.6.249")
WS = f"ws://{HOST}/ws"
STATE = f"http://{HOST}/json/state"
AR = f"http://{HOST}/json/ar"

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = os.path.join(HERE, "..", "usermods", "audioreactive", "prolink_fake.py")

# The Bar Mover: fx 237.  The light's position is the bar's phase.
FX_BAR_MOVER = 237


def get(url):
    with urllib.request.urlopen(url, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def post_state(body):
    req = urllib.request.Request(
        STATE, data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def decode(frame):
    """The liveview frame: 'L', a version, the width, the height, then RGB."""
    if len(frame) < 4 or frame[0:1] != b"L":
        return None
    w, h = frame[2], frame[3]
    px = []
    for i in range(w * h):
        o = 4 + i * 3
        if o + 3 > len(frame):
            break
        px.append((frame[o], frame[o + 1], frame[o + 2]))
    return w, h, px


def head_of(w, px):
    """Where the Bar Mover's light is, to a fraction of a pixel.

    The trail is one pixel long (sx=0), so exactly two pixels are lit:
    the one the head is over and the one it is crossing into, at
    (1 - f) and f.  The brightness-weighted centre of the lit pixels is
    then the head exactly - not to the nearest pixel, but to the
    fraction between them.
    """
    top = px[:w]                      # row 0 of the matrix
    num = 0.0
    den = 0.0
    for i, (r, g, b) in enumerate(top):
        v = r + g + b
        if v > 8:
            num += i * v
            den += v
    if den <= 0.0:
        return None
    return num / den


async def capture(seconds, frames, events):
    """Watch the liveview, recording (t, head_x).

    The board stops pushing the liveview on a connection after about
    twenty seconds - it does not say so and it does not close the
    connection, it just goes quiet.  So the watch reconnects when the
    frames stop arriving, and the time base is kept across the
    reconnections.
    """
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            async with websockets.connect(WS, max_size=None, open_timeout=5) as ws:
                await ws.send(json.dumps({"lv": True}))
                last = time.time()
                while time.time() - t0 < seconds:
                    if time.time() - last > 2.5:
                        print(f"  the liveview went quiet at "
                              f"{last-t0:.1f}s - reconnecting")
                        break
                    try:
                        raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
                    except asyncio.TimeoutError:
                        continue
                    if isinstance(raw, str) or not isinstance(raw, (bytes, bytearray)):
                        continue
                    last = time.time()
                    d = decode(raw)
                    if not d:
                        continue
                    w, h, px = d
                    x = head_of(w, px)
                    if x is not None:
                        frames.append((time.time() - t0, x, w))
        except Exception as e:
            print(f"  liveview dropped ({type(e).__name__}) - reconnecting")
            await asyncio.sleep(0.3)


def tempo_now():
    try:
        t = get(AR)["AudioReactive"]["tempo"]
        return t
    except Exception:
        return None


async def watch_tempo(seconds, out):
    """Read the tempo slowly.  The board's web server has a GET budget."""
    t0 = time.time()
    while time.time() - t0 < seconds:
        t = tempo_now()
        if t:
            out.append((time.time() - t0, t.get("source"), t.get("source_bpm"),
                        t.get("source_beat"), t.get("source_32"),
                        t.get("ref_err_ms"), t.get("ref_offset"),
                        t.get("ref_locked"), t.get("ref_slips")))
        await asyncio.sleep(4.0)


def fit(frames, t0, t1):
    """A straight line through the light's path."""
    seg = [(t, x) for (t, x, w) in frames if t0 <= t <= t1]
    if len(seg) < 8:
        return None
    n = len(seg)
    sx = sum(t for t, x in seg)
    sy = sum(x for t, x in seg)
    sxx = sum(t * t for t, x in seg)
    sxy = sum(t * x for t, x in seg)
    den = n * sxx - sx * sx
    if abs(den) < 1e-9:
        return None
    b = (n * sxy - sx * sy) / den
    a = (sy - b * sx) / n
    res = [(t, x - (a + b * t)) for (t, x) in seg]
    return {"a": a, "b": b, "res": res, "n": n,
            "t0": seg[0][0], "t1": seg[-1][0]}


def median(v):
    if not v:
        return 0.0
    s = sorted(v)
    m = len(s) // 2
    if len(s) % 2:
        return s[m]
    return 0.5 * (s[m - 1] + s[m])


def unwrap(frames, n):
    """The path, with the bar's wrap taken out.

    The bar's phase goes 0..1 and then back to 0, so the light falls off
    the end and comes back on.  That is a jump of the whole bar, and it
    is not the jump we are looking for - so the position is unwrapped
    against the shortest way round, which keeps the real jumps and
    throws the wrap away.

    A GAP in the frames - the liveview goes quiet and the watch
    reconnects - is handled separately: the light moved on, and the bar
    may have turned over more than once while nobody was looking, so the
    raw difference is meaningless and the steady rate is used instead.
    """
    rates = []
    for i in range(1, len(frames)):
        dt = frames[i][0] - frames[i - 1][0]
        if 0 < dt < 0.6:
            rates.append((frames[i][1] - frames[i - 1][1]) / dt)
    rate = median(rates) if rates else 0.0

    out = []
    prevT = None
    prevX = None
    acc = None
    for t, x, w in frames:
        if prevT is None:
            acc = x
        else:
            dt = t - prevT
            if dt > 0.6:
                acc += rate * dt
            else:
                d = x - prevX
                half = n / 2.0
                while d > half:
                    d -= n
                while d < -half:
                    d += n
                acc += d
        prevT = t
        prevX = x
        out.append((t, acc, x))
    return out


def residual_steps(path):
    """The steps of the path that the bar's steady progress cannot explain.

    The unwrapped path has no wrap in it - the bar's own reset is taken
    out - so ANY step left over is a real discontinuity: the grid
    changing hands.

    The speed is taken as the MEDIAN of the steps, so one big jump
    cannot drag it.
    """
    out = []
    for i in range(1, len(path)):
        dt = path[i][0] - path[i - 1][0]
        if dt <= 0:
            continue
        out.append((path[i][0], path[i][1] - path[i - 1][1], dt))
    if not out:
        return []
    speed = median([d for (_, d, _) in out]) / median([dt for (_, _, dt) in out])
    return [(t, d - speed * dt, dt) for (t, d, dt) in out], speed


def jump_at(path, t_at, back=6.0, fwd=6.0, near=0.5):
    """How far the light moved that the bar's steady progress cannot explain.

    Two straight lines are fitted - one to the seconds before the change
    and one to the seconds after - and each is corrected by the MEDIAN
    of its own residuals over the half second nearest the change.  The
    median is used because the liveview's latency jitters, and a median
    does not care about a frame that arrived late.
    """
    fb = fit(path, max(0.0, t_at - back), t_at - near)
    fa = fit(path, t_at + near, t_at + fwd)
    if not fb or not fa:
        return None
    rb = [v for (t, v) in fb["res"] if t >= t_at - near]
    ra = [v for (t, v) in fa["res"] if t <= t_at + near]
    xb = fb["a"] + fb["b"] * t_at + median(rb)
    xa = fa["a"] + fa["b"] * t_at + median(ra)
    noise = median([abs(v) for (_, v) in fb["res"]] +
                   [abs(v) for (_, v) in fa["res"]])
    # The raw way round: where the light actually was, just before and
    # just after, taken as the medians of the UNWRAPPED positions (the
    # bar's own wrap must not be counted as a jump).  The fitted jump
    # above can be thrown off by the fit; this cannot.
    raw_b = median([x for (t, x, r) in path if t_at - 0.35 <= t <= t_at])
    raw_a = median([x for (t, x, r) in path if t_at <= t <= t_at + 0.35])
    return {"jump": xa - xb, "noise": noise,
            "raw_jump": raw_a - raw_b,
            "b_before": fb["b"], "b_after": fa["b"]}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--offset", type=int, default=0,
                    help="rotate the deck's bar numbering against the board's")
    ap.add_argument("--seconds", type=float, default=44.0)
    ap.add_argument("--deck-at", type=float, default=12.0,
                    help="when the deck is started")
    ap.add_argument("--deck-off-at", type=float, default=30.0,
                    help="when the deck is stopped")
    ap.add_argument("--delay", type=float, default=40.0)
    ap.add_argument("--keep", action="store_true",
                    help="leave the segment on the Bar Mover")
    args = ap.parse_args()

    before = get(STATE)["seg"][0]
    saved = {k: before.get(k) for k in
             ("fx", "sx", "ix", "c1", "c2", "c3", "o1", "o2", "o3",
              "col", "pal", "m12", "si")}
    print("saved:", {k: saved[k] for k in ("fx", "sx", "ix", "o1", "o2", "o3")})

    # The Bar Mover, with the trail at one pixel, so the light is a point
    # and its position can be read to a fraction of a pixel.  Round OFF,
    # so the position IS the bar's phase and does not turn round, and
    # STEPS OFF - with Steps on, the position is quantised to the beat,
    # which is four places on the bar and not a position at all.
    #
    # The light is BLUE, not white: the liveview drops the white channel
    # of an RGBW strip, so a white light shows as nothing at all in the
    # preview (the strip itself is fine).
    post_state({"seg": [{"id": 0, "fx": FX_BAR_MOVER, "sx": 0, "ix": 0,
                         "o1": False, "o2": False, "o3": False,
                         "col": [[0, 96, 255, 0], [0, 0, 0, 0], [0, 0, 0, 0]]}]})
    time.sleep(0.6)

    frames, tempo = [], []
    proc = {"p": None}

    def start_deck():
        cmd = [sys.executable, "-u", FAKE, "--host", HOST, "--watch",
               "--delay", str(args.delay)]
        if args.offset:
            cmd += ["--beat-offset", str(args.offset)]
        proc["p"] = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.STDOUT)
        print(f"  t={args.deck_at:5.1f}s  the deck is started "
              f"(numbering rotated by {args.offset})")

    def stop_deck():
        if proc["p"]:
            proc["p"].terminate()
            try:
                proc["p"].wait(timeout=5)
            except Exception:
                proc["p"].kill()
            proc["p"] = None
        print(f"  t={args.deck_off_at:5.1f}s  the deck is stopped")

    async def both():
        task_cap = asyncio.ensure_future(capture(args.seconds, frames, None))
        task_tmp = asyncio.ensure_future(watch_tempo(args.seconds, tempo))
        # The deck is started and stopped on the wall clock, so the
        # switch has a time to be measured against.
        await asyncio.sleep(args.deck_at)
        start_deck()
        await asyncio.sleep(args.deck_off_at - args.deck_at)
        stop_deck()
        await asyncio.gather(task_cap, task_tmp)

    try:
        asyncio.run(both())
    finally:
        if proc["p"]:
            proc["p"].kill()
        if not args.keep:
            post_state({"seg": [dict(saved, id=0)]})
            print("restored:", {k: saved[k] for k in
                                ("fx", "sx", "ix", "o1", "o2", "o3")})

    if not frames:
        print("no frames")
        return

    n = frames[0][2]
    span = frames[-1][0]
    print(f"\n{len(frames)} frames over {span:.1f}s "
          f"({len(frames)/max(span, 0.01):.1f} fps), {n} px wide")

    print("\n--- the source, on the timeline ---")
    for row in tempo:
        t, src, bpm, beat, s32, err, off, lk, slips = row
        print(f"  {t:6.1f}s  {str(src):9s} bpm={bpm if bpm is None else round(bpm,2)}"
              f" beat={beat} 32={s32}"
              f"  err={err if err is None else round(err,1)}ms"
              f"  rot={off}  locked={lk} slips={slips}")

    # When the source actually changed: the first sample with the deck,
    # and the first one without it again.
    t_on = t_off = None
    for t, src, *_ in tempo:
        if src == "PRO LINK" and t_on is None:
            t_on = t
        if t_on is not None and src != "PRO LINK" and t_off is None:
            t_off = t
    print(f"\n  the deck was driving from t={t_on} to t={t_off}")

    path = unwrap(frames, n)

    # The frames, on disk, so the switch can be looked at closely
    # afterwards without having to catch it again.
    try:
        with open(os.path.join(HERE, "beat_handoff_frames.json"), "w") as f:
            json.dump(frames, f)
        print(f"\n  the frames are in {os.path.join(HERE, 'beat_handoff_frames.json')}")
    except Exception as e:
        print("  (could not save the frames:", e, ")")

    print("\n--- the light's path ---")
    for name, t0, t1 in (("before", 0.0, args.deck_at),
                         ("deck up", args.deck_at, args.deck_off_at),
                         ("after", args.deck_off_at, args.seconds)):
        f = fit(path, t0, t1)
        if not f:
            print(f"  {name:8s}  no data")
            continue
        r = [abs(v) for (_, v) in f["res"]]
        print(f"  {name:8s}  {f['n']:4d} frames  {f['b']:7.2f} px/s  "
              f"noise(median |residual|) = {median(r):5.2f} px")

    print("\n--- the handoff ---")
    # The jumps are found in the LIGHT, not in the polls: the polls are
    # four seconds apart, and the change of grid happens in between.
    steps, speed = residual_steps(path)
    big = sorted(steps, key=lambda s: -abs(s[1]))[:6]
    print(f"  the bar's own speed: {speed:.2f} px/s")
    print(f"  the biggest steps the bar cannot explain:")
    for t, e, dt in big:
        print(f"    t={t:6.2f}s  {e:+7.2f} px  ({e/n*100:+6.1f} % of the bar,"
              f" {e/n*4:+5.2f} beats)")

    for label, t_at in (("deck arrives", t_on), ("deck leaves", t_off)):
        if t_at is None:
            print(f"  {label}: the source never changed")
            continue
        j = jump_at(path, t_at)
        if not j:
            print(f"  {label}: not enough data")
            continue
        jump = j["jump"]
        print(f"  {label} at t={t_at:.1f}s (the poll's time; the jump itself "
              f"is in the list above)")
        print(f"      the light jumped {jump:+8.2f} px  (fitted)")
        print(f"      = {abs(jump) / n * 100:5.1f} % of the bar"
              f"   = {abs(jump) / n * 4:5.2f} beats")
        print(f"      the noise floor is {j['noise']:.2f} px, so the jump is "
              f"{abs(jump)/max(j['noise'],0.01):.1f} x the noise")
        print(f"      the bar's own speed: {j['b_before']:.2f} px/s before, "
              f"{j['b_after']:.2f} px/s after")

    errs = [r for r in tempo if r[5] is not None and r[1] == "PRO LINK"]
    if errs:
        print(f"\n  the board measured the two grids {errs[-1][5]:+.1f} ms apart "
              f"while the deck was up")
        print(f"  the rotation between the two numberings was "
              f"{errs[-1][6]} beat(s)")


if __name__ == "__main__":
    main()
