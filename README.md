# PS5 Network Mounting

<div align="center">
  <h3>Network Share FUSE Mount Daemon & Prospero Web Interface for PlayStation 5</h3>
  <p>Mount remote NFS exports and SMB2/SMB3 shares read-only to <code>/hostapp</code> on PS5 homebrew.</p>
</div>

---

## Overview

**PS5 Network Mounting** is a native PS5 homebrew payload and daemon that mounts remote network shares directly into the PlayStation 5 filesystem at `/hostapp` using a user-space FUSE adapter. Once mounted, the console and homebrew loaders can browse, read, and execute applications, games, and packages directly over the local network without transferring files to the internal SSD or USB storage.

The daemon includes an embedded HTTP server serving a modern web control interface designed around the **ProsperoMgr** visual identity. The interface is accessible from the PS5 browser and other devices on the local network. The optional home-screen tile is registered through ShadowMountPlus and opens the local control panel only while this payload daemon is running.

---

## Key Features

- **Console Integration & UX (ProsperoMgr Style):**
  - **WebKit Browser Launch:** On loading the ELF payload, the daemon attempts to open the PS5 browser at its LAN URL and exposes a manual retry action in the panel.
  - **On-Screen PS5 Toast Notifications:** Displays native on-screen toast notifications upon startup showing the payload version and the local network listening URL (`http://<PS5-IP>:18080`).
  - **Resilient Network Daemon:** Recovers gracefully if `sceNetInit` or net pools are already initialized by your payload launcher or earlier homebrew.
- **Supported Protocols:**
  - **NFS (v3/v4):** Backed by `libnfs` and a low-latency PS5 FUSE adapter. Connects directly to TrueNAS, Synology, QNAP, or Linux exports.
  - **SMB2 / SMB3:** Backed by `libsmb2` and a dedicated FUSE adapter with guest and authenticated session support.
  - *FTP and SFTP are intentionally omitted to focus on high-throughput, kernel-mountable block/file protocols.*
- **Kernel & Execution Compatibility:**
  - Regular files are mapped with executable permissions (`0555`), allowing native PS5 game binaries (`eboot.bin`), PRX shared libraries, and assets to execute seamlessly from `/hostapp`.
  - Directories are mapped with standard directory permissions (`0755`).
  - Automatic creation of the `/hostapp` mount target if missing.
- **Embedded Web Control Panel (ProsperoMgr Theme):**
  - Styled to match the dark graphite/navy palette (`#0a0e16`, `#10161f`, `#161e2a`) and blue accents (`#2f8cff`) of ProsperoMgr.
  - Live mount health indicators with pulsing status pill (`.pmgr-live-dot`).
  - Fully embedded in the binary via `tools/embed_ui.py` — zero external runtime asset dependencies.
- **Hardened HTTP Daemon & API:**
  - **Network-wide accessibility:** Binds to `0.0.0.0:18080` by default so any PC, phone, or tablet on your LAN can manage mounts.
  - **Concurrency:** Thread-pool dispatch supporting up to 32 concurrent requests with socket timeouts to prevent stalls.
  - **Standard JSON Parser:** Built-in RFC 8259 parser (`native/json.hpp`) replacing naive substring search, with validation against malformed payloads and key collisions.
  - **CSRF & Origin Protection:** Strict checks against cross-origin attacks with required `X-Requested-With: PS5NM` request validation.

---

## Architecture & Layout

```
├── .github/workflows/
│   ├── ps5-full-nfs-smb-build.yml   # Full PS5 target ELF cross-compilation CI
│   └── source-checks.yml             # Host build, JSON unit tests, smoke tests, FUSE syntax checks
├── docs/
│   ├── API.md                        # HTTP REST API specification
│   ├── BUILD.md                      # Compilation guide & SDK requirements
│   ├── IMPLEMENTATION_STATUS.md      # Protocol & platform implementation details
│   ├── NFS_TEST_PLAN.md              # Test matrix for NFS exports
│   └── THIRD_PARTY_NOTICES.md        # Open-source licenses (libnfs, libsmb2, FUSE)
├── native/
│   ├── json.hpp                      # Zero-dependency, standards-compliant JSON parser
│   ├── main.cpp                      # HTTP server, REST routing, socket worker threads
│   ├── mount_backend.hpp / .cpp      # Core mount orchestration & status state machine
│   ├── nfs_fuse.hpp / .cpp           # NFS FUSE adapter (libnfs)
│   ├── smb_fuse.hpp / .cpp           # SMB FUSE adapter (libsmb2)
│   └── web_assets.hpp                # Deterministically generated C++ header of the embedded UI
├── tests/
│   ├── http_smoke.py                 # Automated HTTP/REST and security test suite
│   ├── net_stubs.cpp                 # Host networking stubs for Linux development
│   └── test_json.cpp                 # JSON parser unit test suite
├── tools/
│   └── embed_ui.py                   # Compresses and embeds web/index.html into C++ header
└── web/
    └── index.html                    # Single-page web UI with ProsperoMgr styling
```

