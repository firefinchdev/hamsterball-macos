#!/usr/bin/env python3
"""Serve the browser build of Hamster Ball to your home network.

  python3 serve.py [--port 8080] [--https]

Run it from a repository checkout (serves dist/web) or from the unzipped release folder
(serves the folder it is in).

Open the printed address on any device on the same Wi-Fi. Plain HTTP works for playing with
mouse, keyboard and touch. Browsers only allow gamepads on secure pages, so --https serves
with a self-signed certificate (created once); each device then shows a
warning the first time, which you accept for this address.
"""
import argparse
import functools
import http.server
import os
import socket
import ssl
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STANDALONE = os.path.exists(os.path.join(HERE, "index.html"))  # inside an unzipped release
ROOT = os.path.dirname(HERE)


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 9))  # no packet is sent; picks the outgoing interface
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def certificate(ip):
    d = os.path.join(HERE, ".cert") if STANDALONE else os.path.join(ROOT, "build", "web-cert")
    cert, key, made_for = os.path.join(d, "cert.pem"), os.path.join(d, "key.pem"), os.path.join(d, "ip.txt")
    if not os.path.exists(cert) or not os.path.exists(made_for) or open(made_for).read() != ip:
        os.makedirs(d, exist_ok=True)
        subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650",
                               "-keyout", key, "-out", cert, "-subj", "/CN=Hamster Ball",
                               "-addext", "subjectAltName=IP:%s,DNS:localhost" % ip],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        open(made_for, "w").write(ip)
    return cert, key


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript", ".data": "application/octet-stream"}

    def log_message(self, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--https", action="store_true", help="serve over HTTPS (needed for gamepads)")
    ap.add_argument("--dir", default=HERE if STANDALONE else os.path.join(ROOT, "dist", "web"))
    a = ap.parse_args()
    if not os.path.exists(os.path.join(a.dir, "index.html")):
        raise SystemExit("%s has no build: run web/build.sh first" % a.dir)
    ip = lan_ip()
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", a.port), functools.partial(Handler, directory=a.dir))
    scheme = "http"
    if a.https:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(*certificate(ip))
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        scheme = "https"
    sys.stdout.reconfigure(line_buffering=True)
    print("Hamster Ball is being served:")
    print("  this computer:          %s://localhost:%d/" % (scheme, a.port))
    print("  other devices on Wi-Fi: %s://%s:%d/" % (scheme, ip, a.port))
    print("Press Ctrl+C to stop.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
