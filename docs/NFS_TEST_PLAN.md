# NFS integration test checklist

Do not treat a successful compile as proof that the mount works. Test on the target PS5 firmware and a real NFS server.

## TrueNAS preparation

1. Create or choose a dataset for the game library and grant read/traverse permissions to the account used by the NFS share.
2. Create an NFS share for that dataset and allow the PS5's IP address or its trusted LAN subnet.
3. For a first test, use read-only access and the exact exported path shown by TrueNAS. If the mount connects but appears empty, check the export path and the share's Mapall User/Group permissions.
4. Confirm the PS5 can reach the NAS on the NFS port. The payload's port field configures the NFS service port; portmapper/MOUNT behavior may depend on the TrueNAS export configuration.

## PS5 test sequence

- Build with the FUSE adapter enabled and confirm the build used the intended ORBISDEV FUSE/NFS libraries.
- Launch the ELF and open `http://127.0.0.1:18080/` in the PS5 browser.
- Refresh status. Before connecting, status must say not mounted.
- Connect using protocol NFS, NAS hostname/IP, port 2049 unless configured otherwise, the exact exported path, and mountpoint `/hostapp`.
- Confirm the response succeeds, then use a PS5 file manager or another trusted homebrew tool to list and read files under `/hostapp`.
- Test an empty export, wrong export path, denied client IP, inaccessible permissions, NAS reboot, network disconnect/reconnect, and repeated connect/disconnect.
- Disconnect from the UI and verify that `/hostapp` returns to its prior local contents and no FUSE thread remains.

## Release gate

Do not ship the adapter as working until the PS5 build compiles and the real-file read, failure cases, and unmount/reconnect tests above pass. The current repository has not yet recorded those results.
