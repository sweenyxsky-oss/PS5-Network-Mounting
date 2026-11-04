#!/usr/bin/env python3
"""HTTP/REST smoke test for the PS5 Network Mounting host build.

Builds nothing: run `make host` first (or `make test`, which does both). Starts the
payload on a loopback port, exercises the panel and every API route, and checks the
security behaviour of the request parser.
"""

from __future__ import annotations

import http.client
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "build" / "ps5nm-host"
PORT = int(os.environ.get("PS5NM_TEST_PORT", "18391"))
TIMEOUT = 15


class Client:
    def __init__(self, port: int) -> None:
        self.port = port

    def request(self, method: str, path: str, body: bytes | None = None,
                headers: dict[str, str] | None = None) -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=TIMEOUT)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, {k.lower(): v for k, v in response.getheaders()}, response.read()
        finally:
            connection.close()

    def json_request(self, method: str, path: str, payload: object | None = None) -> tuple[int, dict]:
        body = None if payload is None else json.dumps(payload).encode()
        headers = {"Content-Type": "application/json", "X-Requested-With": "PS5NM"}
        status, _, raw = self.request(method, path, body, headers)
        try:
            return status, json.loads(raw.decode() or "{}")
        except json.JSONDecodeError:
            return status, {"_raw": raw.decode(errors="replace")}


def wait_ready(client: Client, deadline: float) -> None:
    while time.time() < deadline:
        try:
            status, _, _ = client.request("GET", "/api/v1/status")
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.1)
    raise SystemExit("the payload did not start answering on its port")


def main() -> int:
    if not BINARY.exists():
        raise SystemExit(f"{BINARY} is missing: run `make host` first")
    log_dir = tempfile.mkdtemp(prefix="ps5nm-smoke-")
    environment = dict(os.environ)
    environment.update({
        "PS5NM_PORT": str(PORT),
        "PS5NM_BIND": "127.0.0.1",
        "PS5NM_NO_BROWSER": "1",
        "PS5NM_LOG_DIR": log_dir,
    })
    process = subprocess.Popen([str(BINARY)], env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    failures: list[str] = []

    def check(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    try:
        client = Client(PORT)
        wait_ready(client, time.time() + TIMEOUT)

        status, headers, body = client.request("GET", "/")
        check(status == 200, f"GET / returned {status}")
        check("text/html" in headers.get("content-type", ""), "GET / is served as HTML")
        check(b"PS5 Network Mounting" in body, "the panel carries the product name")
        check(b"/api/v1" in body, "the panel talks to the API")

        status, payload = client.json_request("GET", "/api/v1/status")
        check(status == 200, f"GET /api/v1/status returned {status}")
        check(payload.get("service") == "ps5-network-mounting", "the service name is reported")
        check(payload.get("mode") == "in-process", "the status reports the in-process share layer")
        check(payload.get("mounted") is False, "nothing is attached at startup")
        check(isinstance(payload.get("version"), str), "the version is reported")
        check(isinstance(payload.get("urls"), list), "the panel URL list is reported")

        status, payload = client.json_request("POST", "/api/v1/mount", {})
        check(status == 400, f"an empty mount request is a 400 (got {status})")
        check(payload.get("error"), "the rejection carries an error code")

        status, payload = client.json_request("POST", "/api/v1/mount", {
            "protocol": "nfs", "host": "192.168.1.50", "port": 2049,
            "remote_path": "games", "mountpoint": "/hostapp",
        })
        check(status == 400, f"a relative NFS export is a 400 (got {status})")

        status, payload = client.json_request("POST", "/api/v1/mount", {
            "protocol": "smb", "host": "192.168.1.50", "port": 445,
            "remote_path": "/games", "mountpoint": "/hostapp",
        })
        check(status == 400, f"an SMB path with a leading slash is a 400 (got {status})")

        status, payload = client.json_request("POST", "/api/v1/mount", {
            "protocol": "ftp", "host": "192.168.1.50", "port": 21,
            "remote_path": "/games", "mountpoint": "/hostapp",
        })
        check(status == 400, f"an unsupported protocol is a 400 (got {status})")

        status, payload = client.json_request("POST", "/api/v1/mount", {
            "protocol": "smb", "host": "192.168.1.50", "port": 445,
            "remote_path": "games", "mountpoint": "/hostapp",
        })
        check(status == 500, f"the host build reports that it has no share backends (got {status})")
        check("backend" in payload.get("message", "").lower(), "the host-build message names the backend")

        status, payload = client.json_request("POST", "/api/v1/unmount", {"mountpoint": "/hostapp"})
        check(status == 409, f"unmounting when nothing is attached is a 409 (got {status})")

        status, _, _ = client.request("POST", "/api/v1/mount", b"{}", {
            "Content-Type": "application/json", "Origin": "http://evil.example",
        })
        check(status == 403, f"a foreign Origin is rejected (got {status})")

        status, _, _ = client.request("POST", "/api/v1/mount", b"not json", {
            "Content-Type": "application/json",
        })
        check(status == 400, f"a malformed body is a 400 (got {status})")

        status, _, log_body = client.request("GET", "/api/v1/log")
        check(status == 200, f"GET /api/v1/log returned {status}")
        check(len(log_body) > 0, "the diagnostic log has content")

        status, _ = client.json_request("GET", "/api/v1/missing")
        check(status == 404, f"an unknown endpoint is a 404 (got {status})")

        status, _, _ = client.request("GET", "/api/v1/status")
        check(status == 200, "the payload stays up after the rejected requests")
    finally:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        shutil.rmtree(log_dir, ignore_errors=True)

    if failures:
        print("HTTP smoke test failures:")
        for failure in failures:
            print(f"  - {failure}")
        if process.stdout:
            print("payload output:")
            print(process.stdout.read().decode(errors="replace")[-4000:])
        return 1
    print("http smoke test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
