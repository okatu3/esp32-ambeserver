# SPDX-License-Identifier: GPL-2.0-or-later
"""Pitch (autocorrelation) of the first frames after every onset, input vs decoded:  onset_pitch.py in.wav decoded.wav"""
import sys, wave, struct, math
def load(p):
    w = wave.open(p); return struct.unpack(f"<{w.getnframes()}h", w.readframes(w.getnframes()))
x, y = load(sys.argv[1]), load(sys.argv[2])
def f0(s, k):
    seg = s[max(0, k*160-80): k*160+240]
    if len(seg) < 300: return 0, 0
    e0 = sum(v*v for v in seg) + 1e-9; best, bl = 0, 0
    for lag in range(20, 120):          # 400..67 Hz
        r = sum(seg[i]*seg[i+lag] for i in range(len(seg)-lag)) / e0
        if r > best: best, bl = r, lag
    return (8000/bl if bl else 0), best
n = min(len(x), len(y)) // 160
db = lambda s, k: 10*math.log10(sum(v*v for v in s[k*160:(k+1)*160])/160/32768**2 + 1e-12)
ons = [k for k in range(1, n) if db(y, k-1) < -55 and db(y, k) > -50]
for k in ons[:14]:
    row = []
    for j in range(k, min(k+5, n)):
        a, ra = f0(x, j); b, rb = f0(y, j)
        row.append(f"{a:4.0f}({ra:.1f})/{b:4.0f}({rb:.1f})")
    print(f"{k*0.02:6.2f}s in/out Hz(periodicity): " + "  ".join(row))
