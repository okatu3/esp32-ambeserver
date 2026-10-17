# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic test signal with known truth: 'words' (harmonic voice with formants, pitch 100..250 Hz, some
starting with breath noise) separated by gaps, optional background (390 Hz whine + noise), then a 1 kHz tone.
Writes the WAV and a truth file: per frame 'g' gap, 'b' breath, or the pitch in Hz; 't' for the tone."""
import sys, math, random, struct, wave
random.seed(1)
FS = 8000
out, bg = sys.argv[1], sys.argv[2] == "bg"
x, truth = [], []

def res(sig, f, bw):
    r = math.exp(-math.pi * bw / FS); c = 2 * r * math.cos(2 * math.pi * f / FS); y1 = y2 = 0; o = []
    for v in sig:
        y = v + c * y1 - r * r * y2; y2, y1 = y1, y; o.append(y)
    return o

def add(seg, label):
    global x
    x += seg; truth.extend([label] * (len(seg) // 160))

def gap(ms):
    add([0.0] * (FS * ms // 1000), "g")

def word(f0, ms, breath):
    if breath:
        n = FS * 80 // 1000
        w = [random.gauss(0, 1) for _ in range(n)]
        w = res(res(w, 1500, 900), 3000, 1200)
        pk = max(abs(v) for v in w)
        add([0.08 * v / pk * (i / n) for i, v in enumerate(w)], "b")
    n = FS * ms // 1000; ph = 0; src = []
    for i in range(n):
        f = f0 * (1.05 - 0.1 * i / n)                       # gentle declination
        ph += 2 * math.pi * f / FS
        src.append(sum(math.sin(k * ph) / k for k in range(1, int(3800 / f))))
    v = res(res(src, 700, 130), 1200, 150)
    pk = max(abs(t) for t in v)
    env = [min(1, i / 160) * min(1, (n - i) / 400) for i in range(n)]
    seg = [0.3 * t / pk * e for t, e in zip(v, env)]
    for k in range(0, n - 159, 160):
        x.extend(seg[k:k + 160]); truth.append(f"{f0 * (1.05 - 0.1 * (k + 80) / n):.0f}")

gap(600)
for f0, ms, br in [(100, 500, 0), (130, 400, 1), (160, 600, 0), (200, 400, 1), (250, 500, 0),
                   (120, 300, 1), (150, 500, 0), (180, 300, 1), (220, 400, 0), (110, 600, 1),
                   (140, 250, 0), (170, 250, 1)]:
    word(f0, ms, br); gap(500)
tone = [0.1 * math.sin(2 * math.pi * 1000 * i / FS) for i in range(FS)]
add(tone, "t"); gap(500)

if bg:
    x = [v + 0.003 * math.sin(2 * math.pi * 390 * i / FS) + random.gauss(0, 0.0015) for i, v in enumerate(x)]
w = wave.open(out, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, int(v * 32767)))) for v in x)); w.close()
open(out + ".truth", "w").write("\n".join(truth[:len(x) // 160]))
print(out, len(x) // 160, "frames")
