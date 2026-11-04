# HTTP API

The payload listens on `0.0.0.0:18080` by default (`PS5NM_PORT`, `PS5NM_BIND`). Responses are
JSON unless noted. `POST` bodies must be JSON objects of at most 64 KiB.

Every response sets `Cache-Control: no-store`. A `POST` whose `Origin` header is present and
does not match the `Host` header is refused with `403`.

## `GET /`

The control panel (`text/html`).

## `GET /api/v1/status`

```json
{
  "state": "ready",
  "service": "ps5-network-mounting",
  "version": "2.0.0",
  "mode": "in-process",
  "port": 18080,
  "url": "http://192.168.1.20:18080",
  "urls": ["http://192.168.1.20:18080"],
  "browser_launched": true,
  "mounted": true,
  "can_disconnect": true,
  "share_state": "NFS v3, export /volume1/PS2",
  "protocol": "nfs",
  "endpoint": "nfs://192.168.1.50:2049/volume1/PS2",
  "mountpoint": "/hostapp",
  "last_result": "NFS share attached read-only at /hostapp.",
  "open_files": 2,
  "bytes_served": 1048576,
  "cache_hits": 18,
  "last_error": null
}
```

`protocol`, `endpoint`, `share_state` and `last_error` are `null` when they do not apply.
`mode` is always `in-process`: the payload serves the share itself.

## `POST /api/v1/mount`

```json
{
  "protocol": "smb",
  "host": "192.168.1.50",
  "port": 445,
  "remote_path": "games/PS5",
  "mountpoint": "/hostapp",
  "username": "guest",
  "password": ""
}
```

| Field | Notes |
|---|---|
| `protocol` | `nfs` or `smb` |
| `host` | hostname or IPv4 address |
| `port` | `1`–`65535`; defaults to `2049` (NFS) or `445` (SMB) |
| `remote_path` | NFS: absolute export path. SMB: `ShareName` or `ShareName/subfolder` |
| `mountpoint` | must be `/hostapp` |
| `username`, `password` | SMB only; blank username means guest |

Success returns `200` with the status object. Failures:

| Status | `error` | Cause |
|---|---|---|
| `400` | `invalid_json`, `invalid_request`, `invalid_host`, `invalid_remote_path`, `invalid_port`, `unsafe_mountpoint`, `unsupported_protocol` | request rejected before connecting |
| `403` | `forbidden_origin` | cross-origin `POST` |
| `500` | `mount_failed` | the server refused, authenticated wrongly, or the root is unreadable; `message` carries the detail |

A second mount while a share is attached returns `500` with an explanation; disconnect first.

## `POST /api/v1/unmount`

```json
{ "mountpoint": "/hostapp" }
```

`200` with the status object, or `409` (`unmount_failed`) when nothing is attached. Handles that
are still open stay readable until they are closed.

## `GET /api/v1/log`

`text/plain` download of the rotating log, with
`X-PS5NM-Log-Persistent: available|session-only`.

## `POST /api/v1/browser`

`{ "url": "http://192.168.1.20:18080" }` (optional; the panel URL is the default). Returns
`200 {"ok":true,"url":"..."}`, `400` for a non-HTTP URL, or `502` when WebKit refuses to launch.

## `POST /api/v1/shortcut`

Creates `/data/homebrew` if needed and asks ShadowMountPlus to rescan. Returns
`{"ok":true,"path":"...","shadow_mountplus_detected":true,"message":"..."}`.

## `POST /api/v1/notify`

`{ "message": "..." }` sends a PS5 toast. Always `200` with `{"ok":true|false,"message":"..."}`.
