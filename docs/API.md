# Web API v1 Specification

The control interface embedded in `web/index.html` communicates with the native PS5 payload over HTTP JSON endpoints.

## Endpoints

### GET `/api/v1/log`
Downloads up to the latest 256 KiB of diagnostic events as `text/plain` (`ps5nm.log`). On PS5, the log is persisted at `/data/PS5NetworkMounting/ps5nm.log` and rotated at 512 KiB, retaining one previous file. `X-PS5NM-Log-Persistent` is `available` or `session-only`. The log records service startup, API errors, mount/unmount outcomes, and FUSE attach failures. SMB passwords are never logged.


### 1. GET `/api/v1/status`
Returns real-time mount state, adapter availability, and diagnostic metrics.

**Response (200 OK):**
```json
{
  "mounted": false,
  "can_disconnect": false,
  "protocol": "",
  "endpoint": "",
  "mountpoint": "/hostapp",
  "last_result": "Ready",
  "last_error": ""
}
```

### 2. POST `/api/v1/mount`
Connects to the remote server and attaches the FUSE filesystem to `/hostapp`.

**Headers required:**
- `Content-Type: application/json`
- `X-Requested-With: PS5NM`

**NFS Payload:**
```json
{
  "protocol": "nfs",
  "host": "192.168.1.100",
  "port": 2049,
  "remote_path": "/mnt/dataPool/games",
  "mountpoint": "/hostapp",
  "username": "",
  "password": ""
}
```

**SMB Payload:**
```json
{
  "protocol": "smb",
  "host": "192.168.1.100",
  "port": 445,
  "remote_path": "games/PS5",
  "mountpoint": "/hostapp",
  "username": "user",
  "password": "secret_password"
}
```

**Responses:**
- `200 OK`: `{"status": "ok", "message": "Share successfully mounted at /hostapp"}`
- `400 Bad Request`: Validation failure (e.g. `invalid_json`, `invalid_port`, `unsafe_mountpoint`, `missing_host`).
- `403 Forbidden`: `{"error": "forbidden_origin", "message": "CSRF check failed: missing required header"}`
- `409 Conflict`: Mount already active or `/hostapp` contains existing files.
- `500 Internal Error`: Network timeout, FUSE initialization error, or authentication failure.

### 3. POST `/api/v1/unmount`
Safely unmounts `/hostapp` and tears down the active FUSE event loop.

**Headers required:**
- `Content-Type: application/json`
- `X-Requested-With: PS5NM`

**Payload:**
```json
{
  "mountpoint": "/hostapp"
}
```

**Responses:**
- `200 OK`: `{"status": "ok", "message": "Share successfully disconnected."}`
- `409 Conflict`: `{"error": "unmount_failed", "message": "Mountpoint /hostapp is not currently mounted."}`

### 4. OPTIONS `/api/v1/*`
CORS preflight support. Returns `204 No Content` with appropriate CORS headers (`Access-Control-Allow-Origin: *`, `Access-Control-Allow-Headers: Content-Type, X-Requested-With`).

---

## Security & Concurrency Notes

1. **Network Binding:** Defaults to `0.0.0.0:18080`, allowing both the PS5 local web browser and any LAN device to reach the panel. Overridable via `PS5NM_BIND` and `PS5NM_PORT` environment variables.
2. **CSRF Protection:** State-changing requests (`POST /mount`, `POST /unmount`) require the `X-Requested-With: PS5NM` header. Requests bearing cross-origin headers without this token are rejected with `403 Forbidden`.
3. **Execution Permissions:** Files served via NFS and SMB are automatically mapped with `0555` (`r-xr-xr-x`) mode, allowing PS5 game binaries and PRX modules to execute without kernel permission faults.
4. **Thread Pool:** Multi-threaded worker pool servicing up to 32 simultaneous HTTP connections with 5-second socket timeouts to prevent denial-of-service or pipeline stalls.
