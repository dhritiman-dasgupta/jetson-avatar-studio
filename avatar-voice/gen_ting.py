import wave, struct, math
sr, dur = 16000, 0.16
n = int(sr * dur)
out = []
for i in range(n):
    t = i / sr
    env = math.exp(-t * 20)                     # quick decay
    v = 0.55 * math.sin(2 * math.pi * 880 * t) * env \
      + 0.30 * math.sin(2 * math.pi * 1320 * t) * env
    out.append(int(max(-1.0, min(1.0, v)) * 30000))
w = wave.open("/home/jetson/avatar-voice/ting.wav", "wb")
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b"".join(struct.pack("<h", x) for x in out))
w.close()
print("ting.wav written:", n, "samples")
