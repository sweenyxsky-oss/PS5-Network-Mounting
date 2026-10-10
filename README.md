# PS5 Network Mounting

A controller-friendly PS5 homebrew payload for exposing network shares as local filesystem paths, with `/hostapp` as the intended mount target.

## Source roles

- **ps5-ezremote-client** — reference implementation for SMB and FTP protocol handling.
- **Itemzflow** — reference implementation for the PS5 NFS/FUSE mounting flow.
- **libnfs** — NFS client library.
- **fuse-nfs** — FUSE-to-NFS filesystem adapter.
- **ProsperoMgr** — reference for a web-served PS5 payload UI.

## Design

The target is a single PS5 payload/ELF that serves a local web UI over HTTP, reachable from the PS5 browser or another device on the LAN. The UI will provide protocol selection, server/share/export, credentials, mount status, and explicit connect/disconnect actions.

## Important implementation rule

A file-browser connection is not the same as a filesystem mount. The first milestone must prove that NFS can be mounted and accessed at the chosen mountpoint. SMB and FTP are separate milestones and must not be reported as mounted until a filesystem adapter is implemented and verified.

## First payload milestone

- `web/index.html`: responsive, controller-friendly browser UI with NFS/SMB/FTP/SFTP selection, server/path fields, credential fields, mount/unmount actions, status panel and actionable API errors.
- `docs/API.md`: same-origin API contract for status, mount and unmount.
- The UI targets `/api/v1/status`, `/api/v1/mount`, and `/api/v1/unmount` on the same HTTP origin that serves the page.

## Current status

**Frontend/API contract only — not yet a runnable PS5 ELF.** The native HTTP server and mount backend are not implemented in this commit. The page will show the API as unavailable until those routes exist. NFS is the first backend target; SMB/FTP/SFTP remain explicitly unimplemented until their filesystem-mount adapters are proven. This avoids falsely showing a remote file-browser connection as a successful local mount.
