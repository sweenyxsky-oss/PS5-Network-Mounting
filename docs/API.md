# Web API v1

The UI in `web/index.html` is embedded into the native payload. It calls same-origin endpoints.

## Endpoints

### GET /api/v1/status

Returns actual backend mount state, including `mounted`, `can_disconnect`, `protocol`, `endpoint`, `mountpoint`, `last_result`, and `last_error`. `can_disconnect` remains true for a stale payload-owned mount whose FUSE loop stopped, so the UI can still clean it up. A successful TCP connection alone must never be reported as a mount.

### POST /api/v1/mount

NFS request:

```json
{
  "protocol": "nfs",
  "host": "192.168.1.10",
  "port": 2049,
  "remote_path": "/mnt/dataPool/games",
  "mountpoint": "/hostapp",
  "username": "",
  "password": ""
}
```

SMB request:

```json
{
  "protocol": "smb",
  "host": "192.168.1.10",
  "port": 445,
  "remote_path": "games/PS5",
  "mountpoint": "/hostapp",
  "username": "nas-user",
  "password": "secret"
}
```

For NFS, `remote_path` must be the absolute export path. For SMB, it must be the share name, optionally followed by a subdirectory. Credentials are used only by the SMB adapter and are never returned by the API or logged by the payload.

Both adapters are intended to be read-only. The backend validates the protocol, host, port, path, and mountpoint; rejects paths with traversal markers; and refuses to mount over non-empty `/hostapp`. Only one payload-owned share can be active at a time.

### POST /api/v1/unmount

```json
{"mountpoint": "/hostapp"}
```

Only the mount owned by this payload is eligible for disconnect. The backend must not blindly unmount unrelated system mounts.

## Security and reliability requirements

- No shell commands are built from user input.
- Request bodies are bounded; malformed fields are rejected.
- Passwords are not logged, echoed, or included in status.
- Mount success is reported only after the native adapter creates the FUSE mount and starts its request loop.
- The API binds to loopback by default and is not intended to be exposed to the public internet.
- The FUSE/NFS/SMB adapters are not yet target-build or on-console verified.
