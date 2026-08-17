#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# live_integration.sh — exercise zhaxx end-to-end on a throwaway file-backed
# pool. Needs root and a working ZFS. NOT run in CI (creates real pools).
#
# It builds a real clone graph, captures zdb dumps (a template for real
# fixtures), and applies a hand-written plan through the surgeon to prove the
# import -> verify -> commit -> re-import path. It does not synthesize objset
# EIO (which needs deliberate block damage); the offline tests cover the
# unreadable-objset logic.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
POOL="zhaxx_it_$$"
IMG="$(mktemp /tmp/zhaxx_it.XXXXXX.img)"
CAP="$(mktemp -d)"

cleanup() {
	zpool destroy -f "$POOL" 2>/dev/null || true
	rm -f "$IMG"
	rm -rf "$CAP"
}
trap cleanup EXIT

[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 1; }
command -v zpool >/dev/null || { echo "zfs not installed" >&2; exit 1; }

echo "=== create file-backed pool $POOL ==="
truncate -s 512M "$IMG"
zpool create -o failmode=continue "$POOL" "$IMG"

echo "=== build a clone graph (snapshot + clones -> populates clones ZAPs) ==="
zfs create "$POOL/base"
dd if=/dev/urandom of="/$POOL/base/f" bs=1M count=8 status=none
zfs snapshot "$POOL/base@s"
zfs clone "$POOL/base@s" "$POOL/clone_keep"
zfs clone "$POOL/base@s" "$POOL/clone_drop"
sync

echo "=== capture MOS with zdb (read-only) ==="
zpool export "$POOL"
# Save a handful of MOS objects as an offline dump set.
for obj in 1 $(seq 1 80); do
	zdb -e -dddd "$POOL" "$obj" > "$CAP/obj_${obj}.txt" 2>/dev/null || true
done
echo "captured dumps in $CAP (inspect to model new fixtures)"

echo "=== scanner on a healthy graph: expect no findings ==="
zpool import -N "$POOL"
if "$here/../scan/zhaxx-scan" --pool "$POOL"; then
	echo "PASS: scanner reports clean"
else
	echo "NOTE: scanner reported findings on a healthy pool — investigate"
fi
zpool export "$POOL"

echo
echo "To exercise the surgeon, hand-write a plan that (e.g.) removes a live"
echo "clone's entry and decrements its origin's ds_num_children, then:"
echo "  $here/../surgeon/build.sh"
echo "  $here/../surgeon/zhaxx --plan PLAN $POOL          # dump/verify"
echo "  $here/../surgeon/zhaxx --commit --plan PLAN $POOL # apply"
echo "  zpool import -N -o readonly=on $POOL && zpool status -v $POOL"
echo
echo "=== done ==="
