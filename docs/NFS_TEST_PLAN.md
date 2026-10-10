# NFS and SMB integration test plan

Do not treat a successful compile or a successful server login as proof that the filesystem mount works. Test on the target PS5 firmware and a real TrueNAS share.

## TrueNAS preparation

### NFS

1. Create or choose the dataset for the library and grant read/traverse permissions to the NFS client.
2. Create an NFS share for that dataset and allow the PS5's IP address or trusted LAN subnet.
3. Use the exact exported absolute path shown in TrueNAS. If the mount connects but appears empty, check the export path and Mapall User/Group permissions.
4. NFS defaults to port 2049. Portmapper/MOUNT behavior may depend on the TrueNAS NFS configuration.

### SMB

1. Enable the SMB service and create a share, for example `games`.
2. Create a dedicated user with read-only access to the share and the underlying dataset.
3. Use the share name, optionally followed by a subdirectory, e.g. `games/PS5`. Do not enter the TrueNAS filesystem path in the SMB field.
4. Use port 445 unless the server is configured otherwise. Guest access is attempted when username/password are empty.

## PS5 test sequence

- Build with the PS5 payload SDK and compatible patched libfuse, libnfs, and libsmb2.
- Launch the ELF and open `http://127.0.0.1:18080/` in the PS5 browser.
- Refresh status. Before connecting, status must say not mounted.
- Verify that a non-empty `/hostapp` is rejected without hiding its contents.
- Connect to NFS with the exact absolute export path, or SMB with the share name and optional subpath.
- Confirm the API says mounted, then use a trusted PS5 file manager/homebrew tool to list and read files under `/hostapp`.
- Read small files and large files; test nested directories, names with spaces, empty folders, and directory listings beyond one buffer.
- Test wrong export/share paths, denied client IP, invalid credentials, inaccessible permissions, unreachable server, NAS reboot, network disconnect/reconnect, and repeated connect/disconnect.
- Confirm writes fail for both protocols.
- Disconnect from the UI and verify that `/hostapp` returns to its prior local contents and no FUSE thread remains.

## Release gate

Do not ship the adapter as working until the PS5 build compiles and the real file read, failure cases, read-only behavior, and unmount/reconnect tests above pass on firmware 13.60. No such results have yet been recorded in this repository.
