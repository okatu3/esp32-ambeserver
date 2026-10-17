# SPDX-License-Identifier: GPL-2.0-or-later
"""Runs a test WAV through the board (encode, then decode by the board) and scores it against the truth file.
  score.py COMx test.wav [decoded_out.wav]
Scores: onset = frames after each word start until the decoded pitch is within 12 % (and stays for 2 frames);
glide = words whose first decoded frames start >25 % off; gap_open = AMBE voice frames sent in gaps;
level = decoded minus input level on voiced frames; tone = decoded 1 kHz level and peak frequency."""
import sys, time, wave, struct, math, serial
port, src = sys.argv[1], sys.argv[2]
w = wave.open(src); pcm = w.readframes(w.getnframes()); n = len(pcm) // 320
truth = open(src + ".truth").read().split("\n")
s = serial.Serial(); s.port = port; s.baudrate = 460800; s.timeout = 0.02; s.dtr = False; s.rts = False; s.open()
time.sleep(0.3); s.read(65536)
def xfer(pkt, hdr, size):
    s.write(pkt); buf = b""; t = time.time()
    while time.time() - t < 0.5:
        buf += s.read(max(1, s.in_waiting)); i = buf.find(hdr)
        if i >= 0 and len(buf) >= i + size: return buf[i:i + size]
    raise SystemExit("timeout")
xfer(bytes.fromhex("6100010033"), b"\x61\x00\x01\x00\x39", 5)
xfer(bytes.fromhex("61000d000a013007634000000000000048"), b"\x61\x00\x02\x00\x0a", 6)
SIL = bytes.fromhex("9e8d3288261a3f61e8")
ambe = [xfer(b"\x61\x01\x42\x02\x00\xa0" + struct.pack(">160h", *struct.unpack("<160h", pcm[k*320:(k+1)*320])),
             b"\x61\x00\x0b\x01\x01\x48", 15)[6:] for k in range(n)]
xfer(bytes.fromhex("6100010033"), b"\x61\x00\x01\x00\x39", 5)
dec = []
for a in ambe:
    dec += struct.unpack(">160h", xfer(b"\x61\x00\x0b\x01\x01\x48" + a, b"\x61\x01\x42\x02\x00\xa0", 326)[6:])
s.close()
x = struct.unpack(f"<{n*160}h", pcm[:n*320])
if len(sys.argv) > 3:
    o = wave.open(sys.argv[3], "wb"); o.setnchannels(1); o.setsampwidth(2); o.setframerate(8000)
    o.writeframes(struct.pack(f"<{len(dec)}h", *dec)); o.close()

dW = [0,0,3,2,1,1,0,0,1,1,0,0,3,2,1,1,3,2,1,1,0,0,3,2,0,0,3,2,1,1,0,0,1,1,0,0,3,2,1,1,3,2,1,1,0,0,3,2,0,0,3,2,1,1,0,0,1,1,0,0,3,2,1,1,3,3,2,1,0,0,3,3]
dX = [10,22,11,9,10,22,11,23,8,20,9,21,10,8,9,21,8,6,7,19,8,20,9,7,6,18,7,5,6,18,7,19,4,16,5,17,6,4,5,17,4,2,3,15,4,16,5,3,2,14,3,1,2,14,3,15,0,12,1,13,2,0,1,13,0,12,10,11,0,12,1,13]
def enc_f0(a):
    """pitch the encoder sent (b0 from the C0 word, LSB unknown -> +1)"""
    if a == SIL: return 0
    fr = [[0]*24 for _ in range(4)]
    for i in range(72): fr[dW[i]][dX[i]] = (a[i>>3] >> (i&7)) & 1
    b = sum(fr[0][23-i] << (6-i) for i in range(6)) + 1
    return 8000 * 2 ** (-4.311767578125 - 0.021336 * (b + 0.5))
def f0_dec(sig, k):
    seg = sig[max(0, k*160-80): k*160+240]
    e0 = sum(v*v for v in seg) + 1e-9; best, bl = 0, 0
    for lag in range(20, 128):
        r = sum(seg[i]*seg[i+lag] for i in range(len(seg)-lag)) / e0
        if r > best: best, bl = r, lag
    # prefer the shortest lag whose correlation is close to the best (avoid octave-down picks)
    for lag in range(20, 128):
        r = sum(seg[i]*seg[i+lag] for i in range(len(seg)-lag)) / e0
        if r > 0.85 * best: return 8000 / lag
    return 8000 / bl if bl else 0
db = lambda sig, k: 10 * math.log10(sum(v*v for v in sig[k*160:(k+1)*160]) / 160 / 32768**2 + 1e-12)

starts = [k for k in range(n) if truth[k][0].isdigit() and (k == 0 or not truth[k-1][0].isdigit())]
onsets, glides, rows = [], 0, []
for k0 in starts:
    lock = None; seq = []
    for j in range(k0, min(k0 + 10, n)):
        if not truth[j][0].isdigit(): break
        t = float(truth[j]); d = enc_f0(ambe[j]); seq.append(d)
        ok = abs(d - t) / t < 0.12
        if ok and lock is None and j + 1 < n and truth[j+1][0].isdigit() and abs(enc_f0(ambe[j+1]) - float(truth[j+1])) / float(truth[j+1]) < 0.12:
            lock = j - k0
    onsets.append(lock if lock is not None else 10)
    if seq and abs(seq[0] - float(truth[k0])) / float(truth[k0]) > 0.25: glides += 1
    rows.append(f"  {truth[k0]:>4s} Hz: " + " ".join(f"{v:4.0f}" for v in seq[:6]) + f"   lock {onsets[-1]}")
words = []
cur = []
for k in range(n):
    if truth[k][0].isdigit(): cur.append(k)
    elif cur: words.append(cur); cur = []
inner = [k for w_ in words for k in w_[2:-2]]
bad = [k for k in inner if abs(enc_f0(ambe[k]) - float(truth[k])) / float(truth[k]) > 0.12]
gap_open = sum(1 for k in range(n) if truth[k] == "g" and ambe[k] != SIL and all(truth[j] == "g" for j in range(max(0, k-10), k)))
voiced = [k for k in range(n) if truth[k][0].isdigit()]
lev = sum(db(dec, k) - db(x, k) for k in voiced) / len(voiced)
tk = [k for k in range(n) if truth[k] == "t"][5:-5]
tone_lev = sum(db(dec, k) - db(x, k) for k in tk) / max(len(tk), 1)
print("\n".join(rows))
print(f"SCORE onset_avg {sum(onsets)/len(onsets):.2f} frames  onset_max {max(onsets)}  glides {glides}/{len(starts)}"
      f"  mid_err {len(bad)}/{len(inner)}  gap_open {gap_open}  level {lev:+.1f} dB  tone {tone_lev:+.1f} dB")


