# Architecture

## Why in-process

The first version of this payload mounted a share with FUSE: a filesystem driver answered
`/dev/fuse` requests and the kernel exposed `/hostapp` to everything. That is the only way to
make a share visible to other processes, but it depends on a patched libfuse built for the
console, a compatible kernel FUSE device, and an unmount that behaves on firmware variations.

This rewrite follows PS5SX2's approach instead. PS5SX2 serves its NFS shares without a kernel
mount: it links libnfs into the application, linker-wraps the C library's file calls and answers
the paths itself. The payload does the same for NFS and SMB.

Trade-off, stated plainly: **only code inside the payload sees the share.** Games and the home
screen cannot open `/hostapp`. What the design gains is a short, self-contained code path with
no `/dev/fuse`, no libfuse, no Itemzflow checkout and no mount/unmount recovery.

## The layers

```
     libc calls in the payload                panel / REST
              │                                   │
     native/posix_shim.cpp  ────────────►  native/main.cpp
              │                                   │
              ▼                                   ▼
        native/vfs.cpp  ◄────────────  native/mount_backend.cpp
              │                        (validation, orchestration)
              ▼
      ps5nm::ShareBackend
        ├── native/nfs_share.cpp   (libnfs)
        └── native/smb_share.cpp   (libsmb2)
```

**`share.hpp`** is the contract: `ShareBackend` for connect/stat/open/opendir/volume,
`ShareFile` for positional reads, `ShareDir` for iteration, plus `ShareStatus` (a positive errno
and a sentence). Nothing above it knows which protocol is attached.

**`vfs.cpp`** owns one attached session: the mount root, path resolution, the listing cache and
the counters. `Normalise` resolves `.` and `..` and refuses paths that climb above `/`, so a path
can never escape the root. `IsManaged` decides whether the shim answers a call at all.

**`posix_shim.cpp`** holds a fixed table of 64 slots. A `FILE*` handed to the application points
at the shim's own `FILE` inside that table, never at one the C library created; descriptors are
`0x3d000000 + slot`, far above anything the kernel allocates. Every wrapper checks whether the
path is managed and otherwise forwards to `__real_*` unchanged.

**`nfs_share.cpp`** mounts with its own deadline (libnfs's timeout starts at its first request,
so a silent server could otherwise hold a synchronous mount for minutes). It tries the address's
whole path first, then shorter exports with the rest treated as a folder inside, then NFS v4.
`smb_share.cpp` parses `ShareName/subfolder`, splits an optional `DOMAIN\user`, and passes
`host:port` for a non-default port (libsmb2 has no port setter but parses the server string).

## Concurrency and lifetime

- One mutex per protocol session serialises every call into that context; libnfs and libsmb2
  contexts are not thread-safe.
- A slot's mutex covers one read; `fclose`/`close` removes the slot from the table first and then
  waits for an in-flight read, so a slot can never be recycled underneath a reader.
- A detach marks the session closing and drops the backend's reference. Open handles keep the
  session (and its context) alive; their reads fail with `EIO` and the context is destroyed when
  the last handle closes. No use-after-free, no forced close of someone else's file.

## Reads and caching

- `readdir` remembers every entry and its attributes for 30 seconds. A loader that lists a folder
  and then stats each name therefore costs one round trip per folder, not one per file.
- Each open file keeps a read-ahead window: 64 KiB at first, doubling up to 1 MiB while reads
  follow each other, skipped entirely for requests of 1 MiB or more.
- Failures are logged a bounded number of times so one dead server cannot flood `/data`.

## Security posture

- Only `/hostapp` can be the root, and only absolute, `..`-free paths reach a backend.
- Nothing is writable: `open`/`fopen` with write flags fail with `EROFS`, and writes through a
  share handle fail with `EBADF`.
- State-changing HTTP requests with a foreign `Origin` are refused; bodies are capped at 64 KiB
  and headers at 16 KiB.
