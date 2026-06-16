import sys, wave, array
w = wave.open(sys.argv[1], "rb")
a = array.array("h")
a.frombytes(w.readframes(w.getnframes()))
f = array.array("f", [x / 32768.0 for x in a])
open(sys.argv[2], "wb").write(f.tobytes())
print("wrote %d samples" % len(f))
