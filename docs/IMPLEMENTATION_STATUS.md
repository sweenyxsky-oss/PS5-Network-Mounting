# Implementation status

The branch contains the browser UI, an embedded copy of the page, a native libmicrohttpd HTTP server, API routes for status/mount/unmount, bounded request bodies, validation, JSON escaping, and a PS5 SDK Makefile. A host CMake build and GitHub Actions compile check are included.

The server defaults to port 18080 and can use PS5NM_PORT to select a port from 1024 through 65535. It listens on all interfaces for LAN browser access. Do not expose this service to the public internet.

## Mounting is not enabled yet

The current backend deliberately reports an error instead of claiming a network filesystem is mounted. Itemzflow uses a PS5-specific patched libfuse and fuse-nfs/libnfs implementation inside its daemon; a remote file browser or ordinary desktop mount command is not equivalent. That target-specific filesystem adapter has not yet been integrated or verified on firmware 13.60.

Remaining work for a genuine NFS release:
1. Integrate PS5-compatible patched libfuse and fuse-nfs, preserving required license notices.
2. Build libnfs and the FUSE layer against the PS5 SDK.
3. Integrate owned mount/unmount lifecycle and verify actual files at /hostapp before setting mounted=true.
4. Test with a TrueNAS NFS export, including export permissions, bad paths, network loss, reconnect and cleanup.
5. Implement separate SMB and FTP/SFTP filesystem adapters only after the NFS path is proven.

No PS5 SDK build or on-console test is claimed until it has actually run.
