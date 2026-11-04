# PS5 Network Mounting

<div align="center">
  <h3>In-process NFS &amp; SMB share layer with a Prospero web panel for PlayStation 5</h3>
  <p>Serve a remote NFS export or SMB2/SMB3 share under <code>/hostapp</code> from a native PS5 payload.</p>
</div>

---

## What this is

**PS5 Network Mounting** is a native PS5 homebrew payload. It connects to an NFS export or an
SMB2/SMB3 share, keeps the connection open, and answers reads for every path under
`/hostapp` from that share. A small HTTP server on port `18080` serves the control panel.

The payload does **not** create a kernel mount. It follows the mechanism PS5SX2 uses for its
NFS shares: the protocol client is linked into the payload, the C library's file calls are
redirected with the linker's `--wrap`, and the payload answers them itself. There is no
`/dev/fuse`, no libfuse build, and no Itemzflow dependency.

### The limit this design carries

An in-process share is visible **to code running inside this payload**. Games, the PS5 home
screen and other homebrew run in separate processes and cannot see `/hostapp`; the console's
filesystem is unchanged. If you need a share that other applications can open, this is not the
right layer — that requires the kernel FUSE path this project deliberately dropped.

---

## Features

- **Two protocols, one layer.** NFS (libnfs, v3 with a v4 fallback) and SMB2/SMB3 (libsmb2,
  guest or authenticated, optional `DOMAIN\user`) sit behind a single backend interface, so the
  VFS, the shim, the API and the panel are written once.
- **Read-only, executable.** Files are mapped `0555` and folders `0555`, so ELFs and PRXs run
  straight from a share and nothing can be written back.
- **Built for game reads.** A folder listing fills a 30-second stat cache, so a loader that
  stats every entry after `readdir` costs no extra round trips. Each open file reads through a
  read-ahead window that grows from 64 KiB to 1 MiB while reads are sequential.
- **Complete libc coverage where it matters.** `fopen`/`fread`/`fseek`/`fgetc`/`fgets`/`stat`/
  `lstat`/`access`/`realpath`/`open`/`read`/`pread`/`lseek`/`fstat`/`close` and the directory
  calls are all intercepted (`native/wrap.list`). Anything outside the root is untouched.
- **Bounded failure.** Every call into a protocol context is serialised, an open handle keeps
  its session alive across a detach, and a disconnect makes further reads fail with `EIO`
  instead of touching freed memory.
- **Control panel kept.** The ProsperoMgr-styled panel is unchanged in look, now reporting the
  share layer, protocol, endpoint, open files and bytes served.
- **Console integration.** Startup toast with the version and panel URL, automatic WebKit launch
  (retried, and disableable with `PS5NM_NO_BROWSER=1`), a home-screen shortcut action, and a
  rotating diagnostic log on `/data`.

---

## Layout

```
├── native/
│   ├── share.hpp               # backend interface, node info, path helpers
│   ├── vfs.hpp / .cpp          # the mount table, resolution, listing cache
│   ├── nfs_share.cpp           # libnfs backend (v3 with v4 fallback, export walk-down)
│   ├── smb_share.cpp           # libsmb2 backend (guest/authenticated, domain support)
│   ├── posix_shim.cpp          # the __wrap_* file and directory calls
│   ├── wrap.list               # symbols the Makefile passes to --wrap
│   ├── mount_backend.hpp/.cpp  # request validation and orchestration
│   ├── main.cpp                # HTTP server, REST routes, console integration
│   ├── diagnostic_log.*        # rotating log on /data
│   ├── shortcut.*              # ShadowMountPlus home-screen shortcut
│   ├── json.hpp                # dependency-free JSON parser
│   └── web_assets.hpp          # the panel, embedded by tools/embed_ui.py
├── web/index.html              # panel source
├── tests/                      # JSON, VFS, shim and HTTP tests (host build)
├── docs/                       # API, build, architecture, third-party notices
└── .github/workflows/          # host checks and the PS5 ELF release build
```

---

## Quick start

1. Send `PS5NetworkMounting.elf` with your payload loader. It initialises networking, starts the
   HTTP server on `0.0.0.0:18080` and shows a toast with the panel URL.
2. Open `http://<ps5-ip>:18080` from the console browser or any device on the LAN.
3. Choose NFS or SMB, fill in the server and share details, and press **Connect & Attach**.

- **NFS:** server `192.168.1.50`, port `2049`, export `/mnt/dataPool/games`.
- **SMB:** server `192.168.1.50`, port `445`, share `games` or `games/PS5`, plus a user and
  password if the server needs them (leave blank for guest).

From then on, code inside the payload reads `/hostapp/...` from the share. Disconnecting closes
the session; handles already open stay readable until they are closed.

### Environment variables

| Variable | Meaning |
|---|---|
| `PS5NM_PORT` | Panel port (default `18080`) |
| `PS5NM_BIND` | Bind address (default `0.0.0.0`) |
| `PS5NM_NO_BROWSER` | `1` disables the automatic WebKit launch |
| `PS5NM_LOG_DIR` | Directory for the rotating log (default `/data/PS5NetworkMounting`) |
| `PS5NM_LAUNCHER_ELF` | Launcher path for the home-screen shortcut (non-embedded builds) |

---

## REST API

| Endpoint | Method | Description |
|---|---|---|
| `/api/v1/status` | `GET` | Share layer, protocol, endpoint, state, counters |
| `/api/v1/mount` | `POST` | Validate and attach an NFS or SMB share |
| `/api/v1/unmount` | `POST` | Detach the share |
| `/api/v1/log` | `GET` | Download the diagnostic log |
| `/api/v1/browser` | `POST` | Open the panel in the PS5 browser |
| `/api/v1/shortcut` | `POST` | Create the home-screen shortcut |
| `/api/v1/notify` | `POST` | Send a toast notification |

`POST` bodies are JSON; requests whose `Origin` does not match the `Host` are refused. See
[`docs/API.md`](docs/API.md).

---

## Building

```bash
# PS5 ELF: needs the payload SDK, libnfs, libsmb2, OpenSSL and zlib
PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk ORBISDEV=$PS5_PAYLOAD_SDK/target/user/homebrew make

# Host: build, unit tests and the HTTP smoke test
make test
```

The `PS5 payload build` workflow builds the ELF and publishes it as a release when `main`
changes. [`docs/BUILD.md`](docs/BUILD.md) has the details.

---

## Credits

- **[PS5SX2](https://github.com/Swordpdf/PS5SX2):** the in-process share architecture this
  rewrite follows (`OrbisNfs`, the `--wrap` list, the listing cache and read-ahead).
- **[ProsperoMgr](https://github.com/Sweeney-development/ProsperoMgr):** the panel's visual
  identity and palette.
- **[libnfs](https://github.com/sahlberg/libnfs)** and
  **[libsmb2](https://github.com/sahlberg/libsmb2):** the protocol clients.
- **[Itemzflow](https://github.com/LightningMods/Itemzflow):** the original FUSE reference this
  version no longer uses.
