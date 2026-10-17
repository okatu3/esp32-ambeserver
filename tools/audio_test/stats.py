# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the S3 link/input counters (private command F4). Arg 2 'clear' = read and clear. Works over COM port or UDP ip."""
import sys, time, struct, socket
names = ["bytes_in","skipped","bad_hdr","bad_len","pk_ctrl","pk_audio","pk_ambe","bad_audio","tx_fail","speech_frames","speech_db_sum_x10","peak","clipped","quiet_frames"]
cmd = bytes.fromhex("610002" + "00f401") if len(sys.argv) > 2 and sys.argv[2] == "clear" else bytes.fromhex("61000100f4")
dst = sys.argv[1]
if dst.upper().startswith("COM"):
    import serial
    s = serial.Serial(); s.port=dst; s.baudrate=460800; s.timeout=0.5; s.dtr=False; s.rts=False; s.open(); time.sleep(0.3); s.read(65536)
    s.write(cmd); time.sleep(0.4); r = s.read(4096); s.close()
else:
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.settimeout(1.0); u.sendto(cmd, (dst, 2460)); r = u.recv(4096)
i = r.find(b"\x61\x00\x39\x00\xf4")
if i < 0: print("no reply", r[:40].hex(" ")); sys.exit(1)
v = struct.unpack(">14I", r[i+5:i+5+56]); d = dict(zip(names, v))
d["speech_db_sum_x10"] = struct.unpack(">i", struct.pack(">I", d["speech_db_sum_x10"]))[0]
for k in names: print(f"  {k:18s} {d[k]}")
if d["speech_frames"]:
    print(f"  => speech RMS avg {d['speech_db_sum_x10']/10/d['speech_frames']:.1f} dBFS over {d['speech_frames']} frames ({d['speech_frames']*0.02:.1f} s), peak {20*__import__('math').log10(max(d['peak'],1)/32768):.1f} dBFS")
