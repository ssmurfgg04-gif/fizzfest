#!/usr/bin/env python3
"""
fizzbuzz-arena correctness checker.

Usage: python3 checker.py PORT [HOST]

Passes only if EVERY check passes. Prints a summary and exits 0 on pass,
1 on fail (with the first failures listed).

Checks:
  * 1000 random N in [1, 2147483647]: status 200, exact body bytes
    (fizz / buzz / fizzbuzz / decimal N, no trailing newline), correct
    Content-Length, all over ONE keep-alive connection.
  * handpicked edges: 1, 2, 3, 4, 5, 15, big multiples, 2147483647 and
    friends near the int32 ceiling.
  * junk paths must get a 4xx: /0, /-5, /abc, /3.5, /0x10, /3x, /foo/bar,
    /2147483648 (int32 overflow), /99999999999999
  * non-GET methods must get a 4xx: POST, PUT, DELETE on a valid path.
"""

import random
import socket
import sys

HOST = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
TIMEOUT = 5.0

failures = []
checks = 0


def expected(n: int) -> bytes:
    if n % 15 == 0:
        return b"fizzbuzz"
    if n % 3 == 0:
        return b"fizz"
    if n % 5 == 0:
        return b"buzz"
    return str(n).encode()


def recv_response(sock, buf: bytearray):
    """Read one full response. Returns (status, headers, body) or raises."""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("eof")
        buf.extend(chunk)
    head, _, rest = bytes(buf).partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1])
    clen = None
    for h in lines[1:]:
        k, _, v = h.partition(b":")
        if k.strip().lower() == b"content-length":
            clen = int(v.strip())
    if clen is None:
        raise ValueError("no Content-Length")
    while len(rest) < clen:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("eof in body")
        rest += chunk
    body = rest[:clen]
    leftover = rest[clen:]
    buf.clear()
    buf.extend(leftover)
    return status, lines[0], body


def check_valid(sock, buf, n):
    global checks
    checks += 1
    try:
        sock.sendall(f"GET /{n} HTTP/1.1\r\nHost: fizz.local\r\n\r\n".encode())
        status, statusline, body = recv_response(sock, buf)
    except Exception as e:
        failures.append(f"/{n}: transport error: {e}")
        return
    want = expected(n)
    if status != 200:
        failures.append(f"/{n}: status {status}, want 200")
    elif body != want:
        failures.append(f"/{n}: body {body!r}, want {want!r}")


def check_bad_path(path):
    global checks
    checks += 1
    try:
        s = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
        s.settimeout(TIMEOUT)
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: fizz.local\r\n\r\n".encode())
        buf = bytearray()
        status, _, body = recv_response(s, buf)
        s.close()
    except Exception as e:
        failures.append(f"junk {path}: transport error: {e}")
        return
    if status < 400 or status > 499:
        failures.append(f"junk {path}: status {status}, want 4xx")


def check_bad_method(method):
    global checks
    checks += 1
    try:
        s = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
        s.settimeout(TIMEOUT)
        s.sendall(f"{method} /3 HTTP/1.1\r\nHost: fizz.local\r\n\r\n".encode())
        buf = bytearray()
        status, _, body = recv_response(s, buf)
        s.close()
    except Exception as e:
        failures.append(f"{method} /3: transport error: {e}")
        return
    if status < 400 or status > 499:
        failures.append(f"{method} /3: status {status}, want 4xx")


def main():
    # one shared keep-alive connection for all valid requests
    sock = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
    sock.settimeout(TIMEOUT)
    buf = bytearray()

    edges = [
        1, 2, 3, 4, 5, 6, 9, 10, 14, 15, 30, 90,
        999999999, 999999990, 999999995,
        2147483647, 2147483646, 2147483645, 2147483644, 2147483643,
        2147483642, 2147483641, 2147483640, 300000003, 500000005,
        1500000000, 1500000015, 65535, 65536, 1048576, 999999989,
    ]
    for n in edges:
        check_valid(sock, buf, n)
        if failures:
            break

    rng = random.Random(20261016)
    for _ in range(1000):
        n = rng.randint(1, 2147483647)
        check_valid(sock, buf, n)
        if failures and len(failures) > 5:
            break

    sock.close()

    if not failures:
        for p in ["/0", "/-5", "/abc", "/3.5", "/0x10", "/3x",
                  "/foo/bar", "/2147483648", "/99999999999999", "/"]:
            check_bad_path(p)
        for m in ["POST", "PUT", "DELETE", "HEAD"]:
            check_bad_method(m)

    print(f"checker: {checks} checks, {len(failures)} failures")
    for f in failures[:10]:
        print(f"  FAIL {f}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
