#!/usr/bin/env python3
import json
import os
import socket
import subprocess
import tempfile
import zlib
import time
import urllib.error
import urllib.request


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


PORT = find_free_port()
BASE = f"http://127.0.0.1:{PORT}"


def request(path, method="GET", body=None, headers=None):
    data = None
    h = {}
    if headers:
        h.update(headers)
    if body is not None:
        if isinstance(body, (dict, list)):
            data = json.dumps(body).encode()
            h.setdefault("Content-Type", "application/json")
        elif isinstance(body, str):
            data = body.encode()
            h.setdefault("Content-Type", "application/json")
        elif isinstance(body, bytes):
            data = body
    req = urllib.request.Request(
        BASE + path,
        data=data,
        method=method,
        headers=h,
    )
    try:
        with urllib.request.urlopen(req, timeout=3) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()

env = os.environ.copy()
env["PS5NM_PORT"] = str(PORT)
env["PS5NM_BIND"] = "127.0.0.1"
env["PS5NM_NO_BROWSER"] = "1"
homebrew = tempfile.TemporaryDirectory(prefix="ps5nm-homebrew-")
env["PS5NM_HOME_BREW_DIR"] = homebrew.name
env["PS5NM_LOG_DIR"] = homebrew.name
env["PS5NM_LAUNCHER_ELF"] = "/tmp/ps5nm-host"
proc = subprocess.Popen(["/tmp/ps5nm-host"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    for _ in range(60):
        if proc.poll() is not None:
            raise RuntimeError(f"host server exited early with {proc.returncode}")
        try:
            status_code, raw = request("/api/v1/status")
            if status_code == 200:
                break
        except (OSError, TimeoutError):
            time.sleep(0.1)
    else:
        raise RuntimeError("HTTP server did not start")

    status = json.loads(raw)
    assert status["mounted"] is False
    assert status["mountpoint"] == "/hostapp"
    assert status["service"] == "ps5-network-mounting"
    assert status["version"] == "1.1.4"
    assert status["port"] == PORT
    assert status["url"].endswith(f":{PORT}")
    assert isinstance(status["urls"], list)
    assert status["browser_launched"] is False

    # Persistent/session diagnostics are downloadable over the same API.
    code, raw_log = request("/api/v1/log")
    assert code == 200
    assert b"Initializing PS5 Network Mounting payload" in raw_log

    # HTML page delivery
    code, page = request("/?cache=1")
    page = page.decode()
    assert code == 200 and "PS5 Network Mounting" in page
    assert 'value="nfs"' in page and 'value="smb"' in page
    assert 'value="ftp"' not in page.lower() and 'value="sftp"' not in page.lower()
    assert 'id="browserBtn"' in page and 'id="shortcutBtn"' in page
    assert 'id="payloadVersion"' in page and 'id="consoleUrls"' in page and 'id="browserState"' in page

    # Verify that an all-interface listener can be reached through the host's LAN address.
    route_probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        route_probe.connect(("192.0.2.1", 9))
        lan_ip = route_probe.getsockname()[0]
    finally:
        route_probe.close()
    assert lan_ip and not lan_ip.startswith("127."), f"no usable LAN address found: {lan_ip}"
    lan_port = find_free_port()
    lan_listener = subprocess.Popen(
        ["/tmp/ps5nm-host"],
        env={**env, "PS5NM_PORT": str(lan_port), "PS5NM_BIND": "0.0.0.0", "PS5NM_NO_BROWSER": "1"},
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        for _ in range(60):
            if lan_listener.poll() is not None:
                raise RuntimeError("LAN listener exited early")
            try:
                with urllib.request.urlopen(f"http://{lan_ip}:{lan_port}/api/v1/status", timeout=1) as response:
                    assert response.status == 200
                break
            except (OSError, TimeoutError):
                time.sleep(0.1)
        else:
            raise RuntimeError("Could not reach the all-interface listener through a local interface")
    finally:
        lan_listener.terminate()
        lan_listener.wait(timeout=3)

    # CORS OPTIONS preflight
    code, _ = request("/api/v1/mount", "OPTIONS")
    assert code == 204

    # Mount validation
    code, raw = request("/api/v1/mount", "POST", {
        "protocol": "nfs", "host": "127.0.0.1", "port": 2049,
        "remote_path": "/mnt/dataPool/games", "mountpoint": "/unsafe",
    })
    assert code == 400 and json.loads(raw)["error"] == "unsafe_mountpoint"

    # Substring key trap in JSON parser
    code, raw = request("/api/v1/mount", "POST", {
        "note": "my favorite protocol is nfs",
        "protocol": "smb", "host": "127.0.0.1", "port": 445,
        "remote_path": "games", "mountpoint": "/hostapp",
    })
    assert code == 500
    assert "no PS5 FUSE/NFS/SMB adapters linked" in json.loads(raw)["message"]

    # Malformed JSON body
    code, raw = request("/api/v1/mount", "POST", '{"protocol": "nfs", "unclosed": ')
    assert code == 400 and json.loads(raw)["error"] == "invalid_json"

    code, raw_log = request("/api/v1/log")
    assert code == 200 and b"invalid_json" in raw_log and b"[ERROR]" in raw_log

    # Trailing garbage after valid JSON
    code, raw = request("/api/v1/mount", "POST", '{"protocol": "nfs"} trailing_garbage')
    assert code == 400 and json.loads(raw)["error"] == "invalid_json"

    # CSRF check: foreign Origin without X-Requested-With
    code, raw = request("/api/v1/unmount", "POST", {"mountpoint": "/hostapp"}, headers={
        "Origin": "http://evil-attacker.com",
        "Host": f"127.0.0.1:{PORT}"
    })
    assert code == 403 and json.loads(raw)["error"] == "forbidden_origin"

    # CSRF check: foreign Origin WITH X-Requested-With remains forbidden; the header is not a token.
    code, raw = request("/api/v1/unmount", "POST", {"mountpoint": "/hostapp"}, headers={
        "Origin": "http://evil-attacker.com",
        "Host": f"127.0.0.1:{PORT}",
        "X-Requested-With": "PS5NM"
    })
    assert code == 403 and json.loads(raw)["error"] == "forbidden_origin"

    # Standard unmount request
    code, raw = request("/api/v1/unmount", "POST", {"mountpoint": "/hostapp"})
    assert code == 409 and json.loads(raw)["error"] == "unmount_failed"

    # Browser launch endpoint and URL validation
    code, raw = request("/api/v1/browser", "POST", {})
    assert code == 200
    browser_resp = json.loads(raw)
    assert browser_resp["ok"] is True
    assert browser_resp["url"] == status["url"]
    code, raw = request("/api/v1/browser", "POST", {"url": "javascript:alert(1)"})
    assert code == 400 and json.loads(raw)["error"] == "invalid_url"
    code, raw = request("/api/v1/browser", "POST", '{"url":')
    assert code == 400 and json.loads(raw)["error"] == "invalid_json"

    # Shortcut installation writes valid metadata, icon data, and an executable launcher.
    code, raw = request("/api/v1/shortcut", "POST", {})
    assert code == 200
    shortcut_resp = json.loads(raw)
    assert shortcut_resp["ok"] is True
    shortcut_root = os.path.join(homebrew.name, "HBRW00001")
    assert shortcut_resp["path"] == shortcut_root
    with open(os.path.join(shortcut_root, "sce_sys", "param.json"), encoding="utf-8") as metadata_file:
        metadata = json.load(metadata_file)
    assert metadata["titleId"] == "HBRW00001"
    assert metadata["titleId"].startswith("HBRW") and len(metadata["titleId"]) == 9
    assert metadata["contentId"].startswith("UP9000-HBRW00001_")
    assert metadata["localizedParameters"]["en-US"]["titleName"] == "PS5 Network Mounting"
    launcher_path = os.path.join(shortcut_root, "eboot.bin")
    assert open(launcher_path, "rb").read() == open("/tmp/ps5nm-host", "rb").read()
    assert os.stat(launcher_path).st_mode & 0o111
    with open(os.path.join(shortcut_root, "sce_sys", "icon0.png"), "rb") as icon_file:
        icon = icon_file.read()
    assert icon.startswith(bytes([137, 80, 78, 71, 13, 10, 26, 10]))
    image_data = bytearray()
    offset = 8
    while offset < len(icon):
        length = int.from_bytes(icon[offset:offset + 4], "big")
        chunk_type = icon[offset + 4:offset + 8]
        chunk_data = icon[offset + 8:offset + 8 + length]
        chunk_crc = int.from_bytes(icon[offset + 8 + length:offset + 12 + length], "big")
        assert zlib.crc32(chunk_type + chunk_data) & 0xffffffff == chunk_crc
        if chunk_type == b"IHDR":
            assert int.from_bytes(chunk_data[:4], "big") == 512
            assert int.from_bytes(chunk_data[4:8], "big") == 512
        if chunk_type == b"IDAT":
            image_data.extend(chunk_data)
        offset += 12 + length
    assert zlib.decompress(image_data)

    # A second instance must identify this service via its status response, not just any open port.
    relaunch_env = env.copy()
    relaunch_env["PS5NM_NO_BROWSER"] = "1"
    relaunch = subprocess.run(["/tmp/ps5nm-host"], env=relaunch_env, capture_output=True, text=True, timeout=10)
    assert relaunch.returncode == 0, relaunch.stderr + relaunch.stdout
    assert "Verified active PS5 Network Mounting service" in relaunch.stdout
    assert "Already active" in relaunch.stdout

    # An unrelated listener occupying the port must not be mistaken for our server.
    occupied_port = find_free_port()
    occupied = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    occupied.bind(("127.0.0.1", occupied_port))
    occupied.listen(1)
    try:
        unrelated_env = {**relaunch_env, "PS5NM_PORT": str(occupied_port)}
        unrelated = subprocess.run(["/tmp/ps5nm-host"], env=unrelated_env, capture_output=True, text=True, timeout=10)
        assert unrelated.returncode == 3
        assert f"Could not bind port {occupied_port}" in unrelated.stdout
        assert "Already active" not in unrelated.stdout
    finally:
        occupied.close()

    # Notification toast endpoint
    code, raw = request("/api/v1/notify", "POST", {"message": "Test notification"})
    assert code == 200
    notify_resp = json.loads(raw)
    assert notify_resp["ok"] is True

    print("HTTP smoke tests passed (host build with JSON parser and security checks).")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)
    assert proc.returncode == 0, f"host server did not shut down cleanly: {proc.returncode}"
    homebrew.cleanup()
