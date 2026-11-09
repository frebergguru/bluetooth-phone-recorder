#!/usr/bin/env python3
"""Minimal headless Broadway viewer: connects to a local gtk4-broadwayd like
a browser would and discards what it receives, so GTK keeps drawing frames.
Used only by make-doc-screenshots.sh. Usage: broadway-viewer.py <port>"""
import base64, os, socket, struct, sys, time

port = int(sys.argv[1])
for _ in range(50):
    try:
        s = socket.create_connection(("127.0.0.1", port))
        break
    except OSError:
        time.sleep(0.1)
else:
    sys.exit("broadway-viewer: cannot connect")
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /socket HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Protocol: broadway\r\n\r\n" % key).encode())

def send_text(msg):
    data = msg.encode()
    mask = os.urandom(4)
    head = bytes([0x81])
    head += bytes([0x80 | len(data)]) if len(data) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(data))
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

buf = b""
while b"\r\n\r\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        sys.exit(0)
    buf += chunk
# tell the server our "screen" size (event 's': width, height, scale)
send_text("s%d,1600,1000,1" % 0)
s.settimeout(1.0)
while True:
    try:
        if not s.recv(65536):
            break
    except socket.timeout:
        continue
    except OSError:
        break
