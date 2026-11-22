"""Prove the F1's input-report layout from raw usbmon captures.

The captures are usbmon "STREAM" dumps: a timestamp line, then the report's
bytes as hex, 16 on the first line and the rest on the second. Byte 0 of
the report is the report ID (0x01).

For every capture this prints which byte INDICES changed (0-based, report ID
= index 0), and decodes the eight 12-bit controls under two competing
layouts, so the layout can be settled by the data and not by a doc:

  A) bytes 6..21 as eight 16-bit-aligned 12-bit values (LSB first)
  B) the same, but the wheel candidate byte at index 6 instead of 5

    python tools/f1_layout_proof.py <capture dir>
"""
import sys, os, glob


def parse(path):
    """usbmon dump -> list of (timestamp, bytes)."""
    reports = []
    ts = None
    pending = b""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if "STREAM" in line:
                ts = float(line.split()[-1])
                pending = b""
                continue
            hexpart = line.replace(" ", "")
            try:
                pending += bytes.fromhex(hexpart)
            except ValueError:
                continue
            if len(pending) >= 22:
                reports.append((ts, pending[:22]))
                pending = pending[22:]
    return reports


def twelve(b, off):
    """A 12-bit little-endian value at byte offset off."""
    return b[off] | ((b[off + 1] & 0x0F) << 8)


def changed_indices(reports):
    """The set of byte indices that ever differ from the first report."""
    base = reports[0][1]
    out = set()
    for _, r in reports:
        for i in range(min(len(base), len(r))):
            if base[i] != r[i]:
                out.add(i)
    return sorted(out)


def summary(reports, label):
    base = reports[0][1]
    ch = changed_indices(reports)
    print(f"\n=== {label}: {len(reports)} reports, {len(base)} bytes")
    print(f"    bytes that ever changed: {ch}")
    # the wheel candidate: which index moves by exactly one step per report
    for cand in (4, 5, 6):
        if cand not in ch:
            continue
        vals = [r[cand] for _, r in reports]
        deltas = {((b - a + 128) % 256) - 128 for a, b in zip(vals, vals[1:])}
        print(f"    byte[{cand}]: first={vals[0]} last={vals[-1]} "
              f"distinct={len(set(vals))} step-set={sorted(deltas)}")
    # the eight 12-bit controls
    vals = [[twelve(r, o) for o in (6, 8, 10, 12, 14, 16, 18, 20)]
            for _, r in reports]
    for k in range(8):
        series = [v[k] for v in vals]
        moved = len(set(series)) > 1
        mono = all(b >= a for a, b in zip(series, series[1:])) or \
            all(b <= a for a, b in zip(series, series[1:]))
        tag = "MOVES" if moved else "     "
        print(f"    ctrl{k} @byte{6 + k * 2}: {tag} "
              f"min={min(series)} max={max(series)} "
              f"first={series[0]} last={series[-1]} "
              f"monotonic={mono} n_distinct={len(set(series))}")


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    for path in sorted(glob.glob(os.path.join(d, "*.txt"))):
        reports = parse(path)
        if not reports:
            print(f"\n=== {os.path.basename(path)}: no reports parsed")
            continue
        summary(reports, os.path.basename(path))


if __name__ == "__main__":
    main()
