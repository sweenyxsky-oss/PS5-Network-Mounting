# PS5 Network Mounting

A PS5 homebrew payload intended to expose one network share as a local, read-only filesystem at `/hostapp`.

## Protocols

- **NFS** — libnfs + the PS5-compatible FUSE layer.
- **SMB2/SMB3** — libsmb2 + the PS5-compatible FUSE layer.
- FTP and SFTP are intentionally out of scope.

The web UI is embedded in the payload and served by a native HTTP API. It defaults to `http://127.0.0.1:18080/` on the console. Only one share can be mounted at a time. The backend refuses to mount when `/hostapp` contains files, to avoid hiding existing data.

## Important status

The source contains first-pass NFS and SMB filesystem adapters, but **has not yet been compiled with the target PS5 SDK or tested on firmware 13.60**. The build requires compatible ORBISDEV builds of patched libfuse, liborbisNfs, and libsmb2. Do not consider the program ready until the build succeeds and real file reads plus disconnect/reconnect pass on the console.

See [build prerequisites](docs/BUILD.md), [implementation status](docs/IMPLEMENTATION_STATUS.md), [API contract](docs/API.md), and the [NFS/SMB test plan](docs/NFS_TEST_PLAN.md).
