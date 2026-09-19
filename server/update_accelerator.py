#!/usr/bin/env python3
"""Read-only GitHub Release accelerator for the Sempervirens update client."""
from __future__ import annotations
import argparse, os, re, tempfile, threading, time, urllib.parse, urllib.request
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPOSITORY = "XintingleiTeam/sempervirens"
PREFIX = "/api/sempervirens/update/"
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$")
ASSET = re.compile(r"^(?:stable\.json(?:\.sig)?|SempervirensApp\.exe|[0-9.]+-to-[0-9.]+\.svdelta)$")
FINAL_HOSTS = {"github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"}
locks: dict[str, threading.Lock] = {}
locks_guard = threading.Lock()

def route(path: str) -> tuple[str, str, int] | None:
    relative = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
    if not relative.startswith(PREFIX): return None
    relative = relative[len(PREFIX):]
    if relative in {"stable.json", "stable.json.sig"}:
        return (f"https://github.com/{REPOSITORY}/releases/latest/download/{relative}", relative, 60)
    match = re.fullmatch(r"releases/([^/]+)/([^/]+)", relative)
    if not match or not VERSION.fullmatch(match.group(1)) or not ASSET.fullmatch(match.group(2)): return None
    version, asset = match.groups()
    return (f"https://github.com/{REPOSITORY}/releases/download/v{version}/{asset}", f"releases/{version}/{asset}", 31536000)

def fetch(url: str, target: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "Sempervirens-China-Accelerator/1"})
    with urllib.request.urlopen(request, timeout=45) as response:
        final = urllib.parse.urlsplit(response.geturl())
        if final.scheme != "https" or (final.hostname or "").lower() not in FINAL_HOSTS:
            raise RuntimeError("GitHub redirected outside release asset hosts")
        target.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(prefix=".download-", dir=target.parent)
        try:
            with os.fdopen(descriptor, "wb") as output:
                while chunk := response.read(1024 * 1024): output.write(chunk)
                output.flush(); os.fsync(output.fileno())
            os.replace(temporary, target)
        finally:
            if os.path.exists(temporary): os.unlink(temporary)

class Handler(BaseHTTPRequestHandler):
    server_version = "SempervirensAccelerator/1"
    def do_GET(self) -> None:
        item = route(self.path)
        if not item: self.send_error(HTTPStatus.NOT_FOUND); return
        upstream, relative, max_age = item
        target = self.server.cache_root / relative  # type: ignore[attr-defined]
        with locks_guard: lock = locks.setdefault(relative, threading.Lock())
        try:
            with lock:
                stale = not target.is_file() or (max_age == 60 and time.time() - target.stat().st_mtime > 60)
                if stale: fetch(upstream, target)
            size = target.stat().st_size
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "application/json" if relative.endswith(".json") else "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Cache-Control", f"public, max-age={max_age}, immutable" if max_age > 60 else "public, max-age=60")
            self.send_header("X-Content-Type-Options", "nosniff"); self.end_headers()
            with target.open("rb") as source:
                while chunk := source.read(1024 * 1024): self.wfile.write(chunk)
        except Exception: self.send_error(HTTPStatus.BAD_GATEWAY)
    def log_message(self, fmt: str, *args: object) -> None:
        print(f"{self.address_string()} [{self.log_date_time_string()}] {fmt % args}", flush=True)

def main() -> None:
    parser = argparse.ArgumentParser(); parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18181); parser.add_argument("--cache", type=Path, default=Path("/var/cache/sempervirens-update"))
    args = parser.parse_args(); args.cache.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((args.listen, args.port), Handler); server.cache_root = args.cache; server.serve_forever()
if __name__ == "__main__": main()
