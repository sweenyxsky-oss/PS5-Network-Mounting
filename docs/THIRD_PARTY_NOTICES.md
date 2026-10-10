# Third-party dependencies and attribution

This repository links against external PS5-compatible builds of the following libraries. The dependency source and binary builds are not vendored in this repository.

- **libnfs** — https://github.com/sweenyxsky-oss/libnfs. Upstream libnfs is distributed under LGPL-2.1-or-later; retain its license and notices when redistributing.
- **libsmb2** — https://github.com/sahlberg/libsmb2. Distributed under LGPL-2.1; retain its license and notices when redistributing.
- **libfuse 2.8-compatible API** — FUSE headers are referenced from the Itemzflow checkout at https://github.com/sweenyxsky-oss/Itemzflow. Its patched PS5-compatible library is an external build dependency. Preserve the applicable upstream and patch licenses when distributing a binary.
- **PS5 payload SDK / ORBISDEV** — follow the license terms supplied with those SDKs.

The NFS and SMB adapter source in this repository is new integration code written against public library APIs; it does not copy Itemzflow or ezRemote implementation files. Check each dependency's current license and any local patches before distributing a combined binary.
