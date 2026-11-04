# Building

## Requirements

### PS5 payload

- The PS5 payload SDK (`PS5_PAYLOAD_SDK`, for `toolchain/prospero.mk` and `prospero-clang`).
- `ORBISDEV` providing `include/nfsc/libnfs.h`, `include/smb2/libsmb2.h` and the static
  libraries `libnfs.a`, `libsmb2.a`, `libssl.a`, `libcrypto.a`, `libz.a`.

The CI workflow builds the SDK and those libraries from
[`pacbrew-repo`](https://github.com/ps5-payload-dev/pacbrew-repo) (`sdk`, `openlibm`, `libcxx`,
`zlib`, `openssl`, `libnfs`, `libsmb2`) against the organization's SDK checkout. FUSE and
Itemzflow are no longer needed.

### Host

- `g++` with C++17 and `python3` (for the panel embedder and the smoke test).

## Commands

```bash
# Regenerate native/web_assets.hpp after editing web/index.html
make embed-ui

# Host: build the payload with stubs, run the unit tests and the HTTP smoke test
make test

# PS5 ELF
PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk \
ORBISDEV=$PS5_PAYLOAD_SDK/target/user/homebrew \
make
```

Output: `build/PS5NetworkMounting-v<version>.elf`, with `<version>` taken from `VERSION` in the
Makefile. `make test` fails when that value and `kVersion` in `native/main.cpp` disagree.

## What `make test` runs

| Target | Covers |
|---|---|
| `build/test-json` | the JSON parser |
| `build/test-vfs` | path resolution, the listing cache, read-only enforcement, attach/detach |
| `build/test-shim` | the real `--wrap` layer on Linux: `fopen`/`read`/`seek`/`stat`/`readdir`, pass-through outside the root |
| `tests/http_smoke.py` | the panel, every API route, error codes, foreign-Origin refusal |

`make check-arch` verifies that `native/wrap.list` and `native/posix_shim.cpp` agree in both
directions and that the Makefile still passes `--wrap` and no longer mentions FUSE. The
`Source checks` workflow additionally syntax-checks both backends against upstream libnfs and
libsmb2 headers.

## Why `--wrap`

`--wrap=fopen` makes every call in the payload resolve to `__wrap_fopen`, with the C library's
own function available as `__real_fopen`. The shim answers paths under `/hostapp` and forwards
everything else. Adding a new intercepted call means adding it to `native/wrap.list` **and**
defining `__wrap_<name>`; `make check-arch` fails if only one side is done.

## Notes

- `-D_FILE_OFFSET_BITS=64` is required: share files are larger than 2 GiB.
- The embedded launcher is built first and linked into the payload as a read-only object; the
  home-screen shortcut points at it.
- Nothing in the build needs a patched libc, a `fuse` header or a kernel module.
