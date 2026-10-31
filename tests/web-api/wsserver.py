#!/usr/bin/env python3
# A server for tests/web-api/websocket.html: serves the tests directory over HTTP and speaks WebSocket (RFC 6455) on the same port.
#   python3 tests/web-api/wsserver.py [port]   (then open http://127.0.0.1:<port>/websocket.html)
import base64, hashlib, os, socket, struct, sys, threading, zlib

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ROOT = os.path.dirname(os.path.abspath(__file__))

def read_exact(c, n):
    data = b""
    while len(data) < n:
        chunk = c.recv(n - len(data))
        if not chunk: raise EOFError
        data += chunk
    return data

def frame(opcode, payload, fin=True):
    head = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    if n < 126: head += bytes([n])
    elif n < 65536: head += bytes([126]) + struct.pack(">H", n)
    else: head += bytes([127]) + struct.pack(">Q", n)
    return head + payload

def read_frame(c):
    b0, b1 = read_exact(c, 2)
    n = b1 & 0x7F
    if n == 126: n = struct.unpack(">H", read_exact(c, 2))[0]
    elif n == 127: n = struct.unpack(">Q", read_exact(c, 8))[0]
    mask = read_exact(c, 4) if b1 & 0x80 else None
    data = read_exact(c, n)
    if mask: data = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
    read_frame.rsv1 = bool(b0 & 0x40)
    return bool(b0 & 0x80), b0 & 15, data

def websocket(c, headers, path):
    protocols = [p.strip() for p in headers.get("sec-websocket-protocol", "").split(",") if p.strip()]
    accept = base64.b64encode(hashlib.sha1(headers["sec-websocket-key"].encode() + GUID).digest()).decode()
    reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n"
    if protocols: reply += "Sec-WebSocket-Protocol: " + protocols[-1] + "\r\n"
    if path == "/deflate" and "permessage-deflate" in headers.get("sec-websocket-extensions", ""):
        reply += "Sec-WebSocket-Extensions: permessage-deflate\r\n"
    c.sendall((reply + "\r\n").encode())
    if path == "/deflate":
        # permessage-deflate: the extension is taken, every message is deflated (context kept), and what the page sent is inflated and echoed back
        c.sendall(frame(1, b"deflate-ready"))
        inflater = zlib.decompressobj(-15)
        deflater = zlib.compressobj(wbits=-15)
        while True:
            fin, op, data = read_frame(c)
            if op == 8:
                c.sendall(frame(8, data[:2] if data else b"")); return
            if op in (9, 10): continue
            compressed = read_frame.rsv1
            text = inflater.decompress(data + b"\xff\xff"[:0] + (b"\x00\x00\xff\xff" if compressed else b"")) if compressed else data
            packed = deflater.compress(b"echo:" + text) + deflater.flush(zlib.Z_SYNC_FLUSH)
            head = bytes([0xC0 | op])
            n = len(packed[:-4])
            body = packed[:-4]
            head += bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n)
            c.sendall(head + body)
    if path == "/refuse-close":
        c.sendall(frame(8, struct.pack(">H", 4001) + b"bye")); return
    if path == "/drop":
        c.sendall(frame(1, b"before")); return
    c.sendall(frame(1, ("origin=" + headers.get("origin", "") + " protocols=" + headers.get("sec-websocket-protocol", "")).encode()))
    message, kind = b"", 1
    while True:
        fin, op, data = read_frame(c)
        if op == 8:
            c.sendall(frame(8, data[:2] if data else b"")); return
        if op == 9: c.sendall(frame(10, data)); continue
        if op == 10: continue
        if op in (1, 2): message, kind = data, op
        elif op == 0: message += data
        if not fin: continue
        if kind == 1 and message == b"fragment":
            c.sendall(frame(1, b"frag", False) + frame(0, b"ment", False) + frame(9, b"p") + frame(0, b"ed", True))
        elif kind == 1 and message == b"big":
            c.sendall(frame(2, bytes(range(256)) * 400))
        elif kind == 1 and message == b"ping":
            c.sendall(frame(9, b"x")); c.sendall(frame(1, b"pinged"))
        elif kind == 1 and message == b"close-me":
            c.sendall(frame(8, struct.pack(">H", 4000) + b"server done"))
        elif kind == 1 and message == b"badutf8":
            c.sendall(frame(1, b"\xff\xfe"))
        else:
            c.sendall(frame(kind, message))

def sse(c, path, headers):
    import time
    if path.startswith("/sse-204"):
        c.sendall(b"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"); return
    if path.startswith("/sse-html"):
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi"); return
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n")
    last = headers.get("last-event-id", "")
    if last == "":
        c.sendall(b"\xef\xbb\xbf: comment\r\ndata: one\r\n\r\n")
        time.sleep(0.1)
        c.sendall(b"id: 7\nevent: custom\ndata: a\ndata: b\n\n")
        time.sleep(0.1)
        c.sendall(b"retry: 100\ndata:x\n\r\n")
        time.sleep(0.05)
        c.sendall(b"data: split")
        time.sleep(0.05)
        c.sendall(b"ed\r")
        time.sleep(0.05)
        c.sendall(b"\n\r\n")
        time.sleep(0.1)
    else:
        c.sendall(("data: again " + last + "\n\n").encode())
        time.sleep(30)

def serve(c):
    try:
        buf = b""
        while b"\r\n\r\n" not in buf: buf += c.recv(4096)
        head = buf.split(b"\r\n\r\n")[0].decode()
        lines = head.split("\r\n")
        method, path, _ = lines[0].split(" ")
        headers = {l.split(":", 1)[0].strip().lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
        if headers.get("upgrade", "").lower() == "websocket":
            if path == "/reject":
                c.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"); return
            websocket(c, headers, path.split("?")[0]); return
        if path.startswith("/sse"):
            sse(c, path, headers); return
        name = os.path.join(ROOT, path.lstrip("/").split("?")[0])
        if os.path.isfile(name):
            body = open(name, "rb").read()
            kind = b"text/javascript" if name.endswith(".js") else b"text/html; charset=utf-8"
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: " + kind + b"\r\nContent-Length: " + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
        else:
            c.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
    except Exception:
        pass
    finally:
        try: c.close()
        except Exception: pass

s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8765)); s.listen(16)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
