# Build setup

The default target now enables the NFS FUSE adapter. It requires both the PS5 payload SDK (with `libmicrohttpd.a`) and an ORBISDEV SDK containing the PS5-compatible patched `libfuse.a` and `liborbisNfs.a`.

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk ORBISDEV=/path/to/orbisdev
```

To build only the UI/API scaffold while the FUSE libraries are being prepared:

```sh
make PS5NM_WITH_FUSE=0 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
```

The optional FUSE adapter uses Itemzflow-compatible FUSE APIs and the ORBISDEV NFS client library. The build currently links libraries from two SDK installations; ABI compatibility must be checked by compiling and running on the target before treating the output as a release.

The payload defaults to HTTP port 18080 and binds to the console loopback address. Open `http://127.0.0.1:18080/` in the PS5 browser. It is not exposed to other LAN devices by default.

The NFS adapter is read-only. SMB, FTP, and SFTP are not mounted filesystems yet.
