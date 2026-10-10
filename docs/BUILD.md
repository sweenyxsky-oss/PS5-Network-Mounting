# Build setup

The default build includes both read-only filesystem adapters. It requires:

- PS5 payload SDK, including `toolchain/prospero.mk` (the HTTP server uses POSIX sockets and has no libmicrohttpd dependency)
- Itemzflow's patched FUSE headers (`itemz-daemon/include/fuse`) and the matching ORBISDEV `libfuse` binary
- ORBISDEV `libnfs`
- ORBISDEV-compiled `libsmb2`, plus `libssl`, `libcrypto`, and `libz`
- Compatible headers and static libraries under the ORBISDEV prefix

Build (recommended helper):

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export ORBISDEV=/path/to/orbisdev
export ITEMZFLOW_ROOT=/path/to/Itemzflow
bash scripts/build_ps5.sh
```

Equivalent direct Make invocation:

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk ORBISDEV=/path/to/orbisdev ITEMZFLOW_ROOT=/path/to/Itemzflow
```

Build only the native UI/API scaffold (no actual filesystem mounting):

```sh
make PS5NM_WITH_FUSE=0 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
```

The build checks for the expected headers and libraries before linking. If your ORBISDEV environment does not provide a PS5-compatible `libsmb2`, the SMB adapter cannot be considered buildable until that dependency is ported and installed. Do not substitute a desktop library binary.

The payload defaults to HTTP port 18080 and binds to console loopback. Open `http://127.0.0.1:18080/` in the PS5 browser. One share at a time can be mounted read-only at `/hostapp`.

The code has not yet been compiled against the actual target SDK or validated on a PS5. The build command is a documented build path, not a claim of successful compilation.
