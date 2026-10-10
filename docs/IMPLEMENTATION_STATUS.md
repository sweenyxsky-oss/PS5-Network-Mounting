# Implementation status

The branch includes the native HTTP server, embedded UI, and a read-only NFS FUSE adapter. The Makefile enables FUSE by default; set `PS5NM_WITH_FUSE=0` to build the UI/API scaffold only.

The adapter has not yet been compiled against the target SDK or tested on a PS5. The build combines the PS5 payload SDK with ORBISDEV libraries, so ABI compatibility must be proven. Do not treat NFS mounting as verified until firmware 13.60 tests confirm real file reads, permissions, unmount, and reconnect.

SMB, FTP, and SFTP filesystem mounts are not implemented. See `docs/BUILD.md` and `docs/NFS_TEST_PLAN.md`.
