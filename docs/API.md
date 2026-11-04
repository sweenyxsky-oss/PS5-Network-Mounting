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
  "version": "2.1.0",
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
  "reads": 312,
  "requests": 340,
  "bytes_served": 1048576,
  "cache_hits": 18,
  "last_error": null
}
```

`protocol`, `endpoint`, `share_state` and `last_error` are `null` when they do not apply.
`endpoint` omits the port when the protocol default is in use. `mode` is always `in-process`:
the payload serves the share itself.

## `GET /api/v1/probe`

Answers "does this kernel have FUSE?" The payload itself never mounts with FUSE, so this is a
diagnostic: it tells you whether FUSE-based software can work on this console at all.

```json
{
  "service": "ps5-network-mounting",
  "version": "2.1.0",
  "mode": "in-process",
  "used_by_payload": false,
  "fuse": {
    "kernel_type": "missing",
    "kernel_checked": true,
    "check_method": "nmount(fusefs) probe",
    "type_errno": 19,
    "dev_fuse": { "present": false, "opens": false, "errno": 2 },
    "uid": 0,
    "euid": 0,
    "jailed": 0,
    "hostapp": { "present": true, "fstype": "nullfs" },
    "sandbox_base_present": true,
    "hint": "The kernel does not have the fusefs filesystem type, so no FUSE mount can work here.",
    "report": "uid=0 euid=0 jailed=0\nfusefs kernel type: NOT registered (nmount(fusefs) probe)\n..."
  }
}
```

| Field | Meaning |
|---|---|
| `kernel_type` | `registered`, `missing`, or `unknown` when the platform cannot answer |
| `kernel_checked` | true only when the filesystem type itself was tested |
| `check_method` | how it was tested: `nmount(fusefs) probe` on PS5, `/proc/filesystems` on the host |
| `type_errno` | errno of an inconclusive check; the probe never guesses between `EINVAL` cases |
| `dev_fuse` | whether `/dev/fuse` exists and can be opened read/write |
| `jailed`, `uid`, `euid` | sandbox and privilege context of the probe |
| `hint` | one sentence naming the most likely blocker |

On PS5 the type is checked with a deliberately incomplete `nmount` of `fusefs`, which cannot
succeed and is unmounted if it ever does. The same probe runs once at startup at less depth and is
written to the diagnostic log.

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
| `port` | optional; `1`–`65535`. Omitted or `0` uses the protocol default: SMB `445`, NFS the portmapper for v3 (v4 uses `2049`) |
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
