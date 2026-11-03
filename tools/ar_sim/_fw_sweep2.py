#!/usr/bin/env python3
"""Sweep the collapse rule: how long a collapse must last, and how much
better the far rate has to be before a dead local rate gives up the grid.
"""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache, _fw_band as B, _fw_port as P
from _fw_check import oracle_bpm

N = int(sys.argv[1]) if len(sys.argv) > 1 else 20
data = []
for k in _cache.keys()[:N]:
    x = _cache.load(k)
    if x is None: continue
    odf = B.firmware_odf(x, 90.0, 320.0)
    if len(odf) < 1200: continue
    data.append((k, odf, oracle_bpm(k)))
print("windows", len(data), flush=True)

def run(cc, dm):
    errs, wan, jmp = [], [], []
    for k, odf, lb in data:
        t = P.Tracker(); t.collapse_count_max = cc; t.dead_margin = dm
        t.run(odf)
        log = np.array(t.bpm_log); log = log[log > 0]
        if len(log) == 0: continue
        if lb > 0: errs.append(float(np.median(np.abs(log - lb))))
        half = log[len(log) // 2:]
        wan.append(float(np.std(half)))
        jmp.append(int(np.sum(np.abs(np.diff(half)) > 0.5)))
    e = np.array(errs)
    return np.median(e), e.mean(), np.mean(wan), np.mean(jmp)

for cc, dm in [(1,0.0),(4,0.0),(4,0.05),(4,0.2),(8,0.05),(8,0.2)]:
    m, mean, w, j = run(cc, dm)
    print("cc=%2d dm=%.2f  err med %.3f mean %.3f  wander %.3f  jumps %.2f"
          % (cc, dm, m, mean, w, j), flush=True)
