# Third-party notices

## libnfs

- Upstream: <https://github.com/sahlberg/libnfs>
- Licence: LGPL-2.1 (library), BSD for parts of the RPC code.
- Used for: the NFS backend (`native/nfs_share.cpp`), linked statically.

## libsmb2

- Upstream: <https://github.com/sahlberg/libsmb2>
- Licence: LGPL-2.1.
- Used for: the SMB2/SMB3 backend (`native/smb_share.cpp`), linked statically.

## OpenSSL and zlib

- OpenSSL: <https://www.openssl.org/source/> (Apache-2.0).
- zlib: <https://zlib.net/> (zlib licence).
- Used by libnfs/libsmb2 for TLS and compression.

## PS5SX2

- Upstream: <https://github.com/Swordpdf/PS5SX2>
- Licence: GPL-3.0-or-later.
- Used for: the architecture this rewrite follows. `native/nfs_share.cpp` mirrors the technical
  decisions of `ps5/coreorbis/orbis-shims/OrbisNfs.cpp` — an in-process libnfs client, the
  `--wrap` list of C library calls, a timed async mount, the export path walk-down, a listing
  cache for stat and a read-ahead window. The implementation here is written for this payload
  (one backend interface for two protocols, no `FILE` array takeover on the host, a different
  slot and directory model) but the design is theirs.

## ProsperoMgr

- Upstream: <https://github.com/Sweeney-development/ProsperoMgr>
- Used for: the control panel's visual identity, colour palette and component styling.

## Itemzflow

- Upstream: <https://github.com/LightningMods/Itemzflow>
- No longer used. It was the libfuse compatibility source for the previous FUSE-based versions.
