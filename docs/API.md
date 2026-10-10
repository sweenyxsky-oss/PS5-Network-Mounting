# Web API v1

The first UI in `web/index.html` is served by the PS5 payload itself. The browser uses same-origin requests; do not hard-code a remote API hostname.

## Endpoints

### GET /api/v1/status

Returns the state verified by the native backend.

```json
{
  "state": "ready",
  "mounted": false,
  "protocol": null,
  "endpoint": null,
  "mountpoint": "/hostapp",
  "last_result": "No operation yet",
  "last_error": null
}
```

The mounted value must reflect actual mount state, not just a successful TCP connection.

### POST /api/v1/mount

Request:

```json
{
  "protocol": "nfs",
  "host": "192.168.1.10",
  "port": 2049,
  "remote_path": "/mnt/tank/ps5",
  "mountpoint": "/hostapp",
  "username": "",
  "password": ""
}
```

The initial supported protocol target is NFS. SMB, FTP and SFTP must return an explicit unsupported response until their filesystem adapters are implemented. A remote directory browser alone is not a mount adapter.

Success response (only after verifying the mount):

```json
{
  "ok": true,
  "mounted": true,
  "protocol": "nfs",
  "endpoint": "nfs://192.168.1.10:2049/mnt/tank/ps5",
  "mountpoint": "/hostapp",
  "message": "NFS share mounted and verified."
}
```

Failure response:

```json
{
  "ok": false,
  "error": "mount_failed",
  "message": "Human-readable reason"
}
```

### POST /api/v1/unmount

Request:

```json
{"mountpoint": "/hostapp"}
```

The backend must only unmount a mount created/owned by this payload. It must not blindly unmount arbitrary system mounts.

## Security and reliability requirements

- Validate protocol, host, port, path and mountpoint on the native side; never trust client-side validation.
- Do not log passwords or return them in status responses.
- Bound request body sizes and reject malformed JSON.
- Prevent command injection: do not build shell commands from user-controlled strings.
- Return actionable errors for unsupported protocol, DNS/network timeout, authentication failure, export/share denial, mountpoint conflict, and unmount failure.
- Keep `/hostapp` unchanged until the mount implementation is confirmed safe for the target firmware.
