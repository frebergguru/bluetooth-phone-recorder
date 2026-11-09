#!/usr/bin/env python3
"""Send one obs-websocket v5 request (no password) and print the reply.
Used only by make-obs-screenshots.sh.
Usage: obs-ws-request.py <port> <RequestType> [json-request-data]"""
import base64, json, os, socket, struct, sys, time

port, rtype = int(sys.argv[1]), sys.argv[2]
rdata = json.loads(sys.argv[3]) if len(sys.argv) > 3 else None
for _ in range(100):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        break
    except OSError:
        time.sleep(0.2)
else:
    sys.exit("obs-ws-request: OBS websocket not reachable")
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Protocol: obswebsocket.json\r\n\r\n" % key).encode())
buf = b""
while b"\r\n\r\n" not in buf:
    buf += s.recv(4096)
buf = buf.split(b"\r\n\r\n", 1)[1]

def recv_msg():
    global buf
    while True:
        if len(buf) >= 2:
            ln = buf[1] & 0x7F
            off = 2
            if ln == 126:
                if len(buf) < 4: pass
                else: ln = struct.unpack(">H", buf[2:4])[0]; off = 4
            elif ln == 127:
                if len(buf) >= 10: ln = struct.unpack(">Q", buf[2:10])[0]; off = 10
            if len(buf) >= off + ln and not (buf[1] & 0x7F in (126, 127) and off == 2):
                payload, buf = buf[off:off + ln], buf[off + ln:]
                return json.loads(payload)
        chunk = s.recv(65536)
        if not chunk:
            sys.exit("obs-ws-request: connection closed")
        buf += chunk

def send_msg(obj):
    data = json.dumps(obj).encode()
    mask = os.urandom(4)
    n = len(data)
    head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

recv_msg()  # Hello
send_msg({"op": 1, "d": {"rpcVersion": 1, "eventSubscriptions": 0}})
recv_msg()  # Identified
req = {"requestType": rtype, "requestId": "docs"}
if rdata is not None:
    req["requestData"] = rdata
send_msg({"op": 6, "d": req})
while True:
    m = recv_msg()
    if m.get("op") == 7:
        print(json.dumps(m["d"].get("requestStatus")))
        break
