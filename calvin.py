#!/usr/bin/env python3
"""Calvin - Remote JS script sender for HOOK framework

Usage:
    python calvin.py script.js
    python calvin.py script.js --host 192.168.1.100
    python calvin.py -e 'send("hello")'
"""

import sys
import socket
import base64
import json
import argparse

HOST = "127.0.0.1"
PORT = 45896
RECV_SIZE = 4096


def send_script(host: str, port: int, source: str):
    payload = json.dumps({"type": "script", "data": base64.b64encode(source.encode()).decode()})

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    sock.connect((host, port))
    sock.sendall((payload + "\n").encode())

    data = b""
    while True:
        try:
            chunk = sock.recv(RECV_SIZE)
            if not chunk:
                break
            data += chunk
            if b"\n" in data:
                break
        except socket.timeout:
            break

    sock.close()

    if data:
        resp = data.decode().strip()
        try:
            obj = json.loads(resp)
            value = obj.get("value", "")
            print(value if value else "(no output)")
        except json.JSONDecodeError:
            print(resp)
    else:
        print("(no response)")


def main():
    parser = argparse.ArgumentParser(description="Calvin - Send JS to HOOK")
    parser.add_argument("script", help="JS file path or inline code with -e")
    parser.add_argument("-e", "--eval", action="store_true", help="Evaluate inline JS string")
    parser.add_argument("--host", default=HOST, help=f"Target host (default: {HOST})")
    parser.add_argument("--port", type=int, default=PORT, help=f"Target port (default: {PORT})")
    args = parser.parse_args()

    if args.eval:
        source = args.script
    else:
        with open(args.script, "r", encoding="utf-8") as f:
            source = f.read()

    send_script(args.host, args.port, source)


if __name__ == "__main__":
    main()
