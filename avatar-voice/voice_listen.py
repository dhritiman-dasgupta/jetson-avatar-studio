import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/run/user/1000/avatar-voice.sock")
buf = b""
while True:
    d = s.recv(4096)
    if not d:
        break
    buf += d
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        t = line.decode().strip()
        if '"transcript"' in t or '"reply"' in t:
            print(t, flush=True)
