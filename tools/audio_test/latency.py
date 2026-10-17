# SPDX-License-Identifier: GPL-2.0-or-later
"""Real-time encode test over USB: speech paced like BlueDV (5 frames / 100 ms), per-frame reply latency."""
import sys, time, wave, struct, threading, serial
w = wave.open(sys.argv[2]); pcm = w.readframes(w.getnframes()); n = len(pcm)//320
n = min(n, int(sys.argv[3]) if len(sys.argv) > 3 else n)
s = serial.Serial(); s.port=sys.argv[1]; s.baudrate=460800; s.timeout=0.05; s.dtr=False; s.rts=False; s.open(); time.sleep(0.5); s.read(65536)
s.write(bytes.fromhex("6100010033")); time.sleep(0.3); s.write(bytes.fromhex("610003004b0505")); s.write(bytes.fromhex("61000d000a013007634000000000000048")); time.sleep(0.3); s.read(65536)
sent_t = []; recv_t = []; stop = False; buf = bytearray()
def rd():
    while not stop or len(recv_t) < len(sent_t):
        d = s.read(max(1, s.in_waiting))
        if d:
            buf.extend(d)
            while len(buf) >= 15:
                i = buf.find(b"\x61\x00\x0b\x01")
                if i < 0: del buf[:-3]; break
                if len(buf) < i+15: break
                recv_t.append(time.perf_counter()); del buf[:i+15]
        elif stop and time.perf_counter() - t_end > 2: break
th = threading.Thread(target=rd); th.start()
t0 = time.perf_counter()
for b in range(0, n, 5):
    target = t0 + (b//5)*0.1
    while time.perf_counter() < target: time.sleep(0.001)
    for k in range(b, min(b+5, n)):
        s.write(b"\x61\x01\x42\x02\x00\xa0" + struct.pack(">160h", *struct.unpack("<160h", pcm[k*320:(k+1)*320])))
        sent_t.append(time.perf_counter())
t_end = time.perf_counter(); stop = True; th.join()
print(f"sent {len(sent_t)} frames ({len(sent_t)*0.02:.1f} s), replies {len(recv_t)}")
for sec in range(0, len(sent_t)//50 + 1, 5):
    seg = [recv_t[i]-sent_t[i] for i in range(sec*50, min((sec+5)*50, len(recv_t)))]
    if seg: print(f"  {sec:3d}-{sec+5:3d} s: latency avg {1000*sum(seg)/len(seg):6.0f} ms  max {1000*max(seg):6.0f} ms")

