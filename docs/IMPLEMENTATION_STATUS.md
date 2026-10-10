# Implementation status

## In the development branch

- Native HTTP server and embedded browser UI.
- API routes for mount, status, and unmount.
- NFS read-only FUSE adapter using libnfs.
- SMB2/SMB3 read-only FUSE adapter using libsmb2.
- Single active share, strict `/hostapp` mountpoint, non-empty mountpoint protection, credential redaction, and bounded HTTP request bodies.
- Build-time generation of `native/web_assets.hpp` from `web/index.html`.

## Not verified

The GitHub Actions source workflow passed its host compiler/API smoke test and now syntax-checks the NFS/SMB adapter source against upstream headers. These checks still do not compile the adapters against ORBISDEV or validate PS5 ABI compatibility.

The project has not yet been compiled with the target PS5 SDK or tested on firmware 13.60. The build depends on patched PS5-compatible libfuse, libnfs, and libsmb2 libraries under ORBISDEV. ABI and link compatibility remain unproven.

The adapters must be tested on console for export/share access, file listing, metadata, file reads, large files, permissions, wrong credentials, server disconnect, unmount, and reconnect. The current code is an implementation attempt, not a verified release.

FTP and SFTP are intentionally excluded. Do not tag or distribute a production release until build and hardware tests pass.
