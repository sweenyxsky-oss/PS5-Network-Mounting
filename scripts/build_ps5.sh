#!/usr/bin/env bash
set -eo pipefail

if [ -z "$PS5_PAYLOAD_SDK" ]; then PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk; fi
if [ -z "$ORBISDEV" ]; then
  echo "Set ORBISDEV to the PS5-compatible ORBISDEV SDK" >&2
  exit 2
fi
if [ -z "$ITEMZFLOW_ROOT" ]; then ITEMZFLOW_ROOT=../Itemzflow; fi

if [ ! -f "$PS5_PAYLOAD_SDK/toolchain/prospero.mk" ]; then
  echo "PS5 payload SDK not found at: $PS5_PAYLOAD_SDK" >&2
  exit 2
fi
if [ ! -f "$ITEMZFLOW_ROOT/itemz-daemon/include/fuse/fuse.h" ]; then
  echo "Itemzflow patched FUSE headers not found at: $ITEMZFLOW_ROOT" >&2
  exit 2
fi

make clean
make PS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" ORBISDEV="$ORBISDEV" ITEMZFLOW_ROOT="$ITEMZFLOW_ROOT"
test -s build/PS5NetworkMounting.elf
echo "Created build/PS5NetworkMounting.elf"
echo "Target-console NFS/SMB mount tests are still required before this build is considered verified."
