# Implementation status

## Present in the branch

- Browser UI in `web/index.html`, embedded as `native/web_assets.hpp`.
- Native HTTP API for status, mount, and unmount.
- Request-body limits, input validation, JSON escaping, and no credential logging.
- A read-only NFS FUSE adapter in `native/nfs_fuse.cpp`, using the PS5-compatible FUSE and libnfs APIs.
- An optional build switch: `PS5NM_WITH_FUSE=1` with `ORBISDEV` set to an SDK that has the patched FUSE and `liborbisNfs` libraries installed.

## Not yet verified

The NFS adapter is new integration code and has **not been compiled against the target SDK or tested on a PS5**. The default build excludes it, so the default payload reports that the adapter is not linked. The optional build currently mixes the PS5 payload SDK toolchain with ORBISDEV libraries; ABI compatibility must be confirmed before relying on it.

The FUSE adapter is read-only by design. It initializes libnfs, checks the NFS export, creates a FUSE mount at `/hostapp`, and exposes directory listing, metadata, symlinks, and file reads. It must be validated for firmware 13.60, unmount/reconnect, empty exports, permissions, thread shutdown, and actual file reads before being treated as working.

SMB, FTP, and SFTP are still not filesystem mounts and return explicit unsupported errors. ezRemote's remote browsing/transfer code is not a mount adapter.

## Build notes

The default native HTTP server listens on port 18080. The optional FUSE adapter requires `PS5NM_WITH_FUSE=1` and `ORBISDEV`. No successful build or on-console test is claimed.
