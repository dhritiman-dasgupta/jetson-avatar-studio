import socket, time, os
p = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "avatar-voice.sock")
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(p)
print(">> sending wake")
s.sendall(b'{"type":"wake"}\n')
s.settimeout(1.0)
end = time.time() + 8
while time.time() < end:
    try:
        d = s.recv(4096)
        if not d:
            break
        for line in d.decode().splitlines():
            print("  ", line)
    except socket.timeout:
        pass
print(">> done")