---

## Quick Start & Usage

### 1. Launching the Payload on PS5
1. Send or run the compiled `ps5nm.elf` using your preferred PS5 payload sender (e.g. Netcat on port `9021` or payload loader).
2. The payload initializes networking (`sceNetInit`), starts the FUSE subsystem, and begins listening on `0.0.0.0:18080`.

### 2. Accessing the Control Panel
- **From PS5 Browser:** Use the automatic browser launch or navigate to `http://<ps5-ip-address>:18080`.
- **From PC / Phone / Mac:** Navigate to `http://<ps5-ip-address>:18080`. The console and client device must be on a reachable network.
- **Home-screen tile:** The tile is a convenience shortcut to the panel, registered through ShadowMountPlus. It does not itself start the network-mounting ELF, so load the payload first after a reboot.

### 3. Mounting Shares
- **NFS Share:**
  - **Server:** `192.168.1.50`
  - **Port:** `2049` (default)
  - **Export Path:** `/mnt/dataPool/games` (absolute path as exported on the NAS)
  - *Tip: Ensure the NFS export allows read permissions and `no_root_squash` or all-squash to a user with read access.*
- **SMB Share:**
  - **Server:** `192.168.1.50`
  - **Port:** `445` (default)
  - **Share Name:** `games` or `games/PS5` (do not prefix with slashes)
  - **Username / Password:** Provide if your SMB server requires authentication, or leave blank for guest access.

---

## REST API Reference

The web control panel communicates via JSON endpoints under `/api/v1`:

| Endpoint | Method | Description |
|---|---|---|
| `/api/v1/status` | `GET` | Returns live mount status, active protocol, endpoint, and last error. |
| `/api/v1/mount` | `POST` | Validates parameters and mounts the requested NFS or SMB share to `/hostapp`. |
| `/api/v1/unmount` | `POST` | Safely detaches the active FUSE filesystem from `/hostapp`. |
| `/api/v1/shortcut` | `POST` | Creates the HBRW homebrew folder and requests a ShadowMountPlus scan. |

All `POST` requests require `Content-Type: application/json` and `X-Requested-With: PS5NM`. Detailed request/response schemas can be found in [`docs/API.md`](docs/API.md).

---

## Building

### Building on Host (Linux/macOS for Development & Testing)
```bash
# Compile and run JSON unit tests
g++ -std=c++17 -O2 -Wall -Wextra -Inative tests/test_json.cpp -o /tmp/test_json
/tmp/test_json

# Compile host binary with network stubs
g++ -std=c++17 -O2 -Wall -Wextra -Inative \
    native/main.cpp native/mount_backend.cpp tests/net_stubs.cpp \
    -pthread -o /tmp/ps5nm-host

# Run automated HTTP test suite
python3 tests/http_smoke.py
```

### Building the PS5 ELF (Target Build)
Target compilation requires the OpenOrbis / Prospero toolchain with ORBISDEV libraries (`libfuse`, `libnfs`, `libsmb2`, `mbedtls`, `zlib`).
```bash
export OO_PS4_TOOLCHAIN=/path/to/openorbis
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make PS5NM_WITH_FUSE=1
```
The automated GitHub Actions workflow (`.github/workflows/ps5-full-nfs-smb-build.yml`) builds the full production ELF release automatically on push.

---

## Acknowledgments & Credits

- **[ProsperoMgr](https://github.com/Sweeney-development/ProsperoMgr):** UI design reference, color palette, and styling system.
- **[Itemzflow](https://github.com/LightningMods/Itemzflow):** Reference implementation for PS5 FUSE / NFS integration (`fuse-nfs`).
- **[ps5-ezremote-client](https://github.com/Sweeney-development/ps5-ezremote-client):** SMB client and `libsmb2` reference patterns.
- **[libnfs](https://github.com/sahlberg/libnfs):** Client library for NFS.
- **[libsmb2](https://github.com/sahlberg/libsmb2):** Client library for SMB2/SMB3.
