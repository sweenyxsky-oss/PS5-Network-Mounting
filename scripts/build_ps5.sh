#!/usr/bin/env bash
# Builds the PS5 payload against the payload SDK and the share libraries.
set -eo pipefail

PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}
ORBISDEV=${ORBISDEV:-$PS5_PAYLOAD_SDK/target/user/homebrew}

if [ ! -f "$PS5_PAYLOAD_SDK/toolchain/prospero.mk" ]; then
  echo "PS5 payload SDK not found at: $PS5_PAYLOAD_SDK" >&2
  exit 2
fi
if [ ! -f "$ORBISDEV/include/nfsc/libnfs.h" ]; then
  echo "libnfs headers not found at: $ORBISDEV/include/nfsc" >&2
  exit 2
fi
if [ ! -f "$ORBISDEV/include/smb2/libsmb2.h" ]; then
  echo "libsmb2 headers not found at: $ORBISDEV/include/smb2" >&2
  exit 2
fi

make clean PS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" ORBISDEV="$ORBISDEV"
make PS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" ORBISDEV="$ORBISDEV"
test -s build/PS5NetworkMounting.elf
echo "Created build/PS5NetworkMounting.elf"
echo "Console-side NFS and SMB smoke tests are still required before a build is verified."
